#!/usr/bin/env python3
"""Native PufferLib environment using python-chess.

The rest of Lulzfish's local tooling uses bulletchess for speed. PufferLib's
current PyPI dependency stack is easier to run on Python 3.10/3.11, where
bulletchess wheels are not available, so this native Puffer environment uses
python-chess for portability.
"""

from __future__ import annotations

import random
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import chess
import numpy as np
import pufferlib
import gymnasium.spaces as spaces


ACTION_SIZE = 64 * 64 * 5
FEATURE_SIZE = 12 * 64 + 64 + 64 + 32
PUFFER_OBS_SIZE = FEATURE_SIZE + ACTION_SIZE
PROMOTION_CODES = {None: 0, chess.KNIGHT: 1, chess.BISHOP: 2, chess.ROOK: 3, chess.QUEEN: 4}
PROMOTION_SUFFIXES = {0: "", 1: "n", 2: "b", 3: "r", 4: "q"}
PIECE_TYPES = [chess.PAWN, chess.KNIGHT, chess.BISHOP, chess.ROOK, chess.QUEEN, chess.KING]
PIECE_VALUES_CP = {
    chess.PAWN: 100,
    chess.KNIGHT: 320,
    chess.BISHOP: 330,
    chess.ROOK: 500,
    chess.QUEEN: 900,
    chess.KING: 0,
}


@dataclass(frozen=True)
class EngineResult:
    bestmove: str
    score_cp: int


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
        self.ready()

    def close(self) -> None:
        if self.process.poll() is None:
            try:
                self._send("quit")
                self.process.wait(timeout=2)
            except (BrokenPipeError, subprocess.TimeoutExpired):
                self.process.kill()

    def ready(self) -> None:
        self._send("isready")
        self._read_until("readyok")

    def analyse_fen(self, fen: str, depth: int) -> EngineResult:
        self._send(f"position fen {fen}")
        self._send(f"go depth {depth}")
        score_cp = 0
        while True:
            line = self._readline()
            if line.startswith("info "):
                parts = line.split()
                if "score" in parts:
                    idx = parts.index("score")
                    if idx + 2 < len(parts):
                        if parts[idx + 1] == "cp":
                            score_cp = int(parts[idx + 2])
                        elif parts[idx + 1] == "mate":
                            score_cp = 30000 if int(parts[idx + 2]) > 0 else -30000
            elif line.startswith("bestmove"):
                parts = line.split()
                return EngineResult(parts[1] if len(parts) > 1 else "0000", score_cp)

    def bestmove_fen(self, fen: str, depth: int) -> str:
        return self.analyse_fen(fen, depth).bestmove

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
            stderr = self.process.stderr.read() if self.process.stderr is not None else ""
            raise RuntimeError(f"{self.path} exited while waiting for UCI output: {stderr}")
        return line.strip()

    def _read_until(self, token: str) -> None:
        while True:
            if self._readline() == token:
                return


def square_name(index: int) -> str:
    return chr(ord("a") + (index & 7)) + chr(ord("1") + (index >> 3))


def action_from_move(move: chess.Move) -> int:
    promotion = PROMOTION_CODES.get(move.promotion, 0)
    return ((move.from_square * 64) + move.to_square) * 5 + promotion


def move_from_action(action: int) -> chess.Move:
    promotion_code = action % 5
    packed = action // 5
    to_square = packed % 64
    from_square = packed // 64
    return chess.Move.from_uci(square_name(from_square) + square_name(to_square) + PROMOTION_SUFFIXES[promotion_code])


def legal_action_mask(board: chess.Board) -> np.ndarray:
    mask = np.zeros(ACTION_SIZE, dtype=np.float32)
    for move in board.legal_moves:
        mask[action_from_move(move)] = 1.0
    return mask


def material_balance_cp(board: chess.Board) -> int:
    score = 0
    for piece_type, value in PIECE_VALUES_CP.items():
        score += len(board.pieces(piece_type, chess.WHITE)) * value
        score -= len(board.pieces(piece_type, chess.BLACK)) * value
    return score


def terminal_reward(board: chess.Board, agent_color: chess.Color, hit_max_plies: bool) -> tuple[float | None, str]:
    outcome = board.outcome(claim_draw=True)
    if outcome is not None:
        if outcome.winner is None:
            return 0.0, outcome.termination.name.lower()
        reward = 1.0 if outcome.winner == agent_color else -1.0
        return reward, outcome.termination.name.lower()
    if hit_max_plies:
        material = material_balance_cp(board)
        if material > 200:
            return 0.25 if agent_color == chess.WHITE else -0.25, "max_plies_material"
        if material < -200:
            return -0.25 if agent_color == chess.WHITE else 0.25, "max_plies_material"
        return 0.0, "max_plies"
    return None, "ongoing"


