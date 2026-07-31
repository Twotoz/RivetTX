# Hardware and bring-up

## Presentation profiles

RivetTX supports one presentation path per product build:

- A standalone RivetTX transmitter uses the SSD1306 OLED and buttons. This
  profile does not provide OpenPocket support.
- An OpenPocket product uses the analog character OSD for its menu and status
  display. It does not also initialize or mirror the menu on an OLED.

The OpenPocket profile has a physical AT7456E/MAX7456-compatible SPI backend.
It autodetects PAL/NTSC from the chip status register, drives all 30×16 cells
in PAL and the safe 30×13 region in NTSC, and retries chip communication
without running SPI in the control task. Target-PCB signal-integrity and
composite-video HIL are still required before flight.

## OpenPocket AT7456E wiring

Select `Use OpenPocket AT7456E analog OSD instead of SSD1306` under
`Component config -> RivetTX hardware`, then assign SCLK, MOSI/SDIN,
MISO/SDOUT, active-low CS and optional active-low RESET. There are deliberately
no default OSD GPIOs: the correct pins depend on the exact ESP32 module's
flash, PSRAM, USB, ADC and board routing. The SPI clock is configurable up to
the chip's 10 MHz limit; 8 MHz with one setup and one hold cycle is the
default. The firmware uses SPI mode 0 and the ESP32 SPI2 peripheral.

Complete signal path:

```text
antenna -> VRX -> VRX CVBS OUT -> AT7456E VIN
                                      |
ESP32 SPI2 -> level shifting -> AT7456E SPI/control
                                      |
composite LCD CVBS IN <- AT7456E VOUT-+

VRX GND ---------+
ESP32 GND -------+--- AT7456E AGND/DGND --- LCD video GND
5 V video rail ------ AT7456E AVDD/DVDD
3.3 V rail ---------- ESP32 (and 3.3 V side of translators)
```

| ESP32 / board net | AT7456E module net | Direction and requirement |
|---|---|---|
| configured SCLK | SCLK | ESP32 to OSD, SPI mode 0 |
| configured MOSI | SDIN / MOSI | ESP32 to OSD |
| configured MISO | SDOUT / MISO | OSD to ESP32; level-shift to 3.3 V |
| configured CS | CS | ESP32 to OSD, active low; add a pull-up so it stays deselected during boot |
| optional RESET | RESET | active low; use a 5 V-safe open-drain stage or translator and a pull-up at the OSD |
| VRX CVBS OUT | VIDEO IN / VIN | one composite path through the OSD, AC-coupled as required by the module/reference circuit |
| VIDEO OUT / VOUT | LCD CVBS IN | AC-coupled composite output; LCD is the single 75 ohm load |
| common video ground | AGND, DGND, module GND | low-impedance common reference; do not route video return through RF or digital switching current |
| regulated 5 V | AVDD, DVDD, module +5V | local 100 nF at each supply pin plus at least 10 uF bulk at the OSD module |

For a ready-made AT7456E OSD module, use its `VIDEO IN` and `VIDEO OUT` pins;
the module must already contain the 27 MHz clock, supply decoupling, video
coupling and SAG/COUT network. For a bare IC, copy the oscillator, VIN/VOUT,
SAG/COUT, clamp/bias and decoupling network from the exact AT7456E vendor
reference schematic. Do not substitute the similarly programmed MAX7456
reference values without checking the chosen AT7456E package and revision.

The AT7456E video rail is normally 5 V. Do not assume its digital pins are
3.3 V-safe. A robust bare-chip design uses 3.3-to-5 V AHCT-family buffers for
SCLK, SDIN and CS, and a 5-to-3.3 V unidirectional translator (or a calculated
divider at the selected SPI rate) for SDOUT. RESET is simplest as an
open-drain transistor. Some modules include this translation; verify the
schematic rather than the product label.

The LCD input, not the OSD output, should provide the one 75 ohm termination.
Do not tee an independently terminated direct VRX-to-LCD path around the OSD,
and do not connect baseband composite to an LCD's audio or power pin. Power
the VRX and LCD from rails sized for their peak loads, keep their noisy supply
returns out of the video return, and add ESD protection where video leaves
the PCB.

The driver reads LOS/PAL/NTSC status every 100 ms. On loss it leaves video
pass-through and the last safe standard configured, reports `VIDEO NO SIGNAL`,
and keeps controls running. A recovered or changed standard invalidates the
shadow screen and redraws it. Display RAM writes are run-based and include
only changed cells. Custom characters are 54-byte glyphs; uploads disable only
the OSD overlay while the chip programs NVM and are staged one bounded SPI
operation at a time. Because character NVM has finite endurance, provision
icons, selection markers and warning glyphs during setup or maintenance, not
on every boot.

