# CYD readers (2.8" resistive and 3.5" capacitive)

A chat with her that fits in a pocket, read a page at a time: a touchscreen
in landscape or portrait, a keyboard that takes the glass while you type and
leaves when you read, a picker in the bar for which chat you are in. It
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so the board is added in Settings > Devices as a **Satellite** that says it
has a `keyboard`, a `screen`, a `light`, `power`, a `speaker`, `sensors`,
`buttons` and, with a card in, `storage`.

One program, two boards (since 0.5.0; the trees had drifted apart within a
week as two copies). `bash build.sh cyd28` or `bash build.sh cyd35` in the
folder above builds it for one of them: everything under `main/` is the same
file for both, and `main/boards/<board>.c` with its `.h` holds what differs:
the pins, the panel and its driver, the touch chip.

| Board | Glass | Touch | Notes |
|---|---|---|---|
| `cyd28` | 2.8" "Cheap Yellow Display", ESP32-2432S028R: a 240x320 panel, an ILI9341 on the one-USB boards or an ST7789 on the two-USB revision (the program asks the chip at boot; `screen panel` overrides a wrong guess) | XPT2046, resistive, on wires of its own (the chip has two SPI buses and the board uses three), driven by hand, so it has a calibration: `touch cal`, three crosses, kept on the board in the panel's own frame, so one calibration serves every rotation | The first board, 2026-10-15 |
| `cyd35` | 3.5", ESP32-3248S035C: an ST7796 320x480, BGR | GT911, capacitive, on I2C, polled (its INT line is soldered to ground by a routing mistake on this board) | Was the 0.4.5 pocket with the pet; GPIO 21 is that INT line and is never driven |

Both: ESP32-WROOM-32 (4 MB flash, no PSRAM), one RGB LED, a light sensor, a
speaker amplifier on the DAC pin, a TF slot, the BOOT button.

| 0.5 does | Not yet |
|---|---|
| Type at her, her reply on the screen as she writes, a page at a time: a tap on the top half of the page turns it back, on the bottom half on, a swipe does the same, and on a wide bar the arrows too; a tap on `3/7` goes to the end | The light sensor dimming the backlight |
| Rotation: `screen rotate 0|90|180|270` on the console or "Rotation" in Settings > Devices. The bar, the pages and the touch follow the size the glass has | |
| The chat picker in the bar: the chat the board talks in, or with "Browse chats" ticked in Settings > Devices every plain chat of hers; a pick lands your words there (`GET /chats`, `POST /chat`) | |
| The chat read back: the page opens on the conversation and paging back fetches older lines without end (`GET /history`) | |
| Pages behind the menu at the bar's left: **Status** (the board and its link), **Backups** (the card, a Back up now, progress as one lands), **Board** (flashlight, brightness, a tone, screen off, reload, restart), **Actions** (nine buttons of your own, none set until you do in Settings > Devices > Buttons: a message for her, or a job of the board's such as backup, flashlight, the chat page; labelled with the message) | A frame page: her pictures, pulled by the board |
| The page it opens on: `screen home backups` on the console, or "Opens on" in Settings > Devices | |
| The chat font 14 to 24 px (`screen font 22`; the 2.8" starts at 16, the 3.5" at 18) | |
| Her `screen` / `show`: a line across the top; `picture`: the whole glass, whichever way it sits | |
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

1. `bash build.sh cyd28` (or `cyd35`) in the folder above, once (see its README).
2. Plug the board in with a DATA cable (the board shows up as a CH340 on
   `/dev/ttyUSB0`). `python setup_board.py flash` (`--board cyd35` for the
   3.5") - the first time it saves what was on the board to `backups/`
   first. With no build here it writes `dist/<board>-*.bin` whole, which
   starts the settings over.
3. `python setup_board.py test` - the test card. If the picture is garbage:
   `screen panel ili9341` (or `st7789`). If red shows as cyan: `screen
   invert 1`; if red and blue trade places: `screen bgr 1`. If TOP is at
   the bottom: `screen rotate 180`; if the text reads mirrored: `screen
   mirror 1`. On the 2.8": `python setup_board.py touch cal` and tap the
   three crosses. `screen font 18` for bigger text (14 to 24). Once the
   board is added (step 5), rotation, invert and mirror live in its
   Screen tab too, and a save there is what the board keeps.
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
         <-- GET  /api/devices/<name>/history?before=&max=          the chat read back
LED      <-- the events stream: thinking, tool, idle, error
Sapphire --> GET /health, POST /screen, PUT /screen/settings, POST /led, POST /power    the board's own server, port 80
```

The screen is a window, not a transcript: the last 24 entries, one reply
drawn up to 3000 characters, cut into pages with LVGL's own line breaker so a
page is exactly the lines that fit. Nothing scrolls: a page is a few labels
drawn once, a turn of the page replaces them. Her reply lands in the last
page; a page that fills opens the next, and the view follows her unless you
paged back. Everything stays in the chat on Sapphire's side.

How the glass sits: the panels are portrait glass turned in hardware.
`rotation` (kept on the board) tells the panel how to sit, sizes the screen
(the bar lays itself out for the width it has: arrows and the clock on a
wide one, the page count alone on a narrow one) and maps the touch the same
way. A touch chip reports in the panel's own frame (the resistive glass gets
there through its calibration), and `board.c` turns that into the screen as
it sits now. A change restarts the board.

The chat picker: the bar names the chat the board talks in (Sapphire sends
it with `POST /screen {"chat"}`). Opening the picker asks Sapphire for the
chats it may show: its own, or every plain chat with "Browse chats" on; a
pick is the device's **Talks in chat** setting from then on. The chat open
in the browser is never changed by the board.

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
| `main/board.c` | What every CYD shares: the words kept in flash, backlight, LED, light sensor, button, how the glass sits and the touch mapped the same way |
| `main/boards/cyd28.c`, `.h` | The 2.8": its pins, the panel asked which chip it is, the bit-banged touch chip and its calibration |
| `main/boards/cyd35.c`, `.h` | The 3.5": its pins, the ST7796, the GT911 on I2C |
| `main/ui.c` | The screen: the chat's pages, the keyboard, the bar with the menu and the picker, the test card, the calibration crosses |
| `main/pages.c` | The other pages: Status, Backups, Board, Actions |
| `main/buttons.c` | The button's timing: short, long, double. The same file on every board |
| `main/link.c` | What the board sends to Sapphire, the events stream, the pulls, the picker's doors, the chat read back |
| `main/server.c` | The addresses Sapphire calls |
| `main/light.c` | The LED, in the ring's words |
| `main/console.c` | The serial setup line: `show`, `scan`, `setup {json}`, `keys`, `test`, `touch`, `touch cal`, `screen ...`, `light r g b`, `i2c sda scl`, `restart`, `erase` |
| `main/settings.c` | Flash storage of the settings |
| `setup_board.py` | The host side: flash (`--board`), test, touch, setup, keys, monitor |
