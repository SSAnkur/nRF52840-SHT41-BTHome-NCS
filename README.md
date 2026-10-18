# nRF52840 SHT41 BTHome Beacon

An ultra-low-power Bluetooth LE temperature and humidity beacon for the
Pro Micro nRF52840 (Zephyr board `promicro_nrf52840`, UF2 bootloader variant)
with a Sensirion SHT41 sensor, built on the nRF Connect SDK v3.4.1.

The device broadcasts [BTHome v2](https://bthome.io/) advertisements, so it
is discovered automatically by Home Assistant's BTHome integration (and any
other BTHome-compatible receiver). It never accepts connections: it only
advertises.

## How it works

Every **60 s** the firmware:

1. Switches on the SHT41's supply from a GPIO (P0.17) and waits 3 ms for it to start.
2. Takes one high-repeatability temperature / humidity measurement over I2C.
3. Puts the I2C pins into a disconnected "sleep" state and cuts sensor power.
4. If the sensor read succeeded, measures the supply voltage (VDDH) with the
   SAADC, right after the sensor load.
5. Updates the advertising payload.

Between measurements the CPU sits in System ON idle while the radio sends a
non-connectable advertisement every **10 s** (the payload therefore carries
the same values for six consecutive adverts). If a sensor read fails, the
battery is not measured and the advertisement is not updated, so the last
good values (including voltage) keep being advertised.

Power-saving measures, mostly configured in `app.overlay` and `prj.conf`:

- Sensor supply gated through a `power-domain-gpio` node, driven by Zephyr's
  runtime device power management (`pm_device_runtime_get/put`).
- I2C bus enabled only during transfers; SDA/SCL disconnected while idle so
  the breakout's pull-ups can't back-feed the unpowered sensor.
- DC/DC converters enabled on both REG0 (VDDH → VDD) and REG1 (VDD → core).
- 0 dBm TX power, 10 s advertising interval.
- UART, USB, console, logging and unused peripherals (I2C1, SPI2, PWM0) disabled.

## Hardware

| Signal         | nRF52840 pin | SHT41 breakout |
| -------------- | ------------ | -------------- |
| Sensor power   | P0.17        | VIN / VCC      |
| I2C SDA        | P0.20        | SDA            |
| I2C SCL        | P0.22        | SCL            |
| Ground         | GND          | GND            |

The SHT41 is at I2C address `0x44`. Battery voltage is read internally
from VDDH (VDDH/5 input), so no external divider is needed — power the board
from a cell connected to the battery / VDDH input.

The sensor supply pin uses high drive (`NRF_GPIO_DRIVE_H0H1`). Keep the
breakout's total load small; if your board has large bulk capacitance or a
power LED, remove it or increase `startup-delay-us` in `app.overlay`.

## Project layout

```
.
├── CMakeLists.txt
├── prj.conf        # Kconfig: BLE broadcaster, drivers, PM, console off
├── app.overlay     # Devicetree: sensor power domain, I2C, ADC, regulators
├── README.md
└── src/
    └── main.c      # Measurement loop and BTHome payload
```

## Installing the SDK

The project is built with the nRF Connect SDK (NCS) **v3.4.1**, installed via
Nordic's `nrfutil` command-line tool and its `sdk-manager` plugin.

### 1. Get nrfutil

Download the `nrfutil` executable for your OS from Nordic's nRF Util page:
<https://www.nordicsemi.com/Products/Development-tools/nRF-Util>

On Linux / macOS, make it executable and put it on your `PATH`, e.g.:

```sh
chmod +x nrfutil
mv nrfutil ~/.local/bin/        # or any directory on your PATH
nrfutil --version
```

On Windows, place `nrfutil.exe` in a folder that is on your `PATH`.

### 2. Install the sdk-manager command

```sh
nrfutil install sdk-manager
```

### 3. Install NCS v3.4.1 (SDK + matching toolchain)

```sh
nrfutil sdk-manager install v3.4.1
```

