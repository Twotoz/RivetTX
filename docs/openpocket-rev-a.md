# OpenPocket Revision A

Revision A targets the `ESP32-S3-MINI-1U-N8` without PSRAM. Its checked-in
defaults are in `sdkconfig.openpocket-rev-a.defaults`; its 8 MiB flash layout
is in `partitions.openpocket-rev-a.csv`.

Build from a clean ESP-IDF tree:

```sh
idf.py set-target esp32s3
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.openpocket-rev-a.defaults" reconfigure
idf.py build
```

The partition layout reserves two 2.5 MiB OTA slots, 1.5 MiB for model data,
and independent crash, recovery, and display-firmware staging partitions. The
layout ends at `0x700000`, leaving 1 MiB unallocated in the 8 MiB device. PSRAM
is explicitly disabled. Host tests and the OpenPocket compositor allocate no
PSRAM-specific objects; an ESP32-S3 CI build is the release gate for flash and
static-DRAM headroom.

## Startup ordering

RX5808 GPIO and ADC configuration is validated and initialized during board
startup, but no physical tune is started there. RivetTX creates `vrx_task` and
then appends the active model's stored band/channel to its fixed-size command
queue. The task starts the RTC6715 transaction and its timeout when it consumes
that command. First-run calibration, cancelled calibration, recovery startup,
and storage recovery therefore cannot consume the tuning timeout before the
VRX state machine is running. A model switch made immediately after startup is
queued behind the restore command instead of overwriting it.

## GPIO allocation

GPIO19 and GPIO20 are reserved for native USB D- and D+. GPIO22–25 and
GPIO27–32 are not bonded out by the MINI-1U module and are rejected during
startup validation. GPIO45 and GPIO46 are sampled as strapping inputs during
reset; their I2S loads are high impedance until after boot and have no pull
resistors. All analog inputs use ADC1.

| GPIO | Revision-A function |
|---:|---|
| 1–4 | Left/right gimbal X/Y (ADC1) |
| 5 | RX5808 RSSI (ADC1) |
| 6 | NS4168 hardware enable (default-low) |
| 7 / 8 / 9 | RX5808 DATA / LE / CLK |
| 10 | TCA9535 control-expander interrupt |
| 11 / 12 / 13 / 14 | AT7456E SCLK / MOSI / MISO / CS |
| 15 / 16 | Board I2C SDA / SCL |
| 17 / 18 | CRSF TX / RX, from the ESP32 perspective |
| 19 / 20 | USB D- / D+ |
| 21 | AT7456E reset |
| 26 / 33 / 34 / 36 / 37 | AMT flash CS / CLK / MOSI / mux select / MISO |
| 35 | PCB magnetic-buzzer PWM |
| 38 | AMT630A active-low reset |
| 39 / 45 / 46 | NS4168 I2S BCLK / WS / DATA |
| 40 / 41 / 42 | 5V_VIDEO / 5V_DISPLAY / 5V_ELRS enables |
| 43 / 44 | BQ25895 interrupt / MAX17048 ALERT |
| 47 / 48 | Backlight PWM / protected VBUS detect |

Two onboard TCA9535 devices at `0x20` and `0x21` handle the slow active-low
controls, including encoder A/B on flattened bits 20/21 and encoder press on
bit 11. `board_io_task` samples the expanders and publishes an atomic
snapshot; the 250 Hz control task never performs I2C traffic.

## Power and simulator policy

The board-power service starts all switched 5 V domains and the backlight off,
with AMT630A reset asserted. It reads BQ25895 status, MAX17048 cell voltage and
state of charge, and protected USB VBUS state outside the control task.

Entering USB simulator mode immediately disables `5V_ELRS` and `5V_VIDEO`,
blocks CRSF transmission, requests the safety lock, and consequently holds
ARM/CH5 low. Display power and backlight remain independently controllable, so
USB-only simulator operation does not require a battery.

The AMT630A normally owns its W25X05 boot flash. For factory recovery, a
SN74CB3Q3257 mux transfers all four SPI signals only while AMT reset is held
low. RivetTX performs bounded erase/program/readback operations, verifies the
complete image SHA-256, restores AMT ownership before releasing reset, and
keeps the TFT backlight disabled until the controller reports ready.

## Validation status

Native tests cover delayed RX5808 startup, safe power-state logic, bounded
AMT630A recovery-flash sequencing including erase/program/readback/checksum/
boot failures, non-blocking prioritized buzzer alerts, and bounded I2S DMA.
The merged profile is an engineering prototype: RX5808, AT7456E, AMT630A,
ER-TFT050A3-2, charger, rail, USB, RF-coexistence, and propeller-off HIL evidence
is recorded only after first-article boards exist.
