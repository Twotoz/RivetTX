# Hardware and bring-up

## Minimum transmitter

An ExpressLRS **receiver is not sufficient**. RivetTX generates channel data
and sends it over CRSF to an ExpressLRS **transmitter module**. The minimum
useful radio therefore contains:

- ESP32-C3 or ESP32-S3 with at least 4 MiB flash
- ExpressLRS TX module with a full-duplex 3.3 V CRSF UART
- two two-axis gimbals or four other analog controls
- one dedicated two-position ARM switch and up to three two- or three-position
  maintained AUX switches
- SSD1306-compatible 128x64 I2C OLED
- UP, DOWN, ENTER, and BACK buttons
- regulator sized for the chosen ESP32 and the peak current of the TX module
- common ground, local bulk capacitance at the module, and a power switch
- passive piezo buzzer when audible Finder feedback is wanted

Battery sensing needs a protected resistor divider whose maximum ADC pin
voltage stays inside the selected ESP32's ADC limit. USB is strongly recommended for
initial flashing and recovery. An audible alarm, haptic motor, module power
switch, current sensor, SD card, charger, and controlled power latch are useful
later but are not required by the first profile.

Never power a high-power external module from a development board's 3.3 V pin.
Use the module manufacturer's voltage and current limits.

## Target choice

Both targets run the same mixer, safety, CRSF, ELRS, UI, Lua, storage, OTA, and
audio code. Choose the C3 for a compact, inexpensive single-core board. Choose
the S3 when the extra GPIO, RAM, native USB options, or dual-core isolation are
useful. On a dual-core S3 build RivetTX pins the 250 Hz control task to core 1
and UI/service tasks to core 0. The C3 runs all three on its single core with
their existing priorities.

The full direct-GPIO control set (eight analog axes, three extra
three-position contacts, encoder, eight trim contacts, battery, and buzzer)
needs an S3 module/PCB with enough exposed pins; it does not fit alongside the
development defaults on a C3. A C3 can use any smaller subset. On either
target, verify ADC capability and flash/PSRAM/USB restrictions for every
selected pin.

Changing target regenerates the ESP-IDF configuration:

```bash
idf.py set-target esp32c3
# or
idf.py set-target esp32s3
```

Do not flash a C3 artifact to an S3 or vice versa. OTA manifests also reject a
target mismatch.

## Default development wiring

The defaults are examples, not a PCB design:

| Function | ESP32-C3 | ESP32-S3 |
|---|---:|---:|
| gimbal axes 0-3 | 0, 1, 2, 3 | 1, 2, 3, 4 |
| OLED SDA / SCL | 4 / 5 | 8 / 9 |
| CRSF TX / RX | 6 / 7 | 17 / 18 |
| UP / DOWN | 8 / 9 | 10 / 11 |
| ENTER / BACK | 10 / 20 | 12 / 14 |
| AUX1-4 switches | disabled | disabled |
| extra axes 4-7 | disabled | disabled |
| AUX2-4 LOW contacts | disabled | disabled |
| rotary encoder A / B / press | disabled | disabled |
| eight trim contacts | disabled | disabled |
| battery ADC | disabled | disabled |
| passive piezo buzzer | disabled | disabled |

Battery low/critical voltage and ELRS weak/critical LQ thresholds are also
configured in this menu. The default battery values assume a one-cell
development supply and must be changed for other pack configurations.

RivetTX rejects invalid and duplicate GPIO assignments during startup. That
cannot identify every board-level conflict: boot-strapping pins, USB pins, and
pins wired to flash or PSRAM depend on the exact module. In particular,
GPIO33-37 can be unavailable on ESP32-S3 modules that use octal flash or
octal PSRAM. Confirm the module datasheet and board schematic, then change
every assignment with `idf.py menuconfig`.

All optional digital controls are active low and use internal pull-ups. AUX1
maps to CH5 and remains a dedicated two-position Betaflight/ExpressLRS arming
switch. AUX2-AUX4 map to CH6-CH8. A two-position switch needs only its normal
AUX GPIO. For a three-position switch, wire its two outer contacts to the AUX
GPIO and matching `*_LOW_GPIO`, and its common contact to ground. The output is
-100% / 0% / +100%; activating both outer contacts together invalidates the
input frame. Disabled AUX inputs remain low.

Axes 4-7 accept analog scroll wheels, pots, or sliders and map to CH9-CH12 in
the default model. Enable these axes contiguously: axis 4 first, then 5, 6, and
7. Startup calibration automatically includes every configured axis.

The optional quadrature encoder uses A and B contacts plus an independent
press contact. Rotation selects fields or changes the current value; pressing
acts as ENTER. Swap A and B if its physical direction is reversed.

The eight trim GPIOs provide negative/positive buttons for AIL, ELE, THR, and
RUD. A short press changes the current flight mode's trim by 8 units, holding
repeats after 500 ms, and pressing both directions centers that trim. Changes
are used immediately and persisted through the normal model-save path. See the
[Betaflight setup guide](betaflight.md) before assigning GPIOs or modes.

## Controls

- Hold UP+DOWN during boot to enter stick calibration.
- During calibration, ENTER advances and BACK cancels.
- BACK cycles through normal screens.
- UP/DOWN select a field; ENTER enters or leaves edit mode.
- In edit mode UP/DOWN changes the value.
- The rotary encoder can replace UP/DOWN and ENTER for those menu actions.
- Trim buttons adjust the active flight mode live; press both directions to
  center the corresponding trim.
- Hold ENTER+BACK for one second to enable outputs or lock them again.
- Configuration changes are saved after one second while locked and become
  active on the next boot.
- Hold BACK during boot for the recovery Wi-Fi portal.
- Hold UP + DOWN + BACK during boot to explicitly reformat model storage
  after an unrecoverable mount failure. This erases stored models and
  calibration; a normal mount failure never formats automatically.

## Required bring-up sequence

1. Build and flash development settings with the RF module disconnected.
2. Verify all ADC directions and button levels in a serial log.
3. Calibrate and verify CH1-CH4 reach the intended limits, configured
   scroll/pot axes move CH9-CH12, and CH5-CH8 follow only their dedicated
   maintained switches at 1000/1500/2000 as applicable.
4. Verify every trim direction, hold repeat, center chord, and encoder
   direction/press before enabling RF output.
5. Scope the CRSF UART and measure the worst control-loop time.
6. Connect the TX module at minimum RF power and verify telemetry/model ID.
7. Verify mW selection, dynamic power, bind, Finder, and the module's Wi-Fi
   update mode.
8. With a receiver and flight controller, verify RSSI dBm, link quality, and
   TX power independently in the Betaflight OSD.
9. Test loss of telemetry, input disconnect, low battery, brownout,
   watchdog reset, corrupt model storage, and failed OTA.
10. Perform hardware-in-the-loop soak tests before removing propellers or
   otherwise making a vehicle capable of motion.
11. Provision unique signing keys and a unique recovery password before applying
   the production security configuration.

The buzzer must be a passive piezo suitable for GPIO drive or use an
appropriate transistor driver. Set its GPIO and duty percentage under
`Component config -> RivetTX hardware`; the default `-1` keeps audio disabled.
See the [ExpressLRS guide](expresslrs.md) for the module/receiver distinction,
Finder behavior, Betaflight path, and update workflow. The
[audio-alert guide](audio-alerts.md) lists every pattern and its priority.
