//==============================================================================
// Lulzfish Graph Evaluation Benchmark
//==============================================================================
//
// This is a measurement harness for the core novelty claim: the relational
// representation must become cheap enough to update and evaluate during search.
// It intentionally reports numbers rather than enforcing thresholds because the
// first job is to establish a baseline on the local machine.
//==============================================================================

#include "lulzfish/core/attacks.hpp"
#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/eval/graph_eval.hpp"
#include "lulzfish/eval/sheaftop.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <tuple>
#include <vector>

using namespace lulzfish::core;

namespace {

using Clock = std::chrono::steady_clock;
using RelationKey = std::tuple<int, int, int>;
using NodeKey = std::pair<int, int>;

struct BenchPosition {
    std::string name;
    std::string fen;
};

struct PreparedPosition {
    std::string name;
    Position pos;
    std::vector<Move> legal_moves;
};

struct TimerResult {
    double seconds = 0.0;
    uint64_t units = 0;
};

std::vector<RelationKey> sorted_relation_keys(const lulzfish::eval::graph::PositionGraph& graph) {
    std::vector<RelationKey> keys;
    keys.reserve(graph.relation_count());
    for (const auto& relation : graph.relations()) {
        keys.emplace_back(static_cast<int>(relation.type),
                          static_cast<int>(relation.from),
                          static_cast<int>(relation.to));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<NodeKey> sorted_node_keys(const lulzfish::eval::graph::PositionGraph& graph) {
    std::vector<NodeKey> keys;
    keys.reserve(graph.node_count());
    for (const auto& node : graph.nodes()) {
        keys.emplace_back(static_cast<int>(node.piece), static_cast<int>(node.square));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

bool graph_matches_rebuild(const Position& pos) {
    lulzfish::eval::graph::PositionGraph rebuilt;
    rebuilt.update_from_position(pos);

    return sorted_node_keys(pos.graph()) == sorted_node_keys(rebuilt) &&
           sorted_relation_keys(pos.graph()) == sorted_relation_keys(rebuilt);
}

double per_second(const TimerResult& result) {
    if (result.seconds <= 0.0) return 0.0;
    return static_cast<double>(result.units) / result.seconds;
}

TimerResult bench_eval(const std::vector<PreparedPosition>& positions, int iterations) {
    volatile int sink = 0;
    uint64_t evals = 0;
    auto start = Clock::now();

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& entry : positions) {
            sink += lulzfish::eval::graph::evaluate(entry.pos);
            ++evals;
        }
    }

    auto end = Clock::now();
    (void)sink;
    return {std::chrono::duration<double>(end - start).count(), evals};
}

TimerResult bench_feature_extract(const std::vector<PreparedPosition>& positions, int iterations) {
    volatile float sink = 0.0f;
    uint64_t extracts = 0;
    std::array<float, lulzfish::eval::graph::FEATURES_TOTAL> features{};
    auto start = Clock::now();

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& entry : positions) {
            lulzfish::eval::graph::extract_features(entry.pos, features);
            sink += features[0];
            ++extracts;
        }
    }

    auto end = Clock::now();
    (void)sink;
    return {std::chrono::duration<double>(end - start).count(), extracts};
}

TimerResult bench_graph_rebuild(const std::vector<PreparedPosition>& positions, int iterations) {
    volatile size_t sink = 0;
    uint64_t rebuilds = 0;
    auto start = Clock::now();

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& entry : positions) {
            lulzfish::eval::graph::PositionGraph graph;
            graph.update_from_position(entry.pos);
            sink += graph.relation_count();
            ++rebuilds;
        }
    }

    auto end = Clock::now();
    (void)sink;
    return {std::chrono::duration<double>(end - start).count(), rebuilds};
}

TimerResult bench_make_unmake_graph_updates(const std::vector<PreparedPosition>& positions, int iterations) {
    uint64_t moves = 0;
    auto start = Clock::now();

    for (const auto& entry : positions) {
        Position pos = entry.pos;

        for (int iter = 0; iter < iterations; ++iter) {
            StateInfo undo;
            for (Move move : entry.legal_moves) {
                pos.make_move(move, undo);
                pos.unmake_move(move, undo);
                ++moves;
            }
        }
    }

    auto end = Clock::now();
    return {std::chrono::duration<double>(end - start).count(), moves};
}

TimerResult bench_topo_rebuild(const std::vector<PreparedPosition>& positions, int iterations) {
    uint64_t rebuilds = 0;
    auto start = Clock::now();

    for (int iter = 0; iter < iterations; ++iter) {
        for (const auto& entry : positions) {
            lulzfish::eval::sheaftop::TopoSummary summary;
            lulzfish::eval::sheaftop::full_rebuild(entry.pos, summary);
            ++rebuilds;
        }
    }

    auto end = Clock::now();
    return {std::chrono::duration<double>(end - start).count(), rebuilds};
}

