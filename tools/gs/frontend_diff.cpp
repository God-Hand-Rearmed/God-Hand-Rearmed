// Differential harness for gs_frontend.cpp (tools/gs/frontend_diff.sh): the same random GIF streams through two versions
// (raw GIF back end and the classic one); every back end call, SIGNAL / FINISH / LABEL effect, trace event, the debug
// snapshot and history are hashed. Usage: frontend_diff <seed> <streams> <raw 0/1> [bench]
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/gs_threaded_backend.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_async_vif.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern std::atomic<uint32_t> g_gsDownloadSerial;
void ps2xGsReleaseStall();
void ps2xGsResetStall();

namespace
{
    bool g_raw = true;
    uint64_t g_hash = 1469598103934665603ull;
    uint64_t g_calls = 0;
    bool g_bench = false;
    void mix(const void *p, size_t n)
    {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i)
            g_hash = (g_hash ^ b[i]) * 1099511628211ull;
    }
    template <typename T> void mixv(T v) { mix(&v, sizeof(v)); }
    void ev(uint32_t code) { ++g_calls; mixv(code); }

    struct FakeBackend : GSRasterBackend
    {
        void Initialize(uint8_t *, uint32_t) override { ev(1); }
        void Reset() override { ev(2); }
        void Submit(const GSPrimitiveBatch &b) override
        {
            ev(3);
            mixv(b.vertexCount);
            for (uint32_t i = 0; i < b.vertexCount; ++i)
            {
                const GSVertex &v = b.vertices[i];
                mixv(v.x); mixv(v.y); mixv(v.z); mixv(v.r); mixv(v.g); mixv(v.b); mixv(v.a);
                mixv(v.q); mixv(v.s); mixv(v.t); mixv(v.u); mixv(v.v); mixv(v.fog);
            }
            mixv(static_cast<uint32_t>(b.state.prim.type)); mixv(b.state.context.frame.fbp); mixv(b.state.context.tex0.tbp0);
        }
        void LoadClut(const GSTex0Reg &t, const GSTexClutReg &) override { ev(4); mixv(t.cbp); }
        void BeginTransfer(const GSTransferCommand &c) override
        {
            ev(5); mixv(c.direction); mixv(c.bitbltbuf.sbp); mixv(c.bitbltbuf.dbp); mixv(c.trxpos.dsax); mixv(c.trxreg.rrw); mixv(c.trxreg.rrh);
        }
        void UploadImage(const uint8_t *d, uint32_t n) override { ev(6); mixv(n); mix(d, n); }
        void Flush() override { if (!g_bench) ev(7); }
        void TextureFlush() override { ev(8); }
        void Sync(GSSyncReason r) override { ev(9); mixv(static_cast<uint8_t>(r)); }
        PresentationFrame Present(const GSPresentationRequest &) override { return {}; }
        bool ClearFramebuffer(const GSContext &, uint32_t) override { return false; }
        uint32_t ConsumeLocalToHostBytes(uint8_t *, uint32_t) override { return 0; }
        uint32_t ReadVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) const override { return 0; }
        void WriteVram(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
        void SnapshotVram(std::vector<uint8_t> &) const override {}
        GSTransferSnapshot GetTransferSnapshot() const override { return {}; }
        bool WantsRawGif() const override { return g_raw; }
        void RawGif(const uint8_t *d, uint32_t n) override
        {
            if (g_bench) return;
            ev(10); mixv(n); mix(d, n & ~15u);
        }
        void VSync(const GSVSyncRegs &) override { ev(11); }
    };
}

std::unique_ptr<GSRasterBackend> GSThreadedBackend::MakeDefault() { return std::make_unique<FakeBackend>(); }

namespace ps2x::asyncvif
{
    void traceEvent(char code, uint32_t arg) noexcept { if (!g_bench) { ev(20); mixv(code); mixv(arg); } }
}

