// The PCSX2 GS's "Preparing shaders" notice drawn into the game window (runtime/gs/gs_shader_notice.h). A file of its
// own: raylib.h cannot share one with <Windows.h> (gs_pcsx2_backend.cpp) or Xlib.h (gs_pcsx2_backend_test.cpp).
#include "runtime/gs/gs_shader_notice.h"

#include "raylib.h"

#include <algorithm>

void ps2x::gs::drawShaderNotice(const char *title, const char *detail, float blockStart, float blockEnd)
{
    const int width = GetScreenWidth();
    const int height = GetScreenHeight();
    const int titleSize = std::max(18, height / 26);
    const int detailSize = std::max(14, height / 36);
    const int barWidth = std::max(1, std::min(width - 2 * titleSize, std::max(240, width / 2)));
    const int barHeight = std::max(8, height / 60);
    const int barX = (width - barWidth) / 2;
    const int titleY = height / 2 - titleSize - barHeight;
    const int barY = height / 2;
    const int detailY = barY + barHeight + detailSize;

    const int titleWidth = MeasureText(title, titleSize);
    DrawText(title, (width - titleWidth) / 2, titleY, titleSize, RAYWHITE);
    DrawRectangle(barX, barY, barWidth, barHeight, Color{60, 60, 60, 255});
    const int blockX = barX + static_cast<int>(std::clamp(blockStart, 0.0f, 1.0f) * static_cast<float>(barWidth));
    const int blockRight = barX + static_cast<int>(std::clamp(blockEnd, 0.0f, 1.0f) * static_cast<float>(barWidth));
    DrawRectangle(blockX, barY, std::max(1, blockRight - blockX), barHeight, Color{230, 41, 55, 255}); // raylib RED
    const int detailWidth = MeasureText(detail, detailSize);
    DrawText(detail, (width - detailWidth) / 2, detailY, detailSize, LIGHTGRAY);
}
