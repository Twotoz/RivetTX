# OpenPocket roadmap

The canonical roadmap is GitHub epic
[#56 — OpenPocket product readiness](https://github.com/Twotoz/RivetTX/issues/56).
An item is complete only after its acceptance criteria are merged and, where
required, proven on the target PCB.

## P0 — software safety and usable product core

- [ ] [#16](https://github.com/Twotoz/RivetTX/issues/16) Keep telemetry
  logging inside an exclusive maintenance transaction, enforce a partition
  sized quota, propagate write failures, and finish target timing tests.
- [ ] [#32](https://github.com/Twotoz/RivetTX/issues/32) Keep the CRSF UART
  under one control-task owner; Lua uses a fixed-capacity mailbox.
- [ ] [#33](https://github.com/Twotoz/RivetTX/issues/33) Remove steady-state
  home/output screen allocation and complete the display-failure endurance
  evidence.
- [ ] [#34](https://github.com/Twotoz/RivetTX/issues/34) Publish synchronized
  battery snapshots and fail closed on configured ADC errors.
- [ ] [#35](https://github.com/Twotoz/RivetTX/issues/35) Accept firmware from
  MCU/control health without requiring an optional ELRS module to be online.
- [ ] [#36](https://github.com/Twotoz/RivetTX/issues/36) Provide 32
  transactional model slots with migration, copy, select, delete, recovery,
  and storage-full handling.
- [ ] [#37](https://github.com/Twotoz/RivetTX/issues/37) Activate a saved
  model without reboot while outputs and CH5 remain locked.
- [ ] [#38](https://github.com/Twotoz/RivetTX/issues/38) Freeze the exact
  ESP32-S3, VRX, OSD, display, ELRS supply, charger, gauge, latch, and pinout.
- [ ] [#39](https://github.com/Twotoz/RivetTX/issues/39) Connect the
  implemented non-blocking RTC6715 backend to the selected SPI-modified
  RX5808 hardware and complete RX5808 + AT7456E + composite LCD HIL. Keep this
  open until real tuning, RSSI calibration, loss/recovery and latency evidence
  is attached.
- [ ] [#40](https://github.com/Twotoz/RivetTX/issues/40) Connect the
  fixed-size 30×16 analog OSD compositor to the selected AT7456E-class
  hardware and composite-video HIL.
- [ ] [#41](https://github.com/Twotoz/RivetTX/issues/41) Validate the native
  ESP32-S3 TinyUSB gamepad with Windows, Linux, macOS, Liftoff, VelociDrone,
  and Uncrashed. C3 remains explicitly unsupported for HID.

## P1 — OpenPocket user experience

- [ ] [#42](https://github.com/Twotoz/RivetTX/issues/42) Finish golden tests
  for the home page, two live gimbal plots, warning banner, warnings list, and
  Home → Menu → Detail navigation.
- [ ] [#43](https://github.com/Twotoz/RivetTX/issues/43) Expose add/remove and
  all remaining model-schema properties; keep every UI range identical to
  validation.
- [ ] [#44](https://github.com/Twotoz/RivetTX/issues/44) Complete schema/HIL
  validation for exact LOW, MIDDLE, and HIGH switch predicates.
- [ ] [#45](https://github.com/Twotoz/RivetTX/issues/45) Replace the fallback
  voltage percentage estimator with the selected fuel gauge, charger/VBUS
  status, runtime estimate, rail control, and real soft-off.
- [ ] [#46](https://github.com/Twotoz/RivetTX/issues/46) Expand the
  locked-only browser portal from model backup/restore to all model, input,
  ELRS, VRX, power, update, and diagnostic settings with per-device
  credentials.
- [ ] [#47](https://github.com/Twotoz/RivetTX/issues/47) Continue the
  automatic first-boot calibration into ARM/AUX, ELRS, VRX, battery, and
  Betaflight CH5 preview steps.
- [ ] [#48](https://github.com/Twotoz/RivetTX/issues/48) Build the factory
  fixture, target timing tests, storage power-cut tests, fuzzers, endurance,
  brownout, RF, USB, video, and Betaflight HIL suite.
- [ ] [#49](https://github.com/Twotoz/RivetTX/issues/49) Present Lua frames,
  deliver UI events/model metadata, and complete bounded scripting tests.
- [ ] [#50](https://github.com/Twotoz/RivetTX/issues/50) Add authenticated
  manifest discovery, cryptographic image verification, progress UI, and
  production rollback tests.
- [ ] [#51](https://github.com/Twotoz/RivetTX/issues/51) Add a bounded USB
  CDC or authenticated network bridge for CRSF passthrough.
- [ ] [#52](https://github.com/Twotoz/RivetTX/issues/52) Extend named,
  unit-aware telemetry with freshness, additional frames, and per-model
  alarms.
- [ ] [#53](https://github.com/Twotoz/RivetTX/issues/53) Render the complete
  discovered CRSF parameter tree instead of relying on known English names.
- [ ] [#54](https://github.com/Twotoz/RivetTX/issues/54) Finish failure
  injection for watchdog, storage recovery, screenshots, Wi-Fi cleanup,
  crash state, and every maintenance guard.

## P2 — release and documentation

- [ ] [#55](https://github.com/Twotoz/RivetTX/issues/55) Keep README,
  architecture, feature matrix, build version, exported diagnostics, and
  validation claims aligned with actual end-to-end behavior.

## Required hardware evidence

These gates cannot be completed in the native simulator:

- Exact OpenPocket schematic, PCB and power tree review.
- ELRS RX-as-TX peak-current, thermal, 100 mW RF, UART, antenna, and
  Betaflight failsafe validation.
- VRX tuning/RSSI/video-loss tests and AT7456E NTSC/PAL overlay tests.
- Charger, fuel gauge, VBUS, brownout, power-latch, and shutdown tests.
- USB enumeration and FPV-game compatibility on the ESP32-S3 board.
- Factory fixture and long-duration target-PCB testing.
