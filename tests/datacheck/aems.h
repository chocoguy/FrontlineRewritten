#pragma once

#include <cstdint>
#include <set>
#include <vector>

struct AemsStats {
    long systems = 0, events = 0, banks = 0, modules = 0, comps = 0, envelopes = 0, tables = 0;
    long bankPlayers = 0, streamPlayers = 0, midiBanks = 0;
    long sampleBanks = 0, patches = 0, timbres = 0, sampleEnvelopes = 0, adpcm = 0;
    std::set<int> banksReferenced, types;
};

bool CheckAEMSystem(const std::vector<uint8_t>& file, AemsStats& stats);
bool CheckAEMSBank(const std::vector<uint8_t>& file, AemsStats& stats);
