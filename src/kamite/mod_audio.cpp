// A mod's audio as an archive entry's ADX (see mod_audio.h).
#include "kamite/mod_audio.h"
#include "kamite/adx_codec.h"

#include "raylib.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace kamite::mod_audio
{
    namespace
    {
        // Changes when the encoder's output does: the cached files are made again.
        constexpr int kEncoderVersion = 1;
        constexpr uint64_t kMaxSource = 1024ull * 1024ull * 1024ull;

        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.generic_u8string();
            return std::string(s.begin(), s.end());
        }

        uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }

        bool readFile(const fs::path &file, std::vector<uint8_t> &out)
        {
            std::error_code error;
            const uint64_t size = fs::file_size(file, error);
            if (error || size == 0 || size > kMaxSource)
                return false;
            std::ifstream in(file, std::ios::binary);
            out.resize(static_cast<size_t>(size));
            return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(size)));
        }

        // A whole number at the start of text ("123456", spaces allowed around it).
        bool number(const std::string &text, uint64_t &out)
        {
            size_t i = 0;
            while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                ++i;
            if (i == text.size() || !std::isdigit(static_cast<unsigned char>(text[i])))
                return false;
            out = 0;
            for (; i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); ++i)
                out = out * 10u + static_cast<uint64_t>(text[i] - '0');
            return true;
        }

        uint64_t fnv(const std::string &s)
        {
            uint64_t h = 1469598103934665603ull;
            for (const unsigned char c : s)
            {
                h ^= c;
                h *= 1099511628211ull;
            }
            return h;
        }
    }

    bool isAudio(const fs::path &file)
    {
        const std::string ext = lower(utf8(file.extension()));
        return ext == ".adx" || ext == ".wav" || ext == ".ogg" || ext == ".mp3";
    }

    bool oggLoop(const std::vector<uint8_t> &ogg, uint64_t &start, uint64_t &end)
    {
        // The second packet of the first stream (Vorbis' comment header), put together from the pages' segments.
        std::vector<uint8_t> packet;
        int packets = 0;
        uint32_t serial = 0;
        bool first = true;
        for (size_t at = 0; at + 27 <= ogg.size() && packets < 2;)
        {
            if (std::memcmp(ogg.data() + at, "OggS", 4) != 0)
                return false;
            const uint32_t pageSerial = le32(ogg.data() + at + 14);
            const size_t segments = ogg[at + 26];
            if (at + 27 + segments > ogg.size())
                return false;
            size_t data = at + 27 + segments;
            const bool ours = first || pageSerial == serial;
            if (first)
                serial = pageSerial, first = false;
            for (size_t s = 0; s < segments; ++s)
            {
                const size_t length = ogg[at + 27 + s];
                if (data + length > ogg.size())
                    return false;
                if (ours && packets == 1)
                    packet.insert(packet.end(), ogg.begin() + static_cast<std::ptrdiff_t>(data),
                                  ogg.begin() + static_cast<std::ptrdiff_t>(data + length));
                data += length;
                if (ours && length < 255 && ++packets == 2)
                    break;
            }
            at = data; // the next page (when the packet ended mid-page, the loop ends here anyway)
        }
        if (packets < 2 || packet.size() < 7 + 8 || packet[0] != 3 || std::memcmp(packet.data() + 1, "vorbis", 6) != 0)
            return false;
        size_t p = 7;
        const uint32_t vendor = le32(packet.data() + p);
        p += 4 + static_cast<size_t>(vendor);
        if (p + 4 > packet.size())
            return false;
        const uint32_t count = le32(packet.data() + p);
        p += 4;
        bool haveStart = false, haveLength = false, haveEnd = false;
        uint64_t loopStart = 0, loopLength = 0, loopEnd = 0;
        for (uint32_t i = 0; i < count && p + 4 <= packet.size(); ++i)
        {
            const uint32_t length = le32(packet.data() + p);
            p += 4;
            if (p + length > packet.size())
                return false;
            const std::string comment(reinterpret_cast<const char *>(packet.data() + p), length);
            p += length;
            const size_t equals = comment.find('=');
            if (equals == std::string::npos)
                continue;
            const std::string key = lower(comment.substr(0, equals)), value = comment.substr(equals + 1);
            if (key == "loopstart" || key == "loop_start")
                haveStart = number(value, loopStart);
            else if (key == "looplength" || key == "loop_length")
                haveLength = number(value, loopLength);
            else if (key == "loopend" || key == "loop_end")
                haveEnd = number(value, loopEnd);
        }
        if (!haveStart || !(haveLength || haveEnd))
            return false;
        start = loopStart;
        end = haveLength ? loopStart + loopLength : loopEnd;
        return end > start;
    }

    bool wavLoop(const std::vector<uint8_t> &wav, uint64_t &start, uint64_t &end)
    {
        if (wav.size() < 12 || std::memcmp(wav.data(), "RIFF", 4) != 0 || std::memcmp(wav.data() + 8, "WAVE", 4) != 0)
            return false;
        for (size_t at = 12; at + 8 <= wav.size();)
        {
            const uint32_t size = le32(wav.data() + at + 4);
            if (std::memcmp(wav.data() + at, "smpl", 4) == 0 && size >= 36 + 24 && at + 8 + size <= wav.size())
            {
                const uint8_t *chunk = wav.data() + at + 8;
                if (le32(chunk + 28) == 0)
                    return false;
                const uint8_t *loop = chunk + 36;
                start = le32(loop + 8);
                end = static_cast<uint64_t>(le32(loop + 12)) + 1u; // the smpl loop's end is its last sample
                return end > start;
            }
            at += 8 + static_cast<size_t>(size) + (size & 1u);
        }
        return false;
    }

    fs::path adxFor(const fs::path &source, const Target &target, const fs::path &cacheDir, std::string &note, std::string &error)
    {
        note.clear();
        error.clear();
        const std::string ext = lower(utf8(source.extension()));
        if (ext == ".adx")
        {
            std::vector<uint8_t> head(0x40);
            std::ifstream in(source, std::ios::binary);
            in.read(reinterpret_cast<char *>(head.data()), static_cast<std::streamsize>(head.size()));
            const adx::Info info = adx::readInfo(head.data(), static_cast<size_t>(in.gcount()));
            if (!info.valid)
            {
                error = "not an ADX file";
                return {};
            }
            note = "ADX as it is (" + std::to_string(info.channels) + " channel(s), " + std::to_string(info.rate) + " Hz" +
                   (info.loops ? ", loops" : "") + ")";
            if (info.channels != target.channels || info.rate != target.rate)
                note += "; the game's entry is " + std::to_string(target.channels) + " channel(s), " + std::to_string(target.rate) + " Hz";
            return source;
        }
        if (!isAudio(source))
        {
            error = "not an audio file GHR reads (.adx, .wav, .ogg, .mp3)";
            return {};
        }
        std::error_code ec;
        const uint64_t size = fs::file_size(source, ec);
        const auto stamp = ec ? 0 : static_cast<long long>(fs::last_write_time(source, ec).time_since_epoch().count());
        std::ostringstream key;
        key << utf8(fs::absolute(source, ec)) << '|' << size << '|' << stamp << '|' << target.channels << '|' << target.rate << '|'
            << target.loops << '|' << kEncoderVersion;
        char name[40];
        std::snprintf(name, sizeof(name), "%016llx.adx", static_cast<unsigned long long>(fnv(key.str())));
        const fs::path cached = cacheDir / name;
        if (fs::file_size(cached, ec) > 0x28u && !ec)
        {
            note = "from the cache (" + std::string(name) + ")";
            return cached;
        }
        std::vector<uint8_t> bytes;
        if (!readFile(source, bytes))
        {
            error = "can't be read";
            return {};
        }
        uint64_t loopStart = 0, loopEnd = 0;
        const bool tagged = (ext == ".ogg" && oggLoop(bytes, loopStart, loopEnd)) || (ext == ".wav" && wavLoop(bytes, loopStart, loopEnd));
        const auto began = std::chrono::steady_clock::now();
        Wave wave = LoadWaveFromMemory(ext.c_str(), bytes.data(), static_cast<int>(bytes.size()));
        bytes.clear();
        bytes.shrink_to_fit();
        if (wave.data == nullptr || wave.frameCount == 0 || wave.sampleRate == 0)
        {
            UnloadWave(wave);
            error = "can't be decoded";
            return {};
        }
        const uint32_t sourceRate = wave.sampleRate;
        WaveFormat(&wave, static_cast<int>(target.rate), 16, target.channels);
        adx::Loop loop;
        if (tagged)
        {
            const double scale = static_cast<double>(target.rate) / sourceRate;
            loop.on = true;
            loop.start = static_cast<uint32_t>(std::min<double>(std::llround(loopStart * scale), wave.frameCount));
            loop.end = static_cast<uint32_t>(std::min<double>(std::llround(loopEnd * scale), wave.frameCount));
            loop.on = loop.end > loop.start;
        }
        else if (target.loops)
        {
            loop.on = true;
            loop.start = 0;
            loop.end = wave.frameCount;
        }
        const std::vector<uint8_t> adxFile =
            adx::encode(static_cast<const int16_t *>(wave.data), wave.frameCount, target.channels, target.rate, loop);
        const uint32_t frames = wave.frameCount;
        UnloadWave(wave);
        if (adxFile.empty())
        {
            error = "can't be converted";
            return {};
        }
        fs::create_directories(cacheDir, ec);
        const fs::path temporary = cached.string() + ".part";
        {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(adxFile.data()), static_cast<std::streamsize>(adxFile.size()));
            if (!out)
            {
                error = "the converted file can't be written to " + utf8(cacheDir);
                return {};
            }
        }
        fs::rename(temporary, cached, ec);
        if (ec)
        {
            error = "the converted file can't be written to " + utf8(cacheDir);
            return {};
        }
        const long long ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - began).count();
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1f s", static_cast<double>(frames) / target.rate);
        note = "converted (" + std::string(seconds) + ", " + std::to_string(target.channels) + " channel(s), " +
               std::to_string(target.rate) + " Hz, " +
               (tagged ? (loop.on ? "loops from sample " + std::to_string(loop.start) + " to " + std::to_string(loop.end) + " (tags)"
                                  : std::string("loop tags out of range: plays once"))
                       : loop.on ? std::string("the whole track loops, as the game's did")
                                 : std::string("plays once, as the game's did")) +
               ") in " + std::to_string(ms) + " ms";
        return cached;
    }
}
