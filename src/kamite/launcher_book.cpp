#include "kamite/launcher_book.h"

#include "imgui_impl_raylib.h"
#include "imgui_internal.h"
#include "rlgl.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace kamite::launcher::book
{
    namespace
    {
        // A draw list's commands for the renderer below: one marker callback, its data copied into the list (the
        // first field says what it is). The marker itself is never called.
        enum class Kind : uint8_t
        {
            Ink,
            Paint,
            Loop,
            Tick,
            Line,
            Paper,
        };
        struct Mode
        {
            Kind kind;
        };
        struct LoopData
        {
            Kind kind = Kind::Loop;
            float x0, y0, x1, y1, drawn, width, squareness;
            uint32_t seed;
            ImU32 color;
        };
        struct TickData
        {
            Kind kind = Kind::Tick;
            float x, y, size;
            bool ticked;
            ImU32 color;
        };
        struct LineData
        {
            Kind kind = Kind::Line;
            int count;
            float width, taper;
            ImU32 color;
            float points[2 * kMaxLinePoints];
        };
        struct PaperData
        {
            Kind kind = Kind::Paper;
            float quad[8];
            float shadow;
            ImU32 tint;
        };

        void marker(const ImDrawList *, const ImDrawCmd *) {}

        Texture2D g_paper{};

        ImU32 faded(ImU32 color)
        {
            const float a = static_cast<float>((color >> IM_COL32_A_SHIFT) & 0xFFu) * ImGui::GetStyle().Alpha;
            return (color & ~IM_COL32_A_MASK) | (static_cast<ImU32>(std::clamp(a, 0.0f, 255.0f) + 0.5f) << IM_COL32_A_SHIFT);
        }
        Color toColor(ImU32 c)
        {
            return Color{static_cast<unsigned char>(c >> IM_COL32_R_SHIFT), static_cast<unsigned char>(c >> IM_COL32_G_SHIFT),
                         static_cast<unsigned char>(c >> IM_COL32_B_SHIFT), static_cast<unsigned char>(c >> IM_COL32_A_SHIFT)};
        }

        template <typename T>
        void record(ImDrawList *list, const T &data)
        {
            list->AddCallback(marker, const_cast<T *>(&data), sizeof(T));
        }

        // ---- the renderer ---------------------------------------------------------------------------------------------
        Kind g_mode = Kind::Ink;
        bool g_modeOn = false;

        void endMode()
        {
            if (!g_modeOn)
                return;
            rlDrawRenderBatchActive();
            if (g_mode == Kind::Ink)
                saloon::endInk();
            else
                EndBlendMode();
            g_modeOn = false;
        }

        void setMode(Kind mode)
        {
            if (g_modeOn && g_mode == mode)
                return;
            endMode();
            g_mode = mode;
            if (mode == Kind::Ink)
                saloon::beginInk();
            else
            {
                BeginBlendMode(BLEND_ALPHA);
                rlDisableBackfaceCulling();
            }
            g_modeOn = true;
        }

        void scissor(const Space &space, const ImVec4 &clip)
        {
            const Vector2 c[4] = {space.map(Vector2{clip.x, clip.y}), space.map(Vector2{clip.z, clip.y}),
                                  space.map(Vector2{clip.z, clip.w}), space.map(Vector2{clip.x, clip.w})};
            float x0 = c[0].x, y0 = c[0].y, x1 = c[0].x, y1 = c[0].y;
            for (const Vector2 &p : c)
            {
                x0 = std::min(x0, p.x);
                y0 = std::min(y0, p.y);
                x1 = std::max(x1, p.x);
                y1 = std::max(y1, p.y);
            }
            const float w = static_cast<float>(GetScreenWidth()), h = static_cast<float>(GetScreenHeight());
            if (!(std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1)) || x1 <= x0 || y1 <= y0)
            {
                x0 = y0 = 0.0f; // a clip far off the book (its homography folds over): the whole screen
                x1 = w;
                y1 = h;
            }
            x0 = std::clamp(std::floor(x0), 0.0f, w);
            y0 = std::clamp(std::floor(y0), 0.0f, h);
            x1 = std::clamp(std::ceil(x1), 0.0f, w);
            y1 = std::clamp(std::ceil(y1), 0.0f, h);
            rlDrawRenderBatchActive();
            BeginScissorMode(static_cast<int>(x0), static_cast<int>(y0), static_cast<int>(std::max(0.0f, x1 - x0)),
                             static_cast<int>(std::max(0.0f, y1 - y0)));
        }

        void vertex(const Space &space, ImVec2 pos, ImVec2 uv, ImU32 col)
        {
            const Vector2 p = space.map(Vector2{pos.x, pos.y});
            const Color c = toColor(col);
            rlColor4ub(c.r, c.g, c.b, c.a);
            rlTexCoord2f(uv.x, uv.y);
            rlVertex2f(p.x, p.y);
        }

        void triangles(const Space &space, const ImDrawList *list, const ImDrawCmd &cmd)
        {
            if (cmd.ElemCount < 3)
                return;
            const ImDrawIdx *idx = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert *vtx = list->VtxBuffer.Data + cmd.VtxOffset;
            // A glyph's quad grows a texel each way into the atlas's padding: warped (scaled and moved off the pixel grid)
            // its edge would otherwise cut through the glyph's soft outer pixels (flat-bottomed letters).
            const ImTextureData *tex = cmd.TexRef._TexData;
            const float texelU = tex && tex->Width > 0 ? 1.0f / static_cast<float>(tex->Width) : 0.0f;
            const float texelV = tex && tex->Height > 0 ? 1.0f / static_cast<float>(tex->Height) : 0.0f;
            rlBegin(RL_TRIANGLES);
            rlSetTexture(static_cast<unsigned int>(cmd.GetTexID()));
            for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3)
            {
                if (texelU > 0.0f && i + 5 < cmd.ElemCount && idx[i + 3] == idx[i] && idx[i + 4] == idx[i + 2])
                {
                    const ImDrawVert &a = vtx[idx[i]], &b = vtx[idx[i + 1]], &c = vtx[idx[i + 2]], &d = vtx[idx[i + 5]];
                    const float w = c.pos.x - a.pos.x, h = c.pos.y - a.pos.y, du = c.uv.x - a.uv.x, dv = c.uv.y - a.uv.y;
                    if (b.pos.y == a.pos.y && d.pos.x == a.pos.x && b.pos.x == c.pos.x && d.pos.y == c.pos.y && w > 0.0f && h > 0.0f &&
                        du > 0.0f && dv > 0.0f && a.col == c.col)
                    {
                        const float ex = texelU * w / du, ey = texelV * h / dv; // a texel, in pixels
                        const ImVec2 p0(a.pos.x - ex, a.pos.y - ey), p1(c.pos.x + ex, c.pos.y + ey);
                        const ImVec2 t0(a.uv.x - texelU, a.uv.y - texelV), t1(c.uv.x + texelU, c.uv.y + texelV);
                        vertex(space, p0, t0, a.col);
                        vertex(space, ImVec2(p1.x, p0.y), ImVec2(t1.x, t0.y), a.col);
                        vertex(space, p1, t1, a.col);
                        vertex(space, p0, t0, a.col);
                        vertex(space, p1, t1, a.col);
                        vertex(space, ImVec2(p0.x, p1.y), ImVec2(t0.x, t1.y), a.col);
                        i += 3;
                        continue;
                    }
                }
                for (unsigned int k = 0; k < 3; ++k)
                {
                    const ImDrawVert &v = vtx[idx[i + k]];
                    vertex(space, v.pos, v.uv, v.col);
                }
            }
            rlEnd();
            rlDrawRenderBatchActive();
        }

        void drawPaper(const Space &space, const PaperData &d)
        {
            Vector2 q[4];
            for (int i = 0; i < 4; ++i)
                q[i] = Vector2{d.quad[2 * i], d.quad[2 * i + 1]};
            const Color tint = toColor(d.tint);
            const float alpha = tint.a / 255.0f;
            if (d.shadow > 0.0f)
            {
                // three widening rings, down and out a little (like the corner slip's)
                const Vector2 centre{(q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f};
                for (int ring = 3; ring >= 1; --ring)
                {
                    const float grow = d.shadow * static_cast<float>(ring) / 3.0f;
                    Vector2 s[4];
                    for (int i = 0; i < 4; ++i)
                    {
                        const float dx = q[i].x - centre.x, dy = q[i].y - centre.y, len = std::max(1.0f, std::hypot(dx, dy));
                        s[i] = space.map(Vector2{q[i].x + dx / len * grow + d.shadow * 0.4f, q[i].y + dy / len * grow + d.shadow * 0.8f});
                    }
                    const Color shade{0, 0, 0, static_cast<unsigned char>(26.0f * alpha)};
                    DrawTriangle(s[0], s[3], s[2], shade);
                    DrawTriangle(s[0], s[2], s[1], shade);
                }
            }
            constexpr int kCells = 4;
            constexpr float kTile = 300.0f; // virtual pixels a tile of the paper texture
            const auto lerp = [](Vector2 a, Vector2 b, float t) { return Vector2{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t}; };
            const auto at = [&](float u, float v) { return lerp(lerp(q[0], q[1], u), lerp(q[3], q[2], u), v); };
            // the paper's colour, then its grain from the scene's texture laid over it, faint (the texture alone reads
            // as kraft paper beside the book's cream pages)
            rlDrawRenderBatchActive();
            for (int pass = 0; pass < (g_paper.id != 0u ? 2 : 1); ++pass)
            {
            rlSetTexture(pass == 1 ? g_paper.id : rlGetTextureIdDefault());
            rlBegin(RL_QUADS);
            for (int y = 0; y < kCells; ++y)
                for (int x = 0; x < kCells; ++x)
                {
                    const int corners[4][2] = {{x, y}, {x, y + 1}, {x + 1, y + 1}, {x + 1, y}};
                    for (const auto &corner : corners)
                    {
                        const float u = static_cast<float>(corner[0]) / kCells, v = static_cast<float>(corner[1]) / kCells;
                        const bool rim = corner[0] == 0 || corner[0] == kCells || corner[1] == 0 || corner[1] == kCells;
                        const float shade = rim ? 0.90f : 1.0f;
                        const Vector2 p = at(u, v);
                        if (pass == 0)
                            rlColor4ub(static_cast<unsigned char>(tint.r * shade), static_cast<unsigned char>(tint.g * shade * 0.98f),
                                       static_cast<unsigned char>(tint.b * shade * 0.95f), tint.a);
                        else
                            rlColor4ub(255, 255, 255, static_cast<unsigned char>(tint.a * 0.32f));
                        rlTexCoord2f(p.x / kTile, p.y / kTile);
                        const Vector2 s = space.map(p);
                        rlVertex2f(s.x, s.y);
                    }
                }
            rlEnd();
            rlSetTexture(0);
            rlDrawRenderBatchActive();
            }
        }

        void command(const Space &space, const void *data)
        {
            const Kind kind = static_cast<const Mode *>(data)->kind;
            switch (kind)
            {
            case Kind::Ink:
            case Kind::Paint:
                setMode(kind);
                break;
            case Kind::Loop:
            {
                const Kind was = g_mode;
                setMode(Kind::Ink);
                const auto &d = *static_cast<const LoopData *>(data);
                saloon::inkLoop(Rectangle{d.x0, d.y0, d.x1 - d.x0, d.y1 - d.y0}, d.drawn, d.seed, toColor(d.color), space.toScreen, d.width, d.squareness);
                setMode(was);
                break;
            }
            case Kind::Tick:
            {
                const Kind was = g_mode;
                setMode(Kind::Ink);
                const auto &d = *static_cast<const TickData *>(data);
                saloon::inkTickBox(Vector2{d.x, d.y}, d.size, d.ticked, toColor(d.color), space.toScreen);
                setMode(was);
                break;
            }
            case Kind::Line:
            {
                const Kind was = g_mode;
                setMode(Kind::Ink);
                const auto &d = *static_cast<const LineData *>(data);
                Vector2 points[kMaxLinePoints];
                for (int i = 0; i < d.count; ++i)
                    points[i] = Vector2{d.points[2 * i], d.points[2 * i + 1]};
                saloon::inkLine(points, d.count, d.width, toColor(d.color), space.toScreen, d.taper);
                setMode(was);
                break;
            }
            case Kind::Paper:
            {
                const Kind was = g_mode;
                setMode(Kind::Paint);
                drawPaper(space, *static_cast<const PaperData *>(data));
                setMode(was);
                break;
            }
            }
        }
    }

    Space spaceFrom(const saloon::Quad &left, const saloon::Quad &right)
    {
        Space space;
        const saloon::Quad q{{left.p[0], right.p[1], right.p[2], left.p[3]}};
        const float x0 = (q.p[0].x + q.p[3].x) * 0.5f, x1 = (q.p[1].x + q.p[2].x) * 0.5f;
        const float y0 = (q.p[0].y + q.p[1].y) * 0.5f, y1 = (q.p[2].y + q.p[3].y) * 0.5f;
        if (!(x1 - x0 > 8.0f && y1 - y0 > 8.0f))
            return space;
        space.rect = Rectangle{x0, y0, x1 - x0, y1 - y0};
        const saloon::Homography h = saloon::Homography::rectToQuad(space.rect.width, space.rect.height, q);
        // moved so virtual pixels are the rectangle's own: H x T(-x0, -y0)
        saloon::Homography t = h;
        for (int row = 0; row < 3; ++row)
            t.m[row * 3 + 2] = h.m[row * 3 + 2] - h.m[row * 3] * x0 - h.m[row * 3 + 1] * y0;
        space.toScreen = t;
        space.toVirtual = t.inverse();
        const Vector2 top{(left.p[1].x + right.p[0].x) * 0.5f, (left.p[1].y + right.p[0].y) * 0.5f};
        space.spine = space.unmap(top).x;
        space.valid = true;
        return space;
    }

    void beginFrame(const Space &space)
    {
        ImGui_ImplRaylib_NewFrame();
        ImGuiContext &g = *ImGui::GetCurrentContext();
        const int first = g.InputEventsQueue.Size; // events trickled over from the last frame are virtual already
        ImGui_ImplRaylib_ProcessEvents();
        // The mouse in the book's own (virtual) pixels: rlImGui's position events (screen pixels) mapped in place. A
        // second, virtual position event after them was wrong on a click: ImGui trickles a position event that follows
        // a button change to the next frame, so the press and the release were taken at the screen position, up to a
        // row off where the book is tilted (K, Nov 21: a click near a row's top took the row above).
        const bool mouse = IsWindowFocused() && IsCursorOnScreen();
        for (int i = first; i < g.InputEventsQueue.Size; ++i)
        {
            ImGuiInputEvent &e = g.InputEventsQueue[i];
            if (e.Type != ImGuiInputEventType_MousePos)
                continue;
            if (!mouse)
            {
                e.MousePos.PosX = e.MousePos.PosY = -FLT_MAX; // ImGui's "no mouse" (rlImGui sends FLT_MIN)
                continue;
            }
            const Vector2 v = space.unmap(Vector2{e.MousePos.PosX, e.MousePos.PosY});
            e.MousePos.PosX = v.x;
            e.MousePos.PosY = v.y;
        }
        ImGui::NewFrame();
    }

    void endFrame(const Space &space)
    {
        ImGui::Render();
        ImDrawData *data = ImGui::GetDrawData();
        {
            // the font atlas and other textures: rlImGui's own path, with no lists to draw
            ImDrawData textures = *data;
            textures.CmdLists.clear();
            textures.CmdListsCount = 0;
            ImGui_ImplRaylib_RenderDrawData(&textures);
        }
        rlDrawRenderBatchActive();
        for (int l = 0; l < data->CmdListsCount; ++l)
        {
            const ImDrawList *list = data->CmdLists[l];
            setMode(Kind::Ink);
            for (const ImDrawCmd &cmd : list->CmdBuffer)
            {
                scissor(space, cmd.ClipRect);
                if (cmd.UserCallback == marker)
                {
                    command(space, cmd.UserCallbackData);
                    continue;
                }
                if (cmd.UserCallback != nullptr)
                {
                    if (cmd.UserCallback != ImDrawCallback_ResetRenderState)
                        cmd.UserCallback(list, &cmd);
                    continue;
                }
                triangles(space, list, cmd);
            }
        }
        endMode();
        EndScissorMode();
        rlEnableBackfaceCulling();
    }

    void setPaperTexture(const Texture2D &paper) { g_paper = paper; }

    void ink(ImDrawList *list) { record(list, Mode{Kind::Ink}); }
    void paint(ImDrawList *list) { record(list, Mode{Kind::Paint}); }

    void loop(ImDrawList *list, ImVec2 min, ImVec2 max, float drawn, uint32_t seed, ImU32 color, float width)
    {
        // Round the box, not across it: a rounded-rectangle loop (squareness 4) whose path clears the box's corners
        // even where the hand's spiral and wobble pull it in (~13 %): |x / rx|^4 + |y / ry|^4 ~ 0.5 at the corners.
        const float a = (max.x - min.x) * 0.5f, b = (max.y - min.y) * 0.5f, cx = (min.x + max.x) * 0.5f, cy = (min.y + max.y) * 0.5f;
        const float rx = a * 1.25f + width * 1.5f, ry = b * 1.75f + width;
        LoopData d;
        d.squareness = 4.0f;
        d.x0 = cx - rx;
        d.y0 = cy - ry;
        d.x1 = cx + rx;
        d.y1 = cy + ry;
        d.drawn = drawn;
        d.width = width;
        d.seed = seed;
        d.color = faded(color);
        record(list, d);
    }

    void tickBox(ImDrawList *list, ImVec2 centre, float size, bool ticked, ImU32 color)
    {
        TickData d;
        d.x = centre.x;
        d.y = centre.y;
        d.size = size;
        d.ticked = ticked;
        d.color = faded(color);
        record(list, d);
    }

    void line(ImDrawList *list, const ImVec2 *points, int count, float width, ImU32 color, float taper)
    {
        if (count < 2)
            return;
        LineData d;
        d.count = std::min(count, kMaxLinePoints);
        d.width = width;
        d.taper = taper;
        d.color = faded(color);
        for (int i = 0; i < d.count; ++i)
        {
            d.points[2 * i] = points[i].x;
            d.points[2 * i + 1] = points[i].y;
        }
        record(list, d);
    }

    void paper(ImDrawList *list, const ImVec2 quad[4], ImU32 tint, float shadow)
    {
        PaperData d;
        for (int i = 0; i < 4; ++i)
        {
            d.quad[2 * i] = quad[i].x;
            d.quad[2 * i + 1] = quad[i].y;
        }
        d.shadow = shadow;
        d.tint = faded(tint);
        record(list, d);
    }
}
