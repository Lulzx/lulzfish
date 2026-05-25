#pragma once

//==============================================================================
// Lulzfish Position Representation
//==============================================================================
//
// This is the central data structure of the engine.
//
// Design goals:
//   - Extremely fast make() / unmake() with full incremental updates
//   - Cache-friendly layout
//   - Clear path toward future relational graph state (we will add
//     incremental attack/relation maps here or in a companion structure)
//   - Zobrist hashing done correctly and incrementally
//
// The Position owns the current board state. Search will typically work
// with a single Position object and call make/unmake as it descends the tree.
//==============================================================================

#include "bitboard.hpp"
#include "types.hpp"
#include "lulzfish/eval/graph_eval.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lulzfish::core {

// Forward declarations
class MoveGenerator; // Will be implemented in Phase 3

//------------------------------------------------------------------------------
// StateInfo — data that changes with a move and must be restored on unmake
//------------------------------------------------------------------------------

struct StateInfo {
    Key        pawn_key;           // Zobrist for pawn structure (useful later)
    Key        material_key;       // Rough material signature (optional)
    uint8_t    castling_rights;
    Square     en_passant_square;  // NoneSquare if none
    uint8_t    halfmove_clock;
    Piece      captured_piece;     // Piece::None if no capture
    Key        previous_key;       // Previous full Zobrist key (for unmake)

    // For special moves undo
    Piece      moved_piece;        // The piece that moved (before any promotion)
    Square     castling_rook_from;
    Square     castling_rook_to;
    Piece      promoted_to;        // For promotion undo

    // Graph delta for exact symmetric undo (key for efficient relational eval)
    std::vector<lulzfish::eval::graph::Relation> graph_removed_relations;
};

//------------------------------------------------------------------------------
// Position — the main board state
//------------------------------------------------------------------------------

class Position {
public:
    Position();
    explicit Position(std::string_view fen);

    //--------------------------------------------------------------------------
    // Core State Accessors (all extremely cheap)
    //--------------------------------------------------------------------------

    [[nodiscard]] Color side_to_move() const noexcept { return side_to_move_; }
    [[nodiscard]] uint8_t castling_rights() const noexcept { return castling_rights_; }
    [[nodiscard]] Square en_passant_square() const noexcept { return en_passant_square_; }
    [[nodiscard]] int halfmove_clock() const noexcept { return halfmove_clock_; }
    [[nodiscard]] int fullmove_number() const noexcept { return fullmove_number_; }

    [[nodiscard]] Bitboard pieces(Piece p) const noexcept { return piece_bb_[static_cast<size_t>(p)]; }
    [[nodiscard]] Bitboard pieces(Color c) const noexcept { return color_bb_[static_cast<size_t>(c)]; }
    [[nodiscard]] Bitboard occupancy() const noexcept { return occupancy_bb_; }

    [[nodiscard]] Piece piece_on(Square sq) const noexcept {
        // Slow path for now (we can add a mailbox[] later for O(1) if needed)
        for (int pt = 1; pt < static_cast<int>(Piece::Count); ++pt) {
            Piece p = static_cast<Piece>(pt);
            if (test_bit(piece_bb_[static_cast<size_t>(pt)], sq)) return p;
        }
        return Piece::None;
    }

    [[nodiscard]] bool empty(Square sq) const noexcept {
        return !test_bit(occupancy_bb_, sq);
    }

    [[nodiscard]] Key key() const noexcept { return key_; }
    [[nodiscard]] bool is_repetition() const;

    //--------------------------------------------------------------------------
    // Make / Unmake (the most performance-critical path)
    //--------------------------------------------------------------------------

    void make_move(Move m, StateInfo& undo);
    void unmake_move(Move m, const StateInfo& undo);

    // Special moves
    void make_null_move(StateInfo& undo);
    void unmake_null_move(const StateInfo& undo);

    //--------------------------------------------------------------------------
    // Setup & I/O
    //--------------------------------------------------------------------------

    void set_from_fen(std::string_view fen);
    [[nodiscard]] std::string fen() const;

    void set_startpos() { set_from_fen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"); }

    //--------------------------------------------------------------------------
    // Attack & Check Queries (stubs — will be powered by movegen later)
    //--------------------------------------------------------------------------

    [[nodiscard]] bool is_check() const;                    // Uses attackers_to
    [[nodiscard]] Bitboard attackers_to(Square sq, Color by_color) const;
    [[nodiscard]] Bitboard attackers_to(Square sq) const;

    //--------------------------------------------------------------------------
    // Debug
    //--------------------------------------------------------------------------

    void print() const;   // Human-readable board + state

private:
    //--------------------------------------------------------------------------
    // Internal State (keep this layout cache-friendly)
    //--------------------------------------------------------------------------

    // 12 piece bitboards (index by Piece enum value 1..12)
    std::array<Bitboard, 13> piece_bb_{};

    // 2 color occupancy bitboards (White=0, Black=1)
    std::array<Bitboard, 2> color_bb_{};

    Bitboard occupancy_bb_{EmptyBB};

    Color  side_to_move_{Color::White};
    uint8_t castling_rights_{NoCastling};
    Square  en_passant_square_{NoneSquare};
    int     halfmove_clock_{0};
    int     fullmove_number_{1};

    Key key_{0};                    // Full Zobrist key
    Key pawn_key_{0};               // Pawn-only Zobrist (future use)
    std::vector<Key> key_history_;   // Reversible-line history for repetition detection

    // Novel relational graph - kept incrementally updated
    lulzfish::eval::graph::PositionGraph graph_;

    //--------------------------------------------------------------------------
    // Zobrist Tables (static, initialized once)
    //--------------------------------------------------------------------------

    static std::array<std::array<Key, 64>, 13> zobrist_piece_; // [piece][square]
    static std::array<Key, 16>                 zobrist_castling_;
    static std::array<Key, 65>                 zobrist_ep_;      // 64 + 1 (none)
    static Key                                 zobrist_side_;

    static void init_zobrist_tables();
    static bool zobrist_initialized_;

    void update_key_incremental(Piece p, Square sq, bool add);   // helper

    //--------------------------------------------------------------------------
    // Internal Helpers
    //--------------------------------------------------------------------------

    void clear();
    void put_piece(Piece p, Square sq);
    void remove_piece(Square sq);
    void move_piece(Square from, Square to);

    void update_occupancy();
};

//------------------------------------------------------------------------------
// Free Functions
//------------------------------------------------------------------------------

[[nodiscard]] inline bool is_valid_castling_rights(uint8_t cr) {
    // Simple sanity (can be stricter later)
    return (cr & AllCastling) == cr;
}

} // namespace lulzfish::core
