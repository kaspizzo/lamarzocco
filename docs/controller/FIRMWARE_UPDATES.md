# Firmware updates over Wi-Fi

The local portal includes **Firmware → Install Firmware**. Select the controller
application `.bin` (not a merged flash image, bootloader or partition table).
The progress bar covers upload; validation and restart follow. Install while
the machine is idle and keep controller power connected. After restart, the
page checks the running image ID and startup confirmation. A protected portal
requires signing in again because sessions are intentionally cleared on reboot;
the Firmware section displays the current version and slot after sign-in.

The route uses the portal's existing access rules, session cookie and header
CSRF token. Images must identify this ESP32-S3 controller project and carry the
`LMCTRL-OTA-V1` layout ABI after the IDF application descriptor. Older firmware
without this updater is rejected. ESP-IDF validates the complete image, chip
revision and appended digest before its boot slot can change. This is integrity
and compatibility validation; firmware signing is not enabled by this feature.

Uploads stream through a 4 KiB buffer into the inactive slot, with a two-minute
receive limit. Invalid headers never erase a slot. Interrupted uploads, flash
errors and failed image validation leave the current boot selection intact.
A committed image blocks further uploads until restart. If scheduling the
automatic reboot fails, the portal explicitly asks for **Restart Controller**.

## Startup confirmation and rollback

`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is required in both the application
and the bootloader. After mandatory display, storage/Wi-Fi initialization,
machine-link initialization, power, UI and input startup, the main event loop
must run for ten seconds before confirming the image. Available Internet,
cloud credentials, a machine or a scale connection are not required. This is a
startup check, not a complete hardware or BLE regression test.

An updated image remains pending until confirmed. A reset/crash before
confirmation makes the bootloader return to the previous valid slot. A startup
timer is armed before hardware initialization and restarts a pending image if
confirmation has not completed within 120 seconds, triggering rollback. This
timer requires the scheduler/timer task to remain operational; a deeper hang
still needs the hardware watchdog or a manual reset. After the first USB migration,
there is initially only one installed image; subsequent successful updates
retain the previous firmware for rollback. New firmware must preserve the NVS
schema so the prior version can still read settings after rollback.

See the [ESP-IDF 5.5.4 OTA reference](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-reference/system/ota.html).

## One-time USB migration from the old factory layout

**Do not use an app-only flash for this transition. Do not erase the chip.**
The new bootloader, partition table, initialized OTA metadata and application
must be installed together once. Later normal updates use the portal.

| Region | Offset | Size |
| --- | --- | --- |
| NVS (unchanged) | `0x9000` | `0x20000` |
| PHY (unchanged) | `0x29000` | `0x1000` |
| OTA metadata | `0x2a000` | `0x2000` |
| Application slot 0 | `0x30000` | `0x480000` |
| Application slot 1 | `0x4b0000` | `0x480000` |
| Storage (unchanged) | `0x930000` | `0x400000` |
| Debug journal (within storage, unchanged) | `0xc30000` | `0x100000` |

Encrypted settings, cloud provisioning, presets and the custom logo stay in the
existing NVS region. Storage and the diagnostic journal retain their addresses.
The migration helper refuses an unknown/modified source layout, unexpected
target layout, wrong device MAC, incompatible app ABI or disabled rollback.

Build with ESP-IDF **5.5.4** and a fresh sdkconfig generated from the repository
defaults. For an existing sdkconfig, enable **Bootloader config → Enable app
rollback support** before rebuilding. The source intentionally rejects builds
that omit this option. Check the complete bootloader/application build first.

Prepare a plan without connecting to or writing the controller:

```sh
python tools/controller_ota_migrate.py \
  --build-dir /absolute/path/to/build \
  --expected-mac AA:BB:CC:DD:EE:FF \
  --backup-dir /absolute/private/path/to/new-backup
```

Replace the example MAC with the target controller's MAC reported by esptool.
For the one-time USB migration, add `--port /dev/cu.usbmodem... --execute` using
the same ESP-IDF Python environment that provides `esptool`. The helper:

1. Reads a complete 16 MiB flash backup into a new private directory before any
   writes, checks the actual legacy partition table and verifies the device MAC.
2. Freezes checked build artifacts and their hashes in that directory. Writes
   only the bootloader, partition table, OTA metadata and slot-0 application.
3. Reads the complete flash again and compares NVS, PHY and all storage bytes,
   including the journal, before releasing the controller from ROM mode.

The initial metadata selects slot 0 as **undefined**, not already healthy;
the new application confirms its first startup after the health window. Backup
files contain credentials/settings: retain the private directory and its
manifest until the migration has been checked on hardware.

If migration is interrupted, enter the controller's ROM download mode again.
The private `flash-before.bin` is a complete recovery image for that same device.
Restore it only to the original controller using the ESP-IDF environment:

```sh
python -m esptool --chip esp32s3 --port /dev/cu.usbmodem... write_flash \
  --flash_mode keep --flash_freq keep --flash_size keep 0x0 /private/path/flash-before.bin
```

After USB migration, verify the boot version/slot and startup confirmation,
Wi-Fi settings, presets, custom logo and downloaded connection log. Then test
a normal Wi-Fi update, interrupted upload, and a deliberately unconfirmed
test build returning to the previous version.

## Validation status (2026-11-10)

The legacy-to-OTA USB migration was completed on the targeted ESP32-S3 controller.
The bootloader, partition table, metadata and application matched their prepared
images exactly. Complete before/after reads confirmed byte-identical NVS, PHY
and storage regions, including the connection journal. Slot 0 passed startup
confirmation, the Wi-Fi portal responded, and the log-download button worked.
The legacy Acaia source and Mini scale link also reconnected after the controller
returned to its normal position.

143 C host tests passed normally and under AddressSanitizer/UndefinedBehaviorSanitizer,
including fragmented uploads, rejected images, interrupted writes, failed image
validation, access/CSRF guards and the pending-image startup deadline. Five Python
migration tests passed. Browser checks covered upload/progress/confirmation and
desktop/mobile layout with a simulated controller endpoint. Actual wireless
installation and rollback on the controller remain pending; the startup check
does not establish end-to-end OTA or brew-by-weight accuracy.
