#include "sts2/Sts2Run.h"
#include "sts2/Sts2Ascension.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "sts2/Sts2Ids.h"
#include "sts2/Sts2Enchantment.h"
#include "sts2/Sts2Stamp.h"
#include "sts2/search/Sts2Search.h"

namespace sts2 {

// Reward helpers defined later in this file; generateCombatReward/fight call them.
static RelicRarity rollRelicRarity(RunState& run);
static int pullRelic(RunState& run, RelicRarity rarity, bool fromBack);
static int rollPotionReward(RunState& run);
static void grantPotionById(RunState& run, int pid);
static std::vector<int> freshSavePotionOptions();

namespace {

const char* streamName(int idx) {
    switch (static_cast<RngStream>(idx)) {
        case RngStream::Shuffle: return "shuffle";
        case RngStream::CombatCardGeneration: return "combat_card_generation";
        case RngStream::CombatPotionGeneration: return "combat_potion_generation";
        case RngStream::CombatCardSelection: return "combat_card_selection";
        case RngStream::CombatEnergyCosts: return "combat_energy_costs";
        case RngStream::CombatTargets: return "combat_targets";
        case RngStream::CombatOrbGeneration: return "combat_orbs";
        case RngStream::MonsterAi: return "monster_ai";
        case RngStream::Niche: return "niche";
        default: return "";
    }
}

void unstableShuffleCards(std::vector<CardInstance>& v, Sts2Rng& rng) {
    int n = static_cast<int>(v.size());
    while (n > 1) {
        --n;
        const int j = rng.NextInt(n + 1);
        std::swap(v[n], v[j]);
    }
}

CardInstance makeCard(CardId id, int upgrade, int uniqueIdx) {
    CardInstance c;
    c.id = id;
    c.upgrade = upgrade;
    c.uniqueIdx = uniqueIdx;
    return c;
}

const char* actNameAt(int idx) {
    switch (idx) {
        case 0: return "OVERGROWTH";
        case 1: return "HIVE";
        case 2: return "GLORY";
        default: return "OVERGROWTH";
    }
}

void beginAct(RunState& run) {
    run.actName = actNameAt(run.actIndex);
    const ActConfig cfg = actConfigFor(run.actName);
    // Game: new Rng(RunState.Rng.Seed, $"act_{actIndex+1}_map")
    Sts2Rng mapRng(run.seed, "act_" + std::to_string(run.actIndex + 1) + "_map");
    const MapPointTypeCounts counts = rollPointTypeCounts(run.actName, mapRng, run.ascension);
    run.map = generateStandardActMap(mapRng, cfg.baseRooms, counts);
    // A10 DoubleBoss: the final act grows a second boss node past the first.
    if (run.actIndex == 2 && run.ascension >= 10) addSecondBossPoint(run.map);
    run.current = -1;
    run.actFloor = 0;
    run.visitedPoints.clear();
}

int pickEncounterIndex(RunState& run, bool elite, bool boss) {
    std::vector<int> pool;
    std::vector<int> fallback;
    const auto& all = scaffoldEncounters();
    for (int attempt = 0; attempt < 2; ++attempt) {
        pool.clear();
        fallback.clear();
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            if (elite && !all[i].elite) continue;
            if (boss && !all[i].boss) continue;
            if (!elite && !boss && (all[i].elite || all[i].boss)) continue;
            // A10: the second boss pick must differ from the first boss.
            if (attempt == 0 && boss && !run.secondBossExclude.empty()
                && all[i].id == run.secondBossExclude)
                continue;
            fallback.push_back(i);
            if (all[i].act == run.actIndex) pool.push_back(i);
        }
        if (!pool.empty() || attempt == 1) break;
        // only one implemented boss in this act: accept a repeat second boss
        run.secondBossExclude.clear();
    }
    // Acts with no implemented encounter of this category fall back to the whole
    // scaffold (content gap recorded by T2-E, not a silent substitution).
    if (pool.empty()) pool = fallback;
    if (pool.empty()) return -1;
    // The game rolls room contents upfront (RunRngSet.UpFront); the scaffold selection
    // keeps that stream so later reward rolls stay deterministic.
    return pool[run.upFront.NextInt(static_cast<int>(pool.size()))];
}

struct FightResult {
    bool ok = false;
    bool won = false;
    int hp = 0;
    int turns = 0;
    int goldGained = 0;
    bool potionReward = false;
    std::vector<std::pair<int,int>> cardChoices;
    int perfectFitMoves = 0; // diagnostic: PerfectFit cards moved to the pile top
    bool timedOut = false;   // BattlewornDummy: the dummy escaped on the time limit
};

// Encounter compositions that the game rolls at room entry. RNG seed matches
// EncounterModel.GenerateMonstersWithSlots: new Rng(RunSeed + TotalFloor + hash(id)).
std::vector<std::pair<std::string, std::string>> buildMonsterList(const EncounterDef& enc,
                                                                 RunState& run) {
    using P = std::pair<std::string, std::string>;
    std::vector<P> out;
    if (!enc.generator || !enc.generator[0]) {
        for (const auto& m : enc.monsters) out.push_back({m.first, m.second ? m.second : ""});
        return out;
    }
    Sts2Rng rng(run.seed + static_cast<std::uint64_t>(run.totalFloor) + xxhash64(enc.id));
    if (std::strcmp(enc.generator, "slimes_weak") == 0) {
        // SlimesWeak.GenerateMonsters: two distinct smalls (order: small1, medium, small2)
        // and one rolled medium.
        std::vector<std::string> smalls{"LEAF_SLIME_S", "TWIG_SLIME_S"};
        const int i1 = rng.NextInt(static_cast<int>(smalls.size()));
        const std::string s1 = smalls[i1];
        smalls.erase(smalls.begin() + i1);
        const std::string s2 = smalls[rng.NextInt(static_cast<int>(smalls.size()))];
        const std::vector<std::string> mediums{"LEAF_SLIME_M", "TWIG_SLIME_M"};
        const std::string med = mediums[rng.NextInt(static_cast<int>(mediums.size()))];
        out = {{s1, ""}, {med, ""}, {s2, ""}};
    } else if (std::strcmp(enc.generator, "slimes_normal") == 0) {
        // SlimesNormal.GenerateMonsters: NextBool picks the small pair order.
        const bool flag = rng.NextBool();
        const std::string a = flag ? "LEAF_SLIME_S" : "TWIG_SLIME_S";
        const std::string b = flag ? "TWIG_SLIME_S" : "LEAF_SLIME_S";
        out = {{"TWIG_SLIME_M", ""}, {"LEAF_SLIME_M", ""}, {a, ""}, {b, ""}};
    } else if (std::strcmp(enc.generator, "flyconid_normal") == 0) {
        // FlyconidNormal.GenerateMonsters: one rolled medium slime, then the Flyconid.
        const std::vector<std::string> mediums{"LEAF_SLIME_M", "TWIG_SLIME_M"};
        const std::string med = mediums[rng.NextInt(static_cast<int>(mediums.size()))];
        out = {{med, ""}, {"FLYCONID", ""}};
    } else if (std::strcmp(enc.generator, "strangler_normal") == 0) {
        // SlitheringStranglerNormal: secondary enemy rolled from
        // {SnappingJaxfruit, MediumSlime, SmallSlimes}, then the Strangler last.
        const int kind = rng.NextInt(3);
        if (kind == 0) {
            out = {{"SNAPPING_JAXFRUIT", ""}};
        } else if (kind == 1) {
            const std::vector<std::string> mediums{"LEAF_SLIME_M", "TWIG_SLIME_M"};
            out = {{mediums[rng.NextInt(2)], ""}};
        } else {
            const std::vector<std::string> smalls{"LEAF_SLIME_S", "TWIG_SLIME_S"};
            out = {{smalls[rng.NextInt(2)], ""}, {smalls[rng.NextInt(2)], ""}};
        }
        out.push_back({"SLITHERING_STRANGLER", ""});
    } else if (std::strcmp(enc.generator, "ruby_raiders") == 0) {
        // RubyRaidersNormal: three distinct raiders from the five, dictionary order.
        std::vector<std::string> pool{"AXE_RUBY_RAIDER", "ASSASSIN_RUBY_RAIDER",
                                      "BRUTE_RUBY_RAIDER", "CROSSBOW_RUBY_RAIDER",
                                      "TRACKER_RUBY_RAIDER"};
        for (int i = 0; i < 3; ++i) {
            const int pick = rng.NextInt(static_cast<int>(pool.size()));
            out.push_back({pool[pick], ""});
            pool.erase(pool.begin() + pick);
        }
    } else if (std::strcmp(enc.generator, "punch_off") == 0) {
        // PunchOffEventEncounter: two constructs, the first starts on FastPunch; each
        // rolls a 2..9 current-HP reduction at room entry (encounter Rng).
        const int r1 = rng.NextInt(2, 10);
        const int r2 = rng.NextInt(2, 10);
        out = {{"PUNCH_CONSTRUCT", "@1:hp" + std::to_string(r1)},
               {"PUNCH_CONSTRUCT", "@0:hp" + std::to_string(r2)}};
    } else if (std::strcmp(enc.generator, "scrolls_of_biting_weak") == 0) {
        // ScrollsOfBitingWeak.GenerateMonsters: one starter roll, then +1/+2 (mod 3).
        const int s0 = rng.NextInt(3);
        for (int k = 0; k < 3; ++k)
            out.push_back({"SCROLL_OF_BITING", "@" + std::to_string((s0 + k) % 3)});
    } else if (std::strcmp(enc.generator, "scrolls_of_biting_normal") == 0) {
        // ScrollsOfBitingNormal: four scrolls, the fourth always starts on MORE_TEETH.
        const int s0 = rng.NextInt(3);
        for (int k = 0; k < 3; ++k)
            out.push_back({"SCROLL_OF_BITING", "@" + std::to_string((s0 + k) % 3)});
        out.push_back({"SCROLL_OF_BITING", "@2"});
    }
    return out;
}

// Builds the opening battle state for an encounter (deck shuffle, monsters, relic
// hooks, opening draw). Shared by fight() and the preview/diagnostic entry point.
static bool buildBattle(RunState& run, const EncounterDef& enc,
                        const std::vector<std::pair<std::string, std::string>>& monsters,
                        BattleContext& bc) {

    bc.player.curHp = run.player.hp;
    bc.potionCap = run.potionSlots; // TightBelt/A4: the belt the battle may fill
    bc.player.maxHp = run.player.maxHp;
    bc.player.energy = 3;
    bc.player.maxEnergy = 3;
    bc.player.gold = run.player.gold;
    for (int i = 0; i < 3; ++i) bc.player.potions[i] = run.player.potions[i];
    bc.characterPool = run.player.character;
    // In-combat relic hooks. The opening effects below are already baked into real-game
    // root stamps; replay corpora do not re-run buildBattle.
    bc.relics = run.relics;
    if (bc.hasRelic(RelicId::AKABEKO)) bc.applyPowerToPlayer(PowerId::VIGOR, 8);
    if (bc.hasRelic(RelicId::BLOOD_VIAL))
        bc.player.curHp = std::min(bc.player.maxHp, bc.player.curHp + 2);
    // Pantograph.BeforeCombatStart: heal 25 in boss rooms.
    if (enc.boss && bc.hasRelic(RelicId::PANTOGRAPH))
        bc.player.curHp = std::min(bc.player.maxHp, bc.player.curHp + 25);
    if (bc.hasRelic(RelicId::VERY_HOT_COCOA)) bc.player.energy += 4;
    // BeltBuckle.BeforeCombatStart: an empty potion belt grants +2 Dexterity.
    if (bc.hasRelic(RelicId::BELT_BUCKLE) && bc.player.potions[0] == 0
        && bc.player.potions[1] == 0 && bc.player.potions[2] == 0) {
        bc.applyPowerToPlayer(PowerId::DEXTERITY, 2);
        bc.relicBeltBuckleApplied = true;
    }
    // Happy Flower's TurnsSeen persists across combats; the new combat's turn-1 tick
    // happens before the first play.
    if (bc.hasRelic(RelicId::HAPPY_FLOWER))
        bc.relicHappyTurns = (run.happyFlowerTurns + 1) % 3;
    bc.relicPenNibAttacks = run.penNibAttacks;
    bc.relicNunchakuAttacks = run.nunchakuAttacks;
    bc.relicTuningForkSkills = run.tuningForkSkills;
    bc.relicJossExhausted = run.jossPaperExhausted;
    if (bc.hasRelic(RelicId::PENDULUM))
        bc.relicPendulumTurns = (run.pendulumTurns + 1) % 3; // turn-1 tick
    bc.syncRedSkull();
    // Creature combat ids: the player is 0, enemies start at 1 (stamp convention).
    bc.nextCombatId = 1;
    for (int i = 0; i < RNG_STREAM_COUNT; ++i) bc.rng[i] = run.rng[i];

    int uid = 1000;
    for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
        const RunCard& card = run.player.deck[i];
        CardInstance inst = makeCard(card.id, card.upgrade, uid++);
        inst.enchant = card.enchant;
        inst.enchantAmount = card.enchantAmount;
        inst.enchantDisabled = card.enchantDisabled;
        inst.deckIdx = i;
        inst.tinkerType = card.tinkerType;
        inst.tinkerRider = card.tinkerRider;
        // Steady: OnEnchant adds the Retain keyword to the card.
        if (card.enchant == static_cast<int>(EnchantmentId::STEADY)) inst.retain = true;
        bc.drawPile.push_back(inst);
    }
    unstableShuffleCards(bc.drawPile, run.stream(RngStream::Shuffle));

    // Creature.Create + SetUniqueMonsterHpValue: Niche stream, one roll per monster,
    // excluding max HP values already chosen on the enemy side.
    std::vector<int> chosenMaxHp;
    for (const auto& ms : monsters) {
        const int defIdx = BattleContext::monsterDefIdx(ms.first);
        if (defIdx < 0) {
            std::fprintf(stderr, "MISSING_CONTENT monster=%s encounter=%s\n", ms.first.c_str(), enc.id);
            return false;
        }
        const MonsterDef& def = BattleContext::monsterDef(defIdx);
        Monster m;
        m.combatId = bc.nextCombatId++;
        m.defIdx = defIdx;
        m.name = def.name;
        // "@N" slot prefix selects the encounter-assigned starting move (Nibbit INIT).
        if (!ms.second.empty() && ms.second[0] == '@') {
            const size_t colon = ms.second.find(':');
            m.starterMoveIdx = std::atoi(ms.second.c_str() + 1);
            m.slot = colon == std::string::npos ? "-" : ms.second.substr(colon + 1);
        } else {
            m.slot = ms.second.empty() ? "-" : ms.second;
        }
        // Wriggler INIT (ConditionalBranchState): wriggler1/3 start on BITE, 2/4 on WRIGGLE.
        if (m.slot.rfind("wriggler", 0) == 0) {
            const int n = std::atoi(m.slot.c_str() + 8);
            m.slotInitAlt = (n % 2 == 1) ? 1 : 2;
        }
        // Exoskeleton INIT_MOVE (ConditionalBranchState on SlotName): first SKITTER,
        // second MANDIBLES, third ENRAGE, fourth RAND.
        if (std::strcmp(def.name, "EXOSKELETON") == 0) {
            if (m.slot == "first") m.slotInitAlt = 2;
            else if (m.slot == "second") m.slotInitAlt = 3;
            else if (m.slot == "third") m.slotInitAlt = 4;
            else if (m.slot == "fourth") m.slotInitAlt = 1;
        }
        std::vector<int> candidates;
        for (int hp = def.minHp; hp <= def.maxHp; ++hp) {
            if (std::find(chosenMaxHp.begin(), chosenMaxHp.end(), hp) == chosenMaxHp.end())
                candidates.push_back(hp);
        }
        int hp = def.maxHp;
        if (!candidates.empty())
            hp = candidates[run.stream(RngStream::Niche).NextInt(
                static_cast<int>(candidates.size()))];
        m.maxHp = hp;
        m.curHp = hp;
        // PunchOff constructs enter with a rolled current-HP reduction ("hpN" slot).
        if (m.slot.rfind("hp", 0) == 0) {
            m.curHp = std::max(1, hp - std::atoi(m.slot.c_str() + 2));
            m.slot = "-"; // the marker is not a real encounter slot name
        }
        m.block = def.startBlock;
        chosenMaxHp.push_back(hp);
        m.curState = def.startState;
        if (m.starterMoveIdx >= 0 && m.starterMoveIdx < static_cast<int>(def.states.size()))
            m.curState = m.starterMoveIdx;
        if (!def.states.empty()) {
            m.nextMove = def.states[m.curState].moveId;
            m.moveHistory.push_back(m.nextMove);
        }
        m.stockLeft = def.respawns ? 2 : 0;
        bc.applyStartPowers(m, def);
        bc.resolveInitialIntent(m); // first RollMove (branch start states only)
        bc.enemies.push_back(m);
    }

    // Turn-1 AfterSideTurnStart relics (Mercury Hourglass / Brimstone). Real-game root
    // stamps already carry these; replay corpora do not re-run buildBattle.
    bc.applyRelicTurnStart();
    // FestivePopper.AfterPlayerTurnStart turn 1: 9 unpowered damage to all enemies.
    if (bc.hasRelic(RelicId::FESTIVE_POPPER)) {
        for (int i = 0; i < (int)bc.enemies.size() && !bc.battleOver; ++i)
            if (!bc.enemies[i].removed && bc.enemies[i].curHp > 0)
                bc.damageEnemy(i, 9, /*powered=*/false);
    }
    // RedMask.BeforeSideTurnStart turn 1: Weak 1 to every hittable enemy.
    if (bc.hasRelic(RelicId::RED_MASK)) {
        for (int i = 0; i < (int)bc.enemies.size(); ++i)
            if (!bc.enemies[i].removed && bc.enemies[i].curHp > 0)
                bc.applyPowerToEnemy(i, PowerId::WEAK, 1);
    }
    // PetrifiedToad.BeforeCombatStartLate: procure POTION_SHAPED_ROCK.
    if (bc.hasRelic(RelicId::PETRIFIED_TOAD)) {
        for (int i = 0; i < bc.potionCap; ++i)
            if (bc.player.potions[i] == 0) {
                bc.player.potions[i] = static_cast<int>(PotionId::POTION_SHAPED_ROCK);
                break;
            }
    }
    // Bread.AfterSideTurnStart turn 1: lose 2 energy.
    if (bc.hasRelic(RelicId::BREAD)) bc.player.energy = std::max(0, bc.player.energy - 2);
    // VenerableTeaSet: the extra energy granted after a rest site heal.
    if (run.venerableTeaEnergy) {
        bc.player.energy += 1;
        run.venerableTeaEnergy = false;
    }
    // GhostSeed.AfterRoomEntered: Basic Strike/Defend cards gain Ethereal.
    if (bc.hasRelic(RelicId::GHOST_SEED)) {
        for (auto* pile : {&bc.hand, &bc.drawPile, &bc.discardPile, &bc.exhaustPile})
            for (auto& c : *pile) {
                const CardDef& cd = c.def();
                if (cd.rarity == CardRarity::BASIC && (cd.hasTag(CT_STRIKE) || cd.hasTag(CT_DEFEND))
                    && !(cd.hasFlag(CF_ETHEREAL) || c.ephemeral))
                    c.ephemeral = true;
            }
    }
    // Bellows.AfterPlayerTurnStart turn 1: upgrade every card in the opening hand.
    if (bc.hasRelic(RelicId::BELLOWS)) {
        for (auto& c : bc.hand)
            if (c.upgrade < c.def().maxUpgradeLevel) upgradeCardInstance(c);
    }
    // Toolbox.BeforeHandDraw turn 1: three distinct colorless cards, player picks one.
    // The battle cannot pause for an opening choice before the search loop, so the
    // scaffold takes the first offered card; the pool is filtered to implemented cards.
    if (bc.hasRelic(RelicId::TOOLBOX)) {
        int n = 0;
        const CardId* pool = sts2CombatGenPool("COLORLESS", &n);
        std::vector<CardId> cands;
        for (int i = 0; i < n; ++i)
            if (BattleContext::cardImplemented(pool[i])) cands.push_back(pool[i]);
        Sts2Rng& r = bc.rng[static_cast<int>(RngStream::CombatCardGeneration)];
        std::vector<CardId> offered;
        for (int k = 0; k < 3 && !cands.empty(); ++k) {
            const int pick = r.NextInt(static_cast<int>(cands.size()));
            offered.push_back(cands[pick]);
            cands.erase(cands.begin() + pick);
        }
        if (!offered.empty()) {
            CardInstance nc;
            nc.id = offered.front();
            nc.uniqueIdx = bc.nextUniqueIdx++;
            nc.genCard = true;
            if ((int)bc.hand.size() < bc.maxHandSize) bc.hand.push_back(nc);
            else bc.discardPile.push_back(nc);
        }
    }
    // VexingPuzzlebox.AfterPlayerTurnStart turn 1: a random character card, free this turn.
    if (bc.hasRelic(RelicId::VEXING_PUZZLEBOX)) {
        int n = 0;
        const CardId* pool = sts2RewardPool(bc.characterPool.c_str(), &n);
        if (pool && n > 0) {
            Sts2Rng& r = bc.rng[static_cast<int>(RngStream::CombatCardGeneration)];
            CardInstance nc;
            nc.id = pool[r.NextInt(n)];
            nc.uniqueIdx = bc.nextUniqueIdx++;
            nc.genCard = true;
            nc.freeThisTurn = true;
            if ((int)bc.hand.size() < bc.maxHandSize) bc.hand.push_back(nc);
            else bc.discardPile.push_back(nc);
        }
    }


    // ---- relic hooks (AfterRoomEntered(CombatRoom) / BeforeCombatStart) ----
    // Vajra/OddlySmoothStone/BronzeScales apply their power, Anchor its block, and Bag of
    // Marbles its Vulnerable before the first turn's state exists.
    int relicDraw = 0;
    for (int rid : run.relics) {
        const RelicDef& rd = sts2RelicDef(static_cast<RelicId>(rid));
        switch (rd.effect) {
            case RelicEffect::RELIC_POWER_SELF:
                bc.applyPowerToPlayer(rd.power, rd.amount);
                break;
            case RelicEffect::RELIC_BLOCK:
                bc.player.block += rd.amount; // unpowered
                break;
            case RelicEffect::RELIC_VULNERABLE_ALL:
                for (auto& e : bc.enemies)
                    if (!e.removed && e.curHp > 0)
                        bc.applyPowerToEnemy(static_cast<int>(&e - &bc.enemies[0]),
                                             PowerId::VULNERABLE, rd.amount);
                break;
            case RelicEffect::RELIC_DRAW:
                relicDraw += rd.amount;
                break;
            case RelicEffect::RELIC_ENERGY:
                bc.player.energy += rd.amount; // Lantern: first turn only
                break;
            case RelicEffect::RELIC_ENDTURN_BLOCK:
                bc.relicEndTurnBlock += rd.amount; // Orichalcum
                break;
            default:
                break;
        }
    }
    // Toolbox.BeforeHandDraw: the picked colorless card joins the opening hand before
    // the natural draw (generated combat card, deck flag False).
    if (run.toolboxCard != 0) {
        CardInstance tc = makeCard(static_cast<CardId>(run.toolboxCard), 0, uid++);
        tc.genCard = true;
        bc.hand.push_back(tc);
        run.toolboxCard = 0;
    }
    bc.drawCards(5 + relicDraw, /*naturalDraw=*/true);
    return true;
}

FightResult fight(RunState& run, const EncounterDef& enc,
                  const std::vector<std::pair<std::string, std::string>>& monsters,
                  long long searchSims) {
    FightResult out;
    out.hp = run.player.hp;

    BattleContext bc;
    if (!buildBattle(run, enc, monsters, bc)) return out;
    run.lastBattleStartEnergy = bc.player.energy;

    // Boss fights get a larger search budget, mirroring sts-rl-agent
    // ScumSearchAgent2 (MIT) where `simulationCount` is multiplied by
    // bossSimulationMultiplier = 3 for boss encounters. Measured on 500 seeded runs at
    // 300 sims: 48% of all runs end on the act-1 boss (floor 16-17), so this is the one
    // fight where extra search pays. STS2_BOSS_SIMS_MULT overrides the multiplier.
    static const double kBossSimsMultiplier = [] {
        const char* v = std::getenv("STS2_BOSS_SIMS_MULT");
        return v ? std::atof(v) : 3.0;
    }();
    // Low HP is where a single misplay ends the run: a won fight at 4 HP still loses the
    // next one. Scale the budget by HP fraction so the search spends most where a mistake
    // is fatal. STS2_LOWHP_MULT=1 disables it.
    static const double kLowHpMultiplier = [] {
        const char* v = std::getenv("STS2_LOWHP_MULT");
        return v ? std::atof(v) : 2.0;
    }();
    static const double kLowHpFrac = [] {
        const char* v = std::getenv("STS2_LOWHP_FRAC");
        return v ? std::atof(v) : 0.35;
    }();
    // Act-2 boss deserves more than the global boss multiplier: measured over 2000 seeds,
    // the act-1 boss is fought with a median 53 HP and lost 32% of the time, while the
    // act-2 boss is fought with 37 HP and lost 71%. Same boss cost (~30-32 HP), half the
    // arrival HP, so the same budget buys less there.
    static const double kAct2BossExtra = [] {
        const char* v = std::getenv("STS2_ACT2BOSS_SIMS_MULT");
        return v ? std::atof(v) : 1.0; // off by default: the effect is unmeasurable (see ledger)
    }();
    double simsMultiplier = enc.boss ? kBossSimsMultiplier : 1.0;
    if (enc.boss && run.actIndex == 1) simsMultiplier *= kAct2BossExtra;
    if (kLowHpMultiplier > 1.0 && run.player.maxHp > 0
        && static_cast<double>(run.player.hp) / run.player.maxHp < kLowHpFrac) {
        simsMultiplier *= kLowHpMultiplier;
    }
    // Early act-2 choke point. Measured with a per-room HP ledger (STS2_TRACE_HP): in act 2
    // a run meets ~14 monsters per rest site, because it dies on floors 17-18 before the
    // first act-2 rest. The recovery side is therefore not scarce in size (a rest heals
    // ~19-23 HP) but unreachable; the only lever left at that point is losing less per
    // fight. Scale the budget for the first floors of each act so the chokepoint gets more
    // search than the comfortable stretch.
    // STS2_EARLY_ACT_BOOST=1 disables it; STS2_EARLY_ACT_FLOORS sets the window (floor
    // index inside the act, 0-based).
    static const double kEarlyActBoost = [] {
        const char* v = std::getenv("STS2_EARLY_ACT_BOOST");
        return v ? std::atof(v) : 2.0;
    }();
    static const int kEarlyActFloors = [] {
        const char* v = std::getenv("STS2_EARLY_ACT_FLOORS");
        return v ? std::atoi(v) : 3;
    }();
    if (kEarlyActBoost > 1.0 && run.actFloor < kEarlyActFloors) simsMultiplier *= kEarlyActBoost;
    const long long simsForFight =
        std::max<long long>(1, static_cast<long long>(simsMultiplier * searchSims));

    // Search reuse, ported from sts-rl-agent ScumSearchAgent2::playoutBattle (MIT):
    // one search, then walk the best line for up to kStepsWithSolution actions before
    // re-searching; a line that did not reach a winning terminal walks fewer steps, the
    // same split as their stepThroughSolution vs stepThroughSearchTree. Re-searching
    // from the root at every action pays the whole budget again for one move.
    // Tuned through STS2_STEPS_SOLUTION / STS2_STEPS_NO_SOLUTION for A/B batches.
    static const int kStepsWithSolution = [] {
        const char* v = std::getenv("STS2_STEPS_SOLUTION");
        return v ? std::atoi(v) : 15;
    }();
    static const int kStepsNoSolution = [] {
        const char* v = std::getenv("STS2_STEPS_NO_SOLUTION");
        return v ? std::atoi(v) : 5;
    }();

    // Capture the act-2 boss entry position as a stamp, so the boss can later be
    // benchmarked as a fixed corpus instead of waiting for whole runs to reach it (500
    // seeded runs at 2000 sims reach it 43 times). Paired replay over these stamps is the
    // only way to compare this fight with any statistical power. Re-capture after any
    // macro change: the corpus is the entry distribution of the policy that produced it.
    if (enc.boss && run.actIndex == 1) {
        if (const char* dir = std::getenv("STS2_CAPTURE_ACT2BOSS")) {
            std::string path = std::string(dir) + "/" + run.stringSeed + "_" + enc.id + ".stamp";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                const std::string stamp = sts2::emitStamp(bc);
                std::fwrite(stamp.data(), 1, stamp.size(), f);
                std::fclose(f);
            }
        }
    }

    const int kMaxDecisions = 400;
    int decisions = 0;
    int searches = 0;
    while (!bc.battleOver && bc.turn <= 60 && decisions < kMaxDecisions
           && searches < kMaxDecisions) {
        search::BattleSearcher searcher(bc, 0xC0FFEEULL + static_cast<std::uint64_t>(searches), 60);
        // Potion policy: hold potions for the fights that decide the run. Measured on 500
        // seeded runs at 300 sims, the act-1 boss costs a median 45 HP (56% of an 80 HP
        // pool), so a run enters act 2 at ~0.43 of max HP; act 2 then recovers to ~0.95 by
        // its own boss, which costs a median 67 HP -- more than the pool, so that fight is
        // where runs die. A potion spent in a normal encounter buys a few HP; the same
        // potion at the boss or an elite buys much more. STS2_POTION_POLICY=always restores
        // spending them anywhere.
        static const bool kHoldPotions = [] {
            const char* v = std::getenv("STS2_POTION_POLICY");
            return !(v && std::string(v) == "always");
        }();
        if (kHoldPotions) {
            static const double kPotionHpFrac = [] {
                const char* v = std::getenv("STS2_POTION_HP_FRAC");
                return v ? std::atof(v) : 0.5;
            }();
            const double hpFrac = run.player.maxHp > 0
                ? static_cast<double>(run.player.hp) / run.player.maxHp : 1.0;
            searcher.allowPotions_ = enc.boss || enc.elite
                                     || hpFrac < kPotionHpFrac;
        }
        searcher.search(simsForFight);
        ++searches;
        const auto& res = searcher.result();
        if (res.bestSequence.empty()) break;
        const bool hasWin = res.reachedTerminal && res.outcomeHp > 0;
        const int steps = hasWin ? kStepsWithSolution : kStepsNoSolution;
        int walked = 0;
        for (; walked < steps && walked < static_cast<int>(res.bestSequence.size())
               && !bc.battleOver && decisions < kMaxDecisions; ++walked) {
            if (!search::applyAction(bc, res.bestSequence[walked])) break;
            ++decisions;
        }
        if (walked == 0) break; // the first action was rejected: nothing safe to do
    }

    // Relic counters that persist across combats (Pen Nib / Nunchaku).
    run.penNibAttacks = bc.relicPenNibAttacks;
    run.nunchakuAttacks = bc.relicNunchakuAttacks;
    run.happyFlowerTurns = bc.relicHappyTurns;
    run.tuningForkSkills = bc.relicTuningForkSkills;
    run.jossPaperExhausted = bc.relicJossExhausted;
    run.pendulumTurns = bc.relicPendulumTurns;
    for (int i = 0; i < RNG_STREAM_COUNT; ++i) run.rng[i] = bc.rng[i];
    for (int i = 0; i < 3; ++i) run.player.potions[i] = bc.player.potions[i]; // drunk potions leave
    // Sown disables itself on first play; the deck card keeps that status.
    for (int idx : bc.enchantDisabledDeckIdx)
        if (idx >= 0 && idx < static_cast<int>(run.player.deck.size()))
            run.player.deck[idx].enchantDisabled = true;
    out.ok = true;
    out.won = bc.playerWon;
    out.hp = bc.player.curHp;
    out.turns = bc.turn;
    out.perfectFitMoves = bc.perfectFitMoves;
    run.lastPerfectFitMoves = bc.perfectFitMoves;
    run.lastCardsGenerated = bc.cardsGeneratedByOwner;
    run.lastUnsupportedCard = bc.unsupportedCard;
    if (!bc.unsupportedCard.empty() && run.runUnsupportedCard.empty())
        run.runUnsupportedCard = bc.unsupportedCard;
    run.lastSharpApplications = bc.sharpApplications;
    run.lastNimbleApplications = bc.nimbleApplications;
    run.lastSlitherDraws = bc.slitherDraws;
    run.lastToricRegrants = bc.toricRegrants;
    run.lastSpiralReplays = bc.spiralReplays;
    run.lastVigorousApplications = bc.vigorousApplications;
    run.lastCorruptedTriggers = bc.corruptedTriggers;
    run.lastSoulsPowerKept = bc.soulsPowerKept;
    out.timedOut = bc.battleTimedOut;
    // ImprovementPower.AfterCombatEnd: upgrade Amount random upgradable deck cards on
    // the CombatCardSelection stream.
    for (const PowerInst& pw : bc.player.powers) {
        if (pw.id != PowerId::IMPROVEMENT) continue;
        for (int k = 0; k < pw.amount; ++k) {
            std::vector<int> up;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& card = run.player.deck[i];
                if (card.upgrade < BattleContext::cardDef(card.id).maxUpgradeLevel)
                    up.push_back(i);
            }
            if (up.empty()) break;
            const int pick = up[bc.rng[static_cast<int>(RngStream::CombatCardSelection)]
                                  .NextInt(static_cast<int>(up.size()))];
            ++run.player.deck[pick].upgrade;
        }
        break;
    }
    run.lastFightWon = bc.playerWon;
    run.lastFightTimedOut = bc.battleTimedOut;
    run.lastFightTurns = bc.turn;
    {
        int left = 0;
        for (const auto& m : bc.enemies)
            if (!m.removed && m.curHp > 0) left += m.curHp;
        run.lastFightEnemyHpLeft = left;
    }
    run.player.hp = bc.player.curHp;
    run.player.maxHp = bc.player.maxHp;
    if (out.won && !enc.noRewards) {
        // Burning Blood (AfterCombatEnd): heal 6.
        if (run.player.hp > 0)
            for (int rid : run.relics) {
                const RelicDef& rd = sts2RelicDef(static_cast<RelicId>(rid));
                if (rd.effect == RelicEffect::RELIC_POST_COMBAT_HEAL)
                    run.player.hp = std::min(run.player.maxHp, run.player.hp + rd.amount);
                // MeatOnTheBone: heal 12 after a combat win when at or below 50% HP.
                else if (rid == static_cast<int>(RelicId::MEAT_ON_THE_BONE)
                         && run.player.hp * 2 <= run.player.maxHp)
                    run.player.hp = std::min(run.player.maxHp, run.player.hp + 12);
            }
        CombatReward rw = generateCombatReward(run, enc.elite, enc.boss, enc.goldReward);
        // LavaLamp.TryModifyCardRewardOptionsLate: no unblocked damage this combat upgrades
        // every card reward.
        if (run.relics.end() != std::find(run.relics.begin(), run.relics.end(),
                                         static_cast<int>(RelicId::LAVA_LAMP))
            && !bc.relicLavaLampTookDamage) {
            for (auto& c : rw.cards) {
                const CardDef& cd = BattleContext::cardDef(static_cast<CardId>(c.first));
                if (cd.maxUpgradeLevel > 0) c.second = 1;
            }
        }
        out.goldGained = rw.gold;
        out.potionReward = rw.potion;
        out.cardChoices = rw.cards;
        // Reward screen order: potion, card, relic. Each optional reward stops as a
        // pending decision; the caller resolves potion -> card -> relic.
        if (rw.potionId) {
            run.awaitingPotionReward = true;
            run.potionRewardId = rw.potionId;
        }
        run.awaitingCardReward = true;
        run.cardOffer = rw.cards;
        if (rw.relicId) {
            run.awaitingRelicReward = true;
            run.relicRewardId = rw.relicId;
        }
    }
    return out;
}

} // namespace

