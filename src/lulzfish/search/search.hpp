#pragma once

//==============================================================================
// Lulzfish Search (Minimal Alpha-Beta Baseline)
//==============================================================================

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"

using lulzfish::core::Position;

namespace lulzfish::search {

struct SearchLimits {
    int depth = 4;
    // time, nodes, etc. can be added later
};

int search(Position& pos, SearchLimits limits);

// Basic self-play for data recording (foundation for future training of controller or graph net)
void self_play_game(int num_games = 1, int max_depth = 6, int max_moves = 80);

// Simple benchmarking: timed perft or self-play matches
void bench(int perft_depth = 4);

// Training stub for self-play data
void train_from_selfplay(const std::string& filename = "selfplay_data.txt");

} // namespace lulzfish::search
