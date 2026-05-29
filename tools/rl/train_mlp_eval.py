#!/usr/bin/env python3
"""Train a small MLP evaluator for Lulzfish and export C++ weights.

Pipeline:
  1. Generate self-play data with the engine (or load existing JSONL).
  2. Reconstruct the 32-per-color feature vectors from FENs using python-chess.
  3. Train a 64->32->16->1 MLP to predict the target score.
  4. Export weights as binary blob consumed by graph_eval.cpp.

The Python feature extractor mirrors graph_eval.cpp extract_features() but
omits the graph-derived relation features (indices 6-17) and center control
(index 28), which depend on the C++ PositionGraph. Training on it therefore
has a train/serve mismatch once a model is loaded in the engine.

Prefer --features-from-engine: the engine's `features` command returns the
exact 64-d vector used at inference, so training and serving stay identical.
The pure-Python path is kept only for quick offline experiments.
"""

from __future__ import annotations

import argparse
import json
import random
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np

try:
    import bulletchess as bc
except ImportError:
    raise SystemExit("Install bulletchess: python3 -m pip install bulletchess")

FEATURES_PER_COLOR = 32
FEATURES_TOTAL = FEATURES_PER_COLOR * 2  # 64
INPUT_DIM = FEATURES_TOTAL
HIDDEN1_DIM = 32
HIDDEN2_DIM = 16
OUTPUT_DIM = 1

PIECE_VALUES = {
    bc.PAWN: 100, bc.KNIGHT: 320, bc.BISHOP: 330,
    bc.ROOK: 500, bc.QUEEN: 900, bc.KING: 0,
}

PIECE_TYPES = [bc.PAWN, bc.KNIGHT, bc.BISHOP, bc.ROOK, bc.QUEEN, bc.KING]
COLORS = [bc.WHITE, bc.BLACK]


def relative_rank(sq: bc.Square, color: bc.Color) -> int:
    r = sq.index() >> 3
    return r if color is bc.WHITE else 7 - r


def center_score(sq: bc.Square) -> int:
    file = sq.index() & 7
    rank = sq.index() >> 3
    file_dist = min(abs(file - 3), abs(file - 4))
    rank_dist = min(abs(rank - 3), abs(rank - 4))
    return 6 - 2 * (file_dist + rank_dist)