CombatReward generateCombatReward(RunState& run, bool elite, bool boss, int goldReward) {
    CombatReward out;
    // RewardsSet.WithRewardsFromRoom builds the list first: the potion ROLL happens there,
    // before any reward is populated. Populate then runs in insertion order
    // (gold -> potion -> card -> relic), so the reward-stream order is:
    //   potion roll, gold, [potion rarity+pick], card x3, [elite relic rarity].
    // PotionRewardOdds.Roll: pity state starts at 0.4; elite adds 0.25*0.5.
    const float potionBonus = elite ? 0.125f : 0.0f;
    // WhiteBeastStatue.ShouldForcePotionReward: combat rooms always offer a potion; the
    // source returns before the odds roll, so no Rewards float is consumed.
    const bool forcePotion = std::find(run.relics.begin(), run.relics.end(),
        static_cast<int>(RelicId::WHITE_BEAST_STATUE)) != run.relics.end();
    if (forcePotion) {
        out.potion = true;
    } else {
        const float potionRoll = run.rewards.NextFloat();
        out.potion = potionRoll < run.potionOdds + potionBonus;
        run.potionOdds += out.potion ? -0.1f : 0.1f;
    }

    // GoldReward.Populate: EncounterModel defaults, single-player GoldProportion = 1.
    int minGold = 10, maxGold = 20;
    if (elite) { minGold = 35; maxGold = 45; }
    if (boss) { minGold = 100; maxGold = 100; }
    if (run.ascension >= 3) {
        // Poverty: EncounterModel Min/MaxGoldReward x0.75, truncated to int.
        minGold = static_cast<int>(minGold * 0.75);
        maxGold = static_cast<int>(maxGold * 0.75);
    }
    if (goldReward >= 0) { minGold = goldReward; maxGold = goldReward; }
    out.gold = run.rewards.NextInt(minGold, maxGold + 1);
    run.player.gold += out.gold;

    // PotionReward.Populate -> PotionFactory.CreateRandomPotionOutOfCombat: one rarity
    // roll and one uniform pick. NextInt consumes one draw regardless of the range, so
    // resolving the identity keeps the RNG sequence identical to the old placeholder.
    if (out.potion) out.potionId = rollPotionReward(run);

    // CardReward.Populate x3: CardRarityOdds.Roll (accumulating offset, NOT base odds),
    // then the pick, then the always-consumed upgrade roll.
    // Scarcity (A7) lowers the rare odds: 0.03->0.0149 regular, 0.1->0.05 elite.
    const float baseRare = boss ? 1.0f
        : (elite ? (run.ascension >= 7 ? 0.05f : 0.1f)
                 : (run.ascension >= 7 ? 0.0149f : 0.03f));
    const float baseUncommon = boss ? 0.0f : (elite ? 0.4f : 0.37f);
    int poolCount = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &poolCount);
    std::vector<CardId> offered; // blacklist: the three offers are distinct
    for (int pickNo = 0; pickNo < 3 && pool && poolCount > 0; ++pickNo) {
        // CardRarityOdds.Roll: BossEncounter passes offset 0 (not the accumulated value).
        const float offset = boss ? 0.0f : run.cardRarityOffset;
        const float rareThreshold = baseRare + offset;
        const float uncommonThreshold = baseUncommon + rareThreshold;
        const float r = run.rewards.NextFloat();
        const CardRarity want = r < rareThreshold ? CardRarity::RARE
            : (r < uncommonThreshold ? CardRarity::UNCOMMON : CardRarity::COMMON);
        std::vector<CardId> candidates;
        for (int i = 0; i < poolCount; ++i) {
            const CardId cid = pool[i];
            if (BattleContext::cardDef(cid).rarity != want) continue;
            if (std::find(offered.begin(), offered.end(), cid) != offered.end()) continue;
            candidates.push_back(cid);
        }
        if (candidates.empty()) continue;
        const int pickIdx = run.rewards.NextInt(static_cast<int>(candidates.size()));
        const CardId chosen = candidates[pickIdx];
        if (std::getenv("STS2_REWARD_DEBUG"))
            std::fprintf(stderr, "card%d r=%.4f off=%.3f want=%d cands=%zu idx=%d chosen=%s\n",
                         pickNo, r, offset, (int)want, candidates.size(), pickIdx,
                         BattleContext::cardDef(chosen).name);
        const CardDef& cd = BattleContext::cardDef(chosen);
        // RollForUpgrade: the roll is always consumed; Rare cards never upgrade.
        const float upRoll = run.rewards.NextFloat();
        int upgrade = 0;
        if (cd.maxUpgradeLevel > 0 && cd.rarity != CardRarity::RARE
            && upRoll <= static_cast<float>(run.actIndex)
                     * (run.ascension >= 7 ? 0.125f : 0.25f))
            upgrade = 1;
        offered.push_back(chosen);
        out.cards.emplace_back(static_cast<int>(chosen), upgrade);
        // Roll(): a rare resets the offset, otherwise it grows by 1% (ascension 0).
        run.cardRarityOffset = want == CardRarity::RARE
            ? -0.05f : std::min(run.cardRarityOffset
                                + (run.ascension >= 7 ? 0.005f : 0.01f), 0.4f);
    }

    // LastingCandy.TryModifyCardRewardOptions: on odd combat-reward counts add a fourth
    // distinct card option (same rarity roll/pick/upgrade stream shape).
    if (std::find(run.relics.begin(), run.relics.end(),
                  static_cast<int>(RelicId::LASTING_CANDY)) != run.relics.end()) {
        ++run.lastingCandyRewards;
        if (run.lastingCandyRewards % 2 == 1 && pool && poolCount > 0) {
            const float offset = boss ? 0.0f : run.cardRarityOffset;
            const float rareThreshold = baseRare + offset;
            const float uncommonThreshold = baseUncommon + rareThreshold;
            const float r = run.rewards.NextFloat();
            const CardRarity want = r < rareThreshold ? CardRarity::RARE
                : (r < uncommonThreshold ? CardRarity::UNCOMMON : CardRarity::COMMON);
            std::vector<CardId> candidates;
            for (int i = 0; i < poolCount; ++i) {
                const CardId cid = pool[i];
                if (BattleContext::cardDef(cid).rarity != want) continue;
                if (std::find(offered.begin(), offered.end(), cid) != offered.end()) continue;
                candidates.push_back(cid);
            }
            if (!candidates.empty()) {
                const int pickIdx = run.rewards.NextInt(static_cast<int>(candidates.size()));
                const CardId chosen = candidates[pickIdx];
                const CardDef& cd = BattleContext::cardDef(chosen);
                const float upRoll = run.rewards.NextFloat();
                int upgrade = 0;
                if (cd.maxUpgradeLevel > 0 && cd.rarity != CardRarity::RARE
                    && upRoll <= static_cast<float>(run.actIndex)
                     * (run.ascension >= 7 ? 0.125f : 0.25f))
                    upgrade = 1;
                out.cards.emplace_back(static_cast<int>(chosen), upgrade);
            }
        }
    }

    // EliteReward: RelicReward.Populate -> RelicFactory.PullNextRelicFromFront. One
    // rarity roll on the Rewards stream, then a pull from the front of that bag (no RNG).
    // Boss/normal rooms add no relic reward.
    if (elite) {
        const RelicRarity rarity = rollRelicRarity(run);
        out.relicId = pullRelic(run, rarity, /*fromBack=*/false);
    }
    return out;
}

const std::vector<EncounterDef>& scaffoldEncounters() {
    // Static compositions come from the frozen parity corpus root stamps (game output);
    // rolled compositions follow the encounter classes in the decompiled source.
    static const std::vector<EncounterDef> table = {
        {"FUZZY_WURM_CRAWLER_WEAK", false, false, 0, "", {{"FUZZY_WURM_CRAWLER", "-"}}},
        {"NIBBITS_WEAK", false, false, 0, "", {{"NIBBIT", "-"}}},
        {"NIBBITS_NORMAL", false, false, 0, "", {{"NIBBIT", "@1"}, {"NIBBIT", "@2"}}},
        {"FOGMOG_NORMAL", false, false, 0, "", {{"FOGMOG", "-"}}},
        {"SLIMES_WEAK", false, false, 0, "slimes_weak", {}},
        {"SLIMES_NORMAL", false, false, 0, "slimes_normal", {}},
        {"SHRINKER_BEETLE_WEAK", false, false, 0, "", {{"SHRINKER_BEETLE", "-"}}},
        {"OVERGROWTH_CRAWLERS", false, false, 0, "",
         {{"SHRINKER_BEETLE", "-"}, {"FUZZY_WURM_CRAWLER", "-"}}},
        {"MAWLER_NORMAL", false, false, 0, "", {{"MAWLER", "-"}}},
        {"SNAPPING_JAXFRUIT_NORMAL", false, false, 0, "",
         {{"SNAPPING_JAXFRUIT", "-"}, {"FLYCONID", "-"}}},
        {"FLYCONID_NORMAL", false, false, 0, "flyconid_normal", {}},
        {"INKLETS_NORMAL", false, false, 0, "",
         {{"INKLET", ""}, {"INKLET", "@2"}, {"INKLET", ""}}},
        {"BYGONE_EFFIGY_ELITE", true, false, 0, "", {{"BYGONE_EFFIGY", "-"}}},
        {"CUBEX_CONSTRUCT_NORMAL", false, false, 0, "", {{"CUBEX_CONSTRUCT", "-"}}},
        {"SLITHERING_STRANGLER_NORMAL", false, false, 0, "strangler_normal", {}},
        {"VINE_SHAMBLER_NORMAL", false, false, 0, "", {{"VINE_SHAMBLER", "-"}}},
        {"PHROG_PARASITE_ELITE", true, false, 0, "", {{"PHROG_PARASITE", "phrog"}}},
        {"RUBY_RAIDERS_NORMAL", false, false, 0, "ruby_raiders", {}},
        {"BOWLBUGS_NORMAL", false, false, 1, "", {{"BOWLBUG_ROCK", "-"}, {"BOWLBUG_NECTAR", "-"}}},
        {"EXOSKELETONS_WEAK", false, false, 1, "",
         {{"EXOSKELETON", "@2:first"}, {"EXOSKELETON", "@3:second"}, {"EXOSKELETON", "@4:third"}}},
        {"EXOSKELETONS_NORMAL", false, false, 1, "",
         {{"EXOSKELETON", "@0:first"}, {"EXOSKELETON", "@0:second"},
          {"EXOSKELETON", "@0:third"}, {"EXOSKELETON", "@0:fourth"}}},
        {"TUNNELER_WEAK", false, false, 1, "", {{"TUNNELER", "-"}}},
        {"CHOMPERS_NORMAL", false, false, 1, "", {{"CHOMPER", "-"}, {"CHOMPER", "@1"}}},
        {"SPINY_TOAD_NORMAL", false, false, 1, "", {{"SPINY_TOAD", "-"}}},
        {"MYTES_NORMAL", false, false, 1, "",
         {{"MYTE", "first"}, {"MYTE", "@2:second"}}},
        {"SLUMBERING_BEETLE_NORMAL", false, false, 1, "",
         {{"BOWLBUG_ROCK", "first"}, {"BOWLBUG_SILK", "second"},
          {"SLUMBERING_BEETLE", "third"}}},
        {"THE_OBSCURA_NORMAL", false, false, 1, "", {{"THE_OBSCURA", "obscura"}}},
        {"THIEVING_HOPPER_WEAK", false, false, 1, "", {{"THIEVING_HOPPER", "-"}}},
        {"ENTOMANCER_ELITE", true, false, 1, "", {{"ENTOMANCER", "-"}}},
        {"THE_INSATIABLE_BOSS", false, true, 1, "", {{"THE_INSATIABLE", "-"}}},
        {"KNOWLEDGE_DEMON_BOSS", false, true, 1, "", {{"KNOWLEDGE_DEMON", "-"}}},
        {"KAISER_CRAB_BOSS", false, true, 1, "",
         {{"CRUSHER", "crusher"}, {"ROCKET", "rocket"}}},
        // DenseVegetation event fight: four non-stunned Wrigglers (wriggler1..4 INIT).
        {"DENSE_VEGETATION_ENCOUNTER", false, false, -1, "",
         {{"WRIGGLER", "wriggler1"}, {"WRIGGLER", "wriggler2"},
          {"WRIGGLER", "wriggler3"}, {"WRIGGLER", "wriggler4"}}},
        {"PUNCH_OFF_ENCOUNTER", false, false, -1, "punch_off", {}},
        {"BATTLEWORN_DUMMY_V1", false, false, -1, "", {{"BATTLE_FRIEND_V1", "-"}}, true},
        {"BATTLEWORN_DUMMY_V2", false, false, -1, "", {{"BATTLE_FRIEND_V2", "-"}}, true},
        {"BATTLEWORN_DUMMY_V3", false, false, -1, "", {{"BATTLE_FRIEND_V3", "-"}}, true},
        {"MYSTERIOUS_KNIGHT_EVENT", false, false, 1, "", {{"MYSTERIOUS_KNIGHT", "-"}}},
        {"FAKE_MERCHANT_EVENT", false, false, 1, "", {{"FAKE_MERCHANT_MONSTER", "merchant"}}, false, 300},
        {"INFESTED_PRISMS_ELITE", true, false, 1, "", {{"INFESTED_PRISM", "-"}}},
        {"HUNTER_KILLER_NORMAL", false, false, 1, "", {{"HUNTER_KILLER", "-"}}},
        {"LOUSE_PROGENITOR_NORMAL", false, false, 1, "", {{"LOUSE_PROGENITOR", "-"}}},
        {"OVICOPTER_NORMAL", false, false, 1, "", {{"OVICOPTER", "ovicopter"}}},
        {"VANTOM_BOSS", false, true, 0, "", {{"VANTOM", "-"}}},
        {"CEREMONIAL_BEAST_BOSS", false, true, 0, "", {{"CEREMONIAL_BEAST", "-"}}},
        {"THE_KIN_BOSS", false, true, 0, "",
         {{"KIN_FOLLOWER", "@2:slot1"}, {"KIN_FOLLOWER", "slot2"}, {"KIN_PRIEST", "leaderSlot"}}},
        {"BOWLBUGS_WEAK", false, false, 1, "", {{"BOWLBUG_ROCK", "-"}, {"BOWLBUG_NECTAR", "-"}}},
        {"AXEBOTS_NORMAL", false, false, 2, "", {{"AXEBOT", "-"}}},
        {"CONSTRUCT_MENAGERIE_NORMAL", false, false, 2, "",
         {{"PUNCH_CONSTRUCT", "-"}, {"CUBEX_CONSTRUCT", "-"}, {"CUBEX_CONSTRUCT", "-"}}},
        {"DEVOTED_SCULPTOR_WEAK", false, false, 2, "", {{"DEVOTED_SCULPTOR", "-"}}},
        {"SCROLLS_OF_BITING_WEAK", false, false, 2, "scrolls_of_biting_weak", {}},
        {"SLIMED_BERSERKER_NORMAL", false, false, 2, "", {{"SLIMED_BERSERKER", "-"}}},
        {"SCROLLS_OF_BITING_NORMAL", false, false, 2, "scrolls_of_biting_normal", {}},
        {"OWL_MAGISTRATE_NORMAL", false, false, 2, "", {{"OWL_MAGISTRATE", "-"}}},
        {"FROG_KNIGHT_NORMAL", false, false, 2, "", {{"FROG_KNIGHT", "-"}}},
        {"TURRET_OPERATOR_WEAK", false, false, 2, "",
         {{"LIVING_SHIELD", "-"}, {"TURRET_OPERATOR", "-"}}},
        {"THE_LOST_AND_FORGOTTEN_NORMAL", false, false, 2, "",
         {{"THE_LOST", "-"}, {"THE_FORGOTTEN", "-"}}},
        {"GLOBE_HEAD_NORMAL", false, false, 2, "", {{"GLOBE_HEAD", "-"}}},
        {"KNIGHTS_ELITE", true, false, 2, "",
         {{"FLAIL_KNIGHT", "first"}, {"SPECTRAL_KNIGHT", "second"},
          {"MAGI_KNIGHT", "third"}}},
        {"MECHA_KNIGHT_ELITE", true, false, 2, "", {{"MECHA_KNIGHT", "-"}}},
        {"SOUL_NEXUS_ELITE", true, false, 2, "", {{"SOUL_NEXUS", "-"}}},
        {"FABRICATOR_NORMAL", false, false, 2, "", {{"FABRICATOR", "fabricator"}}},
        {"QUEEN_BOSS", false, true, 2, "",
         {{"TORCH_HEAD_AMALGAM", "amalgam"}, {"QUEEN", "queen"}}},
        {"TEST_SUBJECT_BOSS", false, true, 2, "", {{"TEST_SUBJECT", "-"}}},
        {"BYRDONIS_ELITE", true, false, 0, "", {{"BYRDONIS", "-"}}},
        {"DECIMILLIPEDE_ELITE", true, false, 1, "",
         {{"DECIMILLIPEDE_SEGMENT_FRONT", "-"},
          {"DECIMILLIPEDE_SEGMENT_MIDDLE", "-"},
          {"DECIMILLIPEDE_SEGMENT_BACK", "-"}}},
        {"AEONGLASS_BOSS", false, true, 2, "", {{"AEONGLASS", "-"}}},
    };
    return table;
}

const EncounterDef* findEncounter(const std::string& id) {
    for (const auto& e : scaffoldEncounters())
        if (id == e.id) return &e;
    return nullptr;
}

// ---------------- relics (RelicGrabBag / RelicFactory / MerchantRelicEntry) ----------------

static bool relicRarityRollable(RelicRarity r) {
    return r == RelicRarity::COMMON || r == RelicRarity::UNCOMMON
        || r == RelicRarity::RARE || r == RelicRarity::SHOP;
}

// RelicGrabBag.Populate: shared pool first, then the character pool; one deque per
// rarity, each UnstableShuffle'd with the run's UpFront stream.
static void populateRelicBag(RunState& run) {
    for (auto& deque : run.relicBag) deque.clear();
    for (int pass = 0; pass < 2; ++pass) {
        for (int i = 1; i <= RelicId_COUNT; ++i) {
            const RelicDef& r = sts2RelicDef(static_cast<RelicId>(i));
            if (pass == 0 && !r.sharedPool) continue;
            if (pass == 1 && !r.ironcladPool) continue;
            if (!relicRarityRollable(r.rarity)) continue;
            run.relicBag[static_cast<int>(r.rarity)].push_back(i);
        }
    }
    for (auto& deque : run.relicBag) {
        for (int i = static_cast<int>(deque.size()) - 1; i > 0; --i) {
            const int j = run.upFront.NextInt(i + 1);
            std::swap(deque[i], deque[j]);
        }
    }
}

// RelicFactory.RollRarity: Rewards float; <0.5 Common, <0.83 Uncommon, else Rare.
static RelicRarity rollRelicRarity(RunState& run) {
    const float f = run.rewards.NextFloat();
    if (f < 0.5f) return RelicRarity::COMMON;
    if (f < 0.83f) return RelicRarity::UNCOMMON;
    return RelicRarity::RARE;
}

// RelicGrabBag.PullFromBack (shops) / PullFromFront (elite rewards later).
static int pullRelic(RunState& run, RelicRarity rarity, bool fromBack) {
    auto& deque = run.relicBag[static_cast<int>(rarity)];
    if (deque.empty()) return static_cast<int>(RelicId::INVALID);
    const int id = fromBack ? deque.back() : deque.front();
    if (fromBack) deque.pop_back();
    else deque.erase(deque.begin());
    return id;
}

// RelicModel.MerchantCost by rarity (non-ascension).
static int relicMerchantCost(RelicRarity rarity) {
    switch (rarity) {
        case RelicRarity::COMMON: return 175;
        case RelicRarity::UNCOMMON: return 225;
        case RelicRarity::RARE: return 275;
        case RelicRarity::SHOP: return 200;
        default: return 999999999;
    }
}

// ---------------- merchant inventory (MerchantInventory.cs) ----------------

// CardRarityOdds: shopUncommonOdds = 0.37, ShopRareOdds 0.09 (Scarcity/A7: 0.045).
static constexpr float CardRarityOddsShopUncommon = 0.37f;
static constexpr float CardRarityOddsShopRareBase = 0.09f;
static constexpr float CardRarityOddsShopRareAscension = 0.045f;

// MerchantCardEntry.GetCost: Rare 150 / Uncommon 75 / else 50; colorless x1.15.
static int merchantCardBaseCost(CardRarity rarity, bool colorless) {
    int cost = rarity == CardRarity::RARE ? 150
             : rarity == CardRarity::UNCOMMON ? 75 : 50;
    if (colorless) cost = static_cast<int>(std::lround(static_cast<float>(cost) * 1.15f));
    return cost;
}

// CardFactory.CreateForMerchant: non-Basic cards of the requested rarity/type, excluding
// cards already on the shelf; GetNextAllowedRarity walks Common -> Uncommon -> Rare ->
// Common until a non-empty bucket appears. The pick draws on the Shops stream.
static int pickMerchantCard(RunState& run, const CardId* pool, int count, int typeFilter,
                            CardRarity want, const std::vector<int>& used) {
    CardRarity rarity = want;
    for (int hop = 0; hop < 4; ++hop) {
        std::vector<CardId> cand;
        for (int i = 0; i < count; ++i) {
            const CardDef& d = BattleContext::cardDef(pool[i]);
            if (d.rarity == CardRarity::BASIC) continue;
            if (d.rarity != rarity) continue;
            if (typeFilter >= 0 && static_cast<int>(d.type) != typeFilter) continue;
            bool taken = false;
            for (int u : used) if (u == static_cast<int>(pool[i])) taken = true;
            if (!taken) cand.push_back(pool[i]);
        }
        if (!cand.empty())
            return static_cast<int>(cand[run.shops.NextInt(static_cast<int>(cand.size()))]);
        rarity = rarity == CardRarity::COMMON ? CardRarity::UNCOMMON
               : rarity == CardRarity::UNCOMMON ? CardRarity::RARE : CardRarity::COMMON;
    }
    return -1;
}

// MerchantInventory.CreateForNormalMerchant, card shelves only: 5 character cards
// ([Attack, Attack, Skill, Skill, Power] with one random sale slot) and 2 colorless cards
// (Uncommon, Rare). Relic/potion shelves are content the engine does not model yet.
void generateShop(RunState& run) {
    run.shop.clear();
    static const CardType kTypes[5] = {CardType::ATTACK, CardType::ATTACK,
                                       CardType::SKILL, CardType::SKILL, CardType::POWER};
    const int saleIdx = run.shops.NextInt(5);
    int count = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &count);
    std::vector<int> used;
    for (int i = 0; i < 5 && pool && count > 0; ++i) {
        // CardRarityOdds.RollWithoutChangingFutureOdds(Shop): Rewards float, no offset change.
        const float offset = run.cardRarityOffset;
        const float r = run.rewards.NextFloat();
        const float rareThreshold =
            (run.ascension >= 7 ? CardRarityOddsShopRareAscension
                                : CardRarityOddsShopRareBase) + offset;
        const CardRarity want = r < rareThreshold ? CardRarity::RARE
            : (r < CardRarityOddsShopUncommon + rareThreshold ? CardRarity::UNCOMMON
                                                             : CardRarity::COMMON);
        const int pick = pickMerchantCard(run, pool, count, static_cast<int>(kTypes[i]),
                                          want, used);
        if (pick < 0) continue;
        used.push_back(pick);
        // RollForUpgrade(baseChance = -999999999): never upgrades, always consumes a float.
        run.rewards.NextFloat();
        ShopEntry e;
        e.cardId = pick;
        e.onSale = (i == saleIdx);
        const int base = merchantCardBaseCost(BattleContext::cardDef((CardId)pick).rarity, false);
        float price = static_cast<float>(base) * run.shops.NextFloat(0.95f, 1.05f);
        if (e.onSale) {
            // SetOnSale() calls CalcCost a second time with a fresh float, then halves.
            price = static_cast<float>(base) * run.shops.NextFloat(0.95f, 1.05f);
            e.price = static_cast<int>(std::lround(price)) / 2;
        } else {
            e.price = static_cast<int>(std::lround(price));
        }
        run.shop.push_back(e);
    }
    // Colorless: fixed rarities Uncommon then Rare (no rarity roll).
    int ccount = 0;
    const CardId* cpool = sts2CombatGenPool("COLORLESS", &ccount);
    std::vector<int> cused;
    for (CardRarity rarity : {CardRarity::UNCOMMON, CardRarity::RARE}) {
        if (!cpool || ccount <= 0) break;
        const int pick = pickMerchantCard(run, cpool, ccount, -1, rarity, cused);
        if (pick < 0) continue;
        cused.push_back(pick);
        run.rewards.NextFloat(); // upgrade roll (never upgrades)
        ShopEntry e;
        e.cardId = pick;
        const int base = merchantCardBaseCost(BattleContext::cardDef((CardId)pick).rarity, true);
        e.price = static_cast<int>(std::lround(
            static_cast<float>(base) * run.shops.NextFloat(0.95f, 1.05f)));
        run.shop.push_back(e);
    }
    // PopulateRelicEntries: two RollRarity slots plus one fixed Shop slot; the pull takes
    // from the BACK of the rarity deque and the price is MerchantCost * 0.85..1.15.
    RelicRarity relicRarities[3] = {rollRelicRarity(run), rollRelicRarity(run),
                                    RelicRarity::SHOP};
    for (int i = 0; i < 3; ++i) {
        const RelicRarity rarity = relicRarities[i];
        const int rid = pullRelic(run, rarity, /*fromBack=*/true);
        if (rid == static_cast<int>(RelicId::INVALID)) continue;
        ShopEntry e;
        e.relic = true;
        e.relicId = rid;
        e.price = static_cast<int>(std::lround(
            static_cast<float>(relicMerchantCost(sts2RelicDef(static_cast<RelicId>(rid)).rarity))
            * run.shops.NextFloat(0.85f, 1.15f)));
        run.shop.push_back(e);
    }
    // PopulatePotionEntries: 3 potions from the Shops stream (rarity roll + uniform pick
    // without replacement), priced 50/75/100 * 0.95..1.05.
    {
        std::vector<int> options = freshSavePotionOptions();
        for (int i = 0; i < 3; ++i) {
            const float r = run.shops.NextFloat();
            const PotionRarity want = r <= 0.1f ? PotionRarity::RARE
                : (r <= 0.35f ? PotionRarity::UNCOMMON : PotionRarity::COMMON);
            std::vector<int> cand;
            for (int id : options)
                if (sts2PotionDef(static_cast<PotionId>(id)).rarity == want) cand.push_back(id);
            if (cand.empty()) continue;
            const int pick = cand[run.shops.NextInt(static_cast<int>(cand.size()))];
            options.erase(std::remove(options.begin(), options.end(), pick), options.end());
            ShopEntry e;
            e.potion = true;
            e.potionId = pick;
            const int base = want == PotionRarity::RARE ? 100
                           : want == PotionRarity::UNCOMMON ? 75 : 50;
            e.price = static_cast<int>(std::lround(
                static_cast<float>(base) * run.shops.NextFloat(0.95f, 1.05f)));
            run.shop.push_back(e);
        }
    }
    // MerchantCardRemovalEntry: 75 + 25 per removal already bought this run
    // (Inflation/A6: base 100, +50 per removal).
    const int rmBase = run.ascension >= 6 ? 100 : 75;
    const int rmStep = run.ascension >= 6 ? 50 : 25;
    ShopEntry removal;
    removal.removal = true;
    removal.price = rmBase + rmStep * run.cardRemovalsUsed;
    run.shop.push_back(removal);
}

bool buyShop(RunState& run, int idx, int removeIdx) {
    if (!run.awaitingShop || idx < 0 || idx >= static_cast<int>(run.shop.size())) return false;
    ShopEntry& e = run.shop[idx];
    if (e.sold || e.price > run.player.gold) return false;
    if (e.removal) {
        if (removeIdx < 0 || removeIdx >= static_cast<int>(run.player.deck.size())) return false;
        run.player.deck.erase(run.player.deck.begin() + removeIdx);
        ++run.cardRemovalsUsed;
    } else if (e.relic) {
        run.relics.push_back(e.relicId); // RelicCmd.Obtain
    } else if (e.potion) {
        bool placed = false;
        for (auto& slot : run.player.potions)
            if (slot == 0) { slot = e.potionId; placed = true; break; }
        if (!placed) return false; // potion belt full
    } else {
        run.player.deck.push_back({static_cast<CardId>(e.cardId), e.upgrade});
    }
    run.player.gold -= e.price;
    e.sold = true;
    return true;
}

void leaveShop(RunState& run) { run.awaitingShop = false; }

// ---------------- unknown map point (UnknownMapPointOdds.Roll) ----------------

// Returns 0 = Monster, 1 = Treasure, 2 = Shop, 3 = Event. The first two ? rooms of a
// fresh save are Events and the third is a Monster (RunManager tutorial override); the
// odds then grow by their base value for every non-event room that was not rolled.
static int rollUnknownRoom(RunState& run) {
    if (run.unknownPointsVisited < 2) return 3;
    if (run.unknownPointsVisited == 2) return 0;
    const float r = run.unknownMapPoint.NextFloat();
    float cum = 0.0f;
    int chosen = 3;
    cum += std::max(0.0f, run.oddsMonster);
    if (r <= cum) chosen = 0;
    else {
        cum += std::max(0.0f, run.oddsTreasure);
        if (r <= cum) chosen = 1;
        else {
            cum += std::max(0.0f, run.oddsShop);
            if (r <= cum) chosen = 2;
        }
    }
    // Rolled type resets to base; every other rollable type grows by its base.
    run.oddsMonster = chosen == 0 ? 0.1f : run.oddsMonster + 0.1f;
    run.oddsTreasure = chosen == 1 ? 0.02f : run.oddsTreasure + 0.02f;
    run.oddsShop = chosen == 2 ? 0.03f : run.oddsShop + 0.03f;
    return chosen;
}

// ---------------- event room (DenseVegetation.cs) ----------------

// DenseVegetation: Gold = event Rng NextInt(61, 100); Heal = 30% max HP; HpLoss = 8.
// INITIAL: TRUDGE_ON (8 unblockable damage, gain gold) or REST (heal, then one FIGHT
// option that runs the 4-Wriggler event encounter).
// AbyssalBaths: IMMERSE/LINGER gain 2 max HP then take 3 (+1 per immerse, capped at 12)
// unblockable damage; ABSTAIN heals 10 and ends the event.
static void startAbyssalBaths(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "ABYSSAL_BATHS";
    run.eventPage = 0;
    run.eventOptions = {"IMMERSE", "ABSTAIN"};
    run.eventMaxHp = 2;
    run.eventDamage = 3;
    run.eventHeal = 10;
    run.eventGold = 0;
    run.eventHpLoss = 0;
}

// JungleMazeAdventure: SOLO_QUEST takes 18 unblockable damage for 150+/-15 gold;
// JOIN_FORCES takes 50+/-15 gold with no downside.
static void startJungleMaze(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "JUNGLE_MAZE_ADVENTURE";
    run.eventPage = 0;
    run.eventOptions = {"SOLO_QUEST", "JOIN_FORCES"};
    Sts2Rng rng(run.seed + xxhash64("JUNGLE_MAZE_ADVENTURE"));
    run.eventGold = static_cast<int>(150.0f + rng.NextFloat(-15.0f, 15.0f));   // SoloGold
    run.eventHeal = static_cast<int>(50.0f + rng.NextFloat(-15.0f, 15.0f));    // JoinForcesGold
    run.eventHpLoss = 18;
}

// HungryForMushrooms: BIG_MUSHROOM grants the relic and +20 max HP; FRAGRANT_MUSHROOM
// grants the relic, costs 15 HP and upgrades two random upgradable deck cards (Niche
// shuffle, matching StableShuffle(RunState.Rng.Niche)).
static void startHungryForMushrooms(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "HUNGRY_FOR_MUSHROOMS";
    run.eventPage = 0;
    run.eventOptions = {"BIG_MUSHROOM", "FRAGRANT_MUSHROOM"};
    run.eventMaxHp = 20;
    run.eventHpLoss = 15;
    run.eventGold = 0;
    run.eventHeal = 0;
}

