#pragma once

//==============================================================================
// Lulzfish Attack Generation
//==============================================================================
//
// Current: Ray casting (correct and stable).
// Next major step: Full magic bitboards for significant NPS improvement.
// Infrastructure for magics is prepared.
//==============================================================================

#include "bitboard.hpp"
#include "types.hpp"

namespace lulzfish::core {

//==============================================================================
// Magic Bitboard Tables and Functions
//==============================================================================

// Initialization (call once at startup)
void init_attack_tables();

// Magic-based attacks (fast path)
[[nodiscard]] Bitboard rook_attacks_bb(Square sq, Bitboard occupied);
[[nodiscard]] Bitboard bishop_attacks_bb(Square sq, Bitboard occupied);
[[nodiscard]] Bitboard queen_attacks_bb(Square sq, Bitboard occupied);

// Non-slider attacks (unchanged)
[[nodiscard]] inline Bitboard king_attacks_bb(Square sq)   { return king_attacks(sq); }
[[nodiscard]] inline Bitboard knight_attacks_bb(Square sq) { return knight_attacks(sq); }
[[nodiscard]] inline Bitboard pawn_attacks_bb(Square sq, Color c) { return pawn_attacks(sq, c); }

} // namespace lulzfish::core
