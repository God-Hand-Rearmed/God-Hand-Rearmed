// Linux test of the runtime's side of PCSX2's GS (src/lib/gs/gs_pcsx2_backend.cpp) without a disc: a raylib game
// window as ps2_host_display::openHostWindow makes it (GSPcsx2Backend::BeforeHostWindow first), the X11 child window
// AttachHostWindow puts over it, then a GSPcsx2Backend fed a GIF packet and vsyncs as the GS thread feeds it. Checks
// what GHR depends on: libpcsx2gs.so found next to the executable, GLFW on X11, the child window following the game
// window's size, hidden and shown again for a host UI, a graphics API switch to Direct3D 11 turned down with the
// Vulkan window kept, the shader cache written under KAMITE_HOME, and the picture on the child window. First, the
// "Preparing shaders" notice of a first start: the GS opened on a thread of its own (as GHR's GS thread does) on an
// empty shader cache while the main thread keeps the window: the notice drawn over the hidden child window, the child
// shown again after the first frame, and the shader count against the library's own cache index.
//   gs_pcsx2_backend_test [frames]      (exit 0 = every check passed; run under Xvfb like pcsx2_gs_smoke)
//   gs_pcsx2_backend_test --no-x11      (no display at all: the PCSX2 GS turns itself down)
// PS2X_NOTICE_SNAPSHOT=<file.ppm>: the game window with the notice on it, written once it has shown for 0.4 s.
#include "runtime/gs/gs_pcsx2_backend.h"
#include "runtime/gs/gs_shader_notice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
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

    // The window's image as a binary PPM.
    bool WritePpm(Display *display, Window window, const char *path)
    {
        XWindowAttributes a{};
        if (!XGetWindowAttributes(display, window, &a))
            return false;
        XImage *image = XGetImage(display, window, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
        std::FILE *file = image ? std::fopen(path, "wb") : nullptr;
        if (file)
        {
            std::fprintf(file, "P6\n%d %d\n255\n", a.width, a.height);
            for (int y = 0; y < a.height; ++y)
                for (int x = 0; x < a.width; ++x)
                {
                    const unsigned long px = XGetPixel(image, x, y);
                    const unsigned char rgb[3] = {static_cast<unsigned char>(px >> 16), static_cast<unsigned char>(px >> 8),
                                                  static_cast<unsigned char>(px)};
                    std::fwrite(rgb, 1, 3, file);
                }
            std::fclose(file);
        }
        if (image)
            XDestroyImage(image);
        return file != nullptr;
    }

    // Entries in PCSX2's Vulkan shader cache index (gs_shader_notice.h: header, then one per shader); -1 = no file or
    // a size the layout does not explain.
    long VulkanIndexEntries(const std::filesystem::path &cacheDir)
    {
        const ps2x::gs::ShaderIndexFormat &format = ps2x::gs::kShaderIndexes[0];
        std::error_code ec;
        const uintmax_t size = std::filesystem::file_size(cacheDir / format.name, ec);
        if (ec || size < format.headerBytes || (size - format.headerBytes) % format.entryBytes != 0u)
            return -1;
        return static_cast<long>((size - format.headerBytes) / format.entryBytes);
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
    // The shader notice on, and shown from the open's start: its drawn path runs whatever the machine's speed (the
    // delay itself is shader_notice_test's). No Mesa disk cache, so the shaders are really compiled.
    setenv("PS2X_SHADER_NOTICE", "1", 1);
    setenv("PS2X_SHADER_NOTICE_DELAY_MS", "0", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
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
    auto feed = [&](GSPcsx2Backend &gs)
    {
        gs.RawGif(reinterpret_cast<const uint8_t *>(packet.data()), static_cast<uint32_t>(packet.size() * 8u));
        regs.csr ^= 0x2000u;
        gs.VSync(regs);
    };
    auto frame = [&](GSPcsx2Backend &gs)
    {
        BeginDrawing();
        ClearBackground(RlColor{0, 0, 0, 255});
        EndDrawing();
        GSPcsx2Backend::HostTick();
        feed(gs);
    };

    // The first start: the GS thread opens the GS on an empty shader cache; the main thread runs the host loop's part
    // (HostTick, then the notice drawn when there is one) until the first frame is out.
    {
        const std::filesystem::path cacheDir = home / "shadercache_pcsx2";
        double ms = 0.0;
        int64_t compiled = 0;
        check(!GSPcsx2Backend::ShaderNoticeResult(ms, compiled), "before the GS opens: no shader notice result yet");
        std::atomic<bool> firstFrame{false};
        std::atomic<bool> stop{false};
        std::thread gsThread([&]
        {
            GSPcsx2Backend gs;
            gs.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
            feed(gs);
            firstFrame.store(true);
            while (!stop.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }); // ~GSPcsx2Backend closes the GS on its own thread
        const char *snapshot = std::getenv("PS2X_NOTICE_SNAPSHOT");
        bool shown = false, hiddenWhileShown = true, drawn = false, snapped = false, seconds = false;
        const auto started = std::chrono::steady_clock::now();
        auto firstShown = started;
        std::string lastDetail;
        while (std::chrono::steady_clock::now() - started < std::chrono::seconds(120))
        {
            GSPcsx2Backend::HostTick();
            std::string title, detail;
            float blockStart = 0.0f, blockEnd = 0.0f;
            const bool notice = GSPcsx2Backend::ShaderNotice(title, detail, blockStart, blockEnd);
            BeginDrawing();
            ClearBackground(RlColor{0, 0, 0, 255});
            if (notice)
                ps2x::gs::drawShaderNotice(title.c_str(), detail.c_str(), blockStart, blockEnd);
            EndDrawing();
            if (notice)
            {
                if (!shown)
                    firstShown = std::chrono::steady_clock::now();
                shown = true;
                lastDetail = detail;
                seconds = seconds || detail.rfind("1 s", 0) == 0;
                hiddenWhileShown = hiddenWhileShown && !Mapped(display, child);
                // The bar's row (gs_shader_notice_draw.cpp: from the middle of the window, max(8, height / 60) high):
                // its grey track or its red block.
                const long bar = Pixel(display, parent, 0.5f, (224.0f + 4.0f) / 448.0f);
                drawn = drawn || Near(bar, 60, 60, 60) || Near(bar, 230, 41, 55);
                if (snapshot && *snapshot && !snapped &&
                    std::chrono::steady_clock::now() - firstShown > std::chrono::milliseconds(400))
                    snapped = WritePpm(display, parent, snapshot);
            }
            if (firstFrame.load() && GSPcsx2Backend::ShaderNoticeResult(ms, compiled))
                break;
        }
        GSPcsx2Backend::HostTick(); // the notice's end: the child shown again
        if (!firstFrame.load())
            std::fprintf(stderr, "[backend-test] FAIL: the GS thread never got its first frame out (waiting for it)\n");
        stop.store(true);
        gsThread.join();
        check(GSPcsx2Backend::ShaderNoticeResult(ms, compiled), "the first frame ends the shader notice");
        std::fprintf(stderr, "[backend-test] first frame %.0f ms after the open, %lld shaders compiled, notice %s%s%s\n", ms,
                     static_cast<long long>(compiled), shown ? "shown, last: " : "not shown", lastDetail.c_str(),
                     snapped ? " (snapshot written)" : "");
        const long entries = VulkanIndexEntries(cacheDir);
        check(entries > 0, "the Vulkan shader index has the layout gs_shader_notice.h reads (header + 32-byte entries)");
        // Counted up to the first present; the present itself may add a couple more (2 on lavapipe) to the index.
        check(compiled > 0 && compiled <= entries && entries - compiled <= 8,
              ("an empty cache: the notice counted the shaders PCSX2 compiled (" + std::to_string(entries) + " in the index)").c_str());
        check(shown, "the notice shows while the GS opens");
        check(hiddenWhileShown, "the GS window is hidden while the notice shows");
        check(drawn, "the notice's bar is drawn in the game window");
        if (ms >= 2500.0)
            check(seconds, "the notice counts the seconds");
        check(Mapped(display, child), "after the first frame the GS window shows again");
        std::string title, detail;
        float blockStart = 0.0f, blockEnd = 0.0f;
        check(!GSPcsx2Backend::ShaderNotice(title, detail, blockStart, blockEnd), "after the first frame: no notice");
    }

    {
        GSPcsx2Backend gs;
        gs.Initialize(vram.data(), static_cast<uint32_t>(vram.size()));
        const uint64_t presentsBefore = GSPcsx2Backend::Presents(); // the first start's above
        for (int i = 0; i < frames / 2; ++i)
            frame(gs);
        check(GSPcsx2Backend::Presents() - presentsBefore == static_cast<uint64_t>(frames / 2), "every vsync presented");
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
