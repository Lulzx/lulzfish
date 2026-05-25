#pragma once

//==============================================================================
// Lulzfish Relational Graph Evaluator (The Core Novel Idea)
//==============================================================================
//
// This is the foundation for the "Graph NNUE" / efficiently updatable
// relational evaluator described in DESIGN.md.
//
// Idea:
// - Represent the position as a dynamic graph:
//     Nodes  = active pieces (with type, square, color, mobility features)
//     Edges  = explicit relations: attacks, defends, pins, discovered attacks,
//              pawn chains, king safety zones, color complexes, etc.
// - The evaluator operates on this graph (message passing / attention).
// - All updates must be incremental when a move is made (like NNUE accumulators).
//
// This file defines the data structures. The actual network / feature
// computation will be added in subsequent iterations.
//
// For the v0.3 baseline we can start with hand-crafted relational features
// extracted from the graph (highly efficient), then move to a learned model.
//==============================================================================

#include "lulzfish/core/types.hpp"   // for Move, Square, etc.
#include <vector>

namespace lulzfish::core {
    class Position;
    struct StateInfo;
}

namespace lulzfish::eval::graph {

int evaluate(const core::Position& pos);

void set_graph_bias(double b);
double get_graph_bias();

//==============================================================================
// Graph Node
//==============================================================================

struct PieceNode {
    core::Piece piece;
    core::Square square;
    // Future: cached mobility, phase-adjusted value, etc.
};

//==============================================================================
// Relation Edge (the key innovation)
//==============================================================================

enum RelationType : uint8_t {
    ATTACKS,
    DEFENDS,
    PINS,
    DISCOVERED_ATTACK,
    PAWN_CHAIN,
    KING_ZONE,
    // More to be added
};

struct Relation {
    RelationType type;
    core::Square from;
    core::Square to;
    // Future: strength, direction, etc.
};

//==============================================================================
// PositionGraph — maintained incrementally alongside Position
//==============================================================================

class PositionGraph {
public:
    void update_from_position(const core::Position& pos);

    // Incremental (delta) update hooks - the key to NNUE-like efficiency
    void apply_move(core::Move m, core::StateInfo& undo, const core::Position& pos_after);
    void undo_move(core::Move m, const core::StateInfo& before, const core::Position& pos_after_undo);

    // Accessors for the evaluator
    const std::vector<PieceNode>& nodes() const { return nodes_; }
    const std::vector<Relation>& relations() const { return relations_; }

    // For debugging / future training
    size_t node_count() const { return nodes_.size(); }
    size_t relation_count() const { return relations_.size(); }

private:
    std::vector<PieceNode> nodes_;
    std::vector<Relation> relations_;

    void refresh_nodes(const core::Position& pos);
    void rebuild_relations(const core::Position& pos); // fallback
    void refresh_relations_after_changed_squares(const core::Position& pos,
                                                 const std::vector<core::Square>& changed_squares);

    // Delta helpers (local rescans around changed squares)
    void remove_relations_involving(core::Square sq);
    void remove_relations_from(core::Square sq);
    void add_relations_around(const core::Position& pos, core::Square sq);
};

} // namespace lulzfish::eval::graph
