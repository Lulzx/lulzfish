//==============================================================================
// Lulzfish Search Regression Tests
//==============================================================================
//
// Small tactical/sanity positions that protect against obvious playing-strength
// regressions. These are not Elo claims; they are cheap guardrails for blunders
// seen during local GUI and Stockfish smoke tests.
//==============================================================================

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/search/search.hpp"

#include <iostream>
#include <string>
#include <vector>

using namespace lulzfish::core;
using namespace lulzfish::search;

namespace {

std::string move_to_uci(Move move) {
    std::string out;
    Square from = from_sq(move);
    Square to = to_sq(move);

    out += static_cast<char>('a' + file_of(from));
    out += static_cast<char>('1' + rank_of(from));
    out += static_cast<char>('a' + file_of(to));
    out += static_cast<char>('1' + rank_of(to));

    if (is_promotion(move)) {
        switch (promotion_type(move)) {
            case PieceType::Knight: out += 'n'; break;
            case PieceType::Bishop: out += 'b'; break;
            case PieceType::Rook:   out += 'r'; break;
            case PieceType::Queen:  out += 'q'; break;
            default: break;
        }
    }

    return out;
}

struct SearchCase {
    std::string name;
    std::string fen;
    int depth;
    std::vector<std::string> acceptable_moves;
    std::vector<std::string> forbidden_moves;
};

bool contains(const std::vector<std::string>& values, const std::string& value) {
    for (const std::string& candidate : values) {
        if (candidate == value) return true;
    }
    return false;
}

} // namespace

int main() {
    std::vector<SearchCase> cases = {
        {
            "Develop from startpos",
            "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
            2,
            {"g1f3", "b1c3", "e2e4", "d2d4", "c2c4"},
            {},
        },
        {
            "Black occupies the center after 1 e4",
            "rnbqkbnr/pppppppp/8/8/4P3/8/PPPP1PPP/RNBQKBNR b KQkq - 0 1",
            2,
            {"e7e5", "c7c5", "d7d5", "g8f6"},
            {},
        },
        {
            "Avoid a premature knight sortie",
            "rnbqkbnr/ppp2ppp/4p3/3p4/8/2N2N2/PPPPPPPP/R1BQKB1R w KQkq - 0 3",
            2,
            {},
            {"f3e5"},
        },
        {
            "Avoid repeated knight moves before taking the center",
            "rnbqkbnr/ppp2ppp/4p3/3p4/8/2N2N2/PPPPPPPP/R1BQKB1R w KQkq - 0 3",
            2,
            {},
            {"f3d4", "c3b5"},
        },
        {
            "Keep c-pawn counterplay in Queen's Gambit structures",
            "rnbqkbnr/ppp1pppp/8/3p4/2PP4/8/PP2PPPP/RNBQKBNR b KQkq c3 0 2",
            2,
            {},
            {"b8c6"},
        },
        {
            "Challenge the center in Pirc structures",
            "rnbqkb1r/ppp1pp1p/3p1np1/8/3PP3/2N5/PPP1BPPP/R1BQK1NR b KQkq - 1 4",
            2,
            {"e7e5"},
            {"b8c6", "d6d5"},
        },
        {
            "Develop before exchanging in Slav structures",
            "rnbqkbnr/pp2pppp/2p5/3p4/2PP4/8/PP2PPPP/RNBQKBNR w KQkq - 0 3",
            2,
            {"g1f3", "e2e3", "c1f4"},
            {"c4d5", "d1a4", "b1d2", "b1c3"},
        },
        {
            "Take the hanging queen",
            "4k3/8/8/3q4/8/2N5/8/4K3 w - - 0 1",
            2,
            {"c3d5"},
            {},
        },
        {
            "Avoid a poisoned king-pawn push",
            "2r2r1k/4Q1p1/pB4qp/P2Pp3/2P2p2/3P3b/5PPP/1R3RK1 w - - 1 24",
            2,
            {},
            {"g2g4"},
        },
    };

    bool all_passed = true;

    std::cout << "Lulzfish Search Regression\n";
    std::cout << "==========================\n\n";

    for (const SearchCase& test : cases) {
        clear_search_state();
        Position pos(test.fen);
        SearchLimits limits;
        limits.depth = test.depth;
        SearchResult result = search_root(pos, limits);
        std::string best = move_to_uci(result.best_move);
        bool pass = (test.acceptable_moves.empty() || contains(test.acceptable_moves, best)) &&
                    !contains(test.forbidden_moves, best);
        all_passed = all_passed && pass;

        std::cout << test.name << "... " << (pass ? "PASS" : "FAIL")
                  << "  best " << best
                  << "  score " << result.score << "\n";
    }

    return all_passed ? 0 : 1;
}
