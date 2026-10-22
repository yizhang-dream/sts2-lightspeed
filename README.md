# sts2-lightspeed

A high-performance, RNG-faithful **C++ engine for Slay the Spire 2** (early-access
v0.111.0), built for reinforcement-learning research: full-run environment (map,
rewards, shops, rest sites, events, Ancients, potions, relics), MCTS battle search,
ascension 0-10, Python bindings, and a REINFORCE trainer — with a replay-parity test
harness that keeps the engine honest against the real game.

Not affiliated with MegaCrit. Unofficial research artifact; no game assets are
included. Please support the official game.

## What you get

| Piece | Entry point | What it does |
|---|---|---|
| Full-run engine | `run play <seed> [sims] [hp] [boss] [asc]` | One complete run: map routing, rooms, battles, rewards, act transitions |
| Benchmark | `run bench <seedStart> <count> [sims] [asc]` | Scaffold-policy batch with per-seed floors/wins |
| Battle MCTS | `search bench parity_data/corpus 2000` | Search quality/latency on recorded positions |
| Parity harness | `tools/parity_batch.sh` | Replays recorded game positions; zero field diffs required |
| Determinism | `run determinism <seed>` | Same seed, same fingerprint, always |
| Python API | `python/train_armg.py` | `sts2.Run.start(seed, ascension=...)` + candidate-scoring trainer |

### Performance

~**21 µs per MCTS simulation** single-threaded (bench-tuned build, desktop CPU), on
par with the STS1 `sts_lightspeed` lineage this search structure was ported from.
A full run at 2000 simulations per battle completes in ~1.5 s; a 12-process worker
pool sustains ~3.3 runs/second at that budget.

The searcher is a structural port of `BattleScumSearcher2` (UCB playouts,
progressive bias, transposition dedup, balanced root sampling) onto this engine's
rules; recent work added per-node state snapshots and buffer reuse, which reached
parity without changing a single decision — the optimization shipped with a
bit-identical gate (same seeds, byte-identical outcomes).

### Faithfulness

- **Parity corpus**: 65 recorded real-game positions replayed step-by-step; the
  gate requires zero field diffs (`tools/parity_batch.sh`, wired into CI).
- **RNG discipline**: every random stream is seeded and consumed in the game's
  order; `run determinism` verifies run-level reproducibility.
- **Ascension** implements the game's ten named modifiers (elite swarm, gold
  penalty, rarity shift, per-monster HP/damage tables at A8/A9, double boss at
  A10), generated from the game's own data and cross-checked at load time.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --target run parity search map
```

C++20, no external dependencies (nlohmann/json is vendored under `third_party/`).

Python bindings (optional):

```bash
pip install pybind11
cmake -B build -DSTS2_BUILD_PYTHON=ON
cmake --build build --config Release --target sts2
python python/train_armg.py --episodes 4 --sims 50 --out output/smoke
```

## Quickstart

```bash
# one run at 2000 simulations per battle, verbose
./build/Release/run play 960000 2000

# 50-seed benchmark, scaffold policy
./build/Release/run bench 960000 50 300

# the ascension ladder (mean floors fall off a cliff at A8/A9)
for a in 0 5 8 9 10; do ./build/Release/run bench 960000 50 300 $a | tail -1; done

# replay the frozen parity corpus (CI gate)
bash tools/parity_batch.sh
```

Python:

```python
import sts2  # from the built module directory
run = sts2.Run.start("960000")            # optional: hp, boss_override, event_override, ascension
while not run.over:
    for idx, col, row, room in run.available_points():
        ...                                # route however you like
    run.enter(run.available_points()[0][0], sims=500)
    ...                                    # pending() surfaces each room's decision
print(run.summary())
```

The trainer (`python/train_armg.py`) is a compact REINFORCE loop over a
candidate-scoring MLP: workers play full runs with the current weights (pure
inference), the main process recomputes log-probs and updates; battles are played
by the engine's built-in MCTS. It ports the training structure of
[Jialeiv/sts-rl-agent](https://github.com/Jialeiv/sts-rl-agent) (MIT).

## Lineage and credits

- Search structure and evaluation contract ported from
  [gamerpuppy/sts_lightspeed](https://github.com/gamerpuppy/sts_lightspeed) (MIT) —
  the STS1 engine this project's performance targets descend from.
- Trainer structure (candidate-scoring REINFORCE, frozen eval seeds, scaffold
  baselines) after [Jialeiv/sts-rl-agent](https://github.com/Jialeiv/sts-rl-agent)
  (MIT).
- The STS2 rules engine, run layer, observation export, parity harness, and
  trainer integration are original to this project.

## License

MIT — see [LICENSE](LICENSE). The vendored nlohmann/json carries its own MIT
license notices.
