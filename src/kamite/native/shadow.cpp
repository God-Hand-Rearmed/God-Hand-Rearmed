// The shadow harness: shadow.h says what it does, claude/native-next.md "Rung 2 as built" why. Dev builds only.

#include "kamite/native/shadow.h"

#if !defined(KAMITE_RELEASE)

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#endif

namespace kamite::native::shadow
{
    namespace
    {
        constexpr uint32_t kMaxBytes = 64u * 1024u; // a call's footprint, all ranges
        constexpr int kMaxDepth = 8;                // shadowed calls inside the original of a shadowed call

        struct Slot
        {
            NativeFunction fn;
            GuestFunction held = nullptr;
            Stats stats;
            std::string details; // the saved mismatches' differing fields, for the summary
        };

        // One shadowed call's buffers (a nested shadowed call gets the next one).
        struct Frame
        {
            R5900Context before;
            R5900Context native;
            Footprint fp;
            uint32_t offsets[Footprint::kMaxRanges];
            uint32_t total = 0;
            std::vector<uint8_t> in, nativeBytes;
            uint64_t charged = 0;
            int checkpoints = 0;
        };

        Settings g_settings;
        std::vector<Slot> g_slots; // sorted by address; fixed once the game runs
        uint8_t *g_scratch = nullptr;
        Frame *g_frames = nullptr;
        int g_depth = 0;
        std::FILE *g_bin = nullptr;
        std::string g_output; // set at the first checked call
        bool g_binOpen = false;
        std::mutex g_writeMutex;
        uint64_t g_sinceWrite = 0;
        std::chrono::steady_clock::time_point g_lastWrite;

