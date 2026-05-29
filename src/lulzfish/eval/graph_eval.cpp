#include "graph_eval.hpp"

#include "nnue.hpp"
#include "sheaftop.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/core/attacks.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <vector>

using namespace lulzfish::core;

namespace lulzfish::eval::graph {

namespace {

constexpr int PIECE_VALUES[7] = {
    0,    // None
    100,  // Pawn
    320,  // Knight
    330,  // Bishop
    500,  // Rook
    900,  // Queen
    0     // King
};

int piece_value(PieceType pt) {
    return PIECE_VALUES[static_cast<int>(pt)];
}

int relative_rank(Square sq, Color color) {
    int rank = rank_of(sq);
    return color == Color::White ? rank : 7 - rank;
}

Bitboard file_mask(int file) {
    return FileA << file;
}

int center_score(Square sq) {
    int file = file_of(sq);
    int rank = rank_of(sq);
    int file_dist = std::min(std::abs(file - 3), std::abs(file - 4));
    int rank_dist = std::min(std::abs(rank - 3), std::abs(rank - 4));
    return 6 - 2 * (file_dist + rank_dist);
}

int square_distance(Square a, Square b) {
    return std::max(std::abs(file_of(a) - file_of(b)), std::abs(rank_of(a) - rank_of(b)));
}

bool is_passed_pawn(const Position& pos, Square sq, Color color) {
    Color enemy = opposite(color);
    Bitboard enemy_pawns = pos.pieces(make_piece(enemy, PieceType::Pawn));
    int file = file_of(sq);
    int rank = rank_of(sq);

    for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
        Bitboard pawns = enemy_pawns & file_mask(f);
        while (pawns) {
            Square enemy_sq = lsb_square(pawns);
            int enemy_rank = rank_of(enemy_sq);
            if ((color == Color::White && enemy_rank > rank) ||
                (color == Color::Black && enemy_rank < rank)) {
                return false;
            }
            (void)pop_lsb(pawns);
        }
    }

    return true;
}

bool is_isolated_pawn(const Position& pos, Square sq, Color color) {
    int file = file_of(sq);
    Bitboard own_pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
        if (f == file) continue;
        if (own_pawns & file_mask(f)) return false;
    }
    return true;
}

bool is_doubled_pawn(const Position& pos, Square sq, Color color, Square* duplicate_out = nullptr) {
    int file = file_of(sq);
    Bitboard own_pawns = pos.pieces(make_piece(color, PieceType::Pawn)) & file_mask(file);
    clear_bit(own_pawns, sq);
    if (own_pawns) {
        if (duplicate_out) *duplicate_out = lsb_square(own_pawns);
        return true;
    }
    return false;
}

//==============================================================================
// Incremental update helpers (kept for future optimization)
//==============================================================================

#if 0
void add_unique_square(std::vector<Square>& squares, Square sq) {
    if (sq == NoneSquare) return;
    if (std::find(squares.begin(), squares.end(), sq) == squares.end()) {
        squares.push_back(sq);
    }
}

std::vector<Square> changed_squares_for_move(Move m, const StateInfo& state) {
    std::vector<Square> squares;
    squares.reserve(6);

    Square from = from_sq(m);
    Square to = to_sq(m);
    add_unique_square(squares, from);
    add_unique_square(squares, to);

    if (is_en_passant(m)) {
        Color us = color_of(state.moved_piece);
        int dir = (us == Color::White) ? -8 : 8;
        add_unique_square(squares, static_cast<Square>(static_cast<int>(to) + dir));
    }

    if (is_castling(m)) {
        add_unique_square(squares, state.castling_rook_from);
        add_unique_square(squares, state.castling_rook_to);
    }

    return squares;
}

void add_current_attackers(const Position& pos, Square sq, std::vector<Square>& sources) {
    for (Color color : {Color::White, Color::Black}) {
        Bitboard attackers = pos.attackers_to(sq, color);
        while (attackers) {
            add_unique_square(sources, lsb_square(attackers));
            (void)pop_lsb(attackers);
        }
    }
}

void add_first_slider_on_rays(const Position& pos, Square sq, std::vector<Square>& sources) {
    constexpr int DIRECTIONS[8][2] = {
        {0, 1}, {0, -1}, {1, 0}, {-1, 0},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };

    int file = file_of(sq);
    int rank = rank_of(sq);

    for (const auto& direction : DIRECTIONS) {
        bool orthogonal = direction[0] == 0 || direction[1] == 0;
        for (int f = file + direction[0], r = rank + direction[1];
             f >= 0 && f < 8 && r >= 0 && r < 8;
             f += direction[0], r += direction[1]) {
            Square ray_sq = make_square(f, r);
            Piece piece = pos.piece_on(ray_sq);
            if (piece == Piece::None) continue;

            PieceType type = type_of(piece);
            bool rook_like = type == PieceType::Rook || type == PieceType::Queen;
            bool bishop_like = type == PieceType::Bishop || type == PieceType::Queen;
            if ((orthogonal && rook_like) || (!orthogonal && bishop_like)) {
                add_unique_square(sources, ray_sq);
            }
            break;
        }
    }
}

#endif // #if 0 — incremental update helpers

int non_pawn_material(const Position& pos) {
    int score = 0;
    for (Color color : {Color::White, Color::Black}) {
        score += popcount(pos.pieces(make_piece(color, PieceType::Knight))) * PIECE_VALUES[2];
        score += popcount(pos.pieces(make_piece(color, PieceType::Bishop))) * PIECE_VALUES[3];
        score += popcount(pos.pieces(make_piece(color, PieceType::Rook))) * PIECE_VALUES[4];
        score += popcount(pos.pieces(make_piece(color, PieceType::Queen))) * PIECE_VALUES[5];
    }
    return score;
}

