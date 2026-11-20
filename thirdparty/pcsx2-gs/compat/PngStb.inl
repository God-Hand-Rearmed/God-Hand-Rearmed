// PNG loading for texture replacement with stb_image instead of libpng, on Linux (included by the generated
// compat/GSTextureReplacementLoaders.cpp in place of PCSX2's libpng handlers, as compat/PngWic.inl is on Windows;
// tools/make_loaders.py). Same output as PCSX2's PNGLoader and PngWic.inl: 32-bit RGBA rows; images without alpha get
// A = 0x80 (opaque in GS terms), images with alpha keep it as stored. PCSX2 is GPL-3; so is this file.
// stb_image.h comes from the build (thirdparty/pcsx2-gs/CMakeLists.txt downloads it).

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool PNGLoader(GSTextureReplacements::File& file, const char* filename, GSTextureReplacements::ReplacementTexture* tex, bool only_base_image)
{
    const s64 size = file.Size();
    if (size <= 0 || size > 0x7FFFFFFF || !file.Seek(0))
        return false;
    std::vector<u8> bytes(static_cast<size_t>(size));
    if (!file.Read(bytes.data(), bytes.size()))
        return false;

    int width = 0, height = 0, channels = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels || width <= 0 || height <= 0)
    {
        Console.Error("Failed to decode replacement texture %s (%s)", filename, stbi_failure_reason());
        stbi_image_free(pixels);
        return false;
    }

    const u32 pitch = static_cast<u32>(width) * sizeof(u32);
    tex->width = static_cast<u32>(width);
    tex->height = static_cast<u32>(height);
    tex->format = GSTexture::Format::Color;
    tex->pitch = pitch;
    tex->data.assign(pixels, pixels + static_cast<size_t>(pitch) * static_cast<size_t>(height));
    stbi_image_free(pixels);

    // An alpha channel (grey + alpha, RGBA) keeps its values. stb reports a palette or colour key transparency (tRNS)
    // as 3 channels, so an image whose alpha is not all 255 had transparency too and keeps it, as with WIC and libpng.
    u8* pixel = tex->data.data();
    const size_t count = static_cast<size_t>(width) * static_cast<size_t>(height);
    bool hasAlpha = channels == 2 || channels == 4;
    for (size_t i = 0; !hasAlpha && i < count; ++i)
        hasAlpha = pixel[i * 4 + 3] != 0xFF;
    if (!hasAlpha)
    {
        for (size_t i = 0; i < count; ++i)
            pixel[i * 4 + 3] = 0x80;
    }
    return true;
}

// Texture dumping is not used by the port.
bool GSTextureReplacements::SavePNGImage(const std::string& filename, u32 width, u32 height, const u8* buffer, u32 pitch)
{
    return false;
}
