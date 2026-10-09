# Sapphire board firmware

The programs for the small boards [Sapphire](https://github.com/ddxfish/sapphire)
uses as devices: a voice box, a touchscreen, a USB stick with a screen. Each
speaks the [Satellite Protocol](https://github.com/ddxfish/sapphire/blob/main/docs/SATELLITE-PROTOCOL.md),
so Sapphire shows only what a board says it has.

| Folder | Board | State |
|---|---|---|
| `ai-speaker-mic/` | Waveshare ESP32-S3-AUDIO-Board: mics, speaker, light ring, optional screen and camera | works |
| `cyd35/` | 3.5" Cheap Yellow Display, ESP32-3248S035C, capacitive touch (GT911) | works |
| `cyd28/` | 2.8" Cheap Yellow Display, ESP32-2432S028R, resistive touch (XPT2046) | not started |
| `tdongle/` | LilyGO T-Dongle-S3: small screen, one LED, one button | not started |

## Put it on a board

You do not need this repository for that. In Sapphire: Settings > Devices >
**+ Add Device** > **New board (USB)**. She downloads the board's files from
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
what a flasher needs into `dist/`:

```
dist/index.json                 the boards in this folder
dist/cyd35-manifest.json        version, chip, the parts and where they go
dist/cyd35-*.bin                the parts
dist/cyd35-README.md
```

To flash your own build from Sapphire, set **DEVICE_FIRMWARE_SOURCE** in her
settings to the `dist` folder. Each board's `setup_board.py` flashes and sets
up from a terminal instead.

## A release

A release's files are everything in `dist/`, as they are. The names carry the
board because a release has one flat list of files. Sapphire reads
`index.json` first and downloads only the board that was picked.

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