int piece_square_bonus(const Position& pos, PieceType pt, Square sq, Color color) {
    int file = file_of(sq);
    int rel_rank = relative_rank(sq, color);
    int center = center_score(sq);

    switch (pt) {
        case PieceType::Pawn: {
            int score = rel_rank * 7;
            if (file == 3 || file == 4) score += 8;
            if (file == 2 || file == 5) score += 4;
            if (is_passed_pawn(pos, sq, color)) score += rel_rank * rel_rank * 4;
            return score;
        }
        case PieceType::Knight:
            return center * 12;
        case PieceType::Bishop:
            return center * 5;
        case PieceType::Rook: {
            int score = (rel_rank == 6) ? 18 : 0;
            Bitboard own_pawns = pos.pieces(make_piece(color, PieceType::Pawn)) & file_mask(file);
            Bitboard enemy_pawns = pos.pieces(make_piece(opposite(color), PieceType::Pawn)) & file_mask(file);
            if (!own_pawns && !enemy_pawns) score += 15;
            else if (!own_pawns) score += 8;
            return score;
        }
        case PieceType::Queen:
            return center * 2;
        case PieceType::King:
            if (non_pawn_material(pos) > 2600) {
                int score = (rel_rank == 0) ? 10 : -rel_rank * 7;
                if (file == 6 || file == 2) score += 18;
                if (file == 3 || file == 4) score -= 12;
                return score;
            }
            return center * 10;
        default:
            return 0;
    }
}

Bitboard attacks_for_piece(const Position& pos, PieceType pt, Square sq, Color color) {
    switch (pt) {
        case PieceType::Pawn:   return pawn_attacks(sq, color);
        case PieceType::Knight: return knight_attacks_bb(sq);
        case PieceType::Bishop: return bishop_attacks_bb(sq, pos.occupancy());
        case PieceType::Rook:   return rook_attacks_bb(sq, pos.occupancy());
        case PieceType::Queen:  return queen_attacks_bb(sq, pos.occupancy());
        case PieceType::King:   return king_attacks_bb(sq);
        default:                return EmptyBB;
    }
}

int mobility_bonus(const Position& pos, PieceType pt, Square sq, Color color) {
    if (pt == PieceType::Pawn || pt == PieceType::King) return 0;

    Bitboard attacks = attacks_for_piece(pos, pt, sq, color) & ~pos.pieces(color);
    int mobility = popcount(attacks);

    switch (pt) {
        case PieceType::Knight: return mobility * 4;
        case PieceType::Bishop: return mobility * 3;
        case PieceType::Rook:   return mobility * 2;
        case PieceType::Queen:  return mobility;
        default:                return 0;
    }
}

int king_safety_score(const Position& pos, Color color) {
    Bitboard king = pos.pieces(make_piece(color, PieceType::King));
    if (!king) return 0;

    Square king_sq = lsb_square(king);
    Color enemy = opposite(color);
    Bitboard ring = king_attacks_bb(king_sq) | square_bb(king_sq);
    int score = 0;

    constexpr int ATTACK_WEIGHTS[7] = {
        0, 3, 9, 8, 12, 18, 0
    };

    for (int pt_index = 1; pt_index <= 6; ++pt_index) {
        PieceType pt = static_cast<PieceType>(pt_index);
        Bitboard pieces = pos.pieces(make_piece(enemy, pt));
        while (pieces) {
            Square sq = lsb_square(pieces);
            Bitboard ring_attacks = attacks_for_piece(pos, pt, sq, enemy) & ring;
            score -= popcount(ring_attacks) * ATTACK_WEIGHTS[pt_index];

            if (pt == PieceType::Queen) {
                int distance = square_distance(sq, king_sq);
                if (distance <= 2) score -= 35;
                else if (distance == 3) score -= 12;
            }

            (void)pop_lsb(pieces);
        }
    }

    if (non_pawn_material(pos) > 1800) {
        int king_file = file_of(king_sq);
        int shield_rank = rank_of(king_sq) + (color == Color::White ? 1 : -1);
        for (int file = std::max(0, king_file - 1); file <= std::min(7, king_file + 1); ++file) {
            if (shield_rank >= 0 && shield_rank <= 7) {
                Square shield_sq = make_square(file, shield_rank);
                if (pos.piece_on(shield_sq) == make_piece(color, PieceType::Pawn)) {
                    score += 8;
                } else {
                    score -= 8;
                }
            }

            if ((pos.pieces(make_piece(color, PieceType::Pawn)) & file_mask(file)) == EmptyBB) {
                score -= 6;
            }
        }
    }

    return score;
}

int evaluate_color_baseline(const Position& pos, Color color) {
    int score = 0;

    for (int pt_index = 1; pt_index <= 6; ++pt_index) {
        PieceType pt = static_cast<PieceType>(pt_index);
        Bitboard pieces = pos.pieces(make_piece(color, pt));
        while (pieces) {
            Square sq = lsb_square(pieces);
            score += piece_value(pt);
            score += piece_square_bonus(pos, pt, sq, color);
            score += mobility_bonus(pos, pt, sq, color);
            (void)pop_lsb(pieces);
        }
    }

    if (popcount(pos.pieces(make_piece(color, PieceType::Bishop))) >= 2) {
        score += 30;
    }

    score += king_safety_score(pos, color);

    return score;
}

//==============================================================================
// Handcrafted scoring per relation type
//==============================================================================

int score_defends(const Position& pos, Color color) {
    int value = 0;

    for (int pt = 1; pt <= 6; ++pt) {
        PieceType p = static_cast<PieceType>(pt);
        Bitboard pieces = pos.pieces(make_piece(color, p));
        while (pieces) {
            Square sq = lsb_square(pieces);
            Bitboard defenders = pos.attackers_to(sq, color);
            if (defenders) {
                int def_count = popcount(defenders);
                int bonus = def_count * 3 + piece_value(p) / 40;
                value += bonus;
            } else if (p != PieceType::Pawn && p != PieceType::King) {
                value -= piece_value(p) / 33;
            }
            (void)pop_lsb(pieces);
        }
    }

    return value;
}

