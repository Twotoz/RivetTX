# Architecture

## Safety invariants

1. The control task does not allocate memory, access flash, draw UI, run Lua,
   or wait on Wi-Fi.
2. A channel frame is transmitted only when its input timestamp is fresh and
   the mixer completed before its deadline.
3. Boot, calibration, storage corruption, low battery, watchdog recovery, and
   unhandled faults all enter a safe-output state.
4. Lua and UI failures are isolated and logged; neither can stop CRSF output.
5. Model writes are copy-on-write and verified before becoming active.
6. New OTA firmware must pass its startup self-test or the bootloader rolls
   back to the previously valid image.

## Tasks

| Task | Typical period | Priority | May access flash? |
|---|---:|---:|---|
| control | 2-10 ms, synchronized to module | highest | no |
| CRSF RX | event driven | high | no |
| safety/watchdog | 10 ms | high | no |
| telemetry alarms | 50 ms | medium | no |
| UI | 20-100 ms | low | no |
| Lua | cooperative budget | low | no |
| storage/log flush | idle/event | lowest | yes |
| Wi-Fi/OTA/backup | explicit maintenance mode | lowest | yes |

Flash operations and OTA are prohibited while the transmitter is armed.

On the single-core ESP32-C3 all tasks share core 0 and are separated by
priority. On the dual-core ESP32-S3 the control task is pinned to core 1 while
UI and service tasks are pinned to core 0. The same fixed-allocation control
path and safety gates are used on both targets.

## Hardware profiles

The target layer supplies validated GPIO/ADC mappings and scheduling. Above
that layer, the application sees capabilities rather than concrete
peripherals:

```text
HardwareProfile
|- display: width, height, color depth, touch, partial refresh
|- inputs: axes, switches, buttons, encoder
|- module: CRSF baud rates, power control, pass-through
|- storage: model capacity and log capacity
`- power: battery sensing, shutdown control, charging state
```

A display driver implements pixel transfer. The common canvas and layout
engine render the same screen tree at three responsive densities:

- compact: 128x64 and similar
- medium: 128x128 through 320x240
- large: 320x240 and above, optionally touch

Normal screens declare rows and fields. Only custom telemetry widgets use
absolute canvas coordinates.

## Model storage

Each file consists of a fixed header and a deterministic little-endian payload:

```text
magic | schema | payload size | generation | payload CRC | payload
```

Saving `model.rvm` performs:

1. encode to `model.rvm.new`
2. read back and validate
3. move valid current file to `model.rvm.bak`
4. atomically rename `.new` to the active file
5. recover `.bak` on the next boot if the active file is corrupt

Migrations happen after decoding and before a model is activated.
Both ESP32 targets mount the `models` partition as wear-levelled FATFS; the
codec and transaction protocol are filesystem-independent.

## Updates and recovery

The ESP-IDF second-stage bootloader owns A/B selection. RivetTX:

- downloads only over authenticated HTTPS
- checks project name, target, minimum compatible model schema, and version
- relies on Secure Boot V2 for production signature validation
- marks a new image valid only after storage, input, display, CRSF, and task
  self-tests pass
- requests rollback after repeated startup failures
- enters recovery/maintenance mode when the recovery button is held

## Lua boundary

Lua is a low-priority client of a message-oriented API. It receives snapshots
of sources, telemetry, UI events, and model metadata. It can submit validated
configuration transactions and CRSF management frames.

Every invocation has:

- an instruction/time budget
- a memory ceiling
- a bounded mailbox
- script paths restricted to `/models/scripts`
- no raw GPIO, UART, flash, or watchdog access

The native control task keeps operating if a script is killed.

## Audio boundary

Control-path state changes publish fixed-size bits and snapshots to a single
service-task audio scheduler. Pattern playback is non-blocking and uses no
dynamic allocation. Critical link, battery, and telemetry-loss patterns
pre-empt Finder and Lua tones, so competing clients cannot hide a safety
warning or delay channel generation.
