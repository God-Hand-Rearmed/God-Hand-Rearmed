#include "ps2_host_cursor.h"

#include "ps2_host_backend.h" // raylib

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

// GLFW, built into raylib (rglfw.c; raylib does not expose GLFW's header): the cursor calls, declared as glfw3.h does.
extern "C"
{
    struct GLFWwindow;
    struct GLFWcursor;
    struct GLFWimage
    {
        int width;
        int height;
        unsigned char *pixels;
    };
    GLFWwindow *glfwGetCurrentContext(void);
    GLFWcursor *glfwCreateCursor(const GLFWimage *image, int xhot, int yhot);
    void glfwSetCursor(GLFWwindow *window, GLFWcursor *cursor);
    void glfwDestroyCursor(GLFWcursor *cursor);
}

namespace ps2_host_cursor
{
    namespace
    {
        std::mutex g_mutex;
        std::string g_path;   // setImage's, guarded
        float g_hot[2] = {};
        float g_share = 0.0f;
        bool g_changed = false;

        // Window thread.
        std::vector<uint8_t> g_rgba; // the image as loaded
        int g_width = 0, g_height = 0;
        float g_hotUsed[2] = {}, g_shareUsed = 0.0f;
        GLFWcursor *g_cursor = nullptr;
        int g_cursorHeight = 0;   // the pointer's height it was made at

        // An area average of the premultiplied pixels (rows, then columns), so the outline stays clean when shrunk.
        std::vector<uint8_t> shrink(const std::vector<uint8_t> &rgba, int w, int h, int ow, int oh)
        {
            std::vector<float> pm(static_cast<size_t>(w) * h * 4u), rows(static_cast<size_t>(ow) * h * 4u),
                out(static_cast<size_t>(ow) * oh * 4u);
            for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            {
                const float a = rgba[i * 4u + 3u] / 255.0f;
                for (int c = 0; c < 3; ++c)
                    pm[i * 4u + c] = rgba[i * 4u + c] * a;
                pm[i * 4u + 3u] = a;
            }
            const auto area = [](const float *in, int n, size_t inStride, float *dst, int m, size_t outStride)
            {
                const float scale = static_cast<float>(n) / static_cast<float>(m);
                for (int i = 0; i < m; ++i)
                {
                    const float a = static_cast<float>(i) * scale, b = static_cast<float>(i + 1) * scale;
                    float sum[4] = {}, weight = 0.0f;
                    for (int k = static_cast<int>(a); k < n && static_cast<float>(k) < b; ++k)
                    {
                        const float part = std::min(b, static_cast<float>(k) + 1.0f) - std::max(a, static_cast<float>(k));
                        if (part <= 0.0f)
                            continue;
                        for (int c = 0; c < 4; ++c)
                            sum[c] += in[static_cast<size_t>(k) * inStride + c] * part;
                        weight += part;
                    }
                    for (int c = 0; c < 4; ++c)
                        dst[static_cast<size_t>(i) * outStride + c] = weight > 0.0f ? sum[c] / weight : 0.0f;
                }
            };
            for (int y = 0; y < h; ++y)
                area(&pm[static_cast<size_t>(y) * w * 4u], w, 4u, &rows[static_cast<size_t>(y) * ow * 4u], ow, 4u);
            for (int x = 0; x < ow; ++x)
                area(&rows[static_cast<size_t>(x) * 4u], h, static_cast<size_t>(ow) * 4u, &out[static_cast<size_t>(x) * 4u], oh,
                     static_cast<size_t>(ow) * 4u);
            std::vector<uint8_t> result(static_cast<size_t>(ow) * oh * 4u);
            for (size_t i = 0; i < static_cast<size_t>(ow) * oh; ++i)
            {
                const float a = out[i * 4u + 3u];
                for (int c = 0; c < 3; ++c)
                    result[i * 4u + c] = static_cast<uint8_t>(std::clamp(a > 0.0f ? out[i * 4u + c] / a : 0.0f, 0.0f, 255.0f) + 0.5f);
                result[i * 4u + 3u] = static_cast<uint8_t>(std::clamp(a * 255.0f, 0.0f, 255.0f) + 0.5f);
            }
            return result;
        }

        void load(const std::string &path)
        {
            g_rgba.clear();
            g_width = g_height = 0;
            if (path.empty())
                return;
            Image image = LoadImage(path.c_str());
            if (!image.data)
            {
                std::fprintf(stderr, "[cursor] the pointer image did not load: %s\n", path.c_str());
                return;
            }
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            g_width = image.width;
            g_height = image.height;
            g_rgba.assign(static_cast<const uint8_t *>(image.data),
                          static_cast<const uint8_t *>(image.data) + static_cast<size_t>(g_width) * g_height * 4u);
            UnloadImage(image);
        }
    }

    void setImage(const std::string &path, float hotU, float hotV, float heightShare)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = path;
        g_hot[0] = hotU;
        g_hot[1] = hotV;
        g_share = heightShare;
        g_changed = true;
    }

    void apply(bool ours)
    {
        if (!IsWindowReady())
            return;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_changed)
            {
                g_changed = false;
                load(g_path);
                g_hotUsed[0] = g_hot[0], g_hotUsed[1] = g_hot[1];
                g_shareUsed = g_share;
                g_cursorHeight = 0; // made again below
            }
        }
        if (g_rgba.empty() || g_shareUsed <= 0.0f)
            return;
        GLFWwindow *window = glfwGetCurrentContext();
        if (!window)
            return;
        const int wanted = std::clamp(static_cast<int>(std::lround(g_shareUsed * static_cast<float>(GetRenderHeight()))), 16, 256);
        if (!g_cursor || wanted != g_cursorHeight)
        {
            const int h = wanted;
            const int w = std::max(1, static_cast<int>(std::lround(static_cast<float>(h) * g_width / g_height)));
            std::vector<uint8_t> pixels = h < g_height ? shrink(g_rgba, g_width, g_height, w, h) : g_rgba;
            const int iw = h < g_height ? w : g_width, ih = h < g_height ? h : g_height;
            GLFWimage image{iw, ih, pixels.data()};
            const int hx = std::clamp(static_cast<int>(std::lround(g_hotUsed[0] * iw)), 0, iw - 1);
            const int hy = std::clamp(static_cast<int>(std::lround(g_hotUsed[1] * ih)), 0, ih - 1);
            GLFWcursor *made = glfwCreateCursor(&image, hx, hy);
            if (!made)
            {
                std::fprintf(stderr, "[cursor] glfwCreateCursor failed (%dx%d): the OS arrow stays\n", iw, ih);
                g_rgba.clear(); // no retry every frame
                return;
            }
            GLFWcursor *old = g_cursor;
            g_cursor = made;
            g_cursorHeight = wanted;
            if (ours)
                glfwSetCursor(window, g_cursor);
            if (old)
                glfwDestroyCursor(old); // after the new one is set: a destroyed current cursor reverts to the arrow
            return;
        }
        if (ours)
            glfwSetCursor(window, g_cursor); // every frame: a frame that set its own shape (rlImGui) gets ours back
    }
}
