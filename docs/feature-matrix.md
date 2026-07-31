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
| OLED UI | standalone RivetTX home with two live gimbal plots, link/battery/ELRS status, prioritized warning banner/list, persistent EDIT indicator, Home → Menu → Detail navigation, scrolling/editing and touch events; it does not expose or claim the OpenPocket VRX/OSD menu | 128×64, 240×135, and 480×320 simulator renders plus native tests; target display endurance pending |
| displays | mutually exclusive presentation profiles: standalone SSD1306 OLED or OpenPocket AT7456E analog character OSD; configurable async SPI/CS, PAL/NTSC autodetection, 30×16 PAL and 30×13 NTSC, delta runs, LOS/standard recovery, retry/backoff and staged custom-glyph NVM uploads | OLED and AT7456E drivers plus native fake-SPI tests; target composite-video HIL remains pending |
| Lua | real Lua 5.5, allocator ceiling, instruction budgets for load/init/runtime, restricted libraries and script paths, LCD/model/telemetry/CRSF APIs, active `RSSI`, telemetry-age query, and bounded tone output | ESP32-C3/S3 target builds; real buzzer/script test pending |
| storage/models | versioned schema, CRC and migration; 32 transactional model slots; active-index recovery; create, copy, select, delete, import/export; verified mirror recovery without automatic formatting | native corruption, migration, library and recovery tests; target power-cut/storage-full campaign pending |
| removable microSD | dedicated ESP32-S3 SDMMC 1-bit backend at 400 kHz identification/20 MHz maximum; switched `3V3_SD`; debounced detect; FAT32 BPB validation; fixed directory policy; bounded 512-byte request/completion queues; path and update-manifest gates; safe removal and isolated faults | native fake-media tests cover no card, FAT32, corrupt/unsupported/I/O-fault media, insertion/removal, queue overflow and update approval; 8/16/32 GB first-article card matrix and signal integrity remain pending |
| diagnostics | bounded event ring, crash snapshot, reset reason and ESP core-dump partition | native tests; reset injection pending |
| telemetry log | special-function opt-in, exclusive locked maintenance transaction, rate-limited CSV, 64 KiB rotation and propagated write/flush failure | native success/failure tests; flash endurance and target timing pending |
| battery/power | synchronized validated ADC snapshot, calibrated divider scaling, fail-closed sensor faults, hysteresis, voltage-based percentage fallback, alarms and inactivity/shutdown policy | native policy/fault tests; real charger, fuel gauge, latch and divider validation pending |
| backup/recovery | locked-only Wi-Fi page for active-model export/verified restore, boot-failure counter, held-button recovery | codec/library tests; complete web configurator and portal hardware pending |
| USB simulator | native ESP32-S3 TinyUSB HID gamepad, four gimbals plus four analog controls and switch buttons, output lock and per-model RF lock; C3 explicitly unsupported | host policy tests and S3 CI build; Windows/Linux/macOS and simulator compatibility pending |
| OpenPocket VRX/video OSD | non-blocking 6×8 frequency controller and scan state machine; ESP32-C3/S3 RX5808/RTC6715 25-bit DATA/CLK/LE backend; strict table validation; immediate manual/model tuning; bounded scan/cancel/restore; calibrated filtered RSSI with valid/stale/unavailable/fault states; AT7456E-only sync state; existing 30×16 HUD/menu compositor and physical AT7456E frame output | native fake-GPIO/ADC tests cover all 48 channels, exact representative synthesizer frames, invalid input/configuration, manual/startup/model tuning, scan dwell/cancel/best selection, RSSI filtering/faults, sync independence, transport failure/recovery, PAL/NTSC, UI and OSD failures; target RX5808 + AT7456E + composite LCD HIL remains open and software tests are not hardware validation |
| OpenPocket Revision-A board services | safe-default `5V_VIDEO`, `5V_DISPLAY`, and `5V_ELRS` domains; charger/gauge/VBUS telemetry; backlight PWM; AMT630A reset, detection, bounded I2C ISP erase/program/readback/SHA-256/boot status; non-blocking I2C control snapshots; simulator RF lock and USB HID | native delayed-start, ISP-fault, checksum, and power-policy tests plus ESP32-S3 compile; assembled-board rail sequencing, AMT630A programming, charging, thermal, USB, and complete HIL execution remain pending first articles |
| onboarding | automatic missing-calibration entry and bounded first-run state machine for calibration, ARM/AUX, ELRS, optional video, battery and CH5-low preview | native state-machine tests; the post-calibration UI flow remains to be connected |
| update/boot | ESP-IDF bootloader, A/B OTA, HTTPS, manifest gates, post-boot self-test, rollback, Secure Boot V2 production config | mock OTA tests; signed-device drill pending |
| development | deterministic virtual hardware simulator, JSON report, PBM outputs, strict-warning tests, ASan/UBSan, dual ESP-IDF target builds with firmware headroom gates | host, ESP32-C3, and ESP32-S3 CI verified |

## Deliberate boundaries

- Lua, UI, filesystem, Wi-Fi, logging, and OTA never execute in the
  flight-critical control task.
- Model edits and model selection are saved only while outputs are locked.
  The control task atomically replaces the runtime model, resets mixer state,
  keeps CH5 low, and releases maintenance only after the hand-off completes.
- Physical presentation is selected at build time: SSD1306 for standalone or
  AT7456E for OpenPocket. The latter consumes the character compositor rather
  than the pixel-canvas sink.
- The pass-through and update services have safe core/platform APIs, but a
  production product should choose its authenticated USB or maintenance UI
  workflow after the PCB is fixed.
- The VRX scan/selection state machine, frequency table, and analog character
  compositor remain hardware-independent. The ESP32 backend only supplies a
  bounded RTC6715 serial transport and RSSI ADC. Native RTC6715/AT7456E
  register tests are not proof of RF tuning, analog signal integrity, or
  recovery timing; a product claim still requires the exact modified tuner,
  OSD revision, routing, LCD controller, and target-PCB PAL/NTSC HIL evidence.
