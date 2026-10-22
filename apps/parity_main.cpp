#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sts2/Sts2Battle.h"
#include "sts2/Sts2Relic.h"
#include "sts2/Sts2Stamp.h"

using json = nlohmann::json;
using namespace sts2;

int runRngCheck();

namespace {

std::string readFile(const std::string& path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { ok = false; return {}; }
    std::ostringstream ss;
    ss << f.rdbuf();
    ok = true;
    return ss.str();
}

std::vector<std::string> splitFields(const std::string& stamp) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : stamp) {
        if (ch == ';') { out.push_back(cur); cur.clear(); }
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}

// field-level diff of two stamps
int diffStamps(const std::string& label, const std::string& expected, const std::string& actual, bool verbose) {
    if (expected == actual) { if (verbose) std::printf("  [%s] EXACT MATCH\n", label.c_str()); return 0; }
    auto e = splitFields(expected), a = splitFields(actual);
    int diffs = 0;
    for (size_t i = 0; i < e.size() || i < a.size(); ++i) {
        std::string ev = i < e.size() ? e[i] : "<missing>";
        std::string av = i < a.size() ? a[i] : "<missing>";
        if (ev != av) {
            ++diffs;
            std::printf("  [%s] DIFF %s\n    expected: %.2400s\n    actual:   %.2400s\n",
                        label.c_str(), ev.substr(0, ev.find('=')).c_str(), ev.c_str(), av.c_str());
        }
    }
    return diffs;
}

} // namespace

