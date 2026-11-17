#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""One-command per-user install/uninstall, prompting for native root actions."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
HELPER = HERE / 'desktop/essx-volume'


def run(args, **kwargs):
    subprocess.run([str(a) for a in args], check=True, **kwargs)


def helper(action, *args):
    run([sys.executable, HELPER, action, *args])


def driver_active():
    result = subprocess.run(['sysctl', '-n', 'dev.essxsof.0.pcm_enable'],
                            text=True, capture_output=True)
    return result.returncode == 0 and result.stdout.strip() == '1'


def pause_existing():
    # The helper handles both the previous bridge and the current direct path.
    helper('stop')
    if driver_active():
        run([sys.executable, HERE / 'desktop/session.py', 'pause'])


def native(action):
    command = shlex.join(['/bin/sh', str(HERE / 'native-action.sh'), action])
    # FreeBSD su: arguments AFTER the login name are passed to root's shell.
    run(['su', 'root', '-c', command])


def refresh_pulse():
    subprocess.run(['pulseaudio', '--kill'], stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, check=False)
    run(['pulseaudio', '--start'])


def install():
    # Never stop working audio for a build that cannot complete.
    run(['/bin/sh', HERE / 'build.sh'])
    run(['pactl', 'info'], stdout=subprocess.DEVNULL)
    pause_existing()
    try:
        native('install')
    except subprocess.CalledProcessError:
        if driver_active():
            try:
                refresh_pulse()
                helper('install')
                helper('start', '--wait')
            except subprocess.CalledProcessError:
                print('Audio recovery did not complete; keep the error output.', file=sys.stderr)
        raise
    refresh_pulse()
    helper('install')
    helper('start', '--wait')
    print('Installed. Driver starts at boot; desktop audio starts at login.')
    print('Startup volume: 100%. Manual range: 0–200%.')


def uninstall():
    # Stop PulseAudio users of the device before native detach is requested.
    pause_existing()
    native('uninstall')
    helper('remove')
    refresh_pulse()
    print('Uninstalled. Driver boot start and desktop autostart are disabled.')


def main():
    if sys.argv[1:] not in (['install'], ['uninstall']):
        sys.exit('Use sh install.sh or sh uninstall.sh.')
    if os.geteuid() == 0:
        sys.exit('Run from your ordinary desktop account. Root is requested automatically.')
    if not sys.platform.startswith('freebsd'):
        sys.exit('FreeBSD amd64 is required.')
    required = ('su', 'sysctl', 'pactl', 'pulseaudio')
    for name in required:
        if not shutil.which(name):
            sys.exit(name + ' is required; see README.md.')
    if sys.argv[1] == 'install':
        install()
    else:
        uninstall()


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit('Setup stopped: ' + str(exc))
