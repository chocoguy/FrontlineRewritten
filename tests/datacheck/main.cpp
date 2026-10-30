// Loads the game's GameCube-only (big-endian) data files from the user's
// disc through the native byte swaps and relocation, and checks the result.
//
//   frontline_datacheck <disc folder>   (the folder holding sys/ and files/)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

#include "frontline/platform.h"
#include "types.h"
#include "game/cdb.h"
#include "game/compartment.h"
#include "game/shape.h"
#include "game/staticmesh.h"
#include "game/dmesh.h"
#include "game/gxstore.h"
#include "game/offsetptr.h"
#include "ea/sys/font.h"
#include "aems.h"
#include "check.h"
#include "font.h"
#include "messages.h"
#include "mpf.h"
#include "scr.h"
#include "viv.h"

namespace fs = std::filesystem;

// A bump allocator in the arena, like the game's heaps.
uint8_t* g_next;
int g_failures;

void* ArenaCopy(const std::vector<uint8_t>& data) {
    uint8_t* p = g_next;
    std::memcpy(p, data.data(), data.size());
    g_next += (data.size() + 31) & ~size_t(31);
    return p;
}

namespace {

// The relocation half of CCDBObject::Init, after CDBFromBigEndian.
void RelocateCDB(CDB* cdb) {
    int base = (int)PTR_TO_ADDR(cdb);
    offsetPtr(cdb->m_tris, base);
    offsetPtr(cdb->m_triGroups, base);
    offsetPtr(cdb->m_leaves, base);
    offsetPtr(cdb->m_branches, base);
    offsetPtr(cdb->surfaceTypes, base);
    CDBTriGroup* group = cdb->m_triGroups;
    for (int i = 0; i < cdb->m_numTriGroups; i++) {
        for (int j = 0; j < group->m_count; j++) {
            offsetPtr(group->m_tris[j], base);
            CDBTri* tri = group->m_tris[j];
            for (int k = 0; k < 3; k++) {
                offsetPtr(tri->m_verts[k], base);
            }
        }
        group = (CDBTriGroup*)((u8*)group + group->m_count * 4 + 0x14);
    }
    CDBLeafNode* leaf = cdb->m_leaves;
    for (int i = 0; i < cdb->m_numLeaves; i++) {
        for (int j = 0; j < leaf->m_count; j++) {
            offsetPtr(leaf->m_groups[j], base);
        }
        leaf = (CDBLeafNode*)((u8*)leaf + leaf->m_count * 4 + 0x44);
    }
    CDBBranchNode* branch = cdb->m_branches;
    for (int i = 0; i < cdb->m_numBranches; i++, branch++) {
        offsetPtr(branch->m_front, base);
        offsetPtr(branch->m_back, base);
    }
}

struct CdbStats {
    long tris = 0, noSwap = 0, passThru = 0, lineSkip = 0, surfaceTables = 0;
    std::set<int> surfaceTypes;
};

bool CheckCDB(const std::string& name, const std::vector<uint8_t>& file, CdbStats& stats) {
    auto* cdb = static_cast<CDB*>(ArenaCopy(file));
    CDBFromBigEndian(cdb);
    CHECK(cdb->m_fileSize == file.size(), "file size %u vs %zu", cdb->m_fileSize, file.size());
    CHECK(cdb->m_numVerts == 0 || cdb->m_verts == sizeof(CDB), "vertices at %#x", cdb->m_verts);
    RelocateCDB(cdb);

    auto* verts = reinterpret_cast<CDBVector*>((u8*)cdb + cdb->m_verts);
    CDBVector* vertsEnd = verts + cdb->m_numVerts;
    CDBTri* tris = cdb->m_tris;
    CDBTri* trisEnd = tris + cdb->m_numTris;
    CHECK(cdb->m_numTris == 0 || (u8*)vertsEnd == (u8*)tris, "vertices end at the triangles");
    for (CDBTri* t = tris; t < trisEnd; t++) {
        for (int k = 0; k < 3; k++) {
            CDBVector* v = t->m_verts[k];
            CHECK(v >= verts && v < vertsEnd, "triangle %ld vertex %d out of range", long(t - tris), k);
            CHECK(std::isfinite(v->x) && std::fabs(v->x) < 1e6f, "vertex x %f", v->x);
        }
        stats.tris++;
        stats.noSwap += t->m_texSwap == -1;
        stats.passThru += t->m_passThru;
        stats.lineSkip += t->m_lineSkip;
        stats.surfaceTypes.insert(t->m_surfaceType);
    }
    std::set<const void*> groups, nodes;
    CDBTriGroup* g = cdb->m_triGroups;
    for (int i = 0; i < cdb->m_numTriGroups; i++) {
        groups.insert(g);
        CHECK(g->m_count > 0 && g->m_count < 10000, "group %d count %d", i, g->m_count);
        float n = g->m_normal[0] * g->m_normal[0] + g->m_normal[1] * g->m_normal[1] + g->m_normal[2] * g->m_normal[2];
        CHECK(std::fabs(n - 1.0f) < 1e-3f, "group %d normal length^2 %f", i, n);
        for (int j = 0; j < g->m_count; j++) {
            CDBTri* t = g->m_tris[j];
            CHECK(t >= tris && t < trisEnd, "group %d triangle out of range", i);
        }
        g = (CDBTriGroup*)((u8*)g + g->m_count * 4 + 0x14);
    }
    CHECK(cdb->m_numTriGroups == 0 || (u8*)g == (u8*)(CDBLeafNode*)cdb->m_leaves, "groups end at the leaves");
    CDBLeafNode* leaf = cdb->m_leaves;
    for (int i = 0; i < cdb->m_numLeaves; i++) {
        nodes.insert(leaf);
        CHECK(leaf->m_type != CDBNODE_BRANCH, "leaf %d type %d", i, leaf->m_type);
        for (int j = 0; j < leaf->m_count; j++) {
            CHECK(groups.count((CDBTriGroup*)leaf->m_groups[j]), "leaf %d group %d isn't a group", i, j);
        }
        leaf = (CDBLeafNode*)((u8*)leaf + leaf->m_count * 4 + 0x44);
    }
    CHECK(cdb->m_numLeaves == 0 || (u8*)leaf == (u8*)(CDBBranchNode*)cdb->m_branches, "leaves end at the branches");
    for (int i = 0; i < cdb->m_numSurfaceTypes; i++) {
        CDBSurfaceType& s = cdb->surfaceTypes[i];
        CHECK(s.decalIndex >= -1 && s.decalIndex < 256 && s.soundIndex >= -1 && s.soundIndex < 256,
              "surface type %d: decal %d sound %d", i, s.decalIndex, s.soundIndex);
        stats.surfaceTables++;
    }
    CDBBranchNode* b = cdb->m_branches;
    for (int i = 0; i < cdb->m_numBranches; i++) {
        nodes.insert(&b[i]);
    }
    for (int i = 0; i < cdb->m_numBranches; i++) {
        CHECK(b[i].m_type == CDBNODE_BRANCH, "branch %d type %d", i, b[i].m_type);
        for (CDBNode* child : {(CDBNode*)b[i].m_front, (CDBNode*)b[i].m_back}) {
            CHECK(child == nullptr || nodes.count(child), "branch %d child isn't a node", i);
        }
    }
    (void)name;
    return true;
}


struct CptStats {
    long chunks = 0, lists = 0, nodes = 0, swaps = 0, materials = 0, shapes = 0;
};

bool CheckCPT(const std::vector<uint8_t>& file, CptStats& stats) {
    auto* h = static_cast<CPTHeader*>(ArenaCopy(file));
    u8* lo = (u8*)h;
    u8* hi = lo + file.size();
    auto inFile = [&](const void* p) { return (u8*)p >= lo && (u8*)p < hi; };
    CPTFromBigEndian(h);
    int base = (int)PTR_TO_ADDR(h);
    offsetPtr(h->m_opaque, base);
    offsetPtr(h->m_alpha, base);
    offsetPtr(h->m_chunks, base);
    offsetPtr(h->m_nodes, base);
    offsetPtr(h->m_texSwaps, base);
    CHECK(h->m_numNodes == 0 || inFile(h->m_nodes.get()), "nodes");
    CPTNode* nodesEnd = h->m_nodes + h->m_numNodes;
    CHECK(h->m_numNodes == 0 || (u8*)nodesEnd <= hi, "nodes end");
    auto isNode = [&](CPTNode* n) {
        return n >= h->m_nodes && n < nodesEnd && ((u8*)n - (u8*)h->m_nodes.get()) % sizeof(CPTNode) == 0;
    };
    auto isMaterial = [&](u32 addr) {
        u32 sets[2] = {h->m_opaque.addr(), h->m_alpha.addr()};
        int counts[2] = {h->m_numOpaque, h->m_numAlpha};
        for (int s = 0; s < 2; s++) {
            u32 off = addr - sets[s];
            if (off < (u32)counts[s] * 0x70 && off % 0x70 == 0) {
                return true;
            }
        }
        return false;
    };
    u32 sets[2] = {h->m_opaque.addr(), h->m_alpha.addr()};
    int counts[2] = {h->m_numOpaque, h->m_numAlpha};
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < counts[s]; i++) {
            auto* rec = ADDR_TO_PTR(CPTMaterialRecord, sets[s] + i * 0x70);
            CHECK(inFile(rec), "material record");
            CHECK(rec->m_flags < 0x100, "material %d flags %#x", i, rec->m_flags);
            if (rec->m_shape.addr() != 0) {
                auto* shape = ADDR_TO_PTR(ShapeFile, rec->m_shape.addr() + base);
                CHECK(inFile(shape) && std::memcmp(shape->magic, "SHP", 3) == 0, "material %d shape", i);
                ShapeFileFromBigEndian(shape);
                Shape* first = (Shape*)((u8*)shape + shape->firstShape);
                CHECK(first->width > 0 && first->width <= 1024 && first->height > 0 && first->height <= 1024,
                      "shape %dx%d", first->width, first->height);
                CHECK(first->format >= 0x14 && first->format <= 0x1E, "shape format %#x", (unsigned)first->format);
                stats.shapes++;
            }
            stats.materials++;
        }
    }
    auto checkChunk = [&](CPTChunk* c) -> bool {
        offsetPtr(c->m_node, base);
        offsetPtr(c->m_material, base);
        offsetPtr(c->m_list, base);
        CHECK(isNode(c->m_node), "chunk node");
        CHECK(isMaterial(c->m_material.addr()), "chunk material %#x", c->m_material.addr());
        if (c->m_list != NULL) {
            CPTDisplayList* l = c->m_list;
            CHECK(inFile(l), "display list");
            offsetPtr(l->m_positions, base);
            offsetPtr(l->m_normals, base);
            offsetPtr(l->m_texCoords, base);
            offsetPtr(l->m_colors, base);
            offsetPtr(l->m_list, base);
            CHECK(inFile(l->m_list.get()) && inFile(l->m_positions.get()), "display list data");
            CHECK(l->m_size > 0 && l->m_size < 0x10000, "display list vertices %u", l->m_size);
            stats.lists++;
        }
        stats.chunks++;
        return true;
    };
    for (int i = 0; i < h->m_numChunks; i++) {
        if (!checkChunk(&h->m_chunks[i])) {
            return false;
        }
    }
    std::set<const void*> swaps;
    for (int i = 0; i < h->m_numNodes; i++) {
        CPTNode* n = &h->m_nodes[i];
        offsetPtr(n->m_children[0], base);
        offsetPtr(n->m_children[1], base);
        offsetPtr(n->m_chunks, base);
        offsetPtr(n->m_swaps, base);
        for (CPTNode* child : {n->m_children[0].get(), n->m_children[1].get()}) {
            CHECK(child == nullptr || isNode(child), "node %d child", i);
        }
        CHECK(std::isfinite(n->m_center.x) && std::fabs(n->m_center.x) < 1e6f, "node %d center", i);
        CHECK(n->m_numSwaps >= 0 && n->m_numSwaps < 256, "node %d swaps %d", i, n->m_numSwaps);
        for (int j = 0; j < n->m_numSwaps; j++) {
            CPTTexSwap* s = &n->m_swaps[j];
            CHECK(inFile(s), "swap");
            swaps.insert(s);
            if (!checkChunk(s)) {
                return false;
            }
            CHECK(s->m_numFrames >= 1 && s->m_numFrames <= 5, "swap frames %d", (int)s->m_numFrames);
            CHECK(s->m_mode <= 2, "swap mode %d", (int)s->m_mode);
            for (u32 k = 0; k < s->m_numFrames; k++) {
                offsetPtr(s->m_frames[k], base);
                CHECK(isMaterial(s->m_frames[k].addr()), "swap frame material");
            }
            stats.swaps++;
        }
        stats.nodes++;
    }
    for (int i = 0; i < h->m_numTexSwaps; i++) {
        offsetPtr(h->m_texSwaps[i], base);
        CHECK(swaps.count(h->m_texSwaps[i].get()), "texture swap %d isn't a node's", i);
    }
    return true;
}


