//==============================================================================
// Lulzfish SheafTop — Persistence Engine Implementation
//==============================================================================
//
// Phase 0: Exact computation using simplified union-find for H0 and
//          cycle detection for H1. Used only at root and data generation.
//
// Phase 1+: Learned linear surrogates for incremental updates.
//==============================================================================

#include "persistence.hpp"

#include "lulzfish/core/attacks.hpp"
#include "lulzfish/core/position.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lulzfish::eval::sheaftop {

//==============================================================================
// Static members
//==============================================================================

std::array<float, STALK_DIM> PersistenceEngine::surrogate_weights_h1_tension_{};
std::array<float, STALK_DIM> PersistenceEngine::surrogate_weights_h1_loop_{};
std::array<float, STALK_DIM> PersistenceEngine::surrogate_weights_h0_consistency_{};
bool PersistenceEngine::initialized_ = false;

namespace {

constexpr int PIECE_VALUES[7] = {0, 100, 320, 330, 500, 900, 0};

int piece_value(core::PieceType pt) {
    return PIECE_VALUES[static_cast<int>(pt)];
}

int square_distance(core::Square a, core::Square b) {
    return std::max(std::abs(core::file_of(a) - core::file_of(b)),
                    std::abs(core::rank_of(a) - core::rank_of(b)));
}

float normalized_mobility(const core::Position& pos, core::Square sq, core::PieceType pt, core::Color color) {
    if (pt == core::PieceType::Pawn || pt == core::PieceType::King) return 0.0f;

    core::Bitboard attacks = core::EmptyBB;
    core::Bitboard occ = pos.occupancy();

    switch (pt) {
        case core::PieceType::Knight: attacks = core::knight_attacks_bb(sq); break;
        case core::PieceType::Bishop: attacks = core::bishop_attacks_bb(sq, occ); break;
        case core::PieceType::Rook:   attacks = core::rook_attacks_bb(sq, occ); break;
        case core::PieceType::Queen:  attacks = core::queen_attacks_bb(sq, occ); break;
        default: break;
    }
    attacks &= ~pos.pieces(color);

    return static_cast<float>(core::popcount(attacks)) / 28.0f;
}

float king_pressure(const core::Position& pos, core::Square sq, core::PieceType pt, core::Color color) {
    core::Color enemy = core::opposite(color);
    core::Bitboard enemy_king = pos.pieces(core::make_piece(enemy, core::PieceType::King));
    if (!enemy_king) return 0.0f;

    core::Square king_sq = core::lsb_square(enemy_king);
    int dist = square_distance(sq, king_sq);
    float pressure = std::max(0.0f, (8.0f - static_cast<float>(dist)) / 8.0f);

    // Weight by piece value
    float value_norm = static_cast<float>(piece_value(pt)) / 1000.0f;
    return pressure * value_norm;
}

float pawn_structure_flags(const core::Position& pos, core::Square sq, core::Color color) {
    if (pos.piece_on(sq) != core::make_piece(color, core::PieceType::Pawn)) {
        return 0.0f;
    }

    float flags = 0.0f;
    int file = core::file_of(sq);
    int rank = core::rank_of(sq);
    int rel_rank = (color == core::Color::White) ? rank : (7 - rank);

    // Passed pawn check
    core::Color enemy = core::opposite(color);
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
    if (passed) flags += 0.25f;

    // Isolated pawn check
    core::Bitboard own_pawns = pos.pieces(core::make_piece(color, core::PieceType::Pawn));
    bool isolated = true;
    for (int f = std::max(0, file - 1); f <= std::min(7, file + 1); ++f) {
        if (f == file) continue;
        if (own_pawns & (core::FileA << f)) {
            isolated = false;
            break;
        }
    }
    if (isolated) flags += 0.25f;

    // Doubled pawn check
    core::Bitboard same_file = own_pawns & (core::FileA << file);
    core::clear_bit(same_file, sq);
    if (same_file) flags += 0.25f;

    // Chain participation (defended by another pawn)
    core::Bitboard defenders = pos.attackers_to(sq, color) & own_pawns;
    if (defenders) flags += 0.25f;

    return flags;
}

float positional_context(core::Square sq, core::Color color) {
    int rel_rank = (color == core::Color::White) ? core::rank_of(sq) : (7 - core::rank_of(sq));
    int file = core::file_of(sq);
    float rank_norm = static_cast<float>(rel_rank) / 7.0f;
    float file_norm = (file < 4) ? static_cast<float>(file) / 3.0f
                                 : static_cast<float>(7 - file) / 3.0f;
    return (rank_norm + file_norm) * 0.5f;
}

float compute_tension(const core::Position& pos, core::Square from, core::Square to, core::PieceType attacker_type) {
    core::Piece victim = pos.piece_on(to);
    if (victim == core::Piece::None) return 0.0f;

    float victim_val = static_cast<float>(piece_value(core::type_of(victim)));
    float attacker_val = static_cast<float>(piece_value(attacker_type));
    int dist = square_distance(from, to);

    // Base tension: material exchange value weighted by distance
    float material_tension = (victim_val / (attacker_val + 1.0f)) *
                             (8.0f - static_cast<float>(dist)) / 8.0f;

    // King safety bonus: attacks near the enemy king are more important
    core::Color attacker_color = core::color_of(pos.piece_on(from));
    core::Color enemy = core::opposite(attacker_color);
    core::Bitboard enemy_king = pos.pieces(core::make_piece(enemy, core::PieceType::King));
    float king_bonus = 0.0f;
    if (enemy_king) {
        core::Square king_sq = core::lsb_square(enemy_king);
        int king_dist = square_distance(to, king_sq);
        if (king_dist <= 2) {
            king_bonus = 2.0f * (3.0f - static_cast<float>(king_dist)) / 3.0f;
        }
    }

    // Piece activity bonus: central squares are more important
    int file = core::file_of(to);
    int rank = core::rank_of(to);
    int center_dist = std::min(std::abs(file - 3), std::abs(file - 4)) +
                      std::min(std::abs(rank - 3), std::abs(rank - 4));
    float activity_bonus = (4.0f - static_cast<float>(center_dist)) / 4.0f * 0.5f;

    // Check bonus: if the attack gives check, it's much more important
    float check_bonus = 0.0f;
    core::Bitboard attacks = core::EmptyBB;
    switch (attacker_type) {
        case core::PieceType::Pawn:   attacks = core::pawn_attacks(from, attacker_color); break;
        case core::PieceType::Knight: attacks = core::knight_attacks_bb(from); break;
        case core::PieceType::Bishop: attacks = core::bishop_attacks_bb(from, pos.occupancy()); break;
        case core::PieceType::Rook:   attacks = core::rook_attacks_bb(from, pos.occupancy()); break;
        case core::PieceType::Queen:  attacks = core::queen_attacks_bb(from, pos.occupancy()); break;
        case core::PieceType::King:   attacks = core::king_attacks_bb(from); break;
        default: break;
    }
    if (attacks & enemy_king) {
        check_bonus = 3.0f;
    }

    float tension = material_tension + king_bonus + activity_bonus + check_bonus;
    return std::clamp(tension, 0.0f, 10.0f);
}

} // anonymous namespace

