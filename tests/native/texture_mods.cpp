#include "sotc_texture_mods.h"

#include <array>
#include <cassert>

int main(int argc, char **argv)
{
    assert(argc == 2);
    namespace mods = sotc_texture_mods;
    const mods::fs::path folder = argv[1];
    constexpr uint64_t id = 0x0400123456789abcull;
    constexpr std::array<uint8_t, 16> guest{
        240, 0, 0, 128, 0, 80, 0, 0,
        240, 0, 0, 64, 0, 0, 100, 0,
    };
    mods::dump(folder, id, 2, 2, guest.data());
    const auto path = mods::texturePath(folder, id);
    assert(path.filename() == "0400123456789abc.png");
    assert(mods::fs::is_regular_file(path));
    mods::Texture texture;
    assert(mods::load(path, 1, 1, true, texture));
    assert(texture.width == 2 && texture.height == 2 && texture.levels == 2);
    assert(texture.pixels.size() == 20 && std::equal(guest.begin(), guest.end(), texture.pixels.begin()));
    assert(texture.pixels[16] == 240 && texture.pixels[17] == 0 &&
           texture.pixels[18] == 0 && texture.pixels[19] == 48);
    assert(!mods::load(path, 2, 1, true, texture));
    assert(!mods::load(folder / "missing.png", 1, 1, true, texture));
    assert(texture.pixels.size() == 20);
    assert(mods::load(path, 2, 2, false, texture));
    assert(texture.levels == 1 && texture.pixels.size() == 16);

    // Reject an unbounded IHDR before decoding any pixels.
    std::array<uint8_t, 24> header{};
    {
        std::ifstream input(path, std::ios::binary);
        input.read(reinterpret_cast<char *>(header.data()), header.size());
        assert(input);
    }
    header[16] = 0x7f;
    uint32_t width = 0, height = 0;
    assert(!mods::pngSize(header.data(), width, height));
    return 0;
}
