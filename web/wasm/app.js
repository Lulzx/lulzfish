import { Chessground } from "https://unpkg.com/chessground@9.2.1/dist/chessground.js";

let ground = null;
let requestId = 0;

const START_FEN = "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1";
const START_BOARD_FEN = START_FEN.split(" ")[0];

const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
const pending = new Map();

const RELATION_COLORS = {
  ATTACKS: "#2f6f4e",
  DEFENDS: "#3d6ea8",
  PINS: "#a24d3d",
  DISCOVERED_ATTACK: "#8b5a2b",
  PAWN_CHAIN: "#6d7467",
  KING_ZONE: "#7a4d8f",
};

const state = {
  legal: [],
  playerColor: "white",
  orientation: "white",
  pendingPromotion: null,
  lastMove: null,
  fen: START_FEN,
  turn: "white",
  checkSquare: null,
  result: "",
  moves: [],
  score: 0,
  searchInfo: null,
  pv: [],
  evalHistory: [],
  busy: true,
  mode: "play",
  showGraph: false,
  showHeatmap: false,
  graphRelations: [],
  attackHeatmap: null,
  hintArrow: null,
};

const boardEl = document.getElementById("board");
const statusMain = document.getElementById("statusMain");
const statusSub = document.getElementById("statusSub");
const evalText = document.getElementById("evalText");
const evalBarFill = document.getElementById("evalBarFill");
const movesEl = document.getElementById("moves");
const errorEl = document.getElementById("error");
const colorEl = document.getElementById("color");
const depthEl = document.getElementById("depth");
const promotionEl = document.getElementById("promotion");
const newGameEl = document.getElementById("newGame");
const thinkingEl = document.getElementById("thinking");
const thinkDepth = document.getElementById("thinkDepth");
const thinkScore = document.getElementById("thinkScore");
const thinkNodes = document.getElementById("thinkNodes");
const thinkNps = document.getElementById("thinkNps");
const graphOverlay = document.getElementById("graphOverlay");
const featureChart = document.getElementById("featureChart");
const toggleGraph = document.getElementById("toggleGraph");
const toggleHeatmap = document.getElementById("toggleHeatmap");

worker.onmessage = (event) => {
  const { id, ok, data, error, progress, search } = event.data || {};
  const callbacks = pending.get(id);
  if (!callbacks) return;

  if (progress && search) {
    updateThinking(search);
    callbacks.onProgress?.(search);
    return;
  }

  pending.delete(id);
  if (ok) callbacks.resolve(data);
  else callbacks.reject(new Error(error || "Worker request failed"));
};

function askWorker(type, payload = {}, options = {}) {
  const id = ++requestId;
  return new Promise((resolve, reject) => {
    pending.set(id, {
      resolve,
      reject,
      onProgress: options.onProgress,
    });
    worker.postMessage({
      id,
      type,
      payload: { ...payload, onProgress: Boolean(options.onProgress) },
    });
  });
}

function askWorkerProgress(type, payload = {}, onProgress) {
  return askWorker(type, payload, { onProgress });
}

function legalDests() {
  const dests = new Map();
  if (state.result || state.busy) return dests;
  if (state.mode === "play" && state.turn !== state.playerColor) return dests;
  for (const move of state.legal) {
    const from = move.slice(0, 2);
    const to = move.slice(2, 4);
    const current = dests.get(from) || [];
    if (!current.includes(to)) current.push(to);
    dests.set(from, current);
  }
  return dests;
}

function boardFen() {
  return state.fen === "startpos" ? START_BOARD_FEN : state.fen.split(" ")[0];
}

function scoreCpWhite(score, turn) {
  if (score == null) return 0;
  const abs = Math.abs(score);
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
  const abs = Math.abs(score ?? 0);
  if (abs > 25000) {
    const mateIn = 30000 - abs;
    return cpWhite > 0 ? `#${mateIn}` : `#-${mateIn}`;
  }
  const pawns = cpWhite / 100;
  return `${pawns >= 0 ? "+" : ""}${pawns.toFixed(2)}`;
}

function updateEvalBar(score, turn) {
  const cpWhite = scoreCpWhite(score ?? 0, turn);
  const clamped = Math.max(-800, Math.min(800, cpWhite));
  const pct = 50 + (clamped / 800) * 50;
  evalBarFill.style.height = `${pct}%`;
  evalText.textContent = formatEval(score, turn);
}