//==============================================================================
// UnionFind
//==============================================================================

void PersistenceEngine::UnionFind::init(size_t n) {
    parent.resize(n);
    rank.resize(n, 0);
    std::iota(parent.begin(), parent.end(), 0);
}

int PersistenceEngine::UnionFind::find(int x) {
    if (parent[x] != x) {
        parent[x] = find(parent[x]);
    }
    return parent[x];
}

void PersistenceEngine::UnionFind::unite(int x, int y) {
    int rx = find(x);
    int ry = find(y);
    if (rx == ry) return;

    if (rank[rx] < rank[ry]) std::swap(rx, ry);
    parent[ry] = rx;
    if (rank[rx] == rank[ry]) ++rank[rx];
}

//==============================================================================
// Filtered Complex Construction
//==============================================================================

void PersistenceEngine::build_filtered_complex(
    const core::Position& pos,
    std::vector<FiltrationEdge>& edges)
{
    edges.clear();

    // Collect all pieces with indices
    struct PieceEntry {
        core::Square sq;
        core::Piece piece;
        core::Color color;
        core::PieceType type;
    };

    std::vector<PieceEntry> pieces;
    for (int p = 1; p < 13; ++p) {
        core::Piece piece = static_cast<core::Piece>(p);
        core::Bitboard bb = pos.pieces(piece);
        while (bb) {
            core::Square sq = core::lsb_square(bb);
            pieces.push_back({sq, piece, core::color_of(piece), core::type_of(piece)});
            (void)core::pop_lsb(bb);
        }
    }

    // Build edges for ATTACKS and DEFENDS relations
    for (size_t i = 0; i < pieces.size(); ++i) {
        const auto& pi = pieces[i];
        core::Bitboard attacks = core::EmptyBB;
        core::Bitboard occ = pos.occupancy();

        switch (pi.type) {
            case core::PieceType::Pawn:   attacks = core::pawn_attacks(pi.sq, pi.color); break;
            case core::PieceType::Knight: attacks = core::knight_attacks_bb(pi.sq); break;
            case core::PieceType::Bishop: attacks = core::bishop_attacks_bb(pi.sq, occ); break;
            case core::PieceType::Rook:   attacks = core::rook_attacks_bb(pi.sq, occ); break;
            case core::PieceType::Queen:  attacks = core::queen_attacks_bb(pi.sq, occ); break;
            case core::PieceType::King:   attacks = core::king_attacks_bb(pi.sq); break;
            default: break;
        }

        // ATTACKS (enemy pieces)
        core::Bitboard enemies = attacks & pos.pieces(core::opposite(pi.color));
        while (enemies) {
            core::Square target = core::lsb_square(enemies);
            float tension = compute_tension(pos, pi.sq, target, pi.type);

            // Find target index
            for (size_t j = 0; j < pieces.size(); ++j) {
                if (pieces[j].sq == target) {
                    edges.push_back({
                        static_cast<uint8_t>(i),
                        static_cast<uint8_t>(j),
                        tension,
                        0  // ATTACKS
                    });
                    break;
                }
            }
            (void)core::pop_lsb(enemies);
        }

        // DEFENDS (friendly pieces)
        core::Bitboard friends = attacks & pos.pieces(pi.color) & ~core::square_bb(pi.sq);
        while (friends) {
            core::Square target = core::lsb_square(friends);
            float tension = 0.1f;  // Defense relations have low tension

            for (size_t j = 0; j < pieces.size(); ++j) {
                if (pieces[j].sq == target) {
                    edges.push_back({
                        static_cast<uint8_t>(i),
                        static_cast<uint8_t>(j),
                        tension,
                        1  // DEFENDS
                    });
                    break;
                }
            }
            (void)core::pop_lsb(friends);
        }
    }

    // Sort by tension (descending) for filtration
    std::sort(edges.begin(), edges.end(),
        [](const FiltrationEdge& a, const FiltrationEdge& b) {
            return a.tension > b.tension;
        });
}

