#!/usr/bin/env python3
"""
Local browser GUI for playing against Lulzfish.

The browser handles presentation only. bulletchess is the referee, and the
Lulzfish binary is used through UCI for engine moves.
"""

from __future__ import annotations

import argparse
import json
import secrets
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from http import HTTPStatus
from http.cookies import CookieError, SimpleCookie
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

try:
    import bulletchess as bc
except ImportError as exc:
    raise SystemExit(
        "bulletchess is required. Install it in a venv with: "
        "python3 -m pip install bulletchess"
    ) from exc


SESSION_COOKIE = "lulzfish_session"
SESSION_ID_BYTES = 24
DEFAULT_SESSION_TTL_SECONDS = 12 * 60 * 60
DEFAULT_MAX_SESSIONS = 128


HTML = r"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Lulzfish Board</title>
  <link rel="stylesheet" href="https://unpkg.com/chessground@9.2.1/assets/chessground.base.css">
  <link rel="stylesheet" href="https://unpkg.com/chessground@9.2.1/assets/chessground.brown.css">
  <link rel="stylesheet" href="https://unpkg.com/chessground@9.2.1/assets/chessground.cburnett.css">
  <style>
    :root {
      color-scheme: light;
      --bg: #f6f7f2;
      --ink: #20231f;
      --muted: #6d7467;
      --panel: #ffffff;
      --line: #d8dccf;
      --dark-square: #7e9674;
      --light-square: #eee6d3;
      --accent: #2f6f4e;
      --accent-2: #a24d3d;
      --last: #d6b95f;
      --legal: rgba(32, 35, 31, 0.28);
      --check: #b9463f;
    }

    * { box-sizing: border-box; }

    body {
      margin: 0;
      min-height: 100dvh;
      background:
        linear-gradient(110deg, rgba(47,111,78,0.08), transparent 34%),
        var(--bg);
      color: var(--ink);
      font-family: ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
    }

    button, select, input {
      font: inherit;
    }

    .shell {
      width: min(1180px, calc(100vw - 32px));
      min-height: 100dvh;
      margin: 0 auto;
      display: grid;
      grid-template-columns: minmax(320px, 72dvh) minmax(280px, 1fr);
      gap: clamp(20px, 4vw, 56px);
      align-items: center;
      padding: 28px 0;
    }

    .board-wrap {
      width: 100%;
      max-width: 760px;
    }

    .board-frame {
      border: 1px solid rgba(32,35,31,0.18);
      background: #283025;
      padding: clamp(8px, 1.4vw, 14px);
      box-shadow: 0 22px 70px rgba(32,35,31,0.18);
    }

    .board {
      width: 100%;
      aspect-ratio: 1;
      border: 1px solid rgba(32,35,31,0.42);
      overflow: hidden;
    }

    .board cg-board {
      background-color: var(--light-square);
    }

    .board square.last-move {
      background-color: rgba(214,185,95,0.44);
    }

    .board square.check {
      background: radial-gradient(circle, rgba(185,70,63,0.82) 0%, rgba(185,70,63,0.28) 58%, transparent 75%);
    }

    .board square.move-dest {
      background: radial-gradient(rgba(32,35,31,0.28) 19%, transparent 20%);
    }

    .board square.premove-dest {
      background: radial-gradient(rgba(162,77,61,0.35) 19%, transparent 20%);
    }

    .side {
      align-self: stretch;
      display: grid;
      align-content: center;
      gap: 20px;
    }

    .brand {
      display: flex;
      align-items: baseline;
      justify-content: space-between;
      gap: 20px;
      border-bottom: 1px solid var(--line);
      padding-bottom: 18px;
    }

    h1 {
      margin: 0;
      font-size: clamp(30px, 5vw, 54px);
      line-height: 0.95;
      letter-spacing: 0;
      font-weight: 760;
    }

    .badge {
      color: var(--muted);
      font-size: 13px;
      letter-spacing: 0.08em;
      text-transform: uppercase;
      white-space: nowrap;
    }

    .status {
      display: grid;
      gap: 8px;
      border-bottom: 1px solid var(--line);
      padding-bottom: 18px;
    }

    .status-main {
      min-height: 32px;
      font-size: clamp(20px, 2.4vw, 28px);
      font-weight: 680;
      line-height: 1.15;
    }

    .status-sub {
      color: var(--muted);
      min-height: 24px;
      font-size: 15px;
    }

    .controls {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
      align-items: end;
    }

    .field {
      display: grid;
      gap: 6px;
    }

    label {
      color: var(--muted);
      font-size: 13px;
      font-weight: 650;
    }

    select, input {
      width: 100%;
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--ink);
      padding: 11px 12px;
      min-height: 44px;
      outline: none;
    }

    select:focus, input:focus {
      border-color: var(--accent);
      box-shadow: 0 0 0 3px rgba(47,111,78,0.12);
    }

    .actions {
      display: grid;
      grid-template-columns: 1fr 1fr;
      gap: 12px;
    }

    .button {
      border: 1px solid #244e39;
      background: var(--accent);
      color: #fff;
      min-height: 46px;
      cursor: pointer;
      font-weight: 720;
      transition: transform 140ms ease, filter 140ms ease;
    }

    .button.secondary {
      background: var(--panel);
      color: var(--ink);
      border-color: var(--line);
    }

    .button:active {
      transform: translateY(1px) scale(0.99);
    }

    .move-list {
      max-height: min(34dvh, 360px);
      overflow: auto;
      border-top: 1px solid var(--line);
      border-bottom: 1px solid var(--line);
      padding: 12px 0;
      font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
      font-size: 13px;
      color: #3a4038;
      line-height: 1.8;
    }

    .moves-empty {
      color: var(--muted);
      font-family: ui-sans-serif, system-ui, sans-serif;
      line-height: 1.4;
    }

    .promotion {
      display: none;
      gap: 8px;
      align-items: center;
      color: var(--muted);
      font-size: 13px;
    }

    .promotion.open {
      display: flex;
    }

    .promo-button {
      width: 42px;
      aspect-ratio: 1;
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--ink);
      cursor: pointer;
      font-size: 22px;
    }

    .error {
      min-height: 22px;
      color: var(--accent-2);
      font-size: 14px;
    }

    @media (max-width: 860px) {
      .shell {
        grid-template-columns: 1fr;
        align-items: start;
        width: min(100vw - 24px, 720px);
        padding: 16px 0 28px;
      }
      .side {
        align-content: start;
      }
      .controls, .actions {
        grid-template-columns: 1fr;
      }
      .move-list {
        max-height: 220px;
      }
    }
  </style>
