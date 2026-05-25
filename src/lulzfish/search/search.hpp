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
    int threads = 1;
    // time, nodes, etc. can be added later
};

struct SearchResult {
    int score = 0;
    core::Move best_move = core::MOVE_NONE;
    core::Move pv[128] = {};
    int pv_length = 0;
};

int search(Position& pos, SearchLimits limits);
SearchResult search_root(Position& pos, SearchLimits limits);
void clear_search_state();

// Basic self-play for data recording (foundation for future training of controller or graph net)
void self_play_game(int num_games = 1, int max_depth = 6, int max_moves = 80);

// Simple benchmarking: timed perft or self-play matches
void bench(int perft_depth = 4);

// Training stub for self-play data
void train_from_selfplay(const std::string& filename = "selfplay_data.txt");

} // namespace lulzfish::search
