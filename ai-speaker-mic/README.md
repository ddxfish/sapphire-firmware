# AI speaker and mic (Waveshare ESP32-S3 audio board)

The program for a small voice box: it listens for its wake word, sends what
was said to Sapphire, and speaks her answer. It speaks the
[Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md), so the box is added in
Settings > Devices as a **Satellite**, like a Raspberry Pi body.

First board: the Waveshare ESP32-S3-AUDIO-Board (two mics, a speaker, a ring
of 7 lights, 8 MB PSRAM, 16 MB flash).

| Version 0.3 does | Not yet |
|---|---|
| Wake word, then a question to Sapphire | The ring as a light she can set |
| Her answer through the speaker | The touch top |
| `listen` on demand | |
| Restart and sleep from Sapphire | |
| Backups on the SD card (0.2.0) | |
| Updates over WiFi: `PUT /firmware` into the other 4 MB slot, rollback if it fails (0.3.0, ota.c) | |
| `scan` on the console and `mac` in /health, for the browser flasher (0.2.3) | |
| Where the time goes: console `tasks` (each task's share of a core, its core), `i2c` (who answers on the bus), `cpu` idle per core in /health (0.3.1) | |
| The glass (0.4.0): Waveshare's 2inch Touch LCD on the 18 pin plug. The scene of her chat as the background (`POST /screen/background`, pushed by Sapphire), her `show` line and `picture`, what she heard, the ring's color as a halo. Five touch zones: the corners toggle the ring, the screen, the wake word and the speaker; the middle listens now, or stops her while she speaks | |
| `/mute` (the speaker), `/screen/power`; `heap`, `test` and `screen invert\|flip\|bright` on the console (0.4.0) | |
| Staying up and telling why (0.5.2): the glass's doors give up after half a second (503), a glass stuck 10 s restarts the board with every task's call chain kept (`lastwords`, `where` for the live board, `last_words` + `reset` + `problems` + `bus` in /health), a crash leaves a core dump summed up at the next boot; `fault glass\|crash` on the console proves both | |
| The glass, laid out (0.5.3 to 0.5.5): clock top middle with a gear (tap either: the status page), the chat's name bottom middle, four round toggles, the chat's scene behind; `POST /screen {"chat"}`, `PUT /screen/settings` (brightness, dim, dim after, dark after); the RAM diet (60 KB internal free); console `test scene [off]`, `test chat <name>`, `lastwords forget` | |
| The eye (0.5.0): the kit's OV5640 (any OV2640/OV3660/OV5640) on the 24 pin plug, powered ONLY for a shot. `GET /camera/snap?b64=true&size=800x600`: the ring blinks white and two notes sound, then one JPEG from the sensor itself. `camera` in `has` only when a sensor answered at boot | |

## Set up a board

**From the browser** (Chrome or Edge): Settings > Devices > + Add Device >
New board (USB). Plug the board in, Connect, Install, then a name and the
WiFi (`scan` lists what the board can see, 0.2.3). Sapphire mints the keys
and learns the board's address when it calls in.

**From a terminal:**

Needs `esptool` and `pyserial` (`pip install esptool pyserial`).

1. `bash build.sh ai-speaker-mic` in the folder above, once (see its README).
2. Plug the board in. `python setup_board.py flash`
   The first time, it saves what was on the board to `backups/` first.
3. `python setup_board.py test`
   Plays a tone through the speaker and reports whether the mic heard it.
4. `python setup_board.py setup`
   Run this one in your own terminal. It asks for the WiFi name and password
   and Sapphire's address, fetches her certificate so the board trusts that
   one only, makes the two keys, and prints them. `python setup_board.py keys`
   shows them again.
5. Settings > Devices > + Add Device > Satellite. Paste the address and the keys.

After that the board only needs power.

## How it works

```
mics --> codec --> AFE (echo cancel, noise, VAD) --> wake word --> POST /wake, record --> POST /voice
speaker <-- codec <-- /audio/speak (16 kHz WAV, played as it arrives, on its own task) and /audio/stop
ring <-- its own state, and GET /events from Sapphire (thinking, tool, idle, error, standdown)
```

The sound runs at 16 kHz both ways: what the wake word and speech
recognition want, and no decoding on the board. `/health` says so in `plays`,
and Sapphire converts her voice to match.

The board reaches Sapphire over HTTPS and checks her certificate against the
one it was given at setup. Sapphire reaches the board over plain HTTP with a
key, on the home network only.

**The SD card (0.2.0).** A FAT32 card in the TF slot makes the board a backup
target: `/health` says `storage` in `has`, and every night Sapphire sends a
sealed copy of her memory to `/sapphire-backups` on the card, keeps the counts
set on the device page and drops the opener scripts beside the archives. Pull
the card and any computer opens them with the backup password. The board
refuses anything that is not a sealed backup (the `SAPPHIREBAK` magic on the
first 12 bytes), so plaintext never lands here even through a bug on her
side. The slot is wired CLK 40, CMD 42, D0 41, with the card's D3/CS on the
expander (EXIO3, held high): SDMMC 1-bit. No card at boot = no storage, the
rest runs as before. A card over 32 GB ships exFAT and will not mount; format
it FAT32 first (the device page's Format button does, once it mounted once).
A 70 MB backup takes a minute or two over WiFi; the board keeps answering
meanwhile, the write runs on its own task.

## Files

| File | What |
|---|---|
| `main/main.c` | Boot order |
| `main/board.c` | This board's pins and codec bring-up. Another board gets its own |
| `main/audio.c` | The codec both ways, the mic pipeline through the AFE |
| `main/wake.c` | The wake word model, recording until the speaker stops |
| `main/server.c` | The addresses Sapphire calls |
| `main/link.c` | What the board sends to Sapphire, and the events stream |
| `main/light.c` | The ring |
| `main/console.c` | The USB setup line |
| `main/settings.c` | Flash storage of the settings |
| `main/storage.c` | The SD card: mount, the sealed-only write, format |
| `main/ui.c` | The glass: the scene, the five touch zones, her line, her picture, what she heard |
| `main/buttons.c` | BOOT as a button (0.6.0): short, long and double press. Left alone a short press listens, or stops her while she speaks. The three keys on the port expander (Key1 to Key3, read 30 times a second) and the glass's four corners (a tap is a short press) are buttons too (0.6.1); left alone the keys turn the wake word, the speaker and the ring on or off. In Settings > Devices each press can be set to a message for Sapphire or to one of the board's own jobs (listen, ring, screen, wake word, speaker); the board is told on its events stream. The same file on every board; the jobs are in `main.c` |
| `main/camera.c` | The eye: the probe at boot, a shot on demand, powered down between |
| `main/stats.c` | Where the time goes: run-time stats, idle per core |
| `main/lv_mem_psram.c` | LVGL's heap on PSRAM |
| `setup_board.py` | The host side: flash, setup, test, monitor |