struct MshStats {
    long meshes = 0, frames = 0, chunks = 0, nodes = 0, hardPoints = 0, shapes = 0;
};

// The relocation half of CStaticMesh::Init, after StaticMeshFromBigEndian.
bool CheckStaticMesh(StaticMesh* m, u8* lo, u8* hi, MshStats& stats) {
    auto inFile = [&](const void* p) { return (u8*)p >= lo && (u8*)p < hi; };
    StaticMeshFromBigEndian(m);
    int base = (int)PTR_TO_ADDR(m);
    offsetPtr(m->m_opaque, base);
    offsetPtr(m->m_alpha, base);
    offsetPtr(m->m_chunks, base);
    offsetPtr(m->m_nodes, base);
    offsetPtr(m->m_hardPoints, base);
    CHECK(m->m_numNodes > 0 && inFile(m->m_nodes.get()), "nodes %d", m->m_numNodes);
    MSHNode* nodes = m->m_nodes;
    auto isNode = [&](MSHNode* n) {
        return n >= nodes && n < nodes + m->m_numNodes && ((u8*)n - (u8*)nodes) % sizeof(MSHNode) == 0;
    };
    auto isMaterial = [&](u32 addr) {
        u32 sets[2] = {m->m_opaque.addr(), m->m_alpha.addr()};
        int counts[2] = {m->m_numOpaque, m->m_numAlpha};
        for (int s = 0; s < 2; s++) {
            u32 off = addr - sets[s];
            if (off < (u32)counts[s] * 0x70 && off % 0x70 == 0) {
                return true;
            }
        }
        return false;
    };
    u32 sets[2] = {m->m_opaque.addr(), m->m_alpha.addr()};
    int counts[2] = {m->m_numOpaque, m->m_numAlpha};
    for (int s = 0; s < 2; s++) {
        for (int i = 0; i < counts[s]; i++) {
            auto* rec = ADDR_TO_PTR(MSHMaterialRecord, sets[s] + i * 0x70);
            CHECK(inFile(rec) && rec->m_flags < 0x100, "material %d flags %#x", i, rec->m_flags);
            if (rec->m_shape.addr() != 0) {
                auto* shape = ShapeFileFromBigEndian(ADDR_TO_PTR(ShapeFile, rec->m_shape.addr() + base));
                CHECK(inFile(shape) && std::memcmp(shape->magic, "SHP", 3) == 0, "material %d shape", i);
                Shape* first = (Shape*)((u8*)shape + shape->firstShape);
                CHECK(first->width > 0 && first->width <= 1024 && first->format >= 0x14 && first->format <= 0x1E,
                      "shape %dx%d format %#x", first->width, first->height, (unsigned)first->format);
                stats.shapes++;
            }
        }
    }
    for (int i = 0; i < m->m_numChunks; i++) {
        MSHChunk* c = &m->m_chunks[i];
        offsetPtr(c->m_node, base);
        offsetPtr(c->m_material, base);
        offsetPtr(c->m_list, base);
        CHECK(isNode(c->m_node), "chunk %d node", i);
        CHECK(isMaterial(c->m_material.addr()), "chunk %d material", i);
        if (c->m_list != NULL) {
            MSHDisplayList* l = c->m_list;
            offsetPtr(l->m_positions, base);
            offsetPtr(l->m_list, base);
            CHECK(inFile(l) && inFile(l->m_list.get()) && inFile(l->m_positions.get()), "chunk %d display list", i);
            CHECK(l->m_size > 0 && l->m_size < 0x10000 && l->m_flags < 0x100, "display list size %u flags %#x",
                  l->m_size, l->m_flags);
        }
        stats.chunks++;
    }
    for (int i = 0; i < m->m_numNodes; i++) {
        MSHNode* n = &nodes[i];
        offsetPtr(n->m_children[0], base);
        offsetPtr(n->m_children[1], base);
        offsetPtr(n->m_chunks, base);
        for (MSHNode* child : {n->m_children[0].get(), n->m_children[1].get()}) {
            CHECK(child == nullptr || isNode(child), "node %d child", i);
        }
        CHECK(std::isfinite(n->m_center.x) && std::fabs(n->m_center.x) < 1e6f, "node %d center", i);
        stats.nodes++;
    }
    for (int i = 0; i < m->m_numHardPoints; i++) {
        StaticMeshHardPoint& hp = m->m_hardPoints[i];
        CHECK(inFile(&hp), "hard point %d", i);
        float w = hp.m_tm.m[3][3];
        CHECK(std::isfinite(w) && std::fabs(w - 1.0f) < 1e-3f, "hard point %d matrix w %f", i, w);
        stats.hardPoints++;
    }
    stats.meshes++;
    return true;
}

