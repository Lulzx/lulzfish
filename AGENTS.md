# Lulzfish Agent Guidelines

This document governs how humans and AI agents work on the Lulzfish chess engine. It is the single source of truth for project conventions.

## Project Goals (Never Forget)

We are building a **novel, extremely efficient, high-performance chess engine** that can realistically challenge or beat many strong engines.

Core technical bets (from first principles):

1. **Relational / graph representation** of chess positions (attacks, pins, structures, piece interactions) is a superior inductive bias compared to flat or CNN-style features.
2. We can make such a representation **efficiently updatable** (the NNUE trick applied to a much better representation).
3. **Learned search control** (pruning, ordering, extensions) will outperform purely hand-crafted heuristics long-term.
4. Hardware co-design and incremental everything from day one is non-negotiable for real strength.

We do not chase raw Elo at the expense of cleanliness early on. We do not add complexity without measurement. We verify correctness ruthlessly (especially move generation).

## Development Philosophy

- **Correctness first, then speed.** A fast engine that generates illegal moves or crashes is worthless.
- **Measure everything.** NPS, effective branching factor, win rates via SPRT or long matches, perft speeds.
- **Incremental is sacred.** Almost every hot path in a chess engine benefits from incremental/differential updates. This principle will be even more important for our graph evaluator.
- **Small, reviewable changes.** Prefer many small PRs / commits over giant ones.
- **Test the scary parts hardest.** Move generation, make/unmake, Zobrist, castling, en passant, promotions, 960 if we support it later.
- **No premature optimization.** Profile before you hand-write AVX intrinsics.

## Coding Style & Conventions

### Language & Tooling
- Modern C++23.
- Use `std::` where reasonable. Avoid unnecessary macros.
- `constexpr` and `consteval` aggressively for anything that can be compile-time.
- Prefer value types and small structs. Avoid heavy inheritance.
- Namespaces: `lulzfish::` (or nested, e.g. `lulzfish::core`).
- Files: snake_case for .cpp/.hpp (e.g. `bitboard.hpp`).
- Classes: PascalCase (`Position`, `MoveGenerator`).
- Functions/variables: snake_case (`generate_moves`, `white_to_move`).
- Constants / enums: UPPER_SNAKE or Pascal for enum classes.

### Bitboards & Low-Level
- `using Bitboard = uint64_t;`
- Square: 0-63, a1=0, h1=7, a8=56, h8=63 (little-endian rank-file or consistent with Stockfish convention — document choice).
- Document any magic numbers or platform-specific intrinsics clearly.
- Provide scalar fallback + SIMD paths where relevant (later).

### Error Handling
- In debug: heavy use of `assert()` for invariants.
- In release: fast paths, minimal checks. Crashing on illegal input to UCI is acceptable if documented.
- Never silently ignore corruption in make/unmake or zobrist.

### Performance
- Hot functions should be small and inlinable.
- Avoid virtual dispatch in search/eval hot paths.
- Data layouts matter: think about cache lines for Position, Move lists, etc.
- When adding new eval terms or graph features, always measure NPS impact immediately.

## Project Structure (Current & Planned)

```
src/
  lulzfish/
    core/          # types, bitboard, attacks, position, movegen
    search/        # alpha-beta, TT, time management, search controller (future)
    eval/          # material baseline → NNUE baseline → Graph NNUE (the big one)
    uci/           # protocol handling
    utils/         # magic numbers, PRNG, logging, etc.
  main.cpp

tests/             # perft, correctness, regression
docs/              # DESIGN.md and research notes
```

## Critical Workflows

### Adding or Changing Move Generation
1. Update perft numbers on standard positions (Kiwipete, startpos, etc.).
2. Compare against Stockfish or another reference engine's perft.
3. Run both debug and release builds.
4. Test castling, en passant, promotions, pins, checks thoroughly.

### Changing make() / unmake()
This is one of the most dangerous areas. Any change requires:
- Full perft validation
- Zobrist collision / consistency tests (make + unmake should return to identical hash)
- Tests with 960 positions if supported

### Evaluation Changes
- Always run a small fixed-node or fixed-time match against the previous version.
- Measure NPS delta.
- For the future Graph NNUE: we will maintain a "conventional NNUE baseline" branch or build for direct comparison.

### Search Changes
- Use SPRT (Sequential Probability Ratio Test) for strength claims whenever possible.
- Document the exact time controls and hardware.
- Be extremely skeptical of +20 Elo claims from small sample sizes.

## Git & Commit Hygiene

- Commit messages: imperative mood, explain *why* for non-obvious changes.
- Never commit generated files, large binaries, or personal data.
- Rebase or clean history before opening PRs against main.

## When Working with AI Agents (including future Grok sessions)

- Always read `AGENTS.md`, `README.md`, and `docs/DESIGN.md` at the start of a session.
- Update DESIGN.md and relevant docs when architectural decisions are made.
- Never skip perft validation when touching movegen or Position.
- If a change touches the core incremental update paths, explicitly call out the impact on future Graph NNUE work.
- Prefer writing clear code + comments over clever one-liners in hot paths.

## Testing Strategy (Evolving)

Current (early phases):
- Perft as the primary correctness oracle.
- Manual UCI play + simple self-play for sanity.
- Fixed-depth or fixed-node matches vs. known engines once we have a playable version.

Later:
- Proper test suite (Catch2 or custom).
- Regression suite of tactical positions.
- Large-scale gauntlets (thousands of games) using cutechess-cli.
- Node-based and time-based testing.

## Long-Term Technical Bets We Are Making

- Explicit relational modeling (graph of pieces + attack/relation edges) will beat flat feature sets.
- We can solve the incremental update problem for graph attention / message passing at acceptable cost.
- A tiny learned controller network can meaningfully improve search decisions over hand-crafted code.
- The combination of (better eval + smarter search) is superadditive.

We are not just reimplementing Stockfish with a different net. We are attempting something meaningfully different in the representation layer.

## Final Rule

If you are unsure whether a change is a good idea for Lulzfish's long-term goals, ask: "Does this move us closer to a high-quality, efficiently-updatable relational evaluator + learned search controller, or is it just local optimization of the old paradigm?"

When in doubt, build the foundation correctly and measure.

---

*Last updated: 2026-05 (initial version)*
