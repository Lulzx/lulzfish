#!/usr/bin/env python3
"""PufferLib/Gym-style environment and feature extraction for Lulzfish.

This module intentionally keeps training code out of the C++ engine. It gives
PufferLib or Gymnasium a fixed action space, compact observations, and optional
UCI-engine opponents for data generation experiments.
"""

from __future__ import annotations

import random
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np

try:
    import bulletchess as bc
except ImportError as exc:  # pragma: no cover - dependency error path
    raise SystemExit("Install bulletchess with: python3 -m pip install bulletchess") from exc

try:
    import gymnasium as gym
    from gymnasium import spaces
except ImportError:  # PufferLib can still import the env after gym is installed.
    gym = None
    spaces = None


START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1"
PROMOTION_CODES = {"": 0, "n": 1, "b": 2, "r": 3, "q": 4}
PROMOTION_SUFFIXES = {value: key for key, value in PROMOTION_CODES.items()}
ACTION_SIZE = 64 * 64 * 5
FEATURE_SIZE = 12 * 64 + 64 + 64 + 32
PUFFER_OBS_SIZE = FEATURE_SIZE + ACTION_SIZE

PIECE_TYPES = [bc.PAWN, bc.KNIGHT, bc.BISHOP, bc.ROOK, bc.QUEEN, bc.KING]
COLORS = [bc.WHITE, bc.BLACK]
PIECE_VALUES_CP = {
    bc.PAWN: 100,
    bc.KNIGHT: 320,
    bc.BISHOP: 330,
    bc.ROOK: 500,
    bc.QUEEN: 900,
    bc.KING: 0,
}


@dataclass(frozen=True)
class EngineResult:
    bestmove: str
    score_cp: int
    pv: tuple[str, ...]


class UciEngine:
    """Small UCI wrapper for analysis labels and engine opponents."""

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
        pv: tuple[str, ...] = ()

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
                            mate = int(parts[idx + 2])
                            score_cp = 30000 if mate > 0 else -30000
                if "pv" in parts:
                    pv = tuple(parts[parts.index("pv") + 1 :])
            elif line.startswith("bestmove"):
                pieces = line.split()
                bestmove = pieces[1] if len(pieces) > 1 else "0000"
                return EngineResult(bestmove=bestmove, score_cp=score_cp, pv=pv)

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
            stderr = ""
            if self.process.stderr is not None:
                stderr = self.process.stderr.read()
            raise RuntimeError(f"{self.path} exited while waiting for UCI output: {stderr}")
        return line.strip()

    def _read_until(self, token: str) -> None:
        while True:
            if self._readline() == token:
                return


def action_from_uci(move: str) -> int:
    from_sq = bc.Square.from_str(move[:2]).index()
    to_sq = bc.Square.from_str(move[2:4]).index()
    promotion = PROMOTION_CODES.get(move[4:5], 0)
    return ((from_sq * 64) + to_sq) * 5 + promotion


def uci_from_action(action: int) -> str:
    promotion = action % 5
    packed = action // 5
    to_sq = packed % 64
    from_sq = packed // 64
    return square_name(from_sq) + square_name(to_sq) + PROMOTION_SUFFIXES[promotion]


def square_name(index: int) -> str:
    return chr(ord("a") + (index & 7)) + chr(ord("1") + (index >> 3))


def legal_action_mask(board: bc.Board) -> np.ndarray:
    mask = np.zeros(ACTION_SIZE, dtype=np.int8)
    for move in board.legal_moves():
        mask[action_from_uci(move.uci())] = 1
    return mask


def flat_puffer_observation(board: bc.Board, ply: int = 0, max_plies: int = 200) -> np.ndarray:
    """Flat observation used by native PufferLib envs.

    The tail is the legal-action mask. Keeping it in the observation makes the
    env usable by PufferLib without requiring algorithm-side API changes.
    """

    features = extract_features(board, ply, max_plies)
    mask = legal_action_mask(board).astype(np.float32)
    return np.concatenate([features, mask]).astype(np.float32)