// SunkenStatue: GRAB_SWORD obtains SwordOfStone; DIVE_INTO_WATER gains 111+/-10 gold and
// costs 7 unblockable HP.
static void startSunkenStatue(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SUNKEN_STATUE";
    run.eventPage = 0;
    run.eventOptions = {"GRAB_SWORD", "DIVE_INTO_WATER"};
    Sts2Rng rng(run.seed + xxhash64("SUNKEN_STATUE"));
    run.eventGold = 111 + rng.NextInt(-10, 11);
    run.eventHpLoss = 7;
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// RoomFullOfCheese: GORGE offers eight distinct commons and lets the player take two;
// SEARCH costs 14 unblockable HP for the ChosenCheese relic.
static void startRoomFullOfCheese(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "ROOM_FULL_OF_CHEESE";
    run.eventPage = 0;
    run.eventOptions = {"GORGE", "SEARCH"};
    run.eventHpLoss = 14;
    run.eventGold = 0;
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// PotionFactory.GetPotionOptions on a fresh save. The character potion pool is gated
// behind Ironclad4Epoch, and SharedPotionPool removes Potion1Epoch/Potion2Epoch potions;
// the harness/training save has no epochs revealed, so only the ungated shared potions
// are available. The Alchemize corpus caught the missing filter (Swift vs Vulnerable).
static std::vector<int> freshSavePotionOptions() {
    int pcount = 0;
    const PotionId* ptable = sts2PotionShopPool(&pcount);
    std::vector<int> out;
    for (int i = 0; i < pcount; ++i) {
        const PotionDef& p = sts2PotionDef(ptable[i]);
        if (!p.sharedPool) continue;
        switch (p.id) {
            case PotionId::BEETLE_JUICE:
            case PotionId::MAZALETHS_GIFT:
            case PotionId::DROPLET_OF_PRECOGNITION:
            case PotionId::POWDERED_DEMISE:
            case PotionId::SHIP_IN_A_BOTTLE:
            case PotionId::TOUCH_OF_INSANITY:
                continue;
            default: break;
        }
        out.push_back(static_cast<int>(ptable[i]));
    }
    return out;
}

// One out-of-combat potion reward (PotionReward.Populate -> PotionFactory): rarity roll on
// the Rewards stream then a uniform pick over the fresh-save potion options.
static int rollPotionReward(RunState& run) {
    const std::vector<int> options = freshSavePotionOptions();
    const float r = run.rewards.NextFloat();
    const PotionRarity want = r <= 0.1f ? PotionRarity::RARE
        : (r <= 0.35f ? PotionRarity::UNCOMMON : PotionRarity::COMMON);
    std::vector<int> cand;
    for (int id : options)
        if (sts2PotionDef(static_cast<PotionId>(id)).rarity == want) cand.push_back(id);
    if (cand.empty()) return 0;
    return cand[run.rewards.NextInt(static_cast<int>(cand.size()))];
}

// Puts a specific potion into the first empty belt slot (a full belt discards it).
static void grantPotionById(RunState& run, int pid) {
    if (pid == 0) return;
    for (int i = 0; i < run.potionSlots; ++i)
        if (run.player.potions[i] == 0) { run.player.potions[i] = pid; return; }
}

// Grants one potion reward into the first empty belt slot (a full belt discards it).
static void grantPotionReward(RunState& run) {
    grantPotionById(run, rollPotionReward(run));
}

// WhisperingHollow: GOLD spends 35+/-9 gold for two potion rewards; HUG transforms one
// chosen card and costs 9 unblockable HP. Only offered with at least 44 gold.
static void startWhisperingHollow(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WHISPERING_HOLLOW";
    run.eventPage = 0;
    run.eventOptions = {"GOLD", "HUG"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("WHISPERING_HOLLOW"));
    run.eventGold = 35 + run.eventRng.NextInt(-9, 10);
    run.eventHpLoss = 9;
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// TeaMaster: three relics for gold (BoneTea 50, EmberTea 150, TeaOfDiscourtesy free).
// The game only offers the event before act 3 and with at least 150 gold.
static void startTeaMaster(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "TEA_MASTER";
    run.eventPage = 0;
    run.eventOptions = {"BONE_TEA", "EMBER_TEA", "TEA_OF_DISCOURTESY"};
    run.eventGold = 50;    // BoneTea cost
    run.eventGold2 = 150;  // EmberTea cost
    run.eventHpLoss = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// ColossalFlower: three dig stages. Each stage offers "extract the current prize"
// (35/75/135 gold) or dig deeper (5/6/7 unblockable damage). At the last stage the deep
// option trades 7 damage for the PollinousCore relic. Requires at least 19 HP.
static void startColossalFlower(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "COLOSSAL_FLOWER";
    run.eventPage = 0;   // digs so far
    run.eventDamage = 5;
    run.eventGold = 35;
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventHpLoss = 0;
    run.eventOptions = {"EXTRACT_CURRENT_PRIZE", "REACH_DEEPER"};
}

// WelcomeToWongos: act 2 only, at least 100 gold. Bargain bin 100g -> random common
// relic, featured item 200g -> the rare relic rolled at generation, mystery box 300g ->
// WongosMysteryTicket; leaving downgrades one random upgraded card. Purchases add Wongo
// points (32/16/8) and every 2000 points grants the loyalty badge relic.
static void startWelcomeToWongos(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WELCOME_TO_WONGOS";
    run.eventPage = 0;
    run.eventOptions = {"BARGAIN_BIN", "FEATURED_ITEM", "MYSTERY_BOX", "LEAVE"};
    run.eventGold = 100;   // bargain bin
    run.eventGold2 = 200;  // featured item
    run.eventHpLoss = 300; // mystery box (reused field; see chooseEventOption)
    run.eventRelicId = pullRelic(run, RelicRarity::RARE, /*fromBack=*/false);
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

static void addWongoPoints(RunState& run, int points) {
    run.wongoPoints += points;
    if (run.wongoPoints >= 2000) {
        run.wongoPoints -= 2000;
        run.relics.push_back(static_cast<int>(RelicId::WONGO_CUSTOMER_APPRECIATION_BADGE));
    }
}

// RoundTeaParty: ENJOY_TEA grants RoyalPoison and heals to full; PICK_FIGHT leads to a
// single CONTINUE_FIGHT option that costs 11 unblockable HP for a random relic.
// Requires at least 12 HP.
static void startRoundTeaParty(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "ROUND_TEA_PARTY";
    run.eventPage = 0;
    run.eventOptions = {"ENJOY_TEA", "PICK_FIGHT"};
    run.eventHpLoss = 11;
    run.eventGold = 0;
    run.eventGold2 = 0;
    run.eventHeal = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// MorphicGrove: GROUP spends all gold and transforms two chosen deck cards; LONER grants
// +5 max HP. Requires at least 100 gold and two transformable cards.
static void startMorphicGrove(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "MORPHIC_GROVE";
    run.eventPage = 0;
    run.eventOptions = {"GROUP", "LONER"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("MORPHIC_GROVE"));
    run.eventMaxHp = 5;
    run.eventGold = 0;
    run.eventGold2 = 0;
    run.eventHeal = 0;
    run.eventHpLoss = 0;
    run.eventDamage = 0;
}

// ByrdonisNest: EAT gains 7 max HP; TAKE adds the unplayable ByrdonisEgg quest card.
// IsAllowed only when the run does not already carry the egg.
static void startByrdonisNest(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "BYRDONIS_NEST";
    run.eventPage = 0;
    run.eventOptions = {"EAT", "TAKE"};
    run.eventMaxHp = 7;
    run.eventGold = 0;
    run.eventGold2 = 0;
    run.eventHeal = 0;
    run.eventHpLoss = 0;
    run.eventDamage = 0;
}

static bool hasByrdonisEgg(const RunState& run) {
    for (const RunCard& c : run.player.deck)
        if (c.id == CardId::BYRDONIS_EGG) return true;
    return false;
}

// LanternKey.ModifyNextEvent / ModifyUnknownMapPointRoomTypes: the quest card forces ?
// rooms to be events in the third act and replaces the event with WarHistorianRepy.
static bool hasLanternKey(const RunState& run) {
    for (const RunCard& c : run.player.deck)
        if (c.id == CardId::LANTERN_KEY) return true;
    return false;
}

static void startWarHistorianRepy(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WAR_HISTORIAN_REPY";
    run.eventPage = 0;
    run.eventOptions = {"UNLOCK_CAGE", "UNLOCK_CHEST"};
}

// SunkenTreasury: FIRST_CHEST grants 60+/-8 gold; SECOND_CHEST grants 333+/-30 gold and
// adds the Greed curse to the deck.
static void startSunkenTreasury(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SUNKEN_TREASURY";
    run.eventPage = 0;
    run.eventOptions = {"FIRST_CHEST", "SECOND_CHEST"};
    Sts2Rng rng(run.seed + xxhash64("SUNKEN_TREASURY"));
    run.eventGold = 60 + (rng.NextInt(16) - 8);
    run.eventGold2 = 333 + (rng.NextInt(61) - 30);
    run.eventHeal = 0;
    run.eventHpLoss = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// LostWisp: CLAIM adds the Decay curse and grants the LostWisp relic; SEARCH grants
// 60+/-15 gold.
static void startLostWisp(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "LOST_WISP";
    run.eventPage = 0;
    run.eventOptions = {"CLAIM", "SEARCH"};
    Sts2Rng rng(run.seed + xxhash64("LOST_WISP"));
    run.eventGold = 60 + rng.NextInt(-15, 16);
    run.eventGold2 = 0;
    run.eventHeal = 0;
    run.eventHpLoss = 0;
    run.eventMaxHp = 0;
    run.eventDamage = 0;
}

// TabletOfTruth: DECIPHER pays max HP (3, then 6/12/24/maxHp-1) and upgrades one random
// upgradable deck card, upgrading every upgradable card on the fifth decipher; SMASH
// heals SmashHPGain (20) and ends the event.
static void startTabletOfTruth(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "TABLET_OF_TRUTH";
    run.eventPage = 0;
    run.eventOptions = {"DECIPHER", "SMASH"};
    run.eventDamage = 3; // CanonicalVars DecipherMaxHpLoss base value
    run.eventHeal = 20;  // CanonicalVars SmashHPGain
    // EventModel.BeginEvent: Rng(runSeed + hash(eventId)); the picks advance this state.
    run.eventRng = Sts2Rng(run.seed + xxhash64("TABLET_OF_TRUTH"));
}

// PotionCourier: GRAB_POTIONS grants three FoulPotion rewards; RANSACK grants one random
// Uncommon potion from the character + shared pool on the Rewards stream.
static void startPotionCourier(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "POTION_COURIER";
    run.eventPage = 0;
    run.eventOptions = {"GRAB_POTIONS", "RANSACK"};
}

// Test hook (mirrors bossOverride): maps a forced event id to the dispatch index below.
// Unknown names fall back to the normal UpFront roll.
static int eventIndexForName(const std::string& id) {
    static const char* kNames[55] = {
        "DENSE_VEGETATION", "ABYSSAL_BATHS", "JUNGLE_MAZE_ADVENTURE",
        "HUNGRY_FOR_MUSHROOMS", "SUNKEN_STATUE", "ROOM_FULL_OF_CHEESE",
        "WHISPERING_HOLLOW", "TEA_MASTER", "COLOSSAL_FLOWER", "WELCOME_TO_WONGOS",
        "ROUND_TEA_PARTY", "MORPHIC_GROVE", "BYRDONIS_NEST", "SUNKEN_TREASURY",
        "LOST_WISP", "TABLET_OF_TRUTH", "POTION_COURIER", "THIS_OR_THAT",
        "WELLSPRING", "LUMINOUS_CHOIR", "AMALGAMATOR", "REFLECTIONS",
        "DOORS_OF_LIGHT_AND_DARK", "UNREST_SITE", "DOLL_ROOM",
        "SAPPHIRE_SEED", "FIELD_OF_MAN_SIZED_HOLES", "AROMA_OF_CHAOS",
        "SPIRIT_GRAFTER", "TRASH_HEAP", "RELIC_TRADER", "THE_FUTURE_OF_POTIONS",
        "THE_LEGENDS_WERE_TRUE", "RANWID_THE_ELDER", "DROWNING_BEACON",
        "BUGSLAYER", "ZEN_WEAVER", "ENDLESS_CONVEYOR", "SELF_HELP_BOOK",
        "SLIPPERY_BRIDGE", "BRAIN_LEECH", "TRIAL", "WATERLOGGED_SCRIPTORIUM",
        "WOOD_CARVINGS", "PUNCH_OFF", "INFESTED_AUTOMATON", "SPIRALING_WHIRLPOOL",
        "STONE_OF_ALL_TIME", "SYMBIOTE", "GRAVE_OF_THE_FORGOTTEN", "BATTLEWORN_DUMMY",
        "TINKER_TIME", "THE_LANTERN_KEY", "FAKE_MERCHANT", "CRYSTAL_SPHERE",
    };
    for (int i = 0; i < 55; ++i)
        if (id == kNames[i]) return i;
    return -1;
}

// ThisOrThat: PLAIN pays 6 unblockable HP for 41..68 gold; ORNATE pulls a relic from the
// front of the grab bag (rarity on the Rewards stream) and adds the Clumsy curse.
static void startThisOrThat(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "THIS_OR_THAT";
    run.eventPage = 0;
    run.eventOptions = {"PLAIN", "ORNATE"};
    run.eventHpLoss = 6;
    Sts2Rng rng(run.seed + xxhash64("THIS_OR_THAT"));
    run.eventGold = rng.NextInt(41, 69);
}

// Wellspring: BOTTLE grants one random potion from the character + shared pool (one
// Rewards draw over the whole pool); BATHE removes one chosen card and adds Guilty.
static void startWellspring(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WELLSPRING";
    run.eventPage = 0;
    run.eventOptions = {"BOTTLE", "BATHE"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("WELLSPRING"));
}

// LuminousChoir: REACH_INTO_THE_FLESH removes two chosen cards and adds SporeMind;
// OFFER_TRIBUTE spends 149 - Rng.NextInt(0,50) gold for a front-bag relic.
static void startLuminousChoir(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "LUMINOUS_CHOIR";
    run.eventPage = 0;
    run.eventOptions = {"REACH_INTO_THE_FLESH", "OFFER_TRIBUTE"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("LUMINOUS_CHOIR"));
    run.eventGold = 149 - run.eventRng.NextInt(0, 50);
}

// TheArchitect: the victory room entered by RunManager.EnterNextAct once the last act's
// boss is dead (EventRoom). The layout is EventLayoutType.Combat and the canonical
// encounter spawns an Architect with 9999 HP, but that combat never starts: TheArchitect
// never calls ReadyToEnterCombat, and the monster's only move is NOTHING, so the room is
// dialogue + PROCEED. Each dialogue line offers one option; the last line offers PROCEED,
// which calls RunManager.WinRun. Line count comes from the character's dialogue entry
// (Ironclad has three lines on every visit index); the text itself is presentational and
// is not modeled here.
static void startTheArchitect(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "THE_ARCHITECT";
    run.eventPage = 0;
    run.eventOptions = {"CONTINUE"};
}

static bool hasAnyRelic(const RunState& run) {
    for (const auto& deque : run.relicBag)
        if (!deque.empty()) return true;
    return false;
}

static int countBasicTag(const RunState& run, unsigned tag) {
    int n = 0;
    for (const RunCard& c : run.player.deck) {
        const CardDef& d = BattleContext::cardDef(static_cast<CardId>(c.id));
        if (d.hasTag(tag) && d.rarity == CardRarity::BASIC) ++n;
    }
    return n;
}

// Amalgamator: combine two Basic Strike-tagged (or Defend-tagged) cards into one
// UltimateStrike / UltimateDefend. IsAllowed needs two of each in the deck.
static void startAmalgamator(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "AMALGAMATOR";
    run.eventPage = 0;
    run.eventOptions = {"COMBINE_STRIKES", "COMBINE_DEFENDS"};
}

// Reflections: TOUCH_A_MIRROR downgrades two random upgraded cards then upgrades four
// random upgradable ones (event Rng); SHATTER clones the whole deck and adds BadLuck.
static void startReflections(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "REFLECTIONS";
    run.eventPage = 0;
    run.eventOptions = {"TOUCH_A_MIRROR", "SHATTER"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("REFLECTIONS"));
}

// ListExtensions.UnstableShuffle: Fisher-Yates from the end, one NextInt(i+1) per step.
static void unstableShuffleIdx(std::vector<int>& v, Sts2Rng& rng) {
    for (int i = static_cast<int>(v.size()) - 1; i > 0; --i) {
        const int j = rng.NextInt(i + 1);
        std::swap(v[i], v[j]);
    }
}

// DoorsOfLightAndDark: LIGHT upgrades two random upgradable cards (StableShuffle on the
// event Rng); DARK removes one chosen card.
static void startDoorsOfLightAndDark(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "DOORS_OF_LIGHT_AND_DARK";
    run.eventPage = 0;
    run.eventOptions = {"LIGHT", "DARK"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("DOORS_OF_LIGHT_AND_DARK"));
}

// UnrestSite: REST heals to full and adds PoorSleep; KILL trades 8 max HP for a relic.
// IsAllowed: current HP at or below 70% of max.
static void startUnrestSite(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "UNREST_SITE";
    run.eventPage = 0;
    run.eventOptions = {"REST", "KILL"};
    run.eventMaxHp = 8;
}

// DollRoom (act 2): three fixed dolls. RANDOM is free, TAKE_SOME_TIME costs 5 HP and
// shows two dolls, EXAMINE costs 15 HP and shows all three.
static void startDollRoom(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "DOLL_ROOM";
    run.eventPage = 0;
    run.eventOptions = {"RANDOM", "TAKE_SOME_TIME", "EXAMINE"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("DOLL_ROOM"));
}

static const int kDollRelics[3] = {
    static_cast<int>(RelicId::BING_BONG),
    static_cast<int>(RelicId::DAUGHTER_OF_THE_WIND),
    static_cast<int>(RelicId::MR_STRUGGLES),
};

// EnchantmentModel.CanEnchant: Attack/Skill/Power only (no Status/Curse/Quest), no
// unplayable deck cards, and no second enchantment (none of ours are stackable).
static bool canEnchantCard(const RunCard& c, EnchantmentId e) {
    if (!sts2EnchantmentImplemented(e)) return false;
    const CardDef& d = BattleContext::cardDef(c.id);
    if (d.type == CardType::STATUS || d.type == CardType::CURSE || d.type == CardType::QUEST)
        return false;
    if (d.hasFlag(CF_UNPLAYABLE)) return false;
    if (e == EnchantmentId::SHARP && d.type != CardType::ATTACK) return false;
    if (e == EnchantmentId::VIGOROUS && d.type != CardType::ATTACK) return false;
    if (e == EnchantmentId::CORRUPTED && d.type != CardType::ATTACK) return false;
    if (e == EnchantmentId::SOULS_POWER && !d.hasFlag(CF_EXHAUST)) return false;
    if (e == EnchantmentId::NIMBLE && !d.hasFlag(CF_GAINS_BLOCK)) return false;
    if (e == EnchantmentId::SLITHER && d.hasFlag(CF_ENERGY_X)) return false;
    if (e == EnchantmentId::SPIRAL) {
        const bool basicStrikeDefend = d.rarity == CardRarity::BASIC
            && (d.hasTag(CT_STRIKE) || d.hasTag(CT_DEFEND));
        if (!basicStrikeDefend) return false;
    }
    if (c.enchant != 0 && c.enchant != static_cast<int>(e)) return false;
    return true;
}

// SapphireSeed: EAT heals 9 and upgrades one chosen card; PLANT enchants one chosen card
// with Sown (Amount 1 energy on its first play).
static void startSapphireSeed(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SAPPHIRE_SEED";
    run.eventPage = 0;
    run.eventOptions = {"EAT", "PLANT"};
    run.eventHeal = 9;
    run.eventEnchantId = static_cast<int>(EnchantmentId::SOWN);
    run.eventEnchantAmount = 1;
}

// FieldOfManSizedHoles: RESIST removes two chosen cards and adds Normality;
// ENTER_YOUR_HOLE enchants one chosen card with PerfectFit.
static void startFieldOfManSizedHoles(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "FIELD_OF_MAN_SIZED_HOLES";
    run.eventPage = 0;
    run.eventOptions = {"RESIST", "ENTER_YOUR_HOLE"};
    run.eventEnchantId = static_cast<int>(EnchantmentId::PERFECT_FIT);
    run.eventEnchantAmount = 1;
}

// AromaOfChaos: LET_GO transforms one chosen deck card; MAINTAIN_CONTROL upgrades one.
static void startAromaOfChaos(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "AROMA_OF_CHAOS";
    run.eventPage = 0;
    run.eventOptions = {"LET_GO", "MAINTAIN_CONTROL"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("AROMA_OF_CHAOS"));
}

// SpiritGrafter: LET_IT_IN heals 25 and adds Metamorphosis; REJECTION upgrades one chosen
// card then takes 10 unblockable damage.
static void startSpiritGrafter(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SPIRIT_GRAFTER";
    run.eventPage = 0;
    run.eventOptions = {"LET_IT_IN", "REJECTION"};
    run.eventHeal = 25;
    run.eventHpLoss = 10;
}

// TrashHeap: DIVE_IN takes 8 unblockable damage for a relic from a fixed 5-relic list;
// GRAB gains 100 gold and adds a card from a fixed 10-card list (both picks on event Rng).
static const int kTrashRelics[5] = {
    static_cast<int>(RelicId::DARKSTONE_PERIAPT),
    static_cast<int>(RelicId::DREAM_CATCHER),
    static_cast<int>(RelicId::HAND_DRILL),
    static_cast<int>(RelicId::MAW_BANK),
    static_cast<int>(RelicId::THE_BOOT),
};
static const int kTrashCards[10] = {
    static_cast<int>(CardId::CALTROPS),
    static_cast<int>(CardId::CLASH),
    static_cast<int>(CardId::DISTRACTION),
    static_cast<int>(CardId::DUAL_WIELD),
    static_cast<int>(CardId::ENTRENCH),
    static_cast<int>(CardId::HELLO_WORLD),
    static_cast<int>(CardId::OUTMANEUVER),
    static_cast<int>(CardId::REBOUND),
    static_cast<int>(CardId::RIP_AND_TEAR),
    static_cast<int>(CardId::STACK),
};

static void startTrashHeap(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "TRASH_HEAP";
    run.eventPage = 0;
    run.eventOptions = {"DIVE_IN", "GRAB"};
    run.eventHpLoss = 8;
    run.eventGold = 100;
    run.eventRng = Sts2Rng(run.seed + xxhash64("TRASH_HEAP"));
}

// RelicModel.IsTradable (approximation): pool relics only. Starter/Event/Ancient relics
// are excluded; the used-up / pickup-only / pet-spawning flags are not modelled yet.
static bool isTradableRelic(int id) {
    const RelicRarity r = sts2RelicDef(static_cast<RelicId>(id)).rarity;
    return r == RelicRarity::COMMON || r == RelicRarity::UNCOMMON
        || r == RelicRarity::RARE || r == RelicRarity::SHOP;
}

static int tradableRelicCount(const RunState& run) {
    int n = 0;
    for (int id : run.relics)
        if (isTradableRelic(id)) ++n;
    return n;
}

// RelicTrader: three tradable owned relics (StableShuffle on the event Rng) each paired
// with a fresh grab-bag relic (rarity on Rewards); the player swaps one and the event ends.
static void startRelicTrader(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "RELIC_TRADER";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("RELIC_TRADER"));
    std::vector<int> owned;
    for (int id : run.relics)
        if (isTradableRelic(id)) owned.push_back(id);
    std::sort(owned.begin(), owned.end());
    unstableShuffleIdx(owned, run.eventRng);
    run.eventOwnedRelics.clear();
    run.eventNewRelics.clear();
    static const char* kOpts[3] = {"TOP", "MIDDLE", "BOTTOM"};
    for (int i = 0; i < 3 && i < static_cast<int>(owned.size()); ++i) {
        const int nid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
        if (nid == static_cast<int>(RelicId::INVALID)) continue;
        run.eventOwnedRelics.push_back(owned[i]);
        run.eventNewRelics.push_back(nid);
    }
    run.eventOptions.clear();
    for (int i = 0; i < static_cast<int>(run.eventOwnedRelics.size()); ++i)
        run.eventOptions.push_back(kOpts[i]);
    if (run.eventOptions.empty()) run.eventOptions.push_back("PROCEED");
}

// TheFutureOfPotions: one option per held potion. Each option rolls a card type up front
// (Attack/Skill/Power; Power excluded for Common/Token potions), then trades the potion for
// a 3-card reward of that type at the potion's rarity tier, every card upgraded.
static void startTheFutureOfPotions(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "THE_FUTURE_OF_POTIONS";
    run.eventPage = 0;
    run.eventOptions.clear();
    run.eventPotionTypes.clear();
    run.eventRng = Sts2Rng(run.seed + xxhash64("THE_FUTURE_OF_POTIONS"));
    for (int slot = 0; slot < 3; ++slot) {
        const int pid = run.player.potions[slot];
        if (pid == 0) continue;
        const PotionRarity pr = sts2PotionDef(static_cast<PotionId>(pid)).rarity;
        std::vector<int> types = {static_cast<int>(CardType::ATTACK),
                                  static_cast<int>(CardType::SKILL),
                                  static_cast<int>(CardType::POWER)};
        if (pr == PotionRarity::COMMON || pr == PotionRarity::TOKEN) types.pop_back();
        const int t = types[run.eventRng.NextInt(static_cast<int>(types.size()))];
        run.eventPotionTypes.push_back(t);
        run.eventOptions.push_back("POTION_" + std::to_string(slot));
    }
}

static int heldPotionCount(const RunState& run) {
    int n = 0;
    for (int pid : run.player.potions)
        if (pid != 0) ++n;
    return n;
}

// RanwidTheElder: one random held potion / 100 gold / one random tradable relic, each
// traded for fresh grab-bag relics (1, 1 and 2 respectively; relic pick on the event Rng).
static void startRanwidTheElder(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "RANWID_THE_ELDER";
    run.eventPage = 0;
    run.eventOptions = {"POTION", "GOLD", "RELIC"};
    run.eventGold = 100;
    run.eventRng = Sts2Rng(run.seed + xxhash64("RANWID_THE_ELDER"));
    std::vector<int> slots;
    for (int i = 0; i < 3; ++i)
        if (run.player.potions[i] != 0) slots.push_back(i);
    run.eventPotionSlot = slots.empty() ? -1
        : slots[run.eventRng.NextInt(static_cast<int>(slots.size()))];
    std::vector<int> relics;
    for (int id : run.relics)
        if (isTradableRelic(id)) relics.push_back(id);
    run.eventPickRelicId = relics.empty() ? 0
        : relics[run.eventRng.NextInt(static_cast<int>(relics.size()))];
}

// TheLegendsWereTrue (act 1 only): NAB_THE_MAP adds the SpoilsMap quest card;
// SLOWLY_FIND_AN_EXIT costs 8 unblockable HP and grants one random pool potion.
static void startTheLegendsWereTrue(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "THE_LEGENDS_WERE_TRUE";
    run.eventPage = 0;
    run.eventOptions = {"NAB_THE_MAP", "SLOWLY_FIND_AN_EXIT"};
    run.eventDamage = 8;
}

// DrowningBeacon: BOTTLE grants a GlowwaterPotion; CLIMB costs 13 max HP for FresnelLens.
static void startDrowningBeacon(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "DROWNING_BEACON";
    run.eventPage = 0;
    run.eventOptions = {"BOTTLE", "CLIMB"};
    run.eventHpLoss = 13;
    run.eventMaxHp = 13;
}

// Bugslayer: add Exterminate or Squash to the deck (both event attack cards).
static void startBugslayer(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "BUGSLAYER";
    run.eventPage = 0;
    run.eventOptions = {"EXTERMINATION", "SQUASH"};
}

// ZenWeaver: pay 50 for two Enlightenment cards, or pay 125 / 250 to remove one / two
// chosen cards (gold charged when the removal grid closes).
static void startZenWeaver(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "ZEN_WEAVER";
    run.eventPage = 0;
    run.eventOptions = {"BREATHING_TECHNIQUES"};
    if (run.player.gold >= 125) run.eventOptions.push_back("EMOTIONAL_AWARENESS");
    if (run.player.gold >= 250) run.eventOptions.push_back("ARACHNID_ACUPUNCTURE");
}

// ---- EndlessConveyor -------------------------------------------------------------
// Dish table mirrors EndlessConveyor.RollDish: weights, conditional entries, a forced
// SeapunkSalad every fifth grab, and the previous dish removed from the pool.
static void rollConveyorDish(RunState& run) {
    ++run.eventConveyorGrabs;
    if (run.eventConveyorGrabs % 5 == 0) {
        run.eventConveyorLastDish = "SEAPUNK_SALAD";
        run.eventConveyorDish = "SEAPUNK_SALAD";
        return;
    }
    std::vector<std::pair<std::string, float>> list = {
        {"CAVIAR", 6.0f}, {"SPICY_SNAPPY", 3.0f}, {"JELLY_LIVER", 3.0f},
        {"FRIED_EEL", 3.0f},
    };
    if (heldPotionCount(run) < 3) list.push_back({"SUSPICIOUS_CONDIMENT", 3.0f});
    if (run.player.hp != run.player.maxHp) list.push_back({"CLAM_ROLL", 6.0f});
    if (run.eventConveyorGrabs > 1) list.push_back({"GOLDEN_FYSH", 1.0f});
    list.erase(std::remove_if(list.begin(), list.end(),
                              [&run](const std::pair<std::string, float>& p) {
                                  return p.first == run.eventConveyorLastDish;
                              }),
               list.end());
    if (list.empty()) return;
    float total = 0.0f;
    for (const auto& p : list) total += p.second;
    const float r = run.eventRng.NextFloat() * total;
    float acc = 0.0f;
    std::string picked = list.back().first;
    for (const auto& p : list) {
        acc += p.second;
        if (r < acc) { picked = p.first; break; }
    }
    run.eventConveyorLastDish = run.eventConveyorDish;
    run.eventConveyorDish = picked;
}

static void setConveyorOptions(RunState& run, bool first) {
    run.eventOptions.clear();
    if (run.player.gold >= 40) run.eventOptions.push_back(run.eventConveyorDish);
    run.eventOptions.push_back(first ? "OBSERVE_CHEF" : "LEAVE");
    run.eventPage = first ? 0 : 1;
}

// EndlessConveyor: pay 40 gold (GoldenFysh is free) for the current dish, then the belt
// rolls the next one. IsAllowed: at least 120 gold.
static void startEndlessConveyor(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "ENDLESS_CONVEYOR";
    run.eventPage = 0;
    run.eventConveyorGrabs = 0;
    run.eventConveyorDish.clear();
    run.eventConveyorLastDish.clear();
    run.eventConveyorResume = false;
    run.eventRng = Sts2Rng(run.seed + xxhash64("ENDLESS_CONVEYOR"));
    rollConveyorDish(run); // CalculateVars
    setConveyorOptions(run, /*first=*/true);
}

// Applies one dish. Returns true when the effect defers to the card grid (JellyLiver).
static bool applyConveyorDish(RunState& run, const std::string& dish) {
    if (dish == "CAVIAR") {
        run.player.maxHp += 4; // CreatureCmd.GainMaxHp(4): max +4, then heal 4
        run.player.hp = std::min(run.player.maxHp, run.player.hp + 4);
        return false;
    }
    if (dish == "CLAM_ROLL") {
        run.player.hp = std::min(run.player.maxHp, run.player.hp + 10);
        return false;
    }
    if (dish == "GOLDEN_FYSH") {
        run.player.gold += 75;
        return false;
    }
    if (dish == "SUSPICIOUS_CONDIMENT") {
        // EndlessConveyor.SuspiciousCondiment: NextItem over GetPotionOptions (Rewards).
        const std::vector<int> options = freshSavePotionOptions();
        if (!options.empty())
            grantPotionById(run, options[run.rewards.NextInt(static_cast<int>(options.size()))]);
        return false;
    }
    if (dish == "SPICY_SNAPPY") {
        std::vector<int> up;
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            const RunCard& c = run.player.deck[i];
            if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) up.push_back(i);
        }
        if (!up.empty())
            ++run.player.deck[up[run.eventRng.NextInt(static_cast<int>(up.size()))]].upgrade;
        return false;
    }
    if (dish == "SEAPUNK_SALAD") {
        run.player.deck.push_back({CardId::FEEDING_FRENZY, 0});
        return false;
    }
    if (dish == "FRIED_EEL") {
        // CardReward(1) over the colorless pool with default odds (0.03 rare / 0.37
        // uncommon), no upgrade roll. The engine only has the combat-generatable
        // colorless pool, which stands in for the full reward pool here.
        const float r = run.rewards.NextFloat();
        const CardRarity want = r < 0.03f ? CardRarity::RARE
                              : (r < 0.40f ? CardRarity::UNCOMMON : CardRarity::COMMON);
        int n = 0;
        const CardId* pool = sts2CombatGenPool("COLORLESS", &n);
        std::vector<int> cand;
        for (int i = 0; i < n; ++i) {
            const CardDef& d = BattleContext::cardDef(pool[i]);
            if (d.rarity == want && BattleContext::cardImplemented(pool[i]))
                cand.push_back(static_cast<int>(pool[i]));
        }
        if (cand.empty())
            for (int i = 0; i < n; ++i)
                if (BattleContext::cardImplemented(pool[i]))
                    cand.push_back(static_cast<int>(pool[i]));
        if (!cand.empty())
            run.player.deck.push_back(
                {static_cast<CardId>(cand[run.rewards.NextInt(static_cast<int>(cand.size()))]), 0});
        return false;
    }
    if (dish == "JELLY_LIVER") {
        // CardCmd.TransformToRandom waits for the player's grid pick; the conveyor resumes
        // after the grid closes.
        run.eventCardPool.clear();
        run.eventCardDeckIdx.clear();
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                         run.player.deck[i].upgrade});
            run.eventCardDeckIdx.push_back(i);
        }
        if (run.eventCardPool.empty()) return false;
        run.eventCardsTransform = true;
        run.eventCardsRemove = false;
        run.eventCardsEnchant = false;
        run.eventCardsUpgrade = false;
        run.eventCardsLeft = 1;
        run.eventConveyorResume = true;
        run.awaitingEventCards = true;
        return true;
    }
    return false;
}

// SelfHelpBook: enchant one Attack with Sharp 2, one Skill that gains block with
// Nimble 2, or one Power with Swift 2, depending on what the deck can take.
static void startSelfHelpBook(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SELF_HELP_BOOK";
    run.eventPage = 0;
    run.eventOptions.clear();
    bool canSharp = false, canNimble = false, canSwift = false;
    for (const RunCard& c : run.player.deck) {
        const CardDef& d = BattleContext::cardDef(c.id);
        if (d.type == CardType::ATTACK && canEnchantCard(c, EnchantmentId::SHARP))
            canSharp = true;
        if (d.type == CardType::SKILL && canEnchantCard(c, EnchantmentId::NIMBLE))
            canNimble = true;
        if (d.type == CardType::POWER && canEnchantCard(c, EnchantmentId::SWIFT))
            canSwift = true;
    }
    if (canSharp) run.eventOptions.push_back("READ_THE_BACK");
    if (canNimble) run.eventOptions.push_back("READ_PASSAGE");
    if (canSwift) run.eventOptions.push_back("READ_ENTIRE_BOOK");
    if (run.eventOptions.empty()) run.eventOptions.push_back("NO_OPTIONS");
}

// SlipperyBridge: OVERCOME loses the offered card; HOLD_ON pays 3 + holds so far HP and
// rerolls the card (first pick skips Basic cards; later picks skip the previous card's
// model and every card already skipped).
static void pickBridgeCard(RunState& run) {
    const int prevIdx = run.eventBridgeCardIdx;
    if (prevIdx >= 0 && prevIdx < static_cast<int>(run.player.deck.size()))
        run.eventBridgeSkipped.push_back(static_cast<int>(run.player.deck[prevIdx].id));
    std::vector<int> cand;
    for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
        const RunCard& c = run.player.deck[i];
        const CardDef& d = BattleContext::cardDef(c.id);
        if (d.hasFlag(CF_ETERNAL)) continue; // !IsRemovable
        if (prevIdx < 0) {
            if (d.rarity == CardRarity::BASIC) continue;
        } else if (prevIdx < static_cast<int>(run.player.deck.size())
                   && static_cast<int>(c.id) == static_cast<int>(run.player.deck[prevIdx].id)) {
            continue; // different card model
        }
        if (std::find(run.eventBridgeSkipped.begin(), run.eventBridgeSkipped.end(),
                      static_cast<int>(c.id)) != run.eventBridgeSkipped.end())
            continue;
        cand.push_back(i);
    }
    if (cand.empty()) { // fallback: every removable card
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i)
            if (!BattleContext::cardDef(run.player.deck[i].id).hasFlag(CF_ETERNAL))
                cand.push_back(i);
    }
    if (!cand.empty())
        run.eventBridgeCardIdx = cand[run.eventRng.NextInt(static_cast<int>(cand.size()))];
}

