// Python bindings for the STS2 engine (T4d): run layer first, battle layer next.
//
// Built only when STS2_BUILD_PYTHON=ON (needs pybind11 + CPython). The C++ entry points
// (run/search/map/parity) stay buildable without Python.

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <string>
#include <vector>

#include "sts2/Sts2Enchantment.h"
#include "sts2/Sts2Obs.h"
#include "sts2/Sts2ObsVocab.h"
#include "sts2/Sts2Run.h"

namespace py = pybind11;
using namespace sts2;

namespace {

int choosePoint(const RunState& run) {
    const std::vector<int> options = availablePoints(run);
    if (options.empty()) return -1;
    int boss = -1, rest = -1, monster = -1, other = -1;
    for (int idx : options) {
        const MapPointType t = run.map.points[idx].type;
        if (t == MapPointType::Boss) { if (boss < 0) boss = idx; }
        else if (t == MapPointType::RestSite) { if (rest < 0) rest = idx; }
        else if (t == MapPointType::Monster) { if (monster < 0) monster = idx; }
        else if (other < 0) other = idx;
    }
    if (boss >= 0) return boss;
    if (run.player.hp * 2 < run.player.maxHp && rest >= 0) return rest;
    if (monster >= 0) return monster;
    if (rest >= 0) return rest;
    return other >= 0 ? other : options.front();
}

py::dict runSummary(const RunState& run) {
    py::dict d;
    d["seed"] = run.stringSeed;
    d["act"] = run.actIndex + 1;
    d["act_name"] = run.actName;
    d["hp"] = run.player.hp;
    d["max_hp"] = run.player.maxHp;
    d["gold"] = run.player.gold;
    d["floor"] = run.totalFloor;
    d["over"] = run.over;
    d["won"] = run.won;
    d["deck"] = run.player.deck.size();
    d["wongo_points"] = run.wongoPoints;
    {
        py::list relics;
        for (int rid : run.relics) relics.append(sts2RelicName(static_cast<RelicId>(rid)));
        d["relics"] = relics;
    }
    {
        py::list potions;
        for (int pid : run.player.potions)
            if (pid != 0) potions.append(sts2PotionName(static_cast<PotionId>(pid)));
        d["potions"] = potions;
    }
    py::list rooms;
    for (const auto& r : run.rooms) {
        py::dict rd;
        rd["floor"] = r.floor;
        rd["type"] = mapPointTypeName(r.type);
        rd["encounter"] = r.encounter;
        rd["won"] = r.won;
        rd["hp_after"] = r.hpAfter;
        rd["turns"] = r.turns;
        rd["goldReward"] = r.goldReward;
        rooms.append(rd);
    }
    d["rooms"] = rooms;
    return d;
}

} // namespace

