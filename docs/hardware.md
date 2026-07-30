# Hardware and bring-up

## Minimum transmitter

An ExpressLRS **receiver is not sufficient**. RivetTX generates channel data
and sends it over CRSF to an ExpressLRS **transmitter module**. The minimum
useful radio therefore contains:

- ESP32-C3 or ESP32-S3 with at least 4 MiB flash
- ExpressLRS TX module with a full-duplex 3.3 V CRSF UART
- two two-axis gimbals or four other analog controls
- one dedicated maintained ARM switch and up to three maintained AUX switches
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

The four optional AUX inputs are active low and use internal pull-ups. Wire
each maintained switch between its configured GPIO and ground. AUX1 maps to
CH5 and is the dedicated Betaflight/ExpressLRS arming switch; AUX2-AUX4 map to
CH6-CH8. Disabled inputs remain low. See the
[Betaflight setup guide](betaflight.md) before assigning GPIOs or modes.

## Controls

- Hold UP+DOWN during boot to enter stick calibration.
- During calibration, ENTER advances and BACK cancels.
- BACK cycles through normal screens.
- UP/DOWN select a field; ENTER enters or leaves edit mode.
- In edit mode UP/DOWN changes the value.
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
3. Calibrate and verify CH1-CH4 reach the intended limits and CH5-CH8 follow
   only their dedicated maintained switches.
4. Scope the CRSF UART and measure the worst control-loop time.
5. Connect the TX module at minimum RF power and verify telemetry/model ID.
6. Verify mW selection, dynamic power, bind, Finder, and the module's Wi-Fi
   update mode.
7. With a receiver and flight controller, verify RSSI dBm, link quality, and
   TX power independently in the Betaflight OSD.
8. Test loss of telemetry, input disconnect, low battery, brownout,
   watchdog reset, corrupt model storage, and failed OTA.
9. Perform hardware-in-the-loop soak tests before removing propellers or
   otherwise making a vehicle capable of motion.
10. Provision unique signing keys and a unique recovery password before applying
   the production security configuration.

The buzzer must be a passive piezo suitable for GPIO drive or use an
appropriate transistor driver. Set its GPIO and duty percentage under
`Component config -> RivetTX hardware`; the default `-1` keeps audio disabled.
See the [ExpressLRS guide](expresslrs.md) for the module/receiver distinction,
Finder behavior, Betaflight path, and update workflow. The
[audio-alert guide](audio-alerts.md) lists every pattern and its priority.
