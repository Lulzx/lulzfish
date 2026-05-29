#!/usr/bin/env python3
"""Phase 0 signal discovery: train residual model with topological features.

Compares MSE between:
  - Baseline: 64-feature MLP (32 per color)
  - Topo-augmented: 128-feature MLP (32 base + 64 topo per color)

Usage:
  python3 tools/rl/train_topo_residual.py --engine build/lulzfish --data data/training.jsonl
  python3 tools/rl/train_topo_residual.py --engine build/lulzfish --generate --games 20
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

# Feature dimensions
BASE_FEATURES_PER_COLOR = 32
TOPO_FEATURES_PER_COLOR = 64
BASE_FEATURES_TOTAL = BASE_FEATURES_PER_COLOR * 2  # 64
TOPO_FEATURES_TOTAL = (BASE_FEATURES_PER_COLOR + TOPO_FEATURES_PER_COLOR) * 2  # 192

# MLP architecture
BASE_INPUT_DIM = BASE_FEATURES_TOTAL  # 64
TOPO_INPUT_DIM = TOPO_FEATURES_TOTAL  # 192
HIDDEN1_DIM = 32
HIDDEN2_DIM = 16
OUTPUT_DIM = 1


class UciEngine:
    """Minimal UCI engine wrapper for feature extraction."""

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

    def features(self, fen: str) -> np.ndarray:
        """Extract 64-dim base features."""
        self._send(f"position fen {fen}")
        self._send("features")
        while True:
            line = self._readline()
            if line.startswith("features"):
                vals = [float(x) for x in line.split()[1:]]
                if len(vals) != BASE_FEATURES_TOTAL:
                    raise RuntimeError(f"Expected {BASE_FEATURES_TOTAL} features, got {len(vals)}")
                return np.array(vals, dtype=np.float32)

    def features_with_topo(self, fen: str) -> np.ndarray:
        """Extract 192-dim features (64 base + 128 topo)."""
        self._send(f"position fen {fen}")
        self._send("features_with_topo")
        while True:
            line = self._readline()
            if line.startswith("features_with_topo"):
                vals = [float(x) for x in line.split()[1:]]
                if len(vals) != TOPO_FEATURES_TOTAL:
                    raise RuntimeError(f"Expected {TOPO_FEATURES_TOTAL} features, got {len(vals)}")
                return np.array(vals, dtype=np.float32)

    def analyse_fen(self, fen: str, depth: int) -> tuple[str, int]:
        """Run search and return (bestmove, score_cp)."""
        self._send(f"position fen {fen}")
        self._send(f"go depth {depth}")
        score_cp = 0
        bestmove = "0000"
        while True:
            line = self._readline()
            if line.startswith("info ") and "score cp" in line:
                parts = line.split()
                idx = parts.index("score")
                if idx + 2 < len(parts) and parts[idx + 1] == "cp":
                    score_cp = int(parts[idx + 2])
            elif line.startswith("bestmove"):
                bestmove = line.split()[1] if len(line.split()) > 1 else "0000"
                return bestmove, score_cp

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


def generate_data(engine: UciEngine, games: int, target_depth: int, seed: int) -> list[dict]:
    """Generate training data using the engine."""
    try:
        import bulletchess as bc
    except ImportError:
        raise SystemExit("Install bulletchess: python3 -m pip install bulletchess")

    rng = random.Random(seed)
    records = []

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

            records.append({
                "fen": board.fen(),
                "ply": plies,
                "target_score_cp": score,
                "target_bestmove": best,
            })

            move = bc.Move.from_uci(chosen)
            if move not in board.legal_moves():
                break
            board.apply(move)
            plies += 1

        print(f"Game {game + 1}: {len(records)} total positions", flush=True)

    return records


def load_records(path: Path) -> list[dict]:
    """Load training data from JSONL file."""
    records = []
    with path.open("r", encoding="utf-8") as f:
        for line in f:
            if line.strip():
                records.append(json.loads(line))
    if not records:
        raise ValueError(f"No records in {path}")
    return records


def prepare_data(records: list[dict], engine: UciEngine,
                 use_topo: bool) -> tuple[np.ndarray, np.ndarray]:
    """Extract features and targets from records."""
    xs = []
    ys = []

    for i, r in enumerate(records):
        if i % 100 == 0:
            print(f"  Extracting features: {i}/{len(records)}", flush=True)

        if use_topo:
            feats = engine.features_with_topo(r["fen"])
        else:
            feats = engine.features(r["fen"])

        # Compute diff features (stm perspective)
        n = len(feats) // 2
        diff = np.zeros_like(feats)
        diff[:n] = feats[:n] - feats[n:]
        diff[n:] = feats[n:] - feats[:n]

        xs.append(diff)
        target = max(-1500, min(1500, r.get("target_score_cp", 0)))
        ys.append(target / 100.0)

    return np.stack(xs).astype(np.float32), np.array(ys, dtype=np.float32)


def init_weights(input_dim: int, scale: float = 0.1, seed: int = 42) -> dict[str, np.ndarray]:
    """Initialize MLP weights."""
    rng = np.random.default_rng(seed)
    return {
        "w1": rng.normal(0, scale, (HIDDEN1_DIM, input_dim)).astype(np.float32),
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


def train_step(params: dict[str, np.ndarray], features: np.ndarray,
               targets: np.ndarray, lr: float) -> tuple[dict[str, np.ndarray], float]:
    """Single training step with SGD."""
    n = features.shape[0]
    x = features

    h1_pre = x @ params["w1"].T + params["b1"]
    h1 = relu(h1_pre)
    h2_pre = h1 @ params["w2"].T + params["b2"]
    h2 = relu(h2_pre)
    out = (h2 @ params["w3"].T + params["b3"]).reshape(-1)

    loss = np.mean((out - targets) ** 2)

    dout = (2.0 / n) * (out - targets).reshape(-1, 1)
    dw3 = dout.T @ h2
    db3 = dout.sum(axis=0)
    dh2 = dout @ params["w3"]
    dh2[h2 <= 0] = 0
    dw2 = dh2.T @ h1
    db2 = dh2.sum(axis=0)
    dh1 = dh2 @ params["w2"]
    dh1[h1 <= 0] = 0
    dw1 = dh1.T @ x
    db1 = dh1.sum(axis=0)

    params["w3"] -= lr * dw3
    params["b3"] -= lr * db3
    params["w2"] -= lr * dw2
    params["b2"] -= lr * db2
    params["w1"] -= lr * dw1
    params["b1"] -= lr * db1

    return params, float(loss)


def train_model(X: np.ndarray, y: np.ndarray, input_dim: int,
                epochs: int, lr: float, seed: int) -> tuple[dict[str, np.ndarray], float, float]:
    """Train MLP and return (params, final_mae, final_r2)."""
    params = init_weights(input_dim, seed=seed)

    for epoch in range(epochs):
        params, loss = train_step(params, X, y, lr)
        if epoch % 50 == 0:
            preds = forward(params, X)
            mae = np.mean(np.abs(preds - y))
            print(f"    epoch {epoch:3d}  loss={loss:.4f}  mae={mae:.3f}", flush=True)

    preds = forward(params, X)
    mae = np.mean(np.abs(preds - y))
    r2 = 1.0 - np.sum((y - preds) ** 2) / np.sum((y - np.mean(y)) ** 2)

    return params, mae, r2


def serialize_weights(params: dict[str, np.ndarray]) -> bytes:
    parts: list[bytes] = []
    for key in ("w1", "b1", "w2", "b2", "w3", "b3"):
        parts.append(params[key].astype(np.float32).tobytes())
    return b"".join(parts)


def write_binary(path: Path, params: dict[str, np.ndarray]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(serialize_weights(params))


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--data", type=Path, default=Path("data/lulzfish_training.jsonl"))
    p.add_argument("--engine", type=Path, default=Path("build/lulzfish"))
    p.add_argument("--generate", action="store_true", help="Generate training data")
    p.add_argument("--games", type=int, default=10, help="Games for data generation")
    p.add_argument("--target-depth", type=int, default=3, help="Search depth for labels")
    p.add_argument("--epochs", type=int, default=200, help="Training epochs")
    p.add_argument("--lr", type=float, default=0.01, help="Learning rate")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out-dir", type=Path, default=Path("data/topo_experiment"))
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("SheafTop Phase 0: Signal Discovery")
    print("=" * 60)

    # Initialize engine
    engine = UciEngine(args.engine)
    print(f"Engine: {args.engine}")

    # Generate or load data
    if args.generate:
        print(f"\nGenerating {args.games} games of training data...")
        records = generate_data(engine, args.games, args.target_depth, args.seed)
        args.data.parent.mkdir(parents=True, exist_ok=True)
        with args.data.open("w", encoding="utf-8") as f:
            for r in records:
                f.write(json.dumps(r, separators=(",", ":")) + "\n")
        print(f"Wrote {len(records)} positions to {args.data}")
    else:
        records = load_records(args.data)
        print(f"Loaded {len(records)} positions from {args.data}")

    # Subsample for faster experiments
    if len(records) > 2000:
        rng = random.Random(args.seed)
        records = rng.sample(records, 2000)
        print(f"Subsampled to {len(records)} positions for experiment")

    # Train baseline model (64 features)
    print("\n" + "=" * 60)
    print("Training BASELINE model (64 features)")
    print("=" * 60)
    print("Extracting base features...")
    X_base, y = prepare_data(records, engine, use_topo=False)
    print(f"  X: {X_base.shape}, y: {y.shape}")

    print("Training...")
    params_base, mae_base, r2_base = train_model(
        X_base, y, BASE_INPUT_DIM, args.epochs, args.lr, args.seed)

    # Train topo-augmented model (192 features)
    print("\n" + "=" * 60)
    print("Training TOPO-AUGMENTED model (192 features)")
    print("=" * 60)
    print("Extracting features with topological features...")
    X_topo, y_topo = prepare_data(records, engine, use_topo=True)
    print(f"  X: {X_topo.shape}, y: {y_topo.shape}")

    print("Training...")
    params_topo, mae_topo, r2_topo = train_model(
        X_topo, y_topo, TOPO_INPUT_DIM, args.epochs, args.lr, args.seed)

    # Compare results
    print("\n" + "=" * 60)
    print("RESULTS COMPARISON")
    print("=" * 60)
    print(f"{'Model':<25} {'MAE (cp)':<12} {'R²':<12}")
    print("-" * 49)
    print(f"{'Baseline (64 feat)':<25} {mae_base:<12.3f} {r2_base:<12.4f}")
    print(f"{'Topo-augmented (192)':<25} {mae_topo:<12.3f} {r2_topo:<12.4f}")

    mae_improvement = (mae_base - mae_topo) / mae_base * 100
    r2_improvement = r2_topo - r2_base

    print(f"\nMAE improvement: {mae_improvement:+.1f}%")
    print(f"R² improvement:  {r2_improvement:+.4f}")

    # Save results
    results = {
        "baseline": {"mae": mae_base, "r2": r2_base, "features": BASE_INPUT_DIM},
        "topo_augmented": {"mae": mae_topo, "r2": r2_topo, "features": TOPO_INPUT_DIM},
        "mae_improvement_pct": mae_improvement,
        "r2_improvement": r2_improvement,
        "num_positions": len(records),
        "epochs": args.epochs,
        "lr": args.lr,
    }
    results_path = args.out_dir / "results.json"
    with results_path.open("w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults saved to {results_path}")

    # Save weights
    write_binary(args.out_dir / "baseline_weights.bin", params_base)
    write_binary(args.out_dir / "topo_weights.bin", params_topo)
    print(f"Weights saved to {args.out_dir}/")

    # Phase 0 success criteria
    print("\n" + "=" * 60)
    print("PHASE 0 SUCCESS CRITERIA")
    print("=" * 60)
    if mae_improvement >= 8:
        print(f"✓ MAE improvement {mae_improvement:.1f}% >= 8% threshold")
    else:
        print(f"✗ MAE improvement {mae_improvement:.1f}% < 8% threshold")

    if r2_improvement >= 0.02:
        print(f"✓ R² improvement {r2_improvement:.4f} >= 0.02 threshold")
    else:
        print(f"✗ R² improvement {r2_improvement:.4f} < 0.02 threshold")

    engine.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
