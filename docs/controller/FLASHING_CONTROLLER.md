# Flashing The Controller Firmware (ESP32 / ESP-IDF)

This is the shortest working path for building and flashing the round controller firmware in `firmware/esp32`.

No vendor package is required.

This repository currently supports two documented flashing paths:

- `macOS/Linux`: use the local Bash helper `./dev.sh`
- `Windows (native)`: use the official ESP-IDF Windows environment with `idf.py`

## 1. Requirements

- a USB data cable
- Python 3.10 or newer
- `git`
- ESP-IDF 5.5 or newer
- the controller board connected over USB

## 2. macOS / Linux

### Install ESP-IDF once

If ESP-IDF is not installed yet on macOS or Linux:

```bash
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32s3
```

In every new shell session, load the environment first:

```bash
source ~/esp/esp-idf/export.sh
```

### First flash

```bash
cd firmware/esp32
./dev.sh full
```

`full` handles the normal first-time workflow:

- selects the `esp32s3` target
- builds the firmware
- detects the serial port
- flashes bootloader, partition table, and app
- opens the serial monitor

### Later updates

For normal firmware updates after the first flash:

```bash
cd firmware/esp32
./dev.sh quick
```

`quick` uses `idf.py app-flash monitor`, so only the app partition is reflashed.

If you pull a change that modifies `firmware/esp32/partitions.csv`, do one full
flash again instead of `quick`. Partition-layout changes move partition offsets,
so `app-flash` alone is not enough for that transition.

### If port detection fails

List available serial ports:

```bash
./dev.sh ports
```

Set the port manually:

```bash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh quick
```

Exit the serial monitor with `Ctrl+]`.

`dev.sh` is Bash-based and currently auto-detects `/dev/cu.*`-style serial ports. It is therefore the documented helper path for macOS/Linux, not for native Windows flashing.

### Useful commands

```bash
./dev.sh build
./dev.sh flash
./dev.sh monitor
./dev.sh erase
./dev.sh clean
./dev.sh menuconfig
```

To run the host-side firmware tests automatically before each local `git push`, install the repo-local hook once:

```bash
./dev.sh install-hooks
```

The hook runs `./firmware/esp32/dev.sh test` from the repository root. On a fresh checkout, build the firmware once first so `managed_components/` exists for the host test runner.

## 3. Windows (native)

### Install ESP-IDF once

On Windows, use Espressif's official installer and ESP-IDF terminal instead of `dev.sh`:

- [ESP-IDF Windows setup](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/get-started/windows-setup.html)
- [ESP-IDF start project on Windows](https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/get-started/windows-start-project.html)

Use a project path without spaces, for example:

```text
C:\dev\lamarzocco
```

The firmware component manifest requires ESP-IDF `>=5.5.0`. ESP-IDF 5.4 is no
longer a supported build target for this repository.

If installing ESP-IDF from Git instead of the installer, clone the stable 5.5
branch and export the environment before building:

```powershell
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git C:\esp-idf
cd C:\esp-idf
.\install.ps1 esp32s3
.\export.ps1
```

### First flash

1. Open `ESP-IDF PowerShell` or `ESP-IDF Command Prompt`.
2. Change into the firmware directory:

```powershell
cd C:\dev\lamarzocco\firmware\esp32
```

3. Set the target once for this project:

```powershell
idf.py set-target esp32s3
```

4. Find the board's serial port in Windows Device Manager, for example `COM5`.
5. Build, flash, and open the serial monitor:

```powershell
idf.py -p COM5 flash monitor
```

6. Exit the serial monitor with `Ctrl+]`.

This is the standard Windows path for this repository. Windows uses `COMx` serial ports, so this repository does not document native Windows flashing through `./dev.sh`.

## 4. Common Problems

- **Permission denied / port busy**
  Close Arduino IDE, another serial monitor, or any other tool that may still hold the port.
- **No serial data received**
  Check that the USB cable supports data, not only charging.
- **Wrong target selected**
  On macOS/Linux, rebuild with `./dev.sh clean && ./dev.sh full`.
  On Windows, run `idf.py fullclean`, then `idf.py set-target esp32s3`, then `idf.py -p COM5 flash monitor`.
- **Firmware was updated, but a partition-layout change was also pulled**
  Re-run a full flash so bootloader, partition table, and app are flashed together.
  `./dev.sh quick` only refreshes the app image.
- **ESP-IDF not found**
  On macOS/Linux, run `source ~/esp/esp-idf/export.sh` or make sure `IDF_PATH` points to your ESP-IDF install.
  On Windows, use the ESP-IDF PowerShell or ESP-IDF Command Prompt created by the official installer.

## 5. See Also

- [`firmware/esp32/README.md`](../../firmware/esp32/README.md)
- [`SETUP_GUIDE.md`](./SETUP_GUIDE.md)
- [`SCREENSHOTS.md`](./SCREENSHOTS.md)
