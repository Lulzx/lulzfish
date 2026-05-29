#!/usr/bin/env python3
"""Generate JSONL labels for graph-eval and search-controller experiments."""

from __future__ import annotations

import argparse
import json
import random
import sys
from pathlib import Path

import bulletchess as bc

from lulzfish_env import UciEngine, game_result, material_balance_cp


OPENINGS: list[list[str]] = [
    [],
    ["e2e4", "e7e5"],
    ["e2e4", "c7c5"],
    ["d2d4", "d7d5", "c2c4"],
    ["d2d4", "g8f6", "c2c4", "e7e6"],
    ["c2c4", "e7e5"],
    ["g1f3", "d7d5", "c2c4"],
]


def board_from_opening(moves: list[str]) -> bc.Board:
    board = bc.Board()
    for move_text in moves:
        move = bc.Move.from_uci(move_text)
        if move not in board.legal_moves():
            raise ValueError(f"illegal opening move {move_text} in {board.fen()}")
        board.apply(move)
    return board


def generate(args: argparse.Namespace) -> int:
    rng = random.Random(args.seed)
    engine = UciEngine(args.engine)
    out_path = args.out
    out_path.parent.mkdir(parents=True, exist_ok=True)
    positions = 0

    try:
        with out_path.open("w", encoding="utf-8") as out:
            for game in range(args.games):
                opening = OPENINGS[game % len(OPENINGS)]
                board = board_from_opening(opening)
                plies = len(opening)

                while plies < args.max_plies and game_result(board)[0] is None:
                    fen = board.fen()
                    legal_moves = [move.uci() for move in board.legal_moves()]
                    if not legal_moves:
                        break

                    shallow = engine.analyse_fen(fen, args.shallow_depth)
                    target = engine.analyse_fen(fen, args.target_depth)
                    chosen = target.bestmove if target.bestmove in legal_moves else rng.choice(legal_moves)

                    record = {
                        "fen": fen,
                        "ply": plies,
                        "legal_moves": legal_moves,
                        "side_to_move": "white" if board.turn is bc.WHITE else "black",
                        "material_cp": material_balance_cp(board),
                        "shallow_depth": args.shallow_depth,
                        "shallow_bestmove": shallow.bestmove,
                        "shallow_score_cp": shallow.score_cp,
                        "target_depth": args.target_depth,
                        "target_bestmove": target.bestmove,
                        "target_score_cp": target.score_cp,
                        "target_pv": list(target.pv),
                    }
                    out.write(json.dumps(record, separators=(",", ":")) + "\n")
                    positions += 1

                    board.apply(bc.Move.from_uci(chosen))
                    plies += 1

                print(f"game {game + 1:03d}: collected {positions} total positions", flush=True)
    finally:
        engine.close()

    print(f"wrote {positions} positions to {out_path}")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--engine", type=Path, default=Path("./build/lulzfish"))
    parser.add_argument("--out", type=Path, default=Path("data/lulzfish_training.jsonl"))
    parser.add_argument("--games", type=int, default=8)
    parser.add_argument("--max-plies", type=int, default=80)
    parser.add_argument("--shallow-depth", type=int, default=1)
    parser.add_argument("--target-depth", type=int, default=3)
    parser.add_argument("--seed", type=int, default=1)
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    return generate(parse_args(argv))


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))

