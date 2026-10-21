#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Exercise the real Qt frontend, DS core and all three renderers without hardware.

Requires Xvfb, clang, llvm-objcopy, Mesa software GL, and barista-qt-driver.
All settings, sockets, ROMs and screenshots stay in the artifact directory.
"""
import argparse
import json
import os
from pathlib import Path
import select
import shutil
import socket
import struct
import subprocess
import tempfile
import time


def build_rom(directory):
    source = Path(__file__).resolve().parent
    code = []
    for cpu, name in (("arm946e-s", "arm9"), ("arm7tdmi", "arm7")):
        obj, binary = directory / (name + ".o"), directory / (name + ".bin")
        subprocess.run(["clang", "--target=arm-none-eabi", "-mcpu=" + cpu,
                        "-c", str(source / (name + ".s")), "-o", str(obj)], check=True)
        subprocess.run(["llvm-objcopy", "-O", "binary", str(obj), str(binary)], check=True)
        code.append(binary.read_bytes())
    # Homebrew direct boot: original ARM code and a minimal header.
    rom = bytearray(0x20000)
    rom[:12] = b"BARISTATEST\0"
    rom[12:16] = b"####"
    for offset, address, header, program in ((0x200, 0x02000000, 0x20, code[0]),
                                              (0x400, 0x03800000, 0x30, code[1])):
        struct.pack_into("<4I", rom, header, offset, address, address, len(program))
        rom[offset:offset + len(program)] = program
    struct.pack_into("<II", rom, 0x80, len(rom), 0x200)
    path = directory / "barista-test.nds"
    path.write_bytes(rom)
    return path


class Peer:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.socket.bind(str(path))
        self.socket.listen()
        self.socket.settimeout(8)
        self.peer = None

    def accept(self):
        if self.peer:
            self.peer.close()
        self.peer, _ = self.socket.accept()
        self.peer.settimeout(5)

    def send_input(self, buttons=0, touch=None):
        report = bytearray(128)
        report[7] = report[9] = 8  # Center the sticks.
        struct.pack_into(">H", report, 2, buttons)
        if touch:
            x, y = touch
            raw_x = int((x * 854 / 864 + 23.1097) / .2210755)
            raw_y = int((507.639 - y) / .12772)
            for i in range(10):
                struct.pack_into("<HH", report, 36 + 4 * i, raw_x, raw_y)
            report[37] |= 16
        self.peer.send(struct.pack("<4I", 0x3147554d, 5, 0, 0) + report)

    def until(self, predicate, timeout=8):
        deadline = time.monotonic() + timeout
        frames, counts = {}, {}
        pcm, heartbeat = 0, None
        while time.monotonic() < deadline:
            packet = self.peer.recv(16400)
            if not packet:
                self.accept()
                continue
            magic, kind, frame_id, offset = struct.unpack_from("<4I", packet)
            assert magic == 0x3147554d, "MUG1 magic"
            data = packet[16:]
            counts[kind] = counts.get(kind, 0) + 1
            if kind == 3:
                heartbeat = data[0]
            if kind == 4:
                assert len(data) % 4 == 0, "Whole stereo audio frames"
                pcm += len(data)
            if kind in (1, 2):
                if offset == 0:
                    frames[(kind, frame_id)] = bytearray()
                frame = frames.get((kind, frame_id))
                if frame is None:
                    continue
                assert len(frame) == offset, "Contiguous video chunks"
                frame.extend(data)
                if len(frame) == 622080:
                    if predicate(kind, frame, heartbeat, pcm):
                        return frame
                    frames.pop((kind, frame_id))
            elif predicate(kind, data, heartbeat, pcm):
                return data
        raise AssertionError(f"Stream state not observed: {counts}, active={heartbeat}")

    def close(self):
        if self.peer:
            self.peer.close()
        self.socket.close()


def command(directory, **kwargs):
    result = directory / "result.json"
    result.unlink(missing_ok=True)
    temporary = directory / "command.tmp"
    temporary.write_text(json.dumps(kwargs))
    temporary.rename(directory / "command.json")
    deadline = time.monotonic() + 5
    while not result.exists():
        assert time.monotonic() < deadline, "UI command timeout"
        time.sleep(.025)
    assert not json.loads(result.read_text())["error"], result.read_text()
    if "screenshot" in kwargs:
        while not Path(kwargs["screenshot"]).exists():
            assert time.monotonic() < deadline, "Settings screenshot timeout"
            time.sleep(.025)


def run_renderer(args, root, display, rom, renderer):
    directory = root / str(renderer)
    directory.mkdir()
    config = directory / "config" / "melonDS"
    config.mkdir(parents=True)
    socket_path = directory / "media.sock"
    server = Peer(socket_path)
    (config / "melonDS.toml").write_text(
        f'[Barista]\nEnabled = true\nInput = true\nScreen = 0\nSocketPath = "{socket_path}"\n'
        f'["3D"]\nRenderer = {renderer}\n["3D".GL]\nScaleFactor = 2\n')
    env = os.environ | {
        "DISPLAY": display, "QT_QPA_PLATFORM": "xcb", "SDL_AUDIODRIVER": "dummy",
        "XDG_CONFIG_HOME": str(directory / "config"), "LD_PRELOAD": str(args.qt_driver),
        "MELONDS_TEST_DIR": str(directory), "LIBGL_ALWAYS_SOFTWARE": "1",
    }
    with (directory / "emulator.log").open("w") as log:
        process = subprocess.Popen([str(args.emulator), str(rom)], env=env, stdout=log, stderr=log)
        try:
            server.accept()
            # The fixture displays green on the bottom. A changes it to blue;
            # ARM7 observes pen contact and changes it to yellow.
            server.until(lambda k, f, a, p: k == 1 and 100 < f[240 * 864 + 432] < 170)
            server.send_input(0x8000)
            server.until(lambda k, f, a, p: k == 1 and f[240 * 864 + 432] < 60)
            server.send_input(touch=(432, 240))
            server.until(lambda k, f, a, p: k == 1 and f[240 * 864 + 432] > 180)
            server.until(lambda k, f, a, p: k == 1 and 100 < f[240 * 864 + 432] < 170)
            print(f"Renderer {renderer}: video, buttons, touch and stale release passed", flush=True)

            command(directory, action="settings", mode=1, apply=True)
            server.accept()
            # Top screen has red above blue, detecting swapped channels or rows.
            server.until(lambda k, f, a, p: k == 1 and 65 < f[100 * 864 + 432] < 100
                         and f[380 * 864 + 432] < 60)
            command(directory, action="settings", mode=2, apply=True)
            server.accept()
            server.until(lambda k, f, a, p: k == 1 and 65 < f[120 * 864 + 216] < 100
                         and f[360 * 864 + 216] < 60 and 100 < f[240 * 864 + 648] < 170)
            server.send_input(touch=(648, 240))
            server.until(lambda k, f, a, p: k == 1 and f[240 * 864 + 648] > 180)

            command(directory, action="settings", mode=3, apply=True,
                    screenshot=str(directory / "settings.png"))
            server.accept()
            server.until(lambda k, f, a, p: k == 1 and 65 < f[60 * 864 + 432] < 100
                         and f[180 * 864 + 432] < 60 and 100 < f[360 * 864 + 432] < 170
                         and f[0] == 16 and p > 0)
            server.send_input(touch=(432, 360))
            server.until(lambda k, f, a, p: k == 1 and f[360 * 864 + 432] > 180)
            server.send_input(touch=(432, 60))
            server.until(lambda k, f, a, p: k == 1 and 100 < f[360 * 864 + 432] < 170)
            print(f"Renderer {renderer}: four layouts, orientation, touch mapping and PCM passed", flush=True)

            command(directory, action="settings", input=False, apply=True)
            server.accept()
            server.send_input(buttons=0x8000, touch=(432, 360))
            # Observe several frames while the report is fresh. Accepting input
            # would turn these yellow/blue, before the 500 ms expiry.
            for _ in range(5):
                frame = server.until(lambda k, f, a, p: k == 1)
                assert 100 < frame[360 * 864 + 432] < 170, "Input toggle ignored"
            command(directory, action="settings", input=True, apply=True, close=True)
            server.accept()
            command(directory, action="pause")
            server.until(lambda k, f, a, p: k == 3 and a == 0)
            command(directory, action="pause")
            server.until(lambda k, f, a, p: k == 3 and a == 1)
            command(directory, action="stop")
            server.until(lambda k, f, a, p: k == 3 and a == 0)
            command(directory, action="quit")
            assert process.wait(timeout=8) == 0, "Clean emulator exit"
            saved = (config / "melonDS.toml").read_text()
            assert "Screen = 3" in saved and "Enabled = true" in saved and "Input = true" in saved
            print(f"Renderer {renderer}: input toggle, lifecycle and persisted settings passed", flush=True)
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
            server.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("emulator", type=Path)
    parser.add_argument("--qt-driver", type=Path, required=True)
    parser.add_argument("--xvfb", default=shutil.which("Xvfb"))
    parser.add_argument("--artifacts", type=Path)
    args = parser.parse_args()
    args.emulator = args.emulator.resolve()
    args.qt_driver = args.qt_driver.resolve()
    assert args.xvfb and args.emulator.is_file() and args.qt_driver.is_file(), "Missing test tools/binaries"
    root = args.artifacts.resolve() if args.artifacts else Path(tempfile.mkdtemp(prefix="melonds-smoke-"))
    root.mkdir(parents=True, exist_ok=True)
    print("Artifacts:", root, flush=True)
    rom = build_rom(root)
    read_fd, write_fd = os.pipe()
    with (root / "xvfb.log").open("w") as log:
        xvfb = subprocess.Popen([args.xvfb, "-displayfd", str(write_fd), "-screen", "0", "1280x900x24",
                                 "-nolisten", "tcp", "-ac"], pass_fds=(write_fd,), stdout=log, stderr=log)
        os.close(write_fd)
        try:
            assert select.select([read_fd], [], [], 10)[0], "Xvfb startup timeout"
            display = ":" + os.read(read_fd, 32).decode().strip()
            os.close(read_fd)
            for renderer in (0, 1, 2):
                run_renderer(args, root, display, rom, renderer)
        finally:
            xvfb.terminate()
            xvfb.wait(timeout=3)


if __name__ == "__main__":
    main()
