// The Modern renderer's meshes (mesh_cache.h).
//
// A display list is walked once as GX would run it (gx_vertex.h
// ScanDisplayList), its vertices decoded but not transformed, and its
// primitives made into one list of triangles. Meshes live in chunks of
// shared GPU memory; a chunk goes when no mesh in it is left (command buffers
// still drawing from it keep it alive). Tables are malloc'd, as in the GX
// backend.

#include "mesh_cache.h"

#include <time.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// The game's headers define BOOL as int; Objective-C's is bool.
#define BOOL FRONTLINE_GAME_BOOL
#include "gx/gx_efb.h"
#include "gx/gx_metal.h"
#include "gx/gx_state.h"
#include "gx/gx_vertex.h"
#include "mesh_shader.h"
#include "types.h"
#undef BOOL

namespace frontline::mesh {
namespace {

using gx::g_state;

constexpr uint32_t kChunkSize = 8u << 20;
constexpr uint32_t kMaxChunks = 512;
constexpr uint64_t kBudgetNanos = 2000000;  // making meshes, a frame
constexpr uint32_t kGuardBytes = 64;

// A hash of a list's first bytes (kGuardBytes), 8 at a time.
uint64_t Guard(const uint8_t* list, uint32_t size) {
    const uint32_t n = size < kGuardBytes ? size : kGuardBytes;
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        std::memcpy(&w, list + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
    }
    for (; i < n; i++) {
        h = (h ^ list[i]) * 0x100000001b3ull;
    }
    return h;
}

// An FNV-style hash, a word at a time.
uint64_t FnvWords(const uint32_t* words, size_t count, uint64_t h) {
    for (size_t i = 0; i < count; i++) {
        h = (h ^ words[i]) * 0x100000001b3ull;
    }
    return h;
}

// The vertex format: descriptors and every format's attributes, hashed again
// only when GX changed them (g_formatGeneration). The world bin switches
// between two descriptors from chunk to chunk, so the attribute formats
// (2.5 KB) are hashed again only when they're different.
uint64_t FormatKey() {
    static uint32_t generation = ~0u;
    static uint64_t key, attrKey;
    static gx::AttrFmt attrs[8][gx::kNumAttrs];
    static bool attrsHashed;
    if (generation != gx::g_formatGeneration) {
        generation = gx::g_formatGeneration;
        bool same = attrsHashed;
        for (int f = 0; f < 8 && same; f++) {
            for (uint32_t a = 0; a < gx::kNumAttrs && same; a++) {
                const gx::AttrFmt &x = attrs[f][a], &y = g_state.vat[f][a];
                same = x.cnt == y.cnt && x.type == y.type && x.frac == y.frac;
            }
        }
        if (!same) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (int f = 0; f < 8; f++) {
                for (uint32_t a = 0; a < gx::kNumAttrs; a++) {
                    const gx::AttrFmt& x = g_state.vat[f][a];
                    const uint32_t words[3] = {x.cnt, x.type, x.frac};
                    h = FnvWords(words, 3, h);
                    attrs[f][a] = x;
                }
            }
            attrKey = h;
            attrsHashed = true;
        }
        key = FnvWords(g_state.vtxDesc, gx::kNumAttrs, attrKey);
    }
    return key;
}

// The meshes, by list address: open addressing, never shrinking.
struct Table {
    Mesh* entries;
    uint32_t mask, count;

    uint32_t Slot(uint32_t key) const { return (uint32_t)(((uint64_t)key * 0x9E3779B97F4A7C15ull) >> 32) & mask; }

    Mesh* Find(uint32_t key) {
        if (entries == nullptr) {
            return nullptr;
        }
        for (uint32_t i = Slot(key);; i = (i + 1) & mask) {
            if (entries[i].list == key) {
                return &entries[i];
            }
            if (entries[i].list == 0) {
                return nullptr;
            }
        }
    }

