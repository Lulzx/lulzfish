#pragma once

//==============================================================================
// Lulzfish Core Types
//==============================================================================
//
// This file defines the fundamental types and constants used throughout the
// engine. Everything is designed with performance, clarity, and future
// incremental/graph evaluation work in mind.
//
// Square layout (little-endian rank-file):
//   a1 = 0, b1 = 1, ..., h1 = 7
//   a2 = 8, ..., h8 = 63
//
// Bitboards are uint64_t with the same mapping (bit 0 = a1).
//==============================================================================

#include <cstdint>
#include <string>
#include <string_view>

namespace lulzfish::core {

//------------------------------------------------------------------------------
// Fundamental Integer Types
//------------------------------------------------------------------------------

using Bitboard = uint64_t;   // 64-bit board representation
using Key      = uint64_t;   // Zobrist / position hash key

//------------------------------------------------------------------------------
// Enumerations (strongly typed where possible)
//------------------------------------------------------------------------------

enum class Color : uint8_t {
    White = 0,
    Black = 1,
    Both  = 2   // Sometimes useful for occupancy
};

enum class PieceType : uint8_t {
    None = 0,
    Pawn,
    Knight,
    Bishop,
    Rook,
    Queen,
    King,
    // Number of real piece types (excluding None)
    Count = 6
};

enum class Piece : uint8_t {
    None = 0,

    // White pieces (1-6)
    WhitePawn   = 1,
    WhiteKnight = 2,
    WhiteBishop = 3,
    WhiteRook   = 4,
    WhiteQueen  = 5,
    WhiteKing   = 6,

    // Black pieces (7-12)
    BlackPawn   = 7,
    BlackKnight = 8,
    BlackBishop = 9,
    BlackRook   = 10,
    BlackQueen  = 11,
    BlackKing   = 12,

