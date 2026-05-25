#!/usr/bin/env python3
"""
Small match harness for Lulzfish strength work.

Uses bulletchess for board legality, FEN, SAN, and draw/checkmate detection,
and talks to UCI engines directly. This keeps the referee fast without relying
on python-chess's engine wrapper.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path

try:
    import bulletchess as bc
except ImportError as exc:
    raise SystemExit(
        "bulletchess is required. Install it in a venv with: "
        "python3 -m pip install bulletchess"
    ) from exc


START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"

PIECE_VALUES_CP = {
    bc.PAWN: 100,
    bc.KNIGHT: 320,
    bc.BISHOP: 330,
    bc.ROOK: 500,
    bc.QUEEN: 900,
}

OPENINGS: list[tuple[str, list[str]]] = [
    ("startpos", []),
    ("open_game", ["e2e4", "e7e5"]),
    ("italian", ["e2e4", "e7e5", "g1f3", "b8c6", "f1c4", "f8c5"]),
    ("ruy_lopez", ["e2e4", "e7e5", "g1f3", "b8c6", "f1b5"]),
    ("scotch", ["e2e4", "e7e5", "g1f3", "b8c6", "d2d4", "e5d4"]),
    ("sicilian", ["e2e4", "c7c5"]),
    ("caro_kann", ["e2e4", "c7c6", "d2d4", "d7d5"]),
    ("queens_pawn", ["d2d4", "d7d5", "c2c4"]),
    ("slav", ["d2d4", "d7d5", "c2c4", "c7c6"]),
    ("french", ["e2e4", "e7e6", "d2d4", "d7d5"]),
    ("scandinavian", ["e2e4", "d7d5", "e4d5", "d8d5"]),
    ("pirc", ["e2e4", "d7d6", "d2d4", "g8f6", "b1c3", "g7g6"]),
    ("alekhine", ["e2e4", "g8f6"]),
    ("kings_indian", ["d2d4", "g8f6", "c2c4", "g7g6", "b1c3", "f8g7", "e2e4", "d7d6"]),
    ("nimzo_indian", ["d2d4", "g8f6", "c2c4", "e7e6", "b1c3", "f8b4"]),
    ("queen_indian", ["d2d4", "g8f6", "c2c4", "e7e6", "g1f3", "b7b6"]),
    ("benoni", ["d2d4", "g8f6", "c2c4", "c7c5", "d4d5", "e7e6"]),
    ("dutch", ["d2d4", "f7f5"]),
    ("english", ["c2c4", "e7e5", "b1c3", "g8f6"]),
    ("reti", ["g1f3", "d7d5", "c2c4"]),
    ("london", ["d2d4", "d7d5", "c1f4", "g8f6", "e2e3"]),
]


class UciEngine:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.process = subprocess.Popen(
            [str(path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._send("uci")
        self._read_until("uciok")

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self._send("quit")
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()

    def configure(self, options: dict[str, int | str]) -> None:
        for name, value in options.items():
            self._send(f"setoption name {name} value {value}")
        self.ready()

    def ready(self) -> None:
        self._send("isready")
        self._read_until("readyok")

    def play(self, board: bc.Board, depth: int, start_fen: str, uci_moves: list[str]) -> bc.Move | None:
        if start_fen == START_FEN:
            position = "position startpos"
        else:
            position = f"position fen {start_fen}"
        if uci_moves:
            position += " moves " + " ".join(uci_moves)

        self._send(position)
        self._send(f"go depth {depth}")

        while True:
            line = self._readline()
            if line.startswith("bestmove"):
                parts = line.split()
                if len(parts) < 2 or parts[1] == "0000":
                    return None
                return bc.Move.from_uci(parts[1])

    def _send(self, command: str) -> None:
        if self.process.stdin is None:
            raise RuntimeError(f"{self.path} stdin is closed")
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

    def _readline(self) -> str:
        if self.process.stdout is None:
            raise RuntimeError(f"{self.path} stdout is closed")
        line = self.process.stdout.readline()
        if line == "":
            stderr = ""
            if self.process.stderr is not None:
                stderr = self.process.stderr.read()
            raise RuntimeError(f"{self.path} exited while waiting for UCI output: {stderr}")
        return line.strip()

    def _read_until(self, token: str) -> None:
        while True:
            if self._readline() == token:
                return


@dataclass
class GameRecord:
    white_name: str
    black_name: str
    opening_name: str
    start_fen: str
    san_moves: list[str]
    uci_moves: list[str]
    result: str
    termination: str
    plies: int
    final_material_cp: int


def board_from_opening(moves: list[str]) -> bc.Board:
    board = bc.Board()
    for move_text in moves:
        move = bc.Move.from_uci(move_text)
        if move not in board.legal_moves():
            raise ValueError(f"built-in opening move is illegal: {move_text} in {board.fen()}")
        board.apply(move)
    return board


def score_for_lulzfish(result: str, lulzfish_white: bool) -> float:
    if result == "1-0":
        return 1.0 if lulzfish_white else 0.0
    if result == "0-1":
        return 0.0 if lulzfish_white else 1.0
    return 0.5


def material_balance_cp(board: bc.Board) -> int:
    white = 0
    black = 0
    for piece_type, value in PIECE_VALUES_CP.items():
        white += len(board[bc.WHITE, piece_type]) * value
        black += len(board[bc.BLACK, piece_type]) * value
    return white - black


def outcome(board: bc.Board, hit_max_plies: bool, material_adjudication: int = 0) -> tuple[str, str]:
    if board in bc.CHECKMATE:
        return ("0-1", "CHECKMATE") if board.turn is bc.WHITE else ("1-0", "CHECKMATE")
    if board in bc.STALEMATE:
        return "1/2-1/2", "STALEMATE"
    if board in bc.THREEFOLD_REPETITION:
        return "1/2-1/2", "THREEFOLD_REPETITION"
    if board in bc.FIVEFOLD_REPETITION:
        return "1/2-1/2", "FIVEFOLD_REPETITION"
    if board in bc.FIFTY_MOVE_TIMEOUT:
        return "1/2-1/2", "FIFTY_MOVE_TIMEOUT"
    if board in bc.FORCED_DRAW or board in bc.DRAW:
        return "1/2-1/2", "DRAW"
    if hit_max_plies:
        if material_adjudication > 0:
            material = material_balance_cp(board)
            if material >= material_adjudication:
                return "1-0", f"MATERIAL_ADJUDICATION+{material}"
            if material <= -material_adjudication:
                return "0-1", f"MATERIAL_ADJUDICATION{material}"
        return "1/2-1/2", "MAX_PLIES"
    return "*", "UNKNOWN"


def play_game(
    *,
    board: bc.Board,
    white: UciEngine,
    black: UciEngine,
    white_name: str,
    black_name: str,
    opening_name: str,
    white_depth: int,
    black_depth: int,
    max_plies: int,
    material_adjudication: int,
    initial_uci_moves: list[str],
) -> tuple[str, str, int, GameRecord]:
    start_fen = board.fen()
    uci_moves = list(initial_uci_moves)
    san_moves: list[str] = []
    plies = 0

    while plies < max_plies and outcome(board, hit_max_plies=False)[0] == "*":
        engine = white if board.turn is bc.WHITE else black
        depth = white_depth if board.turn is bc.WHITE else black_depth
        move = engine.play(board, depth, START_FEN, uci_moves)
        if move is None:
            break
        if move not in board.legal_moves():
            raise RuntimeError(f"{engine.path} returned illegal move {move.uci()} in {board.fen()}")
        san_moves.append(move.san(board))
        uci_moves.append(move.uci())
        board.apply(move)
        plies += 1

    result_text, termination = outcome(
        board,
        hit_max_plies=(plies >= max_plies),
        material_adjudication=material_adjudication,
    )
    record = GameRecord(
        white_name=white_name,
        black_name=black_name,
        opening_name=opening_name,
        start_fen=start_fen,
        san_moves=san_moves,
        uci_moves=uci_moves,
        result=result_text,
        termination=termination,
        plies=plies,
        final_material_cp=material_balance_cp(board),
    )
    return result_text, termination, plies, record


def format_movetext(record: GameRecord) -> str:
    board = bc.Board.from_fen(record.start_fen)
    move_no = board.fullmove_number
    white_to_move = board.turn is bc.WHITE
    parts: list[str] = []

    for san in record.san_moves:
        if white_to_move:
            parts.append(f"{move_no}. {san}")
        else:
            if not parts or parts[-1].endswith("..."):
                parts.append(f"{move_no}... {san}")
            else:
                parts[-1] = f"{parts[-1]} {san}"
            move_no += 1
        white_to_move = not white_to_move

    parts.append(record.result)
    return " ".join(parts)


def append_pgn(path: Path, record: GameRecord) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    headers = {
        "Event": "?",
        "Site": "?",
        "Date": "????.??.??",
        "Round": "?",
        "White": record.white_name,
        "Black": record.black_name,
        "Result": record.result,
        "Opening": record.opening_name,
        "Termination": record.termination,
        "PlyCount": str(record.plies),
        "MaterialBalanceCp": str(record.final_material_cp),
    }
    if record.start_fen != START_FEN:
        headers["SetUp"] = "1"
        headers["FEN"] = record.start_fen

    with path.open("a", encoding="utf-8") as out:
        for key, value in headers.items():
            out.write(f'[{key} "{value}"]\n')
        out.write("\n")
        out.write(format_movetext(record))
        out.write("\n\n")


def run_stockfish(args: argparse.Namespace) -> int:
    openings = OPENINGS[: max(1, args.openings)]
    total = 0.0
    started = time.time()

    for game_no in range(args.games):
        opening_name, opening_moves = openings[game_no % len(openings)]
        lulzfish_white = game_no % 2 == 0
        board = board_from_opening(opening_moves)

        white_path = args.engine if lulzfish_white else args.stockfish
        black_path = args.stockfish if lulzfish_white else args.engine
        white_name = "Lulzfish" if lulzfish_white else "Stockfish"
        black_name = "Stockfish" if lulzfish_white else "Lulzfish"
        white_depth = args.depth if lulzfish_white else args.stockfish_depth
        black_depth = args.stockfish_depth if lulzfish_white else args.depth

        white = UciEngine(white_path)
        black = UciEngine(black_path)
        try:
            if lulzfish_white:
                black.configure({"Threads": 1, "Hash": 16})
            else:
                white.configure({"Threads": 1, "Hash": 16})

            result_text, termination, plies, record = play_game(
                board=board,
                white=white,
                black=black,
                white_name=white_name,
                black_name=black_name,
                opening_name=opening_name,
                white_depth=white_depth,
                black_depth=black_depth,
                max_plies=args.max_plies,
                material_adjudication=args.material_adjudication,
                initial_uci_moves=opening_moves,
            )
        finally:
            white.close()
            black.close()

        score = score_for_lulzfish(result_text, lulzfish_white)
        total += score
        color = "White" if lulzfish_white else "Black"
        lulzfish_material = record.final_material_cp if lulzfish_white else -record.final_material_cp
        print(
            f"Game {game_no + 1:02d} {opening_name:11s}: "
            f"Lulzfish {color:5s} result {result_text:7s} "
            f"score {score:.1f} plies {plies:3d} "
            f"mat {lulzfish_material:+5d} {termination}",
            flush=True,
        )
        if args.pgn:
            append_pgn(args.pgn, record)

    print(
        f"Lulzfish total: {total:.1f}/{args.games} "
        f"at depth {args.depth} vs Stockfish depth {args.stockfish_depth} "
        f"({time.time() - started:.1f}s)",
        flush=True,
    )
    return 0


def run_selfplay(args: argparse.Namespace) -> int:
    openings = OPENINGS[: max(1, args.openings)]
    started = time.time()

    for game_no in range(args.games):
        opening_name, opening_moves = openings[game_no % len(openings)]
        board = board_from_opening(opening_moves)

        white = UciEngine(args.engine)
        black = UciEngine(args.engine)
        try:
            result_text, termination, plies, record = play_game(
                board=board,
                white=white,
                black=black,
                white_name="Lulzfish",
                black_name="Lulzfish",
                opening_name=opening_name,
                white_depth=args.depth,
                black_depth=args.depth,
                max_plies=args.max_plies,
                material_adjudication=args.material_adjudication,
                initial_uci_moves=opening_moves,
            )
        finally:
            white.close()
            black.close()

        print(
            f"Game {game_no + 1:02d} {opening_name:11s}: "
            f"result {result_text:7s} plies {plies:3d} "
            f"mat {record.final_material_cp:+5d} {termination}",
            flush=True,
        )
        if args.pgn:
            append_pgn(args.pgn, record)

    print(
        f"Self-play complete: {args.games} games at depth {args.depth} ({time.time() - started:.1f}s)",
        flush=True,
    )
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Lulzfish self-play or Stockfish smoke matches.")
    parser.add_argument("--mode", choices=("selfplay", "stockfish"), default="stockfish")
    parser.add_argument("--engine", type=Path, default=Path("./build/lulzfish"))
    parser.add_argument("--stockfish", type=Path, default=Path("/opt/homebrew/bin/stockfish"))
    parser.add_argument("--games", type=int, default=10)
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--stockfish-depth", type=int, default=2)
    parser.add_argument("--max-plies", type=int, default=120)
    parser.add_argument(
        "--material-adjudication",
        type=int,
        default=0,
        help="centipawn material threshold for adjudicating games that hit --max-plies; 0 disables it",
    )
    parser.add_argument("--openings", type=int, default=len(OPENINGS), help="number of built-in openings to cycle")
    parser.add_argument("--pgn", type=Path, help="optional PGN output path")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if args.mode == "selfplay":
        return run_selfplay(args)
    return run_stockfish(args)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
