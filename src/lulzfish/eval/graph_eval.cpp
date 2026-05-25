#include "graph_eval.hpp"

#include "lulzfish/core/position.hpp"   // full definition here (cpp only)
#include "lulzfish/core/attacks.hpp"

using namespace lulzfish::core;

namespace lulzfish::eval::graph {

static double g_graph_bias = 0.0;  // set by training stub

double get_graph_bias() { return g_graph_bias; }
void set_graph_bias(double b) { g_graph_bias = b; }

int evaluate(const Position& pos) {
    // Enhanced relational graph evaluator (the novel core).
    // Uses explicit attack relations + derived features: king safety, 
    // piece pressure, crude pawn structure, and mobility.
    // This is designed to be incrementally updatable in the future.

    PositionGraph graph;
    graph.update_from_position(pos);

    int score = 0;
    Color us = pos.side_to_move();
    Color them = opposite(us);

    // 1. Attack pressure on enemy pieces (core relational feature)
    int attack_pressure = 0;
    for (const auto& rel : graph.relations()) {
        if (rel.type == ATTACKS) {
            attack_pressure += 2;  // each attack on enemy is valuable
        }
    }
    score += attack_pressure;

    // 2. King safety from the graph (attacks on king + defenders)
    Bitboard our_king_bb = pos.pieces(make_piece(us, PieceType::King));
    if (our_king_bb) {
        Square king_sq = lsb_square(our_king_bb);
        Bitboard enemy_attacks_on_king = pos.attackers_to(king_sq, them);
        int att_count = popcount(enemy_attacks_on_king);
        if (att_count > 0) {
            score -= 40 * att_count;  // strong penalty
        }

        // Bonus for own pieces defending the king (relational defense)
        Bitboard defenders = pos.attackers_to(king_sq, us);
        score += popcount(defenders) * 3;
    }

    // 3. Pawn structure / space (using bitboards + relations)
    Bitboard our_pawns = pos.pieces(make_piece(us, PieceType::Pawn));
    Bitboard their_pawns = pos.pieces(make_piece(them, PieceType::Pawn));
    score += popcount(our_pawns) * 8;
    score -= popcount(their_pawns) * 8;

    // Crude "passed pawn" hint via relations (pawns attacking forward)
    // (full passed pawn detection would use more graph edges)

    // 4. Mobility bonus from number of relations
    score += static_cast<int>(graph.relations().size()) / 2;

    // 5. Simple pin detection (relational structure)
    Bitboard our_king = pos.pieces(make_piece(us, PieceType::King));
    if (our_king) {
        Square ksq = lsb_square(our_king);
        Bitboard their_sliders = pos.pieces(make_piece(them, PieceType::Bishop)) |
                                 pos.pieces(make_piece(them, PieceType::Rook)) |
                                 pos.pieces(make_piece(them, PieceType::Queen));
        Bitboard potential_pins = their_sliders & pos.attackers_to(ksq, them);
        score -= popcount(potential_pins) * 8;
    }

    // 6. Discovered attack bonus (relational - moving a piece reveals a slider attack)
    // Lightweight: count cases where one of our sliders attacks through an enemy piece toward their king or valuable targets.
    // For baseline, add a small bonus if we have open files/diagonals toward their king (using rook/bishop attacks).
    Bitboard their_king = pos.pieces(make_piece(them, PieceType::King));
    if (their_king) {
        Square tksq = lsb_square(their_king);
        Bitboard our_rooks_queens = pos.pieces(make_piece(us, PieceType::Rook)) | pos.pieces(make_piece(us, PieceType::Queen));
        Bitboard our_bishops_queens = pos.pieces(make_piece(us, PieceType::Bishop)) | pos.pieces(make_piece(us, PieceType::Queen));
        score += popcount(our_rooks_queens & rook_attacks_bb(tksq, pos.occupancy())) * 3;
        score += popcount(our_bishops_queens & bishop_attacks_bb(tksq, pos.occupancy())) * 3;
    }

    // 7. Color complex control (light/dark square dominance - classic relational concept)
    Bitboard light_squares = 0x55AA55AA55AA55AAULL; // approximate light squares
    int our_light = popcount(pos.pieces(us) & light_squares);
    int their_light = popcount(pos.pieces(them) & light_squares);
    score += (our_light - their_light) * 2;

    // 8. Outposts (relational - pieces on strong squares not attackable by enemy pawns, supported)
    // Simple version: bonus for knights/bishops on advanced ranks
    Bitboard our_minors = pos.pieces(make_piece(us, PieceType::Knight)) | pos.pieces(make_piece(us, PieceType::Bishop));
    // Count minors on 5th+ ranks as rough outpost bonus
    int outpost_bonus = 0;
    Bitboard minors = our_minors;
    while (minors) {
        Square s = lsb_square(minors);
        int r = rank_of(s);
        if ((us == Color::White && r >= 4) || (us == Color::Black && r <= 3)) {
            outpost_bonus += 5;
        }
        (void)pop_lsb(minors);
    }
    score += outpost_bonus;

    // 9. Passed pawns (strong relational pawn structure)
    // For baseline, give bonus to advanced pawns.
    score += popcount(our_pawns & Rank7) * 20;
    score += popcount(our_pawns & Rank6) * 10;

    // 10. Piece coordination (using relations - own pieces supporting each other)
    int own_coordination = 0; // Placeholder until DEFENDS relations are emitted.
    score += own_coordination * 2;

    // Apply training bias from self-play data
    score += static_cast<int>(g_graph_bias * 10);

    return (us == Color::White) ? score : -score;
}

void PositionGraph::update_from_position(const Position& pos) {
    nodes_.clear();
    relations_.clear();

    for (int p = 1; p < 13; ++p) {
        Piece piece = static_cast<Piece>(p);
        Bitboard bb = pos.pieces(piece);
        while (bb) {
            Square sq = lsb_square(bb);
            nodes_.push_back({piece, sq});
            (void)pop_lsb(bb);
        }
    }

    rebuild_relations(pos);
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
    Square from = from_sq(m);
    Square to   = to_sq(m);

    // Record relations we are about to remove for exact undo
    undo.graph_removed_relations.clear();
    for (const auto& rel : relations_) {
        if (rel.from == from || rel.to == from || rel.from == to || rel.to == to) {
            undo.graph_removed_relations.push_back(rel);
        }
    }

    // Delta update: remove stale relations around changed squares
    remove_relations_involving(from);
    remove_relations_involving(to);

    // Recompute local relations (much cheaper than full rebuild)
    add_relations_around(pos_after, from);
    add_relations_around(pos_after, to);

    if (is_castling(m)) {
        add_relations_around(pos_after, to);
    }
}

void PositionGraph::undo_move(Move m, const StateInfo& before) {
    Square from = from_sq(m);
    Square to   = to_sq(m);

    // Fully symmetric undo: exactly restore what was removed in apply_move
    remove_relations_involving(to);
    remove_relations_involving(from);

    // Restore the exact relations that were present before the move
    for (const auto& rel : before.graph_removed_relations) {
        relations_.push_back(rel);
    }

    // For castling/ep/promotion edge cases, the recorded deltas handle it
}

void PositionGraph::remove_relations_involving(Square sq) {
    relations_.erase(
        std::remove_if(relations_.begin(), relations_.end(),
            [sq](const Relation& r){ return r.from == sq || r.to == sq; }),
        relations_.end());
}

void PositionGraph::add_relations_around(const Position& pos, Square sq) {
    Color c = color_of(pos.piece_on(sq));
    if (c == Color::Both) return;

    Bitboard attacks = EmptyBB;
    PieceType pt = type_of(pos.piece_on(sq));

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
