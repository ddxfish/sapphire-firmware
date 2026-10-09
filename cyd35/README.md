# CYD 3.5" touchscreen (ESP32-3248S035C, GT911 touch)

A chat with her that fits in a pocket: a 3.5" touchscreen with an on-screen
keyboard. Type, and her reply streams onto the screen as she writes it. It
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md), so the
board is added in Settings > Devices as a **Satellite** that says it has a
`keyboard`, a `screen`, a `light` and `power`: no mic, no Mic tab.

First board: the 3.5" "Cheap Yellow Display" with the capacitive touch panel,
ESP32-3248S035C (ESP32-WROOM-32, 4 MB flash, no PSRAM, ST7796 320x480 in BGR,
GT911 touch, one RGB LED, a light sensor, a speaker amplifier, a TF slot).
It was sold as a 2.8" with a CST820; what it is was read off the chips:
`i2c 33 32` on the console found the GT911 at 0x5D, and the GT911's own
config says 320 x 480. Its INT line is soldered to ground by a routing
mistake on this board, so the touch chip is polled, never interrupt-driven -
and the LVGL port must not be told about an INT pin, or it waits for an
interrupt that never comes.

| Version 0.3 does | Not yet |
|---|---|
| Type at her, her reply on the screen as she writes | The light sensor dimming the backlight |
| Her `screen` / `show`: a line across the top; `picture`: the whole glass | More screens |
| The LED as her `light`, same words as the ring | Stored sounds (`/sounds` is empty) |
| Restart and sleep from Sapphire | |
| The last 12 bubbles; the whole chat stays with her | |
| Her voice through the amplifier: `speaker`, 22.05 kHz mono into the 8-bit DAC, `volume` kept (0.2.0) | |
| Backups on the TF card: the four `storage` doors + Format, sealed only, sha256 checked, same storage.c as the audio board (0.2.0) | |
| The light sensor as a reading: `sensors: {"light": raw}` in /health (0.2.0) | |
| Her firmware over the air: `PUT /firmware` into the other slot, rollback if it fails (0.3.0, ota.c; two 1.9 MB slots on the 4 MB chip) | |
| `scan` on the console and `mac` in /health, for the browser flasher (0.2.1) | |

Proven 2026-10-06: typed "hey", her reply on the glass as she wrote it, her
`screen` / `show` line, the Keyboard/Screen/Light/Power tabs and no Mic tab
in Settings > Devices. 145 KB of RAM free with everything running.

## Set up a board

**From the browser** (Chrome or Edge): Settings > Devices > + Add Device >
New board (USB). Plug the board in, Connect, Install, then a name and the
WiFi (`scan` lists what the board can see, 0.2.1). Sapphire mints the keys
and learns the board's address when it calls in.

**From a terminal**: needs `esptool` and `pyserial`
(`pip install esptool pyserial`).

1. `bash build.sh cyd35` in the folder above, once (see its README).
2. Plug the board in with a DATA cable (many micro-USB cables carry power
   only; the board shows up as a CH340 on `/dev/ttyUSB0`).
   `python setup_board.py flash` - the first time it saves what was on the
   board to `backups/` first.
3. `python setup_board.py test` - the test card. If red shows as cyan:
   `python setup_board.py screen invert 1`. If TOP is at the bottom:
   `python setup_board.py screen flip 1`. Tap the card: where you tapped prints
   in the terminal. `screen font 20` for bigger text (14, 16, 18, 20).
4. `python setup_board.py setup` - in your own terminal. WiFi, Sapphire's
   address, her certificate, the two keys.
5. Settings > Devices > + Add Device > Satellite. Paste the address and the
   keys. Press Test now: the Keyboard, Screen, Light and Power tabs appear.

Then type on it.

## How it works

```
keyboard --> POST /api/devices/<name>/text                          what was typed
screen   <-- GET  /api/devices/<name>/events  {"state":"text", msg, rev, have, done}   a doorbell
         <-- GET  /api/devices/<name>/text?msg=&from=&max=          the words, pulled
LED      <-- the events stream: thinking, tool, idle, error
Sapphire --> GET /health, POST /screen, POST /led, POST /power    the board's own server, port 80
```

Her words never ride the events stream: a stream that falls behind drops a
line, and a dropped line of prose is garbage on a screen. The stream rings a
doorbell; the board pulls what it lacks. A missed doorbell is made good by
the next one. One connection holds the stream and one more comes and goes
for a post or a pull, so this board never holds more than two TLS sessions
(~50 KB of its ~200 KB).

The screen is a window, not a transcript: the last 12 bubbles, one reply
drawn up to 2400 characters. Everything stays in the chat on Sapphire's side.

BOOT button: a tap shows or hides the keyboard (and wakes the screen); two
seconds clears the window. The backlight dims after 90 s untouched.

Opening the serial port resets the board (the CH340 auto-reset wiring), so
`setup_board.py` waits for the boot after every open. Bring-up tools on the
console: `i2c <sda> <scl>` scans a bus, `touch` and `gt` read the touch chip
raw, `test` is the card.

## Files

| File | What |
|---|---|
| `main/main.c` | Boot order |
| `main/board.c` | This board's pins: panel, touch, backlight, LED, light sensor, button |
| `main/ui.c` | The screen: bubbles, keyboard, status bar, the test card |
| `main/link.c` | What the board sends to Sapphire, the events stream, the pulls |
| `main/server.c` | The addresses Sapphire calls |
| `main/light.c` | The LED, in the ring's words |
| `main/console.c` | The serial setup line: `show`, `scan`, `setup {json}`, `keys`, `restart`, `erase`, the bring-up tools |
| `main/settings.c` | Flash storage of the settings |
| `setup_board.py` | The host side: flash, test, setup, keys, monitor |
