# Feature matrix

This matrix separates implemented software from claims that require real
hardware evidence.

| Area | Implemented | Verification |
|---|---|---|
| inputs | ADC sampling, calibration, deadband, filtering, inversion | native unit tests; ESP ADC needs board test |
| mixer | inputs, expo, curves, 64 mixes, add/multiply/replace, delay, slow, trims, GVars, five flight modes | native unit tests |
| outputs | 16 channels, reverse, subtrim, min/max, per-channel failsafe | native unit tests |
| logic | 24 logical switches, edge/sticky/timer functions, three timers, 24 special-function slots | native unit tests |
| safety | locked boot, throttle/switch checks, stale input, mixer deadline, low battery, watchdog recovery, failsafe gating | native unit tests; HIL required |
| ExpressLRS | CRSF channels/CRC, telemetry parser, discovery, model ID, bind, failsafe capture, baud recovery, pass-through API | protocol tests; real ELRS module pending |
| UI | responsive compact/medium/large layout, 13 screens, scrolling/editing, touch events, staged model edits | 128x64 simulator and native tests |
| displays | capability-driven layout and monochrome canvas; SSD1306 128x64 driver | simulator; SSD1306 hardware pending |
| Lua | real Lua 5.5, allocator ceiling, instruction/time budget, safe libraries, LCD/model/telemetry/CRSF APIs and common ELRS names | code-level integration; real ELRS Lua tool pending |
| storage | versioned schema, CRC, migration hook, copy-on-write save, read-back, backup recovery | corruption/recovery tests |
| diagnostics | bounded event ring, crash snapshot, reset reason and ESP core-dump partition | native tests; reset injection pending |
| telemetry log | rate-limited CSV logging and flush | native tests; flash endurance pending |
| battery/power | calibrated ADC when eFuse data exists, divider scaling, hysteresis, alarms, inactivity/shutdown policy | native policy tests; divider validation pending |
| backup/recovery | locked-only Wi-Fi export/restore, boot-failure counter, held-button recovery | codec tests; portal hardware pending |
| update/boot | ESP-IDF bootloader, A/B OTA, HTTPS, manifest gates, post-boot self-test, rollback, Secure Boot V2 production config | mock OTA tests; signed-device drill pending |
| development | deterministic host simulator, PBM screen output, strict-warning tests, sanitizer run | verified locally |

## Deliberate boundaries

- Lua, UI, filesystem, Wi-Fi, logging, and OTA never execute in the
  flight-critical control task.
- Model edits are staged and activated only after reboot. This trades instant
  editing for a simple, auditable model boundary.
- The current physical display driver is monochrome SSD1306. Adding a display
  means implementing a sink/canvas backend and declaring its capabilities; the
  screen data and responsive layout do not depend on 128x64 coordinates.
- The pass-through and update services have safe core/platform APIs, but a
  production product should choose its authenticated USB or maintenance UI
  workflow after the PCB is fixed.
