#include "graph_eval.hpp"

#include "lulzfish/core/position.hpp"   // full definition here (cpp only)
#include "lulzfish/core/attacks.hpp"

#include <algorithm>
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

    return score;
}

} // namespace

static double g_graph_bias = 0.0;  // set by training stub

double get_graph_bias() { return g_graph_bias; }
void set_graph_bias(double b) { g_graph_bias = b; }

int evaluate(const Position& pos) {
    // Use the Position's incrementally maintained graph instead of rebuilding.
    const PositionGraph& graph = pos.graph();

    int white_score = evaluate_color_baseline(pos, Color::White) -
                      evaluate_color_baseline(pos, Color::Black);
    white_score += relational_score(pos, graph, Color::White) -
                   relational_score(pos, graph, Color::Black);
    white_score += static_cast<int>(g_graph_bias * 10);

    return pos.side_to_move() == Color::White ? white_score : -white_score;
}

void PositionGraph::update_from_position(const Position& pos) {
    refresh_nodes(pos);
    rebuild_relations(pos);
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

    for (const auto& node : nodes_) {
        Color c = color_of(node.piece);
        Bitboard attacks = EmptyBB;

        PieceType pt = type_of(node.piece);
        if (pt == PieceType::Pawn) attacks = pawn_attacks(node.square, c);
        else if (pt == PieceType::Knight) attacks = knight_attacks_bb(node.square);
        else if (pt == PieceType::Bishop) attacks = bishop_attacks_bb(node.square, pos.occupancy());
        else if (pt == PieceType::Rook) attacks = rook_attacks_bb(node.square, pos.occupancy());
        else if (pt == PieceType::Queen) attacks = queen_attacks_bb(node.square, pos.occupancy());
        else if (pt == PieceType::King) attacks = king_attacks_bb(node.square);

        Bitboard targets = attacks & pos.pieces(opposite(c));
        while (targets) {
            Square target = lsb_square(targets);
            relations_.push_back({ATTACKS, node.square, target});
            (void)pop_lsb(targets);
        }
    }
}

void PositionGraph::apply_move(Move m, StateInfo& undo, const Position& pos_after) {
    undo.graph_removed_relations.clear();

    refresh_nodes(pos_after);
    refresh_relations_after_changed_squares(pos_after, changed_squares_for_move(m, undo));
}

void PositionGraph::undo_move(Move m, const StateInfo& before, const Position& pos_after_undo) {
    refresh_nodes(pos_after_undo);
    refresh_relations_after_changed_squares(pos_after_undo, changed_squares_for_move(m, before));
}

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

    Bitboard attacks = EmptyBB;
    PieceType pt = type_of(piece);

    if (pt == PieceType::Pawn)      attacks = pawn_attacks(sq, c);
    else if (pt == PieceType::Knight) attacks = knight_attacks_bb(sq);
    else if (pt == PieceType::Bishop) attacks = bishop_attacks_bb(sq, pos.occupancy());
    else if (pt == PieceType::Rook)   attacks = rook_attacks_bb(sq, pos.occupancy());
    else if (pt == PieceType::Queen)  attacks = queen_attacks_bb(sq, pos.occupancy());
    else if (pt == PieceType::King)   attacks = king_attacks_bb(sq);

    Bitboard targets = attacks & pos.pieces(opposite(c));
    while (targets) {
        Square target = lsb_square(targets);
        relations_.push_back({ATTACKS, sq, target});
        (void)pop_lsb(targets);
    }
}

} // namespace lulzfish::eval::graph