// parity replay: `parity replay <result.json> <route.json>`
// Replays the solver-chosen route from the root stamp and diffs each turn-start state
// against the harness's recorded continuations.
static int runReplay(const std::string& resultPath, const std::string& routePath) {
    bool ok;
    std::string resultRaw = readFile(resultPath, ok);
    if (!ok) { std::printf("cannot read %s\n", resultPath.c_str()); return 2; }
    std::string routeRaw = readFile(routePath, ok);
    if (!ok) { std::printf("cannot read %s\n", routePath.c_str()); return 2; }

    json result = json::parse(resultRaw);
    json route = json::parse(routeRaw);

    BattleContext bc;
    std::string err;
    if (!parseStamp(result["rootContinuationStamp"].get<std::string>(), bc, &err)) {
        std::printf("stamp parse failed: %s\n", err.c_str());
        return 2;
    }

    // In-combat relic hooks: the root stamp does not list relics, so the corpus carries
    // them in result.json (names or ids). Start-only effects are already in the stamp.
    if (result.contains("relics") && result["relics"].is_array()) {
        for (auto& r : result["relics"]) {
            if (r.is_number_integer()) {
                bc.relics.push_back(r.get<int>());
            } else if (r.is_string()) {
                const std::string name = r.get<std::string>();
                for (int id = 1; id <= static_cast<int>(RelicId_COUNT); ++id)
                    if (name == sts2RelicName(static_cast<RelicId>(id))) {
                        bc.relics.push_back(id);
                        break;
                    }
            }
        }
    }
    // Red Skull's +3 Strength is already in the root stamp; start the toggle in the state
    // the stamp represents.
    if (bc.hasRelic(RelicId::RED_SKULL) && bc.player.curHp * 2 <= bc.player.maxHp)
        bc.redSkullActive = true;

    // character pool fallback for corpora with no Strike (explicit decks)
    if (bc.characterPool.empty()) {
        if (result.contains("character") && result["character"].is_string())
            bc.characterPool = result["character"].get<std::string>();
    }

    // character pool for CardFactory.GetForCombat (inferred from the deck names)
    {
        std::string stamp = result["rootContinuationStamp"].get<std::string>();
        const char* pools[] = {"IRONCLAD", "SILENT", "DEFECT", "NECROBINDER", "REGENT"};
        for (const char* pn : pools) {
            if (stamp.find(std::string("STRIKE_") + pn) != std::string::npos) {
                bc.characterPool = pn; break;
            }
        }
    }
    std::printf("parsed root stamp: turn=%d hp=%d/%d energy=%d stars=%d enemies=%zu hand=%zu draw=%zu\n",
                bc.turn, bc.player.curHp, bc.player.maxHp, bc.player.energy, bc.player.stars,
                bc.enemies.size(), bc.hand.size(), bc.drawPile.size());

    auto strOr = [](const json& j, const char* k, const char* dflt = "") {
        return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string(dflt);
    };
    // index continuations by turn
    std::vector<std::pair<int, std::string>> continuations;
    if (result.contains("continuations") && result["continuations"].is_array()) {
        for (auto& c : result["continuations"]) {
            int tn = c.contains("startTurnNumber") ? c["startTurnNumber"].get<int>()
                                                   : c.value("StartTurnNumber", 0);
            std::string txt = strOr(c, "stateText");
            if (txt.empty()) txt = strOr(c, "state");
            continuations.emplace_back(tn, txt);
        }
    }

    int totalDiffs = 0;
    int turnEnds = 0;

    // report missing content (T1 gate TODO list)
    {
        std::string unknownMonsters, unknownCards;
        for (auto& m : bc.enemies)
            if (m.defIdx < 0) unknownMonsters += m.name + " ";
        auto scanPile = [&](const std::vector<CardInstance>& pile) {
            for (auto& c : pile)
                if ((int)c.id == 0) unknownCards += "?(root-stamp) ";
        };
        for (auto& c : bc.hand) if ((int)c.id == 0) unknownCards += "? ";
        (void)scanPile;
        if (!unknownMonsters.empty() || !unknownCards.empty())
            std::printf("MISSING_CONTENT monsters=[%s] cards=[%s]\n", unknownMonsters.c_str(), unknownCards.c_str());
    }

    for (auto& act : route) {
        if (bc.battleOver) break;
        std::string kind = strOr(act, "kind");
        int turn = act.value("turn", 0);
        std::fprintf(stderr, "step: T%d %s %s\n", turn, kind.c_str(), strOr(act, "cardId").c_str());
        if (std::getenv("STS2_TRACE_HAND")) {
            std::string ids;
            for (auto& hc : bc.hand) ids += std::string(CardId_NAMES[(int)hc.id]) + "+" + std::to_string(hc.upgrade) + " ";
            std::fprintf(stderr, "   hand(%zu): %s | draw=%zu disc=%zu\n", bc.hand.size(), ids.c_str(), bc.drawPile.size(), bc.discardPile.size());
        }

        if (kind == "PlayCard") {
            std::string cardId = strOr(act, "cardId");
            int occurrence = act.value("cardOccurrence", 0);
            int target = act.value("targetIndex", -1);
            int upgrade = act.value("cardUpgradeLevel", 0);

            // resolve the target by combat id when the route carries one (indices shift with summons/deaths)
            if (act.contains("targetCombatId") && act["targetCombatId"].is_number()) {
                int wantId = act["targetCombatId"].get<int>();
                for (int i = 0; i < (int)bc.enemies.size(); ++i) {
                    // map to the VECTOR index — playCard indexes enemies[] directly, and a
                    // removed monster still occupies its slot (live-display index would hit it)
                    if (!bc.enemies[i].removed && bc.enemies[i].combatId == wantId) { target = i; break; }
                }
            }

            // find the occurrence-th card in hand with this id — occurrences count
            // by card id alone (upgrades differ per instance: Aggression upgrades in place)
            int seen = 0, hit = -1;
            for (int i = 0; i < (int)bc.hand.size(); ++i) {
                if (cardId == CardId_NAMES[(int)bc.hand[i].id]) {
                    if (seen == occurrence) { hit = i; break; }
                    ++seen;
                }
            }
            if (hit < 0) {
                std::printf("T%d: card %s+%d occ=%d NOT IN HAND (hand=%zu) — aborting replay\n",
                            turn, cardId.c_str(), upgrade, occurrence, bc.hand.size());
                return 1;
            }
            // route-recorded card choice (CardSelectCmd result): every chosen instance is
            // recorded with id + upgrade + source occurrence, in the order the game picked
            // them. `cards[0]` also fills the legacy single-card slots.
            bc.pendingChoice = {};
            if (act.contains("choice") && act["choice"].is_object()
                && act["choice"].contains("cards") && act["choice"]["cards"].is_array()
                && !act["choice"]["cards"].empty()) {
                const auto& chCards = act["choice"]["cards"];
                for (const auto& chCard : chCards) {
                    if (!chCard.contains("cardId") || !chCard["cardId"].is_string()) continue;
                    BattleContext::PendingChoice::SelCard sc;
                    std::string cid = chCard["cardId"].get<std::string>();
                    std::snprintf(sc.cardId, sizeof(sc.cardId), "%s", cid.c_str());
                    sc.upgrade = chCard.value("upgradeLevel", 0);
                    sc.sourceOccurrence = chCard.value("sourceOccurrence", 0);
                    bc.pendingChoice.cards.push_back(sc);
                }
                if (!bc.pendingChoice.cards.empty()) {
                    const auto& first = bc.pendingChoice.cards[0];
                    std::snprintf(bc.pendingChoice.cardId, sizeof(bc.pendingChoice.cardId),
                                  "%s", first.cardId);
                    bc.pendingChoice.upgrade = first.upgrade;
                    bc.pendingChoice.sourceOccurrence = first.sourceOccurrence;
                    if (act["choice"].contains("sourcePile") && act["choice"]["sourcePile"].is_string()) {
                        std::string pile = act["choice"]["sourcePile"].get<std::string>();
                        std::snprintf(bc.pendingChoice.sourcePile, sizeof(bc.pendingChoice.sourcePile),
                                      "%s", pile.c_str());
                    }
                    bc.pendingChoice.valid = true;
                }
            }
            bc.playCard(hit, target);
        } else if (kind == "UsePotion") {
            const int slot = act.value("potionSlot", -1);
            int target = act.value("targetIndex", -1);
            const int cid = act.value("targetCombatId", -1);
            if (cid >= 0)
                for (int i = 0; i < (int)bc.enemies.size(); ++i)
                    if (bc.enemies[i].combatId == cid) { target = i; break; }
            if (!bc.usePotion(slot, target)) {
                std::printf("T%d: UsePotion replay failed (slot=%d target=%d)\n", turn, slot, target);
                return 3;
            }
            std::printf("T%d: UsePotion slot=%d target=%d)\n", turn, slot, target);
        } else if (kind == "EndTurn") {
            // Queue the turn-start choice requests the harness logged for the NEXT turn's
            // start hooks (EntropyPower transform / StratagemPower move-to-hand). They are
            // recorded on this EndTurn action and consumed when the hooks fire.
            if (act.contains("turnStartChoices") && act["turnStartChoices"].is_array()) {
                for (auto& tsc : act["turnStartChoices"]) {
                    BattleContext::TurnStartChoice req;
                    auto getStr = [](const json& j, const char* k) {
                        return j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
                    };
                    std::string effect = getStr(tsc, "effect");
                    std::string srcPile = getStr(tsc, "sourcePile");
                    std::string srcId = getStr(tsc, "sourceId");
                    if (effect.empty() || srcId.empty()) continue;
                    std::snprintf(req.effect, sizeof(req.effect), "%s", effect.c_str());
                    std::snprintf(req.sourcePile, sizeof(req.sourcePile), "%s", srcPile.c_str());
                    std::snprintf(req.sourceId, sizeof(req.sourceId), "%s", srcId.c_str());
                    if (tsc.contains("cards") && tsc["cards"].is_array()) {
                        for (auto& cc : tsc["cards"]) {
                            BattleContext::TurnStartChoiceCard card;
                            std::string cid = getStr(cc, "cardId");
                            std::snprintf(card.cardId, sizeof(card.cardId), "%s", cid.c_str());
                            card.upgrade = cc.value("upgradeLevel", 0);
                            card.sourceOccurrence = cc.value("sourceOccurrence", 0);
                            req.cards.push_back(card);
                        }
                    }
                    req.valid = true;
                    bc.pendingTurnStartChoices.push_back(req);
                }
            }
            const int turnBefore = bc.turn;
            bc.endTurn();
            ++turnEnds;
            if (bc.turn == turnBefore) break; // combat ended mid-turn: no turn-start state follows
            // compare state at the start of the next turn
            std::string mine = emitStamp(bc);
            for (auto& [tn, txt] : continuations) {
                if (tn == bc.turn) {
                    totalDiffs += diffStamps("turn " + std::to_string(tn), txt, mine, false);
                    break;
                }
            }
            { // dump both stamps for external diffing (parity-dump mode)
                const char* dumpEnv = std::getenv("STS2_PARITY_DUMP");
                if (dumpEnv) {
                    std::string base(dumpEnv);
                    auto writeFile = [](const std::string& path, const std::string& data) {
                        FILE* f = std::fopen(path.c_str(), "w");
                        if (f) { std::fputs(data.c_str(), f); std::fclose(f); }
                    };
                    for (auto& [tn, txt] : continuations)
                        if (tn == bc.turn)
                            writeFile(base + "_exp_T" + std::to_string(tn) + ".txt", txt);
                    writeFile(base + "_act_T" + std::to_string(bc.turn) + ".txt", mine);
                }
            }
            if (!bc.battleOver) {
                std::printf("T%d start: hp=%d block=%d energy=%d | enemy0 hp=%d block=%d next=%s\n",
                            bc.turn, bc.player.curHp, bc.player.block, bc.player.energy,
                            bc.enemies.empty() ? -1 : bc.enemies[0].curHp,
                            bc.enemies.empty() ? -1 : bc.enemies[0].block,
                            bc.enemies.empty() ? "-" : bc.enemies[0].nextMove.c_str());
            for (size_t pi = 0; pi < (bc.enemies.empty() ? 0 : bc.enemies[0].powers.size()); ++pi) std::printf(" pow%d=%s/%d/misc%d", (int)pi, sts2::powerName(bc.enemies[0].powers[pi].id), bc.enemies[0].powers[pi].amount, bc.enemies[0].powers[pi].misc);
            std::printf(" msMisc=%d\n", bc.enemies.empty() ? -1 : bc.enemies[0].msMisc);
            }
        }
    }

    std::printf("\nreplay done: battleOver=%d won=%d finalTurn=%d finalHp=%d finalEnemyHp=%d turnEnds=%d\n",
                (int)bc.battleOver, (int)bc.playerWon, bc.turn, bc.player.curHp,
                bc.enemies.empty() ? -1 : bc.enemies[0].curHp, turnEnds);
    if (result.contains("solverMetrics")) {
        auto& sm = result["solverMetrics"];
        if (sm.contains("finalHp"))
            std::printf("harness finalHp=%s ours=%d\n",
                        sm["finalHp"].dump().c_str(), bc.player.curHp);
        if (sm.contains("finalEnemyHp"))
            std::printf("harness finalEnemyHp=%s ours=%d\n",
                        sm["finalEnemyHp"].dump().c_str(), bc.enemies.empty() ? -1 : bc.enemies[0].curHp);
        if (sm.contains("combatEndedTurn"))
            std::printf("harness combatEndedTurn=%s ours=%d\n",
                        sm["combatEndedTurn"].dump().c_str(), bc.turn);
    }
    std::printf("TOTAL FIELD DIFFS: %d\n", totalDiffs);
    return totalDiffs == 0 ? 0 : 1;
}

int main(int argc, char** argv) {
    setvbuf(stderr, nullptr, _IONBF, 0);
    try {
    if (argc > 1 && std::string(argv[1]) == "rngcheck") {
        return runRngCheck();
    }
    if (argc > 3 && std::string(argv[1]) == "replay") {
        return runReplay(argv[2], argv[3]);
    }
    std::cout << "usage:\n"
              << "  parity rngcheck              RNG vectors vs game implementation\n"
              << "  parity replay <result.json> <route.json>   replay + stamp diff\n";
    return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "EXCEPTION: %s\n", e.what());
        return 3;
    }
}