        uint8_t *allocateScratch()
        {
            // The runtime's whole backing, so a native addresses it exactly as the real RAM; pages are touched only
            // where footprints land.
#if defined(_WIN32)
            return static_cast<uint8_t *>(VirtualAlloc(nullptr, PS2_RDRAM_SIZE, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
#else
            void *p = mmap(nullptr, PS2_RDRAM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            return p == MAP_FAILED ? nullptr : static_cast<uint8_t *>(p);
#endif
        }

        Slot *find(uint32_t address)
        {
            const auto it = std::lower_bound(g_slots.begin(), g_slots.end(), address,
                                             [](const Slot &slot, uint32_t value) { return slot.fn.address < value; });
            return it != g_slots.end() && it->fn.address == address ? &*it : nullptr;
        }

        // ---- comparison ------------------------------------------------------------------------------------------
        enum : uint32_t
        {
            kCtx = 1u,
            kRam = 2u,
            kCycles = 4u,
        };

        uint32_t ulpDistance(uint32_t a, uint32_t b)
        {
            auto key = [](uint32_t x) -> int64_t {
                return (x & 0x80000000u) ? -static_cast<int64_t>(x & 0x7FFFFFFFu) : static_cast<int64_t>(x);
            };
            const int64_t d = key(a) - key(b);
            return static_cast<uint32_t>(std::min<int64_t>(d < 0 ? -d : d, 0xFFFFFFFF));
        }

        // The context's float words (where a loose function may differ by its ulps).
        bool floatWord(size_t offset)
        {
            auto in = [offset](size_t begin, size_t size) { return offset >= begin && offset < begin + size; };
            return in(offsetof(R5900Context, vu0_vf), sizeof(R5900Context::vu0_vf)) ||
                   in(offsetof(R5900Context, vu0_q), 12) || // q, p, i
                   in(offsetof(R5900Context, vu0_r), 16) || in(offsetof(R5900Context, vu0_acc), 16) ||
                   in(offsetof(R5900Context, vu0_cf), 16) || in(offsetof(R5900Context, f), sizeof(R5900Context::f)) ||
                   in(offsetof(R5900Context, f_acc), 4);
        }

        bool ignored(const NativeFunction &fn, size_t offset)
        {
            for (int i = 0; i < fn.dontCareCount; ++i)
                if (offset - fn.dontCare[i].offset < fn.dontCare[i].size)
                    return true;
            return false;
        }

        // Equal bytes, or (ulps > 0, float words) within ulps; worst = the largest float difference seen.
        bool sameWords(const uint8_t *a, const uint8_t *b, size_t size, uint32_t ulps, uint32_t &worst,
                       bool (*isFloat)(size_t), const NativeFunction *fn)
        {
            if (ulps == 0u && !fn)
                return std::memcmp(a, b, size) == 0;
            bool same = true;
            for (size_t at = 0; at < size; at += 4)
            {
                const size_t n = std::min<size_t>(4, size - at);
                if (std::memcmp(a + at, b + at, n) == 0)
                    continue;
                if (fn && ignored(*fn, at))
                    continue;
                if (ulps != 0u && n == 4 && (!isFloat || isFloat(at)))
                {
                    uint32_t x, y;
                    std::memcpy(&x, a + at, 4);
                    std::memcpy(&y, b + at, 4);
                    const uint32_t d = ulpDistance(x, y);
                    if (d <= ulps)
                    {
                        worst = std::max(worst, d);
                        continue;
                    }
                }
                same = false;
            }
            return same;
        }

        uint32_t compare(Slot &slot, Frame &f, const R5900Context *real, const uint8_t *rdram, uint64_t realCycles)
        {
            const NativeFunction &fn = slot.fn;
            uint32_t worst = 0, kinds = 0;
            const bool plain = fn.ulps == 0u && fn.dontCareCount == 0;
            if (!sameWords(reinterpret_cast<const uint8_t *>(&f.native), reinterpret_cast<const uint8_t *>(real),
                           sizeof(R5900Context), fn.ulps, worst, &floatWord, plain ? nullptr : &fn))
                kinds |= kCtx;
            for (int i = 0, at = 0; i < f.fp.count; at += static_cast<int>(f.fp.ranges[i].size), ++i)
                if (!sameWords(f.nativeBytes.data() + at, rdram + f.offsets[i], f.fp.ranges[i].size, fn.ulps, worst,
                               nullptr, nullptr))
                    kinds |= kRam;
            if (f.charged != realCycles)
                kinds |= kCycles;
            if (kinds == 0u)
                slot.stats.worstUlps = std::max(slot.stats.worstUlps, worst);
            return kinds;
        }

        // ---- the native's run on the scratch copy ----------------------------------------------------------------
        bool runNative(Slot &slot, Frame &f, int dueAt)
        {
            for (int i = 0, at = 0; i < f.fp.count; at += static_cast<int>(f.fp.ranges[i].size), ++i)
                std::memcpy(g_scratch + f.offsets[i], f.in.data() + at, f.fp.ranges[i].size);
            std::memcpy(static_cast<void *>(&f.native), &f.before, sizeof(R5900Context));
            ShadowEnv env{g_scratch};
            env.dueAt = dueAt;
            env.loopCycles = g_settings.loopCycles;
            const bool ran = slot.fn.runShadow(env, &f.native);
            f.charged = env.charged;
            f.checkpoints = env.checkpoints;
            for (int i = 0, at = 0; i < f.fp.count; at += static_cast<int>(f.fp.ranges[i].size), ++i)
                std::memcpy(f.nativeBytes.data() + at, g_scratch + f.offsets[i], f.fp.ranges[i].size);
            for (int i = 0; i < f.fp.count; ++i) // after all of them (ranges may overlap): a later read outside sees 0s
                std::memset(g_scratch + f.offsets[i], 0, f.fp.ranges[i].size);
            return ran;
        }

        // ---- reports ---------------------------------------------------------------------------------------------
        struct Field
        {
            const char *name;
            size_t offset, size;
        };
        std::vector<Field> contextFields()
        {
            std::vector<Field> fields;
            static const char *const gpr[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                                "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                                "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
            for (int i = 0; i < 32; ++i)
                fields.push_back({gpr[i], offsetof(R5900Context, r) + 16u * i, 16});
            static const char *const vf[32] = {"vf0",  "vf1",  "vf2",  "vf3",  "vf4",  "vf5",  "vf6",  "vf7",
                                               "vf8",  "vf9",  "vf10", "vf11", "vf12", "vf13", "vf14", "vf15",
                                               "vf16", "vf17", "vf18", "vf19", "vf20", "vf21", "vf22", "vf23",
                                               "vf24", "vf25", "vf26", "vf27", "vf28", "vf29", "vf30", "vf31"};
            for (int i = 0; i < 32; ++i)
                fields.push_back({vf[i], offsetof(R5900Context, vu0_vf) + 16u * i, 16});
            static const char *const fr[32] = {"f0",  "f1",  "f2",  "f3",  "f4",  "f5",  "f6",  "f7",
                                               "f8",  "f9",  "f10", "f11", "f12", "f13", "f14", "f15",
                                               "f16", "f17", "f18", "f19", "f20", "f21", "f22", "f23",
                                               "f24", "f25", "f26", "f27", "f28", "f29", "f30", "f31"};
            for (int i = 0; i < 32; ++i)
                fields.push_back({fr[i], offsetof(R5900Context, f) + 4u * i, 4});
#define KM_FIELD(member) fields.push_back({#member, offsetof(R5900Context, member), sizeof(R5900Context::member)})
            KM_FIELD(pc);
            KM_FIELD(hi);
            KM_FIELD(lo);
            KM_FIELD(hi1);
            KM_FIELD(lo1);
            KM_FIELD(sa);
            KM_FIELD(vi);
            KM_FIELD(vu0_q);
            KM_FIELD(vu0_p);
            KM_FIELD(vu0_i);
            KM_FIELD(vu0_r);
            KM_FIELD(vu0_acc);
            KM_FIELD(vu0_status);
            KM_FIELD(vu0_mac_flags);
            KM_FIELD(vu0_clip_flags);
            KM_FIELD(f_acc);
            KM_FIELD(fcr31);
#undef KM_FIELD
            return fields;
        }

        void appendHex(std::string &out, const uint8_t *bytes, size_t size)
        {
            char text[12];
            for (size_t at = 0; at < size; at += 4)
            {
                uint32_t word = 0;
                std::memcpy(&word, bytes + at, std::min<size_t>(4, size - at));
                std::snprintf(text, sizeof(text), " %08X", word);
                out += text;
            }
        }

        std::string describe(const Slot &slot, const Frame &f, const R5900Context *real, const uint8_t *rdram,
                             uint64_t realCycles, uint32_t kinds)
        {
            static const std::vector<Field> fields = contextFields();
            std::string out;
            char line[160];
            std::snprintf(line, sizeof(line), "  call %llu:%s%s%s\n", static_cast<unsigned long long>(slot.stats.calls),
                          (kinds & kCtx) ? " context" : "", (kinds & kRam) ? " ram" : "", (kinds & kCycles) ? " cycles" : "");
            out += line;
            const auto *n = reinterpret_cast<const uint8_t *>(&f.native);
            const auto *r = reinterpret_cast<const uint8_t *>(real);
            std::vector<bool> named(sizeof(R5900Context), false);
            for (const Field &field : fields)
            {
                std::fill(named.begin() + field.offset, named.begin() + field.offset + field.size, true);
                if (std::memcmp(n + field.offset, r + field.offset, field.size) == 0)
                    continue;
                out += "    ";
                out += field.name;
                out += " native";
                appendHex(out, n + field.offset, field.size);
                out += " / real";
                appendHex(out, r + field.offset, field.size);
                out += '\n';
            }
            for (size_t at = 0; at < sizeof(R5900Context); ++at)
                if (!named[at] && n[at] != r[at])
                {
                    std::snprintf(line, sizeof(line), "    context +0x%zX native %02X / real %02X\n", at, n[at], r[at]);
                    out += line;
                }
            for (int i = 0, at = 0; i < f.fp.count; at += static_cast<int>(f.fp.ranges[i].size), ++i)
                for (uint32_t w = 0; w < f.fp.ranges[i].size; w += 16)
                {
                    const uint32_t size = std::min<uint32_t>(16, f.fp.ranges[i].size - w);
                    if (std::memcmp(f.nativeBytes.data() + at + w, rdram + f.offsets[i] + w, size) == 0)
                        continue;
                    std::snprintf(line, sizeof(line), "    ram %08X native", f.fp.ranges[i].address + w);
                    out += line;
                    appendHex(out, f.nativeBytes.data() + at + w, size);
                    out += " / real";
                    appendHex(out, rdram + f.offsets[i] + w, size);
                    out += '\n';
                }
            if (kinds & kCycles)
            {
                std::snprintf(line, sizeof(line), "    cycles native %llu / real %llu\n",
                              static_cast<unsigned long long>(f.charged), static_cast<unsigned long long>(realCycles));
                out += line;
            }
            return out;
        }

        void put(std::FILE *file, const void *data, size_t size) { std::fwrite(data, 1, size, file); }

        // One record (tools/native/shadow_test rerun reads them): "GHRSHDW1", the sizes, the function, the call, then
        // the input (context + footprint bytes) and both results.
        void save(const Slot &slot, const Frame &f, const R5900Context *real, const uint8_t *rdram, uint64_t realCycles,
                  uint32_t kinds)
        {
            if (!g_binOpen)
            {
                g_binOpen = true;
                g_bin = std::fopen((g_output + ".native.bin").c_str(), "wb");
            }
            if (!g_bin)
                return;
            const uint32_t ctxSize = sizeof(R5900Context), count = static_cast<uint32_t>(f.fp.count);
            char name[32] = {};
            std::snprintf(name, sizeof(name), "%s", slot.fn.name);
            put(g_bin, "GHRSHDW1", 8);
            put(g_bin, &ctxSize, 4);
            put(g_bin, &slot.fn.address, 4);
            put(g_bin, name, sizeof(name));
            put(g_bin, &slot.stats.calls, 8);
            put(g_bin, &kinds, 4);
            put(g_bin, &f.charged, 8);
            put(g_bin, &realCycles, 8);
            put(g_bin, &count, 4);
            for (int i = 0; i < f.fp.count; ++i)
            {
                put(g_bin, &f.fp.ranges[i].address, 4);
                put(g_bin, &f.fp.ranges[i].size, 4);
            }
            put(g_bin, &f.before, ctxSize);
            put(g_bin, &f.native, ctxSize);
            put(g_bin, real, ctxSize);
            put(g_bin, f.in.data(), f.total);
            put(g_bin, f.nativeBytes.data(), f.total);
            for (int i = 0; i < f.fp.count; ++i)
                put(g_bin, rdram + f.offsets[i], f.fp.ranges[i].size);
            std::fflush(g_bin);
        }

        void maybeWriteSummary()
        {
            if (++g_sinceWrite < 4096u)
                return;
            g_sinceWrite = 0;
            const auto now = std::chrono::steady_clock::now();
            if (now - g_lastWrite >= std::chrono::seconds(3))
                writeSummary();
        }

        void check(Slot &slot, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            Stats &stats = slot.stats;
            ++stats.calls;
            if (g_output.empty())
            {
                g_output = g_settings.output ? g_settings.output() : std::string("native_shadow");
                std::error_code ec;
                std::filesystem::create_directories(std::filesystem::path(g_output).parent_path(), ec);
                std::filesystem::remove(g_output + ".native.bin", ec); // a previous run's
            }
            if (g_depth >= kMaxDepth || (slot.fn.every > 1u && stats.calls % slot.fn.every != 0u))
            {
                if (g_depth >= kMaxDepth)
                    ++stats.skipped;
                slot.held(rdram, ctx, runtime);
                return;
            }
            Frame &f = g_frames[g_depth];
            f.fp = Footprint{};
            slot.fn.footprint(ctx, f.fp);
            bool shadowable = !f.fp.overflow;
            f.total = 0;
            for (int i = 0; shadowable && i < f.fp.count; ++i)
            {
                shadowable = plainRam(f.fp.ranges[i].address, f.fp.ranges[i].size, f.offsets[i]);
                f.total += f.fp.ranges[i].size;
            }
            if (!shadowable || f.total > kMaxBytes)
            {
                ++stats.skipped;
                slot.held(rdram, ctx, runtime);
                return;
            }
            f.in.resize(f.total);
            f.nativeBytes.resize(f.total);
            for (int i = 0, at = 0; i < f.fp.count; at += static_cast<int>(f.fp.ranges[i].size), ++i)
                std::memcpy(f.in.data() + at, rdram + f.offsets[i], f.fp.ranges[i].size);
            std::memcpy(static_cast<void *>(&f.before), ctx, sizeof(R5900Context));

            if (!runNative(slot, f, 0))
            {
                ++stats.declined;
                slot.held(rdram, ctx, runtime);
                return;
            }
            const int checkpoints = f.checkpoints;
            const uint64_t cyclesBefore = g_settings.cycles ? g_settings.cycles() : 0u;
            ++g_depth;
            slot.held(rdram, ctx, runtime); // the real call
            --g_depth;
            const uint64_t realCycles = (g_settings.cycles ? g_settings.cycles() : 0u) - cyclesBefore;
            ++stats.checked;

            uint32_t kinds = compare(slot, f, ctx, rdram, realCycles);
            // The original stopped at a checkpoint (it left a resume pc): find the native's checkpoint that matches.
            if (kinds != 0u && ctx->pc != f.native.pc)
                for (int due = 1; due <= checkpoints; ++due)
                {
                    runNative(slot, f, due);
                    if (compare(slot, f, ctx, rdram, realCycles) == 0u)
                    {
                        kinds = 0u;
                        ++stats.earlyExits;
                        break;
                    }
                }
            if (kinds == 0u)
                ++stats.matched;
            else
            {
                if (ctx->pc != f.native.pc && checkpoints > 0)
                    runNative(slot, f, 0); // report against the full run
                ++stats.mismatches;
                stats.mismatchCtx += (kinds & kCtx) ? 1u : 0u;
                stats.mismatchRam += (kinds & kRam) ? 1u : 0u;
                stats.mismatchCycles += (kinds & kCycles) ? 1u : 0u;
                if (stats.saved < g_settings.savedPerFunction)
                {
                    ++stats.saved;
                    save(slot, f, ctx, rdram, realCycles, kinds);
                    slot.details += describe(slot, f, ctx, rdram, realCycles, kinds);
                    writeSummary();
                }
            }
            maybeWriteSummary();
        }
    }

    void begin(const Settings &settings)
    {
        g_settings = settings;
        g_scratch = allocateScratch();
        g_frames = new Frame[kMaxDepth];
        g_lastWrite = std::chrono::steady_clock::now();
    }

    void add(const NativeFunction &function, GuestFunction held)
    {
        if (!g_scratch)
            return;
        Slot slot;
        slot.fn = function;
        slot.held = held;
        g_slots.push_back(slot);
        std::sort(g_slots.begin(), g_slots.end(), [](const Slot &a, const Slot &b) { return a.fn.address < b.fn.address; });
    }

    void entry(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The dispatcher stores the called address in ctx->pc before it invokes the slot.
        Slot *slot = find(ctx->pc);
        if (!slot)
        {
            std::fprintf(stderr, "[native-shadow] entered with unknown pc 0x%08X\n", ctx->pc);
            runtime->requestStop();
            return;
        }
        check(*slot, rdram, ctx, runtime);
    }

    void writeSummary()
    {
        std::lock_guard<std::mutex> lock(g_writeMutex);
        g_lastWrite = std::chrono::steady_clock::now();
        if (g_slots.empty() || g_output.empty())
            return;
        const std::string path = g_output + ".native.txt", temp = path + ".tmp";
        std::FILE *f = std::fopen(temp.c_str(), "wb");
        if (!f)
            return;
        std::fprintf(f, "# GHR native shadow v1 %s\n", g_settings.header ? g_settings.header().c_str() : "");
        std::fprintf(f, "# function address calls checked matched declined skipped early_exits mismatches (context ram "
                        "cycles) saved worst_ulps\n");
        for (const Slot &slot : g_slots)
        {
            const Stats &s = slot.stats;
            std::fprintf(f, "%s 0x%08X %llu %llu %llu %llu %llu %llu %llu (%llu %llu %llu) %u %u\n", slot.fn.name,
                         slot.fn.address, static_cast<unsigned long long>(s.calls),
                         static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(s.matched),
                         static_cast<unsigned long long>(s.declined), static_cast<unsigned long long>(s.skipped),
                         static_cast<unsigned long long>(s.earlyExits), static_cast<unsigned long long>(s.mismatches),
                         static_cast<unsigned long long>(s.mismatchCtx), static_cast<unsigned long long>(s.mismatchRam),
                         static_cast<unsigned long long>(s.mismatchCycles), s.saved, s.worstUlps);
        }
        for (const Slot &slot : g_slots)
        {
            for (int i = 0; i < slot.fn.dontCareCount; ++i)
                std::fprintf(f, "# %s ignores context +0x%X (%u bytes): %s\n", slot.fn.name, slot.fn.dontCare[i].offset,
                             slot.fn.dontCare[i].size, slot.fn.dontCare[i].reason);
            if (!slot.details.empty())
                std::fprintf(f, "\n%s 0x%08X mismatches:\n%s", slot.fn.name, slot.fn.address, slot.details.c_str());
        }
        std::fclose(f);
        std::error_code ec;
        std::filesystem::rename(temp, path, ec);
        if (ec)
        {
            std::filesystem::remove(path, ec);
            std::filesystem::rename(temp, path, ec);
        }
    }

    const Stats *stats(uint32_t address)
    {
        const Slot *slot = find(address);
        return slot ? &slot->stats : nullptr;
    }
}

#else // KAMITE_RELEASE: no shadow harness

namespace kamite::native::shadow
{
    void begin(const Settings &) {}
    void add(const NativeFunction &, GuestFunction) {}
    void entry(uint8_t *, R5900Context *, PS2Runtime *) {}
    void writeSummary() {}
    const Stats *stats(uint32_t) { return nullptr; }
}

#endif
