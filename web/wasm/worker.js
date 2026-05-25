import createLulzfishModule from "./lulzfish.js";

let modulePromise = null;
let api = null;
let playerColor = "white";
let depth = 2;

function parseState(raw) {
  const data = JSON.parse(raw);
  if (data && data.ok === false) {
    throw new Error(data.error || "Engine request failed");
  }
  data.player_color = playerColor;
  data.depth = depth;
  return data;
}

async function ensureApi() {
  if (!modulePromise) {
    modulePromise = createLulzfishModule();
  }
  const Module = await modulePromise;
  if (!api) {
    api = {
      newGame: Module.cwrap("lulzfish_new_game", "string", []),
      state: Module.cwrap("lulzfish_state_json", "string", []),
      makeMove: Module.cwrap("lulzfish_make_move", "string", ["string"]),
      playEngineMove: Module.cwrap("lulzfish_play_engine_move", "string", ["number"]),
      evaluate: Module.cwrap("lulzfish_evaluate", "number", []),
      clearSearch: Module.cwrap("lulzfish_clear_search", null, []),
    };
  }
  return api;
}

function gameOver(state) {
  return Boolean(state.result);
}

async function maybePlayEngine(state) {
  if (!gameOver(state) && state.turn !== playerColor) {
    const next = parseState(api.playEngineMove(depth));
    return next;
  }
  return state;
}

async function init(payload) {
  await ensureApi();
  playerColor = payload.playerColor || "white";
  depth = Math.max(1, Math.min(5, Number(payload.depth || 2)));
  api.clearSearch();
  const state = parseState(api.newGame());
  return maybePlayEngine(state);
}

async function newGame(payload) {
  await ensureApi();
  playerColor = payload.playerColor || "white";
  depth = Math.max(1, Math.min(5, Number(payload.depth || 2)));
  api.clearSearch();
  const state = parseState(api.newGame());
  return maybePlayEngine(state);
}

async function userMove(payload) {
  await ensureApi();
  const afterUser = parseState(api.makeMove(payload.uci || ""));
  return maybePlayEngine(afterUser);
}

async function playerMove(payload) {
  await ensureApi();
  return parseState(api.makeMove(payload.uci || ""));
}

async function engineMove() {
  await ensureApi();
  const state = parseState(api.state());
  return maybePlayEngine(state);
}

async function currentState() {
  await ensureApi();
  return parseState(api.state());
}

self.onmessage = async (event) => {
  const { id, type, payload = {} } = event.data || {};
  try {
    let data;
    if (type === "init") {
      data = await init(payload);
    } else if (type === "newGame") {
      data = await newGame(payload);
    } else if (type === "move") {
      data = await userMove(payload);
    } else if (type === "playerMove") {
      data = await playerMove(payload);
    } else if (type === "engineMove") {
      data = await engineMove();
    } else if (type === "state") {
      data = await currentState();
    } else {
      throw new Error(`Unknown worker command: ${type}`);
    }
    self.postMessage({ id, ok: true, data });
  } catch (err) {
    self.postMessage({ id, ok: false, error: err instanceof Error ? err.message : String(err) });
  }
};