void ps2xGsSignal(GSRegisters &gs, uint32_t id, uint32_t mask)
{
    ev(21); mixv(id); mixv(mask);
    if (gs.csr.load() & 1u)
        return;
    gs.csr.fetch_or(1u);
    uint32_t lo = static_cast<uint32_t>(gs.siglblid);
    lo = (lo & ~mask) | (id & mask);
    gs.siglblid = (gs.siglblid & 0xFFFFFFFF00000000ull) | lo;
}
bool ps2xGsSignalBlocked(const GSRegisters &gs) { return (gs.csr.load() & 1u) != 0u; }
void ps2xGsFinish(GSRegisters &gs) { ev(22); gs.csr.fetch_or(2u); }

namespace
{
    std::mt19937_64 rng;
    uint32_t rnd(uint32_t n) { return static_cast<uint32_t>(rng() % n); }

    const uint8_t kAdRegs[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x0A, 0x0C, 0x0D, 0x14, 0x16, 0x18,
                               0x1A, 0x1B, 0x1C, 0x22, 0x3B, 0x3D, 0x3F, 0x40, 0x42, 0x44, 0x45, 0x46, 0x47, 0x49, 0x4A,
                               0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x59, 0x5A, 0x60, 0x61, 0x62, 0x34};

    void put64(std::vector<uint8_t> &s, uint64_t v) { const size_t o = s.size(); s.resize(o + 8); std::memcpy(&s[o], &v, 8); }

    uint64_t adValue(uint8_t reg)
    {
        uint64_t v = rng();
        if (reg == 0x53)
            v &= 3u;                       // TRXDIR
        if (reg == 0x60 || reg == 0x62)
            v &= 0x00FF00FF00FF00FFull;     // SIGNAL / LABEL id+mask
        return v;
    }

    // One GIF tag with its data. vertexHeavy: the VU1 output shape (ST / RGBAQ / XYZF2 loops).
    void appendTag(std::vector<uint8_t> &s, bool vertexHeavy)
    {
        const uint32_t kind = vertexHeavy ? (rnd(10) < 8 ? 0u : 1u) : rnd(6);
        uint32_t flg = 0, nreg = 1, nloop = rnd(12);
        uint64_t regs = 0;
        bool pre = rnd(4) == 0;
        if (kind == 0) // vertex data
        {
            static const uint64_t shapes[] = {0x412, 0x512, 0x4312, 0x5312, 0x4, 0xC, 0x1D, 0x4A12, 0xF412};
            regs = shapes[rnd(9)];
            nreg = 0;
            for (uint64_t r = regs; r; r >>= 4) ++nreg;
            if (regs == 0x1D) nreg = 2;
            if (vertexHeavy) nloop = 20 + rnd(100);
        }
        else if (kind == 1) // A+D setup
        {
            regs = 0xE;
            nreg = 1;
            nloop = 1 + rnd(8);
        }
        else if (kind == 2) // mixed PACKED (any descriptor)
        {
            nreg = 1 + rnd(16);
            for (uint32_t i = 0; i < nreg; ++i)
                regs |= static_cast<uint64_t>(rnd(16)) << (4 * i);
            if (nreg == 16 && rnd(2)) nreg = 0; // NREG 0 = 16
        }
        else if (kind == 3) // REGLIST
        {
            flg = 1;
            nreg = 1 + rnd(16);
            for (uint32_t i = 0; i < nreg; ++i)
                regs |= static_cast<uint64_t>(rnd(3) ? (rnd(2) ? 1 + rnd(5) : 0xC + rnd(2)) : rnd(16)) << (4 * i);
            if (nreg == 16 && rnd(2)) nreg = 0;
        }
        else if (kind == 4) // IMAGE
        {
            flg = 2;
            nloop = rnd(6);
        }
        else // vertex-only REGLIST
        {
            flg = 1;
            static const uint64_t shapes[] = {0x412, 0x512, 0x4312, 0x5, 0xD1};
            regs = shapes[rnd(5)];
            nreg = 0;
            for (uint64_t r = regs; r; r >>= 4) ++nreg;
            if (regs == 0xD1) nreg = 2;
        }
        if (rnd(40) == 0) flg = 3;
        const uint64_t lo = nloop | (static_cast<uint64_t>(rnd(2)) << 15) | (static_cast<uint64_t>(pre) << 46) |
                            (static_cast<uint64_t>(rnd(0x800)) << 47) | (static_cast<uint64_t>(flg) << 58) |
                            (static_cast<uint64_t>(nreg & 0xF) << 60);
        put64(s, lo);
        put64(s, regs);
        const uint32_t n = nreg == 0 ? 16 : nreg;
        if (flg == 0)
        {
            for (uint32_t l = 0; l < nloop; ++l)
                for (uint32_t r = 0; r < n; ++r)
                {
                    const uint8_t d = static_cast<uint8_t>((regs >> (4 * r)) & 0xF);
                    if (d == 0xE)
                    {
                        uint8_t reg = kAdRegs[rnd(sizeof(kAdRegs))];
                        if (g_bench && reg >= 0x60)
                            reg = 0x42; // no SIGNAL / FINISH / LABEL in the timed stream
                        put64(s, adValue(reg));
                        put64(s, reg | (rng() & ~0xFFull));
                    }
                    else
                    {
                        put64(s, rng());
                        put64(s, rng());
                    }
                }
        }
        else if (flg == 1)
        {
            const uint32_t units = nloop * n;
            for (uint32_t i = 0; i < units; ++i)
                put64(s, rng());
            if (units & 1u)
                put64(s, 0);
        }
        else if (flg == 2)
        {
            for (uint32_t i = 0; i < nloop * 2; ++i)
                put64(s, rng());
        }
    }

