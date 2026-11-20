// Linux smoke test of libpcsx2gs.so through its C interface (include/pcsx2gs_api.h), the way GHR uses it
// (runtime/ps2xRuntime/src/lib/gs/gs_pcsx2_backend.cpp): dlopen from the executable's folder, pcsx2gs_set_display,
// open on an X11 window with Vulkan, a GIF packet (a blue background and two sprites) at every field, vsyncs that
// present it, a resize, the Direct3D 11 refusal, then checks the picture twice: in the GS's own image (snapshot) and
// on the window itself (XGetImage of what the swap chain presented). Exit 0 = every check passed.
//   pcsx2_gs_smoke [frames]
// Runs headless under Xvfb with Mesa's lavapipe (thirdparty/pcsx2-gs/test/run_smoke.sh; CTest pcsx2_gs_smoke).
#include "pcsx2gs_api.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
    constexpr unsigned kWidth = 640;
    constexpr unsigned kHeight = 448;
    int g_failures = 0;

    void check(bool ok, const char *what)
    {
        std::fprintf(stderr, "[smoke] %s: %s\n", ok ? "PASS" : "FAIL", what);
        if (!ok)
            ++g_failures;
    }

    // A+D packed GIF data (as test/GSTest.cpp).
    struct Packet
    {
        std::vector<uint64_t> words;
        void ad(uint8_t reg, uint64_t value)
        {
            words.push_back(value);
            words.push_back(reg);
        }
        std::vector<uint64_t> finish() const
        {
            const uint64_t nloop = words.size() / 2u;
            std::vector<uint64_t> out{nloop | (1ull << 15) | (1ull << 60), 0xEull}; // NLOOP, EOP, PACKED, NREG=1, A+D
            out.insert(out.end(), words.begin(), words.end());
            return out;
        }
    };

    uint64_t Rgbaq(uint8_t r, uint8_t g, uint8_t b)
    {
        return r | (uint64_t(g) << 8) | (uint64_t(b) << 16) | (0x80ull << 24) | (uint64_t(0x3F800000u) << 32);
    }

    uint64_t Xyz(int x, int y) { return uint64_t(x * 16) | (uint64_t(y * 16) << 16); }

    void Sprite(Packet &p, int x0, int y0, int x1, int y1, uint64_t color)
    {
        p.ad(0x00, 6); // PRIM: sprite
        p.ad(0x01, color);
        p.ad(0x05, Xyz(x0, y0));
        p.ad(0x05, Xyz(x1, y1));
    }

    struct Rgb
    {
        int r = -1, g = -1, b = -1;
    };

    bool near(const Rgb &c, int r, int g, int b)
    {
        const auto close = [](int a, int b) { return a >= b - 24 && a <= b + 24; };
        return close(c.r, r) && close(c.g, g) && close(c.b, b);
    }

    // A pixel of a 24-bit bottom-up .bmp (Pcsx2GsApi::snapshot), at a fraction of its width and height.
    Rgb bmpPixel(const std::vector<unsigned char> &bmp, float fx, float fy, uint32_t &width, uint32_t &height)
    {
        if (bmp.size() < 54 || bmp[0] != 'B' || bmp[1] != 'M')
            return {};
        uint32_t offset = 0;
        std::memcpy(&offset, &bmp[10], 4);
        std::memcpy(&width, &bmp[18], 4);
        std::memcpy(&height, &bmp[22], 4);
        const uint32_t row = (width * 3u + 3u) & ~3u;
        const uint32_t x = static_cast<uint32_t>(fx * width), y = static_cast<uint32_t>(fy * height);
        const size_t at = offset + static_cast<size_t>(height - 1u - y) * row + x * 3u;
        if (at + 2 >= bmp.size())
            return {};
        return {bmp[at + 2], bmp[at + 1], bmp[at]};
    }

    void pump(Display *display)
    {
        while (XPending(display))
        {
            XEvent event;
            XNextEvent(display, &event);
        }
    }
}

