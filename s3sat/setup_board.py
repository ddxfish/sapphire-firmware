#!/usr/bin/env python3
"""Set up a Sapphire S3 satellite over USB: the pendant (Elecrow AI Camera board) or
the AI speaker (Waveshare ESP32-S3-AUDIO-Board). Which one is told by its USB chip
(the pendant's CH340, the speaker's own Espressif USB); --board says it outright.

    python setup_board.py status     what is on the port, and what the board says about itself
    python setup_board.py flash      write the firmware from build/ (saves the old contents first)
    python setup_board.py setup      WiFi, name, keys and Sapphire's address: run this in your own terminal
    python setup_board.py keys       the two keys again, for Settings > Devices
    python setup_board.py test       a tone through the speaker, heard by its own mic
    python setup_board.py aec        the pendant's echo reference: a burst is played and timed on the way back
    python setup_board.py flash --board ai-speaker-mic     when both boards are plugged in
    python setup_board.py monitor    watch the board's log

Run it in an environment that has esptool and pyserial. Build
the firmware first, in the firmware folder: bash build.sh pendant | ai-speaker-mic

setup asks for the WiFi password without showing it. The password and the two
keys are written to the board only. The keys are printed once, for
Settings > Devices.

The pendant's USB chip is a CH340 (vendor 1a86, /dev/ttyUSB0): each port open resets
it. The speaker's is the S3's own (vendor 303a, /dev/ttyACM0): never lower DTR and RTS
together there, that resets it. Only those two are looked for.
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
CACHE = ROOT / 'backups'
FLASH_BYTES = 0x1000000                                  # 16 MB
BAUD = 115200
# what differs between the two boards on the USB side
BOARDS = {
    'pendant': {'vid': 0x1A86, 'chip': 'CH340', 'rule': '71-ch340.rules', 'after': 'hard-reset', 'reset_on_open': True,
                'factory': 'factory-elecrow-ai-camera', 'name': 'pendant'},
    'ai-speaker-mic': {'vid': 0x303A, 'chip': 'Espressif', 'rule': '70-espressif.rules', 'after': 'watchdog-reset', 'reset_on_open': False,
                       'factory': 'factory-waveshare-audio', 'name': 'satellite'},
}
BOARD = None                                             # main() decides: --board, else the USB chip found

NO_ACCESS = """
This user may not use {port}. Run this once, then unplug and replug the board:

  echo 'SUBSYSTEM=="tty", ATTRS{{idVendor}}=="{vid:04x}", TAG+="uaccess", ENV{{ID_MM_DEVICE_IGNORE}}="1"' | sudo tee /etc/udev/rules.d/{rule}
  sudo udevadm control --reload && sudo udevadm trigger
"""


def find_port(wanted=None, board=None):
    """(port, board). With --port the board must be told or is guessed from the port's chip."""
    from serial.tools import list_ports
    vids = {b['vid']: name for name, b in BOARDS.items()}
    ports = list(list_ports.comports())
    if wanted:
        vid = next((p.vid for p in ports if p.device == wanted), None)
        return wanted, board or vids.get(vid) or sys.exit(f"Say which board is on {wanted}: --board pendant | ai-speaker-mic")
    found = [(p.device, vids[p.vid]) for p in ports if p.vid in vids and (not board or vids[p.vid] == board)]
    if not found:
        sys.exit(f"No {board or 'pendant or AI speaker'} found on USB. Is it plugged in with a DATA cable?")
    if len(found) > 1:
        sys.exit(f"More than one board found: {', '.join(f'{d} ({b})' for d, b in found)}. Pick one with --port or --board.")
    return found[0]


def check_access(port):
    if os.name != 'nt' and not os.access(port, os.R_OK | os.W_OK):
        b = BOARDS[BOARD]
        sys.exit(NO_ACCESS.format(port=port, vid=b['vid'], rule=b['rule']))


def esptool(*args, timeout=900):
    return subprocess.run([sys.executable, '-m', 'esptool', *args], text=True, timeout=timeout)


# --- talking to the board over its USB line -------------------------------------------

