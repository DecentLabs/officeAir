# officeAir — ESP32 Office Presence & Climate Monitor with Google Sheets Logging

An ESP32-based office monitoring system that logs **temperature, humidity and occupancy** (via WiFi device counting) into a Google Sheet — complete with self-healing firmware, on-device crash forensics, and a physiological heartbeat LED that "pants" under CPU load. 💓

The name and use case come from real life: we originally set this up to monitor the DecentLabs office climate during a hot summer when the air conditioning had some weaknesses. It has since evolved from a Raspberry Pi + Python script into a full-featured ESP32 telemetry platform. (The original RPi version is preserved at the bottom of this README as legacy.)

Code by: [Róbert Szalóki](https://github.com/rszaloki), Hardware configuration, ESP32 port and manual by: [Zoltan Dóczi (RFsparkling)](http://rfsparkling.com), Licensed by [DecentLabs](https://decent.org/)

![ESP32 pcb](https://github.com/DecentLabs/officeAir/blob/master/example/8_office_air%20esp32.jpg)

[![YouTube Video GMutHe-9XjI](https://utfs.io/f/nGnSqDveMsqxOoMkS70k5fKEn2LbBoPAuZ6XMTHDcNJ0QiG1)](https://www.youtube.com/watch?v=GMutHe-9XjI)

[![YouTube Video B6L4vFoD-HM](https://img.youtube.com/vi/B6L4vFoD-HM/maxresdefault.jpg)](https://www.youtube.com/watch?v=B6L4vFoD-HM)

<img width="2535" height="1140" alt="image" src="https://github.com/user-attachments/assets/bc42e7f4-98bf-45c6-b327-662bd091cae3" />
<img width="989" height="349" alt="image" src="https://github.com/user-attachments/assets/49e78d07-e276-447e-9c72-0622df6f3a21" />,
<img width="2154" height="1252" alt="image" src="https://github.com/user-attachments/assets/045fe3e2-a1eb-4aa4-a985-c0a9d72735b1" />



---

## ✨ Features

### Occupancy & climate sensing
- **WiFi device counting as an occupancy proxy** — low-level ARP sweep of the /24 subnet using raw lwIP `etharp` calls (no ping, so firewalled phones answer too)
- **4-pass multi-sweep scan with result union** — defeats WiFi frame loss and catches power-saving mobile clients that miss single-pass scans; accuracy matches an `nmap -sn` reference within ±1 device, at half the scan time (~18 s on a 240 MHz MCU)
- **Thread-safe lwIP access** — all ARP calls wrapped in the TCP/IP core lock, eliminating a race condition that otherwise causes random freezes after hours of operation
- **SHT21 temperature & humidity logging** every 10 minutes
- **Battery voltage telemetry** via the Heltec V3 onboard divider

### Reliable data pipeline
- **RAM FIFO buffer (24 h deep)** — measurements survive network outages and upload in order once connectivity returns
- **ACK-based, idempotent upload to Google Sheets** — entries are only dropped from the buffer after a confirmed write; the Apps Script backend deduplicates re-sent measurements by timestamp, so a lost ACK can never cause data loss, duplicates, or retry storms
- **Explicit TLS/HTTP timeouts** — a stuck connection can never block the firmware forever
- **Manual measurement button** — the USER button triggers an immediate measure + upload cycle

### Self-healing & crash forensics
- **Hardware task watchdog** (90 s) with automatic recovery restart
- **Reset-reason tracking** — every boot logs *why* the previous run ended (`POWERON`, `TASK_WDT`, `PANIC`, `BROWNOUT`, `SW`...), with lifetime counters persisted in NVS flash
- **RTC-memory breadcrumbs** — after a crash, the firmware knows which phase it died in (sensor read, scan pass N, TLS handshake, ...)
- **Core dump harvesting** — panic PC + backtrace addresses are read back from flash on reboot and reported; decode them offline with `addr2line`
- **Reboot-storm-proof reporting** — un-uploaded forensics chain up in NVS and ship with the first successful upload; the device effectively writes its own incident reports into the log
- **Health telemetry in every row** — free heap, largest allocatable block (fragmentation early-warning), uptime, boot/watchdog counters

### The heartbeat LED 💓
- **Physiological heart model** on the onboard LED: lub-dub double beat, respiratory sinus arrhythmia, beat-to-beat HRV jitter, and HRV narrowing under load — just like a real heart
- **CPU-load-driven BPM** — resting ~50 BPM when idle, ramping toward 160 BPM during the TLS crypto sprint, with asymmetric ramp-up/cool-down kinetics (fast sprint, slow athletic recovery)
- **Honest CPU measurement** — an IRAM-safe FreeRTOS tick-hook sampling profiler (1 kHz), immune to WiFi-interrupt artifacts; runs in its own FreeRTOS task, so the heart keeps beating even while the main thread blocks
- **12-bit gamma-corrected PWM** — perceptually smooth breathing instead of stepped dimming

### Privacy
- **MAC anonymization switch** (on by default) — UART logs show `AA:BB:CC:XX:XX:XX`, keeping the vendor OUI visible for debugging while masking device identities; safe for screen recordings and published logs

---

## 🔩 Hardware

| Component | Detail |
| :--- | :--- |
| Board | Heltec WiFi LoRa 32 **V3** (ESP32-S3) |
| Sensor | SHT21 (I2C temperature + humidity) |
| I2C pins | **SDA: GPIO 41**, **SCL: GPIO 42** (custom mapping for clean prototyping) |
| Sensor rail | switched via the onboard **`Vext`** power switch (GPIO 36) |
| Battery | optional 18650 cell, charged over USB; voltage read via onboard divider (GPIO 1 / GPIO 37) |
| Button | onboard USER/PRG button (GPIO 0) — manual measurement trigger |
| LED | onboard LED (GPIO 35) — the heart ❤️ |

> ⚠️ **Power supply note:** USB-PD chargers (e.g. the Raspberry Pi 5 supply) may refuse to deliver power without PD negotiation — use a plain 5V USB supply. We learned this the hard way; the battery-voltage telemetry column exists because of it. :)

---

## 📂 Directory structure

```
esp32_version/
├── platformio.ini   # Project configuration, dependencies, board definition
└── src/
    └── main.cpp     # Firmware (scanner, buffer, forensics, heartbeat LED)
code.gs              # Google Apps Script backend (idempotent ingestion endpoint)
uptime_dashboard.gs  # Optional: date × hour uptime heatmap generator
```

---

## 🚀 Setup

### 1. Google Sheet + Apps Script backend

**One-click template:** make your own copy of the pre-configured sheet (the attached Apps Script comes with it):

👉 **[Copy the officeAir template sheet](https://docs.google.com/spreadsheets/u/1/d/1cFYKWmYqLdmBjP9kKl1BjMEiIiHa2lmY5L9W9_Fk8bM/copy)**

Or build it manually: create a spreadsheet with a tab named `Raw` and this header row:

```
time | temperature | humidity | buffer | devices | heap | maxblock | uptime | boots | wdt | rr | vbat | crash
```

Column mapping is **header-driven**: the script matches incoming URL parameters to column names, so you can reorder columns or add new telemetry fields later without touching the code.

Then deploy your own endpoint (deployments are **not** copied with the template — everyone needs their own):

1. Open **Extensions → Apps Script** (paste `code.gs` if you built the sheet manually)
2. **Deploy → New deployment → Web app**
3. Execute as: **Me** | Who has access: **Anyone**
4. Authorize when prompted, then copy the **Web app URL** (`https://script.google.com/macros/s/.../exec`)

> 💡 After any future script edit: **Deploy → Manage deployments → New version** — saving alone does not go live!

Column meanings, beyond the sensor readings: `devices` is the ARP scan result (`-1` = scan skipped), `heap`/`maxblock` track memory health (a shrinking `maxblock` reveals heap fragmentation long before it bites), `uptime` drops to zero on silent reboots, `boots`/`wdt` are lifetime restart counters, `rr` names the reset reason that started the current run, `vbat` is the cell voltage, and `crash` holds the post-mortem of any abnormal restart — phase, program counter and backtrace, chained across reboot storms. **The device writes its own incident reports.**

### 2. Firmware

1. Open `esp32_version/` in VS Code + PlatformIO
2. Edit the configuration block at the top of the source:
   - `ssid` / `password` — your WiFi credentials
   - `googleScriptUrl` — the Web app URL from step 1
   - `ANONYMIZE_MACS` — leave `true` unless you want full MACs in the UART log
3. Upload. Keep the built `firmware.elf` (`.pio/build/<env>/firmware.elf`) — you'll need it to decode backtrace addresses if a crash report ever lands in the `crash` column:
   ```
   xtensa-esp32s3-elf-addr2line -pfiaC -e firmware.elf 0x4200....
   ```

### 3. Uptime dashboard (optional)

Paste `uptime_dashboard.gs` into the same Apps Script project and add a time-driven trigger (e.g. every 10 minutes) for `generateUptimeDashboard`. It renders a **date × hour heatmap** of measurement delivery on a separate tab. Pair it with a conditional-formatting color scale — Minpoint: *Number* `0`, Midpoint: *Number* `90`, Maxpoint: *Number* `100`. Future hours are left empty by the script, so they're never painted as false downtime.

---

## 📊 Example output

Temperature and humidity charts over several days:

![example chart](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart.png)
![example chart2](https://github.com/DecentLabs/officeAir/blob/master/example/7_new_office_example.png)

Current consumption profile measured on an Otii Arc:

![ESP32 current chart](https://github.com/DecentLabs/officeAir/blob/master/example/9_office_air%20esp32_current_consumption_chart_on_Otii_Arc.jpg)

[![Youtube Video](https://github.com/DecentLabs/officeAir/blob/master/example/11_esp32_current_youtube.png)](https://www.youtube.com/watch?v=iRE4Ly5Vs14&t)

---

## 🔄 ESP32 vs. the original Raspberry Pi version

| Feature | Legacy (Raspberry Pi) | Current (ESP32) |
| :--- | :--- | :--- |
| **Hardware** | Raspberry Pi (SBC) | ESP32-S3 (Heltec WiFi LoRa 32 V3) |
| **Language** | Python + Bash + cron | C++ (Arduino / PlatformIO) |
| **Sensing** | Temperature + humidity | Temp + humidity + **occupancy (ARP device counting)** + battery |
| **Reliability** | Fire-and-forget `curl` | Buffered, ACK-based, idempotent pipeline + watchdog + crash forensics |
| **Power** | ~2–5 W, always-on | ~0.4 W always-on (mains); a deep-sleep variant (µA-range standby) exists for battery/solar use |
| **HTTP** | `curl -L` | `HTTPClient` with strict redirect following (Apps Script 302s handled natively) |
| **Form factor** | Large, needs a solid 5 V supply | Compact, USB-powered, battery/solar friendly |

---
---

# 🗄️ Legacy: Raspberry Pi version (no longer developed)

> The original project — kept here for reference and for existing installations. New setups should use the ESP32 version above.

You can take this example Google sheet (make a copy and format to your needs):
[example google RPi sheet](https://docs.google.com/spreadsheets/d/1t9r_rUyFjBkhwC56gJk5tmbUgx4k2Acqq_upUnAoVYM/edit?usp=sharing)

Raw data looks like this:

![example raw data](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2.png)

### 1. Open the script menu of the Google Sheet:
![script menu](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2b.png)

[You can also download the script from the repo](https://github.com/DecentLabs/officeAir/blob/master/code.gs)

### 2. Select Publish → Deploy as web app:
![web app deploy](https://github.com/DecentLabs/officeAir/blob/master/example/5_balcony_temp_hum_chart_2c.png)

### 3. Copy the web app link — it will be pasted into the Raspberry Pi script in step 8. This key links the RPi to your Google Sheet:
![web app key, copy that](https://github.com/DecentLabs/officeAir/blob/master/example/5balcony_temp_hum_chart_2d.png)

### 4. Enable the I2C interface on the Raspberry Pi:

	sudo raspi-config

![raspi-config](https://github.com/DecentLabs/officeAir/blob/master/example/1_raspi-config_intef_options.png)

![Interface Options](https://github.com/DecentLabs/officeAir/blob/master/example/2_raspi-config_intef_options_i2c.png)

### 4b. Connect the sensor to the RPi pins as shown:
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring1.png)
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring2.png)
![wiring](https://github.com/DecentLabs/officeAir/blob/master/example/6_sensor_wiring3.png)

### 5. Update, upgrade and install i2c-tools:

	sudo apt update
	sudo apt upgrade
	sudo apt install i2c-tools

### 6. Verify the I2C sensor:

	sudo i2cdump -y 1 64

It should look something like this:
![I2C map](https://github.com/DecentLabs/officeAir/blob/master/example/4_i2cdump_map.png)

### 7. Create the log script that reads the sensor and uploads to the Google web app:

	sudo mkdir /usr/local/bin/log
	cd /usr/local/bin/log
	sudo nano log

### 8. Paste this into `log` — replace the web app key with the one you copied in step 3:

	#/bins/sh
	DATA=`sht21.py`
	curl -L "https://script.google.com/macros/s/**your web app key here**/exec?$DATA"

### 9. Set permissions:

	sudo chmod +x log

### 10. Copy the Python script into place:

	cd /usr/local/bin/
	sudo wget https://raw.githubusercontent.com/DecentLabs/officeAir/master/sht21.py
	sudo chmod +x sht21.py

### 11. Add a crontab entry to run the script every minute:

	sudo nano /etc/crontab

Append as the last line:

	*  *    * * *   root  /usr/local/bin/log/log #log air quality

### Finally, reboot the RPi — once it comes back it will report temperature and humidity to the Google Sheet every minute:

	sudo reboot
