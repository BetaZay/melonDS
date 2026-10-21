# Barista GamePad integration

This fork connects melonDS to [Barista](https://github.com/BetaZay/Barista)
using the Linux-local AppHook/MUG1 API, following the
[Azahar](https://github.com/BetaZay/azahar) and
[Cemu](https://github.com/BetaZay/Cemu/tree/barista-connector-prototype) integrations.
It adds no runtime dependency on Barista libraries and does not change pairing,
radio settings or the installed Barista engine.

## Play

1. Build this fork following [BUILD.md](BUILD.md), or use an artifact from this
   fork's **Barista integration** workflow.
2. Start a real GamePad session in Barista as your desktop user.
3. In melonDS, open **Config → Barista settings**, select **Enable Barista
   connection**, choose a display layout and click **Apply**.
4. Start a DS or DSi game. The dialog shows connection state, endpoint, frame/audio
   counts and the age of the last input report. It can remain open during play.

The primary emulator instance owns the connection. Extra multiplayer instances
keep their own ordinary input/output. Other programs must release the Barista
connection before melonDS can use it; a server rejection appears in the dialog.
The client retries automatically when the session or service returns.

| Display | GamePad output | Touch region |
| --- | --- | --- |
| Bottom screen (default) | 640×480 centered | Bottom screen |
| Top screen | 640×480 centered | None |
| Side by side | Two 432×324 screens centered vertically | Right screen |
| Top and bottom | Two 320×240 screens centered horizontally | Lower screen |

All modes retain the DS screens' 4:3 aspect ratio. The stream is 864×480 I420;
unused areas are black. Hardware renderers preserve detail up to 640×480 per
screen before the selected layout scales it. Desktop layout, rotation, window
size and OSD do not affect the GamePad stream. The software, OpenGL and OpenGL
compute renderers are supported.

A/B/X/Y, D-pad, L/R and Plus/Minus map to the DS's corresponding buttons and
Start/Select. The left stick also acts as the D-pad. Desktop buttons combine
with GamePad buttons; a desktop mouse/stylus contact takes precedence over a
GamePad contact. Touch maps only inside the displayed bottom screen, with black
borders and the top screen excluded. Reports expire after 500 ms, releasing
GamePad buttons and touch. **Use GamePad buttons, left stick and touchscreen**
disables remote input while retaining video and audio.

Audio follows melonDS's volume and mute settings and is sent as signed 16-bit
48 kHz stereo, resampled when the SDL output device uses another rate. Idle,
pause and stop show melonDS artwork rather than continuing an active stream.
Video uses one replaceable pending frame; audio is bounded to 100 ms. A slow or
disconnected service cannot grow an unbounded queue or block emulator progress.
The worker disconnects and retries after an unresponsive socket write.

## Connection settings

The feature is disabled by default. Settings persist in melonDS.toml:

```toml
[Barista]
Enabled = true
Input = true
Screen = 0 # 0 bottom, 1 top, 2 side by side, 3 top and bottom
SocketPath = "" # automatic
```

A nonempty socket path overrides `BARISTA_MUG_SOCKET`; otherwise the environment
override or `/run/barista/media-<desktop-uid>.sock` is used. **Apply and reconnect**
applies the visible settings and starts a new connection. **Cancel** discards
unapplied edits; previously applied settings stay in effect. No elevated
privileges are needed for the emulator. AppHook is available only on Linux;
other platforms retain the normal emulator and show the unavailable state.

The connector uses the documented 16-byte little-endian MUG1 header over Unix
`SOCK_SEQPACKET`, with video/idle chunks up to 16,384 bytes, 100 ms activity
heartbeats, little-endian PCM and exact 128-byte GamePad input reports. It
consumes rejection messages and clears queued media/input on disconnect.
The implementation is in `src/frontend/qt_sdl/barista/Connector.*`, with emulator
hooks in `EmuInstanceBarista.cpp`, `EmuThread.cpp` and `EmuInstanceAudio.cpp`.

## Verify

With the normal melonDS build dependencies installed:

```sh
cmake -S . -B build-barista -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_BARISTA_TESTS=ON
cmake --build build-barista --parallel
ctest --test-dir build-barista --output-on-failure
```

The connector tests cover layout/color conversion, all DS buttons, stick and
touch decoding, stale input, video/idle chunk assembly, 44.1-to-48 kHz PCM,
rejection/reconnection, disabled input, malformed reports and bounded shutdown
against an unresponsive peer. Linux socket tests require an environment that
permits local Unix sockets. A core-only build can run these tests with
`-DBUILD_QT_SDL=OFF`.

For the actual frontend/core/renderers, install Xvfb, clang, llvm-objcopy and Mesa
software GL, then run:

```sh
python3 tests/barista/emulator_smoke.py build-barista/melonDS \
  --qt-driver build-barista/tests/libbarista-qt-driver.so \
  --artifacts build-barista/smoke-results
```

Use a fresh artifact directory for each run. This generates an original
homebrew ROM, boots it with melonDS's built-in BIOS/firmware, and verifies all
three renderers at 2× hardware scale. It checks colors/orientation, all four
layouts, A-button and ARM7 pen contact, stale-touch release, input disable,
PCM, idle/active transitions, pause/resume/stop, settings persistence and clean
exit. It saves a settings screenshot and emulator log per renderer. Settings,
sockets and the test display are isolated; the test needs no commercial ROM,
GamePad, Wi-Fi adapter or privileged service. The Qt automation library is
built only when tests are enabled and is never installed with the emulator.

The automated checks establish emulator/API behavior. Physical GamePad playback
and DSi-specific games still need hardware/game testing.
