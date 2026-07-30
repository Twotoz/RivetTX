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
| UI | responsive compact/medium/large layout; OpenPocket home with two live gimbal plots, link/battery/ELRS/VRX status, prioritized warning banner and warning list; Home → Menu → Detail navigation; scrolling/editing and touch events | 128×64, 240×135, and 480×320 simulator renders plus native tests; target display endurance pending |
| displays | capability-driven layout and monochrome canvas; SSD1306 128x64 driver | three virtual profiles; SSD1306 hardware pending |
| Lua | real Lua 5.5, allocator ceiling, instruction budgets for load/init/runtime, restricted libraries and script paths, LCD/model/telemetry/CRSF APIs, active `RSSI`, telemetry-age query, and bounded tone output | ESP32-C3/S3 target builds; real buzzer/script test pending |
| storage/models | versioned schema, CRC and migration; 32 transactional model slots; active-index recovery; create, copy, select, delete, import/export; verified mirror recovery without automatic formatting | native corruption, migration, library and recovery tests; target power-cut/storage-full campaign pending |
| diagnostics | bounded event ring, crash snapshot, reset reason and ESP core-dump partition | native tests; reset injection pending |
| telemetry log | special-function opt-in, exclusive locked maintenance transaction, rate-limited CSV, 64 KiB rotation and propagated write/flush failure | native success/failure tests; flash endurance and target timing pending |
| battery/power | synchronized validated ADC snapshot, calibrated divider scaling, fail-closed sensor faults, hysteresis, voltage-based percentage fallback, alarms and inactivity/shutdown policy | native policy/fault tests; real charger, fuel gauge, latch and divider validation pending |
| backup/recovery | locked-only Wi-Fi page for active-model export/verified restore, boot-failure counter, held-button recovery | codec/library tests; complete web configurator and portal hardware pending |
| USB simulator | native ESP32-S3 TinyUSB HID gamepad, four gimbals plus four analog controls and switch buttons, output lock and per-model RF lock; C3 explicitly unsupported | host policy tests and S3 CI build; Windows/Linux/macOS and simulator compatibility pending |
| VRX/video OSD | non-blocking 6×8 frequency controller and scan state machine, RSSI/video-loss state, fixed 30×16 character compositor | native tests only; no physical VRX or AT7456E-class driver until the OpenPocket hardware profile is frozen |
| onboarding | automatic missing-calibration entry and bounded first-run state machine for calibration, ARM/AUX, ELRS, optional video, battery and CH5-low preview | native state-machine tests; the post-calibration UI flow remains to be connected |
| update/boot | ESP-IDF bootloader, A/B OTA, HTTPS, manifest gates, post-boot self-test, rollback, Secure Boot V2 production config | mock OTA tests; signed-device drill pending |
| development | deterministic virtual hardware simulator, JSON report, PBM outputs, strict-warning tests, ASan/UBSan, dual ESP-IDF target builds with firmware headroom gates | host, ESP32-C3, and ESP32-S3 CI verified |

## Deliberate boundaries

- Lua, UI, filesystem, Wi-Fi, logging, and OTA never execute in the
  flight-critical control task.
- Model edits and model selection are saved only while outputs are locked.
  The control task atomically replaces the runtime model, resets mixer state,
  keeps CH5 low, and releases maintenance only after the hand-off completes.
- The current physical display driver is monochrome SSD1306. Adding a display
  means implementing a sink/canvas backend and declaring its capabilities; the
  screen data and responsive layout do not depend on 128x64 coordinates.
- The pass-through and update services have safe core/platform APIs, but a
  production product should choose its authenticated USB or maintenance UI
  workflow after the PCB is fixed.
- The VRX scanner and analog character compositor are hardware-independent
  cores, not proof of a working video path. A product claim requires the exact
  tuner, OSD IC, routing, NTSC/PAL behavior and target-PCB HIL evidence.
