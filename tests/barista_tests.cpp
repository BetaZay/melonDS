// SPDX-License-Identifier: GPL-3.0-or-later
#include "barista/Connector.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#if defined(__linux__)
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using namespace Barista;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Wait(Predicate predicate, const char* message)
{
    const auto deadline = Clock::now() + 3s;
    while (!predicate())
    {
        Check(Clock::now() < deadline, message);
        std::this_thread::sleep_for(2ms);
    }
}
std::array<uint8_t, 128> Report(float x = 432, float y = 240, bool touch = false)
{
    std::array<uint8_t, 128> report{};
    // Neutral sticks.
    report[7] = report[9] = 8;
    int rawX = int((x * 854 / Width + 23.1097f) / 0.2210755f);
    int rawY = int((507.639f-y) / 0.12772f);
    for (int i = 0; i < 10; i++)
    {
        int b = 36+4*i;
        report[b] = rawX; report[b+1] = rawX >> 8;
        report[b+2] = rawY; report[b+3] = rawY >> 8;
    }
    if (touch) report[37] |= 0x10;
    return report;
}
void TestPixelsAndInput()
{
    std::vector<uint32_t> top(256*192, 0xFFFF0000), bottom(256*192, 0xFF00FF00);
    for (auto mode : {ScreenMode::Bottom, ScreenMode::Top, ScreenMode::SideBySide, ScreenMode::TopBottom})
    {
        auto layout = GetLayout(mode);
        auto rgb = ComposeScreens(top.data(), bottom.data(), 256, 192, mode);
        Check(rgb.size() == Width*Height*3, "RGB frame size");
        auto checkRect = [&](Rect rect, int channel)
        {
            if (!rect.width) return;
            size_t offset = ((rect.y+rect.height/2)*Width + rect.x+rect.width/2)*3;
            Check(rgb[offset+channel] == 255 && rgb[offset+2] == 0, "Screen color/channel/layout");
            Check(rect.width*3 == rect.height*4, "Screen aspect ratio");
        };
        checkRect(layout.top, 0); checkRect(layout.bottom, 1);
        auto input = DecodeInput(Report(432, 240, true), mode);
        if (mode == ScreenMode::Top) Check(!input.touching, "Top-only mode must reject touch");
        else
        {
            auto rect = layout.bottom;
            auto contact = DecodeInput(Report(rect.x+rect.width/2, rect.y+rect.height/2, true), mode);
            Check(contact.touching && std::abs(contact.x-128) <= 1 && std::abs(contact.y-96) <= 1, "Touch must follow displayed bottom screen");
            Check(!DecodeInput(Report(5, 5, true), mode).touching, "Borders must reject touch");
        }
        if (layout.top.width)
            Check(!DecodeInput(Report(layout.top.x+layout.top.width/2, layout.top.y+layout.top.height/2, true), mode).touching, "Top region must reject touch");
        Check(!DecodeInput(Report(), mode).touching, "Zero pressure must release touch");
    }
    auto neutral = Report();
    Check(DecodeInput(neutral, ScreenMode::Bottom).mask == 0xFFF, "Neutral report");
    constexpr uint16_t mapping[] = {0x8000,0x4000,4,8,0x400,0x800,0x200,0x100,0x10,0x20,0x2000,0x1000};
    for (int bit = 0; bit < 12; bit++)
    {
        auto report = neutral;
        report[2] = mapping[bit] >> 8; report[3] = mapping[bit];
        Check(DecodeInput(report, ScreenMode::Bottom).mask == (0xFFF & ~(1 << bit)), "Button mapping");
    }
    neutral[6] = 0xFF; neutral[7] = 0xF;
    Check(!(DecodeInput(neutral, ScreenMode::Bottom).mask & (1 << 4)), "Left stick maps to D-pad");
    std::vector<uint8_t> red(Width*Height*3, 0);
    for (size_t i = 0; i < red.size(); i += 3) red[i] = 255;
    auto yuv = RgbToI420(red);
    Check(yuv.size() == FrameBytes && yuv[0] == 82 && yuv[Width*Height] == 90 && yuv.back() == 240, "Limited-range BT.601 red");
    auto black = RgbToI420(std::vector<uint8_t>(Width*Height*3, 0));
    Check(black[0] == 16 && black[Width*Height] == 128 && black.back() == 128, "Limited-range black");
    Check(RgbToI420({1,2,3}).empty(), "Reject incorrect RGB size");
    // Asymmetric rows catch a vertical flip in the software composition.
    top[0] = 0xFF0000FF;
    auto rgb = ComposeScreens(top.data(), bottom.data(), 256,192,ScreenMode::Top);
    Check(rgb[(112*3)+2] == 255 && rgb[((479*Width+112)*3)] == 255, "Framebuffer row orientation");
}

