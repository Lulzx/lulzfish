//==============================================================================
// Lulzfish SheafTop — Public API Implementation
//==============================================================================

#include "sheaftop.hpp"
#include "persistence.hpp"

#include "lulzfish/core/attacks.hpp"
#include "lulzfish/core/position.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace lulzfish::eval::sheaftop {

namespace {

bool g_enabled = true;
int g_current_tier = 0;  // 0=hot path, 1=PV, 2=root

// Compute a SheafStalk for a piece on a given square
SheafStalk compute_stalk(const core::Position& pos, core::Square sq) {
    SheafStalk stalk;
    stalk.clear();

    core::Piece piece = pos.piece_on(sq);
    if (piece == core::Piece::None) return stalk;

    core::Color color = core::color_of(piece);
    core::PieceType type = core::type_of(piece);

    // Piece type one-hot encoding (dims 0-4)
    switch (type) {
        case core::PieceType::Pawn:   stalk.data[0] = 1.0f; break;
        case core::PieceType::Knight: stalk.data[1] = 1.0f; break;
        case core::PieceType::Bishop: stalk.data[2] = 1.0f; break;
        case core::PieceType::Rook:   stalk.data[3] = 1.0f; break;
        case core::PieceType::Queen:  stalk.data[4] = 1.0f; break;
        default: break;
    }

    // Material value normalized (dim 5)
    constexpr int PIECE_VALUES[7] = {0, 100, 320, 330, 500, 900, 0};
    stalk.data[5] = static_cast<float>(PIECE_VALUES[static_cast<int>(type)]) / 1000.0f;

    // Mobility normalized (dim 6)
    if (type != core::PieceType::Pawn && type != core::PieceType::King) {
        core::Bitboard attacks = core::EmptyBB;
        core::Bitboard occ = pos.occupancy();

        switch (type) {
            case core::PieceType::Knight: attacks = core::knight_attacks_bb(sq); break;
            case core::PieceType::Bishop: attacks = core::bishop_attacks_bb(sq, occ); break;
            case core::PieceType::Rook:   attacks = core::rook_attacks_bb(sq, occ); break;
            case core::PieceType::Queen:  attacks = core::queen_attacks_bb(sq, occ); break;
            default: break;
        }
        attacks &= ~pos.pieces(color);
        stalk.data[6] = static_cast<float>(core::popcount(attacks)) / 28.0f;
    }

    // King pressure (dim 7)
    core::Color enemy = core::opposite(color);
    core::Bitboard enemy_king = pos.pieces(core::make_piece(enemy, core::PieceType::King));
    if (enemy_king) {
        core::Square king_sq = core::lsb_square(enemy_king);
        int dist = std::max(std::abs(core::file_of(sq) - core::file_of(king_sq)),
                           std::abs(core::rank_of(sq) - core::rank_of(king_sq)));
        float pressure = std::max(0.0f, (8.0f - static_cast<float>(dist)) / 8.0f);
        stalk.data[7] = pressure * stalk.data[5];  // Weight by piece value
    }

    // Pawn structure flags (dim 8)
    if (type == core::PieceType::Pawn) {
        int file = core::file_of(sq);
        int rank = core::rank_of(sq);

        // Passed pawn
        core::Bitboard enemy_pawns = pos.pieces(core::make_piece(enemy, core::PieceType::Pawn));
        bool passed = true;
        for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
            core::Bitboard pawns = enemy_pawns & (core::FileA << f);
            while (pawns) {
                core::Square enemy_sq = core::lsb_square(pawns);
                int enemy_rank = core::rank_of(enemy_sq);
                if ((color == core::Color::White && enemy_rank > rank) ||
                    (color == core::Color::Black && enemy_rank < rank)) {
                    passed = false;
                    break;
                }
                (void)core::pop_lsb(pawns);
            }
            if (!passed) break;
        }
        if (passed) stalk.data[8] += 0.25f;

        // Isolated pawn
        core::Bitboard own_pawns = pos.pieces(core::make_piece(color, core::PieceType::Pawn));
        bool isolated = true;
        for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
            if (f == file) continue;
            if (own_pawns & (core::FileA << f)) {
                isolated = false;
                break;
            }
        }
        if (isolated) stalk.data[8] += 0.25f;

