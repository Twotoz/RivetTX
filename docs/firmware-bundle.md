# Firmware bundle

Each successful continuous-integration run publishes a
`rivettx-esp32c3-<commit>` and `rivettx-esp32s3-<commit>` artifacts for
development hardware. Each artifact is generated only after its complete
ESP-IDF build and firmware headroom gate pass.

## Contents

| File | Purpose |
|---|---|
| `rivettx-factory.bin` | complete development image for a blank matching target, flashed at offset `0x0` |
| `rivettx.bin` | application image for the first OTA slot or the RivetTX updater |
| `bootloader/bootloader.bin` | ESP-IDF bootloader |
| `partition_table/partition-table.bin` | RivetTX 4 MiB A/B partition layout |
| `ota_data_initial.bin` | initial OTA selection data |
| `flash_args` | exact ESP-IDF/esptool arguments for the separate images |
| `flasher_args.json` | machine-readable flash layout |
| `SOURCE_COMMIT` | full Git commit used for the build |
| `TARGET` | exact ESP-IDF chip target: `esp32c3` or `esp32s3` |
| `SHA256SUMS` | checksums for every bundled file |

## Verify

From the extracted artifact directory:

```bash
sha256sum --check SHA256SUMS
```

Do not flash if a checksum fails, if `SOURCE_COMMIT` is not the revision you
intended to test, or if `TARGET` does not match the chip:

```bash
cat TARGET
```

## Flash a development board

Download the artifact for the exact chip. Install a current `esptool`, put the
board into download mode, and replace `PORT` and `TARGET` below:

```bash
python -m pip install esptool
python -m esptool --chip TARGET --port PORT write-flash \
  0x0 rivettx-factory.bin
```

The factory image contains the bootloader, partition table, initial OTA data,
and application at their configured offsets. Alternatively, an ESP-IDF 5.5.2
environment can flash the separate images with:

```bash
esptool.py --chip TARGET --port PORT write_flash @flash_args
```

> [!CAUTION]
> These are unsigned engineering-preview images for disposable development
> hardware. They are not production releases and must not be used on a
> Secure-Boot- or flash-encryption-provisioned transmitter. Keep the RF module
> disconnected during initial flashing and bring-up.
