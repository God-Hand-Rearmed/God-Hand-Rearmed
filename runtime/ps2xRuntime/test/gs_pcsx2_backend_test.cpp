// Linux test of the runtime's side of PCSX2's GS (src/lib/gs/gs_pcsx2_backend.cpp) without a disc: a raylib game
// window as ps2_host_display::openHostWindow makes it (GSPcsx2Backend::BeforeHostWindow first), the X11 child window
// AttachHostWindow puts over it, then a GSPcsx2Backend fed a GIF packet and vsyncs as the GS thread feeds it. Checks
// what GHR depends on: libpcsx2gs.so found next to the executable, GLFW on X11, the child window following the game
// window's size, hidden and shown again for a host UI, a graphics API switch to Direct3D 11 turned down with the
// Vulkan window kept, the shader cache written under KAMITE_HOME, and the picture on the child window.
//   gs_pcsx2_backend_test [frames]      (exit 0 = every check passed; run under Xvfb like pcsx2_gs_smoke)
//   gs_pcsx2_backend_test --no-x11      (no display at all: the PCSX2 GS turns itself down)
#include "runtime/gs/gs_pcsx2_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <unistd.h>

// raylib's and GLFW's few functions used here, declared as they are: raylib.h and Xlib.h cannot share a file (both
// define Font, among others).
extern "C"
{
    struct RlColor
    {
        unsigned char r, g, b, a;
    };
    void SetTraceLogLevel(int level);
    void SetConfigFlags(unsigned int flags);
    void InitWindow(int width, int height, const char *title);
    bool IsWindowReady(void);
    void *GetWindowHandle(void);
    void SetWindowSize(int width, int height);
    int GetRenderWidth(void);
    void BeginDrawing(void);
    void ClearBackground(RlColor color);
    void EndDrawing(void);
    void CloseWindow(void);

    struct GLFWwindow;
    int glfwGetPlatform(void);
    unsigned long glfwGetX11Window(GLFWwindow *window);
}

namespace
{
    constexpr int kLogWarning = 4;                  // raylib LOG_WARNING
    constexpr unsigned kFlagWindowResizable = 0x4u; // FLAG_WINDOW_RESIZABLE
    constexpr unsigned kFlagAlwaysRun = 0x100u;     // FLAG_WINDOW_ALWAYS_RUN
}

