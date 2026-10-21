// SPDX-License-Identifier: GPL-3.0-or-later
// MUG1 transport follows the Barista connectors in BetaZay/Cemu and BetaZay/azahar.
#include "Connector.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace Barista
{
using Clock = std::chrono::steady_clock;

Layout GetLayout(ScreenMode mode)
{
    switch (mode)
    {
    case ScreenMode::Top: return {{112, 0, 640, 480}, {0, 0, 0, 0}};
    case ScreenMode::SideBySide: return {{0, 78, 432, 324}, {432, 78, 432, 324}};
    case ScreenMode::TopBottom: return {{272, 0, 320, 240}, {272, 240, 320, 240}};
    default: return {{0, 0, 0, 0}, {112, 0, 640, 480}};
    }
}

std::vector<uint8_t> ComposeScreens(const uint32_t* top, const uint32_t* bottom,
                                   unsigned width, unsigned height, ScreenMode mode)
{
    if (!top || !bottom || !width || !height || width > 8192 || height > 8192) return {};
    std::vector<uint8_t> rgb(Width * Height * 3, 0);
    const auto layout = GetLayout(mode);
    const auto draw = [&](const uint32_t* pixels, Rect rect)
    {
        for (int y = 0; y < rect.height; y++)
            for (int x = 0; x < rect.width; x++)
            {
                uint32_t pixel = pixels[size_t(y) * height / rect.height * width + size_t(x) * width / rect.width];
                size_t dest = (size_t(y + rect.y) * Width + x + rect.x) * 3;
                rgb[dest] = pixel >> 16;
                rgb[dest + 1] = pixel >> 8;
                rgb[dest + 2] = pixel;
            }
    };
    draw(top, layout.top);
    draw(bottom, layout.bottom);
    return rgb;
}

std::vector<uint8_t> RgbToI420(const std::vector<uint8_t>& rgb)
{
    if (rgb.size() != Width * Height * 3) return {};
    std::vector<uint8_t> out(FrameBytes);
    for (unsigned y = 0; y < Height; y += 2)
        for (unsigned x = 0; x < Width; x += 2)
        {
            int rs = 0, gs = 0, bs = 0;
            for (unsigned dy = 0; dy < 2; dy++)
                for (unsigned dx = 0; dx < 2; dx++)
                {
                    size_t pixel = (y + dy) * Width + x + dx;
                    int r = rgb[pixel * 3], g = rgb[pixel * 3 + 1], b = rgb[pixel * 3 + 2];
                    out[pixel] = std::clamp(((66*r + 129*g + 25*b + 128) >> 8) + 16, 16, 235);
                    rs += r; gs += g; bs += b;
                }
            int r = rs / 4, g = gs / 4, b = bs / 4;
            size_t chroma = (y / 2) * (Width / 2) + x / 2;
            out[Width * Height + chroma] = std::clamp(((-38*r - 74*g + 112*b + 128) >> 8) + 128, 16, 240);
            out[Width * Height * 5 / 4 + chroma] = std::clamp(((112*r - 94*g - 18*b + 128) >> 8) + 128, 16, 240);
        }
    return out;
}

Input DecodeInput(const std::array<uint8_t, 128>& report, ScreenMode mode)
{
    Input input;
    const uint16_t buttons = (uint16_t(report[2]) << 8) | report[3];
    constexpr uint16_t mapping[] = {0x8000, 0x4000, 0x4, 0x8, 0x400, 0x800,
                                     0x200, 0x100, 0x10, 0x20, 0x2000, 0x1000};
    for (unsigned i = 0; i < 12; i++)
        if (buttons & mapping[i]) input.mask &= ~(1 << i);
    // Left stick also acts as a D-pad, using the same calibration as Azahar.
    const int stickX = report[6] | (unsigned(report[7]) << 8);
    const int stickY = report[8] | (unsigned(report[9]) << 8);
    if (stickX > 2508) input.mask &= ~(1 << 4);
    if (stickX < 1588) input.mask &= ~(1 << 5);
    if (stickY > 2508) input.mask &= ~(1 << 6);
    if (stickY < 1588) input.mask &= ~(1 << 7);

    int pressure = ((report[37] >> 4) & 7) | (((report[39] >> 4) & 7) << 3) |
                   (((report[41] >> 4) & 7) << 6) | (((report[43] >> 4) & 7) << 9);
    Rect rect = GetLayout(mode).bottom;
    if (!pressure || !rect.width) return input;
    int rawX = 0, rawY = 0;
    for (int i = 0; i < 10; i++)
    {
        int b = 36 + 4 * i;
        rawX += ((report[b + 1] & 15) << 8) | report[b];
        rawY += ((report[b + 3] & 15) << 8) | report[b + 2];
    }
    // The physical panel is 854 pixels wide; the encoded frame is padded to 864.
    const float x = std::clamp(-23.1097f + rawX / 10.0f * 0.2210755f, 0.0f, 854.0f) * Width / 854.0f;
    const float y = std::clamp(507.639f - rawY / 10.0f * 0.12772f, 0.0f, 480.0f);
    if (x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height)
    {
        input.touching = true;
        input.x = std::clamp(int((x - rect.x) * 256 / rect.width), 0, 255);
        input.y = std::clamp(int((y - rect.y) * 192 / rect.height), 0, 191);
    }
    return input;
}

class Connector::Session
{
public:
    explicit Session(Options opts, std::vector<uint8_t> idle) : options(std::move(opts)), logo(RgbToI420(idle))
    {
        status.enabled = options.enabled;
        status.endpoint = options.socketPath;
    }
    ~Session() { Stop(); }
    void Start()
    {
#if defined(__linux__)
        if (options.socketPath.empty() || options.socketPath.size() >= sizeof(sockaddr_un::sun_path))
        {
            status.error = "Invalid socket path (maximum 107 bytes).";
            return;
        }
        worker = std::thread([this] { Run(); });
#else
        status.error = "Barista AppHook requires Linux.";
#endif
    }
    void Stop()
    {
        stopping = true;
        if (worker.joinable()) worker.join();
    }
    const Options options;
    std::atomic<bool> connected{false}, on{false};
    mutable std::mutex mutex;
    Status status;
    std::vector<uint8_t> pending;
    std::deque<int16_t> audio;
    std::array<uint8_t, 128> input{};
    Clock::time_point inputTime{};
private:
#if defined(__linux__)
    static constexpr size_t Chunk = 16384;
    static void Put(uint8_t* p, uint32_t v) { for (unsigned i = 0; i < 4; i++) p[i] = v >> (8*i); }
    static uint32_t Get(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
    bool Send(int fd, uint32_t type, uint32_t id, size_t offset, const uint8_t* data, size_t size)
    {
        std::array<uint8_t, 16 + Chunk> packet{};
        Put(packet.data(), 0x3147554d); Put(packet.data()+4, type);
        Put(packet.data()+8, id); Put(packet.data()+12, offset);
        std::copy_n(data, size, packet.data()+16);
        const auto deadline = Clock::now() + std::chrono::milliseconds(100);
        do
        {
            auto n = send(fd, packet.data(), size+16, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (n == ssize_t(size+16)) return true;
            if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) return false;
            pollfd p{fd, POLLOUT, 0};
            poll(&p, 1, 2);
        } while (!stopping && Clock::now() < deadline);
        return false;
    }
    bool SendFrame(int fd, uint32_t type, uint32_t id, const std::vector<uint8_t>& bytes)
    {
        for (size_t offset = 0; offset < bytes.size(); offset += Chunk)
            if (!Send(fd, type, id, offset, bytes.data()+offset, std::min(Chunk, bytes.size()-offset))) return false;
        return true;
    }
    bool Receive(int fd)
    {
        // Drain reports before sending a whole frame to keep input latency bounded.
        for (int i = 0; i < 64; i++)
        {
            std::array<uint8_t, 16 + Chunk> packet{};
            auto n = recv(fd, packet.data(), packet.size(), MSG_DONTWAIT | MSG_TRUNC);
            if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
            if (n < 16 || n > ssize_t(packet.size()) || Get(packet.data()) != 0x3147554d) return false;
            const auto type = Get(packet.data()+4);
            if (type == 6)
            {
                std::lock_guard<std::mutex> lock(mutex);
                status.error = "Connection rejected: " + std::string(reinterpret_cast<char*>(packet.data()+16), n-16);
                return false;
            }
            if (type != 5) continue; // Future optional server messages.
            if (n != 144 || Get(packet.data()+12) != 0) return false;
            std::lock_guard<std::mutex> lock(mutex);
            std::copy_n(packet.data()+16, input.size(), input.begin());
            inputTime = Clock::now();
            status.inputReports++;
        }
        return true;
    }
    void RunSession(int fd)
    {
        {
            std::lock_guard<std::mutex> lock(mutex);
            status.error.clear();
            inputTime = {};
        }
        connected = true;
        uint32_t id = 0;
        if (!SendFrame(fd, 2, ++id, logo)) return;
        auto heartbeat = Clock::time_point{};
        while (!stopping)
        {
            if (!Receive(fd)) break;
            if (Clock::now() >= heartbeat)
            {
                uint8_t a = on;
                if (!Send(fd, 3, 0, 0, &a, 1)) break;
                heartbeat = Clock::now() + std::chrono::milliseconds(100);
            }
            std::vector<uint8_t> frame, pcm;
            {
                std::lock_guard<std::mutex> lock(mutex);
                frame.swap(pending);
                while (!audio.empty() && pcm.size() < Chunk)
                {
                    uint16_t value = audio.front(); audio.pop_front();
                    pcm.push_back(value); pcm.push_back(value >> 8);
                }
            }
            if (on && !frame.empty())
            {
                if (!SendFrame(fd, 1, ++id, RgbToI420(frame))) break;
                std::lock_guard<std::mutex> lock(mutex);
                status.frames++;
            }
            if (on && !pcm.empty())
            {
                if (!Send(fd, 4, 0, 0, pcm.data(), pcm.size())) break;
                std::lock_guard<std::mutex> lock(mutex);
                status.audioChunks++;
            }
            pollfd p{fd, POLLIN, 0};
            poll(&p, 1, 2);
        }
    }
    void Run()
    {
        while (!stopping)
        {
            int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            std::memcpy(address.sun_path, options.socketPath.c_str(), options.socketPath.size()+1);
            int bufferSize = 2 * 1024 * 1024;
            if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufferSize, sizeof(bufferSize));
            if (fd >= 0 && connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0)
                RunSession(fd);
            else
            {
                std::lock_guard<std::mutex> lock(mutex);
                status.error = std::strerror(errno);
            }
            connected = false;
            {
                std::lock_guard<std::mutex> lock(mutex);
                pending.clear(); audio.clear(); inputTime = {};
            }
            if (fd >= 0) close(fd);
            for (int i = 0; i < 25 && !stopping; i++)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
#endif
    std::atomic<bool> stopping{false};
    std::thread worker;
    const std::vector<uint8_t> logo;
};

std::string Connector::DefaultSocketPath()
{
    if (const char* path = std::getenv("BARISTA_MUG_SOCKET"); path && *path) return path;
#if defined(__linux__)
    return "/run/barista/media-" + std::to_string(geteuid()) + ".sock";
#else
    return {};
#endif
}
Connector::~Connector() { Stop(); }
void Connector::Stop()
{
    auto old = std::atomic_exchange(&session, std::shared_ptr<Session>{});
    if (old) old->Stop();
}
void Connector::Configure(const Options& options, std::vector<uint8_t> idleRgb)
{
    Stop();
    if (!options.enabled) return;
    Options resolved = options;
    if (resolved.socketPath.empty()) resolved.socketPath = DefaultSocketPath();
    auto next = std::make_shared<Session>(resolved, std::move(idleRgb));
    next->on = active.load();
    next->Start();
    std::atomic_store(&session, std::move(next));
}
void Connector::SetActive(bool value)
{
    active = value;
    if (auto s = std::atomic_load(&session))
    {
        s->on = value;
        if (!value)
        {
            std::lock_guard<std::mutex> lock(s->mutex);
            s->pending.clear(); s->audio.clear(); s->inputTime = {};
        }
    }
}
bool Connector::WantsFrame()
{
    auto s = std::atomic_load(&session);
    if (!s || !s->on || !s->connected) return false;
    auto now = Clock::now();
    if (now - lastFrame < std::chrono::milliseconds(16)) return false;
    lastFrame = now;
    return true;
}
void Connector::SubmitScreens(const uint32_t* top, const uint32_t* bottom, unsigned width, unsigned height)
{
    auto s = std::atomic_load(&session);
    if (!s || !s->on || !s->connected) return;
    auto rgb = ComposeScreens(top, bottom, width, height, s->options.screen);
    std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
    if (lock && s->on) s->pending = std::move(rgb);
}
void Connector::SubmitAudio(const int16_t* samples, size_t frames, unsigned sampleRate)
{
    auto s = std::atomic_load(&session);
    if (!s || !s->on || !s->connected || !frames || !sampleRate) { audioPhase = 0; return; }
    if (audioSession.lock() != s) { audioPhase = 0; audioSession = s; }
    std::vector<int16_t> pcm;
    pcm.reserve(size_t(frames * 48000.0 / sampleRate + 1) * 2);
    for (; audioPhase < frames; audioPhase += sampleRate / 48000.0)
    {
        size_t a = size_t(audioPhase), b = std::min(a+1, frames-1);
        double frac = audioPhase-a;
        for (unsigned channel = 0; channel < 2; channel++)
            pcm.push_back(int16_t(samples[a*2+channel] + frac * (samples[b*2+channel]-samples[a*2+channel])));
    }
    audioPhase -= frames;
    std::unique_lock<std::mutex> lock(s->mutex, std::try_to_lock);
    if (!lock || !s->on) return;
    s->audio.insert(s->audio.end(), pcm.begin(), pcm.end());
    while (s->audio.size() > 9600) s->audio.pop_front();
}
Input Connector::ReadInput() const
{
    auto s = std::atomic_load(&session);
    if (!s || !s->on || !s->connected || !s->options.input) return {};
    std::lock_guard<std::mutex> lock(s->mutex);
    if (s->inputTime == Clock::time_point{} || Clock::now()-s->inputTime > std::chrono::milliseconds(500)) return {};
    return DecodeInput(s->input, s->options.screen);
}
Status Connector::GetStatus() const
{
    auto s = std::atomic_load(&session);
    if (!s) return {};
    std::lock_guard<std::mutex> lock(s->mutex);
    Status result = s->status;
    result.connected = s->connected;
    result.active = s->on;
    if (s->inputTime != Clock::time_point{}) result.inputAgeMs = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-s->inputTime).count();
    return result;
}
}
