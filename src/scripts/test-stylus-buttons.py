#!/usr/bin/env python3
"""Compare Linux pen events with Qt Quick events, without grabbing any device.

Run ./src/scripts/test-stylus-buttons.py and test the pen in the window that opens.
The log distinguishes kernel BTN_STYLUS events from Qt button mappings.
Only pen/stylus devices are monitored at the kernel layer; ordinary keyboards
are excluded. Qt key events are logged only while the test window has focus.
"""

import argparse
import datetime
import fcntl
import os
from pathlib import Path
import re
import selectors
import shlex
import shutil
import struct
import subprocess
import sys
import time

from environment import development_root


ROOT = Path(__file__).resolve().parents[2]
EVENT = struct.Struct('@llHHi')  # Linux input_event, native timeval layout.
WORD_BITS = struct.calcsize('@L') * 8
EV_SYN, EV_KEY, EV_ABS, EV_MSC = 0, 1, 3, 4
ABS_PRESSURE = 0x18
ABS_INFO = struct.Struct('@iiiiii')
KEY_NAMES = {
    0x140: 'BTN_TOOL_PEN', 0x141: 'BTN_TOOL_RUBBER', 0x143: 'BTN_TOOL_PENCIL',
    0x149: 'BTN_STYLUS3', 0x14a: 'BTN_TOUCH', 0x14b: 'BTN_STYLUS', 0x14c: 'BTN_STYLUS2',
}
ABS_NAMES = {0: 'ABS_X', 1: 'ABS_Y', 0x18: 'ABS_PRESSURE', 0x19: 'ABS_DISTANCE',
             0x1a: 'ABS_TILT_X', 0x1b: 'ABS_TILT_Y'}


def capability_bits(path):
    try:
        words = path.read_text().split()
    except OSError:
        return set()
    return {index * WORD_BITS + bit for index, word in enumerate(reversed(words))
            for bit in range(WORD_BITS) if int(word, 16) & (1 << bit)}


def pen_devices():
    result = []
    for event in Path('/sys/class/input').glob('event*'):
        try:
            name = (event / 'device/name').read_text().strip()
        except OSError:
            continue
        keys = capability_bits(event / 'device/capabilities/key')
        pen_keys = KEY_NAMES.keys() - {0x14a}  # BTN_TOUCH alone also describes touchscreens.
        if not keys.intersection(pen_keys) and not re.search(r'\b(pen|stylus)\b', name, re.I):
            continue
        result.append((Path('/dev/input') / event.name, name, keys))
    return sorted(result, key=lambda device: int(device[0].name[5:]))


def build_probe():
    if not shutil.which('c++') or not shutil.which('pkg-config'):
        raise RuntimeError('The Qt probe needs c++ and pkg-config (the same tools used to build Pilon).')
    flags = subprocess.run(['pkg-config', '--cflags', '--libs', 'Qt6Quick', 'Qt6Gui', 'Qt6Core'],
                           text=True, capture_output=True)
    if flags.returncode:
        raise RuntimeError('Qt 6 development libraries are unavailable. Use --raw for kernel events.\n' + flags.stderr)
    output = development_root() / 'builds/work/stylus-diagnostics/stylus-button-probe'
    output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(['c++', '-std=c++23', '-O2', '-fPIC',
                    str(ROOT / 'src/tests/core/stylus_button_probe.cpp'), '-o', str(output),
                    *shlex.split(flags.stdout)], check=True)
    return output


