#include "search.hpp"

#include "transposition.hpp"
#include "lulzfish/eval/graph_eval.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <vector>

using namespace lulzfish::core;

namespace lulzfish::search {

static constexpr int MATE = 30000;
static constexpr int INF  = 31000;
static constexpr int MAX_PLY = 128;
static constexpr int ROOT_KNIGHT_VERIFICATION_PENALTY = 60;
static constexpr int ROOT_CENTER_PAWN_BONUS = 35;
static constexpr int ROOT_WING_PAWN_PENALTY = 30;
static constexpr int ROOT_BLOCKED_C_PAWN_PENALTY = 70;

static TranspositionTable tt(16); // 16MB TT

// Simple history and killer tables for move ordering (on top of SEE)
static int history[64][64] = {};
static Move killers[MAX_PLY][2] = {};  // [ply][slot]

namespace {

bool move_gives_check(Position& pos, Move move) {
    StateInfo undo;
    pos.make_move(move, undo);
    bool gives_check = pos.is_check();
    pos.unmake_move(move, undo);
    return gives_check;
}

int relative_rank(Square sq, Color color) {
    int rank = rank_of(sq);
    return color == Color::White ? rank : 7 - rank;
}

bool has_advanced_center_pawn(const Position& pos, Color color) {
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    while (pawns) {
        Square sq = lsb_square(pawns);
        int file = file_of(sq);
        if ((file == 3 || file == 4) && relative_rank(sq, color) >= 2) {
            return true;
        }
        (void)pop_lsb(pawns);
    }
    return false;
}

bool needs_root_knight_verification(const Position& pos, Move move) {
    Piece mover = pos.piece_on(from_sq(move));
    if (mover == Piece::None || type_of(mover) != PieceType::Knight) return false;
    if (is_promotion(move) || is_en_passant(move) || pos.piece_on(to_sq(move)) != Piece::None) return false;

    Color color = color_of(mover);
    return relative_rank(to_sq(move), color) >= 3 && !has_advanced_center_pawn(pos, color);
}

bool blocks_c_pawn_counterplay(const Position& pos, Move move) {
    Piece mover = pos.piece_on(from_sq(move));
    if (mover == Piece::None || type_of(mover) != PieceType::Knight) return false;

    Color color = color_of(mover);
    if (color == Color::White) {
        return from_sq(move) == B1 && to_sq(move) == C3 &&
               pos.piece_on(C2) == Piece::WhitePawn &&
               pos.piece_on(D4) == Piece::WhitePawn &&
               pos.piece_on(C5) == Piece::BlackPawn;
    }

    return from_sq(move) == B8 && to_sq(move) == C6 &&
           pos.piece_on(C7) == Piece::BlackPawn &&
           pos.piece_on(D5) == Piece::BlackPawn &&
           pos.piece_on(C4) == Piece::WhitePawn;
}

int root_opening_adjustment(const Position& pos, Move move) {
    Piece mover = pos.piece_on(from_sq(move));
    if (mover == Piece::None) return 0;

    if (blocks_c_pawn_counterplay(pos, move)) {
        return -ROOT_BLOCKED_C_PAWN_PENALTY;
    }

    if (type_of(mover) != PieceType::Pawn) return 0;
    if (pos.piece_on(to_sq(move)) != Piece::None || is_en_passant(move) || is_promotion(move)) return 0;

    Color color = color_of(mover);
    bool has_center_pawn = has_advanced_center_pawn(pos, color);

    int from_rel = relative_rank(from_sq(move), color);
    int to_rel = relative_rank(to_sq(move), color);
    if (from_rel != 1 || to_rel < 2) return 0;

    int file = file_of(from_sq(move));
    if (!has_center_pawn && (file == 3 || file == 4)) {
        return to_rel >= 3 ? ROOT_CENTER_PAWN_BONUS : 0;
    }
    if ((file <= 1 || file >= 6) && pos.fullmove_number() <= 10) {
        return -ROOT_WING_PAWN_PENALTY;
    }
    return 0;
}

} // namespace

void clear_search_state() {
    tt.clear();
    for (auto& row : history) {
        for (int& value : row) value = 0;
    }
    for (auto& row : killers) {
        for (Move& move : row) move = MOVE_NONE;
    }
}

int qsearch(Position& pos, int alpha, int beta, int checks_left = 1, int ply = 0) {
    if (ply > 0 && pos.is_repetition()) return 0;

    bool in_check = pos.is_check();

    int stand_pat = lulzfish::eval::graph::evaluate(pos);

    if (!in_check) {
        if (stand_pat >= beta) return beta;
        if (stand_pat > alpha) alpha = stand_pat;
    }

    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        if (in_check) return -MATE + ply;
        return stand_pat;
    }