int main(int argc, char *argv[])
{
    const int frames = argc > 1 ? std::max(4, std::atoi(argv[1])) : 30;
    const std::filesystem::path exeDir = std::filesystem::read_symlink("/proc/self/exe").parent_path();

    // The library as GHR loads it.
    const std::filesystem::path library = exeDir / "libpcsx2gs.so";
    void *module = dlopen(library.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
    {
        std::fprintf(stderr, "[smoke] FAIL: %s did not load: %s\n", library.c_str(), dlerror());
        return 1;
    }
    const auto getApi = reinterpret_cast<Pcsx2GsGetApiFn>(dlsym(module, "pcsx2gs_get_api"));
    const auto setDisplay = reinterpret_cast<void (*)(void *)>(dlsym(module, "pcsx2gs_set_display"));
    const auto setApi = reinterpret_cast<int (*)(const char *, void *, uint32_t, uint32_t)>(dlsym(module, "pcsx2gs_set_api"));
    const auto setScreen = reinterpret_cast<void (*)(const Pcsx2GsScreenItem *, uint32_t, const char *, uint32_t)>(
        dlsym(module, "pcsx2gs_set_screen"));
    const Pcsx2GsApi *api = getApi ? getApi(PCSX2GS_API_VERSION) : nullptr;
    check(api && setDisplay && setApi && setScreen, "the C interface is exported (get_api, set_display, set_api, set_screen)");
    check(dlsym(module, "_ZN8GSConfigE") == nullptr && dlsym(module, "ImGui_ImplVulkan_Init") == nullptr &&
              dlsym(module, "cpuinfo_initialize") == nullptr,
          "nothing but the C interface is exported");
    if (!api || !setDisplay || !setApi || !setScreen)
        return 1;

    Display *display = XOpenDisplay(nullptr);
    if (!display)
    {
        std::fprintf(stderr, "[smoke] FAIL: no X11 display (DISPLAY=%s)\n", std::getenv("DISPLAY") ? std::getenv("DISPLAY") : "");
        return 1;
    }
    const Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, kWidth, kHeight, 0, 0, 0);
    XStoreName(display, window, "pcsx2_gs_smoke");
    XMapWindow(display, window);
    XSync(display, False);

    // A fresh cache each run, so the Vulkan device compiles every shader through libshaderc_shared.so.1.
    const std::filesystem::path work = std::filesystem::temp_directory_path() / ("pcsx2_gs_smoke_" + std::to_string(getpid()));
    std::filesystem::remove_all(work);
    std::filesystem::create_directories(work / "cache");
    const std::string resources = (exeDir / "resources").string();
    const std::string cache = (work / "cache").string();

    // A 4x4 opaque yellow PNG for the port's screen (the OPTIONS screen draws its PNGs the same way): decoded by the
    // replacement loader (compat/PngStb.inl on Linux) and drawn over the picture at each present.
    static constexpr unsigned char kYellowPng[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00,
        0x04, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00, 0x00, 0xA9, 0xF1, 0x9E, 0x7E, 0x00, 0x00, 0x00, 0x12, 0x49,
        0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xFF, 0x9F, 0xE1, 0x3F, 0x32, 0x66, 0x20, 0x5D, 0x00, 0x00, 0x62, 0x36,
        0x2F, 0xD1, 0x88, 0xC1, 0xA7, 0xDF, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};
    const std::string png = (work / "yellow.png").string();
    std::ofstream(png, std::ios::binary).write(reinterpret_cast<const char *>(kYellowPng), sizeof(kYellowPng));
    Pcsx2GsScreenItem pngItem{};
    pngItem.kind = PCSX2GS_SCREEN_IMAGE;
    pngItem.text = 0; // the path, at the start of the strings block
    pngItem.fill = 0xFFFFFFFFu; // no tint
    pngItem.x = 0.8f, pngItem.y = 0.05f, pngItem.x1 = 0.95f, pngItem.y1 = 0.2f; // over the blue background

    setDisplay(display);
    Pcsx2GsOpenParams params{};
    params.hwnd = reinterpret_cast<void *>(window);
    params.width = kWidth;
    params.height = kHeight;
    params.scale = 1.0f;
    params.resources = resources.c_str();
    params.cache = cache.c_str();
    params.vsync = 0; // Xvfb has no refresh to wait for
    params.stretch = 1;
    // GHR's default image options (gs_pcsx2_backend.cpp EnsureOpen: FXAA on, 16x anisotropy, post effects upscaled;
    // FXAA leaves the inside of the sprites as it is) and api=dx11: Linux has Vulkan only, so the library must use
    // Vulkan anyway. PCSX2_GS_SMOKE_OPTIONS replaces them.
    const char *optionsEnv = std::getenv("PCSX2_GS_SMOKE_OPTIONS");
    params.options = optionsEnv ? optionsEnv : "api=dx11;fxaa=1;anisotropy=16;native_scaling=upscaled";
    const bool opened = api->open(&params) != 0;
    check(opened, "open (Vulkan, X11 window; api=dx11 asked and refused)");
    if (!opened)
        return 1;

    bool shaderc = false;
    std::string driver;
    {
        std::ifstream maps("/proc/self/maps");
        for (std::string line; std::getline(maps, line);)
        {
            shaderc = shaderc || line.find("libshaderc_shared.so") != std::string::npos;
            if (const size_t at = line.find("/libvulkan_"); driver.empty() && at != std::string::npos)
                driver = line.substr(at + 1);
        }
    }
    check(shaderc, "libshaderc_shared.so is loaded (shaders compiled at run time)");
    std::fprintf(stderr, "[smoke] Vulkan driver: %s\n", driver.empty() ? "(not a Mesa one)" : driver.c_str());

    // Urban Reign's 480p display (test/GSTest.cpp): PMODE circuit 1, SMODE1 LC 32, DISPFB1 frame buffer 0 640 wide
    // PSMCT32, DISPLAY1 as the game sets it.
    Pcsx2GsPrivRegs regs{};
    regs.regset1[0] = 0xFF21ull;
    regs.regset1[1] = 32ull << 3;
    regs.regset1[2] = 0ull;
    regs.regset1[7] = 10ull << 9;
    regs.regset1[8] = 0x1BF4FF00834138ull;

    Packet p;
    p.ad(0x4C, 10ull << 16);                              // FRAME_1: FBP 0, FBW 10, PSMCT32
    p.ad(0x4E, 1ull << 32);                               // ZBUF_1: ZMSK
    p.ad(0x40, 639ull << 16 | 0ull << 32 | 447ull << 48); // SCISSOR_1
    p.ad(0x18, 0ull);                                     // XYOFFSET_1
    p.ad(0x47, (1ull << 16) | (1ull << 17));              // TEST_1: ZTE, ZTST always
    p.ad(0x1A, 1ull);                                     // PRMODECONT
    Sprite(p, 0, 0, 640, 448, Rgbaq(0x10, 0x30, 0x90));   // background
    Sprite(p, 100, 100, 300, 250, Rgbaq(0xE0, 0x20, 0x20)); // red
    Sprite(p, 340, 180, 600, 400, Rgbaq(0x20, 0xC0, 0x40)); // green
    const std::vector<uint64_t> packet = p.finish();

    for (int i = 0; i < frames; ++i)
    {
        pump(display);
        if (i == frames / 2)
        {
            // The window grows (as the game window's child follows it) and the device resizes its swap chain.
            XResizeWindow(display, window, kWidth * 3 / 2, kHeight * 3 / 2);
            XSync(display, False);
            api->resize(kWidth * 3 / 2, kHeight * 3 / 2);
        }
        setScreen(&pngItem, 1u, png.c_str(), static_cast<uint32_t>(png.size() + 1u));
        api->gif(reinterpret_cast<const uint8_t *>(packet.data()), static_cast<uint32_t>(packet.size() / 2u));
        regs.csr ^= 0x2000u; // the other field
        api->vsync(&regs);
    }
    check(setApi("dx11", nullptr, kWidth, kHeight) == 2, "pcsx2gs_set_api(\"dx11\") is refused, Vulkan keeps running");

    // The GS's image (internal resolution, before the window).
    const std::string bmpPath = (work / "frame.bmp").string();
    check(api->snapshot(bmpPath.c_str()) != 0, "snapshot written");
    std::vector<unsigned char> bmp;
    if (std::FILE *f = std::fopen(bmpPath.c_str(), "rb"))
    {
        bmp.resize(static_cast<size_t>(std::filesystem::file_size(bmpPath)));
        bmp.resize(std::fread(bmp.data(), 1, bmp.size(), f));
        std::fclose(f);
    }
    uint32_t w = 0, h = 0;
    const Rgb red = bmpPixel(bmp, 200.0f / 640.0f, 175.0f / 448.0f, w, h);
    const Rgb green = bmpPixel(bmp, 470.0f / 640.0f, 290.0f / 448.0f, w, h);
    const Rgb blue = bmpPixel(bmp, 20.0f / 640.0f, 20.0f / 448.0f, w, h);
    std::fprintf(stderr, "[smoke] snapshot %ux%u: red %d,%d,%d green %d,%d,%d background %d,%d,%d\n", w, h, red.r, red.g,
                 red.b, green.r, green.g, green.b, blue.r, blue.g, blue.b);
    check(near(red, 0xE0, 0x20, 0x20) && near(green, 0x20, 0xC0, 0x40) && near(blue, 0x10, 0x30, 0x90),
          "the GS image has the two sprites on the background");

    // What the swap chain presented, read back from the X server (stretch=1: the picture fills the window).
    XWindowAttributes attributes{};
    XGetWindowAttributes(display, window, &attributes);
    XImage *image = XGetImage(display, window, 0, 0, attributes.width, attributes.height, AllPlanes, ZPixmap);
    Rgb shown[4];
    if (image)
    {
        // red sprite, green sprite, background, the PNG's middle
        const float at[4][2] = {{200.0f, 175.0f}, {470.0f, 290.0f}, {20.0f, 20.0f}, {560.0f, 56.0f}};
        for (int i = 0; i < 4; ++i)
        {
            const unsigned long px = XGetPixel(image, static_cast<int>(at[i][0] / 640.0f * attributes.width),
                                               static_cast<int>(at[i][1] / 448.0f * attributes.height));
            shown[i] = {static_cast<int>((px >> 16) & 0xFF), static_cast<int>((px >> 8) & 0xFF), static_cast<int>(px & 0xFF)};
        }
        XDestroyImage(image);
    }
    std::fprintf(stderr, "[smoke] window %dx%d: red %d,%d,%d green %d,%d,%d background %d,%d,%d\n", attributes.width,
                 attributes.height, shown[0].r, shown[0].g, shown[0].b, shown[1].r, shown[1].g, shown[1].b, shown[2].r,
                 shown[2].g, shown[2].b);
    check(attributes.width == static_cast<int>(kWidth * 3 / 2), "the window was resized");
    check(near(shown[0], 0xE0, 0x20, 0x20) && near(shown[1], 0x20, 0xC0, 0x40) && near(shown[2], 0x10, 0x30, 0x90),
          "the presented window shows the two sprites on the background");
    std::fprintf(stderr, "[smoke] screen image %d,%d,%d\n", shown[3].r, shown[3].g, shown[3].b);
    check(near(shown[3], 0xFF, 0xFF, 0x00), "the screen's PNG (stb_image) is drawn over the picture");

    bool cached = false;
    for (const auto &entry : std::filesystem::directory_iterator(work / "cache"))
        cached = cached || entry.file_size() > 0;
    check(cached, "the shader cache was written to the cache folder");

    api->close();
    XDestroyWindow(display, window);
    XCloseDisplay(display);
    std::filesystem::remove_all(work);
    std::fprintf(stderr, "[smoke] %s (%d frames)\n", g_failures ? "FAILED" : "all checks passed", frames);
    return g_failures ? 1 : 0;
}
