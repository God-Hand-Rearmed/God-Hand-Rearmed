#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/ps2_cd_image.h"

class PS2Runtime;

// The file layer: any game file in _DATA.AFS, by its disc path, replaced whole by a loose file or by bytes, of any
// size, served at the game's read. The disc image and the game's tables in RAM (the path table, CRI's partition) stay
// as they are, so a save state means what it meant. Each replacement is served past the end of the disc
// (PS2ServedFiles, runtime/ps2_cd_image.h) and three hooks lead the game there:
//   0x324640 ADXF_GetFnameRangeEx (a0 partition, a1 index; out a3 dir, t0 first sector, t1 sectors, t2 bytes): every
//            AFS lookup of CRI's (ADXF_OpenAfs for cDvd's jobs and LoadEntryByValue, ADXF partition loads, the music
//            and voice streams, movies) asks it where a file lies. For partition 0 (_DATA.AFS, loaded by func_001FF858)
//            and a replaced index it answers the served file's LBA, sectors and size. A nested archive replaced whole
//            (bgm/bgm00.afs, se/ev/*.afs) carries that LBA into the partition loaded from it.
//   0x327E88 the ADXSTM bind (a0 stream, a1 file name, a2 dir, a3 first sector, t0 sectors): a range in a served file
//            is bound to DVCI's "DVD-ROM" (0x4545B0, the whole disc from LBA 0, no size limit) instead of _DATA.AFS,
//            whose own size would cut the read.
//   0x1FFD98 the path table's size of a file (a1 its entry: u8 type, u24 size / 16, name, u32 index): the replaced
//            file's size, so cDvd's jobs read all of it and allocate for it.
// With nothing replaced nothing is hooked.
namespace kamite::file_layer
{
    struct Replaced
    {
        uint32_t index = 0;    // archive index
        uint32_t gameSize = 0; // the game's own file
        uint32_t size = 0;     // the replacement
        uint32_t lba = 0;      // where it is served
    };

    // Before the game runs (after PS2Runtime::setIoPaths). path: its disc path ("pl/pl00.dat", any case). False: no
    // such file in the archive, or the replacement can't be read or is empty. A later replacement of a path wins.
    bool replace(std::string_view path, const std::filesystem::path &looseFile);
    bool replace(std::string_view path, std::vector<uint8_t> bytes);
    // A file built from pieces (PS2ServedFiles::Piece: disc ranges, host files, bytes) of `size` bytes: an archive with
    // some entries swapped (afs_archive.h). from names it in the log.
    bool replace(std::string_view path, std::vector<PS2ServedFiles::Piece> pieces, uint64_t size, const std::string &from);
    // Every file under folder whose path relative to it is a game file's (folder/pl/pl00.dat replaces pl/pl00.dat);
    // anything else (character.ini, textures/) is skipped. Returns how many were replaced; who names them in the log.
    int replaceFolder(const std::filesystem::path &folder, const std::string &who);

    // The replaced files by disc path (lower case).
    const std::map<std::string, Replaced> &replaced();
    // The replaced file a guest path names (the game's spelling: "\\PL\\pl00.dat" ...), or nullptr.
    const Replaced *replacedPath(const uint8_t *rdram, uint32_t pathAddress, std::string *pathOut = nullptr);

    // The hooks; after FunctionHooks::attach. A replacement made after this installs them then.
    void install(PS2Runtime &runtime);
}