    // The entry for `key`, zeroed but for the key if it's new.
    Mesh& Insert(uint32_t key) {
        if ((count + 1) * 4 > (entries != nullptr ? mask + 1 : 0) * 3) {
            Mesh* old = entries;
            const uint32_t oldCapacity = old != nullptr ? mask + 1 : 0;
            const uint32_t capacity = oldCapacity != 0 ? oldCapacity * 2 : 4096;
            entries = static_cast<Mesh*>(std::calloc(capacity, sizeof(Mesh)));
            mask = capacity - 1;
            for (uint32_t i = 0; i < oldCapacity; i++) {
                if (old[i].list != 0) {
                    uint32_t j = Slot(old[i].list);
                    while (entries[j].list != 0) {
                        j = (j + 1) & mask;
                    }
                    entries[j] = old[i];
                }
            }
            std::free(old);
        }
        for (uint32_t i = Slot(key);; i = (i + 1) & mask) {
            Mesh& e = entries[i];
            if (e.list == key) {
                return e;
            }
            if (e.list == 0) {
                std::memset(&e, 0, sizeof(e));
                e.list = key;
                count++;
                return e;
            }
        }
    }
} g_meshes;

// GPU memory: chunks filled in order, each counting the meshes in it.
id<MTLBuffer> g_chunks[kMaxChunks];
uint32_t g_chunkUsed[kMaxChunks], g_chunkLive[kMaxChunks];
uint32_t g_chunk = kMaxChunks;  // the one being filled
uint64_t g_bytes;               // in live chunks

void Release(Mesh& mesh) {
    if (mesh.gxOnly || mesh.numIndices == 0 || mesh.chunk >= kMaxChunks) {
        return;
    }
    const uint32_t c = mesh.chunk;
    mesh.numIndices = 0;
    if (--g_chunkLive[c] == 0 && c != g_chunk) {
        g_bytes -= g_chunks[c].length;
        g_chunks[c] = nil;
        g_chunkUsed[c] = 0;
    }
}

// Room for `size` bytes, 16-byte aligned: the chunk and offset.
bool Allocate(uint32_t size, uint32_t* chunk, uint32_t* offset) {
    size = (size + 15) & ~15u;
    if (g_chunk >= kMaxChunks || g_chunkUsed[g_chunk] + size > g_chunks[g_chunk].length) {
        // A new chunk (bigger for a mesh that's bigger than one).
        if (g_chunk < kMaxChunks && g_chunkLive[g_chunk] == 0) {
            g_bytes -= g_chunks[g_chunk].length;
            g_chunks[g_chunk] = nil;
        }
        uint32_t c = 0;
        while (c < kMaxChunks && g_chunks[c] != nil) {
            c++;
        }
        if (c == kMaxChunks) {
            return false;
        }
        const uint32_t length = size > kChunkSize ? size : kChunkSize;
        g_chunks[c] = [gx::Device() newBufferWithLength:length options:MTLResourceStorageModeShared];
        if (g_chunks[c] == nil) {
            return false;
        }
        g_chunkUsed[c] = 0;
        g_chunkLive[c] = 0;
        g_chunk = c;
        g_bytes += length;
    }
    *chunk = g_chunk;
    *offset = g_chunkUsed[g_chunk];
    g_chunkUsed[g_chunk] += size;
    return true;
}

// Malloc'd scratch arrays, grown as needed.
template <class T>
struct Scratch {
    T* data = nullptr;
    uint32_t capacity = 0;
    T* Get(uint32_t n) {
        if (n > capacity) {
            capacity = n > capacity * 2 ? n : capacity * 2;
            data = static_cast<T*>(std::realloc(data, capacity * sizeof(T)));
        }
        return data;
    }
};

// Walks a list as GX would, keeping its vertices and triangles.
struct Builder : gx::ListVisitor {
    Scratch<gx::DecodedVertex> decoded;
    Scratch<MeshVertex> vertices;
    Scratch<uint32_t> indices;
    uint32_t numVertices = 0, numIndices = 0;
    bool unsupported = false;
    uint32_t setBase = 0, setStride = 0;  // arrays the list set (bit per attribute)
    uint32_t inheritedBases = 0, inheritedStrides = 0;
    uint32_t inheritedBase[4] = {}, inheritedStride[4] = {};
    uint8_t numWrites = 0;
    uint8_t writeReg[16];
    uint32_t writeValue[16];
    uint32_t flags = 0;
    MatrixRows rows = {63, 0, 93, 0};

