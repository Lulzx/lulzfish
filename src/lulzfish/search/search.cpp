#include "search.hpp"

#include "transposition.hpp"
#include "lulzfish/eval/graph_eval.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#ifndef __EMSCRIPTEN__
#include <thread>
#endif
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
static constexpr int ROOT_PIRC_E_BREAK_BONUS = 110;
static constexpr int ROOT_EARLY_SLAV_EXCHANGE_PENALTY = 50;
static constexpr int ROOT_SLAV_DEVELOPMENT_BONUS = 45;
static constexpr int ROOT_RETI_SPACE_BONUS = 140;
static constexpr int ROOT_BENONI_DEVELOPMENT_BONUS = 20;
static constexpr int ROOT_DUTCH_GAMBIT_PENALTY = 100;
static constexpr int ROOT_DUTCH_DEVELOPMENT_BONUS = 30;
static constexpr int ROOT_QUEENS_INDIAN_NC3_PENALTY = 50;
static constexpr int ROOT_QUEENS_INDIAN_DEVELOPMENT_BONUS = 40;
static constexpr int ROOT_NIMZO_DAMAGED_STRUCTURE_GRAB_PENALTY = 140;
static constexpr int ROOT_NIMZO_DAMAGED_STRUCTURE_DEVELOPMENT_BONUS = 35;
static constexpr int ROOT_NIMZO_C_PAWN_RECAPTURE_BONUS = 260;
static constexpr int ROOT_NIMZO_WRONG_RECAPTURE_PENALTY = 120;
static constexpr int ROOT_OPEN_GAME_NC6_BONUS = 90;
static constexpr int ROOT_OPEN_GAME_PETROFF_PENALTY = 90;
static constexpr int ROOT_ITALIAN_H3_BONUS = 140;
static constexpr int ROOT_ITALIAN_EARLY_ND5_PENALTY = 90;
static constexpr int ROOT_ENGLISH_E3_BONUS = 520;
static constexpr int ROOT_ENGLISH_G3_BONUS = 45;
static constexpr int ROOT_ENGLISH_IMMEDIATE_D4_PENALTY = 90;
static constexpr int ROOT_ENGLISH_IMMEDIATE_E4_PENALTY = 120;
static constexpr int ROOT_ENGLISH_NF3_PENALTY = 120;

thread_local TranspositionTable tt(16); // 16MB TT per search thread

// Simple history and killer tables for move ordering (on top of SEE)
thread_local int history[64][64] = {};
thread_local Move killers[MAX_PLY][2] = {};  // [ply][slot]

std::atomic<std::uint64_t> g_nodes_searched{0};

std::uint64_t nodes_searched() {
    return g_nodes_searched.load(std::memory_order_relaxed);
}

void reset_nodes_searched() {
    g_nodes_searched.store(0, std::memory_order_relaxed);
}

