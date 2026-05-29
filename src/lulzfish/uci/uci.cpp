#include "uci.hpp"

#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/eval/graph_eval.hpp"
#include "lulzfish/eval/nnue.hpp"
#include "lulzfish/eval/sheaftop.hpp"
#include "lulzfish/search/search.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <iomanip>
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
    std::cout << "option name NNUEFile type string default <empty>\n";
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
    } else if (name == "NNUEFile" && !value.empty() && value != "<empty>") {
        bool ok = lulzfish::eval::nnue::load(value);
        std::cout << "info string NNUEFile " << (ok ? "loaded " : "failed to load ") << value << "\n";
    }
}

static void emit_search_info(const SearchResult& result) {
    std::cout << "info depth " << result.depth
              << " score cp " << result.score
              << " nodes " << result.nodes
              << " time " << result.time_ms;
    if (result.time_ms > 0) {
        const auto nps = (result.nodes * 1000ULL) / static_cast<std::uint64_t>(result.time_ms);
        std::cout << " nps " << nps;
    }
    std::cout << " pv";
    for (int i = 0; i < result.pv_length; ++i) {
        std::cout << " " << move_to_uci(result.pv[i]);
    }
    std::cout << "\n";
}

static std::string relation_type_name(lulzfish::eval::graph::RelationType type) {
    using lulzfish::eval::graph::RelationType;
    switch (type) {
        case RelationType::ATTACKS: return "ATTACKS";
        case RelationType::DEFENDS: return "DEFENDS";
        case RelationType::PINS: return "PINS";
        case RelationType::DISCOVERED_ATTACK: return "DISCOVERED_ATTACK";
        case RelationType::PAWN_CHAIN: return "PAWN_CHAIN";
        case RelationType::KING_ZONE: return "KING_ZONE";
        default: return "UNKNOWN";
    }
}

static int relation_priority(lulzfish::eval::graph::RelationType type) {
    using lulzfish::eval::graph::RelationType;
    switch (type) {
        case RelationType::PINS: return 0;
        case RelationType::DISCOVERED_ATTACK: return 1;
        case RelationType::ATTACKS: return 2;
        case RelationType::DEFENDS: return 3;
        case RelationType::KING_ZONE: return 4;
        case RelationType::PAWN_CHAIN: return 5;
        default: return 9;
    }
}

static std::string square_to_text(Square sq) {
    std::string out;
    out += static_cast<char>('a' + file_of(sq));
    out += static_cast<char>('1' + rank_of(sq));
    return out;
}

static void handle_graph() {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    constexpr size_t kMaxGraphRelations = 400;
    const auto& graph = current_position.graph();
    const auto& relations = graph.relations();

    std::vector<size_t> indices(relations.size());
    for (size_t i = 0; i < relations.size(); ++i) {
        indices[i] = i;
    }
    std::sort(indices.begin(), indices.end(), [&](size_t a, size_t b) {
        const auto& ra = relations[a];
        const auto& rb = relations[b];
        int pa = relation_priority(ra.type);
        int pb = relation_priority(rb.type);
        if (pa != pb) return pa < pb;
        return a < b;
    });

    const size_t limit = std::min(indices.size(), kMaxGraphRelations);
    const bool capped = relations.size() > limit;

    std::ostringstream out;
    out << "graph {\"ok\":true,\"capped\":" << (capped ? "true" : "false")
        << ",\"total\":" << relations.size() << ",\"relations\":[";
    for (size_t i = 0; i < limit; ++i) {
        const auto& rel = relations[indices[i]];
        if (i > 0) out << ",";
        out << "{\"type\":\"" << relation_type_name(rel.type) << "\",";
        out << "\"from\":\"" << square_to_text(rel.from) << "\",";
        out << "\"to\":\"" << square_to_text(rel.to) << "\"}";
    }
    out << "]}";
    std::cout << out.str() << "\n";
}

static void handle_heatmap() {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    std::ostringstream out;
    out << "heatmap {\"ok\":true,\"white\":{";
    bool first_white = true;
    for (int raw_sq = 0; raw_sq < 64; ++raw_sq) {
        Square sq = static_cast<Square>(raw_sq);
        int count = popcount(current_position.attackers_to(sq, Color::White));
        if (count == 0) continue;
        if (!first_white) out << ",";
        first_white = false;
        out << "\"" << square_to_text(sq) << "\":" << count;
    }
    out << "},\"black\":{";
    bool first_black = true;
    for (int raw_sq = 0; raw_sq < 64; ++raw_sq) {
        Square sq = static_cast<Square>(raw_sq);
        int count = popcount(current_position.attackers_to(sq, Color::Black));
        if (count == 0) continue;
        if (!first_black) out << ",";
        first_black = false;
        out << "\"" << square_to_text(sq) << "\":" << count;
    }
    out << "}}";
    std::cout << out.str() << "\n";
}

static void handle_topology(bool exact) {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    if (!lulzfish::eval::sheaftop::is_enabled()) {
        std::cout << "topology {\"ok\":false,\"error\":\"SheafTop disabled\"}\n";
        return;
    }

    // Force a full rebuild if exact mode requested
    if (exact) {
        const_cast<lulzfish::eval::graph::PositionGraph&>(
            current_position.graph()).rebuild_topology(current_position);
    } else {
        // Ensure topology is up to date (lazy rebuild)
        const_cast<lulzfish::eval::graph::PositionGraph&>(
            current_position.graph()).ensure_topology(current_position);
    }

    const auto& summary = current_position.graph().topo_summary();

    std::ostringstream out;
    out << "topology {\"ok\":true,"
        << "\"exact\":" << (exact ? "true" : "false") << ","
        << "\"h0_persist_sum\":" << summary.h0_persist_sum << ","
        << "\"h1_tension\":" << summary.h1_tension << ","
        << "\"h1_loop_count\":" << summary.h1_loop_count << ","
        << "\"h0_consistency\":" << summary.h0_consistency << ","
        << "\"rebuild_generation\":" << summary.rebuild_generation << ","
        << "\"persist_image\":[";
    for (size_t i = 0; i < lulzfish::eval::sheaftop::PERSIST_IMAGE_DIM; ++i) {
        if (i > 0) out << ",";
        out << std::fixed << std::setprecision(4) << summary.persist_image[i];
    }
    out << "]}";
    std::cout << out.str() << "\n";
}

