#include "uci.hpp"

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/search/search.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace lulzfish::core;
using namespace lulzfish::search;

namespace lulzfish::uci {

static Position current_position;
static bool position_set = false;

static void handle_uci() {
    std::cout << "id name Lulzfish 0.2.0 (Baseline)\n";
    std::cout << "id author lulz-chess project\n";
    std::cout << "uciok\n";
}

static void handle_isready() {
    std::cout << "readyok\n";
}

static void handle_position(const std::vector<std::string>& tokens) {
    size_t idx = 1;
    if (tokens.size() > 1 && tokens[1] == "startpos") {
        current_position.set_startpos();
        idx = 2;
    } else if (tokens.size() > 2 && tokens[1] == "fen") {
        std::string fen;
        for (size_t i = 2; i < tokens.size() && tokens[i] != "moves"; ++i) {
            fen += tokens[i] + " ";
        }
        current_position.set_from_fen(fen);
        idx = 3;
        while (idx < tokens.size() && tokens[idx] != "moves") ++idx;
    }

    // Apply moves if present
    if (idx < tokens.size() && tokens[idx] == "moves") {
        ++idx;
        StateInfo undo;
        for (; idx < tokens.size(); ++idx) {
            // Very naive move parsing from UCI string (e.g. "e2e4")
            std::string mstr = tokens[idx];
            if (mstr.size() < 4) continue;

            int from_file = mstr[0] - 'a';
            int from_rank = mstr[1] - '1';
            int to_file   = mstr[2] - 'a';
            int to_rank   = mstr[3] - '1';

            Square from = make_square(from_file, from_rank);
            Square to   = make_square(to_file, to_rank);

            // For baseline we just use normal moves
            Move m = make_move(from, to);
            current_position.make_move(m, undo);
        }
    }

    position_set = true;
}

static void handle_go(const std::vector<std::string>& tokens) {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    SearchLimits limits;
    limits.depth = 4;

    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "depth" && i + 1 < tokens.size()) {
            limits.depth = std::stoi(tokens[i+1]);
        }
    }

    int score = lulzfish::search::search(current_position, limits);

    // For a real engine we would return the best move.
    // For this baseline we just return a legal move (first one) + score.
    MoveList moves;
    generate_legal(current_position, moves);

    if (!moves.empty()) {
        Move best = moves[0];
        // Very crude best move selection (we don't track pv)
        std::cout << "info depth " << limits.depth << " score cp " << score << "\n";

        // Print a move in UCI format
        Square f = from_sq(best);
        Square t = to_sq(best);
        char from_file = 'a' + static_cast<char>(file_of(f));
        char from_rank = '1' + static_cast<char>(rank_of(f));
        char to_file   = 'a' + static_cast<char>(file_of(t));
        char to_rank   = '1' + static_cast<char>(rank_of(t));
        std::cout << "bestmove " << from_file << from_rank << to_file << to_rank << "\n";
    } else {
        std::cout << "bestmove 0000\n";
    }
}

static void handle_quit() {
    // nothing
}

void loop() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream iss(line);
        std::vector<std::string> tokens;
        std::string token;
        while (iss >> token) tokens.push_back(token);

        if (tokens.empty()) continue;

        if (tokens[0] == "uci") {
            handle_uci();
        } else if (tokens[0] == "isready") {
            handle_isready();
        } else if (tokens[0] == "position") {
            handle_position(tokens);
        } else if (tokens[0] == "go") {
            handle_go(tokens);
        } else if (tokens[0] == "quit") {
            break;
        } else if (tokens[0] == "ucinewgame") {
            // reset if needed
        } else if (tokens[0] == "bench") {
            lulzfish::search::bench(4);
        } else if (tokens[0] == "train") {
            lulzfish::search::train_from_selfplay();
        }
    }
}

} // namespace lulzfish::uci
