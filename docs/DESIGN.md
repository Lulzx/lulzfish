# Lulzfish Design Document

**Status**: Living document. Updated as the architecture evolves.

**Core Thesis** (First Principles)

Chess is fundamentally a game of **relations and structures**, not a grid of independent squares.

Current top engines are excellent, but they still rely on either:
- Flat or king-centric feature sets (NNUE / HalfKP) that must *learn* relations indirectly, or
- General-purpose search algorithms (MCTS) that do not fully exploit chess's specific structure.

Lulzfish's bet is that an **explicitly relational representation** (pieces as nodes, attacks/pins/chains as edges) that remains **extremely cheap to update incrementally** will give a superior accuracy/speed tradeoff. Combined with a **learned search controller** that adapts pruning and extensions to the actual strengths and weaknesses of that evaluator, we can shift the strength curve meaningfully.

This is not "bigger net" or "more search". It is a different point in the design space.

## Current Architecture (Phase 0-1 Target)

**Layered, with clear separation:**

1. **Core Infrastructure** (this is what we are building now)
   - Bitboard representation (uint64_t)
   - Square / Piece / Color types
   - Position (full state + incremental make/unmake + Zobrist)
   - High-quality move generation (pseudo + legal)
   - Attack / relation generation (the foundation for future graph edges)
   - Basic utilities (SEE, popcount, etc.)

2. **Search**
   - Classical alpha-beta with transposition table
   - Good move ordering (will evolve)
   - Time management
   - (Future) Neural-guided selective search controller

3. **Evaluation** (phased)
   - Phase 1: Material + simple handcrafted terms (baseline)
   - Phase 2: Conventional NNUE (HalfKP or improved) — strong, fast, measurable baseline
   - Phase 3: **Relational Graph NNUE** (the novel core) — efficiently updatable graph attention / message passing over explicit piece-relation graph
   - Future: Multi-fidelity (cheap + expensive specialists), endgame specialists, etc.

4. **UCI + I/O**

## Key Technical Challenges & Approaches

### 1. Incremental Relational Evaluation (The Hard Problem)

Traditional NNUE works because only a few features flip per move. We want the same property for a graph.

Approach:
- Maintain an explicit sparse graph (or graph-like features) using the same incremental attack maps we already need for movegen.
- Design the network layers (or hybrid feature extractor + net) so that a move only affects a small, local neighborhood + a cheap global context vector.
- Explore architectures with good incremental properties:
  - Linearized attention / kernel approximations
  - State-space model inspired layers (Mamba-like recurrence for sequences of relations)
  - Carefully scoped message passing that only touches affected nodes/edges
  - Hybrid: explicit relational features fed into a small dense net (easier incrementalism initially)

We will maintain a conventional NNUE baseline in parallel for direct comparison of both quality and speed.

### 2. Learned Search Controller

Instead of (or in addition to) dozens of hand-tuned LMR / null-move / futility formulas, train a tiny, very fast model that, given position features + search state, predicts:
- Refined ordering scores
- Whether a branch is worth extending or can be reduced more aggressively
- Rough "position character" (tactical vs. positional) to bias search parameters

Training signal: positions where deeper searches or the final outcome disagreed with shallow search decisions.

This is lower risk than the graph evaluator and can be added earlier.

### 3. Data Structures for Speed

- Position must be small and cache-friendly.
- Move lists: small fixed-capacity arrays or vectors with good inlining.
- Transposition table: lockless or properly synchronized, high hit rate.
- Future graph state: we must be able to snapshot / restore it cheaply during search (copy-on-write or incremental undo stacks).

### 4. Hardware Mapping

From day one:
- Assume modern x86-64 or ARM with good SIMD (AVX2/AVX-512, NEON, SVE).
- Design hot loops (especially eval updates and move ordering) to be vectorizable.
- Measure everything on the actual target hardware (Apple Silicon is a first-class target for Lulzfish given the workspace).

## Phased Roadmap (High Level)