//==============================================================================
// H0 Persistence (Connected Components)
//==============================================================================

void PersistenceEngine::compute_h0_persistence(
    const std::vector<FiltrationEdge>& edges,
    size_t num_pieces,
    std::vector<PersistencePair>& diagram)
{
    UnionFind uf;
    uf.init(num_pieces);

    // Track when each component was "born"
    std::vector<float> birth_time(num_pieces, 0.0f);

    // Initially, each piece is its own component born at t=0
    for (size_t i = 0; i < num_pieces; ++i) {
        birth_time[i] = 0.0f;
    }

    int num_components = static_cast<int>(num_pieces);

    // Process edges from highest to lowest tension
    for (const auto& edge : edges) {
        int root_from = uf.find(edge.from_idx);
        int root_to = uf.find(edge.to_idx);

        if (root_from != root_to) {
            // Merge components: the younger one "dies"
            float death_time = edge.tension;

            // The component that dies is the one born later
            float from_birth = birth_time[root_from];
            float to_birth = birth_time[root_to];

            if (from_birth >= to_birth) {
                // root_from dies
                diagram.push_back({from_birth, death_time, 0});
                uf.unite(root_from, root_to);
                int new_root = uf.find(root_from);
                birth_time[new_root] = std::min(from_birth, to_birth);
            } else {
                // root_to dies
                diagram.push_back({to_birth, death_time, 0});
                uf.unite(root_from, root_to);
                int new_root = uf.find(root_from);
                birth_time[new_root] = std::min(from_birth, to_birth);
            }

            --num_components;
        }
    }

    // The last surviving component has infinite persistence
    // We represent this as birth=0, death=max_tension+1
    if (num_components > 0) {
        float max_tension = edges.empty() ? 1.0f : edges.front().tension;
        diagram.push_back({0.0f, max_tension + 1.0f, 0});
    }
}

//==============================================================================
// H1 Persistence (Loops) — Improved for chess
//==============================================================================
//
// Detects cycles in the attack/defense graph:
// - 3-cycles (triangles): basic tactical patterns
// - 4-cycles: defensive perimeters, pawn chains
// - 5-cycles: more complex structures
//
// Each cycle's "birth" is when the last edge completes it,
// "death" is when the weakest edge appears in the filtration.