class RawInputs:
    def __init__(self, selector, emit, verbose):
        self.selector, self.emit, self.verbose = selector, emit, verbose
        self.devices = {}
        self.denied = set()

    @staticmethod
    def refresh_state(state):
        # EVIOCGKEY and EVIOCGABS: start with the actual state even if the pen
        # was already touching the tablet when this monitor opened the device.
        key_bits = bytearray(96)  # Linux KEY_CNT is 768.
        try:
            fcntl.ioctl(state['fd'], (2 << 30) | (len(key_bits) << 16) | (ord('E') << 8) | 0x18, key_bits)
            state['keys'] = {bit for bit in range(len(key_bits) * 8) if key_bits[bit // 8] & (1 << (bit % 8))}
            state['tip'] = int(0x14a in state['keys'])
        except OSError:
            pass
        pressure = bytearray(ABS_INFO.size)
        try:
            fcntl.ioctl(state['fd'], (2 << 30) | (ABS_INFO.size << 16) | (ord('E') << 8) | (0x40 + ABS_PRESSURE), pressure)
            state['pressure'] = ABS_INFO.unpack(pressure)[0]
        except OSError:
            pass  # Pen Bluetooth keyboard devices have no pressure axis.

    def scan(self):
        found = {str(path): (path, name, keys) for path, name, keys in pen_devices()}
        for path in list(self.devices):
            if path not in found:
                self.close_device(path)
        for path, (_, name, keys) in found.items():
            if path in self.devices or path in self.denied:
                continue
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except OSError as exc:
                self.denied.add(path)
                self.emit(f'Linux cannot read {path} ({name}): {exc.strerror}')
                self.emit('For kernel events only, use a second terminal: sudo ./src/scripts/test-stylus-buttons.py --raw')
                continue
            state = {'fd': fd, 'path': path, 'name': name, 'buffer': b'', 'pressure': 0, 'tip': 0, 'keys': set(), 'dropped': False}
            self.refresh_state(state)
            self.devices[path] = state
            self.selector.register(fd, selectors.EVENT_READ, ('raw', state))
            buttons = ', '.join(KEY_NAMES[key] for key in sorted(keys) if key in KEY_NAMES)
            self.emit(f'Linux monitoring {path}: {name}; capabilities: {buttons or "keyboard keys"}')
            self.emit(f'Linux initial state {path}: tip={state["tip"]} pressure={state["pressure"]}')
        self.denied.intersection_update(found)

    def read(self, state):
        try:
            data = os.read(state['fd'], EVENT.size * 64)
        except BlockingIOError:
            return
        except OSError as exc:
            self.emit(f'Linux {state["path"]}: {exc.strerror}')
            self.close_device(state['path'])
            return
        if not data:
            self.close_device(state['path'])
            return
        state['buffer'] += data
        while len(state['buffer']) >= EVENT.size:
            seconds, micros, kind, code, value = EVENT.unpack_from(state['buffer'])
            state['buffer'] = state['buffer'][EVENT.size:]
            if state.get('dropped'):
                if kind == EV_SYN and code == 0:
                    self.refresh_state(state)
                    state['dropped'] = False
                    self.emit(f'Linux resynchronized {state["path"]}: tip={state["tip"]} pressure={state["pressure"]}')
                continue
            label = None
            if kind == EV_KEY:
                if value == 0:
                    state['keys'].discard(code)
                else:
                    state['keys'].add(code)
                if code == 0x14a:
                    state['tip'] = value
                action = {0: 'released', 1: 'pressed', 2: 'repeated'}.get(value, str(value))
                label = f'{KEY_NAMES.get(code, "KEY_" + str(code))} {action} (code={code}, value={value})'
            elif kind == EV_ABS and code == ABS_PRESSURE:
                previous = state['pressure']
                state['pressure'] = value
                if self.verbose or bool(previous) != bool(value):
                    label = f'ABS_PRESSURE value={value}'
            elif kind == EV_MSC and code == 4:
                label = f'MSC_SCAN value=0x{value:x}'
            elif kind == EV_SYN and code == 3:
                label = 'SYN_DROPPED (events lost; repeat the test)'
                state['dropped'] = True
            elif self.verbose and kind == EV_ABS:
                label = f'{ABS_NAMES.get(code, "ABS_" + str(code))} value={value}'
            if label:
                held = ','.join(KEY_NAMES.get(key, 'KEY_' + str(key)) for key in sorted(state['keys'])) or 'none'
                self.emit(f'Linux {state["path"]} ({state["name"]}) {label}; '
                          f'tip={state["tip"]} pressure={state["pressure"]} held={held} '
                          f'kernelTime={seconds}.{micros:06d}')

    def close_device(self, path):
        state = self.devices.pop(path)
        self.selector.unregister(state['fd'])
        os.close(state['fd'])
        self.emit(f'Linux stopped monitoring {path}')

    def close(self):
        for path in list(self.devices):
            self.close_device(path)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--raw', action='store_true', help='Monitor Linux pen devices only; do not open a Qt window.')
    parser.add_argument('--list', action='store_true', help='List detected pen devices and exit.')
    parser.add_argument('--build-only', action='store_true', help='Compile the Qt probe and exit.')
    parser.add_argument('--verbose', action='store_true', help='Also log continuous motion and pressure samples.')
    parser.add_argument('--duration', type=float, help='Stop after this many seconds (otherwise close the window or press Ctrl+C).')
    parser.add_argument('--log', type=Path, help='Log path (default: a timestamped file in the development build directory).')
    args = parser.parse_args()
    if args.duration is not None and args.duration <= 0:
        parser.error('--duration must be greater than zero')
    if args.list:
        for path, name, keys in pen_devices():
            labels = ', '.join(KEY_NAMES[key] for key in sorted(keys) if key in KEY_NAMES)
            print(f'{path}: {name}\n  {labels or "keyboard keys"}')
        return 0
    if os.geteuid() == 0 and not args.raw and not args.build_only:
        parser.error('Run the Qt window as your normal desktop user. Use sudo only with --raw.')
    probe = None if args.raw else build_probe()
    if args.build_only:
        print(probe)
        return 0
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    log_path = (args.log or development_root() / f'builds/work/stylus-buttons-{stamp}.log').resolve()
    log_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log_path.open('w', buffering=1) as log, selectors.DefaultSelector() as selector:
        def emit(line):
            message = f'{time.monotonic() - start:9.3f}s {line}'
            print(message, flush=True)
            log.write(message + '\n')

        emit(f'Stylus button diagnostic; log: {log_path}')
        emit('Session: ' + ' '.join(f'{name}={os.environ.get(name, "unset")}' for name in
                                  ('XDG_SESSION_TYPE', 'QT_QPA_PLATFORM', 'WAYLAND_DISPLAY', 'DISPLAY')))
        surface = 'the test window' if probe else 'your tablet or pen display'
        emit(f'Hover over {surface} and press/release each side button. Repeat with the tip down, then lift it.')
        emit('Try the top button too, if present. Close the window or press Ctrl+C to finish.' if probe
             else 'Try the top button too, if present. Press Ctrl+C to finish.')
        raw = RawInputs(selector, emit, args.verbose)
        process = None
        qt_buffer = b''
        try:
            raw.scan()
            if not raw.devices:
                emit('No readable pen devices currently detected. Waiting for device connection; Qt events still work if a window is open.')
            if probe:
                command = [str(probe)] + (['--verbose'] if args.verbose else [])
                process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                os.set_blocking(process.stdout.fileno(), False)
                selector.register(process.stdout, selectors.EVENT_READ, ('qt', None))
            next_scan = start + 1
            while args.duration is None or time.monotonic() - start < args.duration:
                for key, _ in selector.select(timeout=0.2):
                    kind, state = key.data
                    if kind == 'raw':
                        raw.read(state)
                    else:
                        chunk = os.read(key.fd, 65536)
                        if not chunk:
                            selector.unregister(key.fileobj)
                        qt_buffer += chunk
                        while b'\n' in qt_buffer:
                            line, qt_buffer = qt_buffer.split(b'\n', 1)
                            emit(line.decode(errors='replace'))
                if process and process.poll() is not None:
                    break
                if time.monotonic() >= next_scan:
                    raw.scan()
                    next_scan = time.monotonic() + 1
        except KeyboardInterrupt:
            emit('Test stopped with Ctrl+C.')
        finally:
            raw.close()
            if process:
                if process.poll() is None:
                    process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
                for line in process.stdout.read().decode(errors='replace').splitlines():
                    emit(line)
                process.stdout.close()
            if qt_buffer:
                emit(qt_buffer.decode(errors='replace'))
        if process and process.returncode not in (0, -15, -2):
            emit(f'Qt probe exited with code {process.returncode}.')
            return 1
        emit(f'Finished. Share this log: {log_path}')
    return 0


if __name__ == '__main__':
    from environment import load_environment
    load_environment()
    try:
        sys.exit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f'Stylus test failed: {exc}', file=sys.stderr)
        sys.exit(1)
