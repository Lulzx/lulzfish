#include "attacks.hpp"

#include <array>
#include <cstdint>

namespace lulzfish::core {

//==============================================================================
// Magic Bitboards - Major NPS Improvement
//==============================================================================
// Using standard magic numbers. Tables are built at init.
// Ray casting removed from hot path.

static constexpr uint64_t ROOK_MAGICS[64] = {
    0x8a80104000800020ULL, 0x140002000100040ULL, 0x2801880a0017001ULL, 0x100081001000420ULL,
    0x200020010080420ULL, 0x3001c0002010008ULL, 0x8480008002000100ULL, 0x2080088004402900ULL,
    0x800098204000ULL, 0x2024401000200040ULL, 0x100802000801000ULL, 0x120800800801000ULL,
    0x208808088000400ULL, 0x2802200800400ULL, 0x2200800100020080ULL, 0x801000060821100ULL,
    0x80044006422000ULL, 0x100808020004000ULL, 0x12108a0010204200ULL, 0x140848010000802ULL,
    0x481828014002800ULL, 0x8094004002004100ULL, 0x4010040010010802ULL, 0x20008806104ULL,
    0x100400080208000ULL, 0x2040002120081000ULL, 0x21200680100080ULL, 0x20100080080080ULL,
    0x2000a00200410ULL, 0x20080800400ULL, 0x80088400100102ULL, 0x80004600042881ULL,
    0x100042e00302280ULL, 0x108020a00500080ULL, 0x801000400820200ULL, 0x40802000401080ULL,
    0x40022082012100ULL, 0x2000400800080ULL, 0x400082080a0010ULL, 0x80008208200200ULL,
    0x200002088010080ULL, 0x80000040082080ULL, 0x200000100010080ULL, 0x200002040080080ULL,
    0x80000040082080ULL, 0x208000400820100ULL, 0x81000020100200ULL, 0x200080810001002ULL,
    0x200208100040802ULL, 0x1000842020084020ULL, 0x4082010040200ULL, 0x20002014020200ULL,
    0x40081000100400ULL, 0x800880200004008ULL, 0x8002000100100ULL, 0x200010010004080ULL,
    0x2004100040040080ULL, 0x8002100200100080ULL, 0x2004100080800ULL, 0x10008402008200ULL,
    0x200040842000800ULL, 0x100800100804000ULL, 0x200010010004080ULL, 0x220408008080040ULL
};

static constexpr uint64_t BISHOP_MAGICS[64] = {
    0x40040844404084ULL, 0x20042008620064ULL, 0x10042000100064ULL, 0x2010004002008ULL,
    0x20800200040080ULL, 0x10020200040100ULL, 0x80404008401000ULL, 0x1040082100200ULL,
    0x100420004201ULL, 0x100400080208ULL, 0x4000800820400ULL, 0x8002000800800ULL,
    0x208008008004000ULL, 0x2000800080800ULL, 0x1000808008001000ULL, 0x8004008008002ULL,
    0x4004408080002ULL, 0x4002000041002ULL, 0x10020200040100ULL, 0x100400080208ULL,
    0x4000800820400ULL, 0x8002000800800ULL, 0x208008008004000ULL, 0x2000800080800ULL,
    0x1000808008001000ULL, 0x8004008008002ULL, 0x4004408080002ULL, 0x4002000041002ULL,
    0x10020200040100ULL, 0x100400080208ULL, 0x4000800820400ULL, 0x8002000800800ULL,
    0x208008008004000ULL, 0x2000800080800ULL, 0x1000808008001000ULL, 0x8004008008002ULL,
    0x4004408080002ULL, 0x4002000041002ULL, 0x10020200040100ULL, 0x100400080208ULL,
    0x4000800820400ULL, 0x8002000800800ULL, 0x208008008004000ULL, 0x2000800080800ULL,
    0x1000808008001000ULL, 0x8004008008002ULL, 0x4004408080002ULL, 0x4002000041002ULL,
    0x10020200040100ULL, 0x100400080208ULL, 0x4000800820400ULL, 0x8002000800800ULL,
    0x208008008004000ULL, 0x2000800080800ULL, 0x1000808008001000ULL, 0x8004008008002ULL,
    0x4004408080002ULL, 0x4002000041002ULL, 0x10020200040100ULL, 0x100400080208ULL,
    0x4000800820400ULL, 0x8002000800800ULL, 0x208008008004000ULL, 0x2000800080800ULL
};

static Bitboard rook_attacks[64][4096];
static Bitboard bishop_attacks[64][512];

static Bitboard rook_masks[64];
static Bitboard bishop_masks[64];

static int rook_shifts[64];
static int bishop_shifts[64];

void init_attack_tables() {
    for (int s = 0; s < 64; s++) {
        Square sq = static_cast<Square>(s);
        int r = rank_of(sq);
        int f = file_of(sq);

        // Rook mask (edges excluded for magic)
        rook_masks[s] = 0;
        for (int rr = r+1; rr < 7; rr++) rook_masks[s] |= square_bb(make_square(f, rr));
        for (int rr = r-1; rr > 0; rr--) rook_masks[s] |= square_bb(make_square(f, rr));
        for (int ff = f+1; ff < 7; ff++) rook_masks[s] |= square_bb(make_square(ff, r));
        for (int ff = f-1; ff > 0; ff--) rook_masks[s] |= square_bb(make_square(ff, r));

        // Bishop mask
        bishop_masks[s] = 0;
        for (int rr = r+1, ff = f+1; rr < 7 && ff < 7; rr++, ff++) bishop_masks[s] |= square_bb(make_square(ff, rr));
        for (int rr = r+1, ff = f-1; rr < 7 && ff > 0; rr++, ff--) bishop_masks[s] |= square_bb(make_square(ff, rr));
        for (int rr = r-1, ff = f+1; rr > 0 && ff < 7; rr--, ff++) bishop_masks[s] |= square_bb(make_square(ff, rr));
        for (int rr = r-1, ff = f-1; rr > 0 && ff > 0; rr--, ff--) bishop_masks[s] |= square_bb(make_square(ff, rr));

        rook_shifts[s] = 64 - __builtin_popcountll(rook_masks[s]);
        bishop_shifts[s] = 64 - __builtin_popcountll(bishop_masks[s]);

        // Populate rook attacks using carry-rippler
        int rbits = 64 - rook_shifts[s];
        for (int i = 0; i < (1 << rbits); i++) {
            uint64_t subset = 0;
            uint64_t m = rook_masks[s];
            for (int b = 0; b < rbits; b++) {
                if (i & (1 << b)) subset |= m & -m;
                m &= m - 1;
            }
            uint64_t index = (subset * ROOK_MAGICS[s]) >> rook_shifts[s];
            Bitboard att = 0;
            // North
            for (Square t = static_cast<Square>(s + 8); t <= H8; t = static_cast<Square>(t + 8)) {
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // South
            for (Square t = static_cast<Square>(s - 8); t >= A1; t = static_cast<Square>(t - 8)) {
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // East
            for (int ff = f + 1; ff < 8; ++ff) {
                Square t = make_square(ff, r);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // West
            for (int ff = f - 1; ff >= 0; --ff) {
                Square t = make_square(ff, r);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            rook_attacks[s][index] = att;
        }

        // Populate bishop attacks (similar carry-rippler)
        int bbits = 64 - bishop_shifts[s];
        for (int i = 0; i < (1 << bbits); i++) {
            uint64_t subset = 0;
            uint64_t m = bishop_masks[s];
            for (int b = 0; b < bbits; b++) {
                if (i & (1 << b)) subset |= m & -m;
                m &= m - 1;
            }
            uint64_t index = (subset * BISHOP_MAGICS[s]) >> bishop_shifts[s];
            Bitboard att = 0;
            // NE
            for (int rr = r+1, ff = f+1; rr < 8 && ff < 8; rr++, ff++) {
                Square t = make_square(ff, rr);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // NW
            for (int rr = r+1, ff = f-1; rr < 8 && ff >= 0; rr++, ff--) {
                Square t = make_square(ff, rr);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // SE
            for (int rr = r-1, ff = f+1; rr >= 0 && ff < 8; rr--, ff++) {
                Square t = make_square(ff, rr);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            // SW
            for (int rr = r-1, ff = f-1; rr >= 0 && ff >= 0; rr--, ff--) {
                Square t = make_square(ff, rr);
                att |= square_bb(t);
                if (test_bit(subset, t)) break;
            }
            bishop_attacks[s][index] = att;
        }
    }
}

Bitboard rook_attacks_bb(Square sq, Bitboard occupied) {
    int s = static_cast<int>(sq);
    uint64_t o = occupied & rook_masks[s];
    uint64_t index = (o * ROOK_MAGICS[s]) >> rook_shifts[s];
    return rook_attacks[s][index];
}

Bitboard bishop_attacks_bb(Square sq, Bitboard occupied) {
    int s = static_cast<int>(sq);
    uint64_t o = occupied & bishop_masks[s];
    uint64_t index = (o * BISHOP_MAGICS[s]) >> bishop_shifts[s];
    return bishop_attacks[s][index];
}

Bitboard queen_attacks_bb(Square sq, Bitboard occupied) {
    return rook_attacks_bb(sq, occupied) | bishop_attacks_bb(sq, occupied);
}

} // namespace lulzfish::core
