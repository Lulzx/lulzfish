#pragma once

//==============================================================================
// Lulzfish Move Generation
//==============================================================================
//
// Generates pseudo-legal and fully legal moves.
// Designed to be correct first, then fast.
//
// Move representation uses the 16-bit Move type from types.hpp for now
// (we will extend the encoding as needed for promotions/castling).
//==============================================================================

#include "attacks.hpp"
#include "position.hpp"
#include "types.hpp"

#include <array>
#include <cstdint>

namespace lulzfish::core {

//==============================================================================
// Move List (small, stack-allocated, fast)
//==============================================================================

struct MoveList {
    static constexpr int MAX_MOVES = 256;

    std::array<Move, MAX_MOVES> moves{};
    int count = 0;

    void add(Move m) {
        if (count < MAX_MOVES) moves[count++] = m;
    }

    [[nodiscard]] Move operator[](int i) const { return moves[i]; }
    [[nodiscard]] int size() const { return count; }
    [[nodiscard]] bool empty() const { return count == 0; }
};

//==============================================================================
// Move Generation
//==============================================================================

// Generate all pseudo-legal moves (may leave king in check)
void generate_pseudo_legal(const Position& pos, MoveList& list);

// Generate only fully legal moves (does not leave own king in check).
// This version uses make/unmake on the provided (non-const) Position for efficiency.
void generate_legal(Position& pos, MoveList& list);

//==============================================================================
// SEE - Static Exchange Evaluation (basic version for ordering)
//==============================================================================

// Returns the approximate material gain (for the side about to capture on 'to')
// if a series of captures happens on that square. Positive = good for capturer.
int see(const Position& pos, Square to);

// Simple capture value for ordering (victim value - attacker value)
int capture_value(const Position& pos, Move m);

} // namespace lulzfish::core
