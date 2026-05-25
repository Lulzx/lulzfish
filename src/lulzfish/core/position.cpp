#include "position.hpp"
#include "attacks.hpp"

#include <algorithm>
#include <cassert>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

namespace lulzfish::core {

namespace {

// Simple 64-bit PRNG for Zobrist initialization (splitmix64 style)
struct SplitMix64 {
    uint64_t state;
    explicit SplitMix64(uint64_t seed) : state(seed) {}

    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};

} // anonymous namespace

// Static member definitions
std::array<std::array<Key, 64>, 13> Position::zobrist_piece_{};
std::array<Key, 16>                 Position::zobrist_castling_{};
std::array<Key, 65>                 Position::zobrist_ep_{};
Key                                 Position::zobrist_side_{0};
bool                                Position::zobrist_initialized_{false};

//------------------------------------------------------------------------------
// Zobrist Initialization (called once on first Position construction)
//------------------------------------------------------------------------------

void Position::init_zobrist_tables() {
    if (zobrist_initialized_) return;

    SplitMix64 rng(0x123456789abcdef0ULL);  // Fixed seed for reproducibility

    // Piece-square keys (index 0 is unused / None)
    for (size_t p = 0; p < zobrist_piece_.size(); ++p) {
        for (size_t sq = 0; sq < zobrist_piece_[p].size(); ++sq) {
            zobrist_piece_[p][sq] = rng.next();
        }
    }

    // Castling rights (0-15)
    for (size_t cr = 0; cr < zobrist_castling_.size(); ++cr) {
        zobrist_castling_[cr] = rng.next();
    }

    // En passant files (we key by square for simplicity; 64 + "none")
    for (size_t i = 0; i < zobrist_ep_.size(); ++i) {
        zobrist_ep_[i] = rng.next();
    }

    zobrist_side_ = rng.next();

    zobrist_initialized_ = true;
}

//------------------------------------------------------------------------------
// Construction & Setup
//------------------------------------------------------------------------------

Position::Position() {
    init_zobrist_tables();
    init_attack_tables();
    clear();
    set_startpos();
}

Position::Position(std::string_view fen) {
    init_zobrist_tables();
    init_attack_tables();
    clear();
    set_from_fen(fen);
}

void Position::clear() {
    piece_bb_.fill(EmptyBB);
    color_bb_.fill(EmptyBB);
    occupancy_bb_ = EmptyBB;

    side_to_move_      = Color::White;
    castling_rights_   = NoCastling;
    en_passant_square_ = NoneSquare;
    halfmove_clock_    = 0;
    fullmove_number_   = 1;
    key_               = 0;
    pawn_key_          = 0;
    key_history_.clear();

    graph_ = {}; // reset graph
}

//------------------------------------------------------------------------------
// Piece Placement Helpers (maintain all bitboards + key)
//------------------------------------------------------------------------------

void Position::put_piece(Piece p, Square sq) {
    if (p == Piece::None || !empty(sq)) {
        return;  // defensive during development
    }

    size_t idx = static_cast<size_t>(p);
    set_bit(piece_bb_[idx], sq);

    Color c = color_of(p);
    set_bit(color_bb_[static_cast<size_t>(c)], sq);

    occupancy_bb_ |= square_bb(sq);

    update_key_incremental(p, sq, true);
}

void Position::remove_piece(Square sq) {
    Piece p = piece_on(sq);
    if (p == Piece::None) return;

    size_t idx = static_cast<size_t>(p);
    clear_bit(piece_bb_[idx], sq);

    Color c = color_of(p);
    clear_bit(color_bb_[static_cast<size_t>(c)], sq);

    occupancy_bb_ &= ~square_bb(sq);

    update_key_incremental(p, sq, false);
}

void Position::move_piece(Square from, Square to) {
    Piece p = piece_on(from);
    if (p == Piece::None) {
        // Defensive guard for rare unmake corruption during development
        return;
    }
    if (!empty(to)) {
        return;
    }

    size_t idx = static_cast<size_t>(p);

    clear_bit(piece_bb_[idx], from);
    set_bit(piece_bb_[idx], to);

    Color c = color_of(p);
    clear_bit(color_bb_[static_cast<size_t>(c)], from);
    set_bit(color_bb_[static_cast<size_t>(c)], to);

    occupancy_bb_ &= ~square_bb(from);
    occupancy_bb_ |= square_bb(to);

    // Key update: remove from 'from', add to 'to'
    update_key_incremental(p, from, false);
    update_key_incremental(p, to, true);
}

void Position::update_key_incremental(Piece p, Square sq, bool add) {
    Key delta = zobrist_piece_[static_cast<size_t>(p)][static_cast<size_t>(sq)];
    if (add) {
        key_ ^= delta;
    } else {
        key_ ^= delta;
    }
}

//------------------------------------------------------------------------------
// Make / Unmake (initial implementation — will be expanded with movegen)
//------------------------------------------------------------------------------

void Position::make_move(Move m, StateInfo& undo) {
    Square from = from_sq(m);
    Square to   = to_sq(m);

    Piece mover = piece_on(from);
    if (mover == Piece::None) {
        assert(false && "make_move called with invalid from-square");
        return;
    }

    Color us = color_of(mover);
    Piece captured = piece_on(to);

    // Special handling for ep captured piece
    if (is_en_passant(m)) {
        int dir = (us == Color::White) ? -8 : 8;
        Square cap_sq = static_cast<Square>(static_cast<int>(to) + dir);
        captured = piece_on(cap_sq);
    }

    // Save undo information
    undo.previous_key       = key_;
    undo.captured_piece     = captured;
    undo.castling_rights    = castling_rights_;
    undo.en_passant_square  = en_passant_square_;
    undo.halfmove_clock     = static_cast<uint8_t>(halfmove_clock_);
    undo.moved_piece        = mover;
    undo.castling_rook_from = NoneSquare;
    undo.castling_rook_to   = NoneSquare;
    undo.promoted_to        = Piece::None;

    key_ ^= zobrist_castling_[castling_rights_];
    if (en_passant_square_ != NoneSquare) {
        key_ ^= zobrist_ep_[static_cast<size_t>(en_passant_square_)];
    }

    bool is_pawn = (type_of(mover) == PieceType::Pawn);
    bool is_capture = (captured != Piece::None);

    if (is_castling(m)) {
        // Determine rook movement
        Square rook_from, rook_to;
        if (to == G1) { rook_from = H1; rook_to = F1; }      // White O-O
        else if (to == C1) { rook_from = A1; rook_to = D1; }  // White O-O-O
        else if (to == G8) { rook_from = H8; rook_to = F8; }  // Black O-O
        else { rook_from = A8; rook_to = D8; }                // Black O-O-O

        undo.castling_rook_from = rook_from;
        undo.castling_rook_to   = rook_to;

        // Move king
        move_piece(from, to);
        // Move rook
        move_piece(rook_from, rook_to);

        // Update castling rights
        if (us == Color::White) castling_rights_ &= ~(WhiteOO | WhiteOOO);
        else castling_rights_ &= ~(BlackOO | BlackOOO);

    } else if (is_en_passant(m)) {
        int dir = (us == Color::White) ? -8 : 8;
        Square cap_sq = static_cast<Square>(static_cast<int>(to) + dir);
        remove_piece(cap_sq);
        move_piece(from, to);

    } else if (is_promotion(m)) {
        Piece promo_piece = make_piece(us, promotion_type(m));
        undo.promoted_to = promo_piece;

        remove_piece(from);
        if (is_capture) remove_piece(to);
        put_piece(promo_piece, to);

    } else {
        if (is_capture) remove_piece(to);
        move_piece(from, to);
    }

    // Update castling rights for king/rook moves (non-castling)
    if (!is_castling(m)) {
        if (type_of(mover) == PieceType::King) {
            if (us == Color::White) castling_rights_ &= ~(WhiteOO | WhiteOOO);
            else castling_rights_ &= ~(BlackOO | BlackOOO);
        }
        if (type_of(mover) == PieceType::Rook) {
            if (from == A1) castling_rights_ &= ~WhiteOOO;
            if (from == H1) castling_rights_ &= ~WhiteOO;
            if (from == A8) castling_rights_ &= ~BlackOOO;
            if (from == H8) castling_rights_ &= ~BlackOO;
        }
    }

    // Clear rights if a rook was captured on its starting square
    if (is_capture) {
        if (to == A1) castling_rights_ &= ~WhiteOOO;
        if (to == H1) castling_rights_ &= ~WhiteOO;
        if (to == A8) castling_rights_ &= ~BlackOOO;
        if (to == H8) castling_rights_ &= ~BlackOO;
    }

    // Set new en passant square
    en_passant_square_ = NoneSquare;
    if (is_pawn && std::abs(static_cast<int>(to) - static_cast<int>(from)) == 16) {
        en_passant_square_ = static_cast<Square>((static_cast<int>(from) + static_cast<int>(to)) / 2);
    }

    key_ ^= zobrist_castling_[castling_rights_];
    if (en_passant_square_ != NoneSquare) {
        key_ ^= zobrist_ep_[static_cast<size_t>(en_passant_square_)];
    }

    // Halfmove clock
    halfmove_clock_ = (is_pawn || is_capture) ? 0 : (halfmove_clock_ + 1);

    // Switch side and update key
    side_to_move_ = opposite(side_to_move_);
    key_ ^= zobrist_side_;

    if (side_to_move_ == Color::White) {
        ++fullmove_number_;
    }

    key_history_.push_back(key_);

    // Keep the relational graph in sync (incremental delta with exact undo recording)
    graph_.apply_move(m, undo, *this);
}

void Position::unmake_move(Move m, const StateInfo& undo) {
    Square from = from_sq(m);
    Square to   = to_sq(m);

    if (!key_history_.empty() && key_history_.back() == key_) {
        key_history_.pop_back();
    }

    if (is_castling(m)) {
        // Undo rook first, then king
        move_piece(undo.castling_rook_to, undo.castling_rook_from);
        move_piece(to, from);
    } else if (is_en_passant(m)) {
        move_piece(to, from);
        Color us = color_of(undo.moved_piece);
        int dir = (us == Color::White) ? -8 : 8;
        Square cap_sq = static_cast<Square>(static_cast<int>(to) + dir);
        put_piece(undo.captured_piece, cap_sq);  // captured was the enemy pawn
    } else if (is_promotion(m)) {
        // Remove promoted piece, put pawn back on from
        remove_piece(to);
        put_piece(undo.moved_piece, from);
        if (undo.captured_piece != Piece::None) {
            put_piece(undo.captured_piece, to);
        }
    } else {
        // Normal
        move_piece(to, from);
        if (undo.captured_piece != Piece::None) {
            put_piece(undo.captured_piece, to);
        }
    }

    // Restore state
    key_               = undo.previous_key;
    castling_rights_   = undo.castling_rights;
    en_passant_square_ = undo.en_passant_square;
    halfmove_clock_    = undo.halfmove_clock;
    side_to_move_      = opposite(side_to_move_);

    if (side_to_move_ == Color::Black) {
        --fullmove_number_;
    }

    graph_.undo_move(m, undo, *this);
}

// Null move (for null-move pruning later)
void Position::make_null_move(StateInfo& undo) {
    undo.previous_key       = key_;
    undo.en_passant_square  = en_passant_square_;
    undo.halfmove_clock     = static_cast<uint8_t>(halfmove_clock_);

    if (en_passant_square_ != NoneSquare) {
        key_ ^= zobrist_ep_[static_cast<size_t>(en_passant_square_)];
    }

    en_passant_square_ = NoneSquare;
    side_to_move_      = opposite(side_to_move_);
    key_ ^= zobrist_side_;
}

void Position::unmake_null_move(const StateInfo& undo) {
    key_               = undo.previous_key;
    en_passant_square_ = undo.en_passant_square;
    halfmove_clock_    = undo.halfmove_clock;
    side_to_move_      = opposite(side_to_move_);
}

//------------------------------------------------------------------------------
// FEN Parsing (basic, sufficient for startpos and many positions)
//------------------------------------------------------------------------------

void Position::set_from_fen(std::string_view fen) {
    clear();

    std::istringstream ss{std::string(fen)};
    std::string board, stm, castling, ep, half, full;

    ss >> board >> stm >> castling >> ep >> half >> full;

    // Parse board
    int sq = 56; // Start at a8
    for (char c : board) {
        if (c == '/') {
            sq -= 16; // Move to next rank down
            continue;
        }
        if (std::isdigit(c)) {
            sq += c - '0';
            continue;
        }
        Piece p = piece_from_char(c);
        if (p != Piece::None) {
            put_piece(p, static_cast<Square>(sq));
        }
        ++sq;
    }

    // Side to move
    side_to_move_ = (stm == "w") ? Color::White : Color::Black;

    // Castling
    castling_rights_ = NoCastling;
    for (char c : castling) {
        switch (c) {
            case 'K': castling_rights_ |= WhiteOO;  break;
            case 'Q': castling_rights_ |= WhiteOOO; break;
            case 'k': castling_rights_ |= BlackOO;  break;
            case 'q': castling_rights_ |= BlackOOO; break;
            default: break;
        }
    }

    // En passant
    if (ep != "-") {
        int file = ep[0] - 'a';
        int rank = ep[1] - '1';
        en_passant_square_ = make_square(file, rank);
    } else {
        en_passant_square_ = NoneSquare;
    }

    halfmove_clock_  = half.empty() ? 0 : std::stoi(half);
    fullmove_number_ = full.empty() ? 1 : std::stoi(full);

    // Recompute key from scratch for safety on FEN load
    key_ = 0;
    for (int p = 1; p < 13; ++p) {
        Piece piece = static_cast<Piece>(p);
        Bitboard bb = pieces(piece);
        while (bb) {
            Square s = lsb_square(bb);
            key_ ^= zobrist_piece_[static_cast<size_t>(p)][static_cast<size_t>(s)];
            (void)pop_lsb(bb);
        }
    }
    if (side_to_move_ == Color::Black) key_ ^= zobrist_side_;
    key_ ^= zobrist_castling_[castling_rights_];
    if (en_passant_square_ != NoneSquare) {
        key_ ^= zobrist_ep_[static_cast<size_t>(en_passant_square_)];
    }

    // Sync the relational graph after FEN load
    graph_.update_from_position(*this);
    key_history_.push_back(key_);
}

// Very basic FEN exporter (good enough for debugging)
std::string Position::fen() const {
    std::ostringstream out;

    // Board
    for (int rank = 7; rank >= 0; --rank) {
        int empty_run = 0;
        for (int file = 0; file < 8; ++file) {
            Square sq = make_square(file, rank);
            Piece p = piece_on(sq);
            if (p == Piece::None) {
                ++empty_run;
            } else {
                if (empty_run) {
                    out << empty_run;
                    empty_run = 0;
                }
                out << piece_char(p);
            }
        }
        if (empty_run) out << empty_run;
        if (rank > 0) out << '/';
    }

    out << (side_to_move_ == Color::White ? " w " : " b ");

    // Castling
    bool any_castle = false;
    if (castling_rights_ & WhiteOO)  { out << 'K'; any_castle = true; }
    if (castling_rights_ & WhiteOOO) { out << 'Q'; any_castle = true; }
    if (castling_rights_ & BlackOO)  { out << 'k'; any_castle = true; }
    if (castling_rights_ & BlackOOO) { out << 'q'; any_castle = true; }
    if (!any_castle) out << '-';
    out << ' ';

    // EP
    if (en_passant_square_ != NoneSquare) {
        int f = file_of(en_passant_square_);
        int r = rank_of(en_passant_square_);
        out << static_cast<char>('a' + f) << (r + 1);
    } else {
        out << '-';
    }

    out << ' ' << halfmove_clock_ << ' ' << fullmove_number_;
    return out.str();
}

bool Position::is_repetition() const {
    if (key_history_.size() < 2) return false;

    int reversible_plies = std::min<int>(halfmove_clock_, static_cast<int>(key_history_.size()) - 1);
    for (int offset = 1; offset <= reversible_plies; ++offset) {
        size_t idx = key_history_.size() - 1 - static_cast<size_t>(offset);
        if (key_history_[idx] == key_) {
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------------
// Attack Queries (very slow stubs — replace with proper attack maps)
//------------------------------------------------------------------------------

Bitboard Position::attackers_to(Square sq, Color by_color) const {
    Bitboard result = EmptyBB;
    Bitboard occ = occupancy();

    Bitboard k = pieces(make_piece(by_color, PieceType::King));
    if (k) result |= king_attacks_bb(sq) & k;

    Bitboard n = pieces(make_piece(by_color, PieceType::Knight));
    if (n) result |= knight_attacks_bb(sq) & n;

    Color them = opposite(by_color);
    Bitboard p = pieces(make_piece(by_color, PieceType::Pawn));
    if (p) result |= pawn_attacks(sq, them) & p;

    Bitboard r = pieces(make_piece(by_color, PieceType::Rook)) | pieces(make_piece(by_color, PieceType::Queen));
    if (r) result |= rook_attacks_bb(sq, occ) & r;

    Bitboard b = pieces(make_piece(by_color, PieceType::Bishop)) | pieces(make_piece(by_color, PieceType::Queen));
    if (b) result |= bishop_attacks_bb(sq, occ) & b;

    return result;
}

Bitboard Position::attackers_to(Square sq) const {
    return attackers_to(sq, Color::White) | attackers_to(sq, Color::Black);
}

bool Position::is_check() const {
    Color us = side_to_move_;
    Bitboard king_bb = pieces(make_piece(us, PieceType::King));
    if (king_bb == EmptyBB) return false; // defensive for development

    Square king_sq = lsb_square(king_bb);
    Color them = opposite(us);
    return (attackers_to(king_sq, them) != EmptyBB);
}

//------------------------------------------------------------------------------
// Debug Printing
//------------------------------------------------------------------------------

void Position::print() const {
    std::cout << "\n  +------------------------+\n";
    for (int rank = 7; rank >= 0; --rank) {
        std::cout << " " << (rank + 1) << " |";
        for (int file = 0; file < 8; ++file) {
            Square sq = make_square(file, rank);
            Piece p = piece_on(sq);
            std::cout << ' ' << piece_char(p) << ' ';
        }
        std::cout << "|\n";
    }
    std::cout << "   +------------------------+\n";
    std::cout << "     a  b  c  d  e  f  g  h\n\n";

    std::cout << "FEN: " << fen() << "\n";
    std::cout << "Side: " << (side_to_move_ == Color::White ? "White" : "Black") << "\n";
    std::cout << "Castling: " << std::bitset<4>(castling_rights_) << "\n";
    std::cout << "EP: " << (en_passant_square_ == NoneSquare ? "-" : std::to_string(static_cast<int>(en_passant_square_))) << "\n";
    std::cout << "Halfmove: " << halfmove_clock_ << "  Fullmove: " << fullmove_number_ << "\n";
    std::cout << "Key: 0x" << std::hex << key_ << std::dec << "\n\n";
}

} // namespace lulzfish::core