int score_pins(const Position& pos, Color color) {
    int value = 0;
    Color enemy = opposite(color);

    Bitboard sliders = pos.pieces(make_piece(enemy, PieceType::Bishop)) |
                       pos.pieces(make_piece(enemy, PieceType::Rook)) |
                       pos.pieces(make_piece(enemy, PieceType::Queen));
    Bitboard our_king = pos.pieces(make_piece(color, PieceType::King));

    while (sliders) {
        Square slider_sq = lsb_square(sliders);
        PieceType slider_type = type_of(pos.piece_on(slider_sq));
        (void)pop_lsb(sliders);

        constexpr int DIRS[8][2] = {
            {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1}
        };
        int s_file = file_of(slider_sq);
        int s_rank = rank_of(slider_sq);

        for (const auto& d : DIRS) {
            bool orthogonal = d[0] == 0 || d[1] == 0;
            bool diagonal = !orthogonal;
            bool rook_dir = slider_type == PieceType::Rook && orthogonal;
            bool bishop_dir = slider_type == PieceType::Bishop && diagonal;
            bool queen_dir = slider_type == PieceType::Queen;
            if (!rook_dir && !bishop_dir && !queen_dir) continue;

            Square pinned = NoneSquare;
            for (int f = s_file + d[0], r = s_rank + d[1];
                 f >= 0 && f < 8 && r >= 0 && r < 8;
                 f += d[0], r += d[1]) {
                Square sq = make_square(f, r);
                Piece p = pos.piece_on(sq);
                if (p == Piece::None) continue;
                if (pinned == NoneSquare) {
                    if (color_of(p) == color) {
                        pinned = sq;
                    } else {
                        break;
                    }
                } else {
                    if (color_of(p) == color) {
                        Square king_sq = our_king ? lsb_square(our_king) : NoneSquare;
                        if (sq == king_sq) {
                            value -= piece_value(type_of(pos.piece_on(pinned))) / 2;
                        } else if (piece_value(type_of(p)) > piece_value(type_of(pos.piece_on(pinned)))) {
                            value -= piece_value(type_of(pos.piece_on(pinned))) / 3;
                        }
                    }
                    break;
                }
            }
        }
    }

    return value;
}

int score_discovered(const Position& pos, Color color) {
    int value = 0;

    Bitboard sliders = pos.pieces(make_piece(color, PieceType::Bishop)) |
                       pos.pieces(make_piece(color, PieceType::Rook)) |
                       pos.pieces(make_piece(color, PieceType::Queen));
    Color enemy = opposite(color);

    while (sliders) {
        Square slider_sq = lsb_square(sliders);
        PieceType slider_type = type_of(pos.piece_on(slider_sq));
        (void)pop_lsb(sliders);

        constexpr int DIRS[8][2] = {
            {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1}
        };
        int s_file = file_of(slider_sq);
        int s_rank = rank_of(slider_sq);

        for (const auto& d : DIRS) {
            bool orthogonal = d[0] == 0 || d[1] == 0;
            bool diagonal = !orthogonal;
            bool rook_dir = slider_type == PieceType::Rook && orthogonal;
            bool bishop_dir = slider_type == PieceType::Bishop && diagonal;
            bool queen_dir = slider_type == PieceType::Queen;
            if (!rook_dir && !bishop_dir && !queen_dir) continue;

            Square blocker = NoneSquare;
            for (int f = s_file + d[0], r = s_rank + d[1];
                 f >= 0 && f < 8 && r >= 0 && r < 8;
                 f += d[0], r += d[1]) {
                Square sq = make_square(f, r);
                Piece p = pos.piece_on(sq);
                if (p == Piece::None) continue;
                if (blocker == NoneSquare) {
                    if (color_of(p) == color) {
                        blocker = sq;
                    } else {
                        break;
                    }
                } else {
                    if (color_of(p) == enemy) {
                        int target_value = piece_value(type_of(p));
                        int blocker_value = piece_value(type_of(pos.piece_on(blocker)));
                        value += 4 + target_value / 60;
                        if (p == make_piece(enemy, PieceType::King)) value += 25;
                        if (blocker_value < 320) value += 8;
                    }
                    break;
                }
            }
        }
    }

    return value;
}

int score_pawn_chain(const Position& pos, Color color) {
    int value = 0;
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    Color enemy = opposite(color);

    Bitboard bb = pawns;
    while (bb) {
        Square sq = lsb_square(bb);
        (void)pop_lsb(bb);
        int rel_rank = relative_rank(sq, color);
        int file = file_of(sq);

        bool defended = false;
        Bitboard defenders = pos.attackers_to(sq, color) & pawns;
        if (defenders) defended = true;

        bool passed = is_passed_pawn(pos, sq, color);
        bool isolated = is_isolated_pawn(pos, sq, color);
        bool doubled = is_doubled_pawn(pos, sq, color);

        if (defended) {
            value += rel_rank * 5;
            Bitboard chain = pawns;
            while (chain) {
                Square other = lsb_square(chain);
                (void)pop_lsb(chain);
                if (other != sq && pos.attackers_to(other, color) & pawns & square_bb(sq)) {
                    value += 6;
                }
            }
        }

        if (passed) value += rel_rank * rel_rank * 4;

        if (isolated) {
            value -= 8 + rel_rank * 2;
            int enemies = popcount(pos.pieces(make_piece(enemy, PieceType::Pawn)) & file_mask(file));
            if (enemies > 0) value -= 10;
        }

        if (doubled) {
            value -= 12;
        }

        if (file >= 3 && file <= 4 && rel_rank >= 2) {
            value += 2;
        }
    }

    return value;
}

int score_king_zone() {
    return 0; // king_safety_score already handles this well
}

//==============================================================================
// Combined relational scoring
//==============================================================================

int relational_score(const Position& pos, const PositionGraph& graph, Color color) {
    int score = 0;
    Color enemy = opposite(color);

    for (const auto& rel : graph.relations()) {
        if (rel.type != ATTACKS) continue;

        Piece attacker = pos.piece_on(rel.from);
        if (attacker == Piece::None || color_of(attacker) != color) continue;

        Piece victim = pos.piece_on(rel.to);
        if (victim == Piece::None || color_of(victim) != enemy) continue;

        int attacker_value = piece_value(type_of(attacker));
        int victim_value = piece_value(type_of(victim));
        score += 3 + victim_value / 90;
        if (victim_value > attacker_value) score += (victim_value - attacker_value) / 80;
    }

    Bitboard king = pos.pieces(make_piece(color, PieceType::King));
    if (king) {
        Square king_sq = lsb_square(king);
        score -= popcount(pos.attackers_to(king_sq, enemy)) * 35;
        score += popcount(pos.attackers_to(king_sq, color)) * 4;
    }

    Bitboard minors = pos.pieces(make_piece(color, PieceType::Knight)) |
                      pos.pieces(make_piece(color, PieceType::Bishop));
    while (minors) {
        Square sq = lsb_square(minors);
        int rel_rank = relative_rank(sq, color);
        Bitboard supporting_pawns = pos.attackers_to(sq, color) &
                                    pos.pieces(make_piece(color, PieceType::Pawn));
        Bitboard enemy_pawn_attacks = pos.attackers_to(sq, enemy) &
                                      pos.pieces(make_piece(enemy, PieceType::Pawn));
        if (rel_rank >= 4 && supporting_pawns && !enemy_pawn_attacks) {
            score += 20;
        }
        (void)pop_lsb(minors);
    }

    // New handcrafted scoring from the extended relation types
    score += score_defends(pos, color);
    score += score_pins(pos, color);
    score += score_discovered(pos, color);
    score += score_pawn_chain(pos, color);
    score += score_king_zone();

    return score;
}

