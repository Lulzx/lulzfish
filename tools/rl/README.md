# Lulzfish RL Tooling

This directory contains the first training-side integration for:

- a native PufferLib chess environment plus a Gym-compatible wrapper,
- self-play/deeper-search label generation,
- baseline export of graph-eval and search-controller weights.

The C++ UCI engine stays dependency-free. PufferLib, Gymnasium, NumPy, and
training code live on the Python side until a model proves useful.

## Dependencies

```bash
python3 -m pip install bulletchess numpy gymnasium
```

For native PufferLib vectorization, use a separate Python 3.10/3.11 venv:

```bash
python3.11 -m venv .venv-puffer
source .venv-puffer/bin/activate
python -m pip install -r tools/rl/requirements-puffer.txt
```

The PyPI `pufferlib` package currently targets an older Gym/Numpy stack. The
public PufferLib docs describe newer native workflows. The native Puffer env
uses `python-chess` for compatibility with that older stack; the rest of the
repo can continue using `bulletchess`.

## Fast Self-Play / Label Generation

Build the engine, then generate deeper-search labels:

```bash
cmake --build build -j
python3 tools/rl/generate_training_data.py \
  --engine ./build/lulzfish \
  --games 8 \
  --shallow-depth 1 \
  --target-depth 3 \
  --out data/lulzfish_training.jsonl
```

Each JSONL row stores:

- FEN and ply,
- legal moves,
- shallow best move and score,
- target-depth best move, score, and PV.

This is usable both as graph-eval target data and as search-controller data.

## Train Baseline Weights

```bash
python3 tools/rl/train_linear_models.py \
  --data data/lulzfish_training.jsonl \
  --out-json data/lulzfish_linear_models.json \
  --out-header data/lulzfish_linear_models.hpp
```

The exported header is a measurement artifact, not automatically compiled into
the engine. Integrate it only after comparing NPS and search-regression impact.

## PufferLib Environment

`tools/rl/lulzfish_env.py` exposes the bulletchess/Gym-side helpers:

- `LulzfishEnv`: Gymnasium-compatible environment.
- `make_env(**kwargs)`: Gymnasium factory.
- fixed action space: `64 * 64 * 5`, encoding from, to, and promotion.
- Gym observation dict: compact features plus legal action mask.

Example:

```python
from tools.rl.lulzfish_env import make_env, action_from_uci

env = make_env(opponent="random")
obs, info = env.reset()
obs, reward, terminated, truncated, info = env.step(action_from_uci("e2e4"))
```

`tools/rl/puffer_lulzfish_env.py` exposes the native PufferLib env:

- `PufferLulzfishEnv`: native `pufferlib.PufferEnv`.
- `make_puffer_env(**kwargs)`: factory for `pufferlib.vector`.
- Puffer observation vector: compact features followed by legal action mask.

Run the native PufferLib vectorization smoke test:

```bash
source .venv-puffer/bin/activate
python tools/rl/run_puffer_smoke.py --num-envs 4 --steps 128 --opponent random
```

The smoke test uses `pufferlib.vector.Serial`, samples legal actions from the
mask embedded in the observation, and reports rough steps/sec. Use
`--opponent engine --engine ./build/lulzfish` only for small tests; UCI engine
opponents serialize expensive searches and are not intended for high-throughput
policy rollouts.
