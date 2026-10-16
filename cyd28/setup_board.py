#!/usr/bin/env python3
"""Set up a Sapphire reader board (the 2.8" resistive Cheap Yellow Display) over USB.

    python setup_board.py status     what is on the port, and what the board says about itself
    python setup_board.py flash      write the firmware from build/ (saves the old contents first)
    python setup_board.py test       the test card: say which way is up and whether the colors are right
    python setup_board.py screen invert 1 | flip 1 | mirror 1 | bgr 1 | bright 70 | panel ili9341     fix what the test card showed
    python setup_board.py screen font 18 | home backups           bigger text (14..24); the page it opens on
    python setup_board.py touch      the touch chip raw for 10 s: press, and the numbers print
    python setup_board.py touch cal  three crosses to tap; the calibration is kept on the board
    python setup_board.py setup      WiFi, name, keys and Sapphire's address: run this in your own terminal
    python setup_board.py keys       the two keys again, for Settings > Devices
    python setup_board.py monitor    watch the board's log

Run it in an environment that has esptool and pyserial. Build
the firmware first, in the firmware folder: bash build.sh <this folder's name>

The board's USB chip is a CH340 (vendor 1a86), which shows up as /dev/ttyUSB0.
Only that chip is looked for, so an Espressif board on the same computer is
never touched.
"""
import argparse
import getpass
import json
import os
import secrets
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import urlsplit

HERE = Path(__file__).absolute().parent
ROOT = HERE.parent                                       # the firmware folder
BUILD = HERE / 'build'
CACHE = ROOT / 'backups'
CH340 = 0x1A86
FLASH_BYTES = 0x400000                                   # 4 MB
BAUD = 115200

NO_ACCESS = """
This user may not use {port}. Run this once, then unplug and replug the board:

  echo 'SUBSYSTEM=="tty", ATTRS{{idVendor}}=="1a86", TAG+="uaccess", ENV{{ID_MM_DEVICE_IGNORE}}="1"' | sudo tee /etc/udev/rules.d/71-ch340.rules
  sudo udevadm control --reload && sudo udevadm trigger
"""


def find_port(wanted=None):
    if wanted:
        return wanted
    from serial.tools import list_ports
    found = [p.device for p in list_ports.comports() if p.vid == CH340]
    if not found:
        sys.exit("No CH340 board found on USB. Is the display plugged in with a DATA cable?")
    if len(found) > 1:
        sys.exit(f"More than one CH340 board found: {', '.join(found)}. Pick one with --port.")
    return found[0]


def check_access(port):
    if os.name != 'nt' and not os.access(port, os.R_OK | os.W_OK):
        sys.exit(NO_ACCESS.format(port=port))


def esptool(*args, timeout=900):
    return subprocess.run([sys.executable, '-m', 'esptool', *args], text=True, timeout=timeout)


# --- talking to the board over its serial line ---------------------------------------

class Board:
    """One line in, one ">> " line back. Opening the port resets the board
    (the CH340 asserts DTR and RTS before anyone can hold them), so each
    open waits for the boot: about a second, two and a half to be sure."""

    def __init__(self, port, loud=False):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, BAUD, 1
        self.s.dtr = self.s.rts = False
        self.s.open()
        time.sleep(2.5)
        self.s.reset_input_buffer()
        self.loud = loud

    def ask(self, line, wait=10, tries=3):
        for _ in range(tries):
            self.s.reset_input_buffer()
            self.s.write((line + '\n').encode('utf-8'))
            self.s.flush()
            end = time.time() + wait
            while time.time() < end:
                raw = self.s.readline().decode('utf-8', 'replace').strip()
                at = raw.find('>> ')
                if at >= 0:
                    try:
                        return json.loads(raw[at + 3:])
                    except ValueError:
                        break
                elif raw and self.loud:
                    print('   ' + raw)
        return None

    def wait_for(self, prefix, seconds, echo=None):
        end = time.time() + seconds
        while time.time() < end:
            raw = self.s.readline().decode('utf-8', 'replace').strip()
            if raw.startswith(prefix):
                return raw[len(prefix):].strip()
            if echo and raw.startswith(echo):
                print('  ' + raw)
        return None

    def close(self):
        self.s.close()


def with_board(port, fn, loud=False, tries=20):
    import serial
    for _ in range(tries):
        try:
            b = Board(port, loud)
            try:
                return fn(b)
            finally:
                b.close()
        except (OSError, serial.SerialException):
            time.sleep(1)
    sys.exit(f"Could not open {port}.")


# --- commands --------------------------------------------------------------------------

