import { Chessground } from "https://unpkg.com/chessground@9.2.1/dist/chessground.js";

let ground = null;
let requestId = 0;

const worker = new Worker(new URL("./worker.js", import.meta.url), { type: "module" });
const pending = new Map();

const state = {
  legal: [],
  playerColor: "white",
  orientation: "white",
  pendingPromotion: null,
  lastMove: null,
  fen: "startpos",
  turn: "white",
  checkSquare: null,
  result: "",
  moves: [],
  busy: true,
};

const boardEl = document.getElementById("board");
const statusMain = document.getElementById("statusMain");
const statusSub = document.getElementById("statusSub");
const movesEl = document.getElementById("moves");
const errorEl = document.getElementById("error");
const colorEl = document.getElementById("color");
const depthEl = document.getElementById("depth");
const promotionEl = document.getElementById("promotion");
const newGameEl = document.getElementById("newGame");

worker.onmessage = (event) => {
  const { id, ok, data, error } = event.data || {};
  const callbacks = pending.get(id);
  if (!callbacks) return;
  pending.delete(id);
  if (ok) callbacks.resolve(data);
  else callbacks.reject(new Error(error || "Worker request failed"));
};

function askWorker(type, payload = {}) {
  const id = ++requestId;
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject });
    worker.postMessage({ id, type, payload });
  });
}

function legalDests() {
  const dests = new Map();
  if (state.result || state.busy || state.turn !== state.playerColor) return dests;
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
  return state.fen === "startpos" ? "startpos" : state.fen.split(" ")[0];
}

function renderBoard() {
  const config = {
    fen: boardFen(),
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
      color: state.result || state.busy ? undefined : state.playerColor,
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
      enabled: !state.busy,
      showGhost: true,
    },
  };

  if (!ground) ground = Chessground(boardEl, config);
  else ground.set(config);
}

function renderMoves(moves) {
  if (!moves.length) {
    movesEl.innerHTML = '<span class="moves-empty">No moves yet.</span>';
    return;
  }
  const rows = [];
  for (let i = 0; i < moves.length; i += 2) {
    const moveNo = Math.floor(i / 2) + 1;
    rows.push(`${moveNo}. ${moves[i]}${moves[i + 1] ? " " + moves[i + 1] : ""}`);
  }
  movesEl.textContent = rows.join("  ");
  movesEl.scrollTop = movesEl.scrollHeight;
}

function setBusy(isBusy, label = "") {
  state.busy = isBusy;
  newGameEl.disabled = isBusy;
  colorEl.disabled = isBusy;
  depthEl.disabled = isBusy;
  errorEl.textContent = "";
  if (isBusy) statusSub.textContent = label || "Engine is thinking.";
  renderBoard();
}

function updateFromEngine(data) {
  state.legal = data.legal_moves || [];
  state.playerColor = data.player_color || state.playerColor;
  state.lastMove = data.last_move || null;
  state.checkSquare = data.check_square || null;
  state.fen = data.fen || "startpos";
  state.turn = data.turn || "white";
  state.result = data.result || "";
  state.moves = data.moves || [];
  colorEl.value = state.playerColor;
  depthEl.value = data.depth || depthEl.value;

  if (state.result) {
    statusMain.textContent = "Game over";
    statusSub.textContent = state.result;
  } else if (state.turn === state.playerColor) {
    statusMain.textContent = "Your move";
    statusSub.textContent = `${state.turn === "white" ? "White" : "Black"} to move`;
  } else {
    statusMain.textContent = "Lulzfish to move";
    statusSub.textContent = `Depth ${depthEl.value}`;
  }

  renderMoves(state.moves);
  renderBoard();
}

async function runRequest(action, busyLabel) {
  setBusy(true, busyLabel);
  state.pendingPromotion = null;
  promotionEl.classList.remove("open");
  try {
    updateFromEngine(await action());
  } catch (err) {
    errorEl.textContent = err.message;
    renderBoard();
  } finally {
    setBusy(false);
  }
}

async function newGame() {
  await runRequest(
    () => askWorker("newGame", {
      playerColor: colorEl.value,
      depth: Number(depthEl.value || 2),
    }),
    "Starting game."
  );
  state.orientation = colorEl.value;
  renderBoard();
}

async function sendMove(uci) {
  await runRequest(() => askWorker("move", { uci }), "Lulzfish is thinking.");
}

function onGroundMove(orig, dest) {
  if (state.busy || state.pendingPromotion || state.result || state.turn !== state.playerColor) {
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
  () => askWorker("init", {
    playerColor: colorEl.value,
    depth: Number(depthEl.value || 2),
  }),
  "Loading WASM engine."
);