PYBIND11_MODULE(sts2, m) {
    m.doc() = "STS2 C++ engine bindings (run layer)";

    // Frozen card vocabulary (Sts2ObsVocab.h, 581 slots): the collision-free mapping
    // the observation uses; the armG descriptor shares it so offered cards stay
    // distinguishable (cid % 256 collided 497 cards into 217 slots).
    m.def("card_vocab_size", []() { return static_cast<int>(sts2::OBS_CARD_VOCAB); });
    m.def("card_obs_index", [](int cid) {
        return (cid >= 0 && cid <= sts2::OBS_CARD_ID_COUNT)
            ? static_cast<int>(sts2::kCardIdToObsIndex[cid]) : -1;
    });

    py::class_<RunState>(m, "Run")
        .def_static("start", [](const std::string& seed, int hpOverride,
                                const std::string& bossOverride,
                                const std::string& eventOverride,
                                int ascension) {
            RunState run = startRun(seed, hpOverride, ascension);
            run.bossOverride = bossOverride;
            run.eventOverride = eventOverride;
            // Opt-in Ancient opening (same hook style as the ? room override).
            if (eventOverride == "NEOW") startNeowOpening(run);
            if (eventOverride == "DARV") startDarvOpening(run);
            if (eventOverride == "OROBAS") startOrobasOpening(run);
            if (eventOverride == "VAKUU") startVakuuOpening(run);
            if (eventOverride == "PAEL") startPaelOpening(run);
            if (eventOverride == "NONUPEIPE") startNonupeipeOpening(run);
            if (eventOverride == "TANX") startTanxOpening(run);
            if (eventOverride == "TEZCATARA") startTezcataraOpening(run);
            // The game always opens through Neow: at A2 the run starts at 80% max HP
            // even when the Neow event itself is not modelled.
            if (run.ascension >= 2 && eventOverride != "NEOW")
                run.player.hp = static_cast<int>(run.player.maxHp * 0.8f);
            return run;
        }, py::arg("seed"), py::arg("hp_override") = 0, py::arg("boss_override") = "",
           py::arg("event_override") = "", py::arg("ascension") = 0)
        .def("available_points", [](const RunState& run) {
            // (point index, col, row, type) for every reachable room.
            py::list out;
            for (int idx : availablePoints(run)) {
                const MapPoint& p = run.map.points[idx];
                out.append(py::make_tuple(idx, p.col, p.row, mapPointTypeName(p.type)));
            }
            return out;
        })
        .def("enter", [](RunState& run, int point, long long sims) {
            return enterPoint(run, point, sims);
        }, py::arg("point"), py::arg("sims") = 500)
        // ---- decision surface (T4) ----
        .def("pending", [](const RunState& run) {
            // None when the run is between rooms; otherwise the room's decision.
            if (!run.awaitingCardReward && !run.awaitingRest && !run.awaitingShop
                && !run.awaitingEvent && !run.awaitingEventCards
                && !run.awaitingPotionReward && !run.awaitingRelicReward
                && !run.awaitingToolbox)
                return py::object(py::none());
            py::dict d;
            if (run.awaitingCardReward) {
                py::list offer;
                for (const auto& c : run.cardOffer) offer.append(py::make_tuple(c.first, c.second));
                d["card_offer"] = offer;
            }
            if (run.awaitingToolbox) {
                py::list opts;
                for (int cid : run.toolboxOptions) opts.append(cid);
                d["toolbox"] = opts;
            }
            if (run.awaitingPotionReward) {
                py::dict pr;
                pr["id"] = run.potionRewardId;
                pr["name"] = sts2PotionName(static_cast<PotionId>(run.potionRewardId));
                d["potion_reward"] = pr;
            }
            if (run.awaitingRelicReward) {
                py::dict rr;
                rr["id"] = run.relicRewardId;
                rr["name"] = sts2RelicName(static_cast<RelicId>(run.relicRewardId));
                d["relic_reward"] = rr;
            }
            if (run.awaitingRest) {
                py::dict r;
                r["heal"] = run.restHeal;
                py::list up;
                for (int idx : run.restUpgradable) up.append(idx);
                r["upgradable"] = up;
                d["rest"] = r;
            }
            if (run.awaitingShop) {
                py::list items;
                for (const ShopEntry& e : run.shop) {
                    py::dict it;
                    it["card"] = e.cardId;
                    it["upgrade"] = e.upgrade;
                    it["price"] = e.price;
                    it["sale"] = e.onSale;
                    it["sold"] = e.sold;
                    it["removal"] = e.removal;
                    it["relic"] = e.relic;
                    it["relic_id"] = e.relicId;
                    it["potion"] = e.potion;
                    it["potion_id"] = e.potionId;
                    items.append(it);
                }
                py::dict sh;
                sh["items"] = items;
                sh["removals_used"] = run.cardRemovalsUsed;
                d["shop"] = sh;
            }
            if (run.awaitingEventCards) {
                py::list pool;
                for (const auto& c : run.eventCardPool)
                    pool.append(py::make_tuple(c.first, c.second));
                py::dict g;
                g["cards"] = pool;
                g["left"] = run.eventCardsLeft;
                d["event_cards"] = g;
            }
            if (run.awaitingEvent) {
                py::dict ev;
                ev["id"] = run.eventId;
                ev["page"] = run.eventPage;
                py::list opts;
                for (const auto& o : run.eventOptions) opts.append(o);
                ev["options"] = opts;
                ev["gold"] = run.eventGold;
                ev["heal"] = run.eventHeal;
                ev["hp_loss"] = run.eventHpLoss;
                if (!run.eventOwnedRelics.empty()) {
                    py::list owned, fresh;
                    for (int id : run.eventOwnedRelics) owned.append(sts2RelicName(static_cast<RelicId>(id)));
                    for (int id : run.eventNewRelics) fresh.append(sts2RelicName(static_cast<RelicId>(id)));
                    py::dict pair;
                    pair["owned"] = owned;
                    pair["new"] = fresh;
                    ev["relic_pairs"] = pair;
                }
                if (run.eventPotionSlot >= 0)
                    ev["picked_potion_slot"] = run.eventPotionSlot;
                if (run.eventPickRelicId != 0)
                    ev["picked_relic"] =
                        sts2RelicName(static_cast<RelicId>(run.eventPickRelicId));
                if (run.eventBridgeCardIdx >= 0
                    && run.eventBridgeCardIdx < static_cast<int>(run.player.deck.size()))
                    ev["bridge_card"] = BattleContext::cardDef(
                        run.player.deck[run.eventBridgeCardIdx].id).name;
                if (run.eventId == "TRIAL") ev["trial_case"] = run.eventTrialCase;
                if (run.eventId == "TINKER_TIME") {
                    ev["tinker_type"] = run.eventTinkerType;
                    py::list riders;
                    for (int r : run.eventTinkerRiders) riders.append(r);
                    ev["tinker_riders"] = riders;
                }
                if (run.eventId == "CRYSTAL_SPHERE") {
                    py::dict sp;
                    sp["divinations"] = run.eventSphereDivinations;
                    sp["tool"] = run.eventSphereTool;
                    py::list hidden;
                    for (int h : run.eventSphereHidden) hidden.append(h);
                    sp["hidden"] = hidden;
                    py::list items;
                    for (int i = 0; i < static_cast<int>(run.eventSphereType.size()); ++i) {
                        py::dict it;
                        it["pos"] = run.eventSpherePos[i];
                        it["w"] = run.eventSphereW[i];
                        it["h"] = run.eventSphereH[i];
                        it["type"] = run.eventSphereType[i];
                        items.append(it);
                    }
                    sp["items"] = items;
                    py::list revealed;
                    for (int i : run.eventSphereRevealed) revealed.append(i);
                    sp["revealed"] = revealed;
                    ev["sphere"] = sp;
                }
                d["event"] = ev;
            }
            return py::object(d);
        })
        .def("buy", [](RunState& run, int idx, int removeIdx) {
            return buyShop(run, idx, removeIdx);
        }, py::arg("idx"), py::arg("remove_idx") = -1)
        .def("set_relics", [](RunState& run, const std::vector<std::string>& names) {
            // Test/curriculum hook: replace the owned relics by name (starter included).
            run.relics.clear();
            for (const auto& n : names) {
                for (int i = 1; i <= RelicId_COUNT; ++i) {
                    if (n == sts2RelicName(static_cast<RelicId>(i))) {
                        run.relics.push_back(i);
                        break;
                    }
                }
            }
        })
        .def("set_deck", [](RunState& run, const std::vector<std::string>& names, int upgrade) {
            // Test/curriculum hook: replace the deck by card names (upgrade level applies
            // to every card). Used by the Lava Lamp no-damage reward check.
            run.player.deck.clear();
            for (const auto& n : names) {
                const int cid = lookupCardId(n.c_str());
                if (cid > 0) run.player.deck.push_back(
                    {static_cast<CardId>(cid), upgrade});
            }
        }, py::arg("names"), py::arg("upgrade") = 0)
        .def("set_potions", [](RunState& run, const std::vector<std::string>& names) {
            // Test/curriculum hook: fill the three slots by potion name (empty = clear).
            run.player.potions = {0, 0, 0};
            int slot = 0;
            for (const auto& n : names) {
                if (slot >= 3) break;
                for (int i = 1; i <= PotionId_COUNT; ++i)
                    if (n == sts2PotionName(static_cast<PotionId>(i))) {
                        run.player.potions[slot++] = i;
                        break;
                    }
            }
        })
        .def("set_gold", [](RunState& run, int gold) {
            // Test/curriculum hook: gold used by event-gate end-to-end checks.
            run.player.gold = gold;
        })
        .def("leave_shop", [](RunState& run) { leaveShop(run); })
        .def("choose_event_card", [](RunState& run, int idx) {
            return chooseEventCard(run, idx);
        }, py::arg("idx"))
        .def("choose_event", [](RunState& run, int idx, long long sims) {
            return chooseEventOption(run, idx, sims);
        }, py::arg("idx"), py::arg("sims") = 300)
        .def("sphere_click", [](RunState& run, int x, int y, int tool) {
            return chooseSphereCell(run, x, y, tool);
        }, py::arg("x"), py::arg("y"), py::arg("tool") = 0)
        .def("choose_card", [](RunState& run, int idx) {
            return chooseCardReward(run, idx);
        }, py::arg("idx"))
        .def("choose_toolbox", [](RunState& run, int idx) {
            return chooseToolbox(run, idx);
        }, py::arg("idx"))
        .def("choose_potion_reward", [](RunState& run, bool take) {
            return choosePotionReward(run, take);
        }, py::arg("take"))
        .def("choose_relic_reward", [](RunState& run, bool take) {
            return chooseRelicReward(run, take);
        }, py::arg("take"))
        .def("choose_rest", [](RunState& run, const std::string& option, int deckIdx) {
            return chooseRest(run, option.c_str(), deckIdx);
        }, py::arg("option"), py::arg("deck_idx") = -1)
        .def("default_decisions", [](RunState& run) { withDefaultDecisions(run); })
        .def("obs_v8", [](const RunState& run) { return runObsV8(run); })
        .def("obs", [](const RunState& run) {
            // Compact run observation for the first armG slice (T5 wiring). Fixed layout:
            //  0 hp/max, 1 maxHp/100, 2 gold/999, 3 act/3, 4 floor/45, 5 deck/40,
            //  6 attacks/20, 7 skills/20, 8 powers/20, 9 upgrades/20,
            // 10 over, 11 won.  (obs v8 / 3566-dim port is a separate item.)
            int attacks = 0, skills = 0, powers = 0, upgrades = 0;
            for (const RunCard& c : run.player.deck) {
                const CardDef& d = BattleContext::cardDef(c.id);
                if (d.type == CardType::ATTACK) ++attacks;
                else if (d.type == CardType::SKILL) ++skills;
                else if (d.type == CardType::POWER) ++powers;
                upgrades += c.upgrade > 0 ? 1 : 0;
            }
            std::vector<double> v = {
                run.player.maxHp > 0 ? static_cast<double>(run.player.hp) / run.player.maxHp : 0.0,
                run.player.maxHp / 100.0,
                run.player.gold / 999.0,
                (run.actIndex + 1) / 3.0,
                run.totalFloor / 45.0,
                run.player.deck.size() / 40.0,
                attacks / 20.0,
                skills / 20.0,
                powers / 20.0,
                upgrades / 20.0,
                run.over ? 1.0 : 0.0,
                run.won ? 1.0 : 0.0};
            return v;
        })
        .def("map_children", [](const RunState& run, int point) {
            py::list out;
            if (point < 0 || point >= static_cast<int>(run.map.points.size())) return out;
            for (int child : run.map.points[point].children) {
                const MapPoint& p = run.map.points[child];
                out.append(py::make_tuple(child, p.col, p.row, mapPointTypeName(p.type)));
            }
            return out;
        }, py::arg("point"))
        .def("deck", [](const RunState& run) {
            py::list out;
            for (const RunCard& c : run.player.deck)
                out.append(py::make_tuple(BattleContext::cardDef(c.id).name, c.upgrade));
            return out;
        })
        .def("deck_detail", [](const RunState& run) {
            py::list out;
            for (const RunCard& c : run.player.deck) {
                py::dict d;
                d["name"] = BattleContext::cardDef(c.id).name;
                d["upgrade"] = c.upgrade;
                d["enchant"] = sts2EnchantmentName(static_cast<EnchantmentId>(c.enchant));
                d["amount"] = c.enchantAmount;
                d["disabled"] = c.enchantDisabled;
                d["tinker_type"] = c.tinkerType;
                d["tinker_rider"] = c.tinkerRider;
                out.append(d);
            }
            return out;
        })
        .def("summary", [](const RunState& run) { return runSummary(run); })
        .def_readonly("over", &RunState::over)
        .def_readonly("won", &RunState::won)
        .def_readonly("last_perfect_fit_moves", &RunState::lastPerfectFitMoves)
        .def_readonly("last_cards_generated", &RunState::lastCardsGenerated)
        .def_readonly("last_unsupported_card", &RunState::lastUnsupportedCard)
        .def_readonly("run_unsupported_card", &RunState::runUnsupportedCard)
        .def_readonly("last_sharp_applications", &RunState::lastSharpApplications)
        .def_readonly("last_nimble_applications", &RunState::lastNimbleApplications)
        .def_readonly("last_slither_draws", &RunState::lastSlitherDraws)
        .def_readonly("last_toric_regrants", &RunState::lastToricRegrants)
        .def_readonly("last_spiral_replays", &RunState::lastSpiralReplays)
        .def_readonly("last_vigorous_applications", &RunState::lastVigorousApplications)
        .def_readonly("last_corrupted_triggers", &RunState::lastCorruptedTriggers)
        .def_readonly("last_souls_power_kept", &RunState::lastSoulsPowerKept)
        .def_readonly("last_fight_won", &RunState::lastFightWon)
        .def_readonly("last_fight_timed_out", &RunState::lastFightTimedOut)
        .def_readonly("last_fight_turns", &RunState::lastFightTurns)
        .def_readonly("last_battle_start_energy", &RunState::lastBattleStartEnergy)
        .def_readonly("last_fight_enemy_hp_left", &RunState::lastFightEnemyHpLeft);

    m.def("preview_battle", [](const std::string& seed, const std::string& encounterId,
                               const std::vector<std::string>& relicNames) {
        RunState run = startRun(seed);
        run.relics.clear();
        for (const auto& n : relicNames) {
            for (int i = 1; i <= RelicId_COUNT; ++i) {
                if (n == sts2RelicName(static_cast<RelicId>(i))) {
                    run.relics.push_back(i);
                    break;
                }
            }
        }
        return previewBattleStamp(run, encounterId);
    }, py::arg("seed"), py::arg("encounter"), py::arg("relics") = std::vector<std::string>{});
    m.def("card_implemented", [](int id) {
        return BattleContext::cardImplemented(static_cast<CardId>(id));
    });
    m.def("card_id_by_name", [](const std::string& name) {
        for (int i = 0; i < CardId_COUNT; ++i)
            if (name == CardId_NAMES[i]) return i;
        return -1;
    });
    m.def("card_info", [](int id) {
        const CardDef& d = BattleContext::cardDef(static_cast<CardId>(id));
        py::dict o;
        o["name"] = d.name;
        o["type"] = static_cast<int>(d.type);
        o["rarity"] = static_cast<int>(d.rarity);
        o["cost"] = d.cost;
        o["max_upgrade"] = d.maxUpgradeLevel;
        o["implemented"] = BattleContext::cardImplemented(static_cast<CardId>(id));
        o["exhaust"] = d.hasFlag(CF_EXHAUST);
        return o;
    });
    m.def("simulate", [](const std::string& seed, long long sims, int hpOverride,
                         const std::string& bossOverride) {
        RunState run = startRun(seed, hpOverride);
        run.bossOverride = bossOverride;
        int guard = 0;
        while (!run.over && guard++ < 500) {
            // Same scaffold policy as `run play`: resolve the previous room's pending
            // decisions (card pick, rest, event) before choosing the next map point.
            withDefaultDecisions(run);
            const int point = choosePoint(run);
            if (point < 0) break;
            enterPoint(run, point, sims);
        }
        withDefaultDecisions(run);
        return runSummary(run);
    }, py::arg("seed"), py::arg("sims") = 500, py::arg("hp_override") = 0,
       py::arg("boss_override") = "");
}