Register behavior and the required external video network are compatible with
the [MAX7456 reference data sheet](https://www.analog.com/media/en/technical-documentation/data-sheets/max7456.pdf),
but the populated AT7456E vendor data sheet and module schematic remain the
authority for electrical limits.

## OpenPocket RX5808 / RTC6715 wiring

Enable `Physical RX5808 / RTC6715 receiver` under
`Component config -> RivetTX hardware`. Configure four separate pins for
DATA, CLK, LE/latch enable, and RSSI ADC. The ESP32-C3 and ESP32-S3 backends
use a bounded GPIO state machine for the RTC6715's 25-bit, LSB-first,
mode-0-compatible serial write. It does not use delays or execute in the
250 Hz control task.

The supported hardware is an RX5808-class module with the RTC6715
three-wire programming interface actually enabled and exposed. Many stock
RX5808 modules ship in fixed channel-pin mode. They require the known
RX5808 SPI modification: the exact module schematic must be checked, its
RTC6715 `SPI_SE` function must be asserted, and the former channel-select
pads must be converted to DATA, LE, and CLK. Resistor locations and pad names
vary by module revision, so RivetTX does not publish one photograph as a
universal modification recipe. Verify continuity and logic levels before
connecting the ESP32.

Complete receiver path:

```text
regulated, filtered 5 V -------------------------- RX5808 +5V
common ground ------------------------------------ RX5808 GND
5.8 GHz antenna ---------------------------------- RX5808 RF IN

ESP32 configured DATA ---------------------------> RX5808 DATA
ESP32 configured CLK ----------------------------> RX5808 CLK
ESP32 configured LE -----------------------------> RX5808 LE / SELECT
ESP32 configured ADC1 input <--- scale/filter ---- RX5808 RSSI

RX5808 VIDEO OUT ---> AT7456E VIDEO IN / VIN
AT7456E VIDEO OUT ---> composite LCD controller CVBS IN

ESP32 GND ----------+
RX5808 GND ---------+---- common digital and video reference
AT7456E GND --------+
LCD controller GND -+
```

| Connection | Requirement |
|---|---|
| RX5808 +5V | use a regulated rail sized from the exact module; add local 100 nF ceramic and at least 10 uF low-ESR bulk capacitance |
| RX5808 GND | short common reference to the ESP32 digital ground and AT7456E video ground; keep regulator and RF return current out of the composite return |
| VIDEO OUT | route once to AT7456E VIN through the module/reference coupling network; never drive an ESP32 GPIO and do not tee a second terminated LCD path |
| DATA | dedicated 3.3 V output GPIO to the SPI-enabled RTC6715 DATA net |
| CLK | dedicated 3.3 V output GPIO to the SPI-enabled RTC6715 CLK net |
| LE / SELECT | dedicated 3.3 V output GPIO to the RTC6715 latch net; idle high |
| RSSI | dedicated ADC1-capable GPIO; add an RC filter and scale/clamp only if measurement proves the module output can exceed the ESP32 ADC limit |

Place a ferrite bead or suitably rated low-noise filter between a noisy shared
5 V converter and the receiver rail, followed by local bulk and ceramic
decoupling. Keep DATA/CLK/LE edges, the ESP32 clock, and DC/DC switch node away
from RF input and composite video. Do not power the module from an unverified
development-board rail.

The frequency table remains in the platform-independent controller. Every
requested value is checked against that 6x8 table before the physical backend
generates Synthesizer Register B and its 25-bit write frame. Tuning, settle
time, scan dwell, RSSI sample interval, and tune/stale timeouts are bounded and
configurable. A low-priority VRX task advances only a small fixed number of
GPIO transitions per tick.

RSSI calibration uses menuconfig minimum/noise and maximum-signal ADC values.
RivetTX applies an integer IIR filter and displayed-percent hysteresis, and
reports RSSI as valid, stale, unavailable, or sensor-faulted. It never turns a
failed ADC read into a plausible percentage. RSSI strength and composite sync
are separate: the RX5808 provides RSSI, while only the AT7456E status register
provides PAL/NTSC sync and no-signal state.

An RTC6715 write-only interface cannot prove RF operation or detect every
disconnected wire. `available` means that the configured GPIO transfer
completed; real receiver presence still requires RSSI, video-sync, and HIL
evidence. A failed VRX initialization, tune, or ADC read disables/degrades only
the receiver display state. It does not stop CRSF, the control task, telemetry,
or safety processing.

RTC6715 framing and synthesizer calculations follow the
[RTC6715 data sheet](https://cdn.ozdisan.com/ETicaret_Dosya/582271_6664727.PDF).
The physical RX5808 modification must follow the schematic and continuity of
the exact populated module.

## Minimum standalone transmitter

RivetTX generates channel data and sends it over CRSF to ExpressLRS firmware
running in **transmitter mode**. That firmware may run on a regular TX module
or on a supported ESP8285/ESP32 receiver that has deliberately been flashed
as a transmitter. A receiver running normal RX firmware is not sufficient.
The minimum useful radio therefore contains:

- ESP32-C3 or ESP32-S3 with at least 4 MiB flash
- ExpressLRS TX module, or supported RX flashed as TX, with a full-duplex
  non-inverted 3.3 V CRSF UART
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

When battery sensing is configured, RivetTX treats ADC setup/read failures as
a diagnosed sensor fault. A successful low or zero reading is a valid critical
voltage, not an absent sensor; either condition locks channel outputs.

Never power a high-power external module from a development board's 3.3 V pin.
Use the module manufacturer's voltage and current limits.

### ExpressLRS receiver flashed as transmitter

An RX-as-TX board is a transmitter after flashing and still needs a separate
ExpressLRS receiver in the aircraft. ESP8285 boards connect naturally to
RivetTX because RivetTX exposes separate, non-inverted CRSF TX and RX signals:

```text
RivetTX CRSF TX  -> converted module RX
RivetTX CRSF RX  <- converted module TX
RivetTX GND      --- converted module GND
regulated supply -> converted module VCC
```

Use the current ExpressLRS Configurator RX-as-TX option or the official web
flasher target appropriate for the exact radio chip and MCU. Before conversion:

1. Confirm the board works with its original receiver firmware.
2. Back up its original `hardware.json` from the ExpressLRS Web UI.
3. Flash the matching full-duplex RX-as-TX target without UART inversion.
4. Restore that same board's `hardware.json`; never use a file from a different
   receiver revision.
5. Power-cycle the module and connect it to RivetTX at 400 kbaud.
6. Start at the lowest RF power, verify telemetry, and confirm that the power
   choices advertised in RivetTX match the physical board.

Some receivers contain only the radio IC's low-power output; others include a
power amplifier and can advertise 50 mW, 100 mW, or more. Restoring the exact
hardware file is what supplies the firmware with the correct PA control pins
and power table. A label or seller claim alone is not enough to prove that
100 mW is safe. Size the regulator for peak current, add local bulk
capacitance, provide airflow where required, and verify RF output into an
antenna or suitable RF load.

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

## Firmware health versus RF readiness

For both standalone OLED and OpenPocket profiles, storage, calibrated inputs,
the selected presentation device, CRSF UART, required tasks, watchdog and
healthy deadline-compliant control cycles are essential firmware-health
dependencies. The external or replaceable ExpressLRS module is not: an
absent, slow, incompatible or reconnecting module cannot reject a pending OTA
image by itself.

RF readiness is a separate operational safety gate, not an OTA rollback
condition. Outputs, throttle and CH5 remain locked while the module is
starting or offline, and are locked again if an online module is lost. The UI
reports `ELRS OFFLINE` until the module is ready.

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

OpenPocket SCLK, MOSI, MISO, CS and RESET all default to disabled (`-1`) and
must be assigned for the actual PCB. Pin validation rejects a missing required
SPI signal, duplicate assignment, or a non-output-capable SCLK/MOSI/CS pin.

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
- On OpenPocket, ENTER on the minimal flying HUD opens the full menu. BACK or
  HOME from anywhere in that menu returns immediately to the HUD.
- UP/DOWN select a field; ENTER enters or leaves edit mode.
- In edit mode UP/DOWN changes the value.
- `EDIT` is shown at the top-right for as long as a value is being changed.
- A blocking startup warning replaces the gimbal view with the exact action
  needed. It clears automatically as soon as the live safety condition is
  valid again.
- The rotary encoder can replace UP/DOWN and ENTER for those menu actions.
- Trim buttons adjust the active flight mode live; press both directions to
  center the corresponding trim.
- Hold ENTER+BACK for one second to enable outputs or lock them again.
- Configuration changes are saved after one second while locked and are
  atomically activated without reboot; outputs and CH5 stay low throughout.
- On ESP32-S3, start USB Simulator from the menu only while locked. It exposes
  the controls as a native HID gamepad and keeps RF locked by default. ESP32-C3
  has no native programmable USB HID device.
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