//==============================================================================
// Feature Extraction
//==============================================================================

float game_phase(const Position& pos) {
    int np_material = non_pawn_material(pos);
    float max_np = 6000.0f;
    float ratio = std::clamp(static_cast<float>(np_material) / max_np, 0.0f, 1.0f);
    return 1.0f - ratio;
}

int count_pawns_attacking_center(const Position& pos, Color color) {
    int count = 0;
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    Bitboard center = square_bb(D4) | square_bb(E4) | square_bb(D5) | square_bb(E5);
    while (pawns) {
        Square sq = lsb_square(pawns);
        Bitboard attacks = attacks_for_piece(pos, PieceType::Pawn, sq, color);
        if (attacks & center) count++;
        (void)pop_lsb(pawns);
    }
    return count;
}

int count_open_file_rooks(const Position& pos, Color color) {
    int count = 0;
    Bitboard rooks = pos.pieces(make_piece(color, PieceType::Rook));
    while (rooks) {
        Square sq = lsb_square(rooks);
        int f = file_of(sq);
        Bitboard file_pawns = pos.pieces(make_piece(Color::White, PieceType::Pawn)) |
                              pos.pieces(make_piece(Color::Black, PieceType::Pawn));
        if (!(file_pawns & file_mask(f))) count++;
        (void)pop_lsb(rooks);
    }
    return count;
}

int max_pawn_chain_length(const Position& pos, Color color) {
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    int max_len = 0;
    Bitboard visited = EmptyBB;

    Bitboard bb = pawns;
    while (bb) {
        Square sq = lsb_square(bb);
        (void)pop_lsb(bb);
        if (test_bit(visited, sq)) continue;

        int chain = 0;
        Square cur = sq;
        while (cur != NoneSquare && test_bit(pawns, cur)) {
            chain++;
            set_bit(visited, cur);
            Square next = NoneSquare;
            for (int df : {-1, 1}) {
                int nr = rank_of(cur) + (color == Color::White ? 1 : -1);
                if (nr >= 0 && nr < 8) {
                    int nf = file_of(cur) + df;
                    if (nf >= 0 && nf < 8) {
                        Square ns = make_square(nf, nr);
                        if (test_bit(pawns, ns) && !test_bit(visited, ns)) {
                            if (pos.attackers_to(cur, color) & square_bb(ns)) {
                                next = ns;
                                break;
                            }
                            if (pos.attackers_to(ns, color) & square_bb(cur)) {
                                next = ns;
                                break;
                            }
                        }
                    }
                }
            }
            cur = next;
        }
        if (chain > max_len) max_len = chain;
    }
    return max_len;
}

int count_doubled_pawns(const Position& pos, Color color) {
    int count = 0;
    for (int f = 0; f < 8; ++f) {
        Bitboard pawns_on_file = pos.pieces(make_piece(color, PieceType::Pawn)) & file_mask(f);
        int n = popcount(pawns_on_file);
        if (n > 1) count += n - 1;
    }
    return count;
}

int count_isolated_pawns(const Position& pos, Color color) {
    int count = 0;
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    Bitboard bb = pawns;
    while (bb) {
        Square sq = lsb_square(bb);
        if (is_isolated_pawn(pos, sq, color)) count++;
        (void)pop_lsb(bb);
    }
    return count;
}

int count_passed_pawns(const Position& pos, Color color) {
    int count = 0;
    Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
    Bitboard bb = pawns;
    while (bb) {
        Square sq = lsb_square(bb);
        if (is_passed_pawn(pos, sq, color)) count++;
        (void)pop_lsb(bb);
    }
    return count;
}

int king_shield_pawns(const Position& pos, Color color) {
    Bitboard king = pos.pieces(make_piece(color, PieceType::King));
    if (!king) return 0;

    Square king_sq = lsb_square(king);
    int king_file = file_of(king_sq);
    int king_rank = rank_of(king_sq);
    int shield_rank = king_rank + (color == Color::White ? 1 : -1);
    int shield_rank2 = king_rank + (color == Color::White ? 2 : -2);
    int count = 0;

    for (int f = std::max(0, king_file - 1); f <= std::min(7, king_file + 1); ++f) {
        for (int r : {shield_rank, shield_rank2}) {
            if (r >= 0 && r <= 7) {
                Square s = make_square(f, r);
                if (pos.piece_on(s) == make_piece(color, PieceType::Pawn)) {
                    count += (r == shield_rank) ? 1 : 0;
                }
            }
        }
    }
    return count;
}

int king_attacker_pressure(const Position& pos, Color color) {
    Bitboard king = pos.pieces(make_piece(color, PieceType::King));
    if (!king) return 0;

    Square king_sq = lsb_square(king);
    Color enemy = opposite(color);
    Bitboard ring = king_attacks_bb(king_sq) | square_bb(king_sq);
    int pressure = 0;

    constexpr int ATTACK_WEIGHTS2[7] = {0, 1, 3, 3, 5, 9, 0};
    for (int pt_idx = 1; pt_idx <= 6; ++pt_idx) {
        PieceType pt = static_cast<PieceType>(pt_idx);
        Bitboard pieces = pos.pieces(make_piece(enemy, pt));
        while (pieces) {
            Square sq = lsb_square(pieces);
            Bitboard attacks = attacks_for_piece(pos, pt, sq, enemy) & ring;
            pressure += popcount(attacks) * ATTACK_WEIGHTS2[pt_idx];
            (void)pop_lsb(pieces);
        }
    }
    return pressure;
}

} // namespace

//==============================================================================
// Global state
//==============================================================================

static double g_graph_bias = 0.0;

double get_graph_bias() { return g_graph_bias; }
void set_graph_bias(double b) { g_graph_bias = b; }

//==============================================================================
// Global MLP model
//==============================================================================

static MLPWeights g_model{};
static bool g_model_loaded = false;

const MLPWeights& global_model() { return g_model; }
bool global_model_loaded() { return g_model_loaded; }