def cmd_status(port):
    print(f"Port: {port}")
    said = with_board(port, lambda b: b.ask('show'))
    if not said:
        print("The board did not answer. Is the firmware on it? Run: flash")
        return
    print(f"Firmware  {said.get('firmware')} on {said.get('board')}")
    print(f"Name      {said.get('name')}")
    print(f"Panel     {said.get('panel')} ({said.get('panel_set') or 'asked at boot'}; it answered {said.get('panel_id')}), "
          f"invert {said.get('invert')}, bgr {said.get('bgr')}, flip {said.get('flip')}, mirror {said.get('mirror')}")
    print(f"Touch     chip {'answers' if said.get('touch') else 'SILENT'}, calibration "
          f"{'kept' if said.get('calibrated') else 'a GUESS: run touch cal'}")
    print(f"Screen    brightness {said.get('brightness')}%, font {said.get('font')} px, room light {said.get('light_raw')}")
    print(f"WiFi      {said.get('wifi_ssid') or '(not set)'}  address {said.get('ip') or '(not joined)'}")
    print(f"Sapphire  {said.get('sapphire') or '(not set)'}  certificate {'pinned' if said.get('cert') else 'NOT checked'}")
    print(f"Keys      {'set' if said.get('key') and said.get('voice_key') else 'NOT set'}")
    print(f"RAM       {said.get('free_kb')} KB free")
    print("Ready" if said.get('ready') else "Not set up yet. Run: setup")


def cmd_flash(port, skip_backup=False):
    args = BUILD / 'flasher_args.json'
    if args.exists():
        plan = json.loads(args.read_text())
        files = [x for offset, path in plan['flash_files'].items() for x in (offset, str(BUILD / path))]
        s = plan['flash_settings']
        mode, size, freq = s['flash_mode'], s['flash_size'], s['flash_freq']
    else:                                                    # no build here: the released image in dist/, whole, at 0
        images = sorted((ROOT / 'dist').glob('cyd28-*.bin'))
        if not images:
            sys.exit("No build and no dist/cyd28-*.bin. Run: bash build.sh cyd28 (in the firmware folder), or put a release in dist/")
        files, mode, size, freq = ['0x0', str(images[-1])], 'dio', '4MB', '40m'
        print(f"Writing the released image {images[-1].name}: the whole flash, so settings and the touch calibration start over (setup, then touch cal)")
    CACHE.mkdir(parents=True, exist_ok=True)
    if not skip_backup and not list(CACHE.glob('factory-cyd-*.bin')):
        backup = CACHE / time.strftime('factory-cyd-%Y%m%d-%H%M%S.bin')
        print(f"Saving what is on the board now to {backup.relative_to(ROOT)} (about a minute) ...")
        r = esptool('--port', port, '--chip', 'esp32', '--baud', '460800', '--after', 'hard-reset',
                    'read-flash', '0', hex(FLASH_BYTES), str(backup))
        if r.returncode != 0 or not backup.exists() or backup.stat().st_size != FLASH_BYTES:
            sys.exit("The backup failed, so nothing was written.")
    print("Writing the firmware ...")
    r = esptool('--port', port, '--chip', 'esp32', '--baud', '460800', '--after', 'hard-reset',
                'write-flash', '--flash-mode', mode, '--flash-size', size, '--flash-freq', freq, *files)
    if r.returncode != 0:
        sys.exit("Writing failed. Nothing to undo: run flash again.")
    print("\nDone. The board restarts by itself. Next: test, then setup")


def this_machine_ip():
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('192.0.2.1', 9))
            return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'


def fetch_cert(url):
    u = urlsplit(url)
    try:
        return ssl.get_server_certificate((u.hostname, u.port or 443), timeout=8)
    except (OSError, ssl.SSLError) as e:
        print(f"Could not fetch the certificate from {u.netloc}: {e}. Is Sapphire running?")
        return ''


def cmd_setup(port, new_keys=False):
    old = with_board(port, lambda b: b.ask('show')) or {}
    print("Setting up the pocket. The WiFi password and the keys go to the board only.\n")
    name = input(f"Device name [{old.get('name') or 'pocket'}]: ").strip().lower() or old.get('name') or 'pocket'
    ssid = input(f"WiFi name [{old.get('wifi_ssid') or ''}]: ").strip() or old.get('wifi_ssid') or ''
    if not ssid:
        sys.exit("A WiFi name is needed.")
    password = getpass.getpass("WiFi password (empty keeps the stored one): ")
    default_url = old.get('sapphire') or f"https://{this_machine_ip()}:8073"
    url = (input(f"Sapphire's address [{default_url}]: ").strip() or default_url).rstrip('/')
    cert = fetch_cert(url)
    fresh = new_keys or not (old.get('key') and old.get('voice_key'))
    cfg = {'name': name, 'wifi_ssid': ssid, 'sapphire': url, 'cert': cert}
    if password:
        cfg['wifi_password'] = password
    if fresh:
        cfg['key'] = secrets.token_urlsafe(24)
        cfg['voice_key'] = secrets.token_urlsafe(24)

    def send(b):
        said = b.ask('setup ' + json.dumps(cfg), wait=15)
        if not said or not said.get('ok'):
            sys.exit(f"The board did not take the settings: {said}")
    with_board(port, send)
    print("\nThe board took the settings. For Settings > Devices > + Add Device:")
    print("  Type                     Satellite")
    print(f"  Name                     {name}")
    if fresh:
        print(f"  Key Sapphire sends       {cfg['key']}")
        print(f"  Key the satellite sends  {cfg['voice_key']}")
        print("  (`keys` shows them again, `setup --new-keys` makes new ones)")
    else:
        print("  Keys                     unchanged. `keys` shows them")
    if not cert:
        print("\nThe board could not fetch Sapphire's certificate, so it will NOT check who it talks to. "
              "Run setup again with Sapphire running.")
    print("\nStarting it and waiting for WiFi ...")
    ip = with_board(port, lambda b: b.wait_for('POCKET IP', 45))
    if ip:
        print(f"It is on WiFi at {ip} (also {name}.local).")
        print(f"  Address                  http://{ip}")
    else:
        print("It did not join WiFi within 45 seconds. Check the WiFi name and password, then run setup again.")


