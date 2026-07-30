# ExpressLRS control, Finder, and updates

RivetTX talks to an ExpressLRS **TX module** over its full-duplex CRSF UART.
The receiver stays in the aircraft and talks directly to the flight
controller. The two links have different responsibilities:

```text
RivetTX -- CRSF --> ELRS TX module )) RF (( ELRS receiver -- CRSF --> flight controller
              settings + handset telemetry       RSSI/LQ/TX-power for OSD
```

## Module settings

Open the **ExpressLRS** screen while outputs are locked. RivetTX discovers the
module's CRSF parameters at runtime and shows the options that the connected
module actually advertises:

- maximum RF power in mW
- dynamic power
- packet rate (limited to 250 Hz by the 400 kbaud handset UART)
- switch mode
- telemetry ratio
- model match
- bind
- Enable WiFi for a module firmware update

Field IDs and power lists are deliberately not hard-coded. Available powers
depend on the module hardware, regulatory domain, firmware, and any unlock
state. A selected value is written as the module's option index and then read
back through a fresh discovery pass.

Keep the module at the lowest useful power during bench work. Never select a
power that is illegal in your location, exceeds the module's cooling limits,
or overloads the transmitter power supply.

## Simple bind

1. Remove propellers or otherwise make the vehicle safe.
2. Lock RivetTX outputs.
3. Open **ExpressLRS**, select **BIND**, and press ENTER.
4. Put the receiver in bind mode if it does not use a matching binding phrase.

Bind is offered only when the connected module exposes the CRSF `Bind`
command. Binding phrases remain the preferred ExpressLRS workflow for
repeatable setups.

## Packet rate and Model Match

RivetTX uses the broadly compatible 400 kbaud CRSF link to the TX module.
ExpressLRS supports packet rates up to 250 Hz at that baud rate, so RivetTX
rejects faster advertised choices instead of accepting a configuration that
can overrun the handset link.

RivetTX sends each model's ID to the module and exposes the discovered
`Model Match` setting. When matching is enabled, set the receiver to the same
ID. A mismatch can still look connected over RF while the receiver withholds
control data from the flight controller.

## ELRS Finder

The **ELRS Finder** screen uses live uplink RSSI reported by the aircraft. It
selects the active antenna from the CRSF link-statistics frame, applies a
small moving filter, renders dBm and a 0–100% bar, and emits increasingly fast
beeps as signal strength rises.

For a directional search:

1. Leave the lost model and receiver powered.
2. Open **ELRS Finder** and rotate the transmitter slowly.
3. Walk in the direction that makes the beeps faster.
4. Reduce transmitter power if the signal saturates nearby.

Finder marks data stale and silences the buzzer after one second without a
fresh RSSI sample. It cannot find an unpowered model and it is not a distance
measurement: antenna orientation, obstacles, receiver power, and multipath
all affect RSSI.

A passive piezo on the configured buzzer GPIO enables sound. Without it, the
numeric dBm and bar display still work. Lua scripts can use `getValue("RSSI")`,
`getValueAge("RSSI")`, and `playTone(frequency, duration)` for custom finder
interfaces; `1RSS` and `2RSS` expose the individual antennas.

Outside the Finder screen, RivetTX monitors link quality while outputs are
enabled. Weak, critical, lost, and recovered states have distinct prioritized
patterns; see [Audio alerts](audio-alerts.md).

## Betaflight OSD

Betaflight gets link statistics from the **ELRS receiver connected to the
flight controller**, not from RivetTX. RivetTX therefore does not synthesize
or inject an OSD RSSI value. It correctly decodes the same CRSF telemetry for
its own screen:

- active-antenna uplink RSSI in negative dBm
- uplink link quality in percent
- TX power converted from the CRSF enum to real mW

On the aircraft, configure the receiver UART as a serial RX port with receiver
provider `CRSF`, then enable the desired Betaflight OSD items such as RSSI dBm,
link quality, and TX power. Use an ELRS/Wide-compatible RSSI display mode when
required by the Betaflight version. If the OSD is wrong while RivetTX telemetry
is right, inspect the receiver-to-flight-controller UART and Betaflight
configuration; that path does not pass through the transmitter.

## Easy TX-module update

With outputs locked, choose **UPDATE ELRS** on the **ExpressLRS** screen.
RivetTX runs the module's discovered `Enable WiFi` command and confirms it
when the module asks. The screen then shows:

```text
Wi-Fi:    ExpressLRS TX
Password: expresslrs
Open:     http://elrs_tx.local
Upload:   firmware.bin
```

Connect a phone or computer to that network, open the shown address, and
upload the correct ExpressLRS `firmware.bin`. Some systems can instead use the
module's fallback address shown by its Web UI. Keep the transmitter powered,
do not enable outputs, and do not interrupt the upload or reboot.

This updates the ExpressLRS TX module. RivetTX itself uses its separate signed
A/B OTA and rollback path described in [Architecture](architecture.md).

## Validation boundary

The virtual ELRS module verifies discovery, fragmented CRSF frames, power and
mode writes, packet-rate limits, Model Match, bind, Wi-Fi confirmation, RSSI
filtering, Geiger timing, offline detection, and reconnection. A real
module/receiver/flight-controller test is still mandatory because simulation
cannot prove RF behavior, regional power limits, UART electrical integrity,
or a Betaflight configuration.

## Primary references

- [ExpressLRS Lua configuration](https://www.expresslrs.org/quick-start/transmitters/lua-howto/)
- [ExpressLRS TX-module Web UI](https://www.expresslrs.org/quick-start/webui/)
- [ExpressLRS transmitter update methods](https://www.expresslrs.org/quick-start/transmitters/updating/)
- [EdgeTX CRSF telemetry decoder](https://github.com/EdgeTX/edgetx/blob/main/radio/src/telemetry/crossfire.cpp)
- [Betaflight CRSF receiver implementation](https://github.com/betaflight/betaflight/blob/master/src/main/rx/crsf.c)