void set_global_model(const MLPWeights& model) {
    g_model = model;
    g_model_loaded = true;
}

void set_global_model_from_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return;

    std::string data((std::istreambuf_iterator<char>(file)),
                      std::istreambuf_iterator<char>());
    file.close();

    if (g_model.deserialize(data)) {
        g_model_loaded = true;
    }
}

//==============================================================================
// MLP implementation
//==============================================================================

void MLPWeights::load_zeros() {
    w1.fill(0.0f); b1.fill(0.0f);
    w2.fill(0.0f); b2.fill(0.0f);
    w3.fill(0.0f); b3.fill(0.0f);
}

void MLPWeights::load_random(float scale) {
    static std::mt19937 rng(42);
    std::normal_distribution<float> dist(0.0f, scale);

    for (auto& v : w1) v = dist(rng);
    for (auto& v : b1) v = 0.0f;
    for (auto& v : w2) v = dist(rng) * 0.5f;
    for (auto& v : b2) v = 0.0f;
    for (auto& v : w3) v = dist(rng) * 0.25f;
    for (auto& v : b3) v = 0.0f;
}

std::string MLPWeights::serialize() const {
    std::ostringstream out(std::ios::binary);

    auto write_vec = [&](const auto& vec) {
        out.write(reinterpret_cast<const char*>(vec.data()),
                  static_cast<std::streamsize>(vec.size() * sizeof(float)));
    };

    write_vec(w1); write_vec(b1);
    write_vec(w2); write_vec(b2);
    write_vec(w3); write_vec(b3);

    return out.str();
}

bool MLPWeights::deserialize(const std::string& data) {
    size_t expected = (INPUT_DIM * HIDDEN1_DIM + HIDDEN1_DIM +
                       HIDDEN1_DIM * HIDDEN2_DIM + HIDDEN2_DIM +
                       HIDDEN2_DIM * OUTPUT_DIM + OUTPUT_DIM) * sizeof(float);
    if (data.size() < expected) return false;

    const char* ptr = data.data();

    auto read_vec = [&](auto& vec) {
        size_t bytes = vec.size() * sizeof(float);
        std::memcpy(vec.data(), ptr, bytes);
        ptr += bytes;
    };

    read_vec(w1); read_vec(b1);
    read_vec(w2); read_vec(b2);
    read_vec(w3); read_vec(b3);

    return true;
}

namespace {

float relu(float x) { return x > 0.0f ? x : 0.0f; }

void matmul_add(const float* weights, const float* input, float* output,
                size_t in_dim, size_t out_dim, const float* bias) {
    for (size_t o = 0; o < out_dim; ++o) {
        float sum = bias[o];
        for (size_t i = 0; i < in_dim; ++i) {
            sum += weights[o * in_dim + i] * input[i];
        }
        output[o] = sum;
    }
}

} // namespace

float mlp_forward(const std::array<float, FEATURES_TOTAL>& features, const MLPWeights& weights) {
    std::array<float, MLPWeights::HIDDEN1_DIM> h1;
    std::array<float, MLPWeights::HIDDEN2_DIM> h2;
    std::array<float, MLPWeights::OUTPUT_DIM> out;

    matmul_add(weights.w1.data(), features.data(), h1.data(),
               FEATURES_TOTAL, MLPWeights::HIDDEN1_DIM, weights.b1.data());
    for (auto& v : h1) v = relu(v);

    matmul_add(weights.w2.data(), h1.data(), h2.data(),
               MLPWeights::HIDDEN1_DIM, MLPWeights::HIDDEN2_DIM, weights.b2.data());
    for (auto& v : h2) v = relu(v);

    matmul_add(weights.w3.data(), h2.data(), out.data(),
               MLPWeights::HIDDEN2_DIM, MLPWeights::OUTPUT_DIM, weights.b3.data());

    return out[0];
}

//==============================================================================
// Feature extraction implementation
//==============================================================================