static void startSlipperyBridge(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SLIPPERY_BRIDGE";
    run.eventPage = 0;
    run.eventOptions = {"OVERCOME", "HOLD_ON"};
    run.eventBridgeCardIdx = -1;
    run.eventBridgeHoldOns = 0;
    run.eventBridgeSkipped.clear();
    run.eventRng = Sts2Rng(run.seed + xxhash64("SLIPPERY_BRIDGE"));
    pickBridgeCard(run); // GenerateInitialOptions
}

// CardFactory.CreateForReward with ForNonCombatWithDefaultOdds: RegularEncounter base
// odds (0.03 rare / 0.37 uncommon), uniform pick inside the rolled rarity, distinct
// picks, no upgrade roll.
// filterKind: 0 = none, 1 = Power cards only, 2 = canonical cost 0 (not X).
static bool defaultOddsFilter(const CardDef& d, int kind) {
    if (kind == 1) return d.type == CardType::POWER;
    if (kind == 2) return d.cost == 0 && !d.hasFlag(CF_ENERGY_X);
    return true;
}

static std::vector<int> defaultOddsCards(RunState& run, const CardId* pool, int poolCount,
                                         int count, int filterKind = 0) {
    std::vector<int> out;
    // GetNextAllowedRarity: a rolled rarity missing from the pool is promoted to the next
    // rarity that still has an unpicked candidate (Common -> Uncommon -> Rare -> wrap).
    static const CardRarity kOrder[3] = {CardRarity::COMMON, CardRarity::UNCOMMON,
                                         CardRarity::RARE};
    for (int k = 0; k < count && pool && poolCount > 0; ++k) {
        const float r = run.rewards.NextFloat();
        const CardRarity rolled = r < 0.03f ? CardRarity::RARE
                                : (r < 0.40f ? CardRarity::UNCOMMON : CardRarity::COMMON);
        const int start = rolled == CardRarity::COMMON ? 0
                        : (rolled == CardRarity::UNCOMMON ? 1 : 2);
        std::vector<int> cand;
        for (int step = 0; step < 3; ++step) {
            const CardRarity want = kOrder[(start + step) % 3];
            cand.clear();
            for (int i = 0; i < poolCount; ++i) {
                const CardDef& d = BattleContext::cardDef(pool[i]);
                if (d.rarity != want) continue;
                if (!defaultOddsFilter(d, filterKind)) continue;
                if (std::find(out.begin(), out.end(), static_cast<int>(pool[i])) != out.end())
                    continue;
                if (!BattleContext::cardImplemented(pool[i])) continue;
                cand.push_back(static_cast<int>(pool[i]));
            }
            if (!cand.empty()) break;
        }
        if (cand.empty()) break;
        out.push_back(cand[run.rewards.NextInt(static_cast<int>(cand.size()))]);
    }
    return out;
}

// BrainLeech (acts 1-2): SHARE_KNOWLEDGE offers five character-pool cards (default odds)
// and takes one; RIP costs 5 unblockable HP for a three-card colorless reward.
static void startBrainLeech(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "BRAIN_LEECH";
    run.eventPage = 0;
    run.eventOptions = {"SHARE_KNOWLEDGE", "RIP"};
    run.eventHpLoss = 5;
}

// Trial: ACCEPT rolls one of three cases (merchant / noble / nondescript), each with a
// guilty and an innocent branch; REJECT offers a second ACCEPT or DOUBLING DOWN (which
// abandons the run).
static void startTrial(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "TRIAL";
    run.eventPage = 0;
    run.eventOptions = {"ACCEPT", "REJECT"};
    run.eventTrialCase = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("TRIAL"));
}

// WaterloggedScriptorium: BLOODY_INK gains 6 max HP; TENTACLE_QUILL (55 gold) enchants
// one card with Steady; PRICKLY_SPONGE (99 gold) enchants two.
static void startWaterloggedScriptorium(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WATERLOGGED_SCRIPTORIUM";
    run.eventPage = 0;
    run.eventOptions = {"BLOODY_INK"};
    if (run.player.gold >= 55) run.eventOptions.push_back("TENTACLE_QUILL");
    if (run.player.gold >= 99) run.eventOptions.push_back("PRICKLY_SPONGE");
}

// WoodCarvings: BIRD replaces a Basic card with Peck, TORUS with ToricToughness, and
// SNAKE enchants one card with Slither (option omitted when nothing can take it).
static bool isBasicRemovable(const RunCard& c) {
    const CardDef& d = BattleContext::cardDef(c.id);
    return d.rarity == CardRarity::BASIC && !d.hasFlag(CF_ETERNAL);
}

static void startWoodCarvings(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "WOOD_CARVINGS";
    run.eventPage = 0;
    run.eventOptions = {"BIRD"};
    bool canSlither = false;
    for (const RunCard& c : run.player.deck)
        if (canEnchantCard(c, EnchantmentId::SLITHER)) { canSlither = true; break; }
    if (canSlither) run.eventOptions.push_back("SNAKE");
    run.eventOptions.push_back("TORUS");
}

// PunchOff (floor > 6): NAB takes the Injury curse for a relic; I_CAN_TAKE_THEM leads to
// a fight against two PunchConstructs (the winner also gets a relic and a potion).
static void startPunchOff(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "PUNCH_OFF";
    run.eventPage = 0;
    run.eventOptions = {"NAB", "I_CAN_TAKE_THEM"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("PUNCH_OFF"));
    run.eventGold = run.eventRng.NextInt(91, 99); // CalculateVars (flavor value)
}

// InfestedAutomaton: STUDY adds one random Power card; TOUCH_CORE adds one random
// zero-cost (non-X) card. Both from the character pool at default odds.
static void startInfestedAutomaton(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "INFESTED_AUTOMATON";
    run.eventPage = 0;
    run.eventOptions = {"STUDY", "TOUCH_CORE"};
}

// SpiralingWhirlpool: OBSERVE enchants one Basic Strike/Defend with Spiral (one extra
// play); DRINK heals 33% of max HP.
static void startSpiralingWhirlpool(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SPIRALING_WHIRLPOOL";
    run.eventPage = 0;
    run.eventOptions = {"OBSERVE", "DRINK"};
    run.eventHeal = run.player.maxHp * 33 / 100;
}

// StoneOfAllTime (act 2, needs a potion): LIFT discards a random held potion for +10 max
// HP; PUSH costs 6 unblockable HP and enchants one Attack with Vigorous 8.
static void startStoneOfAllTime(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "STONE_OF_ALL_TIME";
    run.eventPage = 0;
    run.eventOptions.clear();
    run.eventRng = Sts2Rng(run.seed + xxhash64("STONE_OF_ALL_TIME"));
    std::vector<int> slots;
    for (int i = 0; i < 3; ++i)
        if (run.player.potions[i] != 0) slots.push_back(i);
    run.eventPotionSlot = slots.empty() ? -1
        : slots[run.eventRng.NextInt(static_cast<int>(slots.size()))];
    if (run.eventPotionSlot >= 0) run.eventOptions.push_back("LIFT");
    bool canVigorous = false;
    for (const RunCard& c : run.player.deck)
        if (canEnchantCard(c, EnchantmentId::VIGOROUS)) { canVigorous = true; break; }
    if (canVigorous) run.eventOptions.push_back("PUSH");
}

// Symbiote (act 2+): APPROACH enchants one Attack with Corrupted (x1.5 damage, 2 HP per
// play); KILL_WITH_FIRE transforms one card.
static void startSymbiote(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "SYMBIOTE";
    run.eventPage = 0;
    run.eventOptions.clear();
    bool canCorrupt = false;
    for (const RunCard& c : run.player.deck)
        if (canEnchantCard(c, EnchantmentId::CORRUPTED)) { canCorrupt = true; break; }
    if (canCorrupt) run.eventOptions.push_back("APPROACH");
    run.eventOptions.push_back("KILL_WITH_FIRE");
}

// GraveOfTheForgotten: CONFRONT adds the Decay curse and enchants one Exhaust card with
// SoulsPower (removes Exhaust); ACCEPT grants the ForgottenSoul relic.
static void startGraveOfTheForgotten(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "GRAVE_OF_THE_FORGOTTEN";
    run.eventPage = 0;
    run.eventOptions = {"CONFRONT", "ACCEPT"};
}

// BattlewornDummy: three settings summon a training dummy (75/150/300 HP) that escapes
// after three enemy turns. Killing it in time pays V1 a potion, V2 two random upgrades,
// V3 a relic; timing out gives nothing.
static void startBattlewornDummy(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "BATTLEWORN_DUMMY";
    run.eventPage = 0;
    run.eventOptions = {"SETTING_1", "SETTING_2", "SETTING_3"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("BATTLEWORN_DUMMY"));
}

// TinkerTime: choose one of two card types, then one of two riders; the resulting
// MadScience card keeps that configuration in the deck.
static void startTinkerTime(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "TINKER_TIME";
    run.eventPage = 0;
    run.eventOptions = {"CHOOSE_CARD_TYPE"};
    run.eventTinkerType = 0;
    run.eventTinkerRiders.clear();
    run.eventRng = Sts2Rng(run.seed + xxhash64("TINKER_TIME"));
}

static const int kTinkerRiders[3][3] = {
    {1, 2, 3}, // Attack: Sapping / Violence / Choking
    {4, 5, 6}, // Skill: Energized / Wisdom / Chaos
    {7, 8, 9}, // Power: Expertise / Curious / Improvement
};

// TheLanternKey: RETURN_THE_KEY grants 100 gold; KEEP_THE_KEY leads to a fight against the
// MysteriousKnight, and winning adds the LanternKey quest card to the deck.
static void startTheLanternKey(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "THE_LANTERN_KEY";
    run.eventPage = 0;
    run.eventOptions = {"RETURN_THE_KEY", "KEEP_THE_KEY"};
    run.eventGold = 100;
}

// FakeMerchant: a shop of six fake relics (50 gold each); throwing a FoulPotion starts a
// fight, and winning pays 300 gold plus the rug and every unsold relic.
static const int kFakeRelics[9] = {
    static_cast<int>(RelicId::FAKE_ANCHOR),
    static_cast<int>(RelicId::FAKE_BLOOD_VIAL),
    static_cast<int>(RelicId::FAKE_HAPPY_FLOWER),
    static_cast<int>(RelicId::FAKE_LEES_WAFFLE),
    static_cast<int>(RelicId::FAKE_MANGO),
    static_cast<int>(RelicId::FAKE_ORICHALCUM),
    static_cast<int>(RelicId::FAKE_SNECKO_EYE),
    static_cast<int>(RelicId::FAKE_STRIKE_DUMMY),
    static_cast<int>(RelicId::FAKE_VENERABLE_TEA_SET),
};

static bool hasFoulPotion(const RunState& run) {
    for (int pid : run.player.potions)
        if (pid == static_cast<int>(PotionId::FOUL_POTION)) return true;
    return false;
}

static void refreshFakeMerchantOptions(RunState& run) {
    run.eventOptions.clear();
    for (int i = 0; i < static_cast<int>(run.eventShopRelics.size()); ++i)
        if (run.eventShopRelics[i] >= 0)
            run.eventOptions.push_back("BUY_" + std::to_string(i));
    if (hasFoulPotion(run)) run.eventOptions.push_back("THROW_POTION");
    run.eventOptions.push_back("LEAVE");
}

static void startFakeMerchant(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "FAKE_MERCHANT";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("FAKE_MERCHANT"));
    std::vector<int> pool(kFakeRelics, kFakeRelics + 9);
    unstableShuffleIdx(pool, run.eventRng);
    run.eventShopRelics.assign(pool.begin(), pool.begin() + 6);
    refreshFakeMerchantOptions(run);
}

// ---- CrystalSphere ----
// The event runs an 11x11 fog grid. Item kinds follow the source's item subclasses;
// only 1..9 are used here.
enum SphereItemKind {
    SPHERE_RELIC = 1,
    SPHERE_POTION_COMMON = 2,
    SPHERE_POTION_RARE = 3,
    SPHERE_CARD_COMMON = 4,
    SPHERE_CARD_UNCOMMON = 5,
    SPHERE_CARD_RARE = 6,
    SPHERE_CURSE = 7,
    SPHERE_GOLD_SMALL = 8,
    SPHERE_GOLD_BIG = 9,
};

static const int kSphereSide = 11;

// CrystalSphereItem.PlaceItem: collect every in-bounds block whose cells are still hidden
// and item-free, then pick one uniformly on the minigame Rng.
static bool placeSphereItem(RunState& run, int kind, int w, int h) {
    std::vector<int> cand;
    for (int y = 0; y + h <= kSphereSide; ++y)
        for (int x = 0; x + w <= kSphereSide; ++x) {
            bool ok = true;
            for (int dy = 0; dy < h && ok; ++dy)
                for (int dx = 0; dx < w; ++dx) {
                    const int idx = (y + dy) * kSphereSide + (x + dx);
                    if (!run.eventSphereHidden[idx] || run.eventSphereCells[idx] != 0) {
                        ok = false;
                        break;
                    }
                }
            if (ok) cand.push_back(y * kSphereSide + x);
        }
    if (cand.empty()) return false;
    const int pos = cand[run.eventRng.NextInt(static_cast<int>(cand.size()))];
    const int itemIdx = static_cast<int>(run.eventSphereType.size());
    run.eventSpherePos.push_back(pos);
    run.eventSphereW.push_back(w);
    run.eventSphereH.push_back(h);
    run.eventSphereType.push_back(kind);
    const int px = pos % kSphereSide;
    const int py = pos / kSphereSide;
    for (int dy = 0; dy < h; ++dy)
        for (int dx = 0; dx < w; ++dx)
            run.eventSphereCells[(py + dy) * kSphereSide + (px + dx)] = itemIdx + 1;
    return true;
}

// PopulateItems: the source places in this order and stops at the first failure
// (flag = flag && PlaceItem short-circuits), returning false so the caller retries.
static bool populateSphereItems(RunState& run) {
    bool ok = true;
    if (ok) ok = placeSphereItem(run, SPHERE_RELIC, 4, 4);
    for (int i = 0; i < 2 && ok; ++i)
        ok = placeSphereItem(run, SPHERE_POTION_COMMON, 1, 3);
    if (ok) ok = placeSphereItem(run, SPHERE_POTION_RARE, 2, 2);
    if (ok) ok = placeSphereItem(run, SPHERE_CARD_COMMON, 2, 2);
    if (ok) ok = placeSphereItem(run, SPHERE_CARD_UNCOMMON, 2, 2);
    if (ok) ok = placeSphereItem(run, SPHERE_CARD_RARE, 2, 2);
    if (ok) ok = placeSphereItem(run, SPHERE_CURSE, 2, 2);
    for (int i = 0; i < 5 && ok; ++i)
        ok = placeSphereItem(run, SPHERE_GOLD_SMALL, 1, 1);
    for (int i = 0; i < 2 && ok; ++i)
        ok = placeSphereItem(run, SPHERE_GOLD_BIG, 2, 1);
    return ok;
}

// ClearCell: only hidden cells change; an item is revealed once every covered cell is clear.
static void clearSphereCell(RunState& run, int x, int y) {
    if (x < 0 || x >= kSphereSide || y < 0 || y >= kSphereSide) return;
    const int idx = y * kSphereSide + x;
    if (!run.eventSphereHidden[idx]) return;
    run.eventSphereHidden[idx] = 0;
    const int item = run.eventSphereCells[idx];
    if (item <= 0) return;
    const int ii = item - 1;
    const int px = run.eventSpherePos[ii] % kSphereSide;
    const int py = run.eventSpherePos[ii] / kSphereSide;
    for (int dy = 0; dy < run.eventSphereH[ii]; ++dy)
        for (int dx = 0; dx < run.eventSphereW[ii]; ++dx)
            if (run.eventSphereHidden[(py + dy) * kSphereSide + (px + dx)]) return;
    run.eventSphereRevealed.push_back(ii);
}

static void beginCrystalSphereMinigame(RunState& run, int divinations) {
    run.eventPage = 1;
    run.eventOptions.clear();
    run.eventSphereHidden.assign(kSphereSide * kSphereSide, 1);
    run.eventSphereCells.assign(kSphereSide * kSphereSide, 0);
    run.eventSpherePos.clear();
    run.eventSphereW.clear();
    run.eventSphereH.clear();
    run.eventSphereType.clear();
    run.eventSphereRevealed.clear();
    run.eventSphereDivinations = divinations;
    run.eventSphereTool = 2; // CrystalSphereToolType.Big

    // Constructor: the four corners expanded by horizontal/vertical neighbours twice.
    std::vector<std::pair<int,int>> list = {{0, 0}, {kSphereSide - 1, 0},
                                            {kSphereSide - 1, kSphereSide - 1},
                                            {0, kSphereSide - 1}};
    for (int k = 0; k < 2; ++k) {
        std::vector<std::pair<int,int>> next = list;
        for (const auto& c : list)
            for (int i = -1; i <= 1; i += 2) {
                const int nx = c.first + i;
                if (nx >= 0 && nx < kSphereSide) next.push_back({nx, c.second});
                const int ny = c.second + i;
                if (ny >= 0 && ny < kSphereSide) next.push_back({c.first, ny});
            }
        list = next;
    }
    for (const auto& c : list) clearSphereCell(run, c.first, c.second);

    int tries = 0;
    bool placed = false;
    do {
        placed = populateSphereItems(run);
        ++tries;
    } while (!placed && tries < 10);
}

// PotionFactory.GetPotionOptions filtered by rarity, then Rng.NextItem.
static int spherePotionOfRarity(RunState& run, PotionRarity want) {
    const std::vector<int> options = freshSavePotionOptions();
    std::vector<int> cand;
    for (int id : options)
        if (sts2PotionDef(static_cast<PotionId>(id)).rarity == want) cand.push_back(id);
    if (cand.empty()) return 0;
    return cand[run.eventRng.NextInt(static_cast<int>(cand.size()))];
}

// CardCreationOptions with Uniform odds and a rarity filter: three distinct picks, no
// upgrade roll (CardReward's own Rng override).
static std::vector<std::pair<int,int>> sphereCardOffer(RunState& run, CardRarity rarity) {
    int poolCount = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &poolCount);
    std::vector<int> cand;
    for (int i = 0; i < poolCount; ++i) {
        if (BattleContext::cardDef(pool[i]).rarity != rarity) continue;
        if (!BattleContext::cardImplemented(pool[i])) continue;
        cand.push_back(static_cast<int>(pool[i]));
    }
    std::vector<std::pair<int,int>> out;
    for (int k = 0; k < 3 && !cand.empty(); ++k) {
        const int pickIdx = run.eventRng.NextInt(static_cast<int>(cand.size()));
        out.emplace_back(cand[pickIdx], 0);
        cand.erase(cand.begin() + pickIdx);
    }
    return out;
}

// DoLocalCrystalSphereRewards: grant every revealed item in reveal order. Card rewards
// become queued offers; the first one is served by the normal reward flow.
static void finishCrystalSphere(RunState& run) {
    std::vector<std::vector<std::pair<int,int>>> offers;
    for (int ii : run.eventSphereRevealed) {
        switch (run.eventSphereType[ii]) {
            case SPHERE_RELIC: {
                const float f = run.eventRng.NextFloat();
                const RelicRarity rarity = f < 0.5f ? RelicRarity::COMMON
                    : (f < 0.83f ? RelicRarity::UNCOMMON : RelicRarity::RARE);
                const int rid = pullRelic(run, rarity, /*fromBack=*/false);
                if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
                break;
            }
            case SPHERE_POTION_COMMON:
                grantPotionById(run, spherePotionOfRarity(run, PotionRarity::COMMON));
                break;
            case SPHERE_POTION_RARE:
                grantPotionById(run, spherePotionOfRarity(run, PotionRarity::RARE));
                break;
            case SPHERE_CARD_COMMON: {
                auto o = sphereCardOffer(run, CardRarity::COMMON);
                if (!o.empty()) offers.push_back(std::move(o));
                break;
            }
            case SPHERE_CARD_UNCOMMON: {
                auto o = sphereCardOffer(run, CardRarity::UNCOMMON);
                if (!o.empty()) offers.push_back(std::move(o));
                break;
            }
            case SPHERE_CARD_RARE: {
                auto o = sphereCardOffer(run, CardRarity::RARE);
                if (!o.empty()) offers.push_back(std::move(o));
                break;
            }
            case SPHERE_CURSE:
                run.player.deck.push_back({CardId::DEBT, 0});
                break;
            case SPHERE_GOLD_SMALL:
                run.player.gold += 10;
                break;
            case SPHERE_GOLD_BIG:
                run.player.gold += 30;
                break;
            default:
                break;
        }
    }
    run.awaitingEvent = false;
    if (!offers.empty()) {
        run.cardOffer = std::move(offers[0]);
        if (offers.size() > 1) {
            run.cardOffer2 = std::move(offers[1]);
            run.pendingSecondCardReward = true;
        }
        for (std::size_t k = 2; k < offers.size(); ++k)
            run.extraCardOffers.push_back(std::move(offers[k]));
        run.awaitingCardReward = true;
    }
}

static void startCrystalSphere(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "CRYSTAL_SPHERE";
    run.eventPage = 0;
    run.eventOptions = {"UNCOVER_FUTURE", "PAYMENT_PLAN"};
    run.eventRng = Sts2Rng(run.seed + xxhash64("CRYSTAL_SPHERE"));
    // CalculateVars: 50 + Rng.NextInt(1, 50) -> 51..99 gold.
    run.eventGold = 50 + run.eventRng.NextInt(1, 50);
}

static void startDenseVegetation(RunState& run) {
    run.awaitingEvent = true;
    run.eventId = "DENSE_VEGETATION";
    run.eventPage = 0;
    run.eventOptions = {"TRUDGE_ON", "REST"};
    run.eventHpLoss = 8;
    run.eventHeal = run.player.maxHp * 3 / 10;
    // EventModel.BeginEvent: Rng(runSeed + hash(eventId)) for shared events.
    Sts2Rng rng(run.seed + xxhash64("DENSE_VEGETATION"));
    run.eventGold = rng.NextInt(61, 100);
}

// ---- Neow (Ancient opening event) ----
// Curse options and positive options mirror Neow.CurseOptions / PositiveOptions. The
// multi-character and multiplayer relics are excluded by their IsAllowed gates
// (Kaleidoscope and ScrollBoxes need every character pool, MassiveScroll needs 2+ players).
static const int kNeowCurses[10] = {
    static_cast<int>(RelicId::CURSED_PEARL), static_cast<int>(RelicId::DOWSING_ROD),
    static_cast<int>(RelicId::HEFTY_TABLET), static_cast<int>(RelicId::LARGE_CAPSULE),
    static_cast<int>(RelicId::LEAFY_POULTICE), static_cast<int>(RelicId::NEOWS_BONES),
    static_cast<int>(RelicId::NEOWS_SACRIFICE), static_cast<int>(RelicId::PRECARIOUS_SHEARS),
    static_cast<int>(RelicId::SILKEN_TRESS), static_cast<int>(RelicId::SILVER_CRUCIBLE),
};

static void neowTakeOut(std::vector<int>& v, int relic) {
    v.erase(std::remove(v.begin(), v.end(), relic), v.end());
}

// RegularEncounter odds (0.03 rare / 0.37 uncommon) on the event's own Rng.
static std::vector<std::pair<int,int>> neowRegularCards(RunState& run,
        const CardId* pool, int poolCount, int count) {
    std::vector<std::pair<int,int>> out;
    for (int k = 0; k < count && pool && poolCount > 0; ++k) {
        const float r = run.eventRng.NextFloat();
        const CardRarity want = r < 0.03f ? CardRarity::RARE
                              : (r < 0.40f ? CardRarity::UNCOMMON : CardRarity::COMMON);
        std::vector<int> cand;
        for (int i = 0; i < poolCount; ++i) {
            if (BattleContext::cardDef(pool[i]).rarity != want) continue;
            if (!BattleContext::cardImplemented(pool[i])) continue;
            bool used = false;
            for (const auto& o : out) if (o.first == static_cast<int>(pool[i])) used = true;
            if (!used) cand.push_back(static_cast<int>(pool[i]));
        }
        if (cand.empty()) continue;
        out.emplace_back(cand[run.eventRng.NextInt(static_cast<int>(cand.size()))], 0);
    }
    return out;
}

// Uniform rarity filter over the character pool (CardRarityOddsType.Uniform), NoUpgradeRoll.
static std::vector<std::pair<int,int>> neowCardsOfRarity(RunState& run, CardRarity want,
                                                         int count) {
    int poolCount = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &poolCount);
    std::vector<int> cand;
    for (int i = 0; i < poolCount; ++i)
        if (BattleContext::cardDef(pool[i]).rarity == want
            && BattleContext::cardImplemented(pool[i]))
            cand.push_back(static_cast<int>(pool[i]));
    std::vector<std::pair<int,int>> out;
    for (int k = 0; k < count && !cand.empty(); ++k) {
        const int at = run.eventRng.NextInt(static_cast<int>(cand.size()));
        out.emplace_back(cand[at], 0);
        cand.erase(cand.begin() + at);
    }
    return out;
}

// RelicReward.Populate: rarity roll (50/33/17) then a front pull from the grab bag.
static void neowRelicReward(RunState& run) {
    const float f = run.eventRng.NextFloat();
    const RelicRarity rarity = f < 0.5f ? RelicRarity::COMMON
        : (f < 0.83f ? RelicRarity::UNCOMMON : RelicRarity::RARE);
    const int rid = pullRelic(run, rarity, /*fromBack=*/false);
    if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
}

// CardCmd.TransformToRandom for the Basic Strike/Defend pair (LeafyPoultice).
static void neowTransformBasic(RunState& run, CardId basicId) {
    int n = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
    std::vector<int> cand;
    for (int i = 0; i < n; ++i)
        if (pool[i] != basicId && BattleContext::cardImplemented(pool[i]))
            cand.push_back(static_cast<int>(pool[i]));
    if (cand.empty()) return;
    for (RunCard& c : run.player.deck)
        if (c.id == basicId) {
            c.id = static_cast<CardId>(cand[run.eventRng.NextInt(static_cast<int>(cand.size()))]);
            c.upgrade = 0;
            return;
        }
}

// AncientEventModel.BeforeEventStarted: every Ancient heals the missing HP on entry
// (Neow drains to 0 first, so it defines the run's opening HP). WearyTraveler/A2
// multiplies the heal by 0.8 (decimal 0.8m, truncated when it lands on int HP).
static void ancientOpeningHeal(RunState& run) {
    const int missing = run.player.maxHp - run.player.hp;
    if (missing <= 0) return;
    const float factor = run.ascension >= 2 ? 0.8f : 1.0f;
    run.player.hp += static_cast<int>(missing * factor);
}

static void startNeow(RunState& run) {
    // Neow drains to 0 and heals the full amount (x0.8 at A2 WearyTraveler).
    run.player.hp = run.ascension >= 2
        ? static_cast<int>(run.player.maxHp * 0.8f) : run.player.maxHp;
    run.awaitingEvent = true;
    run.eventId = "NEOW";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("NEOW"));
    run.eventRelicOptions.clear();

    // CurseOptions: one uniform pick (all ten allowed in a single-player Ironclad run).
    std::vector<int> curses(kNeowCurses, kNeowCurses + 10);
    const int curse = curses[run.eventRng.NextInt(static_cast<int>(curses.size()))];

    std::vector<int> pos = {
        static_cast<int>(RelicId::ARCANE_SCROLL), static_cast<int>(RelicId::BOOMING_CONCH),
        static_cast<int>(RelicId::FISHING_ROD), static_cast<int>(RelicId::GOLDEN_PEARL),
        static_cast<int>(RelicId::LEAD_PAPERWEIGHT), static_cast<int>(RelicId::LOST_COFFER),
        static_cast<int>(RelicId::NEOWS_TORMENT), static_cast<int>(RelicId::NEW_LEAF),
        static_cast<int>(RelicId::PHIAL_HOLSTER), static_cast<int>(RelicId::PRECISE_SCISSORS),
        static_cast<int>(RelicId::SCROLL_BOXES), static_cast<int>(RelicId::WINGED_BOOTS),
    };
    // Paired removals: the positive counterpart of the rolled curse is not offered.
    switch (curse) {
        case static_cast<int>(RelicId::CURSED_PEARL):
            neowTakeOut(pos, static_cast<int>(RelicId::GOLDEN_PEARL)); break;
        case static_cast<int>(RelicId::HEFTY_TABLET):
            neowTakeOut(pos, static_cast<int>(RelicId::ARCANE_SCROLL)); break;
        case static_cast<int>(RelicId::LEAFY_POULTICE):
            neowTakeOut(pos, static_cast<int>(RelicId::NEW_LEAF)); break;
        case static_cast<int>(RelicId::PRECARIOUS_SHEARS):
            neowTakeOut(pos, static_cast<int>(RelicId::PRECISE_SCISSORS)); break;
        case static_cast<int>(RelicId::NEOWS_SACRIFICE):
            neowTakeOut(pos, static_cast<int>(RelicId::PHIAL_HOLSTER));
            neowTakeOut(pos, static_cast<int>(RelicId::LOST_COFFER));
            break;
        default:
            break;
    }
    if (curse != static_cast<int>(RelicId::LARGE_CAPSULE))
        pos.push_back(run.eventRng.NextBool() ? static_cast<int>(RelicId::LAVA_ROCK)
                                              : static_cast<int>(RelicId::SMALL_CAPSULE));
    pos.push_back(run.eventRng.NextBool() ? static_cast<int>(RelicId::NUTRITIOUS_OYSTER)
                                          : static_cast<int>(RelicId::STONE_HUMIDIFIER));
    pos.push_back(run.eventRng.NextBool() ? static_cast<int>(RelicId::NEOWS_TALISMAN)
                                          : static_cast<int>(RelicId::POMANDER));

    unstableShuffleIdx(pos, run.eventRng);
    const int p1 = pos[0];
    const int p2 = pos[1];
    run.eventRelicOptions = {p1, p2, curse};
    run.eventOptions = {sts2RelicName(static_cast<RelicId>(p1)),
                        sts2RelicName(static_cast<RelicId>(p2)),
                        sts2RelicName(static_cast<RelicId>(curse))};
}

