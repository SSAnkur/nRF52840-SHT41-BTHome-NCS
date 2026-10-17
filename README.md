# nRF52840 SHT41 BTHome Beacon

An ultra-low-power Bluetooth LE temperature and humidity beacon for the
Pro Micro nRF52840 (nice!nano v2 compatible) with a Sensirion SHT41 sensor,
built on Zephyr / the nRF Connect SDK.

The device broadcasts [BTHome v2](https://bthome.io/) advertisements, so it
is discovered automatically by Home Assistant's BTHome integration (and any
other BTHome-compatible receiver). It never accepts connections: it only
advertises.

## How it works

Every **60 s** the firmware:

1. Switches on the SHT41's supply from a GPIO (P0.17) and waits 3 ms for it to start.
2. Takes one high-repeatability temperature / humidity measurement over I2C.
3. Puts the I2C pins into a disconnected "sleep" state and cuts sensor power.
4. Measures the supply voltage (VDDH) with the SAADC, right after the sensor load.
5. Updates the advertising payload.

Between measurements the CPU sits in System ON idle while the radio sends a
non-connectable advertisement every **10 s** (the payload therefore carries
the same values for six consecutive adverts). If a sensor read fails, the
last good values keep being advertised.

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

## Building

Requires the nRF Connect SDK (or upstream Zephyr) with `west`.

```sh
west build -b nice_nano/nrf52840 -p
```

On older SDKs that use the pre-hardware-model-v2 board names, use
`-b nice_nano_v2` instead.

## Flashing

The nice!nano ships with a UF2 bootloader:

1. Double-tap the reset button (or briefly short RST to GND twice). A USB
   drive named `NICENANO` appears.
2. Copy `build/zephyr/zephyr.uf2` onto the drive. The board reboots into the
   new firmware.

Because USB and serial are disabled in the application, the board will **not**
enumerate over USB while running. Use the double-tap reset each time you
want to reflash. You can also flash over SWD with a J-Link or similar probe
using `west flash`.

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
  for temperature and humidity.
- **Temperature/humidity stuck at 0:** the sensor isn't responding. Check the
  wiring, the I2C address and that P0.17 actually powers the breakout.
- **Voltage reads 0:** the ADC read failed; check the `&adc` node and that
  `CONFIG_ADC=y`.
- **Can't flash:** the application disables USB, so you must enter the
  bootloader with a double reset first.
