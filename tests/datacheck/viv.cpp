#include "viv.h"

#include <fstream>
#include <iterator>

namespace {

uint32_t Be(const std::vector<uint8_t>& d, size_t p, int bytes) {
    uint32_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = (v << 8) | d.at(p + i);
    }
    return v;
}

}  // namespace

std::vector<VivEntry> ReadViv(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), {});
    std::vector<VivEntry> out;
    size_t p;
    uint32_t count;
    int offBytes, sizeBytes;
    if (d.size() >= 6 && d[0] == 0xC0 && d[1] == 0xFB) {
        count = Be(d, 4, 2);
        p = 6;
        offBytes = sizeBytes = 3;
    } else if (d.size() >= 16 && d[0] == 'B' && d[1] == 'I' && d[2] == 'G') {
        count = Be(d, 8, 4);
        p = 16;
        offBytes = sizeBytes = 4;
    } else {
        return out;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t off = Be(d, p, offBytes);
        uint32_t size = Be(d, p + offBytes, sizeBytes);
        p += offBytes + sizeBytes;
        size_t end = p;
        while (d.at(end) != 0) {
            end++;
        }
        VivEntry e;
        e.name.assign(reinterpret_cast<const char*>(&d[p]), end - p);
        p = end + 1;
        if (off + static_cast<size_t>(size) <= d.size()) {
            e.data.assign(d.begin() + off, d.begin() + off + size);
        }
        out.push_back(std::move(e));
    }
    return out;
}
