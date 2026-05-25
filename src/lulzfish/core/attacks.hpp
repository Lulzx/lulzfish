#pragma once

//==============================================================================
// Lulzfish Attack Generation
//==============================================================================
//
// Magic bitboards for rooks and bishops with automatic scalar fallback.
// Magic numbers are generated at runtime; tables verified against scalar
// ray scans for correctness.
//==============================================================================

#include "bitboard.hpp"
#include "types.hpp"

namespace lulzfish::core {

// Initialization (call once at startup)
void init_attack_tables();

// Returns true when magic tables are successfully generated and verified
[[nodiscard]] bool magic_tables_ready();

// Scalar fallback implementations (exposed for verification / diagnostics)
[[nodiscard]] Bitboard rook_attacks_bb_scalar(Square sq, Bitboard occupied);
[[nodiscard]] Bitboard bishop_attacks_bb_scalar(Square sq, Bitboard occupied);

// Main attack functions — dispatch to magic if available, scalar otherwise
[[nodiscard]] Bitboard rook_attacks_bb(Square sq, Bitboard occupied);
[[nodiscard]] Bitboard bishop_attacks_bb(Square sq, Bitboard occupied);
[[nodiscard]] Bitboard queen_attacks_bb(Square sq, Bitboard occupied);

// Non-slider attacks (constexpr, zero-cost)
[[nodiscard]] inline Bitboard king_attacks_bb(Square sq)   { return king_attacks(sq); }
[[nodiscard]] inline Bitboard knight_attacks_bb(Square sq) { return knight_attacks(sq); }
[[nodiscard]] inline Bitboard pawn_attacks_bb(Square sq, Color c) { return pawn_attacks(sq, c); }

} // namespace lulzfish::core
