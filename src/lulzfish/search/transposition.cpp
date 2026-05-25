#include "transposition.hpp"

#include <algorithm>

namespace lulzfish::search {

TranspositionTable::TranspositionTable(size_t mb) {
    size_t bytes = mb * 1024 * 1024;
    size_t entries = bytes / sizeof(TTEntry);
    // Power of 2
    size_t size = 1;
    while (size * 2 <= entries) size *= 2;
    table.resize(size);
    mask = size - 1;
    clear();
}

void TranspositionTable::clear() {
    std::fill(table.begin(), table.end(), TTEntry{});
}

void TranspositionTable::store(core::Key key, int score, int depth, int flag, core::Move move) {
    size_t idx = key & mask;
    TTEntry& e = table[idx];

    // Always replace for simplicity (can improve with depth/replace strategy)
    e.key = key;
    e.score = static_cast<int16_t>(score);
    e.depth = static_cast<uint8_t>(depth);
    e.flag = static_cast<uint8_t>(flag);
    e.best_move = move;
}

TTEntry* TranspositionTable::probe(core::Key key) {
    size_t idx = key & mask;
    if (table[idx].key == key) {
        return &table[idx];
    }
    return nullptr;
}

} // namespace lulzfish::search
