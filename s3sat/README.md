# Sapphire S3 satellites: the pendant and the AI speaker

One program, two boards (1.0.0, 2026-11-12; it follows pendant 0.6.1 and ai-speaker-mic 0.9.5):
`bash build.sh pendant` or `bash build.sh ai-speaker-mic`. `main/boards/<board>.c` and `.h` (and
`main/boards/<board>/`: its audio and camera) hold what differs; `main/boards/<board>.defaults` its
configuration (console, camera sensors and their bus). Everything else is shared, so a lesson found on
one board lands on both. Both speak the
[Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md) and are
added in Settings > Devices as a **Satellite**.

| | Pendant | AI speaker |
|---|---|---|
| Board | Elecrow AI Camera (ESP32-S3R8, GC2145, PDM mic, NS4168, CST816S, AW9523) | Waveshare ESP32-S3-AUDIO-Board (ES8311 + ES7210, TCA9555, 7-LED ring, OV5640) |
| Glass | 240x284 portrait (her face box 240x288, 2 px under each curved edge) | 320x240 landscape |
| Her voice | 24 kHz (16 live), echo reference made in software | 16 kHz (one clock with the mics), echo reference looped back in hardware |
| Console | UART (CH340, `/dev/ttyUSB0`; each open resets it) | USB-Serial-JTAG (`/dev/ttyACM0`) |
| Sleep | rail off, BOOT wakes; dead-battery guard | never sleeps (no rail) |

What both do:
- Tiles: BACKUPS, SETTINGS, VOICE (home), AVATAR, ACTIONS; swipe up from any for its status. It opens on
  the tile it was left on.
- Her voice and face as ONE stream (`POST /face/packet` on port 81): only the changed parts of her face
  cross the air, her voice in IMA ADPCM (a quarter of PCM), the audio is the clock. The board tells
  Sapphire when her tile shows or hides (nothing is sent while she is hidden).
- Actions: nine buttons of your own, each set in Settings > Devices > Buttons.
- Awake hours (Settings > Devices > Power): inside them she lives on a dimmed screen, outside an idle
  screen goes dark and she wakes for a reply.
- Older Sapphire (2.16.x, no avatar hub): the stream still works. Its 24 kHz voice is taken down to the
  speaker's 16 kHz on the board, its square frames are centred in the face box, and the board stops
  asking a door that is not there.

## Set up a board

**From the browser** (Chrome or Edge): Settings > Devices > + Add Satellite >
Dev board. Plug the board in, Connect, Install, then a name and the WiFi.

**From a terminal:** needs `esptool` and `pyserial`.

1. `bash build.sh pendant` (or `ai-speaker-mic`) in the folder above, once.
2. Plug the board in. `python setup_board.py flash --board pendant` (saves what was on the board to `backups/` first; without `--board` it asks before writing to the board its USB chip suggests).
3. `python setup_board.py setup`, in your own terminal: WiFi, Sapphire's address, the keys.
4. Settings > Devices > + Add Satellite. Paste the address and the keys.

The pendant's USB chip is a CH340 (`/dev/ttyUSB0`); the speaker's is the S3's own (`/dev/ttyACM0`).

## The console

`python setup_board.py monitor`, then one command a line: `show`, `selftest`,
`aec` / `aec measure` / `aec delay 80` / `aec on|off`, `snap 800x600`, `sleep`,
`rail 0|1`, `fill 0|1` (pendant), `app 0..4`, `test` (the touch card), `screen flip 1`,
`i2c`, `heap`, `tasks`, `lastwords`.

## The bench over WiFi (signed calls, like Sapphire's)

`GET /health` has an `aec` block: `out_rms` (the front end's output) against `mic_rms` (the raw
mic) is the canceller's cut while she speaks; `mic_hz` and `spk_hz` are the two clocks measured
against the timer; `mic_dropped_ms`, `feed_max_ms`, `feed_slow`, `pops_stale` say whether the mic
feed is keeping up (any drop mid-sentence and the canceller cannot converge on that sentence).
`GET /log` (the last 160 lines the board logged, oldest first: what happened while nobody watched the console; `GET /log?last=1` is the boot before this one, kept in PSRAM across a restart, a crash or a watchdog, so the story of a death is still there after it; the RESET button is a power-on to the chip and the PSRAM loses its refresh for the reset: the lines come back bit-rotted or blank, 2026-11-08; `/health` → `sleep` is the trail that does read through it: how far the last sleep and its wake got, its heartbeats, its wake cause) and `GET /stats` is the console's `tasks` without a serial port: every task's share of its core.

`POST /mic?...` knobs: `speaker_rate=16000|24000` (her voice; Kokoro makes 24 kHz; live),
`aec=on|off`, `aec_delay=-100..500` (ms; negative holds the MIC that long, more lead for the
canceller), `afe_mode=0|1` (low cost, high performance) and `aec_filter=1..8` (both restart the
board), `ref_ratio=on|off` (bench only: resample the reference by the measured clock ratio; OFF
by default, it cut the canceller to a quarter: a walking interpolator phase wobbles the high band).
`POST /storage/check` reads every backup back against its hash; `?stop=1` ends the running one.
Policy: a backup is read back the moment it lands, the Backups page's button checks all (and
stops a check), boot checks only the newest.

Her face lives on the AVATAR tile, to the right of home, with nothing else on it; a stream opens
it by itself, and the centre of her face is the talk tap. From 0.2.0 her voice and her face come
as ONE STREAM: packets of ~0.96 s of audio plus the frames due within it (a key frame, or only the
region that changed), through `POST /face/packet` on the glass door (port 81, `avatar.c`). The
audio fed to the speaker is the clock each frame is painted on, so voice and face cannot drift;
a painter task paints, the feeder never waits for it; one packet of slack is held before it plays (`screen.av_buffer`, the Devices field "Stream slack", default 2: at 1 any late slice is a hole in her voice). Her idle rides the same stream (0.3.0): packets without audio are paced on the wall over the span their head states, the speaker is hers only while packets carry her voice, and her idle neither opens her tile nor wakes a dimmed screen; the board answers `shown` on every packet, and the sender keeps her idle full only while her tile is up on a lit screen (a key every two seconds otherwise, so she is fresh the moment the tile comes up). `/health screen.av_stream` says packets,
holes (gaps in her voice), frames, late, queued, gap, decode and paint. The beat (`screen.beat`,
the Devices page field, default 25, the face server's own rate) sets which frames the plugin
picks: with region frames the panel bus no longer caps it (whole frames capped at ~10 a
second; at 25 the pendant paints 22-27 with both cores ~40% idle). Any avatar plugin that
hands core such packets (`glass.av_packet`) streams the same way; the format is in `avatar.h`
and the protocol doc. A board that does not state `screen.av` still gets frames alone through
`/screen/picture`, paced by core.

What held on the bench (2026-11-02): low cost, filter 4, delay 0, 24 kHz voice: 18-85x cut on a
sentence, her own voice gone from the VAD. Filter 8 saturates core 0. High performance is a
wash at +6% CPU. The card check must stay off the mic's core and paced: unpinned, it inherited
priority 20 through the storage lock and starved the feed.
