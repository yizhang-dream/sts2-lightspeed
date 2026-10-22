#!/usr/bin/env python3
"""T5 armG-lite: candidate scorer + REINFORCE over the STS2 C++ run layer.

Ported structure from sts-rl-agent/agent/armG_train_parallel.py (MIT):
  workers play full runs with the current weights (pure inference, return plain data);
  the main process recomputes log-probs with gradients and applies one update per batch;
  reward = final floor (armG uses floor/50; this run layer tops out at 45 floors);
  0.99 moving-average baseline; frozen eval seed set; checkpoints named step{N}_eval{X}.

The learned surface covers every run-layer decision: map point, potion reward (take or
skip), card reward, relic reward (take or skip), rest site, shop, and event options. The
combat itself is driven by the engine's search at --sims.

Usage:
  python engine/python/train_armg.py --episodes 200 --sims 300 --workers 4 \
      --batch 8 --eval-every 50 --eval-seeds 20 --out output/armg_lite
"""
import argparse
import json
import os
import statistics
import sys
import tempfile
import time

import torch
import torch.nn as nn

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_MODULE = os.path.join(HERE, "..", "build-py", "Release")
sys.path.insert(0, os.path.abspath(os.environ.get("STS2_MODULE_DIR", DEFAULT_MODULE)))
import sts2  # noqa: E402

# ---------------- descriptor layout (STS2 revision, 2026-10-22) ----------------
# Cards use the engine's frozen obs vocabulary (sts2.card_obs_index, 581 slots,
# collision-free) instead of cid % 256, which collided 497 cards into 217 slots.
# Potions/Relics use their dense enum ids directly (67 / 300).
# Descriptor v2: dims changed; checkpoints from the modulo layout do not load.
DT_MAP, DT_REST, DT_SHOP, DT_EVENT, DT_CARD = 0, 1, 2, 3, 4
DT_POTION, DT_RELIC = 5, 6
ROOM_IDX = {"MONSTER": 0, "ELITE": 1, "REST": 2, "SHOP": 3, "UNKNOWN": 4,
            "TREASURE": 5, "BOSS": 6}
EVENT_CAP = 64
EOPT_CAP = 8
POTION_CAP = 67    # PotionId domain: invalid + 66, dense enum -> no modulo
RELIC_CAP = 300    # RelicId domain: invalid + 299, dense enum -> no modulo
VOCAB_CAP = sts2.card_vocab_size() + 1  # frozen 581 + one OOB slot at the end


def _card_slot(cid):
    """Frozen-vocabulary slot; cards outside the vocab (should not be offered --
    they are also unimplemented) land on the shared OOB slot."""
    idx = sts2.card_obs_index(int(cid))
    return idx if idx >= 0 else VOCAB_CAP - 1

OFF_DTYPE = 0; W_DTYPE = 7
OFF_CARD = OFF_DTYPE + W_DTYPE; W_CARD = VOCAB_CAP
OFF_MROOM = OFF_CARD + W_CARD; W_MROOM = 7
OFF_MLA1 = OFF_MROOM + W_MROOM; W_MLA1 = 7
OFF_MLA2 = OFF_MLA1 + W_MLA1; W_MLA2 = 7
OFF_REST = OFF_MLA2 + W_MLA2; W_REST = 7
OFF_SITEM = OFF_REST + W_REST; W_SITEM = 5
OFF_SPRICE = OFF_SITEM + W_SITEM; W_SPRICE = 1
OFF_EVID = OFF_SPRICE + W_SPRICE; W_EVID = EVENT_CAP
OFF_EOPT = OFF_EVID + W_EVID; W_EOPT = EOPT_CAP
OFF_PASS = OFF_EOPT + W_EOPT; W_PASS = 1
OFF_POTION = OFF_PASS + W_PASS; W_POTION = POTION_CAP
OFF_RELIC = OFF_POTION + W_POTION; W_RELIC = RELIC_CAP
OFF_TAKE = OFF_RELIC + W_RELIC; W_TAKE = 1
DESC_DIM = OFF_TAKE + W_TAKE

OBS_DIM = 3566  # obs v8 (Sts2Obs.h): base 151 + 3415 extension blocks
INPUT_DIM = OBS_DIM + DESC_DIM
ARCH = (128, 128)


def blank():
    return [0.0] * DESC_DIM


def room_counts(desc, off, kids):
    for (_idx, _col, _row, typ) in kids:
        ri = ROOM_IDX.get(typ)
        if ri is not None:
            desc[off + ri] += 1.0


