#!/usr/bin/env python3
"""Run Lulzfish through PufferLib vectorization with random legal actions."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

from puffer_lulzfish_env import ACTION_SIZE, FEATURE_SIZE, make_puffer_env


def legal_random_actions(obs: np.ndarray, rng: np.random.Generator) -> np.ndarray:
    masks = obs[:, FEATURE_SIZE : FEATURE_SIZE + ACTION_SIZE]
    actions = np.zeros(obs.shape[0], dtype=np.int32)
    for i, mask in enumerate(masks):
        legal = np.flatnonzero(mask > 0.5)
        if legal.size == 0:
            actions[i] = 0
        else:
            actions[i] = int(rng.choice(legal))
    return actions


def run(args: argparse.Namespace) -> int:
    try:
        import pufferlib.vector
    except ImportError as exc:
        raise SystemExit(
            "PufferLib is not importable. Create a Python 3.10/3.11 venv and run:\n"
            "  python -m pip install -r tools/rl/requirements-puffer.txt"
        ) from exc

    rng = np.random.default_rng(args.seed)
    env_kwargs = {
        "engine_path": args.engine,
        "opponent": args.opponent,
        "opponent_depth": args.opponent_depth,
        "max_plies": args.max_plies,
    }

    vec = pufferlib.vector.make(
        make_puffer_env,
        backend=pufferlib.vector.Serial,
        num_envs=args.num_envs,
        env_kwargs=env_kwargs,
        seed=args.seed,
    )

    started = time.time()
    obs, infos = vec.reset(seed=args.seed)
    total_reward = np.zeros(args.num_envs, dtype=np.float32)
    terminals = np.zeros(args.num_envs, dtype=bool)

    try:
        for _ in range(args.steps):
            actions = legal_random_actions(obs, rng)
            obs, rewards, terminals, truncations, infos = vec.step(actions)
            total_reward += rewards
    finally:
        vec.close()

    elapsed = max(time.time() - started, 1e-9)
    print(
        f"PufferLib smoke: envs={args.num_envs} steps={args.steps} "
        f"sps={args.num_envs * args.steps / elapsed:.1f} "
        f"terminals={int(terminals.sum())} reward_sum={float(total_reward.sum()):.2f}"
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path("./build/lulzfish"))
    parser.add_argument("--num-envs", type=int, default=4)
    parser.add_argument("--steps", type=int, default=128)
    parser.add_argument("--opponent", choices=("random", "engine", "none"), default="random")
    parser.add_argument("--opponent-depth", type=int, default=1)
    parser.add_argument("--max-plies", type=int, default=200)
    parser.add_argument("--seed", type=int, default=1)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