static void handle_go(const std::vector<std::string>& tokens) {
    if (!position_set) {
        current_position.set_startpos();
        position_set = true;
    }

    constexpr int kMaxIterativeDepth = 64;

    SearchLimits limits;
    limits.depth = 4;
    limits.threads = search_threads;

    int depth_token = 0;
    int movetime = 0;
    long long nodes = 0;
    int wtime = 0, btime = 0, winc = 0, binc = 0, movestogo = 0;
    bool infinite = false;

    auto next_int = [&](size_t i) -> int {
        return (i + 1 < tokens.size()) ? std::stoi(tokens[i + 1]) : 0;
    };

    for (size_t i = 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "depth") depth_token = next_int(i);
        else if (t == "nodes") nodes = (i + 1 < tokens.size()) ? std::stoll(tokens[i + 1]) : 0;
        else if (t == "movetime") movetime = next_int(i);
        else if (t == "wtime") wtime = next_int(i);
        else if (t == "btime") btime = next_int(i);
        else if (t == "winc") winc = next_int(i);
        else if (t == "binc") binc = next_int(i);
        else if (t == "movestogo") movestogo = next_int(i);
        else if (t == "infinite") infinite = true;
    }

    if (nodes > 0) {
        // Node-budgeted search: deepen until the budget is spent (flat, fast,
        // position-independent move cost). Honour an explicit depth cap too.
        limits.max_nodes = static_cast<std::uint64_t>(nodes);
        limits.depth = depth_token > 0 ? depth_token : kMaxIterativeDepth;
        if (movetime > 0) limits.movetime_ms = movetime;
    } else if (depth_token > 0 && movetime > 0) {
        // Depth-limited search with a wall-clock safety cap: deepen up to
        // depth_token but abort if the budget is spent (prevents pathological
        // qsearch blow-ups from hanging on a single move).
        limits.depth = depth_token;
        limits.movetime_ms = movetime;
    } else if (movetime > 0) {
        limits.movetime_ms = movetime;
        limits.depth = kMaxIterativeDepth;
    } else if (wtime > 0 || btime > 0) {
        // Allocate a per-move slice of the remaining clock plus most of the
        // increment, assuming a finite horizon when movestogo is absent.
        bool white = current_position.side_to_move() == Color::White;
        int time_left = white ? wtime : btime;
        int inc = white ? winc : binc;
        int moves = movestogo > 0 ? movestogo : 30;
        int budget = time_left / moves + (inc * 3) / 4;
        budget = std::min(budget, time_left - 30);   // never flag
        limits.movetime_ms = std::max(10, budget);
        limits.depth = kMaxIterativeDepth;
    } else if (infinite) {
        limits.depth = kMaxIterativeDepth;
    } else if (depth_token > 0) {
        limits.depth = depth_token;
    }

    SearchResult result = lulzfish::search::search_root(
        current_position, limits, emit_search_info);

    MoveList moves;
    generate_legal(current_position, moves);

    if (!moves.empty() && result.best_move != MOVE_NONE) {
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
        } else if (tokens[0] == "graph") {
            handle_graph();
        } else if (tokens[0] == "heatmap") {
            handle_heatmap();
        } else if (tokens[0] == "topology") {
            // Debug: dump topological summary (exact rebuild path)
            handle_topology(true);
        } else if (tokens[0] == "topology_approx") {
            // Debug: dump topological summary (incremental approximate path)
            handle_topology(false);
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
        } else if (tokens[0] == "features_with_topo") {
            // Debug: dump the 128-d feature vector (64 base + 64 topological)
            // for the current position. Used by train_topo_residual.py.
            if (!position_set) {
                current_position.set_startpos();
                position_set = true;
            }
            // Ensure topology is computed
            const_cast<lulzfish::eval::graph::PositionGraph&>(
                current_position.graph()).ensure_topology(current_position);

            std::array<float, lulzfish::eval::graph::FEATURES_WITH_TOPO_TOTAL> feats;
            lulzfish::eval::graph::extract_features_with_topo(current_position, feats);
            std::cout << "features_with_topo";
            for (std::size_t i = 0; i < feats.size(); ++i) {
                std::cout << ' ' << feats[i];
            }
            std::cout << "\n";
        } else if (tokens[0] == "nnueeval") {
            // Debug: NNUE static eval (cp, side-to-move POV) for the current
            // position. Used by tools/rl/nnue_parity.py to verify C++/torch match.
            if (!position_set) {
                current_position.set_startpos();
                position_set = true;
            }
            if (lulzfish::eval::nnue::loaded()) {
                std::cout << "nnueeval cp " << lulzfish::eval::nnue::evaluate(current_position) << "\n";
            } else {
                std::cout << "nnueeval none\n";
            }
        } else if (tokens[0] == "bench") {
            lulzfish::search::bench(4);
        } else if (tokens[0] == "train") {
            lulzfish::search::train_from_selfplay();
        }
    }
}

} // namespace lulzfish::uci