def game_result(board: bc.Board, hit_max_plies: bool = False) -> tuple[float | None, str]:
    if board in bc.CHECKMATE:
        return (-1.0 if board.turn is bc.WHITE else 1.0), "checkmate"
    if board in bc.STALEMATE:
        return 0.0, "stalemate"
    if board in bc.THREEFOLD_REPETITION or board in bc.FIVEFOLD_REPETITION:
        return 0.0, "repetition"
    if board in bc.FIFTY_MOVE_TIMEOUT or board in bc.FORCED_DRAW or board in bc.DRAW:
        return 0.0, "draw"
    if hit_max_plies:
        material = material_balance_cp(board)
        if material > 200:
            return 0.25, "max_plies_material"
        if material < -200:
            return -0.25, "max_plies_material"
        return 0.0, "max_plies"
    return None, "ongoing"


def terminal_reward(board: bc.Board, agent_color: Any, hit_max_plies: bool = False) -> tuple[float | None, str]:
    white_reward, reason = game_result(board, hit_max_plies)
    if white_reward is None:
        return None, reason
    return (white_reward if agent_color is bc.WHITE else -white_reward), reason


def material_balance_cp(board: bc.Board) -> int:
    white = 0
    black = 0
    for piece_type, value in PIECE_VALUES_CP.items():
        white += len(board[bc.WHITE, piece_type]) * value
        black += len(board[bc.BLACK, piece_type]) * value
    return white - black


def board_arrays(board: bc.Board) -> tuple[list[Any | None], np.ndarray]:
    pieces: list[Any | None] = [None] * 64
    planes = np.zeros(12 * 64, dtype=np.float32)
    for color_index, color in enumerate(COLORS):
        for type_index, piece_type in enumerate(PIECE_TYPES):
            plane = (color_index * 6 + type_index) * 64
            for square in board[color, piece_type]:
                idx = square.index()
                pieces[idx] = (color, piece_type)
                planes[plane + idx] = 1.0
    return pieces, planes


def attack_counts(pieces: list[Any | None]) -> tuple[np.ndarray, np.ndarray]:
    white = np.zeros(64, dtype=np.float32)
    black = np.zeros(64, dtype=np.float32)
    for sq, piece in enumerate(pieces):
        if piece is None:
            continue
        color, piece_type = piece
        target = white if color is bc.WHITE else black
        for attacked in attacks_from(sq, piece_type, color, pieces):
            target[attacked] += 1.0
    return np.clip(white / 4.0, 0.0, 1.0), np.clip(black / 4.0, 0.0, 1.0)


def attacks_from(square: int, piece_type: Any, color: Any, pieces: list[Any | None]) -> list[int]:
    file = square & 7
    rank = square >> 3
    attacks: list[int] = []

    def add(f: int, r: int) -> None:
        if 0 <= f < 8 and 0 <= r < 8:
            attacks.append((r << 3) | f)

    if piece_type is bc.PAWN:
        step = 1 if color is bc.WHITE else -1
        add(file - 1, rank + step)
        add(file + 1, rank + step)
    elif piece_type is bc.KNIGHT:
        for df, dr in ((1, 2), (2, 1), (2, -1), (1, -2), (-1, -2), (-2, -1), (-2, 1), (-1, 2)):
            add(file + df, rank + dr)
    elif piece_type is bc.KING:
        for df in (-1, 0, 1):
            for dr in (-1, 0, 1):
                if df or dr:
                    add(file + df, rank + dr)
    else:
        directions: list[tuple[int, int]] = []
        if piece_type in (bc.ROOK, bc.QUEEN):
            directions += [(0, 1), (1, 0), (0, -1), (-1, 0)]
        if piece_type in (bc.BISHOP, bc.QUEEN):
            directions += [(1, 1), (1, -1), (-1, -1), (-1, 1)]
        for df, dr in directions:
            f = file + df
            r = rank + dr
            while 0 <= f < 8 and 0 <= r < 8:
                target = (r << 3) | f
                attacks.append(target)
                if pieces[target] is not None:
                    break
                f += df
                r += dr
    return attacks


