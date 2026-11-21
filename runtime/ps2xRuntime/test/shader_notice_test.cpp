// The PCSX2 GS's "Preparing shaders" notice (runtime/gs/gs_shader_notice.h): when it shows, what it says, its moving
// bar, the log line, and the shader count read from PCSX2's cache index files (fresh, grown, started again, deleted,
// absent). gs_pcsx2_backend_test checks the same count against the real libpcsx2gs.so.
// Exit code 0 = all passed. Plain program, no framework: ctest runs it (KAMITE_BUILD_TESTS=ON).
#include "runtime/gs/gs_shader_notice.h"

#include <cstdio>
#include <string>

namespace
{
    using namespace ps2x::gs;

    int g_failures = 0;
    int g_checks = 0;
    void check(bool ok, const std::string &what)
    {
        ++g_checks;
        if (!ok)
        {
            ++g_failures;
            std::printf("FAIL %s\n", what.c_str());
        }
    }

    bool contains(const std::string &text, const std::string &part) { return text.find(part) != std::string::npos; }

    const ShaderIndexFormat kVulkan = kShaderIndexes[0];
    const std::string kHeadA(36, 'a');
    const std::string kHeadB(36, 'b');

    ShaderIndexSample sample(uint64_t entries, const std::string &head = kHeadA)
    {
        ShaderIndexSample s;
        s.exists = true;
        s.size = kVulkan.headerBytes + entries * kVulkan.entryBytes;
        s.head = head;
        return s;
    }

    void timing()
    {
        ShaderNotice notice;
        check(!notice.started() && !notice.visible(100.0), "not started: never visible");
        check(notice.elapsed(100.0) == 0.0, "not started: no elapsed time");

        notice.start(10.0);
        check(notice.started() && !notice.finished(), "started, not finished");
        check(!notice.visible(10.0), "visible at once: no (a warm cache would flash it)");
        check(!notice.visible(10.0 + ShaderNotice::kShowAfterSeconds - 0.01), "just before the delay: hidden");
        check(notice.visible(10.0 + ShaderNotice::kShowAfterSeconds), "at the delay: shown");
        check(notice.visible(70.0), "a minute in: still shown");
        check(!notice.visible(5.0), "a clock before the start: hidden, no negative time");
        check(notice.elapsed(5.0) == 0.0, "a clock before the start: elapsed is 0");

        notice.finish(73.5);
        check(notice.finished(), "finished");
        check(!notice.visible(73.5) && !notice.visible(1000.0), "finished: never visible again");
        check(notice.elapsed(1000.0) == 63.5, "finished: elapsed stops at the first frame");
        notice.finish(90.0);
        check(notice.elapsed(1000.0) == 63.5, "a second finish changes nothing");

        // Fast: a warm cache, first frame 120 ms after the open. Asked every 16 ms in between, it never shows.
        ShaderNotice fast;
        fast.start(2.0);
        bool shown = false;
        for (double t = 2.0; t < 2.12; t += 0.016)
            shown = shown || fast.visible(t);
        fast.finish(2.12);
        shown = shown || fast.visible(2.12);
        check(!shown, "a 120 ms open never shows the notice");
        check(fast.logLine().find("120 ms") != std::string::npos, "the log line has the 120 ms: " + fast.logLine());

        ShaderNotice soon;
        soon.setShowAfter(0.0);
        soon.start(1.0);
        check(soon.visible(1.0), "a delay of 0 (PS2X_SHADER_NOTICE_DELAY_MS=0): shown at once");

        ShaderNotice never;
        never.finish(5.0);
        check(!never.finished(), "finish without start is ignored");
    }

    void text()
    {
        ShaderNotice notice;
        notice.start(0.0);
        check(std::string(ShaderNotice::title()) == "Preparing shaders for your GPU (first start can take a while)", "the title");
        check(notice.detail(12.7) == "12 s", "no count known: seconds only (" + notice.detail(12.7) + ")");
        notice.setCompiled(0);
        check(notice.detail(3.0) == "3 s", "a count of 0: seconds only (" + notice.detail(3.0) + ")");
        notice.setCompiled(1);
        check(notice.detail(3.0) == "3 s, 1 shader compiled", "one shader: singular (" + notice.detail(3.0) + ")");
        notice.setCompiled(37);
        check(notice.detail(12.2) == "12 s, 37 shaders compiled", "the count (" + notice.detail(12.2) + ")");
        check(notice.detail(0.7) == "37 shaders compiled", "under a second: no \"0 s\" (" + notice.detail(0.7) + ")");
        notice.setCompiled(-1);
        check(notice.detail(0.7).empty(), "under a second, no count: nothing (" + notice.detail(0.7) + ")");
        notice.setCompiled(37);

        notice.finish(1.2344);
        check(notice.logLine() == "[gs] PCSX2 GS: first frame 1234 ms after opening, 37 shaders compiled",
              "log line with the count: " + notice.logLine());
        notice.setCompiled(-1);
        check(notice.logLine() == "[gs] PCSX2 GS: first frame 1234 ms after opening, shader count unavailable",
              "log line without a count: " + notice.logLine());
        notice.setCompiled(1);
        check(contains(notice.logLine(), "1 shader compiled"), "log line, one shader: singular");
    }

