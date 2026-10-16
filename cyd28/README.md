# CYD 2.8" reader (ESP32-2432S028R, XPT2046 touch)

A chat with her that fits in a pocket, read a page at a time: a 2.8"
touchscreen in landscape, a keyboard that takes the glass while you type and
leaves when you read, a picker in the bar for which chat you are in. It
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so the board is added in Settings > Devices as a **Satellite** that says it
has a `keyboard`, a `screen`, a `light`, `power`, a `speaker`, `sensors`,
`buttons` and, with a card in, `storage`.

The board: the 2.8" "Cheap Yellow Display" with the resistive touch panel,
ESP32-2432S028R (ESP32-WROOM-32, 4 MB flash, no PSRAM, a 240x320 panel, an
XPT2046 touch chip, one RGB LED, a light sensor, a speaker amplifier, a TF
slot, the BOOT button). The one-USB boards carry an ILI9341 behind the glass
and the two-USB revision an ST7789: the program asks the chip at boot and
`screen panel` on the console overrides a wrong guess.

The chip has two SPI buses and the board uses three: the panel holds one,
the card the other, and the touch chip sits on wires of its own. So the touch
chip is driven by hand (`board.c`, a few lines of bit-banging), which is why
the touch has a calibration of its own: `touch cal` on the console, three
crosses to tap, kept on the board.

| 0.1 does | Not yet |
|---|---|
| Type at her, her reply on the screen as she writes, a page at a time: swipe or the arrows turn it, a tap on `3/7` goes to the end | The light sensor dimming the backlight |
| The chat picker in the bar: every plain chat of hers, a pick lands your words there (`GET /chats`, `POST /chat`) | |
| The chat read back: the page opens on the conversation and paging back fetches older lines without end (`GET /history`) | |
| Pages behind the menu at the bar's left: **Status** (the board and its link), **Backups** (the card, a Back up now, progress as one lands), **Board** (flashlight, brightness, a tone, screen off, reload, restart), **Actions** (nine buttons of your own, none set until you do in Settings > Devices > Buttons: a message for her, or a job of the board's such as backup, flashlight, the chat page; labelled with the message) | A frame page: her pictures, pulled by the board |
| The page it opens on: `screen home backups` on the console, or "Opens on" in Settings > Devices | |
| The chat font 14 to 24 px (`screen font 22`) | |
| Her `screen` / `show`: a line across the top; `picture`: the whole glass | |
| The LED as her `light`, same words as the ring | |
| Her voice through the amplifier (`speaker`, 22.05 kHz mono into the 8-bit DAC) | |
| Backups on the TF card: the four `storage` doors + Format, sealed only | |
| The light sensor as a reading: `sensors: {"light": raw}` | |
| BOOT: a tap shows or hides the keyboard, a hold clears the window; each press can be set in Settings > Devices, and so can the nine on the Actions page | |
| Her firmware over the air: `PUT /firmware` into the other slot, rollback if it fails | |

## Set up a board

**From the browser** (Chrome or Edge): Settings > Devices > + Add Device >
New board (USB). Plug the board in, Connect, Install, then a name and the
WiFi. Sapphire mints the keys and learns the board's address when it calls in.

**From a terminal**: needs `esptool` and `pyserial`
(`pip install esptool pyserial`).

1. `bash build.sh cyd28` in the folder above, once (see its README).
2. Plug the board in with a DATA cable (the board shows up as a CH340 on
   `/dev/ttyUSB0`). `python setup_board.py flash` - the first time it saves
   what was on the board to `backups/` first.
3. `python setup_board.py test` - the test card. If the picture is garbage:
   `screen panel ili9341` (or `st7789`). If red shows as cyan: `screen
   invert 1`; if red and blue trade places: `screen bgr 1`. If TOP is at
   the bottom: `screen flip 1`; if the text reads mirrored: `screen mirror 1`. Then `python setup_board.py touch cal` and
   tap the three crosses. `screen font 18` for bigger text (14 to 24).
4. `python setup_board.py setup` - in your own terminal. WiFi, Sapphire's
   address, her certificate, the two keys.
5. Settings > Devices > + Add Device > Satellite. Paste the address and the
   keys. Press Test now.

Then type on it.

## How it works

```
keyboard --> POST /api/devices/<name>/text                          what was typed
screen   <-- GET  /api/devices/<name>/events  {"state":"text", msg, rev, have, done}   a doorbell
         <-- GET  /api/devices/<name>/text?msg=&from=&max=          the words, pulled
picker   <-- GET  /api/devices/<name>/chats                         the chats it may pick from
         --> POST /api/devices/<name>/chat   {"name"}               the pick
LED      <-- the events stream: thinking, tool, idle, error
Sapphire --> GET /health, POST /screen, POST /led, POST /power    the board's own server, port 80
```

The screen is a window, not a transcript: the last 12 entries, one reply
drawn up to 2400 characters, cut into pages with LVGL's own line breaker so a
page is exactly the lines that fit. Nothing scrolls: a page is a few labels
drawn once, a turn of the page replaces them. Her reply lands in the last
page; a page that fills opens the next, and the view follows her unless you
paged back. Everything stays in the chat on Sapphire's side.

The chat picker: the bar names the chat the board talks in (Sapphire sends
it with `POST /screen {"chat"}`). Opening the picker asks Sapphire for the
plain chats; a pick is the device's **Talks in chat** setting from then on.
The chat open in the browser is never changed by the board.

The pages: the list symbol at the bar's left drops a menu. Every page but
the chat is built when opened and freed when left (`main/pages.c`), so a
page costs RAM only while it shows. Backups asks Sapphire for a backup
through the protocol's `POST /ask {"job": "backup"}` and draws the card's
own progress as the file lands. Actions holds nine touch buttons (`a` to
`i`, "a tap is a short press and nothing else"): Settings > Devices > Buttons
sets each one to a message for her or a job of the board's (`backup`,
`flashlight`, `chat`, `screen`, `clear`, `keyboard`), and the first words of
a message come back on the stream (`labels`) to label the button. Nothing is
set until you set it.

## Files

| File | What |
|---|---|
| `main/main.c` | Boot order |
| `main/board.c` | This board's pins: panel (asked which chip at boot), the bit-banged touch chip and its calibration, backlight, LED, light sensor, button |
| `main/ui.c` | The screen: the chat's pages, the keyboard, the bar with the menu and the picker, the test card, the calibration crosses |
| `main/pages.c` | The other pages: Status, Backups, Board, Actions |
| `main/buttons.c` | The button's timing: short, long, double. The same file on every board |
| `main/link.c` | What the board sends to Sapphire, the events stream, the pulls, the picker's two doors |
| `main/server.c` | The addresses Sapphire calls |
| `main/light.c` | The LED, in the ring's words |
| `main/console.c` | The serial setup line: `show`, `scan`, `setup {json}`, `keys`, `test`, `touch`, `touch cal`, `screen ...`, `light r g b`, `i2c sda scl`, `restart`, `erase` |
| `main/settings.c` | Flash storage of the settings |
| `setup_board.py` | The host side: flash, test, touch, setup, keys, monitor |
