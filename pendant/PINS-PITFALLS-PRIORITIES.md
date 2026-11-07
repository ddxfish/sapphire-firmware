# Pins, Pitfalls, Priorities

What one evening of bringing up a software echo canceller on the Elecrow AI Camera board
(ESP32-S3R8, 8 MB octal PSRAM, 16 MB flash, PDM mic, NS4168 amplifier, 1.83" touch glass,
GC2145 camera, SD slot) taught us. Written for the next person who puts esp-sr's AFE on a
board with no hardware echo reference and wonders why the numbers will not sit still.

The short version: the canceller was never the problem. Every bad result traced to something
upstream of it: a task starving the microphone, a reference placed a few milliseconds on the
wrong side of the echo, a resampler whose filter changed shape as it ran. Fix the plumbing,
measure honestly, and the stock low-cost AEC cancels her own voice twenty to eighty times
over on a 240 MHz core with half its time to spare.

## Pins (the hardware as it actually is)

- Console is UART0 through a CH340K (GPIO 43/44). There is no native USB; GPIO 19/20 are camera
  data. Opening the serial port resets the board twice. Do your bench work over WiFi.
- I2C0 (SDA 16, SCL 15) at **100 kHz**. At 400 kHz the CST816T touch chip answered 0xFF bytes
  and the AW9523 expander's id came back bit-flipped. The bus scan shows ghost addresses at
  either speed; ignore them.
- AW9523 expander at 0x5A. P1_0 is the rail for everything (glass, camera, mic, amp, card,
  LEDs). P1_1 fill light. P1_3 backlight cathode in LED mode (the dimmer). On a **V1.0** board
  P1_4 is the amplifier's P-FET (low = on) and the touch reset is GPIO 12; V1.1 moved the amp
  to P1_5 and the touch reset to P1_4. Check which you have before trusting the schematic that
  came with the other one: with the V1.1 map on a V1.0 board the speaker simply goes silent.
- Touch: X straight, Y mirrored, plus about 3 mm of Y offset. Touch reads every 10 ms.
- Mic: PDM on I2S0 (CLK 1, DATA 2, left slot). Amplifier: I2S1 standard stereo (BCLK 5, WS 4,
  DOUT 3), both slots the same sample. Volume is software.
- Camera: GC2145 on DVP, SCCB on its own pins (41/40) as a second I2C port. PSRAM-direct DMA
  tears lines above 5 MHz XCLK on this board; 10 MHz hung it for 90 s. The driver's internal
  ring fixes the tearing but costs 35 KB of internal RAM while the sensor is warm.
- Card: SD over SPI3 (SCK 7, MISO 6, MOSI 8), CS tied on the expander. 20 MHz is reliable;
  ~190 KB/s. Every card read is a polled SPI transaction: it costs CPU in proportion.
- Battery: JST SH 1.0 two-pin, pin 2 positive. The 1.25 mm plug is the speaker. No battery
  sense, no USB-present pin. Light sleep (rail off, WiFi off, wake on GPIO0 or a UART line)
  works; deep sleep is avoided because the ROM samples GPIO0 at wake.

## Pitfalls (symptom, cause, fix)

**The canceller works on some sentences and not others.** Identical sentences cut 1x on one
try and 47x on the next. Cause: a background task was starving the mic feed and the I2S driver
was discarding whole 20 ms descriptors mid-sentence. Each hole shifts the reference against the
echo by 20 ms and the adaptive filter never converges on that sentence. Nothing in the
canceller's own numbers says so; only a drop counter does. Fix: register the driver's
`on_recv_q_ovf` callback, count what it threw away, publish it, and fold the lost samples into
the capture index so the reference stays aligned across a hole. Then remove the cause.

**The starving task ran at priority 2.** It showed up in the task list at 5, then at 20.
That is FreeRTOS priority inheritance: the card integrity check held the storage mutex for the
whole of every 143 MB read-back, and whoever waited on that mutex lent it their priority for
the duration, up to the system event task's 20, above the mic feed's 8. It was also unpinned,
so it landed on the mic's core. Fix: pin background work to the core that has slack behind it
(the glass core, where the AFE's 640 ms ring absorbs a stall; the mic's DMA ring absorbs only
160 ms), and pace it (rest as long as each read took). A task's configured priority is not the
priority it runs at while it holds a lock someone wants.

**60 ms of mic DMA is not enough** under WiFi and TLS on the same core. Eight descriptors of
320 frames (160 ms) cost 2.5 KB more internal RAM and ended the drops from ordinary load.

