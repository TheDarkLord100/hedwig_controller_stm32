# Hedwig — Blue Pill V1 Firmware

Firmware for the STM32F103C8T6 "Blue Pill" board that drives Hedwig's two
motors via a DRV8833 dual H-bridge, controlled by 4-byte commands sent
over serial from a phone (or any USB host). This is **V1**: motor control
only — no encoders, no ultrasonic, no closed-loop anything yet.

## Hardware

- **MCU board:** STM32F103C8T6 "Blue Pill" clone, USB-C
- **Motor driver:** DRV8833 dual H-bridge breakout
- **Important:** this board's USB-C port is wired through an onboard
  **CH340 USB-to-serial chip**, not the STM32's native USB peripheral.
  The MCU talks over **USART1** (PA9=TX, PA10=RX); the CH340 bridges
  that to a standard serial port (`/dev/ttyUSB0` on Linux) on whatever
  it's plugged into.

### Pin mapping

| Signal | Pin | Notes |
|---|---|---|
| DRV8833 IN1 | PA0 | TIM2_CH1 — Motor, forward PWM |
| DRV8833 IN2 | PA1 | TIM2_CH2 — Motor, reverse PWM |
| DRV8833 IN3 | PA2 | TIM2_CH3 — Motor, forward PWM |
| DRV8833 IN4 | PA3 | TIM2_CH4 — Motor, reverse PWM |
| DRV8833 EEP (sleep/enable) | — | Wired directly to Blue Pill 3V3, not to an MCU pin. Must be high or the driver stays in low-power sleep and ignores all PWM input. |
| DRV8833 GND | — | Common ground with Blue Pill and battery |
| DRV8833 VCC | — | Battery positive (also powers the logic side on this breakout — check your battery voltage is within the DRV8833's logic-safe range) |
| USART1 TX/RX | PA9 / PA10 | Internal to the board — goes to the onboard CH340, not exposed externally |

**Left/right and forward/reverse are corrected in software**, not by wiring —
see `motor_apply()` in `motor_control_v1.c`. If you rewire the motors or
driver, you may need to re-check/re-flip these.

### DRV8833 drive scheme

Each motor uses two PWM channels (not one PWM + one static direction pin):
whichever channel matches the commanded direction's sign gets the duty
cycle, the other stays at 0%. Both channels are still timer-driven, so
direction changes are glitch-free — this is a software behavior, not a
hardware DIR pin.

## Firmware architecture

- **`Core/Src/main.c`** — CubeMX-generated init (clocks, GPIO, TIM2, USART1)
  plus the USART1 receive-interrupt hook (`HAL_UART_RxCpltCallback`) that
  feeds bytes into the motor control module.
- **`Core/Src/motor_control_v1.c`** — ring buffer, command packet parser,
  PWM output, and the safety watchdog. This is the file to read to
  understand the actual control logic.
- **`Core/Src/motor_test.c`** — standalone bring-up test (spins both
  motors forward 5s / reverse 5s on a loop, no serial involved). Useful
  for verifying the DRV8833 + motor + wiring chain in isolation. Not
  wired into `main()` by default — swap the `motor_init()`/`motor_loop()`
  calls for `motor_test_init()`/`motor_test_loop()` to use it.
- **`hedwig_v1.ioc`** — CubeMX project file. Open this in CubeMX to see
  or regenerate the peripheral configuration (clocks: 72MHz from the
  8MHz HSE crystal via PLL x9; TIM2: 4x PWM channels, prescaler 3,
  ARR 999, ~18kHz; USART1: 115200 8N1, RX interrupt enabled).

Clock note: HCLK/SYSCLK run at 72MHz (needed for the TIM2 PWM math above).
USART1 doesn't need a specific clock like USB did, so no special prescaler
juggling is required here.

## Serial protocol

4-byte packets, host → MCU only (no acks, no telemetry back):

| Byte | Field | Type | Notes |
|---|---|---|---|
| 0 | header | `uint8` | always `0xAA` |
| 1 | left_speed | `int8` | -100..100, negative = reverse |
| 2 | right_speed | `int8` | -100..100, negative = reverse |
| 3 | checksum | `uint8` | `byte0 ^ byte1 ^ byte2` |

- Serial config: **115200 baud, 8 data bits, no parity, 1 stop bit**
- Host should send continuously at **≥20Hz**, including at zero speed —
  the firmware doesn't distinguish "no data" from "commanding zero."
- **200ms watchdog:** if no valid packet arrives within 200ms, both
  motors are forced to zero. This is the actual safety mechanism; the
  host's own send-rate logic is a separate, independent layer on top.
- Malformed/partial packets are dropped and the parser resyncs on the
  next `0xAA` byte — a single bad packet won't desync the whole stream.

### Quick manual test (Linux, no host app needed)

```bash
stty -F /dev/ttyUSB0 115200 cs8 -cstopb -parenb raw -echo

# left=50, right=-50 (checksum: 0xAA ^ 0x32 ^ 0xCE = 0x18)
printf '\xAA\x32\xCE\x18' > /dev/ttyUSB0
```

## Building and flashing

Built with **STM32CubeIDE 2.2.0** and **STM32CubeMX** (separate apps as of
CubeIDE 2.x — CubeMX generates the project, CubeIDE builds/flashes it).

### Building
Open the project in CubeIDE and build normally (Project → Build Project).

### Flashing — two options

**Preferred: UART bootloader via the CH340/USB-C connection** (no ST-Link
needed):
1. Hold the **BT0** button, plug in USB-C (or tap RST) while holding it,
   release after ~2 seconds — this boots into the STM32's built-in ROM
   bootloader instead of your firmware.
2. Open **STM32CubeProgrammer**, switch connection type to **UART**,
   port `/dev/ttyUSB0`, baud 115200.
3. **Parity must be set to Even** for the bootloader handshake — this is
   different from the firmware's own 8N1/no-parity config and easy to
   miss.
4. Connect, browse to `Debug/hedwig_v1.elf`, Start Programming.
5. Power-cycle the board normally (no BT0 held) to boot into the new
   firmware.

**Alternative: ST-Link via SWD** — standard CubeIDE Run/Debug flow, if you
have a working SWD connection to the board's debug pads. Note that only
UART flashing has been reliably tested on this specific board; SWD access
depends on your own wiring to the (unlabeled, SMD) debug pads.

## Repo contents

- `Core/`, `Drivers/` — firmware source and vendored ST HAL/CMSIS libraries
- `hedwig_v1.ioc` — CubeMX project/peripheral configuration
- `STM32F103C8TX_FLASH.ld` — linker script
- `.project`, `.cproject`, `.mxproject` — Eclipse/CubeIDE project files
  (needed to reopen this as a CubeIDE project as-is)
- `Debug/` is gitignored — build output, regenerated on every build

## Known issues / not yet done

- No encoders, no ultrasonic — V1 is motor control only
- No telemetry back to host — one-directional serial only
- SWD debug access is currently unreliable on this board (fragile
  hand-soldered leads on SMD debug pads); UART bootloader flashing is
  the working path