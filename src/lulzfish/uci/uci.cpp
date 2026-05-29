#include "uci.hpp"

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/eval/graph_eval.hpp"
#include "lulzfish/search/search.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace lulzfish::core;
using namespace lulzfish::search;

namespace lulzfish::uci {

static Position current_position;
static bool position_set = false;
static int search_threads = 1;

static int max_search_threads() {
    unsigned count = std::thread::hardware_concurrency();
    if (count == 0) return 1;
    if (count > static_cast<unsigned>(std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(count);
}

static std::string move_to_uci(Move move) {
    Square from = from_sq(move);
    Square to = to_sq(move);

    std::string out;
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

static bool apply_uci_move(Position& pos, const std::string& move_text) {
    MoveList legal_moves;
    generate_legal(pos, legal_moves);

    for (int i = 0; i < legal_moves.size(); ++i) {
        Move move = legal_moves[i];
        if (move_to_uci(move) == move_text) {
            StateInfo undo;
            pos.make_move(move, undo);
            return true;
        }
    }

    return false;
}

static void handle_uci() {
    std::cout << "id name Lulzfish 0.5.0 (Verified Baseline)\n";
    std::cout << "id author lulz-chess project\n";
    std::cout << "option name Threads type spin default 1 min 1 max "
              << max_search_threads() << "\n";
    std::cout << "option name EvalFile type string default <empty>\n";
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
        for (; idx < tokens.size(); ++idx) {
            if (!apply_uci_move(current_position, tokens[idx])) {
                std::cerr << "info string ignored illegal move " << tokens[idx] << "\n";
                break;
            }
        }
    }

    position_set = true;
}

static void handle_setoption(const std::vector<std::string>& tokens) {
    std::string name;
    std::string value;
    bool reading_name = false;
    bool reading_value = false;

    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "name") {
            reading_name = true;
            reading_value = false;
            continue;
        }
        if (tokens[i] == "value") {
            reading_name = false;
            reading_value = true;
            continue;
        }
        if (reading_name) {
            if (!name.empty()) name += ' ';
            name += tokens[i];
        } else if (reading_value) {
            if (!value.empty()) value += ' ';
            value += tokens[i];
        }
    }

    if (name == "Threads" && !value.empty()) {
        search_threads = std::clamp(std::stoi(value), 1, max_search_threads());
    } else if (name == "EvalFile" && !value.empty() && value != "<empty>") {
        // Loads the learned MLP residual evaluator. Off unless a file is set,
        // so the handcrafted baseline remains the default. Silently no-ops if
        // the file is missing or malformed (see set_global_model_from_file).
        lulzfish::eval::graph::set_global_model_from_file(value);
        std::cout << "info string EvalFile "
                  << (lulzfish::eval::graph::global_model_loaded() ? "loaded " : "failed to load ")
                  << value << "\n";
    }
}

static void handle_go(const std::vector<std::string>& tokens) {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    SearchLimits limits;
    limits.depth = 4;
    limits.threads = search_threads;

    for (size_t i = 1; i < tokens.size(); ++i) {
        if (tokens[i] == "depth" && i + 1 < tokens.size()) {
            limits.depth = std::stoi(tokens[i+1]);
        }
    }

    SearchResult result = lulzfish::search::search_root(current_position, limits);

    MoveList moves;
    generate_legal(current_position, moves);

    if (!moves.empty() && result.best_move != MOVE_NONE) {
        std::cout << "info depth " << limits.depth << " score cp " << result.score << " pv";
        for (int i = 0; i < result.pv_length; ++i) {
            std::cout << " " << move_to_uci(result.pv[i]);
        }
        std::cout << "\n";
        std::cout << "bestmove " << move_to_uci(result.best_move) << "\n";
    } else {
        std::cout << "bestmove 0000\n";
    }
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
        } else if (tokens[0] == "setoption") {
            handle_setoption(tokens);
        } else if (tokens[0] == "position") {
            handle_position(tokens);
        } else if (tokens[0] == "go") {
            handle_go(tokens);
        } else if (tokens[0] == "quit") {
            break;
        } else if (tokens[0] == "ucinewgame") {
            lulzfish::search::clear_search_state();
            current_position.set_startpos();
            position_set = false;
        } else if (tokens[0] == "features") {
            // Debug: dump the 64-d feature vector for the current position so
            // training tooling can use the engine as the single source of truth
            // for feature extraction (incl. graph-derived relation features).
            if (!position_set) {
                current_position.set_startpos();
                position_set = true;
            }
            std::array<float, lulzfish::eval::graph::FEATURES_TOTAL> feats;
            lulzfish::eval::graph::extract_features(current_position, feats);
            std::cout << "features";
            for (std::size_t i = 0; i < feats.size(); ++i) {
                std::cout << ' ' << feats[i];
            }
            std::cout << "\n";
        } else if (tokens[0] == "bench") {
            lulzfish::search::bench(4);
        } else if (tokens[0] == "train") {
            lulzfish::search::train_from_selfplay();
        }
    }
}

} // namespace lulzfish::uci