def attacks_for_piece(board: bc.Board, sq: bc.Square, color: bc.Color,
                      pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> list[int]:
    file = sq.index() & 7
    rank = sq.index() >> 3
    piece_type = pieces[sq.index()][2]
    if piece_type is None:
        return []
    pt = piece_type
    result: list[int] = []

    if pt is bc.PAWN:
        step = 1 if color is bc.WHITE else -1
        for df in (-1, 1):
            f, r = file + df, rank + step
            if 0 <= f < 8 and 0 <= r < 8:
                result.append((r << 3) | f)
    elif pt is bc.KNIGHT:
        for df, dr in ((1,2),(2,1),(2,-1),(1,-2),(-1,-2),(-2,-1),(-2,1),(-1,2)):
            f, r = file + df, rank + dr
            if 0 <= f < 8 and 0 <= r < 8:
                result.append((r << 3) | f)
    elif pt is bc.KING:
        for df in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if df or dr:
                    f, r = file + df, rank + dr
                    if 0 <= f < 8 and 0 <= r < 8:
                        result.append((r << 3) | f)
    else:
        dirs: list[tuple[int, int]] = []
        if pt in (bc.ROOK, bc.QUEEN):
            dirs += [(0, 1), (1, 0), (0, -1), (-1, 0)]
        if pt in (bc.BISHOP, bc.QUEEN):
            dirs += [(1, 1), (1, -1), (-1, -1), (-1, 1)]
        for df, dr in dirs:
            f, r = file + df, rank + dr
            while 0 <= f < 8 and 0 <= r < 8:
                result.append((r << 3) | f)
                if pieces[(r << 3) | f][0]:
                    break
                f += df
                r += dr
    return result


def board_pieces(board: bc.Board) -> list[tuple[bool, bc.Color | None, bc.PieceType | None]]:
    result = [(False, None, None)] * 64
    for color in COLORS:
        for pt in PIECE_TYPES:
            for sq in board[color, pt]:
                result[sq.index()] = (True, color, pt)
    return result


def mobility(board: bc.Board, sq: bc.Square, color: bc.Color,
             pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> int:
    pt = pieces[sq.index()][2]
    if pt is None or pt in (bc.PAWN, bc.KING):
        return 0
    attacks = attacks_for_piece(board, sq, color, pieces)
    return sum(1 for a in attacks if not pieces[a][0] or pieces[a][1] is not color)


def is_passed_pawn(board: bc.Board, sq: bc.Square, color: bc.Color,
                   pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> bool:
    file = sq.index() & 7
    rank = sq.index() >> 3
    opp_color = bc.BLACK if color is bc.WHITE else bc.WHITE
    for f in range(max(0, file - 1), min(8, file + 2)):
        for r in range(8):
            idx = (r << 3) | f
            occ, c, pt = pieces[idx]
            if occ and pt is bc.PAWN and c is opp_color:
                if (color is bc.WHITE and r > rank) or (color is bc.BLACK and r < rank):
                    return False
    return True


def is_isolated_pawn(board: bc.Board, sq: bc.Square, color: bc.Color,
                     pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> bool:
    file = sq.index() & 7
    for f in range(max(0, file - 1), min(8, file + 2)):
        if f == file:
            continue
        for r in range(8):
            idx = (r << 3) | f
            occ, c, pt = pieces[idx]
            if occ and pt is bc.PAWN and c is color:
                return False
    return True


def count_doubled_pawns(pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]], color: bc.Color) -> int:
    count = 0
    for file in range(8):
        pawns = 0
        for r in range(8):
            occ, c, pt = pieces[(r << 3) | file]
            if occ and pt is bc.PAWN and c is color:
                pawns += 1
        if pawns > 1:
            count += pawns - 1
    return count


def count_open_file_rooks(pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]], color: bc.Color) -> int:
    count = 0
    for sq_idx in range(64):
        occ, c, pt = pieces[sq_idx]
        if occ and pt is bc.ROOK and c is color:
            file = sq_idx & 7
            has_pawn = False
            for r in range(8):
                occ2, _, pt2 = pieces[(r << 3) | file]
                if occ2 and pt2 is bc.PAWN:
                    has_pawn = True
                    break
            if not has_pawn:
                count += 1
    return count


def max_pawn_chain(board: bc.Board, color: bc.Color) -> int:
    pawns = set()
    for sq in board[color, bc.PAWN]:
        pawns.add(sq.index())
    if not pawns:
        return 0
    visited = set()
    max_len = 0
    for sq_idx in pawns:
        if sq_idx in visited:
            continue
        chain = 0
        cur = sq_idx
        while cur in pawns and cur not in visited:
            chain += 1
            visited.add(cur)
            nxt = None
            file = cur & 7
            rank = cur >> 3
            step = 1 if color is bc.WHITE else -1
            for df in (-1, 1):
                nf, nr = file + df, rank + step
                if 0 <= nf < 8 and 0 <= nr < 8:
                    ni = (nr << 3) | nf
                    if ni in pawns and ni not in visited:
                        nxt = ni
                        break
            cur = nxt if nxt is not None else -1
        max_len = max(max_len, chain)
    return max_len


