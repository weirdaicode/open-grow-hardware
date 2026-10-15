# Open Grow Hardware

A $15 CYD (Cheap Yellow Display) by your grow tent that tells you, at a glance, whether the air is right for the plants in it.

**[Try the live demo in your browser](https://weirdaicode.github.io/open-grow-hardware/)**. It is the real screen layout running on two simulated tents through a 96-second day. No board needed.

<!-- photo: the climate dial on a real board next to a tent. Replace with docs/img/dial.jpg once taken. -->

## What it is

Firmware for the **ESP32-2432S028R**, the "Cheap Yellow Display": a 2.8-inch 320x240 resistive touchscreen with an ESP32 on the back, sold for about $15. Plug it into USB power near your tent and it shows:

- **Home**: a card per tent or room. Temperature, humidity and VPD, each coloured by how it sits against the targets for the plants' stage: green ideal, yellow OK, orange risk, red danger.
- **Space**: VPD large, a band bar per reading so you can see how far from ideal you are, and the next two things to do.
- **To do**: the most urgent actions across every space ("Lower humidity below 55%", "No reading for 4 h: check the sensor").
- **Plugs**: on/off buttons for lights, fans, dehumidifiers and heaters. Off until you allow it.
- **Climate dial** (idle screen): two crossed needles, temperature and humidity. Where they cross is your air, drawn over the ideal box, the mold zone and VPD curves for the stage.

A status LED stays dark when everything is fine, pulses amber on risk and blinks red on danger. With "wake on alert", a danger reading lights the screen even at night.

## Two firmwares, pick one

| | **LogMyGrow Display** (`cyd/LogMyGrowDisplay/`) | **ESPHome** (`cyd/esphome-cyd-ha.yaml`) |
|---|---|---|
| Needs | A LogMyGrow account (free) or your own LogMyGrow server | Home Assistant with the ESPHome add-on |
| Data | Every space you log, with stage targets that change when you flip to flower | Two HA sensors you name (temperature, humidity) |
| Mold risk | Yes, by stage, with a tighter line in late flower | No |
| To-dos and plug control | Yes | No |
| VPD | From the server, with a leaf offset | Worked out on the device |
| Updates | Over the air, checked against a sha256 | ESPHome OTA |

## Hardware

- ESP32-2432S028R ("CYD"), either screen variant. Boards that show a white or inverted screen have the ST7789 panel: build the `cyd_st7789` env.
- A USB cable that carries data, and a 5 V USB supply.
- Optional: a printed case. Use PETG or ASA. PLA softens in a warm, humid tent.
- A temperature/humidity sensor the server can read: Govee, SwitchBot, AC Infinity, or anything Home Assistant can see.

## Flash it

Full steps, troubleshooting and the button reference are in [`cyd/README.md`](cyd/README.md). Short version:

1. Install VS Code and the PlatformIO extension.
2. Open the `cyd` folder (the one with `platformio.ini`).
3. Plug the board in and click **Upload**. If it stops at "Connecting...", hold BOOT, tap RST, release BOOT, and upload again.

Then on first start: join the `LogMyGrow-Display` Wi-Fi from your phone, pick your home Wi-Fi, tap the two calibration crosses, and type the 6-letter code the screen shows into **LogMyGrow > Equipment > Desk display**.

## How it works

- **Pairing**: the board asks the server for a short code and shows it with a QR. You confirm the code in the app while signed in. The board then receives its own display token. Your account password never touches the board.
- **Data**: every 60 seconds (every 20 while a fetch is failing) the board fetches a small JSON feed for its token: each space's readings, tiers, bands, mold lines and actions. The screen draws from that. The network runs in its own FreeRTOS task, so the UI never freezes while it fetches.
- **Updates**: every 6 hours, the board reads `/firmware/display-<board>.json`. When it names a newer version, the board downloads the image into the second app slot (`min_spiffs` partitions), checks its sha256, and only then switches over. A failed check aborts and keeps the running firmware.
- **Plug control**: a tap sends a request to the server, which switches the plug through Kasa or Home Assistant. The server only accepts plugs you declared in your own spaces, and only after you turned control on for that display.
- **Self-hosting**: the Wi-Fi setup page has a Server field. Point it at your own LogMyGrow server.

## Privacy

The board stores your Wi-Fi details, its display token and its settings. It sends nothing but its feed request (with the firmware version, so the app can show it) and plug taps. There is no analytics on the device.

## Roadmap

1. A rules page on the screen ("if VPD > 1.5, fan on") with a toggle and "last fired" per rule. Rules stay on the server so they work when the screen is off.
2. A sensor node: ESPHome for an ESP32-C3 with an SHT45 (temperature/humidity) and SCD41 (CO2), posting straight to the webhook. About $20 in parts.
3. A 6-hour sparkline per reading on the Space page.

Ideas and pull requests are welcome. Please open an issue first for anything larger than a fix.

## Who made this

I grow a few plants indoors and build [LogMyGrow](https://logmygrow.com), a grow journal with an AI coach that reads your own logs and sensor history. The display started as a gauge for my own tent. The firmware is open so you can read exactly what it does, change it, or point it at your own server.

## License

Firmware and configs: [MIT](LICENSE). Case files (when added): CERN-OHL-P v2.