void PersistenceEngine::compute_h1_persistence(
    const std::vector<FiltrationEdge>& edges,
    size_t num_pieces,
    std::vector<PersistencePair>& diagram)
{
    // Build adjacency list (only significant edges)
    struct AdjEntry {
        uint8_t neighbor;
        float tension;
    };

    std::vector<std::vector<AdjEntry>> adj(num_pieces);
    for (const auto& edge : edges) {
        if (edge.tension > 0.1f) {  // Filter noise
            adj[edge.from_idx].push_back({edge.to_idx, edge.tension});
            adj[edge.to_idx].push_back({edge.from_idx, edge.tension});
        }
    }

    // Find cycles up to length 5
    for (size_t i = 0; i < num_pieces; ++i) {
        for (const auto& e1 : adj[i]) {
            if (e1.neighbor <= i) continue;

            for (const auto& e2 : adj[e1.neighbor]) {
                if (e2.neighbor <= i) continue;  // Allow back to i for 3-cycle

                // 3-cycle: i -> e1.neighbor -> e2.neighbor -> i
                if (e2.neighbor == i) {
                    float min_t = std::min({e1.tension, e2.tension});
                    float max_t = std::max({e1.tension, e2.tension});
                    if (max_t - min_t > 0.01f) {
                        diagram.push_back({min_t, max_t, 1});
                    }
                    continue;
                }

                for (const auto& e3 : adj[e2.neighbor]) {
                    if (e3.neighbor <= i) continue;

                    // 4-cycle: i -> e1 -> e2 -> e3 -> i
                    if (e3.neighbor == i) {
                        float min_t = std::min({e1.tension, e2.tension, e3.tension});
                        float max_t = std::max({e1.tension, e2.tension, e3.tension});
                        if (max_t - min_t > 0.01f) {
                            diagram.push_back({min_t, max_t, 1});
                        }
                        continue;
                    }

                    // 5-cycle: i -> e1 -> e2 -> e3 -> e4 -> i
                    for (const auto& e4 : adj[e3.neighbor]) {
                        if (e4.neighbor == i) {
                            float min_t = std::min({e1.tension, e2.tension, e3.tension, e4.tension});
                            float max_t = std::max({e1.tension, e2.tension, e3.tension, e4.tension});
                            if (max_t - min_t > 0.01f) {
                                diagram.push_back({min_t, max_t, 1});
                            }
                        }
                    }
                }
            }
        }
    }
}

//==============================================================================
// Cohomology Scalars
//==============================================================================

void PersistenceEngine::compute_cohomology_scalars(
    const core::Position& pos,
    const std::vector<PersistencePair>& h0_diagram,
    const std::vector<PersistencePair>& h1_diagram,
    TopoSummary& out)
{
    // H0: Total persistence (sum of death-birth for all components)
    out.h0_persist_sum = 0.0f;
    for (const auto& pair : h0_diagram) {
        out.h0_persist_sum += pair.persistence();
    }

    // H1: Tension (persistence-weighted sum of loops)
    out.h1_tension = 0.0f;
    out.h1_loop_count = 0.0f;
    float weighted_tension = 0.0f;
    for (const auto& pair : h1_diagram) {
        float persist = pair.persistence();
        if (persist > 0.01f) {  // Filter noise
            out.h1_tension += persist;
            out.h1_loop_count += 1.0f;
            // Weight by persistence (more persistent = more important)
            weighted_tension += persist * persist;
        }
    }
    // Normalize weighted tension
    if (out.h1_loop_count > 0) {
        weighted_tension /= out.h1_loop_count;
    }
    out.h1_tension = weighted_tension;  // Use persistence-weighted version

    // H0 consistency: ratio of long-lived components to total
    int long_lived = 0;
    int total = static_cast<int>(h0_diagram.size());
    for (const auto& pair : h0_diagram) {
        if (pair.persistence() > 0.5f) {
            ++long_lived;
        }
    }
    out.h0_consistency = (total > 0) ?
        static_cast<float>(long_lived) / static_cast<float>(total) : 0.0f;

    // Additional chess-specific scalars (stored in persistence image slots)
    // We'll encode these as the first few values of the persistence image

    // Count pieces in each color for normalization
    int white_pieces = core::popcount(pos.pieces(core::Color::White));
    int black_pieces = core::popcount(pos.pieces(core::Color::Black));
    float piece_count_norm = static_cast<float>(white_pieces + black_pieces);

    // Average persistence per component (structural stability)
    float avg_h0_persist = (total > 0) ? out.h0_persist_sum / static_cast<float>(total) : 0.0f;

    // Max persistence (most stable structure)
    float max_h0_persist = 0.0f;
    for (const auto& pair : h0_diagram) {
        max_h0_persist = std::max(max_h0_persist, pair.persistence());
    }

    // Store additional features in the first slots of persistence image
    // These will be extracted separately
    out.persist_image[0] = avg_h0_persist;
    out.persist_image[1] = max_h0_persist;
    out.persist_image[2] = piece_count_norm / 32.0f;  // Normalized
    out.persist_image[3] = out.h1_loop_count / 10.0f;  // Normalized
}