bool topo_consistency_check(const std::vector<PreparedPosition>& positions) {
    bool all_ok = true;
    int checked = 0;
    int passed = 0;

    for (const auto& entry : positions) {
        // Compute exact topology
        lulzfish::eval::sheaftop::TopoSummary exact;
        lulzfish::eval::sheaftop::full_rebuild(entry.pos, exact);

        // Check that incremental matches exact (Phase 0: they should be identical)
        const auto& incremental = entry.pos.graph().topo_summary();
        bool ok = lulzfish::eval::sheaftop::verify_consistency(entry.pos, incremental, 0.01f);

        if (!ok) {
            std::cout << "  WARNING: Topology consistency check failed for " << entry.name << "\n";
            std::cout << "    Exact:      h1_tension=" << exact.h1_tension
                      << " h1_loop=" << exact.h1_loop_count << "\n";
            std::cout << "    Incremental: h1_tension=" << incremental.h1_tension
                      << " h1_loop=" << incremental.h1_loop_count << "\n";
            all_ok = false;
        }
        ++checked;
        if (ok) ++passed;
    }

    std::cout << "  Topology consistency: " << passed << "/" << checked << " passed\n";
    return all_ok;
}

void print_topo_summary(const std::string& name, const Position& pos) {
    const auto& summary = pos.graph().topo_summary();
    std::cout << "  " << std::left << std::setw(12) << name
              << " h0_persist=" << std::right << std::setw(6) << std::fixed << std::setprecision(2) << summary.h0_persist_sum
              << " h1_tension=" << std::setw(6) << summary.h1_tension
              << " h1_loops=" << std::setw(4) << summary.h1_loop_count
              << " h0_consist=" << std::setw(5) << summary.h0_consistency << "\n";
}

void print_metric(const std::string& name, const TimerResult& result, const std::string& unit) {
    std::cout << std::left << std::setw(34) << name
              << std::right << std::setw(12) << result.units << " "
              << std::setw(10) << std::fixed << std::setprecision(3) << result.seconds << " s  "
              << std::setw(12) << std::fixed << std::setprecision(1) << per_second(result)
              << " " << unit << "/s\n";
}

} // namespace

int main() {
    init_attack_tables();

    const std::vector<BenchPosition> bench_positions = {
        {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
        {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"},
        {"open_game", "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 2 3"},
        {"middlegame", "2r2r1k/4Q1p1/pB4qp/P2Pp3/2P2p2/3P3b/5PPP/1R3RK1 w - - 1 24"},
        {"endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"},
    };

    std::vector<PreparedPosition> positions;
    positions.reserve(bench_positions.size());
    for (const auto& entry : bench_positions) {
        PreparedPosition prepared{entry.name, Position(entry.fen), {}};
        MoveList legal;
        generate_legal(prepared.pos, legal);
        prepared.legal_moves.reserve(static_cast<size_t>(legal.size()));
        for (int i = 0; i < legal.size(); ++i) {
            prepared.legal_moves.push_back(legal[i]);
        }
        positions.push_back(std::move(prepared));
    }

    bool all_ok = magic_tables_ready();

    std::cout << "Lulzfish Graph Evaluation Benchmark\n";
    std::cout << "===================================\n\n";
    std::cout << "Magic slider tables: " << (magic_tables_ready() ? "ready" : "not ready") << "\n\n";

    std::cout << "Position graph sizes:\n";
    for (const auto& entry : positions) {
        bool ok = graph_matches_rebuild(entry.pos);
        all_ok = all_ok && ok;
        std::cout << "  " << std::left << std::setw(12) << entry.name
                  << " nodes " << std::right << std::setw(2) << entry.pos.graph().node_count()
                  << "  relations " << std::setw(3) << entry.pos.graph().relation_count()
                  << "  legal " << std::setw(2) << entry.legal_moves.size()
                  << "  rebuild-check " << (ok ? "PASS" : "FAIL") << "\n";
    }

    if (!all_ok) {
        std::cout << "\nGraph correctness precheck failed; benchmark aborted.\n";
        return 1;
    }

    std::cout << "\nThroughput:\n";
    print_metric("graph evaluate", bench_eval(positions, 120000), "eval");
    print_metric("feature extraction", bench_feature_extract(positions, 120000), "extract");
    print_metric("full graph rebuild", bench_graph_rebuild(positions, 120000), "rebuild");
    print_metric("make/unmake graph update", bench_make_unmake_graph_updates(positions, 1200), "move");
    print_metric("topology full rebuild", bench_topo_rebuild(positions, 12000), "topo");

    std::cout << "\nTopological features:\n";
    for (const auto& entry : positions) {
        print_topo_summary(entry.name, entry.pos);
    }

    std::cout << "\nTopology consistency check:\n";
    bool topo_ok = topo_consistency_check(positions);

    std::cout << "\nNote: make/unmake graph update currently includes full relation rebuilds.\n";
    std::cout << "A future incremental accumulator should move that line materially closer to normal make/unmake cost.\n";

    if (!topo_ok) {
        std::cout << "\nWARNING: Topology consistency check had failures.\n";
    }

    return 0;
}