    void dumpState(GS &gs, GSRegisters &regs)
    {
        const GSDebugSnapshot sn = gs.getDebugSnapshot();
        for (int i = 0; i < 2; ++i)
        {
            const GSContext &c = sn.ctx[i];
            mixv(c.frame.fbp); mixv(c.frame.fbw); mixv(c.frame.psm); mixv(c.frame.fbmsk); mixv(c.zbuf.zbp); mixv(c.zbuf.psm);
            mixv(c.zbuf.zmask); mixv(c.tex0.tbp0); mixv(c.tex0.tbw); mixv(c.tex0.psm); mixv(c.tex0.tw); mixv(c.tex0.th);
            mixv(c.tex0.cbp); mixv(c.tex0.cld); mixv(c.tex1); mixv(c.clamp); mixv(c.alpha); mixv(c.test); mixv(c.fba);
            mixv(c.scissor.x0); mixv(c.scissor.y1); mixv(c.xyoffset.ofx); mixv(c.xyoffset.ofy); mixv(c.miptbp1); mixv(c.miptbp2);
        }
        mixv(static_cast<uint32_t>(sn.prim.type)); mixv(sn.prim.tme); mixv(sn.prim.ctxt); mixv(sn.texa.ta0); mixv(sn.texa.ta1);
        mixv(sn.texclut.cbw); mixv(sn.texclut.cov); mixv(sn.scanmsk); mixv(sn.dimx); mixv(sn.dthe); mixv(sn.colclamp);
        mixv(sn.bitbltbuf.sbp); mixv(sn.bitbltbuf.dbp); mixv(sn.bitbltbuf.dpsm); mixv(sn.trxpos.dsax); mixv(sn.trxpos.dir);
        mixv(sn.trxreg.rrw); mixv(sn.trxreg.rrh);
        for (const GSDebugHistoryEntry &e : gs.getDebugHistory())
        {
            mixv(e.seq); mixv(e.frameIndex); mixv(static_cast<uint8_t>(e.kind)); mixv(e.reg); mixv(e.regValue); mixv(e.gifSizeBytes);
            mixv(e.gifNloop); mixv(e.gifFlg); mixv(e.gifNreg); mixv(static_cast<uint32_t>(e.prim.type)); mixv(e.frame.fbp);
            mixv(e.tex0.tbp0); mixv(e.test); mixv(e.alpha); mixv(e.vertexCount); mixv(e.bitbltbuf.dbp); mixv(e.trxreg.rrw);
        }
        mixv(regs.csr.load()); mixv(regs.siglblid); mixv(regs.dispfb1); mixv(regs.display1); mixv(regs.bgcolor);
        mixv(g_gsDownloadSerial.load());
    }
}

