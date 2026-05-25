# Lulzfish

**A novel, extremely efficient high-performance chess engine built from first principles.**

Lulzfish aims to push the Pareto frontier of strength vs. compute by combining:

- **Relational graph-based evaluation** (explicit modeling of attacks, pins, structures, and piece interactions — the true nature of chess)
- **Efficiently updatable representations** (in the spirit of NNUE, but with far better inductive bias)
- **Learned selective search control** (neural guidance for pruning, extensions, and move ordering instead of brittle hand-tuned heuristics)
- **Hardware-first systems design** (SIMD, cache-friendly data layouts, incremental everything)

The long-term goal is an engine that is both **very strong** and **remarkably efficient** — capable of beating many top engines on modest hardware by doing more effective work per node and per watt.

## Current Status

**Verified correctness baseline** (May 2026)

Lulzfish is a playable UCI chess engine prototype with a now-verified move generation baseline:

- Move generation and make/unmake pass the bundled standard perft suite in Release and Debug builds
- Correct special move handling for the covered castling, en passant, and promotion positions
- Scalar ray-scanned slider attacks as the correctness baseline; verified magic bitboards are the next optimization target
- Material/PST/mobility/pawn-structure evaluator baseline with a relational graph overlay for attack pressure, king-ring safety, pawn shield, and outposts
- Search prototype: alpha-beta + iterative deepening root search + bounded check/root verification extensions + TT bounds/hash move ordering + repetition scoring + QSearch with bounded quiet checks + Null Move + SEE + History/Killers
- Search regression guardrails for basic development, center control, and tactical material capture
- `tools/lulzfish_match.py` for repeatable lightweight self-play and Stockfish smoke matches
- Self-play with data recording (selfplay_data.txt for future ML training)

The current priority is to keep correctness locked down while replacing scalar attack generation with measured, verified fast paths and turning the graph evaluator from a rebuilt-per-eval prototype into an actually incremental accumulator.

Run it with any UCI GUI, but treat playing strength and training output as prototype-level until principal variation reporting and benchmark/match infrastructure are hardened.

We are deliberately following a phased approach:

1. World-class traditional engine skeleton (bitboards, movegen, search, UCI)
2. Strong baseline NNUE-style evaluator (for apples-to-apples comparison)
3. The novel **relational / graph efficiently-updatable evaluator**
4. Learned search controller + adaptive search
5. Heavy optimization, scaling, and testing

## Building

### Requirements

- CMake 3.20+
- C++23 capable compiler (Clang 17+ or GCC 13+ recommended)
- macOS / Linux (Windows supported in theory)

### Quick Start

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-march=native"
cmake --build . -j
./lulzfish
```

For development (more debug info, less aggressive opts):

```bash
cmake .. -DCMAKE_BUILD_TYPE=Debug
```

## Running

Lulzfish speaks UCI. It can be used with any UCI-compatible GUI (CuteChess, Arena, Lichess local analysis, etc.).

Basic command line:

```bash
./lulzfish
uci
isready
position startpos
go depth 10
```

Lightweight strength probes:

```bash
python3 -m pip install bulletchess
python3 tools/lulzfish_match.py --mode selfplay --engine ./build/lulzfish --games 4 --depth 2
python3 tools/lulzfish_match.py --mode stockfish --engine ./build/lulzfish --stockfish /path/to/stockfish --games 10 --depth 2 --stockfish-depth 2 --material-adjudication 500
```

## Philosophy & First Principles

- Chess strength = search effectiveness × evaluation quality.
- Representation matters more than most people admit. Flat features force the net (or search) to rediscover structure.
- Incremental computation is a superpower (see NNUE).
- Hand-tuned heuristics have a ceiling; learned controllers that adapt to the actual evaluator can go further.
- We optimize for **effective nodes**, not just raw NPS.

See `docs/DESIGN.md` for the full technical vision.

## Contributing / Development

See `AGENTS.md` for project conventions, coding style, and how to work on Lulzfish effectively.

This is an ambitious research + engineering project. Small, correct, well-tested increments win.

## License

To be decided (likely GPL-3.0 or similar once we have real code, to match the spirit of the chess engine community).

## Acknowledgments

Inspired by the great engines that came before — Stockfish (especially its NNUE revolution), Leela Chess Zero, Ethereal, and the research community pushing graph representations and efficient search.

Built with curiosity, rigor, and a sense of humor.
