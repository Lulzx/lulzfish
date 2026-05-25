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
- Verified magic-bitboard slider attacks with scalar ray scans kept as the correctness fallback
- Material/PST/mobility/pawn-structure evaluator baseline with a relational graph overlay for attack pressure, king-ring safety, pawn shield, and outposts
- Search: alpha-beta + iterative deepening + TT bounds/hash-move ordering + history + killers + SEE + Null Move + QSearch with bounded quiet checks + repetition scoring + root opening priors across the built-in 21-opening match suite + bounded check/root verification extensions
- Search regression guardrails (19 positions covering development, center control, early knight sorties, Open Game/Italian/English/Slav/Reti/Nimzo/Benoni/Pirc/Dutch/Queen's Indian structures, tactical capture, and poisoned pawn avoidance)
- `tools/lulzfish_match.py` for repeatable lightweight self-play and Stockfish smoke matches across a 21-opening built-in suite with capped-game material adjudication
- `tools/lulzfish_gui.py` browser board backed by bulletchess legality checks and per-browser game sessions
- Browser-side WASM build (`tools/build_wasm.sh`) with a static chessground GUI and Web Worker engine isolation
- Self-play with data recording (`selfplay_data.txt` for future ML training)
- `perft_test` and `search_regression` build targets for correctness and strength guardrails

The current priority is to keep correctness locked down while replacing scalar attack generation with measured, verified fast paths and turning the graph evaluator from a rebuilt-per-eval prototype into an actually incremental accumulator.

Run it with any UCI GUI.

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
- Emscripten for the browser/WASM build

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

Browser GUI:

```bash
python3 -m pip install bulletchess
python3 tools/lulzfish_gui.py --engine ./build/lulzfish --port 8765
```

Each browser gets an isolated session cookie and game state; engine searches are serialized through the UCI process.

Browser-side WASM build:

```bash
# macOS example
brew install emscripten

tools/build_wasm.sh
python3 -m http.server 8008 --directory web/dist
```

Open `http://127.0.0.1:8008`. The static app runs Lulzfish inside a Web Worker, so each browser executes its own engine instance without a Python backend.

## Testing

Lulzfish bundles two test executables built alongside the engine when `LULZFISH_BUILD_TESTS=ON` (default).

```bash
# Build (from the build directory)
cmake --build . -j

# Correctness: perft on standard positions (startpos, Kiwipete, etc.)
./perft_test

# Strength guardrails: search-regression positions
./search_regression
```

Both must pass in Release *and* Debug builds before any change touching movegen, search, or eval is considered safe.

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
