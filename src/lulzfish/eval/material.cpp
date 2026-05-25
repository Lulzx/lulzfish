#include "material.hpp"

#include "lulzfish/core/bitboard.hpp"

using namespace lulzfish::core;

namespace lulzfish::eval {

static constexpr int PIECE_VALUES[7] = {
    0,      // None
    100,    // Pawn
    320,    // Knight
    330,    // Bishop
    500,    // Rook
    900,    // Queen
    20000   // King
};

int evaluate(const Position& pos) {
    int score = 0;

    for (int pt = 1; pt <= 6; ++pt) {
        PieceType p = static_cast<PieceType>(pt);
        score += popcount(pos.pieces(make_piece(Color::White, p))) * PIECE_VALUES[pt];
        score -= popcount(pos.pieces(make_piece(Color::Black, p))) * PIECE_VALUES[pt];
    }

    return (pos.side_to_move() == Color::White) ? score : -score;
}

} // namespace lulzfish::eval