def map_candidates(run):
    descs, points = [], []
    for (idx, _col, _row, typ) in run.available_points():
        d = blank()
        d[OFF_DTYPE + DT_MAP] = 1.0
        ri = ROOM_IDX.get(typ)
        if ri is not None:
            d[OFF_MROOM + ri] = 1.0
        kids = run.map_children(idx)
        room_counts(d, OFF_MLA1, kids)
        for (kid, _c2, _r2, _t2) in kids:
            room_counts(d, OFF_MLA2, run.map_children(kid))
        descs.append(d)
        points.append(idx)
    return descs, points


class Scorer(nn.Module):
    def __init__(self, arch=ARCH):
        super().__init__()
        layers = []
        prev = INPUT_DIM
        for h in arch:
            layers += [nn.Linear(prev, h), nn.ReLU()]
            prev = h
        layers.append(nn.Linear(prev, 1))
        self.net = nn.Sequential(*layers)

    def forward(self, x):
        return self.net(x).squeeze(-1)


_IMPL_CACHE = {}


def _implemented(cid):
    v = _IMPL_CACHE.get(int(cid))
    if v is None:
        v = bool(sts2.card_implemented(int(cid)))
        _IMPL_CACHE[int(cid)] = v
    return v


def card_reward_candidates(pend):
    """One candidate per offered card plus an explicit skip.

    choose_card takes the index into the offer list, not the card id: passing the id
    silently became "skip" whenever the id was >= the offer size.
    """
    descs, opts = [], []
    for i, (cid, up) in enumerate(pend["card_offer"]):
        if not _implemented(cid):
            continue  # the engine would play this card as a no-op
        d = blank()
        d[OFF_DTYPE + DT_CARD] = 1.0
        d[OFF_CARD + _card_slot(cid)] += 1.0
        descs.append(d)
        opts.append(("card", i, int(up)))
    d = blank()
    d[OFF_DTYPE + DT_CARD] = 1.0
    descs.append(d)
    opts.append(("skip", -1, 0))
    return descs, opts


def rest_candidates(pend):
    """HEAL plus one SMITH option per upgradable deck card."""
    rest = pend["rest"]
    descs, opts = [], []
    d = blank()
    d[OFF_DTYPE + DT_REST] = 1.0
    d[OFF_REST] = 1.0
    descs.append(d)
    opts.append(("HEAL", -1))
    for idx in rest["upgradable"]:
        d = blank()
        d[OFF_DTYPE + DT_REST] = 1.0
        d[OFF_REST + 1] = 1.0
        descs.append(d)
        opts.append(("SMITH", int(idx)))
    return descs, opts


def potion_candidates(pend):
    """Take or skip the pending potion reward."""
    pid = int(pend["potion_reward"]["id"])
    descs, opts = [], []
    for take in (1, 0):
        d = blank()
        d[OFF_DTYPE + DT_POTION] = 1.0
        d[OFF_TAKE] = float(take)
        if take:
            d[OFF_POTION + pid] += 1.0
        descs.append(d)
        opts.append(bool(take))
    return descs, opts


def relic_candidates(pend):
    """Take or skip the pending relic reward."""
    rid = int(pend["relic_reward"]["id"])
    descs, opts = [], []
    for take in (1, 0):
        d = blank()
        d[OFF_DTYPE + DT_RELIC] = 1.0
        d[OFF_TAKE] = float(take)
        if take:
            d[OFF_RELIC + rid] += 1.0
        descs.append(d)
        opts.append(bool(take))
    return descs, opts


def shop_candidates(pend):
    """One candidate per affordable, unsold shelf slot plus an explicit leave.

    The card-removal service needs a second (deck-index) pick, so it is left out of the
    learned surface; the scaffold policy never buys it either.
    """
    descs, opts = [], []
    for i, it in enumerate(pend["shop"]["items"]):
        if it["sold"] or it["removal"]:
            continue
        if not it.get("relic") and not _implemented(it["card"]):
            continue
        d = blank()
        d[OFF_DTYPE + DT_SHOP] = 1.0
        d[OFF_SITEM + (2 if it.get("relic") else (1 if it["sale"] else 0))] = 1.0
        d[OFF_SPRICE] = min(1.0, it["price"] / 300.0)
        descs.append(d)
        opts.append(("buy", i))
    d = blank()
    d[OFF_DTYPE + DT_SHOP] = 1.0
    d[OFF_SITEM + 4] = 1.0
    descs.append(d)
    opts.append(("leave", -1))
    return descs, opts