    // Sentinel
    Count = 13
};

//------------------------------------------------------------------------------
// Square Representation
//------------------------------------------------------------------------------

enum Square : uint8_t {
    A1 = 0, B1, C1, D1, E1, F1, G1, H1,
    A2, B2, C2, D2, E2, F2, G2, H2,
    A3, B3, C3, D3, E3, F3, G3, H3,
    A4, B4, C4, D4, E4, F4, G4, H4,
    A5, B5, C5, D5, E5, F5, G5, H5,
    A6, B6, C6, D6, E6, F6, G6, H6,
    A7, B7, C7, D7, E7, F7, G7, H7,
    A8, B8, C8, D8, E8, F8, G8, H8,
    SquareCount = 64,
    NoneSquare  = 255   // Sentinel for "no square"
};

//------------------------------------------------------------------------------
// File / Rank Helpers (constexpr, zero-cost)
//------------------------------------------------------------------------------

constexpr int file_of(Square sq) { return static_cast<int>(sq) & 7; }
constexpr int rank_of(Square sq) { return static_cast<int>(sq) >> 3; }

constexpr Square make_square(int file, int rank) {
    return static_cast<Square>((rank << 3) | file);
}

//------------------------------------------------------------------------------
// Color & Piece Helpers
//------------------------------------------------------------------------------

constexpr Color opposite(Color c) {
    return static_cast<Color>(static_cast<uint8_t>(c) ^ 1);
}

constexpr bool is_white(Piece p) {
    return static_cast<uint8_t>(p) >= 1 && static_cast<uint8_t>(p) <= 6;
}

constexpr bool is_black(Piece p) {
    return static_cast<uint8_t>(p) >= 7 && static_cast<uint8_t>(p) <= 12;
}

constexpr Color color_of(Piece p) {
    if (p == Piece::None) return Color::Both;
    return is_white(p) ? Color::White : Color::Black;
}

constexpr PieceType type_of(Piece p) {
    if (p == Piece::None) return PieceType::None;
    uint8_t v = static_cast<uint8_t>(p);
    return static_cast<PieceType>(((v - 1) % 6) + 1);
}

constexpr Piece make_piece(Color c, PieceType pt) {
    if (pt == PieceType::None) return Piece::None;
    uint8_t base = (c == Color::White) ? 0 : 6;
    return static_cast<Piece>(base + static_cast<uint8_t>(pt));
}

//------------------------------------------------------------------------------
// Piece / Square to Character (for FEN, debugging, UCI)
//------------------------------------------------------------------------------

constexpr char piece_char(Piece p) {
    switch (p) {
        case Piece::WhitePawn:   return 'P';
        case Piece::WhiteKnight: return 'N';
        case Piece::WhiteBishop: return 'B';
        case Piece::WhiteRook:   return 'R';
        case Piece::WhiteQueen:  return 'Q';
        case Piece::WhiteKing:   return 'K';
        case Piece::BlackPawn:   return 'p';
        case Piece::BlackKnight: return 'n';
        case Piece::BlackBishop: return 'b';
        case Piece::BlackRook:   return 'r';
        case Piece::BlackQueen:  return 'q';
        case Piece::BlackKing:   return 'k';
        default:                 return '.';
    }
}

constexpr Piece piece_from_char(char c) {
    switch (c) {
        case 'P': return Piece::WhitePawn;
        case 'N': return Piece::WhiteKnight;
        case 'B': return Piece::WhiteBishop;
        case 'R': return Piece::WhiteRook;
        case 'Q': return Piece::WhiteQueen;
        case 'K': return Piece::WhiteKing;
        case 'p': return Piece::BlackPawn;
        case 'n': return Piece::BlackKnight;
        case 'b': return Piece::BlackBishop;
        case 'r': return Piece::BlackRook;
        case 'q': return Piece::BlackQueen;
        case 'k': return Piece::BlackKing;
        default:  return Piece::None;
    }
}

//------------------------------------------------------------------------------
// Move Encoding (16-bit, extended for all special moves)
//------------------------------------------------------------------------------
// bits  0-5 : from square
// bits  6-11: to square
// bits 12-13: promotion type (0=none, 1=knight, 2=bishop, 3=rook, 4=queen)
// bits 14-15: move flag
//   0 = normal
//   1 = promotion
//   2 = en passant
//   3 = castling
//------------------------------------------------------------------------------

using Move = uint16_t;

enum MoveFlag : uint16_t {
    MOVE_NORMAL     = 0,
    MOVE_PROMOTION  = 1 << 14,
    MOVE_EN_PASSANT = 2 << 14,
    MOVE_CASTLING   = 3 << 14,
};

constexpr Move make_move(Square from, Square to, MoveFlag flag = MOVE_NORMAL, PieceType promo = PieceType::None) {
    uint16_t p = (promo == PieceType::None) ? 0 : static_cast<uint16_t>(promo);
    return static_cast<Move>(static_cast<uint16_t>(from) |
                             (static_cast<uint16_t>(to) << 6) |
                             (p << 12) |
                             static_cast<uint16_t>(flag));
}

constexpr Square from_sq(Move m) { return static_cast<Square>(m & 0x3F); }
constexpr Square to_sq(Move m)   { return static_cast<Square>((m >> 6) & 0x3F); }

constexpr MoveFlag move_flag(Move m)   { return static_cast<MoveFlag>(m & 0xC000); }
constexpr PieceType promotion_type(Move m) {
    uint16_t p = (m >> 12) & 0x3;
    return (p == 0) ? PieceType::None : static_cast<PieceType>(p);
}

constexpr bool is_promotion(Move m)  { return move_flag(m) == MOVE_PROMOTION; }
constexpr bool is_en_passant(Move m) { return move_flag(m) == MOVE_EN_PASSANT; }
constexpr bool is_castling(Move m)   { return move_flag(m) == MOVE_CASTLING; }

// Special move constants
constexpr Move MOVE_NONE = 0;

//------------------------------------------------------------------------------
// Castling Rights (bitmask, easy to update incrementally)
//------------------------------------------------------------------------------

enum CastlingRights : uint8_t {
    NoCastling   = 0,
    WhiteOO      = 1 << 0,
    WhiteOOO     = 1 << 1,
    BlackOO      = 1 << 2,
    BlackOOO     = 1 << 3,
    AllCastling  = WhiteOO | WhiteOOO | BlackOO | BlackOOO
};

//------------------------------------------------------------------------------
// Direction & Attack Constants (useful for incremental work later)
//------------------------------------------------------------------------------

enum Direction : int8_t {
    North =  8, South = -8,
    East  =  1, West  = -1,
    NorthEast =  9, NorthWest =  7,
    SouthEast = -7, SouthWest = -9
};

constexpr Direction pawn_push_dir(Color c) {
    return (c == Color::White) ? North : South;
}

//------------------------------------------------------------------------------
// Basic Bitboard Constants (will be expanded in bitboard.hpp)
//------------------------------------------------------------------------------

constexpr Bitboard EmptyBB = 0ULL;
constexpr Bitboard FullBB  = ~0ULL;

// Individual files and ranks (very useful for masks and incremental updates)
constexpr Bitboard FileA = 0x0101010101010101ULL;
constexpr Bitboard FileB = FileA << 1;
constexpr Bitboard FileC = FileA << 2;
constexpr Bitboard FileD = FileA << 3;
constexpr Bitboard FileE = FileA << 4;
constexpr Bitboard FileF = FileA << 5;
constexpr Bitboard FileG = FileA << 6;
constexpr Bitboard FileH = FileA << 7;

constexpr Bitboard Rank1 = 0x00000000000000FFULL;
constexpr Bitboard Rank2 = Rank1 << (8 * 1);
constexpr Bitboard Rank3 = Rank1 << (8 * 2);
constexpr Bitboard Rank4 = Rank1 << (8 * 3);
constexpr Bitboard Rank5 = Rank1 << (8 * 4);
constexpr Bitboard Rank6 = Rank1 << (8 * 5);
constexpr Bitboard Rank7 = Rank1 << (8 * 6);
constexpr Bitboard Rank8 = Rank1 << (8 * 7);

} // namespace lulzfish::core
