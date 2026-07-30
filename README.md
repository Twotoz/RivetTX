# RivetTX

RivetTX is a compact, hardware-adaptive RC transmitter firmware for an
ESP32-C3 connected to an ExpressLRS transmitter module over CRSF.

The project deliberately separates the flight-critical control path from Lua,
the user interface, storage, Wi-Fi, logging, and firmware updates:

```text
ADC/GPIO -> input calibration -> mixer -> safety gate -> CRSF UART
                                      |
                 telemetry, Lua, UI, storage, logging, Wi-Fi
```

The first hardware profile is:

- ESP32-C3
- SSD1306-compatible 128x64 I2C OLED
- four analog stick axes
- configurable buttons and switches
- external ExpressLRS TX module using a full-duplex CRSF UART

RivetTX is not a fork of EdgeTX. Its model concepts and CRSF-facing Lua API are
designed to feel familiar, while the implementation is specific to one modern
platform and a capability-driven display system.

## Implemented foundation

- fixed-allocation real-time input, mixer, logical-switch, timer, and output path
- expo, curves, add/multiply/replace mixes, delays, slew rates, flight modes,
  trims, GVars, subtrim, reverse, and output limits
- safety state machine with throttle/switch checks, stale-input detection,
  deadline monitoring, battery lockout, and watchdog integration
- CRSF channel encoding, CRC-checked parsing, telemetry discovery, module
  liveness, model ID, bind, failsafe, and serial pass-through state
- bounded diagnostic ring log, crash snapshots, rate-limited telemetry logging,
  and persistent reset reasons
- versioned model codec with CRC and transactional save/backup/recovery
- capability-driven responsive UI for compact, medium, and large displays
- sandboxed Lua 5.4 with common EdgeTX/ELRS telemetry, CRSF, and LCD APIs
- calibration wizard, battery monitor, telemetry alarms, model backups, OTA
  validation/rollback interfaces, and production security configuration
- native simulator and tests for the safety-critical components

## Host build

The host build needs only GNU Make and a C++17 compiler:

```sh
make
make test
./build/rivettx-sim
```

## ESP32-C3 build

Install ESP-IDF 5.5 or newer, source its environment, then:

```sh
idf.py set-target esp32c3
idf.py menuconfig
idf.py build
idf.py flash monitor
```

The default GPIO assignments are examples. Set the actual board wiring under
`Component config -> RivetTX hardware`.

See [hardware and bring-up](docs/hardware.md) for the minimum electronics and
controls. RivetTX requires an ExpressLRS **TX module**; an ELRS receiver cannot
transmit control data.

For production, merge `sdkconfig.production.defaults` only after the signing
and recovery procedure has been tested on disposable hardware. Secure-boot
eFuses are intentionally not enabled by the development defaults.

## Project status

This repository is an engineering foundation and simulator, not yet a
flight-proven transmitter release. Before controlling a real model, the exact
board needs electrical validation, calibrated ADC inputs, latency measurement,
brownout testing, RF-module power validation, and hardware-in-the-loop testing.

The name "RivetTX" is a working project name, not a trademark clearance.

The detailed implementation/validation status is in
[the feature matrix](docs/feature-matrix.md). The code is MIT licensed.
