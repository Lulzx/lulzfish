#include "search.hpp"

#include "transposition.hpp"
#include "lulzfish/eval/graph_eval.hpp"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <vector>

using namespace lulzfish::core;

namespace lulzfish::search {

static constexpr int MATE = 30000;
static constexpr int INF  = 31000;

static TranspositionTable tt(16); // 16MB TT

// Simple history and killer tables for move ordering (on top of SEE)
static int history[64][64] = {};
static Move killers[2][2] = {};  // [ply % 2][slot]

int qsearch(Position& pos, int alpha, int beta) {
    bool in_check = pos.is_check();

    int stand_pat = lulzfish::eval::graph::evaluate(pos);

    if (!in_check) {
        if (stand_pat >= beta) return beta;
        if (stand_pat > alpha) alpha = stand_pat;
    }

    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        if (in_check) return -MATE;
        return stand_pat;
    }

    // Collect forcing moves and sort by SEE / capture value (best first)
    std::vector<std::pair<int, Move>> forcing;
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        Square to = to_sq(m);
        bool is_capture = (pos.piece_on(to) != Piece::None) || is_en_passant(m);
        bool is_promo = is_promotion(m);

        if (in_check || is_capture || is_promo) {
            int val = lulzfish::core::capture_value(pos, m);
            val += lulzfish::core::see(pos, to);  // add SEE
            forcing.emplace_back(-val, m); // negative for descending sort
        }
    }

    std::sort(forcing.begin(), forcing.end()); // best (highest SEE) first

    StateInfo undo;

    for (auto& p : forcing) {
        Move m = p.second;

        pos.make_move(m, undo);
        int score = -qsearch(pos, -beta, -alpha);
        pos.unmake_move(m, undo);

        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    return alpha;
}

int alpha_beta(Position& pos, int depth, int alpha, int beta) {
    if (depth <= 0) {
        return qsearch(pos, alpha, beta);
    }

    bool in_check = pos.is_check();

    // TT probe (very basic)
    TTEntry* entry = tt.probe(pos.key());
    if (entry && entry->depth >= depth) {
        if (entry->flag == 0) return entry->score;
    }

    // Null move pruning (simple version for efficiency)
    if (!in_check && depth >= 3) {
        StateInfo undo;
        pos.make_null_move(undo);
        int score = -alpha_beta(pos, depth - 1 - 2, -beta, -beta + 1); // R=2
        pos.unmake_null_move(undo);
        if (score >= beta) return beta;
    }

    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        if (in_check) {
            return -MATE + (10 - depth);
        }
        return 0;
    }

    // Basic ordering using SEE + capture value + killers + history
    std::vector<std::pair<int, Move>> ordered;
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        int val = lulzfish::core::capture_value(pos, m) + lulzfish::core::see(pos, to_sq(m));

        // Killer bonus (very high priority)
        if (m == killers[0][0] || m == killers[0][1]) val += 10000;
        if (m == killers[1][0] || m == killers[1][1]) val += 9000;

        // History bonus
        Square f = from_sq(m);
        Square t = to_sq(m);
        val += history[f][t] / 4;

        ordered.emplace_back(-val, m);
    }
    std::sort(ordered.begin(), ordered.end());

    int best = -INF;
    Move best_move = 0;

    StateInfo undo;
    for (size_t i = 0; i < ordered.size(); ++i) {
        Move m = ordered[i].second;
        pos.make_move(m, undo);

        int new_depth = depth - 1;
        // Basic Late Move Reduction (LMR)
        if (i >= 4 && depth >= 3 && !is_promotion(m) && ! (pos.piece_on(to_sq(m)) != Piece::None)) {
            new_depth -= 1;
        }

        int score = -alpha_beta(pos, new_depth, -beta, -alpha);

        // If reduced search fails high, re-search at full depth
        if (new_depth < depth - 1 && score > alpha) {
            score = -alpha_beta(pos, depth - 1, -beta, -alpha);
        }

        pos.unmake_move(m, undo);

        if (score > best) {
            best = score;
            best_move = m;
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    int flag = 0;
    tt.store(pos.key(), best, depth, flag, best_move);

    // Update killers and history on good cutoff / best move
    if (best_move) {
        killers[depth % 2][1] = killers[depth % 2][0];
        killers[depth % 2][0] = best_move;

        Square f = from_sq(best_move);
        Square t = to_sq(best_move);
        history[f][t] += depth * depth;
        if (history[f][t] > 100000) history[f][t] = 100000;
    }

    return best;
}

SearchResult search_root(Position& pos, SearchLimits limits) {
    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        return {pos.is_check() ? -MATE : 0, MOVE_NONE};
    }

    std::vector<std::pair<int, Move>> ordered;
    ordered.reserve(static_cast<size_t>(moves.size()));
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        int val = lulzfish::core::capture_value(pos, m) + lulzfish::core::see(pos, to_sq(m));
        ordered.emplace_back(-val, m);
    }
    std::sort(ordered.begin(), ordered.end());

    int alpha = -INF;
    int beta = INF;
    int best_score = -INF;
    Move best_move = ordered.front().second;
    int child_depth = std::max(0, limits.depth - 1);

    StateInfo undo;
    for (const auto& entry : ordered) {
        Move move = entry.second;
        pos.make_move(move, undo);
        int score = -alpha_beta(pos, child_depth, -beta, -alpha);
        pos.unmake_move(move, undo);

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    tt.store(pos.key(), best_score, limits.depth, 0, best_move);
    return {best_score, best_move};
}

