// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Barista
{
constexpr unsigned Width = 864, Height = 480;
constexpr size_t FrameBytes = Width * Height * 3 / 2;
enum class ScreenMode { Bottom, Top, SideBySide, TopBottom };
struct Rect { int x, y, width, height; };
struct Layout { Rect top, bottom; };
Layout GetLayout(ScreenMode mode);
// Screens use melonDS's packed 0xAARRGGBB pixels, in top-to-bottom row order.
std::vector<uint8_t> ComposeScreens(const uint32_t* top, const uint32_t* bottom,
                                   unsigned width, unsigned height, ScreenMode mode);
std::vector<uint8_t> RgbToI420(const std::vector<uint8_t>& rgb);
struct Input
{
    uint16_t mask = 0xFFF; // DS active-low buttons
    bool touching = false;
    int x = 0, y = 0;
};
Input DecodeInput(const std::array<uint8_t, 128>& report, ScreenMode mode);
struct Options
{
    bool enabled = false;
    bool input = true;
    ScreenMode screen = ScreenMode::Bottom;
    std::string socketPath;
};
struct Status
{
    bool enabled = false, connected = false, active = false;
    std::string endpoint, error;
    uint64_t frames = 0, audioChunks = 0, inputReports = 0;
    int64_t inputAgeMs = -1;
};

class Connector
{
public:
    Connector() = default;
    ~Connector();
    void Configure(const Options& options, std::vector<uint8_t> idleRgb = {});
    void Stop();
    void SetActive(bool active);
    bool WantsFrame(); // emulation thread only
    void SubmitScreens(const uint32_t* top, const uint32_t* bottom, unsigned width, unsigned height);
    // Interleaved signed 16-bit stereo. Resampling state belongs to the audio thread.
    void SubmitAudio(const int16_t* samples, size_t frames, unsigned sampleRate);
    Input ReadInput() const;
    Status GetStatus() const;
    static std::string DefaultSocketPath();
private:
    class Session;
    std::shared_ptr<Session> session;
    std::atomic<bool> active{false};
    std::chrono::steady_clock::time_point lastFrame{};
    std::weak_ptr<Session> audioSession;
    double audioPhase = 0;
};
}