</head>
<body>
  <main class="shell">
    <section class="board-wrap" aria-label="Chess board">
      <div class="board-frame">
        <div id="board" class="board"></div>
      </div>
    </section>

    <section class="side">
      <div class="brand">
        <h1>Lulzfish</h1>
        <div class="badge">local UCI</div>
      </div>

      <div class="status">
        <div id="statusMain" class="status-main">Loading board</div>
        <div id="statusSub" class="status-sub"></div>
      </div>

      <div class="controls">
        <div class="field">
          <label for="color">Play as</label>
          <select id="color">
            <option value="white">White</option>
            <option value="black">Black</option>
          </select>
        </div>
        <div class="field">
          <label for="depth">Engine depth</label>
          <input id="depth" type="number" min="1" max="5" value="2">
        </div>
      </div>

      <div class="actions">
        <button id="newGame" class="button">New game</button>
        <button id="flip" class="button secondary">Flip board</button>
      </div>

      <div id="promotion" class="promotion">
        <span>Promote to</span>
        <button class="promo-button" data-promo="q">Q</button>
        <button class="promo-button" data-promo="r">R</button>
        <button class="promo-button" data-promo="b">B</button>
        <button class="promo-button" data-promo="n">N</button>
      </div>

      <div id="error" class="error"></div>
      <div id="moves" class="move-list"><span class="moves-empty">No moves yet.</span></div>
    </section>
  </main>

  <script type="module">
    import { Chessground } from "https://unpkg.com/chessground@9.2.1/dist/chessground.js";

    let ground = null;
    const state = {
      legal: [],
      playerColor: "white",
      orientation: "white",
      pendingPromotion: null,
      lastMove: null,
      fen: "startpos",
      turn: "white",
      checkSquare: null,
      busy: false
    };

    const boardEl = document.getElementById("board");
    const statusMain = document.getElementById("statusMain");
    const statusSub = document.getElementById("statusSub");
    const movesEl = document.getElementById("moves");
    const errorEl = document.getElementById("error");
    const colorEl = document.getElementById("color");
    const depthEl = document.getElementById("depth");
    const promotionEl = document.getElementById("promotion");

    function legalDests() {
      const dests = new Map();
      for (const move of state.legal) {
        const from = move.slice(0, 2);
        const to = move.slice(2, 4);
        const current = dests.get(from) || [];
        if (!current.includes(to)) current.push(to);
        dests.set(from, current);
      }
      return dests;
    }

    function renderBoard() {
      const config = {
        fen: state.fen,
        orientation: state.orientation,
        turnColor: state.turn,
        check: state.checkSquare || false,
        lastMove: state.lastMove ? [state.lastMove.slice(0, 2), state.lastMove.slice(2, 4)] : undefined,
        coordinates: true,
        highlight: {
          lastMove: true,
          check: true,
        },
        animation: {
          enabled: true,
          duration: 160,
        },
        movable: {
          free: false,
          color: state.legal.length ? state.playerColor : undefined,
          dests: legalDests(),
          showDests: true,
          events: {
            after: onGroundMove,
          },
        },
        premovable: {
          enabled: false,
        },
        draggable: {
          enabled: true,
          showGhost: true,
        },
      };

      if (!ground) ground = Chessground(boardEl, config);
      else ground.set(config);
    }

    function renderMoves(san) {
      if (!san.length) {
        movesEl.innerHTML = '<span class="moves-empty">No moves yet.</span>';
        return;
      }
      const rows = [];
      for (let i = 0; i < san.length; i += 2) {
        const moveNo = Math.floor(i / 2) + 1;
        rows.push(`${moveNo}. ${san[i]}${san[i + 1] ? " " + san[i + 1] : ""}`);
      }
      movesEl.textContent = rows.join("  ");
      movesEl.scrollTop = movesEl.scrollHeight;
    }

    function setBusy(isBusy) {
      state.busy = isBusy;
      document.getElementById("newGame").disabled = isBusy;
      errorEl.textContent = "";
      if (isBusy) {
        statusSub.textContent = "Engine is thinking.";
      }
    }

    function updateFromServer(data) {
      state.legal = data.legal_moves;
      state.playerColor = data.player_color;
      state.lastMove = data.last_move;
      state.checkSquare = data.check_square;
      state.fen = data.fen;
      state.turn = data.turn;
      colorEl.value = data.player_color;
      depthEl.value = data.depth;

      statusMain.textContent = data.status;
      statusSub.textContent = data.result || `${data.turn === "white" ? "White" : "Black"} to move`;
      renderMoves(data.san);
      renderBoard();
    }

    async function api(path, body) {
      const res = await fetch(path, {
        method: body ? "POST" : "GET",
        headers: body ? {"Content-Type": "application/json"} : {},
        body: body ? JSON.stringify(body) : undefined
      });
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || "Request failed");
      return data;
    }

    async function loadState() {
      try {
        updateFromServer(await api("/api/state"));
      } catch (err) {
        errorEl.textContent = err.message;
      }
    }

    async function newGame() {
      setBusy(true);
      state.pendingPromotion = null;
      promotionEl.classList.remove("open");
      try {
        const data = await api("/api/new", {
          player_color: colorEl.value,
          depth: Number(depthEl.value || 2)
        });
        state.orientation = colorEl.value;
        updateFromServer(data);
      } catch (err) {
        errorEl.textContent = err.message;
      } finally {
        setBusy(false);
      }
    }

    async function sendMove(uci) {
      setBusy(true);
      state.pendingPromotion = null;
      promotionEl.classList.remove("open");
      try {
        updateFromServer(await api("/api/move", {uci}));
      } catch (err) {
        errorEl.textContent = err.message;
        renderBoard();
      } finally {
        setBusy(false);
      }
    }

    function onGroundMove(orig, dest) {
      if (state.busy || state.pendingPromotion) {
        renderBoard();
        return;
      }

      const moves = state.legal.filter((move) => move.slice(0, 2) === orig && move.slice(2, 4) === dest);
      if (!moves.length) {
        renderBoard();
        return;
      }

      const promotionMoves = moves.filter((move) => move.length === 5);
      if (promotionMoves.length > 1) {
        state.pendingPromotion = {from: orig, to: dest, moves: promotionMoves};
        promotionEl.classList.add("open");
        renderBoard();
        return;
      }

      sendMove(moves[0]);
    }

    document.getElementById("newGame").addEventListener("click", newGame);
    document.getElementById("flip").addEventListener("click", () => {
      state.orientation = state.orientation === "white" ? "black" : "white";
      renderBoard();
    });

    document.querySelectorAll(".promo-button").forEach((button) => {
      button.addEventListener("click", () => {
        if (!state.pendingPromotion) return;
        const promo = button.dataset.promo;
        const move = state.pendingPromotion.moves.find((item) => item.endsWith(promo));
        if (move) sendMove(move);
      });
    });

    loadState();
  </script>
