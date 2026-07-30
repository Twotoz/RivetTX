# Feature matrix

This matrix separates implemented software from claims that require real
hardware evidence.

| Area | Implemented | Verification |
|---|---|---|
| inputs | eight-axis ADC sampling, calibration, deadband, filtering, inversion; two-/three-position AUX switches; eight live trim contacts; quadrature encoder with press | native unit tests cover processing, debounce, trim repeat/center, and encoder decoding; ESP ADC/GPIO needs board test |
| mixer | inputs, expo, curves, 64 mixes, add/multiply/replace, delay, slow, per-flight-mode live trims, GVars, five flight modes | native unit tests |
| outputs | 16 channels, reverse, subtrim, min/max, per-channel failsafe | native unit tests |
| logic | 24 logical switches, edge/sticky/timer functions, three timers, 24 special-function slots | native unit tests |
| safety | locked boot, throttle/switch checks, stale input, mixer deadline, low battery, watchdog recovery, failsafe gating | native tests plus stale/deadline system scenarios; HIL required |
| ExpressLRS | CRSF channels/CRC, CH5 arming polarity, dynamic parameter discovery, packet-rate/model-match/mW/dynamic-power/switch-mode/telemetry-ratio writes, command bind, confirmed Wi-Fi update launch, model ID, telemetry, failsafe capture, offline recovery, pass-through API | virtual module tests with fragmented UART, settings read-back, commands, loss, corruption, disconnect and recovery; real ELRS module pending |
| Finder | active-antenna RSSI, one-second freshness gate, integer smoothing, dBm/bar display, signal-rate buzzer | native virtual-telemetry and tone tests; RF search test pending |
| audio alerts | fixed-allocation priority sequencer; link weak/critical/lost/recovered, module loss/recovery, TX battery low/critical, safety transitions, telemetry alarms, Finder and Lua | native pattern/pre-emption/policy tests plus audible disconnect simulation; buzzer hardware pending |
| UI | responsive compact/medium/large layout, 15 screens, scrolling/editing, touch events, staged model edits | 128×64, 240×135, and 480×320 simulator renders plus native tests |
| displays | capability-driven layout and monochrome canvas; SSD1306 128x64 driver | three virtual profiles; SSD1306 hardware pending |
| Lua | real Lua 5.5, allocator ceiling, instruction budgets for load/init/runtime, restricted libraries and script paths, LCD/model/telemetry/CRSF APIs, active `RSSI`, telemetry-age query, and bounded tone output | ESP32-C3/S3 target builds; real buzzer/script test pending |
| storage | versioned schema, CRC, migration hook, copy-on-write save, read-back, directory sync, and backup recovery without automatic formatting | corruption/recovery tests |
| diagnostics | bounded event ring, crash snapshot, reset reason and ESP core-dump partition | native tests; reset injection pending |
| telemetry log | special-function opt-in, rate-limited CSV logging, one-file rotation, and flush | native tests; flash endurance pending |
| battery/power | calibrated ADC when eFuse data exists, divider scaling, hysteresis, alarms, inactivity/shutdown policy | native policy tests; divider validation pending |
| backup/recovery | locked-only Wi-Fi export/restore, boot-failure counter, held-button recovery | codec tests; portal hardware pending |
| update/boot | ESP-IDF bootloader, A/B OTA, HTTPS, manifest gates, post-boot self-test, rollback, Secure Boot V2 production config | mock OTA tests; signed-device drill pending |
| development | deterministic virtual hardware simulator, JSON report, PBM outputs, strict-warning tests, ASan/UBSan, dual ESP-IDF target builds with firmware headroom gates | host, ESP32-C3, and ESP32-S3 CI verified |

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
