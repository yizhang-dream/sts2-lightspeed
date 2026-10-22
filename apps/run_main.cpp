// T4 run driver: headless full run over the STS2 map/room layer.
//
//   run play        <seed> [sims]
//   run bench       <seedStart> <count> [sims]
//   run determinism <seed> [sims]

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "sts2/Sts2Run.h"
#include "sts2/Sts2Ids.h"

using namespace sts2;

namespace {


// Rows until the first RestSite reachable from `idx`, or -1 when none is reachable before
// the boss. The map is a forward DAG by rows, so this is a row-by-row reachability sweep
// (7 wide, height <= 15) rather than a general search.
int rowsToNearestRest(const ActMap& map, int idx) {
    std::vector<int> frontier{idx};
    const int startRow = map.points[idx].row;
    for (int row = startRow + 1; row <= map.height; ++row) {
        std::vector<int> next;
        for (int cur : frontier)
            for (int child : map.points[cur].children)
                if (std::find(next.begin(), next.end(), child) == next.end()) next.push_back(child);
        if (next.empty()) return -1;
        for (int cand : next)
            if (map.points[cand].type == MapPointType::RestSite) return row - startRow;
        frontier = std::move(next);
    }
    return -1;
}

int choosePoint(const RunState& run) {
    const std::vector<int> options = availablePoints(run);
    if (options.empty()) return -1;

    // Default map policy. The scaffold line is the benchmark the run layer is measured
    // on, so its routing rules matter: it used to prefer a monster node over a rest site
    // whenever HP was above 50%, and it treated elites like any other node. HP is the
    // binding constraint across acts (47.6% of runs die on the act-1 boss at 300 sims),
    // so route to rest sites earlier and only take an elite when healthy.
    // STS2_MACRO=legacy restores the previous rules.
    static const bool kSmart = [] {
        const char* v = std::getenv("STS2_MACRO");
        return !(v && std::string(v) == "legacy");
    }();
    static const double kRestFrac = [] {
        const char* v = std::getenv("STS2_MAP_REST_FRAC");
        return v ? std::atof(v) : 0.7;
    }();
    static const double kEliteFrac = [] {
        const char* v = std::getenv("STS2_MAP_ELITE_FRAC");
        return v ? std::atof(v) : 0.85;
    }();

    int boss = -1, rest = -1, monster = -1, elite = -1, treasure = -1, shop = -1,
        other = -1;
    for (int idx : options) {
        const MapPointType t = run.map.points[idx].type;
        if (t == MapPointType::Boss) { if (boss < 0) boss = idx; }
        else if (t == MapPointType::RestSite) { if (rest < 0) rest = idx; }
        else if (t == MapPointType::Monster) { if (monster < 0) monster = idx; }
        else if (t == MapPointType::Elite) { if (elite < 0) elite = idx; }
        else if (t == MapPointType::Treasure) { if (treasure < 0) treasure = idx; }
        else if (t == MapPointType::Shop) { if (shop < 0) shop = idx; }
        else if (other < 0) other = idx;
    }
    if (boss >= 0) return boss;

    if (!kSmart) {
        if (run.player.hp * 2 < run.player.maxHp && rest >= 0) return rest;
        if (monster >= 0) return monster;
        if (rest >= 0) return rest;
        return other >= 0 ? other : options.front();
    }

    const double hpFrac = run.player.maxHp > 0
        ? static_cast<double>(run.player.hp) / run.player.maxHp : 1.0;

    // Route planning: when HP is short, prefer the branch that reaches a rest site
    // soonest, not merely "a rest is a legal next step". Measured with the per-room HP
    // ledger: a rest heals ~19-23 HP but act 2 offers 1 rest per 14 fights, so reaching
    // one earlier is worth more than the type of the next room. STS2_MAP_PLAN=0 keeps the
    // type-only rule.
    static const bool kPlan = [] {
        const char* v = std::getenv("STS2_MAP_PLAN");
        return !(v && std::string(v) == "0");
    }();
    if (kPlan && hpFrac < 0.85) {
        int bestIdx = -1;
        int bestDist = 1 << 30;
        for (int idx : options) {
            const MapPointType t = run.map.points[idx].type;
            if (t == MapPointType::Boss) continue;
            const int d = rowsToNearestRest(run.map, idx);
            if (d < 0) continue;                       // no rest before the boss down this line
            // Prefer the closer rest; break ties toward a normal fight (elites cost more
            // than the rest gains).
            if (d < bestDist
                || (d == bestDist && t == MapPointType::Monster
                    && (bestIdx < 0 || run.map.points[bestIdx].type != MapPointType::Monster))) {
                bestDist = d;
                bestIdx = idx;
            }
        }
        // Only override when the planned line reaches a rest sooner than what the type
        // rule would pick did.
        if (bestIdx >= 0) {
            int fallbackDist = 1 << 30;
            if (rest >= 0) fallbackDist = rowsToNearestRest(run.map, rest);
            else if (monster >= 0) fallbackDist = rowsToNearestRest(run.map, monster);
            if (fallbackDist < 0 || bestDist < fallbackDist) return bestIdx;
        }
    }

    if (hpFrac < kRestFrac && rest >= 0) return rest;     // top up before the next fight

    // Economy nodes: a treasure is a free relic and a shop converts gold into deck
    // quality. The scaffold used to walk past both whenever a normal fight was legal,
    // leaving relics and purchases on the table. STS2_MAP_ECON=0 restores that.
    static const bool kEcon = [] {
        const char* v = std::getenv("STS2_MAP_ECON");
        return !(v && std::string(v) == "0");
    }();
    static const int kShopGold = [] {
        const char* v = std::getenv("STS2_MAP_SHOP_GOLD");
        return v ? std::atoi(v) : 150;
    }();
    if (kEcon) {
        if (treasure >= 0) return treasure;
        if (shop >= 0 && run.player.gold >= kShopGold) return shop;
    }

    if (monster >= 0) return monster;                     // normal fights are the value
    if (hpFrac >= kEliteFrac && elite >= 0) return elite; // healthy: trade HP for a relic
    if (rest >= 0) return rest;
    if (elite >= 0 && hpFrac >= 0.5) return elite;
    return other >= 0 ? other : options.front();
}

struct RunOutcome {
    bool finished = false;
    bool won = false;
    int hp = 0;
    int floor = 0;
    int act = 0;
    int rooms = 0;
    int combats = 0;
};

RunOutcome playRun(const std::string& seed, long long sims, bool verbose, int hpOverride = 0,
                    int ascension = 0,
                   const std::string& bossOverride = "") {
    RunState run = startRun(seed, hpOverride, ascension);
    run.bossOverride = bossOverride;
    static const bool kTraceHp = std::getenv("STS2_TRACE_HP") != nullptr;
    // HP sampled around each room (entry, after its decisions). RoomLog.hpAfter is written
    // before a rest/shop decision resolves, so it cannot answer "what HP did the boss start
    // at" -- a rest right before the boss is exactly the common case, and the stale value
    // understated boss entry HP by a whole rest heal.
    std::vector<std::pair<int, int>> roomHp;
    int guard = 0;
    while (!run.over && guard++ < 500) {
        const int point = choosePoint(run);
        if (point < 0) break;
        const int preHp = run.player.hp;
        const int preAct = run.actIndex; // label rooms by the act they belong to, not the act they open
        const MapPointType pt = run.map.points[point].type;
        enterPoint(run, point, sims);
        withDefaultDecisions(run); // CLI scaffold policy: first card, heal-or-smith
        roomHp.emplace_back(preHp, run.player.hp);
        if (kTraceHp) {
            // HP must be sampled around the whole room (entry + decisions): RoomLog's
            // hpAfter is written before a rest/shop decision resolves, so a post-hoc
            // ledger cannot see rest healing at all.
            const char* enc = run.rooms.empty() ? "" : run.rooms.back().encounter;
            std::fprintf(stderr, "HP act=%d floor=%d type=%s enc=%s pre=%d post=%d",
                         preAct + 1, run.totalFloor, mapPointTypeName(pt), enc,
                         preHp, run.player.hp);
            std::fputc(10, stderr);
        }
    }
    RunOutcome out;
    out.finished = run.over;
    out.won = run.won;
    out.hp = run.player.hp;
    out.floor = run.totalFloor;
    out.act = run.actIndex;
    out.rooms = static_cast<int>(run.rooms.size());
    for (const auto& r : run.rooms)
        if (r.type == MapPointType::Monster || r.type == MapPointType::Elite ||
            r.type == MapPointType::Boss || r.type == MapPointType::Unknown)
            ++out.combats;
    if (verbose) {
        std::printf("seed=%s act=%d hp=%d/%d gold=%d floor=%d\n", run.stringSeed.c_str(),
                    run.actIndex + 1, run.player.hp, run.player.maxHp, run.player.gold,
                    run.totalFloor);
        for (const auto& r : run.rooms) {
            std::string cards;
            for (const auto& cc : r.cardChoices) {
                if (!cards.empty()) cards += ",";
                cards += CardId_NAMES[cc.first];
                if (cc.second > 0) cards += "+" + std::to_string(cc.second);
            }
            std::printf("  f%-3d %-9s %-26s won=%d hp=%-3d turns=%-2d gold=+%-3d pot=%d cards=[%s]\n",
                        r.floor, mapPointTypeName(r.type), r.encounter, r.won ? 1 : 0,
                        r.hpAfter, r.turns, r.goldReward, r.potionReward ? 1 : 0,
                        cards.c_str());
        }
    }
    // Act-transition diagnostic. Printed to stderr so the bench summary on stdout keeps
    // parsing the same. Enable with STS2_TRACE_ACT2=1.
    //
    // Two line kinds:
    //   BOSS act=<n> ...  every boss room with the HP it started and ended at, which is
    //                     what separates "arrived low" from "the boss is expensive".
    //   ACT2 / ACT3 ...   the HP carried into each act (hp after the previous boss) plus,
    //                     for act 2, what the first fight cost.
    if (std::getenv("STS2_TRACE_ACT2")) {
        const int n = static_cast<int>(run.rooms.size());
        int bossIdxAct1 = -1, bossIdxAct2 = -1;
        int act = 1;
        int bossCount = 0;
        for (int i = 0; i < n; ++i) {
            const RoomLog& r = run.rooms[i];
            if (r.type == MapPointType::Boss) {
                // Sampled HP, not RoomLog.hpAfter: a rest room right before the boss keeps
                // its pre-decision HP in the log, which made entry HP look a rest-heal low.
                const int before = i < static_cast<int>(roomHp.size())
                    ? roomHp[i].first : run.player.maxHp;
                const int post = i < static_cast<int>(roomHp.size())
                    ? roomHp[i].second : r.hpAfter;
                // Potion count at the moment the boss starts: the prep layer's job is to
                // have potions available for this fight, so it has to be visible.
                int potionsHeld = 0;
                for (int pid : run.player.potions) if (pid != 0) ++potionsHeld;
                std::fprintf(stderr,
                    "BOSS act=%d seed=%s maxHp=%d pre=%d post=%d won=%d turns=%d "
                    "floor=%d pot=%d enc=%s",
                    act, seed.c_str(), run.player.maxHp, before, post,
                    r.won ? 1 : 0, r.turns, r.floor, potionsHeld, r.encounter);
                std::fputc(10, stderr);
                if (bossCount == 0) bossIdxAct1 = i;
                else if (bossCount == 1) bossIdxAct2 = i;
                ++bossCount;
                ++act; // a boss closes its act
            }
        }
        if (bossIdxAct1 >= 0) {
            const int entryHp = bossIdxAct1 < static_cast<int>(roomHp.size())
                ? roomHp[bossIdxAct1].second : run.rooms[bossIdxAct1].hpAfter;
            int firstIdx = -1;
            for (int i = bossIdxAct1 + 1; i < n; ++i) {
                const MapPointType t = run.rooms[i].type;
                if (t == MapPointType::Monster || t == MapPointType::Elite ||
                    t == MapPointType::Boss || t == MapPointType::Unknown) { firstIdx = i; break; }
            }
            if (firstIdx >= 0) {
                const RoomLog& r = run.rooms[firstIdx];
                const int before = firstIdx < static_cast<int>(roomHp.size())
                    ? roomHp[firstIdx].first : run.rooms[firstIdx - 1].hpAfter;
                const int post = firstIdx < static_cast<int>(roomHp.size())
                    ? roomHp[firstIdx].second : r.hpAfter;
                std::fprintf(stderr,
                    "ACT2 seed=%s maxHp=%d bossHp=%d preHp=%d postHp=%d won=%d turns=%d "
                    "floor=%d enc=%s",
                    seed.c_str(), run.player.maxHp, entryHp, before, post,
                    r.won ? 1 : 0, r.turns, r.floor, r.encounter);
                std::fputc(10, stderr);
            }
        }
        // Only a run that actually survived the act-2 boss has entered act 3 (actIndex is
        // 0-based). Printing this whenever an act-2 boss room exists counted the boss
        // losses as act-3 entries, which inflated the reach rate.
        if (bossIdxAct2 >= 0 && run.actIndex >= 2) {
            const int entryHp = bossIdxAct2 < static_cast<int>(roomHp.size())
                ? roomHp[bossIdxAct2].second : run.rooms[bossIdxAct2].hpAfter;
            std::fprintf(stderr, "ACT3 seed=%s maxHp=%d entryHp=%d finalAct=%d won=%d",
                         seed.c_str(), run.player.maxHp, entryHp, run.actIndex + 1,
                         run.won ? 1 : 0);
            std::fputc(10, stderr);
        }
    }

    return out;
}

std::string fingerprint(const RunState& run) {
    std::string s;
    s += "act=" + std::to_string(run.actIndex) + ";hp=" + std::to_string(run.player.hp) +
         ";gold=" + std::to_string(run.player.gold) + ";floor=" +
         std::to_string(run.totalFloor) + ";";
    for (const auto& r : run.rooms) {
        s += mapPointTypeName(r.type);
        s += ":";
        s += r.encounter;
        s += ":";
        s += std::to_string(r.won ? 1 : 0);
        s += ":";
        s += std::to_string(r.hpAfter);
        s += ":g" + std::to_string(r.goldReward);
        s += ":p" + std::to_string(r.potionReward ? 1 : 0);
        for (const auto& cc : r.cardChoices)
            s += ":c" + std::to_string(cc.first) + "+" + std::to_string(cc.second);
        s += "|";
    }
    return s;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: run play <seed> [sims] [hp] [boss] [asc] | "
                    "run bench <seedStart> <count> [sims] [asc] | "
                    "run determinism <seed> [sims]\n");
        return 2;
    }
    const std::string cmd = argv[1];

    if (cmd == "rewards") {
        RunState run = startRun(argv[2]);
        const std::string kind = argc > 3 ? argv[3] : "elite";
        const int count = argc > 4 ? std::atoi(argv[4]) : 1;
        if (argc > 5) run.actIndex = std::atoi(argv[5]); // reward upgrade odds use the act
        for (int i = 0; i < count; ++i) {
            const CombatReward rw = generateCombatReward(run, kind == "elite", kind == "boss");
            std::printf("gold=%d potion=%d cards=[", rw.gold, rw.potion ? 1 : 0);
            for (size_t k = 0; k < rw.cards.size(); ++k) {
                if (k) std::printf(",");
                std::printf("%s+%d", CardId_NAMES[rw.cards[k].first], rw.cards[k].second);
            }
            std::printf("]\n");
            if (std::getenv("STS2_REWARD_DEBUG")) {
                const auto st = run.rewards.CaptureState();
                std::fprintf(stderr, "rng_after=%d:%llu:%llu:%llu:%llu\n",
                             st.counter, (unsigned long long)st.state.s0,
                             (unsigned long long)st.state.s1, (unsigned long long)st.state.s2,
                             (unsigned long long)st.state.s3);
            }
        }
        return 0;
    }

    if (cmd == "play") {
        const long long sims = argc > 3 ? std::atoll(argv[3]) : 500;
        const int hpOverride = argc > 4 ? std::atoi(argv[4]) : 0;
        const std::string bossOverride = argc > 5 ? argv[5] : "";
        const int ascension = argc > 6 ? std::atoi(argv[6]) : 0;
        const RunOutcome out = playRun(argv[2], sims, true, hpOverride, ascension,
                                       bossOverride);
        std::printf("result: won=%d hp=%d act=%d floor=%d combats=%d\n",
                    out.won ? 1 : 0, out.hp, out.act + 1, out.floor, out.combats);
        return 0;
    }

    if (cmd == "bench") {
        if (argc < 4) return 2;
        const long long seedStart = std::atoll(argv[2]);
        const int count = std::atoi(argv[3]);
        const long long sims = argc > 4 ? std::atoll(argv[4]) : 500;
        const int ascension = argc > 5 ? std::atoi(argv[5]) : 0;
        int wins = 0, finished = 0;
        long long floorSum = 0, combats = 0;
        for (int i = 0; i < count; ++i) {
            const std::string seed = std::to_string(seedStart + i);
            const RunOutcome out = playRun(seed, sims, false, 0, ascension);
            if (out.finished) ++finished;
            if (out.won) ++wins;
            floorSum += out.floor;
            combats += out.combats;
            std::printf("seed=%-10s won=%d hp=%-3d act=%d floor=%-3d combats=%d\n",
                        seed.c_str(), out.won ? 1 : 0, out.hp, out.act + 1, out.floor,
                        out.combats);
        }
        std::printf("\n== %d runs, finished=%d wins=%d, mean floor=%.1f, mean combats=%.1f ==\n",
                    count, finished, wins,
                    static_cast<double>(floorSum) / count,
                    static_cast<double>(combats) / count);
        return 0;
    }

    if (cmd == "determinism") {
        const long long sims = argc > 3 ? std::atoll(argv[3]) : 500;
        RunState a = startRun(argv[2]);
        int guardA = 0;
        while (!a.over && guardA++ < 500) {
            const int p = choosePoint(a);
            if (p < 0) break;
            enterPoint(a, p, sims);
            withDefaultDecisions(a);
        }
        RunState b = startRun(argv[2]);
        int guardB = 0;
        while (!b.over && guardB++ < 500) {
            const int p = choosePoint(b);
            if (p < 0) break;
            enterPoint(b, p, sims);
            withDefaultDecisions(b);
        }
        const std::string fa = fingerprint(a);
        const std::string fb = fingerprint(b);
        std::printf("seed=%s same=%d rooms=%zu/%zu\n", argv[2], fa == fb ? 1 : 0,
                    a.rooms.size(), b.rooms.size());
        if (fa != fb) {
            std::printf("A: %s\nB: %s\n", fa.c_str(), fb.c_str());
            return 1;
        }
        return 0;
    }

    return 2;
}
