// The extended item icon archive (runtime/src/mods/item_icons.cpp) for tools/story_mods/test_lyt.py, which
// compares its output byte for byte with tools/story_mods/lyt/item_icons.py:
//   item_icons_test PACK_IN PACK_OUT ICON.rgba...   extend_pack (each icon: 128 x 128 RGBA bytes)
// Without arguments: the builder's own checks (refusals, the Yaz0 reader).
#include "mods/item_icons.h"
#include "mods/lyt_build.h"

#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>

void log_msg(const char* fmt, ...) {
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

static item_icons::Bytes read(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

template <class F>
static bool refused(F f) {
    try {
        f();
    } catch (const std::runtime_error&) {
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    using item_icons::Bytes;
    if (argc >= 3) {
        std::vector<Bytes> icons;
        for (int i = 3; i < argc; i++) icons.push_back(read(argv[i]));
        try {
            const Bytes out = item_icons::extend_pack(read(argv[1]), icons);
            std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
        } catch (const std::exception& e) {
            fprintf(stderr, "item_icons_test: %s\n", e.what());
            return 1;
        }
        return 0;
    }
    // Yaz0: a literal run, then a back reference (3 bytes from 3 back) and a long one (0x12 + 2)
    const Bytes yaz{'Y', 'a', 'z', '0', 0, 0, 0, 26, 0, 0, 0, 0, 0, 0, 0, 0, 0xE0, 'a', 'b', 'c', 0x10, 0x02, 0x00, 0x02, 0x02};
    const Bytes plain = item_icons::yaz0_decode(yaz);
    assert(std::string(plain.begin(), plain.end()) == "abcabcabcabcabcabcabcabcab");
    assert(refused([] { item_icons::yaz0_decode(Bytes{'Y', 'a', 'z', '0', 0, 0, 0, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x10}); }));
    assert(refused([] { item_icons::read_sarc(Bytes(64, 0)); }));
    assert(refused([] { item_icons::glow(Bytes(16, 0)); }));
    // a transparent icon glows nowhere; an opaque one everywhere but at the softened edge
    const Bytes none = item_icons::glow(Bytes(128 * 128 * 4, 0));
    for (size_t i = 0; i < none.size(); i += 4) assert(none[i] == 255 && none[i + 3] == 0);
    const Bytes full = item_icons::glow(Bytes(128 * 128 * 4, 255));
    assert(full[(64 * 128 + 64) * 4 + 3] == 255 && full[3] < 255);
    // the game's own icons (the VR item wheel): RGBA8 untiled again in every tile mode and swizzle, as the .bflim
    // reader and the icon archive's reader see them
    {
        Bytes px(37 * 29 * 4);
        for (size_t i = 0; i < px.size(); i++) px[i] = uint8_t(i * 7 + i / 11);
        for (int mode : {1, 2, 4})
            for (uint32_t sw = 0; sw < (mode == 4 ? 8u : 1u); sw++)
                assert(lyt_build::untile_rgba8(lyt_build::tile_rgba8(px, 37, 29, mode, sw), 37, 29, mode, sw) == px);
        Bytes icon(128 * 128 * 4);
        for (size_t i = 0; i < icon.size(); i++) icon[i] = uint8_t(i % 251);
        const Bytes flim = lyt_build::texture(icon, 128, 128, "rgba8", "tiled", 3);
        Bytes back;
        uint32_t w = 0, h = 0;
        assert(item_icons::flim_rgba8(flim, back, w, h) && w == 128 && h == 128 && back == icon);
        assert(!item_icons::flim_rgba8(lyt_build::texture(icon, 128, 128, "bc1", "tiled", 0), back, w, h));  // not RGBA8
        assert(!item_icons::flim_rgba8(Bytes(64, 0), back, w, h));
        const std::vector<Bytes> icons = item_icons::icons_of({{"timg/Icon128_05^l.bflim", flim}});
        assert(int(icons.size()) == item_icons::kGameFrames && icons[5] == icon && icons[4].empty());
        // the Wind Waker's picture (the Quest screen's, 118 x 118): centred on a clear 128 x 128 icon
        Bytes small(118 * 118 * 4);
        for (size_t i = 0; i < small.size(); i++) small[i] = uint8_t(i * 7 + 3);
        const Bytes centred = item_icons::centred_icon(small, 118, 118);
        assert(centred.size() == 128 * 128 * 4 && centred[0] == 0 && centred[3] == 0);  // a clear border
        assert(centred[(5 * 128 + 5) * 4] == small[0] && centred[((5 + 117) * 128 + 5 + 117) * 4 + 3] == small[(117 * 118 + 117) * 4 + 3]);
        assert(item_icons::centred_icon(small, 130, 118).empty() && item_icons::centred_icon(Bytes(16), 118, 118).empty());
        const Bytes collect = lyt_build::texture(small, 118, 118, "rgba8", "tiled", 0);
        assert(item_icons::wind_waker_icon({{item_icons::kWindWakerCollectIcon, collect}}) == centred);
        assert(item_icons::wind_waker_icon({{"timg/CollectIcon118_06^l.bflim", collect}}).empty());  // another collect icon
    }
    // with the player's own 2D pack (WWHD_TEST_2D_PACK=path): every game icon reads
    if (const char* pack = std::getenv("WWHD_TEST_2D_PACK")) {
        const auto members = item_icons::read_sarc(read(pack));
        const std::vector<Bytes> icons = item_icons::icons_of(item_icons::read_sarc(item_icons::yaz0_decode(members.at("BtnItemIcon_00.szs"))));
        int n = 0;
        for (const Bytes& i : icons) n += !i.empty();
        printf("item_icons_test: %d of %d game icons read from the pack\n", n, item_icons::kGameFrames);
        assert(n == item_icons::kGameFrames);
        // and the Wind Waker's from the Quest screen's icons
        const Bytes windWaker = item_icons::wind_waker_icon(
            item_icons::read_sarc(item_icons::yaz0_decode(members.at(item_icons::kCollectMember))));
        printf("item_icons_test: the Wind Waker's icon %s\n", windWaker.empty() ? "NOT read" : "read");
        assert(windWaker.size() == size_t(item_icons::kIconSize) * item_icons::kIconSize * 4);
        if (const char* out = std::getenv("WWHD_TEST_ICON_DUMP"))  // raw RGBA of icon 0 (to look at)
            std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(icons[0].data()), std::streamsize(icons[0].size()));
    }
    puts("item_icons_test: ok");
    return 0;
}
