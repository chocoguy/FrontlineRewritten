// Interactive music maps (.mpf): game/path.h's swap, then the structure
// PATH_addmapfile and the player walk.

#include "mpf.h"

#include "check.h"
#include "types.h"
#include "game/path.h"

bool CheckMPF(const std::vector<uint8_t>& file, MpfStats& stats) {
    auto* map = static_cast<PATHMAP*>(ArenaCopy(file));
    u8* base = (u8*)map;
    u32 size = file.size();
    auto inFile = [&](const void* p, u32 n) { return (u8*)p >= base && (u8*)p + n <= base + size; };
    if (base[0] == 'x' && base[4] == 3 && base[5] != 4) {
        // An older (3.2) little-endian map: PATH_addmapfile turns it down on
        // GameCube (magic and version) and natively (version).
        stats.legacy++;
        return true;
    }
    CHECK(PathMapFromBigEndian(map), "magic %.4s", (char*)base);
    CHECK(!PathMapFromBigEndian(map), "swapped twice");
    CHECK(map->magic == 0x50464478 && map->version == 3 && map->subversion == 4, "magic %#x version %d.%d",
          map->magic, map->version, map->subversion);
    CHECK(map->numNodes > 0 && map->numTracks > 0 && map->numTracks <= PATH_MAXTRACKS, "%u nodes, %u tracks",
          map->numNodes, map->numTracks);
    int maxWave = 0, maxFade = 0, maxRemap = 0;
    PATHFINDNODE* last = NULL;
    for (int i = 0; i < map->numNodes; i++) {
        auto* n = (PATHFINDNODE*)((u32*)map + map->nodeOffsets[i]);
        CHECK(map->nodeOffsets[i] > 0 && inFile(n, 0xC) && inFile(n, 0xC + n->numBranches * 4), "node %d at %d", i,
              map->nodeOffsets[i]);
        CHECK(n->track < map->numTracks && n->wave >= -2 && n->beatsPerBar > 0, "node %d: track %u wave %d, %u beats", i,
              n->track, n->wave, n->beatsPerBar);
        for (u32 k = 0; k < n->numBranches; k++) {
            PATHBRANCH& b = n->branches[k];
            CHECK(b.node >= -1 && b.node < map->numNodes && b.lo <= b.hi, "node %d branch %u: %d..%d -> %d", i, k, b.lo,
                  b.hi, b.node);
            stats.branches++;
        }
        maxWave = std::max<int>(maxWave, n->wave);
        maxFade = std::max<int>(maxFade, std::max<int>(n->fadeIn, n->fadeOut));
        maxRemap = std::max<int>(maxRemap, n->remap);
        last = n;
        stats.nodes++;
    }
    u8* eventTable = (u8*)last + last->numBranches * sizeof(PATHBRANCH) + 0xC;
    auto* values = (PATHEVENTVALUE*)(eventTable + ((map->numTracks * (map->numEvents * map->numSections) + 3) & ~3));
    CHECK(inFile(values, map->numValues * 4), "%u values", map->numValues);
    for (int i = 0; i < map->numEvents * map->numSections * map->numTracks; i++) {
        CHECK(eventTable[i] < map->numValues || map->numValues == 0, "event cell %d -> %u", i, eventTable[i]);
    }
    for (int i = 0; i < map->numValues; i++) {
        CHECK(values[i].node >= -1 && values[i].node < map->numNodes, "value %d node %d", i, values[i].node);
    }
    u32* tables = (u32*)(values + map->numValues);
    for (int list = 1; list <= maxRemap; list++) {
        u32 first = tables[list - 1], end = tables[list];
        CHECK(first <= end && inFile((u32*)map + first, (end - first) * 4), "remap list %d: %u..%u", list, first, end);
        for (u32 k = first; k < end; k++) {
            u32 pair = ((u32*)map)[k];
            CHECK((pair >> 16) < map->numNodes && (pair & 0xFFFF) < map->numNodes, "remap pair %#x", pair);
            stats.remaps++;
        }
    }
    u32* fadeOffsets = (u32*)map + tables[map->tableIndex];
    CHECK(inFile(fadeOffsets, (map->numTracks + 1 + maxWave * 2) * 4), "fade offsets at %u", tables[map->tableIndex]);
    auto* waves = (s32(*)[2])(fadeOffsets + map->numTracks + 1);
    for (int i = 0; i < maxWave; i++) {
        CHECK(waves[i][0] >= 0 && waves[i][1] > 0 && waves[i][1] < 600000, "wave %d: %d, %d ms", i + 1, waves[i][0],
              waves[i][1]);
        stats.waves++;
    }
    auto* fades = (PATHFADE*)((u32*)map + fadeOffsets[map->numTracks]);
    CHECK(inFile(fades, maxFade * 4), "%d fades at %u", maxFade, fadeOffsets[map->numTracks]);
    // The swap covers the whole file: the fade table, last, ends it. (It is
    // empty on the disc, so PATH_fade's default curve 1 reads the 4 bytes
    // after the map, on GameCube too.)
    CHECK((u8*)(waves + maxWave) == (u8*)fades && (u8*)(fades + maxFade) == base + size,
          "waves end at %#lx, fades at %#lx, file at %#x", long((u8*)(waves + maxWave) - base), long((u8*)fades - base),
          size);
    for (int i = 0; i < maxFade; i++) {
        CHECK(fades[i].type >= 1 && fades[i].type <= 3 && fades[i].duration > 0, "fade %d: type %u, %u ms", i + 1,
              fades[i].type, fades[i].duration);
        stats.fades++;
    }
    stats.maps++;
    return true;
}
