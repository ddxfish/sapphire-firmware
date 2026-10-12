# Sapphire board firmware

The programs for the small boards [Sapphire](https://github.com/ddxfish/sapphire)
uses as devices: a voice box, a touchscreen, a USB stick that keeps her backups. Each
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so Sapphire shows only what a board says it has.

| Folder | Board | State |
|---|---|---|
| `ai-speaker-mic/` | Waveshare ESP32-S3-AUDIO-Board: mics, speaker, light ring, optional screen and camera | works |
| `cyd35/` | 3.5" Cheap Yellow Display, ESP32-3248S035C, capacitive touch (GT911) | works |
| `cyd28/` | 2.8" Cheap Yellow Display, ESP32-2432S028R, resistive touch (XPT2046) | not started |
| `tdongle/` | LilyGO T-Dongle-S3: a backup stick. A card for her backups, a small screen and an LED that say whether they are safe | works |

## Put it on a board

You do not need this repository for that. In Sapphire: Settings > Devices >
**+ Add Satellite**, then **Dev board**. She downloads the board's files from
this repository's release and writes them, from Chrome or Edge or from the
computer she runs on.

## Build it yourself

Needs [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/)
on Linux, macOS or WSL. Set `IDF_PATH` to it, then:

```
bash build.sh cyd35             one board
bash build.sh all               every board
bash build.sh cyd35 clean
bash build.sh cyd35 menuconfig
```

The first build downloads the board's components. Each build ends by writing
one file per board into `dist/` (`tools/pack.py`):

```
dist/index.json                 every board here: version, chip, sha256, what sits where in its file
dist/cyd35-0.3.0.bin            the whole board: bootloader, partition table, program
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
esptool.py write_flash 0x0 cyd35-0.3.0.bin
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

## License

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
