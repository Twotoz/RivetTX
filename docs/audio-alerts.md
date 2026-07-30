# Audio alerts

RivetTX has one central, non-blocking audio scheduler. The ELRS Finder, Lua,
link monitoring, battery monitoring, telemetry alarms, module state, and
safety state all share it. Fixed-size events cross into the service task;
audio never sleeps, allocates, or runs in the 4 ms control path.

## Built-in sounds

| Event | Pattern | Repeats |
|---|---|---|
| startup | three rising notes | once |
| outputs enabled | two rising notes | on transition |
| outputs locked/faulted | two falling notes | on transition |
| safety fault | three very low urgent notes | on transition |
| weak ELRS link | two short high notes | every 10 seconds |
| critical ELRS link | four urgent high notes | every 3 seconds |
| telemetry lost | two long low notes | every 5 seconds |
| link recovered | two rising notes | on recovery |
| ELRS module offline | three low notes | every 10 seconds |
| ELRS module recovered | three rising notes | on recovery |
| TX battery low | two low notes | every 30 seconds |
| TX battery critical | three long low notes | every 10 seconds |
| TX battery recovered | two rising notes | on recovery |
| configured telemetry alarm | two warning notes | alarm repeat interval |
| telemetry alarm recovered | two rising notes | on recovery |
| Finder/Lua tone | requested frequency | when no higher alert is active |

Urgent events pre-empt lower-priority sounds. The priority order is critical
link, critical TX battery, telemetry lost, module offline, weak link and
safety fault,
telemetry warning, low TX battery, recovery/status sounds, and finally
Finder/Lua tones. A lower-priority sound can never cut off a safety warning.

## Link warning behavior

Link warnings use fresh CRSF uplink link quality rather than pretending RSSI
is a distance measurement. Defaults are:

- weak below 70% LQ
- critical below 30% LQ
- telemetry lost after 1.5 seconds without a fresh link-quality sample
- 8 percentage points of hysteresis to prevent threshold chatter

Warnings sound only while outputs are enabled. Link and module recovery sounds
make it clear when the connection returns. Change the weak and critical LQ
thresholds under `Component config -> RivetTX hardware`.

## Transmitter battery

The warning measures the transmitter's own battery ADC after applying the
configured divider. Defaults are 3500 mV low and 3200 mV critical, suitable
only as development values for a one-cell supply. Configure both thresholds
for the actual complete battery pack. The critical threshold also causes the
safety manager to lock outputs.

Battery sensing must be enabled with a valid ADC GPIO. When sensing is
disabled, the state remains unknown and no false battery alarm is played.

## Hardware

Set `RIVETTX_BUZZER_GPIO` to the passive-piezo output and select a duty
percentage with `RIVETTX_BUZZER_VOLUME`. The default GPIO value `-1` disables
physical sound while keeping visual warnings and all alarm decisions active.
Use a transistor driver if the sounder exceeds the GPIO's electrical limits.
