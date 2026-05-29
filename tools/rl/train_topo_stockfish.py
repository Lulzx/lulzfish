#!/usr/bin/env python3
"""Phase 0 signal discovery using Stockfish for labels.

Uses Stockfish for high-quality labels and Lulzfish for feature extraction.
Tests whether topological features improve prediction of Stockfish's eval.

Usage:
  python3 tools/rl/train_topo_stockfish.py --positions 1000
"""

from __future__ import annotations

import argparse
import json
import random
import subprocess
import sys
from pathlib import Path

import numpy as np

try:
    import bulletchess as bc
except ImportError:
    raise SystemExit("Install bulletchess: python3 -m pip install bulletchess")


class StockfishEngine:
    """Minimal Stockfish wrapper for position labeling."""

    def __init__(self, path: str = "stockfish", depth: int = 8):
        self.process = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self.depth = depth
        self._send("uci")
        self._read_until("uciok")

    def close(self):
        if self.process.poll() is None:
            self._send("quit")
            self.process.wait(timeout=2)

    def evaluate(self, fen: str) -> int:
        """Return Stockfish's evaluation in centipawns."""
        self._send(f"position fen {fen}")
        self._send(f"go depth {self.depth}")
        score = 0
        while True:
            line = self._readline()
            if "score cp" in line:
                parts = line.split()
                idx = parts.index("score")
                if idx + 2 < len(parts) and parts[idx + 1] == "cp":
                    score = int(parts[idx + 2])
            elif line.startswith("bestmove"):
                return score

    def _send(self, cmd: str):
        self.process.stdin.write(cmd + "\n")
        self.process.stdin.flush()

    def _readline(self) -> str:
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("Stockfish exited")
        return line.strip()

    def _read_until(self, token: str):
        while self._readline() != token:
            pass


class LulzfishEngine:
    """Minimal Lulzfish wrapper for feature extraction."""

    def __init__(self, path: str = "build/lulzfish"):
        self.process = subprocess.Popen(
            [path], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, text=True, bufsize=1)
        self._send("uci")
        self._read_until("uciok")
        self._send("isready")
        self._read_until("readyok")

    def close(self):
        if self.process.poll() is None:
            self._send("quit")
            self.process.wait(timeout=2)

    def features(self, fen: str) -> np.ndarray:
        """Extract 64-dim base features."""
        self._send(f"position fen {fen}")
        self._send("features")
        while True:
            line = self._readline()
            if line.startswith("features"):
                vals = [float(x) for x in line.split()[1:]]
                return np.array(vals, dtype=np.float32)

    def features_with_topo(self, fen: str) -> np.ndarray:
        """Extract 192-dim features (64 base + 128 topo)."""
        self._send(f"position fen {fen}")
        self._send("features_with_topo")
        while True:
            line = self._readline()
            if line.startswith("features_with_topo"):
                vals = [float(x) for x in line.split()[1:]]
                return np.array(vals, dtype=np.float32)

    def _send(self, cmd: str):
        self.process.stdin.write(cmd + "\n")
        self.process.stdin.flush()

    def _readline(self) -> str:
        line = self.process.stdout.readline()
        if not line:
            raise RuntimeError("Lulzfish exited")
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
    ["e2e4", "e7e6"],
    ["d2d4", "d7d5"],
    ["c2c4", "c7c5"],
]


def generate_positions(num_positions: int, seed: int = 42) -> list[str]:
    """Generate positions by playing random games."""
    rng = random.Random(seed)
    positions = []
    game = 0

    while len(positions) < num_positions:
        board = bc.Board()
        opening = OPENINGS[game % len(OPENINGS)]
        for move_text in opening:
            move = bc.Move.from_uci(move_text)
            if move in board.legal_moves():
                board.apply(move)

        plies = len(opening)
        while plies < 40 and len(positions) < num_positions:
            legal = list(board.legal_moves())
            if not legal:
                break

            positions.append(board.fen())

            # Mix of random and captures
            board.apply(rng.choice(legal))
            plies += 1

        game += 1

    return positions[:num_positions]


def make_diff(feats: np.ndarray) -> np.ndarray:
    """Convert features to stm-perspective diff format."""
    n = len(feats) // 2
    diff = np.zeros_like(feats)
    diff[:n] = feats[:n] - feats[n:]
    diff[n:] = feats[n:] - feats[:n]
    return diff


def relu(x: np.ndarray) -> np.ndarray:
    return np.maximum(x, 0)