//==============================================================================
// Persistence Image Computation
//==============================================================================

void PersistenceEngine::compute_persistence_image(
    const std::vector<PersistencePair>& diagram,
    std::array<float, PERSIST_IMAGE_DIM>& out_image)
{
    out_image.fill(0.0f);

    if (diagram.empty()) return;

    // Find the range of birth/death values
    float min_birth = diagram[0].birth;
    float max_birth = diagram[0].birth;
    float min_death = diagram[0].death;
    float max_death = diagram[0].death;

    for (const auto& pair : diagram) {
        min_birth = std::min(min_birth, pair.birth);
        max_birth = std::max(max_birth, pair.birth);
        min_death = std::min(min_death, pair.death);
        max_death = std::max(max_death, pair.death);
    }

    // Create a grid representation
    // We use a simplified approach: divide the persistence plane into cells
    // and accumulate weighted counts

    const int grid_size = 8;  // 8x8 grid = 64 cells, but we compress to PERSIST_IMAGE_DIM
    float birth_range = max_birth - min_birth;
    float death_range = max_death - min_death;

    if (birth_range < 0.001f) birth_range = 1.0f;
    if (death_range < 0.001f) death_range = 1.0f;

    // Temporary grid
    std::array<float, 64> grid{};
    grid.fill(0.0f);

    for (const auto& pair : diagram) {
        float persistence = pair.persistence();
        if (persistence <= 0.0f) continue;

        // Normalize birth and death to [0, 1]
        float norm_birth = (pair.birth - min_birth) / birth_range;
        float norm_death = (pair.death - min_death) / death_range;

        // Find grid cell
        int bx = std::min(static_cast<int>(norm_birth * grid_size), grid_size - 1);
        int by = std::min(static_cast<int>(norm_death * grid_size), grid_size - 1);

        int cell = bx * grid_size + by;
        if (cell >= 0 && cell < 64) {
            // Weight by persistence (more persistent = more important)
            grid[cell] += persistence;
        }
    }

    // Compress 64 -> PERSIST_IMAGE_DIM by averaging pairs
    for (size_t i = 0; i < PERSIST_IMAGE_DIM; ++i) {
        size_t src_idx = i * 64 / PERSIST_IMAGE_DIM;
        out_image[i] = grid[src_idx];
    }
}

//==============================================================================
// Full Rebuild (Tier 2)
//==============================================================================

void PersistenceEngine::full_rebuild_exact(
    const core::Position& pos,
    TopoSummary& out)
{
    out.clear();

    // Build filtered complex
    std::vector<FiltrationEdge> edges;
    build_filtered_complex(pos, edges);

    // Count pieces
    size_t num_pieces = 0;
    for (int p = 1; p < 13; ++p) {
        num_pieces += core::popcount(pos.pieces(static_cast<core::Piece>(p)));
    }

    // Compute persistence diagrams
    std::vector<PersistencePair> h0_diagram;
    std::vector<PersistencePair> h1_diagram;

    compute_h0_persistence(edges, num_pieces, h0_diagram);
    compute_h1_persistence(edges, num_pieces, h1_diagram);

    // Compute cohomology scalars
    compute_cohomology_scalars(pos, h0_diagram, h1_diagram, out);

    // Combine diagrams for persistence image
    std::vector<PersistencePair> combined_diagram;
    combined_diagram.reserve(h0_diagram.size() + h1_diagram.size());
    combined_diagram.insert(combined_diagram.end(), h0_diagram.begin(), h0_diagram.end());
    combined_diagram.insert(combined_diagram.end(), h1_diagram.begin(), h1_diagram.end());

    compute_persistence_image(combined_diagram, out.persist_image);

    out.rebuild_generation = 1;
}

//==============================================================================
// Learned Surrogate (Phase 1)
//==============================================================================