**The reference landed behind the echo.** A reference fed after the echo it should cancel is
non-causal; the filter cannot model it and you get nothing. A reference fed a few milliseconds
early is fine, the filter spans it (here: 16 ms of lead costs 3x, 32 ms costs 7x). So every
estimate on the mic's clock must err LATE, never early. The pop from the I2S driver returns at
a descriptor's end, 0 to 16 ms after the chunk's last sample (320-frame descriptors against
512-sample pops, the phase repeating every five pops), which makes the clock look late, which
is the safe direction. An all-time minimum of those estimates is NOT safe: as the clock drifts
it goes stale and the reference falls behind for good (four bad sentences in four). Keep the
minimum of the last six.

**A measured sample rate that is "800 ppm slow."** It was not. The start mark was taken from
the first pops after boot, when the DMA backlog made every estimate late, and the error divided
by uptime looked like a rate error that shrank as the board ran. Take the marks ten seconds in,
when the clock window is full. The real clocks read 15999 and 23999 Hz against the timer.

**Correcting for drift made it four times worse.** A two-point interpolator resampling the
reference by the measured clock ratio turned a 32x canceller into 4.6x, on the same build,
toggled live. The step was within 100 ppm of nominal; the damage was the interpolator's phase
walking through the sample grid, which sweeps its high-band response by up to 6 dB. The echo
path the filter had learned kept changing shape under it. At the nominal 2:3 the phase pattern
repeats every three frames and the filter absorbs one fixed response. If you must correct real
drift, use a phase-flat (windowed sinc) resampler, and first prove there is drift to correct.

