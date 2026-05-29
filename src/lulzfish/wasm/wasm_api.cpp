#include "lulzfish/core/movegen.hpp"
#include "lulzfish/core/position.hpp"
#include "lulzfish/eval/graph_eval.hpp"
#include "lulzfish/eval/material.hpp"
#include "lulzfish/search/search.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

using namespace lulzfish::core;

namespace {

Position g_position;
std::vector<Move> g_moves;
std::string g_last_result;
lulzfish::search::SearchResult g_last_search;

constexpr int kMaxWasmDepth = 6;
constexpr size_t kMaxGraphRelations = 400;

std::string json_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (char ch : text) {
        switch (ch) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(ch) < 0x20) {
                    char buf[7] = {};
                    constexpr char hex[] = "0123456789abcdef";
                    buf[0] = '\\';
                    buf[1] = 'u';
                    buf[2] = '0';
                    buf[3] = '0';
                    buf[4] = hex[(ch >> 4) & 0x0f];
                    buf[5] = hex[ch & 0x0f];
                    out += buf;
                } else {
                    out += ch;
                }
                break;
        }
    }
    return out;
}

std::string square_to_text(Square sq) {
    if (sq == NoneSquare) return "";
    std::string out;
    out += static_cast<char>('a' + file_of(sq));
    out += static_cast<char>('1' + rank_of(sq));
    return out;
}

std::string move_to_uci(Move move) {
    if (move == MOVE_NONE) return "0000";

    std::string out;
    out += square_to_text(from_sq(move));
    out += square_to_text(to_sq(move));

    if (is_promotion(move)) {
        switch (promotion_type(move)) {
            case PieceType::Knight: out += 'n'; break;
            case PieceType::Bishop: out += 'b'; break;
            case PieceType::Rook: out += 'r'; break;
            case PieceType::Queen: out += 'q'; break;
            default: break;
        }
    }

    return out;
}

std::vector<std::string> current_uci_moves() {
    std::vector<std::string> moves;
    moves.reserve(g_moves.size());
    for (Move move : g_moves) {
        moves.push_back(move_to_uci(move));
    }
    return moves;
}

Square king_square(const Position& pos, Color color) {
    Bitboard king = pos.pieces(make_piece(color, PieceType::King));
    if (king == EmptyBB) return NoneSquare;
    return lsb_square(king);
}

std::string status_text(Position& pos, const MoveList& legal_moves) {
    if (!legal_moves.empty()) {
        return pos.side_to_move() == Color::White ? "White to move" : "Black to move";
    }
    if (pos.is_check()) {
        return "Game over";
    }
    return "Game over";
}