    void Start() {
        numVertices = numIndices = 0;
        unsupported = false;
        setBase = setStride = inheritedBases = inheritedStrides = 0;
        numWrites = 0;
        rows = {63, 0, 93, 0};
        const uint32_t* desc = g_state.vtxDesc;
        flags = (desc[gx::kNrm] != 0 ? kMeshHasNormals : 0) | (desc[gx::kPnMtxIdx] == 0 ? kMeshCurrentMtx : 0);
        // Position, normal, color 0 and texture coordinate 0 (and a matrix index) only.
        for (uint32_t a = gx::kTex0MtxIdx; a < gx::kPosMtxArray; a++) {
            const bool allowed = a == gx::kPos || a == gx::kNrm || a == gx::kClr0 || a == gx::kTex0;
            if (desc[a] != 0 && !allowed) {
                unsupported = true;
            }
        }
        if (desc[gx::kPos] == 0) {
            unsupported = true;
        }
    }

    uint32_t Draw(uint32_t prim, uint32_t fmt, const uint8_t* data, uint32_t count) override {
        // Arrays read before the list set them come from whatever was set before.
        static const uint32_t kAttrs[4] = {gx::kPos, gx::kNrm, gx::kClr0, gx::kTex0};
        for (uint32_t i = 0; i < 4; i++) {
            const uint32_t a = kAttrs[i], bit = 1u << a;
            if (g_state.vtxDesc[a] < 2) {
                continue;
            }
            if (!(setBase & bit) && !(inheritedBases & bit)) {
                inheritedBases |= bit;
                inheritedBase[i] = g_state.arrayBase[a];
            }
            if (!(setStride & bit) && !(inheritedStrides & bit)) {
                inheritedStrides |= bit;
                inheritedStride[i] = g_state.arrayStride[a];
            }
        }
        gx::DecodedVertex* dv = decoded.Get(count);
        const uint32_t consumed = gx::DecodeVertices(fmt, data, count, dv);
        if (unsupported || !gx::Triangles(prim) || count == 0) {
            return consumed;  // (lines and points: GX doesn't draw them either)
        }
        if (numVertices + count > (1u << 22)) {
            unsupported = true;
            return consumed;
        }
        MeshVertex* out = vertices.Get(numVertices + count) + numVertices;
        for (uint32_t i = 0; i < count; i++) {
            const gx::DecodedVertex& v = dv[i];
            MeshVertex& m = out[i];
            std::memcpy(m.pos, v.pos, sizeof(m.pos));
            m.mtx = (uint8_t)v.posMtx;
            m.pad[0] = m.pad[1] = m.pad[2] = 0;
            std::memcpy(m.nrm, v.nrm, sizeof(m.nrm));
            if (!v.hasNrm) {
                m.nrm[0] = m.nrm[1] = m.nrm[2] = 0.0f;
            }
            for (int c = 0; c < 4; c++) {
                m.color[c] = (uint8_t)std::lround(v.color[0][c] * 255.0f);  // (n / 255 exactly)
            }
            m.st[0] = v.tex[0][0];
            m.st[1] = v.tex[0][1];
            if (!(flags & kMeshCurrentMtx)) {
                const uint32_t row = v.posMtx & 63, nrm = (v.posMtx & 31) * 3;
                rows.lo = row < rows.lo ? row : rows.lo;
                rows.hi = row > rows.hi ? row : rows.hi;
                rows.nrmLo = nrm < rows.nrmLo ? nrm : rows.nrmLo;
                rows.nrmHi = nrm > rows.nrmHi ? nrm : rows.nrmHi;
            }
        }
        uint32_t* idx = indices.Get(numIndices + count * 3 + 6) + numIndices;
        numIndices += gx::TriangleIndices<uint32_t>(prim, count, numVertices, idx);
        numVertices += count;
        return consumed;
    }

    void SetsArray(uint32_t reg, uint32_t value) override {
        const uint32_t bit = 1u << (gx::kPos + (reg & 15));
        if (reg < 0xB0) {
            setBase |= bit;
        } else {
            setStride |= bit;
        }
        if (numWrites == 16) {
            unsupported = true;
            return;
        }
        writeReg[numWrites] = (uint8_t)reg;
        writeValue[numWrites] = value;
        numWrites++;
    }

