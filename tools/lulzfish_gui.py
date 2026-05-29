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
from dataclasses import dataclass, field
from collections.abc import Callable
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
      --accent-dark: #244e39;
      --eval-white: #f4f0e6;
      --eval-black: #3a4038;
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

    .board-column {
      display: grid;
      grid-template-columns: 18px 1fr;
      gap: 10px;
      width: 100%;
      max-width: 760px;
    }

    .eval-bar {
      position: relative;
      border: 1px solid var(--line);
      background: var(--eval-black);
      min-height: 200px;
      overflow: hidden;
    }

    .eval-bar-fill {
      position: absolute;
      left: 0;
      right: 0;
      bottom: 0;
      height: 50%;
      background: var(--eval-white);
      transition: height 180ms ease;
    }

    .board-wrap {
      width: 100%;
    }

    .board-frame {
      position: relative;
      border: 1px solid rgba(32,35,31,0.18);
      background: #283025;
      padding: clamp(8px, 1.4vw, 14px);
      box-shadow: 0 22px 70px rgba(32,35,31,0.18);
    }

    .board-overlay {
      position: absolute;
      inset: clamp(8px, 1.4vw, 14px);
      pointer-events: none;
      z-index: 4;
    }

    .board-overlay line {
      stroke-width: 2.5;
      stroke-linecap: round;
      opacity: 0.72;
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

    .toggle-row {
      display: flex;
      flex-wrap: wrap;
      gap: 8px;
      align-items: center;
    }

    .chip {
      border: 1px solid var(--line);
      background: var(--panel);
      color: var(--ink);
      padding: 8px 12px;
      min-height: 38px;
      cursor: pointer;
      font-weight: 650;
    }

    .chip.active {
      border-color: var(--accent-dark);
      background: var(--accent);
      color: #fff;
    }

    .features {
      border: 1px solid var(--line);
      background: var(--panel);
    }

    .features summary {
      cursor: pointer;
      padding: 10px 12px;
      font-weight: 650;
      color: var(--muted);
    }

    .feature-chart {
      display: grid;
      gap: 6px;
      padding: 0 12px 12px;
      max-height: 220px;
      overflow: auto;
    }

    .feature-row {
      display: grid;
      grid-template-columns: 28px 1fr 48px;
      gap: 8px;
      align-items: center;
      font-size: 12px;
    }

    .feature-bar-wrap {
      height: 8px;
      background: #eceee6;
      border-radius: 2px;
      overflow: hidden;
    }

    .feature-bar {
      height: 100%;
      background: var(--accent);
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

    .eval-text {
      color: var(--accent-dark);
      font-size: 15px;
      font-weight: 650;
      font-variant-numeric: tabular-nums;
    }

    .thinking {
      border: 1px solid var(--line);
      background: rgba(255,255,255,0.72);
      padding: 10px 12px;
      font-size: 13px;
      display: none;
    }

    .thinking.open { display: block; }

    .thinking table {
      width: 100%;
      border-collapse: collapse;
      font-variant-numeric: tabular-nums;
    }

    .thinking th {
      text-align: left;
      color: var(--muted);
      font-weight: 650;
      padding: 2px 8px 6px 0;
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
    <section class="board-column" aria-label="Chess board">
      <div class="eval-bar" aria-label="Evaluation bar">
        <div id="evalBarFill" class="eval-bar-fill"></div>
      </div>
      <div class="board-wrap">
        <div class="board-frame">
          <div id="board" class="board"></div>
          <svg id="graphOverlay" class="board-overlay" viewBox="0 0 100 100" preserveAspectRatio="none"></svg>
        </div>
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
        <div id="evalText" class="eval-text">+0.00</div>
      </div>

      <div class="toggle-row">
        <button type="button" id="toggleGraph" class="chip">Graph</button>
        <button type="button" id="toggleHeatmap" class="chip">Heatmap</button>
        <button type="button" id="refreshViz" class="chip">Refresh viz</button>
      </div>

      <div id="thinking" class="thinking">
        <table>
          <thead>
            <tr><th>Depth</th><th>Score</th><th>Nodes</th><th>NPS</th></tr>
          </thead>
          <tbody>
            <tr>
              <td id="thinkDepth">—</td>
              <td id="thinkScore">—</td>
              <td id="thinkNodes">—</td>
              <td id="thinkNps">—</td>
            </tr>
          </tbody>
        </table>
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
          <input id="depth" type="number" min="1" max="6" value="2">
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

      <details id="featuresPanel" class="features">
        <summary>Feature vector (dev)</summary>
        <div id="featureChart" class="feature-chart"></div>
      </details>
    </section>
  </main>

  <script type="module">
    import { Chessground } from "https://unpkg.com/chessground@9.2.1/dist/chessground.js";

    const RELATION_COLORS = {
      ATTACKS: "#2f6f4e",
      DEFENDS: "#4a7c9b",
      PINS: "#a24d3d",
      DISCOVERED_ATTACK: "#8b5a2b",
      PAWN_CHAIN: "#6d7467",
      KING_ZONE: "#b9463f",
    };

    let ground = null;
    let pollTimer = null;
    const state = {
      legal: [],
      playerColor: "white",
      orientation: "white",
      pendingPromotion: null,
      lastMove: null,
      fen: "startpos",
      turn: "white",
      checkSquare: null,
      busy: false,
      score: 0,
      pv: [],
      showGraph: false,
      showHeatmap: false,
      graphRelations: [],
      attackHeatmap: null,
    };

    const boardEl = document.getElementById("board");
    const graphOverlay = document.getElementById("graphOverlay");
    const featureChart = document.getElementById("featureChart");
    const statusMain = document.getElementById("statusMain");
    const statusSub = document.getElementById("statusSub");
    const evalText = document.getElementById("evalText");
    const evalBarFill = document.getElementById("evalBarFill");
    const thinkingEl = document.getElementById("thinking");
    const thinkDepth = document.getElementById("thinkDepth");
    const thinkScore = document.getElementById("thinkScore");
    const thinkNodes = document.getElementById("thinkNodes");
    const thinkNps = document.getElementById("thinkNps");
    const movesEl = document.getElementById("moves");
    const errorEl = document.getElementById("error");
    const colorEl = document.getElementById("color");
    const depthEl = document.getElementById("depth");
    const promotionEl = document.getElementById("promotion");
    const toggleGraph = document.getElementById("toggleGraph");
    const toggleHeatmap = document.getElementById("toggleHeatmap");

    function scoreCpWhite(score, turn) {
      const abs = Math.abs(score || 0);
      if (abs > 25000) {
        const matePly = 30000 - abs;
        const sign = score > 0 ? 1 : -1;
        const mateForWhite = turn === "white" ? sign : -sign;
        return mateForWhite > 0 ? 800 - matePly : -800 + matePly;
      }
      return turn === "white" ? score : -score;
    }

    function formatEval(score, turn) {
      const cpWhite = scoreCpWhite(score, turn);
      const abs = Math.abs(score || 0);
      if (abs > 25000) {
        const mateIn = 30000 - abs;
        return cpWhite > 0 ? `#${mateIn}` : `#-${mateIn}`;
      }
      const pawns = cpWhite / 100;
      return `${pawns >= 0 ? "+" : ""}${pawns.toFixed(2)}`;
    }

    function updateEvalBar(score, turn) {
      const cpWhite = scoreCpWhite(score, turn);
      const clamped = Math.max(-800, Math.min(800, cpWhite));
      const pct = 50 + (clamped / 800) * 50;
      evalBarFill.style.height = `${pct}%`;
      evalText.textContent = formatEval(score, turn);
    }

    function pvToShapes(pv) {
      if (!pv?.length) return [];
      return pv.filter((uci) => uci && uci.length >= 4).map((uci, i) => ({
        orig: uci.slice(0, 2),
        dest: uci.slice(2, 4),
        brush: i === 0 ? "green" : "paleGreen",
      }));
    }

    function updateThinking(info) {
      if (!info || !Object.keys(info).length) {
        if (!state.busy) thinkingEl.classList.remove("open");
        return;
      }
      thinkingEl.classList.add("open");
      thinkDepth.textContent = info.depth ?? "—";
      thinkScore.textContent = formatEval(info.score, state.turn);
      thinkNodes.textContent = info.nodes != null ? Number(info.nodes).toLocaleString() : "—";
      const nps = info.nps ?? (info.time_ms > 0 && info.nodes != null
        ? Math.floor((info.nodes * 1000) / info.time_ms)
        : null);
      thinkNps.textContent = nps != null ? Number(nps).toLocaleString() : "—";
      if (info.score != null) {
        updateEvalBar(info.score, state.turn);
      }
    }

    function squareCenterPct(sq, orientation) {
      const file = sq.charCodeAt(0) - 97;
      let rank = Number(sq[1]) - 1;
      if (orientation === "black") {
        return { x: (7 - file) * 12.5 + 6.25, y: rank * 12.5 + 6.25 };
      }
      return { x: file * 12.5 + 6.25, y: (7 - rank) * 12.5 + 6.25 };
    }

    function renderGraphOverlay() {
      graphOverlay.innerHTML = "";
      if (!state.showGraph || !state.graphRelations.length) return;
      for (const rel of state.graphRelations) {
        const from = squareCenterPct(rel.from, state.orientation);
        const to = squareCenterPct(rel.to, state.orientation);
        const line = document.createElementNS("http://www.w3.org/2000/svg", "line");
        line.setAttribute("x1", String(from.x));
        line.setAttribute("y1", String(from.y));
        line.setAttribute("x2", String(to.x));
        line.setAttribute("y2", String(to.y));
        line.setAttribute("stroke", RELATION_COLORS[rel.type] || "#6d7467");
        graphOverlay.appendChild(line);
      }
    }

    function applyHeatmapTint() {
      boardEl.querySelectorAll("square").forEach((el) => {
        el.style.removeProperty("box-shadow");
      });
      if (!state.showHeatmap || !state.attackHeatmap) return;
      const side = state.turn === "white" ? state.attackHeatmap.white : state.attackHeatmap.black;
      const max = Math.max(1, ...Object.values(side || {}));
      for (const [sq, count] of Object.entries(side || {})) {
        const node = boardEl.querySelector(`square.${sq}`);
        if (!node) continue;
        const alpha = 0.1 + (count / max) * 0.38;
        node.style.boxShadow = `inset 0 0 0 999px rgba(47,111,78,${alpha})`;
      }
    }

    async function refreshVisualization() {
      if (state.showGraph) {
        const graph = await api("/api/graph");
        state.graphRelations = graph.relations || [];
      }
      if (state.showHeatmap) {
        const heat = await api("/api/heatmap");
        state.attackHeatmap = heat;
      }
      renderBoard();
    }

    async function refreshFeatures() {
      const data = await api("/api/features");
      const features = data.features || [];
      const indexed = features.map((value, index) => ({ index, value: Math.abs(value), raw: value }));
      indexed.sort((a, b) => b.value - a.value);
      const top = indexed.slice(0, 12);
      const max = top[0]?.value || 1;
      featureChart.innerHTML = top.map((item) => {
        const width = Math.max(4, (item.value / max) * 100);
        return `<div class="feature-row">
          <span>${item.index}</span>
          <div class="feature-bar-wrap"><div class="feature-bar" style="width:${width}%"></div></div>
          <span>${item.raw.toFixed(3)}</span>
        </div>`;
      }).join("");
    }

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
        drawable: {
          enabled: true,
          visible: true,
          autoShapes: pvToShapes(state.pv),
        },
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
      applyHeatmapTint();
      renderGraphOverlay();
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
        thinkingEl.classList.add("open");
      }
    }

    function stopPolling() {
      if (pollTimer) {
        clearTimeout(pollTimer);
        pollTimer = null;
      }
    }

    async function pollThinking() {
      stopPolling();
      while (state.busy) {
        try {
          const data = await api("/api/thinking");
          if (data.search_info) {
            state.pv = data.search_info.pv || [];
            updateThinking(data.search_info);
            renderBoard();
          }
          if (!data.thinking) {
            if (data.error) errorEl.textContent = data.error;
            const fresh = await api("/api/state");
            updateFromServer(fresh);
            await refreshVisualization();
            if (document.getElementById("featuresPanel").open) await refreshFeatures();
            break;
          }
        } catch (err) {
          errorEl.textContent = err.message;
          break;
        }
        await new Promise((resolve) => {
          pollTimer = setTimeout(resolve, 120);
        });
      }
    }

    function updateFromServer(data) {
      state.legal = data.legal_moves;
      state.playerColor = data.player_color;
      state.lastMove = data.last_move;
      state.checkSquare = data.check_square;
      state.fen = data.fen;
      state.turn = data.turn;
      state.score = data.score ?? 0;
      state.pv = data.pv || data.search_info?.pv || [];
      colorEl.value = data.player_color;
      depthEl.value = data.depth;

      statusMain.textContent = data.status;
      statusSub.textContent = data.result || `${data.turn === "white" ? "White" : "Black"} to move`;
      const info = data.search_info || {};
      if (info.score != null) updateEvalBar(info.score, state.turn);
      else updateEvalBar(state.score, state.turn);
      updateThinking(info);
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

    async function waitForEngine(data) {
      updateFromServer(data);
      if (data.thinking) {
        setBusy(true);
        await pollThinking();
        setBusy(false);
      } else {
        await refreshVisualization();
        if (document.getElementById("featuresPanel").open) await refreshFeatures();
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
        await waitForEngine(data);
      } catch (err) {
        errorEl.textContent = err.message;
      } finally {
        setBusy(false);
        stopPolling();
      }
    }

    async function sendMove(uci) {
      setBusy(true);
      state.pendingPromotion = null;
      promotionEl.classList.remove("open");
      try {
        await waitForEngine(await api("/api/move", {uci}));
      } catch (err) {
        errorEl.textContent = err.message;
        renderBoard();
      } finally {
        setBusy(false);
        stopPolling();
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

    toggleGraph.addEventListener("click", async () => {
      state.showGraph = !state.showGraph;
      toggleGraph.classList.toggle("active", state.showGraph);
      if (state.showGraph) await refreshVisualization();
      else {
        state.graphRelations = [];
        renderGraphOverlay();
      }
    });

    toggleHeatmap.addEventListener("click", async () => {
      state.showHeatmap = !state.showHeatmap;
      toggleHeatmap.classList.toggle("active", state.showHeatmap);
      if (state.showHeatmap) await refreshVisualization();
      else renderBoard();
    });

    document.getElementById("refreshViz").addEventListener("click", () => refreshVisualization());
    document.getElementById("featuresPanel").addEventListener("toggle", () => {
      if (document.getElementById("featuresPanel").open) refreshFeatures();
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

    @staticmethod
    def _parse_info_line(line: str) -> dict[str, Any]:
        tokens = line.split()
        if len(tokens) < 4 or tokens[0] != "info":
            return {}
        info: dict[str, Any] = {}
        idx = 1
        while idx < len(tokens):
            key = tokens[idx]
            if key == "depth" and idx + 1 < len(tokens):
                info["depth"] = int(tokens[idx + 1])
                idx += 2
            elif key == "score" and idx + 2 < len(tokens) and tokens[idx + 1] == "cp":
                info["score"] = int(tokens[idx + 2])
                idx += 3
            elif key == "nodes" and idx + 1 < len(tokens):
                info["nodes"] = int(tokens[idx + 1])
                idx += 2
            elif key == "nps" and idx + 1 < len(tokens):
                info["nps"] = int(tokens[idx + 1])
                idx += 2
            elif key == "time" and idx + 1 < len(tokens):
                info["time_ms"] = int(tokens[idx + 1])
                idx += 2
            elif key == "pv":
                info["pv"] = tokens[idx + 1 :]
                idx = len(tokens)
            else:
                idx += 1
        return info

    def set_position(self, moves: list[str]) -> None:
        with self.lock:
            position = "position startpos"
            if moves:
                position += " moves " + " ".join(moves)
            self._send(position)

    def _read_json_line(self, prefix: str) -> dict[str, Any]:
        assert self.proc.stdout is not None
        deadline = time.time() + 10.0
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                continue
            line = line.strip()
            if line.startswith(prefix):
                return json.loads(line[len(prefix) :])
        raise TimeoutError(f"engine did not return {prefix.strip()}")

    def graph(self) -> dict[str, Any]:
        with self.lock:
            self._send("graph")
            return self._read_json_line("graph ")

    def heatmap(self) -> dict[str, Any]:
        with self.lock:
            self._send("heatmap")
            return self._read_json_line("heatmap ")

    def features(self) -> list[float]:
        with self.lock:
            self._send("features")
            assert self.proc.stdout is not None
            deadline = time.time() + 10.0
            while time.time() < deadline:
                line = self.proc.stdout.readline()
                if not line:
                    continue
                line = line.strip()
                if line.startswith("features "):
                    return [float(token) for token in line.split()[1:]]
            raise TimeoutError("engine did not return features")

    def bestmove(
        self,
        moves: list[str],
        depth: int,
        on_info: Callable[[dict[str, Any]], None] | None = None,
    ) -> tuple[str, dict[str, Any]]:
        with self.lock:
            position = "position startpos"
            if moves:
                position += " moves " + " ".join(moves)
            self._send(position)
            self._send(f"go depth {depth}")

            assert self.proc.stdout is not None
            deadline = time.time() + 30.0
            search_info: dict[str, Any] = {}
            while time.time() < deadline:
                line = self.proc.stdout.readline()
                if not line:
                    continue
                line = line.strip()
                if line.startswith("info "):
                    parsed = self._parse_info_line(line)
                    if parsed:
                        search_info = parsed
                        if on_info is not None:
                            on_info(parsed)
                if line.startswith("bestmove "):
                    return line.split()[1], search_info
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
        self.last_search_info: dict[str, Any] = {}
        self.lock = threading.Lock()

    def engine_to_move(self) -> bool:
        engine_is_white = self.player_color == "black"
        return self.board.turn is bc.WHITE if engine_is_white else self.board.turn is bc.BLACK

    def is_game_over(self) -> bool:
        return _status_and_result(self.board)[0] == "Game over"

    def player_to_move(self) -> bool:
        return not self.is_game_over() and not self.engine_to_move()

    def apply_engine_move(self, move_text: str, search_info: dict[str, Any]) -> None:
        self.last_search_info = search_info
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

            score = int(self.last_search_info.get("score", 0))
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
                "score": score,
                "search_info": self.last_search_info,
                "pv": self.last_search_info.get("pv", []),
            }


@dataclass
class GameSession:
    game: Game
    last_seen: float
    thinking: bool = False
    search_info: dict[str, Any] = field(default_factory=dict)
    thinking_error: str | None = None
    session_lock: threading.Lock = field(default_factory=threading.Lock)


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

    def _public_state(self, session: GameSession) -> dict[str, Any]:
        state = session.game.state()
        with session.session_lock:
            state["thinking"] = session.thinking
            if session.thinking:
                state["search_info"] = dict(session.search_info)
                state["pv"] = session.search_info.get("pv", [])
            if session.thinking_error:
                state["thinking_error"] = session.thinking_error
        return state

    def _start_engine_think(self, session: GameSession) -> None:
        with session.game.lock:
            if session.game.is_game_over() or not session.game.engine_to_move():
                return
            moves = list(session.game.moves)
            depth = session.game.depth

        with session.session_lock:
            if session.thinking:
                return
            session.thinking = True
            session.thinking_error = None
            session.search_info = {}

        def worker() -> None:
            try:
                def on_info(info: dict[str, Any]) -> None:
                    with session.session_lock:
                        session.search_info = dict(info)

                move_text, search_info = self.engine.bestmove(moves, depth, on_info=on_info)
                with session.game.lock:
                    session.game.apply_engine_move(move_text, search_info)
            except Exception as exc:
                with session.session_lock:
                    session.thinking_error = str(exc)
            finally:
                with session.session_lock:
                    session.thinking = False

        threading.Thread(target=worker, daemon=True).start()

    def _maybe_start_engine(self, session: GameSession) -> None:
        with session.game.lock:
            needs_engine = not session.game.is_game_over() and session.game.engine_to_move()
        if needs_engine:
            self._start_engine_think(session)

    def game_for_session(self, session_id: str | None) -> tuple[str, GameSession]:
        now = time.time()
        with self.lock:
            self._prune_sessions_locked(now)
            if not self.valid_session_id(session_id) or session_id not in self.sessions:
                session_id = self._new_session_id_locked()
                self.sessions[session_id] = GameSession(self._make_game(), now)
            else:
                self.sessions[session_id].last_seen = now
            self._enforce_session_cap_locked(session_id)
            return session_id, self.sessions[session_id]

    def state_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        return session_id, self._public_state(session)

    def thinking_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        with session.session_lock:
            payload: dict[str, Any] = {
                "thinking": session.thinking,
                "search_info": dict(session.search_info),
            }
            if session.thinking_error:
                payload["error"] = session.thinking_error
        return session_id, payload

    def graph_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        with session.game.lock:
            moves = list(session.game.moves)
        self.engine.set_position(moves)
        return session_id, self.engine.graph()

    def heatmap_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        with session.game.lock:
            moves = list(session.game.moves)
        self.engine.set_position(moves)
        return session_id, self.engine.heatmap()

    def features_for_session(self, session_id: str | None) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        with session.game.lock:
            moves = list(session.game.moves)
        self.engine.set_position(moves)
        return session_id, {"features": self.engine.features()}

    def new_game(self, session_id: str | None, depth: int, player_color: str) -> tuple[str, dict[str, Any]]:
        now = time.time()
        with self.lock:
            self._prune_sessions_locked(now)
            if not self.valid_session_id(session_id) or session_id not in self.sessions:
                session_id = self._new_session_id_locked()
            self.sessions[session_id] = GameSession(self._make_game(depth, player_color), now)
            self._enforce_session_cap_locked(session_id)
            session = self.sessions[session_id]
        self._maybe_start_engine(session)
        return session_id, self._public_state(session)

    def make_move(self, session_id: str | None, move: str) -> tuple[str, dict[str, Any]]:
        session_id, session = self.game_for_session(session_id)
        session.game.make_player_move(move)
        self._maybe_start_engine(session)
        return session_id, self._public_state(session)

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
            if path == "/api/thinking":
                session_id, payload = app.thinking_for_session(self.request_session_id())
                json_response(
                    self,
                    HTTPStatus.OK,
                    payload,
                    session_id=session_id,
                    session_max_age=app.session_ttl_seconds,
                )
                return
            if path == "/api/graph":
                session_id, payload = app.graph_for_session(self.request_session_id())
                json_response(
                    self,
                    HTTPStatus.OK,
                    payload,
                    session_id=session_id,
                    session_max_age=app.session_ttl_seconds,
                )
                return
            if path == "/api/heatmap":
                session_id, payload = app.heatmap_for_session(self.request_session_id())
                json_response(
                    self,
                    HTTPStatus.OK,
                    payload,
                    session_id=session_id,
                    session_max_age=app.session_ttl_seconds,
                )
                return
            if path == "/api/features":
                session_id, payload = app.features_for_session(self.request_session_id())
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
                    depth = max(1, min(6, int(payload.get("depth", 2))))
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
        max(1, min(6, args.depth)),
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