// Applies the relic's AfterObtained effect. Passive relics only join the relic list;
// the immediate settlements use the engine primitives that already exist.
static void neowGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::GOLDEN_PEARL):
            run.player.gold += 150;
            break;
        case static_cast<int>(RelicId::CURSED_PEARL):
            run.player.deck.push_back({CardId::GREED, 0});
            run.player.gold += 333;
            break;
        case static_cast<int>(RelicId::DOWSING_ROD):
            run.player.deck.push_back({CardId::DOWSING, 0});
            break;
        case static_cast<int>(RelicId::NEOWS_TORMENT):
            run.player.deck.push_back({CardId::NEOWS_FURY, 0});
            break;
        case static_cast<int>(RelicId::ARCANE_SCROLL): {
            auto c = neowCardsOfRarity(run, CardRarity::RARE, 1);
            if (!c.empty()) run.player.deck.push_back({static_cast<CardId>(c[0].first), 0});
            break;
        }
        case static_cast<int>(RelicId::HEFTY_TABLET): {
            run.player.deck.push_back({CardId::INJURY, 0});
            auto offer = neowCardsOfRarity(run, CardRarity::RARE, 3);
            if (!offer.empty()) {
                run.cardOffer = std::move(offer);
                run.awaitingCardReward = true;
            }
            break;
        }
        case static_cast<int>(RelicId::LEAD_PAPERWEIGHT): {
            int n = 0;
            const CardId* pool = sts2CombatGenPool("COLORLESS", &n);
            auto offer = neowRegularCards(run, pool, n, 2);
            if (!offer.empty()) {
                run.cardOffer = std::move(offer);
                run.awaitingCardReward = true;
            }
            break;
        }
        case static_cast<int>(RelicId::LOST_COFFER): {
            int n = 0;
            const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
            auto offer = neowRegularCards(run, pool, n, 3);
            if (!offer.empty()) {
                run.cardOffer = std::move(offer);
                run.awaitingCardReward = true;
            }
            grantPotionReward(run);
            break;
        }
        case static_cast<int>(RelicId::SMALL_CAPSULE):
            neowRelicReward(run);
            break;
        case static_cast<int>(RelicId::LARGE_CAPSULE):
            neowRelicReward(run);
            neowRelicReward(run);
            run.player.deck.push_back({CardId::STRIKE_IRONCLAD, 0});
            run.player.deck.push_back({CardId::DEFEND_IRONCLAD, 0});
            break;
        case static_cast<int>(RelicId::NEOWS_TALISMAN): {
            // Upgrade the last Strike and the last Defend in the deck.
            for (int pass = 0; pass < 2; ++pass) {
                const CardId want = pass == 0 ? CardId::STRIKE_IRONCLAD : CardId::DEFEND_IRONCLAD;
                for (int i = static_cast<int>(run.player.deck.size()) - 1; i >= 0; --i)
                    if (run.player.deck[i].id == want) {
                        if (run.player.deck[i].upgrade
                            < BattleContext::cardDef(want).maxUpgradeLevel)
                            ++run.player.deck[i].upgrade;
                        break;
                    }
            }
            break;
        }
        case static_cast<int>(RelicId::NUTRITIOUS_OYSTER):
            run.player.maxHp += 11;
            run.player.hp += 11;
            break;
        case static_cast<int>(RelicId::LEAFY_POULTICE):
            run.player.maxHp = std::max(1, run.player.maxHp - 12);
            run.player.hp = std::min(run.player.hp, run.player.maxHp);
            neowTransformBasic(run, CardId::STRIKE_IRONCLAD);
            neowTransformBasic(run, CardId::DEFEND_IRONCLAD);
            break;
        case static_cast<int>(RelicId::NEOWS_SACRIFICE):
            grantPotionById(run, static_cast<int>(PotionId::AMBERGRIS));
            run.player.deck.push_back({CardId::GUILTY, 0});
            break;
        case static_cast<int>(RelicId::NEOWS_BONES): {
            static const CardId kCurses[5] = {CardId::GREED, CardId::GUILTY, CardId::INJURY,
                                              CardId::CLUMSY, CardId::DEBT};
            neowRelicReward(run);
            neowRelicReward(run);
            run.player.deck.push_back({kCurses[run.eventRng.NextInt(5)], 0});
            break;
        }
        case static_cast<int>(RelicId::PRECARIOUS_SHEARS):
        case static_cast<int>(RelicId::PRECISE_SCISSORS):
        case static_cast<int>(RelicId::POMANDER):
            break; // handled by the caller (card grid)
        case static_cast<int>(RelicId::SILKEN_TRESS):
            run.player.gold = 0;
            break;
        case static_cast<int>(RelicId::PHIAL_HOLSTER):
            // The engine's potion belt is three slots; the extra slot is not modeled.
            grantPotionReward(run);
            grantPotionReward(run);
            break;
        case static_cast<int>(RelicId::NEW_LEAF):
            break; // handled by the caller (transform grid)
        default:
            break;
    }
}

// Card-grid setup shared by Neow's removal/upgrade/transform relics.
static void neowOpenCardGrid(RunState& run, bool remove, bool upgrade, bool transform,
                             int picks) {
    run.eventCardPool.clear();
    run.eventCardDeckIdx.clear();
    for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
        const RunCard& c = run.player.deck[i];
        if (upgrade && c.upgrade >= BattleContext::cardDef(c.id).maxUpgradeLevel) continue;
        run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
        run.eventCardDeckIdx.push_back(i);
    }
    run.eventCardsRemove = remove;
    run.eventCardsUpgrade = upgrade;
    run.eventCardsTransform = transform;
    run.eventCardsEnchant = false;
    run.eventCardsTransformTo = false;
    run.eventCardsLeft = std::min(picks, static_cast<int>(run.eventCardPool.size()));
    run.awaitingEvent = false;
    run.awaitingEventCards = !run.eventCardPool.empty();
}

// ---- Darv (Ancient, act 2+) ----
// Darv.GenerateInitialOptions: one relic per valid set, shuffled, then either the first
// three or the first two plus DustyTome (NextBool). Act gates mirror the source:
// Ectoplasm/Sozu only in act 2, Philosopher's Stone/Velvet Choker from act 2 on.
struct DarvSet { int relic; int gate; }; // gate 0 = any act, 1 = act 2 only, 2 = act 2+
static const DarvSet kDarvSets[11] = {
    {static_cast<int>(RelicId::ASTROLABE), 0},
    {static_cast<int>(RelicId::BLACK_STAR), 0},
    {static_cast<int>(RelicId::CALLING_BELL), 0},
    {static_cast<int>(RelicId::EMPTY_CAGE), 0},
    {static_cast<int>(RelicId::PANDORAS_BOX), 0},
    {static_cast<int>(RelicId::RUNIC_PYRAMID), 0},
    {static_cast<int>(RelicId::SNECKO_EYE), 0},
    {static_cast<int>(RelicId::ECTOPLASM), 1},
    {static_cast<int>(RelicId::SOZU), 1},
    {static_cast<int>(RelicId::PHILOSOPHERS_STONE), 2},
    {static_cast<int>(RelicId::VELVET_CHOKER), 2},
};

// DustyTome.SetupForPlayer: an Ancient-rarity card from the character pool on the
// PlayerRng.Rewards stream (implemented cards only).
static int pickDustyTomeCard(RunState& run) {
    int n = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
    std::vector<int> cand;
    for (int i = 0; i < n; ++i)
        if (BattleContext::cardDef(pool[i]).rarity == CardRarity::ANCIENT
            && BattleContext::cardImplemented(pool[i]))
            cand.push_back(static_cast<int>(pool[i]));
    if (cand.empty()) return 0;
    return cand[run.rewards.NextInt(static_cast<int>(cand.size()))];
}

static void startDarv(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "DARV";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("DARV"));
    run.eventRelicOptions.clear();

    std::vector<int> picks;
    for (const DarvSet& s : kDarvSets) {
        if (s.gate == 1 && run.actIndex != 1) continue;
        if (s.gate == 2 && run.actIndex < 1) continue;
        // Rng.NextItem over a one-relic set still consumes one draw.
        run.eventRng.NextInt(1);
        picks.push_back(s.relic);
    }
    unstableShuffleIdx(picks, run.eventRng);

    const bool withTome = run.eventRng.NextBool();
    const int take = withTome ? 2 : 3;
    for (int i = 0; i < take && i < static_cast<int>(picks.size()); ++i)
        run.eventRelicOptions.push_back(picks[i]);
    run.eventDustyTomeCard = 0;
    if (withTome) {
        run.eventDustyTomeCard = pickDustyTomeCard(run);
        run.eventRelicOptions.push_back(static_cast<int>(RelicId::DUSTY_TOME));
    }
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

// CardCmd.Transform with a random implemented card (no upgrade).
static void darvTransformDeckCard(RunState& run, int deckIdx) {
    int n = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
    const int oldId = static_cast<int>(run.player.deck[deckIdx].id);
    std::vector<int> cand;
    for (int i = 0; i < n; ++i)
        if (static_cast<int>(pool[i]) != oldId && BattleContext::cardImplemented(pool[i]))
            cand.push_back(static_cast<int>(pool[i]));
    if (cand.empty()) return;
    run.player.deck[deckIdx].id =
        static_cast<CardId>(cand[run.eventRng.NextInt(static_cast<int>(cand.size()))]);
    run.player.deck[deckIdx].upgrade = 0;
}

static void darvGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::DUSTY_TOME):
            if (run.eventDustyTomeCard != 0)
                run.player.deck.push_back(
                    {static_cast<CardId>(run.eventDustyTomeCard), 1});
            break;
        case static_cast<int>(RelicId::CALLING_BELL):
            run.player.deck.push_back({CardId::CURSE_OF_THE_BELL, 0});
            // RelicReward with a fixed rarity: one Common, one Uncommon, one Rare.
            for (RelicRarity rarity : {RelicRarity::COMMON, RelicRarity::UNCOMMON,
                                       RelicRarity::RARE}) {
                const int rid = pullRelic(run, rarity, /*fromBack=*/false);
                if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            }
            break;
        case static_cast<int>(RelicId::PANDORAS_BOX):
            // Transform every removable Basic Strike/Defend.
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const CardId id = run.player.deck[i].id;
                if (id == CardId::STRIKE_IRONCLAD || id == CardId::DEFEND_IRONCLAD)
                    darvTransformDeckCard(run, i);
            }
            break;
        default:
            break;
    }
}

// ---- Orobas (Ancient) ----
// Three independent picks: pool 1 (ElectricShrymp / GlassEye / PrismaticGem 1/3 or
// SeaGlass), pool 2 (four relic choices), pool 3 (TouchOfOrobas / ArchaicTooth, each
// offered only when its SetupForPlayer finds the pieces it needs).
static std::vector<std::pair<int,int>> orobasCardsOfRarity(RunState& run, CardRarity want,
                                                           int count) {
    int poolCount = 0;
    const CardId* pool = sts2RewardPool(run.player.character.c_str(), &poolCount);
    std::vector<int> cand;
    for (int i = 0; i < poolCount; ++i)
        if (BattleContext::cardDef(pool[i]).rarity == want
            && BattleContext::cardImplemented(pool[i]))
            cand.push_back(static_cast<int>(pool[i]));
    std::vector<std::pair<int,int>> out;
    for (int k = 0; k < count && !cand.empty(); ++k) {
        const int at = run.eventRng.NextInt(static_cast<int>(cand.size()));
        out.emplace_back(cand[at], 0);
        cand.erase(cand.begin() + at);
    }
    return out;
}

// Appends one card-reward batch to the run's offer queue (first + second + extras).
static void queueCardOffer(RunState& run, std::vector<std::pair<int,int>> offer) {
    if (offer.empty()) return;
    if (run.cardOffer.empty()) {
        run.cardOffer = std::move(offer);
        run.awaitingCardReward = true;
    } else if (run.cardOffer2.empty() && !run.pendingSecondCardReward) {
        run.cardOffer2 = std::move(offer);
        run.pendingSecondCardReward = true;
    } else {
        run.extraCardOffers.push_back(std::move(offer));
    }
}

static void startOrobas(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "OROBAS";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("OROBAS"));
    run.eventRelicOptions.clear();

    // NextItem over "other unlocked characters" is empty here, so it draws nothing; the
    // source then falls back to the current character for SeaGlass.
    const int pool1Swap = run.eventRng.NextFloat() < (1.0f / 3.0f)
        ? static_cast<int>(RelicId::PRISMATIC_GEM)
        : static_cast<int>(RelicId::SEA_GLASS);
    const int kPool1[3] = {static_cast<int>(RelicId::ELECTRIC_SHRYMP),
                           static_cast<int>(RelicId::GLASS_EYE), pool1Swap};
    const int kPool2[4] = {static_cast<int>(RelicId::ALCHEMICAL_COFFER),
                           static_cast<int>(RelicId::DRIFTWOOD),
                           static_cast<int>(RelicId::RADIANT_PEARL),
                           static_cast<int>(RelicId::SAND_CASTLE)};
    std::vector<int> pool3;
    pool3.push_back(static_cast<int>(RelicId::TOUCH_OF_OROBAS));
    bool hasStarterCard = false;
    for (const RunCard& c : run.player.deck)
        if (c.id == CardId::BASH) { hasStarterCard = true; break; }
    if (hasStarterCard) pool3.push_back(static_cast<int>(RelicId::ARCHAIC_TOOTH));

    run.eventRelicOptions.push_back(kPool1[run.eventRng.NextInt(3)]);
    run.eventRelicOptions.push_back(kPool2[run.eventRng.NextInt(4)]);
    run.eventRelicOptions.push_back(pool3[run.eventRng.NextInt(
        static_cast<int>(pool3.size()))]);
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

static void orobasGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::ELECTRIC_SHRYMP): {
            // Enchant one chosen card with Imbued 1. Imbued has no combat behaviour yet,
            // but the enchantment id is written onto the card (recorded gap).
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                const CardDef& d = BattleContext::cardDef(c.id);
                if (d.type == CardType::STATUS || d.type == CardType::CURSE
                    || d.type == CardType::QUEST || d.hasFlag(CF_UNPLAYABLE))
                    continue;
                if (c.enchant != 0) continue;
                run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::IMBUED);
            run.eventEnchantAmount = 1;
            run.eventCardsTransform = false;
            run.eventCardsTransformTo = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            break;
        }
        case static_cast<int>(RelicId::GLASS_EYE):
            // Five rewards: Common, Common, Uncommon, Uncommon, Rare (three cards each).
            queueCardOffer(run, orobasCardsOfRarity(run, CardRarity::COMMON, 3));
            queueCardOffer(run, orobasCardsOfRarity(run, CardRarity::COMMON, 3));
            queueCardOffer(run, orobasCardsOfRarity(run, CardRarity::UNCOMMON, 3));
            queueCardOffer(run, orobasCardsOfRarity(run, CardRarity::UNCOMMON, 3));
            queueCardOffer(run, orobasCardsOfRarity(run, CardRarity::RARE, 3));
            break;
        case static_cast<int>(RelicId::SEA_GLASS): {
            // Five cards of each rarity offered on one grid; the player may take any
            // subset (the engine grid takes all picks, recorded as a scaffold choice).
            std::vector<std::pair<int,int>> cards;
            for (CardRarity r : {CardRarity::COMMON, CardRarity::UNCOMMON,
                                 CardRarity::RARE}) {
                auto batch = orobasCardsOfRarity(run, r, 5);
                cards.insert(cards.end(), batch.begin(), batch.end());
            }
            run.eventCardPool = std::move(cards);
            run.eventCardDeckIdx.clear();
            run.eventCardsEnchant = false;
            run.eventCardsTransform = false;
            run.eventCardsTransformTo = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = static_cast<int>(run.eventCardPool.size());
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            break;
        }
        case static_cast<int>(RelicId::ALCHEMICAL_COFFER):
            // Four potions; the engine belt has three slots, so overflow is discarded.
            for (int i = 0; i < 4; ++i) grantPotionReward(run);
            break;
        case static_cast<int>(RelicId::SAND_CASTLE): {
            // Upgrade up to six random upgradable deck cards.
            std::vector<int> idx;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i)
                if (run.player.deck[i].upgrade
                    < BattleContext::cardDef(run.player.deck[i].id).maxUpgradeLevel)
                    idx.push_back(i);
            unstableShuffleIdx(idx, run.eventRng);
            for (int k = 0; k < 6 && k < static_cast<int>(idx.size()); ++k)
                ++run.player.deck[idx[k]].upgrade;
            break;
        }
        case static_cast<int>(RelicId::TOUCH_OF_OROBAS): {
            // RelicCmd.Replace: the starter relic becomes its upgraded counterpart.
            static const struct { int from; int to; } kUpgrades[5] = {
                {static_cast<int>(RelicId::BURNING_BLOOD), static_cast<int>(RelicId::BLACK_BLOOD)},
                {static_cast<int>(RelicId::RING_OF_THE_SNAKE), static_cast<int>(RelicId::RING_OF_THE_DRAKE)},
                {static_cast<int>(RelicId::DIVINE_RIGHT), static_cast<int>(RelicId::DIVINE_DESTINY)},
                {static_cast<int>(RelicId::BOUND_PHYLACTERY), static_cast<int>(RelicId::PHYLACTERY_UNBOUND)},
                {static_cast<int>(RelicId::CRACKED_CORE), static_cast<int>(RelicId::INFUSED_CORE)},
            };
            int starter = static_cast<int>(RelicId::INVALID);
            for (int rid : run.relics)
                if (sts2RelicDef(static_cast<RelicId>(rid)).rarity == RelicRarity::STARTER) {
                    starter = rid;
                    break;
                }
            if (starter != static_cast<int>(RelicId::INVALID))
                for (const auto& u : kUpgrades)
                    if (starter == u.from) {
                        for (int& rid : run.relics) if (rid == starter) rid = u.to;
                        break;
                    }
            break;
        }
        case static_cast<int>(RelicId::ARCHAIC_TOOTH): {
            // Transform the transcendence starter card (Ironclad: Bash -> Break).
            static const struct { CardId from; CardId to; } kMap[5] = {
                {CardId::BASH, CardId::BREAK},
                {CardId::NEUTRALIZE, CardId::SUPPRESS},
                {CardId::UNLEASH, CardId::PROTECTOR},
                {CardId::FALLING_STAR, CardId::METEOR_SHOWER},
                {CardId::DUALCAST, CardId::QUADCAST},
            };
            for (RunCard& c : run.player.deck)
                for (const auto& m : kMap)
                    if (c.id == m.from) {
                        c.id = m.to;
                        break;
                    }
            break;
        }
        default:
            break; // PrismaticGem / Driftwood / RadiantPearl are passive for now.
    }
}

// Opt-in opening event (the test hook mirrors eventOverride for the ? rooms): the caller
// asks for it explicitly so existing seeds keep their current opening.
bool startNeowOpening(RunState& run) {
    startNeow(run);
    return true;
}

bool startDarvOpening(RunState& run) {
    startDarv(run);
    return true;
}

bool startOrobasOpening(RunState& run) {
    startOrobas(run);
    return true;
}

// ---- Vakuu (Ancient) ----
// Three pools, each unstable-shuffled in order, then the first of each is offered.
static void startVakuu(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "VAKUU";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("VAKUU"));
    std::vector<int> p1 = {static_cast<int>(RelicId::BLOOD_SOAKED_ROSE),
                           static_cast<int>(RelicId::WHISPERING_EARRING),
                           static_cast<int>(RelicId::FIDDLE)};
    std::vector<int> p2 = {static_cast<int>(RelicId::PRESERVED_FOG),
                           static_cast<int>(RelicId::SERE_TALON),
                           static_cast<int>(RelicId::DISTINGUISHED_CAPE)};
    std::vector<int> p3 = {static_cast<int>(RelicId::CHOICES_PARADOX),
                           static_cast<int>(RelicId::MUSIC_BOX),
                           static_cast<int>(RelicId::LORDS_PARASOL),
                           static_cast<int>(RelicId::JEWELED_MASK)};
    unstableShuffleIdx(p1, run.eventRng);
    unstableShuffleIdx(p2, run.eventRng);
    unstableShuffleIdx(p3, run.eventRng);
    run.eventRelicOptions = {p1[0], p2[0], p3[0]};
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

static void vakuuGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::BLOOD_SOAKED_ROSE):
            run.player.deck.push_back({CardId::ENTHRALLED, 0});
            break;
        case static_cast<int>(RelicId::PRESERVED_FOG):
            // The curse is appended before the grid snapshot so it cannot be removed.
            run.player.deck.push_back({CardId::FOLLY, 0});
            neowOpenCardGrid(run, true, false, false, 3);
            break;
        case static_cast<int>(RelicId::SERE_TALON): {
            run.player.maxHp = std::max(1, run.player.maxHp - 9);
            run.player.hp = std::min(run.player.hp, run.player.maxHp);
            for (int i = 0; i < 3; ++i) run.player.deck.push_back({CardId::WISH, 0});
            break;
        }
        case static_cast<int>(RelicId::DISTINGUISHED_CAPE): {
            static const CardId kCurses[5] = {CardId::GREED, CardId::GUILTY,
                                              CardId::INJURY, CardId::CLUMSY,
                                              CardId::DEBT};
            std::vector<int> pool(5);
            for (int i = 0; i < 5; ++i) pool[i] = i;
            unstableShuffleIdx(pool, run.eventRng);
            run.player.deck.push_back({kCurses[pool[0]], 0});
            run.player.deck.push_back({kCurses[pool[1]], 0});
            for (int i = 0; i < 3; ++i) run.player.deck.push_back({CardId::APPARITION, 0});
            break;
        }
        default:
            break; // WhisperingEarring / Fiddle / ChoicesParadox / MusicBox /
                   // LordsParasol / JeweledMask are passive for now.
    }
}

bool startVakuuOpening(RunState& run) {
    startVakuu(run);
    return true;
}

// ---- Pael (Ancient) ----
// Pool 2 in the source is [Wing] + optional Claw/Tooth, then AddRange(list) (the whole
// list is duplicated), then Growth. The duplication is part of the odds and is kept.
static void startPael(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "PAEL";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("PAEL"));

    const int kPool1[3] = {static_cast<int>(RelicId::PAELS_FLESH),
                           static_cast<int>(RelicId::PAELS_HORN),
                           static_cast<int>(RelicId::PAELS_TEARS)};
    std::vector<int> pool2 = {static_cast<int>(RelicId::PAELS_WING)};
    int goopyTargets = 0;
    int removable = 0;
    for (const RunCard& c : run.player.deck) {
        const CardDef& d = BattleContext::cardDef(c.id);
        if (d.hasTag(CT_DEFEND) && c.enchant == 0) ++goopyTargets;
        if (d.maxUpgradeLevel > 0) ++removable;
    }
    if (goopyTargets >= 3) pool2.push_back(static_cast<int>(RelicId::PAELS_CLAW));
    if (removable >= 5) pool2.push_back(static_cast<int>(RelicId::PAELS_TOOTH));
    pool2.insert(pool2.end(), pool2.begin(), pool2.end());
    pool2.push_back(static_cast<int>(RelicId::PAELS_GROWTH));

    std::vector<int> pool3 = {static_cast<int>(RelicId::PAELS_EYE),
                              static_cast<int>(RelicId::PAELS_BLOOD),
                              static_cast<int>(RelicId::PAELS_LEGION)};

    run.eventRelicOptions = {
        kPool1[run.eventRng.NextInt(3)],
        pool2[run.eventRng.NextInt(static_cast<int>(pool2.size()))],
        pool3[run.eventRng.NextInt(static_cast<int>(pool3.size()))],
    };
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

// Generic enchant eligibility (no status/curse/quest, playable, no other enchant).
static bool paelEnchantable(const RunCard& c) {
    const CardDef& d = BattleContext::cardDef(c.id);
    if (d.type == CardType::STATUS || d.type == CardType::CURSE
        || d.type == CardType::QUEST || d.hasFlag(CF_UNPLAYABLE))
        return false;
    return c.enchant == 0;
}

static void paelOpenEnchantGrid(RunState& run, EnchantmentId id, int amount) {
    run.eventCardPool.clear();
    run.eventCardDeckIdx.clear();
    for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
        const RunCard& c = run.player.deck[i];
        if (!paelEnchantable(c)) continue;
        run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
        run.eventCardDeckIdx.push_back(i);
    }
    run.eventCardsEnchant = true;
    run.eventEnchantId = static_cast<int>(id);
    run.eventEnchantAmount = amount;
    run.eventCardsTransform = false;
    run.eventCardsTransformTo = false;
    run.eventCardsRemove = false;
    run.eventCardsUpgrade = false;
    run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
    run.awaitingEvent = false;
    run.awaitingEventCards = !run.eventCardPool.empty();
}

static void paelGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::PAELS_HORN):
            run.player.deck.push_back({CardId::RELAX, 0});
            run.player.deck.push_back({CardId::RELAX, 0});
            break;
        case static_cast<int>(RelicId::PAELS_CLAW):
            // Enchant every Defend-tagged card with Goopy 1.
            for (RunCard& c : run.player.deck) {
                if (!BattleContext::cardDef(c.id).hasTag(CT_DEFEND)) continue;
                if (c.enchant != 0) continue;
                c.enchant = static_cast<int>(EnchantmentId::GOOPY);
                c.enchantAmount = 1;
                c.enchantDisabled = false;
            }
            break;
        case static_cast<int>(RelicId::PAELS_TOOTH): {
            // Remove up to five upgradable cards (the relic keeps them to return after
            // later combats; that run-long bookkeeping is not modeled yet).
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (BattleContext::cardDef(c.id).maxUpgradeLevel <= 0) continue;
                run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsRemove = true;
            run.eventCardsUpgrade = false;
            run.eventCardsTransform = false;
            run.eventCardsEnchant = false;
            run.eventCardsTransformTo = false;
            run.eventCardsLeft = std::min(5, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            break;
        }
        case static_cast<int>(RelicId::PAELS_GROWTH):
            paelOpenEnchantGrid(run, EnchantmentId::CLONE, 4);
            break;
        default:
            break; // Flesh/Tears/Wing/Eye/Blood/Legion are passive for now.
    }
}

bool startPaelOpening(RunState& run) {
    startPael(run);
    return true;
}

// ---- Nonupeipe (Ancient, Glory pool) ----
// GenerateInitialOptions: the nine relic options, plus BeautifulBracelet when at least
// four deck cards can take Swift; then UnstableShuffle and the first three are offered.
static void startNonupeipe(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "NONUPEIPE";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("NONUPEIPE"));

    std::vector<int> pool = {
        static_cast<int>(RelicId::BLESSED_ANTLER),
        static_cast<int>(RelicId::BRILLIANT_SCARF),
        static_cast<int>(RelicId::DELICATE_FROND),
        static_cast<int>(RelicId::DIAMOND_DIADEM),
        static_cast<int>(RelicId::FUR_COAT),
        static_cast<int>(RelicId::GLITTER),
        static_cast<int>(RelicId::JEWELRY_BOX),
        static_cast<int>(RelicId::LOOMING_FRUIT),
        static_cast<int>(RelicId::SIGNET_RING),
    };
    int swiftTargets = 0;
    for (const RunCard& c : run.player.deck)
        if (paelEnchantable(c)) ++swiftTargets;
    if (swiftTargets >= 4)
        pool.push_back(static_cast<int>(RelicId::BEAUTIFUL_BRACELET));
    unstableShuffleIdx(pool, run.eventRng);
    run.eventRelicOptions.assign(pool.begin(), pool.begin() + 3);
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

static void nonupeipeGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::JEWELRY_BOX):
            // RunState.CreateCard<Apotheosis> then CardPileCmd.Add(deck).
            run.player.deck.push_back({CardId::APOTHEOSIS, 0});
            break;
        case static_cast<int>(RelicId::LOOMING_FRUIT):
            // CreatureCmd.GainMaxHp(31): max HP +31, then heal 31.
            run.player.maxHp += 31;
            run.player.hp = std::min(run.player.maxHp, run.player.hp + 31);
            break;
        case static_cast<int>(RelicId::SIGNET_RING):
            run.player.gold += 888;
            break;
        case static_cast<int>(RelicId::BEAUTIFUL_BRACELET): {
            // swift.CanEnchant cards TakeRandom(4, Rng.Niche): UnstableShuffle the
            // eligible deck cards and enchant the first four with Swift 2.
            std::vector<int> idx;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i)
                if (paelEnchantable(run.player.deck[i])) idx.push_back(i);
            unstableShuffleIdx(idx, run.rng[static_cast<int>(RngStream::Niche)]);
            for (int k = 0; k < 4 && k < static_cast<int>(idx.size()); ++k) {
                RunCard& c = run.player.deck[idx[k]];
                c.enchant = static_cast<int>(EnchantmentId::SWIFT);
                c.enchantAmount = 2;
                c.enchantDisabled = false;
            }
            break;
        }
        default:
            // BlessedAntler/BrilliantScarf/DelicateFrond/DiamondDiadem/FurCoat/Glitter
            // act inside combat or on the map; they stay passive for now (recorded gap).
            break;
    }
}

bool startNonupeipeOpening(RunState& run) {
    startNonupeipe(run);
    return true;
}

// ---- Tanx (Ancient, Glory pool) ----
// GenerateInitialOptions: the nine relic options, plus TriBoomerang when at least three
// deck cards can take Instinct; UnstableShuffle in the source, then the first three.
static void startTanx(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "TANX";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("TANX"));

    std::vector<int> pool = {
        static_cast<int>(RelicId::CLAWS),
        static_cast<int>(RelicId::CROSSBOW),
        static_cast<int>(RelicId::IRON_CLUB),
        static_cast<int>(RelicId::MEAT_CLEAVER),
        static_cast<int>(RelicId::SAI),
        static_cast<int>(RelicId::SPIKED_GAUNTLETS),
        static_cast<int>(RelicId::TANXS_WHISTLE),
        static_cast<int>(RelicId::THROWING_AXE),
        static_cast<int>(RelicId::WAR_HAMMER),
    };
    int instinctTargets = 0;
    for (const RunCard& c : run.player.deck)
        if (BattleContext::cardDef(c.id).type == CardType::ATTACK && paelEnchantable(c))
            ++instinctTargets;
    if (instinctTargets >= 3)
        pool.push_back(static_cast<int>(RelicId::TRI_BOOMERANG));
    unstableShuffleIdx(pool, run.eventRng);
    run.eventRelicOptions.assign(pool.begin(), pool.begin() + 3);
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

static void tanxGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::TANXS_WHISTLE):
            run.player.deck.push_back({CardId::WHISTLE, 0});
            break;
        case static_cast<int>(RelicId::CLAWS): {
            // FromDeckForTransformation(0..6): each pick becomes a Maul, keeping the
            // original upgrade and enchantment when the new card can carry them.
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransformTo = true;
            run.eventTransformToId = static_cast<int>(CardId::MAUL);
            run.eventTransformKeep = true;
            run.eventTransformUpgrade = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(6, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            break;
        }
        case static_cast<int>(RelicId::TRI_BOOMERANG): {
            // FromDeckForEnchantment(Instinct, 3): enchant three chosen Attack cards.
            // Instinct has no combat behaviour yet; the id is still written (gap).
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (BattleContext::cardDef(c.id).type != CardType::ATTACK) continue;
                if (!paelEnchantable(c)) continue;
                run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::INSTINCT);
            run.eventEnchantAmount = 1;
            run.eventCardsTransform = false;
            run.eventCardsTransformTo = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(3, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            break;
        }
        default:
            // Crossbow/IronClub/MeatCleaver/Sai/SpikedGauntlets/ThrowingAxe/WarHammer
            // are passive until their combat/rest hooks are modeled (recorded gap).
            break;
    }
}

bool startTanxOpening(RunState& run) {
    startTanx(run);
    return true;
}

// ---- Tezcatara (Ancient, Hive pool) ----
// GenerateInitialOptions: one pick from each of the three pools on the event Rng;
// NutritiousSoup joins the first pool when the deck holds a Basic Strike.
static void startTezcatara(RunState& run) {
    ancientOpeningHeal(run);
    run.awaitingEvent = true;
    run.eventId = "TEZCATARA";
    run.eventPage = 0;
    run.eventRng = Sts2Rng(run.seed + xxhash64("TEZCATARA"));

    std::vector<int> p1 = {static_cast<int>(RelicId::VERY_HOT_COCOA),
                           static_cast<int>(RelicId::YUMMY_COOKIE)};
    bool basicStrike = false;
    for (const RunCard& c : run.player.deck) {
        const CardDef& d = BattleContext::cardDef(c.id);
        if (d.rarity == CardRarity::BASIC && d.hasTag(CT_STRIKE)) { basicStrike = true; break; }
    }
    if (basicStrike) p1.push_back(static_cast<int>(RelicId::NUTRITIOUS_SOUP));
    const std::vector<int> p2 = {static_cast<int>(RelicId::BIIIG_HUG),
                                 static_cast<int>(RelicId::STORYBOOK),
                                 static_cast<int>(RelicId::TOASTY_MITTENS)};
    const std::vector<int> p3 = {static_cast<int>(RelicId::GOLDEN_COMPASS),
                                 static_cast<int>(RelicId::PUMPKIN_CANDLE),
                                 static_cast<int>(RelicId::TOY_BOX),
                                 static_cast<int>(RelicId::SEAL_OF_GOLD)};

    run.eventRelicOptions.clear();
    run.eventRelicOptions.push_back(p1[run.eventRng.NextInt(static_cast<int>(p1.size()))]);
    run.eventRelicOptions.push_back(p2[run.eventRng.NextInt(static_cast<int>(p2.size()))]);
    run.eventRelicOptions.push_back(p3[run.eventRng.NextInt(static_cast<int>(p3.size()))]);
    run.eventOptions.clear();
    for (int rid : run.eventRelicOptions)
        run.eventOptions.push_back(sts2RelicName(static_cast<RelicId>(rid)));
}

static void tezcataraGrant(RunState& run, int relic) {
    run.relics.push_back(relic);
    switch (relic) {
        case static_cast<int>(RelicId::NUTRITIOUS_SOUP): {
            // Enchant every Basic Strike that TezcatarasEmber can attach to (Amount 1;
            // the ember's cost/eternal/damage behaviour is not wired yet, gap).
            for (RunCard& c : run.player.deck) {
                const CardDef& d = BattleContext::cardDef(c.id);
                if (d.rarity != CardRarity::BASIC || !d.hasTag(CT_STRIKE)) continue;
                if (!paelEnchantable(c)) continue;
                c.enchant = static_cast<int>(EnchantmentId::TEZCATARAS_EMBER);
                c.enchantAmount = 1;
                c.enchantDisabled = false;
            }
            break;
        }
        case static_cast<int>(RelicId::YUMMY_COOKIE):
            // FromDeckForUpgrade(4): upgrade four chosen upgradable cards.
            neowOpenCardGrid(run, false, true, false, 4);
            break;
        case static_cast<int>(RelicId::BIIIG_HUG):
            // FromDeckForRemoval(4): remove four chosen cards. The Soot added on each
            // shuffle is a battle hook that is not modeled yet (gap).
            neowOpenCardGrid(run, true, false, false, 4);
            break;
        case static_cast<int>(RelicId::STORYBOOK):
            run.player.deck.push_back({CardId::BRIGHTEST_FLAME, 0});
            break;
        case static_cast<int>(RelicId::TOY_BOX): {
            // Five RelicFactory.PullNextRelicFromFront(Ower) rewards; each rolls its
            // rarity on the Rewards stream. Wax melting is not modeled (gap).
            for (int k = 0; k < 5; ++k) {
                const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
                if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            }
            break;
        }
        default:
            // VeryHotCocoa, ToastyMittens, GoldenCompass (map regeneration),
            // PumpkinCandle (rest-site option) and SealOfGold stay passive (gaps).
            break;
    }
}