def event_candidates(pend):
    """One candidate per event option (option order is the game's display order)."""
    descs, opts = [], []
    for i, _name in enumerate(pend["event"]["options"]):
        d = blank()
        d[OFF_DTYPE + DT_EVENT] = 1.0
        d[OFF_EOPT + min(i, EOPT_CAP - 1)] = 1.0
        descs.append(d)
        opts.append(("event", i))
    return descs, opts


def _choose(net, obs, descs, greedy):
    if len(descs) == 1:
        return 0
    if net is None:
        return int(torch.randint(len(descs), (1,)).item())
    with torch.no_grad():
        logits = net(torch.tensor([obs + d for d in descs], dtype=torch.float32))
        probs = torch.softmax(logits, dim=-1)
        return int(torch.argmax(probs).item()) if greedy \
            else int(torch.multinomial(probs, 1).item())


def play_episode(net, seed, sims, greedy=False, hp=0, ascension=0):
    """One full run; returns plain data only (worker-safe).

    hp is a test hook (0 = the game's normal 80 HP) used by the content sweep to walk
    all three acts; training always calls with the default.
    """
    run = sts2.Run.start(str(seed), hp, "", "", ascension)
    traj = []
    unresolved = False
    while not run.over:
        descs, points = map_candidates(run)
        if not points:
            break
        obs = run.obs_v8()
        action = _choose(net, obs, descs, greedy)
        traj.append((obs, descs, action))
        run.enter(points[action], sims)
        # Room decisions (card reward / rest site): each is one scored candidate set.
        guard = 0
        while not run.over and guard < 30:
            pend = run.pending()
            if not pend:
                break
            guard += 1
            obs2 = run.obs_v8()
            if "toolbox" in pend:
                tdescs, topts = [], []
                for cid in pend["toolbox"]:
                    d = blank()
                    d[OFF_DTYPE + DT_CARD] = 1.0
                    d[OFF_CARD + _card_slot(cid)] += 1.0
                    tdescs.append(d)
                    topts.append(int(cid))
                a2 = _choose(net, obs2, tdescs, greedy)
                traj.append((obs2, tdescs, a2))
                run.choose_toolbox(a2)
            elif "potion_reward" in pend:
                pdescs, popts = potion_candidates(pend)
                a2 = _choose(net, obs2, pdescs, greedy)
                traj.append((obs2, pdescs, a2))
                run.choose_potion_reward(popts[a2])
            elif "card_offer" in pend:
                cdescs, copts = card_reward_candidates(pend)
                a2 = _choose(net, obs2, cdescs, greedy)
                traj.append((obs2, cdescs, a2))
                kind, offer_idx, _up = copts[a2]
                run.choose_card(offer_idx if kind == "card" else -1)
            elif "relic_reward" in pend:
                rrdescs, rropts = relic_candidates(pend)
                a2 = _choose(net, obs2, rrdescs, greedy)
                traj.append((obs2, rrdescs, a2))
                run.choose_relic_reward(rropts[a2])
            elif "rest" in pend:
                rdescs, ropts = rest_candidates(pend)
                a2 = _choose(net, obs2, rdescs, greedy)
                traj.append((obs2, rdescs, a2))
                kind, idx = ropts[a2]
                run.choose_rest(kind, idx)
            elif "event_cards" in pend:
                # Event card grid (RoomFullOfCheese GORGE): pick one card into the deck.
                descs2, opts2 = [], []
                for i, (cid, up) in enumerate(pend["event_cards"]["cards"]):
                    if not _implemented(cid):
                        continue
                    d = blank()
                    d[OFF_DTYPE + DT_CARD] = 1.0
                    d[OFF_CARD + _card_slot(cid)] += 1.0
                    descs2.append(d)
                    opts2.append(i)  # pool index, not the card id
                if descs2:
                    a2 = _choose(net, obs2, descs2, greedy)
                    traj.append((obs2, descs2, a2))
                    run.choose_event_card(opts2[a2])
                else:
                    run.default_decisions()
            elif "shop" in pend:
                sdescs, sopts = shop_candidates(pend)
                a2 = _choose(net, obs2, sdescs, greedy)
                traj.append((obs2, sdescs, a2))
                kind, idx = sopts[a2]
                if kind == "buy":
                    run.buy(idx)
                else:
                    run.leave_shop()
            else:  # event
                edescs, eopts = event_candidates(pend)
                if not edescs:
                    # Pages such as the CrystalSphere minigame expose no list options;
                    # the scaffold clicks the fogged cells to finish the event.
                    run.default_decisions()
                    continue
                a2 = _choose(net, obs2, edescs, greedy)
                traj.append((obs2, edescs, a2))
                if not run.choose_event(eopts[a2][1], sims):
                    # The option's own gate rejected it (e.g. UNCOVER_FUTURE costs more
                    # gold than the player has). Let the scaffold try the other options.
                    run.default_decisions()
        if guard >= 30:
            run.default_decisions()
            if run.pending():
                # Never advance the map with an unresolved decision: doing so leaves the
                # run in a state the game cannot reach.
                unresolved = True
                break
    summary = run.summary()
    reward = summary["floor"] / 45.0 + (1.0 if summary["won"] else 0.0)
    return {"seed": seed, "reward": reward, "floor": summary["floor"],
            "won": summary["won"], "hp": summary["hp"], "summary": summary,
            "unresolved": unresolved,
            "unsupported": run.run_unsupported_card or run.last_unsupported_card,
            "timed_out": bool(run.last_fight_timed_out),
            "traj": [] if greedy else traj}