def cmd_keys(port):
    said = with_board(port, lambda b: b.ask('keys'))
    if not said or not said.get('key'):
        sys.exit("The board has no keys yet. Run: setup")
    print("For Settings > Devices > + Add Device > Satellite:")
    print(f"  Key Sapphire sends       {said['key']}")
    print(f"  Key the satellite sends  {said['voice_key']}")


def cmd_test(port):
    print("The test card: a white frame, TOP at the top, bars RED GREEN BLUE WHITE, a done button below.")
    print("Tap the screen a few times: where you tapped prints here for 30 s. Then press done.\n")

    def run(b):
        said = b.ask('test', wait=5)
        if not said:
            sys.exit("The board did not answer.")
        b.wait_for('\x00never', 30, echo='TOUCH')
    with_board(port, run)
    print("\nIf the colors are wrong (red shows cyan):  python setup_board.py screen invert 0  (or 1)")
    print("If RED and BLUE trade places:                python setup_board.py screen bgr 1  (or 0)")
    print("If the picture is garbage or washed out:     python setup_board.py screen panel ili9341  (or st7789)")
    print("If TOP is at the bottom:                     python setup_board.py screen flip 1")
    print("If the text reads mirrored:                  python setup_board.py screen mirror 1")
    print("If taps land off the mark:                   python setup_board.py touch cal")


def cmd_screen(port, what, value):
    said = with_board(port, lambda b: b.ask(f'screen {what} {value}'))
    print(said if said else "The board did not answer.")


def cmd_touch(port, cal=False):
    if cal:
        print("Three crosses, one at a time: tap each where the lines cross. 60 s.\n")

        def run(b):
            said = b.ask('touch cal', wait=5)
            if not said or not said.get('ok'):
                sys.exit(f"The board did not start: {said}")
            for _ in range(4):
                line = b.wait_for('CAL ', 65)
                if line is None:
                    sys.exit("The board said nothing. Is it awake?")
                print('  ' + line)
                if line.startswith(('done', 'timeout')):
                    return line
        print("\nKept on the board." if (with_board(port, run) or '').startswith('done') else "\nNot calibrated.")
        return
    print("Press the glass: the chip's raw X, Y and pressure print for 10 s.\n")

    def raw(b):
        said = b.ask('touch', wait=5)
        if not said:
            sys.exit("The board did not answer.")
        b.wait_for('\x00never', 11, echo='RAW')
    with_board(port, raw)


def cmd_monitor(port):
    import serial
    print(f"Watching {port}. Ctrl-C ends it.")
    try:
        s = serial.Serial()
        s.port, s.baudrate, s.timeout = port, BAUD, 1
        s.dtr = s.rts = False
        s.open()
        with s:
            while True:
                raw = s.readline().decode('utf-8', 'replace').rstrip()
                if raw:
                    print(raw)
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=('status', 'flash', 'setup', 'keys', 'test', 'screen', 'touch', 'monitor'))
    ap.add_argument('rest', nargs='*', help='screen: invert|flip|mirror|bright|panel|font and a value; touch: cal')
    ap.add_argument('--port')
    ap.add_argument('--skip-backup', action='store_true', help='flash: do not save the old contents first')
    ap.add_argument('--new-keys', action='store_true', help='setup: make new keys')
    a = ap.parse_args()
    port = find_port(a.port)
    check_access(port)
    if a.what == 'screen':
        if len(a.rest) != 2:
            sys.exit("screen invert 0|1, flip 0|1, mirror 0|1, bgr 0|1, bright 10..100, font 14..24, panel auto|ili9341|st7789, home chat|backups|status|board|actions")
        return cmd_screen(port, a.rest[0], a.rest[1])
    if a.what == 'touch':
        return cmd_touch(port, cal=bool(a.rest and a.rest[0] == 'cal'))
    {'status': lambda: cmd_status(port), 'flash': lambda: cmd_flash(port, a.skip_backup),
     'setup': lambda: cmd_setup(port, a.new_keys), 'keys': lambda: cmd_keys(port), 'test': lambda: cmd_test(port),
     'monitor': lambda: cmd_monitor(port)}[a.what]()


if __name__ == '__main__':
    main()
