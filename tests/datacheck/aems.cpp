// AEMS sound event files: the event system (.aem) and module banks (.abk).

#include "aems.h"

#include <cstring>
#include <set>
#include <vector>

#include "check.h"
#include "types.h"
#include "ea/snd/aems.h"
#include "ea/snd/sndi.h"

namespace {

// aemscompinfo[type]: static size, number of inputs read through in[] (0 if any).
const u8 kStaticSize[33] = {0,    0x0C, 0x0C, 0x0C, 0x14, 0x1C, 0x18, 0x18, 0x1C, 0x10, 0x10,
                            0x14, 0x10, 0x14, 0x10, 0x0C, 0x0C, 0x18, 0x10, 0x10, 0x10, 0x10,
                            0x0C, 0x10, 0x10, 0x14, 0,    0,    0x0C, 0x14, 0x20, 0x0C, 0x0C};

// A sample bank ('BNKb') of `size` bytes, after the native swap: walk every
// patch's tags as SNDI_parsetimbre does and check what the swap touched.
bool CheckSampleBank(BANKVER5* bank, u32 size, AemsStats& stats) {
    u8* base = (u8*)bank;
    CHECK(SNDBankHeaderFromBigEndian(bank), "sample bank magic %.4s", (char*)base);
    CHECK(!SNDBankHeaderFromBigEndian(bank), "sample bank swapped twice");
    u32 header = bank->headerSize;
    CHECK(header + bank->sampleSize <= size && header >= 0x14 + bank->numPatches * 4u, "sample bank %#x + %#x",
          header, bank->sampleSize);
    SNDBankFromBigEndian(bank);
    auto inHeader = [&](const u8* p, size_t n) { return p >= base && p + n <= base + header; };
    for (int i = 0; i < bank->numPatches; i++) {
        if (bank->patchOffsets[i] == 0) {
            continue;
        }
        u8* patch = (u8*)&bank->patchOffsets[i] + bank->patchOffsets[i];
        CHECK(inHeader(patch, 4) && patch[0] == 'P' && patch[1] == 'T', "patch %d", i);
        SNDTAGINFO info;
        info.ptr = patch + 4;
        const u8* env = nullptr;
        int envSegs = 1, channels = 1;
        for (;;) {
            CHECK(inHeader(info.ptr, 1), "patch %d tags run off the header", i);
            while (*info.ptr == 0xFC) {
                info.ptr++;
            }
            info.tag = *info.ptr;
            if (info.tag == 0xFF || info.tag == 0xFE) {
                info.ptr++;
                if (env != nullptr) {
                    CHECK(inHeader(env, envSegs * 8), "patch %d envelope", i);
                    for (int k = 0; k < envSegs; k++) {
                        const SNDENVSEG& seg = ((const SNDENVSEG*)env)[k];
                        CHECK(seg.level >= 0 && seg.level <= 0x7F && (seg.time >= 0 || seg.time == -1),
                              "patch %d envelope %d: %d at %d", i, k, seg.level, seg.time);
                    }
                    stats.sampleEnvelopes++;
                }
                stats.timbres++;
                if (info.tag == 0xFF) {
                    break;
                }
                env = nullptr;
                envSegs = channels = 1;
                continue;
            }
            info.ptr++;
            if (info.tag == 0xFD) {
                continue;
            }
            info.size = *info.ptr;
            if (info.size == 0xFF) {
                info.size = SNDBankGetb(info.ptr + 1, 4);
                info.ptr += 4;
            }
            info.ptr++;
            u8* data = info.ptr;
            CHECK(inHeader(data, info.size), "patch %d tag %#x size %u", i, info.tag, info.size);
            int value = info.size <= 4 ? SNDBankGetb(data, info.size) : 0;
            switch (info.tag) {
            case 0x19:
                env = data + value;
                break;
            case 0x09:
                envSegs = value;
                CHECK(envSegs > 0 && envSegs < 64, "patch %d %d envelope segments", i, envSegs);
                break;
            case 0x82:
                channels = value;
                CHECK(channels >= 1 && channels <= 3, "patch %d %d channels", i, channels);
                break;
            case 0x8F:
            case 0x90:
            case 0x91: {
                CHECK(info.size == 0x21, "patch %d ADPCM tag size %u", i, info.size);
                // DSP ADPCM coefficients are 4.11 fixed point, in practice well inside +-2.0.
                const s16* coef = (const s16*)data;
                for (int k = 0; k < 16; k++) {
                    CHECK(coef[k] > -0x2000 && coef[k] < 0x2000, "patch %d coefficient %d = %d", i, k, coef[k]);
                }
                stats.adpcm++;
                break;
            }
            }
            info.ptr += info.size;
        }
        stats.patches++;
    }
    stats.sampleBanks++;
    return true;
}

}  // namespace

