# Sapphire board firmware

The programs for the small boards [Sapphire](https://github.com/ddxfish/sapphire)
uses as devices: a voice box, a touchscreen, a USB stick that keeps her backups. Each
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so Sapphire shows only what a board says it has.

## The boards

| Device | Image | Features | Notes |
|---|---|---|---|
| **AI speaker and mic**<br>Waveshare ESP32-S3-AUDIO-Board<br>`ai-speaker-mic/` · 0.9.4 | | • Wake word<br>• Mic<br>• Speaker<br>• Light ring<br>• Screen (optional)<br>• Touch (optional)<br>• Camera (optional)<br>• Chat picker<br>• Buttons<br>• Backups<br>• Updates over WiFi | A voice box: say the wake word, ask, hear her answer. The screen is Waveshare's 2" touch LCD; the camera is any OV2640/OV3660/OV5640. |
| **CYD 2.8" reader**<br>ESP32-2432S028R<br>`cyd/` (`cyd28`) · 0.5.2 | | • Screen<br>• Touch (resistive)<br>• Keyboard<br>• Chat picker<br>• Speaker<br>• LED<br>• Light sensor<br>• Buttons<br>• Backups<br>• Updates over WiFi | A chat in your pocket, a page at a time. Speaker needs an 8 Ω speaker on its JST 1.25 mm plug. |
| **CYD 3.5" reader**<br>ESP32-3248S035C<br>`cyd/` (`cyd35`) · 0.5.2 | | • Screen<br>• Touch (capacitive)<br>• Keyboard<br>• Chat picker<br>• Speaker<br>• LED<br>• Light sensor<br>• Buttons<br>• Backups<br>• Updates over WiFi | Same program as the 2.8", bigger glass. Builds; not yet proven on a board. |
| **Backup stick**<br>LilyGO T-Dongle-S3<br>`tdongle/` · 0.1.4 | | • Backups<br>• Screen<br>• LED<br>• Buttons<br>• Updates over WiFi | Plug into any USB power. Every backup is read back and checked at each start. |

## What the features mean

- **Wake word**: listens on the board for its word, then sends what you say to Sapphire.
- **Mic**: Sapphire can listen through the board on demand; echo cancel and noise reduction on the board.
- **Speaker**: Sapphire's voice plays there, sentence by sentence as she makes it.
- **Light ring / LED**: shows what she is doing (thinking, speaking, a tool), and takes colors and animations she sets.
- **Screen**: the board's own UI, plus her `show` line and pictures pushed to the glass.
- **Touch**: tap to page, open menus and press the on-screen buttons.
- **Keyboard**: type to her on the glass; her reply shows as she writes it.
- **Chat picker**: choose which of her chats the board talks in.
- **Camera**: she can take a picture through the board; it is powered only for the shot.
- **Light sensor**: a room light reading she can ask for.
- **Buttons**: each press can be set in Settings > Devices to send her a message or do a board job.
- **Backups**: sealed copies of her memory kept on the board's SD card, checked against their hash.
- **Updates over WiFi**: new firmware from Sapphire, into a second slot, with rollback if it fails to start.

## Put it on a board

You do not need this repository for that. In Sapphire: Settings > Devices >
**+ Add Satellite**, then **Dev board**. She downloads the board's files from
this repository's release and writes them, from Chrome or Edge or from the
computer she runs on.

## Build it yourself

Needs [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/)
on Linux, macOS or WSL. Set `IDF_PATH` to it, then:

```
bash build.sh cyd28             one board (a folder, or a board of a family: cyd28 and cyd35 are both cyd/)
bash build.sh all               every board
bash build.sh cyd28 clean
bash build.sh cyd28 menuconfig
```

The first build downloads the board's components. Each build ends by writing
one file per board into `dist/` (`tools/pack.py`):

```
dist/index.json                 every board here: version, chip, sha256, what sits where in its file
dist/cyd28-0.5.2.bin            the whole board: bootloader, partition table, program
```

To flash your own build from Sapphire, set **DEVICE_FIRMWARE_SOURCE** in her
settings to the `dist` folder. Each board's `setup_board.py` flashes and sets
up from a terminal instead.

## A release

A release's files are everything in `dist/`: one `.bin` per board and
`index.json`. Sapphire reads `index.json` first and downloads only the board
that was picked.

## By hand

Download your board's `.bin` from the latest release and write it at address
0 with any ESP flasher:

```
esptool.py write_flash 0x0 cyd28-0.5.2.bin
```

This also clears the board's saved name and WiFi, so it starts as a new
board. Set it up in Sapphire afterwards (**+ Add Satellite**).

## A new board

1. Copy the folder of the nearest board. The folder's name is the board's id:
   lower case, digits and dashes.
2. Set the same id as `FIRMWARE_MODEL` in `main/CMakeLists.txt`, and the chip
   in `sdkconfig.defaults` (`CONFIG_IDF_TARGET`).
3. `main/board.c` holds the pins. Start there.
4. The first heading of the folder's `README.md` is the name people pick in
   Sapphire.

A board that keeps two program slots (`partitions.csv`) and says `firmware`
in `/health` gets its updates over WiFi after the first flash.

## Security

Sapphire signs every request it makes of a board with the board's key (see the protocol's
"What Sapphire asks of the board"), so the key never crosses the network; the board's own calls to
Sapphire go over TLS, pinned to her certificate. What a board hears and is sent is still plain HTTP
on your LAN, and anyone with the board in hand (USB) can read its keys: there is no flash
encryption. Keep boards on a network you trust.

## License

The release `.bin` files also hold LVGL (MIT), TensorFlow Lite Micro (Apache 2.0), cJSON (MIT),
ESP-IDF (Apache 2.0) and, in ai-speaker-mic's image, Espressif's esp-sr models (under the exception
in LICENSE); their notices are in each component's source.

The screens draw with Montserrat, compiled into LVGL's own component under
the SIL Open Font License 1.1 (see its `COPYRIGHTS.md`); a font regenerated
for a board here keeps that license beside it.

The code here is under the GNU Affero General Public License v3 (`LICENSE`),
like Sapphire.

Not ours, and under their own terms:

- `ai-speaker-mic/models/hey_jarvis.*`: the wake word model from ESPHome's
  micro-wake-word-models, Apache License 2.0. See the `NOTICE` and `LICENSE`
  in that folder.
- What a build downloads into `managed_components/` (ESP-IDF components,
  LVGL, TensorFlow Lite Micro and others): Apache 2.0 or MIT, each with its
  license in its folder. Two of Espressif's, `esp-sr` and `esp_new_jpeg`, are
  under the "Espressif MIT" license, which allows them on Espressif chips only.

Additional permission under section 7 of the GNU AGPL v3: you may combine
this code with Espressif's `esp-sr` and `esp_new_jpeg` components and convey
the result, with those two components staying under their own license. If
you change this code you may extend the permission to your version or leave
it out. Only the `ai-speaker-mic` program contains either of them.
