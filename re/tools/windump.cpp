// Dumps a .win UI layout from the game's PK2 archives (format: rrt2-decomp docs/formats/ui.md).
//   windump <game data dir> <name.win> [out.bin]
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static uint32_t name_hash(const char* s) {
    uint32_t h = 7;
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        if ((c >= 'a' && c <= 'z') || c > 0xEF) c -= 0x20;
        h = c + 0x3039 + h * 0x41C64E6Eu;
    }
    return h;
}

static bool find_in_pack(const std::string& path, uint32_t hash, std::vector<uint8_t>* out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint32_t n = 0;
    fread(&n, 4, 1, f);
    std::vector<uint32_t> dir(n * 3);
    fread(dir.data(), 4, dir.size(), f);
    for (uint32_t i = 0; i < n; i++) {
        if (dir[i * 3] != hash) continue;
        out->resize(dir[i * 3 + 2]);
        fseek(f, 4 + 12 * n + dir[i * 3 + 1], SEEK_SET);
        fread(out->data(), 1, out->size(), f);
        fclose(f);
        return true;
    }
    fclose(f);
    return false;
}

static int struct_size(uint32_t type) {
    switch (type) {
    case 0x1: return 0x52; case 0x2: return 0x4d; case 0x4: return 0x3f; case 0x8: return 0x91;
    case 0x10: return 0x35; case 0x20: return 0x45; case 0x40: return 0x45; case 0x80: return 0x4f;
    case 0x100: return 0x61; case 0x200: return 0x55;
    }
    return -1;
}

struct Reader {
    const std::vector<uint8_t>& d;
    size_t p = 0;
    uint32_t u32() { uint32_t v; memcpy(&v, &d[p], 4); p += 4; return v; }
    std::string str() { u32(); uint32_t n = u32(); std::string s((const char*)&d[p], n ? n - 1 : 0); p += n; return s; }
};

static uint16_t u16at(const uint8_t* p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t u32at(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: windump <data dir> <name.win> [out.bin]\n"); return 2; }
    std::string dir = argv[1];
    const char* packs[] = {"RT2_11PL.PK2", "RT2_09GO.PK2", "RT2_06.PK2", "RT2.PK2"};
    std::vector<uint8_t> data;
    uint32_t h = name_hash(argv[2]);
    const char* found = nullptr;
    for (const char* p : packs)
        if (find_in_pack(dir + "\\" + p, h, &data)) { found = p; break; }
    if (!found) { fprintf(stderr, "%s (hash %08X) not found\n", argv[2], h); return 1; }
    if (argc > 3) { FILE* o = fopen(argv[3], "wb"); fwrite(data.data(), 1, data.size(), o); fclose(o); }

    Reader r{data};
    if (r.u32() != 0x7d0) { fprintf(stderr, "not a .win\n"); return 1; }
    const uint8_t* w = &data[r.p];
    printf("%s from %s, %zu bytes: size %ux%u client (%u,%u %ux%u) framed=%u base=%u\n", argv[2], found, data.size(),
           u32at(w + 9), u32at(w + 13), u32at(w + 33), u32at(w + 37), u32at(w + 41), u32at(w + 45), u32at(w + 49),
           u32at(w + 53));
    r.p += 65;
    r.str();
    r.u32();  // 0x7d1
    printf("%-6s %-5s %-4s %-5s %-22s %-4s %s\n", "type", "id", "z", "group", "rect x,y w x h", "font", "bank / extra");
    while (true) {
        uint32_t mark = r.u32();
        if (mark == 0x7d4) break;
        uint32_t type = r.u32();
        r.str(); r.str();
        std::string bank = r.str();
        int n = struct_size(type);
        if (n < 0) { printf("unknown type %X at %zu\n", type, r.p); return 1; }
        const uint8_t* c = &data[r.p];
        r.p += n;
        std::string extra;
        if (type == 1 || type == 2 || type == 8 || type == 0x100) extra = r.str();
        r.u32();  // 0x7d3
        char rect[32];
        snprintf(rect, sizeof(rect), "%u,%u %ux%u", u16at(c + 7), u16at(c + 9), u16at(c + 11), u16at(c + 13));
        printf("%-6X %-5u %-4u %-5u %-22s %-4u %s%s%s\n", type, u16at(c), c[4], u16at(c + 5), rect, c[35], bank.c_str(),
               extra.empty() ? "" : " / ", extra.c_str());
    }
    return 0;
}
