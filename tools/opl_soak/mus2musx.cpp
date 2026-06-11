//
// Convert each D_* music lump in a WAD to the MUSX format the device plays
// (whd_gen's compress_mus), one output file per song. Lets the host soak
// harness exercise the EXACT device music path (MUSX_LoadRaw) instead of
// the standard MUS->MID conversion.
//
// Build (from repo root):
//   c++ -O2 -std=c++17 -o /tmp/mus2musx \
//      -Isrc/whd_gen -Wno-missing-template-arg-list-after-template-kw \
//      tools/opl_soak/mus2musx.cpp src/whd_gen/compress_mus.cpp \
//      src/whd_gen/mus2seq.cpp src/whd_gen/wad.cpp
//
// Run: /tmp/mus2musx DOOM1.WAD /tmp/musx
//
#include <cstdio>
#include <string>
#include <utility>
#include "wad.h"
#include "compress_mus.h"

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <wad> <outdir>\n", argv[0]); return 1; }
    wad w = wad::read(argv[1]);
    int count = 0;
    for (auto &e : w.get_lumps()) {
        lump &l = e.second;
        if (l.name.rfind("D_", 0) != 0) continue;
        auto musx = compress_mus(e);
        std::string out = std::string(argv[2]) + "/" + l.name + ".musx";
        FILE *f = fopen(out.c_str(), "wb");
        if (!f) { fprintf(stderr, "cannot write %s\n", out.c_str()); return 1; }
        // Same wrapper whd_gen writes into the WHX: "MUSX" + u32 payload size.
        uint32_t n = (uint32_t)musx.size();
        fwrite("MUSX", 1, 4, f);
        fwrite(&n, 4, 1, f);
        fwrite(musx.data(), 1, musx.size(), f);
        fclose(f);
        printf("%s: %zu -> %zu bytes\n", l.name.c_str(), l.data.size(), musx.size());
        count++;
    }
    printf("%d songs converted\n", count);
    return 0;
}