class Board:
    """One line in, one ">> " line back. Log lines in between are shown when
    `loud` is set. On the pendant opening the port resets the board (the CH340 asserts
    DTR and RTS before anyone can hold them), so each open waits for the boot; on the
    speaker the port opens as it is (never lower DTR and RTS together: a reset)."""

    def __init__(self, port, loud=False):
        import serial
        if BOARDS[BOARD]['reset_on_open']:
            self.s = serial.Serial()
            self.s.port, self.s.baudrate, self.s.timeout = port, BAUD, 1
            self.s.dtr = self.s.rts = False
            self.s.open()
            time.sleep(2.5)
            self.s.reset_input_buffer()
        else:
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
    b = BOARDS[BOARD]
    build = HERE / 'build' / BOARD
    args = build / 'flasher_args.json'
    if not args.exists():
        sys.exit(f"No build found. Run: bash build.sh {BOARD} (in the firmware folder)")
    plan = json.loads(args.read_text())
    CACHE.mkdir(parents=True, exist_ok=True)
    if not skip_backup and not list(CACHE.glob(f"{b['factory']}-*.bin")):
        backup = CACHE / time.strftime(f"{b['factory']}-%Y%m%d-%H%M%S.bin")
        print(f"Saving what is on the board now to {backup.relative_to(ROOT)} (about two minutes) ...")
        r = esptool('--port', port, '--chip', 'esp32s3', '--baud', '460800', '--after', b['after'],
                    'read-flash', '0', hex(FLASH_BYTES), str(backup))
        if r.returncode != 0 or not backup.exists() or backup.stat().st_size != FLASH_BYTES:
            sys.exit("The backup failed, so nothing was written.")
    files = []
    for offset, path in plan['flash_files'].items():
        files += [offset, str(build / path)]
    s = plan['flash_settings']
    print(f"Writing the {BOARD} firmware ...")
    r = esptool('--port', port, '--chip', 'esp32s3', '--baud', '460800', '--after', b['after'],
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
    print(f"Setting up the {BOARD}. The WiFi password and the keys go to the board only.\n")
    dflt = BOARDS[BOARD]['name']
    name = input(f"Device name [{old.get('name') or dflt}]: ").strip().lower() or old.get('name') or dflt
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


def cmd_aec(port):
    """The echo reference measured on the board: a burst out of the speaker, timed on the mic."""
    said = with_board(port, lambda b: b.ask('aec measure', wait=15))
    if not said:
        sys.exit("The board did not answer.")
    if 'error' in said:
        sys.exit(said['error'])
    print(f"Burst heard: {'yes' if said.get('heard') else 'NO'}  ({said.get('ratio')}x the room)")
    if said.get('heard'):
        print(f"Came back after {said['heard_after_ms']} ms; the reference is read {said['delay_set_ms']} ms after it is played.")
        print(f"Suggested: aec delay {said['suggest_ms']}  (a little ahead of the echo; the filter spans the rest)")


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
    ap.add_argument('what', choices=('status', 'flash', 'setup', 'keys', 'test', 'aec', 'monitor'))
    ap.add_argument('--port')
    ap.add_argument('--board', choices=tuple(BOARDS), help='which board (else told by its USB chip)')
    ap.add_argument('--skip-backup', action='store_true', help='flash: do not save the old contents first')
    ap.add_argument('--new-keys', action='store_true', help='setup: make new keys')
    a = ap.parse_args()
    global BOARD
    port, BOARD = find_port(a.port, a.board)
    print(f"{BOARD} on {port}")
    if a.what == 'flash' and not a.board:          # told by the USB chip alone: a Cardputer or a T-Dongle is Espressif too
        if input(f"Write the {BOARD} program to the board on {port}? [y/N] ").strip().lower() != 'y':
            sys.exit("Nothing written. Say which board with --board pendant | ai-speaker-mic.")
    check_access(port)
    {'status': lambda: cmd_status(port), 'flash': lambda: cmd_flash(port, a.skip_backup),
     'setup': lambda: cmd_setup(port, a.new_keys), 'keys': lambda: cmd_keys(port), 'test': lambda: cmd_test(port),
     'aec': lambda: cmd_aec(port), 'monitor': lambda: cmd_monitor(port)}[a.what]()


if __name__ == '__main__':
    main()
