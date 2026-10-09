#!/usr/bin/env python3
"""Set up a Sapphire satellite board over USB.

    python setup_board.py status     what is on the port, and what the board says about itself
    python setup_board.py flash      write the firmware from build/ (saves the old contents first)
    python setup_board.py setup      WiFi, name, keys and Sapphire's address: run this in your own terminal
    python setup_board.py keys       the two keys again, for Settings > Devices
    python setup_board.py test       a tone through the speaker, heard by its own mic
    python setup_board.py monitor    watch the board's log

Run it in an environment that has esptool and pyserial. Build
the firmware first, in the firmware folder: bash build.sh <this folder's name>

setup asks for the WiFi password without showing it. The password and the two
keys are written to the board only. The keys are printed once, for
Settings > Devices.
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
ESPRESSIF = 0x303A
FLASH_BYTES = 0x1000000                                  # 16 MB
BAUD = 115200

NO_ACCESS = """
This user may not use {port}. Run these once, then unplug and replug the board:

  echo 'SUBSYSTEM=="tty", ATTRS{{idVendor}}=="303a", TAG+="uaccess", ENV{{ID_MM_DEVICE_IGNORE}}="1"' | sudo tee /etc/udev/rules.d/70-espressif.rules
  sudo udevadm control --reload
"""


def find_port(wanted=None):
    if wanted:
        return wanted
    from serial.tools import list_ports
    found = [p.device for p in list_ports.comports() if p.vid == ESPRESSIF]
    if not found:
        sys.exit("No Espressif board found on USB. Is it plugged in?")
    if len(found) > 1:
        sys.exit(f"More than one board found: {', '.join(found)}. Pick one with --port.")
    return found[0]


def check_access(port):
    if os.name != 'nt' and not os.access(port, os.R_OK | os.W_OK):
        sys.exit(NO_ACCESS.format(port=port))


def esptool(*args, timeout=900):
    return subprocess.run([sys.executable, '-m', 'esptool', *args], text=True, timeout=timeout)


# --- talking to the board over its USB line -------------------------------------------

class Board:
    """One line in, one ">> " line back. Log lines in between are shown when
    `loud` is set. Never lower DTR and RTS together: that resets the board."""

    def __init__(self, port, loud=False):
        import serial
        self.s = serial.Serial(port, BAUD, timeout=1)
        self.loud = loud

    def ask(self, line, wait=10, tries=3):
        """The board's answer as a dict, or None. A log line from another task
        can land inside an answer; such an answer is asked for again."""
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
                        break                              # cut by a log line: once more
                elif raw and self.loud:
                    print('   ' + raw)
        return None

    def wait_for(self, prefix, seconds):
        end = time.time() + seconds
        while time.time() < end:
            raw = self.s.readline().decode('utf-8', 'replace').strip()
            if raw.startswith(prefix):
                return raw[len(prefix):].strip()
        return None

    def close(self):
        self.s.close()


def with_board(port, fn, loud=False, tries=20):
    """The port comes and goes around a restart: try for a while."""
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
    print(f"Firmware  {said.get('firmware')}")
    print(f"Name      {said.get('name')}")
    print(f"WiFi      {said.get('wifi_ssid') or '(not set)'}  address {said.get('ip') or '(not joined)'}")
    print(f"Sapphire  {said.get('sapphire') or '(not set)'}  certificate {'pinned' if said.get('cert') else 'NOT checked'}")
    print(f"Keys      {'set' if said.get('key') and said.get('voice_key') else 'NOT set'}")
    print("Ready" if said.get('ready') else "Not set up yet. Run: setup")


def cmd_flash(port, skip_backup=False):
    args = BUILD / 'flasher_args.json'
    if not args.exists():
        sys.exit("No build found. Run: bash build.sh ai-speaker-mic (in the firmware folder)")
    plan = json.loads(args.read_text())
    CACHE.mkdir(parents=True, exist_ok=True)
    if not skip_backup and not list(CACHE.glob('factory-waveshare-audio-*.bin')):
        backup = CACHE / time.strftime('factory-waveshare-audio-%Y%m%d-%H%M%S.bin')
        print(f"Saving what is on the board now to {backup.relative_to(ROOT)} (about two minutes) ...")
        r = esptool('--port', port, '--chip', 'esp32s3', '--baud', '460800', '--after', 'watchdog-reset',
                    'read-flash', '0', hex(FLASH_BYTES), str(backup))
        if r.returncode != 0 or not backup.exists() or backup.stat().st_size != FLASH_BYTES:
            sys.exit("The backup failed, so nothing was written.")
    files = []
    for offset, path in plan['flash_files'].items():
        files += [offset, str(BUILD / path)]
    s = plan['flash_settings']
    print("Writing the firmware ...")
    r = esptool('--port', port, '--chip', 'esp32s3', '--baud', '460800', '--after', 'watchdog-reset',
                'write-flash', '--flash-mode', s['flash_mode'], '--flash-size', s['flash_size'],
                '--flash-freq', s['flash_freq'], *files)
    if r.returncode != 0:
        sys.exit("Writing failed. Nothing to undo: run flash again.")
    print("\nDone. The board restarts by itself. Next: setup")


def this_machine_ip():
    """The address this machine has on the home network."""
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
            s.connect(('192.0.2.1', 9))               # no packet is sent
            return s.getsockname()[0]
    except OSError:
        return '127.0.0.1'


def fetch_cert(url):
    """Sapphire's certificate, PEM, so the board trusts that one and no other."""
    u = urlsplit(url)
    try:
        return ssl.get_server_certificate((u.hostname, u.port or 443), timeout=8)
    except (OSError, ssl.SSLError) as e:
        print(f"Could not fetch the certificate from {u.netloc}: {e}. Is Sapphire running?")
        return ''


