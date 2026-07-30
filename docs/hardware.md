# Hardware and bring-up

## Minimum transmitter

An ExpressLRS **receiver is not sufficient**. RivetTX generates channel data
and sends it over CRSF to an ExpressLRS **transmitter module**. The minimum
useful radio therefore contains:

- ESP32-C3 with at least 4 MiB flash
- ExpressLRS TX module with a full-duplex 3.3 V CRSF UART
- two two-axis gimbals or four other analog controls
- SSD1306-compatible 128x64 I2C OLED
- UP, DOWN, ENTER, and BACK buttons
- regulator sized for the ESP32-C3 and the peak current of the chosen TX module
- common ground, local bulk capacitance at the module, and a power switch
- passive piezo buzzer when audible Finder feedback is wanted

Battery sensing needs a protected resistor divider whose maximum ADC pin
voltage stays inside the ESP32-C3 limit. USB is strongly recommended for
initial flashing and recovery. An audible alarm, haptic motor, module power
switch, current sensor, SD card, charger, and controlled power latch are useful
later but are not required by the first profile.

Never power a high-power external module from a development board's 3.3 V pin.
Use the module manufacturer's voltage and current limits.

## Default development wiring

The defaults are examples, not a PCB design:

| Function | Default GPIO |
|---|---:|
| gimbal axes 0-3 | 0, 1, 2, 3 |
| OLED SDA / SCL | 4 / 5 |
| CRSF TX / RX | 6 / 7 |
| UP / DOWN | 8 / 9 |
| ENTER / BACK | 10 / 20 |
| battery ADC | disabled |
| passive piezo buzzer | disabled |

Battery low/critical voltage and ELRS weak/critical LQ thresholds are also
configured in this menu. The default battery values assume a one-cell
development supply and must be changed for other pack configurations.

Some GPIOs are boot-strapping pins on common ESP32-C3 boards. Confirm the
module datasheet and board schematic, then change every assignment with
`idf.py menuconfig`.

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

## Required bring-up sequence

1. Build and flash development settings with the RF module disconnected.
2. Verify all ADC directions and button levels in a serial log.
3. Calibrate and verify that every channel reaches the intended limits.
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
