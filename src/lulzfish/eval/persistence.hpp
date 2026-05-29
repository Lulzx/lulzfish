#pragma once

//==============================================================================
// Lulzfish SheafTop — Lightweight Persistence Engine
//==============================================================================
//
// Provides persistent homology computation for the chess sheaf.
//
// Phase 0: Exact computation stubs (slow, for signal discovery)
// Phase 1: Approximate incremental path with learned surrogates
// Phase 2+: Optional exact zigzag persistence for Tier 1/2
//
// The key insight: we only need dim-0 (components) and dim-1 (loops)
// for chess evaluation. Higher dimensions are not worth the cost.
//==============================================================================

#include "sheaf_state.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lulzfish::core {
    class Position;
}

namespace lulzfish::eval::sheaftop {

//==============================================================================
// Filtration Edge — weighted relation for building the filtered complex
//==============================================================================

struct FiltrationEdge {
    uint8_t from_idx;   // Index into piece array
    uint8_t to_idx;     // Index into piece array
    float   tension;    // Filtration value (higher = more important)
    uint8_t relation_type;  // Maps to RelationType enum
};

//==============================================================================
// Persistence Pair — birth/death of a topological feature
//==============================================================================

struct PersistencePair {
    float birth;    // Filtration value when feature appears
    float death;    // Filtration value when feature disappears
    uint8_t dim;    // 0 for H0, 1 for H1

    float persistence() const { return death - birth; }
};

//==============================================================================
// Persistence Engine
//==============================================================================

class PersistenceEngine {
public:
    // Phase 0: Full exact rebuild from position (slow, for signal discovery)
    // Computes persistence diagrams and sheaf cohomology scalars.
    // Used only during offline data generation and at root.
    static void full_rebuild_exact(const core::Position& pos, TopoSummary& out);

    // Phase 1: Approximate incremental update using learned surrogates
    // Returns the predicted delta in topological features.
    static TopologicalDelta predict_delta(
        const SheafStalk& old_stalk_white,
        const SheafStalk& new_stalk_white,
        const SheafStalk& old_stalk_black,
        const SheafStalk& new_stalk_black);

    // Persistence image computation from a persistence diagram
    // Compresses birth/death pairs into a fixed-size vector.
    static void compute_persistence_image(
        const std::vector<PersistencePair>& diagram,
        std::array<float, PERSIST_IMAGE_DIM>& out_image);

    // Initialize surrogate model weights (called once at startup)
    static void init();

    // Check if surrogate weights are initialized
    static bool is_initialized() { return initialized_; }

private:
    // Build the filtered simplicial complex from position
    static void build_filtered_complex(
        const core::Position& pos,
        std::vector<FiltrationEdge>& edges);

    // Union-Find for H0 computation
    struct UnionFind {
        std::vector<int> parent;
        std::vector<int> rank;

        void init(size_t n);
        int find(int x);
        void unite(int x, int y);
    };

    // Compute H0 (connected components) persistence
    static void compute_h0_persistence(
        const std::vector<FiltrationEdge>& edges,
        size_t num_pieces,
        std::vector<PersistencePair>& diagram);

    // Compute H1 (loops) persistence (simplified for chess)
    static void compute_h1_persistence(
        const std::vector<FiltrationEdge>& edges,
        size_t num_pieces,
        std::vector<PersistencePair>& diagram);

    // Compute sheaf cohomology scalars
    static void compute_cohomology_scalars(
        const core::Position& pos,
        const std::vector<PersistencePair>& h0_diagram,
        const std::vector<PersistencePair>& h1_diagram,
        TopoSummary& out);

    // Learned linear surrogate weights (trained offline)
    // Predicts ΔH1_tension from stalk deltas
    static std::array<float, STALK_DIM> surrogate_weights_h1_tension_;
    static std::array<float, STALK_DIM> surrogate_weights_h1_loop_;
    static std::array<float, STALK_DIM> surrogate_weights_h0_consistency_;
    static bool initialized_;

    // Helper: dot product for surrogate prediction
    static float dot_product(
        const std::array<float, STALK_DIM>& weights,
        const std::array<float, STALK_DIM>& values);
};

} // namespace lulzfish::eval::sheaftop
