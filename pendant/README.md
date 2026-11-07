# Sapphire pendant (Elecrow AI Camera board)

The program for the board Sapphire wears: a 1.83" touch glass, a camera, a
microphone, a speaker, a card slot and a battery plug, on a cord. It listens for
its wake word, sends what was said to Sapphire, speaks her answer, takes a
picture when she asks, and shows her face when FlashHead streams it. It speaks
the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so the pendant is added in Settings > Devices as a **Satellite**.

Board: Elecrow AI Camera Development Board (ESP32-S3R8, 8 MB PSRAM, 16 MB flash,
GC2145 camera, PDM mic, NS4168 amplifier, CST816S touch, AW9523 expander).

| Version 0.1 does | Not yet |
|---|---|
| Wake word, then a question to Sapphire; her answer through the speaker | A battery reading: the board has no sense line |
| Echo cancelling with a reference made on the board (`aec` on the console: the R&D knob) | A live viewfinder on the glass |
| A row of three apps on the glass: voice, her face (a stream lands there), settings; swipe up from any for its status | |
| BOOT: press to talk (or stop her), hold to sleep. Sleep cuts the rail; BOOT wakes it | |
| A picture: GC2145 YUV to JPEG on the chip (`snap`), `GET /camera/snap` | |
| Backups on the card, updates over WiFi, the chat picker, her `show` and `picture` | |

## Set up a board

**From the browser** (Chrome or Edge): Settings > Devices > + Add Satellite >
Dev board. Plug the board in, Connect, Install, then a name and the WiFi.

**From a terminal:** needs `esptool` and `pyserial`.

1. `bash build.sh pendant` in the folder above, once.
2. Plug the board in. `python setup_board.py flash` (saves what was on the board to `backups/` first).
3. `python setup_board.py setup`, in your own terminal: WiFi, Sapphire's address, the keys.
4. Settings > Devices > + Add Satellite. Paste the address and the keys.

The board's USB chip is a CH340 (`/dev/ttyUSB0`); there is no native USB.

## The console

`python setup_board.py monitor`, then one command a line: `show`, `selftest`,
`aec` / `aec measure` / `aec delay 80` / `aec on|off`, `snap 800x600`, `sleep`,
`rail 0|1`, `fill 0|1`, `app 0|1|2`, `test` (the touch card), `screen flip 1`,
`i2c`, `heap`, `tasks`, `lastwords`.

## The bench over WiFi (signed calls, like Sapphire's)

`GET /health` has an `aec` block: `out_rms` (the front end's output) against `mic_rms` (the raw
mic) is the canceller's cut while she speaks; `mic_hz` and `spk_hz` are the two clocks measured
against the timer; `mic_dropped_ms`, `feed_max_ms`, `feed_slow`, `pops_stale` say whether the mic
feed is keeping up (any drop mid-sentence and the canceller cannot converge on that sentence).
`GET /log` (the last 160 lines the board logged, oldest first: what happened while nobody watched the console; `GET /log?last=1` is the boot before this one, kept in PSRAM across a restart, a crash or a watchdog, so the story of a death is still there after it) and `GET /stats` is the console's `tasks` without a serial port: every task's share of its core.

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