void extract_features(const Position& pos,
                      std::array<float, FEATURES_TOTAL>& features) {
    features.fill(0.0f);

    for (Color color : {Color::White, Color::Black}) {
        size_t base = (color == Color::White) ? 0 : FEATURES_PER_COLOR;

        features[base + F_MATERIAL_PAWN]   = static_cast<float>(popcount(pos.pieces(make_piece(color, PieceType::Pawn)))) * 1.0f;
        features[base + F_MATERIAL_KNIGHT] = static_cast<float>(popcount(pos.pieces(make_piece(color, PieceType::Knight)))) * 3.2f;
        features[base + F_MATERIAL_BISHOP] = static_cast<float>(popcount(pos.pieces(make_piece(color, PieceType::Bishop)))) * 3.3f;
        features[base + F_MATERIAL_ROOK]   = static_cast<float>(popcount(pos.pieces(make_piece(color, PieceType::Rook)))) * 5.0f;
        features[base + F_MATERIAL_QUEEN]  = static_cast<float>(popcount(pos.pieces(make_piece(color, PieceType::Queen)))) * 9.0f;
        features[base + F_MATERIAL_KING]   = 0.0f;

        int knight_mob = 0, bishop_mob = 0, rook_mob = 0, queen_mob = 0;
        for (int pt = 1; pt <= 6; ++pt) {
            PieceType pt_t = static_cast<PieceType>(pt);
            Bitboard pieces = pos.pieces(make_piece(color, pt_t));
            while (pieces) {
                Square sq = lsb_square(pieces);
                int mob = mobility_bonus(pos, pt_t, sq, color) / std::max(1, (pt <= 3 ? 4 : (pt == 4 ? 2 : 1)));
                switch (pt_t) {
                    case PieceType::Knight: knight_mob += mob; break;
                    case PieceType::Bishop: bishop_mob += mob; break;
                    case PieceType::Rook:   rook_mob += mob; break;
                    case PieceType::Queen:  queen_mob += mob; break;
                    default: break;
                }
                (void)pop_lsb(pieces);
            }
        }
        features[base + F_KNIGHT_MOBILITY] = static_cast<float>(knight_mob);
        features[base + F_BISHOP_MOBILITY]  = static_cast<float>(bishop_mob);
        features[base + F_ROOK_MOBILITY]    = static_cast<float>(rook_mob);
        features[base + F_QUEEN_MOBILITY]   = static_cast<float>(queen_mob);

        features[base + F_PASSED_PAWN_COUNT] = static_cast<float>(count_passed_pawns(pos, color));
        features[base + F_DOUBLED_PAWN_COUNT] = static_cast<float>(count_doubled_pawns(pos, color));
        features[base + F_ISOLATED_PAWN_COUNT] = static_cast<float>(count_isolated_pawns(pos, color));
        features[base + F_PAWN_CHAIN_MAX_LEN] = static_cast<float>(max_pawn_chain_length(pos, color));

        features[base + F_KING_SAFETY_ATTACKERS] = static_cast<float>(king_attacker_pressure(pos, color)) * (1.0f / 20.0f);
        features[base + F_KING_SHIELD_COUNT] = static_cast<float>(king_shield_pawns(pos, color));

        features[base + F_CENTER_CONTROL] = static_cast<float>(count_pawns_attacking_center(pos, color));
        features[base + F_OPEN_FILE_ROOKS] = static_cast<float>(count_open_file_rooks(pos, color));
        features[base + F_BISHOP_PAIR] = (popcount(pos.pieces(make_piece(color, PieceType::Bishop))) >= 2) ? 1.0f : 0.0f;
        features[base + F_PHASE] = game_phase(pos);
    }

    // Graph-derived features (relation counts + weighted sums)
    const PositionGraph& graph = pos.graph();
    for (Color color : {Color::White, Color::Black}) {
        size_t base = (color == Color::White) ? 0 : FEATURES_PER_COLOR;
        Color enemy = opposite(color);

        int a_cnt = 0, d_cnt = 0, pc_cnt = 0, kz_cnt = 0, pin_cnt = 0, disc_cnt = 0;
        float a_w = 0, d_w = 0, pc_w = 0, kz_w = 0, pin_w = 0, disc_w = 0;

        for (const auto& rel : graph.relations()) {
            Piece from_p = pos.piece_on(rel.from);
            Piece to_p = pos.piece_on(rel.to);
            if (from_p == Piece::None || to_p == Piece::None) continue;

            Color from_c = color_of(from_p);
            switch (rel.type) {
                case ATTACKS: {
                    if (from_c == color && color_of(to_p) == enemy) {
                        a_cnt++;
                        a_w += static_cast<float>(piece_value(type_of(to_p))) / 100.0f;
                    }
                    break;
                }
                case DEFENDS: {
                    if (from_c == color && color_of(to_p) == color) {
                        d_cnt++;
                        d_w += static_cast<float>(piece_value(type_of(to_p))) / 100.0f;
                    }
                    break;
                }
                case PAWN_CHAIN: {
                    if (from_c == color) {
                        pc_cnt++;
                        pc_w += static_cast<float>(relative_rank(rel.to, color)) / 7.0f;
                    }
                    break;
                }
                case KING_ZONE: {
                    if (from_c == enemy && color_of(to_p) == color) {
                        kz_cnt++;
                        kz_w += static_cast<float>(piece_value(type_of(from_p))) / 500.0f;
                    }
                    break;
                }
                case PINS: {
                    if (from_c == enemy && color_of(to_p) == color) {
                        pin_cnt++;
                        pin_w += static_cast<float>(piece_value(type_of(to_p))) / 500.0f;
                    }
                    break;
                }
                case DISCOVERED_ATTACK: {
                    if (from_c == color) {
                        disc_cnt++;
                        disc_w += 0.5f;
                    }
                    break;
                }
                default: break;
            }
        }

        features[base + F_REL_ATTACKS_COUNT]  = static_cast<float>(a_cnt);
        features[base + F_REL_ATTACKS_WEIGHT] = a_w;
        features[base + F_REL_DEFENDS_COUNT]  = static_cast<float>(d_cnt);
        features[base + F_REL_DEFENDS_WEIGHT] = d_w;
        features[base + F_REL_PAWN_CHAIN_COUNT]  = static_cast<float>(pc_cnt);
        features[base + F_REL_PAWN_CHAIN_WEIGHT] = pc_w;
        features[base + F_REL_KING_ZONE_COUNT]  = static_cast<float>(kz_cnt);
        features[base + F_REL_KING_ZONE_WEIGHT] = kz_w;
        features[base + F_REL_PINS_COUNT]  = static_cast<float>(pin_cnt);
        features[base + F_REL_PINS_WEIGHT] = pin_w;
        features[base + F_REL_DISCOVERED_COUNT]  = static_cast<float>(disc_cnt);
        features[base + F_REL_DISCOVERED_WEIGHT] = disc_w;
    }
}

void extract_features_with_topo(const Position& pos,
                                 std::array<float, FEATURES_WITH_TOPO_TOTAL>& features) {
    // First extract the base 64 features
    std::array<float, FEATURES_TOTAL> base_features;
    extract_features(pos, base_features);

    // Copy base features
    for (size_t i = 0; i < FEATURES_TOTAL; ++i) {
        features[i] = base_features[i];
    }

    // Extract topological features if enabled
    if (sheaftop::is_enabled()) {
        std::array<float, sheaftop::TOPO_FEATURE_DIM> white_topo;
        std::array<float, sheaftop::TOPO_FEATURE_DIM> black_topo;

        sheaftop::extract_features(pos.graph().topo_summary(), white_topo, black_topo);

        // White perspective: base features + white topological features
        size_t white_base = 0;
        for (size_t i = 0; i < sheaftop::TOPO_FEATURE_DIM; ++i) {
            features[FEATURES_TOTAL + white_base + i] = white_topo[i];
        }

        // Black perspective: base features + black topological features
        size_t black_base = FEATURES_WITH_TOPO_PER_COLOR;
        for (size_t i = 0; i < sheaftop::TOPO_FEATURE_DIM; ++i) {
            features[FEATURES_TOTAL + black_base + i] = black_topo[i];
        }
    } else {
        // Zero out topological features when disabled
        for (size_t i = FEATURES_TOTAL; i < FEATURES_WITH_TOPO_TOTAL; ++i) {
            features[i] = 0.0f;
        }
    }
}

float learned_evaluate(const Position& pos, const MLPWeights& weights) {
    std::array<float, FEATURES_TOTAL> features;
    extract_features(pos, features);

    // Must mirror the trainer's input layout exactly (train_mlp_eval.py):
    //   diff[:FEATURES_PER_COLOR]  = white_features - black_features
    //   diff[FEATURES_PER_COLOR:]  = black_features - white_features
    // Any deviation here is a silent train/serve mismatch.
    std::array<float, FEATURES_TOTAL> diff_features;
    for (size_t i = 0; i < FEATURES_PER_COLOR; ++i) {
        float w = features[i];
        float b = features[i + FEATURES_PER_COLOR];
        diff_features[i] = w - b;
        diff_features[i + FEATURES_PER_COLOR] = b - w;
    }

    return mlp_forward(diff_features, weights);
}

