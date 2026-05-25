#pragma once

//==============================================================================
// Lulzfish Transposition Table (Baseline)
//==============================================================================

#include "lulzfish/core/types.hpp"

#include <cstdint>
#include <vector>

namespace lulzfish::search {

struct TTEntry {
    core::Key key = 0;
    int16_t score = 0;
    uint8_t depth = 0;
    uint8_t flag = 0; // 0=exact, 1=lower, 2=upper
    core::Move best_move = 0;
};

class TranspositionTable {
public:
    explicit TranspositionTable(size_t mb = 16);

    void clear();
    void store(core::Key key, int score, int depth, int flag, core::Move move);
    TTEntry* probe(core::Key key);

private:
    std::vector<TTEntry> table;
    size_t mask;
};

} // namespace lulzfish::search