By default this places the SDK in `~/ncs/v3.4.1` and the toolchain alongside it.
You can list what is available or installed with `nrfutil sdk-manager search`
and `nrfutil sdk-manager list`.

## Building

Open a shell with the NCS toolchain, load the Zephyr environment, and build
from the project directory:

```sh
nrfutil sdk-manager toolchain launch --ncs-version v3.4.1 --shell
source ~/ncs/v3.4.1/zephyr/zephyr-env.sh
west build -b promicro_nrf52840/nrf52840/uf2
```

The `/uf2` board variant links the application above the pre-installed UF2
bootloader and produces a `zephyr.uf2` image. Add `-p` to force a pristine
(clean) build, e.g. after changing `app.overlay` or `prj.conf`.

## Flashing

The Pro Micro nRF52840 ships with a UF2 bootloader:

1. Double-tap the reset button (or briefly short RST to GND twice). A USB
   mass-storage drive appears (often named `NICENANO`; the name depends on
   the bootloader your board came with).
2. Copy the generated `zephyr.uf2` onto the drive. With NCS's default
   sysbuild it is at `build/<project-folder>/zephyr/zephyr.uf2` (or
   `build/zephyr/zephyr.uf2` if you build with `--no-sysbuild`). The board
   reboots into the new firmware.

Because USB and serial are disabled in the application, the board will **not**
enumerate over USB while running. Use the double-tap reset each time you
want to reflash.

## Advertising payload

Advertised name: `SHT41` (set by `CONFIG_BT_DEVICE_NAME`). The device uses its
identity address, so it appears with a stable MAC.

Service data (UUID `0xFCD2`):

| Bytes   | Meaning                                         |
| ------- | ----------------------------------------------- |
| `D2 FC` | BTHome UUID (little-endian)                     |
| `40`    | BTHome v2, unencrypted, regular interval        |
| `0C` + 2 | Voltage, uint16 LE, 1 mV                       |
| `2E` + 1 | Humidity, uint8, 1 %                           |
| `45` + 2 | Temperature, sint16 LE, 0.1 °C                 |

Example: `D2 FC 40 0C 1B 0C 2E 2D 45 E7 00` decodes as 3.099 V, 45 % RH,
23.1 °C. The full advertisement (flags + service data + name) is 23 bytes,
inside the 31-byte legacy limit.

## Configuration

| What                    | Where                                        |
| ----------------------- | -------------------------------------------- |
| Measurement interval    | `MEAS_INTERVAL` in `src/main.c`              |
| Advertising interval    | `ADV_INTERVAL` in `src/main.c` (0.625 ms units, legacy max 10.24 s) |
| Device name             | `CONFIG_BT_DEVICE_NAME` in `prj.conf`        |
| TX power                | `CONFIG_BT_CTLR_TX_PWR_*` in `prj.conf`      |
| Pins                    | `sens_pwr` and `pinctrl` nodes in `app.overlay` |
| Sensor power-up delay   | `startup-delay-us` in `app.overlay`          |
| ADC resolution / averaging | `&adc` channel in `app.overlay`           |

## Home Assistant

With a Bluetooth adapter or ESPHome Bluetooth proxy in range, the BTHome
integration discovers the device automatically and creates temperature,
humidity and voltage sensors. Expect the first values within about 10 s of
power-up, and new readings every 60 s.

## Troubleshooting

- **Nothing shows up:** check that the board is powered from the battery
  input and wait at least one advertising interval (10 s). The first
  advertisement is sent even if the initial sensor read fails, with zeros
  for temperature, humidity and voltage.
- **All values stuck at 0:** the sensor has never responded (the battery is
  only measured after a successful sensor read). Check the wiring, the I2C
  address and that P0.17 actually powers the breakout.
- **Values frozen at an old reading:** sensor reads are failing after an
  earlier success; same checks as above.
- **Voltage reads 0 but temperature/humidity update:** the ADC read failed;
  check the `&adc` node and that `CONFIG_ADC=y`.
- **Can't flash:** the application disables USB, so you must enter the
  bootloader with a double reset first.