bool CheckAEMSystem(const std::vector<uint8_t>& file, AemsStats& stats) {
    auto* sys = static_cast<AEMSSYSTEM*>(ArenaCopy(file));
    u8* base = (u8*)sys;
    auto inFile = [&](u32 off, size_t size) { return off + size <= file.size(); };
    CHECK(AEMSSystemFromBigEndian(sys), "magic %02x %02x", base[0], base[1]);
    CHECK(!AEMSSystemFromBigEndian(sys), "swapped twice");
    CHECK(sys->numParams >= 0 && inFile(sys->params.addr(), sys->numParams * sizeof(AEMSPARAM)), "params");
    CHECK(sys->numTrigs >= 0 && inFile(sys->trigs.addr(), sys->numTrigs * sizeof(AEMSTRIG)), "trigs");
    CHECK(sys->numEvents >= 0 && inFile(sys->events.addr(), sys->numEvents * sizeof(AEMSEVENT)), "events");
    CHECK(sys->numGlobals >= 0 && inFile(sys->globals.addr(), sys->numGlobals * sizeof(AEMSLINKLIST)), "globals");
    auto* params = (AEMSPARAM*)(base + sys->params.addr());
    for (int i = 0; i < sys->numParams; i++) {
        CHECK(params[i].min <= params[i].max, "param %d range %d..%d", i, params[i].min, params[i].max);
        stats.banksReferenced.insert(params[i].bankId);
    }
    auto* trigs = (AEMSTRIG*)(base + sys->trigs.addr());
    for (int i = 0; i < sys->numTrigs; i++) {
        stats.banksReferenced.insert(trigs[i].bankId);
    }
    auto checkList = [&](const AEMSLINKLIST& list, int limit) {
        if (list.count == 0) {
            return true;
        }
        CHECK(list.count > 0 && inFile(list.indices.addr(), list.count * 4), "list of %d", list.count);
        const s32* idx = (const s32*)(base + list.indices.addr());
        for (int k = 0; k < list.count; k++) {
            CHECK(idx[k] >= 0 && idx[k] < limit, "index %d of %d", idx[k], limit);
        }
        return true;
    };
    auto* events = (AEMSEVENT*)(base + sys->events.addr());
    for (int i = 0; i < sys->numEvents; i++) {
        AEMSEVENT& ev = events[i];
        CHECK(ev.numParams >= 0 && ev.numParams < 64, "event %d params %d", i, ev.numParams);
        CHECK(ev.numParams == 0 || inFile(ev.params.addr(), ev.numParams * sizeof(AEMSLINKLIST)), "event %d", i);
        for (int j = 0; j < ev.numParams; j++) {
            if (!checkList(((AEMSLINKLIST*)(base + ev.params.addr()))[j], sys->numParams)) {
                return false;
            }
        }
        if (!checkList(ev.begin, sys->numTrigs) || !checkList(ev.update, sys->numTrigs) ||
            !checkList(ev.end, sys->numTrigs)) {
            return false;
        }
    }
    for (int i = 0; i < sys->numGlobals; i++) {
        if (!checkList(((AEMSLINKLIST*)(base + sys->globals.addr()))[i], sys->numParams)) {
            return false;
        }
    }
    stats.systems++;
    stats.events += sys->numEvents;
    return true;
}