bool CheckMSH(const std::vector<uint8_t>& file, MshStats& stats) {
    auto* m = static_cast<StaticMesh*>(ArenaCopy(file));
    return CheckStaticMesh(m, (u8*)m, (u8*)m + file.size(), stats);
}

bool CheckMPK(const std::vector<uint8_t>& file, MshStats& stats) {
    auto* anim = static_cast<StaticAnimMesh*>(ArenaCopy(file));
    u8* lo = (u8*)anim;
    u8* hi = lo + file.size();
    StaticAnimMeshFromBigEndian(anim);
    offsetPtr(anim->m_frames, (int)PTR_TO_ADDR(anim));
    CHECK(anim->m_numFrames > 0 && anim->m_numFrames < 1000 && anim->m_ticksPerFrame > 0, "frames %d ticks %d",
          anim->m_numFrames, anim->m_ticksPerFrame);
    for (int i = 0; i < anim->m_numFrames; i++) {
        offsetPtr(anim->m_frames[i], (int)PTR_TO_ADDR(anim));
        StaticMesh* frame = anim->m_frames[i];
        CHECK((u8*)frame >= lo && (u8*)frame < hi, "frame %d", i);
        if (!CheckStaticMesh(frame, lo, hi, stats)) {
            return false;
        }
        stats.frames++;
    }
    return true;
}