def extract_features(board: bc.Board, ply: int = 0, max_plies: int = 200) -> np.ndarray:
    pieces, planes = board_arrays(board)
    white_attacks, black_attacks = attack_counts(pieces)
    extras = np.zeros(32, dtype=np.float32)
    extras[0] = 1.0 if board.turn is bc.WHITE else -1.0
    extras[1] = float(bc.WHITE_KINGSIDE in board.castling_rights)
    extras[2] = float(bc.WHITE_QUEENSIDE in board.castling_rights)
    extras[3] = float(bc.BLACK_KINGSIDE in board.castling_rights)
    extras[4] = float(bc.BLACK_QUEENSIDE in board.castling_rights)
    if board.en_passant_square is not None:
        extras[5 + (board.en_passant_square.index() & 7)] = 1.0
    extras[13] = min(board.halfmove_clock, 100) / 100.0
    extras[14] = min(board.fullmove_number, 200) / 200.0
    extras[15] = np.clip(material_balance_cp(board) / 4000.0, -1.0, 1.0)
    extras[16] = min(ply, max_plies) / float(max_plies)
    extras[17] = min(len(board.legal_moves()), 128) / 128.0
    extras[18] = float(board in bc.CHECK)
    return np.concatenate([planes, white_attacks, black_attacks, extras]).astype(np.float32)


class LulzfishEnv(gym.Env if gym is not None else object):
    """Fixed-action chess environment suitable for PufferLib prototyping."""

    metadata = {"render_modes": []}

    def __init__(
        self,
        engine_path: str | Path | None = None,
        opponent: str = "random",
        opponent_depth: int = 1,
        max_plies: int = 200,
        illegal_reward: float = -1.0,
        seed: int | None = None,
    ) -> None:
        if gym is None:
            raise RuntimeError("Install gymnasium for Env spaces: python3 -m pip install gymnasium")

        self.engine_path = Path(engine_path) if engine_path else None
        self.opponent = opponent
        self.opponent_depth = opponent_depth
        self.max_plies = max_plies
        self.illegal_reward = illegal_reward
        self.rng = random.Random(seed)
        self.engine: UciEngine | None = None
        self.board = bc.Board()
        self.agent_color = bc.WHITE
        self.ply = 0

        self.action_space = spaces.Discrete(ACTION_SIZE)
        self.observation_space = spaces.Dict(
            {
                "features": spaces.Box(-1.0, 1.0, shape=(FEATURE_SIZE,), dtype=np.float32),
                "action_mask": spaces.Box(0, 1, shape=(ACTION_SIZE,), dtype=np.int8),
            }
        )

    def close(self) -> None:
        if self.engine is not None:
            self.engine.close()
            self.engine = None

    def reset(self, *, seed: int | None = None, options: dict[str, Any] | None = None) -> tuple[dict[str, np.ndarray], dict[str, Any]]:
        if seed is not None:
            self.rng.seed(seed)
        options = options or {}
        fen = options.get("fen")
        self.board = bc.Board.from_fen(fen) if fen else bc.Board()
        self.agent_color = options.get("agent_color", bc.WHITE)
        self.ply = 0
        if self.opponent == "engine" and self.engine is None:
            if self.engine_path is None:
                raise RuntimeError("engine opponent requires engine_path")
            self.engine = UciEngine(self.engine_path)
        return self._obs(), {"fen": self.board.fen()}

    def step(self, action: int) -> tuple[dict[str, np.ndarray], float, bool, bool, dict[str, Any]]:
        legal = {move.uci(): move for move in self.board.legal_moves()}
        move_text = uci_from_action(int(action))
        if move_text not in legal:
            return self._obs(), self.illegal_reward, True, False, {"illegal": move_text, "fen": self.board.fen()}

        self.board.apply(legal[move_text])
        self.ply += 1
        reward, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
        if reward is not None:
            return self._obs(), reward, True, False, {"termination": reason, "fen": self.board.fen()}

        if self.opponent != "none":
            self._play_opponent()
            reward, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
            if reward is not None:
                return self._obs(), reward, True, False, {"termination": reason, "fen": self.board.fen()}

        return self._obs(), 0.0, False, False, {"fen": self.board.fen()}

    def _play_opponent(self) -> None:
        legal = self.board.legal_moves()
        if not legal:
            return
        if self.opponent == "random":
            move = self.rng.choice(legal)
        elif self.opponent == "engine":
            assert self.engine is not None
            best = self.engine.bestmove_fen(self.board.fen(), self.opponent_depth)
            move = bc.Move.from_uci(best)
            if move not in legal:
                move = self.rng.choice(legal)
        else:
            raise ValueError(f"unknown opponent: {self.opponent}")
        self.board.apply(move)
        self.ply += 1

    def _obs(self) -> dict[str, np.ndarray]:
        return {
            "features": extract_features(self.board, self.ply, self.max_plies),
            "action_mask": legal_action_mask(self.board),
        }


