#!/usr/bin/env python3
"""Run Lulzfish against deterministic micro-engine baselines and emit site JSON."""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Callable

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


PIECE_VALUES = {
    bc.PAWN: 100,
    bc.KNIGHT: 320,
    bc.BISHOP: 330,
    bc.ROOK: 500,
    bc.QUEEN: 900,
    bc.KING: 0,
}

CENTER_SQUARES = {bc.Square.from_str(sq) for sq in ("d4", "e4", "d5", "e5")}
RING_SQUARES = {bc.Square.from_str(sq) for sq in ("c3", "d3", "e3", "f3", "c4", "f4", "c5", "f5", "c6", "d6", "e6", "f6")}


@dataclass(frozen=True)
class MicroEngine:
    name: str
    style: str
    description: str
    chooser: Callable[[bc.Board, int], bc.Move]


def square_index(square: bc.Square) -> int:
    return int(square.index())


def square_file(square: bc.Square) -> int:
    return square_index(square) % 8


def square_rank(square: bc.Square) -> int:
    return square_index(square) // 8


def pst_bonus(square: bc.Square, color: bc.Color, piece_type: bc.PieceType) -> int:
    file = square_file(square)
    rank = square_rank(square) if color is bc.WHITE else 7 - square_rank(square)
    center_file = 3.5 - abs(file - 3.5)
    center_rank = 3.5 - abs(rank - 3.5)

    if piece_type is bc.PAWN:
        return int(rank * 8 + center_file * 3)
    if piece_type in (bc.KNIGHT, bc.BISHOP):
        return int((center_file + center_rank) * 9)
    if piece_type is bc.ROOK:
        return int(rank * 3)
    if piece_type is bc.QUEEN:
        return int((center_file + center_rank) * 3)
    return 0


def board_eval(board: bc.Board, color, *, pst: bool, mobility: bool, center: bool, king: bool) -> int:
    score = 0
    for side, sign in ((bc.WHITE, 1), (bc.BLACK, -1)):
        for piece_type, value in PIECE_VALUES.items():
            for square in board[side, piece_type]:
                term = value
                if pst:
                    term += pst_bonus(square, side, piece_type)
                if center and square in CENTER_SQUARES:
                    term += 32
                elif center and square in RING_SQUARES:
                    term += 12
                score += sign * term

        if king:
            score += sign * pawn_shield(board, side)

    if mobility:
        turn = board.turn
        board.turn = bc.WHITE
        white_mobility = len(board.legal_moves())
        board.turn = bc.BLACK
        black_mobility = len(board.legal_moves())
        board.turn = turn
        score += 3 * (white_mobility - black_mobility)

    return score if color is bc.WHITE else -score


def pawn_shield(board: bc.Board, color) -> int:
    king_squares = list(board[color, bc.KING])
    if not king_squares:
        return 0
    king = king_squares[0]
    kf = square_file(king)
    kr = square_rank(king)
    shield_rank = kr + 1 if color is bc.WHITE else kr - 1
    if shield_rank < 0 or shield_rank > 7:
        return 0
    score = 0
    pawns = board[color, bc.PAWN]
    for file in range(max(0, kf - 1), min(7, kf + 1) + 1):
        square = bc.Square.from_str(f"{chr(97 + file)}{shield_rank + 1}")
        if pawns & square.bb():
            score += 18
    return score


def move_eval(board: bc.Board, move: bc.Move, color, **kwargs: bool) -> int:
    next_board = board.copy()
    next_board.apply(move)
    score = board_eval(next_board, color, **kwargs)
    if move.is_capture:
        score += 8
    if move.is_promotion:
        score += 35
    return score


def stable_pick(moves: list[bc.Move], scores: list[int], seed: int) -> bc.Move:
    best = max(scores)
    candidates = [move for move, score in zip(moves, scores) if score == best]
    return sorted(candidates, key=lambda move: move.uci())[seed % len(candidates)]


def random_engine(board: bc.Board, seed: int) -> bc.Move:
    moves = sorted(board.legal_moves(), key=lambda move: move.uci())
    return moves[random.Random(seed + len(board.history)).randrange(len(moves))]


def greedy_captures(board: bc.Board, seed: int) -> bc.Move:
    moves = list(board.legal_moves())
    scores = [
        (120 if move.is_capture else 0)
        + (80 if move.is_promotion else 0)
        + pst_bonus(move.destination, board.turn, bc.QUEEN) // 2
        for move in moves
    ]
    return stable_pick(moves, scores, seed)