function updateThinking(info) {
  if (!info) return;
  thinkingEl.classList.add("open");
  thinkDepth.textContent = info.depth ?? "—";
  thinkScore.textContent = formatEval(info.score, state.turn);
  thinkNodes.textContent = info.nodes != null ? Number(info.nodes).toLocaleString() : "—";
  thinkNps.textContent = info.nps != null ? Number(info.nps).toLocaleString() : "—";
  if (info.score != null) {
    updateEvalBar(info.score, state.turn);
  }
}

function pvToShapes(pv, brush = "paleGreen") {
  if (!pv?.length) return [];
  const shapes = [];
  for (let i = 0; i < pv.length; i += 1) {
    const uci = pv[i];
    if (!uci || uci.length < 4) continue;
    shapes.push({
      orig: uci.slice(0, 2),
      dest: uci.slice(2, 4),
      brush: i === 0 ? "green" : brush,
    });
  }
  return shapes;
}

function hintShape() {
  if (!state.hintArrow) return [];
  const uci = state.hintArrow;
  return [{ orig: uci.slice(0, 2), dest: uci.slice(2, 4), brush: "blue" }];
}

function squareCenterPct(sq, orientation) {
  const file = sq.charCodeAt(0) - 97;
  let rank = Number(sq[1]) - 1;
  if (orientation === "black") {
    return {
      x: (7 - file) * 12.5 + 6.25,
      y: rank * 12.5 + 6.25,
    };
  }
  return {
    x: file * 12.5 + 6.25,
    y: (7 - rank) * 12.5 + 6.25,
  };
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

function renderBoard() {
  const shapes = [
    ...pvToShapes(state.pv),
    ...hintShape(),
  ];

  const config = {
    fen: boardFen(),
    orientation: state.orientation,
    turnColor: state.turn,
    check: state.checkSquare || false,
    lastMove: state.lastMove ? [state.lastMove.slice(0, 2), state.lastMove.slice(2, 4)] : undefined,
    coordinates: true,
    drawable: {
      enabled: true,
      visible: true,
      autoShapes: shapes,
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
      color: state.result || state.busy ? undefined : (state.mode === "play" ? state.playerColor : state.turn),
      dests: legalDests(),
      showDests: true,
      events: {
        after: onGroundMove,
      },
    },
    premovable: { enabled: false },
    draggable: {
      enabled: !state.busy,
      showGhost: true,
    },
  };

  if (!ground) ground = Chessground(boardEl, config);
  else ground.set(config);

  applyHeatmapTint();
  renderGraphOverlay();
}

function applyHeatmapTint() {
  const squares = boardEl.querySelectorAll("square");
  squares.forEach((el) => {
    el.classList.remove("attack-heat");
    el.style.removeProperty("box-shadow");
  });
  if (!state.showHeatmap || !state.attackHeatmap) return;

  const side = state.turn === "white" ? state.attackHeatmap.white : state.attackHeatmap.black;
  const max = Math.max(1, ...Object.values(side || {}));
  for (const [sq, count] of Object.entries(side || {})) {
    const node = boardEl.querySelector(`square.${sq}`);
    if (!node) continue;
    const alpha = 0.1 + (count / max) * 0.38;
    node.classList.add("attack-heat");
    node.style.boxShadow = `inset 0 0 0 999px rgba(47,111,78,${alpha})`;
  }
}

function renderInteractivityOnly() {
  if (!ground) {
    renderBoard();
    return;
  }
  ground.set({
    movable: {
      free: false,
      color: state.result || state.busy ? undefined : (state.mode === "play" ? state.playerColor : state.turn),
      dests: legalDests(),
      showDests: true,
      events: { after: onGroundMove },
    },
    draggable: { enabled: !state.busy, showGhost: true },
  });
}

function renderMoves(moves, evalHistory) {
  if (!moves.length) {
    movesEl.innerHTML = '<span class="moves-empty">No moves yet.</span>';
    return;
  }
  const rows = [];
  for (let i = 0; i < moves.length; i += 2) {
    const moveNo = Math.floor(i / 2) + 1;
    const wEval = evalHistory[i] != null ? ` (${formatEval(evalHistory[i], "white")})` : "";
    const bEval = evalHistory[i + 1] != null ? ` (${formatEval(evalHistory[i + 1], "black")})` : "";
    rows.push(`${moveNo}. ${moves[i]}${wEval}${moves[i + 1] ? " " + moves[i + 1] + bEval : ""}`);
  }
  movesEl.textContent = rows.join("  ");
  movesEl.scrollTop = movesEl.scrollHeight;
}

function setBusy(isBusy, label = "", options = {}) {
  state.busy = isBusy;
  newGameEl.disabled = isBusy;
  colorEl.disabled = isBusy;
  depthEl.disabled = isBusy;
  document.querySelectorAll(".mode-chip").forEach((btn) => {
    btn.disabled = isBusy;
  });
  errorEl.textContent = "";
  if (isBusy) {
    statusSub.textContent = label || "Engine is thinking.";
    thinkingEl.classList.add("open");
  } else {
    thinkingEl.classList.remove("open");
  }
  if (options.preserveBoardPosition) renderInteractivityOnly();
  else renderBoard();
}

function updateFromEngine(data) {
  state.legal = data.legal_moves || [];
  state.playerColor = data.player_color || state.playerColor;
  state.lastMove = data.last_move || null;
  state.checkSquare = data.check_square || null;
  state.fen = data.fen || START_FEN;
  state.turn = data.turn || "white";
  state.result = data.result || "";
  state.moves = data.moves || [];
  state.score = data.score ?? 0;
  if (data.search_info) {
    state.searchInfo = data.search_info;
    state.pv = data.search_info.pv || [];
    updateThinking(data.search_info);
  }
  colorEl.value = state.playerColor;
  depthEl.value = data.depth || depthEl.value;

  updateEvalBar(state.score, state.turn);

  if (state.result) {
    statusMain.textContent = "Game over";
    statusSub.textContent = state.result;
  } else if (state.mode === "analyze") {
    statusMain.textContent = "Analyze mode";
    statusSub.textContent = `${state.turn === "white" ? "White" : "Black"} to move · depth ${depthEl.value}`;
  } else if (state.mode === "hint") {
    statusMain.textContent = "Hint mode";
    statusSub.textContent = "Best move shown as blue arrow";
  } else if (state.turn === state.playerColor) {
    statusMain.textContent = "Your move";
    statusSub.textContent = `${state.turn === "white" ? "White" : "Black"} to move`;
  } else {
    statusMain.textContent = "Lulzfish to move";
    statusSub.textContent = `Depth ${depthEl.value}`;
  }

  renderMoves(state.moves, state.evalHistory);
  renderBoard();
}

async function refreshVisualization() {
  if (state.showGraph) {
    const graph = await askWorker("getGraph");
    state.graphRelations = graph.relations || [];
  }
  if (state.showHeatmap) {
    state.attackHeatmap = await askWorker("getAttackHeatmap");
  }
  renderBoard();
  renderGraphOverlay();
}

async function refreshFeatures() {
  const data = await askWorker("getFeatures");
  const features = data.features || [];
  const indexed = features.map((value, index) => ({ index, value: Math.abs(value), raw: value }));
  indexed.sort((a, b) => b.value - a.value);
  const top = indexed.slice(0, 12);
  const max = top[0]?.value || 1;
  featureChart.innerHTML = top
    .map((item) => {
      const width = Math.max(4, (item.value / max) * 100);
      return `<div class="feature-row">
        <span>${item.index}</span>
        <div class="feature-bar-wrap"><div class="feature-bar" style="width:${width}%"></div></div>
        <span>${item.raw.toFixed(3)}</span>
      </div>`;
    })
    .join("");
}

async function runAnalyze(depth, options = {}) {
  const info = await askWorkerProgress(
    "analyze",
    { depth, progressive: true },
    (search) => {
      state.pv = search.pv || [];
      updateThinking(search);
      if (search?.score != null) updateEvalBar(search.score, state.turn);
      renderBoard();
    }
  );
  state.searchInfo = info;
  state.pv = info?.pv || [];
  updateThinking(info);
  renderBoard();
  return info;
}

async function runHint() {
  const info = await askWorker("hint", { depth: Math.min(3, Number(depthEl.value || 2)) });
  state.hintArrow = info?.pv?.[0] || info?.best_move || null;
  renderBoard();
}

async function afterUserMove(data) {
  if (data.search_info?.score != null) {
    state.evalHistory.push(data.search_info.score);
  } else if (data.score != null) {
    state.evalHistory.push(data.score);
  }

  updateFromEngine(data);

  if (state.result) return;

  if (state.mode === "analyze") {
    setBusy(true, "Analyzing position.", { preserveBoardPosition: true });
    try {
      await runAnalyze(Number(depthEl.value || 2));
      await refreshVisualization();
      if (document.getElementById("featuresPanel").open) await refreshFeatures();
    } finally {
      setBusy(false);
    }
    return;
  }

  if (state.mode === "hint") {
    setBusy(true, "Computing hint.", { preserveBoardPosition: true });
    try {
      await runHint();
    } finally {
      setBusy(false);
    }
    return;
  }

  if (state.turn !== state.playerColor) {
    setBusy(true, "Lulzfish is thinking.", { preserveBoardPosition: true });
    try {
      const next = await askWorkerProgress("engineMove", {}, (search) => {
        state.pv = search.pv || [];
        updateThinking(search);
        renderBoard();
      });
      if (next.search_info?.score != null) {
        state.evalHistory.push(next.search_info.score);
      }
      updateFromEngine(next);
      await refreshVisualization();
    } finally {
      setBusy(false);
    }
  }
}

async function runRequest(action, busyLabel, options = {}) {
  setBusy(true, busyLabel, options);
  state.pendingPromotion = null;
  promotionEl.classList.remove("open");
  try {
    const data = await action();
    state.evalHistory = [];
    state.hintArrow = null;
    updateFromEngine(data);
    if (state.mode === "analyze" && !state.result) {
      await runAnalyze(Number(depthEl.value || 2));
    }
    await refreshVisualization();
    if (document.getElementById("featuresPanel").open) await refreshFeatures();
  } catch (err) {
    errorEl.textContent = err.message;
    renderBoard();
  } finally {
    setBusy(false);
  }
}

async function newGame() {
  await runRequest(
    () => askWorkerProgress("newGame", {
      playerColor: colorEl.value,
      depth: Number(depthEl.value || 2),
    }, (search) => {
      state.pv = search.pv || [];
      updateThinking(search);
      renderBoard();
    }),
    "Starting game."
  );
  state.orientation = colorEl.value;
  renderBoard();
}

async function sendMove(uci) {
  state.pendingPromotion = null;
  promotionEl.classList.remove("open");
  state.hintArrow = null;
  try {
    setBusy(true, "Applying move.", { preserveBoardPosition: true });
    const data = await askWorker("playerMove", { uci });
    setBusy(false);
    await afterUserMove(data);
  } catch (err) {
    errorEl.textContent = err.message;
    renderBoard();
    setBusy(false);
  }
}

function onGroundMove(orig, dest) {
  if (state.busy || state.pendingPromotion || state.result) {
    renderBoard();
    return;
  }
  if (state.mode === "play" && state.turn !== state.playerColor) {
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
    state.pendingPromotion = { from: orig, to: dest, moves: promotionMoves };
    promotionEl.classList.add("open");
    renderBoard();
    return;
  }

  sendMove(moves[0]);
}

document.querySelectorAll(".mode-chip").forEach((button) => {
  button.addEventListener("click", () => {
    if (state.busy) return;
    state.mode = button.dataset.mode;
    document.querySelectorAll(".mode-chip").forEach((chip) => {
      chip.classList.toggle("active", chip === button);
    });
    state.hintArrow = null;
    if (state.mode === "hint" && !state.result) {
      runHint().catch((err) => {
        errorEl.textContent = err.message;
      });
    }
    renderBoard();
    updateFromEngine({
      legal_moves: state.legal,
      player_color: state.playerColor,
      last_move: state.lastMove,
      check_square: state.checkSquare,
      fen: state.fen,
      turn: state.turn,
      result: state.result,
      moves: state.moves,
      score: state.score,
      search_info: state.searchInfo,
      depth: depthEl.value,
    });
  });
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

document.getElementById("refreshViz").addEventListener("click", () => {
  refreshVisualization().catch((err) => {
    errorEl.textContent = err.message;
  });
});

document.getElementById("featuresPanel").addEventListener("toggle", () => {
  if (document.getElementById("featuresPanel").open) {
    refreshFeatures().catch((err) => {
      errorEl.textContent = err.message;
    });
  }
});

newGameEl.addEventListener("click", newGame);

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

runRequest(
  () => askWorkerProgress("init", {
    playerColor: colorEl.value,
    depth: Number(depthEl.value || 2),
  }, (search) => {
    state.pv = search.pv || [];
    updateThinking(search);
    renderBoard();
  }),
  "Loading WASM engine."
);