        // Doubled pawn
        core::Bitboard same_file = own_pawns & (core::FileA << file);
        core::clear_bit(same_file, sq);
        if (same_file) stalk.data[8] += 0.25f;

        // Chain participation
        core::Bitboard defenders = pos.attackers_to(sq, color) & own_pawns;
        if (defenders) stalk.data[8] += 0.25f;
    }

    // Positional context (dim 9)
    int rel_rank = (color == core::Color::White) ? core::rank_of(sq) : (7 - core::rank_of(sq));
    int file = core::file_of(sq);
    float rank_norm = static_cast<float>(rel_rank) / 7.0f;
    float file_norm = (file < 4) ? static_cast<float>(file) / 3.0f
                                 : static_cast<float>(7 - file) / 3.0f;
    stalk.data[9] = (rank_norm + file_norm) * 0.5f;

    return stalk;
}

// Aggregate stalks for a color into a single representative stalk
SheafStalk aggregate_color_stalks(const core::Position& pos, core::Color color) {
    SheafStalk aggregated;
    aggregated.clear();

    int count = 0;
    for (int pt = 1; pt <= 6; ++pt) {
        core::PieceType type = static_cast<core::PieceType>(pt);
        core::Bitboard pieces = pos.pieces(core::make_piece(color, type));

        while (pieces) {
            core::Square sq = core::lsb_square(pieces);
            SheafStalk stalk = compute_stalk(pos, sq);
            aggregated += stalk;
            ++count;
            (void)core::pop_lsb(pieces);
        }
    }

    // Normalize by piece count to keep values bounded
    if (count > 0) {
        float inv = 1.0f / static_cast<float>(count);
        for (auto& v : aggregated.data) {
            v *= inv;
        }
    }

    return aggregated;
}

} // anonymous namespace

//==============================================================================
// Public API
//==============================================================================

void init() {
    PersistenceEngine::init();
}

void full_rebuild(const core::Position& pos, TopoSummary& out) {
    if (!g_enabled) {
        out.clear();
        return;
    }
    PersistenceEngine::full_rebuild_exact(pos, out);
}

void apply_move_incremental(core::StateInfo& undo,
                            const core::Position& pos_after,
                            TopoSummary& current)
{
    if (!g_enabled) return;

    // Store the CURRENT summary state before applying any changes
    // This allows exact restoration on undo
    undo.topo_delta.prev_summary = current;

    // Compute new aggregated stalks
    SheafStalk new_white = aggregate_color_stalks(pos_after, core::Color::White);
    SheafStalk new_black = aggregate_color_stalks(pos_after, core::Color::Black);

    // Store stalks for potential future use
    undo.topo_delta.prev_stalks[0] = undo.topo_delta.new_stalks[0];
    undo.topo_delta.prev_stalks[1] = undo.topo_delta.new_stalks[1];
    undo.topo_delta.new_stalks[0] = new_white;
    undo.topo_delta.new_stalks[1] = new_black;
    undo.topo_delta.white_stalks_changed = 1;
    undo.topo_delta.black_stalks_changed = 1;

    // Phase 0: Don't apply incremental deltas yet
    // The summary keeps its last Tier 2 value
    // Phase 1 will add the learned surrogate path
    undo.topo_delta.delta_h1_tension = 0.0f;
    undo.topo_delta.delta_h1_loop_count = 0.0f;
    undo.topo_delta.delta_h0_consistency = 0.0f;
}

void undo_move_incremental(const core::StateInfo& undo,
                           TopoSummary& current)
{
    if (!g_enabled) return;

    // Restore the exact summary state from before the move
    current = undo.topo_delta.prev_summary;
}

