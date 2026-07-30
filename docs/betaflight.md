# Betaflight and ExpressLRS setup

RivetTX is protocol-compatible with the normal ExpressLRS-to-Betaflight
chain, but it does not connect directly to the flight controller:

```text
RivetTX -- 400 kbaud CRSF --> ELRS TX module )) RF ((
ELRS receiver -- CRSF UART --> Betaflight flight controller
```

The default model uses the Betaflight-friendly `AETR1234` channel order and
four dedicated, active-low switch inputs:

| RivetTX input | CRSF channel | Betaflight use | Low / high |
|---|---:|---|---|
| axis 0 | CH1 | Aileron | approximately 1000 / 2000 |
| axis 1 | CH2 | Elevator | approximately 1000 / 2000 |
| axis 2 | CH3 | Throttle | approximately 1000 / 2000 |
| axis 3 | CH4 | Rudder | approximately 1000 / 2000 |
| AUX1 GPIO | CH5 / AUX1 | ARM | disarmed / armed |
| AUX2 GPIO | CH6 / AUX2 | PREARM (optional) | inactive / active |
| AUX3 GPIO | CH7 / AUX3 | flight mode | inactive / active |
| AUX4 GPIO | CH8 / AUX4 | beeper or another mode | inactive / active |

Each switch GPIO uses the ESP32 internal pull-up. Connect a maintained
two-position switch between the configured GPIO and ground. `-1` disables an
input and keeps its channel low. Do not use the UP, DOWN, ENTER, or BACK
buttons as flight switches: they are momentary controls shared with the UI and
safety chord.

The default model requires AUX1 to be low before RivetTX outputs can be
enabled. Once enabled, AUX1 may go high normally. Any RivetTX-side safety
lockout sends throttle and CH5 low. Legacy, unmodified stored models named
`Default` are migrated to this CH5-CH8 mapping; custom model mappings are
preserved.

## ExpressLRS TX module

RivetTX discovers and exposes these module profile settings:

- Packet Rate
- Telem Ratio
- Switch Mode
- Model Match
- Max Power and Dynamic Power
- Bind and Enable WiFi commands

The handset-to-module UART runs at 400 kbaud. RivetTX therefore accepts packet
rates up to 250 Hz and rejects faster selections with
`400K UART: max 250Hz`. The model ID selected in RivetTX is sent to the module
and must match the receiver when ExpressLRS Model Match is enabled.

## Betaflight configuration

For a UART receiver:

1. Remove propellers and power the receiver from the flight controller.
2. Wire receiver TX to FC RX and receiver RX to FC TX on one free UART.
3. Enable **Serial RX** for that UART in the Ports tab.
4. Select **Serial (via UART)** and provider **CRSF** in the Receiver tab.
5. Enable telemetry. Keep `serialrx_inverted` and `serialrx_halfduplex` off.
6. Select `AETR1234`, or verify and correct the channel map in the Receiver
   preview.
7. Assign ARM to AUX1 with low near 1000 and high near 2000. Assign PREARM,
   modes, beeper, and other functions to AUX2-AUX4 as needed.
8. Leave RSSI Channel and RSSI ADC disabled; use CRSF RSSI dBm and Link Quality
   telemetry.

For a flight controller with an integrated SPI ExpressLRS receiver, select the
SPI receiver mode and `EXPRESSLRS` provider instead of configuring a Serial RX
UART.

## Mandatory bench validation

Software tests verify the 16-channel CRSF frame, CRC, endpoints 172/992/1811,
CH5 arming polarity, safe CH5 fallback, module profile writes, model ID, and
telemetry parsing. They cannot prove the RF or flight-controller wiring.

Before making the craft capable of motion:

1. Confirm CH1-CH4 move in the correct directions and show approximately
   1000/1500/2000 in Betaflight.
2. Confirm CH5 is low after boot, during a RivetTX lockout, and whenever the
   dedicated ARM switch is low.
3. Confirm the craft arms only with the intended ARM/PREARM sequence.
4. Verify Model Match both rejects a wrong RivetTX model ID and accepts the
   correct one.
5. Test Betaflight Stage 1 and Stage 2 failsafe by interrupting the real RF
   link, not only by locking RivetTX.
6. Verify reconnect behavior, telemetry, brownout margin, UART integrity, and
   a long hardware-in-the-loop soak with propellers removed.

Primary references:

- [ExpressLRS receiver/flight-controller configuration](https://www.expresslrs.org/quick-start/receivers/configuring-fc/)
- [ExpressLRS before-first-flight AUX1 guidance](https://www.expresslrs.org/quick-start/pre-1stflight/)
- [ExpressLRS Model Match](https://www.expresslrs.org/software/model-config-match/)
- [ExpressLRS transmitter preparation and baud rates](https://www.expresslrs.org/quick-start/transmitters/tx-prep/)
- [Betaflight Receiver tab](https://betaflight.com/docs/wiki/app/receiver-tab)
- [Betaflight failsafe](https://betaflight.com/docs/wiki/guides/current/Failsafe)
