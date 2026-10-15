# LogMyGrow Display (firmware 3.0) for the CYD (ESP32-2432S028R)

Open-source (MIT) firmware that turns a $15 2.8-inch "Cheap Yellow Display" into a grow-room status screen: every tent
or area you have in LogMyGrow, temperature, humidity and VPD coloured against your plants' stage targets, mold risk,
what to do next, and on/off buttons for your plugs. Works with logmygrow.com or a self-hosted LogMyGrow server.

## What's on the screen
Tabs along the bottom. Tap a tab, or swipe left and right.

- **Home**: one card per space (4 per screen, swipe up for more). Temperature, RH and VPD are each coloured by their own
  tier: green ideal, yellow ok, orange risk, red danger. The stripe on the left is the space's overall tier. Tap a card
  to open that space.
- **Spaces**: the VPD large, then a bar each for temperature, humidity and VPD (yellow = ok range, green = ideal range for
  the stage, white marker = the reading), the top two actions, and how old the reading is. Swipe for the next space.
- **To do**: the five most urgent actions across every space ("Lower humidity to 40-55%", the first mold step, "No
  reading for 4 h: check the sensor"). A dot on the tab means something is waiting.
- **Controls** (only when you allow it in the app): a button per plug (lights, fans, heaters, humidifiers). Tap to switch.
  Kasa switches at once; Home Assistant plugs switch on Home Assistant's next poll (about 30 s).
- **Settings** (gear): theme (dark or light), brightness (auto uses the light sensor on the front), dim after, screen off
  after, wake on alert, cycle pages, status light, alert beep, turn the screen 180 degrees, calibrate touch, pair again,
  Wi-Fi and server. Tap a row to change it. Firmware version, IP and server are shown at the bottom.

The status light on the back stays dark when everything is fine (it sits by a tent at night), pulses amber on risk,
blinks red on danger, and blinks blue when it has been offline for 5 minutes. With "Wake on alert" on, a danger reading
lights the screen even if it was off. The network runs in its own task, so the screen never freezes while it fetches.

## The two buttons on the board
| Button | Press | What it does |
|---|---|---|
| BOOT | tap | next page (or wake the screen) |
| BOOT | hold 2 s | Settings |
| BOOT | hold 5 s | calibrate touch |
| BOOT | hold 10 s | erase everything (Wi-Fi, pairing, settings) and start fresh |
| RST | tap | restart; keeps everything |

A banner at the top tells you what letting go will do, so you can keep holding or release.

## Flash it (PlatformIO, about 10 minutes the first time)
1. Install VS Code, then the **PlatformIO IDE** extension, and let it finish setting up.
2. **File > Open Folder** and pick `devices/cyd` (the folder with `platformio.ini`), not the repo root.
3. Plug the board in with a USB cable that carries data. On Windows, install the CH340 driver if no COM port appears.
4. Click **Upload** (the right arrow in the blue bar at the bottom). If it stops at "Connecting...", hold BOOT, tap RST,
   let go of BOOT, and Upload again.

All the screen settings live in `platformio.ini`; you never edit TFT_eSPI's `User_Setup.h`.

## First start (2 minutes)
1. **Wi-Fi.** The screen shows a QR code. Scan it with your phone's camera to join **LogMyGrow-Display** (or join it by
   hand), and a setup page opens (or go to `192.168.4.1`). Pick your home Wi-Fi, type its password, Save. Self-hosting?
   Put your server's address in the Server box.
2. **Touch.** Tap the centre of the two crosses. Takes five seconds; redo it any time from Settings.
3. **Pair.** The screen shows a 6-letter code and a QR. Scan the QR (it opens LogMyGrow with the code filled in) or type
   the code in **LogMyGrow > Equipment > Desk display**, then tap **Pair**. The screen shows your spaces a few seconds later.

To let the display switch plugs, tick **Let the display switch my plugs** on the same card. It is off until you do.
The app shows when the display last checked in and which firmware it runs.