struct DmfStats {
    long meshes = 0, legacy = 0, parts = 0, lists = 0, vertices = 0, morphs = 0;
};

bool CheckDMF(const std::vector<uint8_t>& file, DmfStats& stats) {
    if (file.size() > 4 && file[4] != 6) {
        stats.legacy++;  // version 5: little-endian PS2 leftovers the game never loads
        return true;
    }
    auto* m = static_cast<CDMesh*>(ArenaCopy(file));
    u8* lo = (u8*)m;
    u8* hi = lo + file.size();
    auto inFile = [&](const void* p, size_t n = 1) { return (u8*)p >= lo && (u8*)p + n <= hi; };
    DMeshFromBigEndian(m);
    CHECK(lo[2] == 'N', "magic");
    int base = (int)PTR_TO_ADDR(m);
    offsetPtr(m->m_parts, base);
    offsetPtr(m->m_positions, base);
    offsetPtr(m->m_normals, base);
    offsetPtr(m->m_nodes, base);
    offsetPtr(m->m_variants, base);
    offsetPtr(m->m_partIndices, base);
    offsetPtr(m->m_basePose, base);
    CHECK(m->m_numParts > 0 && m->m_numParts < 256, "parts %u", m->m_numParts);
    CHECK(inFile(m->m_positions.get(), m->m_numPositions * 6), "positions %u", m->m_numPositions);
    CHECK(inFile(m->m_normals.get(), m->m_numNormals * 6), "normals %u", m->m_numNormals);
    CHECK(m->m_numJoints < 0x80 && inFile(m->m_basePose.get(), m->m_numJoints * 6), "joints %u", m->m_numJoints);
    const DMeshPartRecord* parts = (const DMeshPartRecord*)m->m_parts.get();
    bool morph = m->m_numParts > 1 && (parts[1].m_flags & DMESHPART_MORPH);
    stats.morphs += morph;
    for (u32 i = 0; i < m->m_numParts; i++) {
        const DMeshPartRecord& part = parts[i];
        CHECK(inFile(&part, sizeof(part)), "part %u", i);
        CHECK(part.m_texture < m->m_numTextureNames || m->m_numTextureNames == 0, "part %u texture %u", i,
              part.m_texture);
        CHECK(part.m_numLists < 1024, "part %u lists %u", i, part.m_numLists);
        auto* lists = ADDR_TO_PTR(CDMeshDisplayList, part.m_lists.addr() + base);
        for (u32 j = 0; j < part.m_numLists; j++) {
            CDMeshDisplayList& l = lists[j];
            CHECK(inFile(&l, sizeof(l)) && l.numMatrices <= 10, "part %u list %u", i, j);
            offsetPtr(l.data, base);
            u8* cmd = l.data;
            u8* end = cmd + (l.size << 5);
            CHECK(inFile(cmd, l.size << 5), "part %u list %u data", i, j);
            if (morph && i > 0) {
                continue;  // morph targets: (position, normal) index pairs
            }
            // Walk the commands as DMMorph does: op, big-endian count, 7-byte vertices.
            while (cmd != end && *cmd != 0) {
                CHECK(*cmd == 0x98 || *cmd == 0x90 || *cmd == 0xA0 || *cmd == 0x80 || (*cmd & 0xF8) == 0x98 ||
                          (*cmd & 0xF8) == 0x90,
                      "part %u list %u op %#x", i, j, *cmd);
                u16 count = GX_LOAD16(cmd + 1);
                u8* vtx = cmd + 3;
                for (u16 k = 0; k < count; k++, vtx += 7) {
                    CHECK(vtx + 7 <= end, "part %u list %u overruns", i, j);
                    CHECK(GX_LOAD16(vtx + 1) < m->m_numPositions && GX_LOAD16(vtx + 3) < m->m_numNormals,
                          "part %u list %u vertex %u index", i, j, k);
                    stats.vertices++;
                }
                cmd = vtx;
            }
            stats.lists++;
        }
        stats.parts++;
    }
    stats.meshes++;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <disc folder>\n", argv[0]);
        return 2;
    }
    fs::path data = fs::path(argv[1]) / "files" / "DATA";
    frontline::ArenaInit();
    g_next = static_cast<uint8_t*>(frontline::ArenaLo());

    int files = 0, cptFiles = 0;
    CdbStats stats;
    CptStats cptStats;
    MshStats mshStats;
    DmfStats dmfStats;
    AemsStats aemsStats;
    ScrStats scrStats;
    FontStats fontStats;
    MpfStats mpfStats;
    for (auto& entry : fs::recursive_directory_iterator(data)) {
        if (entry.path().extension() == ".abk") {
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
            uint8_t* mark = g_next;
            if (!CheckAEMSBank(file, aemsStats)) {
                std::printf("  in %s\n", entry.path().filename().c_str());
            }
            g_next = mark;
            continue;
        }
        if (entry.path().extension() == ".gfn") {
            std::ifstream in(entry.path(), std::ios::binary);
            std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), {});
            uint8_t* mark = g_next;
            if (!CheckFont(file, fontStats)) {
                std::printf("  in %s\n", entry.path().filename().c_str());
            }
            g_next = mark;
            continue;
        }
        if (entry.path().extension() != ".viv") {
            continue;
        }
        for (auto& e : ReadViv(entry.path())) {
            std::string ext = e.name.size() > 4 ? e.name.substr(e.name.size() - 4) : "";
            uint8_t* mark = g_next;
            bool ok = true;
            if (ext == ".cdb") {
                ok = CheckCDB(e.name, e.data, stats);
                files++;
            } else if (ext == ".cpt") {
                ok = CheckCPT(e.data, cptStats);
                cptFiles++;
            } else if (ext == ".msh") {
                ok = CheckMSH(e.data, mshStats);
            } else if (ext == ".mpk") {
                ok = CheckMPK(e.data, mshStats);
            } else if (ext == ".dmf") {
                ok = CheckDMF(e.data, dmfStats);
            } else if (ext == ".aem") {
                ok = CheckAEMSystem(e.data, aemsStats);
            } else if (ext == ".mpf") {
                ok = CheckMPF(e.data, mpfStats);
            } else if (ext == ".gfn") {
                ok = CheckFont(e.data, fontStats);
            } else if (ext == ".scr" || ext == ".SCR") {
                ok = CheckSCR(e.data, scrStats);
            }
            if (!ok) {
                std::printf("  in %s (%s)\n", e.name.c_str(), entry.path().parent_path().filename().c_str());
            }
            g_next = mark;
        }
    }
    std::printf("cdb: %d files, %ld triangles; texture swap -1 on %.1f%%, pass-through %ld, line-skip %ld, "
                "%zu surface types (%d..%d), %ld surface table entries\n",
                files, stats.tris, 100.0 * stats.noSwap / std::max(1L, stats.tris), stats.passThru,
                stats.lineSkip, stats.surfaceTypes.size(),
                stats.surfaceTypes.empty() ? 0 : *stats.surfaceTypes.begin(),
                stats.surfaceTypes.empty() ? 0 : *stats.surfaceTypes.rbegin(), stats.surfaceTables);
    std::printf("cpt: %d files, %ld nodes, %ld chunks, %ld display lists, %ld texture swaps, %ld materials, "
                "%ld shapes\n",
                cptFiles, cptStats.nodes, cptStats.chunks, cptStats.lists, cptStats.swaps, cptStats.materials,
                cptStats.shapes);
    std::printf("msh/mpk: %ld meshes (%ld animation frames), %ld chunks, %ld nodes, %ld hard points, %ld shapes\n",
                mshStats.meshes, mshStats.frames, mshStats.chunks, mshStats.nodes, mshStats.hardPoints,
                mshStats.shapes);
    std::printf("dmf: %ld meshes (%ld morphing; %ld legacy v5 skipped), %ld parts, %ld display lists, %ld vertices\n",
                dmfStats.meshes, dmfStats.morphs, dmfStats.legacy, dmfStats.parts, dmfStats.lists, dmfStats.vertices);
    std::printf("aems: %ld systems (%ld events), %ld module banks, %ld modules, %ld components (%zu kinds), "
                "%ld envelopes, %ld tables, %ld bank and %ld stream player entries, %zu bank ids referenced\n",
                aemsStats.systems, aemsStats.events, aemsStats.banks, aemsStats.modules, aemsStats.comps,
                aemsStats.types.size(), aemsStats.envelopes, aemsStats.tables, aemsStats.bankPlayers,
                aemsStats.streamPlayers, aemsStats.banksReferenced.size());
    std::printf("      %ld sample banks (%ld midi), %ld patches, %ld timbres, %ld envelopes, %ld ADPCM channels\n",
                aemsStats.sampleBanks, aemsStats.midiBanks, aemsStats.patches, aemsStats.timbres,
                aemsStats.sampleEnvelopes, aemsStats.adpcm);
    std::printf("scr: %ld screens, %ld controls, %ld event handlers, %ld strings (%ld referenced from data); objects:",
                scrStats.screens, scrStats.controls, scrStats.handlers, scrStats.strings, scrStats.stringRefs);
    for (auto& [type, count] : scrStats.objects) {
        std::printf(" %ld of type %d", count, type);
    }
    std::printf("\n");
    if (!CheckFont(FONTstandfile, fontStats)) {
        std::printf("  in the built-in font (FONTstandfile)\n");
    }
    std::printf("gfn: %ld fonts (%ld wide), %ld characters, %ld kern pairs\n", fontStats.fonts, fontStats.wide,
                fontStats.chars, fontStats.kerns);
    MessageStats messageStats;
    auto read = [](const fs::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {});
    };
    if (!CheckMessages(read(data / "chars.bum"), read(data / "binum.bum"), messageStats)) {
        std::printf("  in chars.bum\n");
    }
    std::printf("bum: %ld user messages, %ld rewritten for the keyboard (%ld \"Press ACTION\")\n",
                messageStats.messages, messageStats.rewritten, messageStats.pressAction);
    std::printf("mpf: %ld music maps (%ld legacy skipped), %ld nodes, %ld branches, %ld waves, %ld fade curves, %ld remap pairs\n",
                mpfStats.maps, mpfStats.legacy, mpfStats.nodes, mpfStats.branches, mpfStats.waves, mpfStats.fades, mpfStats.remaps);
    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ok", g_failures);
    return g_failures ? 1 : 0;
}