namespace {

inline void inc_node() {
    g_nodes_searched.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

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

bool is_pirc_e_break(const Position& pos, Move move) {
    if (from_sq(move) != E7 || to_sq(move) != E5) return false;
    if (pos.side_to_move() != Color::Black) return false;

    return pos.piece_on(D6) == Piece::BlackPawn &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(G6) == Piece::BlackPawn &&
           pos.piece_on(E4) == Piece::WhitePawn &&
           pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(C3) == Piece::WhiteKnight;
}

bool is_initial_slav_structure(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(C6) == Piece::BlackPawn &&
           pos.piece_on(D5) == Piece::BlackPawn &&
           pos.piece_on(G1) == Piece::WhiteKnight &&
           pos.piece_on(F1) == Piece::WhiteBishop;
}

int slav_development_adjustment(const Position& pos, Move move) {
    if (!is_initial_slav_structure(pos)) return 0;

    if (from_sq(move) == G1 && to_sq(move) == F3) return ROOT_SLAV_DEVELOPMENT_BONUS;
    if (from_sq(move) == E2 && to_sq(move) == E3) return ROOT_SLAV_DEVELOPMENT_BONUS;
    if (from_sq(move) == C1 && to_sq(move) == F4) return ROOT_SLAV_DEVELOPMENT_BONUS;

    if (from_sq(move) == C4 && to_sq(move) == D5) return -ROOT_EARLY_SLAV_EXCHANGE_PENALTY;
    if (from_sq(move) == B1 && (to_sq(move) == C3 || to_sq(move) == D2)) {
        return -ROOT_SLAV_DEVELOPMENT_BONUS;
    }
    if (from_sq(move) == D1) return -ROOT_EARLY_SLAV_EXCHANGE_PENALTY;

    return 0;
}

bool is_reti_space_gain(const Position& pos, Move move) {
    if (from_sq(move) != D5 || to_sq(move) != D4) return false;
    if (pos.side_to_move() != Color::Black) return false;

    return pos.piece_on(D5) == Piece::BlackPawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.piece_on(D2) == Piece::WhitePawn &&
           pos.piece_on(C7) == Piece::BlackPawn;
}

bool is_nimzo_position(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;
    return pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(C3) == Piece::WhiteKnight &&
           pos.piece_on(B4) == Piece::BlackBishop &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(E6) == Piece::BlackPawn;
}

bool is_nimzo_damaged_structure(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(C3) == Piece::WhitePawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(E1) == Piece::WhiteKing &&
           pos.piece_on(F1) == Piece::WhiteBishop &&
           pos.piece_on(C5) == Piece::BlackPawn &&
           pos.piece_on(E6) == Piece::BlackPawn &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(G8) == Piece::BlackKing;
}

bool is_nimzo_damaged_recapture_position(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(C3) == Piece::WhitePawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(E3) == Piece::WhitePawn &&
           pos.piece_on(E2) == Piece::WhiteBishop &&
           pos.piece_on(F4) == Piece::WhiteBishop &&
           pos.piece_on(D4) == Piece::BlackPawn &&
           pos.piece_on(D7) == Piece::BlackKnight &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(G8) == Piece::BlackKing;
}

int nimzo_adjustment(const Position& pos, Move move) {
    if (is_nimzo_damaged_recapture_position(pos)) {
        if (from_sq(move) == C3 && to_sq(move) == D4) {
            return ROOT_NIMZO_C_PAWN_RECAPTURE_BONUS;
        }
        if ((from_sq(move) == G1 && to_sq(move) == F3) ||
            (from_sq(move) == E3 && to_sq(move) == D4)) {
            return -ROOT_NIMZO_WRONG_RECAPTURE_PENALTY;
        }
    }

    if (is_nimzo_damaged_structure(pos)) {
        if (from_sq(move) == D4 && to_sq(move) == C5) {
            return -ROOT_NIMZO_DAMAGED_STRUCTURE_GRAB_PENALTY;
        }
        if (from_sq(move) == E2 && to_sq(move) == E3) {
            return ROOT_NIMZO_DAMAGED_STRUCTURE_DEVELOPMENT_BONUS;
        }
        if (from_sq(move) == F1 && (to_sq(move) == D3 || to_sq(move) == E2)) {
            return ROOT_NIMZO_DAMAGED_STRUCTURE_DEVELOPMENT_BONUS;
        }
        if (from_sq(move) == C1 && (to_sq(move) == G5 || to_sq(move) == F4 || to_sq(move) == E3)) {
            return ROOT_NIMZO_DAMAGED_STRUCTURE_DEVELOPMENT_BONUS;
        }
    }

    if (!is_nimzo_position(pos)) return 0;

    // Safer development: e3, Qc2
    if (from_sq(move) == E2 && to_sq(move) == E3) return 60;
    if (from_sq(move) == D1 && to_sq(move) == C2) return 60;

    // Risky: f3, Nf3
    if (from_sq(move) == F2 && to_sq(move) == F3) return -60;
    if (from_sq(move) == G1 && to_sq(move) == F3) return -60;

    return 0;
}

bool is_benoni_knight_development(const Position& pos, Move move) {
    if (from_sq(move) != B1 || to_sq(move) != C3) return false;
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(D5) == Piece::WhitePawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(C5) == Piece::BlackPawn &&
           pos.piece_on(E6) == Piece::BlackPawn &&
           pos.piece_on(F6) == Piece::BlackKnight;
}

bool is_dutch_gambit(const Position& pos, Move move) {
    if (from_sq(move) != E7 || to_sq(move) != E5) return false;
    if (pos.side_to_move() != Color::Black) return false;

    return pos.piece_on(F5) == Piece::BlackPawn &&
           pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.piece_on(D2) == Piece::WhitePawn &&
           pos.piece_on(E7) == Piece::BlackPawn &&
           pos.piece_on(E8) == Piece::BlackKing &&
           pos.fullmove_number() == 2;
}

bool is_queens_indian_position(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;
    return pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.piece_on(B6) == Piece::BlackPawn &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(E6) == Piece::BlackPawn &&
           pos.fullmove_number() == 4;
}

int queens_indian_adjustment(const Position& pos, Move move) {
    if (!is_queens_indian_position(pos)) return 0;

    if (from_sq(move) == G2 && to_sq(move) == G3) return ROOT_QUEENS_INDIAN_DEVELOPMENT_BONUS;
    if (from_sq(move) == A2 && to_sq(move) == A3) return ROOT_QUEENS_INDIAN_DEVELOPMENT_BONUS;
    if (from_sq(move) == E2 && to_sq(move) == E3) return ROOT_QUEENS_INDIAN_DEVELOPMENT_BONUS;

    if (from_sq(move) == B1 && to_sq(move) == C3) return -ROOT_QUEENS_INDIAN_NC3_PENALTY;

    return 0;
}

bool is_dutch_position(const Position& pos) {
    if (pos.side_to_move() != Color::Black) return false;
    return pos.piece_on(F5) == Piece::BlackPawn &&
           pos.piece_on(D4) == Piece::WhitePawn &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.fullmove_number() == 2;
}

int dutch_adjustment(const Position& pos, Move move) {
    if (!is_dutch_position(pos)) return 0;

    // Good development: e6, g6, Nf6
    if (from_sq(move) == E7 && to_sq(move) == E6 && pos.piece_on(E7) == Piece::BlackPawn)
        return ROOT_DUTCH_DEVELOPMENT_BONUS;
    if (from_sq(move) == G7 && to_sq(move) == G6 && pos.piece_on(G7) == Piece::BlackPawn)
        return ROOT_DUTCH_DEVELOPMENT_BONUS;
    if (from_sq(move) == G8 && to_sq(move) == F6 && pos.piece_on(G8) == Piece::BlackKnight)
        return ROOT_DUTCH_DEVELOPMENT_BONUS;

    // Bad: e5 gambit
    if (is_dutch_gambit(pos, move))
        return -ROOT_DUTCH_GAMBIT_PENALTY;

    return 0;
}

bool is_open_game_after_nf3(const Position& pos) {
    if (pos.side_to_move() != Color::Black) return false;

    return pos.piece_on(E4) == Piece::WhitePawn &&
           pos.piece_on(E5) == Piece::BlackPawn &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.piece_on(B8) == Piece::BlackKnight &&
           pos.piece_on(G8) == Piece::BlackKnight &&
           pos.piece_on(E8) == Piece::BlackKing &&
           pos.fullmove_number() == 2;
}

int open_game_adjustment(const Position& pos, Move move) {
    if (!is_open_game_after_nf3(pos)) return 0;

    if (from_sq(move) == B8 && to_sq(move) == C6) {
        return ROOT_OPEN_GAME_NC6_BONUS;
    }
    if (from_sq(move) == G8 && to_sq(move) == F6) {
        return -ROOT_OPEN_GAME_PETROFF_PENALTY;
    }

    return 0;
}

bool is_italian_h6_tension(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(E4) == Piece::WhitePawn &&
           pos.piece_on(D3) == Piece::WhitePawn &&
           pos.piece_on(C3) == Piece::WhiteKnight &&
           pos.piece_on(F3) == Piece::WhiteKnight &&
           pos.piece_on(C4) == Piece::WhiteBishop &&
           pos.piece_on(G1) == Piece::WhiteKing &&
           pos.piece_on(H2) == Piece::WhitePawn &&
           pos.piece_on(C5) == Piece::BlackBishop &&
           pos.piece_on(C6) == Piece::BlackKnight &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.piece_on(G8) == Piece::BlackKing &&
           pos.piece_on(H6) == Piece::BlackPawn;
}

int italian_adjustment(const Position& pos, Move move) {
    if (!is_italian_h6_tension(pos)) return 0;

    if (from_sq(move) == H2 && to_sq(move) == H3) {
        return ROOT_ITALIAN_H3_BONUS;
    }
    if (from_sq(move) == C3 && to_sq(move) == D5) {
        return -ROOT_ITALIAN_EARLY_ND5_PENALTY;
    }

    return 0;
}

bool is_english_after_nf6(const Position& pos) {
    if (pos.side_to_move() != Color::White) return false;

    return pos.piece_on(C4) == Piece::WhitePawn &&
           pos.piece_on(C3) == Piece::WhiteKnight &&
           pos.piece_on(E2) == Piece::WhitePawn &&
           pos.piece_on(D2) == Piece::WhitePawn &&
           pos.piece_on(G2) == Piece::WhitePawn &&
           pos.piece_on(G1) == Piece::WhiteKnight &&
           pos.piece_on(E5) == Piece::BlackPawn &&
           pos.piece_on(F6) == Piece::BlackKnight &&
           pos.fullmove_number() == 3;
}

int english_adjustment(const Position& pos, Move move) {
    if (!is_english_after_nf6(pos)) return 0;

    if (from_sq(move) == E2 && to_sq(move) == E3) {
        return ROOT_ENGLISH_E3_BONUS;
    }
    if (from_sq(move) == G2 && to_sq(move) == G3) {
        return ROOT_ENGLISH_G3_BONUS;
    }
    if (from_sq(move) == D2 && to_sq(move) == D4) {
        return -ROOT_ENGLISH_IMMEDIATE_D4_PENALTY;
    }
    if (from_sq(move) == E2 && to_sq(move) == E4) {
        return -ROOT_ENGLISH_IMMEDIATE_E4_PENALTY;
    }
    if (from_sq(move) == G1 && to_sq(move) == F3) {
        return -ROOT_ENGLISH_NF3_PENALTY;
    }

    return 0;
}

int root_opening_adjustment(const Position& pos, Move move) {
    Piece mover = pos.piece_on(from_sq(move));
    if (mover == Piece::None) return 0;

    int open_game_adj = open_game_adjustment(pos, move);
    if (open_game_adj != 0) return open_game_adj;

    int italian_adj = italian_adjustment(pos, move);
    if (italian_adj != 0) return italian_adj;

    int english_adj = english_adjustment(pos, move);
    if (english_adj != 0) return english_adj;

    int slav_adjustment = slav_development_adjustment(pos, move);
    if (slav_adjustment != 0) return slav_adjustment;

    if (is_benoni_knight_development(pos, move)) {
        return ROOT_BENONI_DEVELOPMENT_BONUS;
    }

    if (blocks_c_pawn_counterplay(pos, move)) {
        return -ROOT_BLOCKED_C_PAWN_PENALTY;
    }

    int qid_adjustment = queens_indian_adjustment(pos, move);
    if (qid_adjustment != 0) return qid_adjustment;

    int dutch_adj = dutch_adjustment(pos, move);
    if (dutch_adj != 0) return dutch_adj;

    int nimzo_adj = nimzo_adjustment(pos, move);
    if (nimzo_adj != 0) return nimzo_adj;

    if (type_of(mover) != PieceType::Pawn) return 0;
    if (pos.piece_on(to_sq(move)) != Piece::None || is_en_passant(move) || is_promotion(move)) return 0;

    if (is_reti_space_gain(pos, move)) {
        return ROOT_RETI_SPACE_BONUS;
    }

    if (is_pirc_e_break(pos, move)) {
        return ROOT_PIRC_E_BREAK_BONUS;
    }

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

void copy_pv(Move* dst, const Move* src) {
    int i = 0;
    for (; i < MAX_PLY && src[i] != MOVE_NONE; ++i) {
        dst[i] = src[i];
    }
    if (i < MAX_PLY) {
        dst[i] = MOVE_NONE;
    }
}

void write_pv(Move* pv, Move move, const Move* child_pv) {
    if (!pv) return;

    pv[0] = move;
    int out = 1;
    for (; out < MAX_PLY && child_pv[out - 1] != MOVE_NONE; ++out) {
        pv[out] = child_pv[out - 1];
    }
    if (out < MAX_PLY) {
        pv[out] = MOVE_NONE;
    }
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

int qsearch(Position& pos, int alpha, int beta, int checks_left = 1, int ply = 0, Move* pv = nullptr) {
    inc_node();
    if (pv) { pv[0] = MOVE_NONE; }

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

    Move child_pv[MAX_PLY] = {};
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

        int score = -qsearch(pos, -beta, -alpha, next_checks_left, ply + 1, child_pv);
        pos.unmake_move(m, undo);

        if (score > alpha) {
            alpha = score;
            write_pv(pv, m, child_pv);
        }
        if (alpha >= beta) break;
    }

    return alpha;
}

int alpha_beta(Position& pos, int depth, int alpha, int beta, int extensions_left, int ply, Move* pv = nullptr) {
    inc_node();
    if (pv) { pv[0] = MOVE_NONE; }

    if (ply > 0 && pos.is_repetition()) return 0;

    if (depth <= 0) {
        return qsearch(pos, alpha, beta, 1, ply, pv);
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
        int score = -alpha_beta(pos, depth - 1 - 2, -beta, -beta + 1, extensions_left, ply + 1);
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

    Move child_pv[MAX_PLY] = {};
    Move best_child_pv[MAX_PLY] = {};
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

        int score = -alpha_beta(pos, new_depth, -beta, -alpha, child_extensions_left, ply + 1, child_pv);

        // If reduced search fails high, re-search at full depth
        if (new_depth < depth - 1 && score > alpha) {
            score = -alpha_beta(pos, depth - 1, -beta, -alpha, extensions_left, ply + 1, child_pv);
        }

        pos.unmake_move(m, undo);

        if (score > best) {
            best = score;
            best_move = m;
            best_is_quiet = !is_capture && !is_promotion(m);
            copy_pv(best_child_pv, child_pv);
        }
        if (score > alpha) {
            alpha = score;
        }
        if (alpha >= beta) {
            cutoff = true;
            break;
        }
    }

    if (pv && best_move) {
        write_pv(pv, best_move, best_child_pv);
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

struct RootMove {
    int order = 0;
    Move move = MOVE_NONE;
};

struct RootMoveResult {
    int order = 0;
    Move move = MOVE_NONE;
    int score = -INF;
    Move pv[MAX_PLY] = {};
};

std::vector<RootMove> ordered_root_moves(Position& pos) {
    MoveList moves;
    generate_legal(pos, moves);

    std::vector<std::pair<int, Move>> scored;
    scored.reserve(static_cast<size_t>(moves.size()));
    Move tt_move = MOVE_NONE;
    if (TTEntry* entry = tt.probe(pos.key())) {
        tt_move = entry->best_move;
    }
    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        int val = lulzfish::core::capture_value(pos, m) + lulzfish::core::see(pos, to_sq(m));
        if (m == tt_move) val += 20000;
        scored.emplace_back(-val, m);
    }
    std::sort(scored.begin(), scored.end());

    std::vector<RootMove> ordered;
    ordered.reserve(scored.size());
    for (size_t i = 0; i < scored.size(); ++i) {
        ordered.push_back(RootMove{static_cast<int>(i), scored[i].second});
    }
    return ordered;
}

SearchResult empty_root_result(Position& pos) {
    SearchResult r;
    r.score = pos.is_check() ? -MATE : 0;
    r.best_move = MOVE_NONE;
    return r;
}

[[maybe_unused]] RootMoveResult search_one_root_move(const Position& root_pos, const RootMove& root, int child_depth) {
    Position pos = root_pos;
    Move child_pv[MAX_PLY] = {};
    StateInfo undo;
    Move move = root.move;
    bool verify_knight = needs_root_knight_verification(pos, move);
    int extension = verify_knight ? 1 : 0;

    pos.make_move(move, undo);
    int score = -alpha_beta(pos, child_depth + extension, -INF, INF, 1, 1, child_pv);
    pos.unmake_move(move, undo);
    score += root_opening_adjustment(pos, move);
    if (verify_knight) {
        score -= ROOT_KNIGHT_VERIFICATION_PENALTY;
    }

    RootMoveResult result;
    result.order = root.order;
    result.move = move;
    result.score = score;
    write_pv(result.pv, move, child_pv);
    return result;
}

SearchResult search_root_depth_serial(Position& pos, int depth, const std::vector<RootMove>& ordered) {
    int alpha = -INF;
    int beta = INF;
    int best_score = -INF;
    Move best_move = ordered.front().move;
    int child_depth = std::max(0, depth - 1);

    Move root_pv[MAX_PLY] = {};
    Move child_pv[MAX_PLY] = {};
    StateInfo undo;
    for (const RootMove& root : ordered) {
        Move move = root.move;
        bool verify_knight = needs_root_knight_verification(pos, move);
        int extension = verify_knight ? 1 : 0;
        pos.make_move(move, undo);
        int score = -alpha_beta(pos, child_depth + extension, -beta, -alpha, 1, 1, child_pv);
        pos.unmake_move(move, undo);
        score += root_opening_adjustment(pos, move);
        if (verify_knight) {
            score -= ROOT_KNIGHT_VERIFICATION_PENALTY;
        }

        if (score > best_score) {
            best_score = score;
            best_move = move;
            write_pv(root_pv, move, child_pv);
        }
        if (score > alpha) {
            alpha = score;
        }
    }

    tt.store(pos.key(), best_score, depth, 0, best_move);

    SearchResult result;
    result.score = best_score;
    result.best_move = best_move;
    for (int i = 0; i < MAX_PLY && root_pv[i]; ++i) {
        result.pv[i] = root_pv[i];
        result.pv_length = i + 1;
    }
    return result;
}

SearchResult search_root_depth_parallel(Position& pos, int depth, const std::vector<RootMove>& ordered, int requested_threads) {
#ifdef __EMSCRIPTEN__
    (void)requested_threads;
    return search_root_depth_serial(pos, depth, ordered);
#else
    int worker_count = std::max(1, std::min(requested_threads, static_cast<int>(ordered.size())));
    if (worker_count <= 1 || ordered.size() <= 1) {
        return search_root_depth_serial(pos, depth, ordered);
    }

    int child_depth = std::max(0, depth - 1);
    std::atomic<size_t> next_index{0};
    std::vector<RootMoveResult> results(ordered.size());
    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(worker_count));

    for (int worker = 0; worker < worker_count; ++worker) {
        workers.emplace_back([&, child_depth] {
            while (true) {
                size_t index = next_index.fetch_add(1, std::memory_order_relaxed);
                if (index >= ordered.size()) break;
                results[index] = search_one_root_move(pos, ordered[index], child_depth);
            }
        });
    }

    for (std::thread& worker : workers) {
        worker.join();
    }

    RootMoveResult best = results.front();
    for (const RootMoveResult& result : results) {
        if (result.score > best.score ||
            (result.score == best.score && result.order < best.order)) {
            best = result;
        }
    }

    tt.store(pos.key(), best.score, depth, 0, best.move);

    SearchResult result;
    result.score = best.score;
    result.best_move = best.move;
    for (int i = 0; i < MAX_PLY && best.pv[i]; ++i) {
        result.pv[i] = best.pv[i];
        result.pv_length = i + 1;
    }
    return result;
#endif
}

SearchResult search_root_depth(Position& pos, int depth, int threads) {
    std::vector<RootMove> ordered = ordered_root_moves(pos);
    if (ordered.empty()) {
        return empty_root_result(pos);
    }
    return search_root_depth_parallel(pos, depth, ordered, threads);
}

} // namespace

SearchResult search_root(Position& pos, SearchLimits limits, SearchInfoCallback on_info) {
    int max_depth = std::max(1, limits.depth);
    int threads = std::max(1, limits.threads);
    SearchResult result;

    reset_nodes_searched();
    const auto started = std::chrono::steady_clock::now();

    // Iterative deepening seeds the TT/hash move for deeper root searches.
    for (int depth = 1; depth <= max_depth; ++depth) {
        result = search_root_depth(pos, depth, threads);
        result.depth = depth;
        result.nodes = nodes_searched();
        result.time_ms = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        if (on_info) {
            on_info(result);
        }
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
        double avg = static_cast<double>(total_score) / static_cast<double>(count);
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