bool CheckAEMSBank(const std::vector<uint8_t>& file, AemsStats& stats) {
    auto* bank = static_cast<MODULEBANK*>(ArenaCopy(file));
    u8* base = (u8*)bank;
    CHECK(AEMSBankHeaderFromBigEndian(bank), "magic %02x %02x", base[0], base[1]);
    CHECK(!AEMSBankHeaderFromBigEndian(bank), "swapped twice");
    CHECK(bank->version >= 5 && bank->version <= 9, "version %d", bank->version);
    CHECK(bank->size == (s32)file.size(), "size %d vs %zu", bank->size, file.size());
    CHECK(bank->rt.addr() == 0, "rt %#x", bank->rt.addr());
    u32 headerSize = bank->size - (bank->sfxBankSize + bank->midiBankSize);
    CHECK(bank->sfxBankOffset == 0 || (u32)bank->sfxBankOffset == headerSize, "sample bank at %#x, header %#x",
          bank->sfxBankOffset, headerSize);
    CHECK(bank->midiBankOffset == 0 || bank->midiBankOffset + bank->midiBankSize <= bank->size, "midi bank");
    auto inHeader = [&](u32 off, size_t size) { return off + size <= headerSize; };
    if (bank->sfxBankOffset != 0 && !CheckSampleBank((BANKVER5*)(base + bank->sfxBankOffset), bank->sfxBankSize,
                                                     stats)) {
        return false;
    }
    stats.midiBanks += bank->midiBankOffset != 0;

    AEMSBankFromBigEndian(bank);
    int moduleSize = bank->version >= 8 ? 0x1C : 0x14;
    CHECK(bank->numModules > 0 && inHeader(bank->modules.addr(), bank->numModules * moduleSize), "%d modules",
          bank->numModules);
    int maxStream = -1;
    for (int m = 0; m < bank->numModules; m++) {
        auto* module = (MODULE*)(base + bank->modules.addr() + m * moduleSize);
        CHECK(module->rt.addr() == 0 && module->numInstances > 0 && module->numInstances < 256,
              "module %d instances %d", m, module->numInstances);
        CHECK(module->numComps > 0 && module->updateType >= 0 && module->updateType <= 1, "module %d", m);
        std::vector<AEMSCOMP*> comps;
        u32 off = module->comps.addr();
        for (int c = 0; c < module->numComps; c++) {
            CHECK(inHeader(off, 0xC), "module %d comp %d", m, c);
            auto* comp = (AEMSCOMP*)(base + off);
            CHECK(comp->type < 33 && kStaticSize[comp->type] != 0, "module %d comp %d type %d", m, c, comp->type);
            CHECK(inHeader(off, kStaticSize[comp->type]), "module %d comp %d", m, c);
            comps.push_back(comp);
            off += kStaticSize[comp->type];
            stats.types.insert(comp->type);
        }
        if (bank->version >= 8) {
            CHECK(comps.back()->type == AEMSCOMP_DESTROY, "module %d ends with type %d", m, comps.back()->type);
            CHECK(module->numExec <= module->numComps && module->numLinks >= 0 && module->numLinks < 4096,
                  "module %d exec %d links %d", m, module->numExec, module->numLinks);
        }
        int links = 0;
        for (int c = 0; c < module->numComps; c++) {
            AEMSCOMP* comp = comps[c];
            s32* p = comp->p;
            CHECK(comp->numOutputs == 0 || inHeader(comp->outputs.addr(), comp->numOutputs * sizeof(AEMSOUTPUT)),
                  "module %d comp %d outputs", m, c);
            for (int k = 0; k < comp->numOutputs; k++) {
                AEMSOUTPUT& out = ((AEMSOUTPUT*)(base + comp->outputs.addr()))[k];
                CHECK(out.count >= 0 && inHeader(out.targets.addr(), out.count * sizeof(AEMSTARGET)),
                      "module %d comp %d output %d", m, c, k);
                for (int t = 0; t < out.count; t++) {
                    AEMSTARGET& target = ((AEMSTARGET*)(base + out.targets.addr()))[t];
                    CHECK(target.comp < module->numComps && target.input < comps[target.comp]->numInputs,
                          "module %d comp %d -> %d.%d", m, c, target.comp, target.input);
                }
                if (comp->type != AEMSCOMP_STATIC) {
                    links += out.count;
                }
            }
            switch (comp->type) {
            case AEMSCOMP_OSCILLATOR:
                CHECK(((s16*)p)[0] >= 0 && ((s16*)p)[0] <= 3 && (((s16*)p)[1] & ~1) == 0, "oscillator %d %d",
                      ((s16*)p)[0], ((s16*)p)[1]);
                break;
            case AEMSCOMP_ENVELOPE: {
                s16 count = ((s16*)p)[2];
                CHECK(count > 0 && count < 256 && inHeader(p[0], count * sizeof(SNDENVSEG)), "envelope %d", count);
                auto* points = (SNDENVSEG*)(base + p[0]);
                for (int i = 0; i < count; i++) {
                    CHECK(points[i].time >= 0 && points[i].time < 600000, "envelope time %d", points[i].time);
                }
                stats.envelopes++;
                break;
            }
            case AEMSCOMP_COUNTER:
                CHECK(p[0] <= p[1] && ((u8*)&p[2])[0] <= 1, "counter %d..%d %d", p[0], p[1], ((u8*)&p[2])[0]);
                break;
            case AEMSCOMP_RANDOM: {
                u8 mode = ((u8*)&p[3])[0];
                CHECK(p[0] <= p[1] && mode <= 2, "random %d..%d mode %d", p[0], p[1], mode);
                // One bank's weights run past the module data into its sample bank; the
                // game stops reading once the running sum passes its roll (< 100).
                CHECK(mode != 0 || p[2] + (p[1] - p[0] + 1) <= bank->size, "random weights at %#x for %d..%d",
                      p[2], p[0], p[1]);
                break;
            }
            case AEMSCOMP_TABLE: {
                s16 size = ((s16*)p)[2];
                CHECK(size > 0 && inHeader(p[0], size * 2) && ((s16*)p)[3] <= ((s16*)p)[4], "table %d", size);
                stats.tables++;
                break;
            }
            case AEMSCOMP_TABLE2:
                CHECK((p[4] == 1 || p[4] == 2 || p[4] == 4) && p[1] > 0 && inHeader(p[0], p[1] * p[4]) &&
                          p[2] <= p[3],
                      "table2 %d x %d", p[1], p[4]);
                stats.tables++;
                break;
            case AEMSCOMP_STATEGEN:
                CHECK(p[0] > 0 && inHeader(p[1], p[0] * 2), "stategen %d", p[0]);
                break;
            case AEMSCOMP_PLAYER: {
                CHECK(inHeader(p[0], sizeof(AEMSPLAYERTABLE)), "player table");
                auto* table = (AEMSPLAYERTABLE*)(base + p[0]);
                CHECK(table->count > 0 && inHeader(table->entries.addr(), table->count * 4), "player %d",
                      table->count);
                for (int k = 0; k < table->count; k++) {
                    u32 entry = ((u32*)(base + table->entries.addr()))[k];
                    CHECK(entry >= 0x4000 && entry <= 0xFFFF, "player entry %#x", entry);
                    if (entry >= 0xC000) {
                        maxStream = std::max(maxStream, (int)(entry & 0x3FFF));
                        stats.streamPlayers++;
                    } else if (entry < 0x8000) {
                        stats.bankPlayers++;
                    }
                }
                break;
            }
            default:
                break;
            }
        }
        if (bank->version >= 8) {
            CHECK(links <= module->numLinks, "module %d links %d > %d", m, links, module->numLinks);
        }
        stats.modules++;
        stats.comps += module->numComps;
    }
    if (maxStream >= 0) {
        CHECK(bank->streams.addr() != 0 &&
                  inHeader(bank->streams.addr(), (maxStream + 1) * sizeof(AEMSSTREAMENTRY)),
              "%d streams", maxStream + 1);
        for (int i = 0; i <= maxStream; i++) {
            auto& s = ((AEMSSTREAMENTRY*)(base + bank->streams.addr()))[i];
            CHECK(s.offset >= 0 && (s.flags & ~1) == 0, "stream %d offset %d flags %#x", i, s.offset, s.flags);
        }
    }
    stats.banks++;
    return true;
}