def cmd_setup(port, new_keys=False):
    old = with_board(port, lambda b: b.ask('show')) or {}
    print("Setting up the satellite. The WiFi password and the keys go to the board only.\n")
    name = input(f"Device name [{old.get('name') or 'satellite'}]: ").strip().lower() or old.get('name') or 'satellite'
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
    # the keys first, the moment the board has them: a Ctrl-C during the WiFi
    # wait once lost them (2026-09-29)
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
    ip = with_board(port, lambda b: b.wait_for('SATELLITE IP', 45))
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
    said = with_board(port, lambda b: b.ask('selftest', wait=20), loud=True)
    if not said:
        sys.exit("The board did not answer.")
    print(f"Codec        {'ok' if said.get('codec_ok') else 'NOT working'}")
    print(f"Mic, quiet   {said.get('quiet_rms')}")
    print(f"Mic, tone    {said.get('tone_rms')}")
    print(f"Slots, quiet {said.get('quiet_slots')}   (a mic hears the room; the speaker's own loopback reads about 0)")
    print(f"Slots, tone  {said.get('slots')}")
    print("The mic hears the speaker." if said.get('mic_hears_speaker')
          else "The mic did NOT hear the tone. Is the speaker plugged in? Was the room loud?")


def cmd_monitor(port):
    import serial
    print(f"Watching {port}. Ctrl-C ends it.")
    try:
        with serial.Serial(port, BAUD, timeout=1) as s:
            while True:
                raw = s.readline().decode('utf-8', 'replace').rstrip()
                if raw:
                    print(raw)
    except KeyboardInterrupt:
        pass


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('what', choices=('status', 'flash', 'setup', 'keys', 'test', 'monitor'))
    ap.add_argument('--port')
    ap.add_argument('--skip-backup', action='store_true', help='flash: do not save the old contents first')
    ap.add_argument('--new-keys', action='store_true', help='setup: make new keys')
    a = ap.parse_args()
    port = find_port(a.port)
    check_access(port)
    {'status': lambda: cmd_status(port), 'flash': lambda: cmd_flash(port, a.skip_backup),
     'setup': lambda: cmd_setup(port, a.new_keys), 'keys': lambda: cmd_keys(port), 'test': lambda: cmd_test(port),
     'monitor': lambda: cmd_monitor(port)}[a.what]()


if __name__ == '__main__':
    main()
