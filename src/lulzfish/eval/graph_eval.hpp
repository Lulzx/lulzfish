#pragma once

//==============================================================================
// Lulzfish Relational Graph Evaluator (The Core Novel Idea)
//==============================================================================
//
// This is the foundation for the "Graph NNUE" / efficiently updatable
// relational evaluator described in DESIGN.md.
//
// Architecture:
//   PositionGraph — incrementally maintained nodes + typed relations
//   Handcrafted scoring — material, PST, mobility, and per-relation-type terms
//   Feature extraction — fixed-size float vector from the graph
//   Learned evaluator (MLP) — residual on top of handcrafted baseline
//
// This file defines the data structures. The actual network / feature
// computation will be added in subsequent iterations.
//
// For the v0.3 baseline we can start with hand-crafted relational features
// extracted from the graph (highly efficient), then move to a learned model.
//==============================================================================

#include "lulzfish/core/types.hpp"   // for Move, Square, etc.
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
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
};

//==============================================================================
// Relation Edge (the key innovation)
//==============================================================================

enum RelationType : uint8_t {
    ATTACKS = 0,
    DEFENDS,
    PINS,
    DISCOVERED_ATTACK,
    PAWN_CHAIN,
    KING_ZONE,
    RELATION_TYPE_COUNT
};

struct Relation {
    RelationType type;
    core::Square from;
    core::Square to;
};

//==============================================================================
// Feature Extraction — fixed-size float vector from the graph
//==============================================================================

// 32 features per color — compact, cache-friendly, SIMD-ready later
constexpr size_t FEATURES_PER_COLOR = 32;
constexpr size_t FEATURES_TOTAL = FEATURES_PER_COLOR * 2;

enum FeatureIndex : size_t {
    // Material (scaled centipawns / 100)
    F_MATERIAL_PAWN   = 0,
    F_MATERIAL_KNIGHT = 1,
    F_MATERIAL_BISHOP = 2,
    F_MATERIAL_ROOK   = 3,
    F_MATERIAL_QUEEN  = 4,
    F_MATERIAL_KING   = 5,  // always 0, reserved
    // Relation counts
    F_REL_ATTACKS_COUNT = 6,
    F_REL_ATTACKS_WEIGHT = 7,
    F_REL_DEFENDS_COUNT = 8,
    F_REL_DEFENDS_WEIGHT = 9,
    F_REL_PAWN_CHAIN_COUNT = 10,
    F_REL_PAWN_CHAIN_WEIGHT = 11,
    F_REL_KING_ZONE_COUNT = 12,
    F_REL_KING_ZONE_WEIGHT = 13,
    F_REL_PINS_COUNT = 14,
    F_REL_PINS_WEIGHT = 15,
    F_REL_DISCOVERED_COUNT = 16,
    F_REL_DISCOVERED_WEIGHT = 17,
    // Mobility sums (per piece type)
    F_KNIGHT_MOBILITY = 18,
    F_BISHOP_MOBILITY  = 19,
    F_ROOK_MOBILITY    = 20,
    F_QUEEN_MOBILITY   = 21,
    // Pawn structure
    F_PASSED_PAWN_COUNT = 22,
    F_DOUBLED_PAWN_COUNT = 23,
    F_ISOLATED_PAWN_COUNT = 24,
    F_PAWN_CHAIN_MAX_LEN = 25,
    // King safety
    F_KING_SAFETY_ATTACKERS = 26,
    F_KING_SHIELD_COUNT = 27,
    // Positional
    F_CENTER_CONTROL = 28,
    F_OPEN_FILE_ROOKS = 29,
    F_BISHOP_PAIR = 30,
    F_PHASE = 31  // game phase indicator (0=opening, 1=endgame)
};

// Mutable feature accumulator (for incremental updates — future)
struct FeatureAccumulator {
    std::array<float, FEATURES_PER_COLOR> values{};

    void clear() { values.fill(0.0f); }
};

//==============================================================================
// Learned Evaluator — tiny MLP: 64→32→16→1
//==============================================================================

struct MLPWeights {
    static constexpr size_t INPUT_DIM  = FEATURES_TOTAL;
    static constexpr size_t HIDDEN1_DIM = 32;
    static constexpr size_t HIDDEN2_DIM = 16;
    static constexpr size_t OUTPUT_DIM  = 1;

    std::array<float, INPUT_DIM * HIDDEN1_DIM>  w1{};
    std::array<float, HIDDEN1_DIM>              b1{};
    std::array<float, HIDDEN1_DIM * HIDDEN2_DIM> w2{};
    std::array<float, HIDDEN2_DIM>              b2{};
    std::array<float, HIDDEN2_DIM * OUTPUT_DIM>  w3{};
    std::array<float, OUTPUT_DIM>                b3{};

    void load_zeros();
    void load_random(float scale = 0.1f);

    // Serialization for Python ↔ C++ bridge
    std::string serialize() const;
    bool deserialize(const std::string& data);
};

// Forward-pass the MLP from pre-extracted features → float score.
// Returns score in centipawn-equivalent (learned model outputs cp).
float mlp_forward(const std::array<float, FEATURES_TOTAL>& features, const MLPWeights& weights);

// Extract features from the position + its graph into a flat float vector.
void extract_features(const core::Position& pos,
                      std::array<float, FEATURES_TOTAL>& features);

// Full learned eval: extract features → MLP forward → cp score.
float learned_evaluate(const core::Position& pos, const MLPWeights& weights);

// Global model — set via UCI or loaded at startup.
const MLPWeights& global_model();
void set_global_model(const MLPWeights& model);
void set_global_model_from_file(const std::string& path);
bool global_model_loaded();

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
