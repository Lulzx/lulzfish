#pragma once

//==============================================================================
// Lulzfish Bitboard Operations
//==============================================================================
//
// High-performance bitboard primitives.
// Designed for both clarity and eventual heavy SIMD / intrinsics usage.
//
// All functions are constexpr / forceinline where it makes sense.
//==============================================================================

#include "types.hpp"

#include <bit>
#include <cassert>
#include <cstdint>

#if defined(_MSC_VER)
    #include <intrin.h>
#endif

namespace lulzfish::core {

//------------------------------------------------------------------------------
// Basic Bit Operations (portable + fast path)
//------------------------------------------------------------------------------

[[nodiscard]] constexpr int popcount(Bitboard bb) noexcept {
#if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L
    return std::popcount(bb);
#else
    // Fallback (compiler should optimize this on modern toolchains)
    int count = 0;
    while (bb) {
        bb &= bb - 1;
        ++count;
    }
    return count;
#endif
}

[[nodiscard]] constexpr int lsb_index(Bitboard bb) noexcept {
    assert(bb != 0 && "lsb_index called on empty bitboard");
#if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L
    return std::countr_zero(bb);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(bb);
#elif defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, bb);
    return static_cast<int>(index);
#else
    int index = 0;
    while ((bb & 1ULL) == 0) {
        ++index;
        bb >>= 1;
    }
    return index;
#endif
}

[[nodiscard]] constexpr Square lsb_square(Bitboard bb) noexcept {
    return static_cast<Square>(lsb_index(bb));
}

[[nodiscard]] constexpr Bitboard lsb_bit(Bitboard bb) noexcept {
    return bb & -bb;   // isolates the least significant set bit
}

// Remove the least significant set bit
[[nodiscard]] constexpr Bitboard pop_lsb(Bitboard& bb) noexcept {
    Bitboard b = lsb_bit(bb);
    bb &= bb - 1;
    return b;
}

//------------------------------------------------------------------------------
// Shift Operations (careful with wrapping)
//------------------------------------------------------------------------------

[[nodiscard]] constexpr Bitboard shift_north(Bitboard bb) { return bb << 8; }
[[nodiscard]] constexpr Bitboard shift_south(Bitboard bb) { return bb >> 8; }
[[nodiscard]] constexpr Bitboard shift_east(Bitboard bb)  { return (bb << 1) & ~FileA; }
[[nodiscard]] constexpr Bitboard shift_west(Bitboard bb)  { return (bb >> 1) & ~FileH; }

[[nodiscard]] constexpr Bitboard shift_north_east(Bitboard bb) { return (bb << 9) & ~FileA; }
[[nodiscard]] constexpr Bitboard shift_north_west(Bitboard bb) { return (bb << 7) & ~FileH; }
[[nodiscard]] constexpr Bitboard shift_south_east(Bitboard bb) { return (bb >> 7) & ~FileA; }
[[nodiscard]] constexpr Bitboard shift_south_west(Bitboard bb) { return (bb >> 9) & ~FileH; }

//------------------------------------------------------------------------------
// Square <-> Bitboard Conversion
//------------------------------------------------------------------------------

[[nodiscard]] constexpr Bitboard square_bb(Square sq) noexcept {
    return 1ULL << static_cast<int>(sq);
}

[[nodiscard]] constexpr bool test_bit(Bitboard bb, Square sq) noexcept {
    return (bb & square_bb(sq)) != 0;
}

constexpr void set_bit(Bitboard& bb, Square sq) noexcept {
    bb |= square_bb(sq);
}

constexpr void clear_bit(Bitboard& bb, Square sq) noexcept {
    bb &= ~square_bb(sq);
}

//------------------------------------------------------------------------------
// Precomputed Attack Masks (will be expanded with magic bitboards later)
// For the initial skeleton we provide simple helpers that the move generator
// can use. Full sliding attack generation comes in the next phase.
//------------------------------------------------------------------------------

// These will be replaced / augmented by proper attack tables + magic in movegen
// For now they serve as building blocks and for early testing.

[[nodiscard]] constexpr Bitboard king_attacks(Square sq) noexcept {
    Bitboard bb = square_bb(sq);
    Bitboard attacks = shift_north(bb) | shift_south(bb) |
                       shift_east(bb)  | shift_west(bb)  |
                       shift_north_east(bb) | shift_north_west(bb) |
                       shift_south_east(bb) | shift_south_west(bb);
    return attacks;
}

[[nodiscard]] constexpr Bitboard knight_attacks(Square sq) noexcept {
    Bitboard bb = square_bb(sq);
    Bitboard attacks = 0;

    // All 8 knight moves
    attacks |= (bb << 17) & ~FileA;                    // North-North-East
    attacks |= (bb << 15) & ~FileH;                    // North-North-West
    attacks |= (bb << 10) & ~(FileA | FileB);          // North-East-East
    attacks |= (bb << 6)  & ~(FileG | FileH);          // North-West-West
    attacks |= (bb >> 6)  & ~(FileA | FileB);          // South-East-East
    attacks |= (bb >> 10) & ~(FileG | FileH);          // South-West-West
    attacks |= (bb >> 15) & ~FileA;                    // South-South-East
    attacks |= (bb >> 17) & ~FileH;                    // South-South-West

    return attacks;
}

// Pawn attacks (not pushes) — direction depends on color
[[nodiscard]] constexpr Bitboard pawn_attacks(Square sq, Color c) noexcept {
    Bitboard bb = square_bb(sq);
    if (c == Color::White) {
        return shift_north_east(bb) | shift_north_west(bb);
    } else {
        return shift_south_east(bb) | shift_south_west(bb);
    }
}

//------------------------------------------------------------------------------
// Debug / Pretty Printing (slow — only for development)
//------------------------------------------------------------------------------

inline std::string bitboard_to_string(Bitboard bb) {
    std::string s;
    s.reserve(72);

    for (int rank = 7; rank >= 0; --rank) {
        for (int file = 0; file < 8; ++file) {
            Square sq = make_square(file, rank);
            s += test_bit(bb, sq) ? 'X' : '.';
            s += ' ';
        }
        s += '\n';
    }
    return s;
}

} // namespace lulzfish::core
