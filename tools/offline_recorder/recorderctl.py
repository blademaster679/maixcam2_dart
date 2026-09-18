#!/usr/bin/env python3
"""Screenless recorder control through a transient systemd service."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import sys
import time

APP = Path(__file__).resolve().parent
UNIT = 'dart-data-recorder.service'


def command(*args, **kwargs):
    return subprocess.run(args, **kwargs)


def active(unit):
    return command('systemctl', 'is-active', '--quiet', unit).returncode == 0


def core_pids(parent):
    """Only signal this supervisor's recorder descendants, never other captures."""
    processes = {}
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            processes[int(entry.name)] = (int(fields[1]), (entry / 'comm').read_text().strip())
        except (OSError, ValueError, IndexError):
            continue
    descendants = {parent}
    while True:
        found = {pid for pid, (ppid, _) in processes.items() if ppid in descendants}
        if found <= descendants:
            break
        descendants |= found
    return [pid for pid in descendants if processes.get(pid, (0, ''))[1] == 'direct_record']


def supervise(options):
    stopping = False

    def request_stop(*_):
        nonlocal stopping
        stopping = True

    signal.signal(signal.SIGUSR1, request_stop)
    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    # Refuse to displace an active camera or other app.
    for entry in Path('/proc').iterdir():
        if not entry.name.isdigit():
            continue
        try:
            exe = os.readlink(entry / 'exe')
        except OSError:
            continue
        if exe.startswith('/maixapp/apps/') and not exe.startswith('/maixapp/apps/launcher/'):
            raise RuntimeError('Another app is active: ' + exe)
    restore = active('launcher.service')
    child = None
    try:
        if restore:
            command('systemctl', 'stop', 'launcher.service', check=True)
        if stopping:
            return 0
        child = subprocess.Popen(['/bin/sh', str(APP / 'main.sh'), '--headless', *options])
        signalled = set()
        while child.poll() is None:
            if stopping:
                for pid in core_pids(child.pid):
                    if pid not in signalled:
                        try:
                            os.kill(pid, signal.SIGINT)
                            signalled.add(pid)
                        except ProcessLookupError:
                            pass
            time.sleep(0.2)
        return child.returncode
    finally:
        if restore:
            command('systemctl', 'start', 'launcher.service', check=True)


def parser():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest='action', required=True)
    for action in ('start', '_run'):
        start = sub.add_parser(action)
        start.add_argument('--seconds', type=int, default=600)
        start.add_argument('--exposure-us', type=int, default=500)
        start.add_argument('--gain', type=float, default=1.0, help='Analog gain multiplier, 1..16')
        start.add_argument('--label', default='unlabeled')
    sub.add_parser('status')
    sub.add_parser('stop')
    return p


def validate(a):
    if not 1 <= a.seconds <= 600 or not 1 <= a.exposure_us <= 5554 or not 1 <= a.gain <= 16:
        raise ValueError('seconds: 1..600; exposure-us: 1..5554; gain: 1..16')
    if not a.label or any(c not in 'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-' for c in a.label):
        raise ValueError('label must contain only ASCII letters, digits, _ or -')


def main():
    a = parser().parse_args()
    if a.action == 'status':
        command('systemctl', 'show', UNIT, '--property=ActiveState,SubState,Result,ExecMainStatus')
        command('journalctl', '-u', UNIT, '-n', '15', '--no-pager')
        # The startup journal reports the exact directory even with a custom data_root.
        return 0
    if a.action == 'stop':
        return command('systemctl', 'kill', '--kill-who=main', '--signal=SIGUSR1', UNIT).returncode
    validate(a)
    if a.action == '_run':
        return supervise(['--seconds', str(a.seconds), '--exposure-us', str(a.exposure_us),
                          '--gain', str(round(a.gain * 1024)), '--label', a.label])
    if active(UNIT):
        raise RuntimeError('Recorder service is already active')
    command('systemctl', 'reset-failed', UNIT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return command('systemd-run', '--unit=' + UNIT, '--collect', '--property=Type=exec',
                   '--property=KillMode=mixed', '--property=TimeoutStopSec=60',
                   sys.executable, str(APP / 'recorderctl.py'), '_run',
                   '--seconds', str(a.seconds), '--exposure-us', str(a.exposure_us),
                   '--gain', str(a.gain), '--label', a.label).returncode


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(str(exc), file=sys.stderr)
        sys.exit(1)