//==============================================================================
// Main evaluation
//==============================================================================

int evaluate(const Position& pos) {
    // When an NNUE net is loaded it fully replaces the handcrafted/graph eval.
    if (nnue::loaded()) {
        return nnue::evaluate(pos);
    }

    const PositionGraph& graph = pos.graph();

    int white_score = evaluate_color_baseline(pos, Color::White) -
                      evaluate_color_baseline(pos, Color::Black);
    white_score += relational_score(pos, graph, Color::White) -
                   relational_score(pos, graph, Color::Black);

    if (g_model_loaded) {
        float learned = learned_evaluate(pos, g_model);
        white_score += static_cast<int>(learned);
    }

    white_score += static_cast<int>(g_graph_bias * 10);

    return pos.side_to_move() == Color::White ? white_score : -white_score;
}

//==============================================================================
// PositionGraph — relation population (all 6 types)
//==============================================================================

void PositionGraph::update_from_position(const Position& pos) {
    refresh_nodes(pos);
    rebuild_relations(pos);

    // SheafTop: Mark topology as stale (lazy rebuild on next access)
    topo_summary_.rebuild_generation = 0;
}

void PositionGraph::refresh_nodes(const Position& pos) {
    nodes_.clear();

    for (int p = 1; p < 13; ++p) {
        Piece piece = static_cast<Piece>(p);
        Bitboard bb = pos.pieces(piece);
        while (bb) {
            Square sq = lsb_square(bb);
            nodes_.push_back({piece, sq});
            (void)pop_lsb(bb);
        }
    }
}

void PositionGraph::rebuild_relations(const Position& pos) {
    relations_.clear();

    Bitboard occ = pos.occupancy();

    // ATTACKS and DEFENDS
    for (const auto& node : nodes_) {
        Color c = color_of(node.piece);
        PieceType pt = type_of(node.piece);
        Bitboard attacks = EmptyBB;

        if (pt == PieceType::Pawn) attacks = pawn_attacks(node.square, c);
        else if (pt == PieceType::Knight) attacks = knight_attacks_bb(node.square);
        else if (pt == PieceType::Bishop) attacks = bishop_attacks_bb(node.square, occ);
        else if (pt == PieceType::Rook) attacks = rook_attacks_bb(node.square, occ);
        else if (pt == PieceType::Queen) attacks = queen_attacks_bb(node.square, occ);
        else if (pt == PieceType::King) attacks = king_attacks_bb(node.square);

        Bitboard enemies = attacks & pos.pieces(opposite(c));
        while (enemies) {
            Square target = lsb_square(enemies);
            relations_.push_back({ATTACKS, node.square, target});
            (void)pop_lsb(enemies);
        }

        Bitboard friends = attacks & pos.pieces(c) & ~square_bb(node.square);
        while (friends) {
            Square target = lsb_square(friends);
            relations_.push_back({DEFENDS, node.square, target});
            (void)pop_lsb(friends);
        }
    }

    // PAWN_CHAIN — pawns defending each other diagonally
    for (Color color : {Color::White, Color::Black}) {
        Bitboard pawns = pos.pieces(make_piece(color, PieceType::Pawn));
        Bitboard bb = pawns;
        while (bb) {
            Square sq = lsb_square(bb);
            (void)pop_lsb(bb);
            Bitboard defenders = pos.attackers_to(sq, color) & pawns;
            while (defenders) {
                Square def_sq = lsb_square(defenders);
                if (relative_rank(def_sq, color) < relative_rank(sq, color)) {
                    relations_.push_back({PAWN_CHAIN, def_sq, sq});
                }
                (void)pop_lsb(defenders);
            }
        }
    }

    // PINS — enemy sliders pinning friendly pieces
    for (Color color : {Color::White, Color::Black}) {
        Color enemy_c = opposite(color);
        Bitboard sliders = pos.pieces(make_piece(enemy_c, PieceType::Bishop)) |
                           pos.pieces(make_piece(enemy_c, PieceType::Rook)) |
                           pos.pieces(make_piece(enemy_c, PieceType::Queen));
        Bitboard king = pos.pieces(make_piece(color, PieceType::King));
        Square king_sq = king ? lsb_square(king) : NoneSquare;

        while (sliders) {
            Square slider_sq = lsb_square(sliders);
            PieceType st = type_of(pos.piece_on(slider_sq));
            (void)pop_lsb(sliders);

            constexpr int DIRS[8][2] = {
                {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1}
            };
            int s_file = file_of(slider_sq);
            int s_rank = rank_of(slider_sq);

            for (const auto& d : DIRS) {
                bool orthogonal = d[0] == 0 || d[1] == 0;
                bool diagonal = !orthogonal;
                bool valid = (st == PieceType::Rook && orthogonal) ||
                             (st == PieceType::Bishop && diagonal) ||
                             (st == PieceType::Queen);
                if (!valid) continue;

                Square pinned = NoneSquare;
                for (int f = s_file + d[0], r = s_rank + d[1];
                     f >= 0 && f < 8 && r >= 0 && r < 8;
                     f += d[0], r += d[1]) {
                    Square sq = make_square(f, r);
                    Piece p = pos.piece_on(sq);
                    if (p == Piece::None) continue;
                    if (pinned == NoneSquare) {
                        if (color_of(p) == color) {
                            pinned = sq;
                        } else break;
                    } else {
                        if (color_of(p) == color) {
                            if (sq == king_sq) {
                                relations_.push_back({PINS, slider_sq, pinned});
                            } else if (piece_value(type_of(p)) > piece_value(type_of(pos.piece_on(pinned)))) {
                                relations_.push_back({PINS, slider_sq, pinned});
                            }
                        }
                        break;
                    }
                }
            }
        }
    }

    // DISCOVERED_ATTACK — friendly sliders with blocking pieces
    for (Color color : {Color::White, Color::Black}) {
        Color enemy_c = opposite(color);
        Bitboard sliders = pos.pieces(make_piece(color, PieceType::Bishop)) |
                           pos.pieces(make_piece(color, PieceType::Rook)) |
                           pos.pieces(make_piece(color, PieceType::Queen));

        while (sliders) {
            Square slider_sq = lsb_square(sliders);
            PieceType st = type_of(pos.piece_on(slider_sq));
            (void)pop_lsb(sliders);

            constexpr int DIRS[8][2] = {
                {0,1},{0,-1},{1,0},{-1,0},{1,1},{1,-1},{-1,1},{-1,-1}
            };
            int s_file = file_of(slider_sq);
            int s_rank = rank_of(slider_sq);

            for (const auto& d : DIRS) {
                bool orthogonal = d[0] == 0 || d[1] == 0;
                bool diagonal = !orthogonal;
                bool valid = (st == PieceType::Rook && orthogonal) ||
                             (st == PieceType::Bishop && diagonal) ||
                             (st == PieceType::Queen);
                if (!valid) continue;

                Square blocker = NoneSquare;
                for (int f = s_file + d[0], r = s_rank + d[1];
                     f >= 0 && f < 8 && r >= 0 && r < 8;
                     f += d[0], r += d[1]) {
                    Square sq = make_square(f, r);
                    Piece p = pos.piece_on(sq);
                    if (p == Piece::None) continue;
                    if (blocker == NoneSquare) {
                        if (color_of(p) == color) {
                            blocker = sq;
                        } else break;
                    } else {
                        if (color_of(p) == enemy_c) {
                            relations_.push_back({DISCOVERED_ATTACK, blocker, slider_sq});
                        }
                        break;
                    }
                }
            }
        }
    }

    // KING_ZONE — pieces attacking squares near the enemy king
    for (Color color : {Color::White, Color::Black}) {
        Bitboard king = pos.pieces(make_piece(color, PieceType::King));
        if (!king) continue;
        Square king_sq = lsb_square(king);
        Color enemy = opposite(color);
        Bitboard ring = king_attacks_bb(king_sq) | square_bb(king_sq);

        for (int pt = 1; pt <= 6; ++pt) {
            PieceType pt_t = static_cast<PieceType>(pt);
            Bitboard pieces = pos.pieces(make_piece(enemy, pt_t));
            while (pieces) {
                Square sq = lsb_square(pieces);
                Bitboard attacks = attacks_for_piece(pos, pt_t, sq, enemy) & ring;
                if (attacks) {
                    relations_.push_back({KING_ZONE, sq, king_sq});
                }
                (void)pop_lsb(pieces);
            }
        }
    }
}