def one_ply(**kwargs: bool) -> Callable[[bc.Board, int], bc.Move]:
    def choose(board: bc.Board, seed: int) -> bc.Move:
        moves = list(board.legal_moves())
        scores = [move_eval(board, move, board.turn, **kwargs) for move in moves]
        return stable_pick(moves, scores, seed)

    return choose


def minimax(depth: int, **kwargs: bool) -> Callable[[bc.Board, int], bc.Move]:
    def search(board: bc.Board, root: bc.Color, remaining: int) -> int:
        result, _termination = outcome(board, hit_max_plies=False)
        if result != "*":
            if result == "1/2-1/2":
                return 0
            won = (result == "1-0" and root is bc.WHITE) or (result == "0-1" and root is bc.BLACK)
            return 100000 if won else -100000
        if remaining == 0:
            return board_eval(board, root, **kwargs)

        scores: list[int] = []
        for move in board.legal_moves():
            child = board.copy()
            child.apply(move)
            scores.append(search(child, root, remaining - 1))
        return max(scores) if board.turn is root else min(scores)

    def choose(board: bc.Board, seed: int) -> bc.Move:
        moves = list(board.legal_moves())
        scores = []
        for move in moves:
            child = board.copy()
            child.apply(move)
            scores.append(search(child, board.turn, depth - 1))
        return stable_pick(moves, scores, seed)

    return choose


MICRO_ENGINES = [
    MicroEngine("Random-01", "random", "Uniform random legal moves with a stable seed.", random_engine),
    MicroEngine("Capture-01", "tactical", "Takes material whenever possible, otherwise drifts centerward.", greedy_captures),
    MicroEngine("Material-01", "material", "One-ply material-only evaluator.", one_ply(pst=False, mobility=False, center=False, king=False)),
    MicroEngine("Space-01", "space", "Material plus pawn advancement and light piece-square pressure.", one_ply(pst=True, mobility=False, center=False, king=False)),
    MicroEngine("Center-01", "center", "Material plus explicit central-square occupation.", one_ply(pst=True, mobility=False, center=True, king=False)),
    MicroEngine("Mobility-01", "mobility", "Material, PST, and legal-move count.", one_ply(pst=True, mobility=True, center=False, king=False)),
    MicroEngine("Safety-01", "king safety", "Material, PST, center, and pawn shield around the king.", one_ply(pst=True, mobility=False, center=True, king=True)),
    MicroEngine("Tactical-02", "depth 2", "Two-ply material/PST minimax.", minimax(2, pst=True, mobility=False, center=False, king=False)),
    MicroEngine("Scout-02", "depth 2", "Two-ply minimax with center and mobility terms.", minimax(2, pst=True, mobility=True, center=True, king=False)),
    MicroEngine("Composite-02", "depth 2", "The strongest micro baseline: PST, center, mobility, and king safety.", minimax(2, pst=True, mobility=True, center=True, king=True)),
]


def play_game(
    *,
    lulzfish_path: Path,
    opponent: MicroEngine,
    game_no: int,
    opening_name: str,
    opening_moves: list[str],
    lulzfish_white: bool,
    lulzfish_depth: int,
    max_plies: int,
    material_adjudication: int,
) -> dict[str, object]:
    board = board_from_opening(opening_moves)
    uci_moves = list(opening_moves)
    san_moves: list[str] = []
    plies = 0
    engine = UciEngine(lulzfish_path)

    try:
        while plies < max_plies and outcome(board, hit_max_plies=False)[0] == "*":
            lulzfish_to_move = (board.turn is bc.WHITE and lulzfish_white) or (board.turn is bc.BLACK and not lulzfish_white)
            if lulzfish_to_move:
                move = engine.play(board, lulzfish_depth, START_FEN, uci_moves)
                if move is None:
                    break
            else:
                move = opponent.chooser(board, seed=(game_no * 257 + plies * 17))

            if move not in board.legal_moves():
                side = "Lulzfish" if lulzfish_to_move else opponent.name
                raise RuntimeError(f"{side} returned illegal move {move.uci()} in {board.fen()}")

            san_moves.append(move.san(board))
            uci_moves.append(move.uci())
            board.apply(move)
            plies += 1
    finally:
        engine.close()

    result, termination = outcome(
        board,
        hit_max_plies=(plies >= max_plies),
        material_adjudication=material_adjudication,
    )
    lulzfish_score = score_for_lulzfish(result, lulzfish_white)
    material = material_balance_cp(board)
    lulzfish_material = material if lulzfish_white else -material

    return {
        "game": game_no + 1,
        "opponent": opponent.name,
        "opening": opening_name,
        "lulzfishColor": "white" if lulzfish_white else "black",
        "white": "Lulzfish" if lulzfish_white else opponent.name,
        "black": opponent.name if lulzfish_white else "Lulzfish",
        "result": result,
        "lulzfishScore": lulzfish_score,
        "termination": termination,
        "plies": plies,
        "materialCpForLulzfish": lulzfish_material,
        "moves": uci_moves,
        "san": san_moves,
        "finalFen": board.fen(),
    }