bool startTezcataraOpening(RunState& run) {
    startTezcatara(run);
    return true;
}

RunState startRun(const std::string& stringSeed, int hpOverride, int ascension) {
    RunState run;
    run.stringSeed = stringSeed;
    run.ascension = ascension;
    // A8/A9 swap the shared monster tables; stating the level on every run keeps
    // back-to-back runs at different levels independent.
    applyAscensionToMonsterDefs(ascension);
    run.seed = xxhash64(stringSeed);
    for (int i = 0; i < RNG_STREAM_COUNT; ++i)
        run.rng[i] = Sts2Rng(run.seed, streamName(i));
    run.upFront = Sts2Rng(run.seed, "up_front");
    run.rewards = Sts2Rng(run.seed, "rewards");
    // PlayerRngSet creates one Rng per PlayerRngType: Rng(seed, snake_case(type)).
    run.shops = Sts2Rng(run.seed, "shops");
    // RunRngSet.UnknownMapPoint: the ? room type roll.
    run.unknownMapPoint = Sts2Rng(run.seed, "unknown_map_point");
    // Ironclad.StartingRelics = [BurningBlood]; the grab bags are built at run start.
    run.relics.push_back(static_cast<int>(RelicId::BURNING_BLOOD));
    populateRelicBag(run);

    run.player.character = "IRONCLAD";
    run.player.hp = 80;
    run.player.maxHp = 80;
    if (hpOverride > 0) {
        run.player.hp = hpOverride;
        run.player.maxHp = hpOverride;
    }
    run.player.gold = 99;
    const int strike = lookupCardId("STRIKE_IRONCLAD");
    const int defend = lookupCardId("DEFEND_IRONCLAD");
    const int bash = lookupCardId("BASH");
    for (int i = 0; i < 5; ++i) run.player.deck.push_back({static_cast<CardId>(strike), 0});
    for (int i = 0; i < 4; ++i) run.player.deck.push_back({static_cast<CardId>(defend), 0});
    if (bash > 0) run.player.deck.push_back({static_cast<CardId>(bash), 0});
    // AscensionManager.ApplyEffectsTo: TightBelt shrinks the belt, AscendersBane adds
    // the curse to the deck before the first shuffle.
    run.potionSlots = 3 - (ascension >= 4 ? 1 : 0);
    if (ascension >= 5)
        run.player.deck.push_back({CardId::ASCENDERS_BANE, 0});

    beginAct(run);
    return run;
}

std::vector<int> availablePoints(const RunState& run) {
    std::vector<int> out;
    const int from = run.current < 0 ? run.map.start : run.current;
    if (from < 0) return out;
    for (int child : run.map.points[from].children) {
        const MapPoint& p = run.map.points[child];
        if (p.type == MapPointType::Boss || p.type == MapPointType::Ancient) {
            out.push_back(child);
        } else if (run.map.inMap(child)) {
            out.push_back(child);
        }
    }
    std::sort(out.begin(), out.end(), [&](int a, int b) {
        const MapPoint& pa = run.map.points[a];
        const MapPoint& pb = run.map.points[b];
        if (pa.col != pb.col) return pa.col < pb.col;
        return pa.row < pb.row;
    });
    return out;
}