TopologicalDelta PersistenceEngine::predict_delta(
    const SheafStalk& old_stalk_white,
    const SheafStalk& new_stalk_white,
    const SheafStalk& old_stalk_black,
    const SheafStalk& new_stalk_black)
{
    TopologicalDelta delta;

    // Compute stalk deltas
    SheafStalk white_delta = new_stalk_white - old_stalk_white;
    SheafStalk black_delta = new_stalk_black - old_stalk_black;
    delta.prev_stalks[0] = old_stalk_white;
    delta.prev_stalks[1] = old_stalk_black;
    delta.new_stalks[0] = new_stalk_white;
    delta.new_stalks[1] = new_stalk_black;
    delta.white_stalks_changed = 1;
    delta.black_stalks_changed = 1;

    // Use learned linear surrogate to predict scalar changes
    // This is a simple dot product: w · delta_stalk

    if (initialized_) {
        delta.delta_h1_tension =
            dot_product(surrogate_weights_h1_tension_, white_delta.data) +
            dot_product(surrogate_weights_h1_tension_, black_delta.data);

        delta.delta_h1_loop_count =
            dot_product(surrogate_weights_h1_loop_, white_delta.data) +
            dot_product(surrogate_weights_h1_loop_, black_delta.data);

        delta.delta_h0_consistency =
            dot_product(surrogate_weights_h0_consistency_, white_delta.data) +
            dot_product(surrogate_weights_h0_consistency_, black_delta.data);
    }

    return delta;
}

//==============================================================================
// Initialization
//==============================================================================

void PersistenceEngine::init()
{
    if (initialized_) return;

    // Initialize surrogate weights to small random values
    // In production, these would be loaded from a trained model file
    // For Phase 0, we use simple heuristics

    // H1 tension: correlates with king pressure and mobility changes
    surrogate_weights_h1_tension_[0] = 0.0f;   // Pawn onehot
    surrogate_weights_h1_tension_[1] = 0.1f;   // Knight onehot
    surrogate_weights_h1_tension_[2] = 0.1f;   // Bishop onehot
    surrogate_weights_h1_tension_[3] = 0.15f;  // Rook onehot
    surrogate_weights_h1_tension_[4] = 0.2f;   // Queen onehot
    surrogate_weights_h1_tension_[5] = 0.3f;   // material_norm
    surrogate_weights_h1_tension_[6] = 0.4f;   // mobility_norm
    surrogate_weights_h1_tension_[7] = 0.8f;   // king_pressure (strongest signal)
    surrogate_weights_h1_tension_[8] = 0.2f;   // pawn_structure_flags
    surrogate_weights_h1_tension_[9] = 0.1f;   // positional_context

    // H1 loop count: correlates with pawn structure changes
    surrogate_weights_h1_loop_[0] = 0.3f;   // Pawn onehot
    surrogate_weights_h1_loop_[1] = 0.0f;
    surrogate_weights_h1_loop_[2] = 0.0f;
    surrogate_weights_h1_loop_[3] = 0.0f;
    surrogate_weights_h1_loop_[4] = 0.0f;
    surrogate_weights_h1_loop_[5] = 0.1f;
    surrogate_weights_h1_loop_[6] = 0.05f;
    surrogate_weights_h1_loop_[7] = 0.0f;
    surrogate_weights_h1_loop_[8] = 0.6f;   // pawn_structure_flags (strongest signal)
    surrogate_weights_h1_loop_[9] = 0.15f;

    // H0 consistency: correlates with overall material balance
    surrogate_weights_h0_consistency_[0] = 0.05f;
    surrogate_weights_h0_consistency_[1] = 0.1f;
    surrogate_weights_h0_consistency_[2] = 0.1f;
    surrogate_weights_h0_consistency_[3] = 0.1f;
    surrogate_weights_h0_consistency_[4] = 0.15f;
    surrogate_weights_h0_consistency_[5] = 0.5f;   // material_norm (strongest)
    surrogate_weights_h0_consistency_[6] = 0.3f;
    surrogate_weights_h0_consistency_[7] = 0.2f;
    surrogate_weights_h0_consistency_[8] = 0.2f;
    surrogate_weights_h0_consistency_[9] = 0.1f;

    initialized_ = true;
}

//==============================================================================
// Helper
//==============================================================================

float PersistenceEngine::dot_product(
    const std::array<float, STALK_DIM>& weights,
    const std::array<float, STALK_DIM>& values)
{
    float sum = 0.0f;
    for (size_t i = 0; i < STALK_DIM; ++i) {
        sum += weights[i] * values[i];
    }
    return sum;
}

} // namespace lulzfish::eval::sheaftop