void rebuild_local(const core::Position& pos,
                   const std::vector<core::Square>& changed_squares,
                   TopoSummary& current)
{
    if (!g_enabled) return;

    // Tier 1: Local rebuild around changed squares
    // For Phase 0-1, we do a simplified version:
    // 1. Compute a local neighborhood around the changed squares
    // 2. Run the exact persistence computation on just that neighborhood
    // 3. Blend the result with the existing summary

    // For now, we just do a full rebuild (this will be optimized later)
    // In Phase 2, we'll implement proper local zigzag persistence

    if (changed_squares.empty()) return;

    // Do a full rebuild for now
    TopoSummary exact;
    PersistenceEngine::full_rebuild_exact(pos, exact);

    // Blend with existing summary (weighted average)
    // Use higher weight for the exact computation
    float alpha = 0.8f;  // Weight for exact computation
    current.h0_persist_sum = alpha * exact.h0_persist_sum + (1.0f - alpha) * current.h0_persist_sum;
    current.h1_tension = alpha * exact.h1_tension + (1.0f - alpha) * current.h1_tension;
    current.h1_loop_count = alpha * exact.h1_loop_count + (1.0f - alpha) * current.h1_loop_count;
    current.h0_consistency = alpha * exact.h0_consistency + (1.0f - alpha) * current.h0_consistency;

    // Update persistence image
    for (size_t i = 0; i < PERSIST_IMAGE_DIM; ++i) {
        current.persist_image[i] = alpha * exact.persist_image[i] + (1.0f - alpha) * current.persist_image[i];
    }

    current.rebuild_generation++;
}

void extract_features(const TopoSummary& summary,
                      std::array<float, TOPO_FEATURE_DIM>& white_features,
                      std::array<float, TOPO_FEATURE_DIM>& black_features)
{
    // The TopoSummary contains position-independent features
    // We distribute them to both color perspectives

    // First 4 dims: cohomology scalars
    white_features[0] = summary.h0_persist_sum;
    white_features[1] = summary.h1_tension;
    white_features[2] = summary.h1_loop_count;
    white_features[3] = summary.h0_consistency;

    black_features[0] = summary.h0_persist_sum;
    black_features[1] = summary.h1_tension;
    black_features[2] = summary.h1_loop_count;
    black_features[3] = summary.h0_consistency;

    // Remaining dims: persistence image (shared)
    for (size_t i = 0; i < PERSIST_IMAGE_DIM && (i + 4) < TOPO_FEATURE_DIM; ++i) {
        white_features[i + 4] = summary.persist_image[i];
        black_features[i + 4] = summary.persist_image[i];
    }

    // Zero any remaining dims
    for (size_t i = PERSIST_IMAGE_DIM + 4; i < TOPO_FEATURE_DIM; ++i) {
        white_features[i] = 0.0f;
        black_features[i] = 0.0f;
    }
}

std::string format_summary(const TopoSummary& summary) {
    std::ostringstream out;
    out << "SheafTop Summary:\n";
    out << "  H0 Persistence Sum: " << summary.h0_persist_sum << "\n";
    out << "  H1 Tension:         " << summary.h1_tension << "\n";
    out << "  H1 Loop Count:      " << summary.h1_loop_count << "\n";
    out << "  H0 Consistency:     " << summary.h0_consistency << "\n";
    out << "  Rebuild Generation: " << summary.rebuild_generation << "\n";

    out << "  Persistence Image (first 16): [";
    for (size_t i = 0; i < 16 && i < PERSIST_IMAGE_DIM; ++i) {
        if (i > 0) out << ", ";
        out << std::fixed;
        out.precision(3);
        out << summary.persist_image[i];
    }
    out << "...]\n";

    return out.str();
}

bool verify_consistency(const core::Position& pos,
                        const TopoSummary& incremental,
                        float tolerance)
{
    // Compute exact version
    TopoSummary exact;
    full_rebuild(pos, exact);

    // Compare scalars
    float error_h0 = std::abs(exact.h0_persist_sum - incremental.h0_persist_sum);
    float error_h1 = std::abs(exact.h1_tension - incremental.h1_tension);
    float error_loop = std::abs(exact.h1_loop_count - incremental.h1_loop_count);
    float error_consist = std::abs(exact.h0_consistency - incremental.h0_consistency);

    // Normalize errors relative to the exact values
    float max_val = std::max({std::abs(exact.h0_persist_sum),
                              std::abs(exact.h1_tension),
                              std::abs(exact.h1_loop_count),
                              std::abs(exact.h0_consistency),
                              1.0f});  // Avoid division by zero

    float rel_error = (error_h0 + error_h1 + error_loop + error_consist) / (4.0f * max_val);

    return rel_error <= tolerance;
}

void set_enabled(bool enabled) {
    g_enabled = enabled;
}

bool is_enabled() {
    return g_enabled;
}

void set_current_tier(int tier) {
    g_current_tier = std::clamp(tier, 0, 2);
}

int get_current_tier() {
    return g_current_tier;
}

} // namespace lulzfish::eval::sheaftop
