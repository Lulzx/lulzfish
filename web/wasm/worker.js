import createLulzfishModule from "./lulzfish.js";

let modulePromise = null;
let api = null;
let playerColor = "white";
let depth = 2;

const MAX_DEPTH = 6;

function parseState(raw) {
  const data = JSON.parse(raw);
  if (data && data.ok === false) {
    throw new Error(data.error || "Engine request failed");
  }
  data.player_color = playerColor;
  data.depth = depth;
  return data;
}

function parseJson(raw) {
  const data = JSON.parse(raw);
  if (data && data.ok === false) {
    throw new Error(data.error || "Engine request failed");
  }
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
      analyze: Module.cwrap("lulzfish_analyze", "string", ["number"]),
      graph: Module.cwrap("lulzfish_graph_json", "string", []),
      features: Module.cwrap("lulzfish_features_json", "string", []),
      attackHeatmap: Module.cwrap("lulzfish_attack_heatmap_json", "string", []),
      clearSearch: Module.cwrap("lulzfish_clear_search", null, []),
    };
  }
  return api;
}

function clampDepth(value) {
  return Math.max(1, Math.min(MAX_DEPTH, Number(value || 2)));
}

function gameOver(state) {
  return Boolean(state.result);
}

async function analyzeDepth(searchDepth) {
  const payload = parseJson(api.analyze(searchDepth));
  return payload.search;
}

async function analyzeProgressive(maxDepth, onProgress, stopBeforeFinal = false) {
  const target = clampDepth(maxDepth);
  const finalDepth = stopBeforeFinal ? Math.max(1, target - 1) : target;
  let latest = null;
  for (let d = 1; d <= finalDepth; d += 1) {
    latest = await analyzeDepth(d);
    if (onProgress) onProgress({ ...latest, partial: d < target });
  }
  return latest;
}

async function maybePlayEngine(state, options = {}) {
  if (!gameOver(state) && state.turn !== playerColor) {
    if (options.progressive && depth > 1) {
      await analyzeProgressive(depth, options.onProgress, true);
    }
    const next = parseState(api.playEngineMove(depth));
    if (options.onProgress && next.search_info) {
      options.onProgress({ ...next.search_info, partial: false });
    }
    return next;
  }
  return state;
}

async function init(payload) {
  await ensureApi();
  playerColor = payload.playerColor || "white";
  depth = clampDepth(payload.depth);
  api.clearSearch();
  const state = parseState(api.newGame());
  return maybePlayEngine(state, { progressive: true, onProgress: payload.onProgress });
}

async function newGame(payload) {
  await ensureApi();
  playerColor = payload.playerColor || "white";
  depth = clampDepth(payload.depth);
  api.clearSearch();
  const state = parseState(api.newGame());
  return maybePlayEngine(state, { progressive: true, onProgress: payload.onProgress });
}

async function userMove(payload) {
  await ensureApi();
  const afterUser = parseState(api.makeMove(payload.uci || ""));
  return maybePlayEngine(afterUser, { progressive: true, onProgress: payload.onProgress });
}

async function playerMove(payload) {
  await ensureApi();
  return parseState(api.makeMove(payload.uci || ""));
}

async function engineMove(payload) {
  await ensureApi();
  const state = parseState(api.state());
  return maybePlayEngine(state, { progressive: true, onProgress: payload.onProgress });
}

async function currentState() {
  await ensureApi();
  return parseState(api.state());
}

async function analyze(payload) {
  await ensureApi();
  const target = clampDepth(payload.depth ?? depth);
  if (payload.progressive) {
    return analyzeProgressive(target, payload.onProgress);
  }
  return analyzeDepth(target);
}

async function hint(payload) {
  await ensureApi();
  const hintDepth = Math.max(1, Math.min(3, clampDepth(payload.depth ?? depth)));
  return analyzeDepth(hintDepth);
}

async function getGraph() {
  await ensureApi();
  return parseJson(api.graph());
}

async function getFeatures() {
  await ensureApi();
  return parseJson(api.features());
}

async function getAttackHeatmap() {
  await ensureApi();
  return parseJson(api.attackHeatmap());
}

self.onmessage = async (event) => {
  const { id, type, payload = {} } = event.data || {};
  const progress = (search) => {
    self.postMessage({ id, ok: true, progress: true, search });
  };

  try {
    let data;
    const withProgress = { ...payload, onProgress: progress };

    if (type === "init") {
      data = await init(withProgress);
    } else if (type === "newGame") {
      data = await newGame(withProgress);
    } else if (type === "move") {
      data = await userMove(withProgress);
    } else if (type === "playerMove") {
      data = await playerMove(payload);
    } else if (type === "engineMove") {
      data = await engineMove(withProgress);
    } else if (type === "state") {
      data = await currentState();
    } else if (type === "analyze") {
      data = await analyze(withProgress);
    } else if (type === "hint") {
      data = await hint(payload);
    } else if (type === "getGraph") {
      data = await getGraph();
    } else if (type === "getFeatures") {
      data = await getFeatures();
    } else if (type === "getAttackHeatmap") {
      data = await getAttackHeatmap();
    } else {
      throw new Error(`Unknown worker command: ${type}`);
    }
    self.postMessage({ id, ok: true, data });
  } catch (err) {
    self.postMessage({ id, ok: false, error: err instanceof Error ? err.message : String(err) });
  }
};