    void bar()
    {
        bool sawLeft = false, sawRight = false, moved = false;
        float lastStart = -1.0f;
        for (double t = -3.0; t < 10.0; t += 0.01)
        {
            float start = -1.0f, end = -1.0f;
            ShaderNotice::block(t, start, end);
            if (!(start >= 0.0f && end <= 1.0001f && end > start))
            {
                check(false, "the block stays inside the bar at t=" + std::to_string(t));
                return;
            }
            check(end - start > 0.2f, "the block has a visible width");
            sawLeft = sawLeft || start < 0.02f;
            sawRight = sawRight || end > 0.98f;
            moved = moved || (lastStart >= 0.0f && start != lastStart);
            lastStart = start;
        }
        check(sawLeft && sawRight, "the block reaches both ends");
        check(moved, "the block moves");
        float a0, a1, b0, b1;
        ShaderNotice::block(0.3, a0, a1);
        ShaderNotice::block(0.3 + ShaderNotice::kSweepSeconds, b0, b1);
        check(a0 == b0 && a1 == b1, "the motion repeats every sweep");
    }

    void counter()
    {
        // No cache yet (the first start): the file appears, written with its header and one entry per shader.
        ShaderIndexCounter fresh;
        fresh.begin(kVulkan, ShaderIndexSample{});
        check(!fresh.seen() && fresh.compiled() == 0u, "no file: nothing seen");
        ShaderIndexSample empty;
        empty.exists = true;
        fresh.observe(empty);
        check(fresh.seen() && fresh.compiled() == 0u, "created, still empty: 0");
        fresh.observe(sample(0));
        check(fresh.compiled() == 0u, "header only: 0");
        fresh.observe(sample(25));
        check(fresh.compiled() == 25u, "25 entries: 25");
        ShaderIndexSample partial = sample(25);
        partial.size += 7u; // an entry being written
        fresh.observe(partial);
        check(fresh.compiled() == 25u, "a partly written entry is not counted");

        // A warm cache: only the growth counts.
        ShaderIndexCounter warm;
        warm.begin(kVulkan, sample(900));
        warm.observe(sample(900));
        check(warm.seen() && warm.compiled() == 0u, "warm cache, nothing new: 0");
        warm.observe(sample(912));
        check(warm.compiled() == 12u, "warm cache, 12 new: 12");

        // Another GPU or driver: PCSX2 starts the file again (truncated, then grown with a new header).
        ShaderIndexCounter newGpu;
        newGpu.begin(kVulkan, sample(900));
        ShaderIndexSample truncated;
        truncated.exists = true;
        newGpu.observe(truncated);
        newGpu.observe(sample(40, kHeadB));
        check(newGpu.compiled() == 40u, "restarted (seen shrinking): counted from 0 (" + std::to_string(newGpu.compiled()) + ")");

        // A damaged file started again with the same header (Direct3D 11's is only the version): the shrink tells.
        ShaderIndexCounter sameHead;
        sameHead.begin(kVulkan, sample(900));
        sameHead.observe(truncated);
        sameHead.observe(sample(40));
        check(sameHead.compiled() == 40u, "restarted with the same header (seen shrinking): counted from 0 (" +
                                             std::to_string(sameHead.compiled()) + ")");

        // ... and grown past the old size before the first look: the new header gives it away.
        ShaderIndexCounter missed;
        missed.begin(kVulkan, sample(10));
        missed.observe(sample(60, kHeadB));
        check(missed.compiled() == 60u, "restarted (only the header changed): counted from 0 (" +
                                            std::to_string(missed.compiled()) + ")");
        missed.observe(sample(70, kHeadB));
        check(missed.compiled() == 70u, "and keeps counting after it");

        // Deleted, then written anew.
        ShaderIndexCounter deleted;
        deleted.begin(kVulkan, sample(300));
        deleted.observe(ShaderIndexSample{});
        deleted.observe(sample(5));
        check(deleted.compiled() == 5u, "deleted and made again: counted from 0");

        // The count never goes back (the notice would count down).
        ShaderIndexCounter monotonic;
        monotonic.begin(kVulkan, ShaderIndexSample{});
        monotonic.observe(sample(50));
        ShaderIndexSample restart;
        restart.exists = true;
        monotonic.observe(restart);
        monotonic.observe(sample(3, kHeadB));
        check(monotonic.compiled() == 50u, "never counts down");

        // Direct3D 11's layout.
        ShaderIndexCounter d3d;
        d3d.begin(kShaderIndexes[1], ShaderIndexSample{});
        ShaderIndexSample d3dSample;
        d3dSample.exists = true;
        d3dSample.size = 4u + 64u * 9u;
        d3dSample.head = std::string("\x01\x00\x00\x00", 4);
        d3d.observe(d3dSample);
        check(d3d.compiled() == 9u, "Direct3D 11 index: 9 entries of 64 bytes");
    }

    void set()
    {
        ShaderIndexSample none[kShaderIndexCount];
        ShaderIndexSet absent;
        absent.begin(none);
        absent.observe(none);
        check(absent.compiled() == -1, "no index file at all: count unavailable");

        ShaderIndexSet vulkan;
        vulkan.begin(none);
        ShaderIndexSample later[kShaderIndexCount];
        later[0] = sample(42);
        vulkan.observe(later);
        check(vulkan.compiled() == 42, "the Vulkan index alone: its count");

        ShaderIndexSample warm[kShaderIndexCount];
        warm[0] = sample(100);
        ShaderIndexSet unchanged;
        unchanged.begin(warm);
        unchanged.observe(warm);
        check(unchanged.compiled() == 0, "a warm cache with nothing new: 0, not unavailable");

        ShaderIndexSample both[kShaderIndexCount];
        both[0] = sample(100);
        both[3].exists = true;
        both[3].size = 4u + 64u * 3u;
        both[3].head = std::string("\x01\x00\x00\x00", 4);
        unchanged.observe(both);
        check(unchanged.compiled() == 3, "two APIs' files: the sum of what each added");
    }
}

int main()
{
    timing();
    text();
    bar();
    counter();
    set();
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