**Anchoring a sentence on "now."** A first write goes into the descriptor after the one
playing (the driver's queue is one short of the ring), so it comes out when that one ends,
0 to 7 ms later than "now" depending on where in a descriptor the write fell. That is a
different alignment every sentence, and the filter re-converges from it each time. The
amplifier's `on_sent` ISR timestamps every descriptor end; anchor on that end plus one
descriptor, and consecutive sentences share one alignment.

**A start time in the future, in unsigned arithmetic.** Once the anchor was pinned to the
descriptor clock, the stream's start lay up to 7 ms ahead of the write that set it. "Now minus
start" went negative for the next few writes, an unsigned conversion made it enormous, the
drain test fired on every one of them, and each re-anchor overwrote the reference's first chunk
with its second. The sentence then ran with a one- or two-chunk lead, 11 or 21 ms, which the
delay sweep had already priced at 3x to 7x. Whether it bit depended on where in a descriptor
the first write fell: one sentence in six was perfect, five were poor. Keep time differences
signed and clamp them; a test like "played >= written" must not be able to pass on a wraparound.

**A frame rate that four fixes could not move.** Her face painted every 212 ms on a 167 ms
beat. Keeping the TCP connection open: 212. Decoding on the other core: 212. Painting while the
next frame decodes: 212. Panel clock from 40 to 60 MHz and three times fewer draw calls: 207.
Each fix removed a real cost and the number stood still, because the pipeline is a chain of
similar-sized costs (receive, decode, blit, overlay repaint, the sender's round trip) serialized
by one display lock, and a chain does not shorten when one link does. The board's own per-frame
accounting (which it had been sending in every answer, unread) showed it in a minute: no single
large term. The lesson is older than this board: when a number refuses to move, stop fixing
costs and measure the structure. Here the structure is "one lock, one slot, one frame at a
time", and only a different structure (frames batched ahead, the board pacing itself, the blit
outside the lock) changes the number.

Postscript, an hour later. The structure turned out to have one more member, and it was the
one that mattered: with nobody speaking, the same board painted every 143 ms (7 fps, nothing
replaced). The speech-time cost was that her sentences arrive through the same single HTTP
server task that takes the frames, about 97 KB a chunk, and every frame posted during a chunk's
receive waits for it. A switch that silenced the speaker to "free CPU" made it worse (330 ms with
the board 94% idle), because the audio handler then blocked waiting for room and held the task
longer. CPU was never short. A second door for frames (its own server task, its own port) is
the fix, and no amount of tuning on the shared one would have found it. Measure what waits on
what, not what costs what.

Second postscript, the same night. The door was built (a second server on port 81, frames
only, its own task) and at a beat of 5 the number did not move: 239 ms. Reading those results
showed why: the board stamped a frame as painted at the END of its blit and enforced the beat
from that stamp, so every period all evening had been the beat plus the blit, about 40 ms.
Stamped at the start, beat 5 read 203, beat 10 read 108 (9.3 fps, nothing dropped, but the glass
core 86% busy), and the kept beat of 7 reads 146 with the worst frame 157. One more: the plugin
had picked every nth frame of the server's 25, so 7 became "every 4th", 6.25; picking by time
on a grid gives the exact beat. Three different "40 ms" problems in one evening, none of them a
cost, all of them a structure or a stamp.

Third postscript, past midnight. Steady frames did not make her face right: her voice (a WAV
from Sapphire, real time) and her face (from the render pipeline, uneven and a second behind)
were two streams on two clocks, and every way of gluing them failed differently, a drift or a
stutter. The browser never had the problem because the face server hands it audio and frames in
one packet. So the board got the same: packets of 0.96 s of audio plus the frames due within it,
each a key or only the region that changed, and the audio fed to the speaker is the clock the
frames are painted on. First reply: no holes, no late frames, decode 3 to 9 ms for a region,
the glass core two thirds idle. The one bug left was the feeder painting between audio chunks
and starving a 40 ms DMA on every key frame: a painter task, and the voice never waits again.
When two things must agree, do not synchronise them. Make them one thing.

Fourth postscript. "The panel can only take ten frames a second" was true of whole frames: 115 KB
over SPI at 60 MHz is 23 ms, and that is a wall. A talking face does not change wholesale. Once
the stream carried only the region that moved (a key every two seconds, otherwise a mouth-sized
JPEG of 2 to 7 KB, decoded in 1 to 9 ms and painted in 3 to 20), the same panel painted the face
server's native 25 frames a second, 319 frames in a fifteen-second reply, no frame late after
the first key, no hole in her voice, both cores about 40% idle. The wall did not move; the work
stopped going through it. The last fault at that rate was the audio feeder losing the race to
WiFi bursts on its core with 40 ms of DMA margin: 80 ms of margin and one priority step above
the server task helped, and the counter named the rest: four holes a reply with the packet
queue at zero. The board was playing each packet the instant it arrived, and the next slice
came a hair late every time, because the server renders around real time, not on it. One
packet of slack, held before the stream starts, and the holes were gone. A queue that reads
zero is not efficiency; it is a hole waiting to happen.

Fifth postscript, a pitfall and a confession. After a cold boot her tile went black while the
same binary had painted all night. The board took every packet and drained its queue; the
JPEG decoder refused every frame; and both failure exits in the painter were silent, so the
health page said only "frames: 0". The decoder's header states that its output buffer must be
16-byte aligned. The scratch buffer came from a plain malloc that happened to land aligned for
nine hours and then did not. Two fixes, in order of importance: the painter now counts and
names every failure in health (the step, the decoder's error, the buffer's address modulo 16),
and the buffer is allocated aligned. The second fix took one line. The first is the one that
would have made the second take ten minutes instead of an hour, and it is the one a tired
author skips. A requirement a library writes in its header is a requirement; a buffer that
has worked so far is a buffer that has not yet moved.

**A 1 kHz tone is a bad probe.** The noise suppressor eats a steady tone whether or not the
canceller works. Test with speech, and measure the raw mic against the front end's output on
the same chunk: the ratio is the cut, and the room's noise cancels out of it.

**One reading lies.** The first sweep found "34x at delay 0" from a single sentence. Four to
six sentences per configuration, or you are measuring luck. Change one thing per build, and
put knobs on the things you want to compare so you can toggle them live on the same binary;
every high-performance-mode trial we ran before the resampler was found was confounded by it.

**Internal RAM leaks you did not write.** Under `SPIRAM_TRY_ALLOCATE_WIFI_LWIP` the WiFi TX
buffers are static and a `DYNAMIC_TX_BUFFER_NUM` line is dead config: 16 of them are 25.6 KB of
DMA-capable internal RAM; 8 are plenty. esp-sr links the nsnet3 noise model's 19 KB of tables
even when you selected nsnet2; a `linker.lf` mapping moves that `.bss` to PSRAM. The SD driver
mallocs and frees an 8 KB internal bounce buffer per 16 KB write unless you hand it a
permanent `dma_aligned_buffer`. From 69 KB free to 111 KB, largest block 31 to 64 KB.

## Priorities (in this order, because each one hides the next)

1. **The ear must never drop a sample.** The mic feed above every task of yours, a DMA ring
   deeper than your worst stall, nothing unpinned that holds a lock, drops counted and
   published. Until this holds, no canceller number means anything.
2. **Alignment by capture index and the DMA's clock.** Index the reference ring by the mic's
   capture count (drops included). Anchor a stream on the amplifier's descriptor clock, not the
   wall clock. Estimate the mic's clock so that errors lead rather than lag. Leave drift alone
   unless measured honestly and found.
3. **The rig before the fix.** Raw mic level and canceller output on the same chunk, dropped
   samples, the slowest feed, stale pops, both clocks, and every task's share of its core, all
   over WiFi from the running board. Every real cause on this list was found by one of those
   numbers moving when the result did.
4. **Then the quality knobs.** Low-cost and high-performance AFE modes at filter length 4 were
   a wash over six sentences each (means 24x and 19x, worst 6x and 12x), high performance
   costing 6% more of a core. Filter length 8 saturated the core and the drops came back. The
   stock low-cost canceller was the right answer from the start; everything else was plumbing.

## Proof

Same sentence through the pendant, raw mic over front-end output while she speaks, the room's
3D printer running throughout.

| state | cut per sentence | her voice heard as speech by the VAD |
|---|---|---|
| canceller off | 1x | 93% |
| feed starving (card check) | 1x to 47x, by luck | 85 to 98% on the bad ones |
| feed pinned and paced, anchor on the clock window | 18x, 28x, 39x, 85x | 0 to 32% |
| plus the measured-ratio resampler | 1.4x to 5.7x | 80 to 90% |
| resampler off again | 21x, 43x | 26 to 32% |
| low-cost vs high-performance, six each | 24x vs 19x mean | 23 to 35% vs 17 to 40% |
| anchor pinned to the DMA clock, six each: delay 0 / +4 ms / -4 ms | medians 14x / 29x / 30x | best sentences 0 to 8% |

**What is still open.** With the feed clean and the anchor pinned, identical sentences still
land anywhere from 4x to 145x, and no firmware variable moves the spread monotonically: 4 ms
of lag and 4 ms of lead both beat the middle. The best sentences cluster in time. The room's
loudest variable (a 3D printer's stepper squeal through every test above) also comes in phases,
and a linear canceller freezes its adaptation when its double-talk detector hears something that
is not echo. The quiet-room run decides that; it had not happened when this was written. Until
it has, the honest reading is: the plumbing is right, the room is a variable, and a single
evening's best number is not a specification.

Barge-in: she spoke for 25 s, the user said the wake word over her, the pendant claimed the
room, she stopped and answered.

Sixth postscript, the quiet room. With the printer off and the board on its own supply, twelve
sentences through the canceller: a mean cut of 17 to 18 times, a worst sentence of 8 to 11, no
microphone drop in any, at two delay settings that the run could not tell apart. The number that
matters more than the cut is the spread: 8 to 37 times from one sentence to the next with nothing
changed. A canceller's average is a comfort; its worst sentence is what the wake word hears.

Seventh postscript, the hunt and the wake. Eight scouts over 0.4.5 and the core chain brought back
ninety findings; the pattern under most of them was four stand-ins for one fact, "is her voice on,
and whose is it": the halt gate guarded the sentence door and not the stream door, core took three
seconds of recency for ownership, the hook claimed a board on a promise, and five places on the
board asked only the old sentence player. 0.5.0 closed those (one `avatar_voice()` truth, the
stream door answering `stopped`, a stop that reaches the face server by the reply's own id).

The wake was something else, and it was not in the ninety. Krem's board came back from sleep dim,
brightened under a finger, answered nothing, went dark in ten seconds, and BOOT did nothing after.
Read with the IDF source open: esp_timer replays every period a periodic timer missed in light
sleep, one callback per period, unless the timer was made with `skip_unhandled_events`. Three ran
through the pendant's sleep: LVGL's 5 ms tick, the button sampler's 5 ms, mDNS at 100 ms. An
eight-hour sleep is five and three-quarter million tick callbacks, the same again for the buttons,
and three hundred thousand for mDNS, fired back to back on core 0 at priority 22, while LVGL's
clock leaps by the whole sleep. Dim: the glass thinks it has been idle for hours. Unresponsive:
core 0 is the burst's. Touch still brightens: LVGL lives on core 1. Dark: the leaped clock reads
idle past the sleep limit and asks for sleep again. Each BOOT press wakes into the next burst.
0.5.1 stops the tick for the sleep (the port's own stop and resume, inside `ui_sleep`), flags the
sampler to skip, and frees mDNS for the sleep. The lesson is general: on this chip a periodic
esp_timer is a debt that light sleep calls in at the wake.

The other lesson is about evidence. The RESET button is EN, a power-on to the chip, and it threw
away the RTC record every time; a hang or a brownout wrote nothing to begin with. 0.5.0 keeps the
log ring in PSRAM, which no reset but a power cut touches (`SPIRAM_MEMTEST` off), and the next boot
finds the last boot's ring at the same address and serves it at `GET /log?last=1`. It failed the
first time for a reason worth keeping: the heap writes its free-list pointers into the first eight
bytes of the pool's first block at every boot, exactly where the magic was. The magic now sits past
a sixteen-byte guard. Proven across a plain restart; the reset button is next.
