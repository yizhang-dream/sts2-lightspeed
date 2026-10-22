// T3 search entry: run the STS2 BattleSearcher on real parity positions.
//
//   search replay <result.json> <route.json> [simulations]
//   search bench  <corpusDir>   [simulations] [limit]
//
// The parity corpus is the frozen reference: each directory holds the game's root stamp
// and the VeryHigh+10s solver route. bench() replays the reference route through this
// engine (which is parity-green on these positions) to get the reference outcome, then
// compares the searcher's chosen first action and predicted outcome against it.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sts2/Sts2Battle.h"
#include "sts2/Sts2Stamp.h"
#include "sts2/search/Sts2Search.h"

using json = nlohmann::json;
using namespace sts2;
using sts2::search::Action;
using sts2::search::BattleSearcher;

namespace {

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

bool loadJson(const std::string& path, json& out) {
    bool ok = false;
    const std::string raw = readFile(path, ok);
    if (!ok) return false;
    try {
        out = json::parse(raw);
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void inferCharacterPool(BattleContext& bc, const std::string& stamp) {
    const char* pools[] = {"IRONCLAD", "SILENT", "DEFECT", "NECROBINDER", "REGENT"};
    for (const char* p : pools) {
        if (stamp.find(std::string("STRIKE_") + p) != std::string::npos) {
            bc.characterPool = p;
            return;
        }
    }
}

int enemyIdxByCombatId(const BattleContext& bc, int combatId) {
    for (int i = 0; i < static_cast<int>(bc.enemies.size()); ++i)
        if (bc.enemies[i].combatId == combatId) return i;
    return -1;
}

// Route occurrence counts identical card ids in hand order (upgrade level not folded in).
int handIdxForOccurrence(const BattleContext& bc, const std::string& cardId, int occurrence) {
    int seen = 0;
    for (int i = 0; i < static_cast<int>(bc.hand.size()); ++i) {
        if (cardId != bc.hand[i].def().name) continue;
        if (seen == occurrence) return i;
        ++seen;
    }
    return -1;
}

struct RouteOutcome {
    bool complete = false;
    bool won = false;
    int hp = 0;
    int applied = 0;
    int turns = 0;
};

RouteOutcome replayRoute(BattleContext bc, const json& route) {
    RouteOutcome out;
    out.hp = bc.player.curHp;
    for (const auto& e : route) {
        if (bc.battleOver) break;
        const std::string kind = e.value("kind", "");
        if (kind == "EndTurn") {
            bc.endTurn();
            out.turns = bc.turn;
        } else if (kind == "PlayCard") {
            const std::string cardId = e.value("cardId", "");
            const int occ = e.value("cardOccurrence", 0);
            const int hi = handIdxForOccurrence(bc, cardId, occ);
            if (hi < 0) { out.hp = bc.player.curHp; return out; }
            int target = -1;
            if (e.contains("targetCombatId") && e["targetCombatId"].is_number_integer())
                target = enemyIdxByCombatId(bc, e["targetCombatId"].get<int>());
            if (target < 0 && e.value("targetIndex", -1) >= 0)
                target = e.value("targetIndex", -1);
            if (!sts2::search::applyAction(bc, {Action::Kind::PLAY_CARD, hi, target})) {
                out.hp = bc.player.curHp;
                return out;
            }
        } else {
            out.hp = bc.player.curHp;
            return out; // potion/choice actions do not appear in the T3 corpus
        }
        ++out.applied;
    }
    out.complete = bc.battleOver;
    out.won = bc.playerWon;
    out.hp = bc.player.curHp;
    return out;
}

std::string actionKey(const BattleContext& bc, const Action& a) {
    if (a.kind == Action::Kind::END_TURN) return "END_TURN";
    if (a.handIdx < 0 || a.handIdx >= static_cast<int>(bc.hand.size())) return "?";
    std::string k = bc.hand[a.handIdx].def().name;
    if (a.targetIdx >= 0 && a.targetIdx < static_cast<int>(bc.enemies.size()))
        k += "@" + std::to_string(bc.enemies[a.targetIdx].combatId);
    return k;
}

std::string routeKey(const json& e) {
    if (e.value("kind", "") == "EndTurn") return "END_TURN";
    std::string k = e.value("cardId", "?");
    if (e.contains("targetCombatId") && e["targetCombatId"].is_number_integer())
        k += "@" + std::to_string(e["targetCombatId"].get<int>());
    return k;
}

struct PositionResult {
    std::string label;
    bool parsed = false;
    long long sims = 0;
    double microsPerSim = 0;
    bool firstActionMatch = false;
    std::string ours, theirs;
    bool predictedWin = false;
    int predictedHp = 0;
    int referenceHp = 0;
    bool referenceWon = false;
    bool terminal = false;
    int decisions = 0;
    double playMicros = 0;
};

// Receding-horizon play: search at every decision and apply only the first action.
// This is the mode that is directly comparable to the frozen solver's route.
RouteOutcome playWithSearch(const BattleContext& root, long long sims, std::uint64_t seed,
                            int* decisions, double* micros) {
    BattleContext bc = root;
    RouteOutcome out;
    *decisions = 0;
    *micros = 0;
    int guard = 0;
    while (!bc.battleOver && bc.turn <= 60 && guard++ < 400) {
        BattleSearcher searcher(bc, seed + static_cast<std::uint64_t>(*decisions), 60);
        const auto t0 = std::chrono::steady_clock::now();
        searcher.search(sims);
        const auto t1 = std::chrono::steady_clock::now();
        *micros += std::chrono::duration<double, std::micro>(t1 - t0).count();
        const auto& res = searcher.result();
        if (res.bestSequence.empty()) break;
        const Action a = res.bestSequence.front();
        if (!sts2::search::applyAction(bc, a)) break;
        ++*decisions;
    }
    out.complete = bc.battleOver;
    out.won = bc.playerWon;
    out.hp = bc.player.curHp;
    return out;
}

// Same receding-horizon play, but with the run layer's search reuse: one search, then
// walk up to walkWith steps of the best line when it reaches a terminal win, walkWithout
// otherwise, and search again. The live fight() uses this (15/5); the plain version above
// re-searches at every decision. Comparing the two on one stamp corpus is the only way to
// tell "reuse buys more sims per second" from "reuse commits to a stale line".
RouteOutcome playWithReuse(const BattleContext& root, long long sims, std::uint64_t seed,
                           int* decisions, int walkWith, int walkWithout) {
    BattleContext bc = root;
    RouteOutcome out;
    *decisions = 0;
    int guard = 0;
    while (!bc.battleOver && bc.turn <= 60 && guard++ < 400) {
        BattleSearcher searcher(bc, seed + static_cast<std::uint64_t>(*decisions), 60);
        searcher.search(sims);
        const auto& res = searcher.result();
        if (res.bestSequence.empty()) break;
        const bool hasWin = res.reachedTerminal && res.outcomeHp > 0;
        const int steps = hasWin ? walkWith : walkWithout;
        int walked = 0;
        for (; walked < steps && walked < static_cast<int>(res.bestSequence.size())
               && !bc.battleOver; ++walked)
            if (!sts2::search::applyAction(bc, res.bestSequence[walked])) break;
        *decisions += walked > 0 ? walked : 1;
        if (walked == 0) break;
    }
    out.complete = bc.battleOver;
    out.won = bc.playerWon;
    out.hp = bc.player.curHp;
    return out;
}

PositionResult runPosition(const std::string& resultPath, const std::string& routePath,
                           long long sims, bool playMode) {
    PositionResult pr;
    pr.sims = sims;
    json result, route;
    if (!loadJson(resultPath, result) || !loadJson(routePath, route)) return pr;

    BattleContext root;
    std::string err;
    if (!result.contains("rootContinuationStamp")) return pr;
    const std::string stamp = result["rootContinuationStamp"].get<std::string>();
    if (!parseStamp(stamp, root, &err)) return pr;
    inferCharacterPool(root, stamp);
    pr.parsed = true;
    pr.label = result.value("label", "");

    // Reference outcome: replay the frozen solver's route through the parity-green engine.
    RouteOutcome ref = replayRoute(root, route);
    pr.referenceHp = ref.hp;
    pr.referenceWon = ref.won;

    if (playMode) {
        RouteOutcome mine = playWithSearch(root, sims, 0x5EED1234ULL, &pr.decisions,
                                           &pr.playMicros);
        pr.terminal = mine.complete;
        pr.predictedHp = mine.hp;
        pr.predictedWin = mine.won;
        pr.microsPerSim = pr.decisions > 0
            ? pr.playMicros / static_cast<double>(pr.decisions * sims) : 0.0;
        if (!route.empty()) pr.theirs = routeKey(route.front());
        // First-decision agreement: one more single-decision search on the root.
        BattleSearcher first(root, 0x5EED1234ULL, 60);
        first.search(sims);
        if (!first.result().bestSequence.empty())
            pr.ours = actionKey(root, first.result().bestSequence.front());
        pr.firstActionMatch = !pr.ours.empty() && pr.ours == pr.theirs;
        return pr;
    }

    BattleSearcher searcher(root, 0x5EED1234ULL, 60);
    const auto t0 = std::chrono::steady_clock::now();
    searcher.search(sims);
    const auto t1 = std::chrono::steady_clock::now();
    const double totalUs =
        std::chrono::duration<double, std::micro>(t1 - t0).count();
    pr.microsPerSim = sims > 0 ? totalUs / static_cast<double>(sims) : 0.0;

    const auto& res = searcher.result();
    pr.terminal = res.reachedTerminal;
    pr.predictedHp = res.outcomeHp;
    pr.predictedWin = res.reachedTerminal && res.bestValue > 0;
    if (!res.bestSequence.empty()) {
        pr.ours = actionKey(root, res.bestSequence.front());
    }
    if (!route.empty()) pr.theirs = routeKey(route.front());
    pr.firstActionMatch = !pr.ours.empty() && pr.ours == pr.theirs;
    return pr;
}

} // namespace


// Paired benchmark over captured BattleContext stamps (STS2_CAPTURE_ACT2BOSS writes them).
// Each stamp is one boss-entry position; every arm plays the SAME positions, so the
// comparison is paired and needs far fewer samples than whole-run A/B.
static int stampsMode(int argc, char** argv) {
    const std::string dir = argv[2];
    const long long sims = argc > 3 ? std::atoll(argv[3]) : 2000;
    const int limit = argc > 4 ? std::atoi(argv[4]) : 100000;
    // Fifth argument "reuse" replays with the run layer's 15/5 walk instead of
    // re-searching at every decision, so a stamp corpus can be measured under the same
    // driver the live fight uses.
    const bool reuse = argc > 5 && std::string(argv[5]) == "reuse";
    std::vector<std::string> files;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        if (e.path().extension() != ".stamp") continue;
        files.push_back(e.path().string());
    }
    std::sort(files.begin(), files.end());
    if (static_cast<int>(files.size()) > limit) files.resize(limit);
    int played = 0, won = 0, failed = 0;
    long long hpSum = 0;
    double secs = 0;
    for (const std::string& path : files) {
        bool ok = false;
        const std::string stamp = readFile(path, ok);
        if (!ok) { ++failed; continue; }
        BattleContext root;
        std::string err;
        if (!parseStamp(stamp, root, &err)) { ++failed; continue; }
        inferCharacterPool(root, stamp);
        int decisions = 0;
        double micros = 0;
        const auto t0 = std::chrono::steady_clock::now();
        RouteOutcome o = reuse
            ? playWithReuse(root, sims, 0x5EED1234ULL, &decisions, 15, 5)
            : playWithSearch(root, sims, 0x5EED1234ULL, &decisions, &micros);
        const auto t1 = std::chrono::steady_clock::now();
        secs += std::chrono::duration<double>(t1 - t0).count();
        ++played;
        if (o.won) ++won;
        hpSum += o.hp;
        std::printf("stamp=%s won=%d hp=%d decisions=%d complete=%d",
                    std::filesystem::path(path).filename().string().c_str(),
                    o.won ? 1 : 0, o.hp, decisions, o.complete ? 1 : 0);
        std::fputc(10, stdout);
    }
    std::fputc(10, stdout);
    std::printf("== %d stamps, sims=%lld, wins=%d (%.1f%%), mean end hp=%.1f, "
                "failed=%d, wall=%.1fs ==",
                played, sims, won, played ? won * 100.0 / played : 0.0,
                played ? static_cast<double>(hpSum) / played : 0.0, failed, secs);
    std::fputc(10, stdout);
    return 0;
}

static int usage() {
    std::printf(
        "usage:\n"
        "  search replay <result.json> <route.json> [simulations] [play]\n"
        "  search bench  <corpusDir>   [simulations] [limit] [play]\n");
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    const std::string cmd = argv[1];

    if (cmd == "replay") {
        if (argc < 4) return usage();
        const long long sims = argc > 4 ? std::atoll(argv[4]) : 2000;
        const bool playMode = argc > 5 && std::string(argv[5]) == "play";
        const PositionResult pr = runPosition(argv[2], argv[3], sims, playMode);
        if (!pr.parsed) {
            std::printf("failed to load position\n");
            return 2;
        }
        std::printf("position: %s\n", pr.label.c_str());
        std::printf("reference route: hp=%d won=%s\n", pr.referenceHp,
                    pr.referenceWon ? "yes" : "no");
        if (playMode) {
            std::printf("play: %d decisions, %.1f us/sim, terminal=%s, hp=%d, win=%s\n",
                        pr.decisions, pr.microsPerSim, pr.terminal ? "yes" : "no",
                        pr.predictedHp, pr.predictedWin ? "yes" : "no");
        } else {
            std::printf("search: %lld sims, %.1f us/sim, terminal=%s, hp=%d, win=%s\n",
                        pr.sims, pr.microsPerSim, pr.terminal ? "yes" : "no",
                        pr.predictedHp, pr.predictedWin ? "yes" : "no");
        }
        std::printf("first action: ours=%s theirs=%s match=%s\n",
                    pr.ours.c_str(), pr.theirs.c_str(), pr.firstActionMatch ? "yes" : "no");
        return 0;
    }

    if (cmd == "stamps") return stampsMode(argc, argv);

    if (cmd == "bench") {
        if (argc < 3) return usage();
        const std::string dir = argv[2];
        const long long sims = argc > 3 ? std::atoll(argv[3]) : 1000;
        const int limit = argc > 4 ? std::atoi(argv[4]) : 0;
        const bool playMode = argc > 5 && std::string(argv[5]) == "play";

        std::vector<std::string> labels;
        for (const auto& de : std::filesystem::directory_iterator(dir)) {
            if (de.is_directory()) labels.push_back(de.path().filename().string());
        }
        std::sort(labels.begin(), labels.end());
        if (limit > 0 && static_cast<int>(labels.size()) > limit) labels.resize(limit);

        int ok = 0, parsed = 0, match = 0, terminal = 0, refWins = 0, ourWins = 0;
        int bothFinished = 0;
        long long hpDeltaSum = 0, simsPerUs = 0, decisionsTotal = 0;
        for (const std::string& name : labels) {
            const std::string base = dir + "/" + name;
            const PositionResult pr =
                runPosition(base + "/result.json", base + "/route.json", sims, playMode);
            if (!pr.parsed) continue;
            ++parsed;
            if (pr.terminal) ++terminal;
            if (pr.firstActionMatch) ++match;
            if (pr.referenceWon) ++refWins;
            if (pr.predictedWin) ++ourWins;
            if (pr.terminal && pr.referenceHp >= 0) {
                ++bothFinished;
                hpDeltaSum += pr.predictedHp - pr.referenceHp;
            }
            simsPerUs += static_cast<long long>(pr.microsPerSim);
            decisionsTotal += pr.decisions;
            std::printf("%-30s %7.1f us/sim pred=%s hp=%-3d ref=%s hp=%-3d match=%d dec=%-3d ours=%-24s theirs=%-24s\n",
                        pr.label.c_str(), pr.microsPerSim, pr.predictedWin ? "W" : "L",
                        pr.predictedHp, pr.referenceWon ? "W" : "L", pr.referenceHp,
                        pr.firstActionMatch ? 1 : 0, pr.decisions,
                        pr.ours.c_str(), pr.theirs.c_str());
            ++ok;
        }
        if (ok == 0) {
            std::printf("no positions\n");
            return 2;
        }
        std::printf("\n== %d/%zu positions, %lld sims/%s ==\n", ok, labels.size(), sims,
                    playMode ? "decision" : "position");
        std::printf("mean %.1f us/sim\n", static_cast<double>(simsPerUs) / ok);
        std::printf("terminal reached: %d/%d\n", terminal, ok);
        std::printf("first-action agreement: %d/%d\n", match, ok);
        std::printf("wins: search %d/%d vs reference %d/%d\n", ourWins, ok, refWins, ok);
        if (bothFinished > 0)
            std::printf("mean final-HP delta (search - reference): %+.1f over %d positions\n",
                        static_cast<double>(hpDeltaSum) / bothFinished, bothFinished);
        if (playMode && ok > 0)
            std::printf("mean decisions/position: %.1f\n",
                        static_cast<double>(decisionsTotal) / ok);
        (void)parsed;
        return 0;
    }

    return usage();
}
