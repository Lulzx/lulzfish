#include "attacks.hpp"

namespace lulzfish::core {

//==============================================================================
// Slider Attack Generation
//==============================================================================
// Correct scalar ray scans. Verified magics can replace this after perft is green.
static bool attack_tables_initialized = false;

void init_attack_tables() {
    if (attack_tables_initialized) return;

    attack_tables_initialized = true;
}

Bitboard rook_attacks_bb(Square sq, Bitboard occupied) {
    Bitboard attacks = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    for (int r = rank + 1; r < 8; ++r) {
        Square to = make_square(file, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int r = rank - 1; r >= 0; --r) {
        Square to = make_square(file, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file + 1; f < 8; ++f) {
        Square to = make_square(f, rank);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1; f >= 0; --f) {
        Square to = make_square(f, rank);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }

    return attacks;
}

Bitboard bishop_attacks_bb(Square sq, Bitboard occupied) {
    Bitboard attacks = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    for (int f = file + 1, r = rank + 1; f < 8 && r < 8; ++f, ++r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1, r = rank + 1; f >= 0 && r < 8; --f, ++r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file + 1, r = rank - 1; f < 8 && r >= 0; ++f, --r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1, r = rank - 1; f >= 0 && r >= 0; --f, --r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }

    return attacks;
}

Bitboard queen_attacks_bb(Square sq, Bitboard occupied) {
    return rook_attacks_bb(sq, occupied) | bishop_attacks_bb(sq, occupied);
}

} // namespace lulzfish::core
