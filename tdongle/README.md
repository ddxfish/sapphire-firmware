# Backup stick (LilyGO T-Dongle-S3)

A USB stick that holds Sapphire's backups on its own card and says whether
they are safe. It needs power only: any USB port or charger. It speaks the
[Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md), so it is added
in Settings > Devices as a **Satellite** that says it has `storage`, a
`screen`, a `light`, `buttons` and `power`.

The board: ESP32-S3 (16 MB flash, no PSRAM), a 0.96" ST7735S screen 160 x 80,
one APA102 LED, the BOOT button, a TF slot inside the USB plug. The card
cannot come out while the stick is plugged in. Use a FAT32 card; the stick
never formats one by itself (Format is a button in Sapphire).

## What it shows

The LED and the two lines of the screen say the same thing:

| LED | Screen | Means |
|---|---|---|
| green, dim | `12/29 GB` / `14  Oct 9` | Room used, how many backups, the newest. All checked, newest under 36 hours old |
| blue, quick pulse | `WRITE 43%` / `do not pull` | A backup is arriving. **Do not unplug** |
| cyan, slow pulse | `CHECKING` / `3 of 14` | Reading every backup back after a start |
| red, blinking | `2 DAMAGED` | Backups that no longer match their hash. Sapphire replaces them at the next backup |
| red | `NO CARD`, or `LAST FAILED` | No card (or not FAT32), or the last backup did not land |
| amber | `no WiFi`, `no Sapphire`, `no backups`, or a date over 36 hours old | Nothing is wrong with the card; nothing new is arriving |
| white, dim | `NOT SET UP` | Set it up in Sapphire |

The screen goes dark after two minutes. The button, a change of status or a
line from Sapphire lights it again. Sapphire's thinking and speaking are not
shown here; a color she sets shows for as long as she set it, under the
blue, cyan and red above.

## The button

A press on a dark screen only lights it. Then: a short press asks Sapphire
for a backup onto this stick, now; a long press turns the screen off. Each
press can be set to something else in Settings > Devices (a message to her,
or nothing).

## Why it can be trusted

- A backup is written to `name.partial` and renamed only when it is whole.
- Its room on the card is taken in one go before the first byte, so the
  card's own table is written at the start and the end, not all through.
- It is **read back from the card** and compared with what arrived. Its
  sha256 is kept beside it (`name.sha256`).
- **At every start** every backup is read back and compared with its hash.
  A pulled plug or a tired card shows up that day, not on the day a backup
  is needed. About 2 MB a second: a minute for every 120 MB on the card.
- Nothing plaintext lands here: a file must be a sealed backup.

Pulling the stick while it writes loses that one backup, never silently: the
half file is dropped at the next start, and every other backup is checked.

Proven on the board 2026-10-09: 32 MB written, read back and kept in 32
seconds; one byte flipped in it on the card was found at the next start.

## Set up a stick

Settings > Devices > **+ New Board** in Sapphire (Chrome or Edge), or from a
terminal: `python setup_board.py flash`, then `setup`, then `test`.

A stick that still runs MicroPython or another program does not enter its
bootloader by itself: hold the button while plugging it in.

## Files

| File | |
|---|---|
| `main/board.c` | pins, the panel's start-up sequence, the LED |
| `main/storage.c` | the card: the same file on every board; this board turns its checks on (`BOARD_STORAGE_CHECKS` in `board.h`) |
| `main/status.c` | the stick's one word, for the LED, the screen and `/health` |
| `main/ui.c` | the two lines |
| `main/light.c` | the LED: status first, then what she set |
| `main/buttons.c` | the button: the same file on every board; its jobs are in `main.c` |
| `main/link.c` | its line to Sapphire: the clock, what the button is set to, a press |
| `main/server.c` | what Sapphire calls |
| `main/ota.c` | her program over the air |