def extract_features(board: chess.Board, ply: int, max_plies: int) -> np.ndarray:
    out = np.zeros(FEATURE_SIZE, dtype=np.float32)
    for color_index, color in enumerate([chess.WHITE, chess.BLACK]):
        for type_index, piece_type in enumerate(PIECE_TYPES):
            plane = (color_index * 6 + type_index) * 64
            for square in board.pieces(piece_type, color):
                out[plane + square] = 1.0

    white_attacks = np.zeros(64, dtype=np.float32)
    black_attacks = np.zeros(64, dtype=np.float32)
    for square, piece in board.piece_map().items():
        target = white_attacks if piece.color == chess.WHITE else black_attacks
        for attacked in board.attacks(square):
            target[attacked] += 1.0
    out[12 * 64 : 12 * 64 + 64] = np.clip(white_attacks / 4.0, 0.0, 1.0)
    out[12 * 64 + 64 : 12 * 64 + 128] = np.clip(black_attacks / 4.0, 0.0, 1.0)

    base = 12 * 64 + 128
    out[base + 0] = 1.0 if board.turn == chess.WHITE else -1.0
    out[base + 1] = float(board.has_kingside_castling_rights(chess.WHITE))
    out[base + 2] = float(board.has_queenside_castling_rights(chess.WHITE))
    out[base + 3] = float(board.has_kingside_castling_rights(chess.BLACK))
    out[base + 4] = float(board.has_queenside_castling_rights(chess.BLACK))
    if board.ep_square is not None:
        out[base + 5 + chess.square_file(board.ep_square)] = 1.0
    out[base + 13] = min(board.halfmove_clock, 100) / 100.0
    out[base + 14] = min(board.fullmove_number, 200) / 200.0
    out[base + 15] = np.clip(material_balance_cp(board) / 4000.0, -1.0, 1.0)
    out[base + 16] = min(ply, max_plies) / float(max_plies)
    out[base + 17] = min(board.legal_moves.count(), 128) / 128.0
    out[base + 18] = float(board.is_check())
    return out


def flat_observation(board: chess.Board, ply: int, max_plies: int) -> np.ndarray:
    return np.concatenate([extract_features(board, ply, max_plies), legal_action_mask(board)]).astype(np.float32)


class PufferLulzfishEnv(pufferlib.PufferEnv):
    def __init__(
        self,
        engine_path: str | Path | None = None,
        opponent: str = "random",
        opponent_depth: int = 1,
        max_plies: int = 200,
        illegal_reward: float = -1.0,
        buf: dict[str, np.ndarray] | None = None,
        seed: int | None = 0,
    ) -> None:
        self.single_observation_space = spaces.Box(-1.0, 1.0, shape=(PUFFER_OBS_SIZE,), dtype=np.float32)
        self.single_action_space = spaces.Discrete(ACTION_SIZE)
        self.num_agents = 1
        self.engine_path = Path(engine_path) if engine_path else None
        self.opponent = opponent
        self.opponent_depth = opponent_depth
        self.max_plies = max_plies
        self.illegal_reward = illegal_reward
        self.rng = random.Random(seed)
        self.engine: UciEngine | None = None
        self.board = chess.Board()
        self.agent_color = chess.WHITE
        self.ply = 0
        self._done = True
        super().__init__(buf=buf)

    @property
    def done(self) -> bool:
        return self._done

    def reset(self, seed: int | None = None) -> tuple[np.ndarray, list[dict[str, Any]]]:
        if seed is not None:
            self.rng.seed(seed)
        self.board = chess.Board()
        self.agent_color = chess.WHITE
        self.ply = 0
        self._done = False
        if self.opponent == "engine" and self.engine is None:
            if self.engine_path is None:
                raise RuntimeError("engine opponent requires engine_path")
            self.engine = UciEngine(self.engine_path)
        self._write_obs()
        self.rewards[0] = 0.0
        self.terminals[0] = False
        self.truncations[0] = False
        self.masks[0] = True
        return self.observations, [{"fen": self.board.fen()}]

    def step(self, actions: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, list[dict[str, Any]]]:
        if self._done:
            self.reset()

        action = int(np.asarray(actions).reshape(-1)[0])
        move = move_from_action(action)
        reward = 0.0
        terminal = False
        info: dict[str, Any] = {"fen": self.board.fen()}

        if move not in self.board.legal_moves:
            reward = self.illegal_reward
            terminal = True
            info = {"illegal": move.uci(), "fen": self.board.fen()}
        else:
            self.board.push(move)
            self.ply += 1
            terminal_reward_value, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
            if terminal_reward_value is not None:
                reward = terminal_reward_value
                terminal = True
                info = {"termination": reason, "fen": self.board.fen()}
            elif self.opponent != "none":
                self._play_opponent()
                terminal_reward_value, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
                if terminal_reward_value is not None:
                    reward = terminal_reward_value
                    terminal = True
                    info = {"termination": reason, "fen": self.board.fen()}

        self._done = terminal
        self._write_obs()
        self.rewards[0] = reward
        self.terminals[0] = terminal
        self.truncations[0] = False
        self.masks[0] = True
        return self.observations, self.rewards, self.terminals, self.truncations, [info]

    def close(self) -> None:
        if self.engine is not None:
            self.engine.close()
            self.engine = None

    def _play_opponent(self) -> None:
        legal = list(self.board.legal_moves)
        if not legal:
            return
        if self.opponent == "random":
            move = self.rng.choice(legal)
        elif self.opponent == "engine":
            assert self.engine is not None
            move = chess.Move.from_uci(self.engine.bestmove_fen(self.board.fen(), self.opponent_depth))
            if move not in self.board.legal_moves:
                move = self.rng.choice(legal)
        else:
            raise ValueError(f"unknown opponent: {self.opponent}")
        self.board.push(move)
        self.ply += 1

    def _write_obs(self) -> None:
        self.observations[0] = flat_observation(self.board, self.ply, self.max_plies)


def make_puffer_env(**kwargs: Any) -> PufferLulzfishEnv:
    return PufferLulzfishEnv(**kwargs)