    // Collect forcing moves and sort by SEE / capture value (best first).
    // A small quiet-check budget catches common one-move mate continuations
    // without turning quiescence into full-width search.
    std::vector<std::pair<int, Move>> forcing;
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        Square to = to_sq(m);
        bool is_capture = (pos.piece_on(to) != Piece::None) || is_en_passant(m);
        bool is_promo = is_promotion(m);
        bool is_quiet_check = false;

        if (!in_check && !is_capture && !is_promo) {
            if (checks_left <= 0) continue;
            is_quiet_check = move_gives_check(pos, m);
            if (!is_quiet_check) continue;
        }

        int val = lulzfish::core::capture_value(pos, m);
        val += lulzfish::core::see(pos, to);  // add SEE
        if (is_quiet_check) val += 40;
        forcing.emplace_back(-val, m); // negative for descending sort
    }

    std::sort(forcing.begin(), forcing.end()); // best (highest SEE) first

    StateInfo undo;

    for (auto& p : forcing) {
        Move m = p.second;
        bool is_capture = (pos.piece_on(to_sq(m)) != Piece::None) || is_en_passant(m);
        bool is_promo = is_promotion(m);

        pos.make_move(m, undo);
        bool gives_check = pos.is_check();
        int next_checks_left = checks_left;
        if (!in_check && gives_check && !is_capture && !is_promo) {
            next_checks_left -= 1;
        }

        int score = -qsearch(pos, -beta, -alpha, next_checks_left, ply + 1);
        pos.unmake_move(m, undo);

        if (score > alpha) alpha = score;
        if (alpha >= beta) break;
    }

    return alpha;
}

