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

Copy:

`include/secrets.example.h`

to:

`include/secrets.h`

Then optionally fill in your normal Wi-Fi credentials. Leave `WIFI_SSID` blank to run entirely through the NanoC6's own access point.

The default direct AP is:

- SSID: `MUSHROOM-ENV`
- URL: `http://192.168.4.1/`

Change the example AP password before deployment.

## Build

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