def king_attacker_pressure(board: bc.Board, color: bc.Color,
                           pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> int:
    opp = bc.BLACK if color is bc.WHITE else bc.WHITE
    king_sqs = list(board[color, bc.KING])
    if not king_sqs:
        return 0
    king_sq = king_sqs[0]
    king_file = king_sq.index() & 7
    king_rank = king_sq.index() >> 3
    ring = set()
    for df in (-1, 0, 1):
        for dr in (-1, 0, 1):
            f, r = king_file + df, king_rank + dr
            if 0 <= f < 8 and 0 <= r < 8:
                ring.add((r << 3) | f)

    weights = {bc.PAWN: 1, bc.KNIGHT: 3, bc.BISHOP: 3, bc.ROOK: 5, bc.QUEEN: 9, bc.KING: 0}
    pressure = 0
    for pt in PIECE_TYPES:
        for sq in board[opp, pt]:
            for a in attacks_for_piece(board, sq, opp, pieces):
                if a in ring:
                    pt_val = pieces[sq.index()][2]
                    pressure += weights.get(pt_val, 0)
    return pressure


def king_shield_pawns(board: bc.Board, color: bc.Color,
                      pieces: list[tuple[bool, bc.Color | None, bc.PieceType | None]]) -> int:
    king_sqs = list(board[color, bc.KING])
    if not king_sqs:
        return 0
    ks = king_sqs[0]
    kf = ks.index() & 7
    kr = ks.index() >> 3
    step = 1 if color is bc.WHITE else -1
    count = 0
    for shield_r in (kr + step, kr + 2 * step):
        if not (0 <= shield_r < 8):
            continue
        for f in range(max(0, kf - 1), min(8, kf + 2)):
            occ, c, pt = pieces[(shield_r << 3) | f]
            if occ and pt is bc.PAWN and c is color:
                count += 1
    return count


def game_phase(board: bc.Board) -> float:
    np_material = 0
    for pt, val in PIECE_VALUES.items():
        if pt is bc.KING:
            continue
        np_material += len(board[bc.WHITE, pt]) * val
        np_material += len(board[bc.BLACK, pt]) * val
    ratio = max(0.0, min(1.0, np_material / 6000.0))
    return 1.0 - ratio


def extract_features(board: bc.Board) -> np.ndarray:
    features = np.zeros(FEATURES_TOTAL, dtype=np.float32)
    pieces = board_pieces(board)

    for ci, color in enumerate(COLORS):
        base = ci * FEATURES_PER_COLOR

        features[base + 0] = len(board[color, bc.PAWN]) * 1.0
        features[base + 1] = len(board[color, bc.KNIGHT]) * 3.2
        features[base + 2] = len(board[color, bc.BISHOP]) * 3.3
        features[base + 3] = len(board[color, bc.ROOK]) * 5.0
        features[base + 4] = len(board[color, bc.QUEEN]) * 9.0
        features[base + 5] = 0.0

        knight_mob = sum(mobility(board, sq, color, pieces) for sq in board[color, bc.KNIGHT])
        bishop_mob = sum(mobility(board, sq, color, pieces) for sq in board[color, bc.BISHOP])
        rook_mob = sum(mobility(board, sq, color, pieces) for sq in board[color, bc.ROOK])
        queen_mob = sum(mobility(board, sq, color, pieces) for sq in board[color, bc.QUEEN])
        features[base + 18] = float(knight_mob)
        features[base + 19] = float(bishop_mob)
        features[base + 20] = float(rook_mob)
        features[base + 21] = float(queen_mob)

        passed = 0
        for sq in board[color, bc.PAWN]:
            if is_passed_pawn(board, sq, color, pieces):
                passed += 1
        features[base + 22] = float(passed)
        features[base + 23] = float(count_doubled_pawns(pieces, color))
        features[base + 24] = float(sum(1 for sq in board[color, bc.PAWN] if is_isolated_pawn(board, sq, color, pieces)))
        features[base + 25] = float(max_pawn_chain(board, color))

        features[base + 26] = float(king_attacker_pressure(board, color, pieces)) / 20.0
        features[base + 27] = float(king_shield_pawns(board, color, pieces))
        features[base + 28] = 0.0
        features[base + 29] = float(count_open_file_rooks(pieces, color))
        features[base + 30] = 1.0 if len(board[color, bc.BISHOP]) >= 2 else 0.0
        features[base + 31] = game_phase(board)

    return features


def init_weights(scale: float = 0.1, seed: int = 42) -> dict[str, np.ndarray]:
    rng = np.random.default_rng(seed)
    return {
        "w1": rng.normal(0, scale, (HIDDEN1_DIM, INPUT_DIM)).astype(np.float32),
        "b1": np.zeros(HIDDEN1_DIM, dtype=np.float32),
        "w2": rng.normal(0, scale * 0.5, (HIDDEN2_DIM, HIDDEN1_DIM)).astype(np.float32),
        "b2": np.zeros(HIDDEN2_DIM, dtype=np.float32),
        "w3": rng.normal(0, scale * 0.25, (OUTPUT_DIM, HIDDEN2_DIM)).astype(np.float32),
        "b3": np.zeros(OUTPUT_DIM, dtype=np.float32),
    }


def relu(x: np.ndarray) -> np.ndarray:
    return np.maximum(x, 0)


def forward(params: dict[str, np.ndarray], x: np.ndarray) -> np.ndarray:
    h1 = relu(x @ params["w1"].T + params["b1"])
    h2 = relu(h1 @ params["w2"].T + params["b2"])
    return (h2 @ params["w3"].T + params["b3"]).reshape(-1)


def train_step(params: dict[str, np.ndarray], features: np.ndarray, targets: np.ndarray,
               lr: float) -> tuple[dict[str, np.ndarray], float]:
    n = features.shape[0]
    x = features

    h1_pre = x @ params["w1"].T + params["b1"]
    h1 = relu(h1_pre)
    h2_pre = h1 @ params["w2"].T + params["b2"]
    h2 = relu(h2_pre)
    out = (h2 @ params["w3"].T + params["b3"]).reshape(-1)

    loss = np.mean((out - targets) ** 2)

    dout = (2.0 / n) * (out - targets).reshape(-1, 1)
    dw3 = dout.T @ h2 / 1.0
    db3 = dout.sum(axis=0)
    dh2 = dout @ params["w3"]
    dh2[h2 <= 0] = 0
    dw2 = dh2.T @ h1 / 1.0
    db2 = dh2.sum(axis=0)
    dh1 = dh2 @ params["w2"]
    dh1[h1 <= 0] = 0
    dw1 = dh1.T @ x / 1.0
    db1 = dh1.sum(axis=0)

    params["w3"] -= lr * dw3
    params["b3"] -= lr * db3
    params["w2"] -= lr * dw2
    params["b2"] -= lr * db2
    params["w1"] -= lr * dw1
    params["b1"] -= lr * db1

    return params, float(loss)


def prepare_data(records: list[dict], engine: "UciEngine | None" = None) -> tuple[np.ndarray, np.ndarray]:
    xs = []
    ys = []
    for r in records:
        if engine is not None:
            # Single source of truth: identical extraction to inference,
            # including graph-derived relation features.
            fen_features = engine.features(r["fen"])
        else:
            board = bc.Board.from_fen(r["fen"])
            fen_features = extract_features(board)

        wf = fen_features[:FEATURES_PER_COLOR]
        bf = fen_features[FEATURES_PER_COLOR:]
        diff = np.zeros(FEATURES_TOTAL, dtype=np.float32)
        diff[:FEATURES_PER_COLOR] = wf - bf
        diff[FEATURES_PER_COLOR:] = bf - wf

        xs.append(diff)
        target = max(-1500, min(1500, r.get("target_score_cp", 0)))
        ys.append(target / 100.0)

    return np.stack(xs).astype(np.float32), np.array(ys, dtype=np.float32)


def serialize_weights(params: dict[str, np.ndarray]) -> bytes:
    parts: list[bytes] = []
    for key in ("w1", "b1", "w2", "b2", "w3", "b3"):
        parts.append(params[key].astype(np.float32).tobytes())
    return b"".join(parts)


def write_binary(path: Path, params: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(serialize_weights(params))


def write_cpp_header(path: Path, params: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "#pragma once",
        "",
        "// Auto-generated by tools/rl/train_mlp_eval.py",
        "// Small MLP eval: 64->32->16->1",
        "",
        "#include <array>",
        "#include <cstddef>",
        "",
        "namespace lulzfish::eval::learned {",
        "",
        f"inline constexpr std::size_t MLP_INPUT_DIM = {INPUT_DIM};",
        f"inline constexpr std::size_t MLP_HIDDEN1_DIM = {HIDDEN1_DIM};",
        f"inline constexpr std::size_t MLP_HIDDEN2_DIM = {HIDDEN2_DIM};",
        f"inline constexpr std::size_t MLP_OUTPUT_DIM = {OUTPUT_DIM};",
        "",
    ]

    for key, name in [("w1", "W1"), ("b1", "B1"), ("w2", "W2"), ("b2", "B2"), ("w3", "W3"), ("b3", "B3")]:
        arr = params[key].ravel()
        vals = ", ".join(f"{v:.8g}f" for v in arr)
        lines.append(f"inline constexpr std::array<float, {len(arr)}> MLP_{name} = {{")
        lines.append(f"    {vals}")
        lines.append("};")
        lines.append("")

    lines.append("} // namespace lulzfish::eval::learned")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def load_records(path: Path) -> list[dict]:
    records = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                records.append(json.loads(line))
    if not records:
        raise ValueError(f"No records in {path}")
    return records


class UciEngine:
    def __init__(self, path: Path):
        self.process = subprocess.Popen(
            [str(path)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self._send("uci")
        self._read_until("uciok")
        self.ready()

    def close(self):
        if self.process.poll() is None:
            try:
                self._send("quit")
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()

    def ready(self):
        self._send("isready")
        self._read_until("readyok")

    def analyse_fen(self, fen: str, depth: int) -> tuple[str, int]:
        self._send(f"position fen {fen}")
        self._send(f"go depth {depth}")
        score_cp = 0
        bestmove = "0000"
        while True:
            line = self._readline()
            if line.startswith("info ") and "score cp" in line:
                parts = line.split()
                idx = parts.index("score")
                if idx + 2 < len(parts) and parts[idx+1] == "cp":
                    score_cp = int(parts[idx+2])
            elif line.startswith("bestmove"):
                bestmove = line.split()[1] if len(line.split()) > 1 else "0000"
                return bestmove, score_cp

    def features(self, fen: str) -> np.ndarray:
        """Return the engine's 64-d feature vector for a FEN.

        Uses the engine as the single source of truth so training and
        inference share identical extraction (including the graph-derived
        relation features that cannot be reconstructed in Python).
        """
        self._send(f"position fen {fen}")
        self._send("features")
        while True:
            line = self._readline()
            if line.startswith("features"):
                vals = [float(x) for x in line.split()[1:]]
                if len(vals) != FEATURES_TOTAL:
                    raise RuntimeError(
                        f"engine returned {len(vals)} features, expected {FEATURES_TOTAL}")
                return np.array(vals, dtype=np.float32)

    def _send(self, cmd: str):
        assert self.process.stdin
        self.process.stdin.write(cmd + "\n")
        self.process.stdin.flush()

    def _readline(self) -> str:
        assert self.process.stdout
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("Engine exited")
        return line.strip()

    def _read_until(self, token: str):
        while self._readline() != token:
            pass


OPENINGS = [
    [],
    ["e2e4", "e7e5"],
    ["e2e4", "c7c5"],
    ["d2d4", "d7d5", "c2c4"],
    ["d2d4", "g8f6", "c2c4", "e7e6"],
    ["c2c4", "e7e5"],
    ["g1f3", "d7d5", "c2c4"],
]


def generate_data(engine_path: Path, out: Path, games: int, target_depth: int, seed: int) -> list[dict]:
    engine = UciEngine(engine_path)
    rng = random.Random(seed)
    records = []
    try:
        with out.open("w", encoding="utf-8") as f:
            for game in range(games):
                board = bc.Board()
                opening = OPENINGS[game % len(OPENINGS)]
                for move_text in opening:
                    move = bc.Move.from_uci(move_text)
                    if move in board.legal_moves():
                        board.apply(move)
                plies = len(opening)

                while plies < 80:
                    legal = [m.uci() for m in board.legal_moves()]
                    if not legal:
                        break

                    best, score = engine.analyse_fen(board.fen(), target_depth)
                    chosen = best if best in legal else rng.choice(legal)

                    rec = {"fen": board.fen(), "ply": plies, "target_score_cp": score, "target_bestmove": best}
                    records.append(rec)
                    f.write(json.dumps(rec, separators=(",", ":")) + "\n")

                    move = bc.Move.from_uci(chosen)
                    if move not in board.legal_moves():
                        break
                    board.apply(move)
                    plies += 1

                print(f"Game {game+1}: {len(records)} total positions", flush=True)
    finally:
        engine.close()
    return records


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data", type=Path, default=Path("data/lulzfish_training.jsonl"))
    p.add_argument("--out-bin", type=Path, default=Path("data/lulzfish_mlp.bin"))
    p.add_argument("--out-header", type=Path, default=Path("data/lulzfish_mlp.hpp"))
    p.add_argument("--engine", type=Path, default=Path("build/lulzfish"))
    p.add_argument("--generate", action="store_true", help="Generate training data before training")
    p.add_argument("--games", type=int, default=20, help="Games for data generation")
    p.add_argument("--target-depth", type=int, default=3, help="Search depth for labels")
    p.add_argument("--features-from-engine", action="store_true",
                   help="Extract features via the engine's `features` command "
                        "(single source of truth; includes graph features). "
                        "Recommended — the Python extractor omits graph relations.")
    p.add_argument("--epochs", type=int, default=200, help="Training epochs")
    p.add_argument("--lr", type=float, default=0.01, help="Learning rate")
    p.add_argument("--seed", type=int, default=42)
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if args.generate:
        print(f"Generating {args.games} games of training data...")
        records = generate_data(args.engine, args.data, args.games, args.target_depth, args.seed)
        print(f"Wrote {len(records)} positions to {args.data}")
    else:
        records = load_records(args.data)
        print(f"Loaded {len(records)} positions from {args.data}")

    print("Extracting features...")
    feat_engine = UciEngine(args.engine) if args.features_from_engine else None
    if feat_engine is not None:
        print(f"  using engine features from {args.engine}")
    try:
        X, y = prepare_data(records, feat_engine)
    finally:
        if feat_engine is not None:
            feat_engine.close()
    print(f"  X: {X.shape}, y: {y.shape}, y range: [{y.min():.1f}, {y.max():.1f}]")

    params = init_weights(seed=args.seed)
    print(f"Training MLP ({INPUT_DIM}->{HIDDEN1_DIM}->{HIDDEN2_DIM}->{OUTPUT_DIM}) for {args.epochs} epochs...")

    for epoch in range(args.epochs):
        params, loss = train_step(params, X, y, args.lr)
        if epoch % 20 == 0:
            preds = forward(params, X)
            mae = np.mean(np.abs(preds - y))
            print(f"  epoch {epoch:3d}  loss={loss:.4f}  mae={mae:.3f} cp")

    preds = forward(params, X)
    mae = np.mean(np.abs(preds - y))
    r2 = 1.0 - np.sum((y - preds)**2) / np.sum((y - np.mean(y))**2)
    print(f"Final: mae={mae:.3f} cp  r2={r2:.4f}")

    write_binary(args.out_bin, params)
    print(f"Wrote binary weights to {args.out_bin} ({args.out_bin.stat().st_size} bytes)")

    write_cpp_header(args.out_header, params)
    print(f"Wrote C++ header to {args.out_header}")

    print("\nTo load in the engine, call from C++:")
    print(f'  lulzfish::eval::graph::set_global_model_from_file("{args.out_bin}");')
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
