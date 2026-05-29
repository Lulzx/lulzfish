#pragma once

//==============================================================================
// Lulzfish SheafTop — Public API
//==============================================================================
//
// This is the public interface for the SheafTop evaluator.
// The rest of the engine (search, UCI, PositionGraph) only talks to this header.
//
// Tiered update model:
//   Tier 0 (Hot path): Stalk deltas + learned linear surrogate (< 100ns)
//   Tier 1 (PV nodes): Local zigzag / approximate cohomology (1-5µs)
//   Tier 2 (Root): Full exact persistence + cohomology (50-500µs)
//
// See docs/SHEAF_TOP.md for the full specification.
//==============================================================================

#include "sheaf_state.hpp"
#include "lulzfish/core/types.hpp"

#include <array>
#include <string>
#include <vector>

namespace lulzfish::core {
    class Position;
    struct StateInfo;
}

namespace lulzfish::eval::sheaftop {

//==============================================================================
// Lifecycle
//==============================================================================

/// Initialize surrogate model weights and global tables. Call once at startup.
void init();

//==============================================================================
// Full Rebuild (Tier 2 — exact, slow, for root/data generation)
//==============================================================================

/// Compute exact topological features from scratch. Expensive.
/// Used at root, for data generation, and for consistency validation.
void full_rebuild(const core::Position& pos, TopoSummary& out);

//==============================================================================
// Incremental Updates (Tier 0 — approximate, fast, for search)
//==============================================================================

/// Apply topological changes for a move. Called from PositionGraph::apply_move.
/// Updates the running TopoSummary using learned surrogates.
void apply_move_incremental(core::StateInfo& undo,
                            const core::Position& pos_after,
                            TopoSummary& current);

/// Undo topological changes. Called from PositionGraph::undo_move.
void undo_move_incremental(const core::StateInfo& undo,
                           TopoSummary& current);

//==============================================================================
// Local Rebuild (Tier 1 — moderate cost, for PV nodes)
//==============================================================================

/// Rebuild topological features locally around changed squares.
/// More accurate than Tier 0 but cheaper than full Tier 2 rebuild.
/// Called on PV nodes and at the root during search.
void rebuild_local(const core::Position& pos,
                   const std::vector<core::Square>& changed_squares,
                   TopoSummary& current);

//==============================================================================
// Feature Extraction
//==============================================================================

/// Extract compact topological features for the evaluator / search controller.
/// Writes TOPO_FEATURE_DIM features per color into the output arrays.
void extract_features(const TopoSummary& summary,
                      std::array<float, TOPO_FEATURE_DIM>& white_features,
                      std::array<float, TOPO_FEATURE_DIM>& black_features);

//==============================================================================
// Debug / Diagnostics
//==============================================================================

/// Print human-readable topological summary (for UCI `topology` command).
std::string format_summary(const TopoSummary& summary);

/// Verify that incremental path stays within tolerance of exact path.
/// Returns true if error is within acceptable bounds.
bool verify_consistency(const core::Position& pos,
                        const TopoSummary& incremental,
                        float tolerance = 0.05f);

//==============================================================================
// Configuration
//==============================================================================

/// Enable/disable topological features (for A/B testing).
void set_enabled(bool enabled);
bool is_enabled();

/// Set the tier for the current evaluation context.
/// 0 = hot path (approximate), 1 = PV nodes, 2 = root (exact)
void set_current_tier(int tier);
int get_current_tier();

} // namespace lulzfish::eval::sheaftop