def worker_play(args):
    seed, wpath, greedy, sims, threads, ascension = args
    torch.set_num_threads(threads)
    net = Scorer()
    net.load_state_dict(torch.load(wpath, weights_only=True))
    net.eval()
    return play_episode(net, seed, sims, greedy, ascension=ascension)


def game_loss(net, traj, advantage):
    if not traj:
        return None
    log_probs = []
    for obs, descs, action in traj:
        logits = net(torch.tensor([obs + d for d in descs], dtype=torch.float32))
        log_probs.append(torch.log_softmax(logits, dim=-1)[action])
    return -torch.stack(log_probs).sum() * advantage


def evaluate(net, seeds, sims, pool=None, wpath=None, threads=1, ascension=0):
    if pool is not None:
        torch.save(net.state_dict(), wpath)
        results = pool.map(worker_play,
                           [(s, wpath, True, sims, threads, ascension) for s in seeds])
    else:
        results = [play_episode(net, s, sims, greedy=True, ascension=ascension)
                   for s in seeds]
    floors = [r["floor"] for r in results]
    return (statistics.mean(floors) if floors else 0.0,
            sum(1 for r in results if r["won"]),
            sum(1 for r in results if r.get("timed_out")))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--episodes", type=int, default=200)
    ap.add_argument("--sims", type=int, default=2000)
    ap.add_argument("--workers", type=int, default=1)
    ap.add_argument("--batch", type=int, default=8)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--seed-start", type=int, default=100000)
    ap.add_argument("--eval-every", type=int, default=50)
    ap.add_argument("--eval-seeds", type=int, default=20)
    ap.add_argument("--eval-seed-start", type=int, default=950000)
    ap.add_argument("--eval-seeds-file", default=os.path.join(HERE, "..", "eval_seeds.txt"),
                    help="Fixed evaluation seed list. When the file exists it replaces "
                         "--eval-seeds/--eval-seed-start, and a training range that overlaps "
                         "it is refused (docs/ledger.md 2026-10-20 T5 evaluation set).")
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--baseline-decay", type=float, default=0.99)
    ap.add_argument("--out", default=os.path.join(HERE, "..", "output", "armg_lite"))
    ap.add_argument("--ascension", type=int, default=0,
                    help="STS2 ascension level 0-10 (A8/A9 monster tables, A1-A7 run effects)")
    ap.add_argument("--max-seconds", type=float, default=0.0,
                    help="wall-clock cap; 0 = no cap. Stops after the current batch and "
                         "still writes policy_final.pt.")
    args = ap.parse_args()

    # ---- fixed evaluation set + train/eval seed isolation ----
    # A single committed list keeps every batch comparable, and refusing an overlapping
    # training range stops a run from learning on its own exam. The guard runs before any
    # engine call so a bad configuration costs nothing.
    eval_seed_list = None
    if args.eval_seeds_file and os.path.isfile(args.eval_seeds_file):
        with open(args.eval_seeds_file, encoding="utf-8") as f:
            eval_seed_list = [int(x) for x in f if x.strip() and not x.lstrip().startswith("#")]
        if not eval_seed_list:
            raise SystemExit(f"eval seed file is empty: {args.eval_seeds_file}")
    else:
        eval_seed_list = [args.eval_seed_start + i for i in range(args.eval_seeds)]
    eval_set = set(eval_seed_list)

    train_range = range(args.seed_start, args.seed_start + args.episodes)
    overlap = [s for s in train_range if s in eval_set]
    if overlap:
        raise SystemExit(
            "refusing to train: %d training seed(s) are in the evaluation set "
            "(first %s). Move --seed-start or use a separate evaluation seed file; "
            "see docs/ledger.md 2026-10-20." % (len(overlap), overlap[:5]))

    print(json.dumps({"sims": args.sims, "ascension": args.ascension,
                      "eval_seeds": len(eval_seed_list),
                      "eval_source": args.eval_seeds_file,
                      "train_seeds": [args.seed_start, args.seed_start + args.episodes - 1],
                      "overlap": 0}), flush=True)

    torch.set_num_threads(args.threads)
    torch.manual_seed(1234)
    os.makedirs(args.out, exist_ok=True)
    model = Scorer()
    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    baseline = None
    metrics = open(os.path.join(args.out, "metrics.jsonl"), "a", encoding="utf-8")
    wpath = os.path.join(tempfile.gettempdir(), "sts2_armg_weights.pt")

    import multiprocessing as mp
    pool = None
    if args.workers > 1:
        pool = mp.get_context("spawn").Pool(args.workers)

    t0 = time.time()
    done = 0
    next_eval = args.eval_every
    recent = []
    n_unresolved = n_timed_out = n_unsupported = 0
    for b in range(0, args.episodes, args.batch):
        if args.max_seconds and time.time() - t0 >= args.max_seconds:
            print(json.dumps({"stopped": "wall_cap", "episode": done,
                              "secs": round(time.time() - t0, 1)}), flush=True)
            break
        seeds = [args.seed_start + i for i in range(b, min(b + args.batch, args.episodes))]
        if pool is not None:
            torch.save(model.state_dict(), wpath)
            results = pool.map(worker_play,
                                [(s, wpath, False, args.sims, args.threads, args.ascension)
                                 for s in seeds])
        else:
            results = [play_episode(model, s, args.sims, ascension=args.ascension)
                       for s in seeds]

        opt.zero_grad()
        losses = []
        for r in results:
            R = r["reward"]
            baseline = R if baseline is None else \
                args.baseline_decay * baseline + (1 - args.baseline_decay) * R
            loss = game_loss(model, r["traj"], R - baseline)
            if loss is not None:
                losses.append(loss)
            recent.append(r["floor"])
            done += 1
            n_unresolved += 1 if r.get("unresolved") else 0
            n_timed_out += 1 if r.get("timed_out") else 0
            n_unsupported += 1 if r.get("unsupported") else 0
            rec = {"episode": done, "seed": r["seed"], "floor": r["floor"],
                   "won": r["won"], "hp": r["hp"], "reward": round(R, 4),
                   "baseline": round(baseline, 4),
                   "loss": round(float(loss.detach()), 4) if loss is not None else None,
                   "unresolved": bool(r.get("unresolved")),
                   "timed_out": bool(r.get("timed_out")),
                   "unsupported": r.get("unsupported", ""),
                   "secs": round(time.time() - t0, 1)}
            metrics.write(json.dumps(rec) + "\n")
        if losses:
            torch.stack(losses).mean().backward()
            opt.step()
        metrics.flush()

        if done >= next_eval:
            seeds_eval = eval_seed_list
            mean_floor, wins, eval_timeouts = evaluate(
                model, seeds_eval, args.sims, pool, wpath, args.threads, args.ascension)
            window = statistics.mean(recent[-args.eval_every:])
            rec = {"episode": done, "train_floor": round(window, 2),
                   "eval_floor": round(mean_floor, 2), "eval_wins": wins,
                   "eval_timeouts": eval_timeouts,
                   "eval_n": len(seeds_eval), "secs": round(time.time() - t0, 1)}
            print(json.dumps(rec), flush=True)
            metrics.write(json.dumps(rec) + "\n")
            metrics.flush()
            torch.save(model.state_dict(),
                       os.path.join(args.out, f"step{done}_eval{mean_floor:.1f}.pt"))
            next_eval += args.eval_every

    if pool is not None:
        pool.close()
        pool.join()
    torch.save(model.state_dict(), os.path.join(args.out, "policy_final.pt"))
    metrics.close()
    elapsed = time.time() - t0
    eps_per_s = done / elapsed if elapsed > 0 else 0.0
    print(json.dumps({"done": done, "seconds": round(elapsed, 1),
                      "episodes_per_sec": round(eps_per_s, 3),
                      "projected_per_day": int(eps_per_s * 86400),
                      "unresolved": n_unresolved, "timed_out": n_timed_out,
                      "unsupported": n_unsupported}, indent=1))


if __name__ == "__main__":
    main()
