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
            "Develop the queen knight in open games",
            "rnbqkbnr/pppp1ppp/8/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R b KQkq - 1 2",
            2,
            {"b8c6"},
            {"g8f6"},
        },
        {
            "Make luft before Italian knight jump",
            "r1bq1rk1/pppp1pp1/2n2n1p/2b1p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 0 7",
            2,
            {"h2h3"},
            {"c3d5"},
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
            "Gain space against Reti c-pawn tension",
            "rnbqkbnr/ppp1pppp/8/3p4/2P5/5N2/PP1PPPPP/RNBQKB1R b KQkq c3 0 2",
            2,
            {"d5d4"},
            {"d5c4", "b8c6"},
        },
        {
            "Use safe setup against Nimzo pressure",
            "rnbqk2r/pppp1ppp/4pn2/8/1bPP4/2N5/PP2PPPP/R1BQKBNR w KQkq - 2 4",
            2,
            {"e2e3", "d1c2"},
            {"f2f3", "g1f3"},
        },
        {
            "Avoid pawn-grabbing with damaged Nimzo structure",
            "r1bq1rk1/pp1p1ppp/2n1pn2/2p5/2PP4/P1P2N2/2Q1PPPP/R1B1KB1R w KQ - 2 8",
            2,
            {},
            {"d4c5"},
        },
        {
            "Develop naturally against Benoni tension",
            "rnbqkb1r/pp1p1ppp/4pn2/2pP4/2P5/8/PP2PPPP/RNBQKBNR w KQkq - 0 4",
            2,
            {"b1c3"},
            {"g1f3"},
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
        {
            "Develop before counter-gambit in Dutch",
            "rnbqkbnr/ppppp1pp/8/5p2/3P4/5N2/PPP1PPPP/RNBQKB1R b KQkq - 0 2",
            2,
            {"e7e6", "g7g6", "g8f6"},
            {"e7e5"},
        },
        {
            "Use natural setup against Queen's Indian",
            "rnbqkb1r/p1pp1ppp/1p2pn2/8/2PP4/5N2/PP2PPPP/RNBQKB1R w KQkq - 0 4",
            2,
            {"g2g3", "a2a3", "e2e3"},
            {"b1c3"},
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