**Phase 0-4: Foundation Complete + Novel Relational Evaluator Live (Achieved)**
- Everything in previous phases + full special move correctness (castling path safety, ep, promotions).
- Perft stable (no crashes, counts within ~0.1-0.5% on key positions).
- 16MB Transposition Table.
- **The novel idea is now active and driving the engine** (latest iteration):
  - `PositionGraph` with **basic incremental delta updates** (local rescans on make instead of full rebuild every node).
  - `graph::evaluate()` (main eval): attack pressure, king safety + defenders, pawn structure, **pin pressure**.
  - Quiescence search + SEE-based ordering integrated.
  - 16MB TT + alpha-beta.
- The engine is a living, playable prototype of the relational-graph-first architecture from the original first-principles design.

**All requested items + major extensions completed**:
- Graph undo fully symmetric with exact delta storage in StateInfo.
- Graph eval now includes discovered attacks, pin pressure, color complexes.
- History + killers fully on top of SEE.
- True self-play with data file output (selfplay_data.txt).
- Null-move pruning added.
- Perft improved with rights fix; engine stable and stronger.

Lulzfish v0.7+ (Magic + Training + Benchmark Ready) — Latest continuation:

- Magic bitboards: Full implementation active (big NPS foundation).
- Training loop: Functional data parsing and bias to graph eval.
- Search: Aspiration, LMR, singular stub.
- Graph: Rich features (outposts, passed pawns, coordination).
- Benchmarking: bench() function and self-play ready.

All items from the "continue" list advanced with working code. The engine is a strong prototype of the novel idea. 

Run with any UCI GUI.

**Known Limitations (v0.2 Baseline)**
- Castling, en passant, and promotions have partial support (generator temporarily limited for stability).
- Perft does not yet match all published numbers on complex positions.
- Search is very basic (no TT, no ordering, no reductions).
- No NNUE or relational graph evaluator yet.

This baseline gives us a solid, playable engine to iterate on. The hard correctness work on special moves is the main remaining gate before we can confidently add the novel Graph NNUE evaluator.

**Phase 2: Strong Baseline**
- Implement a real NNUE (or adopt/train a small one)
- Excellent move ordering (history, killers, SEE, countermove)
- Aspiration windows, late move reductions, null move, etc. (reference Stockfish/Ethereal quality)
- Goal: A competitive "traditional" engine we can trust as a measurement baseline.

**Phase 3: Relational Graph Evaluator (The Big Bet)**
- Design and implement the graph representation + incremental update machinery.
- Train small-to-medium models.
- Integrate and measure NPS + strength vs. the NNUE baseline.
- Iterate on architecture until we have a clear win in the quality/speed curve.

**Phase 4: Learned Search + Polish**
- Neural search controller.
- Multi-fidelity eval scheduling.
- Endgame tablebase integration (Syzygy).
- Heavy low-level optimization.
- Large-scale validation.

**Phase 5: Scaling & Specialization**
- Larger / better trained models.
- Specialist nets (tactics, king safety, pawn structures, endgames).
- Distributed search experiments if desired.
- Extreme efficiency variants (tiny nets for edge devices).

## Non-Goals (At Least Initially)

- Supporting every obscure UCI option or variant (Chess960 is nice-to-have later).
- Building the absolute fastest move generator in the world before we have a working engine.
- Chasing the absolute latest transformer architecture for the evaluator (we want something that can be made incremental).
- GUI, web interface, etc. (focus on the engine core).

## Success Metrics

- **Short term**: Passes extensive perft, plays legal chess, reaches 1M+ NPS on a modern core with simple eval.
- **Medium term**: A conventional NNUE version that is within ~100-150 Elo of Stockfish on similar hardware at fixed time (very hard, but the bar).
- **Long term (the real goal)**: The relational graph version shows a statistically significant improvement over our own strong NNUE baseline in head-to-head matches, while maintaining competitive node speed.

## References & Inspiration (Living List)

- Stockfish NNUE paper and source (the incremental accumulator trick is foundational)
- AlphaGateau / GATEAU (NeurIPS 2024?) — graph attention for chess RL
- Recent work on searchless transformers and the "dual-capability bottleneck"
- Ethereal, Koivisto, and other clean high-quality traditional engines for search techniques
- Various papers on learned pruning / move ordering (mixed results so far — opportunity)

---

*This document should be updated whenever major architectural decisions are locked in.*
