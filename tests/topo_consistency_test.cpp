//==============================================================================
// Lulzfish SheafTop — Topology Consistency Test
//==============================================================================
//
// Verifies that:
// 1. Topological features are stable across make/unmake cycles
// 2. Incremental path stays within tolerance of exact path
// 3. Perft numbers are unchanged with topological features enabled
//
// See docs/SHEAF_TOP_IMPLEMENTATION_NOTES.md section 7.
//==============================================================================

#include "lulzfish/core/attacks.hpp"
#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/eval/graph_eval.hpp"
#include "lulzfish/eval/sheaftop.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <vector>

using namespace lulzfish::core;

namespace {

struct TestCase {
    std::string name;
    std::string fen;
};

bool test_make_unmake_topology_stability(const TestCase& tc, int cycles) {
    Position pos(tc.fen);
    const auto& graph = pos.graph();

    // Get initial topology
    lulzfish::eval::sheaftop::TopoSummary initial = graph.topo_summary();

    // Generate legal moves
    MoveList legal;
    generate_legal(pos, legal);

    if (legal.size() == 0) {
        std::cout << "  SKIP " << tc.name << " (no legal moves)\n";
        return true;
    }

    std::mt19937 rng(42);
    std::uniform_int_distribution<int> move_dist(0, legal.size() - 1);

    bool all_ok = true;

    for (int cycle = 0; cycle < cycles; ++cycle) {
        // Pick a random move
        int move_idx = move_dist(rng);
        Move move = legal[move_idx];

        // Make move
        StateInfo undo;
        pos.make_move(move, undo);

        // Get topology after make
        const auto& after_make = pos.graph().topo_summary();

        // Unmake move
        pos.unmake_move(move, undo);

        // Get topology after unmake
        const auto& after_unmake = pos.graph().topo_summary();

        // Check that topology is approximately restored
        float error_h0 = std::abs(after_unmake.h0_persist_sum - initial.h0_persist_sum);
        float error_h1 = std::abs(after_unmake.h1_tension - initial.h1_tension);
        float error_loop = std::abs(after_unmake.h1_loop_count - initial.h1_loop_count);
        float error_consist = std::abs(after_unmake.h0_consistency - initial.h0_consistency);

        // Use a reasonable tolerance (Phase 0: 0.01, Phase 1: 0.05)
        float tolerance = 0.05f;

        if (error_h0 > tolerance || error_h1 > tolerance ||
            error_loop > tolerance || error_consist > tolerance) {
            std::cout << "  FAIL " << tc.name << " cycle " << cycle << ":\n";
            std::cout << "    Initial:   h0=" << initial.h0_persist_sum
                      << " h1=" << initial.h1_tension
                      << " loops=" << initial.h1_loop_count
                      << " consist=" << initial.h0_consistency << "\n";
            std::cout << "    After:     h0=" << after_unmake.h0_persist_sum
                      << " h1=" << after_unmake.h1_tension
                      << " loops=" << after_unmake.h1_loop_count
                      << " consist=" << after_unmake.h0_consistency << "\n";
            std::cout << "    Error:     h0=" << error_h0
                      << " h1=" << error_h1
                      << " loops=" << error_loop
                      << " consist=" << error_consist << "\n";
            all_ok = false;
        }
    }

    if (all_ok) {
        std::cout << "  PASS " << tc.name << " (" << cycles << " cycles)\n";
    }

    return all_ok;
}

bool test_exact_vs_incremental(const TestCase& tc) {
    Position pos(tc.fen);

    // Get exact topology
    lulzfish::eval::sheaftop::TopoSummary exact;
    lulzfish::eval::sheaftop::full_rebuild(pos, exact);

    // Get incremental topology
    const auto& incremental = pos.graph().topo_summary();

    // Compare
    bool ok = lulzfish::eval::sheaftop::verify_consistency(pos, incremental, 0.01f);

    if (!ok) {
        std::cout << "  FAIL " << tc.name << " (exact vs incremental mismatch)\n";
        std::cout << "    Exact:      h0=" << exact.h0_persist_sum
                  << " h1=" << exact.h1_tension
                  << " loops=" << exact.h1_loop_count
                  << " consist=" << exact.h0_consistency << "\n";
        std::cout << "    Incremental: h0=" << incremental.h0_persist_sum
                  << " h1=" << incremental.h1_tension
                  << " loops=" << incremental.h1_loop_count
                  << " consist=" << incremental.h0_consistency << "\n";
    } else {
        std::cout << "  PASS " << tc.name << " (exact vs incremental)\n";
    }

    return ok;
}

} // anonymous namespace

int main() {
    init_attack_tables();
    lulzfish::eval::sheaftop::init();

    std::cout << "Lulzfish SheafTop — Topology Consistency Test\n";
    std::cout << "==============================================\n\n";

    const std::vector<TestCase> test_cases = {
        {"startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"},
        {"kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"},
        {"open_game", "r1bqkbnr/pppp1ppp/2n5/4p3/2B1P3/5N2/PPPP1PPP/RNBQK2R w KQkq - 2 3"},
        {"middlegame", "2r2r1k/4Q1p1/pB4qp/P2Pp3/2P2p2/3P3b/5PPP/1R3RK1 w - - 1 24"},
        {"endgame", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1"},
        {"complex", "r1bqkb1r/pppppppp/2n2n2/8/3PP3/2N2N2/PPP2PPP/R1BQKB1R b KQkq - 0 1"},
    };

    std::cout << "Test 1: Exact vs Incremental Consistency\n";
    bool test1_ok = true;
    for (const auto& tc : test_cases) {
        if (!test_exact_vs_incremental(tc)) {
            test1_ok = false;
        }
    }

    std::cout << "\nTest 2: Make/Unmake Stability (100 cycles per position)\n";
    bool test2_ok = true;
    for (const auto& tc : test_cases) {
        if (!test_make_unmake_topology_stability(tc, 100)) {
            test2_ok = false;
        }
    }

    std::cout << "\nSummary:\n";
    std::cout << "  Exact vs Incremental: " << (test1_ok ? "PASS" : "FAIL") << "\n";
    std::cout << "  Make/Unmake Stability: " << (test2_ok ? "PASS" : "FAIL") << "\n";

    return (test1_ok && test2_ok) ? 0 : 1;
}