std::string relation_type_name(lulzfish::eval::graph::RelationType type) {
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

int relation_priority(lulzfish::eval::graph::RelationType type) {
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

std::string pv_to_uci_json(const lulzfish::search::SearchResult& result) {
    std::ostringstream out;
    out << "[";
    for (int i = 0; i < result.pv_length; ++i) {
        if (i > 0) out << ",";
        out << "\"" << move_to_uci(result.pv[i]) << "\"";
    }
    out << "]";
    return out.str();
}

std::string search_result_json(const lulzfish::search::SearchResult& result) {
    std::ostringstream out;
    out << "{";
    out << "\"score\":" << result.score << ",";
    out << "\"depth\":" << result.depth << ",";
    out << "\"nodes\":" << result.nodes << ",";
    out << "\"time_ms\":" << result.time_ms << ",";
    int nps = 0;
    if (result.time_ms > 0) {
        nps = static_cast<int>((result.nodes * 1000ULL) / static_cast<std::uint64_t>(result.time_ms));
    }
    out << "\"nps\":" << nps << ",";
    out << "\"best_move\":\"" << move_to_uci(result.best_move) << "\",";
    out << "\"pv\":" << pv_to_uci_json(result);
    out << "}";
    return out.str();
}

lulzfish::search::SearchResult run_search(int depth) {
    lulzfish::search::SearchLimits limits;
    limits.depth = std::clamp(depth, 1, kMaxWasmDepth);
    limits.threads = 1;
    Position search_pos = g_position;
    lulzfish::search::SearchResult result = lulzfish::search::search_root(search_pos, limits);
    g_last_search = result;
    return result;
}

std::string result_text(Position& pos, const MoveList& legal_moves) {
    if (legal_moves.empty()) {
        if (pos.is_check()) {
            return pos.side_to_move() == Color::White ? "0-1 by checkmate" : "1-0 by checkmate";
        }
        return "1/2-1/2 by stalemate";
    }
    if (pos.halfmove_clock() >= 100) {
        return "1/2-1/2 by fifty-move rule";
    }
    if (pos.is_repetition()) {
        return "1/2-1/2 by repetition";
    }
    return "";
}

std::string state_json() {
    MoveList legal_moves;
    generate_legal(g_position, legal_moves);

    std::ostringstream out;
    out << "{";

    out << "\"board\":{";
    bool first = true;
    for (int raw_sq = 0; raw_sq < 64; ++raw_sq) {
        Square sq = static_cast<Square>(raw_sq);
        Piece piece = g_position.piece_on(sq);
        if (piece == Piece::None) continue;
        if (!first) out << ",";
        first = false;
        out << "\"" << square_to_text(sq) << "\":\"" << piece_char(piece) << "\"";
    }
    out << "},";

    out << "\"legal_moves\":[";
    for (int i = 0; i < legal_moves.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << move_to_uci(legal_moves[i]) << "\"";
    }
    out << "],";

    out << "\"turn\":\"" << (g_position.side_to_move() == Color::White ? "white" : "black") << "\",";
    out << "\"moves\":[";
    auto moves = current_uci_moves();
    for (size_t i = 0; i < moves.size(); ++i) {
        if (i > 0) out << ",";
        out << "\"" << moves[i] << "\"";
    }
    out << "],";

    std::string last_move = g_moves.empty() ? "" : move_to_uci(g_moves.back());
    out << "\"last_move\":";
    if (last_move.empty()) {
        out << "null";
    } else {
        out << "\"" << last_move << "\"";
    }
    out << ",";

    out << "\"check_square\":";
    if (g_position.is_check()) {
        Square sq = king_square(g_position, g_position.side_to_move());
        out << "\"" << square_to_text(sq) << "\"";
    } else {
        out << "null";
    }
    out << ",";

    out << "\"status\":\"" << json_escape(status_text(g_position, legal_moves)) << "\",";
    out << "\"result\":\"" << json_escape(result_text(g_position, legal_moves)) << "\",";
    out << "\"fen\":\"" << json_escape(g_position.fen()) << "\",";
    out << "\"score\":" << lulzfish::eval::evaluate(g_position) << ",";
    out << "\"search_info\":" << search_result_json(g_last_search);
    out << "}";

    return out.str();
}

const char* store_result(std::string value) {
    g_last_result = std::move(value);
    return g_last_result.c_str();
}

const char* store_error(std::string_view message) {
    std::string escaped = json_escape(message);
    return store_result(std::string("{\"ok\":false,\"error\":\"") + escaped + "\"}");
}

Move best_move_for_depth(int depth) {
    lulzfish::search::SearchResult result = run_search(depth);
    return result.best_move;
}

} // namespace

extern "C" {

const char* lulzfish_new_game() {
    try {
        lulzfish::search::clear_search_state();
        g_last_search = {};
        g_position.set_startpos();
        g_moves.clear();
        return store_result(state_json());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_set_fen(const char* fen) {
    try {
        lulzfish::search::clear_search_state();
        g_last_search = {};
        g_position.set_from_fen(fen == nullptr ? "" : fen);
        g_moves.clear();
        return store_result(state_json());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_state_json() {
    try {
        return store_result(state_json());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_make_move(const char* move_text) {
    try {
        std::string_view move = move_text == nullptr ? std::string_view{} : std::string_view(move_text);
        MoveList legal_moves;
        generate_legal(g_position, legal_moves);
        for (int i = 0; i < legal_moves.size(); ++i) {
            Move candidate = legal_moves[i];
            if (move_to_uci(candidate) == move) {
                StateInfo undo;
                g_position.make_move(candidate, undo);
                g_moves.push_back(candidate);
                return store_result(state_json());
            }
        }
        return store_error("Illegal move");
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_apply_uci_line(const char* moves_text) {
    try {
        lulzfish::search::clear_search_state();
        g_last_search = {};
        g_position.set_startpos();
        g_moves.clear();

        std::istringstream in(moves_text == nullptr ? "" : moves_text);
        std::string move;
        while (in >> move) {
            MoveList legal_moves;
            generate_legal(g_position, legal_moves);
            bool found = false;
            for (int i = 0; i < legal_moves.size(); ++i) {
                Move candidate = legal_moves[i];
                if (move_to_uci(candidate) == move) {
                    StateInfo undo;
                    g_position.make_move(candidate, undo);
                    g_moves.push_back(candidate);
                    found = true;
                    break;
                }
            }
            if (!found) return store_error("Illegal move in line");
        }

        return store_result(state_json());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_best_move(int depth) {
    try {
        return store_result(move_to_uci(best_move_for_depth(depth)));
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_play_engine_move(int depth) {
    try {
        Move move = best_move_for_depth(depth);
        if (move == MOVE_NONE) return store_result(state_json());
        StateInfo undo;
        g_position.make_move(move, undo);
        g_moves.push_back(move);
        return store_result(state_json());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

int lulzfish_evaluate() {
    return lulzfish::eval::evaluate(g_position);
}

void lulzfish_clear_search() {
    lulzfish::search::clear_search_state();
    g_last_search = {};
}

const char* lulzfish_analyze(int depth) {
    try {
        MoveList legal_moves;
        generate_legal(g_position, legal_moves);
        if (legal_moves.empty()) {
            return store_result(std::string("{\"ok\":true,\"search\":") +
                                search_result_json(g_last_search) + "}");
        }
        return store_result(std::string("{\"ok\":true,\"search\":") +
                            search_result_json(run_search(depth)) + "}");
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_search_info_json(int depth) {
    return lulzfish_analyze(depth);
}

const char* lulzfish_graph_json() {
    try {
        const auto& graph = g_position.graph();
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
        out << "{\"ok\":true,\"capped\":" << (capped ? "true" : "false")
            << ",\"total\":" << relations.size() << ",\"relations\":[";
        for (size_t i = 0; i < limit; ++i) {
            const auto& rel = relations[indices[i]];
            if (i > 0) out << ",";
            out << "{\"type\":\"" << relation_type_name(rel.type) << "\",";
            out << "\"from\":\"" << square_to_text(rel.from) << "\",";
            out << "\"to\":\"" << square_to_text(rel.to) << "\"}";
        }
        out << "]}";
        return store_result(out.str());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_features_json() {
    try {
        std::array<float, lulzfish::eval::graph::FEATURES_TOTAL> features{};
        lulzfish::eval::graph::extract_features(g_position, features);

        std::ostringstream out;
        out << "{\"ok\":true,\"features\":[";
        for (size_t i = 0; i < features.size(); ++i) {
            if (i > 0) out << ",";
            out << features[i];
        }
        out << "]}";
        return store_result(out.str());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

const char* lulzfish_attack_heatmap_json() {
    try {
        std::ostringstream out;
        out << "{\"ok\":true,\"white\":{";
        bool first_white = true;
        for (int raw_sq = 0; raw_sq < 64; ++raw_sq) {
            Square sq = static_cast<Square>(raw_sq);
            int count = popcount(g_position.attackers_to(sq, Color::White));
            if (count == 0) continue;
            if (!first_white) out << ",";
            first_white = false;
            out << "\"" << square_to_text(sq) << "\":" << count;
        }
        out << "},\"black\":{";
        bool first_black = true;
        for (int raw_sq = 0; raw_sq < 64; ++raw_sq) {
            Square sq = static_cast<Square>(raw_sq);
            int count = popcount(g_position.attackers_to(sq, Color::Black));
            if (count == 0) continue;
            if (!first_black) out << ",";
            first_black = false;
            out << "\"" << square_to_text(sq) << "\":" << count;
        }
        out << "}}";
        return store_result(out.str());
    } catch (const std::exception& exc) {
        return store_error(exc.what());
    }
}

} // extern "C"
