# macOS Guide: Flashing the Controller Firmware

This is the beginner-friendly macOS version of
[`FLASHING_CONTROLLER.md`](./FLASHING_CONTROLLER.md). It is written for people
who have never flashed an ESP32 or used ESP-IDF before.

The short version is: install ESP-IDF, connect the controller over USB, then
run `./dev.sh full` from the `firmware/esp32` directory.

## Before You Start

You need:

- a Mac with internet access
- your macOS user password, if macOS asks for it
- a USB data cable; charge-only cables do not work
- the round ESP32-S3 controller connected over USB
- about 30 to 60 minutes for the first setup

Important notes:

- Copy commands into the `Terminal` app one block at a time.
- If a command shows an error, do not continue blindly with the next step.
  Check [Troubleshooting](#troubleshooting) first.
- Put the project in a path without spaces, for example `~/Code/lamarzocco`.
- Do not use a vendor package. This repository builds and flashes directly
  with ESP-IDF.

## 1. Open Terminal

1. Open `Applications`.
2. Open `Utilities`.
3. Start `Terminal`.

All commands below go into that Terminal window.

## 2. Install Apple Command Line Tools

Run:

```bash
xcode-select --install
```

If macOS says the tools are already installed, that is fine. If an installer
window appears, confirm it and wait until it finishes.

Then check that `git` works:

```bash
git --version
```

If you see a version number, this step is done.

## 3. Install Homebrew

First check whether Homebrew is already installed:

```bash
brew --version
```

If you see a version number, skip to step 4.

If you see `command not found: brew`, install Homebrew:

```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

At the end, the installer may show a command that you still need to copy into
Terminal. Run that command. If you are unsure, these two commands cover the
usual Mac install locations:

```bash
if [ -x /opt/homebrew/bin/brew ]; then eval "$(/opt/homebrew/bin/brew shellenv)"; fi
if [ -x /usr/local/bin/brew ]; then eval "$(/usr/local/bin/brew shellenv)"; fi
```

Check again:

```bash
brew --version
```

## 4. Install ESP-IDF Helper Tools

```bash
brew install git python cmake ninja dfu-util ccache
```

Then check the important tools:

```bash
python3 --version
cmake --version
ninja --version
```

If all three commands show a version number, this step is done.

## 5. Get The Repository

If you do not have the repository locally yet:

```bash
mkdir -p ~/Code
cd ~/Code
git clone https://github.com/kaspizzo/lamarzocco.git
cd lamarzocco
```

If you already have the repository, change into your existing folder instead.
Example:

```bash
cd ~/Code/lamarzocco
```

Check that you are in the right folder:

```bash
ls firmware/esp32/dev.sh
```

If Terminal prints `firmware/esp32/dev.sh`, you are in the right place.

## 6. Install ESP-IDF

This project uses ESP-IDF for `esp32s3`. Install ESP-IDF into
`~/esp/esp-idf`, because `firmware/esp32/dev.sh` can find that path
automatically.

```bash
mkdir -p ~/esp
cd ~/esp
git clone -b release/v5.5 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32s3
```

The download and installation can take several minutes.

If you see `fatal: destination path 'esp-idf' already exists`, the folder
already exists. Do not clone it again. Continue with:

```bash
cd ~/esp/esp-idf
git fetch origin release/v5.5
git checkout release/v5.5
git pull --ff-only
git submodule update --init --recursive
./install.sh esp32s3
```

## 7. Load ESP-IDF In This Terminal

```bash
source ~/esp/esp-idf/export.sh
idf.py --version
```

If you see `ESP-IDF v5.5...` or newer, ESP-IDF is ready.

Note: `firmware/esp32/dev.sh` normally loads ESP-IDF by itself when ESP-IDF is
installed under `~/esp/esp-idf`. This step is still a useful sanity check.

## 8. Connect The Controller

1. Connect the controller to your Mac.
2. Prefer a direct Mac USB port or a good USB-C hub.
3. Wait a few seconds.
4. Change into the firmware folder:

```bash
cd ~/Code/lamarzocco/firmware/esp32
```

If your repository is somewhere else, adjust the path.

List detected serial ports:

```bash
./dev.sh ports
```

Typical macOS ports look like this:

```text
/dev/cu.usbmodemXXXX
/dev/cu.usbserialXXXX
/dev/cu.SLAB_USBtoUARTXXXX
/dev/cu.wchusbserialXXXX
```

If no USB-looking port appears, the cable, adapter, or USB port is usually the
problem.

## 9. First Flash

Stay in the `firmware/esp32` folder and start the first full flash:

```bash
./dev.sh full
```

The first build downloads dependencies and compiles a lot of code. This can
take several minutes.

You are done when roughly this happens:

- Terminal shows `Using port /dev/cu...`
- the build finishes without `FAILED`
- the flash output shows lines like `Hash of data verified`
- the serial monitor opens automatically
- the controller reboots and shows the firmware or setup screen

Exit the serial monitor with:

```text
Ctrl+]
```

That means: hold `Control` and press `]`.

## 10. Later Firmware Updates

For normal updates:

```bash
cd ~/Code/lamarzocco
git pull
cd firmware/esp32
./dev.sh quick
```

`quick` flashes only the app partition, so it is faster.

If you are not sure whether the partition layout changed, use this instead:

```bash
./dev.sh full
```

`full` is slower, but it is the safe choice after larger firmware changes.

## 11. Useful Commands

Run all commands in this section from the `firmware/esp32` folder.

```bash
./dev.sh ports
```

Shows detected serial ports.

```bash
./dev.sh full
```

Full flash: bootloader, partition table, and app. Use this for the first flash
and larger updates.

```bash
./dev.sh quick
```

Fast update: flash only the app and then open the monitor.

```bash
./dev.sh monitor
```

View logs without flashing again.

```bash
./dev.sh clean
```

Clean the build directory if the build behaves strangely.

```bash
./dev.sh erase
```

Erase the whole flash. Only use this when you intentionally want to remove all
controller data or someone asks you to do it while troubleshooting.

## Troubleshooting

### `brew: command not found`

Homebrew is not installed or is not in your Terminal path yet. Install Homebrew
from step 3 or open a new Terminal window.

### `xcrun: error: invalid active developer path`

The Apple Command Line Tools are missing or broken. Run:

```bash
xcode-select --install
```

### `ESP-IDF was not found`

Check whether ESP-IDF exists in the expected location:

```bash
ls ~/esp/esp-idf/export.sh
```

If the file exists:

```bash
source ~/esp/esp-idf/export.sh
cd ~/Code/lamarzocco/firmware/esp32
./dev.sh full
```

If the file does not exist, repeat step 6.

### `No serial port found`

Your Mac does not see the controller as a serial USB device.

Check:

- is the controller really connected to the Mac over USB?
- is the cable a data cable, not a charge-only cable?
- does another USB port or adapter work?
- does the controller appear a few seconds after plugging it in?

Then try again:

```bash
cd ~/Code/lamarzocco/firmware/esp32
./dev.sh ports
```

### The Wrong Port Is Selected Automatically

If multiple serial USB devices are connected, set the port manually. Replace
the example port with the port from `./dev.sh ports`:

```bash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh full
```

For later updates:

```bash
ESPPORT=/dev/cu.usbmodemXXXX ./dev.sh quick
```

### Flashing Gets Stuck At `Connecting...`

Unplug the controller, plug it back in, and run the command again:

```bash
./dev.sh full
```

If your board has visible `BOOT` and `RESET` buttons:

1. Start `./dev.sh full`.
2. When `Connecting...` appears, hold `BOOT`.
3. Press `RESET` briefly.
4. Release `BOOT` as soon as writing starts.

### `Permission denied` Or `Resource busy`

Another program is using the serial port.

Close:

- Arduino IDE
- VS Code Serial Monitor
- other Terminal windows running `idf.py monitor`
- other flashing tools

Then try again:

```bash
./dev.sh full
```

### The Build Fails After An Update

Clean the build and flash fully:

```bash
cd ~/Code/lamarzocco/firmware/esp32
./dev.sh clean
./dev.sh full
```

### Apple Silicon Error With `bad CPU type in executable`

Install Rosetta 2:

```bash
/usr/sbin/softwareupdate --install-rosetta --agree-to-license
```

Then open a new Terminal window and flash again.

### Python Certificate Error During ESP-IDF Download

If installation fails with `CERTIFICATE_VERIFY_FAILED`, a Python certificate
bundle may be missing. If you installed Python from python.org, there is an
`Install Certificates.command` file in the Python folder. Run it, then repeat
step 6.

## How To Know It Worked

The flash was successful if:

- `./dev.sh full` ends without errors
- the serial monitor opens automatically
- the controller starts the firmware after reset
- a fresh controller shows the setup screen or setup AP

After flashing, continue with the setup guide:

- [`SETUP_GUIDE.md`](./SETUP_GUIDE.md)

## Reference

- Short repository guide: [`FLASHING_CONTROLLER.md`](./FLASHING_CONTROLLER.md)
- Espressif ESP-IDF 5.5 macOS/Linux setup:
  <https://docs.espressif.com/projects/esp-idf/en/release-v5.5/esp32s3/get-started/linux-macos-setup.html>
