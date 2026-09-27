/* ============================================================
 * HEDWIG - Blue Pill V1 Firmware
 * USB CDC command -> DRV8833 motor control (no encoders, no ultrasonic)
 * Target: STM32F103C8T6 (Blue Pill), STM32CubeIDE / HAL
 * ============================================================
 *
 * CubeMX configuration (set this up in the .ioc GUI BEFORE using this file):
 *
 *   1. Connectivity -> USB: enable USB_DEVICE
 *      Middleware -> USB_DEVICE: Class = "Communication Device Class (Virtual Port Com)"
 *
 *   2. Timers -> TIM2:
 *        Channel1 = PWM Generation CH1   (PA0 -> DRV8833 IN1)
 *        Channel2 = PWM Generation CH2   (PA1 -> DRV8833 IN2)
 *        Channel3 = PWM Generation CH3   (PA2 -> DRV8833 IN3)
 *        Channel4 = PWM Generation CH4   (PA3 -> DRV8833 IN4)
 *      Parameter Settings:
 *        Prescaler        = 3
 *        Counter Period   = 999      (ARR -> gives ~18 kHz PWM at 72 MHz APB1 timer clock)
 *        auto-reload preload = Enable
 *
 *   3. Generate Code.
 *
 * INTEGRATION STEPS (after code generation):
 *
 *   A. In Core/Src/usbd_cdc_if.c, inside CDC_Receive_FS(uint8_t* Buf, uint32_t *Len),
 *      add this loop right after Buf/Len are available (before the
 *      USBD_CDC_ReceivePacket() call at the end of the function):
 *
 *          for (uint32_t i = 0; i < *Len; i++) {
 *              motor_rb_push(Buf[i]);
 *          }
 *
 *   B. In Core/Src/main.c:
 *369*        - Add  #include "motor_control_v1.c"   (or turn this into a proper
 *              motor_control_v1.h / .c pair and #include the header instead —
 *              recommended once this is working, kept as one file for V1 simplicity)
 *        - After MX_TIM2_Init() and before the while(1) loop, call:
 *              motor_init();
 *        - Inside the while(1) loop, call every iteration:
 *              motor_loop();
 *
 * COMMAND PROTOCOL (host -> Blue Pill, 4 bytes, sent over USB CDC):
 *
 *   Byte 0: header       = 0xAA
 *   Byte 1: left_speed   = int8_t, -100..100
 *   Byte 2: right_speed  = int8_t, -100..100
 *   Byte 3: checksum     = header ^ left_speed ^ right_speed
 *
 * SAFETY: if no valid packet arrives for 200ms, both motors are forced to 0.
 * This is the watchdog against a stalled USB link or a crashed host app.
 * ============================================================
 */

#include "main.h"
#include <string.h>
#include <stdlib.h>

/* ---- Packet definition ------------------------------------ */
typedef struct __attribute__((packed)) {
    uint8_t header;       // 0xAA
    int8_t  left_speed;   // -100..100
    int8_t  right_speed;  // -100..100
    uint8_t checksum;     // header ^ left_speed ^ right_speed
} motor_cmd_t;

/* ---- Ring buffer for USB RX bytes --------------------------
 * Bytes land here from the CDC_Receive_FS() callback (interrupt/USB
 * context). Parsing happens in motor_loop() from the main loop instead,
 * so we never do checksum/parsing work inside the USB callback.
 * ------------------------------------------------------------*/
#define RB_SIZE 64
static volatile uint8_t  rb_buf[RB_SIZE];
static volatile uint16_t rb_head = 0, rb_tail = 0;

static void rb_push(uint8_t byte) {
    uint16_t next = (rb_head + 1) % RB_SIZE;
    if (next != rb_tail) {          // silently drop byte if buffer is full
        rb_buf[rb_head] = byte;
        rb_head = next;
    }
}

static int rb_pop(uint8_t *byte) {
    if (rb_tail == rb_head) return 0;   // empty
    *byte = rb_buf[rb_tail];
    rb_tail = (rb_tail + 1) % RB_SIZE;
    return 1;
}

/* Call this from CDC_Receive_FS() in usbd_cdc_if.c — see step A above. */
void motor_rb_push(uint8_t byte) {
    rb_push(byte);
}

/* ---- Motor state --------------------------------------------*/
extern TIM_HandleTypeDef htim2;
static volatile int8_t  left_speed  = 0;
static volatile int8_t  right_speed = 0;
static volatile uint32_t last_cmd_tick = 0;

#define PWM_MAX     999     // must match TIM2 ARR
#define CMD_TIMEOUT 200     // ms with no valid packet -> force stop

/* Applies a -100..100 speed to each motor via PWM/DIR-as-PWM/PWM. */
static void motor_apply(int8_t speed_left, int8_t speed_right) {
    // Motor A: IN1 (TIM2_CH1) = forward PWM, IN2 (TIM2_CH2) = reverse PWM

    if (speed_left >= 0) {
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, (uint32_t)(speed_left * PWM_MAX) / 100);
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0);
    } else {
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, (uint32_t)(-speed_left * PWM_MAX) / 100);
    }

    // Motor B: IN3 (TIM2_CH3) = forward PWM, IN4 (TIM2_CH4) = reverse PWM
    if (speed_right >= 0) {
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, (uint32_t)(speed_right * PWM_MAX) / 100);
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 0);
    } else {
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
        __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, (uint32_t)(-speed_right * PWM_MAX) / 100);
    }
}

/* ---- Packet parser: drains the ring buffer, looking for a
 *      valid 4-byte packet. Resyncs on the 0xAA header byte if
 *      the stream is ever misaligned. Call every main-loop tick. ----*/
static void motor_process_rx(void) {
    static uint8_t pkt[4];
    static uint8_t pkt_idx = 0;
    uint8_t b;

    while (rb_pop(&b)) {
        if (pkt_idx == 0 && b != 0xAA) {
            continue;  // not a header byte, drop it and keep scanning
        }
        pkt[pkt_idx++] = b;

        if (pkt_idx == 4) {
            uint8_t chk = pkt[0] ^ (uint8_t)pkt[1] ^ (uint8_t)pkt[2];
            if (chk == pkt[3]) {
                left_speed     = (int8_t)pkt[1];
                right_speed    = (int8_t)pkt[2];
                last_cmd_tick  = HAL_GetTick();
            }
            // whether valid or not, reset for the next packet
            pkt_idx = 0;
        }
    }
}

/* ---- Call once, after MX_TIM2_Init(), before the main while(1) ----*/
void motor_init(void) {
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);
    motor_apply(0, 0);           // motors must come up silent
    last_cmd_tick = HAL_GetTick();
}

/* ---- Call every iteration of the main while(1) loop ----*/
void motor_loop(void) {
    motor_process_rx();

    if (HAL_GetTick() - last_cmd_tick > CMD_TIMEOUT) {
        left_speed  = 0;
        right_speed = 0;
    }

    motor_apply(left_speed, right_speed);
}