#if defined(__linux__)
uint32_t Get(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1])<<8 | uint32_t(p[2])<<16 | uint32_t(p[3])<<24; }
void Put(uint8_t* p, uint32_t value) { for (int i = 0; i < 4; i++) p[i] = value >> (8*i); }
struct Server
{
    int listener = -1, peer = -1;
    std::string path;
    Server()
    {
        char dir[] = "/tmp/melonds-apphook-XXXXXX";
        Check(mkdtemp(dir) != nullptr, "Create isolated socket directory");
        path = std::string(dir) + "/media.sock";
        listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        Check(listener >= 0, "Create local test socket");
        sockaddr_un address{}; address.sun_family = AF_UNIX;
        std::copy(path.begin(),path.end(),address.sun_path);
        Check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address)) == 0, "Bind local test socket");
        Check(listen(listener,4) == 0, "Listen");
    }
    ~Server()
    {
        if (peer >= 0) close(peer);
        if (listener >= 0) close(listener);
        unlink(path.c_str());
        rmdir(path.substr(0,path.rfind('/')).c_str());
    }
    void Accept()
    {
        Wait([&] { peer = accept4(listener,nullptr,nullptr,SOCK_NONBLOCK | SOCK_CLOEXEC); return peer >= 0; }, "Client must reconnect/accept");
    }
    std::vector<uint8_t> Packet()
    {
        pollfd p{peer,POLLIN,0};
        Check(poll(&p,1,3000) > 0, "Expected protocol packet");
        std::vector<uint8_t> bytes(16400);
        auto n = recv(peer,bytes.data(),bytes.size(),MSG_TRUNC);
        Check(n >= 16 && n <= ssize_t(bytes.size()), "Packet length");
        bytes.resize(n);
        Check(Get(bytes.data()) == 0x3147554d, "MUG1 magic");
        return bytes;
    }
    std::vector<uint8_t> Frame(uint32_t type)
    {
        std::vector<uint8_t> bytes;
        uint32_t id = 0;
        while (bytes.size() < FrameBytes)
        {
            auto packet = Packet();
            if (Get(packet.data()+4) != type) continue;
            if (bytes.empty()) id = Get(packet.data()+8);
            Check(Get(packet.data()+8) == id && Get(packet.data()+12) == bytes.size(), "Frame id/offset continuity");
            bytes.insert(bytes.end(),packet.begin()+16,packet.end());
        }
        Check(bytes.size() == FrameBytes, "Complete frame size");
        return bytes;
    }
    void Send(uint32_t type, const std::vector<uint8_t>& payload)
    {
        std::vector<uint8_t> packet(16+payload.size());
        Put(packet.data(),0x3147554d); Put(packet.data()+4,type);
        std::copy(payload.begin(),payload.end(),packet.begin()+16);
        Check(send(peer,packet.data(),packet.size(),MSG_NOSIGNAL) == ssize_t(packet.size()), "Server packet send");
    }
};
void TestProtocol()
{
    Server server;
    Connector connector;
    Options options{true,true,ScreenMode::Bottom,server.path};
    connector.Configure(options,std::vector<uint8_t>(Width*Height*3,0));
    server.Accept();
    auto idle = server.Frame(2);
    Check(idle[0] == 16, "Idle art transmission");
    connector.SetActive(true);
    Wait([&] { return connector.GetStatus().connected; }, "Connection status");
    auto report = Report(432,240,true); report[2] = 0x80;
    server.Send(5,{report.begin(),report.end()});
    Wait([&] { return connector.GetStatus().inputReports == 1; }, "Input report reception");
    auto input = connector.ReadInput();
    Check(input.touching && !(input.mask & 1), "Live touch and buttons");
    std::this_thread::sleep_for(550ms);
    Check(!connector.ReadInput().touching && connector.ReadInput().mask == 0xFFF, "Stale report must release touch and buttons");
    std::vector<uint32_t> red(256*192,0xFFFF0000), green(256*192,0xFF00FF00);
    connector.SubmitScreens(red.data(),green.data(),256,192);
    auto frame = server.Frame(1);
    Check(frame[240*Width+432] == 144 && frame[0] == 16, "Video content/black borders");
    Check(connector.GetStatus().frames == 1, "Frame statistics");
    std::vector<int16_t> audio(441*2,12345);
    connector.SubmitAudio(audio.data(),441,44100);
    std::vector<uint8_t> pcm;
    while (pcm.empty())
    {
        auto packet = server.Packet();
        if (Get(packet.data()+4) == 4) pcm.assign(packet.begin()+16,packet.end());
    }
    Check(pcm.size() >= 1916 && pcm.size() <= 1924 && pcm.size()%4 == 0, "Resample 44.1kHz to 48kHz stereo");
    Check(pcm[0] == 0x39 && pcm[1] == 0x30, "PCM little-endian samples");
    connector.SetActive(false);
    Check(connector.ReadInput().mask == 0xFFF && !connector.GetStatus().active, "Pause releases input");
    close(server.peer); server.peer = -1;
    Wait([&] { return !connector.GetStatus().connected; }, "Detect disconnect");
    server.Accept(); server.Frame(2);
    Wait([&] { return connector.GetStatus().connected; }, "Reconnect");
    connector.SetActive(true);
    Check(connector.ReadInput().mask == 0xFFF, "Reconnect must discard previous input");
    server.Send(6,{'B','u','s','y'});
    Wait([&] { return connector.GetStatus().error.find("Busy") != std::string::npos; }, "Surface rejection reason");
    connector.Stop();
    Check(!connector.GetStatus().enabled, "Shutdown disables connector");
    close(server.peer); server.peer = -1;

    options.input = false;
    connector.Configure(options);
    server.Accept();
    server.Send(5,{report.begin(),report.end()});
    Wait([&] { return connector.GetStatus().inputReports > 0; }, "Input off still consumes reports");
    connector.SetActive(true);
    Check(connector.ReadInput().mask == 0xFFF, "Input can be independently disabled");
    // Bad reports must close the session, never latch the prior contact.
    server.Send(5,std::vector<uint8_t>(127));
    Wait([&] { return !connector.GetStatus().connected; }, "Malformed report disconnects");
    connector.Stop();
    close(server.peer); server.peer = -1;

    connector.Configure(options);
    server.Accept();
    Wait([&] { return connector.GetStatus().connected; }, "Backpressure connection");
    connector.SetActive(true);
    connector.SubmitScreens(red.data(),green.data(),256,192);
    std::this_thread::sleep_for(20ms); // Let the unread peer block the frame sender.
    auto start = Clock::now(); connector.Stop();
    Check(Clock::now()-start < 1s, "Blocked peer cannot hang shutdown");
    options.socketPath = std::string(108,'x');
    connector.Configure(options);
    Check(!connector.GetStatus().error.empty() && !connector.GetStatus().connected, "Invalid endpoint status");
    connector.Configure(Options{});
    Check(!connector.GetStatus().enabled, "Disable through configuration");
}
#endif
int main()
{
    try
    {
        TestPixelsAndInput();
#if defined(__linux__)
        TestProtocol();
#endif
        std::cout << "Barista layout, colors, input and protocol tests passed\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