bool enterPoint(RunState& run, int pointIdx, long long searchSims) {
    if (run.over || pointIdx < 0 || pointIdx >= static_cast<int>(run.map.points.size())) return false;
    const MapPoint& p = run.map.points[pointIdx];
    ++run.totalFloor;
    ++run.actFloor;
    run.visitedPoints.push_back(pointIdx);

    RoomLog log;
    log.floor = run.totalFloor;
    log.type = p.type;

    switch (p.type) {
        case MapPointType::Monster:
        case MapPointType::Elite:
        case MapPointType::Boss: {
            const EncounterDef* forced = nullptr;
            int encIdx = -1;
            if (run.toolboxResumePoint == pointIdx) {
                run.toolboxResumePoint = -1;
                if (!run.toolboxEncId.empty()) forced = findEncounter(run.toolboxEncId);
                else encIdx = run.toolboxEncIdx;
            } else {
                forced = (p.type == MapPointType::Boss && !run.bossOverride.empty())
                    ? findEncounter(run.bossOverride) : nullptr;
                encIdx = forced ? -1
                    : pickEncounterIndex(run, p.type == MapPointType::Elite,
                                         p.type == MapPointType::Boss);
            }
            if (!forced && encIdx < 0) {
                std::fprintf(stderr, "no scaffold encounter for room type\n");
                run.over = true;
                return false;
            }
            // Toolbox: pause before the fight and offer three implemented colorless cards.
            const bool hasToolbox = std::find(run.relics.begin(), run.relics.end(),
                static_cast<int>(RelicId::TOOLBOX)) != run.relics.end();
            if (hasToolbox && run.toolboxCard == 0 && !run.awaitingToolbox) {
                int pn = 0;
                const CardId* pool = sts2CombatGenPool("COLORLESS", &pn);
                std::vector<CardId> cands;
                for (int i = 0; i < pn; ++i)
                    if (BattleContext::cardImplemented(pool[i])) cands.push_back(pool[i]);
                if (!cands.empty()) {
                    run.awaitingToolbox = true;
                    run.toolboxOptions.clear();
                    Sts2Rng& r = run.stream(RngStream::CombatCardGeneration);
                    for (int k = 0; k < 3 && !cands.empty(); ++k) {
                        const int pick = r.NextInt(static_cast<int>(cands.size()));
                        run.toolboxOptions.push_back(static_cast<int>(cands[pick]));
                        cands.erase(cands.begin() + pick);
                    }
                    run.toolboxPoint = pointIdx;
                    run.toolboxSims = searchSims;
                    run.toolboxEncIdx = forced ? -1 : encIdx;
                    run.toolboxEncId = forced ? forced->id : "";
                    --run.totalFloor;
                    --run.actFloor;
                    run.visitedPoints.pop_back();
                    return true;
                }
            }
            const EncounterDef& enc = forced ? *forced : scaffoldEncounters()[encIdx];
            log.encounter = enc.id;
            const auto mons = buildMonsterList(enc, run);
            const FightResult r = fight(run, enc, mons, searchSims);
            if (!r.ok) {
                run.over = true;
                return false;
            }
            log.won = r.won;
            log.hpAfter = run.player.hp;
            log.turns = r.turns;
            log.goldReward = r.goldGained;
            log.potionReward = r.potionReward;
            log.cardChoices = r.cardChoices;
            if (!r.won || run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.rooms.push_back(log);
                return false;
            }
            if (p.type == MapPointType::Boss) {
                if (run.actIndex >= 2) {
                    // A10 DoubleBoss: after the FIRST boss only advance to the second;
                    // its pick excludes this encounter (RunManager rolls another boss).
                    for (int child : run.map.points[pointIdx].children)
                        if (run.map.points[child].type == MapPointType::Boss) {
                            run.secondBossExclude = enc.id;
                            run.current = pointIdx;
                            run.rooms.push_back(log);
                            return true;
                        }
                    // RunManager.EnterNextAct sees the last act: it enters the victory
                    // room instead of ending the run. The boss card reward stays pending
                    // (the game shows the rewards screen before the dialogue room), and
                    // PROCEED in TheArchitect is what marks the run as won.
                    run.current = pointIdx;
                    run.rooms.push_back(log);
                    startTheArchitect(run);
                    return true;
                }
                ++run.actIndex;
                beginAct(run);
                run.current = -1;
                run.rooms.push_back(log);
                // Opt-in Ancient at the act transition (Darv lives in act 2+).
                if (run.eventOverride == "DARV") startDarv(run);
                return true;
            }
            break;
        }
        case MapPointType::RestSite: {
            // HealRestSiteOption.GetBaseHealAmount = 30% of max HP. The option pick is a
            // policy decision (HEAL or SMITH) — stop here until chooseRest runs.
            run.awaitingRest = true;
            // RegalPillow.ModifyRestSiteHealAmount: +15 to the rest-site heal.
            run.restHeal = run.player.maxHp * 3 / 10;
            if (std::find(run.relics.begin(), run.relics.end(),
                          static_cast<int>(RelicId::REGAL_PILLOW)) != run.relics.end())
                run.restHeal += 15;
            run.restUpgradable.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
                    run.restUpgradable.push_back(i);
            }
            break;
        }
        case MapPointType::Treasure: {
            run.player.gold += 10 + run.upFront.NextInt(11);
            break;
        }
        case MapPointType::Shop: {
            // MerchantRoom: build the inventory on entry; purchases are policy decisions.
            generateShop(run);
            run.awaitingShop = true;
            break;
        }
        case MapPointType::Unknown: {
            // RunManager.RollRoomTypeFor -> UnknownMapPointOdds.Roll: an Unknown point is
            // 85% an event, otherwise a monster / treasure / shop room.
            // LanternKey.ModifyUnknownMapPointRoomTypes: with the quest card in the deck
            // and in the third act, every ? room is an event.
            const bool questEvent = run.actIndex == 2 && hasLanternKey(run);
            const int rolled = questEvent ? 3 : rollUnknownRoom(run);
            ++run.unknownPointsVisited;
            if (rolled == 1) { // treasure
                run.player.gold += 10 + run.upFront.NextInt(11);
                break;
            }
            if (rolled == 2) { // shop
                generateShop(run);
                run.awaitingShop = true;
                break;
            }
            if (rolled == 3) { // event
                if (run.actIndex == 2 && hasLanternKey(run)) {
                    startWarHistorianRepy(run);
                    break;
                }
                // The game rolls act event pools upfront; with two implemented events this
                // picks between them on the same UpFront stream.
                const int forcedEvent = eventIndexForName(run.eventOverride);
                switch (forcedEvent >= 0 ? forcedEvent : run.upFront.NextInt(55)) {
                    case 0: startDenseVegetation(run); break;
                    case 1: startAbyssalBaths(run); break;
                    case 2: startJungleMaze(run); break;
                    case 3: startHungryForMushrooms(run); break;
                    case 4: startSunkenStatue(run); break;
                    case 5: startRoomFullOfCheese(run); break;
                    case 6:
                        // WhisperingHollow.IsAllowed: at least 44 gold.
                        if (run.player.gold >= 44) startWhisperingHollow(run);
                        else startDenseVegetation(run);
                        break;
                    case 7:
                        // TeaMaster.IsAllowed: act < 2 and at least 150 gold.
                        if (run.actIndex < 2 && run.player.gold >= 150) startTeaMaster(run);
                        else startDenseVegetation(run);
                        break;
                    case 8:
                        // ColossalFlower.IsAllowed: at least 19 HP.
                        if (run.player.hp >= 19) startColossalFlower(run);
                        else startDenseVegetation(run);
                        break;
                    case 9:
                        // WelcomeToWongos.IsAllowed: act 2 (index 1) and at least 100 gold.
                        if (run.actIndex == 1 && run.player.gold >= 100) startWelcomeToWongos(run);
                        else startDenseVegetation(run);
                        break;
                    case 10:
                        // RoundTeaParty.IsAllowed: at least 12 HP.
                        if (run.player.hp >= 12) startRoundTeaParty(run);
                        else startDenseVegetation(run);
                        break;
                    case 11:
                        // MorphicGrove.IsAllowed: at least 100 gold and two transformable cards.
                        if (run.player.gold >= 100 && run.player.deck.size() >= 2)
                            startMorphicGrove(run);
                        else startDenseVegetation(run);
                        break;
                    case 12:
                        // ByrdonisNest.IsAllowed: no event pet (no ByrdonisEgg in the deck).
                        if (!hasByrdonisEgg(run)) startByrdonisNest(run);
                        else startDenseVegetation(run);
                        break;
                    case 13:
                        startSunkenTreasury(run);
                        break;
                    case 14:
                        startLostWisp(run);
                        break;
                    case 15:
                        startTabletOfTruth(run);
                        break;
                    case 16:
                        // PotionCourier.IsAllowed: act index > 0.
                        if (run.actIndex > 0) startPotionCourier(run);
                        else startDenseVegetation(run);
                        break;
                    case 17:
                        startThisOrThat(run);
                        break;
                    case 18:
                        startWellspring(run);
                        break;
                    case 19:
                        // LuminousChoir.IsAllowed: at least 149 gold and a relic left in the bag.
                        if (run.player.gold >= 149 && hasAnyRelic(run)) startLuminousChoir(run);
                        else startDenseVegetation(run);
                        break;
                    case 20:
                        // Amalgamator.IsAllowed: two Basic Strikes and two Basic Defends.
                        if (countBasicTag(run, CT_STRIKE) >= 2
                            && countBasicTag(run, CT_DEFEND) >= 2)
                            startAmalgamator(run);
                        else startDenseVegetation(run);
                        break;
                    case 21:
                        startReflections(run);
                        break;
                    case 22:
                        startDoorsOfLightAndDark(run);
                        break;
                    case 23:
                        // UnrestSite.IsAllowed: current HP at or below 70% of max.
                        if (run.player.hp * 100 <= run.player.maxHp * 70) startUnrestSite(run);
                        else startDenseVegetation(run);
                        break;
                    case 24:
                        // DollRoom.IsAllowed: act 2 only.
                        if (run.actIndex == 1) startDollRoom(run);
                        else startDenseVegetation(run);
                        break;
                    case 25:
                        startSapphireSeed(run);
                        break;
                    case 26: {
                        // FieldOfManSizedHoles.IsAllowed: a deck card PerfectFit can enchant.
                        bool any = false;
                        for (const RunCard& c : run.player.deck)
                            if (canEnchantCard(c, EnchantmentId::PERFECT_FIT)) { any = true; break; }
                        if (any) startFieldOfManSizedHoles(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 27:
                        startAromaOfChaos(run);
                        break;
                    case 28:
                        startSpiritGrafter(run);
                        break;
                    case 29:
                        // TrashHeap.IsAllowed: current HP above 5.
                        if (run.player.hp > 5) startTrashHeap(run);
                        else startDenseVegetation(run);
                        break;
                    case 30:
                        // RelicTrader.IsAllowed: act > 1 and at least five tradable relics.
                        if (run.actIndex > 0 && tradableRelicCount(run) >= 5)
                            startRelicTrader(run);
                        else startDenseVegetation(run);
                        break;
                    case 31: {
                        // TheFutureOfPotions.IsAllowed: at least two potions held.
                        int held = 0;
                        for (int pid : run.player.potions) if (pid != 0) ++held;
                        if (held >= 2) startTheFutureOfPotions(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 32:
                        // TheLegendsWereTrue.IsAllowed: act 1, non-empty deck, HP >= 10.
                        if (run.actIndex == 0 && !run.player.deck.empty()
                            && run.player.hp >= 10)
                            startTheLegendsWereTrue(run);
                        else startDenseVegetation(run);
                        break;
                    case 33:
                        // RanwidTheElder.IsAllowed: act > 1, a tradable relic, 100 gold,
                        // at least one potion.
                        if (run.actIndex > 0 && tradableRelicCount(run) >= 1
                            && run.player.gold >= 100 && heldPotionCount(run) >= 1)
                            startRanwidTheElder(run);
                        else startDenseVegetation(run);
                        break;
                    case 34:
                        startDrowningBeacon(run);
                        break;
                    case 35:
                        startBugslayer(run);
                        break;
                    case 36:
                        // ZenWeaver.IsAllowed: at least 125 gold.
                        if (run.player.gold >= 125) startZenWeaver(run);
                        else startDenseVegetation(run);
                        break;
                    case 37:
                        // EndlessConveyor.IsAllowed: at least 120 gold.
                        if (run.player.gold >= 120) startEndlessConveyor(run);
                        else startDenseVegetation(run);
                        break;
                    case 38:
                        startSelfHelpBook(run);
                        break;
                    case 39: {
                        // SlipperyBridge.IsAllowed: floor > 6 and a removable deck card.
                        bool removable = false;
                        for (const RunCard& c : run.player.deck)
                            if (!BattleContext::cardDef(c.id).hasFlag(CF_ETERNAL)) {
                                removable = true;
                                break;
                            }
                        if (run.totalFloor > 6 && removable) startSlipperyBridge(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 40:
                        // BrainLeech.IsAllowed: acts 1-2 only.
                        if (run.actIndex < 2) startBrainLeech(run);
                        else startDenseVegetation(run);
                        break;
                    case 41:
                        startTrial(run);
                        break;
                    case 42:
                        // WaterloggedScriptorium.IsAllowed: at least 55 gold.
                        if (run.player.gold >= 55) startWaterloggedScriptorium(run);
                        else startDenseVegetation(run);
                        break;
                    case 43: {
                        // WoodCarvings.IsAllowed: a Basic removable deck card.
                        bool basic = false;
                        for (const RunCard& c : run.player.deck)
                            if (isBasicRemovable(c)) { basic = true; break; }
                        if (basic) startWoodCarvings(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 44:
                        // PunchOff.IsAllowed: total floor >= 6.
                        if (run.totalFloor >= 6) startPunchOff(run);
                        else startDenseVegetation(run);
                        break;
                    case 45:
                        startInfestedAutomaton(run);
                        break;
                    case 46: {
                        // SpiralingWhirlpool.IsAllowed: a card Spiral can enchant.
                        bool any = false;
                        for (const RunCard& c : run.player.deck)
                            if (canEnchantCard(c, EnchantmentId::SPIRAL)) { any = true; break; }
                        if (any) startSpiralingWhirlpool(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 47:
                        // StoneOfAllTime.IsAllowed: act 2 and at least one potion.
                        if (run.actIndex == 1 && heldPotionCount(run) >= 1)
                            startStoneOfAllTime(run);
                        else startDenseVegetation(run);
                        break;
                    case 48:
                        // Symbiote.IsAllowed: act 2+.
                        if (run.actIndex > 0) startSymbiote(run);
                        else startDenseVegetation(run);
                        break;
                    case 49: {
                        // GraveOfTheForgotten.IsAllowed: an Exhaust card SoulsPower can take.
                        bool any = false;
                        for (const RunCard& c : run.player.deck)
                            if (canEnchantCard(c, EnchantmentId::SOULS_POWER)) { any = true; break; }
                        if (any) startGraveOfTheForgotten(run);
                        else startDenseVegetation(run);
                        break;
                    }
                    case 50:
                        startBattlewornDummy(run);
                        break;
                    case 51:
                        startTinkerTime(run);
                        break;
                    case 52:
                        // TheLanternKey lives in the act-2 event pool.
                        if (run.actIndex >= 1) startTheLanternKey(run);
                        else startDenseVegetation(run);
                        break;
                    case 53:
                        // FakeMerchant.IsAllowed: act 2+, 100 gold or a FoulPotion.
                        if (run.actIndex >= 1
                            && (run.player.gold >= 100 || hasFoulPotion(run)))
                            startFakeMerchant(run);
                        else startDenseVegetation(run);
                        break;
                    case 54:
                        // CrystalSphere.IsAllowed: act 2+ and at least 100 gold.
                        if (run.actIndex > 0 && run.player.gold >= 100) startCrystalSphere(run);
                        else startDenseVegetation(run);
                        break;
                    default:
                        startDenseVegetation(run);
                        break;
                }
                break;
            }
            const int encIdx = pickEncounterIndex(run, false, false);
            if (encIdx < 0) { run.over = true; return false; }
            const EncounterDef& enc = scaffoldEncounters()[encIdx];
            log.encounter = enc.id;
            const auto mons = buildMonsterList(enc, run);
            const FightResult r = fight(run, enc, mons, searchSims);
            if (!r.ok) { run.over = true; return false; }
            log.won = r.won;
            log.hpAfter = run.player.hp;
            log.turns = r.turns;
            log.goldReward = r.goldGained;
            log.potionReward = r.potionReward;
            log.cardChoices = r.cardChoices;
            if (!r.won || run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.rooms.push_back(log);
                return false;
            }
            break;
        }
        default:
            break;
    }

    log.hpAfter = run.player.hp;
    run.current = pointIdx;
    run.rooms.push_back(log);
    return true;
}

bool chooseEventCard(RunState& run, int idx) {
    if (!run.awaitingEventCards) return false;
    if (idx < 0 || idx >= static_cast<int>(run.eventCardPool.size())) return false;
    if (run.eventCardsEnchant && idx < static_cast<int>(run.eventCardDeckIdx.size())) {
        // CardCmd.Enchant: attach the event's enchantment to the chosen deck card.
        const int deckIdx = run.eventCardDeckIdx[idx];
        if (deckIdx >= 0 && deckIdx < static_cast<int>(run.player.deck.size())) {
            RunCard& c = run.player.deck[deckIdx];
            c.enchant = run.eventEnchantId;
            c.enchantAmount = run.eventEnchantAmount;
            c.enchantDisabled = false;
        }
    } else if (run.eventCardsUpgrade && idx < static_cast<int>(run.eventCardDeckIdx.size())) {
        // CardCmd.Upgrade: the chosen upgradable deck card gains one level.
        const int deckIdx = run.eventCardDeckIdx[idx];
        if (deckIdx >= 0 && deckIdx < static_cast<int>(run.player.deck.size())) {
            RunCard& c = run.player.deck[deckIdx];
            if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) ++c.upgrade;
        }
    } else if (run.eventCardsTransformTo
               && idx < static_cast<int>(run.eventCardDeckIdx.size())) {
        // CardCmd.TransformTo<T>: replace the chosen deck card with the named card.
        const int deckIdx = run.eventCardDeckIdx[idx];
        if (deckIdx >= 0 && deckIdx < static_cast<int>(run.player.deck.size())) {
            const RunCard old = run.player.deck[deckIdx];
            RunCard rep{static_cast<CardId>(run.eventTransformToId), 0};
            if (run.eventTransformKeep) {
                // Claws.CreateMaulFromOriginal: keep the upgrade and the enchantment
                // when the replacement card can carry them.
                const CardDef& nd = BattleContext::cardDef(rep.id);
                if (old.upgrade > 0) rep.upgrade = std::min(old.upgrade, nd.maxUpgradeLevel);
                if (old.enchant != 0) {
                    rep.enchant = old.enchant;
                    rep.enchantAmount = old.enchantAmount;
                }
            }
            run.player.deck[deckIdx] = rep;
        }
    } else if (run.eventCardsRemove && idx < static_cast<int>(run.eventCardDeckIdx.size())) {
        // CardPileCmd.RemoveFromDeck: delete the chosen card; later deck indices shift.
        const int deckIdx = run.eventCardDeckIdx[idx];
        if (deckIdx >= 0 && deckIdx < static_cast<int>(run.player.deck.size())) {
            run.player.deck.erase(run.player.deck.begin() + deckIdx);
            for (int k = 0; k < static_cast<int>(run.eventCardDeckIdx.size()); ++k)
                if (run.eventCardDeckIdx[k] > deckIdx) --run.eventCardDeckIdx[k];
        }
    } else if (run.eventCardsTransform && idx < static_cast<int>(run.eventCardDeckIdx.size())) {
        // CardCmd.TransformToRandom: swap the deck card for a random character-pool card
        // (engine-implemented cards only), drawn on the event's own Rng.
        const int deckIdx = run.eventCardDeckIdx[idx];
        const int oldId = run.eventCardPool[idx].first;
        int n = 0;
        const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
        std::vector<int> cand;
        for (int i = 0; i < n; ++i)
            if (static_cast<int>(pool[i]) != oldId && BattleContext::cardImplemented(pool[i]))
                cand.push_back(static_cast<int>(pool[i]));
        if (!cand.empty() && deckIdx >= 0 && deckIdx < static_cast<int>(run.player.deck.size())) {
            const int pick = cand[run.eventRng.NextInt(static_cast<int>(cand.size()))];
            // Astrolabe transforms into an already-upgraded card.
            run.player.deck[deckIdx] = {static_cast<CardId>(pick),
                                        run.eventTransformUpgrade ? 1 : 0};
        }
    } else {
        run.player.deck.push_back({static_cast<CardId>(run.eventCardPool[idx].first),
                                   run.eventCardPool[idx].second});
    }
    run.eventCardPool.erase(run.eventCardPool.begin() + idx);
    if (idx < static_cast<int>(run.eventCardDeckIdx.size()))
        run.eventCardDeckIdx.erase(run.eventCardDeckIdx.begin() + idx);
    if (--run.eventCardsLeft <= 0) {
        run.awaitingEventCards = false;
        run.eventCardPool.clear();
        run.eventCardDeckIdx.clear();
        run.eventCardsTransform = false;
        run.eventCardsRemove = false;
        run.eventCardsEnchant = false;
        run.eventCardsUpgrade = false;
        run.eventCardsTransformTo = false;
        run.eventTransformUpgrade = false;
        run.eventTransformKeep = false;
        // Events that resolve damage after their card selection (WhisperingHollow HUG,
        // SpiritGrafter REJECTION) pay it when the grid closes.
        if (run.eventPendingHpLoss > 0) {
            run.player.hp = std::max(0, run.player.hp - run.eventPendingHpLoss);
            run.eventPendingHpLoss = 0;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
        }
        // ZenWeaver charges its gold after the removal selection.
        if (run.eventPendingGoldCost > 0) {
            run.player.gold = std::max(0, run.player.gold - run.eventPendingGoldCost);
            run.eventPendingGoldCost = 0;
        }
        // EndlessConveyor's JellyLiver: the belt resumes after the transform pick.
        if (run.eventConveyorResume) {
            run.eventConveyorResume = false;
            rollConveyorDish(run);
            setConveyorOptions(run, /*first=*/false);
            run.awaitingEvent = true;
        }
    }
    return true;
}

std::string previewBattleStamp(RunState& run, const std::string& encounterId) {
    const EncounterDef* enc = findEncounter(encounterId);
    if (!enc) return std::string();
    BattleContext bc;
    const auto mons = buildMonsterList(*enc, run);
    if (!buildBattle(run, *enc, mons, bc)) return std::string();
    return emitStamp(bc);
}

bool chooseCardReward(RunState& run, int idx) {
    if (!run.awaitingCardReward) return false;
    if (idx >= 0 && idx < static_cast<int>(run.cardOffer.size()))
        run.player.deck.push_back({static_cast<CardId>(run.cardOffer[idx].first),
                                   run.cardOffer[idx].second});
    run.cardOffer.clear();
    // Trial (nondescript guilty) offers two card rewards in sequence.
    if (run.pendingSecondCardReward) {
        run.pendingSecondCardReward = false;
        run.cardOffer = run.cardOffer2;
        run.cardOffer2.clear();
        run.awaitingCardReward = !run.cardOffer.empty();
    } else if (!run.extraCardOffers.empty()) {
        run.cardOffer = std::move(run.extraCardOffers.front());
        run.extraCardOffers.erase(run.extraCardOffers.begin());
        run.awaitingCardReward = !run.cardOffer.empty();
    } else {
        run.awaitingCardReward = false;
    }
    return true;
}

bool choosePotionReward(RunState& run, bool take) {
    if (!run.awaitingPotionReward) return false;
    if (take && run.potionRewardId) grantPotionById(run, run.potionRewardId);
    run.awaitingPotionReward = false;
    run.potionRewardId = 0;
    return true;
}

bool chooseRelicReward(RunState& run, bool take) {
    if (!run.awaitingRelicReward) return false;
    if (take && run.relicRewardId) run.relics.push_back(run.relicRewardId);
    run.awaitingRelicReward = false;
    run.relicRewardId = 0;
    return true;
}

// CrystalSphere.CellClicked: one divination is spent per click. The big tool clears the
// eight neighbours plus the cell itself (source order: horizontal, vertical, diagonal,
// self); the small tool clears only the clicked cell.
bool chooseSphereCell(RunState& run, int x, int y, int tool) {
    if (run.eventId != "CRYSTAL_SPHERE" || run.eventPage != 1) return false;
    if (run.eventSphereDivinations <= 0) return false;
    if (x < 0 || x >= kSphereSide || y < 0 || y >= kSphereSide) return false;
    if (tool == 1 || tool == 2) run.eventSphereTool = tool;
    --run.eventSphereDivinations;
    if (run.eventSphereTool != 2) {
        clearSphereCell(run, x, y);
    } else {
        for (int i = -1; i <= 1; i += 2) {
            const int nx = x + i;
            if (nx >= 0 && nx < kSphereSide) clearSphereCell(run, nx, y);
            const int ny = y + i;
            if (ny >= 0 && ny < kSphereSide) clearSphereCell(run, x, ny);
        }
        for (int dy = -1; dy <= 1; dy += 2)
            for (int dx = -1; dx <= 1; dx += 2) {
                const int nx = x + dx, ny = y + dy;
                if (nx >= 0 && nx < kSphereSide && ny >= 0 && ny < kSphereSide)
                    clearSphereCell(run, nx, ny);
            }
        clearSphereCell(run, x, y);
    }
    if (run.eventSphereDivinations == 0) finishCrystalSphere(run);
    return true;
}

bool chooseRest(RunState& run, const char* option, int deckIdx) {
    if (!run.awaitingRest) return false;
    const std::string opt = option ? option : "HEAL";
    if (opt == "SMITH") {
        if (deckIdx < 0 || deckIdx >= static_cast<int>(run.player.deck.size())) return false;
        RunCard& c = run.player.deck[deckIdx];
        if (c.upgrade >= BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
            return false;
        ++c.upgrade;
    } else if (opt == "HEAL") {
        run.player.hp = std::min(run.player.maxHp, run.player.hp + run.restHeal);
        // TinyMailbox.TryModifyRestSiteHealRewards: two potion rewards on a rest heal.
        if (std::find(run.relics.begin(), run.relics.end(),
                      static_cast<int>(RelicId::TINY_MAILBOX)) != run.relics.end()) {
            grantPotionReward(run);
            grantPotionReward(run);
        }
        // VenerableTeaSet.AfterRestSiteHeal: +1 energy in the next combat.
        if (std::find(run.relics.begin(), run.relics.end(),
                      static_cast<int>(RelicId::VENERABLE_TEA_SET)) != run.relics.end())
            run.venerableTeaEnergy = true;
    } else {
        return false;
    }
    run.awaitingRest = false;
    run.restUpgradable.clear();
    return true;
}

bool chooseToolbox(RunState& run, int idx) {
    if (!run.awaitingToolbox) return false;
    if (idx < 0 || idx >= static_cast<int>(run.toolboxOptions.size())) return false;
    run.toolboxCard = run.toolboxOptions[idx];
    run.awaitingToolbox = false;
    const int point = run.toolboxPoint;
    const long long sims = run.toolboxSims;
    run.toolboxOptions.clear();
    run.toolboxPoint = -1;
    run.toolboxResumePoint = point;
    return enterPoint(run, point, sims);
}

bool chooseEventOption(RunState& run, int idx, long long searchSims) {
    if (!run.awaitingEvent) return false;
    if (idx < 0 || idx >= static_cast<int>(run.eventOptions.size())) return false;
    const std::string opt = run.eventOptions[idx];
    if (run.eventId == "THE_ARCHITECT") {
        // AdvanceDialogue: lines 0..2; the last line's option is PROCEED (WinRun).
        if (run.eventPage < 2) {
            if (opt != "CONTINUE") return false;
            ++run.eventPage;
            run.eventOptions = {run.eventPage == 2 ? "PROCEED" : "CONTINUE"};
            return true;
        }
        if (opt != "PROCEED") return false;
        run.awaitingEvent = false;
        run.over = true;
        run.won = true;
        return true;
    }
    if (run.eventId == "PAEL") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        paelGrant(run, relic);
        return true;
    }
    if (run.eventId == "NONUPEIPE") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        nonupeipeGrant(run, relic);
        return true;
    }
    if (run.eventId == "TANX") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        tanxGrant(run, relic);
        return true;
    }
    if (run.eventId == "TEZCATARA") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        tezcataraGrant(run, relic);
        return true;
    }
    if (run.eventId == "VAKUU") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        vakuuGrant(run, relic);
        return true;
    }
    if (run.eventId == "OROBAS") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        orobasGrant(run, relic);
        return true;
    }
    if (run.eventId == "DARV") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        darvGrant(run, relic);
        if (relic == static_cast<int>(RelicId::EMPTY_CAGE))
            neowOpenCardGrid(run, true, false, false, 2);
        else if (relic == static_cast<int>(RelicId::ASTROLABE)) {
            neowOpenCardGrid(run, false, false, true, 3);
            run.eventTransformUpgrade = true;
        }
        return true;
    }
    if (run.eventId == "NEOW") {
        if (idx >= static_cast<int>(run.eventRelicOptions.size())) return false;
        const int relic = run.eventRelicOptions[idx];
        run.awaitingEvent = false;
        neowGrant(run, relic);
        if (relic == static_cast<int>(RelicId::NEW_LEAF))
            neowOpenCardGrid(run, false, false, true, 1);
        else if (relic == static_cast<int>(RelicId::PRECISE_SCISSORS))
            neowOpenCardGrid(run, true, false, false, 1);
        else if (relic == static_cast<int>(RelicId::POMANDER))
            neowOpenCardGrid(run, false, true, false, 1);
        else if (relic == static_cast<int>(RelicId::PRECARIOUS_SHEARS)) {
            neowOpenCardGrid(run, true, false, false, 2);
            run.eventPendingHpLoss = 16;
        }
        return true;
    }
    if (run.eventId == "SAPPHIRE_SEED") {
        if (opt == "EAT") {
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsUpgrade = true;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsTransform = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "PLANT") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::SOWN)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsTransform = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "FIELD_OF_MAN_SIZED_HOLES") {
        if (opt == "RESIST") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            // The curse is appended after the grid snapshot so it cannot be selected.
            run.player.deck.push_back({CardId::NORMALITY, 0});
            run.eventCardsRemove = true;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsTransform = false;
            run.eventCardsLeft = std::min(2, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "ENTER_YOUR_HOLE") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::PERFECT_FIT)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsTransform = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "WAR_HISTORIAN_REPY") {
        if (opt != "UNLOCK_CAGE" && opt != "UNLOCK_CHEST") return false;
        const bool second = run.eventPage == 1;
        // Initial choice removes one LanternKey; the follow-up removes every one left.
        for (auto it = run.player.deck.begin(); it != run.player.deck.end();) {
            if (it->id == CardId::LANTERN_KEY) {
                it = run.player.deck.erase(it);
                if (!second) break;
            } else {
                ++it;
            }
        }
        if (opt == "UNLOCK_CAGE") {
            run.relics.push_back(static_cast<int>(RelicId::HISTORY_COURSE));
        } else {
            grantPotionReward(run);
            grantPotionReward(run);
            for (int i = 0; i < 2; ++i) {
                const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
                if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            }
        }
        const bool more = hasLanternKey(run);
        run.awaitingEvent = false;
        if (more) {
            run.eventPage = 1;
            run.eventOptions = {opt == "UNLOCK_CAGE" ? "UNLOCK_CHEST" : "UNLOCK_CAGE"};
            run.awaitingEvent = true;
        }
        return true;
    }
    if (run.eventId == "FAKE_MERCHANT") {
        if (opt == "LEAVE") {
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "THROW_POTION") {
            if (!hasFoulPotion(run)) return false;
            for (auto& slot : run.player.potions)
                if (slot == static_cast<int>(PotionId::FOUL_POTION)) { slot = 0; break; }
            const EncounterDef* enc = findEncounter("FAKE_MERCHANT_EVENT");
            if (!enc) return false;
            const auto mons = buildMonsterList(*enc, run);
            const FightResult r = fight(run, *enc, mons, searchSims);
            run.awaitingEvent = false;
            if (!r.ok) { run.over = true; return false; }
            if (!r.won || run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                return true;
            }
            run.relics.push_back(static_cast<int>(RelicId::FAKE_MERCHANTS_RUG));
            for (int id : run.eventShopRelics)
                if (id >= 0) run.relics.push_back(id);
            return true;
        }
        if (opt.rfind("BUY_", 0) == 0) {
            const int k = std::atoi(opt.c_str() + 4);
            if (k < 0 || k >= static_cast<int>(run.eventShopRelics.size())) return false;
            if (run.eventShopRelics[k] < 0) return false;
            if (run.player.gold < 50) return false;
            run.player.gold -= 50;
            run.relics.push_back(run.eventShopRelics[k]);
            run.eventShopRelics[k] = -1;
            refreshFakeMerchantOptions(run);
            return true;
        }
        return false;
    }
    if (run.eventId == "THE_LANTERN_KEY") {
        if (opt == "RETURN_THE_KEY") {
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "KEEP_THE_KEY") {
            run.eventPage = 1;
            run.eventOptions = {"FIGHT"};
            return true;
        }
        if (opt == "FIGHT" && run.eventPage == 1) {
            const EncounterDef* enc = findEncounter("MYSTERIOUS_KNIGHT_EVENT");
            if (!enc) return false;
            const auto mons = buildMonsterList(*enc, run);
            const FightResult r = fight(run, *enc, mons, searchSims);
            run.awaitingEvent = false;
            if (!r.ok) { run.over = true; return false; }
            if (!r.won || run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                return true;
            }
            // SpecialCardReward: the LanternKey quest card joins the deck.
            run.player.deck.push_back({CardId::LANTERN_KEY, 0});
            return true;
        }
        return false;
    }
    if (run.eventId == "TINKER_TIME") {
        if (opt == "CHOOSE_CARD_TYPE" && run.eventPage == 0) {
            std::vector<int> types = {0, 1, 2};
            unstableShuffleIdx(types, run.eventRng);
            run.eventOptions.clear();
            for (int k = 0; k < 2; ++k) run.eventOptions.push_back("TYPE_" + std::to_string(types[k]));
            run.eventPage = 1;
            return true;
        }
        if (opt.rfind("TYPE_", 0) == 0 && run.eventPage == 1) {
            const int ty = std::atoi(opt.c_str() + 5);
            if (ty < 0 || ty > 2) return false;
            run.eventTinkerType = ty;
            std::vector<int> riders(kTinkerRiders[ty], kTinkerRiders[ty] + 3);
            unstableShuffleIdx(riders, run.eventRng);
            run.eventTinkerRiders = {riders[0], riders[1]};
            run.eventOptions = {"RIDER_" + std::to_string(riders[0]),
                                "RIDER_" + std::to_string(riders[1])};
            run.eventPage = 2;
            return true;
        }
        if (opt.rfind("RIDER_", 0) == 0 && run.eventPage == 2) {
            const int rd = std::atoi(opt.c_str() + 6);
            bool offered = false;
            for (int r : run.eventTinkerRiders) if (r == rd) offered = true;
            if (!offered) return false;
            RunCard c;
            c.id = CardId::MAD_SCIENCE;
            c.upgrade = 0;
            c.tinkerType = run.eventTinkerType;
            c.tinkerRider = rd;
            run.player.deck.push_back(c);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "CRYSTAL_SPHERE") {
        if (run.eventPage == 0 && opt == "UNCOVER_FUTURE") {
            if (run.player.gold < run.eventGold) return false;
            run.player.gold -= run.eventGold;
            beginCrystalSphereMinigame(run, 3);
            return true;
        }
        if (run.eventPage == 0 && opt == "PAYMENT_PLAN") {
            run.player.deck.push_back({CardId::DEBT, 0});
            beginCrystalSphereMinigame(run, 6);
            return true;
        }
        return false;
    }
    if (run.eventId == "BATTLEWORN_DUMMY") {
        int setting = opt == "SETTING_1" ? 1 : (opt == "SETTING_2" ? 2
                    : (opt == "SETTING_3" ? 3 : 0));
        if (setting == 0) return false;
        const char* encId = setting == 1 ? "BATTLEWORN_DUMMY_V1"
                          : (setting == 2 ? "BATTLEWORN_DUMMY_V2" : "BATTLEWORN_DUMMY_V3");
        const EncounterDef* enc = findEncounter(encId);
        if (!enc) return false;
        const auto mons = buildMonsterList(*enc, run);
        const FightResult r = fight(run, *enc, mons, searchSims);
        run.awaitingEvent = false;
        if (!r.ok) { run.over = true; return false; }
        if (r.timedOut) return true; // dummy escaped: the event ends with no reward
        if (!r.won || run.player.hp <= 0) {
            run.over = true;
            run.won = false;
            return true;
        }
        if (setting == 1) {
            grantPotionReward(run);
        } else if (setting == 2) {
            // Two random upgradable cards (StableShuffle on the event Rng).
            std::vector<int> up;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) up.push_back(i);
            }
            std::sort(up.begin(), up.end(), [&run](int a, int b) {
                if (run.player.deck[a].id != run.player.deck[b].id)
                    return static_cast<int>(run.player.deck[a].id)
                         < static_cast<int>(run.player.deck[b].id);
                return run.player.deck[a].upgrade < run.player.deck[b].upgrade;
            });
            unstableShuffleIdx(up, run.eventRng);
            for (int k = 0; k < 2 && k < static_cast<int>(up.size()); ++k)
                ++run.player.deck[up[k]].upgrade;
        } else {
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
        }
        return true;
    }
    if (run.eventId == "GRAVE_OF_THE_FORGOTTEN") {
        if (opt == "ACCEPT") {
            run.relics.push_back(static_cast<int>(RelicId::FORGOTTEN_SOUL));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "CONFRONT") {
            run.player.deck.push_back({CardId::DECAY, 0});
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::SOULS_POWER)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::SOULS_POWER);
            run.eventEnchantAmount = 1;
            run.eventCardsTransformTo = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "SYMBIOTE") {
        if (opt == "KILL_WITH_FIRE") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransform = true;
            run.eventCardsTransformTo = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "APPROACH") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::CORRUPTED)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::CORRUPTED);
            run.eventEnchantAmount = 1;
            run.eventCardsTransformTo = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "STONE_OF_ALL_TIME") {
        if (opt == "LIFT") {
            const int slot = run.eventPotionSlot;
            if (slot < 0 || slot >= 3 || run.player.potions[slot] == 0) return false;
            run.player.potions[slot] = 0; // PotionCmd.Discard
            run.player.maxHp += 10;       // CreatureCmd.GainMaxHp(10)
            run.player.hp = std::min(run.player.maxHp, run.player.hp + 10);
            run.eventRng.NextInt(100);    // flavor roll (source consumes it)
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "PUSH") {
            run.player.hp = std::max(0, run.player.hp - 6);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::VIGOROUS)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::VIGOROUS);
            run.eventEnchantAmount = 8;
            run.eventCardsTransformTo = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            run.eventRng.NextInt(100); // flavor roll (source consumes it)
            return true;
        }
        return false;
    }
    if (run.eventId == "SPIRALING_WHIRLPOOL") {
        if (opt == "DRINK") {
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "OBSERVE") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::SPIRAL)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::SPIRAL);
            run.eventEnchantAmount = 1;
            run.eventCardsTransformTo = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "INFESTED_AUTOMATON") {
        const int filterKind = opt == "STUDY" ? 1 : (opt == "TOUCH_CORE" ? 2 : 0);
        if (filterKind == 0) return false;
        int n = 0;
        const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
        const std::vector<int> cards = defaultOddsCards(run, pool, n, 1, filterKind);
        if (!cards.empty()) run.player.deck.push_back({static_cast<CardId>(cards[0]), 0});
        run.awaitingEvent = false;
        return true;
    }
    if (run.eventId == "PUNCH_OFF") {
        if (opt == "NAB") {
            run.player.deck.push_back({CardId::INJURY, 0});
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "I_CAN_TAKE_THEM") {
            run.eventPage = 1;
            run.eventOptions = {"FIGHT"};
            return true;
        }
        if (opt == "FIGHT" && run.eventPage == 1) {
            const EncounterDef* enc = findEncounter("PUNCH_OFF_ENCOUNTER");
            if (!enc) return false;
            const auto mons = buildMonsterList(*enc, run);
            const FightResult r = fight(run, *enc, mons, searchSims);
            run.awaitingEvent = false;
            if (!r.ok) { run.over = true; return false; }
            if (!r.won || run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                return true;
            }
            // Extra rewards on top of the room's own: one relic and one potion.
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            grantPotionReward(run);
            return true;
        }
        return false;
    }
    if (run.eventId == "WOOD_CARVINGS") {
        if (opt == "BIRD" || opt == "TORUS") {
            const int targetId = opt == "BIRD" ? static_cast<int>(CardId::PECK)
                                               : static_cast<int>(CardId::TORIC_TOUGHNESS);
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (isBasicRemovable(c)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsTransformTo = true;
            run.eventTransformToId = targetId;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "SNAKE") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::SLITHER)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventEnchantId = static_cast<int>(EnchantmentId::SLITHER);
            run.eventEnchantAmount = 1;
            run.eventCardsTransformTo = false;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "WATERLOGGED_SCRIPTORIUM") {
        if (opt == "BLOODY_INK") {
            run.player.maxHp += 6; // CreatureCmd.GainMaxHp(6)
            run.player.hp = std::min(run.player.maxHp, run.player.hp + 6);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "TENTACLE_QUILL" || opt == "PRICKLY_SPONGE") {
            const int cost = opt == "TENTACLE_QUILL" ? 55 : 99;
            const int count = opt == "TENTACLE_QUILL" ? 1 : 2;
            if (run.player.gold < cost) return false;
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (canEnchantCard(c, EnchantmentId::STEADY)) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsEnchant = true;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsTransform = false;
            run.eventEnchantId = static_cast<int>(EnchantmentId::STEADY);
            run.eventEnchantAmount = 1;
            run.eventCardsLeft = std::min(count, static_cast<int>(run.eventCardPool.size()));
            run.eventPendingGoldCost = cost;
            run.awaitingEvent = false;
            if (run.eventCardPool.empty()) {
                run.player.gold = std::max(0, run.player.gold - cost);
                run.eventPendingGoldCost = 0;
                return true;
            }
            run.awaitingEventCards = true;
            return true;
        }
        return false;
    }
    if (run.eventId == "TRIAL") {
        if (opt == "ACCEPT") {
            run.eventTrialCase = run.eventRng.NextInt(3);
            run.eventPage = 1;
            run.eventOptions = {"GUILTY", "INNOCENT"};
            return true;
        }
        if (opt == "REJECT") {
            run.eventPage = 2;
            run.eventOptions = {"ACCEPT", "DOUBLE_DOWN"};
            return true;
        }
        if (opt == "DOUBLE_DOWN") { // abandons the run
            run.player.hp = 0;
            run.over = true;
            run.won = false;
            run.awaitingEvent = false;
            return true;
        }
        const bool guilty = opt == "GUILTY";
        if (!guilty && opt != "INNOCENT") return false;
        if (run.eventTrialCase == 0) { // merchant
            if (guilty) {
                run.player.deck.push_back({CardId::REGRET, 0});
                for (int i = 0; i < 2; ++i) {
                    const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
                    if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
                }
                run.awaitingEvent = false;
                return true;
            }
            run.player.deck.push_back({CardId::SHAME, 0});
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsUpgrade = true;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsLeft = std::min(2, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (run.eventTrialCase == 1) { // noble
            if (guilty) {
                run.player.hp = std::min(run.player.maxHp, run.player.hp + 10);
            } else {
                run.player.deck.push_back({CardId::REGRET, 0});
                run.player.gold += 300;
            }
            run.awaitingEvent = false;
            return true;
        }
        // nondescript
        run.player.deck.push_back({CardId::DOUBT, 0});
        if (guilty) {
            int n = 0;
            const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
            const std::vector<int> first = defaultOddsCards(run, pool, n, 3);
            const std::vector<int> second = defaultOddsCards(run, pool, n, 3);
            run.cardOffer.clear();
            for (int cid : first) run.cardOffer.push_back({cid, 0});
            run.cardOffer2.clear();
            for (int cid : second) run.cardOffer2.push_back({cid, 0});
            run.pendingSecondCardReward = !run.cardOffer2.empty();
            run.awaitingCardReward = !run.cardOffer.empty();
            run.awaitingEvent = false;
            return true;
        }
        run.eventCardPool.clear();
        run.eventCardDeckIdx.clear();
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                         run.player.deck[i].upgrade});
            run.eventCardDeckIdx.push_back(i);
        }
        run.eventCardsTransform = true;
        run.eventCardsRemove = false;
        run.eventCardsUpgrade = false;
        run.eventCardsEnchant = false;
        run.eventCardsLeft = std::min(2, static_cast<int>(run.eventCardPool.size()));
        run.awaitingEvent = false;
        run.awaitingEventCards = !run.eventCardPool.empty();
        return true;
    }
    if (run.eventId == "BRAIN_LEECH") {
        if (opt == "SHARE_KNOWLEDGE") {
            int n = 0;
            const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
            const std::vector<int> cards = defaultOddsCards(run, pool, n, 5);
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int cid : cards) run.eventCardPool.push_back({cid, 0});
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "RIP") {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            int n = 0;
            const CardId* pool = sts2CombatGenPool("COLORLESS", &n);
            const std::vector<int> cards = defaultOddsCards(run, pool, n, 3);
            run.cardOffer.clear();
            for (int cid : cards) run.cardOffer.push_back({cid, 0});
            run.awaitingCardReward = !run.cardOffer.empty();
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "SLIPPERY_BRIDGE") {
        if (opt == "OVERCOME") {
            if (run.eventBridgeCardIdx >= 0
                && run.eventBridgeCardIdx < static_cast<int>(run.player.deck.size()))
                run.player.deck.erase(run.player.deck.begin() + run.eventBridgeCardIdx);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "HOLD_ON") {
            const int dmg = 3 + run.eventBridgeHoldOns;
            run.player.hp = std::max(0, run.player.hp - dmg);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            ++run.eventBridgeHoldOns;
            run.eventPage = run.eventBridgeHoldOns;
            pickBridgeCard(run);
            return true;
        }
        return false;
    }
    if (run.eventId == "SELF_HELP_BOOK") {
        if (opt == "NO_OPTIONS") {
            run.awaitingEvent = false;
            return true;
        }
        EnchantmentId e = EnchantmentId::NONE;
        CardType wantType = CardType::NONE;
        if (opt == "READ_THE_BACK") { e = EnchantmentId::SHARP; wantType = CardType::ATTACK; }
        else if (opt == "READ_PASSAGE") { e = EnchantmentId::NIMBLE; wantType = CardType::SKILL; }
        else if (opt == "READ_ENTIRE_BOOK") { e = EnchantmentId::SWIFT; wantType = CardType::POWER; }
        else return false;
        run.eventCardPool.clear();
        run.eventCardDeckIdx.clear();
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            const RunCard& c = run.player.deck[i];
            if (BattleContext::cardDef(c.id).type == wantType && canEnchantCard(c, e)) {
                run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
        }
        run.eventCardsEnchant = true;
        run.eventCardsRemove = false;
        run.eventCardsUpgrade = false;
        run.eventCardsTransform = false;
        run.eventEnchantId = static_cast<int>(e);
        run.eventEnchantAmount = 2;
        run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
        run.awaitingEvent = false;
        run.awaitingEventCards = !run.eventCardPool.empty();
        return true;
    }
    if (run.eventId == "ENDLESS_CONVEYOR") {
        if (opt == "OBSERVE_CHEF") {
            std::vector<int> up;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) up.push_back(i);
            }
            if (!up.empty())
                ++run.player.deck[up[run.eventRng.NextInt(static_cast<int>(up.size()))]].upgrade;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "LEAVE") {
            run.awaitingEvent = false;
            return true;
        }
        if (opt != run.eventConveyorDish) return false;
        if (run.player.gold < 40) return false; // locked in-game
        if (opt != "GOLDEN_FYSH") run.player.gold -= 40;
        const bool deferred = applyConveyorDish(run, opt);
        if (deferred) {
            run.awaitingEvent = false; // the grid resumes the belt when it closes
            return true;
        }
        rollConveyorDish(run);
        setConveyorOptions(run, /*first=*/false);
        run.awaitingEvent = true;
        return true;
    }
    if (run.eventId == "ZEN_WEAVER") {
        if (opt == "BREATHING_TECHNIQUES") {
            if (run.player.gold < 50) return false;
            run.player.gold -= 50;
            run.player.deck.push_back({CardId::ENLIGHTENMENT, 0});
            run.player.deck.push_back({CardId::ENLIGHTENMENT, 0});
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "EMOTIONAL_AWARENESS" || opt == "ARACHNID_ACUPUNCTURE") {
            const int cost = opt == "EMOTIONAL_AWARENESS" ? 125 : 250;
            const int count = opt == "EMOTIONAL_AWARENESS" ? 1 : 2;
            if (run.player.gold < cost) return false;
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsRemove = true;
            run.eventCardsTransform = false;
            run.eventCardsEnchant = false;
            run.eventCardsUpgrade = false;
            run.eventCardsLeft = std::min(count, static_cast<int>(run.eventCardPool.size()));
            run.eventPendingGoldCost = cost;
            run.awaitingEvent = false;
            if (run.eventCardPool.empty()) {
                run.player.gold = std::max(0, run.player.gold - cost);
                run.eventPendingGoldCost = 0;
                return true;
            }
            run.awaitingEventCards = true;
            return true;
        }
        return false;
    }
    if (run.eventId == "BUGSLAYER") {
        if (opt == "EXTERMINATION") {
            run.player.deck.push_back({CardId::EXTERMINATE, 0});
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "SQUASH") {
            run.player.deck.push_back({CardId::SQUASH, 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "DROWNING_BEACON") {
        if (opt == "BOTTLE") {
            grantPotionById(run, static_cast<int>(PotionId::GLOWWATER_POTION));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "CLIMB") {
            // CreatureCmd.LoseMaxHp(13): max HP drops, current HP clamped to the new cap.
            run.player.maxHp = std::max(1, run.player.maxHp - run.eventMaxHp);
            if (run.player.hp > run.player.maxHp) run.player.hp = run.player.maxHp;
            run.relics.push_back(static_cast<int>(RelicId::FRESNEL_LENS));
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "THE_LEGENDS_WERE_TRUE") {
        if (opt == "NAB_THE_MAP") {
            run.player.deck.push_back({CardId::SPOILS_MAP, 0});
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "SLOWLY_FIND_AN_EXIT") {
            run.player.hp = std::max(0, run.player.hp - run.eventDamage);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            // One potion from GetPotionOptions on the Rewards stream.
            const std::vector<int> options = freshSavePotionOptions();
            if (!options.empty())
                grantPotionById(run, options[run.rewards.NextInt(static_cast<int>(options.size()))]);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "RANWID_THE_ELDER") {
        if (opt == "POTION") {
            const int slot = run.eventPotionSlot;
            if (slot < 0 || slot >= 3 || run.player.potions[slot] == 0) return false;
            run.player.potions[slot] = 0; // PotionCmd.Discard
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "GOLD") {
            if (run.player.gold < run.eventGold) return false;
            run.player.gold -= run.eventGold;
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "RELIC") {
            const int owned = run.eventPickRelicId;
            if (owned == 0) return false;
            for (auto it = run.relics.begin(); it != run.relics.end(); ++it)
                if (*it == owned) { run.relics.erase(it); break; }
            for (int i = 0; i < 2; ++i) {
                const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
                if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            }
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "THE_FUTURE_OF_POTIONS") {
        if (opt.rfind("POTION_", 0) != 0) return false;
        const int slot = std::atoi(opt.c_str() + 7);
        if (slot < 0 || slot >= 3 || run.player.potions[slot] == 0) return false;
        int oi = -1;
        for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i)
            if (run.eventOptions[i] == opt) { oi = i; break; }
        const int type = (oi >= 0 && oi < static_cast<int>(run.eventPotionTypes.size()))
                       ? run.eventPotionTypes[oi] : static_cast<int>(CardType::ATTACK);
        const int pid = run.player.potions[slot];
        const PotionRarity pr = sts2PotionDef(static_cast<PotionId>(pid)).rarity;
        const CardRarity want = (pr == PotionRarity::RARE || pr == PotionRarity::EVENT)
                              ? CardRarity::RARE
                              : (pr == PotionRarity::UNCOMMON ? CardRarity::UNCOMMON
                                                              : CardRarity::COMMON);
        run.player.potions[slot] = 0; // PotionCmd.Discard
        // CardReward(3) over the rarity/type-filtered character pool, uniform on Rewards,
        // distinct picks; every offered card arrives upgraded.
        int n = 0;
        const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
        std::vector<int> cand;
        for (int i = 0; i < n; ++i) {
            const CardDef& d = BattleContext::cardDef(pool[i]);
            if (d.rarity == want && static_cast<int>(d.type) == type
                && BattleContext::cardImplemented(pool[i]))
                cand.push_back(static_cast<int>(pool[i]));
        }
        run.cardOffer.clear();
        for (int k = 0; k < 3 && !cand.empty(); ++k) {
            const int pick = cand[run.rewards.NextInt(static_cast<int>(cand.size()))];
            const CardDef& d = BattleContext::cardDef(static_cast<CardId>(pick));
            run.cardOffer.push_back({pick, d.maxUpgradeLevel > 0 ? 1 : 0});
            cand.erase(std::find(cand.begin(), cand.end(), pick));
        }
        run.awaitingCardReward = !run.cardOffer.empty();
        run.awaitingEvent = false;
        return true;
    }
    if (run.eventId == "RELIC_TRADER") {
        if (opt == "PROCEED") {
            run.awaitingEvent = false;
            return true;
        }
        const int idx = opt == "TOP" ? 0 : (opt == "MIDDLE" ? 1 : (opt == "BOTTOM" ? 2 : -1));
        if (idx < 0 || idx >= static_cast<int>(run.eventOwnedRelics.size())) return false;
        const int owned = run.eventOwnedRelics[idx];
        for (auto it = run.relics.begin(); it != run.relics.end(); ++it)
            if (*it == owned) { run.relics.erase(it); break; }
        run.relics.push_back(run.eventNewRelics[idx]);
        run.awaitingEvent = false;
        return true;
    }
    if (run.eventId == "TRASH_HEAP") {
        if (opt == "DIVE_IN") {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            run.relics.push_back(kTrashRelics[run.eventRng.NextInt(5)]);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "GRAB") {
            run.player.gold += run.eventGold;
            const int cid = kTrashCards[run.eventRng.NextInt(10)];
            run.player.deck.push_back({static_cast<CardId>(cid), 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "SPIRIT_GRAFTER") {
        if (opt == "LET_IT_IN") {
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.player.deck.push_back({CardId::METAMORPHOSIS, 0});
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "REJECTION") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.awaitingEvent = false;
            if (run.eventCardPool.empty()) {
                run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
                if (run.player.hp <= 0) { run.over = true; run.won = false; }
                return true;
            }
            run.eventCardsUpgrade = true;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsLeft = 1;
            run.eventPendingHpLoss = run.eventHpLoss;
            run.awaitingEventCards = true;
            return true;
        }
        return false;
    }
    if (run.eventId == "AROMA_OF_CHAOS") {
        if (opt == "LET_GO") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransform = true;
            run.eventCardsRemove = false;
            run.eventCardsUpgrade = false;
            run.eventCardsEnchant = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "MAINTAIN_CONTROL") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(c.id).maxUpgradeLevel) {
                    run.eventCardPool.push_back({static_cast<int>(c.id), c.upgrade});
                    run.eventCardDeckIdx.push_back(i);
                }
            }
            run.eventCardsUpgrade = true;
            run.eventCardsTransform = false;
            run.eventCardsRemove = false;
            run.eventCardsEnchant = false;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "DOLL_ROOM") {
        if (opt.rfind("DOLL_", 0) == 0) {
            run.relics.push_back(std::atoi(opt.c_str() + 5));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "RANDOM") {
            run.relics.push_back(kDollRelics[run.eventRng.NextInt(3)]);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "TAKE_SOME_TIME" || opt == "EXAMINE") {
            const int dmg = opt == "TAKE_SOME_TIME" ? 5 : 15;
            run.player.hp = std::max(0, run.player.hp - dmg);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            std::vector<int> order(kDollRelics, kDollRelics + 3);
            unstableShuffleIdx(order, run.eventRng);
            const int take = opt == "TAKE_SOME_TIME" ? 2 : 3;
            run.eventOptions.clear();
            for (int k = 0; k < take; ++k)
                run.eventOptions.push_back("DOLL_" + std::to_string(order[k]));
            run.eventPage = 1;
            return true;
        }
        return false;
    }
    if (run.eventId == "UNREST_SITE") {
        if (opt == "REST") {
            run.player.hp = run.player.maxHp;
            run.player.deck.push_back({CardId::POOR_SLEEP, 0});
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "KILL") {
            run.player.maxHp = std::max(1, run.player.maxHp - run.eventMaxHp);
            if (run.player.hp > run.player.maxHp) run.player.hp = run.player.maxHp;
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "DOORS_OF_LIGHT_AND_DARK") {
        if (opt == "LIGHT") {
            std::vector<int> idx;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
                    idx.push_back(i);
            }
            // StableShuffle sorts by (ModelId, upgrade) before the Fisher-Yates pass.
            std::sort(idx.begin(), idx.end(), [&run](int a, int b) {
                if (run.player.deck[a].id != run.player.deck[b].id)
                    return static_cast<int>(run.player.deck[a].id)
                         < static_cast<int>(run.player.deck[b].id);
                return run.player.deck[a].upgrade < run.player.deck[b].upgrade;
            });
            unstableShuffleIdx(idx, run.eventRng);
            for (int k = 0; k < 2 && k < static_cast<int>(idx.size()); ++k)
                ++run.player.deck[idx[k]].upgrade;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "DARK") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransform = false;
            run.eventCardsRemove = true;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "REFLECTIONS") {
        if (opt == "TOUCH_A_MIRROR") {
            std::vector<int> upgraded;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i)
                if (run.player.deck[i].upgrade > 0) upgraded.push_back(i);
            for (int k = 0; k < 2 && !upgraded.empty(); ++k) {
                const int j = run.eventRng.NextInt(static_cast<int>(upgraded.size()));
                --run.player.deck[upgraded[j]].upgrade;
                upgraded.erase(upgraded.begin() + j);
            }
            std::vector<int> upgradable;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
                    upgradable.push_back(i);
            }
            for (int k = 0; k < 4 && !upgradable.empty(); ++k) {
                const int j = run.eventRng.NextInt(static_cast<int>(upgradable.size()));
                ++run.player.deck[upgradable[j]].upgrade;
                upgradable.erase(upgradable.begin() + j);
            }
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "SHATTER") {
            const int n = static_cast<int>(run.player.deck.size());
            for (int i = 0; i < n; ++i) run.player.deck.push_back(run.player.deck[i]);
            run.player.deck.push_back({CardId::BAD_LUCK, 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "AMALGAMATOR") {
        const unsigned tag = opt == "COMBINE_STRIKES" ? CT_STRIKE
                           : (opt == "COMBINE_DEFENDS" ? CT_DEFEND : 0);
        if (tag == 0) return false;
        run.eventCardPool.clear();
        run.eventCardDeckIdx.clear();
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            const CardDef& d = BattleContext::cardDef(
                static_cast<CardId>(run.player.deck[i].id));
            if (d.hasTag(tag) && d.rarity == CardRarity::BASIC) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
        }
        if (static_cast<int>(run.eventCardPool.size()) < 2) return false;
        // The combined card is appended after the grid snapshot (indices stay valid).
        run.player.deck.push_back({opt == "COMBINE_STRIKES" ? CardId::ULTIMATE_STRIKE
                                                            : CardId::ULTIMATE_DEFEND, 0});
        run.eventCardsTransform = false;
        run.eventCardsRemove = true;
        run.eventCardsLeft = 2;
        run.awaitingEvent = false;
        run.awaitingEventCards = true;
        return true;
    }
    if (run.eventId == "LUMINOUS_CHOIR") {
        if (opt == "REACH_INTO_THE_FLESH") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            // The curse is appended after the grid snapshot so it cannot be selected.
            run.player.deck.push_back({CardId::SPORE_MIND, 0});
            run.eventCardsTransform = false;
            run.eventCardsRemove = true;
            run.eventCardsLeft = std::min(2, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        if (opt == "OFFER_TRIBUTE") {
            if (run.player.gold < run.eventGold) return false;
            run.player.gold -= run.eventGold;
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "WELLSPRING") {
        if (opt == "BOTTLE") {
            const std::vector<int> options = freshSavePotionOptions();
            if (!options.empty())
                grantPotionById(run, options[run.rewards.NextInt(static_cast<int>(options.size()))]);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "BATHE") {
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.player.deck.push_back({CardId::GUILTY, 0});
            run.eventCardsTransform = false;
            run.eventCardsRemove = true;
            run.eventCardsLeft = std::min(1, static_cast<int>(run.eventCardPool.size()));
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "THIS_OR_THAT") {
        if (opt == "PLAIN") {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        if (opt == "ORNATE") {
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.player.deck.push_back({CardId::CLUMSY, 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "POTION_COURIER") {
        if (opt == "GRAB_POTIONS") {
            for (int i = 0; i < 3; ++i)
                grantPotionById(run, static_cast<int>(PotionId::FOUL_POTION));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "RANSACK") {
            std::vector<int> cand;
            for (int id : freshSavePotionOptions())
                if (sts2PotionDef(static_cast<PotionId>(id)).rarity == PotionRarity::UNCOMMON)
                    cand.push_back(id);
            if (!cand.empty())
                grantPotionById(run, cand[run.rewards.NextInt(static_cast<int>(cand.size()))]);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "TABLET_OF_TRUTH") {
        if (opt == "SMASH") {
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "GIVE_UP") {
            run.awaitingEvent = false;
            return true;
        }
        if (opt != "DECIPHER") return false;
        const int cost = run.eventDamage;
        // LoseMaxHpAndUpgrade: maxHp <= cost sets max HP to 1, then CreatureCmd.Kill.
        if (cost >= run.player.maxHp) {
            run.player.maxHp = 1;
            run.player.hp = 0;
            run.over = true;
            run.won = false;
            run.awaitingEvent = false;
            return true;
        }
        run.player.maxHp -= cost;
        if (run.player.hp > run.player.maxHp) run.player.hp = run.player.maxHp;
        std::vector<int> upgradable;
        for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
            const RunCard& c = run.player.deck[i];
            if (c.upgrade < BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
                upgradable.push_back(i);
        }
        // _decipherCount == 4 on the fifth call upgrades every upgradable card.
        const int count = run.eventPage;
        if (count == 4) {
            for (int i : upgradable) ++run.player.deck[i].upgrade;
        } else if (!upgradable.empty()) {
            const int pick = upgradable[run.eventRng.NextInt(static_cast<int>(upgradable.size()))];
            ++run.player.deck[pick].upgrade;
        }
        const int next = count + 1;
        run.eventPage = next;
        if (next == 5) {
            run.awaitingEvent = false;
            return true;
        }
        // GetDecipherCost(next): 6 / 12 / 24 / maxHp-1.
        static const int kCost[3] = {6, 12, 24};
        run.eventDamage = next <= 3 ? kCost[next - 1] : run.player.maxHp - 1;
        run.eventOptions = {"DECIPHER", "GIVE_UP"};
        return true;
    }
    if (run.eventId == "LOST_WISP") {
        if (opt == "CLAIM") {
            run.player.deck.push_back({CardId::DECAY, 0});
            run.relics.push_back(static_cast<int>(RelicId::LOST_WISP));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "SEARCH") {
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "SUNKEN_TREASURY") {
        if (opt == "FIRST_CHEST") {
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "SECOND_CHEST") {
            run.player.gold += run.eventGold2;
            run.player.deck.push_back({CardId::GREED, 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "BYRDONIS_NEST") {
        if (opt == "EAT") {
            run.player.maxHp += run.eventMaxHp;
            run.player.hp += run.eventMaxHp;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "TAKE") {
            run.player.deck.push_back({CardId::BYRDONIS_EGG, 0});
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "MORPHIC_GROVE") {
        if (opt == "LONER") {
            run.player.maxHp += run.eventMaxHp;
            run.player.hp += run.eventMaxHp;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "GROUP") {
            run.player.gold = 0; // PlayerCmd.LoseGold(owner.Gold, Stolen)
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransform = true;
            run.eventCardsLeft = 2;
            run.eventPendingHpLoss = 0;
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "ROUND_TEA_PARTY") {
        if (opt == "ENJOY_TEA") {
            run.relics.push_back(static_cast<int>(RelicId::ROYAL_POISON));
            run.player.hp = run.player.maxHp; // CreatureCmd.Heal to full
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "PICK_FIGHT") {
            run.eventPage = 1;
            run.eventOptions = {"CONTINUE_FIGHT"};
            return true;
        }
        if (opt == "CONTINUE_FIGHT" && run.eventPage == 1) {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            const int rid = pullRelic(run, rollRelicRarity(run), /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "WELCOME_TO_WONGOS") {
        if (opt == "BARGAIN_BIN") {
            if (run.player.gold < run.eventGold) return false;
            run.player.gold -= run.eventGold;
            const int rid = pullRelic(run, RelicRarity::COMMON, /*fromBack=*/false);
            if (rid != static_cast<int>(RelicId::INVALID)) run.relics.push_back(rid);
            addWongoPoints(run, 32);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "FEATURED_ITEM") {
            if (run.player.gold < run.eventGold2) return false;
            run.player.gold -= run.eventGold2;
            if (run.eventRelicId != static_cast<int>(RelicId::INVALID))
                run.relics.push_back(run.eventRelicId);
            addWongoPoints(run, 16);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "MYSTERY_BOX") {
            if (run.player.gold < run.eventHpLoss) return false;
            run.player.gold -= run.eventHpLoss;
            run.relics.push_back(static_cast<int>(RelicId::WONGOS_MYSTERY_TICKET));
            addWongoPoints(run, 8);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "LEAVE") {
            // Downgrade one random upgraded card (event Rng).
            std::vector<int> upgraded;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i)
                if (run.player.deck[i].upgrade > 0) upgraded.push_back(i);
            if (!upgraded.empty()) {
                Sts2Rng rng(run.seed + xxhash64("WELCOME_TO_WONGOS"));
                const int pick = upgraded[rng.NextInt(static_cast<int>(upgraded.size()))];
                --run.player.deck[pick].upgrade;
            }
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "COLOSSAL_FLOWER") {
        static const int kPrize[3] = {35, 75, 135};
        static const int kDamage[3] = {5, 6, 7};
        const int stage = std::min(std::max(run.eventPage, 0), 2);
        if (opt == "EXTRACT_CURRENT_PRIZE" || opt == "EXTRACT_INSTEAD") {
            run.player.gold += kPrize[stage];
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "POLLINOUS_CORE" && stage == 2) {
            run.player.hp = std::max(0, run.player.hp - kDamage[2]);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            run.relics.push_back(static_cast<int>(RelicId::POLLINOUS_CORE));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "REACH_DEEPER" && stage < 2) {
            run.player.hp = std::max(0, run.player.hp - kDamage[stage]);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            run.eventPage = stage + 1;
            run.eventDamage = kDamage[run.eventPage];
            run.eventGold = kPrize[run.eventPage];
            if (run.eventPage == 2) run.eventOptions = {"EXTRACT_INSTEAD", "POLLINOUS_CORE"};
            return true;
        }
        return false;
    }
    if (run.eventId == "TEA_MASTER") {
        if (opt == "BONE_TEA") {
            if (run.player.gold < run.eventGold) return false;
            run.player.gold -= run.eventGold;
            run.relics.push_back(static_cast<int>(RelicId::BONE_TEA));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "EMBER_TEA") {
            if (run.player.gold < run.eventGold2) return false;
            run.player.gold -= run.eventGold2;
            run.relics.push_back(static_cast<int>(RelicId::EMBER_TEA));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "TEA_OF_DISCOURTESY") {
            run.relics.push_back(static_cast<int>(RelicId::TEA_OF_DISCOURTESY));
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "WHISPERING_HOLLOW") {
        if (opt == "GOLD") {
            run.player.gold = std::max(0, run.player.gold - run.eventGold);
            grantPotionReward(run);
            grantPotionReward(run);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "HUG") {
            // Transform one chosen deck card (grid), then take the HP loss.
            run.eventCardPool.clear();
            run.eventCardDeckIdx.clear();
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                run.eventCardPool.push_back({static_cast<int>(run.player.deck[i].id),
                                             run.player.deck[i].upgrade});
                run.eventCardDeckIdx.push_back(i);
            }
            run.eventCardsTransform = true;
            run.eventCardsLeft = 1;
            run.eventPendingHpLoss = run.eventHpLoss;
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "ROOM_FULL_OF_CHEESE") {
        if (opt == "SEARCH") {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            run.relics.push_back(static_cast<int>(RelicId::CHOSEN_CHEESE));
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        if (opt == "GORGE") {
            int n = 0;
            const CardId* pool = sts2RewardPool(run.player.character.c_str(), &n);
            std::vector<int> commons;
            for (int i = 0; i < n; ++i)
                if (BattleContext::cardDef(pool[i]).rarity == CardRarity::COMMON
                    && BattleContext::cardImplemented(pool[i]))
                    commons.push_back(static_cast<int>(pool[i]));
            Sts2Rng rng(run.seed + xxhash64("ROOM_FULL_OF_CHEESE"));
            for (int i = static_cast<int>(commons.size()) - 1; i > 0; --i) {
                const int j = rng.NextInt(i + 1);
                std::swap(commons[i], commons[j]);
            }
            run.eventCardPool.clear();
            for (int k = 0; k < 8 && k < static_cast<int>(commons.size()); ++k)
                run.eventCardPool.push_back({commons[k], 0});
            run.eventCardsLeft = 2;
            run.awaitingEvent = false;
            run.awaitingEventCards = !run.eventCardPool.empty();
            return true;
        }
        return false;
    }
    if (run.eventId == "SUNKEN_STATUE") {
        if (opt == "GRAB_SWORD") {
            run.relics.push_back(static_cast<int>(RelicId::SWORD_OF_STONE));
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "DIVE_INTO_WATER") {
            run.player.gold += run.eventGold;
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        return false;
    }
    if (run.eventId == "HUNGRY_FOR_MUSHROOMS") {
        if (opt == "BIG_MUSHROOM") {
            run.relics.push_back(static_cast<int>(RelicId::BIG_MUSHROOM));
            run.player.maxHp += run.eventMaxHp; // AfterObtained: GainMaxHp(20)
            run.player.hp += run.eventMaxHp;
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "FRAGRANT_MUSHROOM") {
            run.relics.push_back(static_cast<int>(RelicId::FRAGRANT_MUSHROOM));
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            // Upgrade two random upgradable cards (StableShuffle on the Niche stream).
            std::vector<int> upgradable;
            for (int i = 0; i < static_cast<int>(run.player.deck.size()); ++i) {
                const RunCard& c = run.player.deck[i];
                if (c.upgrade < BattleContext::cardDef(static_cast<CardId>(c.id)).maxUpgradeLevel)
                    upgradable.push_back(i);
            }
            Sts2Rng& rng = run.stream(RngStream::Niche);
            for (int i = static_cast<int>(upgradable.size()) - 1; i > 0; --i) {
                const int j = rng.NextInt(i + 1);
                std::swap(upgradable[i], upgradable[j]);
            }
            for (int k = 0; k < 2 && k < static_cast<int>(upgradable.size()); ++k)
                ++run.player.deck[upgradable[k]].upgrade;
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        return false;
    }
    if (run.eventId == "JUNGLE_MAZE_ADVENTURE") {
        if (opt == "SOLO_QUEST") {
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        if (opt == "JOIN_FORCES") {
            run.player.gold += run.eventHeal; // JoinForcesGold rolled above
            run.awaitingEvent = false;
            return true;
        }
        return false;
    }
    if (run.eventId == "ABYSSAL_BATHS") {
        if (opt == "ABSTAIN") {
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "EXIT_BATHS") {
            run.awaitingEvent = false;
            return true;
        }
        if (opt == "IMMERSE" || opt == "LINGER") {
            // CreatureCmd.GainMaxHp heals by the same amount, then Damage applies
            // (unblockable / unpowered) and the next damage value grows by 1.
            run.player.maxHp += run.eventMaxHp;
            run.player.hp += run.eventMaxHp;
            run.player.hp = std::max(0, run.player.hp - run.eventDamage);
            run.eventDamage = std::min(run.eventDamage + 1, 12);
            if (run.player.hp <= 0) {
                run.over = true;
                run.won = false;
                run.awaitingEvent = false;
                return true;
            }
            run.eventPage = 1;
            run.eventOptions = {"LINGER", "EXIT_BATHS"};
            return true;
        }
        return false;
    }
    // DenseVegetation
    if (run.eventPage == 0) {
        if (opt == "TRUDGE_ON") {
            // 8 unblockable/unpowered damage, then gain the rolled gold.
            run.player.hp = std::max(0, run.player.hp - run.eventHpLoss);
            run.player.gold += run.eventGold;
            run.awaitingEvent = false;
            if (run.player.hp <= 0) { run.over = true; run.won = false; }
            return true;
        }
        if (opt == "REST") {
            // PlayerCmd.MimicRestSiteHeal: the rest-site heal amount.
            run.player.hp = std::min(run.player.maxHp, run.player.hp + run.eventHeal);
            run.eventPage = 1;
            run.eventOptions = {"FIGHT"};
            return true;
        }
        return false;
    }
    if (run.eventPage == 1 && opt == "FIGHT") {
        const EncounterDef* enc = findEncounter("DENSE_VEGETATION_ENCOUNTER");
        if (!enc) return false;
        const auto mons = buildMonsterList(*enc, run);
        const FightResult r = fight(run, *enc, mons, searchSims);
        run.awaitingEvent = false;
        if (!r.ok) { run.over = true; return false; }
        if (!r.won || run.player.hp <= 0) { run.over = true; run.won = false; }
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Default (scaffold) macro policy: card reward / rest site.
//
// This is the policy `run play|bench` uses and the fallback the training driver calls
// when a decision cannot be scored. The original rules threw information away: it always
// took the FIRST offered card (which bloats the deck with commons) and healed only below
// 50% HP. Measured on 500 seeded runs at 300 sims, 85% of runs end in act 1 and 47.6% die
// on the act-1 boss, so deck quality and HP economy are the levers here.
//
// STS2_MACRO=legacy restores the previous behaviour for A/B runs.
// ---------------------------------------------------------------------------
namespace {

bool macroSmart() {
    static const bool smart = [] {
        const char* v = std::getenv("STS2_MACRO");
        return !(v && std::string(v) == "legacy");
    }();
    return smart;
}

// Deck-size control: copies of one card already in the deck, and the two caps.
// A thin deck draws its good cards more often, and a 4th copy of a common adds nothing.
int deckCopies(const RunState& run, int cardId) {
    int n = 0;
    for (const RunCard& c : run.player.deck)
        if (static_cast<int>(c.id) == cardId) ++n;
    return n;
}

int dupCap() {
    static const int cap = [] {
        const char* v = std::getenv("STS2_DUP_CAP");
        return v ? std::atoi(v) : 2;
    }();
    return cap;
}

int deckSizeCap() {
    static const int cap = [] {
        const char* v = std::getenv("STS2_DECK_SIZE_CAP");
        return v ? std::atoi(v) : 22;
    }();
    return cap;
}

double macroCardValue(int cid, int upgrade) {
    const CardDef& d = BattleContext::cardDef(static_cast<CardId>(cid));
    if (d.rarity == CardRarity::CURSE_RARITY || d.rarity == CardRarity::STATUS_RARITY)
        return -10.0;

    // Score what the card does rather than how rare it is. Rarity is a weak proxy: the
    // run dies from HP attrition (the act-1 boss alone costs ~55% of max HP), so block and
    // damage reduction matter more than a shiny uncommon attack. Weights mirror the search
    // heuristic so the pick and the playout agree on what "good" means.
    static const bool kByEffect = [] {
        const char* v = std::getenv("STS2_CARD_VALUE");
        return !(v && std::string(v) == "rarity");
    }();
    if (!kByEffect) {
        double v = 1.0;
        switch (d.rarity) {
            case CardRarity::ANCIENT:
            case CardRarity::RARE: v = 4.0; break;
            case CardRarity::UNCOMMON: v = 2.5; break;
            case CardRarity::SPECIAL:
            case CardRarity::EVENT: v = 2.0; break;
            default: v = 1.0; break;
        }
        if (d.type == CardType::POWER) v += 1.0;
        else if (d.type == CardType::ATTACK) v += 0.5;
        return v;
    }

    CardInstance inst;
    inst.id = static_cast<CardId>(cid);
    inst.upgrade = upgrade;
    double dmg = 0, blk = 0, debuff = 0, selfBuff = 0, draw = 0, energy = 0;
    for (int i = 0; i < d.nSteps; ++i) {
        const CardStep& st = d.steps[i];
        const int val = st.varIdx >= 0 && st.varIdx < d.nVars
            ? cardVarValue(inst, d, st.varIdx) : st.constVal;
        const int hits = st.hits > 0 ? st.hits
            : (st.hits < 0 ? cardVarValue(inst, d, -(st.hits) - 1) : 1);
        switch (st.op) {
            case StepOp::ATTACK: case StepOp::ATTACK_CALC: case StepOp::ATTACK_FEED:
            case StepOp::ATTACK_HITS_CALC: case StepOp::ATTACK_ALL:
                dmg += static_cast<double>(std::max(0, val)) * hits;
                break;
            case StepOp::GAIN_BLOCK: case StepOp::GAIN_BLOCK_CALC:
            case StepOp::GAIN_BLOCK_EXHAUST_DOUBLE:
                blk += std::max(0, val);
                break;
            case StepOp::APPLY_POWER_ENEMY: case StepOp::APPLY_POWER_ALL_ENEMIES: {
                const double amount = std::max(0, val) * (st.sign < 0 ? -1.0 : 1.0);
                double w = 0.15;
                if (st.power == PowerId::VULNERABLE) w = 0.35;
                else if (st.power == PowerId::WEAK) w = 0.30;
                else if (st.power == PowerId::POISON) w = 0.80;
                debuff += amount * w;
                break;
            }
            case StepOp::APPLY_POWER_SELF: {
                const double amount = std::max(0, val) * (st.sign < 0 ? -1.0 : 1.0);
                double w = 0.40;
                if (st.power == PowerId::STRENGTH) w = 0.80;
                else if (st.power == PowerId::DEXTERITY) w = 0.60;
                selfBuff += amount * w;
                break;
            }
            case StepOp::DRAW: case StepOp::DRAW_AND_RETAIN:
            case StepOp::DRAW_TO_MAX: case StepOp::DRAW_IF_NO_ATTACK:
                draw += std::max(0, val);
                break;
            case StepOp::GAIN_ENERGY:
                energy += std::max(0, val);
                break;
            default:
                break;
        }
    }
    // Weight calibration. The chain measured from 2000 seeded runs is: each boss costs
    // ~30 HP, recovery between bosses is under 30, and the act-2 boss is reached at 37 HP
    // (71% of those fights are lost). HP is therefore the binding resource, not kill speed,
    // so `mitigation` prices block and damage reduction above raw damage while keeping the
    // same step-level inputs. STS2_CARD_WEIGHTS=balance keeps the generic weighting.
    static const std::string kWeights = [] {
        const char* v = std::getenv("STS2_CARD_WEIGHTS");
        return std::string(v ? v : "balance");
    }();
    const bool kMitigation = kWeights == "mitigation";
    const bool kAggressive = kWeights == "aggressive";
    const double dmgW = kMitigation ? 1.0 / 11.0 : (kAggressive ? 1.0 / 6.5 : 1.0 / 8.0);
    const double blkW = kMitigation ? 1.0 / 5.5 : (kAggressive ? 1.0 / 10.0 : 1.0 / 8.0);
    const double debuffW = kMitigation ? 0.75 : (kAggressive ? 0.5 : 0.6);
    const double selfBuffW = kMitigation ? 0.8 : (kAggressive ? 1.1 : 1.0);
    double v = dmg * dmgW + blk * blkW + debuff * debuffW + selfBuff * selfBuffW
               + draw * 0.5 + energy * 1.0;
    // Per-energy efficiency: a 2-cost card must do twice the work.
    const int cost = d.cost > 0 ? d.cost : 1;
    v /= std::max(1, cost);
    // Rarity breaks ties only.
    if (d.rarity == CardRarity::RARE || d.rarity == CardRarity::ANCIENT) v += 0.3;
    else if (d.rarity == CardRarity::UNCOMMON) v += 0.15;
    if (d.type == CardType::POWER) v += 0.5;
    return v;
}

} // namespace


// One-step lookahead over an event's options. RunState is a value type, so each option
// can be tried on a copy and discarded: the policy compares the resulting HP, gold, deck
// size, relic and potion counts and keeps the best. This replaces "take the first
// option", which ignored every reward/cost distinction (65 events are implemented and the
// scaffold used almost none of their structure).
//
// Weights follow the measured chain: HP is the binding resource across acts, so 1 HP is
// worth 1 point; 20 gold (a shop card) ≈ 1 HP; a relic ≈ 8 HP; a card ≈ 2 HP; dying
// dominates everything.
// STS2_EVENT_POLICY=first restores the old behaviour.
int bestEventOption(const RunState& run, long long sims) {
    static const bool kLookahead = [] {
        const char* v = std::getenv("STS2_EVENT_POLICY");
        return !(v && std::string(v) == "first");
    }();
    if (!kLookahead) return 0;
    auto potionsOf = [](const RunState& r) {
        int n = 0;
        for (int p : r.player.potions) if (p != 0) ++n;
        return n;
    };
    int bestIdx = 0;
    double best = -1e18;
    for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i) {
        RunState copy = run;
        if (!chooseEventOption(copy, i, sims)) continue;
        if (!kLookahead) return 0;
        double score = static_cast<double>(copy.player.hp - run.player.hp)
                     + static_cast<double>(copy.player.gold - run.player.gold) * 0.05
                     + (static_cast<double>(copy.relics.size())
                        - static_cast<double>(run.relics.size())) * 8.0
                     + (static_cast<double>(potionsOf(copy))
                        - static_cast<double>(potionsOf(run))) * 2.0;
        score += (static_cast<double>(copy.player.deck.size())
                  - static_cast<double>(run.player.deck.size())) * 1.0;
        // Max HP was not scored at all, so an option that trades current HP for permanent
        // max HP read as a pure loss and was always declined. Max HP is the pool the
        // bosses spend against (act-2 boss costs a median 67 of 80), so it is worth
        // something on its own. Weight 0 keeps the previous behaviour.
        static const double kEventMaxHpW = [] {
            const char* v = std::getenv("STS2_EVENT_MAXHP_W");
            return v ? std::atof(v) : 0.0;
        }();
        if (kEventMaxHpW > 0.0)
            score += static_cast<double>(copy.player.maxHp - run.player.maxHp)
                     * kEventMaxHpW;
        if (copy.over && !copy.won) score -= 60.0;
        if (copy.won) score += 60.0;
        if (score > best) { best = score; bestIdx = i; }
    }
    return bestIdx;
}


// Two remaining blind picks in the scaffold, same fix as the rest of the macro layer:
// compare candidates with the effect score instead of taking index 0.
//   - Toolbox offers three cards for the next combat's opening;
//   - an event card grid adds / upgrades / enchants one card.
int bestToolboxIndex(const RunState& run) {
    int best = 0;
    double bestV = -1e18;
    for (int i = 0; i < static_cast<int>(run.toolboxOptions.size()); ++i) {
        const double v = macroCardValue(run.toolboxOptions[i], 0);
        if (v > bestV) { bestV = v; best = i; }
    }
    return best;
}

int bestEventCardIndex(const RunState& run) {
    int best = 0;
    double bestV = -1e18;
    for (int i = 0; i < static_cast<int>(run.eventCardPool.size()); ++i) {
        double v = 0.0;
        const bool onDeck = (run.eventCardsUpgrade || run.eventCardsEnchant)
                            && i < static_cast<int>(run.eventCardDeckIdx.size());
        if (onDeck) {
            const int d = run.eventCardDeckIdx[i];
            if (d >= 0 && d < static_cast<int>(run.player.deck.size())) {
                const RunCard& rc = run.player.deck[d];
                v = macroCardValue(static_cast<int>(rc.id), rc.upgrade);
                if (run.eventCardsUpgrade) v += 0.5;   // an upgrade is a real gain
            }
        } else {
            v = macroCardValue(run.eventCardPool[i].first, run.eventCardPool[i].second);
        }
        if (v > bestV) { bestV = v; best = i; }
    }
    return best;
}

void withDefaultDecisions(RunState& run) {
    // Toolbox's opening pick: the scaffold takes the first offered card.
    // Default OFF: measured neutral-to-worse (see ledger 2026-10-22). Kept behind
    // STS2_PICK_POLICY=smart as a documented negative result.
    static const bool kSmartPicks = [] {
        const char* v = std::getenv("STS2_PICK_POLICY");
        return v && std::string(v) == "smart";
    }();
    if (run.awaitingToolbox) chooseToolbox(run, kSmartPicks ? bestToolboxIndex(run) : 0);
    // Shop policy. The scaffold used to walk straight out, leaving the whole shop line
    // unused — and card picks are the single biggest lever measured so far (+1.05~1.18
    // floors when the reward pick became effect-based), so paying gold for a good card or
    // a relic is worth it.
    // Priority: relic > removal (thin the starter cards) > potion (free slot only) >
    // card by the same effect score the reward screen uses. STS2_SHOP_POLICY=legacy
    // restores the walk-out behaviour.
    static const bool kSmartShop = [] {
        const char* v = std::getenv("STS2_SHOP_POLICY");
        return !(v && std::string(v) == "legacy");
    }();
    for (int guard = 0; run.awaitingShop && guard < 12; ++guard) {
        if (!kSmartShop) { leaveShop(run); break; }
        int bestIdx = -1;
        int bestRemoveIdx = -1;
        double bestScore = -1e9;
        const int gold = run.player.gold;
        for (int i = 0; i < static_cast<int>(run.shop.size()); ++i) {
            const ShopEntry& e = run.shop[i];
            if (e.sold || e.price > gold) continue;
            double score = -1e9;
            int removeIdx = -1;
            if (e.relic) {
                score = 60.0 - e.price * 0.05;
            } else if (e.removal) {
                // Removing a starter card is a permanent deck improvement; the removal
                // price grows with each use, which the price term already reflects.
                // Which basic to delete: the lowest-value one, not simply the first. The
                // deck holds both Strikes and Defends, and removing the worse copy is the
                // whole point of paying for a removal. STS2_REMOVE_POLICY=first reverts.
                static const bool kSmartRemove = [] {
                    const char* v = std::getenv("STS2_REMOVE_POLICY");
                    return !(v && std::string(v) == "first");
                }();
                double worst = 1e18;
                for (int d = 0; d < static_cast<int>(run.player.deck.size()); ++d) {
                    const RunCard& rc = run.player.deck[d];
                    const CardDef& cd = BattleContext::cardDef(rc.id);
                    if (cd.rarity != CardRarity::BASIC && cd.rarity != CardRarity::STARTER)
                        continue;
                    if (!kSmartRemove) { removeIdx = d; break; }
                    const double v = macroCardValue(static_cast<int>(rc.id), rc.upgrade);
                    if (v < worst) { worst = v; removeIdx = d; }
                }
                if (removeIdx >= 0) {
                    // Deck thinning is the one macro lever with a measured ceiling: a
                    // stamp-level counterfactual (four fewer basics in the same 40 act-2
                    // boss entries) took that boss from 5/40 to 12/40 at equal wall clock.
                    // The scaffold still buys a relic first whenever one is on the shelf.
                    // STS2_SHOP_REMOVAL_FIRST=1 puts the removal ahead of the relic so the
                    // two orders can be compared.
                    static const bool kRemovalFirst = [] {
                        const char* v = std::getenv("STS2_SHOP_REMOVAL_FIRST");
                        return v && std::string(v) == "1";
                    }();
                    score = (kRemovalFirst ? 70.0 : 40.0) - e.price * 0.05;
                }
            } else if (e.potion) {
                bool freeSlot = false;
                for (int pid : run.player.potions) if (pid == 0) freeSlot = true;
                if (freeSlot) score = 25.0 - e.price * 0.05;
            } else if (e.cardId > 0) {
                const int cap = dupCap();
                if (cap > 0 && deckCopies(run, e.cardId) >= cap) continue;
                if (static_cast<int>(run.player.deck.size()) >= deckSizeCap()) continue;
                score = macroCardValue(e.cardId, e.upgrade) * 10.0 - e.price * 0.05;
            }
            if (score > bestScore) { bestScore = score; bestIdx = i; bestRemoveIdx = removeIdx; }
        }
        if (bestIdx < 0) { leaveShop(run); break; }
        if (!buyShop(run, bestIdx, bestRemoveIdx)) { leaveShop(run); break; }
    }
    // Events: pick ABSTAIN/EXIT when offered (AbyssalBaths' LINGER is repeatable, and a
    // naive "first option" policy would keep lingering until the player dies). Cap the
    // decisions per event so a policy that never exits cannot spin forever.
    for (int guard = 0; run.awaitingEventCards && guard < 8; ++guard)
        if (!chooseEventCard(run, kSmartPicks ? bestEventCardIndex(run) : 0)) break;
    for (int guard = 0; run.awaitingEvent && guard < 8; ++guard) {
        // Default: one-step lookahead over the options. The explicit overrides below still
        // win where a repeatable option would otherwise loop or where a known HP cost can
        // kill the run.
        int pick = macroSmart() ? bestEventOption(run, 500) : 0;
        if (pick < 0) pick = 0;
        for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i)
            if (run.eventOptions[i] == "ABSTAIN" || run.eventOptions[i] == "EXIT_BATHS")
                pick = i;
        // TabletOfTruth's DECIPHER is repeatable and each step costs max HP.
        if (run.eventId == "TABLET_OF_TRUTH")
            for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i)
                if (run.eventOptions[i] == "SMASH") pick = i;
        // ThisOrThat's PLAIN is a 6 HP loss; take the relic instead when that could kill.
        if (run.eventId == "THIS_OR_THAT" && run.player.hp <= run.eventHpLoss)
            for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i)
                if (run.eventOptions[i] == "ORNATE") pick = i;
        // TrashHeap's DIVE_IN costs 8 HP; grab the gold/card instead when that could kill.
        if (run.eventId == "TRASH_HEAP" && run.player.hp <= run.eventHpLoss)
            for (int i = 0; i < static_cast<int>(run.eventOptions.size()); ++i)
                if (run.eventOptions[i] == "GRAB") pick = i;
        bool chosen = chooseEventOption(run, pick, 500);
        if (!chosen) {
            // The preferred option can be rejected by its own gate (CrystalSphere's
            // UNCOVER_FUTURE needs eventGold). Try the remaining options instead of
            // leaving the event pending; PAYMENT_PLAN always resolves that page.
            for (int i = 0; i < static_cast<int>(run.eventOptions.size()) && !chosen; ++i)
                if (i != pick) chosen = chooseEventOption(run, i, 500);
        }
        if (!chosen) break;
    }
    // CrystalSphere: once the minigame starts the option list is empty; click the first
    // fogged cell with the big tool so scaffold policies still finish the event.
    for (int guard = 0; run.awaitingEvent && run.eventId == "CRYSTAL_SPHERE"
         && run.eventPage == 1 && run.eventSphereDivinations > 0 && guard < 16; ++guard) {
        int cell = -1;
        for (int i = 0; i < static_cast<int>(run.eventSphereHidden.size()); ++i)
            if (run.eventSphereHidden[i]) { cell = i; break; }
        if (cell < 0) break;
        if (!chooseSphereCell(run, cell % kSphereSide, cell / kSphereSide, 2)) break;
    }
    if (run.awaitingPotionReward) choosePotionReward(run, /*take=*/true);
    while (run.awaitingCardReward) {
        static const bool kSmartCards = [] {
            const char* v = std::getenv("STS2_CARD_POLICY");
            return !(v && std::string(v) == "legacy");
        }();
        int pick = run.cardOffer.empty() ? -1 : 0;
        if (kSmartCards && !run.cardOffer.empty()) {
            // Deck hygiene: keep the best card, but stop adding commons once the deck is
            // large enough that a thin deck draws its good cards more often.
            static const double kDeckCap = [] {
                const char* v = std::getenv("STS2_DECK_CAP");
                return v ? std::atof(v) : 20.0;
            }();
            const int cap = dupCap();
            const bool hardCap = static_cast<int>(run.player.deck.size()) >= deckSizeCap();
            double best = -1e9;
            int bestIdx = -1;
            for (int i = 0; i < static_cast<int>(run.cardOffer.size()); ++i) {
                const int cid = run.cardOffer[i].first;
                const double v = macroCardValue(cid, run.cardOffer[i].second);
                // Duplicate cap and hard size cap apply to every candidate, so the best
                // remaining card is still chosen when the top one is blocked.
                if (cap > 0 && deckCopies(run, cid) >= cap) continue;
                if (hardCap) continue;
                if (v > best) { best = v; bestIdx = i; }
            }
            // Take the card only when it beats the deck's own median. Comparing the
            // candidate against what the deck already holds is a different question from
            // "is the deck too big yet": a deck of 18 decent cards should still decline a
            // below-average common. Small decks take anything reasonable.
            // STS2_CARD_MEDIAN=0 restores the size-only rule.
            // Default OFF: measured neutral-to-negative (ledger 2026-10-22). Kept behind
            // STS2_CARD_MEDIAN=1 as a documented negative result.
            static const bool kMedianRule = [] {
                const char* v = std::getenv("STS2_CARD_MEDIAN");
                return v && std::string(v) == "1";
            }();
            double deckMedian = 0.0;
            if (kMedianRule && run.player.deck.size() >= 10) {
                std::vector<double> vals;
                vals.reserve(run.player.deck.size());
                for (const RunCard& rc : run.player.deck)
                    vals.push_back(macroCardValue(static_cast<int>(rc.id), rc.upgrade));
                std::sort(vals.begin(), vals.end());
                deckMedian = vals[vals.size() / 2];
            }
            if (best < 0.0) pick = -1;                                   // curse/status/blocked
            else if (deckMedian > 0.0 && best < deckMedian * 0.95)
                pick = -1;                                               // below deck average
            else if (static_cast<double>(run.player.deck.size()) >= kDeckCap && best < 2.0)
                pick = -1;                                               // skip a common
            else pick = bestIdx;
        }
        if (!chooseCardReward(run, pick)) break;
    }
    if (run.awaitingRelicReward) chooseRelicReward(run, /*take=*/true);
    while (run.awaitingRest) {
        bool done;
        static const double kHealFrac = [] {
            const char* v = std::getenv("STS2_REST_HEAL_FRAC");
            return v ? std::atof(v) : 0.8;
        }();
        const double hpFrac = run.player.maxHp > 0
            ? static_cast<double>(run.player.hp) / run.player.maxHp : 1.0;
        // Rest policy. Measured chain: each boss costs ~30 HP and the recovery between
        // bosses is under 30 (act-2 boss is reached at 37 HP, 71% of those fights are
        // lost). So the question is when to bank HP versus when to upgrade.
        //   boss   heal when near the boss even if healthy, otherwise the 80% rule
        //   smith  heal only near the boss, otherwise prefer an upgrade
        //   legacy ignore boss distance (the 80% rule everywhere)
        // STS2_REST_POLICY selects; default is `boss`.
        static const std::string kRestPolicy = [] {
            const char* v = std::getenv("STS2_REST_POLICY");
            return std::string(v ? v : "boss");
        }();
        static const int kBossPrepRows = [] {
            const char* v = std::getenv("STS2_BOSS_PREP_ROWS");
            return v ? std::atoi(v) : 3;
        }();
        int rowsToBoss = 99;
        if (run.current >= 0 && run.current < static_cast<int>(run.map.points.size()))
            rowsToBoss = run.map.height - run.map.points[run.current].row;
        const bool nearBoss = kRestPolicy != "legacy" && rowsToBoss <= kBossPrepRows;
        bool wantHeal;
        if (!macroSmart()) {
            wantHeal = run.player.hp * 2 < run.player.maxHp;
        } else if (nearBoss) {
            wantHeal = hpFrac < 0.95;
        } else if (kRestPolicy == "smith") {
            wantHeal = false;
        } else {
            wantHeal = hpFrac < kHealFrac;
        }
        if (wantHeal && run.player.maxHp <= 0) wantHeal = true;
        if (!wantHeal && run.restUpgradable.empty()) wantHeal = true;
        if (wantHeal)
            done = chooseRest(run, "HEAL", -1);
        else if (!run.restUpgradable.empty()) {
            // Upgrade the card that gains the most from it, not the first upgradable one.
            // Same "compare instead of picking blindly" change that paid off on card
            // rewards, events, shops and routing; an upgrade on a card with a small value
            // delta is close to losing the rest site. STS2_SMITH_POLICY=first reverts.
            static const bool kSmartSmith = [] {
                const char* v = std::getenv("STS2_SMITH_POLICY");
                return !(v && std::string(v) == "first");
            }();
            int smithIdx = run.restUpgradable.front();
            if (kSmartSmith) {
                double bestGain = -1e18;
                for (int d : run.restUpgradable) {
                    if (d < 0 || d >= static_cast<int>(run.player.deck.size())) continue;
                    const RunCard& rc = run.player.deck[d];
                    const double before = macroCardValue(static_cast<int>(rc.id), rc.upgrade);
                    const double after = macroCardValue(static_cast<int>(rc.id), rc.upgrade + 1);
                    const double gain = after - before;
                    if (gain > bestGain) { bestGain = gain; smithIdx = d; }
                }
            }
            done = chooseRest(run, "SMITH", smithIdx);
        }
        else
            done = chooseRest(run, "HEAL", -1);
        if (!done) break;
    }
}

} // namespace sts2