</body>
</html>
"""


class Engine:
    def __init__(self, command: str):
        self.command = command
        self.lock = threading.Lock()
        self.proc = subprocess.Popen(
            [command],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        self._send("uci")
        self._read_until("uciok")
        self._send("isready")
        self._read_until("readyok")

    def _send(self, line: str) -> None:
        assert self.proc.stdin is not None
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def _read_until(self, token: str, timeout: float = 10.0) -> list[str]:
        assert self.proc.stdout is not None
        deadline = time.time() + timeout
        lines: list[str] = []
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                continue
            line = line.strip()
            lines.append(line)
            if line == token or line.endswith(token):
                return lines
        raise TimeoutError(f"engine did not return {token}; last output: {lines[-8:]}")

    def newgame(self) -> None:
        with self.lock:
            self._send("ucinewgame")
            self._send("isready")
            self._read_until("readyok")

    def bestmove(self, moves: list[str], depth: int) -> str:
        with self.lock:
            position = "position startpos"
            if moves:
                position += " moves " + " ".join(moves)
            self._send(position)
            self._send(f"go depth {depth}")

            assert self.proc.stdout is not None
            deadline = time.time() + 30.0
            while time.time() < deadline:
                line = self.proc.stdout.readline()
                if not line:
                    continue
                line = line.strip()
                if line.startswith("bestmove "):
                    return line.split()[1]
            raise TimeoutError("engine did not return bestmove")

    def close(self) -> None:
        try:
            self._send("quit")
            self.proc.wait(timeout=2.0)
        except Exception:
            self.proc.kill()


def _status_and_result(board: bc.Board) -> tuple[str, str]:
    if board in bc.CHECKMATE:
        winner = "White" if board.turn is bc.BLACK else "Black"
        return "Game over", f"{'1-0' if winner == 'White' else '0-1'} by checkmate"
    if board in bc.STALEMATE:
        return "Game over", "1/2-1/2 by stalemate"
    if board in bc.THREEFOLD_REPETITION:
        return "Game over", "1/2-1/2 by threefold repetition"
    if board in bc.FIVEFOLD_REPETITION:
        return "Game over", "1/2-1/2 by fivefold repetition"
    if board in bc.FIFTY_MOVE_TIMEOUT:
        return "Game over", "1/2-1/2 by fifty-move rule"
    if board in bc.FORCED_DRAW or board in bc.DRAW:
        return "Game over", "1/2-1/2 by draw"
    return "", ""


def _find_king_sq(board: bc.Board) -> bc.Square | None:
    for sq in bc.SQUARES:
        piece = board[sq]
        if piece is not None and piece.piece_type == bc.KING and piece.color is board.turn:
            return sq
    return None


class Game:
    def __init__(self, engine: Engine, depth: int, player_color: str):
        self.engine = engine
        self.board = bc.Board()
        self.moves: list[str] = []
        self.san: list[str] = []
        self.depth = depth
        self.player_color = player_color
        self.last_move: str | None = None
        self.lock = threading.Lock()
        if self.engine_to_move():
            self.play_engine_move()

    def engine_to_move(self) -> bool:
        engine_is_white = self.player_color == "black"
        return self.board.turn is bc.WHITE if engine_is_white else self.board.turn is bc.BLACK

    def is_game_over(self) -> bool:
        return _status_and_result(self.board)[0] == "Game over"

    def player_to_move(self) -> bool:
        return not self.is_game_over() and not self.engine_to_move()

    def play_engine_move(self) -> None:
        if self.is_game_over():
            return
        move_text = self.engine.bestmove(self.moves, self.depth)
        if move_text == "0000":
            return
        move = bc.Move.from_uci(move_text)
        if move not in self.board.legal_moves():
            raise ValueError(f"Lulzfish returned illegal move {move_text} in {self.board.fen()}")
        self.push(move)

    def push(self, move: bc.Move) -> None:
        self.san.append(move.san(self.board))
        self.board.apply(move)
        self.last_move = move.uci()
        self.moves.append(self.last_move)

    def make_player_move(self, move_text: str) -> None:
        with self.lock:
            if not self.player_to_move():
                raise ValueError("It is not your turn.")
            move = bc.Move.from_uci(move_text)
            if move not in self.board.legal_moves():
                raise ValueError(f"Illegal move: {move_text}")
            self.push(move)
            if not self.is_game_over():
                self.play_engine_move()

    def state(self) -> dict[str, Any]:
        with self.lock:
            legal_moves = []
            if self.player_to_move():
                legal_moves = [move.uci() for move in self.board.legal_moves()]

            status, result = _status_and_result(self.board)
            if not status:
                if self.engine_to_move():
                    status = "Lulzfish to move"
                else:
                    status = "Your move"

            board_map = {}
            for sq in bc.SQUARES:
                piece = self.board[sq]
                if piece is not None:
                    board_map[str(sq).lower()] = str(piece)

            check_square = None
            if self.board in bc.CHECK:
                king_sq = _find_king_sq(self.board)
                if king_sq is not None:
                    check_square = str(king_sq).lower()

            return {
                "board": board_map,
                "legal_moves": legal_moves,
                "turn": "white" if self.board.turn is bc.WHITE else "black",
                "player_color": self.player_color,
                "depth": self.depth,
                "moves": self.moves,
                "san": self.san,
                "last_move": self.last_move,
                "check_square": check_square,
                "status": status,
                "result": result,
                "fen": self.board.fen(),
            }


@dataclass
class GameSession:
    game: Game
    last_seen: float


class App:
    def __init__(
        self,
        engine_path: str,
        depth: int,
        player_color: str,
        session_ttl_seconds: int,
        max_sessions: int,
    ):
        self.engine = Engine(engine_path)
        self.engine.newgame()
        self.default_depth = depth
        self.default_player_color = player_color
        self.session_ttl_seconds = max(60, session_ttl_seconds)
        self.max_sessions = max(1, max_sessions)
        self.sessions: dict[str, GameSession] = {}
        self.lock = threading.Lock()

    @staticmethod
    def valid_session_id(session_id: str | None) -> bool:
        if not session_id or len(session_id) > 128:
            return False
        return all(ch.isalnum() or ch in {"-", "_"} for ch in session_id)

    def _new_session_id_locked(self) -> str:
        while True:
            session_id = secrets.token_urlsafe(SESSION_ID_BYTES)
            if session_id not in self.sessions:
                return session_id

    def _prune_sessions_locked(self, now: float) -> None:
        expired = [
            session_id
            for session_id, session in self.sessions.items()
            if now - session.last_seen > self.session_ttl_seconds
        ]
        for session_id in expired:
            del self.sessions[session_id]

    def _enforce_session_cap_locked(self, protected_session_id: str) -> None:
        overflow = len(self.sessions) - self.max_sessions
        if overflow <= 0:
            return

        candidates = [
            item for item in self.sessions.items() if item[0] != protected_session_id
        ]
        oldest = sorted(candidates, key=lambda item: item[1].last_seen)
        for session_id, _ in oldest[:overflow]:
            del self.sessions[session_id]

    def _make_game(self, depth: int | None = None, player_color: str | None = None) -> Game:
        return Game(
            self.engine,
            self.default_depth if depth is None else depth,
            self.default_player_color if player_color is None else player_color,
        )

    def game_for_session(self, session_id: str | None) -> tuple[str, Game]:
        now = time.time()
        with self.lock:
            self._prune_sessions_locked(now)
            if not self.valid_session_id(session_id) or session_id not in self.sessions:
                session_id = self._new_session_id_locked()
                self.sessions[session_id] = GameSession(self._make_game(), now)
            else:
                self.sessions[session_id].last_seen = now
            self._enforce_session_cap_locked(session_id)
            return session_id, self.sessions[session_id].game

    def state_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, game = self.game_for_session(session_id)
        return session_id, game.state()

    def new_game(self, session_id: str | None, depth: int, player_color: str) -> tuple[str, dict[str, Any]]:
        now = time.time()
        with self.lock:
            self._prune_sessions_locked(now)
            if not self.valid_session_id(session_id) or session_id not in self.sessions:
                session_id = self._new_session_id_locked()
            self.sessions[session_id] = GameSession(self._make_game(depth, player_color), now)
            self._enforce_session_cap_locked(session_id)
            game = self.sessions[session_id].game
        return session_id, game.state()

    def make_move(self, session_id: str | None, move: str) -> tuple[str, dict[str, Any]]:
        session_id, game = self.game_for_session(session_id)
        game.make_player_move(move)
        return session_id, game.state()

    def close(self) -> None:
        self.engine.close()


def session_cookie_header(session_id: str, max_age: int) -> str:
    return f"{SESSION_COOKIE}={session_id}; Path=/; Max-Age={max_age}; HttpOnly; SameSite=Lax"


def json_response(
    handler: BaseHTTPRequestHandler,
    status: HTTPStatus,
    payload: dict[str, Any],
    *,
    session_id: str | None = None,
    session_max_age: int = DEFAULT_SESSION_TTL_SECONDS,
) -> None:
    body = json.dumps(payload).encode("utf-8")
    handler.send_response(status)
    handler.send_header("Content-Type", "application/json")
    handler.send_header("Content-Length", str(len(body)))
    if session_id is not None:
        handler.send_header("Set-Cookie", session_cookie_header(session_id, session_max_age))
    handler.end_headers()
    handler.wfile.write(body)


def make_handler(app: App):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: Any) -> None:
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

        def route_path(self) -> str:
            return self.path.split("?", 1)[0]

        def request_session_id(self) -> str | None:
            try:
                cookie = SimpleCookie(self.headers.get("Cookie", ""))
            except CookieError:
                return None
            morsel = cookie.get(SESSION_COOKIE)
            if morsel is None:
                return None
            return morsel.value

        def serve_index(self, include_body: bool, session_id: str | None = None) -> None:
            body = HTML.encode("utf-8")
            self.send_response(HTTPStatus.OK)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            if session_id is not None:
                self.send_header("Set-Cookie", session_cookie_header(session_id, app.session_ttl_seconds))
            self.end_headers()
            if include_body:
                self.wfile.write(body)

        def do_HEAD(self) -> None:
            path = self.route_path()
            if path == "/" or path == "/index.html":
                self.serve_index(include_body=False)
                return
            self.send_response(HTTPStatus.NOT_FOUND)
            self.end_headers()

        def do_GET(self) -> None:
            path = self.route_path()
            if path == "/" or path == "/index.html":
                session_id, _ = app.game_for_session(self.request_session_id())
                self.serve_index(include_body=True, session_id=session_id)
                return
            if path == "/api/state":
                session_id, payload = app.state_for_session(self.request_session_id())
                json_response(
                    self,
                    HTTPStatus.OK,
                    payload,
                    session_id=session_id,
                    session_max_age=app.session_ttl_seconds,
                )
                return
            json_response(self, HTTPStatus.NOT_FOUND, {"error": "Not found"})

        def do_POST(self) -> None:
            try:
                path = self.route_path()
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length) or b"{}")
                if path == "/api/new":
                    color = payload.get("player_color", "white")
                    if color not in {"white", "black"}:
                        raise ValueError("player_color must be white or black")
                    depth = max(1, min(5, int(payload.get("depth", 2))))
                    session_id, state = app.new_game(self.request_session_id(), depth, color)
                    json_response(
                        self,
                        HTTPStatus.OK,
                        state,
                        session_id=session_id,
                        session_max_age=app.session_ttl_seconds,
                    )
                    return
                if path == "/api/move":
                    move = str(payload.get("uci", ""))
                    session_id, state = app.make_move(self.request_session_id(), move)
                    json_response(
                        self,
                        HTTPStatus.OK,
                        state,
                        session_id=session_id,
                        session_max_age=app.session_ttl_seconds,
                    )
                    return
                json_response(self, HTTPStatus.NOT_FOUND, {"error": "Not found"})
            except Exception as exc:
                json_response(self, HTTPStatus.BAD_REQUEST, {"error": str(exc)})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--engine", default="./build/lulzfish")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--color", choices=["white", "black"], default="white")
    parser.add_argument("--session-ttl", type=int, default=DEFAULT_SESSION_TTL_SECONDS)
    parser.add_argument("--max-sessions", type=int, default=DEFAULT_MAX_SESSIONS)
    args = parser.parse_args()

    engine_path = str(Path(args.engine).resolve())
    if not Path(engine_path).exists():
        raise SystemExit(f"Engine not found: {engine_path}")

    app = App(
        engine_path,
        max(1, min(5, args.depth)),
        args.color,
        args.session_ttl,
        args.max_sessions,
    )
    server = ThreadingHTTPServer((args.host, args.port), make_handler(app))
    print(f"Lulzfish GUI: http://{args.host}:{args.port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.close()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