// Kernel/host_vsync.cpp in GHR: no vblank clock here.
bool ps2xHostVsync(std::chrono::steady_clock::time_point &, std::chrono::nanoseconds &) noexcept { return false; }

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::fprintf(stderr, "[backend-test] %s: %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    std::vector<uint64_t> Packet()
    {
        std::vector<uint64_t> ad;
        auto reg = [&](uint8_t r, uint64_t v) { ad.push_back(v), ad.push_back(r); };
        auto rgbaq = [](uint64_t r, uint64_t g, uint64_t b) { return r | g << 8 | b << 16 | 0x80ull << 24 | uint64_t(0x3F800000u) << 32; };
        auto xyz = [](uint64_t x, uint64_t y) { return x * 16 | (y * 16) << 16; };
        auto sprite = [&](int x0, int y0, int x1, int y1, uint64_t c)
        {
            reg(0x00, 6), reg(0x01, c), reg(0x05, xyz(x0, y0)), reg(0x05, xyz(x1, y1));
        };
        reg(0x4C, 10ull << 16);                              // FRAME_1
        reg(0x4E, 1ull << 32);                               // ZBUF_1: ZMSK
        reg(0x40, 639ull << 16 | 0ull << 32 | 447ull << 48); // SCISSOR_1
        reg(0x18, 0);                                        // XYOFFSET_1
        reg(0x47, (1ull << 16) | (1ull << 17));              // TEST_1
        reg(0x1A, 1);                                        // PRMODECONT
        sprite(0, 0, 640, 448, rgbaq(0x10, 0x30, 0x90));
        sprite(100, 100, 300, 250, rgbaq(0xE0, 0x20, 0x20));
        std::vector<uint64_t> out{(ad.size() / 2) | (1ull << 15) | (1ull << 60), 0xEull};
        out.insert(out.end(), ad.begin(), ad.end());
        return out;
    }

    // The game window's child windows (the GS's), on a connection of the test's own.
    std::vector<Window> Children(Display *display, Window parent)
    {
        Window root = 0, up = 0, *list = nullptr;
        unsigned count = 0;
        std::vector<Window> out;
        if (XQueryTree(display, parent, &root, &up, &list, &count) && list)
        {
            out.assign(list, list + count);
            XFree(list);
        }
        return out;
    }

    bool Mapped(Display *display, Window window)
    {
        XWindowAttributes a{};
        return XGetWindowAttributes(display, window, &a) && a.map_state != IsUnmapped;
    }

    // The colour at a fraction of the window (0xRRGGBB), -1 if it cannot be read.
    long Pixel(Display *display, Window window, float fx, float fy, int *width = nullptr)
    {
        XWindowAttributes a{};
        if (!XGetWindowAttributes(display, window, &a))
            return -1;
        if (width)
            *width = a.width;
        XImage *image = XGetImage(display, window, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
        if (!image)
            return -1;
        const long px = static_cast<long>(XGetPixel(image, static_cast<int>(fx * a.width), static_cast<int>(fy * a.height)) & 0xFFFFFFu);
        XDestroyImage(image);
        return px;
    }

    bool Near(long px, int r, int g, int b)
    {
        const auto close = [](int a, int b) { return a >= b - 24 && a <= b + 24; };
        return px >= 0 && close((px >> 16) & 0xFF, r) && close((px >> 8) & 0xFF, g) && close(px & 0xFF, b);
    }
}

int main(int argc, char *argv[])
{
    if (argc > 1 && std::strcmp(argv[1], "--no-x11") == 0)
    {
        // A Wayland session without Xwayland: no $DISPLAY, so no X11 window can exist. The PCSX2 GS must turn itself
        // down before anything is loaded (the runtime then makes its built-in GS) and leave GLFW's platform alone.
        unsetenv("DISPLAY");
        unsetenv("PS2X_GS_RENDERER");
        GSPcsx2Backend::BeforeHostWindow();
        check(!GSPcsx2Backend::Requested(), "no X11 display: Requested() is false (the built-in GS is used)");
        GSPcsx2Backend::AttachHostWindow(reinterpret_cast<void *>(1));
        check(!GSPcsx2Backend::Active(), "no X11 display: no child window, the GS is not active");
        std::fprintf(stderr, "[backend-test] %s\n", g_failures ? "FAILED" : "all checks passed");
        return g_failures ? 1 : 0;
    }
    const int frames = argc > 1 ? std::max(8, std::atoi(argv[1])) : 40;
    const std::filesystem::path home = std::filesystem::temp_directory_path() / ("gs_backend_test_" + std::to_string(getpid()));
    std::filesystem::remove_all(home);
    std::filesystem::create_directories(home);
    setenv("KAMITE_HOME", home.c_str(), 1);
    setenv("PS2X_PCSX2_VSYNC", "0", 1); // Xvfb has no refresh to wait for
    setenv("PS2X_PCSX2_STRETCH", "1", 1); // the picture fills the window, so a fraction of it is a fraction of the picture
    unsetenv("PS2X_GS_RENDERER");

    // As ps2_host_display::openHostWindow.
    SetTraceLogLevel(kLogWarning);
    SetConfigFlags(kFlagWindowResizable | kFlagAlwaysRun);
    GSPcsx2Backend::BeforeHostWindow();
    InitWindow(640, 448, "gs_pcsx2_backend_test");
    check(IsWindowReady() && glfwGetPlatform() == 0x00060004, "the game window is a GLFW X11 window");
    check(GSPcsx2Backend::Requested(), "Requested(): libpcsx2gs.so loaded from the executable's folder");
    GSPcsx2Backend::AttachHostWindow(GetWindowHandle());
    check(GSPcsx2Backend::Active(), "AttachHostWindow made the child window");

    Display *display = XOpenDisplay(nullptr);
    const Window parent = glfwGetX11Window(static_cast<GLFWwindow *>(GetWindowHandle()));
    std::vector<Window> children = Children(display, parent);
    check(children.size() == 1u && Mapped(display, children[0]), "one mapped X11 child window of the game window");
    if (children.size() != 1u)
        return 1;
    const Window child = children[0];

    std::vector<uint8_t> vram(4u * 1024u * 1024u, 0u);
    GSVSyncRegs regs{};
    regs.regset1[0] = 0xFF21ull;               // PMODE
    regs.regset1[1] = 32ull << 3;              // SMODE1
    regs.regset1[7] = 10ull << 9;              // DISPFB1
    regs.regset1[8] = 0x1BF4FF00834138ull;     // DISPLAY1
    const std::vector<uint64_t> packet = Packet();
    auto frame = [&](GSPcsx2Backend &gs)
    {
        BeginDrawing();
        ClearBackground(RlColor{0, 0, 0, 255});
        EndDrawing();
        GSPcsx2Backend::HostTick();
        gs.RawGif(reinterpret_cast<const uint8_t *>(packet.data()), static_cast<uint32_t>(packet.size() * 8u));
        regs.csr ^= 0x2000u;
        gs.VSync(regs);
    };

    {
        GSPcsx2Backend gs;
        gs.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        for (int i = 0; i < frames / 2; ++i)
            frame(gs);
        check(GSPcsx2Backend::Presents() == static_cast<uint64_t>(frames / 2), "every vsync presented");
        check(Near(Pixel(display, child, 200.0f / 640, 175.0f / 448), 0xE0, 0x20, 0x20) &&
                  Near(Pixel(display, child, 20.0f / 640, 20.0f / 448), 0x10, 0x30, 0x90),
              "the child window shows the picture");

        // The game window grows: HostTick resizes the child, the next vsync the swap chain.
        SetWindowSize(960, 672);
        for (int i = 0; i < 4; ++i)
            frame(gs);
        int width = 0;
        const long red = Pixel(display, child, 200.0f / 640, 175.0f / 448, &width);
        check(width == GetRenderWidth() && width == 960, "the child window follows the game window's size");
        check(Near(red, 0xE0, 0x20, 0x20), "the picture is drawn at the new size");

        // A host UI (the F1 debugger) hides the GS window, and closing it shows the window again.
        GSPcsx2Backend::SetHostUiVisible(true);
        check(!Mapped(display, child), "SetHostUiVisible(true) hides the child window");
        GSPcsx2Backend::SetHostUiVisible(false);
        check(Mapped(display, child), "SetHostUiVisible(false) shows it again");

        // The OPTIONS screen's Graphics API: Direct3D 11 on Linux is turned down, the Vulkan window stays.
        check(GSPcsx2Backend::SetGraphicsApi("dx11"), "SetGraphicsApi accepted");
        for (int i = 0; i < 3 && GSPcsx2Backend::GraphicsApiBusy(); ++i)
            frame(gs);
        check(!GSPcsx2Backend::GraphicsApiBusy() && GSPcsx2Backend::GraphicsApiResult() == 2,
              "the switch to dx11 ends as \"already running\" (Vulkan)");
        children = Children(display, parent);
        check(children.size() == 1u && children[0] == child, "the switch's extra window is gone; the GS keeps its own");
        for (int i = 0; i < 4; ++i)
            frame(gs);
        check(Near(Pixel(display, child, 200.0f / 640, 175.0f / 448), 0xE0, 0x20, 0x20), "the picture is still drawn");
    } // ~GSPcsx2Backend closes the GS

    bool cached = false;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator(home / "shadercache_pcsx2", ec))
        cached = cached || entry.file_size() > 0;
    check(cached, "the shader cache is written under KAMITE_HOME");

    XCloseDisplay(display);
    CloseWindow();
    std::filesystem::remove_all(home);
    std::fprintf(stderr, "[backend-test] %s\n", g_failures ? "FAILED" : "all checks passed");
    return g_failures ? 1 : 0;
}
