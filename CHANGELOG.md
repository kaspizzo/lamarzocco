# Changelog

Changes to the controller firmware and its setup portal are recorded here.
Earlier development history remains available in Git.

## 2026-11-10

### Added

- Wi-Fi firmware installation from the local portal, with file selection,
  upload progress, image validation and restart feedback.
- Two application slots, startup confirmation and a 120-second deadline for
  unconfirmed OTA images to restart into rollback. The existing working image
  remains selected until the complete upload has been validated.
- A USB migration helper that backs up the complete flash, checks the device
  and partition map, and verifies preserved settings/storage after writing.
- A small gold `∞`, `1` or `2` at the connected scale icon's lower-right corner,
  showing Continuous, Dose 1 or Dose 2. It appears only with a fully connected
  scale link and a known BBW mode.
- [Firmware update and migration instructions](docs/controller/FIRMWARE_UPDATES.md)
  and [screenshots of all three mode indicators](docs/controller/SCREENSHOTS.md#scale-connection-states).

### Changed

- **Download Log** is now a secondary button matching the other log controls.
  It downloads the file without submitting the capture settings form.
- Firmware uploads follow the portal's existing authentication/CSRF rules and
  reject incompatible chip/project/layout images before preparing an app slot.

### Upgrade and validation notes

- Existing single-factory-app installations require the one-time USB migration.
  An app-only flash cannot install the new partition table or bootloader.
- Settings, presets, custom logo and journal addresses remain unchanged. The
  first hardware migration verified the protected regions byte for byte and
  confirmed a healthy boot, portal access and legacy Acaia/Mini scale connection.
- 143 C host tests passed normally and with sanitizers; five Python migration
  tests and desktop/mobile portal checks also passed. Actual Wi-Fi installation
  and rollback tests on the controller remain pending. Scale support retains
  its experimental status and existing hardware-validation limits.
