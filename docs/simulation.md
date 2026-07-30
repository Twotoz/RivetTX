# Virtual hardware simulation

RivetTX includes a deterministic host simulation that exercises the production
control, mixer, safety, CRSF parser, module supervisor, telemetry registry,
storage, and UI code. The replaced boundary is deliberately narrow: an
in-memory UART connects the firmware core to a virtual ExpressLRS TX module,
and PBM files replace a physical display.

## Run it

```bash
make build/rivettx-sim
./build/rivettx-sim
```

The default run executes every scenario and display profile. A non-zero exit
status means at least one invariant failed. `build/sim-report.json` contains
the scenario counts and verdicts for scripts and CI.

Focused runs are also available:

```bash
./build/rivettx-sim --scenario corruption
./build/rivettx-sim --scenario disconnect --display large
./build/rivettx-sim --help
```

Supported scenario names are `nominal`, `packet-loss`, `corruption`,
`disconnect`, `stale-input`, and `missed-deadline`. Display names are
`compact`, `medium`, and `large`.

## What is checked

| Scenario | Injected condition | Required outcome |
|---|---|---|
| nominal | fragmented UART reads | model ID, discovery, channels, telemetry, and online state remain valid |
| packet loss | every second telemetry frame is removed | accepted frames stay valid and the link continues |
| corruption | every third telemetry frame gets a bad CRC | every altered frame is rejected by the parser |
| disconnect | UART unavailable for 1.6 seconds | offline detection, diagnostic events, reconnect, and model-ID restore |
| stale input | ADC sample timestamp is too old | immediate failsafe followed by throttle-safe recovery |
| missed deadline | one mixer cycle exceeds its budget | deadline count, failsafe, and throttle-safe recovery |

Every scenario runs 1,000 control cycles at a simulated 4 ms period. It also
checks watchdog service, outbound CRSF CRCs, channel packing/unpacking,
telemetry values, receive-queue bounds, and the final safety/module state.

The UI is rendered at 128×64, 240×135, and 480×320 to verify that screen data
is independent of a single panel size. The resulting PBM files can be opened
with common image viewers. Unit tests additionally drive the virtual module's
discovered power and mode fields, bind and Wi-Fi commands, offline recovery,
and Finder RSSI/audio behavior.

Run the same tests under AddressSanitizer and UndefinedBehaviorSanitizer with:

```bash
make sanitize
```

## What this does not prove

The simulator cannot validate electrical levels, UART signal integrity,
interrupt jitter, RF behaviour, brownouts, ADC noise, real display timing,
flash endurance, or the peak-current demands of an ExpressLRS module. It also
does not replace receiver-side failsafe testing.

Before flight, repeat the disconnect and stale-control tests with an actual
ESP32-C3 or ESP32-S3, ExpressLRS TX module and receiver while props and mechanisms are
made safe. The [hardware guide](hardware.md) lists the remaining bring-up
steps.
