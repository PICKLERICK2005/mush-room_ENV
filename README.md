# mush-room_ENV

Offline-first environmental logger for a mushroom grow-room pilot, built around the M5Stack NanoC6 and ENV III.

## v0.1

- 5 s live sampling
- 5 min persistent aggregate logging
- temperature, relative humidity, pressure, and calculated dew point
- temperature/RH min-max values per logged interval
- LittleFS circular chunk storage
- automatic deletion of the oldest chunks when the log budget is full
- fallback Wi-Fi AP that works without a router or internet connection
- optional normal Wi-Fi client mode
- local live dashboard
- combined CSV download
- clear-logs control
- browser clock sync without internet
- NTP sync when normal Wi-Fi has internet
- persistent session IDs across reboots

The logger is intentionally offline-first: network access is only a way to inspect and retrieve data. Sampling and persistent logging continue without a connected client or upstream Wi-Fi.

## Hardware

- M5Stack NanoC6
- M5Stack ENV III
- Grove cable
- USB-C 5 V power source

ENV III connects directly to the NanoC6 Grove port.

## Setup

Open the **repository root** in PlatformIO, where this README and `platformio.ini`
live. `MUSROOM-ENVSENSE/` is a separate, unfinished ESP-IDF scaffold, not the logger.

Install PlatformIO IDE for VS Code, or PlatformIO Core 6.2.0 with Python:

```sh
python -m pip install platformio==6.2.0
```

Copy:

`include/secrets.example.h`

to:

`include/secrets.h`

Then optionally fill in your normal Wi-Fi credentials. Leave `WIFI_SSID` blank to run entirely through the NanoC6's own access point.

On PowerShell, create the file only if it does not already exist:

```powershell
if (!(Test-Path include/secrets.h)) { Copy-Item include/secrets.example.h include/secrets.h }
```

`include/secrets.h` is ignored by Git. Keep real credentials out of tracked files.

The default direct AP is:

- SSID: `MUSHROOM-ENV`
- URL: `http://192.168.4.1/`

Change the example AP password before deployment.

## Build

From the repository root, using a PlatformIO terminal or a shell with `pio` on PATH:

```sh
pio run -e m5stack-nanoc6
pio run -e m5stack-nanoc6 -t upload
pio device monitor -b 115200
```

If PlatformIO is installed through VS Code on Windows and is not on PATH:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run
```

## Upload

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run -t upload
```

## Serial monitor

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" device monitor
```

When the logger joins a normal LAN, try `http://mushroom.local/` or use the IP printed over serial.

## Notes

The NanoC6 uses an ESP32-C6FH4 with 4 MB embedded flash. The generic PlatformIO ESP32-C6-DevKitC-1 profile can report a larger flash size, so `platformio.ini` explicitly overrides it to 4 MB.

CSV output includes session and uptime information so records remain ordered even if absolute time has not yet been synchronized after a reboot.

The first valid sample is logged immediately, followed by five-minute aggregates.
Logs rotate in approximately 64 KiB chunks with a budget of 80% of LittleFS capacity.
Download CSV regularly: old chunks are automatically deleted, and a filesystem
mount failure triggers a format attempt. The unfinished aggregation interval
is held in RAM and is lost on power failure.

The AP remains enabled even when upstream Wi-Fi connects. The HTTP dashboard has
no authentication; anyone with network access can download or clear logs and set
the clock. Use a trusted local network and change the example AP password.

## Repository checks and maintenance

GitHub Actions builds the root firmware on pushes and pull requests using example
credentials. This checks compilation; it does not test attached hardware. Follow
the [hardware smoke checks](test/README) after firmware or dependency changes.

The platform is pinned to [pioarduino 55.03.312-1](https://github.com/pioarduino/platform-espressif32/releases/tag/55.03.312-1)
and the sensor library to [M5Unit-ENV 1.7.1](https://github.com/m5stack/M5Unit-ENV).
Review upstream releases before updating these pins, rebuild, and run the hardware
checks. Transitive library dependencies may still resolve to newer versions.
Dependabot checks GitHub Actions updates monthly; it does not update PlatformIO dependencies.

No project license has been selected yet.