## If something is off
| What you see | Fix |
|---|---|
| White or blank screen, colours inverted | Your board has the ST7789 screen. Pick the `cyd_st7789` env in PlatformIO's bottom bar and Upload again. |
| Picture upside down | Settings > Turn screen 180. |
| Taps land in the wrong place | Hold BOOT 5 s and calibrate. |
| "can't reach logmygrow.com" | Check the Wi-Fi has internet; Settings > Wi-Fi and server to change it. |
| Pairing screen comes back by itself | Someone tapped New link in the app, or unpaired. Type the new code. |
| Controls tab missing | Turn on "Let the display switch my plugs" in the app. |
| Plug shows "?" | The app has not set that plug yet; tap once to switch it on. |

## Arduino IDE instead of PlatformIO
1. Boards Manager: install **esp32 by Espressif**. Board: **ESP32 Dev Module**. Partition scheme: **Huge APP**.
2. Library Manager: **TFT_eSPI**, **XPT2046_Touchscreen** (Paul Stoffregen, 1.4 or newer), **ArduinoJson** 7.x, **WiFiManager** (tzapu). The ESP32 core must be 2.0.x (the firmware uses its built-in QR generator and the 2.0 LEDC calls).
3. Edit the TFT_eSPI library's `User_Setup.h` to match the `build_flags` in `platformio.ini` (ILI9341_2_DRIVER, the pin
   numbers, TFT_BL 21, the fonts, SPI_FREQUENCY 55000000, USE_HSPI_PORT). This is the step most people get wrong, which is
   why PlatformIO is the recommended route.
4. Open `LogMyGrowDisplay/LogMyGrowDisplay.ino` and Upload.

Good general reference for this board: the `witnessmenow/ESP32-Cheap-Yellow-Display` project on GitHub.

## A case to print
Print in **PETG or ASA**, not PLA: a case near a tent sees warm, humid air and PLA softens and warps. Check which board you
have (micro-USB or USB-C, and where the port sits) before you print, since the cut-outs differ. Leave the vent slots
open: the ESP32 runs warm and a sealed box also traps humidity. Mount it **outside** the tent, on the frame, a pole or
your desk, not inside the grow space.

Designs worth a look (search the number on the site):
- **Printables 744864**: CYD case, has a USB-C version.
- **Printables 705053**: wall or pole mountable, with a stylus holder.
- **Printables 840199** and **Printables 645166**: simple snap-together desk stands.
- **MakerWorld 2243030** and **MakerWorld 799188**: desk stand style, print without supports.

Tip: a stand that tilts the screen back about 15 degrees is easiest to read from across the room.

## The feed (for anyone building their own display)
`GET https://logmygrow.com/display/<id>/<token>` returns JSON, no login needed (the token is the secret):
```
{ v: 3, t, unit: "F"|"C", control: false,
  spaces: [{ name, stage, plants, temp, rh, vpd,
             tier, tiers: { temp, rh, vpd },          // "ideal" | "ok" | "risk" | "danger" | "none"
             mold,                                    // "none" | "watch" | "high" | "severe"
             ideal: { temp: [lo,hi], rh: [lo,hi], vpd: [lo,hi] },
             ok:    { temp: [lo,hi], rh: [lo,hi], vpd: [lo,hi] },
             actions: ["Lower humidity to 40-55%", ...], urgency, ageMin }],
  actions: [{ space, text }],                         // up to 5, most urgent first
  devices: [{ id, name, kind, provider, gardenId, space, on }] }   // only when control is on
```
Pairing: `POST /display-pair/start` gives `{ code, secret, expiresIn }`; poll `GET /display-pair/poll/<code>/<secret>`
every few seconds until it returns `{ link }`. Switching: `POST <link>/device` with `{ gardenId, deviceId, on }`.

## Home Assistant version (ESPHome): straight from your HA sensors
`esphome-cyd-ha.yaml` reads a temperature and a humidity sensor from Home Assistant, computes VPD on the device (with a
leaf-temperature offset) and colours it against a band you set. No LogMyGrow account needed. Add it in the ESPHome add-on,
change the entity ids and band in `substitutions`, install. It shows one space; the LogMyGrow version above shows them all.
