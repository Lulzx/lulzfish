#!/usr/bin/env python3
"""Run a Stockfish skill-level ladder and emit JSON for the website."""

from __future__ import annotations

import argparse
import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

try:
    import bulletchess as bc
except ImportError as exc:
    raise SystemExit("bulletchess is required. Install with: python3 -m pip install bulletchess") from exc

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lulzfish_match import (  # noqa: E402
    OPENINGS,
    START_FEN,
    UciEngine,
    board_from_opening,
    material_balance_cp,
    outcome,
    score_for_lulzfish,
)


def engine_move(engine: UciEngine, start_fen: str, uci_moves: list[str], command: str) -> bc.Move | None:
    if start_fen == START_FEN:
        position = "position startpos"
    else:
        position = f"position fen {start_fen}"
    if uci_moves:
        position += " moves " + " ".join(uci_moves)

    engine._send(position)
    engine._send(command)

    while True:
        line = engine._readline()
        if line.startswith("bestmove"):
            parts = line.split()
            if len(parts) < 2 or parts[1] == "0000":
                return None
            return bc.Move.from_uci(parts[1])


def play_game(
    *,
    lulzfish_path: Path,
    stockfish_path: Path,
    stockfish_level: int,
    game_no: int,
    lulzfish_depth: int,
    lulzfish_threads: int,
    stockfish_movetime_ms: int,
    max_plies: int,
    material_adjudication: int,
) -> dict[str, object]:
    opening_name, opening_moves = OPENINGS[game_no % len(OPENINGS)]
    lulzfish_white = game_no % 2 == 0
    board = board_from_opening(opening_moves)
    start_fen = board.fen()
    uci_moves = list(opening_moves)
    san_moves: list[str] = []
    plies = 0

    lulzfish = UciEngine(lulzfish_path)
    stockfish = UciEngine(stockfish_path)
    lulzfish.configure({"Threads": lulzfish_threads})
    stockfish.configure({
        "Threads": 1,
        "Hash": 16,
        "Skill Level": stockfish_level,
    })

    try:
        while plies < max_plies and outcome(board, hit_max_plies=False)[0] == "*":
            lulzfish_to_move = (board.turn is bc.WHITE and lulzfish_white) or (board.turn is bc.BLACK and not lulzfish_white)
            if lulzfish_to_move:
                move = lulzfish.play(board, lulzfish_depth, START_FEN, uci_moves)
            else:
                move = engine_move(stockfish, START_FEN, uci_moves, f"go movetime {stockfish_movetime_ms}")

            if move is None:
                break
            if move not in board.legal_moves():
                side = "Lulzfish" if lulzfish_to_move else f"Stockfish Skill {stockfish_level}"
                raise RuntimeError(f"{side} returned illegal move {move.uci()} in {board.fen()}")

            san_moves.append(move.san(board))
            uci_moves.append(move.uci())
            board.apply(move)
            plies += 1
    finally:
        lulzfish.close()
        stockfish.close()

    result, termination = outcome(
        board,
        hit_max_plies=(plies >= max_plies),
        material_adjudication=material_adjudication,
    )
    score = score_for_lulzfish(result, lulzfish_white)
    material = material_balance_cp(board)
    lulzfish_material = material if lulzfish_white else -material

    return {
        "game": game_no + 1,
        "stockfishLevel": stockfish_level,
        "opening": opening_name,
        "lulzfishColor": "white" if lulzfish_white else "black",
        "white": "Lulzfish" if lulzfish_white else f"Stockfish Skill {stockfish_level}",
        "black": f"Stockfish Skill {stockfish_level}" if lulzfish_white else "Lulzfish",
        "result": result,
        "lulzfishScore": score,
        "termination": termination,
        "plies": plies,
        "materialCpForLulzfish": lulzfish_material,
        "moves": uci_moves,
        "san": san_moves,
        "finalFen": board.fen(),
    }


def summarize_level(level: int, games: list[dict[str, object]]) -> dict[str, object]:
    score = sum(float(game["lulzfishScore"]) for game in games)
    wins = sum(1 for game in games if game["lulzfishScore"] == 1.0)
    draws = sum(1 for game in games if game["lulzfishScore"] == 0.5)
    losses = sum(1 for game in games if game["lulzfishScore"] == 0.0)
    avg_plies = sum(int(game["plies"]) for game in games) / len(games)
    avg_material = sum(int(game["materialCpForLulzfish"]) for game in games) / len(games)
    return {
        "level": level,
        "games": len(games),
        "score": score,
        "scoreText": f"{score:g}/{len(games)}",
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "avgPlies": round(avg_plies, 1),
        "avgMaterialCp": round(avg_material),
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Lulzfish through Stockfish skill levels.")
    parser.add_argument("--engine", type=Path, default=Path("./build/lulzfish"))
    parser.add_argument("--stockfish", type=Path, default=Path("/opt/homebrew/bin/stockfish"))
    parser.add_argument("--start-level", type=int, default=0)
    parser.add_argument("--max-level", type=int, default=20)
    parser.add_argument("--games-per-level", type=int, default=10)
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--threads", type=int, default=1)
    parser.add_argument("--stockfish-movetime-ms", type=int, default=50)
    parser.add_argument("--max-plies", type=int, default=120)
    parser.add_argument("--material-adjudication", type=int, default=500)
    parser.add_argument("--out", type=Path, default=Path("web/wasm/stockfish/data.json"))
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    started = time.time()
    levels: list[dict[str, object]] = []
    games: list[dict[str, object]] = []
    stop_reason = "Reached maximum Stockfish skill level."

    for level in range(args.start_level, args.max_level + 1):
        level_games: list[dict[str, object]] = []
        for game_no in range(args.games_per_level):
            game = play_game(
                lulzfish_path=args.engine,
                stockfish_path=args.stockfish,
                stockfish_level=level,
                game_no=game_no,
                lulzfish_depth=args.depth,
                lulzfish_threads=args.threads,
                stockfish_movetime_ms=args.stockfish_movetime_ms,
                max_plies=args.max_plies,
                material_adjudication=args.material_adjudication,
            )
            level_games.append(game)
            games.append(game)
            print(
                f"Stockfish skill {level:02d} game {game_no + 1:02d}: "
                f"{game['result']:7s} score {game['lulzfishScore']} "
                f"plies {game['plies']:3d} mat {game['materialCpForLulzfish']:+5d} {game['termination']}",
                flush=True,
            )

        summary = summarize_level(level, level_games)
        levels.append(summary)
        if summary["losses"] == args.games_per_level:
            stop_reason = f"Stopped at Stockfish Skill Level {level}: Lulzfish lost all {args.games_per_level} games."
            break

    total_score = sum(float(game["lulzfishScore"]) for game in games)
    payload = {
        "generatedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "engine": "Lulzfish",
        "enginePath": str(args.engine),
        "stockfishPath": str(args.stockfish),
        "stockfishVersion": "Stockfish 18",
        "lulzfishDepth": args.depth,
        "lulzfishThreads": args.threads,
        "stockfishMovetimeMs": args.stockfish_movetime_ms,
        "gamesPerLevel": args.games_per_level,
        "startLevel": args.start_level,
        "maxRequestedLevel": args.max_level,
        "maxPlies": args.max_plies,
        "materialAdjudicationCp": args.material_adjudication,
        "stopReason": stop_reason,
        "totalGames": len(games),
        "totalScore": total_score,
        "totalScoreText": f"{total_score:g}/{len(games)}",
        "elapsedSeconds": round(time.time() - started, 1),
        "levels": levels,
        "games": games,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.out} with {len(games)} games across {len(levels)} levels")
    print(stop_reason)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