def aggregate(opponent: MicroEngine, games: list[dict[str, object]]) -> dict[str, object]:
    score = sum(float(game["lulzfishScore"]) for game in games)
    wins = sum(1 for game in games if game["lulzfishScore"] == 1.0)
    draws = sum(1 for game in games if game["lulzfishScore"] == 0.5)
    losses = sum(1 for game in games if game["lulzfishScore"] == 0.0)
    avg_plies = sum(int(game["plies"]) for game in games) / len(games)
    avg_material = sum(int(game["materialCpForLulzfish"]) for game in games) / len(games)
    expected = score / len(games)
    elo = None
    if 0 < expected < 1:
        elo = round(-400 * math.log10(1 / expected - 1))
    return {
        "name": opponent.name,
        "style": opponent.style,
        "description": opponent.description,
        "games": len(games),
        "score": score,
        "scoreText": f"{score:g}/{len(games)}",
        "wins": wins,
        "draws": draws,
        "losses": losses,
        "expectedScore": expected,
        "estimatedEloDiff": elo,
        "avgPlies": round(avg_plies, 1),
        "avgMaterialCp": round(avg_material),
    }


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run 10 micro-engine benchmark matches for the website.")
    parser.add_argument("--engine", type=Path, default=Path("./build/lulzfish"))
    parser.add_argument("--games-per-engine", type=int, default=10)
    parser.add_argument("--depth", type=int, default=1)
    parser.add_argument("--max-plies", type=int, default=80)
    parser.add_argument("--material-adjudication", type=int, default=300)
    parser.add_argument("--out", type=Path, default=Path("web/wasm/benchmarks/data.json"))
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    started = time.time()
    all_games: list[dict[str, object]] = []
    summaries: list[dict[str, object]] = []

    for opponent_index, opponent in enumerate(MICRO_ENGINES):
        games: list[dict[str, object]] = []
        for game_index in range(args.games_per_engine):
            opening_name, opening_moves = OPENINGS[(opponent_index * args.games_per_engine + game_index) % len(OPENINGS)]
            lulzfish_white = game_index % 2 == 0
            game = play_game(
                lulzfish_path=args.engine,
                opponent=opponent,
                game_no=game_index,
                opening_name=opening_name,
                opening_moves=opening_moves,
                lulzfish_white=lulzfish_white,
                lulzfish_depth=args.depth,
                max_plies=args.max_plies,
                material_adjudication=args.material_adjudication,
            )
            games.append(game)
            all_games.append(game)
            print(
                f"{opponent.name:16s} game {game_index + 1:02d}: "
                f"{game['result']:7s} score {game['lulzfishScore']} "
                f"plies {game['plies']:3d} mat {game['materialCpForLulzfish']:+5d} {game['termination']}",
                flush=True,
            )
        summaries.append(aggregate(opponent, games))

    total_score = sum(float(game["lulzfishScore"]) for game in all_games)
    payload = {
        "generatedAt": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "engine": "Lulzfish",
        "enginePath": str(args.engine),
        "engineDepth": args.depth,
        "gamesPerOpponent": args.games_per_engine,
        "maxPlies": args.max_plies,
        "materialAdjudicationCp": args.material_adjudication,
        "totalGames": len(all_games),
        "totalScore": total_score,
        "totalScoreText": f"{total_score:g}/{len(all_games)}",
        "elapsedSeconds": round(time.time() - started, 1),
        "opponents": summaries,
        "games": all_games,
    }

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote {args.out} with {len(all_games)} games")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