def forward(params: dict[str, np.ndarray], x: np.ndarray) -> np.ndarray:
    h1 = relu(x @ params["w1"].T + params["b1"])
    h2 = relu(h1 @ params["w2"].T + params["b2"])
    return (h2 @ params["w3"].T + params["b3"]).reshape(-1)


def train_and_eval(X_train: np.ndarray, y_train: np.ndarray,
                   X_test: np.ndarray, y_test: np.ndarray,
                   input_dim: int, epochs: int = 200, lr: float = 0.005,
                   seed: int = 42) -> tuple[float, float]:
    """Train MLP and return (test_mae, test_r2)."""
    rng = np.random.default_rng(seed)
    params = {
        "w1": rng.normal(0, 0.1, (32, input_dim)).astype(np.float32),
        "b1": np.zeros(32, dtype=np.float32),
        "w2": rng.normal(0, 0.05, (16, 32)).astype(np.float32),
        "b2": np.zeros(16, dtype=np.float32),
        "w3": rng.normal(0, 0.025, (1, 16)).astype(np.float32),
        "b3": np.zeros(1, dtype=np.float32),
    }

    best_mae = float("inf")
    patience = 30
    stall = 0

    for epoch in range(epochs):
        # Forward + backward
        h1 = relu(X_train @ params["w1"].T + params["b1"])
        h2 = relu(h1 @ params["w2"].T + params["b2"])
        out = (h2 @ params["w3"].T + params["b3"]).reshape(-1)

        n = X_train.shape[0]
        dout = (2.0 / n) * (out - y_train).reshape(-1, 1)
        dw3 = dout.T @ h2
        db3 = dout.sum(axis=0)
        dh2 = dout @ params["w3"]
        dh2[h2 <= 0] = 0
        dw2 = dh2.T @ h1
        db2 = dh2.sum(axis=0)
        dh1 = dh2 @ params["w2"]
        dh1[h1 <= 0] = 0
        dw1 = dh1.T @ X_train
        db1 = dh1.sum(axis=0)

        params["w3"] -= lr * dw3
        params["b3"] -= lr * db3
        params["w2"] -= lr * dw2
        params["b2"] -= lr * db2
        params["w1"] -= lr * dw1
        params["b1"] -= lr * db1

        # Early stopping
        if epoch % 10 == 0:
            test_preds = forward(params, X_test)
            test_mae = np.mean(np.abs(test_preds - y_test))
            if test_mae < best_mae:
                best_mae = test_mae
                stall = 0
            else:
                stall += 1
            if stall >= patience:
                break

    test_preds = forward(params, X_test)
    test_mae = np.mean(np.abs(test_preds - y_test))
    test_r2 = 1.0 - np.sum((y_test - test_preds) ** 2) / np.sum((y_test - np.mean(y_test)) ** 2)
    return test_mae, test_r2