int alpha_beta(Position& pos, int depth, int alpha, int beta, int extensions_left, int ply) {
    if (ply > 0 && pos.is_repetition()) return 0;

    if (depth <= 0) {
        return qsearch(pos, alpha, beta, 1, ply);
    }

    bool in_check = pos.is_check();

    int original_alpha = alpha;
    Move tt_move = MOVE_NONE;

    // TT probe
    TTEntry* entry = tt.probe(pos.key());
    if (entry) {
        tt_move = entry->best_move;
        if (entry->depth >= depth) {
            if (entry->flag == 0) return entry->score;
            if (entry->flag == 1 && entry->score >= beta) return entry->score;
            if (entry->flag == 2 && entry->score <= alpha) return entry->score;
        }
    }

    // Null move pruning (simple version for efficiency)
    if (!in_check && depth >= 3) {
        StateInfo undo;
        pos.make_null_move(undo);
        int score = -alpha_beta(pos, depth - 1 - 2, -beta, -beta + 1, extensions_left, ply + 1); // R=2
        pos.unmake_null_move(undo);
        if (score >= beta) return beta;
    }

    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        if (in_check) {
            return -MATE + ply;
        }
        return 0;
    }

    // Basic ordering using SEE + capture value + killers + history
    std::vector<std::pair<int, Move>> ordered;
    int ply_index = std::min(ply, MAX_PLY - 1);
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        int val = lulzfish::core::capture_value(pos, m) + lulzfish::core::see(pos, to_sq(m));

        // Hash move first if it is available for this position.
        if (m == tt_move) val += 20000;

        // Killer bonus for quiet cutoffs seen at this ply.
        if (m == killers[ply_index][0]) val += 10000;
        if (m == killers[ply_index][1]) val += 9000;

        // History bonus
        Square f = from_sq(m);
        Square t = to_sq(m);
        val += history[f][t] / 4;

        ordered.emplace_back(-val, m);
    }
    std::sort(ordered.begin(), ordered.end());

    int best = -INF;
    Move best_move = MOVE_NONE;
    bool best_is_quiet = false;
    bool cutoff = false;

    StateInfo undo;
    for (size_t i = 0; i < ordered.size(); ++i) {
        Move m = ordered[i].second;
        bool is_capture = (pos.piece_on(to_sq(m)) != Piece::None) || is_en_passant(m);
        pos.make_move(m, undo);
        bool gives_check = pos.is_check();

        int new_depth = depth - 1;
        int child_extensions_left = extensions_left;
        if ((in_check || gives_check) && child_extensions_left > 0) {
            ++new_depth;
            --child_extensions_left;
        }

        // Basic Late Move Reduction (LMR)
        if (i >= 4 && depth >= 3 && !in_check && !gives_check && !is_capture && !is_promotion(m)) {
            new_depth -= 1;
        }

        int score = -alpha_beta(pos, new_depth, -beta, -alpha, child_extensions_left, ply + 1);

        // If reduced search fails high, re-search at full depth
        if (new_depth < depth - 1 && score > alpha) {
            score = -alpha_beta(pos, depth - 1, -beta, -alpha, extensions_left, ply + 1);
        }

        pos.unmake_move(m, undo);

        if (score > best) {
            best = score;
            best_move = m;
            best_is_quiet = !is_capture && !is_promotion(m);
        }
        if (score > alpha) alpha = score;
        if (alpha >= beta) {
            cutoff = true;
            break;
        }
    }

    int flag = 0;
    if (best <= original_alpha) flag = 2;
    else if (best >= beta) flag = 1;
    tt.store(pos.key(), best, depth, flag, best_move);

    // Update killers and history on good cutoff / best move
    if (cutoff && best_move && best_is_quiet) {
        killers[ply_index][1] = killers[ply_index][0];
        killers[ply_index][0] = best_move;

        Square f = from_sq(best_move);
        Square t = to_sq(best_move);
        history[f][t] += depth * depth;
        if (history[f][t] > 100000) history[f][t] = 100000;
    }

    return best;
}

namespace {

SearchResult search_root_depth(Position& pos, int depth) {
    MoveList moves;
    generate_legal(pos, moves);

    if (moves.empty()) {
        return {pos.is_check() ? -MATE : 0, MOVE_NONE};
    }

    std::vector<std::pair<int, Move>> ordered;
    ordered.reserve(static_cast<size_t>(moves.size()));
    Move tt_move = MOVE_NONE;
    if (TTEntry* entry = tt.probe(pos.key())) {
        tt_move = entry->best_move;
    }
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        int val = lulzfish::core::capture_value(pos, m) + lulzfish::core::see(pos, to_sq(m));
        if (m == tt_move) val += 20000;
        ordered.emplace_back(-val, m);
    }
    std::sort(ordered.begin(), ordered.end());

    int alpha = -INF;
    int beta = INF;
    int best_score = -INF;
    Move best_move = ordered.front().second;
    int child_depth = std::max(0, depth - 1);

    StateInfo undo;
    for (const auto& entry : ordered) {
        Move move = entry.second;
        bool verify_knight = needs_root_knight_verification(pos, move);
        int extension = verify_knight ? 1 : 0;
        pos.make_move(move, undo);
        int score = -alpha_beta(pos, child_depth + extension, -beta, -alpha, 1, 1);
        pos.unmake_move(move, undo);
        score += root_opening_adjustment(pos, move);
        if (verify_knight) {
            score -= ROOT_KNIGHT_VERIFICATION_PENALTY;
        }

        if (score > best_score) {
            best_score = score;
            best_move = move;
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    tt.store(pos.key(), best_score, depth, 0, best_move);
    return {best_score, best_move};
}

} // namespace

SearchResult search_root(Position& pos, SearchLimits limits) {
    int max_depth = std::max(1, limits.depth);
    SearchResult result;

    // Iterative deepening seeds the TT/hash move for deeper root searches.
    for (int depth = 1; depth <= max_depth; ++depth) {
        result = search_root_depth(pos, depth);
    }

    return result;
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