void PositionGraph::apply_move(Move /*m*/, StateInfo& undo, const Position& pos_after) {
    undo.graph_removed_relations.clear();

    refresh_nodes(pos_after);
    rebuild_relations(pos_after);

    // SheafTop: Apply topological delta (Tier 0 approximate update)
    if (sheaftop::is_enabled()) {
        sheaftop::apply_move_incremental(undo, pos_after, topo_summary_);
    }
}

void PositionGraph::undo_move(Move /*m*/, const StateInfo& before, const Position& pos_after_undo) {
    refresh_nodes(pos_after_undo);
    rebuild_relations(pos_after_undo);

    // SheafTop: Undo topological delta
    if (sheaftop::is_enabled()) {
        sheaftop::undo_move_incremental(before, topo_summary_);
    }
}

void PositionGraph::rebuild_topology(const Position& pos) {
    if (sheaftop::is_enabled()) {
        sheaftop::full_rebuild(pos, topo_summary_);
    }
}

void PositionGraph::ensure_topology(const Position& pos) {
    // Lazy rebuild: only recompute if generation is 0 (stale)
    if (topo_summary_.rebuild_generation == 0 && sheaftop::is_enabled()) {
        rebuild_topology(pos);
    }
}

#if 0
//==============================================================================
// Incremental relation update (future optimization — currently using full rebuild)
//==============================================================================

void PositionGraph::refresh_relations_after_changed_squares(
    const Position& pos,
    const std::vector<Square>& changed_squares) {
    std::vector<Square> sources;
    sources.reserve(32);

    for (Square sq : changed_squares) {
        if (pos.piece_on(sq) != Piece::None) {
            add_unique_square(sources, sq);
        }
        add_current_attackers(pos, sq, sources);
        add_first_slider_on_rays(pos, sq, sources);
    }

    for (Square sq : changed_squares) {
        remove_relations_involving(sq);
    }
    for (Square source : sources) {
        remove_relations_from(source);
    }
    for (Square source : sources) {
        add_relations_around(pos, source);
    }
}

void PositionGraph::remove_relations_involving(Square sq) {
    relations_.erase(
        std::remove_if(relations_.begin(), relations_.end(),
            [sq](const Relation& r){ return r.from == sq || r.to == sq; }),
        relations_.end());
}

void PositionGraph::remove_relations_from(Square sq) {
    relations_.erase(
        std::remove_if(relations_.begin(), relations_.end(),
            [sq](const Relation& r){ return r.from == sq; }),
        relations_.end());
}

void PositionGraph::add_relations_around(const Position& pos, Square sq) {
    Piece piece = pos.piece_on(sq);
    Color c = color_of(piece);
    if (c == Color::Both) return;

    PieceType pt = type_of(piece);
    Bitboard attacks = EmptyBB;
    Bitboard occ = pos.occupancy();

    if (pt == PieceType::Pawn)      attacks = pawn_attacks(sq, c);
    else if (pt == PieceType::Knight) attacks = knight_attacks_bb(sq);
    else if (pt == PieceType::Bishop) attacks = bishop_attacks_bb(sq, occ);
    else if (pt == PieceType::Rook)   attacks = rook_attacks_bb(sq, occ);
    else if (pt == PieceType::Queen)  attacks = queen_attacks_bb(sq, occ);
    else if (pt == PieceType::King)   attacks = king_attacks_bb(sq);

    // ATTACKS
    Bitboard targets = attacks & pos.pieces(opposite(c));
    while (targets) {
        Square target = lsb_square(targets);
        relations_.push_back({ATTACKS, sq, target});
        (void)pop_lsb(targets);
    }

    // DEFENDS
    Bitboard friends = attacks & pos.pieces(c) & ~square_bb(sq);
    while (friends) {
        Square target = lsb_square(friends);
        relations_.push_back({DEFENDS, sq, target});
        (void)pop_lsb(friends);
    }
}

#endif // #if 0 — incremental relation update

} // namespace lulzfish::eval::graph
