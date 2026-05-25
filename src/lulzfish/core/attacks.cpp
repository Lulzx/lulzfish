#include "attacks.hpp"

#include <algorithm>
#include <vector>

namespace lulzfish::core {

//==============================================================================
// Magic Bitboard Slider Attack Generation
//==============================================================================
// Full magic bitboard implementation with deterministic runtime magic search.
// Validated against scalar ray scans for correctness.

namespace {

struct MagicEntry {
    Bitboard mask;
    Bitboard magic;
    int shift;
    size_t offset;
};

MagicEntry rook_entries[64];
MagicEntry bishop_entries[64];
std::vector<Bitboard> attacks_table;

uint64_t splitmix64_next(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

Bitboard random_candidate(uint64_t& rng_state) {
    return splitmix64_next(rng_state) & splitmix64_next(rng_state) & splitmix64_next(rng_state);
}

Bitboard compute_mask(Square sq, bool is_rook) {
    Bitboard mask = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    int dirs[4][2];
    if (is_rook) {
        dirs[0][0] = 0; dirs[0][1] = 1;
        dirs[1][0] = 0; dirs[1][1] = -1;
        dirs[2][0] = 1; dirs[2][1] = 0;
        dirs[3][0] = -1; dirs[3][1] = 0;
    } else {
        dirs[0][0] = 1; dirs[0][1] = 1;
        dirs[1][0] = 1; dirs[1][1] = -1;
        dirs[2][0] = -1; dirs[2][1] = 1;
        dirs[3][0] = -1; dirs[3][1] = -1;
    }

    for (const auto& d : dirs) {
        for (int f = file + d[0], r = rank + d[1];
             f >= 0 && f < 8 && r >= 0 && r < 8;
             f += d[0], r += d[1]) {
            int next_f = f + d[0];
            int next_r = r + d[1];
            if (next_f < 0 || next_f >= 8 || next_r < 0 || next_r >= 8) {
                break;
            }
            set_bit(mask, make_square(f, r));
        }
    }

    return mask;
}

Bitboard slider_attacks(Square sq, Bitboard occupied, bool is_rook) {
    Bitboard attacks = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    int dirs[4][2];
    if (is_rook) {
        dirs[0][0] = 0; dirs[0][1] = 1;
        dirs[1][0] = 0; dirs[1][1] = -1;
        dirs[2][0] = 1; dirs[2][1] = 0;
        dirs[3][0] = -1; dirs[3][1] = 0;
    } else {
        dirs[0][0] = 1; dirs[0][1] = 1;
        dirs[1][0] = 1; dirs[1][1] = -1;
        dirs[2][0] = -1; dirs[2][1] = 1;
        dirs[3][0] = -1; dirs[3][1] = -1;
    }

    for (const auto& d : dirs) {
        for (int f = file + d[0], r = rank + d[1];
             f >= 0 && f < 8 && r >= 0 && r < 8;
             f += d[0], r += d[1]) {
            Square target = make_square(f, r);
            set_bit(attacks, target);
            if (test_bit(occupied, target)) break;
        }
    }

    return attacks;
}

void enumerate_occupancies(Bitboard mask, std::vector<Bitboard>& occupancies) {
    occupancies.clear();
    occupancies.reserve(static_cast<size_t>(1ULL << popcount(mask)));

    Bitboard subset = EmptyBB;
    do {
        occupancies.push_back(subset);
        subset = (subset - mask) & mask;
    } while (subset);
}

bool candidate_maps_without_collisions(Bitboard candidate,
                                       int relevant_bits,
                                       const std::vector<Bitboard>& occupancies,
                                       const std::vector<Bitboard>& attacks) {
    size_t table_size = static_cast<size_t>(1ULL << relevant_bits);
    std::vector<Bitboard> used(table_size, EmptyBB);
    std::vector<bool> filled(table_size, false);
    int shift = 64 - relevant_bits;

    for (size_t i = 0; i < occupancies.size(); ++i) {
        size_t index = static_cast<size_t>((occupancies[i] * candidate) >> shift);
        if (!filled[index]) {
            filled[index] = true;
            used[index] = attacks[i];
        } else if (used[index] != attacks[i]) {
            return false;
        }
    }

    return true;
}

bool find_magic(Bitboard mask,
                int relevant_bits,
                const std::vector<Bitboard>& occupancies,
                const std::vector<Bitboard>& attacks,
                uint64_t& rng_state,
                Bitboard& magic) {
    constexpr int MAX_ATTEMPTS = 10000000;

    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
        Bitboard candidate = random_candidate(rng_state);
        if (candidate == EmptyBB) continue;

        if (relevant_bits >= 6 &&
            popcount((mask * candidate) & 0xFF00000000000000ULL) < 6) {
            continue;
        }

        if (candidate_maps_without_collisions(candidate, relevant_bits, occupancies, attacks)) {
            magic = candidate;
            return true;
        }
    }

    return false;
}

bool build_slider_table_for_square(Square s,
                                   bool is_rook,
                                   MagicEntry& entry,
                                   size_t table_offset,
                                   uint64_t& rng_state) {
    std::vector<Bitboard> occupancies;
    enumerate_occupancies(entry.mask, occupancies);

    std::vector<Bitboard> attacks;
    attacks.reserve(occupancies.size());
    for (Bitboard occupancy : occupancies) {
        attacks.push_back(slider_attacks(s, occupancy, is_rook));
    }

    int relevant_bits = popcount(entry.mask);
    Bitboard magic = EmptyBB;
    if (!find_magic(entry.mask, relevant_bits, occupancies, attacks, rng_state, magic)) {
        return false;
    }

    entry.magic = magic;
    entry.shift = 64 - relevant_bits;
    entry.offset = table_offset;

    for (size_t i = 0; i < occupancies.size(); ++i) {
        size_t index = static_cast<size_t>((occupancies[i] * entry.magic) >> entry.shift);
        attacks_table[table_offset + index] = attacks[i];
    }

    return true;
}

bool build_magic_tables() {
    size_t total_entries = 0;
    for (int sq = 0; sq < 64; ++sq) {
        Square s = static_cast<Square>(sq);
        rook_entries[sq].mask = compute_mask(s, true);
        bishop_entries[sq].mask = compute_mask(s, false);
        total_entries += static_cast<size_t>(1ULL << popcount(rook_entries[sq].mask));
        total_entries += static_cast<size_t>(1ULL << popcount(bishop_entries[sq].mask));
    }

    attacks_table.assign(total_entries, EmptyBB);
    size_t table_offset = 0;
    uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

    for (int sq = 0; sq < 64; ++sq) {
        Square s = static_cast<Square>(sq);
        int rbits = popcount(rook_entries[sq].mask);
        int bbits = popcount(bishop_entries[sq].mask);

        if (!build_slider_table_for_square(s, true, rook_entries[sq], table_offset, rng_state)) {
            return false;
        }
        table_offset += static_cast<size_t>(1ULL << rbits);

        if (!build_slider_table_for_square(s, false, bishop_entries[sq], table_offset, rng_state)) {
            return false;
        }
        table_offset += static_cast<size_t>(1ULL << bbits);
    }

    return true;
}

Bitboard magic_attacks(Square sq, Bitboard occupied, const MagicEntry* entries) {
    const MagicEntry& entry = entries[static_cast<int>(sq)];
    size_t idx = entry.offset + static_cast<size_t>(((occupied & entry.mask) * entry.magic) >> entry.shift);
    return attacks_table[idx];
}

bool attack_tables_initialized = false;

bool verify_magic_rook(Bitboard (*ref)(Square, Bitboard)) {
    for (int sq = 0; sq < 64; ++sq) {
        Square s = static_cast<Square>(sq);
        Bitboard mask = rook_entries[sq].mask;
        Bitboard subset = EmptyBB;
        do {
            Bitboard magic_result = magic_attacks(s, subset, rook_entries);
            Bitboard ref_result = ref(s, subset);
            if (magic_result != ref_result) return false;
            subset = (subset - mask) & mask;
        } while (subset);
    }
    return true;
}

bool verify_magic_bishop(Bitboard (*ref)(Square, Bitboard)) {
    for (int sq = 0; sq < 64; ++sq) {
        Square s = static_cast<Square>(sq);
        Bitboard mask = bishop_entries[sq].mask;
        Bitboard subset = EmptyBB;
        do {
            Bitboard magic_result = magic_attacks(s, subset, bishop_entries);
            Bitboard ref_result = ref(s, subset);
            if (magic_result != ref_result) return false;
            subset = (subset - mask) & mask;
        } while (subset);
    }
    return true;
}

} // anonymous namespace

void init_attack_tables() {
    if (attack_tables_initialized) return;

    bool built = build_magic_tables();

    // Verify magic tables against scalar ray scans (correctness guard).
    bool rook_ok = built && verify_magic_rook([](Square s, Bitboard o) {
        return rook_attacks_bb_scalar(s, o);
    });
    bool bishop_ok = built && verify_magic_bishop([](Square s, Bitboard o) {
        return bishop_attacks_bb_scalar(s, o);
    });

    if (!rook_ok || !bishop_ok) {
        attacks_table.clear();
    }

    attack_tables_initialized = true;
}

bool magic_tables_ready() {
    return attack_tables_initialized && !attacks_table.empty();
}

//==============================================================================
// Scalar Fallback (correctness reference)
//==============================================================================

Bitboard rook_attacks_bb_scalar(Square sq, Bitboard occupied) {
    Bitboard attacks = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    for (int r = rank + 1; r < 8; ++r) {
        Square to = make_square(file, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int r = rank - 1; r >= 0; --r) {
        Square to = make_square(file, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file + 1; f < 8; ++f) {
        Square to = make_square(f, rank);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1; f >= 0; --f) {
        Square to = make_square(f, rank);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }

    return attacks;
}

Bitboard bishop_attacks_bb_scalar(Square sq, Bitboard occupied) {
    Bitboard attacks = EmptyBB;
    int file = file_of(sq);
    int rank = rank_of(sq);

    for (int f = file + 1, r = rank + 1; f < 8 && r < 8; ++f, ++r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1, r = rank + 1; f >= 0 && r < 8; --f, ++r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file + 1, r = rank - 1; f < 8 && r >= 0; ++f, --r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }
    for (int f = file - 1, r = rank - 1; f >= 0 && r >= 0; --f, --r) {
        Square to = make_square(f, r);
        attacks |= square_bb(to);
        if (test_bit(occupied, to)) break;
    }

    return attacks;
}

//==============================================================================
// Public API — dispatch to magic or scalar
//==============================================================================

Bitboard rook_attacks_bb(Square sq, Bitboard occupied) {
    if (!attack_tables_initialized) init_attack_tables();
    if (magic_tables_ready()) {
        return magic_attacks(sq, occupied, rook_entries);
    }
    return rook_attacks_bb_scalar(sq, occupied);
}

Bitboard bishop_attacks_bb(Square sq, Bitboard occupied) {
    if (!attack_tables_initialized) init_attack_tables();
    if (magic_tables_ready()) {
        return magic_attacks(sq, occupied, bishop_entries);
    }
    return bishop_attacks_bb_scalar(sq, occupied);
}

Bitboard queen_attacks_bb(Square sq, Bitboard occupied) {
    return rook_attacks_bb(sq, occupied) | bishop_attacks_bb(sq, occupied);
}

} // namespace lulzfish::core