    void LoadsXf() override { unsupported = true; }
    void Unknown() override { unsupported = true; }
} g_builder;

// Makes the list's array writes, as running it does.
void ApplyWrites(const Mesh& mesh) {
    for (uint32_t i = 0; i < mesh.numWrites; i++) {
        const uint32_t reg = mesh.writeReg[i], value = mesh.writeValue[i];
        if (reg < 0xB0) {
            g_state.arrayBase[gx::kPos + (reg - 0xA0)] = value;
        } else {
            g_state.arrayStride[gx::kPos + (reg - 0xB0)] = value & 0xFF;
        }
    }
}

bool InheritedSame(const Mesh& mesh) {
    static const uint32_t kAttrs[4] = {gx::kPos, gx::kNrm, gx::kClr0, gx::kTex0};
    for (uint32_t i = 0; i < 4; i++) {
        const uint32_t a = kAttrs[i], bit = 1u << a;
        if (((mesh.inheritedBases & bit) && g_state.arrayBase[a] != mesh.inheritedBase[i]) ||
            ((mesh.inheritedStrides & bit) && g_state.arrayStride[a] != mesh.inheritedStride[i])) {
            return false;
        }
    }
    return true;
}

uint32_t g_budgetFrame;
uint64_t g_budgetUsed;

uint64_t NowNanos() { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// Walks the list (making its writes) into g_builder; false if it can't be
// drawn natively.
bool Build(const uint8_t* list, uint32_t size) {
    g_builder.Start();
    gx::ScanDisplayList(list, size, g_builder);
    return !g_builder.unsupported && g_builder.numIndices != 0;
}

}  // namespace

Mesh* FindMesh(const uint8_t* list, uint32_t size, bool unlimited, bool* overBudget) {
    *overBudget = false;
    const uint32_t addr = PTR_TO_ADDR(list);
    const uint64_t format = FormatKey();
    const uint64_t guard = Guard(list, size);
    Mesh* found = g_meshes.Find(addr);
    if (found != nullptr && !found->stale && found->size == size && found->format == format &&
        found->guard == guard && InheritedSame(*found)) {
        if (found->gxOnly) {
            return nullptr;
        }
        ApplyWrites(*found);
        return found;
    }
    if (gx::EfbFrame() != g_budgetFrame) {
        g_budgetFrame = gx::EfbFrame();
        g_budgetUsed = 0;
    }
    if (!unlimited && g_budgetUsed >= kBudgetNanos) {
        *overBudget = true;
        return nullptr;
    }
    const uint64_t start = NowNanos();
    const bool drawable = Build(list, size);
    Mesh& mesh = g_meshes.Insert(addr);
    Release(mesh);
    mesh.size = size;
    mesh.format = format;
    mesh.guard = guard;
    mesh.stale = false;
    mesh.gxOnly = !drawable;
    const Builder& b = g_builder;
    mesh.inheritedBases = b.inheritedBases;
    mesh.inheritedStrides = b.inheritedStrides;
    std::memcpy(mesh.inheritedBase, b.inheritedBase, sizeof(mesh.inheritedBase));
    std::memcpy(mesh.inheritedStride, b.inheritedStride, sizeof(mesh.inheritedStride));
    mesh.numWrites = b.numWrites;
    std::memcpy(mesh.writeReg, b.writeReg, sizeof(mesh.writeReg));
    std::memcpy(mesh.writeValue, b.writeValue, sizeof(mesh.writeValue));
    mesh.flags = b.flags;
    mesh.rows = b.rows;
    if (drawable) {
        mesh.index32 = b.numVertices > 0xFFFF;
        const uint32_t vertexBytes = b.numVertices * (uint32_t)sizeof(MeshVertex);
        const uint32_t indexBytes = b.numIndices * (mesh.index32 ? 4u : 2u);
        uint32_t chunk, offset;
        if (Allocate(vertexBytes + 16 + indexBytes, &chunk, &offset)) {
            uint8_t* base = static_cast<uint8_t*>(g_chunks[chunk].contents);
            mesh.chunk = chunk;
            mesh.vertexOffset = offset;
            mesh.indexOffset = (offset + vertexBytes + 15) & ~15u;
            mesh.numVertices = b.numVertices;
            mesh.numIndices = b.numIndices;
            std::memcpy(base + mesh.vertexOffset, b.vertices.data, vertexBytes);
            if (mesh.index32) {
                std::memcpy(base + mesh.indexOffset, b.indices.data, indexBytes);
            } else {
                uint16_t* idx = reinterpret_cast<uint16_t*>(base + mesh.indexOffset);
                for (uint32_t i = 0; i < b.numIndices; i++) {
                    idx[i] = (uint16_t)b.indices.data[i];
                }
            }
            g_chunkLive[chunk]++;
        } else {
            mesh.gxOnly = true;
        }
    }
    const uint64_t took = NowNanos() - start;
    g_budgetUsed += took;
    if (gx::NativeCounts* counts = gx::EfbNativeCounts()) {
        counts->conversions++;
        counts->convertNanos += took;
        counts->bytes = g_bytes;
    }
    return mesh.gxOnly ? nullptr : &mesh;
}

void CheckMesh(const Mesh& mesh, const uint8_t* list) {
    static uint32_t reported;
    if (!Build(list, mesh.size) || reported >= 20) {
        return;
    }
    const Builder& b = g_builder;
    const uint8_t* base = static_cast<const uint8_t*>(g_chunks[mesh.chunk].contents);
    bool same = b.numVertices == mesh.numVertices && b.numIndices == mesh.numIndices &&
                std::memcmp(base + mesh.vertexOffset, b.vertices.data, b.numVertices * sizeof(MeshVertex)) == 0;
    for (uint32_t i = 0; same && i < b.numIndices; i++) {
        const uint32_t cached = mesh.index32 ? reinterpret_cast<const uint32_t*>(base + mesh.indexOffset)[i]
                                             : reinterpret_cast<const uint16_t*>(base + mesh.indexOffset)[i];
        same = cached == b.indices.data[i];
    }
    if (!same) {
        reported++;
        std::fprintf(stderr, "frontline: mesh for display list %#x (%u bytes) is stale: its data changed\n", mesh.list,
                     mesh.size);
    }
}

bool DirectData() {
    for (uint32_t a = gx::kPos; a < gx::kPosMtxArray; a++) {
        if (g_state.vtxDesc[a] == 1) {
            return true;
        }
    }
    return false;
}

bool TransientMesh(const uint8_t* list, uint32_t size, Mesh* mesh, id<MTLBuffer> __strong* buffer) {
    const uint64_t start = NowNanos();
    if (!Build(list, size)) {
        return false;
    }
    const Builder& b = g_builder;
    std::memset(mesh, 0, sizeof(*mesh));
    mesh->list = PTR_TO_ADDR(list);
    mesh->size = size;
    mesh->chunk = kMaxChunks;
    mesh->flags = b.flags;
    mesh->rows = b.rows;
    mesh->numVertices = b.numVertices;
    mesh->numIndices = b.numIndices;
    mesh->index32 = b.numVertices > 0xFFFF;
    // Vertices, then triangles, in one upload.
    static Scratch<uint8_t> blob;
    const uint32_t vertexBytes = b.numVertices * (uint32_t)sizeof(MeshVertex);
    const uint32_t indexAt = (vertexBytes + 15) & ~15u;
    const uint32_t indexBytes = b.numIndices * (mesh->index32 ? 4u : 2u);
    uint8_t* data = blob.Get(indexAt + indexBytes);
    std::memcpy(data, b.vertices.data, vertexBytes);
    if (mesh->index32) {
        std::memcpy(data + indexAt, b.indices.data, indexBytes);
    } else {
        uint16_t* idx = reinterpret_cast<uint16_t*>(data + indexAt);
        for (uint32_t i = 0; i < b.numIndices; i++) {
            idx[i] = (uint16_t)b.indices.data[i];
        }
    }
    uint32_t offset;
    *buffer = gx::EfbUpload(data, indexAt + indexBytes, &offset);
    if (*buffer == nil) {
        return false;
    }
    mesh->vertexOffset = offset;
    mesh->indexOffset = offset + indexAt;
    if (gx::NativeCounts* counts = gx::EfbNativeCounts()) {
        counts->transients++;
        counts->convertNanos += NowNanos() - start;
    }
    return true;
}

void ForgetMesh(uint32_t list) {
    if (Mesh* mesh = g_meshes.Find(list)) {
        Release(*mesh);
        mesh->stale = true;
    }
}

void ForgetMeshes() {
    if (g_meshes.entries != nullptr) {
        std::memset(g_meshes.entries, 0, (g_meshes.mask + 1) * sizeof(Mesh));
        g_meshes.count = 0;
    }
    for (uint32_t c = 0; c < kMaxChunks; c++) {
        g_chunks[c] = nil;
        g_chunkUsed[c] = g_chunkLive[c] = 0;
    }
    g_chunk = kMaxChunks;
    g_bytes = 0;
}

id<MTLBuffer> MeshBuffer(const Mesh& mesh) { return g_chunks[mesh.chunk]; }

}  // namespace frontline::mesh
