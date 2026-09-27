/* ============================================================
 * HEDWIG - Motor Bring-Up Test (bypasses UART entirely)
 * Spins both motors forward for 5s, then reverse for 5s, repeat.
 * Purpose: verify DRV8833 + motor + battery + wiring chain
 * WITHOUT involving the serial command path at all.
 *
 * INTEGRATION (temporary — for testing only):
 *
 *   In Core/Src/main.c:
 *     1. Comment out (or leave, doesn't matter): motor_init();
 *        HAL_UART_Receive_IT(&huart1, &uart_rx_byte, 1);
 *     2. Add near those lines:  motor_test_init();
 *     3. Inside while(1), REPLACE motor_loop(); with:
 *            motor_test_loop();
 *
 *   Add this file to Core/Src the same way motor_control_v1.c
 *   was added (drag into the folder, right-click Core -> Refresh).
 *
 *   To go back to normal serial-controlled operation later,
 *   just revert those main.c lines back to motor_init() / motor_loop()
 *   and remove/stop calling motor_test_init() / motor_test_loop().
 * ============================================================
 */

#include "main.h"

extern TIM_HandleTypeDef htim2;

#define PWM_MAX     999     // matches TIM2 ARR
#define TEST_DUTY   700     // ~70% duty -> a clearly visible/audible speed,
                             // not full 100% so it's gentler on a first test

void motor_test_init(void) {
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_1);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_2);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_3);
    HAL_TIM_PWM_Start(&htim2, TIM_CHANNEL_4);

    // motors must come up silent
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 0);
}

/* Blocking test sequence — fine for a bring-up test, not meant
 * to coexist with anything else running in the loop. */
void motor_test_loop(void) {
    // ---- FORWARD: both motors, 5 seconds ----
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, TEST_DUTY); // Motor A forward
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, TEST_DUTY); // Motor B forward
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 0);
    HAL_Delay(5000);

    // ---- brief stop between directions (avoids a hard direction flip) ----
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 0);
    HAL_Delay(300);

    // ---- REVERSE: both motors, 5 seconds ----
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, TEST_DUTY); // Motor A reverse
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, TEST_DUTY); // Motor B reverse
    HAL_Delay(5000);

    // ---- brief stop before looping back to forward ----
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_2, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_4, 0);
    HAL_Delay(300);
}