def make_env(**kwargs: Any) -> LulzfishEnv:
    """Factory used by PufferLib/Gym vectorizers."""

    return LulzfishEnv(**kwargs)


def puffer_imports() -> tuple[Any, Any]:
    """Import PufferLib lazily so normal data tools do not require it."""

    try:
        import pufferlib
        import gymnasium.spaces as gym_spaces
    except ImportError as exc:
        raise RuntimeError(
            "PufferLib mode requires pufferlib and gymnasium. Use a Python 3.10/3.11 "
            "venv and install tools/rl/requirements-puffer.txt."
        ) from exc
    return pufferlib, gym_spaces


def make_puffer_env(**kwargs: Any) -> Any:
    """Factory returning a native PufferLib environment."""

    pufferlib, gym_spaces = puffer_imports()

    class PufferLulzfishEnv(pufferlib.PufferEnv):
        """Native single-agent PufferEnv backed by bulletchess."""

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
            self.single_observation_space = gym_spaces.Box(
                low=-1.0,
                high=1.0,
                shape=(PUFFER_OBS_SIZE,),
                dtype=np.float32,
            )
            self.single_action_space = gym_spaces.Discrete(ACTION_SIZE)
            self.num_agents = 1

            self.engine_path = Path(engine_path) if engine_path else None
            self.opponent = opponent
            self.opponent_depth = opponent_depth
            self.max_plies = max_plies
            self.illegal_reward = illegal_reward
            self.rng = random.Random(seed)
            self.engine: UciEngine | None = None
            self.board = bc.Board()
            self.agent_color = bc.WHITE
            self.ply = 0
            self._done = True

            super().__init__(buf=buf)

        @property
        def done(self) -> bool:
            return self._done

        def reset(self, seed: int | None = None) -> tuple[np.ndarray, list[dict[str, Any]]]:
            if seed is not None:
                self.rng.seed(seed)
            self.board = bc.Board()
            self.agent_color = bc.WHITE
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
            reward = 0.0
            terminated = False
            truncated = False
            info: dict[str, Any] = {"fen": self.board.fen()}

            legal = {move.uci(): move for move in self.board.legal_moves()}
            move_text = uci_from_action(action)
            if move_text not in legal:
                reward = self.illegal_reward
                terminated = True
                info = {"illegal": move_text, "fen": self.board.fen()}
            else:
                self.board.apply(legal[move_text])
                self.ply += 1
                terminal, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
                if terminal is not None:
                    reward = terminal
                    terminated = True
                    info = {"termination": reason, "fen": self.board.fen()}
                elif self.opponent != "none":
                    self._play_opponent()
                    terminal, reason = terminal_reward(self.board, self.agent_color, self.ply >= self.max_plies)
                    if terminal is not None:
                        reward = terminal
                        terminated = True
                        info = {"termination": reason, "fen": self.board.fen()}

            self._done = terminated or truncated
            self._write_obs()
            self.rewards[0] = reward
            self.terminals[0] = terminated
            self.truncations[0] = truncated
            self.masks[0] = True
            return self.observations, self.rewards, self.terminals, self.truncations, [info]

        def close(self) -> None:
            if self.engine is not None:
                self.engine.close()
                self.engine = None

        def _play_opponent(self) -> None:
            legal = self.board.legal_moves()
            if not legal:
                return
            if self.opponent == "random":
                move = self.rng.choice(legal)
            elif self.opponent == "engine":
                assert self.engine is not None
                best = self.engine.bestmove_fen(self.board.fen(), self.opponent_depth)
                move = bc.Move.from_uci(best)
                if move not in legal:
                    move = self.rng.choice(legal)
            else:
                raise ValueError(f"unknown opponent: {self.opponent}")
            self.board.apply(move)
            self.ply += 1

        def _write_obs(self) -> None:
            self.observations[0] = flat_puffer_observation(self.board, self.ply, self.max_plies)

    return PufferLulzfishEnv(**kwargs)