int search(Position& pos, SearchLimits limits) {
    return search_root(pos, limits).score;
}

// Basic training stub: loads selfplay data and computes a simple bias.
// This can be used to adjust the graph eval (placeholder for linear model on features).
static double g_eval_bias = 0.0;

void train_from_selfplay(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cout << "No training data found at " << filename << "\n";
        return;
    }

    std::string line;
    int count = 0;
    long long total_score = 0;
    while (std::getline(file, line)) {
        size_t pos1 = line.find(" | ");
        if (pos1 != std::string::npos) {
            size_t pos2 = line.find(" | ", pos1 + 3);
            if (pos2 != std::string::npos) {
                std::string score_str = line.substr(pos1 + 3, pos2 - pos1 - 3);
                try {
                    int sc = std::stoi(score_str);
                    total_score += sc;
                    count++;
                } catch (...) {}
            }
        }
    }
    file.close();

    if (count > 0) {
        double avg = total_score / (double)count;
        g_eval_bias = avg / 100.0;  // simple bias for eval
        // Apply to graph eval
        lulzfish::eval::graph::set_graph_bias(g_eval_bias);
        // Simple "linear model" stub: bias based on avg score as weight for graph features
        std::cout << "Training stub: Loaded " << count << " positions, avg score " << avg 
                  << ", applied bias " << g_eval_bias << " to graph eval\n";
        std::cout << "  (Simple model: graph features weighted by " << g_eval_bias << ")\n";
    }
}

double get_eval_bias() { return g_eval_bias; }

// Simple bench: run timed perft
void bench(int perft_depth) {
    using namespace lulzfish::core;
    Position pos;
    pos.set_startpos();
    auto start = std::chrono::high_resolution_clock::now();
    // For stub, just time a search
    SearchLimits lim;
    lim.depth = perft_depth;
    search(pos, lim);
    auto end = std::chrono::high_resolution_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "Bench: search at depth " << perft_depth << " took " << ms << " ms\n";
}

// Simple self-play loop that records (FEN, score) pairs to a file.
// This generates training data for future learned search controller or graph net.
void self_play_game(int num_games, int max_depth, int max_moves) {
    using namespace lulzfish::core;

    std::ofstream data_file("selfplay_data.txt", std::ios::app);
    if (!data_file.is_open()) {
        std::cerr << "Warning: Could not open selfplay_data.txt for writing.\n";
    }

    for (int g = 0; g < num_games; ++g) {
        Position pos;
        pos.set_startpos();

        int recorded = 0;

        for (int m = 0; m < max_moves; ++m) {
            SearchLimits lim;
            lim.depth = max_depth;

            SearchResult result = search_root(pos, lim);
            int score = result.score;
            std::string fen = pos.fen();

            MoveList moves;
            generate_legal(pos, moves);
            if (moves.empty()) break;

            Move chosen = result.best_move;

            if (data_file.is_open()) {
                // Record FEN | score | move (simple UCI-ish from internal Move)
                char mf = static_cast<char>('a' + file_of(from_sq(chosen)));
                char mr = static_cast<char>('1' + rank_of(from_sq(chosen)));
                char mtf = static_cast<char>('a' + file_of(to_sq(chosen)));
                char mtr = static_cast<char>('1' + rank_of(to_sq(chosen)));
                std::string move_str = std::string(1, mf) + mr + mtf + mtr;
                data_file << fen << " | " << score << " | " << move_str << "\n";
            }
            recorded++;

            StateInfo undo;
            pos.make_move(chosen, undo);
        }

        std::cout << "Game " << g << " recorded " << recorded << " positions to selfplay_data.txt\n";
    }

    if (data_file.is_open()) {
        data_file.close();
    }
}

} // namespace lulzfish::search
