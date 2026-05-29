#pragma once

//==============================================================================
// Lulzfish SheafTop — Incremental Stalk & Restriction State
//==============================================================================
//
// Core data structures for the persistent sheaf cohomology evaluator.
//
// Design goals:
//   - Cache-friendly, small structs (< 2-3 cache lines per side)
//   - Support both exact full-rebuild and approximate incremental paths
//   - Clear separation between hot-path deltas and offline exact computation
//
// See docs/SHEAF_TOP.md and docs/SHEAF_TOP_IMPLEMENTATION_NOTES.md
//==============================================================================

#include <algorithm>
#include <array>
#include <cstdint>

namespace lulzfish::eval::sheaftop {

//==============================================================================
// Constants
//==============================================================================

// Stalk dimensionality per piece (compact for Phase 0-1)
constexpr size_t STALK_DIM = 10;

// Persistence image dimensions (compressed from full persistence diagram)
constexpr size_t PERSIST_IMAGE_DIM = 48;

// Total topological feature budget (cohomology scalars + persistence image)
constexpr size_t TOPO_FEATURE_DIM = 64;

//==============================================================================
// SheafStalk — per-piece local data in the sheaf
//==============================================================================
//
// Attached to each piece (vertex) in the filtered hypergraph.
// Encodes the local state that participates in restriction maps.
//
// Fields (all normalized to roughly [0, 1] or [-1, 1]):
//   [0] piece_type_onehot[0]  (Pawn=1, else 0)
//   [1] piece_type_onehot[1]  (Knight=1, else 0)
//   [2] piece_type_onehot[2]  (Bishop=1, else 0)
//   [3] piece_type_onehot[3]  (Rook=1, else 0)
//   [4] piece_type_onehot[4]  (Queen=1, else 0)
//   [5] material_norm          (piece value / 1000)
//   [6] mobility_norm          (mobility count / 28)
//   [7] king_pressure          (attacks near enemy king / 50)
//   [8] pawn_structure_flags   (passed|isolated|doubled|chain packed)
//   [9] positional_context     (relative rank/file contribution)

struct SheafStalk {
    std::array<float, STALK_DIM> data{};

    void clear() { data.fill(0.0f); }

    SheafStalk operator-(const SheafStalk& other) const {
        SheafStalk result;
        for (size_t i = 0; i < STALK_DIM; ++i) {
            result.data[i] = data[i] - other.data[i];
        }
        return result;
    }

    SheafStalk& operator+=(const SheafStalk& delta) {
        for (size_t i = 0; i < STALK_DIM; ++i) {
            data[i] += delta.data[i];
        }
        return *this;
    }

    SheafStalk& operator-=(const SheafStalk& delta) {
        for (size_t i = 0; i < STALK_DIM; ++i) {
            data[i] -= delta.data[i];
        }
        return *this;
    }

    float l2_norm_sq() const {
        float sum = 0.0f;
        for (float v : data) sum += v * v;
        return sum;
    }
};

//==============================================================================
// TopologicalDelta — stored in StateInfo for exact undo
//==============================================================================
//
// Records the changes made during apply_move_topological so that
// undo_move_topological can restore the previous state exactly.
//
// Phase 0-1: Extremely small (stalk deltas + scalar predictions).
// Phase 2+: May include kinetic persistence event queue entries.

struct TopologicalDelta;

//==============================================================================
// TopoSummary — compact topological features for eval/search
//==============================================================================
//
// This is the actual data that gets consumed by extract_features and the
// learned search controller. It is maintained incrementally during search
// and rebuilt exactly at root/PV nodes (Tier 1/2).
//
// The persistence_image array holds compressed persistence diagram features.
// The scalar fields are direct cohomological invariants.

struct TopoSummary {
    // Core cohomological scalars
    float h0_persist_sum     = 0.0f;  // Total H0 persistence (component stability)
    float h1_tension         = 0.0f;  // Total H1 tension (unresolved obstructions)
    float h1_loop_count      = 0.0f;  // Number of persistent H1 loops
    float h0_consistency     = 0.0f;  // Global section consistency measure

    // Compressed persistence image features
    std::array<float, PERSIST_IMAGE_DIM> persist_image{};

    // Timestamp for staleness detection (incremented on Tier 1/2 rebuilds)
    uint32_t rebuild_generation = 0;

    void clear() {
        h0_persist_sum = 0.0f;
        h1_tension = 0.0f;
        h1_loop_count = 0.0f;
        h0_consistency = 0.0f;
        persist_image.fill(0.0f);
        rebuild_generation = 0;
    }

    // Apply a Tier 0 delta (fast approximate update)
    void apply_delta(const TopologicalDelta& delta);

    // Undo a Tier 0 delta (for unmake_move)
    void undo_delta(const TopologicalDelta& delta);
};

//==============================================================================
// TopologicalDelta — stored in StateInfo for exact undo (definition)
//==============================================================================

struct TopologicalDelta {
    // Complete summary state before the move (for exact undo)
    TopoSummary prev_summary;

    // Previous aggregated stalks (for computing deltas)
    std::array<SheafStalk, 2> prev_stalks{};  // [0]=White, [1]=Black

    // New aggregated stalks (for computing deltas)
    std::array<SheafStalk, 2> new_stalks{};  // [0]=White, [1]=Black

    // Number of stalks changed per color (for validation)
    uint8_t white_stalks_changed = 0;
    uint8_t black_stalks_changed = 0;

    // Predicted scalar changes from learned linear surrogate
    float delta_h1_tension     = 0.0f;
    float delta_h1_loop_count  = 0.0f;
    float delta_h0_consistency = 0.0f;

    void clear() {
        prev_summary.clear();
        prev_stalks[0].clear();
        prev_stalks[1].clear();
        new_stalks[0].clear();
        new_stalks[1].clear();
        white_stalks_changed = 0;
        black_stalks_changed = 0;
        delta_h1_tension = 0.0f;
        delta_h1_loop_count = 0.0f;
        delta_h0_consistency = 0.0f;
    }
};

//==============================================================================
// TopoSummary inline method implementations
//==============================================================================

inline void TopoSummary::apply_delta(const TopologicalDelta& delta) {
    h1_tension     += delta.delta_h1_tension;
    h1_loop_count  += delta.delta_h1_loop_count;
    h0_consistency += delta.delta_h0_consistency;

    // Clamp to reasonable ranges
    h1_tension     = std::clamp(h1_tension, -100.0f, 100.0f);
    h1_loop_count  = std::max(h1_loop_count, 0.0f);
    h0_consistency = std::clamp(h0_consistency, 0.0f, 100.0f);
}

inline void TopoSummary::undo_delta(const TopologicalDelta& delta) {
    h1_tension     -= delta.delta_h1_tension;
    h1_loop_count  -= delta.delta_h1_loop_count;
    h0_consistency -= delta.delta_h0_consistency;

    h1_tension     = std::clamp(h1_tension, -100.0f, 100.0f);
    h1_loop_count  = std::max(h1_loop_count, 0.0f);
    h0_consistency = std::clamp(h0_consistency, 0.0f, 100.0f);
}

} // namespace lulzfish::eval::sheaftop
