#pragma once

#include "raylib.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sotc_texture_mods
{
    namespace fs = std::filesystem;
    constexpr uint32_t MaxDimension = 4096;
    constexpr uintmax_t MaxPngBytes = 64u * 1024u * 1024u;
    inline constexpr uint64_t DumpBudget = 512u * 1024u * 1024u;

    struct Texture
    {
        uint32_t width{}, height{}, levels{};
        std::vector<uint8_t> pixels;
    };

    inline fs::path directory(const char *name)
    {
        const char *value = std::getenv(name);
        if (!value || !*value)
            return {};
        const std::string_view utf8(value);
        return fs::path(std::u8string(utf8.begin(), utf8.end()));
    }

    inline const fs::path &replacements()
    {
        static const fs::path path = directory("SOTC_TEXTURE_MOD_DIR");
        return path;
    }

    inline const fs::path &dumps()
    {
        static const fs::path path = directory("SOTC_TEXTURE_EXPORT_DIR");
        return path;
    }

    inline fs::path texturePath(const fs::path &folder, uint64_t key)
    {
        char filename[21];
        std::snprintf(filename, sizeof(filename), "%016llx.png", static_cast<unsigned long long>(key));
        return folder / filename;
    }

    inline bool pngSize(const uint8_t *header, uint32_t &width, uint32_t &height)
    {
        constexpr uint8_t prefix[] = {137, 80, 78, 71, 13, 10, 26, 10, 0, 0, 0, 13, 'I', 'H', 'D', 'R'};
        if (!std::equal(std::begin(prefix), std::end(prefix), header))
            return false;
        auto be32 = [](const uint8_t *p) {
            return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
        };
        width = be32(header + 16);
        height = be32(header + 20);
        return width && height && width <= MaxDimension && height <= MaxDimension;
    }

    inline void appendMips(Texture &texture)
    {
        uint32_t w = texture.width, h = texture.height;
        size_t offset = 0;
        while (w > 1 || h > 1)
        {
            const uint32_t nextW = std::max(1u, w / 2u), nextH = std::max(1u, h / 2u);
            const size_t next = texture.pixels.size();
            texture.pixels.resize(next + size_t(nextW) * nextH * 4u);
            const uint8_t *source = texture.pixels.data() + offset;
            uint8_t *target = texture.pixels.data() + next;
            for (uint32_t y = 0; y < nextH; ++y)
                for (uint32_t x = 0; x < nextW; ++x)
                {
                    uint32_t rgba[4] = {};
                    uint32_t count = 0;
                    for (uint32_t sy = y * h / nextH; sy < (y + 1u) * h / nextH; ++sy)
                        for (uint32_t sx = x * w / nextW; sx < (x + 1u) * w / nextW; ++sx)
                        {
                            const uint8_t *pixel = source + (size_t(sy) * w + sx) * 4u;
                            for (uint32_t channel = 0; channel < 3; ++channel)
                                rgba[channel] += uint32_t(pixel[channel]) * pixel[3];
                            rgba[3] += pixel[3];
                            ++count;
                        }
                    for (uint32_t channel = 0; channel < 3; ++channel)
                        target[channel] = rgba[3] ? static_cast<uint8_t>((rgba[channel] + rgba[3] / 2u) / rgba[3]) : 0;
                    target[3] = static_cast<uint8_t>((rgba[3] + count / 2u) / count);
                    target += 4;
                }
            offset = next;
            w = nextW;
            h = nextH;
            ++texture.levels;
        }
    }

    inline bool load(const fs::path &path, uint32_t originalWidth, uint32_t originalHeight,
                     bool mipmaps, Texture &result)
    {
        try
        {
            std::error_code ec;
            if (!fs::is_regular_file(path, ec))
                return false;
            const auto bytes = fs::file_size(path, ec);
            if (ec || bytes < 33 || bytes > MaxPngBytes)
                return false;
            std::ifstream stream(path, std::ios::binary);
            uint8_t header[24];
            uint32_t width = 0, height = 0;
            if (!stream.read(reinterpret_cast<char *>(header), sizeof(header)) || !pngSize(header, width, height) ||
                uint64_t(width) * originalHeight != uint64_t(height) * originalWidth)
                return false;
            std::vector<unsigned char> encoded(static_cast<size_t>(bytes));
            std::copy(std::begin(header), std::end(header), encoded.begin());
            if (!stream.read(reinterpret_cast<char *>(encoded.data() + sizeof(header)), encoded.size() - sizeof(header)))
                return false;
            Image image = LoadImageFromMemory(".png", encoded.data(), static_cast<int>(encoded.size()));
            struct Unload
            {
                Image &image;
                ~Unload() { if (image.data) UnloadImage(image); }
            } release{image};
            if (!image.data || image.width != static_cast<int>(width) || image.height != static_cast<int>(height))
                return false;
            ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            if (!image.data || image.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8A8)
                return false;
            const auto *pixels = static_cast<const uint8_t *>(image.data);
            Texture decoded{width, height, 1, {pixels, pixels + size_t(width) * height * 4u}};
            // Native shaders use the PS2 alpha range, with 0x80 fully opaque.
            for (size_t i = 3; i < decoded.pixels.size(); i += 4u)
                decoded.pixels[i] = static_cast<uint8_t>((uint32_t(decoded.pixels[i]) + 1u) / 2u);
            if (mipmaps)
                appendMips(decoded);
            result = std::move(decoded);
            return true;
        }
        catch (const std::bad_alloc &)
        {
            return false;
        }
    }

    inline void dump(const fs::path &folder, uint64_t key, uint32_t width, uint32_t height, const uint8_t *pixels)
    {
        static std::mutex mutex;
        static uint64_t written = 0;
        std::lock_guard lock(mutex);
        try
        {
            if (written >= DumpBudget)
                return;
            std::error_code ec;
            const fs::path path = texturePath(folder, key);
            if (fs::exists(path, ec) || ec)
                return;
            std::vector<uint8_t> standard(pixels, pixels + size_t(width) * height * 4u);
            for (size_t i = 3; i < standard.size(); i += 4u)
                standard[i] = static_cast<uint8_t>(std::min(255u, 2u * uint32_t(standard[i])));
            Image image{standard.data(), static_cast<int>(width), static_cast<int>(height), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            int size = 0;
            std::unique_ptr<unsigned char, decltype(&MemFree)> encoded(ExportImageToMemory(image, ".png", &size), MemFree);
            if (!encoded || size <= 0 || uint64_t(size) > DumpBudget - written)
                return;
            fs::path pending = path;
            pending += "." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".part";
            {
                std::ofstream stream(pending, std::ios::binary | std::ios::trunc);
                stream.write(reinterpret_cast<const char *>(encoded.get()), size);
                stream.close();
                if (stream)
                    fs::rename(pending, path, ec);
                else
                    ec = std::make_error_code(std::errc::io_error);
            }
            if (ec)
                fs::remove(pending, ec);
            else
                written += static_cast<uint64_t>(size);
        }
        catch (const std::bad_alloc &)
        {
            // Export is optional. Rendering can continue with the original.
        }
    }
}