int main(int argc, char **argv)
{
    if (argc < 4)
        return 2;
    const uint64_t seed = std::strtoull(argv[1], nullptr, 10);
    const int streams = std::atoi(argv[2]);
    g_raw = std::atoi(argv[3]) != 0;
    g_bench = argc > 4;
    static std::vector<uint8_t> vram(4u << 20);
    static GSRegisters regs{};
    GS gs;
    gs.init(vram.data(), static_cast<uint32_t>(vram.size()), &regs);

    if (g_bench)
    {
        // Vertex-heavy stream (VU1 output shape), timed.
        rng.seed(seed);
        std::vector<uint8_t> s;
        while (s.size() < (8u << 20))
            appendTag(s, true);
        std::vector<std::pair<uint32_t, uint32_t>> packets;
        for (uint32_t o = 0; o < s.size();)
        {
            const uint32_t n = std::min<uint32_t>(static_cast<uint32_t>(s.size()) - o, 16u * (64u + rnd(2048)));
            packets.push_back({o, n});
            o += n;
        }
        uint64_t qw = 0;
        const auto t0 = std::chrono::steady_clock::now();
        for (int rep = 0; rep < streams; ++rep)
        {
            regs.csr.store(0);
            for (const auto &p : packets)
            {
                gs.processGIFPacket(s.data() + p.first, p.second);
                regs.csr.store(0);
                qw += p.second / 16u;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("bench raw=%d: %.1f ms for %llu qwords (%.2f ns/qword)\n", g_raw ? 1 : 0, ms,
                    static_cast<unsigned long long>(qw), ms * 1e6 / static_cast<double>(qw));
        return 0;
    }

    for (int st = 0; st < streams; ++st)
    {
        rng.seed(seed * 1000003ull + static_cast<uint64_t>(st));
        std::vector<uint8_t> s;
        const uint32_t tags = 1 + rnd(40);
        const bool heavy = rnd(3) == 0;
        for (uint32_t t = 0; t < tags; ++t)
            appendTag(s, heavy && rnd(4) != 0);
        // Cut into packets at 16-byte (sometimes 8-byte) boundaries.
        uint32_t o = 0;
        while (o < s.size())
        {
            uint32_t n;
            const uint32_t kind = rnd(5);
            if (kind == 0)
                n = static_cast<uint32_t>(s.size()) - o;
            else
                n = std::min<uint32_t>(static_cast<uint32_t>(s.size()) - o, 16u * (1u + rnd(24)) + (rnd(8) == 0 ? 8u : 0u));
            const uint32_t how = rnd(12);
            if (how == 0)
                gs.processNativePackedGIFPacket(s.data() + o, n);
            else if (how == 1)
            {
                const uint8_t reg = kAdRegs[rnd(sizeof(kAdRegs))];
                gs.writeRegister(reg, adValue(reg));
                gs.processGIFPacket(s.data() + o, n);
            }
            else if (how == 2)
            {
                std::vector<uint8_t> img(16u * (1 + rnd(8)));
                for (auto &b : img) b = static_cast<uint8_t>(rng());
                gs.uploadImageNative(rng(), rng(), rng() & 0x00000FFF00000FFFull, rnd(4), img.data(), static_cast<uint32_t>(img.size()));
                gs.processGIFPacket(s.data() + o, n);
            }
            else
                gs.processGIFPacket(s.data() + o, n);
            o += n;
            // The EE acknowledges a SIGNAL now and then (and releases the stalled GIF data).
            if (rnd(3) == 0 && (regs.csr.load() & 1u))
            {
                regs.csr.fetch_and(~1ull);
                ps2xGsReleaseStall();
            }
            if (rnd(6) == 0)
                regs.csr.fetch_and(~2ull);
        }
        regs.csr.store(0);
        ps2xGsReleaseStall();
        dumpState(gs, regs);
        if (rnd(10) == 0)
        {
            ps2xGsResetStall();
            gs.reset();
        }
    }
    dumpState(gs, regs);
    std::printf("raw=%d calls=%llu hash=%016llx\n", g_raw ? 1 : 0, static_cast<unsigned long long>(g_calls),
                static_cast<unsigned long long>(g_hash));
    return 0;
}