def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--stockfish", default="stockfish", help="Stockfish path")
    p.add_argument("--lulzfish", default="build/lulzfish", help="Lulzfish path")
    p.add_argument("--positions", type=int, default=500, help="Number of positions")
    p.add_argument("--sf-depth", type=int, default=8, help="Stockfish search depth")
    p.add_argument("--epochs", type=int, default=200, help="Training epochs")
    p.add_argument("--folds", type=int, default=5, help="Cross-validation folds")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", type=Path, default=Path("data/topo_experiment"))
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)

    print("=" * 60)
    print("SheafTop Phase 0: Signal Discovery (Stockfish Labels)")
    print("=" * 60)

    # Generate positions
    print(f"\nGenerating {args.positions} positions...")
    positions = generate_positions(args.positions, args.seed)
    print(f"Generated {len(positions)} positions")

    # Initialize engines
    print("\nInitializing engines...")
    sf = StockfishEngine(args.stockfish, args.sf_depth)
    lf = LulzfishEngine(args.lulzfish)

    # Extract features and labels
    print("Extracting features and labels...")
    base_feats = []
    topo_feats = []
    labels = []

    for i, fen in enumerate(positions):
        if i % 50 == 0:
            print(f"  {i}/{len(positions)}", flush=True)

        # Stockfish label
        score = sf.evaluate(fen)
        labels.append(score)

        # Lulzfish features
        base = lf.features(fen)
        topo = lf.features_with_topo(fen)
        base_feats.append(base)
        topo_feats.append(topo)

    sf.close()
    lf.close()

    X_base = np.stack(base_feats)
    X_topo = np.stack(topo_feats)
    y = np.array([max(-1500, min(1500, s)) / 100.0 for s in labels])

    print(f"\nDataset: {len(positions)} positions")
    print(f"y range: [{y.min():.1f}, {y.max():.1f}], mean={y.mean():.2f}, std={y.std():.2f}")

    # Convert to diff format
    X_base_diff = make_diff(X_base)
    X_topo_diff = make_diff(X_topo)

    # Topological feature stats
    topo_features = X_topo_diff[:, 64:]
    print(f"\nTopological feature stats:")
    print(f"  Non-zero: {np.count_nonzero(topo_features)}")
    print(f"  Mean |val|: {np.mean(np.abs(topo_features)):.4f}")
    print(f"  Std: {np.std(topo_features):.4f}")

    # Cross-validation
    n = len(positions)
    fold_size = n // args.folds
    indices = np.arange(n)
    np.random.seed(args.seed)
    np.random.shuffle(indices)

    print(f"\nRunning {args.folds}-fold cross-validation ({args.epochs} epochs)...")
    print("-" * 60)

    base_maes, base_r2s = [], []
    topo_maes, topo_r2s = [], []

    for fold in range(args.folds):
        test_idx = indices[fold * fold_size:(fold + 1) * fold_size]
        train_idx = np.concatenate([indices[:fold * fold_size], indices[(fold + 1) * fold_size:]])

        mae_b, r2_b = train_and_eval(
            X_base_diff[train_idx], y[train_idx],
            X_base_diff[test_idx], y[test_idx], 64, args.epochs)
        mae_t, r2_t = train_and_eval(
            X_topo_diff[train_idx], y[train_idx],
            X_topo_diff[test_idx], y[test_idx], 192, args.epochs)

        base_maes.append(mae_b)
        base_r2s.append(r2_b)
        topo_maes.append(mae_t)
        topo_r2s.append(r2_t)

        delta = (mae_b - mae_t) / mae_b * 100
        print(f"  Fold {fold+1}: Base MAE={mae_b:.3f} R²={r2_b:.3f} | "
              f"Topo MAE={mae_t:.3f} R²={r2_t:.3f} | Δ={delta:+.1f}%")

    # Summary
    print("\n" + "=" * 60)
    print("RESULTS")
    print("=" * 60)

    mae_b = np.mean(base_maes)
    mae_t = np.mean(topo_maes)
    r2_b = np.mean(base_r2s)
    r2_t = np.mean(topo_r2s)
    mae_imp = (mae_b - mae_t) / mae_b * 100
    r2_imp = r2_t - r2_b

    print(f"Baseline (64):  MAE={mae_b:.3f} ± {np.std(base_maes):.3f}  R²={r2_b:.3f} ± {np.std(base_r2s):.3f}")
    print(f"Topo (192):     MAE={mae_t:.3f} ± {np.std(topo_maes):.3f}  R²={r2_t:.3f} ± {np.std(topo_r2s):.3f}")
    print(f"\nMAE improvement: {mae_imp:+.1f}%")
    print(f"R² improvement:  {r2_imp:+.3f}")

    # Statistical test
    try:
        from scipy import stats
        t, p = stats.ttest_rel(base_maes, topo_maes)
        print(f"Paired t-test: t={t:.3f}, p={p:.3f} ({'significant' if p < 0.05 else 'not significant'})")
    except ImportError:
        print("(scipy not available for t-test)")

    # Phase 0 criteria
    print("\n" + "=" * 60)
    print("PHASE 0 CRITERIA")
    print("=" * 60)
    print(f"{'✓' if mae_imp >= 8 else '✗'} MAE improvement {mae_imp:+.1f}% (threshold: 8%)")
    print(f"{'✓' if r2_imp >= 0.02 else '✗'} R² improvement {r2_imp:+.3f} (threshold: 0.02)")

    # Save results
    results = {
        "positions": len(positions),
        "sf_depth": args.sf_depth,
        "baseline": {"mae": float(mae_b), "r2": float(r2_b)},
        "topo": {"mae": float(mae_t), "r2": float(r2_t)},
        "mae_improvement_pct": float(mae_imp),
        "r2_improvement": float(r2_imp),
    }
    (args.out / "results.json").write_text(json.dumps(results, indent=2))
    print(f"\nResults saved to {args.out}/results.json")

    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
