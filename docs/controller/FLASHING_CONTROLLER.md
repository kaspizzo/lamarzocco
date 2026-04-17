# Flashing The Controller Firmware (ESP32 / ESP-IDF)

This is the shortest working path for building and flashing the round controller firmware in `firmware/esp32`.

No vendor package is required.

## 1. Requirements

- a USB data cable
- Python 3.10 or newer
- `git`
- an installed ESP-IDF toolchain
- the controller board connected over USB

## 2. Install ESP-IDF Once

If ESP-IDF is not installed yet:

```bash
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf
./install.sh esp32,esp32s3
```

In every new shell session, load the environment first:

```bash
source ~/esp/esp-idf/export.sh
```

## 3. First Flash

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

## 4. Later Updates

For normal firmware updates after the first flash:

```bash
cd firmware/esp32
./dev.sh quick
```

`quick` uses `idf.py app-flash monitor`, so only the app partition is reflashed.

## 5. If Port Detection Fails

List available serial ports:

```bash
./dev.sh ports
```

Set the port manually:

```bash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh quick
```

Exit the serial monitor with `Ctrl+]`.

## 6. Useful Commands

```bash
./dev.sh build
./dev.sh flash
./dev.sh monitor
./dev.sh erase
./dev.sh clean
./dev.sh menuconfig
```

## 7. Common Problems

- **Permission denied / port busy**
  Close Arduino IDE, another serial monitor, or any other tool that may still hold the port.
- **No serial data received**
  Check that the USB cable supports data, not only charging.
- **Wrong target selected**
  Rebuild with `./dev.sh clean && ./dev.sh full`.
- **ESP-IDF not found**
  Run `source ~/esp/esp-idf/export.sh` or make sure `IDF_PATH` points to your ESP-IDF install.

## 8. See Also

- [`firmware/esp32/README.md`](../../firmware/esp32/README.md)
- [`SETUP_GUIDE.md`](./SETUP_GUIDE.md)
- [`SCREENSHOTS.md`](./SCREENSHOTS.md)
