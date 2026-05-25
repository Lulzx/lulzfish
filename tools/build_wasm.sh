#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/web/dist"
EMXX="${EMXX:-em++}"

if ! command -v "${EMXX}" >/dev/null 2>&1; then
  echo "em++ was not found. Install Emscripten, then rerun this script." >&2
  exit 1
fi

mkdir -p "${OUT_DIR}"

cp "${ROOT_DIR}/web/wasm/index.html" "${OUT_DIR}/index.html"
cp "${ROOT_DIR}/web/wasm/app.js" "${OUT_DIR}/app.js"
cp "${ROOT_DIR}/web/wasm/worker.js" "${OUT_DIR}/worker.js"
if [ -d "${ROOT_DIR}/web/wasm/benchmarks" ]; then
  rm -rf "${OUT_DIR}/benchmarks"
  cp -R "${ROOT_DIR}/web/wasm/benchmarks" "${OUT_DIR}/benchmarks"
fi

"${EMXX}" \
  -std=c++23 \
  -O3 \
  -DNDEBUG \
  -Wall \
  -Wextra \
  -Wpedantic \
  -Wshadow \
  -Wconversion \
  -Wsign-conversion \
  -Wnull-dereference \
  -Wformat=2 \
  -fexceptions \
  -I "${ROOT_DIR}/src" \
  "${ROOT_DIR}/src/lulzfish/core/position.cpp" \
  "${ROOT_DIR}/src/lulzfish/core/attacks.cpp" \
  "${ROOT_DIR}/src/lulzfish/core/movegen.cpp" \
  "${ROOT_DIR}/src/lulzfish/eval/material.cpp" \
  "${ROOT_DIR}/src/lulzfish/eval/graph_eval.cpp" \
  "${ROOT_DIR}/src/lulzfish/search/search.cpp" \
  "${ROOT_DIR}/src/lulzfish/search/transposition.cpp" \
  "${ROOT_DIR}/src/lulzfish/wasm/wasm_api.cpp" \
  -sMODULARIZE=1 \
  -sEXPORT_ES6=1 \
  -sEXPORT_NAME=createLulzfishModule \
  -sENVIRONMENT=web,worker,node \
  -sALLOW_MEMORY_GROWTH=1 \
  -sNO_EXIT_RUNTIME=1 \
  -sDISABLE_EXCEPTION_CATCHING=0 \
  -sEXPORTED_FUNCTIONS="['_lulzfish_new_game','_lulzfish_set_fen','_lulzfish_state_json','_lulzfish_make_move','_lulzfish_apply_uci_line','_lulzfish_best_move','_lulzfish_play_engine_move','_lulzfish_evaluate','_lulzfish_clear_search']" \
  -sEXPORTED_RUNTIME_METHODS="['ccall','cwrap']" \
  -o "${OUT_DIR}/lulzfish.js"

echo "Built ${OUT_DIR}/lulzfish.js and ${OUT_DIR}/lulzfish.wasm"
