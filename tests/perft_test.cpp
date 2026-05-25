//==============================================================================
// Lulzfish Real Perft Test Harness
//==============================================================================
//
// This is the single most important correctness tool for the engine.
// Perft counts the number of legal move sequences to a given depth.
//
// We compare against well-known published numbers.
// If these pass, movegen + make/unmake + check detection are very likely correct.
//==============================================================================

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

using namespace lulzfish::core;

uint64_t perft(Position& pos, int depth) {
    if (depth == 0) return 1;

    MoveList moves;
    generate_legal(pos, moves);

    if (depth == 1) return moves.size();

    uint64_t nodes = 0;
    StateInfo undo;

    for (int i = 0; i < moves.size(); ++i) {
        Move m = moves[i];
        pos.make_move(m, undo);
        nodes += perft(pos, depth - 1);
        pos.unmake_move(m, undo);
    }
    return nodes;
}

struct PerftTest {
    std::string name;
    std::string fen;
    int depth;
    uint64_t expected;
};

int main() {
    std::cout << "Lulzfish Perft Validation\n";
    std::cout << "==========================\n\n";

    // Classic test positions
    std::vector<PerftTest> tests = {
        {"Startpos", "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281ULL},
        {"Kiwipete", "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 3, 97862ULL},
        {"Position 3", "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624ULL},
        {"Position 4", "r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333ULL},
        {"Position 5", "rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 210348ULL},
    };

    bool all_passed = true;

    for (const auto& test : tests) {
        std::cout << "Testing " << test.name << " at depth " << test.depth << "... ";

        Position pos(test.fen);
        auto start = std::chrono::steady_clock::now();

        uint64_t nodes = perft(pos, test.depth);

        auto end = std::chrono::steady_clock::now();
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        bool pass = (nodes == test.expected);
        if (!pass) all_passed = false;

        std::cout << (pass ? "PASS" : "FAIL") << "  got " << nodes
                  << "  expected " << test.expected
                  << "  (" << ms << " ms)\n";
    }

    std::cout << "\n";
    if (all_passed) {
        std::cout << "=== ALL PERFT TESTS PASSED ===\n";
        std::cout << "Move generation and make/unmake are highly likely correct.\n";
    } else {
        std::cout << "=== SOME TESTS FAILED ===\n";
        std::cout << "Debug movegen or Position::make/unmake before proceeding.\n";
    }

    return all_passed ? 0 : 1;
}
