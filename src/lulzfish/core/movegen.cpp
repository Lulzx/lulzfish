#include "movegen.hpp"

#include "bitboard.hpp"

#include <cassert>

namespace lulzfish::core {

//==============================================================================
// Internal Helpers
//==============================================================================

static void add_pawn_moves(const Position& pos, MoveList& list, Color us, Square from, Square to, bool capture) {
    int to_rank = rank_of(to);
    bool is_promotion = (us == Color::White && to_rank == 7) || (us == Color::Black && to_rank == 0);

    if (is_promotion) {
        // Temporarily only queen promotions for stability during debugging
        list.add(make_move(from, to, MOVE_PROMOTION, PieceType::Queen));
    } else {
        list.add(make_move(from, to));
    }
}

static void generate_pawn_moves(const Position& pos, MoveList& list, Color us) {
    Bitboard pawns = pos.pieces(make_piece(us, PieceType::Pawn));
    Bitboard enemies = pos.pieces(opposite(us));
    Bitboard empty = ~pos.occupancy();

    int push = (us == Color::White) ? 8 : -8;
    int left = (us == Color::White) ? 7 : -9;
    int right = (us == Color::White) ? 9 : -7;

    Bitboard bb = pawns;
    while (bb) {
        Square from = lsb_square(bb);
        pop_lsb(bb);

        // Single push
        Square to = static_cast<Square>(static_cast<int>(from) + push);
        if (empty & square_bb(to)) {
            add_pawn_moves(pos, list, us, from, to, false);

            // Double push from starting rank
            if ((us == Color::White && rank_of(from) == 1) ||
                (us == Color::Black && rank_of(from) == 6)) {
                Square to2 = static_cast<Square>(static_cast<int>(to) + push);
                if (empty & square_bb(to2)) {
                    list.add(make_move(from, to2));
                }
            }
        }

        // Captures (including en passant)
        for (int d : {left, right}) {
            Square to = static_cast<Square>(static_cast<int>(from) + d);
            if (to < A1 || to > H8) continue;

            if (test_bit(enemies, to)) {
                add_pawn_moves(pos, list, us, from, to, true);
            } else if (to == pos.en_passant_square()) {
                list.add(make_move(from, to, MOVE_EN_PASSANT));
            }
        }
    }
}

static void generate_piece_moves(const Position& pos, MoveList& list, PieceType pt, Color us) {
    Bitboard pieces = pos.pieces(make_piece(us, pt));
    Bitboard enemies = pos.pieces(opposite(us));
    Bitboard empty = ~pos.occupancy();
    Bitboard occ = pos.occupancy();

    Bitboard bb = pieces;
    while (bb) {
        Square from = lsb_square(bb);
        pop_lsb(bb);

        Bitboard attacks = EmptyBB;
        switch (pt) {
            case PieceType::Knight: attacks = knight_attacks_bb(from); break;
            case PieceType::Bishop: attacks = bishop_attacks_bb(from, occ); break;
            case PieceType::Rook:   attacks = rook_attacks_bb(from, occ); break;
            case PieceType::Queen:  attacks = queen_attacks_bb(from, occ); break;
            case PieceType::King:   attacks = king_attacks_bb(from); break;
            default: break;
        }

        Bitboard targets = attacks & (empty | enemies);

        while (targets) {
            Square to = lsb_square(targets);
            pop_lsb(targets);
            list.add(make_move(from, to));
        }
    }
}

static void generate_castling(const Position& pos, MoveList& list, Color us) {
    if (pos.is_check()) return;

    uint8_t cr = pos.castling_rights();
    Bitboard occ = pos.occupancy();

    auto safe_for_castle = [&](Square king_sq, Square through, Square dest) -> bool {
        // King not moving through or to attacked square
        if (pos.attackers_to(through, opposite(us)) != EmptyBB) return false;
        if (pos.attackers_to(dest, opposite(us)) != EmptyBB) return false;
        return true;
    };

    if (us == Color::White) {
        if ((cr & WhiteOO) && !test_bit(occ, F1) && !test_bit(occ, G1)) {
            if (safe_for_castle(E1, F1, G1)) {
                list.add(make_move(E1, G1, MOVE_CASTLING));
            }
        }
        if ((cr & WhiteOOO) && !test_bit(occ, B1) && !test_bit(occ, C1) && !test_bit(occ, D1)) {
            if (safe_for_castle(E1, D1, C1)) {
                list.add(make_move(E1, C1, MOVE_CASTLING));
            }
        }
    } else {
        if ((cr & BlackOO) && !test_bit(occ, F8) && !test_bit(occ, G8)) {
            if (safe_for_castle(E8, F8, G8)) {
                list.add(make_move(E8, G8, MOVE_CASTLING));
            }
        }
        if ((cr & BlackOOO) && !test_bit(occ, B8) && !test_bit(occ, C8) && !test_bit(occ, D8)) {
            if (safe_for_castle(E8, D8, C8)) {
                list.add(make_move(E8, C8, MOVE_CASTLING));
            }
        }
    }
}

//==============================================================================
// Public API
//==============================================================================

void generate_pseudo_legal(const Position& pos, MoveList& list) {
    list.count = 0;
    Color us = pos.side_to_move();

    generate_pawn_moves(pos, list, us);
    generate_piece_moves(pos, list, PieceType::Knight, us);
    generate_piece_moves(pos, list, PieceType::Bishop, us);
    generate_piece_moves(pos, list, PieceType::Rook,   us);
    generate_piece_moves(pos, list, PieceType::Queen,  us);
    generate_piece_moves(pos, list, PieceType::King,   us);
    generate_castling(pos, list, us);
}

void generate_legal(Position& pos, MoveList& list) {
    MoveList pseudo;
    generate_pseudo_legal(pos, pseudo);

    list.count = 0;

    StateInfo undo;
    for (int i = 0; i < pseudo.size(); ++i) {
        Move m = pseudo[i];

        pos.make_move(m, undo);

        // After the move, if the side that just moved (original side_to_move)
        // left its king attacked by the new side to move, the move is illegal.
        // We ask: are there any attackers to the king of the side that just moved?
        Color just_moved = opposite(pos.side_to_move());
        Square king_sq = lsb_square(pos.pieces(make_piece(just_moved, PieceType::King)));
        Bitboard their_attackers = pos.attackers_to(king_sq, pos.side_to_move());

        if (their_attackers == EmptyBB) {
            list.add(m);
        }

        pos.unmake_move(m, undo);
    }
}

//==============================================================================
// Basic SEE Implementation
//==============================================================================

namespace {

constexpr int SEE_VALUES[7] = {
    0,   // None
    100, // Pawn
    300, // Knight
    300, // Bishop
    500, // Rook
    900, // Queen
    20000 // King (very high)
};

} // anonymous

int see(const Position& pos, Square to) {
    // Very basic SEE: direct capture value minus if protected.
    // Full version would simulate the full exchange sequence.
    Piece victim = pos.piece_on(to);
    if (victim == Piece::None) return 0;

    Color us = pos.side_to_move();
    Color them = opposite(us);

    int victim_val = SEE_VALUES[static_cast<int>(type_of(victim))];

    // Find least valuable attacker for us
    Bitboard our_attackers = pos.attackers_to(to, us);
    if (our_attackers == EmptyBB) return 0;

    // Find the cheapest attacker
    int cheapest = 20000;
    for (int pt = 1; pt <= 6; ++pt) {
        PieceType p = static_cast<PieceType>(pt);
        if (pos.pieces(make_piece(us, p)) & our_attackers) {
            cheapest = std::min(cheapest, SEE_VALUES[pt]);
            break;
        }
    }

    // If protected by them, assume they recapture with their cheapest
    Bitboard their_attackers = pos.attackers_to(to, them);
    int their_cheapest = 20000;
    if (their_attackers) {
        for (int pt = 1; pt <= 6; ++pt) {
            PieceType p = static_cast<PieceType>(pt);
            if (pos.pieces(make_piece(them, p)) & their_attackers) {
                their_cheapest = std::min(their_cheapest, SEE_VALUES[pt]);
                break;
            }
        }
    }

    // Simple: gain victim - their cheapest (if they can recapture)
    if (their_cheapest < 20000) {
        return victim_val - their_cheapest;
    }
    return victim_val;
}

int capture_value(const Position& pos, Move m) {
    Square to = to_sq(m);
    Piece victim = pos.piece_on(to);
    if (victim == Piece::None && !is_en_passant(m) && !is_promotion(m)) return 0;

    int v = 0;
    if (is_promotion(m)) v += 800; // promotion bonus
    if (victim != Piece::None) v += SEE_VALUES[static_cast<int>(type_of(victim))];
    if (is_en_passant(m)) v += 100;

    // Subtract attacker value roughly
    Piece attacker = pos.piece_on(from_sq(m));
    if (attacker != Piece::None) {
        v -= SEE_VALUES[static_cast<int>(type_of(attacker))] / 4; // rough
    }

    return v;
}

} // namespace lulzfish::core
