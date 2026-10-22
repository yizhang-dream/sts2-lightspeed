// sts2 battle core implementation (T1).
// All mechanics values are transcribed from data_private/sts2_decompiled game source.

#include "sts2/Sts2Battle.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdio>

#include "sts2/Sts2Enchantment.h"
#include "sts2/Sts2Potion.h"

namespace sts2 {

static int powerAmount(const std::vector<PowerInst>& list, PowerId id);

// TinkerTime's MadScience takes its card type from the instance configuration.
static CardType effectiveCardType(const CardInstance& c);

// CardFactory.GetDefaultTransformationOptions + GetFilteredTransformationOptions for one
// card: the card transforms against its OWN pool, not the character's (colorless for
// Event/Ancient/Token rarity and Quest cards), and Status/Curse originals skip the
// Common/Uncommon/Rare restriction. The original's id is never in the options. Order is
// the card pool's declaration order, which is the order rng.NextItem draws from.
static std::vector<CardId> transformOptionsFor(CardId orig, const std::string& fallbackPool) {
    int skipRarity = 0;
    const char* pool = sts2CardTransformPoolName(orig, &skipRarity);
    if (!pool || !*pool || std::strcmp(pool, "?") == 0) pool = fallbackPool.c_str();
    int n = 0;
    const CardId* table = skipRarity ? sts2RawTransformPool(pool, &n)
                                     : sts2TransformPool(pool, &n);
    std::vector<CardId> opts;
    if (!table || n <= 0) return opts;
    opts.reserve(n);
    for (int i = 0; i < n; ++i)
        if (table[i] != orig) opts.push_back(table[i]);
    return opts;
}

// ---------------- card table (generated: Sts2CardTable.cpp) ----------------

const CardDef& BattleContext::cardDef(CardId id) { return sts2CardDefFor(id); }

bool BattleContext::cardImplemented(CardId id) {
    const CardDef& d = sts2CardDefFor(id);
    for (int i = 0; i < d.nSteps; ++i)
        if (d.steps[i].op == StepOp::UNSUPPORTED) return false;
    return true;
}

const CardDef& CardInstance::def() const { return sts2CardDefFor(id); }

int CardInstance::energyCost(const BattleContext& bc) const {
    if (freeThisTurn) return 0;
    if (costOverride >= 0) return costOverride;
    const CardDef& d = def();
    if (d.cost < 0) return d.cost; // X/unplayable markers pass through
    const CardType et = effectiveCardType(*this);
    // FreeAttackPower: owner's attack cards cost 0 while stacks remain (late modifier)
    if (et == CardType::ATTACK) {
        for (const auto& p : bc.player.powers)
            if (p.id == PowerId::FREE_ATTACK && p.amount > 0) return 0;
    }
    // CorruptionPower.TryModifyEnergyCostInCombatLate: owner's skills cost 0
    if (et == CardType::SKILL && powerAmount(bc.player.powers, PowerId::CORRUPTION) > 0)
        return 0;
    // FreeSkillPower.TryModifyEnergyCostInCombatLate: owner's skills in hand/play cost 0
    if (et == CardType::SKILL && powerAmount(bc.player.powers, PowerId::FREE_SKILL) > 0)
        return 0;
    int c = d.cost + d.costUp * upgrade;
    // EnergyCost.AddThisCombat: a card that was modified in combat carries its base cost
    // plus every applied delta (parsed from the stamp's cost-state / built in-combat).
    if (costBase >= 0 || !costMods.empty()) {
        c = costBase >= 0 ? costBase : d.cost;
        for (int m : costMods) c += m;
    }
    // BorrowedTimePower.TryModifyEnergyCostInCombat: +Amount on every card while active
    c += powerAmount(bc.player.powers, PowerId::BORROWED_TIME);
    // TangledPower.TryModifyEnergyCostInCombat: an Entangled card costs + Amount.
    if (affliction == 1) c += afflictionAmount;
    // CuriousPower.TryModifyEnergyCostInCombat: Power cards cost Amount less.
    if (et == CardType::POWER) {
        if (int cu = powerAmount(bc.player.powers, PowerId::CURIOUS))
            c = std::max(0, c - cu);
    }
    return c < 0 ? 0 : c;
}
int CardInstance::starCost_() const {
    return starCostOverride >= -1 ? starCostOverride : def().starCost;
}

// effective value of a CardDef var slot at the instance's upgrade level
int cardVarValue(const CardInstance& c, const CardDef& d, int slot) {
    if (slot < 0 || slot >= (int)d.nVars) return 0;
    const CardVarSlot& v = d.vars[slot];
    int val = v.base + v.upBy * c.upgrade;
    if (slot < 4) val += c.varBump[slot]; // Rampage/Thrash permanent in-combat bumps
    return val;
}

bool cardHasFlag(const CardInstance& c, unsigned f) {
    const CardDef& d = c.def();
    bool on = d.hasFlag(f) || (c.upgrade > 0 && (d.flagsUpAdded & f));
    if (c.upgrade > 0 && (d.flagsUpRemoved & f)) on = false;
    return on;
}

// upgrade a card instance, keeping its verbatim stamp text in sync (name level and
// var values); powers pulled by Aggression upgrade without being played
void upgradeCardInstance(CardInstance& c) {
    const CardDef& d = c.def();
    if (d.maxUpgradeLevel <= 0 || c.upgrade >= d.maxUpgradeLevel) return;
    ++c.upgrade;
    if (c.stampText.empty()) return;
    // patch "NAME+N/" -> "NAME+(N+1)/"
    std::string& s = c.stampText;
    size_t plus = s.find('+');
    if (plus != std::string::npos) {
        size_t slash = s.find('/', plus);
        if (slash != std::string::npos) {
            int lvl = std::atoi(s.c_str() + plus + 1);
            s.replace(plus + 1, slash - plus - 1, std::to_string(lvl + 1));
        }
    }
    // patch each var value "[Key=V," -> "[Key=V+upBy,"
    for (int i = 0; i < (int)d.nVars; ++i) {
        std::string pat = std::string(d.vars[i].key) + "=";
        size_t p = s.find(pat);
        if (p == std::string::npos) continue;
        size_t numStart = p + pat.size();
        size_t numEnd = s.find_first_not_of("0123456789", numStart);
        if (numEnd == std::string::npos) numEnd = s.size();
        int v = std::atoi(s.c_str() + numStart);
        s.replace(numStart, numEnd - numStart, std::to_string(v + d.vars[i].upBy));
    }
}

// permanently raise a card var for this combat instance (Rampage/Thrash), keeping the
// verbatim stamp text in sync; synthesized entries re-derive from cardVarValue.
// Rampage also tracks the bump in the stamp's private slot (ExtraDamageFromPlays).
void bumpCardVar(CardInstance& c, int slot, int delta) {
    if (slot < 0 || slot > 3 || delta == 0) return;
    c.varBump[slot] += delta;
    // Per-instance accumulators surfaced in the stamp's private slot: Rampage keeps a
    // separate counter, Claw/Maul/GeneticAlgorithm/TheScythe expose their var bump itself.
    const CardId cid = c.def().id;
    const bool privAccum = cid == CardId::RAMPAGE || cid == CardId::CLAW ||
                           cid == CardId::MAUL || cid == CardId::GENETIC_ALGORITHM ||
                           cid == CardId::THE_SCYTHE;
    if (privAccum) {
        if (cid == CardId::RAMPAGE) c.privateAccum += delta;
        if (!c.stampText.empty()) {
            size_t pp = c.stampText.find("/private=");
            if (pp != std::string::npos) {
                size_t ns = pp + 9;
                size_t ne = c.stampText.find_first_not_of("0123456789", ns);
                if (ne == std::string::npos) ne = c.stampText.size();
                int v = cid == CardId::RAMPAGE ? c.privateAccum : c.varBump[0];
                c.stampText.replace(ns, ne - ns, std::to_string(v));
            }
        }
    }
    if (c.stampText.empty()) return;
    const CardDef& d = c.def();
    if (slot >= (int)d.nVars) return;
    std::string pat = std::string(d.vars[slot].key) + "=";
    size_t p = c.stampText.find(pat);
    if (p == std::string::npos) return;
    size_t numStart = p + pat.size();
    size_t numEnd = c.stampText.find_first_not_of("0123456789", numStart);
    if (numEnd == std::string::npos) numEnd = c.stampText.size();
    int v = std::atoi(c.stampText.c_str() + numStart);
    c.stampText.replace(numStart, numEnd - numStart, std::to_string(v + delta));
}

// ---------------- monster registry ----------------

namespace {

// PowerType.Debuff approximation for relic hooks (Unsettling Lamp, Artifact family).
static bool isDebuffPower(PowerId id) {
    switch (id) {
        case PowerId::VULNERABLE: case PowerId::WEAK: case PowerId::FRAIL:
        case PowerId::POISON: case PowerId::DOOM: case PowerId::SHRINK:
        case PowerId::STRANGLE: case PowerId::CONSTRICT: case PowerId::TANGLED:
        case PowerId::HEX: case PowerId::MANGLE: case PowerId::PIERCING_WAIL:
        case PowerId::DARK_SHACKLES: case PowerId::ENFEEBLING_TOUCH:
        case PowerId::MONARCHS_GAZE_STRENGTH_DOWN:
            return true;
        default: return false;
    }
}

std::vector<MonsterDef>& monsterDefs() {
    static std::vector<MonsterDef> defs;
    if (!defs.empty()) return defs;

    // BYRDONIS (Models.Monsters/Byrdonis.cs): HP 81-84, SWOOP 17 -> PECK 3x3, Territorial +1 str/round.
    {
        MonsterDef d;
        d.name = "BYRDONIS";
        d.minHp = 81; d.maxHp = 84;
        d.startPower = PowerId::TERRITORIAL; d.startPowerAmount = 1;
        d.states = {
            {"SWOOP_MOVE", 17, 1, 0, PowerId::NONE, 0, 1},
            {"PECK_MOVE",   3, 3, 0, PowerId::NONE, 0, 0},
        };
        defs.push_back(d);
    }
    // AEONGLASS (boss): HP 512, WitheringPresence 6 + Artifact 3; EBB 22 (+33 self block)
    // -> EYE_LASERS 11x2 -> INCREASING_INTENSITY (upgrade withers, +1 wither to discard,
    // strength 3+additional, additional++).
    {
        MonsterDef d;
        d.name = "AEONGLASS";
        d.minHp = 512; d.maxHp = 512;
        d.startPower = PowerId::WITHERING_PRESENCE; d.startPowerAmount = 6;
        d.startPower2 = PowerId::ARTIFACT; d.startPower2Amount = 3;
        MoveState ebb;  ebb.moveId = "EBB_MOVE"; ebb.damage = 22; ebb.selfBlock = 33; ebb.next = 1;
        MoveState eye;  eye.moveId = "EYE_LASERS_MOVE"; eye.damage = 11; eye.hits = 2; eye.next = 2;
        MoveState incr; incr.moveId = "INCREASING_INTENSITY_MOVE"; incr.selfPower = PowerId::STRENGTH;
        incr.selfPowerAmount = 3; incr.next = 0;
        incr.upgradePlayerWithers = true; incr.addWithersToDiscard = 1;
        d.states = {ebb, eye, incr};
        defs.push_back(d);
    }
    // DEVOTED_SCULPTOR (DevotedSculptor.cs, act-3 weak): HP 162; FORBIDDEN_INCANTATION
    // (Ritual 9) -> SAVAGE 12, then SAVAGE keeps repeating (Ritual pays out at each
    // enemy-side end after the first one).
    {
        MonsterDef d;
        d.name = "DEVOTED_SCULPTOR";
        d.minHp = 162; d.maxHp = 162;
        MoveState cast; cast.moveId = "FORBIDDEN_INCANTATION_MOVE";
        cast.selfPower = PowerId::RITUAL; cast.selfPowerAmount = 9; cast.next = 1;
        MoveState savage; savage.moveId = "SAVAGE_MOVE"; savage.damage = 12; savage.next = 1;
        d.states = {cast, savage};
        defs.push_back(d);
    }
    // SCROLL_OF_BITING (ScrollOfBiting.cs, act-3 weak): HP 30-37, PaperCuts 2; the
    // encounter assigns each instance a different starting move (0 CHOMP / 1 CHEW /
    // 2 MORE_TEETH). CHOMP -> MORE_TEETH -> CHEW -> rand{CHOMP CannotRepeat, CHEW x2}.
    {
        MonsterDef d;
        d.name = "SCROLL_OF_BITING";
        d.minHp = 30; d.maxHp = 37;
        d.startPower = PowerId::PAPER_CUTS; d.startPowerAmount = 2;
        MoveState chomp; chomp.moveId = "CHOMP"; chomp.damage = 14; chomp.next = 2;
        MoveState chew; chew.moveId = "CHEW"; chew.damage = 5; chew.hits = 2; chew.next = 3;
        MoveState teeth; teeth.moveId = "MORE_TEETH"; teeth.selfPower = PowerId::STRENGTH;
        teeth.selfPowerAmount = 2; teeth.next = 1;
        MoveState rnd; rnd.moveId = "rand"; rnd.branch = {{1.0f, 0, 1}, {1.0f, 1, 2}};
        d.states = {chomp, chew, teeth, rnd};
        defs.push_back(d);
    }
    // SLIMED_BERSERKER (SlimedBerserker.cs, act-3 normal): HP 261; VOMIT_ICHOR (10 Slimed
    // to the discard bottom) -> FURIOUS_PUMMELING 4x4 -> LEECHING_HUG (weak 3, +3 str) ->
    // SMOTHER 30 -> VOMIT...
    {
        MonsterDef d;
        d.name = "SLIMED_BERSERKER";
        d.minHp = 261; d.maxHp = 261;
        MoveState vomit; vomit.moveId = "VOMIT_ICHOR_MOVE";
        vomit.addCardToDiscard = CardId::SLIMED; vomit.addCardToDiscardCount = 10;
        vomit.next = 3;
        MoveState smother; smother.moveId = "SMOTHER_MOVE"; smother.damage = 30;
        smother.next = 0;
        MoveState hug; hug.moveId = "LEECHING_HUG_MOVE";
        hug.playerPower = PowerId::WEAK; hug.playerPowerAmount = 3;
        hug.selfPower = PowerId::STRENGTH; hug.selfPowerAmount = 3; hug.next = 1;
        MoveState pummel; pummel.moveId = "FURIOUS_PUMMELING_MOVE";
        pummel.damage = 4; pummel.hits = 4; pummel.next = 2;
        d.states = {vomit, smother, hug, pummel};
        defs.push_back(d);
    }
    // AXEBOT (Models.Monsters/Axebot.cs): HP 70-78 (+10 per respawn), Stock 2;
    // OWL_MAGISTRATE (OwlMagistrate.cs, act-3 normal): HP 231; SCRUTINY 16 -> PECK_ASSAULT
    // 4x6 -> JUDICIAL_FLIGHT (Soar 1: incoming powered attacks halved) -> VERDICT 33
    // (+Vulnerable 4, Soar removed) -> SCRUTINY...
    {
        MonsterDef d;
        d.name = "OWL_MAGISTRATE";
        d.minHp = 231; d.maxHp = 231;
        MoveState scrutiny; scrutiny.moveId = "MAGISTRATE_SCRUTINY"; scrutiny.damage = 16;
        scrutiny.next = 1;
        MoveState peck; peck.moveId = "PECK_ASSAULT"; peck.damage = 4; peck.hits = 6;
        peck.next = 2;
        MoveState flight; flight.moveId = "JUDICIAL_FLIGHT";
        flight.selfPower = PowerId::SOAR; flight.selfPowerAmount = 1; flight.next = 3;
        MoveState verdict; verdict.moveId = "VERDICT"; verdict.damage = 33;
        verdict.playerPower = PowerId::VULNERABLE; verdict.playerPowerAmount = 4;
        verdict.removeSelfSoar = true; verdict.next = 0;
        d.states = {scrutiny, peck, flight, verdict};
        defs.push_back(d);
    }
    // FROG_KNIGHT (FrogKnight.cs, act-3 normal): HP 191, Plating 15; TONGUE_LASH 13
    // (+Frail 2) -> STRIKE_DOWN_EVIL 21 -> FOR_THE_QUEEN (+5 Strength) -> HALF_HEALTH
    // (TONGUE_LASH while >= 50% HP or already charged, else BEETLE_CHARGE 35) -> ...
    {
        MonsterDef d;
        d.name = "FROG_KNIGHT";
        d.minHp = 191; d.maxHp = 191;
        d.startBlock = 15; // PlatingPower.BeforeSideTurnStart (round 1)
        d.startPower = PowerId::PLATING; d.startPowerAmount = 15;
        MoveState lash; lash.moveId = "TONGUE_LASH"; lash.damage = 13;
        lash.playerPower = PowerId::FRAIL; lash.playerPowerAmount = 2; lash.next = 1;
        MoveState strike; strike.moveId = "STRIKE_DOWN_EVIL"; strike.damage = 21;
        strike.next = 2;
        MoveState queen; queen.moveId = "FOR_THE_QUEEN";
        queen.selfPower = PowerId::STRENGTH; queen.selfPowerAmount = 5; queen.next = 3;
        MoveState half; half.moveId = "HALF_HEALTH";
        half.condPredicate = 4; half.condTrue = 0; half.condFalse = 4;
        MoveState charge; charge.moveId = "BEETLE_CHARGE"; charge.damage = 35;
        charge.markBeetleCharged = true; charge.next = 0;
        d.states = {lash, strike, queen, half, charge};
        defs.push_back(d);
    }
    // LIVING_SHIELD (LivingShield.cs, act-3 weak): HP 55, Rampart 25 (gives each
    // TurretOperator 25 unpowered block at the player's turn start); SHIELD_SLAM 6 ->
    // branch (an ally lives ? SHIELD_SLAM : SMASH 16 +3 Strength) -> ...
    {
        MonsterDef d;
        d.name = "LIVING_SHIELD";
        d.minHp = 55; d.maxHp = 55;
        d.startPower = PowerId::RAMPART; d.startPowerAmount = 25;
        d.rampartShield = true;
        MoveState slam; slam.moveId = "SHIELD_SLAM_MOVE"; slam.damage = 6; slam.next = 2;
        MoveState smash; smash.moveId = "SMASH_MOVE"; smash.damage = 16;
        smash.selfPower = PowerId::STRENGTH; smash.selfPowerAmount = 3; smash.next = 1;
        MoveState branch; branch.moveId = "SHIELD_SLAM_BRANCH";
        branch.condPredicate = 5; branch.condTrue = 0; branch.condFalse = 1;
        d.states = {slam, smash, branch};
        defs.push_back(d);
    }
    // TURRET_OPERATOR (TurretOperator.cs, act-3 weak): HP 41; UNLOAD 3x5 -> UNLOAD_2 3x5
    // -> RELOAD (+1 Strength) -> UNLOAD...
    {
        MonsterDef d;
        d.name = "TURRET_OPERATOR";
        d.minHp = 41; d.maxHp = 41;
        MoveState unload; unload.moveId = "UNLOAD_MOVE"; unload.damage = 3; unload.hits = 5;
        unload.next = 1;
        MoveState unload2; unload2.moveId = "UNLOAD_MOVE_2"; unload2.damage = 3;
        unload2.hits = 5; unload2.next = 2;
        MoveState reload; reload.moveId = "RELOAD_MOVE";
        reload.selfPower = PowerId::STRENGTH; reload.selfPowerAmount = 1; reload.next = 0;
        d.states = {unload, unload2, reload};
        defs.push_back(d);
    }
    // THE_LOST (TheLost.cs, act-3 normal pair): HP 93, PossessStrength 1;
    // DEBILITATING_SMOG (player -2 Str, self +2 Str) <-> EYE_LASERS 4x2.
    {
        MonsterDef d;
        d.name = "THE_LOST";
        d.minHp = 93; d.maxHp = 93;
        d.startPower = PowerId::POSSESS_STRENGTH; d.startPowerAmount = 1;
        MoveState smog; smog.moveId = "DEBILITATING_SMOG"; smog.stealStrength = 2;
        smog.next = 1;
        MoveState eyes; eyes.moveId = "EYE_LASERS"; eyes.damage = 4; eyes.hits = 2;
        eyes.next = 0;
        d.states = {smog, eyes};
        defs.push_back(d);
    }
    // THE_FORGOTTEN (TheForgotten.cs, act-3 normal pair): HP 106, PossessSpeed 1;
    // MIASMA (player -2 Dex, +8 block, self +2 Dex) <-> DREAD (13 + own Dexterity).
    {
        MonsterDef d;
        d.name = "THE_FORGOTTEN";
        d.minHp = 106; d.maxHp = 106;
        d.startPower = PowerId::POSSESS_SPEED; d.startPowerAmount = 1;
        MoveState miasma; miasma.moveId = "MIASMA"; miasma.stealDexterity = 2;
        miasma.selfBlock = 8; miasma.selfBlockAddsSelfDexterity = true; miasma.next = 1;
        MoveState dread; dread.moveId = "DREAD"; dread.damage = 13;
        dread.damageAddsSelfDexterity = true; dread.next = 0;
        d.states = {miasma, dread};
        defs.push_back(d);
    }
    // HAMMER 14 + weak2 + frail2 -> ONE_TWO 10x2 -> HAMMER...; respawn body starts BOOT_UP
    // (block 10, strength +3 per respawn already done).
    {
        MonsterDef d;
        d.name = "AXEBOT";
        d.minHp = 70; d.maxHp = 78;
        d.startPower = PowerId::STOCK; d.startPowerAmount = 2;
        d.respawns = true;
        MoveState hammer; hammer.moveId = "HAMMER_UPPERCUT_MOVE"; hammer.damage = 14; hammer.next = 1;
        hammer.playerPower = PowerId::WEAK; hammer.playerPowerAmount = 2;
        hammer.playerPower2 = PowerId::FRAIL; hammer.playerPower2Amount = 2;
        MoveState onetwo; onetwo.moveId = "ONE_TWO_MOVE"; onetwo.damage = 10; onetwo.hits = 2; onetwo.next = 0;
        // BootUpMove: block + strength x RespawnCount (3 per respawn, A9: 4).
        MoveState boot; boot.moveId = "BOOT_UP_MOVE"; boot.selfBlock = 10;
        boot.selfPower = PowerId::STRENGTH; boot.selfPowerAmount = 3; boot.next = 1;
        d.states = {hammer, onetwo, boot}; // first body starts HAMMER (idx 0); respawn starts BOOT_UP (idx 2)
        defs.push_back(d);
    }
    // BOWLBUG_ROCK (BowlbugRock.cs): HP 45-48, Imbalanced 1; HEADBUTT 15; if the attack is
    // fully blocked -> off-balance -> DIZZY (stun recovery) -> HEADBUTT.
    {
        MonsterDef d;
        d.name = "BOWLBUG_ROCK";
        d.minHp = 45; d.maxHp = 48;
        d.startPower = PowerId::IMBALANCED; d.startPowerAmount = 1;
        MoveState head; head.moveId = "HEADBUTT_MOVE"; head.damage = 15; head.next = 0; // loops; off-balance -> DIZZY
        MoveState dizzy; dizzy.moveId = "DIZZY_MOVE"; dizzy.next = 0;
        d.states = {head, dizzy};
        defs.push_back(d);
    }
    // BOWLBUG_EGG (BowlbugEgg.cs): HP 21-22, BITE 7 (+7 self block) self-loop.
    {
        MonsterDef d;
        d.name = "BOWLBUG_EGG";
        d.minHp = 21; d.maxHp = 22;
        MoveState bite; bite.moveId = "BITE_MOVE"; bite.damage = 7; bite.selfBlock = 7; bite.next = 0;
        d.states = {bite};
        defs.push_back(d);
    }
    // NIBBIT (Models.Monsters/Nibbit.cs): HP 42-46 (ascension 0: butt 12, slice 6+block 5,
    // hiss +2 strength). NIBBITS_WEAK is a single IsAlone nibbit, so the INIT conditional
    // always lands on BUTT; the follow-up chain is BUTT -> SLICE -> HISS -> BUTT.
    {
        MonsterDef d;
        d.name = "NIBBIT";
        d.minHp = 42; d.maxHp = 46;
        MoveState butt;  butt.moveId = "BUTT_MOVE";  butt.damage = 12; butt.next = 1;
        MoveState slice; slice.moveId = "SLICE_MOVE"; slice.damage = 6; slice.selfBlock = 5; slice.next = 2;
        MoveState hiss;  hiss.moveId = "HISS_MOVE";  hiss.selfPower = PowerId::STRENGTH; hiss.selfPowerAmount = 2; hiss.next = 0;
        d.states = {butt, slice, hiss};
        defs.push_back(d);
    }
    // BOWLBUG_NECTAR (BowlbugNectar.cs): HP 35-38; THRASH 3 -> BUFF (+15 str) -> THRASH2 3 loop.
    {
        MonsterDef d;
        d.name = "BOWLBUG_NECTAR";
        d.minHp = 35; d.maxHp = 38;
        MoveState thrash;  thrash.moveId = "THRASH_MOVE"; thrash.damage = 3; thrash.next = 1;
        MoveState buff;    buff.moveId = "BUFF_MOVE"; buff.selfPower = PowerId::STRENGTH;
        buff.selfPowerAmount = 15; buff.next = 2;
        MoveState thrash2; thrash2.moveId = "THRASH2_MOVE"; thrash2.damage = 3; thrash2.next = 2;
        d.states = {thrash, buff, thrash2};
        defs.push_back(d);
    }
    // DECIMILLIPEDE segments (DecimillipedeSegment.cs): HP 40-46 (dedup/even-ized at spawn),
    // Reattach 25; CONSTRICT 8 (+weak1) -> BULK 6 (+str2) -> WRITHE 5x2 -> CONSTRICT...
    // death -> DEAD -> REATTACH (+25, only while another segment lives) -> RAND (equal
    // 3-way, CannotRepeat) back into the ring.
    {
        auto seg = [](const char* name) {
            MonsterDef d;
            d.name = name;
            d.minHp = 40; d.maxHp = 46;
            d.startPower = PowerId::REATTACH; d.startPowerAmount = 25;
            d.deathState = 3;
            d.reviveHeal = 25;
            d.reviveOnlyIfOthersAlive = true;
            MoveState constrict; constrict.moveId = "CONSTRICT_MOVE"; constrict.damage = 8; constrict.next = 1;
            constrict.playerPower = PowerId::WEAK; constrict.playerPowerAmount = 1;
            MoveState bulk; bulk.moveId = "BULK_MOVE"; bulk.damage = 6;
            bulk.selfPower = PowerId::STRENGTH; bulk.selfPowerAmount = 2; bulk.next = 2;
            MoveState writhe; writhe.moveId = "WRITHE_MOVE"; writhe.damage = 5; writhe.hits = 2; writhe.next = 0;
        MoveState dead; dead.moveId = "DEAD_MOVE"; dead.next = 4; dead.noLog = true;
        MoveState reattach; reattach.moveId = "REATTACH_MOVE"; reattach.healSelf = 25; reattach.next = 5; reattach.noLog = true;
            MoveState rnd; rnd.moveId = "RAND";
            rnd.branch = {{1.0f, 2}, {1.0f, 1}, {1.0f, 0}};
            d.states = {constrict, bulk, writhe, dead, reattach, rnd};
            return d;
        };
        defs.push_back(seg("DECIMILLIPEDE_SEGMENT_FRONT"));
        defs.push_back(seg("DECIMILLIPEDE_SEGMENT_MIDDLE"));
        defs.push_back(seg("DECIMILLIPEDE_SEGMENT_BACK"));
    }
    // FOGMOG (Fogmog.cs): HP 74 fixed; ILLUSION (summon eye at slot "illusion") -> SWIPE 8
    // (+str1) -> BRANCH (0.4 SWIPE_RANDOM / 0.6 HEADBUTT, CannotRepeat) loop via HEADBUTT.
    {
        MonsterDef d;
        d.name = "FOGMOG";
        d.minHp = 74; d.maxHp = 74;
        MoveState illusion; illusion.moveId = "ILLUSION_MOVE"; illusion.next = 1;
        MoveState swipe; swipe.moveId = "SWIPE_MOVE"; swipe.damage = 8;
        swipe.selfPower = PowerId::STRENGTH; swipe.selfPowerAmount = 1; swipe.next = 3;
        MoveState swipeRand; swipeRand.moveId = "SWIPE_RANDOM_MOVE"; swipeRand.damage = 8;
        swipeRand.selfPower = PowerId::STRENGTH; swipeRand.selfPowerAmount = 1; swipeRand.next = 4;
        MoveState branch; branch.moveId = "BRANCH";
        branch.branch = {{0.4f, 2}, {0.6f, 4}};
        MoveState headbutt; headbutt.moveId = "HEADBUTT_MOVE"; headbutt.damage = 14; headbutt.next = 1;
        d.states = {illusion, swipe, swipeRand, branch, headbutt};
        defs.push_back(d);
    }
    // EYE_WITH_TEETH (EyeWithTeeth.cs): HP 6, Illusion + Minion; DISTRACT adds 3 Dazed to
    // the player's discard bottom; killed -> REVIVE (heal to full) -> DISTRACT.
    {
        MonsterDef d;
        d.name = "EYE_WITH_TEETH";
        d.minHp = 6; d.maxHp = 6;
        d.startPower = PowerId::ILLUSION; d.startPowerAmount = 1;
        d.startPower2 = PowerId::MINION; d.startPower2Amount = 1;
        d.minion = true;
        d.deathState = 1;
        MoveState distract; distract.moveId = "DISTRACT_MOVE"; distract.addDazedToDiscard = 3; distract.next = 0;
        MoveState revive; revive.moveId = "REVIVE_MOVE"; revive.healSelf = -1; revive.next = 0; revive.noLog = true;
        d.states = {distract, revive};
        defs.push_back(d);
    }
    // FUZZY_WURM_CRAWLER (FuzzyWurmCrawler.cs): HP 55-57 (single monster);
    // FIRST_ACID 4 -> INHALE (+7 str) -> ACID 4 -> FIRST...
    {
        MonsterDef d;
        d.name = "FUZZY_WURM_CRAWLER";
        d.minHp = 55; d.maxHp = 57;
        MoveState first; first.moveId = "FIRST_ACID_GOOP"; first.damage = 4; first.next = 1;
        MoveState inhale; inhale.moveId = "INHALE"; inhale.selfPower = PowerId::STRENGTH;
        inhale.selfPowerAmount = 7; inhale.next = 2;
        MoveState acid; acid.moveId = "ACID_GOOP"; acid.damage = 4; acid.next = 0;
        d.states = {first, inhale, acid};
        defs.push_back(d);
    }
    // LEAF_SLIME_S (LeafSlimeS.cs): HP 11-15; TACKLE 3 -> GOOP (1 Slimed to discard) ->
    // RAND (equal, CannotRepeat between TACKLE/GOOP).
    {
        MonsterDef d;
        d.name = "LEAF_SLIME_S";
        d.minHp = 11; d.maxHp = 15;
        MoveState tackle; tackle.moveId = "TACKLE_MOVE"; tackle.damage = 3; tackle.next = 2;
        MoveState goop; goop.moveId = "GOOP_MOVE"; goop.next = 2;
        goop.addCardToDiscard = CardId::SLIMED; goop.addCardToDiscardCount = 1;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 0}, {1.0f, 1}};
        d.states = {tackle, goop, rnd};
        d.startState = 2;
        defs.push_back(d);
    }
    // LEAF_SLIME_M (LeafSlimeM.cs): HP 32-35; CLUMP_SHOT 8 <-> STICKY_SHOT (2 Slimed).
    {
        MonsterDef d;
        d.name = "LEAF_SLIME_M";
        d.minHp = 32; d.maxHp = 35;
        MoveState clump; clump.moveId = "CLUMP_SHOT"; clump.damage = 8; clump.next = 1;
        MoveState sticky; sticky.moveId = "STICKY_SHOT"; sticky.next = 0;
        sticky.addCardToDiscard = CardId::SLIMED; sticky.addCardToDiscardCount = 2;
        d.states = {clump, sticky};
        d.startState = 1;
        defs.push_back(d);
    }
    // TWIG_SLIME_S (TwigSlimeS.cs): HP 7-11; TACKLE 4, always.
    {
        MonsterDef d;
        d.name = "TWIG_SLIME_S";
        d.minHp = 7; d.maxHp = 11;
        MoveState tackle; tackle.moveId = "TACKLE_MOVE"; tackle.damage = 4; tackle.next = 0;
        d.states = {tackle};
        defs.push_back(d);
    }
    // TWIG_SLIME_M (TwigSlimeM.cs): HP 26-28; POKEY_POUNCE 11 / STICKY_SHOT (1 Slimed)
    // -> RAND (POKEY weight 2 vs STICKY weight 1, STICKY CannotRepeat).
    {
        MonsterDef d;
        d.name = "TWIG_SLIME_M";
        d.minHp = 26; d.maxHp = 28;
        MoveState poke; poke.moveId = "POKEY_POUNCE_MOVE"; poke.damage = 11; poke.next = 2;
        MoveState sticky; sticky.moveId = "STICKY_SHOT_MOVE"; sticky.next = 2;
        sticky.addCardToDiscard = CardId::SLIMED; sticky.addCardToDiscardCount = 1;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 0, 2}, {1.0f, 1, 1}};
        d.states = {poke, sticky, rnd};
        d.startState = 1;
        defs.push_back(d);
    }
    // SHRINKER_BEETLE (ShrinkerBeetle.cs): HP 38-40; SHRINKER (ShrinkPower -1 = infinite,
    // removed when the applier dies) -> CHOMP 7 <-> STOMP 13.
    {
        MonsterDef d;
        d.name = "SHRINKER_BEETLE";
        d.minHp = 38; d.maxHp = 40;
        MoveState shrink; shrink.moveId = "SHRINKER_MOVE"; shrink.next = 1;
        shrink.playerPower = PowerId::SHRINK; shrink.playerPowerAmount = -1;
        MoveState chomp; chomp.moveId = "CHOMP_MOVE"; chomp.damage = 7; chomp.next = 2;
        MoveState stomp; stomp.moveId = "STOMP_MOVE"; stomp.damage = 13; stomp.next = 1;
        d.states = {shrink, chomp, stomp};
        defs.push_back(d);
    }
    // FLYCONID (Flyconid.cs): HP 47-49; INITIAL branch (FRAIL cooldown 2 / SMASH), then
    // VULN(+2 vuln, cd3) / FRAIL(8 +2 frail, cd2) / SMASH(11) via RAND.
    {
        MonsterDef d;
        d.name = "FLYCONID";
        d.minHp = 47; d.maxHp = 49;
        MoveState vuln; vuln.moveId = "VULNERABLE_SPORES_MOVE"; vuln.next = 3;
        vuln.playerPower = PowerId::VULNERABLE; vuln.playerPowerAmount = 2;
        MoveState frail; frail.moveId = "FRAIL_SPORES_MOVE"; frail.damage = 8; frail.next = 3;
        frail.playerPower = PowerId::FRAIL; frail.playerPowerAmount = 2;
        MoveState smash; smash.moveId = "SMASH_MOVE"; smash.damage = 11; smash.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 0, 1, 3}, {1.0f, 1, 1, 2}, {1.0f, 2, 1, 0}};
        MoveState init; init.moveId = "INITIAL";
        init.branch = {{1.0f, 1, 1, 2}, {1.0f, 2, 1, 0}};
        d.states = {vuln, frail, smash, rnd, init};
        d.startState = 4;
        defs.push_back(d);
    }
    // SNAPPING_JAXFRUIT (SnappingJaxfruit.cs): HP 31-33; ENERGY_ORB 3 + self +2 strength.
    {
        MonsterDef d;
        d.name = "SNAPPING_JAXFRUIT";
        d.minHp = 31; d.maxHp = 33;
        MoveState orb; orb.moveId = "ENERGY_ORB_MOVE"; orb.damage = 3; orb.next = 0;
        orb.selfPower = PowerId::STRENGTH; orb.selfPowerAmount = 2;
        d.states = {orb};
        defs.push_back(d);
    }
    // MAWLER (Mawler.cs): HP 72; CLAW 4x2 / RIP 14 / ROAR (+3 vuln, UseOnlyOnce) via RAND.
    {
        MonsterDef d;
        d.name = "MAWLER";
        d.minHp = 72; d.maxHp = 72;
        MoveState rip; rip.moveId = "RIP_AND_TEAR_MOVE"; rip.damage = 14; rip.next = 3;
        MoveState roar; roar.moveId = "ROAR_MOVE"; roar.next = 3;
        roar.playerPower = PowerId::VULNERABLE; roar.playerPowerAmount = 3;
        MoveState claw; claw.moveId = "CLAW_MOVE"; claw.damage = 4; claw.hits = 2; claw.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 0, 1, 0, false}, {1.0f, 1, 1, 0, true}, {1.0f, 2, 1, 0, false}};
        d.states = {rip, roar, claw, rnd};
        d.startState = 2;
        defs.push_back(d);
    }
    // INKLET (Inklet.cs): HP 11-17; Slippery 1; JAB 3 -> RAND(GAZE 10 / WHIRLWIND 2x3);
    // middle inklet starts on WHIRLWIND (starterMoveIdx = 2 from the encounter).
    {
        MonsterDef d;
        d.name = "INKLET";
        d.minHp = 11; d.maxHp = 17;
        d.startPower = PowerId::SLIPPERY; d.startPowerAmount = 1;
        MoveState jab; jab.moveId = "JAB_MOVE"; jab.damage = 3; jab.next = 3;
        MoveState gaze; gaze.moveId = "PIERCING_GAZE_MOVE"; gaze.damage = 10; gaze.next = 0;
        MoveState whirl; whirl.moveId = "WHIRLWIND_MOVE"; whirl.damage = 2; whirl.hits = 3;
        whirl.next = 0;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 1}, {1.0f, 2}};
        d.states = {jab, gaze, whirl, rnd};
        defs.push_back(d);
    }
    // BYGONE_EFFIGY (BygoneEffigy.cs): HP 127; Slow 1; SLEEP -> WAKE (+10 str) ->
    // SLASHES 13 self-loop (initial SLEEP_MOVE has no effect).
    {
        MonsterDef d;
        d.name = "BYGONE_EFFIGY";
        d.minHp = 127; d.maxHp = 127;
        d.startPower = PowerId::SLOW; d.startPowerAmount = 1;
        MoveState sleep1; sleep1.moveId = "SLEEP_MOVE"; sleep1.next = 1;
        MoveState wake; wake.moveId = "WAKE_MOVE"; wake.next = 3;
        wake.selfPower = PowerId::STRENGTH; wake.selfPowerAmount = 10;
        MoveState sleep2; sleep2.moveId = "SLEEP_MOVE_2"; sleep2.next = 3;
        MoveState slash; slash.moveId = "SLASHES_MOVE"; slash.damage = 13; slash.next = 3;
        d.states = {sleep1, wake, sleep2, slash};
        defs.push_back(d);
    }
    // CUBEX_CONSTRUCT (CubexConstruct.cs): HP 65; enters with 13 block + Artifact 1;
    // CHARGE(+2 str) -> BLAST 7(+2 str) -> BLAST 7(+2 str) -> EXPEL 5x2 -> BLAST...
    {
        MonsterDef d;
        d.name = "CUBEX_CONSTRUCT";
        d.minHp = 65; d.maxHp = 65;
        d.startBlock = 13;
        d.startPower = PowerId::ARTIFACT; d.startPowerAmount = 1;
        MoveState charge; charge.moveId = "CHARGE_UP_MOVE"; charge.next = 1;
        charge.selfPower = PowerId::STRENGTH; charge.selfPowerAmount = 2;
        MoveState blast; blast.moveId = "REPEATER_BLAST_MOVE"; blast.damage = 7;
        blast.selfPower = PowerId::STRENGTH; blast.selfPowerAmount = 2; blast.next = 2;
        MoveState blast2; blast2.moveId = "REPEATER_BLAST_MOVE_2"; blast2.damage = 7;
        blast2.selfPower = PowerId::STRENGTH; blast2.selfPowerAmount = 2; blast2.next = 3;
        MoveState expel; expel.moveId = "EXPEL_MOVE"; expel.damage = 5; expel.hits = 2;
        expel.next = 1;
        d.states = {charge, blast, blast2, expel};
        defs.push_back(d);
    }
    // SLITHERING_STRANGLER (SlitheringStrangler.cs): HP 53-55; CONSTRICT (+3 player
    // constrict) -> RAND(THWACK 7 +5 block / LASH 12, CanRepeatForever) -> CONSTRICT...
    {
        MonsterDef d;
        d.name = "SLITHERING_STRANGLER";
        d.minHp = 53; d.maxHp = 55;
        MoveState constrict; constrict.moveId = "CONSTRICT"; constrict.next = 3;
        constrict.playerPower = PowerId::CONSTRICT; constrict.playerPowerAmount = 3;
        MoveState thwack; thwack.moveId = "THWACK"; thwack.damage = 7; thwack.selfBlock = 5;
        thwack.next = 0;
        MoveState lash; lash.moveId = "LASH"; lash.damage = 12; lash.next = 0;
        MoveState rnd; rnd.moveId = "rand"; rnd.branch = {{1.0f, 1, 0}, {1.0f, 2, 0}};
        d.states = {constrict, thwack, lash, rnd};
        defs.push_back(d);
    }
    // VINE_SHAMBLER (VineShambler.cs): HP 61; SWIPE 6x2 -> GRASPING_VINES 8 (+1 Tangled:
    // every Attack card gains Entangled +1 cost until the player side ends) -> CHOMP 16.
    {
        MonsterDef d;
        d.name = "VINE_SHAMBLER";
        d.minHp = 61; d.maxHp = 61;
        MoveState vines; vines.moveId = "GRASPING_VINES_MOVE"; vines.damage = 8; vines.next = 2;
        vines.playerPower = PowerId::TANGLED; vines.playerPowerAmount = 1;
        MoveState swipe; swipe.moveId = "SWIPE_MOVE"; swipe.damage = 6; swipe.hits = 2;
        swipe.next = 0;
        MoveState chomp; chomp.moveId = "CHOMP_MOVE"; chomp.damage = 16; chomp.next = 1;
        d.states = {vines, swipe, chomp};
        d.startState = 1;
        defs.push_back(d);
    }
    // PHROG_PARASITE (PhrogParasite.cs): HP 61-64; entry Infested 4 (spawns 4 Wrigglers on
    // death, combat cannot end while they stand); INFECT (3 Infection to discard) <-> LASH 4x4.
    {
        MonsterDef d;
        d.name = "PHROG_PARASITE";
        d.minHp = 61; d.maxHp = 64;
        d.startPower = PowerId::INFESTED; d.startPowerAmount = 4;
        MoveState infect; infect.moveId = "INFECT_MOVE"; infect.next = 1;
        infect.addCardToDiscard = CardId::INFECTION; infect.addCardToDiscardCount = 3;
        MoveState lash; lash.moveId = "LASH_MOVE"; lash.damage = 4; lash.hits = 4; lash.next = 0;
        d.states = {infect, lash};
        defs.push_back(d);
    }
    // WRIGGLER (Wriggler.cs): HP 17-21; starts SPAWNED (stun, skips its first action), then
    // INIT by slot parity: wriggler1/3 BITE 6 -> WRIGGLE (+2 str, 1 Infection) -> BITE;
    // wriggler2/4 start on WRIGGLE.
    {
        MonsterDef d;
        d.name = "WRIGGLER";
        d.minHp = 17; d.maxHp = 21;
        MoveState spawned; spawned.moveId = "SPAWNED_MOVE"; spawned.next = 3;
        MoveState bite; bite.moveId = "NASTY_BITE_MOVE"; bite.damage = 6; bite.next = 2;
        MoveState wriggle; wriggle.moveId = "WRIGGLE_MOVE"; wriggle.next = 1;
        wriggle.selfPower = PowerId::STRENGTH; wriggle.selfPowerAmount = 2;
        wriggle.addCardToDiscard = CardId::INFECTION; wriggle.addCardToDiscardCount = 1;
        MoveState init; init.moveId = "INIT_MOVE"; init.slotInit = true;
        d.states = {spawned, bite, wriggle, init};
        d.startState = 3;
        defs.push_back(d);
    }
    // RUBY RAIDERS (Axe/Assassin/Brute/Crossbow/TrackerRubyRaider.cs). Simple chains:
    // AXE: SWING 5(+5 block) x2 -> BIG_SWING 12; ASSASSIN: KILLSHOT 10; BRUTE: BEAT 7 ->
    // ROAR(+3 str); CROSSBOW: RELOAD(+3 block) -> FIRE 14; TRACKER: TRACK(+2 frail) ->
    // HOUNDS 1x8 self-loop.
    {
        MonsterDef d;
        d.name = "AXE_RUBY_RAIDER";
        d.minHp = 20; d.maxHp = 22;
        MoveState s1; s1.moveId = "SWING_1"; s1.damage = 5; s1.selfBlock = 5; s1.next = 1;
        MoveState s2; s2.moveId = "SWING_2"; s2.damage = 5; s2.selfBlock = 5; s2.next = 2;
        MoveState big; big.moveId = "BIG_SWING"; big.damage = 12; big.next = 0;
        d.states = {s1, s2, big};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "ASSASSIN_RUBY_RAIDER";
        d.minHp = 18; d.maxHp = 23;
        MoveState kill; kill.moveId = "KILLSHOT_MOVE"; kill.damage = 10; kill.next = 0;
        d.states = {kill};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "BRUTE_RUBY_RAIDER";
        d.minHp = 30; d.maxHp = 33;
        MoveState beat; beat.moveId = "BEAT_MOVE"; beat.damage = 7; beat.next = 1;
        MoveState roar; roar.moveId = "ROAR_MOVE"; roar.next = 0;
        roar.selfPower = PowerId::STRENGTH; roar.selfPowerAmount = 3;
        d.states = {beat, roar};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "CROSSBOW_RUBY_RAIDER";
        d.minHp = 18; d.maxHp = 21;
        MoveState reload; reload.moveId = "RELOAD_MOVE"; reload.selfBlock = 3; reload.next = 1;
        MoveState fire; fire.moveId = "FIRE_MOVE"; fire.damage = 14; fire.next = 0;
        d.states = {reload, fire};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "TRACKER_RUBY_RAIDER";
        d.minHp = 21; d.maxHp = 25;
        MoveState track; track.moveId = "TRACK_MOVE"; track.next = 1;
        track.playerPower = PowerId::FRAIL; track.playerPowerAmount = 2;
        MoveState hounds; hounds.moveId = "HOUNDS_MOVE"; hounds.damage = 1; hounds.hits = 8;
        hounds.next = 1;
        d.states = {track, hounds};
        defs.push_back(d);
    }
    // VANTOM (Vantom.cs, act-1 boss): HP 173; enters with Slippery 8; INK_BLOT 7 ->
    // INKY_LANCE 6x2 -> DISMEMBER 26 (+3 Wounds to discard) -> PREPARE (+2 strength).
    {
        MonsterDef d;
        d.name = "VANTOM";
        d.minHp = 173; d.maxHp = 173;
        d.startPower = PowerId::SLIPPERY; d.startPowerAmount = 8;
        MoveState blot; blot.moveId = "INK_BLOT_MOVE"; blot.damage = 7; blot.next = 1;
        MoveState lance; lance.moveId = "INKY_LANCE_MOVE"; lance.damage = 6; lance.hits = 2;
        lance.next = 2;
        MoveState dismember; dismember.moveId = "DISMEMBER_MOVE"; dismember.damage = 26;
        dismember.next = 3;
        dismember.addCardToDiscard = CardId::WOUND; dismember.addCardToDiscardCount = 3;
        MoveState prepare; prepare.moveId = "PREPARE_MOVE"; prepare.next = 0;
        prepare.selfPower = PowerId::STRENGTH; prepare.selfPowerAmount = 2;
        d.states = {blot, lance, dismember, prepare};
        defs.push_back(d);
    }
    // CEREMONIAL_BEAST (CeremonialBeast.cs, act-1 boss): HP 252; STAMP applies Plow 150,
    // then PLOW 18 (+2 str) loops until unblocked damage brings it to <=150, which removes
    // the strength, stuns it, and starts phase two: BEAST_CRY (Ringing 1) -> STOMP 15 ->
    // CRUSH 17 (+3 str) -> BEAST_CRY.
    {
        MonsterDef d;
        d.name = "CEREMONIAL_BEAST";
        d.minHp = 252; d.maxHp = 252;
        MoveState plow; plow.moveId = "PLOW_MOVE"; plow.damage = 18; plow.next = 0;
        plow.selfPower = PowerId::STRENGTH; plow.selfPowerAmount = 2;
        MoveState stamp; stamp.moveId = "STAMP_MOVE"; stamp.next = 0;
        stamp.selfPower = PowerId::PLOW; stamp.selfPowerAmount = 150;
        MoveState stun; stun.moveId = "STUN_MOVE"; stun.next = 3; stun.noLog = true;
        MoveState cry; cry.moveId = "BEAST_CRY_MOVE"; cry.next = 4;
        cry.playerPower = PowerId::RINGING; cry.playerPowerAmount = 1;
        MoveState stomp; stomp.moveId = "STOMP_MOVE"; stomp.damage = 15; stomp.next = 5;
        MoveState crush; crush.moveId = "CRUSH_MOVE"; crush.damage = 17; crush.next = 3;
        crush.selfPower = PowerId::STRENGTH; crush.selfPowerAmount = 3;
        d.states = {plow, stamp, stun, cry, stomp, crush};
        d.startState = 1;
        defs.push_back(d);
    }
    // THE KIN (KinFollower.cs / KinPriest.cs, act-1 boss): two followers (Minion, HP 58-59)
    // and the priest (HP 190). Follower: QUICK_SLASH 5 -> BOOMERANG 2x2 -> POWER_DANCE(+2
    // str) -> QUICK; the slot1 follower starts on POWER_DANCE. Priest: ORB_OF_FRAILTY 8
    // (+1 frail) -> ORB_OF_WEAKNESS 8 (+1 weak) -> BEAM 3x3 -> RITUAL(+2 str) -> loop.
    {
        MonsterDef d;
        d.name = "KIN_FOLLOWER";
        d.minHp = 58; d.maxHp = 59;
        d.startPower = PowerId::MINION; d.startPowerAmount = 1;
        d.minion = true;
        MoveState quick; quick.moveId = "QUICK_SLASH_MOVE"; quick.damage = 5; quick.next = 1;
        MoveState boom; boom.moveId = "BOOMERANG_MOVE"; boom.damage = 2; boom.hits = 2;
        boom.next = 2;
        MoveState dance; dance.moveId = "POWER_DANCE_MOVE"; dance.next = 0;
        dance.selfPower = PowerId::STRENGTH; dance.selfPowerAmount = 2;
        d.states = {quick, boom, dance};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "KIN_PRIEST";
        d.minHp = 190; d.maxHp = 190;
        MoveState frail; frail.moveId = "ORB_OF_FRAILTY_MOVE"; frail.damage = 8; frail.next = 1;
        frail.playerPower = PowerId::FRAIL; frail.playerPowerAmount = 1;
        MoveState weak; weak.moveId = "ORB_OF_WEAKNESS_MOVE"; weak.damage = 8; weak.next = 2;
        weak.playerPower = PowerId::WEAK; weak.playerPowerAmount = 1;
        MoveState beam; beam.moveId = "BEAM_MOVE"; beam.damage = 3; beam.hits = 3; beam.next = 3;
        MoveState ritual; ritual.moveId = "RITUAL_MOVE"; ritual.next = 0;
        ritual.selfPower = PowerId::STRENGTH; ritual.selfPowerAmount = 2;
        d.states = {frail, weak, beam, ritual};
        defs.push_back(d);
    }
    // EXOSKELETON (Exoskeleton.cs, act-2 weak): HP 24-28; enters with HardToKill 9
    // (incoming damage capped at 9); INIT is slot-conditional (first SKITTER 1x3,
    // second MANDIBLES 8, third ENRAGE +2 strength); afterwards RAND(SKITTER/MANDIBLES).
    {
        MonsterDef d;
        d.name = "EXOSKELETON";
        d.minHp = 24; d.maxHp = 28;
        d.startPower = PowerId::HARD_TO_KILL; d.startPowerAmount = 9;
        MoveState init; init.moveId = "INIT_MOVE"; init.slotInit = true; init.next = 1;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 2, 1}, {1.0f, 3, 1}};
        MoveState skitter; skitter.moveId = "SKITTER_MOVE"; skitter.damage = 1; skitter.hits = 3;
        skitter.next = 1;
        MoveState mandibles; mandibles.moveId = "MANDIBLES_MOVE"; mandibles.damage = 8;
        mandibles.next = 4;
        MoveState enrage; enrage.moveId = "ENRAGE_MOVE"; enrage.next = 1;
        enrage.selfPower = PowerId::STRENGTH; enrage.selfPowerAmount = 2;
        d.states = {init, rnd, skitter, mandibles, enrage};
        defs.push_back(d);
    }
    // TUNNELER (Tunneler.cs, act-2 weak): HP 87; BITE 13 -> BURROW (Burrowed + 32 block,
    // block does not clear) -> BELOW 23 self-loop; breaking the burrowed block forces
    // DIZZY -> BITE.
    {
        MonsterDef d;
        d.name = "TUNNELER";
        d.minHp = 87; d.maxHp = 87;
        MoveState bite; bite.moveId = "BITE_MOVE"; bite.damage = 13; bite.next = 1;
        MoveState burrow; burrow.moveId = "BURROW_MOVE"; burrow.selfBlock = 32; burrow.next = 2;
        burrow.selfPower = PowerId::BURROWED; burrow.selfPowerAmount = 1;
        MoveState below; below.moveId = "BELOW_MOVE"; below.damage = 23; below.next = 2;
        MoveState dizzy; dizzy.moveId = "DIZZY_MOVE"; dizzy.next = 0;
        d.states = {bite, burrow, below, dizzy};
        defs.push_back(d);
    }
    // CHOMPER (Chomper.cs, act-2): HP 60-64; Artifact 2; CLAMP 8x2 -> SCREECH (3 Dazed to
    // discard) -> CLAMP. The encounter's second Chomper starts on SCREECH.
    {
        MonsterDef d;
        d.name = "CHOMPER";
        d.minHp = 60; d.maxHp = 64;
        d.startPower = PowerId::ARTIFACT; d.startPowerAmount = 2;
        MoveState clamp; clamp.moveId = "CLAMP_MOVE"; clamp.damage = 8; clamp.hits = 2;
        clamp.next = 1;
        MoveState screech; screech.moveId = "SCREECH_MOVE"; screech.next = 0;
        screech.addDazedToDiscard = 3;
        d.states = {clamp, screech};
        defs.push_back(d);
    }
    // SPINY_TOAD (SpinyToad.cs, act-2): HP 116-119; PROTRUDING_SPIKES (Thorns 5) ->
    // SPIKE_EXPLOSION 23 (removes the 5 Thorns) -> TONGUE_LASH 17 -> loop.
    {
        MonsterDef d;
        d.name = "SPINY_TOAD";
        d.minHp = 116; d.maxHp = 119;
        MoveState spikes; spikes.moveId = "PROTRUDING_SPIKES_MOVE"; spikes.next = 1;
        spikes.selfPower = PowerId::THORNS; spikes.selfPowerAmount = 5;
        MoveState explosion; explosion.moveId = "SPIKE_EXPLOSION_MOVE"; explosion.damage = 23;
        explosion.next = 2;
        explosion.selfPower = PowerId::THORNS; explosion.selfPowerAmount = -5;
        MoveState lash; lash.moveId = "TONGUE_LASH_MOVE"; lash.damage = 17; lash.next = 0;
        d.states = {spikes, explosion, lash};
        defs.push_back(d);
    }
    // PARAFRIGHT (Parafright.cs, Obscura's illusion): HP 21 fixed; IllusionPower + Minion;
    // SLAM 16 self-loop; on death it stays in combat and REVIVEs to full HP next turn.
    {
        MonsterDef d;
        d.name = "PARAFRIGHT";
        d.minHp = 21; d.maxHp = 21;
        d.startPower = PowerId::ILLUSION; d.startPowerAmount = 1;
        d.startPower2 = PowerId::MINION; d.startPower2Amount = 1;
        d.minion = true;
        d.deathState = 1;
        MoveState slam; slam.moveId = "SLAM_MOVE"; slam.damage = 16; slam.next = 0;
        MoveState revive; revive.moveId = "REVIVE_MOVE"; revive.healSelf = -1; revive.next = 0;
        revive.noLog = true;
        d.states = {slam, revive};
        defs.push_back(d);
    }
    // CRUSHER (Crusher.cs, KaiserCrab left arm): HP 209 fixed; BackAttackLeft + CrabRage.
    // THRASH 12 -> ENLARGING_STRIKE 4 -> BUG_STING 6x2 (+2 weak, +2 frail) -> ADAPT (+2
    // strength) -> GUARDED_STRIKE 12 (+18 block) -> THRASH.
    {
        MonsterDef d;
        d.name = "CRUSHER";
        d.minHp = 209; d.maxHp = 209;
        d.startPower = PowerId::BACK_ATTACK_LEFT; d.startPowerAmount = 1;
        d.startPower2 = PowerId::CRAB_RAGE; d.startPower2Amount = 1;
        MoveState thrash; thrash.moveId = "THRASH_MOVE"; thrash.damage = 12; thrash.next = 1;
        MoveState enlarge; enlarge.moveId = "ENLARGING_STRIKE_MOVE"; enlarge.damage = 4;
        enlarge.next = 2;
        MoveState sting; sting.moveId = "BUG_STING_MOVE"; sting.damage = 6; sting.hits = 2;
        sting.next = 3;
        sting.playerPower = PowerId::WEAK; sting.playerPowerAmount = 2;
        sting.playerPower2 = PowerId::FRAIL; sting.playerPower2Amount = 2;
        MoveState adapt; adapt.moveId = "ADAPT_MOVE"; adapt.next = 4;
        adapt.selfPower = PowerId::STRENGTH; adapt.selfPowerAmount = 2;
        MoveState guarded; guarded.moveId = "GUARDED_STRIKE_MOVE"; guarded.damage = 12;
        guarded.selfBlock = 18; guarded.next = 0;
        d.states = {thrash, enlarge, sting, adapt, guarded};
        defs.push_back(d);
    }
    // ROCKET (Rocket.cs, KaiserCrab right arm): HP 199 fixed; applies Surrounded to the
    // player + BackAttackRight + CrabRage. TARGETING_RETICLE 3 -> PRECISION_BEAM 18 ->
    // CHARGE_UP (+2 strength) -> LASER 31 -> RECHARGE (no effect) -> TARGETING.
    {
        MonsterDef d;
        d.name = "ROCKET";
        d.minHp = 199; d.maxHp = 199;
        d.startPlayerPower = PowerId::SURROUNDED; d.startPlayerPowerAmount = 1;
        d.startPower = PowerId::BACK_ATTACK_RIGHT; d.startPowerAmount = 1;
        d.startPower2 = PowerId::CRAB_RAGE; d.startPower2Amount = 1;
        MoveState reticle; reticle.moveId = "TARGETING_RETICLE_MOVE"; reticle.damage = 3;
        reticle.next = 1;
        MoveState beam; beam.moveId = "PRECISION_BEAM_MOVE"; beam.damage = 18; beam.next = 2;
        MoveState charge; charge.moveId = "CHARGE_UP_MOVE"; charge.next = 3;
        charge.selfPower = PowerId::STRENGTH; charge.selfPowerAmount = 2;
        MoveState laser; laser.moveId = "LASER_MOVE"; laser.damage = 31; laser.next = 4;
        MoveState recharge; recharge.moveId = "RECHARGE_MOVE"; recharge.next = 0;
        d.states = {reticle, beam, charge, laser, recharge};
        defs.push_back(d);
    }
    // KNOWLEDGE_DEMON (KnowledgeDemon.cs, act-2 boss): HP 379 fixed; CURSE_OF_KNOWLEDGE
    // (the player picks one of two curses: Disintegration 6/7/8, MindRot 1, Sloth 3,
    // WasteAway 1) -> SLAP 17 -> KNOWLEDGE_OVERWHELMING 8x3 -> PONDER 11 (heal 30, +2
    // strength) -> branch (curse counter < 3 ? CURSE : SLAP) -> ...
    {
        MonsterDef d;
        d.name = "KNOWLEDGE_DEMON";
        d.minHp = 379; d.maxHp = 379;
        MoveState curse; curse.moveId = "CURSE_OF_KNOWLEDGE_MOVE"; curse.next = 1;
        curse.curseChoice = true;
        MoveState slap; slap.moveId = "SLAP_MOVE"; slap.damage = 17; slap.next = 2;
        MoveState overwhelming; overwhelming.moveId = "KNOWLEDGE_OVERWHELMING_MOVE";
        overwhelming.damage = 8; overwhelming.hits = 3; overwhelming.next = 3;
        MoveState ponder; ponder.moveId = "PONDER_MOVE"; ponder.damage = 11; ponder.next = 4;
        ponder.healSelf = 30;
        ponder.selfPower = PowerId::STRENGTH; ponder.selfPowerAmount = 2;
        MoveState branch; branch.moveId = "CurseOfKnowledgeBranch";
        branch.condPredicate = 3; branch.condTrue = 0; branch.condFalse = 1;
        d.states = {curse, slap, overwhelming, ponder, branch};
        defs.push_back(d);
    }
    // THE_INSATIABLE (TheInsatiable.cs, act-2 boss): HP 321 fixed; LIQUIFY_GROUND (Sandpit
    // 4 on the player + 3 FranticEscape to the draw pile and 3 to the discard, each at a
    // random position) -> THRASH 8x2 -> LUNGING_BITE 28 -> SALIVATE (+2 strength) ->
    // THRASH_MOVE_2 8x2 -> THRASH -> BITE -> SALIVATE -> ...
    {
        MonsterDef d;
        d.name = "THE_INSATIABLE";
        d.minHp = 321; d.maxHp = 321;
        MoveState liquify; liquify.moveId = "LIQUIFY_GROUND_MOVE"; liquify.next = 1;
        // SandpitPower lives on the ENEMY with Target = the player (stamp owner cid =
        // the Insatiable), so it rides selfPower (Instanced) instead of playerPower.
        liquify.selfPower = PowerId::SANDPIT; liquify.selfPowerAmount = 4;
        liquify.addFranticToDraw = 3; liquify.addFranticToDiscard = 3;
        MoveState thrash; thrash.moveId = "THRASH_MOVE"; thrash.damage = 8; thrash.hits = 2;
        thrash.next = 2;
        MoveState bite; bite.moveId = "LUNGING_BITE_MOVE"; bite.damage = 28; bite.next = 3;
        MoveState salivate; salivate.moveId = "SALIVATE_MOVE"; salivate.next = 4;
        salivate.selfPower = PowerId::STRENGTH; salivate.selfPowerAmount = 2;
        MoveState thrash2; thrash2.moveId = "THRASH_MOVE_2"; thrash2.damage = 8;
        thrash2.hits = 2; thrash2.next = 1;
        d.states = {liquify, thrash, bite, salivate, thrash2};
        defs.push_back(d);
    }
    // ENTOMANCER (Entomancer.cs, act-2 elite): HP 145 fixed; PersonalHive 1. Starts on
    // BEES 3x7 -> SPEAR 18 -> PHEROMONE_SPIT (+1 hive and +1 strength; at 3+ stacks only
    // +2 strength) -> BEES. Every powered hit on it adds Hive Dazed cards to the draw pile.
    {
        MonsterDef d;
        d.name = "ENTOMANCER";
        d.minHp = 145; d.maxHp = 145;
        d.startPower = PowerId::PERSONAL_HIVE; d.startPowerAmount = 1;
        MoveState spit; spit.moveId = "PHEROMONE_SPIT_MOVE"; spit.next = 1; spit.hiveSpit = true;
        MoveState bees; bees.moveId = "BEES_MOVE"; bees.damage = 3; bees.hits = 7; bees.next = 2;
        MoveState spear; spear.moveId = "SPEAR_MOVE"; spear.damage = 18; spear.next = 0;
        d.states = {spit, bees, spear};
        d.startState = 1; // BEES_MOVE
        defs.push_back(d);
    }
    // INFESTED_PRISM (InfestedPrism.cs, act-2 elite): HP 161 fixed; VitalSpark 2 (Tainted
    // affliction on Skill cards). JAB 15 -> RADIATE 11 + 11 block -> WHIRLWIND 5x3 ->
    // PULSATE 8 + 20 block + VitalSpark +2 -> JAB.
    {
        MonsterDef d;
        d.name = "INFESTED_PRISM";
        d.minHp = 161; d.maxHp = 161;
        d.startPower = PowerId::VITAL_SPARK; d.startPowerAmount = 2;
        MoveState jab; jab.moveId = "JAB_MOVE"; jab.damage = 15; jab.next = 1;
        MoveState radiate; radiate.moveId = "RADIATE_MOVE"; radiate.damage = 11;
        radiate.selfBlock = 11; radiate.next = 2;
        MoveState whirl; whirl.moveId = "WHIRLWIND_MOVE"; whirl.damage = 5; whirl.hits = 3;
        whirl.next = 3;
        MoveState pulsate; pulsate.moveId = "PULSATE_MOVE"; pulsate.damage = 8;
        pulsate.selfBlock = 20; pulsate.next = 0;
        pulsate.selfPower = PowerId::VITAL_SPARK; pulsate.selfPowerAmount = 2;
        d.states = {jab, radiate, whirl, pulsate};
        defs.push_back(d);
    }
    // THIEVING_HOPPER (ThievingHopper.cs, act-2): HP 79 fixed; EscapeArtist 5 (countdown,
    // visual only). THIEVERY (steal a deck card from draw/discard, then 17) -> FLUTTER
    // (Flutter 5: powered attacks deal half, each unblocked hit burns a stack; at 0 the
    // hopper is STUNNED then follows the last move's follow-up) -> HAT_TRICK 21 ->
    // NAB 14 -> ESCAPE (leaves combat) self-loop.
    {
        MonsterDef d;
        d.name = "THIEVING_HOPPER";
        d.minHp = 79; d.maxHp = 79;
        d.startPower = PowerId::ESCAPE_ARTIST; d.startPowerAmount = 5;
        MoveState theft; theft.moveId = "THIEVERY_MOVE"; theft.damage = 17; theft.next = 1;
        theft.stealCard = true;
        MoveState flutter; flutter.moveId = "FLUTTER_MOVE"; flutter.next = 2;
        flutter.selfPower = PowerId::FLUTTER; flutter.selfPowerAmount = 5;
        MoveState hat; hat.moveId = "HAT_TRICK_MOVE"; hat.damage = 21; hat.next = 3;
        MoveState nab; nab.moveId = "NAB_MOVE"; nab.damage = 14; nab.next = 4;
        MoveState escape; escape.moveId = "ESCAPE_MOVE"; escape.next = 4;
        escape.escapeCombat = true;
        MoveState stunned; stunned.moveId = "STUNNED"; stunned.noLog = true;
        d.states = {theft, flutter, hat, nab, escape, stunned};
        defs.push_back(d);
    }
    // THE_OBSCURA (TheObscura.cs, act-2): HP 123 fixed; ILLUSION (summon a Parafright into
    // the "illusion" slot) then RAND(PIERCING_GAZE 10 / SAIL +3 strength to all allies /
    // HARDENING_STRIKE 6 + 6 block), each CannotRepeat.
    {
        MonsterDef d;
        d.name = "THE_OBSCURA";
        d.minHp = 123; d.maxHp = 123;
        MoveState illusion; illusion.moveId = "ILLUSION_MOVE"; illusion.next = 4;
        MoveState gaze; gaze.moveId = "PIERCING_GAZE_MOVE"; gaze.damage = 10; gaze.next = 4;
        MoveState sail; sail.moveId = "SAIL_MOVE"; sail.next = 4;
        sail.selfPower = PowerId::STRENGTH; sail.selfPowerAmount = 3; sail.selfPowerAllies = true;
        MoveState strike; strike.moveId = "HARDENING_STRIKE_MOVE"; strike.damage = 6;
        strike.selfBlock = 6; strike.next = 4;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 1, 1}, {1.0f, 2, 1}, {1.0f, 3, 1}}; // all CannotRepeat
        d.states = {illusion, gaze, sail, strike, rnd};
        defs.push_back(d);
    }
    // BOWLBUG_SILK (BowlbugSilk.cs, act-2): HP 40-43; TOXIC_SPIT (Weak 1) -> THRASH 4x2
    // -> TOXIC_SPIT (starts on the spit).
    {
        MonsterDef d;
        d.name = "BOWLBUG_SILK";
        d.minHp = 40; d.maxHp = 43;
        MoveState thrash; thrash.moveId = "THRASH_MOVE"; thrash.damage = 4; thrash.hits = 2;
        thrash.next = 1;
        MoveState spit; spit.moveId = "TOXIC_SPIT_MOVE"; spit.next = 0;
        spit.playerPower = PowerId::WEAK; spit.playerPowerAmount = 1;
        d.states = {thrash, spit};
        d.startState = 1; // TOXIC_SPIT_MOVE
        defs.push_back(d);
    }
    // SLUMBERING_BEETLE (SlumberingBeetle.cs, act-2): HP 86 fixed; Plating 15 + Slumber 3.
    // SNORE (no effect) loops while Slumber is up, then ROLL_OUT 16 (+2 strength) self-loops.
    // Slumber drops on unblocked damage and at the end of its own turn; reaching 0 wakes the
    // beetle (removes Plating). Damage wake also stuns it (STUNNED, then ROLL_OUT).
    {
        MonsterDef d;
        d.name = "SLUMBERING_BEETLE";
        d.minHp = 86; d.maxHp = 86;
        d.startBlock = 15; // PlatingPower.BeforeSideTurnStart (round 1)
        d.startPower = PowerId::PLATING; d.startPowerAmount = 15;
        d.startPower2 = PowerId::SLUMBER; d.startPower2Amount = 3;
        MoveState snore; snore.moveId = "SNORE_MOVE"; snore.next = 1;
        MoveState snoreNext; snoreNext.moveId = "SNORE_NEXT";
        snoreNext.condPredicate = 2; // has Slumber
        snoreNext.condTrue = 0; snoreNext.condFalse = 2;
        MoveState roll; roll.moveId = "ROLL_OUT_MOVE"; roll.damage = 16; roll.next = 2;
        roll.selfPower = PowerId::STRENGTH; roll.selfPowerAmount = 2;
        MoveState stunned; stunned.moveId = "STUNNED"; stunned.noLog = true; stunned.next = 2;
        d.states = {snore, snoreNext, roll, stunned};
        defs.push_back(d);
    }
    // MYTE (Myte.cs, act-2): HP 61-67; slot-conditional INIT (first TOXIC, second SUCK);
    // TOXIC (2x Toxic to hand) -> BITE 13 -> SUCK 4 (+2 strength) -> TOXIC.
    {
        MonsterDef d;
        d.name = "MYTE";
        d.minHp = 61; d.maxHp = 67;
        MoveState toxic; toxic.moveId = "TOXIC_MOVE"; toxic.next = 1;
        toxic.addCardToHand = CardId::TOXIC; toxic.addCardToHandCount = 2;
        MoveState bite; bite.moveId = "BITE_MOVE"; bite.damage = 13; bite.next = 2;
        MoveState suck; suck.moveId = "SUCK_MOVE"; suck.damage = 4; suck.next = 0;
        suck.selfPower = PowerId::STRENGTH; suck.selfPowerAmount = 2;
        d.states = {toxic, bite, suck};
        defs.push_back(d);
    }
    // HUNTER_KILLER (HunterKiller.cs, act-2): HP 121; all three moves follow up to the
    // same RAND (the C# chained assignment `m3.FollowUp = (m2.FollowUp = (m1.FollowUp =
    // new RandomBranchState(...)))` assigns RAND to every move): TENDERIZING_GOOP
    // (Tender 1) / BITE 17 / PUNCTURE 7x3, then RAND(BITE CannotRepeat, PUNCTURE x2).
    {
        MonsterDef d;
        d.name = "HUNTER_KILLER";
        d.minHp = 121; d.maxHp = 121;
        MoveState goop; goop.moveId = "TENDERIZING_GOOP_MOVE"; goop.next = 3;
        goop.playerPower = PowerId::TENDER; goop.playerPowerAmount = 1;
        MoveState bite; bite.moveId = "BITE_MOVE"; bite.damage = 17; bite.next = 3;
        MoveState puncture; puncture.moveId = "PUNCTURE_MOVE"; puncture.damage = 7;
        puncture.hits = 3; puncture.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 1, 1}, {1.0f, 2, 2}}; // BITE CannotRepeat, PUNCTURE x2
        d.states = {goop, bite, puncture, rnd};
        defs.push_back(d);
    }
    // LOUSE_PROGENITOR (LouseProgenitor.cs, act-2): HP 134-136; starts with CurlUp 14;
    // WEB_CANNON 9 (+2 frail) -> CURL_AND_GROW (14 block, +5 strength) -> POUNCE 14 -> WEB.
    {
        MonsterDef d;
        d.name = "LOUSE_PROGENITOR";
        d.minHp = 134; d.maxHp = 136;
        d.startPower = PowerId::CURL_UP; d.startPowerAmount = 14;
        MoveState web; web.moveId = "WEB_CANNON_MOVE"; web.damage = 9; web.next = 1;
        web.playerPower = PowerId::FRAIL; web.playerPowerAmount = 2;
        MoveState curl; curl.moveId = "CURL_AND_GROW_MOVE"; curl.selfBlock = 14;
        curl.selfPower = PowerId::STRENGTH; curl.selfPowerAmount = 5; curl.next = 2;
        MoveState pounce; pounce.moveId = "POUNCE_MOVE"; pounce.damage = 14; pounce.next = 0;
        d.states = {web, curl, pounce};
        defs.push_back(d);
    }
    // OVICOPTER (Ovicopter.cs, act-2): HP 124-130; LAY_EGGS (summon 3 ToughEgg into the
    // last free slots) -> SMASH 16 -> TENDERIZER 7 (+2 vulnerable) -> SUMMON_BRANCH
    // (CanLay = alive teammates <= 3 ? LAY_EGGS : NUTRITIONAL_PASTE +3 strength).
    {
        MonsterDef d;
        d.name = "OVICOPTER";
        d.minHp = 124; d.maxHp = 130;
        MoveState paste; paste.moveId = "NUTRITIONAL_PASTE_MOVE"; paste.next = 2;
        paste.selfPower = PowerId::STRENGTH; paste.selfPowerAmount = 3;
        MoveState lay; lay.moveId = "LAY_EGGS_MOVE"; lay.next = 2;
        MoveState smash; smash.moveId = "SMASH_MOVE"; smash.damage = 16; smash.next = 3;
        MoveState tender; tender.moveId = "TENDERIZER_MOVE"; tender.damage = 7; tender.next = 4;
        tender.playerPower = PowerId::VULNERABLE; tender.playerPowerAmount = 2;
        MoveState branchState; branchState.moveId = "SUMMON_BRANCH_STATE";
        branchState.condPredicate = 1; branchState.condTrue = 1; branchState.condFalse = 0;
        d.states = {paste, lay, smash, tender, branchState};
        d.startState = 1; // LAY_EGGS_MOVE
        d.slotOrder = "egg1,egg2,egg3,egg4,egg5,ovicopter";
        defs.push_back(d);
    }
    // TOUGH_EGG (ToughEgg.cs, Ovicopter summon): HP 14-18; Minion; HATCH (becomes a
    // Hatchling with max HP rolled on the Niche stream) -> NIBBLE 4 self-loop.
    {
        MonsterDef d;
        d.name = "TOUGH_EGG";
        d.minHp = 14; d.maxHp = 18;
        d.startPower = PowerId::HATCH; d.startPowerAmount = 1;
        d.startPower2 = PowerId::MINION; d.startPower2Amount = 1;
        d.minion = true;
        MoveState hatch; hatch.moveId = "HATCH_MOVE"; hatch.next = 1;
        MoveState nibble; nibble.moveId = "NIBBLE_MOVE"; nibble.damage = 4; nibble.next = 1;
        d.states = {hatch, nibble};
        defs.push_back(d);
    }
    // ---- act-3 (GLORY) remainder: knights, mecha knight, soul nexus, globe head,
    // fabricator + its bots, queen + amalgam, test subject ----
    // GLOBE_HEAD (GlobeHead.cs, act-3 normal): HP 148, Galvanic 6 at room entry;
    // SHOCKING_SLAP 13 (+Frail 2) -> THUNDER_STRIKE 6x3 -> GALVANIC_BURST 16 (+2 Str) ->
    // SHOCKING_SLAP...
    {
        MonsterDef d;
        d.name = "GLOBE_HEAD";
        d.minHp = 148; d.maxHp = 148;
        d.startPower = PowerId::GALVANIC; d.startPowerAmount = 6;
        MoveState slap; slap.moveId = "SHOCKING_SLAP"; slap.damage = 13;
        slap.playerPower = PowerId::FRAIL; slap.playerPowerAmount = 2; slap.next = 1;
        MoveState thunder; thunder.moveId = "THUNDER_STRIKE"; thunder.damage = 6;
        thunder.hits = 3; thunder.next = 2;
        MoveState burst; burst.moveId = "GALVANIC_BURST"; burst.damage = 16;
        burst.selfPower = PowerId::STRENGTH; burst.selfPowerAmount = 2; burst.next = 0;
        d.states = {slap, thunder, burst};
        defs.push_back(d);
    }
    // FLAIL_KNIGHT (FlailKnight.cs, act-3 elite): HP 101; starts RAM 15; WAR_CHANT (+3 Str),
    // FLAIL 9x2, RAM 15 all followed by RAND{WAR_CHANT CannotRepeat, FLAIL x2, RAM x2}.
    {
        MonsterDef d;
        d.name = "FLAIL_KNIGHT";
        d.minHp = 101; d.maxHp = 101;
        MoveState chant; chant.moveId = "WAR_CHANT"; chant.selfPower = PowerId::STRENGTH;
        chant.selfPowerAmount = 3; chant.next = 3;
        MoveState flail; flail.moveId = "FLAIL_MOVE"; flail.damage = 9; flail.hits = 2;
        flail.next = 3;
        MoveState ram; ram.moveId = "RAM_MOVE"; ram.damage = 15; ram.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 0, 1}, {1.0f, 1, 2}, {1.0f, 2, 2}};
        d.states = {chant, flail, ram, rnd};
        d.startState = 2;
        defs.push_back(d);
    }
    // SPECTRAL_KNIGHT (SpectralKnight.cs, act-3 elite): HP 93; HEX (HexPower 2) ->
    // SOUL_SLASH 15 -> RAND{SOUL_SLASH x2, SOUL_FLAME 3x3 CannotRepeat}.
    {
        MonsterDef d;
        d.name = "SPECTRAL_KNIGHT";
        d.minHp = 93; d.maxHp = 93;
        MoveState hex; hex.moveId = "HEX"; hex.playerPower = PowerId::HEX;
        hex.playerPowerAmount = 2; hex.next = 1;
        MoveState slash; slash.moveId = "SOUL_SLASH"; slash.damage = 15; slash.next = 3;
        MoveState flame; flame.moveId = "SOUL_FLAME"; flame.damage = 3; flame.hits = 3;
        flame.next = 3;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 1, 2}, {1.0f, 2, 1}};
        d.states = {hex, slash, flame, rnd};
        defs.push_back(d);
    }
    // MAGI_KNIGHT (MagiKnight.cs, act-3 elite): HP 82; POWER_SHIELD 6 (+5 block) ->
    // DAMPEN (DampenPower 1) -> RAM 10 -> PREP (+5 block) -> MAGIC_BOMB 35 -> RAM...
    {
        MonsterDef d;
        d.name = "MAGI_KNIGHT";
        d.minHp = 82; d.maxHp = 82;
        MoveState shield; shield.moveId = "POWER_SHIELD_MOVE"; shield.damage = 6;
        shield.selfBlock = 5; shield.next = 1;
        MoveState dampen; dampen.moveId = "DAMPEN_MOVE";
        dampen.playerPower = PowerId::DAMPEN; dampen.playerPowerAmount = 1; dampen.next = 2;
        MoveState ram; ram.moveId = "RAM_MOVE"; ram.damage = 10; ram.next = 3;
        MoveState prep; prep.moveId = "PREP_MOVE"; prep.selfBlock = 5; prep.next = 4;
        MoveState bomb; bomb.moveId = "MAGIC_BOMB"; bomb.damage = 35; bomb.next = 2;
        d.states = {shield, dampen, ram, prep, bomb};
        defs.push_back(d);
    }
    // MECHA_KNIGHT (MechaKnight.cs, act-3 elite): HP 300, Artifact 3; CHARGE 25 ->
    // FLAMETHROWER 8 (+4 Burn into HAND) -> WINDUP (+15 block, +5 Str) -> HEAVY_CLEAVE 35
    // -> FLAMETHROWER...
    {
        MonsterDef d;
        d.name = "MECHA_KNIGHT";
        d.minHp = 300; d.maxHp = 300;
        d.startPower = PowerId::ARTIFACT; d.startPowerAmount = 3;
        MoveState charge; charge.moveId = "CHARGE_MOVE"; charge.damage = 25; charge.next = 3;
        MoveState flame; flame.moveId = "FLAMETHROWER_MOVE"; flame.damage = 8;
        flame.addCardToHand = CardId::BURN; flame.addCardToHandCount = 4; flame.next = 2;
        MoveState windup; windup.moveId = "WINDUP_MOVE"; windup.selfBlock = 15;
        windup.selfPower = PowerId::STRENGTH; windup.selfPowerAmount = 5; windup.next = 1;
        MoveState cleave; cleave.moveId = "HEAVY_CLEAVE_MOVE"; cleave.damage = 35;
        cleave.next = 3;
        d.states = {charge, cleave, windup, flame};
        defs.push_back(d);
    }
    // SOUL_NEXUS (SoulNexus.cs, act-3 elite): HP 234; SOUL_BURN 29 -> RAND{SOUL_BURN,
    // MAELSTROM 6x4, DRAIN_LIFE 18 (+Vulnerable 2 +Weak 2)} all CannotRepeat.
    {
        MonsterDef d;
        d.name = "SOUL_NEXUS";
        d.minHp = 234; d.maxHp = 234;
        MoveState burn; burn.moveId = "SOUL_BURN_MOVE"; burn.damage = 29; burn.next = 3;
        MoveState mael; mael.moveId = "MAELSTROM_MOVE"; mael.damage = 6; mael.hits = 4;
        mael.next = 3;
        MoveState drain; drain.moveId = "DRAIN_LIFE_MOVE"; drain.damage = 18;
        drain.playerPower = PowerId::VULNERABLE; drain.playerPowerAmount = 2;
        drain.playerPower2 = PowerId::WEAK; drain.playerPower2Amount = 2; drain.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 0, 1}, {1.0f, 1, 1}, {1.0f, 2, 1}};
        d.states = {burn, mael, drain, rnd};
        defs.push_back(d);
    }
    // FABRICATOR (Fabricator.cs, act-3 normal): HP 150; FABRICATE (1 defensive + 1 aggro
    // bot) / FABRICATING_STRIKE 18 (+1 aggro bot) / DISINTEGRATE 11 once 4 allies are alive.
    // The branch between RAND and DISINTEGRATE is CanFabricate = alive teammates < 4.
    {
        MonsterDef d;
        d.name = "FABRICATOR";
        d.minHp = 150; d.maxHp = 150;
        d.slotOrder = "bot1,bot2,fabricator,bot3,bot4";
        MoveState fab; fab.moveId = "FABRICATE_MOVE"; fab.fabricateKind = 1; fab.next = 4;
        MoveState strike; strike.moveId = "FABRICATING_STRIKE_MOVE"; strike.damage = 18;
        strike.fabricateKind = 2; strike.next = 4;
        MoveState dis; dis.moveId = "DISINTEGRATE_MOVE"; dis.damage = 11; dis.next = 4;
        MoveState rnd; rnd.moveId = "RAND"; rnd.branch = {{1.0f, 0, 0}, {1.0f, 1, 0}};
        MoveState branch; branch.moveId = "fabricateBranch";
        branch.condPredicate = 6; branch.condTrue = 3; branch.condFalse = 2;
        d.states = {fab, strike, dis, rnd, branch};
        d.startState = 4; // initial state is the branch (RollMove resolves it)
        defs.push_back(d);
    }
    // ZAPBOT / STABBOT / GUARDBOT / NOISEBOT (Fabricator spawns): all Minion.
    // ZAPBOT HP 18-23, HighVoltage 2, ZAP 14 self-loop.
    {
        MonsterDef d;
        d.name = "ZAPBOT";
        d.minHp = 18; d.maxHp = 23;
        d.startPower = PowerId::HIGH_VOLTAGE; d.startPowerAmount = 2;
        d.startPower2 = PowerId::MINION; d.startPower2Amount = 1;
        d.minion = true;
        MoveState zap; zap.moveId = "ZAP"; zap.damage = 14; zap.next = 0;
        d.states = {zap};
        defs.push_back(d);
    }
    // STABBOT HP 18-23, STAB 11 (+Frail 1) self-loop.
    {
        MonsterDef d;
        d.name = "STABBOT";
        d.minHp = 18; d.maxHp = 23;
        d.startPower = PowerId::MINION; d.startPowerAmount = 1;
        d.minion = true;
        MoveState stab; stab.moveId = "STAB_MOVE"; stab.damage = 11;
        stab.playerPower = PowerId::FRAIL; stab.playerPowerAmount = 1; stab.next = 0;
        d.states = {stab};
        defs.push_back(d);
    }
    // GUARDBOT HP 16-20, GUARD (+15 block to every Fabricator) self-loop.
    {
        MonsterDef d;
        d.name = "GUARDBOT";
        d.minHp = 16; d.maxHp = 20;
        d.startPower = PowerId::MINION; d.startPowerAmount = 1;
        d.minion = true;
        MoveState guard; guard.moveId = "GUARD_MOVE"; guard.guardFabricator = true; guard.next = 0;
        d.states = {guard};
        defs.push_back(d);
    }
    // NOISEBOT HP 18-23, NOISE (Dazed to discard bottom + Dazed at a random draw position).
    {
        MonsterDef d;
        d.name = "NOISEBOT";
        d.minHp = 18; d.maxHp = 23;
        d.startPower = PowerId::MINION; d.startPowerAmount = 1;
        d.minion = true;
        MoveState noise; noise.moveId = "NOISE_MOVE"; noise.noisebotNoise = true; noise.next = 0;
        d.states = {noise};
        defs.push_back(d);
    }
    // TORCH_HEAD_AMALGAM (TorchHeadAmalgam.cs, act-3 boss add): HP 199, Minion;
    // STRONG_TACKLE 26 -> TACKLE 18 -> BEAM 8x3 -> TACKLE_3 14 -> TACKLE_4 14 -> BEAM...
    {
        MonsterDef d;
        d.name = "TORCH_HEAD_AMALGAM";
        d.minHp = 199; d.maxHp = 199;
        d.startPower = PowerId::MINION; d.startPowerAmount = 1;
        d.minion = true;
        MoveState strong; strong.moveId = "STRONG_TACKLE_MOVE"; strong.damage = 26; strong.next = 1;
        MoveState tackle; tackle.moveId = "TACKLE_2_MOVE"; tackle.damage = 18; tackle.next = 2;
        MoveState beam; beam.moveId = "BEAM_MOVE"; beam.damage = 8; beam.hits = 3; beam.next = 3;
        MoveState t3; t3.moveId = "TACKLE_3_MOVE"; t3.damage = 14; t3.next = 4;
        MoveState t4; t4.moveId = "TACKLE_4_MOVE"; t4.damage = 14; t4.next = 2;
        d.states = {strong, tackle, beam, t3, t4};
        defs.push_back(d);
    }
    // QUEEN (Queen.cs, act-3 boss): HP 400; PUPPET_STRINGS (ChainsOfBinding 3) ->
    // YOU_ARE_MINE (Frail/Weak/Vulnerable 99) -> branch on HasAmalgamDied:
    // BURN_BRIGHT_FOR_ME (+1 Str to the other enemy, +20 block) loops while the amalgam
    // lives; OFF_WITH_YOUR_HEAD 3x5 -> EXECUTION 15 -> ENRAGE (+2 Str) -> OFF_WITH...
    {
        MonsterDef d;
        d.name = "QUEEN";
        d.minHp = 400; d.maxHp = 400;
        MoveState puppet; puppet.moveId = "PUPPET_STRINGS_MOVE";
        puppet.playerPower = PowerId::CHAINS_OF_BINDING; puppet.playerPowerAmount = 3;
        puppet.next = 1;
        MoveState mine; mine.moveId = "YOU_ARE_MINE_MOVE";
        mine.playerPower = PowerId::FRAIL; mine.playerPowerAmount = 99;
        mine.playerPower2 = PowerId::WEAK; mine.playerPower2Amount = 99;
        mine.playerPower3 = PowerId::VULNERABLE; mine.playerPower3Amount = 99;
        mine.next = 6;
        MoveState burn; burn.moveId = "BURN_BRIGHT_FOR_ME_MOVE";
        burn.selfPower = PowerId::STRENGTH; burn.selfPowerAmount = 1;
        burn.selfPowerTeammates = true; burn.selfBlock = 20; burn.next = 6;
        MoveState head; head.moveId = "OFF_WITH_YOUR_HEAD_MOVE"; head.damage = 3;
        head.hits = 5; head.next = 4;
        MoveState exec; exec.moveId = "EXECUTION_MOVE"; exec.damage = 15; exec.next = 5;
        MoveState enrage; enrage.moveId = "ENRAGE_MOVE"; enrage.selfPower = PowerId::STRENGTH;
        enrage.selfPowerAmount = 2; enrage.next = 3;
        // YOURE_MINE_NOW_BRANCH / BURN_BRIGHT_FOR_ME_BRANCH: !HasAmalgamDied ? burn : head
        MoveState mineBranch; mineBranch.moveId = "YOURE_MINE_NOW_BRANCH";
        mineBranch.condPredicate = 7; mineBranch.condTrue = 2; mineBranch.condFalse = 3;
        MoveState burnBranch; burnBranch.moveId = "BURN_BRIGHT_FOR_ME_BRANCH";
        burnBranch.condPredicate = 7; burnBranch.condTrue = 2; burnBranch.condFalse = 3;
        d.states = {puppet, mine, burn, head, exec, enrage, mineBranch, burnBranch};
        defs.push_back(d);
    }
    // TEST_SUBJECT (TestSubject.cs, act-3 boss): HP 100, Adaptable 1 + Enrage 2.
    // BITE 20 <-> SKULL_BASH 14 (+Vulnerable 1); on death Adaptable forces RESPAWN
    // (stage 1 -> 200 HP + PainfulStabs, MULTI_CLAW 10x(3+n) self-loop; stage 2 -> 300 HP
    // + Nemesis, LACERATE 10x3 -> BIG_POUNCE 45 -> BURNING_GROWL 3 Burn +2 Str -> LACERATE).
    {
        MonsterDef d;
        d.name = "TEST_SUBJECT";
        d.minHp = 100; d.maxHp = 100;
        d.startPower = PowerId::ADAPTABLE; d.startPowerAmount = 1;
        d.startPower2 = PowerId::ENRAGE; d.startPower2Amount = 2;
        MoveState bite; bite.moveId = "BITE_MOVE"; bite.damage = 20; bite.next = 1;
        MoveState skull; skull.moveId = "SKULL_BASH_MOVE"; skull.damage = 14;
        skull.playerPower = PowerId::VULNERABLE; skull.playerPowerAmount = 1; skull.next = 0;
        MoveState claw; claw.moveId = "MULTI_CLAW_MOVE"; claw.damage = 10;
        claw.hits = 3; claw.hitsFromExtraClaw = true; claw.next = 2;
        MoveState lacerate; lacerate.moveId = "PHASE3_LACERATE_MOVE"; lacerate.damage = 10;
        lacerate.hits = 3; lacerate.next = 4;
        MoveState pounce; pounce.moveId = "BIG_POUNCE"; pounce.damage = 45; pounce.next = 5;
        MoveState growl; growl.moveId = "BURNING_GROWL_MOVE";
        growl.addCardToDiscard = CardId::BURN; growl.addCardToDiscardCount = 3;
        growl.selfPower = PowerId::STRENGTH; growl.selfPowerAmount = 2; growl.next = 3;
        MoveState respawn; respawn.moveId = "RESPAWN_MOVE"; respawn.testSubjectRespawn = true;
        respawn.noLog = true; respawn.next = 7;
        // REVIVE_BRANCH: Respawns < 2 ? MULTI_CLAW : LACERATE
        MoveState rev; rev.moveId = "REVIVE_BRANCH"; rev.condPredicate = 8;
        rev.condTrue = 2; rev.condFalse = 3;
        d.states = {bite, skull, claw, lacerate, pounce, growl, respawn, rev};
        d.deathState = 6; // AdaptablePower: death forces RESPAWN (MustPerformOnce)
        defs.push_back(d);
    }
    // cross-references (summon target indices resolved after all defs are registered)
    {
        int fogmog = -1, eye = -1, ovicopter = -1, egg = -1, obscura = -1, parafright = -1;
        for (int i = 0; i < (int)defs.size(); ++i) {
            if (std::strcmp(defs[i].name, "FOGMOG") == 0) fogmog = i;
            if (std::strcmp(defs[i].name, "EYE_WITH_TEETH") == 0) eye = i;
            if (std::strcmp(defs[i].name, "OVICOPTER") == 0) ovicopter = i;
            if (std::strcmp(defs[i].name, "TOUGH_EGG") == 0) egg = i;
            if (std::strcmp(defs[i].name, "THE_OBSCURA") == 0) obscura = i;
            if (std::strcmp(defs[i].name, "PARAFRIGHT") == 0) parafright = i;
        }
        if (fogmog >= 0 && eye >= 0) {
            for (auto& st : defs[fogmog].states)
                if (std::strcmp(st.moveId, "ILLUSION_MOVE") == 0) {
                    st.summonDefIdx = eye;
                    st.summonSlot = "illusion";
                }
        }
        if (ovicopter >= 0 && egg >= 0) {
            for (auto& st : defs[ovicopter].states)
                if (std::strcmp(st.moveId, "LAY_EGGS_MOVE") == 0) {
                    st.summonDefIdx = egg;
                    st.summonCount = 3;
                    st.summonLastFreeSlot = true;
                }
        }
        if (obscura >= 0 && parafright >= 0) {
            for (auto& st : defs[obscura].states)
                if (std::strcmp(st.moveId, "ILLUSION_MOVE") == 0) {
                    st.summonDefIdx = parafright;
                    st.summonSlot = "illusion";
                }
        }
    }
    // PUNCH_CONSTRUCT (PunchOff event, PunchConstruct.cs): HP 55, Artifact 1;
    // READY (+10 block) -> FAST_PUNCH (5x2 + Frail 1) -> STRONG_PUNCH (14) -> READY.
    {
        MonsterDef d;
        d.name = "PUNCH_CONSTRUCT";
        d.minHp = 55; d.maxHp = 55;
        d.startPower = PowerId::ARTIFACT; d.startPowerAmount = 1;
        MoveState ready; ready.moveId = "READY_MOVE"; ready.selfBlock = 10; ready.next = 1;
        MoveState fast; fast.moveId = "FAST_PUNCH_MOVE"; fast.damage = 5; fast.hits = 2;
        fast.playerPower = PowerId::FRAIL; fast.playerPowerAmount = 1; fast.next = 2;
        MoveState strong; strong.moveId = "STRONG_PUNCH_MOVE"; strong.damage = 14;
        strong.next = 0;
        d.states = {ready, fast, strong};
        defs.push_back(d);
    }
    // BATTLE_FRIEND V1/V2/V3 (BattlewornDummy event): HP 75/150/300, does nothing; a
    // 3-turn time-limit power makes it escape (encounter times out).
    {
        MonsterDef d;
        d.name = "BATTLE_FRIEND_V1";
        d.minHp = 75; d.maxHp = 75;
        d.startPower = PowerId::BATTLEWORN_DUMMY_TIME_LIMIT; d.startPowerAmount = 3;
        MoveState nothing; nothing.moveId = "NOTHING_MOVE"; nothing.next = 0;
        d.states = {nothing};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "BATTLE_FRIEND_V2";
        d.minHp = 150; d.maxHp = 150;
        d.startPower = PowerId::BATTLEWORN_DUMMY_TIME_LIMIT; d.startPowerAmount = 3;
        MoveState nothing; nothing.moveId = "NOTHING_MOVE"; nothing.next = 0;
        d.states = {nothing};
        defs.push_back(d);
    }
    {
        MonsterDef d;
        d.name = "BATTLE_FRIEND_V3";
        d.minHp = 300; d.maxHp = 300;
        d.startPower = PowerId::BATTLEWORN_DUMMY_TIME_LIMIT; d.startPowerAmount = 3;
        MoveState nothing; nothing.moveId = "NOTHING_MOVE"; nothing.next = 0;
        d.states = {nothing};
        defs.push_back(d);
    }
    // MYSTERIOUS_KNIGHT (TheLanternKey event, MysteriousKnight.cs): FlailKnight at 101 HP
    // with +6 Strength and +6 Plating; RAM 15 -> RAND {WAR_CHANT (+3 Str, cannot repeat),
    // FLAIL 9x2, RAM 15}.
    {
        MonsterDef d;
        d.name = "MYSTERIOUS_KNIGHT";
        d.minHp = 101; d.maxHp = 101;
        d.startPower = PowerId::STRENGTH; d.startPowerAmount = 6;
        d.startPower2 = PowerId::PLATING; d.startPower2Amount = 6;
        MoveState chant; chant.moveId = "WAR_CHANT"; chant.selfPower = PowerId::STRENGTH;
        chant.selfPowerAmount = 3; chant.next = 3;
        MoveState flail; flail.moveId = "FLAIL_MOVE"; flail.damage = 9; flail.hits = 2;
        flail.next = 3;
        MoveState ram; ram.moveId = "RAM_MOVE"; ram.damage = 15; ram.next = 3;
        MoveState rnd; rnd.moveId = "RAND";
        rnd.branch = {{1.0f, 0, 1}, {1.0f, 1, 2}, {1.0f, 2, 2}};
        d.states = {chant, flail, ram, rnd};
        d.startState = 2; // starts on RAM
        defs.push_back(d);
    }
    // FAKE_MERCHANT_MONSTER (FakeMerchant event): 165 HP; SWIPE 13 -> RAND
    // {SWIPE, SPEW 2x8, THROW_RELIC 9 + Frail 1 -> RAND_ATTACK, ENRAGE +2 Str}.
    {
        MonsterDef d;
        d.name = "FAKE_MERCHANT_MONSTER";
        d.minHp = 165; d.maxHp = 165;
        MoveState swipe; swipe.moveId = "SWIPE_MOVE"; swipe.damage = 13; swipe.next = 4;
        MoveState spew; spew.moveId = "SPEW_COINS_MOVE"; spew.damage = 2; spew.hits = 8;
        spew.next = 4;
        MoveState thr; thr.moveId = "THROW_RELIC_MOVE"; thr.damage = 9; thr.next = 5;
        thr.playerPower = PowerId::FRAIL; thr.playerPowerAmount = 1;
        MoveState enrage; enrage.moveId = "ENRAGE_MOVE"; enrage.next = 4;
        enrage.selfPower = PowerId::STRENGTH; enrage.selfPowerAmount = 2;
        MoveState rnd; rnd.moveId = "RAND_MOVE";
        rnd.branch = {{1.0f, 0, 1}, {1.0f, 1, 1}, {1.0f, 2, 1}, {3.0f, 3, 1}};
        MoveState rndAtk; rndAtk.moveId = "RAND_ATTACK_MOVE";
        rndAtk.branch = {{1.0f, 0, 1}, {1.0f, 1, 1}, {1.0f, 2, 1}};
        d.states = {swipe, spew, thr, enrage, rnd, rndAtk};
        d.startState = 0;
        defs.push_back(d);
    }
    return defs;
}

} // namespace

int BattleContext::registerMonster(const MonsterDef& def) {
    auto& defs = monsterDefs();
    defs.push_back(def);
    return (int)defs.size() - 1;
}

int BattleContext::monsterDefIdx(const std::string& name) {
    auto& defs = monsterDefs();
    for (int i = 0; i < (int)defs.size(); ++i)
        if (name == defs[i].name) return i;
    return -1;
}

const MonsterDef& BattleContext::monsterDef(int idx) { return monsterDefs()[idx]; }
const MonsterDef& Monster::def(const BattleContext&) const { return monsterDefs()[defIdx]; }
std::vector<MonsterDef>& BattleContext::monsterDefsForAscension() { return monsterDefs(); }

// ---------------- powers ----------------

PowerInst* PlayerSide::findPower(PowerId id) {
    for (auto& p : powers) if (p.id == id) return &p;
    return nullptr;
}
PowerInst* Monster::findPower(PowerId id) {
    for (auto& p : powers) if (p.id == id) return &p;
    return nullptr;
}
const PowerInst* Monster::findPower(PowerId id) const {
    for (auto& p : powers) if (p.id == id) return &p;
    return nullptr;
}

// total stacks of a power. Non-instanced powers have a single merged entry; instanced
// ones (Strangle/Panache/...) hold one entry per application, so their amounts add up.
static int powerAmount(const std::vector<PowerInst>& list, PowerId id) {
    int n = 0;
    for (auto& p : list) if (p.id == id) n += p.amount;
    return n;
}

// Adding a power that is Instanced (Panache/TheBomb/Flanking/Strangle/Orbit/...) creates a
// NEW entry instead of stacking; every other id merges by amount. The list mirrors
// PowerInstanceType.Instanced in the decompiled sources.
static bool isInstancedPower(PowerId id) {
    switch (id) {
        case PowerId::PANACHE:
        case PowerId::THE_BOMB:
        case PowerId::FLANKING:
        case PowerId::STRANGLE:
        case PowerId::ORBIT:
        case PowerId::AUTOMATION:
        case PowerId::CACOPHONY:
        case PowerId::COVERED:
        case PowerId::GUARDED:
        case PowerId::HEIST:
        case PowerId::IMITATION_LEARNING:
        case PowerId::KNOCKDOWN:
        case PowerId::MAGIC_BOMB:
        case PowerId::MONOLOGUE:
        case PowerId::NIGHTMARE:
        case PowerId::OBLIVION:
        case PowerId::ROLLING_BOULDER:
        case PowerId::SANDPIT:
        case PowerId::TORIC_TOUGHNESS:
        case PowerId::SWIPE:
        case PowerId::TAG_TEAM:
        case PowerId::THIEVERY:
        case PowerId::WITHERING_PRESENCE:
            return true;
        default:
            return false;
    }
}

void addPower(std::vector<PowerInst>& list, PowerId id, int amount, bool skipNextTick) {
    if (!isInstancedPower(id)) {
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (it->id == id) {
                it->amount += amount;      // stacking = same id adds amount (negatives persist)
                if (it->amount == 0) list.erase(it); // PowerCmd: a power landing on 0 leaves
                return;                    // SkipNextDurationTick is set only on power CREATION
            }
        }
    }
    if (amount == 0) return;
    PowerInst p;
    p.id = id;
    p.amount = amount;
    p.skipNextTick = skipNextTick;
    p.instanced = isInstancedPower(id);
    list.push_back(p);
}

void BattleContext::applyPowerToPlayer(PowerId id, int amount) {
    // RuinedHelmet.TryModifyPowerAmountReceived: the first positive Strength applied to the
    // player each combat is doubled.
    if (id == PowerId::STRENGTH && amount > 0 && hasRelic(RelicId::RUINED_HELMET)
        && !relicRuinedHelmetUsed) {
        amount *= 2;
        relicRuinedHelmetUsed = true;
    }
    // TemporaryStrengthPower family (SetupStrikePower, IsPositive): applying the power
    // shifts real Strength by the same amount right away (BeforeApplied, silent)
    if (id == PowerId::SETUP_STRIKE)
        addPower(player.powers, PowerId::STRENGTH, amount, false);
    // TemporaryFocusPower family (SynchronizePower/FocusedStrikePower): same pattern for Focus
    if (id == PowerId::SYNCHRONIZE || id == PowerId::FOCUSED_STRIKE || id == PowerId::HOTFIX)
        addPower(player.powers, PowerId::FOCUS, amount, false);
    // TemporaryDexterityPower family (AnticipatePower): same pattern for Dexterity
    if (id == PowerId::ANTICIPATE)
        addPower(player.powers, PowerId::DEXTERITY, amount, false);
    // RollingBoulderPower.AfterApplied: SetAmount(Amount + Damage dynvar) — the power is
    // single-row (Counter) and its Amount accumulates by the per-application step
    if (id == PowerId::ROLLING_BOULDER) {
        // PowerCmd.Apply (Counter) stacks IntValue onto Amount; AfterApplied then does
        // SetAmount(Amount + Damage dynvar) — hence Amount=10 after a single 5-damage play.
        // The turn-start hit still deals the DAMAGE dynvar, not the Amount.
        // PowerCmd.Apply(IntValue) sets Amount (5); AfterApplied then does
        // SetAmount(Amount + Damage) => 10 on the combat's first play. Each later
        // AfterPlayerTurnStart also bumps Amount by Damage (5) after dealing the old value.
        // The play puts the IntValue straight onto Amount (5); the per-turn
        // AfterPlayerTurnStart hook is what grows it from then on.
        addPower(player.powers, id, amount, false);
        for (auto& pw : player.powers)
            if (pw.id == PowerId::ROLLING_BOULDER && pw.aux < 0) pw.aux = 5; // Damage dynvar
        return;
    }
    // OrbitPower (Instanced): each instance owns its Data.energySpent counter, which
    // starts at 0 when the power is applied (the card's own cost was already spent).
    if (id == PowerId::ORBIT) {
        addPower(player.powers, id, amount, false);
        for (auto it = player.powers.rbegin(); it != player.powers.rend(); ++it)
            if (it->id == PowerId::ORBIT) {
                it->misc = 0; it->aux = 0;
                it->skipEnergyTick = true; // its own cost was spent before application
                break;
            }
        return;
    }
    // freshly applied player-side debuffs skip their first duration tick
    const bool isDebuff = id == PowerId::VULNERABLE || id == PowerId::WEAK || id == PowerId::FRAIL;
    addPower(player.powers, id, amount, isDebuff);
    // TangledPower.AfterApplied: every Attack card in combat gains Entangled(Amount);
    // AfterCardEnteredCombat covers cards generated later (see generateCardsFromPool).
    if (id == PowerId::TANGLED) {
        auto afflict = [&](std::vector<CardInstance>& pile) {
            for (auto& c : pile) {
                if (c.def().type != CardType::ATTACK) continue;
                c.affliction = 1; c.afflictionAmount = amount;
                c.stampText.clear();
            }
        };
        afflict(hand);
        afflict(drawPile);
        afflict(discardPile);
        afflict(exhaustPile);
    }
    // RingingPower.AfterApplied: every card (all types) gains Ringing(Amount) while the
    // power lasts; ShouldPlay allows only the first card play of the turn.
    if (id == PowerId::RINGING) {
        auto afflictAll = [&](std::vector<CardInstance>& pile) {
            for (auto& c : pile) {
                if (c.affliction != 0) continue;
                c.affliction = 2; c.afflictionAmount = amount;
                c.stampText.clear();
            }
        };
        afflictAll(hand);
        afflictAll(drawPile);
        afflictAll(discardPile);
        afflictAll(exhaustPile);
    }
    // HexPower.AfterApplied: every card the player owns gains Hexed(Amount) while the
    // power lasts (Hexed also grants Ethereal while HexPower is in force).
    if (id == PowerId::HEX) {
        auto afflictAll = [&](std::vector<CardInstance>& pile) {
            for (auto& c : pile) {
                if (c.affliction != 0) continue;
                c.affliction = 5; c.afflictionAmount = amount;
                c.stampText.clear();
            }
        };
        afflictAll(hand);
        afflictAll(drawPile);
        afflictAll(discardPile);
        afflictAll(exhaustPile);
    }
    // ChainsOfBindingPower.AfterCardDrawn covers the Bound cards drawn later; the power
    // itself afflicts nothing on application (Queen PUPPET_STRINGS). The per-turn play
    // counter resets at the player's side start.
    if (id == PowerId::CHAINS_OF_BINDING) boundCardsPlayedThisTurn = 0;
}

void BattleContext::applyPowerToEnemy(int idx, PowerId id, int amount) {
    Monster& m = enemies[idx];
    // UnsettlingLamp: the first card that debuffs an enemy doubles that card's debuffs.
    if (hasRelic(RelicId::UNSETTLING_LAMP) && !relicLampFinished
        && currentPlayUniqueIdx >= 0
        && (relicLampCardIdx < 0 || relicLampCardIdx == currentPlayUniqueIdx)
        && isDebuffPower(id) && powerAmount(m.powers, PowerId::ARTIFACT) <= 0) {
        relicLampCardIdx = currentPlayUniqueIdx;
        amount *= 2;
    }
    // Artifact negates debuffs applied to the owner (one stack per negation).
    // Mangle is a debuff (TemporaryStrength IsPositive=false): negation also cancels
    // its silent strength shift.
    const bool isDebuff = id == PowerId::VULNERABLE || id == PowerId::WEAK
                       || id == PowerId::FRAIL || id == PowerId::MANGLE
                       || id == PowerId::PIERCING_WAIL || id == PowerId::DARK_SHACKLES
                       || id == PowerId::ENFEEBLING_TOUCH
                       || id == PowerId::MONARCHS_GAZE_STRENGTH_DOWN;
    if (isDebuff) {
        if (int art = powerAmount(m.powers, PowerId::ARTIFACT)) {
            for (auto& p : m.powers)
                if (p.id == PowerId::ARTIFACT) { p.amount -= 1; break; }
            // PowerCmd.Decrement removes the power at 0 (stamp no longer lists it).
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::ARTIFACT && p.amount <= 0; }),
                m.powers.end());
            return; // negated
        }    }
    if (id == PowerId::DOOM) doomAppliedByOwnerThisTurn = true;
    if (id == PowerId::MANGLE || id == PowerId::PIERCING_WAIL
        || id == PowerId::DARK_SHACKLES || id == PowerId::ENFEEBLING_TOUCH
        || id == PowerId::MONARCHS_GAZE_STRENGTH_DOWN)
        addPower(m.powers, PowerId::STRENGTH, -amount, false); // Sign=-1 (TemporaryStrength)
    addPower(m.powers, id, amount, false);
}

void BattleContext::applyPowerToMonsterSelf(Monster& m, PowerId id, int amount) {
    addPower(m.powers, id, amount, false);
}

// ---------------- damage ----------------

// player attack -> enemy: additive strength, multiplicative weak(dealer)/vulnerable(target), single trunc.
int BattleContext::attackDamageFromBase(int base, int targetIdx) const {
    Rational r;
    (void)0;
    r.num = base + powerAmount(player.powers, PowerId::STRENGTH);
    // Sharp / Vigorous: the card's enchantment adds Amount to its powered attack damage.
    if (currentCardDamageBonus != 0) {
        if (currentDamageBonusVigorous) ++vigorousApplications;
        else ++sharpApplications;
    }
    r.num += currentCardDamageBonus;
    // Relic ModifyDamageAdditive (powered attacks from the resolving card):
    // Strike Dummy +3 on Strike-tagged cards, Miniature Cannon +3 on upgraded cards,
    // Mystic Lighter +9 on enchanted cards.
    if (currentPlayUniqueIdx >= 0) {
        if (currentCardStrike && hasRelic(RelicId::STRIKE_DUMMY)) r.num += 3;
        if (currentCardUpgraded && hasRelic(RelicId::MINIATURE_CANNON)) r.num += 3;
        if (currentCardEnchanted && hasRelic(RelicId::MYSTIC_LIGHTER)) r.num += 9;
    }
    // VigorPower.BeforeAttack: the owner's powered attacks gain Amount (additive), and the
    // power is consumed by that first attack
    if (int vg = powerAmount(player.powers, PowerId::VIGOR)) r.num += vg;
    // AccuracyPower: owner's powered Shiv-tagged attacks gain Amount (additive)
    if (powerAmount(player.powers, PowerId::ACCURACY) > 0 && lastPlayedWasShiv)
        r.num += powerAmount(player.powers, PowerId::ACCURACY);
    // PhantomBladesPower: owner's first Shiv play each turn gains Amount (additive)
    if (powerAmount(player.powers, PowerId::PHANTOM_BLADES) > 0 && lastPlayedWasShiv
        && shivPlaysFinishedThisTurn == 0)
        r.num += powerAmount(player.powers, PowerId::PHANTOM_BLADES);
    if (powerAmount(player.powers, PowerId::WEAK) > 0) r.mul(3, 4);
    // ShrinkPower.ModifyDamageMultiplicative: the owner's powered attacks deal
    // (100 - 30)/100 damage. The applier (ShrinkerBeetle) applies -1 = infinite stacks;
    // finite stacks tick at side end and the power is removed when the applier dies.
    if (powerAmount(player.powers, PowerId::SHRINK) != 0) r.mul(7, 10);
    if (targetIdx >= 0 && targetIdx < (int)enemies.size()) {
        const Monster& t = enemies[targetIdx];
        if (powerAmount(t.powers, PowerId::VULNERABLE) > 0) {
            // PaperPhrog.ModifyVulnerableMultiplier: +0.25 to the multiplier (1.5 -> 1.75;
            // Debilitate 2.0 -> 2.25).
            const bool deb = powerAmount(t.powers, PowerId::DEBILITATE) > 0;
            if (hasRelic(RelicId::PAPER_PHROG)) r.mul(deb ? 9 : 7, 4);
            else r.mul(deb ? 2 : 3, deb ? 1 : 2);
        }
        // ConquerorPower: a Sovereign Blade's powered attack against the power's owner
        // deals double damage.
        if (currentCardIsSovereignBlade && powerAmount(t.powers, PowerId::CONQUEROR) > 0)
            r.mul(2, 1);
        // FlutterPower.ModifyDamageMultiplicative: a powered attack against the hovering
        // ThievingHopper deals half damage (DamageDecrease = 50).
        if (powerAmount(t.powers, PowerId::FLUTTER) > 0) r.mul(1, 2);
        // SoarPower.ModifyDamageMultiplicative (OwlMagistrate JUDICIAL_FLIGHT): a powered
        // attack against the flying owner deals half damage (DamageDecrease = 50).
        if (powerAmount(t.powers, PowerId::SOAR) > 0) r.mul(1, 2);
        // SlowPower.ModifyDamageMultiplicative: (1 + 0.1 * cardsPlayedThisTurn). The
        // per-turn counter lives in PowerInst.misc; reset at the enemy side start.
        for (const auto& pw : t.powers)
            if (pw.id == PowerId::SLOW) {
                const int slow = pw.misc < 0 ? 0 : pw.misc;
                r.mul(10 + slow, 10);
            }
    }
    // DoubleDamagePower: owner's powered card attacks deal 2x (decrement per attack card)
    if (powerAmount(player.powers, PowerId::DOUBLE_DAMAGE) > 0) r.mul(2, 1);
    // LethalityPower.ModifyDamageMultiplicative: the FIRST attack card the owner started
    // this turn deals (1 + Amount/100)x; later attack cards stay at 1x. attackStartsThisTurn
    // already includes the card being resolved.
    if (int le = powerAmount(player.powers, PowerId::LETHALITY)) {
        if (attackStartsThisTurn <= 1) r.mul(100 + le, 100);
    }
    // Corrupted: the enchanted attack's powered damage is multiplied by 1.5.
    if (currentDamageCorrupted) r.mul(3, 2);
    // PenNib.ModifyDamageMultiplicative: the 10th attack card deals double damage.
    if (penNibDoubleThisPlay) r.mul(2, 1);
    return (int)r.trunc();
}

int BattleContext::countStrikeTagInPiles() const {
    // CardTag.Strike membership (PerfectedStrike multiplier input)
    int n = 0;
    auto cnt = [&](const std::vector<CardInstance>& pile) {
        for (auto& c : pile)
            if (c.def().hasTag(CT_STRIKE)) ++n;
    };
    cnt(hand); cnt(drawPile); cnt(discardPile); cnt(exhaustPile);
    return n;
}

// monster attack -> player: additive monster strength, multiplicative weak(monster)/vulnerable(player).
int BattleContext::calcMonsterAttackDamage(const Monster& m, int base) const {
    Rational r;
    r.num = base + powerAmount(m.powers, PowerId::STRENGTH);
    // TaintedPower.ModifyDamageAdditive: every enemy powered attack against the player
    // gains the stacks (applied when the player played a Tainted card this turn).
    r.num += powerAmount(player.powers, PowerId::TAINTED);
    if (powerAmount(m.powers, PowerId::WEAK) > 0) {
        if (powerAmount(m.powers, PowerId::DEBILITATE) > 0) r.mul(1, 2);
        else r.mul(3, 4);
    }
    if (powerAmount(player.powers, PowerId::VULNERABLE) > 0) r.mul(3, 2);
    // ColossusPower.ModifyDamageMultiplicative: powered attacks from a Vulnerable
    // dealer deal half damage to the owner (DamageDecrease dynvar = 0.5)
    if (powerAmount(player.powers, PowerId::COLOSSUS) > 0
        && powerAmount(m.powers, PowerId::VULNERABLE) > 0)
        r.mul(1, 2);
    // TankPower.ModifyDamageMultiplicative: the tank TAKES 1.5x powered attacks
    if (powerAmount(player.powers, PowerId::TANK) > 0)
        r.mul(3, 2);
    // SurroundedPower (KaiserCrab): attacks from the arm behind the player deal 1.5x.
    // The player's facing lives in the SURROUNDED power's misc (0 = Right, 1 = Left).
    for (const auto& p : player.powers)
        if (p.id == PowerId::SURROUNDED) {
            const bool facingLeft = p.misc == 1;
            const PowerId behind = facingLeft ? PowerId::BACK_ATTACK_RIGHT
                                              : PowerId::BACK_ATTACK_LEFT;
            if (powerAmount(m.powers, behind) > 0) r.mul(3, 2);
            break;
        }
    return (int)r.trunc();
}

void BattleContext::damageEnemy(int idx, int amount, bool powered) {
    if (amount <= 0 || battleOver) return;
    Monster& m = enemies[idx];
    if (m.reviving || m.curHp <= 0) return; // unhittable while dead/reviving
    if (powered) {
        // ThornsPower.BeforeDamageReceived (enemy owner): a powered player attack deals
        // Amount unpowered (blockable) damage back to the dealer, per hit.
        if (int th = powerAmount(m.powers, PowerId::THORNS)) damagePlayer(th, false, idx);
        if (battleOver) return;
        m.hitsTakenThisTurn += 1;  // GangUp: powered player-side hit
        // MonarchsGazePower.AfterDamageGiven: a powered hit temporarily drops the
        // target's strength by the gaze amount (TemporaryStrengthPower, Sign=-1)
        if (int mg = powerAmount(player.powers, PowerId::MONARCHS_GAZE))
            applyPowerToEnemy(idx, PowerId::MONARCHS_GAZE_STRENGTH_DOWN, mg);
        // CurlUpPower.AfterDamageReceived: the powered attack's card source is remembered;
        // the block payout fires in AfterCardPlayed, once the card finished resolving.
        if (powerAmount(m.powers, PowerId::CURL_UP) > 0 && currentPlayUniqueIdx >= 0)
            m.curlUpMarked = currentPlayUniqueIdx;
        // PersonalHivePower.AfterDamageReceived: every powered hit adds Amount Dazed cards
        // to RANDOM positions of the draw pile (CardPilePosition.Random = Shuffle.NextInt).
        if (int hive = powerAmount(m.powers, PowerId::PERSONAL_HIVE)) {
            for (int i = 0; i < hive; ++i) {
                CardInstance dz;
                dz.id = CardId::DAZED;
                dz.uniqueIdx = nextUniqueIdx++;
                dz.genCard = true;
                Sts2Rng& r = rng[(int)RngStream::Shuffle];
                const int at = r.NextInt((int)drawPile.size() + 1);
                drawPile.insert(drawPile.begin() + at, dz);
            }
        }
    }
    // HardToKillPower.ModifyDamageCap: incoming damage to the owner is capped at Amount.
    if (int cap = powerAmount(m.powers, PowerId::HARD_TO_KILL)) {
        if (amount > cap) amount = cap;
    }
    const bool hadBlock = m.block > 0;
    int absorbed = std::min(m.block, amount);
    m.block -= absorbed;
    // BurrowedPower.AfterBlockBroken: breaking the burrowed block stuns the monster
    // (force the DIZZY state, MustPerformOnce) and removes the power.
    if (hadBlock && absorbed > 0 && m.block == 0 &&
        powerAmount(m.powers, PowerId::BURROWED) > 0) {
        const MonsterDef& mdb = m.def(*this);
        for (int si = 0; si < (int)mdb.states.size(); ++si) {
            if (std::strcmp(mdb.states[si].moveId, "DIZZY_MOVE") == 0) {
                m.curState = si;
                m.nextMove = "DIZZY_MOVE";
                m.gate = true;
                m.tunnelerStunned = true;
                break;
            }
        }
        m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
            [](const PowerInst& pw) { return pw.id == PowerId::BURROWED; }), m.powers.end());
    }
    int through = amount - absorbed;
    // SlipperyPower.ModifyHpLostAfterOsty: unblocked HP loss is capped at 1, and the
    // power decrements whenever at least 1 unblocked damage got through.
    if (through > 0 && powerAmount(m.powers, PowerId::SLIPPERY) > 0) {
        through = 1;
        for (auto it = m.powers.begin(); it != m.powers.end();) {
            if (it->id == PowerId::SLIPPERY) {
                if (--it->amount <= 0) it = m.powers.erase(it);
                else ++it;
            } else ++it;
        }
    }
    // PlowPower.AfterDamageReceived: unblocked damage that leaves the owner at or below
    // the power's Amount staggers the beast — temporary + permanent strength are removed
    // and the STUN move is forced (MustPerformOnce, no StateLog entry).
    if (through > 0) {
        int plowAmt = -1;
        for (const auto& pw : m.powers)
            if (pw.id == PowerId::PLOW) plowAmt = pw.amount;
        if (plowAmt >= 0 && m.curHp - through <= plowAmt) {
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::SETUP_STRIKE; }),
                m.powers.end());
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::STRENGTH || p.id == PowerId::PLOW; }),
                m.powers.end());
            const MonsterDef& md2 = m.def(*this);
            for (int si = 0; si < (int)md2.states.size(); ++si) {
                if (std::strcmp(md2.states[si].moveId, "STUN_MOVE") == 0) {
                    m.curState = si;
                    m.nextMove = "STUN_MOVE";
                    m.gate = true; // MustPerformOnceBeforeTransitioning
                    break;
                }
            }
        }
    }

    if (through > 0) {
        m.curHp -= through;
        // SlumberPower.AfterDamageReceived: unblocked damage decrements Slumber; at 0 the
        // beetle wakes (Plating leaves) and is stunned — STUNNED performs once, then the
        // machine follows into ROLL_OUT (CreatureCmd.Stun with nextMoveId).
        if (m.curHp > 0) {
            bool woke = false;
            for (auto it = m.powers.begin(); it != m.powers.end();) {
                if (it->id == PowerId::SLUMBER) {
                    if (--it->amount <= 0) { it = m.powers.erase(it); woke = true; }
                    else ++it;
                } else ++it;
            }
            if (woke) {
                m.isAwake = true;
                m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                    [](const PowerInst& p) { return p.id == PowerId::PLATING; }),
                    m.powers.end());
                const MonsterDef& mdw = m.def(*this);
                for (int si = 0; si < (int)mdw.states.size(); ++si)
                    if (std::strcmp(mdw.states[si].moveId, "STUNNED") == 0) {
                        m.curState = si;
                        m.nextMove = "STUNNED";
                        m.gate = true;
                        break;
                    }
            }
        }
        // FlutterPower.AfterDamageReceived: each unblocked powered hit burns one stack;
        // at 0 the owner is stunned — STUNNED performs once, then the machine follows the
        // LAST logged move's own follow-up (CreatureCmd.Stun with nextState).
        if (powered && m.curHp > 0) {
            bool unfluttered = false;
            for (auto it = m.powers.begin(); it != m.powers.end();) {
                if (it->id == PowerId::FLUTTER) {
                    if (--it->amount <= 0) { it = m.powers.erase(it); unfluttered = true; }
                    else ++it;
                } else ++it;
            }
            if (unfluttered) {
                const MonsterDef& mdf = m.def(*this);
                int stunnedIdx = -1, nextIdx = -1;
                for (int si = 0; si < (int)mdf.states.size(); ++si)
                    if (std::strcmp(mdf.states[si].moveId, "STUNNED") == 0) stunnedIdx = si;
                if (!m.moveHistory.empty()) {
                    const std::string& last = m.moveHistory.back();
                    for (int si = 0; si < (int)mdf.states.size(); ++si)
                        if (last == mdf.states[si].moveId) nextIdx = mdf.states[si].next;
                }
                if (stunnedIdx >= 0) {
                    m.curState = stunnedIdx;
                    m.nextMove = "STUNNED";
                    m.gate = true;
                    m.stunNextState = nextIdx;
                }
            }
        }
        // EnvenomPower.AfterDamageGiven: powered attack with unblocked damage -> Poison
        if (powered) {
            if (int env = powerAmount(player.powers, PowerId::ENVENOM)) {
                if (!battleOver && m.curHp > 0 && !m.reviving)
                    addPower(m.powers, PowerId::POISON, env, false);
            }
        }
        if (m.curHp <= 0) {
            m.curHp = 0;
            onEnemyDeath(idx);
        }
    }
}

void BattleContext::damagePlayer(int amount, bool poweredAttack, int dealerIdx) {
    if (amount <= 0 || battleOver) return;
    // IntangiblePower.ModifyDamageCap: the incoming DAMAGE of each instance is capped at
    // 1 BEFORE block (so a 3-block-hitting attack loses at most 1 to block per hit);
    // ModifyHpLostAfterOsty caps the unblocked HP loss as well (see below).
    if (amount > 1 && powerAmount(player.powers, PowerId::INTANGIBLE) > 0) amount = 1;
    // CreatureCmd.Damage with DieForYouPower (corpus evidence, MATRIX_BANSHEES_CRY):
    //   1. the OWNER's block absorbs first (creature = originalTarget.PetOwner?.Creature)
    //   2. the unblocked remainder is redirected INTO the pet
    //   3. only the pet's OVERKILL comes back to the owner
    //   (a dead pet means no redirect at all)
    int absorbed = std::min(player.block, amount);
    player.block -= absorbed;
    int through = amount - absorbed;

    if (poweredAttack && through > 0 && player.isOstyAlive()) {
        OstyState& o = player.osty;
        int hpBefore = o.curHp;
        o.curHp -= through;
        int overkill = 0;
        if (o.curHp <= 0) {
            o.curHp = 0;
            o.block = 0;              // death clears the pet's block
            overkill = through - hpBefore;
        }
        // NecroMasteryPower.AfterCurrentHpChanged: every point of HP the pet loses deals
        // Amount unblockable/unpowered damage to ALL hittable enemies.
        const int petLost = hpBefore - o.curHp;
        if (petLost > 0) {
            if (int nm = powerAmount(player.powers, PowerId::NECRO_MASTERY)) {
                const int dmg = petLost * nm;
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    Monster& e = enemies[i];
                    if (e.removed || e.curHp <= 0 || e.reviving) continue;
                    e.curHp -= dmg; // unblockable: block is not consulted
                    if (e.curHp <= 0) {
                        e.curHp = 0;
                        onEnemyDeath(i);
                    }
                }
                if (battleOver) return;
            }
        }
        // Thorns/FlameBarrier still fire: the owner did receive a powered attack,
        // though it landed on the pet
        if (dealerIdx >= 0) {
            if (int th = powerAmount(player.powers, PowerId::THORNS)) {
                if (dealerIdx < (int)enemies.size() && !enemies[dealerIdx].removed
                    && enemies[dealerIdx].curHp > 0)
                    damageEnemy(dealerIdx, th, /*powered=*/false);
            }
            if (int th = powerAmount(player.powers, PowerId::FLAME_BARRIER)) {
                if (dealerIdx < (int)enemies.size() && !enemies[dealerIdx].removed
                    && enemies[dealerIdx].curHp > 0)
                    damageEnemy(dealerIdx, th, /*powered=*/false);
            }
        }
        through = overkill;          // only overkill reaches the owner
        if (through <= 0) return;
    }

    // ThornsPower: receiving a powered attack hurts the dealer (blocked or not)
    if (poweredAttack && dealerIdx >= 0) {
        if (int th = powerAmount(player.powers, PowerId::THORNS)) {
            if (dealerIdx < (int)enemies.size() && !enemies[dealerIdx].removed
                && enemies[dealerIdx].curHp > 0)
                damageEnemy(dealerIdx, th, /*powered=*/false); // unpowered retaliation
        }
    }
    // FlameBarrierPower: receiving a powered attack burns the dealer (blocked or not)
    if (poweredAttack && dealerIdx >= 0) {
        if (int th = powerAmount(player.powers, PowerId::FLAME_BARRIER)) {
            if (dealerIdx < (int)enemies.size() && !enemies[dealerIdx].removed
                && enemies[dealerIdx].curHp > 0)
                damageEnemy(dealerIdx, th, /*powered=*/false); // unpowered retaliation
        }
    }
    // InfernoPower retaliation: unblocked damage during the player's own side
    // burns all enemies (self-damage cards like Bloodletting/Offering trigger it)
    if (!poweredAttack && through > 0) {
        if (int amt = powerAmount(player.powers, PowerId::INFERNO)) {
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                    damageEnemy(i, amt, /*powered=*/false);
        }
    }

    if (through > 0) {
        // IntangiblePower.ModifyHpLostAfterOsty: each hp-loss instance shrinks to 1
        if (powerAmount(player.powers, PowerId::INTANGIBLE) > 0) through = std::min(through, 1);
        // BufferPower.ModifyHpLostAfterOstyLate: the loss becomes 0 and one stack burns
        if (powerAmount(player.powers, PowerId::BUFFER) > 0 && through > 0) {
            for (auto it = player.powers.begin(); it != player.powers.end(); ++it) {
                if (it->id != PowerId::BUFFER) continue;
                if (--it->amount <= 0) player.powers.erase(it);
                break;
            }
            through = 0;
        }
        // TungstenRod.ModifyHpLostAfterOsty: each hp-loss instance is reduced by 1.
        if (through > 0 && hasRelic(RelicId::TUNGSTEN_ROD))
            through = std::max(0, through - 1);
        // BeatingRemnant.ModifyHpLostAfterOsty: cap the hp lost this player turn at 20.
        if (through > 0 && hasRelic(RelicId::BEATING_REMNANT))
            through = std::min(through, std::max(0, 20 - relicBeatingDamageThisTurn));
        if (through > 0) {
            hpLostThisTurn += through; // DamageReceivedEntry.UnblockedDamage > 0
            ++hpLossEventsCombat;      // TearAsunder: one entry per hp-loss instance
            player.curHp -= through;
            if (player.curHp <= 0) {
                player.curHp = 0;
                battleOver = true;
                playerWon = false;
            }
            // CentennialPuzzle.AfterDamageReceived: first unblocked damage each combat
            // draws 3.
            if (!battleOver && hasRelic(RelicId::CENTENNIAL_PUZZLE) && !relicPuzzleUsed) {
                relicPuzzleUsed = true;
                drawCards(3, /*naturalDraw=*/false);
            }
            if (hasRelic(RelicId::BEATING_REMNANT)) relicBeatingDamageThisTurn += through;
            if (hasRelic(RelicId::LAVA_LAMP)) relicLavaLampTookDamage = true;
            // DemonTongue: first unblocked damage on the player's own side heals it back.
            if (hasRelic(RelicId::DEMON_TONGUE) && !inEnemyPhase && !relicDemonTongueUsed) {
                relicDemonTongueUsed = true;
                player.curHp = std::min(player.maxHp, player.curHp + through);
            }
            // SelfFormingClay: unblocked damage applies BlockNextTurn 3.
            if (hasRelic(RelicId::SELF_FORMING_CLAY))
                addPower(player.powers, PowerId::SELF_FORMING_CLAY, 3, false);
        }
    }
    syncRedSkull();
    // PaperCutsPower.AfterDamageGiven (ScrollOfBiting): the owner's powered attack that
    // dealt unblocked damage makes the player lose Amount Max HP. LoseMaxHp lowers the
    // max and damages current HP down to it (unblockable/unpowered).
    if (!battleOver && poweredAttack && through > 0 && dealerIdx >= 0
        && dealerIdx < static_cast<int>(enemies.size())) {
        if (int pc = powerAmount(enemies[dealerIdx].powers, PowerId::PAPER_CUTS)) {
            const int newMax = std::max(1, player.maxHp - pc);
            if (newMax < player.curHp) {
                player.curHp = newMax;
                if (player.curHp <= 0) {
                    player.curHp = 0;
                    battleOver = true;
                    playerWon = false;
                }
            }
            player.maxHp = newMax;
        }
    }
}

void BattleContext::damagePlayerUnblockable(int amount) {
    if (amount <= 0 || battleOver) return;
    // Inferno retaliation: its own turn-start self-burn counts as unblocked damage
    // received on the player's side, so the AoE burn fires (corpus INFERNO T3)
    if (int amt = powerAmount(player.powers, PowerId::INFERNO)) {
        for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                damageEnemy(i, amt, /*powered=*/false);
    }
    if (powerAmount(player.powers, PowerId::INTANGIBLE) > 0) amount = std::min(amount, 1);
    if (powerAmount(player.powers, PowerId::BUFFER) > 0 && amount > 0) {
        for (auto it = player.powers.begin(); it != player.powers.end(); ++it) {
            if (it->id != PowerId::BUFFER) continue;
            if (--it->amount <= 0) player.powers.erase(it);
            break;
        }
        amount = 0;
    }
    // TungstenRod.ModifyHpLostAfterOsty: each hp-loss instance is reduced by 1.
    if (amount > 0 && hasRelic(RelicId::TUNGSTEN_ROD))
        amount = std::max(0, amount - 1);
    if (amount > 0 && hasRelic(RelicId::BEATING_REMNANT))
        amount = std::min(amount, std::max(0, 20 - relicBeatingDamageThisTurn));
    if (amount > 0) {
        hpLostThisTurn += amount;
        ++hpLossEventsCombat; // TearAsunder: unblockable hp-loss instance
        player.curHp -= amount;
        if (player.curHp <= 0) {
            player.curHp = 0;
            battleOver = true;
            playerWon = false;
        }
        if (!battleOver && hasRelic(RelicId::CENTENNIAL_PUZZLE) && !relicPuzzleUsed) {
            relicPuzzleUsed = true;
            drawCards(3, /*naturalDraw=*/false);
        }
        if (hasRelic(RelicId::BEATING_REMNANT)) relicBeatingDamageThisTurn += amount;
        if (hasRelic(RelicId::DEMON_TONGUE) && !inEnemyPhase && !relicDemonTongueUsed) {
            relicDemonTongueUsed = true;
            player.curHp = std::min(player.maxHp, player.curHp + amount);
        }
        if (hasRelic(RelicId::SELF_FORMING_CLAY))
            addPower(player.powers, PowerId::SELF_FORMING_CLAY, 3, false);
    }
    syncRedSkull();
}

void BattleContext::syncRedSkull() {
    if (!hasRelic(RelicId::RED_SKULL)) return;
    const bool low = player.maxHp > 0 && player.curHp * 2 <= player.maxHp;
    if (low == redSkullActive) return;
    // RedSkull.ModifyStrengthIfNecessary: +3 while at or below 50% HP, removed above it.
    applyPowerToPlayer(PowerId::STRENGTH, low ? 3 : -3);
    redSkullActive = low;
}

void BattleContext::exhaustCard(const CardInstance& c, bool causedByEthereal) {
    exhaustPile.push_back(c);
    if (causedByEthereal) {
        if (hasRelic(RelicId::JOSS_PAPER)) ++relicJossEthereal;
        return;
    }
    ++cardExhaustsThisTurn;
    if (hasRelic(RelicId::JOSS_PAPER)) ++relicJossExhausted;
    // BurningSticks.AfterCardExhausted: the first exhausted Skill returns as a clone.
    if (hasRelic(RelicId::BURNING_STICKS) && !relicBurningSticksUsed
        && c.def().type == CardType::SKILL) {
        relicBurningSticksUsed = true;
        CardInstance clone = c;
        clone.uniqueIdx = ++nextUniqueIdx;
        clone.genCard = true;
        clone.stampText.clear(); // CardFactory.CreateClone: a fresh combat card (deck flag False)
        clone.deckIdx = -1;
        if ((int)hand.size() < maxHandSize) hand.push_back(clone);
        else discardPile.push_back(clone);
    }
}

void BattleContext::applyRelicTurnStart() {
    // Brimstone.AfterSideTurnStart: +2 Strength to the owner, +1 to every enemy.
    if (hasRelic(RelicId::BRIMSTONE)) {
        addPower(player.powers, PowerId::STRENGTH, 2, false);
        for (auto& m : enemies)
            if (!m.removed && m.curHp > 0 && !m.reviving)
                addPower(m.powers, PowerId::STRENGTH, 1, false);
    }
    // MercuryHourglass.AfterSideTurnStart: 3 unpowered damage to every hittable enemy.
    if (hasRelic(RelicId::MERCURY_HOURGLASS)) {
        for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                damageEnemy(i, 3, /*powered=*/false);
    }
}

void BattleContext::gainBlock(int amount) { (void)gainBlockReturns(amount); }

int BattleContext::gainBlockReturns(int amount) {
    // NoBlockPower.ModifyBlockMultiplicative: while active, powered card block gains are
    // multiplied to 0 (unpowered block, e.g. Plating, is unaffected)
    if (powerAmount(player.powers, PowerId::NO_BLOCK) > 0) return 0;
    Rational r;
    r.num = amount + powerAmount(player.powers, PowerId::DEXTERITY);
    // Nimble: the card's enchantment adds Amount to its block gain.
    if (currentCardBlockBonus != 0) ++nimbleApplications;
    r.num += currentCardBlockBonus;
    // FastenPower.ModifyBlockAdditive: Defend-tagged cards gain Amount block
    if (lastPlayedWasDefend) r.num += powerAmount(player.powers, PowerId::FASTEN);
    // UnmovablePower.ModifyBlockMultiplicative: the owner's first Amount card-sourced
    // block gains each turn are doubled
    if (int um = powerAmount(player.powers, PowerId::UNMOVABLE)) {
        if (blockGainsThisTurn < um) r.mul(2, 1);
    }
    // Vambrace.ModifyBlockMultiplicative: the first card-sourced block gain each combat
    // is doubled (all block gains of that same card are included).
    if (hasRelic(RelicId::VAMBRACE) && currentPlayUniqueIdx >= 0 && !relicVambraceUsed
        && (relicVambraceCardIdx < 0 || relicVambraceCardIdx == currentPlayUniqueIdx)) {
        r.mul(2, 1);
        relicVambraceCardIdx = currentPlayUniqueIdx;
    }
    if (powerAmount(player.powers, PowerId::FRAIL) > 0) r.mul(3, 4);
    // ShadowmeldPower.ModifyBlockMultiplicative: owner's block gain is doubled per stack
    // (Math.Pow(2, Amount))
    if (int sm = powerAmount(player.powers, PowerId::SHADOWMELD)) {
        for (int i = 0; i < sm && i < 20; ++i) r.mul(2, 1);
    }
    int gained = (int)r.trunc();
    ++blockGainsThisTurn; // UnmovablePower: card-sourced block-gain events this turn
    player.block += gained;
    // JuggernautPower: gaining block smashes a random enemy (CombatTargets stream)
    if (gained > 0 && !battleOver) {
        if (int amt = powerAmount(player.powers, PowerId::JUGGERNAUT)) {
            std::vector<int> hittable;
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
            if (!hittable.empty()) {
                Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
                int t = hittable[rt.NextInt((int)hittable.size())];
                damageEnemy(t, amt, /*powered=*/false);
            }
        }
    }
    return gained;
}

// ValueProp.Unpowered block gain (Plating/Rage family): no dexterity/frail modifiers,
// but Juggernaut still triggers on block gained
void BattleContext::gainBlockUnpowered(int amount) {
    if (amount <= 0) return;
    if (currentCardBlockBonus != 0) ++nimbleApplications;
    player.block += amount + currentCardBlockBonus; // Nimble
    if (!battleOver) {
        if (int amt = powerAmount(player.powers, PowerId::JUGGERNAUT)) {
            std::vector<int> hittable;
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
            if (!hittable.empty()) {
                Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
                int t = hittable[rt.NextInt((int)hittable.size())];
                damageEnemy(t, amt, /*powered=*/false);
            }
        }
    }
}

// CombatManager CardPlaysStarted bookkeeping (stamp Y slots 2-4 + AttackStarts)
void BattleContext::recordCardPlayStart(const CardInstance& c, int energyValue) {
    const CardDef& d = c.def();
    ++cardPlayStarts;
    if (d.type == CardType::ATTACK || d.type == CardType::SKILL) ++attackSkillStarts;
    if (d.type == CardType::ATTACK) {
        ++attackStartsThisTurn;
        if (energyValue == 0) ++zeroCostAttackStarts;
    }
}

// ---------------- orbs ----------------

const char* orbStampName(OrbKind k) {
    switch (k) {
        case OrbKind::LIGHTNING: return "LIGHTNING_ORB";
        case OrbKind::FROST:     return "FROST_ORB";
        case OrbKind::DARK:      return "DARK_ORB";
        case OrbKind::PLASMA:    return "PLASMA_ORB";
        case OrbKind::GLASS:     return "GLASS_ORB";
        default:                 return "-";
    }
}

OrbKind orbFromStampName(const std::string& name) {
    if (name == "LIGHTNING_ORB") return OrbKind::LIGHTNING;
    if (name == "FROST_ORB") return OrbKind::FROST;
    if (name == "DARK_ORB") return OrbKind::DARK;
    if (name == "PLASMA_ORB") return OrbKind::PLASMA;
    if (name == "GLASS_ORB") return OrbKind::GLASS;
    return OrbKind::NONE;
}

// ModifyOrbValue hook chain (FocusPower.AddOrbValue): the stamped value is always
// recomputed at trigger time. Per-orb game semantics:
//   Lightning/Frost/Dark passive + Lightning/Frost evoke: base + Focus, floored at 0
//   Plasma: raw 1/2 (its PassiveVal/EvokeVal never call ModifyOrbValue)
//   Dark evoke: the live accumulator (raw, no Focus)
//   Glass: passive = glassPassive + Focus (floored); evoke = that value * 2
int BattleContext::orbValue(const OrbInstance& o, bool evoke) const {
    int focus = powerAmount(player.powers, PowerId::FOCUS);
    switch (o.kind) {
        case OrbKind::LIGHTNING: return std::max(0, (evoke ? 8 : 3) + focus);
        case OrbKind::FROST:     return std::max(0, (evoke ? 5 : 2) + focus);
        case OrbKind::DARK:      return evoke ? o.darkEvoke : std::max(0, 6 + focus);
        case OrbKind::PLASMA:    return evoke ? 2 : 1;
        case OrbKind::GLASS: {
            int pv = std::max(0, o.glassPassive + focus);
            return evoke ? pv * 2 : pv;
        }
        default:                 return 0;
    }
}

// OrbCmd.AddSlots: capped so capacity never exceeds OrbQueue.maxCapacity (10)
void BattleContext::addOrbSlots(int amount) {
    if (amount <= 0) return;
    int room = 10 - player.orbCapacity;
    if (room <= 0) return;
    player.orbCapacity += std::min(room, amount);
}

// OrbCmd.RemoveSlots: reduces capacity and drops orbs from the BACK of the queue
void BattleContext::removeOrbSlots(int amount) {
    if (amount <= 0) return;
    int drop = std::min(player.orbCapacity, amount);
    player.orbCapacity -= drop;
    while ((int)player.orbs.size() > player.orbCapacity) player.orbs.pop_back();
}

int BattleContext::darkOrbCount() const {
    int n = 0;
    for (const auto& o : player.orbs) if (o.kind == OrbKind::DARK) ++n;
    return n;
}

int BattleContext::lightningOrbCount() const {
    int n = 0;
    for (const auto& o : player.orbs) if (o.kind == OrbKind::LIGHTNING) ++n;
    return n;
}

int BattleContext::frostOrbCount() const {
    int n = 0;
    for (const auto& o : player.orbs) if (o.kind == OrbKind::FROST) ++n;
    return n;
}

// group orb by orb.Id -> Count() (CompileDriver/Synchronize/Coolant multiplier)
int BattleContext::distinctOrbKinds() const {
    bool seen[6] = {false, false, false, false, false, false};
    int n = 0;
    for (const auto& o : player.orbs) {
        int k = (int)o.kind;
        if (k > 0 && k < 6 && !seen[k]) { seen[k] = true; ++n; }
    }
    return n;
}

// OrbCmd.Channel: a character with BaseOrbSlotCount 0 auto-gains one slot on the first
// channel; a full queue evokes the front orb first to make room (OrbCmd.cs).
void BattleContext::channelOrb(OrbKind kind) {
    if (kind == OrbKind::NONE || battleOver) return;
    if (player.orbCapacity == 0) addOrbSlots(1);
    if ((int)player.orbs.size() >= player.orbCapacity) {
        if (!evokeFront(true)) return; // full but nothing to evoke: channel fails
        if (battleOver) return;
    }
    OrbInstance o;
    o.kind = kind;
    if (kind == OrbKind::DARK) o.darkEvoke = 6;
    if (kind == OrbKind::GLASS) o.glassPassive = 4;
    player.orbs.push_back(o);
    if (kind == OrbKind::LIGHTNING) ++lightningChanneled; // Voltaic multiplier entry
}

// OrbCmd.EvokeNext / EvokeLast: trigger one orb's Evoke, then (optionally) dequeue it.
bool BattleContext::evokeFront(bool dequeue) {
    if (player.orbs.empty()) return false;
    OrbInstance o = player.orbs.front();
    int v = orbValue(o, /*evoke=*/true);
    switch (o.kind) {
        case OrbKind::LIGHTNING: {
            // random hittable enemy via CombatTargets (unpowered damage)
            std::vector<int> hittable;
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
            if (!hittable.empty()) {
                Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
                int t = hittable[rt.NextInt((int)hittable.size())];
                damageEnemy(t, v, /*powered=*/false);
            }
            break;
        }
        case OrbKind::FROST:
            gainBlockUnpowered(v);
            break;
        case OrbKind::DARK: {
            // weakest enemy by current HP (MinBy CurrentHp; hittable enemies only)
            int best = -1;
            for (int i = 0; i < (int)enemies.size(); ++i) {
                if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                if (best < 0 || enemies[i].curHp < enemies[best].curHp) best = i;
            }
            if (best >= 0) damageEnemy(best, v, /*powered=*/false);
            break;
        }
        case OrbKind::PLASMA:
            player.energy += v; // PlayerCmd.GainEnergy
            break;
        case OrbKind::GLASS: {
            if (v > 0) {
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                    damageEnemy(i, v, /*powered=*/false);
                }
            }
            break;
        }
        default: break;
    }
    if (dequeue && !player.orbs.empty()) player.orbs.erase(player.orbs.begin());
    return true;
}

bool BattleContext::evokeLast() {
    if (player.orbs.empty()) return false;
    OrbInstance o = player.orbs.back();
    player.orbs.pop_back();
    // reuse the front path by temporarily pushing it to the front
    player.orbs.insert(player.orbs.begin(), o);
    return evokeFront(true);
}

// OrbCmd.Passive: one passive trigger (no ModifyOrbPassiveTriggerCount here).
void BattleContext::triggerOrbPassive(OrbInstance& o, int targetIdx) {
    switch (o.kind) {
        case OrbKind::LIGHTNING: {
            // target when given (TeslaCoil), else a random hittable enemy
            int t = targetIdx;
            if (t < 0 || t >= (int)enemies.size() || enemies[t].removed || enemies[t].curHp <= 0
                || enemies[t].reviving) {
                std::vector<int> hittable;
                for (int i = 0; i < (int)enemies.size(); ++i)
                    if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
                if (hittable.empty()) return;
                Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
                t = hittable[rt.NextInt((int)hittable.size())];
            }
            damageEnemy(t, orbValue(o, false), /*powered=*/false);
            break;
        }
        case OrbKind::FROST:
            gainBlockUnpowered(orbValue(o, false));
            break;
        case OrbKind::DARK:
            // PassiveVal accumulates into the evoke value; nothing happens now
            o.darkEvoke += orbValue(o, false);
            break;
        case OrbKind::PLASMA:
            player.energy += orbValue(o, false);
            break;
        case OrbKind::GLASS: {
            int v = orbValue(o, false);
            if (v <= 0) return; // zero-value passives neither damage nor decay
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                damageEnemy(i, v, /*powered=*/false);
            }
            o.glassPassive = std::max(0, o.glassPassive - 1); // decays after the hit
            break;
        }
        default: break;
    }
}

// OrbQueue.BeforeTurnEnd: every orb's BeforeTurnEndOrbTrigger (Lightning/Frost/Dark/Glass
// override it; Plasma's is on AfterTurnStartOrbTrigger instead). Passives never reorder
// the queue, so index iteration is stable.
void BattleContext::orbBeforeTurnEndTriggers() {
    for (size_t i = 0; i < player.orbs.size() && !battleOver; ++i) {
        if (player.orbs[i].kind == OrbKind::PLASMA) continue;
        triggerOrbPassive(player.orbs[i]);
    }
}

// OrbQueue.AfterTurnStart: Plasma orbs fire their passive at player turn start.
void BattleContext::orbAfterTurnStartTriggers() {
    for (size_t i = 0; i < player.orbs.size() && !battleOver; ++i)
        if (player.orbs[i].kind == OrbKind::PLASMA) triggerOrbPassive(player.orbs[i]);
}

void BattleContext::checkBattleOver() {
    // win = no alive OR reviving primary enemy (minions never block)
    for (auto& m : enemies) {
        if (m.removed) continue;
        if (m.defIdx >= 0 && m.def(*this).minion) continue;
        if (m.curHp > 0 || m.reviving) return;
    }
    battleOver = true;
    playerWon = true;
}

void BattleContext::onEnemyDeath(int idx) {
    Monster& m = enemies[idx];
    if (m.defIdx < 0) { checkBattleOver(); return; }
    const MonsterDef& md = m.def(*this);

    // respawn chain (AXEBOT Stock): fresh body, same slot, stock-1, starts BOOT_UP
    if (md.respawns) {
        int stock = powerAmount(m.powers, PowerId::STOCK);
        if (stock > 0) {
            Monster e;
            e.defIdx = m.defIdx;
            e.name = m.name;
            e.slot = m.slot;
            e.combatId = ++nextCombatId;
            int bonus = (2 - (stock - 1)) * 10; // respawn hp bonus: 1st +10, 2nd +20
            e.maxHp = e.curHp = md.maxHp + bonus; // stamp carries exact values on replay anyway
            e.stockLeft = stock - 1;
            addPower(e.powers, PowerId::STOCK, stock - 1, false);
            // respawned body starts BOOT_UP (idx 2 in the axebot table); the fresh machine
            // logs its initial state at construction and gates its first roll
            e.curState = 2;
            e.nextMove = md.states[2].moveId;
            e.moveHistory.push_back(md.states[2].moveId);
            e.gate = true;
            enemies.push_back(e);
            // old body is removed (replaced in its slot)
            enemies.erase(enemies.begin() + idx);
            return; // combat cannot end while a fresh body stands
        }
    }

    // scheduled revive (segments: DEAD->REATTACH; eye: REVIVE) — forced states enter via
    // SetMoveImmediate: never logged, and must perform once before the machine transitions
    if (md.deathState >= 0) {
        m.reviving = true;
        m.curState = md.deathState;
        m.nextMove = md.states[md.deathState].moveId;
        m.gate = true;
        return;
    }
    m.removed = true; // truly dead: leaves the stamp entirely
    // PossessStrength/SpeedPower.AfterDeath: the owner hands the stolen stat back to the
    // player (TheLost / TheForgotten).
    if (m.stolenStrength > 0) applyPowerToPlayer(PowerId::STRENGTH, m.stolenStrength);
    if (m.stolenDexterity > 0) applyPowerToPlayer(PowerId::DEXTERITY, m.stolenDexterity);
    // CrabRagePower.AfterDeath (KaiserCrab): the surviving arm enrages — +6 strength and
    // 99 unpowered block — and CrabRage leaves.
    for (auto& o : enemies) {
        if (o.removed || o.curHp <= 0 || o.combatId == m.combatId) continue;
        if (powerAmount(o.powers, PowerId::CRAB_RAGE) > 0) {
            addPower(o.powers, PowerId::STRENGTH, 6, false);
            o.block += 99;
            o.powers.erase(std::remove_if(o.powers.begin(), o.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::CRAB_RAGE; }), o.powers.end());
        }
    }
    // SurroundedPower.AfterDeath: with only one arm left the player faces it (no rear
    // bonus once the other arm is gone).
    {
        std::vector<int> hittable;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0) hittable.push_back(i);
        if (hittable.size() == 1) {
            const Monster& t = enemies[hittable[0]];
            for (auto& p : player.powers)
                if (p.id == PowerId::SURROUNDED) {
                    if (p.misc != 1 && powerAmount(t.powers, PowerId::BACK_ATTACK_LEFT) > 0)
                        p.misc = 1;
                    else if (p.misc == 1 && powerAmount(t.powers, PowerId::BACK_ATTACK_RIGHT) > 0)
                        p.misc = 0;
                    break;
                }
        }
    }
    // InfestedPower.AfterDeath: four stunned Wrigglers join the enemy side and keep the
    // combat alive (ShouldStopCombatFromEnding). Slots wriggler1..4 decide the INIT move.
    if (md.name && std::strcmp(md.name, "PHROG_PARASITE") == 0 &&
        powerAmount(m.powers, PowerId::INFESTED) > 0) {
        const int wdef = monsterDefIdx("WRIGGLER");
        if (wdef >= 0) {
            const MonsterDef& wd = monsterDef(wdef);
            std::vector<int> used;
            for (const auto& e : enemies)
                if (!e.removed) used.push_back(e.maxHp);
            for (int k = 0; k < 4; ++k) {
                std::vector<int> cand;
                for (int hp = wd.minHp; hp <= wd.maxHp; ++hp)
                    if (std::find(used.begin(), used.end(), hp) == used.end()) cand.push_back(hp);
                int hp = wd.maxHp;
                if (!cand.empty())
                    hp = cand[rng[(int)RngStream::Niche].NextInt((int)cand.size())];
                used.push_back(hp);
                Monster w;
                w.combatId = ++nextCombatId;
                w.defIdx = wdef;
                w.name = "WRIGGLER";
                w.slot = "wriggler" + std::to_string(k + 1);
                w.maxHp = hp;
                w.curHp = hp;
                w.curState = 0; // SPAWNED_MOVE (StartStunned)
                w.nextMove = "SPAWNED_MOVE";
                w.moveHistory.push_back("SPAWNED_MOVE");
                w.gate = true;
                w.slotInitAlt = ((k + 1) % 2 == 1) ? 1 : 2; // odd -> BITE, even -> WRIGGLE
                applyStartPowers(w, wd);
                enemies.push_back(w);
            }
        }
    }
    // ShrinkPower.AfterDeath / ConstrictPower.AfterDeath: these player debuffs leave when
    // their applier dies. The engine does not store per-power appliers, so this maps to
    // the act-1 sources (ShrinkerBeetle / SlitheringStrangler).
    if (md.name && std::strcmp(md.name, "SHRINKER_BEETLE") == 0) {
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::SHRINK; }), player.powers.end());
    }
    if (md.name && std::strcmp(md.name, "SLITHERING_STRANGLER") == 0) {
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::CONSTRICT; }), player.powers.end());
    }
    // HexPower.AfterDeath: the hex leaves with its applier (the SpectralKnight); removing
    // the power also clears every Hexed affliction it applied (AfterRemoved).
    if (md.name && std::strcmp(md.name, "SPECTRAL_KNIGHT") == 0) {
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::HEX; }), player.powers.end());
        clearAffliction(5);
    }
    // DampenPower.AfterDeath: the power remembers its casters; it leaves once the last one
    // is gone (only the MagiKnight applies it). AfterRemoved re-upgrades the cards it
    // downgraded.
    if (md.name && std::strcmp(md.name, "MAGI_KNIGHT") == 0) {
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::DAMPEN; }), player.powers.end());
        for (int idx : dampenDowngradedDeckIdx) {
            for (auto* pile : {&hand, &drawPile, &discardPile, &exhaustPile})
                for (auto& c : *pile)
                    if (c.deckIdx == idx && c.upgrade == 0) { ++c.upgrade; c.stampText.clear(); }
        }
        dampenDowngradedDeckIdx.clear();
    }
    // Queen.AfterDeath(TorchHeadAmalgam): HasAmalgamDied flips and a pending
    // BURN_BRIGHT_FOR_ME is replaced by ENRAGE (SetMoveImmediate).
    if (md.name && std::strcmp(md.name, "TORCH_HEAD_AMALGAM") == 0) {
        for (auto& q : enemies) {
            if (q.removed || q.defIdx < 0) continue;
            if (std::strcmp(q.def(*this).name, "QUEEN") != 0) continue;
            q.amalgamDied = true;
            if (q.nextMove == "BURN_BRIGHT_FOR_ME_MOVE") {
                q.curState = 5; // ENRAGE_MOVE
                q.nextMove = "ENRAGE_MOVE";
                q.gate = true; // SetMoveImmediate: the forced move performs first
            }
        }
    }
    checkBattleOver();
}

// ---------------- piles ----------------

void BattleContext::drawCards(int n, bool naturalDraw) {
    // NoDrawPower.ShouldDraw: the natural hand draw passes, effect draws are blocked
    if (!naturalDraw && powerAmount(player.powers, PowerId::NO_DRAW) > 0) return;
    for (int i = 0; i < n && (int)hand.size() < maxHandSize; ++i) {
        if (drawPile.empty()) {
            if (discardPile.empty()) break;
            shuffleDrawFromDiscard();
        }
        hand.push_back(drawPile.front());
        drawPile.erase(drawPile.begin()); // top of pile = first element (game CardPile convention)
        // Slither.AfterCardDrawn: the drawn card's cost is randomized for this combat.
        if (hand.back().enchant == static_cast<int>(EnchantmentId::SLITHER)) {
            hand.back().costOverride =
                rng[static_cast<int>(RngStream::CombatEnergyCosts)].NextInt(4);
            ++slitherDraws;
        }
        ++totalCardsDrawn; // Murder multiplier (CardDrawnEntry)
        if (!naturalDraw) ++effectDrawsThisTurn; // DeathMarch: !FromHandDraw
        // ChainsOfBindingPower.AfterCardDrawn: while fewer than Amount cards have been
        // Bound this turn, each newly drawn card takes the Bound affliction (Queen
        // PUPPET_STRINGS). Cards already carrying another affliction are skipped.
        if (int chains = powerAmount(player.powers, PowerId::CHAINS_OF_BINDING)) {
            if (boundAppliedThisTurn < chains && hand.back().affliction == 0) {
                hand.back().affliction = 4;
                hand.back().afflictionAmount = chains;
                hand.back().stampText.clear();
                ++boundAppliedThisTurn;
            }
        }
        // AutomationPower.AfterCardDrawn (Instanced): every drawn card decrements the
        // instance's counter — the natural turn draw counts too (the power's hook ignores
        // the fromHandDraw flag); at 0 the owner gains its Amount energy and the counter
        // resets to BaseCards (10).
        for (size_t pi = 0; pi < player.powers.size(); ++pi) {
            PowerInst& pw = player.powers[pi];
            if (pw.id != PowerId::AUTOMATION) continue;
            if (pw.aux < 0) pw.aux = 10;
            if (--pw.aux <= 0) {
                player.energy += pw.amount;
                pw.aux = 10;
            }
        }
        // CorrosiveWavePower.AfterCardDrawn: every card drawn poisons all hittable enemies
        if (int amt = powerAmount(player.powers, PowerId::CORROSIVE_WAVE)) {
            for (int ei = 0; ei < (int)enemies.size(); ++ei) {
                if (enemies[ei].removed || enemies[ei].curHp <= 0 || enemies[ei].reviving) continue;
                applyPowerToEnemy(ei, PowerId::POISON, amt);
            }
        }
        // stamp history: CardDrawnEntry with Card.Type == Status
        if (hand.back().def().type == CardType::STATUS) ++statusDrawsThisTurn;
        // KinglyKick.AfterCardDrawn: each draw lowers this copy's cost by 1 for the rest
        // of the combat (the opening hand's copies already carry their cost-state)
        if (hand.back().id == CardId::KINGLY_KICK)
            addCostModThisCombat(hand.back(), -1);
        // KinglyPunch.AfterCardDrawn: each draw permanently raises this copy's Damage by
        // its Increase var (the opening hand's copies already carry the game's bump)
        if (hand.back().id == CardId::KINGLY_PUNCH) {
            const CardDef& kd = hand.back().def();
            int inc = cardVarValue(hand.back(), kd, 1);
            if (inc != 0) bumpCardVar(hand.back(), 0, inc);
        }
        // Void.OnCardDrawn: drawing it costs 1 energy
        if (hand.back().id == CardId::VOID) player.energy = std::max(0, player.energy - 1);
        // HellraiserPower.AfterCardDrawnEarly: a drawn Strike auto-plays immediately
        if (powerAmount(player.powers, PowerId::HELLRAISER) > 0 && hand.back().def().hasTag(CT_STRIKE)) {
            CardInstance drawn = hand.back();
            hand.pop_back();
            if (!autoplayCard(drawn)) return;
        }
    }
}

void BattleContext::shuffleDrawFromDiscard() {
    // CardPileCmd.Shuffle: list = discard cards (draw pile is empty at call time),
    // StableShuffle = sort by (ModelId ordinal, upgrade) and write back, then UnstableShuffle
    // (Fisher-Yates: for i=n-1..1: j=NextInt(i+1); swap(list[i], list[j])), result added at pile bottom.
    drawPile = discardPile;
    discardPile.clear();
    std::stable_sort(drawPile.begin(), drawPile.end(), [](const CardInstance& a, const CardInstance& b) {
        int c = std::strcmp(CardId_NAMES[(int)a.id], CardId_NAMES[(int)b.id]);
        if (c != 0) return c < 0;
        return a.upgrade < b.upgrade;
    });
    Sts2Rng& r = rng[(int)RngStream::Shuffle];
    for (int i = (int)drawPile.size() - 1; i >= 1; --i) {
        int j = r.NextInt(i + 1);
        std::swap(drawPile[i], drawPile[j]);
    }
    // Hook.ModifyShuffleOrder (non-initial shuffle): PerfectFit moves its card to the top
    // of the shuffled pile. Listeners run in list order; each insert(0) puts the last
    // processed card first.
    std::vector<CardInstance> perfectFit;
    for (size_t i = 0; i < drawPile.size(); ++i)
        if (drawPile[i].enchant == static_cast<int>(EnchantmentId::PERFECT_FIT))
            perfectFit.push_back(drawPile[i]);
    if (!perfectFit.empty()) {
        for (const CardInstance& pf : perfectFit)
            for (size_t i = 0; i < drawPile.size(); ++i)
                if (drawPile[i].uniqueIdx == pf.uniqueIdx) {
                    drawPile.erase(drawPile.begin() + i);
                    break;
                }
        for (const CardInstance& pf : perfectFit) {
            drawPile.insert(drawPile.begin(), pf);
            ++perfectFitMoves;
        }
    }
    // Hook.AfterShuffle: StratagemPower moves Amount chosen cards from the draw pile into the
    // hand. The harness logs the chosen cards on the EndTurn action that precedes the turn
    // (turnStartChoices, timing=PlayerTurnStart, sourceId=STRATAGEM_POWER); the replay driver
    // queued that request, so consume it here.
    if (int amt = powerAmount(player.powers, PowerId::STRATAGEM)) {
        const TurnStartChoice* req = takeTurnStartChoice("STRATAGEM_POWER");
        if (req) {
            for (const auto& rc : req->cards) {
                for (auto it = drawPile.begin(); it != drawPile.end(); ++it) {
                    if (std::strcmp(CardId_NAMES[(int)it->id], rc.cardId) == 0 && it->upgrade == rc.upgrade) {
                        if ((int)hand.size() < maxHandSize) hand.push_back(*it);
                        else discardPile.push_back(*it);
                        drawPile.erase(it);
                        break;
                    }
                }
            }
        }
    }
    if (hasRelic(RelicId::THE_ABACUS)) gainBlockUnpowered(6);
}

// Turn-start choice request queue: the replay driver pushes the route-recorded requests
// before the turn-start hooks run; each hook pops the one whose sourceId is its power.
const BattleContext::TurnStartChoice* BattleContext::takeTurnStartChoice(const char* sourceId) {
    for (size_t i = 0; i < pendingTurnStartChoices.size(); ++i) {
        if (std::strcmp(pendingTurnStartChoices[i].sourceId, sourceId) == 0) {
            activeTurnStartChoice = pendingTurnStartChoices[i];
            pendingTurnStartChoices.erase(pendingTurnStartChoices.begin() + i);
            return &activeTurnStartChoice;
        }
    }
    return nullptr;
}

    // ForgeCmd.Forge: on the first forge this combat a Sovereign Blade is created in hand
// (flagged CreatedThroughForge); every existing Blade — including exhausted ones — gains
// `amount` damage (and any exhausted Blade is still a Blade: AllCards covers all piles).
void BattleContext::forge(int amount) {
    if (amount == 0) return;
    auto bumpBlade = [&](std::vector<CardInstance>& pile) {
        for (auto& c : pile)
            if (c.id == CardId::SOVEREIGN_BLADE) c.varBump[0] += amount; // Damage var
    };
    bool any = false;
    for (auto* pile : {&hand, &drawPile, &discardPile, &exhaustPile}) {
        for (auto& c : *pile)
            if (c.id == CardId::SOVEREIGN_BLADE) any = true;
    }
    if (!any) {
        CardInstance blade;
        blade.id = CardId::SOVEREIGN_BLADE;
        blade.uniqueIdx = ++nextUniqueIdx;
        blade.genCard = true;
        ++cardsGeneratedByOwner;
        if ((int)hand.size() < maxHandSize) hand.push_back(blade);
        else discardPile.push_back(blade);
    }
    bumpBlade(hand);
    bumpBlade(drawPile);
    bumpBlade(discardPile);
    bumpBlade(exhaustPile);
}

// CardFactory.GetForCombat: N cards drawn with CombatCardGeneration.NextItem from the
// character's combat pool (basic/ancient/event rarities and noCombatGen cards excluded).
void BattleContext::generateCardsFromPool(int count) {
    int n = 0;
    const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &n);
    if (!pool || n <= 0) return;
    Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
    for (int i = 0; i < count && !battleOver; ++i) {
        if ((int)hand.size() >= maxHandSize) break;
        CardId pick = pool[r.NextInt(n)];
        CardInstance c;
        c.id = pick;
        c.uniqueIdx = ++nextUniqueIdx;
        c.genCard = true;
        // TangledPower/RingingPower.AfterCardEnteredCombat: generated cards join afflicted.
        if (int tg = powerAmount(player.powers, PowerId::TANGLED)) {
            if (c.def().type == CardType::ATTACK) { c.affliction = 1; c.afflictionAmount = tg; }
        }
        if (int rg = powerAmount(player.powers, PowerId::RINGING)) {
            if (c.affliction == 0) { c.affliction = 2; c.afflictionAmount = rg; }
        }
        ++cardsGeneratedByOwner;
        hand.push_back(c);
    }
}

// ---------------- Osty (Necrobinder pet) ----------------

// OstyCmd.Summon: with a live Osty the amount raises MaxHp AND heals that much
// (CreatureCmd.GainMaxHp); with a dead/absent one it summons (or revives) at that HP.
void BattleContext::summonOsty(int amount) {
    if (amount <= 0) return;
    OstyState& o = player.osty;
    if (!o.present) {
        o.present = true;
        o.curHp = amount;
        o.maxHp = amount;
        o.combatId = ++nextCombatId;
        addPower(o.powers, PowerId::DIE_FOR_YOU, 1, false);
        return;
    }
    if (o.curHp > 0) {
        o.maxHp += amount;             // GainMaxHp: SetMaxHp then heal by the gain
        o.curHp += amount;
    } else {
        o.maxHp = amount;              // reviving: SetMaxHp(amount) then heal
        o.curHp = amount;
    }
}

// CreatureCmd.Kill: hp to 0; DieForYouPower keeps the pet in combat so it can revive later
void BattleContext::killOsty() {
    if (!player.osty.present) return;
    player.osty.curHp = 0;
    player.osty.block = 0;
}

// OstyAttack cards: the pet is the attacker, so Osty's own Strength/Weak apply and the
// player's do not (AttackCommand.FromOsty sets Attacker = osty).
bool BattleContext::ostyAttackFrom(int base, int targetIdx, bool allEnemies) {
    const OstyState& o = player.osty;
    if (!o.present || o.curHp <= 0) return false; // Osty.CheckMissingWithAnim guard
    Rational r;
    r.num = base + powerAmount(o.powers, PowerId::STRENGTH);
    if (powerAmount(o.powers, PowerId::WEAK) > 0) r.mul(3, 4);
    int dmg = (int)r.trunc();
    auto strike = [&](int idx) {
        const Monster& t = enemies[idx];
        if (t.removed || t.curHp <= 0 || t.reviving) return;
        int d = dmg;
        if (powerAmount(t.powers, PowerId::VULNERABLE) > 0) {
            Rational v; v.num = d; v.mul(3, 2); d = (int)v.trunc();
        }
        damageEnemy(idx, d, /*powered=*/true);
    };
    if (allEnemies) {
        for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) strike(i);
    } else if (targetIdx >= 0 && targetIdx < (int)enemies.size()) {
        strike(targetIdx);
    }
    return true;
}

// ---------------- card play ----------------

// execute a card's effect program. Returns false when a step is unimplemented
// (the caller aborts the play and reports; replay harness flags the run).
// TinkerTime's MadScience takes its card type from the instance configuration.
static CardType effectiveCardType(const CardInstance& c) {
    if (c.id == CardId::MAD_SCIENCE) return static_cast<CardType>(c.tinkerType);
    return c.def().type;
}

bool BattleContext::executeCardSteps(CardInstance& c, int targetEnemyIdx) {
    const CardDef& d = c.def();

    // FranticEscape.OnPlay: the Sandpit countdown moves one step away and this copy's own
    // cost grows by 1 for the rest of the combat (EnergyCost.AddThisCombat(1)).
    if (d.id == CardId::FRANTIC_ESCAPE) {
        // the Sandpit power lives on the enemy (Target = this player): ModifyAmount +1
        bool bumped = false;
        for (auto& em : enemies) {
            if (em.removed) continue;
            for (auto& p : em.powers)
                if (p.id == PowerId::SANDPIT) { p.amount += 1; bumped = true; break; }
            if (bumped) break;
        }
        addCostModThisCombat(c, 1);
        return true;
    }

    const int savedDmgBonus = currentCardDamageBonus;
    const int savedBlkBonus = currentCardBlockBonus;
    const bool savedVigorous = currentDamageBonusVigorous;
    const bool savedCorrupted = currentDamageCorrupted;
    currentCardDamageBonus =
        c.enchant == static_cast<int>(EnchantmentId::SHARP) ? c.enchantAmount
        : ((c.enchant == static_cast<int>(EnchantmentId::VIGOROUS) && !c.enchantDisabled)
               ? c.enchantAmount : 0);
    currentDamageBonusVigorous =
        c.enchant == static_cast<int>(EnchantmentId::VIGOROUS) && !c.enchantDisabled;
    currentDamageCorrupted = c.enchant == static_cast<int>(EnchantmentId::CORRUPTED);
    currentCardBlockBonus =
        c.enchant == static_cast<int>(EnchantmentId::NIMBLE) ? c.enchantAmount : 0;

    for (int si = 0; si < (int)d.nSteps; ++si) {
        const CardStep& st = d.steps[si];
        if ((st.cond & STEP_IF_UPGRADED) && c.upgrade <= 0) continue;
        int amt = st.varIdx >= 0 && st.varIdx < (int)d.nVars ? cardVarValue(c, d, st.varIdx)
                                                             : st.constVal;
        int hits = st.hits > 0 ? st.hits
                               : cardVarValue(c, d, -(st.hits) - 1);
        // Calculated-var power applies (NoEscape): base + extra * floor(doom / threshold).
        // The threshold is vars[0] (DoomThreshold); the multiplier counts the target's
        // Doom stacks before this application.
        if (st.calc == CalcKind::DOOM_TIERS) {
            int ti = targetEnemyIdx;
            int doom = (ti >= 0 && ti < (int)enemies.size())
                           ? powerAmount(enemies[ti].powers, PowerId::DOOM) : 0;
            int threshold = cardVarValue(c, d, 0);
            int mult = threshold > 0 ? doom / threshold : 0;
            amt = cardVarValue(c, d, st.varIdx)
                + (st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0) * mult;
        }
        switch (st.op) {
            case StepOp::ATTACK: {
                // Shiv under FanOfKnivesPower retargets to ALL enemies (the power is a
                // marker: Shiv.TargetType becomes AllEnemies while it is held)
                if (lastPlayedWasShiv && powerAmount(player.powers, PowerId::FAN_OF_KNIVES) > 0) {
                    for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                        const Monster& t = enemies[i];
                        if (t.removed || t.curHp <= 0 || t.reviving) continue;
                        int dmg = attackDamageFromBase(amt, i);
                        for (int h = 0; h < hits && !battleOver; ++h) {
                            const Monster& t2 = enemies[i];
                            if (t2.removed || t2.curHp <= 0 || t2.reviving) break;
                            damageEnemy(i, dmg);
                        }
                    }
                    break;
                }
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                int dmg = attackDamageFromBase(amt, ti);
                for (int h = 0; h < hits && !battleOver; ++h) {
                    const Monster& t = enemies[ti];
                    if (t.removed || t.curHp <= 0 || t.reviving) break;
                    damageEnemy(ti, dmg);
                }
                break;
            }
            case StepOp::ATTACK_ALL: {
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    const Monster& t = enemies[i];
                    if (t.removed || t.curHp <= 0 || t.reviving) continue;
                    int dmg = attackDamageFromBase(amt, i);
                    for (int h = 0; h < hits && !battleOver; ++h) {
                        const Monster& t2 = enemies[i];
                        if (t2.removed || t2.curHp <= 0 || t2.reviving) break;
                        damageEnemy(i, dmg);
                    }
                }
                break;
            }
            case StepOp::ATTACK_ALL_X: {
                int xVal = xValue >= 0 ? xValue : player.energy; // ResolveEnergyXValue
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    const Monster& t = enemies[i];
                    if (t.removed || t.curHp <= 0 || t.reviving) continue;
                    int dmg = attackDamageFromBase(amt, i);
                    for (int h = 0; h < xVal && !battleOver; ++h) {
                        const Monster& t2 = enemies[i];
                        if (t2.removed || t2.curHp <= 0 || t2.reviving) break;
                        damageEnemy(i, dmg);
                    }
                }
                break;
            }
            case StepOp::ATTACK_CALC: {
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                // Protecter/Squeeze attack FromOsty: with no living pet the game skips the
                // whole attack (Osty.CheckMissingWithAnim guard), not just the multiplier.
                if ((st.calc == CalcKind::OSTY_MAX_HP || st.calc == CalcKind::OSTY_ATTACKS)
                    && (!player.osty.present || player.osty.curHp <= 0)) break;
                int mult = 0;
                switch (st.calc) {
                    case CalcKind::BLOCK: mult = player.block; break;
                    case CalcKind::STRIKES:
                        // AllCards includes the in-flight card itself (Play pile)
                        mult = countStrikeTagInPiles() + (c.def().hasTag(CT_STRIKE) ? 1 : 0);
                        break;
                    case CalcKind::EXHAUST_COUNT: mult = (int)exhaustPile.size(); break;
                    case CalcKind::TARGET_VULNERABLE:
                        mult = powerAmount(enemies[ti].powers, PowerId::VULNERABLE); break;
                    case CalcKind::DRAW_COUNT: mult = (int)drawPile.size(); break;
                    case CalcKind::PLAYS_FINISHED: mult = totalPlaysFinished; break;
                    case CalcKind::TARGET_DEBUFFS: {
                        // Rend: debuff power instances on the target (temporary powers excluded)
                        const Monster& t = enemies[ti];
                        if (powerAmount(t.powers, PowerId::VULNERABLE) != 0) ++mult;
                        if (powerAmount(t.powers, PowerId::WEAK) != 0) ++mult;
                        if (powerAmount(t.powers, PowerId::FRAIL) != 0) ++mult;
                        break;
                    }
                    case CalcKind::TARGET_HITS_THIS_TURN:
                        mult = enemies[ti].hitsTakenThisTurn; break;
                    case CalcKind::DISCARDED_THIS_TURN: mult = cardsDiscardedThisTurn; break;
                    case CalcKind::TOTAL_CARDS_DRAWN: mult = totalCardsDrawn; break;
                    case CalcKind::STAR_COST_CARDS: {
                        // CrescentSpear: AllCards spans the four piles (the in-flight card is
                        // in the Play pile and has no star cost, so it never self-counts).
                        auto count = [](const std::vector<CardInstance>& p) {
                            int n = 0;
                            for (const auto& ci : p) {
                                const CardDef& cd = ci.def();
                                if (cd.starCost >= 0 || cd.hasFlag(CF_STAR_X)) ++n;
                            }
                            return n;
                        };
                        mult = count(hand) + count(drawPile) + count(discardPile) + count(exhaustPile);
                        // AllCards includes the Play pile: the in-flight card counts itself
                        // when it carries a star cost (CRESCENT_SPEAR has starCost 1).
                        if (d.starCost >= 0 || d.hasFlag(CF_STAR_X)) ++mult;
                        break;
                    }
                    case CalcKind::DRAWN_THIS_TURN_EXTRA: mult = effectDrawsThisTurn; break;
                    case CalcKind::OSTY_MAX_HP:
                        mult = (player.osty.present && player.osty.curHp > 0) ? player.osty.maxHp : 0;
                        break;
                    case CalcKind::EXHAUST_SOULS: {
                        int n = 0;
                        for (const auto& ci : exhaustPile)
                            if (ci.id == CardId::SOUL) ++n;
                        mult = n;
                        break;
                    }
                    case CalcKind::OSTY_ATTACKS: {
                        // Squeeze: OstyAttack-tagged cards in AllCards except the played card.
                        auto count = [](const std::vector<CardInstance>& p) {
                            int n = 0;
                            for (const auto& ci : p)
                                if (ci.def().hasTag(CT_OSTY_ATTACK)) ++n;
                            return n;
                        };
                        mult = count(hand) + count(drawPile) + count(discardPile) + count(exhaustPile);
                        break;
                    }
                    case CalcKind::TARGET_DOOM:
                        mult = powerAmount(enemies[ti].powers, PowerId::DOOM); break;
                    case CalcKind::GENERATED_THIS_COMBAT: mult = cardsGeneratedByOwner; break;
                    case CalcKind::HAND_NEG: mult = -(int)hand.size(); break; // PreciseCut (in-flight card not in hand)
                    default: mult = 0; break;
                }
                int extra = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0;
                int dmg = attackDamageFromBase(amt + extra * mult, ti);
                if (!battleOver && enemies[ti].curHp > 0 && !enemies[ti].reviving)
                    damageEnemy(ti, dmg);
                break;
            }
            case StepOp::GAIN_BLOCK:
                if (amt > 0) gainBlock(amt);
                break;
            case StepOp::APPLY_POWER_SELF:
                if (amt != 0) {
                    if (st.sign < 0) amt = -amt;
                    applyPowerToPlayer(st.power, amt);
                    // Inferno/CrimsonMantle: each play bumps the power's SelfDamage dynvar
                    if (st.power == PowerId::INFERNO || st.power == PowerId::CRIMSON_MANTLE) {
                        for (auto& p : player.powers)
                            if (p.id == st.power) { p.misc = (p.misc < 0 ? 0 : p.misc) + 1; break; }
                    }
                    // TheBomb.OnPlay: SetDamage(BombDamage dynvar) — the power row carries
                    // it as its Damage display value (Amount stays the turn countdown)
                    if (st.power == PowerId::THE_BOMB) {
                        for (auto it = player.powers.rbegin(); it != player.powers.rend(); ++it)
                            if (it->id == PowerId::THE_BOMB) {
                                it->aux = cardVarValue(c, d, 1);
                                break;
                            }
                    }
                }
                break;
            case StepOp::APPLY_POWER_ENEMY: {
                int ti = targetEnemyIdx;
                if (ti >= 0 && ti < (int)enemies.size() && !enemies[ti].removed
                    && enemies[ti].curHp > 0 && amt != 0) {
                    if (st.sign < 0) amt = -amt;
                    applyPowerToEnemy(ti, st.power, amt);
                }
                break;
            }
            case StepOp::APPLY_POWER_ALL_ENEMIES:
                for (int i = 0; i < (int)enemies.size(); ++i)
                    if (!enemies[i].removed && enemies[i].curHp > 0 && amt != 0)
                        applyPowerToEnemy(i, st.power, st.sign < 0 ? -amt : amt);
                break;
            case StepOp::DRAW:
                // CompileDriver draws by a calc multiplier (distinct orb kinds) rather
                // than a var; the step carries calc and its var otherwise
                if (st.calc == CalcKind::ORB_KINDS) { if (distinctOrbKinds() > 0) drawCards(distinctOrbKinds()); }
                else if (amt > 0) drawCards(amt);
                break;
            case StepOp::DRAW_AND_RETAIN: {
                // Expertise: draw N, then each card drawn by THIS effect gets single-turn
                // Retain (CardCmd.ApplySingleTurnRetain). Cards already in hand are untouched.
                const size_t before = hand.size();
                if (amt > 0) drawCards(amt);
                for (size_t i = before; i < hand.size(); ++i) hand[i].retainThisTurn = true;
                break;
            }
            case StepOp::GAIN_ENERGY:
                player.energy += amt;
                break;
            case StepOp::GAIN_STARS:
                player.stars += amt;
                break;
            case StepOp::SELF_DAMAGE:
                if (amt > 0) {
                    if (st.constVal & 1) damagePlayerUnblockable(amt);
                    else damagePlayer(amt);
                }
                break;
            case StepOp::HEAL_SELF:
                player.curHp = std::min(player.maxHp, player.curHp + amt);
                break;
            case StepOp::COPY_SELF_TO_DISCARD:
                // AddGeneratedCardToCombat clone: combat-generated copy (deck flag False);
                // lands before the played original reaches the discard at play end
                {
                    CardInstance copy = c;
                    copy.genCard = true;
                    ++cardsGeneratedByOwner; // CardGeneratedEntry (Anger clone)
                    copy.stampText.clear();
                    copy.uniqueIdx = ++nextUniqueIdx;
                    discardPile.push_back(copy);
                }
                break;
            case StepOp::AUTOPLAY_DRAW_TOP_EXHAUST: {
                // Havoc-family: AutoPlayFromDrawPile(1, Top, forceExhaust) — the top draw
                // card plays for free via the random-target stream and exhausts
                if (drawPile.empty()) break;
                CardInstance top = drawPile.front();
                drawPile.erase(drawPile.begin());
                if (!autoplayCard(top, /*forceExhaust=*/true)) return false;
                break;
            }
            case StepOp::ARMAMENTS_UPGRADE:
                if (c.upgrade) {
                    for (auto& hc : hand)
                        if (&hc != &c) upgradeCardInstance(hc);
                }
                // base (unupgraded) Armaments needs an interactive card choice: unsupported
                // until route-choice replay lands (harness reports it)
                else { std::fprintf(stderr, "[card] ARMAMENTS base needs choice\n"); return false; }
                break;
            case StepOp::AUTOPLAY_DRAW_TOP: {
                // Cascade: X (+1 when upgraded) cards from the top of the draw pile
                // auto-play for free (no forced exhaust). CardPileCmd.AutoPlayFromDrawPile
                // pulls every card first (shuffling the discard in when the draw pile runs
                // dry) and then auto-plays them in order.
                if (xValue < 0) break;
                std::vector<CardInstance> picked;
                for (int i = 0; i < xValue + c.upgrade && !battleOver; ++i) {
                    if (drawPile.empty()) {
                        if (discardPile.empty()) break;
                        shuffleDrawFromDiscard();
                    }
                    if (drawPile.empty()) break;
                    picked.push_back(drawPile.front());
                    drawPile.erase(drawPile.begin());
                }
                for (auto& pc : picked) {
                    if (battleOver) break;
                    if (!autoplayCard(pc)) return false;
                }
                break;
            }
            case StepOp::ATTACK_HITS_CALC: {
                // hits = base step.hits, overridden by the calc condition
                // (Dismantle: target vulnerable; Spite: lost hp this turn)
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                int n = st.hits > 0 ? st.hits : 1;
                bool cond = false;
                if (st.calc == CalcKind::HITS_VULN_TOGGLE)
                    cond = powerAmount(enemies[ti].powers, PowerId::VULNERABLE) > 0;
                else if (st.calc == CalcKind::HITS_LOST_HP)
                    cond = hpLostThisTurn > 0;
                else if (st.calc == CalcKind::ATTACKS_FINISHED)
                    n = attacksFinishedThisTurn; // Finisher: hits = attacks finished this turn
                else if (st.calc == CalcKind::ORB_COUNT)
                    n = (int)player.orbs.size(); // Barrage: hits = orbs in the queue
                else if (st.calc == CalcKind::HAND_SKILLS) {
                    // Flechettes: hits = skills in hand (the played card already left the hand)
                    n = 0;
                    for (const auto& hc : hand)
                        if (hc.def().type == CardType::SKILL) ++n;
                }
                else if (st.calc == CalcKind::SKILLS_PLAYED_THIS_TURN)
                    n = skillPlaysFinishedThisTurn;      // LunarBlast
                else if (st.calc == CalcKind::HP_LOSS_EVENTS_COMBAT)
                    n = 1 + hpLossEventsCombat;          // TearAsunder
                else if (st.calc == CalcKind::ENERGY_SPENT_THIS_TURN)
                    n = energySpentThisTurn;             // HelixDrill (own cost already excluded)
                else if (st.calc == CalcKind::X_VALUE)
                    n = xValue >= 0 ? xValue : player.energy; // Eradicate: hits = X
                if (cond)
                    n = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : st.constVal;
                int dmg = attackDamageFromBase(amt, ti);
                for (int h = 0; h < n && !battleOver; ++h) {
                    const Monster& t = enemies[ti];
                    if (t.removed || t.curHp <= 0 || t.reviving) break;
                    damageEnemy(ti, dmg);
                }
                break;
            }
            case StepOp::ATTACK_RANDOM_HITS: {
                // SwordBoomerang/Volley: hits, each on a random hittable enemy
                // (TargetingRandomOpponents rolls CombatTargets per hit; X-cost cards
                // use the captured energy as the hit count)
                int n;
                if (d.hasFlag(CF_ENERGY_X) && xValue >= 0) n = xValue;
                else n = st.hits > 0 ? st.hits : cardVarValue(c, d, -(st.hits) - 1);
                for (int h = 0; h < n && !battleOver; ++h) {
                    std::vector<int> hittable;
                    for (int i = 0; i < (int)enemies.size(); ++i)
                        if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
                    if (hittable.empty()) break;
                    Sts2Rng& r = rng[(int)RngStream::CombatTargets];
                    int t = hittable[r.NextInt((int)hittable.size())];
                    damageEnemy(t, attackDamageFromBase(amt, t));
                }
                break;
            }
            case StepOp::ATTACK_IF_EXHAUST_GE: {
                // PactsEnd: the AoE only fires when the exhaust pile holds >= threshold
                int need = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : st.constVal;
                if ((int)exhaustPile.size() < need) break;
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    const Monster& t = enemies[i];
                    if (t.removed || t.curHp <= 0 || t.reviving) continue;
                    damageEnemy(i, attackDamageFromBase(amt, i));
                }
                break;
            }
            case StepOp::ATTACK_FEED: {
                // Feed: kill with this attack -> MaxHp (and hp) += vars[extraVarIdx];
                // HandOfGreed (constVal==1): kill -> gold += vars[extraVarIdx]
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                int gain = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : st.constVal;
                int hpBefore = enemies[ti].curHp;
                int dmg = attackDamageFromBase(amt, ti);
                if (!battleOver && enemies[ti].curHp > 0 && !enemies[ti].reviving)
                    damageEnemy(ti, dmg);
                if (enemies[ti].curHp <= 0 && hpBefore > 0 && gain > 0) {
                    if (st.constVal == 1) player.gold += gain;      // HandOfGreed
                    else {
                        player.maxHp += gain;                        // Feed
                        player.curHp = std::min(player.maxHp, player.curHp + gain);
                    }
                }
                break;
            }
            case StepOp::GAIN_BLOCK_CALC: {
                // block = vars[varIdx] + vars[extraVarIdx] * multiplier (ExpectAFight /
                // DemonicShield); normal (Move prop) block: dexterity/frail apply
                int mult = 0;
                if (st.calc == CalcKind::STRENGTH_SCALE)
                    mult = std::max(0, powerAmount(player.powers, PowerId::STRENGTH));
                else if (st.calc == CalcKind::CURRENT_BLOCK)
                    mult = player.block;
                else if (st.calc == CalcKind::DISCARD_COUNT)
                    mult = static_cast<int>(discardPile.size());
                else if (st.calc == CalcKind::TARGET_BLOCK && targetEnemyIdx >= 0
                         && targetEnemyIdx < (int)enemies.size())
                    mult = enemies[targetEnemyIdx].block; // Mimic: target's block
                else if (st.calc == CalcKind::ENEMY_POISON_TOTAL) {
                    for (const auto& m : enemies)
                        if (!m.removed && m.curHp > 0) mult += powerAmount(m.powers, PowerId::POISON);
                }
                int extra = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0;
                gainBlock(amt + extra * mult);
                break;
            }
            case StepOp::GAIN_BLOCK_EXHAUST_DOUBLE: {
                // EvilEye: block once, twice when a card was exhausted this turn
                int times = cardExhaustsThisTurn > 0 ? 2 : 1;
                for (int i = 0; i < times && !battleOver; ++i) gainBlock(amt);
                break;
            }
            case StepOp::VULN_DOUBLE: {
                // MoltenFist: living target with vuln doubles its stacks
                {
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size() && amt > 0) {
                        damageEnemy(ti, attackDamageFromBase(amt, ti));
                        int ti2 = targetEnemyIdx; // re-read after possible death
                        if (ti2 >= 0 && ti2 < (int)enemies.size()) {
                            const Monster& t = enemies[ti2];
                            if (!t.removed && t.curHp > 0 && !t.reviving)
                                if (int v = powerAmount(t.powers, PowerId::VULNERABLE))
                                    applyPowerToEnemy(ti2, PowerId::VULNERABLE, v);
                        }
                    }
                }
                break;
            }
            case StepOp::DOMINATE: {
                // Dominate: vuln N to the target, then strength += its total vuln
                {
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size()) {
                        applyPowerToEnemy(ti, PowerId::VULNERABLE, amt);
                        const Monster& t = enemies[ti];
                        if (!t.removed && t.curHp > 0 && !t.reviving)
                            if (int v = powerAmount(t.powers, PowerId::VULNERABLE))
                                applyPowerToPlayer(PowerId::STRENGTH, v);
                    }
                }
                break;
            }
            case StepOp::EXHAUST_RANDOM_HAND: {
                // Cinder / base TrueGrit: CombatCardSelection.NextItem over the hand
                // (a single candidate still consumes the draw)
                if (!hand.empty()) {
                    Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                    int pick = r.NextInt((int)hand.size());
                    exhaustCard(hand[pick]);
                    hand.erase(hand.begin() + pick);
                }
                break;
            }
            case StepOp::EXHAUST_HAND_ATTACK_HITS: {
                // FiendFire: exhaust the whole hand, then hit once per exhausted card
                {
                    int count = (int)hand.size();
                    for (auto& hc : hand) exhaustCard(hc);
                    hand.clear();
                    int ti = targetEnemyIdx;
                    if (ti < 0 || ti >= (int)enemies.size()) break;
                    int dmg = attackDamageFromBase(amt, ti);
                    for (int h = 0; h < count && !battleOver; ++h) {
                        const Monster& t = enemies[ti];
                        if (t.removed || t.curHp <= 0 || t.reviving) break;
                        damageEnemy(ti, dmg);
                    }
                }
                break;
            }
            case StepOp::SECOND_WIND: {
                // exhaust each non-attack hand card, gaining block per card
                std::vector<CardInstance> rest;
                for (auto& hc : hand) {
                    if (hc.def().type != CardType::ATTACK) {
                        exhaustCard(hc);
                        gainBlock(amt);
                        if (battleOver) { hand = rest; break; }
                    } else rest.push_back(hc);
                }
                if (!battleOver) hand = rest;
                break;
            }
            case StepOp::DRAW_UNTIL_NON_ATTACK: {
                // Pillage: keep drawing while the drawn card is an Attack (stops at a
                // non-attack, an empty pile, or a full hand)
                for (;;) {
                    if ((int)hand.size() >= maxHandSize) break;
                    if (drawPile.empty()) {
                        if (discardPile.empty()) break;
                        shuffleDrawFromDiscard();
                    }
                    CardInstance drawn = drawPile.front();
                    drawPile.erase(drawPile.begin());
                    ++totalCardsDrawn;
                    if (drawn.def().type == CardType::STATUS) ++statusDrawsThisTurn;
                    hand.push_back(drawn);
                    if (drawn.def().type != CardType::ATTACK) break;
                }
                break;
            }
            case StepOp::TRANSFORM_ATTACKS_ROCK: {
                // PrimalForce: each hand Attack becomes a GiantRock (upgraded with the card)
                for (auto& hc : hand) {
                    if (hc.def().type != CardType::ATTACK) continue;
                    CardInstance rock;
                    rock.id = CardId::GIANT_ROCK;
                    // if (base.IsUpgraded) CardCmd.Upgrade(rock): the PLAYED card's
                    // upgrade level decides, not the transformed card's
                    rock.upgrade = c.upgrade > 0 ? 1 : 0;
                    rock.uniqueIdx = ++nextUniqueIdx;
                    rock.genCard = true;
                    ++cardsGeneratedByOwner; // Transform logs CardGeneratedEntry
                    hc = rock;
                }
                break;
            }
            case StepOp::RAMPAGE_BUMP: {
                // Rampage: Damage var grows by Increase for the rest of the combat
                // (the played instance carries the bump through the discard)
                bumpCardVar(const_cast<CardInstance&>(c), st.varIdx,
                            st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : st.constVal);
                break;
            }
            case StepOp::THRASH_ABSORB: {
                // Thrash: 2 hits; a random hand Attack's damage joins this card's var,
                // then that card exhausts
                {
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size()) {
                        int dmg = attackDamageFromBase(amt, ti);
                        for (int h = 0; h < 2 && !battleOver; ++h) {
                            const Monster& t = enemies[ti];
                            if (t.removed || t.curHp <= 0 || t.reviving) break;
                            damageEnemy(ti, dmg);
                        }
                    }
                    std::vector<int> atkIdx;
                    for (int i = 0; i < (int)hand.size(); ++i)
                        if (hand[i].def().type == CardType::ATTACK) atkIdx.push_back(i);
                    if (!atkIdx.empty()) {
                        Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                        int pick = atkIdx[r.NextInt((int)atkIdx.size())];
                        const CardDef& pd = hand[pick].def();
                        int baseDmg = 0;
                        for (int s2 = 0; s2 < (int)pd.nVars; ++s2) {
                            const std::string key = pd.vars[s2].key;
                            if (key == "Damage" || key == "CalculatedDamage" || key == "OstyDamage") {
                                baseDmg = cardVarValue(hand[pick], pd, s2);
                                break;
                            }
                        }
                        // Hook.ModifyDamage with a null target: strength/weak only
                        int mod = baseDmg + powerAmount(player.powers, PowerId::STRENGTH);
                        Rational rr; rr.num = mod;
                        if (powerAmount(player.powers, PowerId::WEAK) > 0) rr.mul(3, 4);
                        bumpCardVar(const_cast<CardInstance&>(c), 0, (int)rr.trunc());
                        exhaustCard(hand[pick]);
                        hand.erase(hand.begin() + pick);
                    }
                }
                break;
            }
            case StepOp::ATTACK_X_HITS: {
                // Volley: single-target attack hitting X (energy spent) times
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size() || xValue < 0) break;
                int dmg = attackDamageFromBase(amt, ti);
                for (int h = 0; h < xValue && !battleOver; ++h) {
                    const Monster& t = enemies[ti];
                    if (t.removed || t.curHp <= 0 || t.reviving) break;
                    damageEnemy(ti, dmg);
                }
                break;
            }
            case StepOp::DRAW_TO_MAX: {
                // Scrawl: draw up to the hand limit
                drawCards(maxHandSize - (int)hand.size(), false);
                break;
            }
            case StepOp::PROLONG_BLOCK: {
                // Prolong: BlockNextTurnPower stores the current block; when block
                // clears at next turn start it returns (unpowered) and the power leaves
                if (player.block > 0)
                    addPower(player.powers, PowerId::BLOCK_NEXT_TURN, player.block, false);
                break;
            }
            case StepOp::DRAW_IF_NO_ATTACK: {
                // Impatience: draw N only when the hand holds no attack
                bool hasAttack = false;
                for (auto& hc : hand)
                    if (hc.def().type == CardType::ATTACK) { hasAttack = true; break; }
                if (!hasAttack) drawCards(amt, false);
                break;
            }
            case StepOp::ATTACK_BLOCK_AS_DAMAGE: {
                // Fisticuffs: attack, then gain block equal to the damage dealt
                // (TotalDamage + OverkillDamage)
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                int hpBefore = enemies[ti].curHp;
                int blockBefore = enemies[ti].block;
                int dmg = attackDamageFromBase(amt, ti);
                if (!battleOver && enemies[ti].curHp > 0 && !enemies[ti].reviving)
                    damageEnemy(ti, dmg);
                int dealt = (blockBefore - enemies[ti].block)
                          + (hpBefore - enemies[ti].curHp);
                if (dealt > 0) gainBlock(dealt);
                break;
            }
            case StepOp::GENERATE_FROM_POOL: {
                // CardFactory.GetDistinctForCombat over the character combat pool filtered to
                // Attack cards (Infernal Blade). TakeRandom = UnstableShuffle then Take, so the
                // whole candidate list is shuffled on the CombatCardGeneration stream — the
                // Fisher-Yates draws must match the game or every later shuffle diverges.
                int n = amt > 0 ? amt : 1;
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i)
                    if (BattleContext::cardDef(pool[i]).type == CardType::ATTACK) opts.push_back(pool[i]);
                if (opts.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    CardId tmp = opts[i];
                    opts[i] = opts[j];
                    opts[j] = tmp;
                }
                for (int i = 0; i < n && i < (int)opts.size() && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = opts[i];
                    nc.costOverride = 0; // SetToFreeThisTurn
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner; // AddGeneratedCardsToCombat
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::STOKE: {
                // Stoke: exhaust the whole hand, then generate that many combat-pool cards into
                // the hand (CardFactory.GetForCombat: one NextItem per card, no shuffle) and
                // upgrade them when this card is upgraded.
                int n = (int)hand.size();
                for (auto& hc : hand) exhaustCard(hc);
                hand.clear();
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                if (!pool || pn <= 0) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = pool[r.NextInt(pn)];
                    nc.upgrade = c.upgrade > 0 ? 1 : 0;
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner;
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::CREATE_SHIVS: {
                // Silent shiv family: Shiv tokens (0-cost 4-dmg Exhaust attack) into hand.
                // Count: the Cards/Shivs var when present, else the constVal count.
                // constVal bit 0 set (and only when >= 0): the tokens arrive upgraded.
                int n = amt > 0 ? amt : (st.constVal > 0 ? (st.constVal & ~1) : 0);
                bool up = st.constVal > 0 && (st.constVal & 1);
                if ((st.cond & STEP_UPGRADE_SHIVS) && c.upgrade > 0) up = true;
                for (int i = 0; i < n && !battleOver; ++i) {
                    if ((int)hand.size() >= maxHandSize) break;
                    CardInstance shiv;
                    shiv.id = CardId::SHIV;
                    shiv.upgrade = up ? 1 : 0;
                    shiv.uniqueIdx = ++nextUniqueIdx;
                    shiv.genCard = true;
                    ++cardsGeneratedByOwner;
                    hand.push_back(shiv);
                }
                break;
            }
            case StepOp::ADD_CARD_TO_PILE: {
                // AddGeneratedCardToCombat(CreateCard<T>, <pile>, owner): a combat-generated
                // copy of the named card joins the requested pile (constVal: 0 Hand /
                // 1 Discard / 2 Draw). Default position is Bottom, i.e. append.
                if (st.cardId == CardId::INVALID) break;
                int n = st.hits > 0 ? st.hits : 1;
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = st.cardId;
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    if (st.constVal == 1) discardPile.push_back(nc);
                    else if (st.constVal == 2) drawPile.push_back(nc);
                    else if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::ADD_CARD_TO_HAND: {
                // CardPileCmd.AddGeneratedCardToCombat(CombatState.CreateCard<T>(owner), Hand):
                // the named card joins the hand as a combat-generated copy. A full hand routes
                // it to the discard pile (CardPileCmd.Add semantics).
                if (st.cardId == CardId::INVALID) break;
                int n = amt > 0 ? amt : (st.constVal > 0 ? st.constVal : 1);
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = st.cardId;
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::POISON_RANDOM_HITS: {
                // BouncingFlask: Repeat iterations, each picking a random hittable enemy
                // (CombatTargets.NextItem) and stacking the card's Poison amount.
                int rep = st.hits > 0 ? st.hits : (st.hits < 0 ? cardVarValue(c, d, -(st.hits) - 1) : 1);
                for (int i = 0; i < rep && !battleOver; ++i) {
                    std::vector<int> hittable;
                    for (int j = 0; j < (int)enemies.size(); ++j)
                        if (!enemies[j].removed && enemies[j].curHp > 0 && !enemies[j].reviving) hittable.push_back(j);
                    if (hittable.empty()) break;
                    Sts2Rng& r = rng[(int)RngStream::CombatTargets];
                    int t = hittable[r.NextInt((int)hittable.size())];
                    applyPowerToEnemy(t, PowerId::POISON, amt > 0 ? amt : 1);
                }
                break;
            }
            case StepOp::OUTBREAK: {
                // poison every enemy, then trigger each stack once (damage + decrement)
                for (int i = 0; i < (int)enemies.size(); ++i) {
                    if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                        applyPowerToEnemy(i, PowerId::POISON, amt);
                }
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    Monster& m = enemies[i];
                    if (m.removed || m.curHp <= 0 || m.reviving) continue;
                    if (int pois = powerAmount(m.powers, PowerId::POISON)) {
                        m.curHp -= pois;
                        if (m.curHp <= 0) {
                            m.curHp = 0;
                            onEnemyDeath(i);
                        } else {
                            for (auto it = m.powers.begin(); it != m.powers.end();) {
                                if (it->id == PowerId::POISON) {
                                    if (--it->amount <= 0) it = m.powers.erase(it);
                                    else ++it;
                                } else ++it;
                            }
                        }
                    }
                }
                break;
            }
            case StepOp::POISON_IF_POISONED: {
                int ti = targetEnemyIdx;
                if (ti >= 0 && ti < (int)enemies.size()) {
                    const Monster& t = enemies[ti];
                    if (!t.removed && t.curHp > 0 && !t.reviving
                        && powerAmount(t.powers, PowerId::POISON) > 0)
                        applyPowerToEnemy(ti, PowerId::POISON, amt);
                }
                break;
            }
            case StepOp::MALAISE_X: {
                // X-cost: -X(+1 upgraded) strength and the same amount of weak
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size() || xValue < 0) break;
                int n = xValue + c.upgrade;
                applyPowerToEnemy(ti, PowerId::STRENGTH, -n);
                applyPowerToEnemy(ti, PowerId::WEAK, n);
                break;
            }
            case StepOp::EXPOSE: {
                // strip the target's block and its ENTIRE artifact power, then vulnerable N
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                Monster& t = enemies[ti];
                if (t.removed || t.curHp <= 0 || t.reviving) break;
                t.block = 0;
                t.powers.erase(std::remove_if(t.powers.begin(), t.powers.end(),
                    [](const PowerInst& p) { return p.id == PowerId::ARTIFACT; }), t.powers.end());
                applyPowerToEnemy(ti, PowerId::VULNERABLE, amt);
                break;
            }
            case StepOp::DRAW_CHECK_SKILL_BLOCK: {
                // EscapePlan: draw 1; when it is a skill, gain block
                int before = (int)hand.size();
                drawCards(1, false);
                if ((int)hand.size() > before && hand.back().def().type == CardType::SKILL)
                    gainBlock(amt);
                break;
            }
            case StepOp::DISCARD_HAND: {
                for (auto& hc : hand) { discardPile.push_back(hc); ++cardsDiscardedThisTurn; }
                hand.clear();
                break;
            }
            case StepOp::STORM_OF_STEEL: {
                // discard the hand, then create as many Shivs as cards discarded
                int n = (int)hand.size();
                for (auto& hc : hand) { discardPile.push_back(hc); ++cardsDiscardedThisTurn; }
                hand.clear();
                for (int i = 0; i < n && !battleOver; ++i) {
                    if ((int)hand.size() >= maxHandSize) break;
                    CardInstance shiv;
                    shiv.id = CardId::SHIV;
                    shiv.uniqueIdx = ++nextUniqueIdx;
                    shiv.genCard = true;
                    if (c.upgrade > 0) shiv.upgrade = 1; // upgraded SoS upgrades its Shivs
                    ++cardsGeneratedByOwner;
                    hand.push_back(shiv);
                }
                break;
            }
            case StepOp::GAMBLE_REDRAW: {
                // CalculatedGamble: discard the hand, redraw the same count
                int n = (int)hand.size();
                for (auto& hc : hand) { discardPile.push_back(hc); ++cardsDiscardedThisTurn; }
                hand.clear();
                drawCards(n, false);
                break;
            }
            case StepOp::BLOCK_AND_RETAIN: {
                // DodgeAndRoll: block now; the GAINED amount returns next turn
                int gained = gainBlockReturns(amt);
                if (gained > 0)
                    addPower(player.powers, PowerId::BLOCK_NEXT_TURN, gained, false);
                break;
            }
            case StepOp::CHOICE_UPGRADE_HAND: {
                // Armaments: upgraded form upgrades every upgradable hand card; the base
                // form upgrades the route-chosen card
                if (c.upgrade) {
                    for (auto& hc : hand)
                        if (&hc != &c) upgradeCardInstance(hc);
                } else if (pendingChoice.valid) {
                    int seen = 0;
                    for (auto& hc : hand) {
                        if (strcmp(CardId_NAMES[(int)hc.id], pendingChoice.cardId) == 0
                            && hc.upgrade == pendingChoice.upgrade) {
                            if (seen == pendingChoice.sourceOccurrence) {
                                upgradeCardInstance(hc);
                                break;
                            }
                            ++seen;
                        }
                    }
                }
                break;
            }
            case StepOp::CHOICE_RETAIN_HAND: {
                // Snap: the route-chosen hand card gains Retain for the rest of combat
                if (pendingChoice.valid) {
                    int seen = 0;
                    for (auto& hc : hand) {
                        if (strcmp(CardId_NAMES[(int)hc.id], pendingChoice.cardId) == 0
                            && hc.upgrade == pendingChoice.upgrade) {
                            if (seen == pendingChoice.sourceOccurrence) {
                                applyRetainKeyword(hc);
                                break;
                            }
                            ++seen;
                        }
                    }
                }
                break;
            }
            case StepOp::CHOICE_EXHAUST_HAND: {
                // no recorded choice (TrueGrit base): fall back to the random exhaust
                if (!pendingChoice.valid) {
                    if (!hand.empty()) {
                        Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                        int pick = r.NextInt((int)hand.size());
                        exhaustCard(hand[pick]);
                        hand.erase(hand.begin() + pick);
                    }
                    break;
                }
                {
                    int seen = 0;
                    for (int i = 0; i < (int)hand.size(); ++i) {
                        if (strcmp(CardId_NAMES[(int)hand[i].id], pendingChoice.cardId) == 0
                            && hand[i].upgrade == pendingChoice.upgrade) {
                            if (seen == pendingChoice.sourceOccurrence) {
                                exhaustCard(hand[i]);
                                hand.erase(hand.begin() + i);
                                break;
                            }
                            ++seen;
                        }
                    }
                }
                break;
            }
            case StepOp::CHOICE_DISCARD_HAND: {
                // discard the chosen cards (single selection cards: amt unused; multi
                // selects arrive as repeated play actions in the route)
                if (pendingChoice.valid) {
                    int seen = 0;
                    for (int i = 0; i < (int)hand.size(); ++i) {
                        if (strcmp(CardId_NAMES[(int)hand[i].id], pendingChoice.cardId) == 0
                            && hand[i].upgrade == pendingChoice.upgrade) {
                            if (seen == pendingChoice.sourceOccurrence) {
                                discardPile.push_back(hand[i]);
                                hand.erase(hand.begin() + i);
                                ++cardsDiscardedThisTurn;
                                break;
                            }
                            ++seen;
                        }
                    }
                }
                break;
            }
            case StepOp::HEADBUTT_CHOICE: {
                // attack; the chosen discard card goes on top of the draw pile
                {
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size()) {
                        int dmg = attackDamageFromBase(amt, ti);
                        if (!battleOver && enemies[ti].curHp > 0 && !enemies[ti].reviving)
                            damageEnemy(ti, dmg);
                    }
                }
                if (pendingChoice.valid) {
                    int seen = 0;
                    for (int i = 0; i < (int)discardPile.size(); ++i) {
                        if (strcmp(CardId_NAMES[(int)discardPile[i].id], pendingChoice.cardId) == 0
                            && discardPile[i].upgrade == pendingChoice.upgrade) {
                            if (seen == pendingChoice.sourceOccurrence) {
                                drawPile.insert(drawPile.begin(), discardPile[i]);
                                discardPile.erase(discardPile.begin() + i);
                                break;
                            }
                            ++seen;
                        }
                    }
                }
                break;
            }
            case StepOp::BUFF_ALL_SAME_ID: {
                // Claw/Maul: every same-id card in AllCards (four piles + the in-flight
                // card in the Play pile) gains vars[extraVarIdx] on vars[varIdx].
                int boost = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0;
                if (boost != 0) {
                    for (auto* pile : {&hand, &drawPile, &discardPile, &exhaustPile})
                        for (auto& ci : *pile)
                            if (ci.id == c.id) bumpCardVar(ci, st.varIdx, boost);
                    bumpCardVar(const_cast<CardInstance&>(c), st.varIdx, boost);
                }
                break;
            }
            case StepOp::BUFF_SELF: {
                // GeneticAlgorithm/TheScythe: the played instance keeps the growth (the deck
                // version also grows, but that is a run-level effect outside one combat).
                int boost = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0;
                bumpCardVar(const_cast<CardInstance&>(c), st.varIdx, boost);
                break;
            }
            case StepOp::MAD_SCIENCE: {
                // TinkerTime card: base effect by configured type, then the rider.
                const int ty = c.tinkerType;
                const int rd = c.tinkerRider;
                if (ty == 0) { // Attack: 12 damage, Violence makes it 3 hits
                    const int hits = rd == 2 ? 3 : 1;
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size()) {
                        const int dmg = attackDamageFromBase(12, ti);
                        for (int h = 0; h < hits && !battleOver; ++h) {
                            const Monster& t = enemies[ti];
                            if (t.removed || t.curHp <= 0 || t.reviving) break;
                            damageEnemy(ti, dmg);
                        }
                        if (rd == 1) { // Sapping: 2 Weak + 2 Vulnerable
                            applyPowerToEnemy(ti, PowerId::WEAK, 2);
                            applyPowerToEnemy(ti, PowerId::VULNERABLE, 2);
                        } else if (rd == 3) { // Choking: Strangle 6
                            applyPowerToEnemy(ti, PowerId::STRANGLE, 6);
                        }
                    }
                } else if (ty == 1) { // Skill: 8 block
                    gainBlock(8);
                } else { // Power: rider-only effects
                    if (rd == 7) { // Expertise
                        applyPowerToPlayer(PowerId::STRENGTH, 2);
                        applyPowerToPlayer(PowerId::DEXTERITY, 2);
                    } else if (rd == 8) {
                        applyPowerToPlayer(PowerId::CURIOUS, 1);
                    } else if (rd == 9) {
                        applyPowerToPlayer(PowerId::IMPROVEMENT, 1);
                    }
                }
                if (rd == 4) { // Energized
                    player.energy += 2;
                } else if (rd == 5) { // Wisdom
                    drawCards(3, /*naturalDraw=*/false);
                } else if (rd == 6) { // Chaos: one free random character-pool card to hand
                    int pn = 0;
                    const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                    std::vector<CardId> opts;
                    for (int i = 0; pool && i < pn; ++i) opts.push_back(pool[i]);
                    if (!opts.empty()) {
                        Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                        for (int i = (int)opts.size() - 1; i > 0; --i) {
                            int j = r.NextInt(i + 1);
                            CardId tmp = opts[i];
                            opts[i] = opts[j];
                            opts[j] = tmp;
                        }
                        CardInstance nc;
                        nc.id = opts[0];
                        nc.costOverride = 0; // SetToFreeThisTurn
                        nc.uniqueIdx = ++nextUniqueIdx;
                        nc.genCard = true;
                        ++cardsGeneratedByOwner;
                        if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                        else discardPile.push_back(nc);
                    }
                }
                break;
            }
            case StepOp::ATTACK_SPLASH: {
                // Omnislice: damage the target, then the same struck amount
                // (DamageResult.TotalDamage + OverkillDamage, i.e. the final damage value
                // after modifiers and caps) hits every other hittable enemy unpowered.
                int ti = targetEnemyIdx;
                if (ti < 0 || ti >= (int)enemies.size()) break;
                {
                    const Monster& t = enemies[ti];
                    if (t.removed || t.curHp <= 0 || t.reviving) break;
                }
                int dmg = attackDamageFromBase(amt, ti);
                if (int cap = powerAmount(enemies[ti].powers, PowerId::HARD_TO_KILL))
                    dmg = std::min(dmg, cap);
                for (int h = 0; h < hits && !battleOver; ++h) {
                    const Monster& t = enemies[ti];
                    if (t.removed || t.curHp <= 0 || t.reviving) break;
                    damageEnemy(ti, dmg);
                }
                if (battleOver) break;
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    if (i == ti) continue;
                    const Monster& t = enemies[i];
                    if (t.removed || t.curHp <= 0 || t.reviving) continue;
                    damageEnemy(i, dmg, /*powered=*/false);
                }
                break;
            }
            case StepOp::RESTLESSNESS: {
                // Restlessness: the play already removed this card from the hand, so
                // IsOnlyCardInHand means the hand is empty now.
                if (!hand.empty()) break;
                if (amt > 0) drawCards(amt);
                if (st.extraVarIdx >= 0) player.energy += cardVarValue(c, d, st.extraVarIdx);
                break;
            }
            case StepOp::GENERATE_ZERO_COST: {
                // Jackpot: CardFactory.GetForCombat over the character pool filtered to
                // cards whose canonical cost is 0 (and not X), NextItem per card with
                // repeats allowed. Upgraded Jackpot upgrades every generated card.
                const int n = amt > 0 ? amt : 1;
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i) {
                    const CardDef& pd = BattleContext::cardDef(pool[i]);
                    if (pd.cost == 0 && !pd.hasFlag(CF_ENERGY_X)) opts.push_back(pool[i]);
                }
                if (opts.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = opts[r.NextInt((int)opts.size())];
                    nc.upgrade = c.upgrade > 0 ? 1 : 0;
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner;
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::GENERATE_COLORLESS_TO_HAND: {
                // JackOfAllTrades: CardFactory.GetDistinctForCombat over the colorless
                // pool minus JackOfAllTrades itself. TakeRandom = UnstableShuffle (the
                // Fisher-Yates loop below, from the end) then take N.
                const int n = amt > 0 ? amt : 1;
                int pn = 0;
                const CardId* pool = sts2CombatGenPool("COLORLESS", &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i)
                    if (pool[i] != CardId::JACK_OF_ALL_TRADES) opts.push_back(pool[i]);
                if (opts.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    CardId tmp = opts[i];
                    opts[i] = opts[j];
                    opts[j] = tmp;
                }
                for (int i = 0; i < n && i < (int)opts.size() && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = opts[i];
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner;
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::ALCHEMIZE: {
                // Alchemize: PotionFactory.CreateRandomPotionInCombat — a rarity roll on
                // the CombatPotionGeneration stream (<=0.1 Rare, <=0.35 Uncommon, else
                // Common) then NextItem over the combat-generatable potions of that
                // rarity. A full belt still consumes the roll and drops the potion.
                int pcount = 0;
                const PotionId* ptable = sts2PotionShopPool(&pcount);
                Sts2Rng& r = rng[(int)RngStream::CombatPotionGeneration];
                const float roll = r.NextFloat();
                const PotionRarity want = roll <= 0.1f ? PotionRarity::RARE
                    : (roll <= 0.35f ? PotionRarity::UNCOMMON : PotionRarity::COMMON);
                std::vector<int> cand;
                for (int i = 0; i < pcount; ++i) {
                    const PotionDef& p = sts2PotionDef(ptable[i]);
                    if (!p.sharedPool || p.rarity != want || !p.canBeGeneratedInCombat)
                        continue;
                    // PotionFactory.GetPotionOptions concatenates the CHARACTER pool, but
                    // that pool is gated behind Ironclad4Epoch, which a fresh save has not
                    // revealed (same rule as the card pools). Potion1/2Epoch gate these
                    // six shared potions the same way. The Alchemize corpus caught the
                    // three Ironclad commons shifting the index (Swift vs Vulnerable).
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
                    cand.push_back(static_cast<int>(ptable[i]));
                }
                if (cand.empty()) break;
                const int pid = cand[r.NextInt((int)cand.size())];
                for (auto& slot : player.potions)
                    if (slot == 0) { slot = pid; break; }
                break;
            }
            case StepOp::ANOINTED: {
                // Anointed: take up to (hand space) Rare cards from the draw pile.
                // TakeRandom = UnstableShuffle over the rare cards in pile order, then
                // take; the chosen cards MOVE to the hand (no generated copies).
                int space = maxHandSize - (int)hand.size();
                if (space <= 0) break;
                std::vector<int> rares;
                for (int i = 0; i < (int)drawPile.size(); ++i)
                    if (drawPile[i].def().rarity == CardRarity::RARE) rares.push_back(i);
                if (rares.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                for (int i = (int)rares.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    int tmp = rares[i];
                    rares[i] = rares[j];
                    rares[j] = tmp;
                }
                const int take = std::min(space, (int)rares.size());
                std::vector<CardInstance> moved;
                for (int i = 0; i < take; ++i) moved.push_back(drawPile[rares[i]]);
                std::sort(rares.begin(), rares.begin() + take);
                for (int i = take - 1; i >= 0; --i)
                    drawPile.erase(drawPile.begin() + rares[i]);
                for (auto& mc : moved) {
                    if ((int)hand.size() < maxHandSize) hand.push_back(mc);
                    else discardPile.push_back(mc);
                }
                break;
            }
            case StepOp::DISCOVERY: {
                // Discovery: GetDistinctForCombat over the character pool (3 options,
                // CombatCardGeneration stream), the route records the chosen card
                // (effect GenerateToHand). The card joins free for this turn. With no
                // route choice (training) the first option is taken.
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i) opts.push_back(pool[i]);
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    CardId tmp = opts[i];
                    opts[i] = opts[j];
                    opts[j] = tmp;
                }
                const int take = std::min(3, (int)opts.size());
                int chosen = take > 0 ? (int)opts[0] : -1;
                int chosenUp = 0;
                if (pendingChoice.valid && pendingChoice.cardId[0]) {
                    for (int i = 0; i < CardId_COUNT; ++i)
                        if (std::strcmp(CardId_NAMES[i], pendingChoice.cardId) == 0) { chosen = i; break; }
                    chosenUp = pendingChoice.upgrade;
                }
                if (chosen < 0) break;
                CardInstance nc;
                nc.id = static_cast<CardId>(chosen);
                nc.upgrade = chosenUp;
                nc.freeThisTurn = true; // SetToFreeThisTurn
                nc.uniqueIdx = ++nextUniqueIdx;
                nc.genCard = true;
                ++cardsGeneratedByOwner;
                if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                else discardPile.push_back(nc);
                break;
            }
            case StepOp::SPLASH: {
                // Splash: UnlockState.CharacterCardPools minus the owner's pool, then
                // GetDistinctForCombat(3). The engine's character order is
                // SILENT/DEFECT/NECROBINDER/REGENT after removing IRONCLAD. The route's
                // chosen card is honoured; in training the first IMPLEMENTED option is
                // taken so an unimplemented other-character card cannot reach the hand.
                static const char* kOtherPools[] = {"SILENT", "DEFECT", "NECROBINDER", "REGENT"};
                std::vector<CardId> opts;
                for (const char* name : kOtherPools) {
                    if (characterPool == name) continue;
                    int pn = 0;
                    const CardId* pool = sts2CombatGenPool(name, &pn);
                    for (int i = 0; pool && i < pn; ++i) opts.push_back(pool[i]);
                }
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    CardId tmp = opts[i];
                    opts[i] = opts[j];
                    opts[j] = tmp;
                }
                const int take = std::min(3, (int)opts.size());
                int chosen = -1;
                int chosenUp = 0;
                if (pendingChoice.valid && pendingChoice.cardId[0]) {
                    for (int i = 0; i < CardId_COUNT; ++i)
                        if (std::strcmp(CardId_NAMES[i], pendingChoice.cardId) == 0) { chosen = i; break; }
                    chosenUp = pendingChoice.upgrade;
                } else {
                    for (int i = 0; i < take; ++i)
                        if (BattleContext::cardImplemented(opts[i])) { chosen = (int)opts[i]; break; }
                }
                if (chosen < 0) break;
                CardInstance nc;
                nc.id = static_cast<CardId>(chosen);
                nc.upgrade = chosenUp;
                nc.freeThisTurn = true; // SetToFreeThisTurn
                nc.uniqueIdx = ++nextUniqueIdx;
                nc.genCard = true;
                ++cardsGeneratedByOwner;
                if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                else discardPile.push_back(nc);
                break;
            }
            case StepOp::LOSE_MAX_HP: {
                // BrightestFlame: LoseMaxHp(amount, isFromCard) — unblockable damage
                // first when the current HP sits above the new cap, then the cap drops
                // (floored at 1). The fight sync writes player.maxHp back to the run.
                if (amt <= 0) break;
                const int newMax = std::max(1, player.maxHp - amt);
                if (player.curHp > newMax) damagePlayerUnblockable(player.curHp - newMax);
                player.maxHp = newMax;
                break;
            }
            case StepOp::HIDDEN_GEM: {
                // HiddenGem: pick a random eligible draw-pile card on the
                // CombatCardSelection stream and add Replay to it. Eligible = playable,
                // type not Curse/Quest, no replay yet; Attack/Skill/Power preferred.
                if (drawPile.empty()) break;
                std::vector<int> eligible, preferred;
                for (int i = 0; i < (int)drawPile.size(); ++i) {
                    const CardInstance& ci = drawPile[i];
                    if (cardHasFlag(ci, CF_UNPLAYABLE) || ci.extraReplays > 0) continue;
                    const CardType ty = effectiveCardType(ci);
                    if (ty == CardType::CURSE || ty == CardType::QUEST) continue;
                    eligible.push_back(i);
                    if (ty == CardType::ATTACK || ty == CardType::SKILL || ty == CardType::POWER)
                        preferred.push_back(i);
                }
                std::vector<int>& pool = preferred.empty() ? eligible : preferred;
                if (pool.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                CardInstance& gemTarget = drawPile[pool[r.NextInt((int)pool.size())]];
                gemTarget.extraReplays += amt;
                // Parsed deck cards re-emit their verbatim stamp entry, so the replay
                // slot (index 3: name, cost, star, replay, ...) must be rewritten too.
                if (!gemTarget.stampText.empty()) {
                    std::vector<std::string> parts;
                    std::string cur;
                    for (char ch : gemTarget.stampText) {
                        if (ch == '/') { parts.push_back(cur); cur.clear(); }
                        else cur += ch;
                    }
                    parts.push_back(cur);
                    if (parts.size() > 3) {
                        parts[3] = std::to_string(gemTarget.extraReplays);
                        std::string out;
                        for (size_t k = 0; k < parts.size(); ++k) {
                            if (k) out += '/';
                            out += parts[k];
                        }
                        gemTarget.stampText = out;
                    }
                }
                break;
            }
            case StepOp::AUTOPLAY_DISCARD_ATTACKS: {
                // BeatDown: StableShuffle (sort by model order, then the Shuffle stream)
                // the playable attacks in the discard pile, take N, auto-play each. The
                // AnyEnemy target roll happens inside autoplayCard on CombatTargets,
                // exactly as CardCmd.AutoPlay(item, randomTarget) would.
                const int n = amt > 0 ? amt : 1;
                std::vector<int> idx;
                for (int i = 0; i < (int)discardPile.size(); ++i) {
                    const CardDef& cd = discardPile[i].def();
                    if (cd.type == CardType::ATTACK && !cardHasFlag(discardPile[i], CF_UNPLAYABLE))
                        idx.push_back(i);
                }
                if (idx.empty()) break;
                std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                    const CardDef& da = discardPile[a].def();
                    const CardDef& db = discardPile[b].def();
                    if (da.id != db.id) return da.id < db.id;
                    return discardPile[a].upgrade < discardPile[b].upgrade;
                });
                Sts2Rng& r = rng[(int)RngStream::Shuffle];
                for (int i = (int)idx.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    int tmp = idx[i];
                    idx[i] = idx[j];
                    idx[j] = tmp;
                }
                const int take = std::min(n, (int)idx.size());
                std::vector<CardInstance> picked;
                for (int i = 0; i < take; ++i) picked.push_back(discardPile[idx[i]]);
                std::vector<int> eraseIdx(idx.begin(), idx.begin() + take);
                std::sort(eraseIdx.begin(), eraseIdx.end());
                for (int i = (int)eraseIdx.size() - 1; i >= 0; --i)
                    discardPile.erase(discardPile.begin() + eraseIdx[i]);
                for (auto& pc : picked) {
                    if (battleOver) break;
                    if (!autoplayCard(pc)) return false;
                }
                break;
            }
            case StepOp::AUTOPLAY_DRAW_RANDOM: {
                // Catastrophe: each iteration StableShuffles the playable draw-pile cards
                // on the Shuffle stream and auto-plays the first; when none is playable it
                // shuffles the whole draw pile instead.
                const int n = amt > 0 ? amt : 1;
                for (int k = 0; k < n && !battleOver; ++k) {
                    auto pickAndPlay = [&](bool playableOnly) -> bool {
                        std::vector<int> idx;
                        for (int i = 0; i < (int)drawPile.size(); ++i)
                            if (!playableOnly || !cardHasFlag(drawPile[i], CF_UNPLAYABLE))
                                idx.push_back(i);
                        if (idx.empty()) return false;
                        std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) {
                            const CardDef& da = drawPile[a].def();
                            const CardDef& db = drawPile[b].def();
                            if (da.id != db.id) return da.id < db.id;
                            return drawPile[a].upgrade < drawPile[b].upgrade;
                        });
                        Sts2Rng& r = rng[(int)RngStream::Shuffle];
                        for (int i = (int)idx.size() - 1; i > 0; --i) {
                            int j = r.NextInt(i + 1);
                            int tmp = idx[i];
                            idx[i] = idx[j];
                            idx[j] = tmp;
                        }
                        const int pick = idx.front();
                        CardInstance card = drawPile[pick];
                        drawPile.erase(drawPile.begin() + pick);
                        return autoplayCard(card);
                    };
                    if (drawPile.empty()) break;
                    bool ok = pickAndPlay(true);
                    if (ok) continue;
                    if (!unsupportedCard.empty()) return false;
                    if (drawPile.empty()) break;
                    if (!pickAndPlay(false)) return unsupportedCard.empty();
                }
                break;
            }
            case StepOp::TORIC_TOUGHNESS: {
                // ToricToughness.OnPlay: gain Block (powered), then an instanced power
                // with Amount = Turns and aux = the block actually gained; the power
                // re-grants that block whenever the owner's block is cleared.
                const int blockAmt = gainBlockReturns(amt);
                const int turns = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 2;
                if (turns > 0) {
                    applyPowerToPlayer(PowerId::TORIC_TOUGHNESS, turns);
                    for (auto it = player.powers.rbegin(); it != player.powers.rend(); ++it)
                        if (it->id == PowerId::TORIC_TOUGHNESS) {
                            it->aux = blockAmt;
                            break;
                        }
                }
                break;
            }
            case StepOp::DOUBLE_BLOCK: {
                // Entrench: CreatureCmd.GainBlock(current block, Unpowered | Move)
                gainBlockUnpowered(player.block);
                break;
            }
            case StepOp::GENERATE_ATTACKS_TO_DRAW: {
                // Metamorphosis: CardFactory.GetForCombat picks N attacks from the
                // character combat pool with NextItem per card (repeats allowed), sets each
                // free for the whole combat, and inserts it at a random draw-pile position
                // (Shuffle.NextInt(count + 1)).
                const int n = st.varIdx >= 0 ? cardVarValue(c, d, st.varIdx) : 1;
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i)
                    if (BattleContext::cardDef(pool[i]).type == CardType::ATTACK)
                        opts.push_back(pool[i]);
                if (opts.empty()) break;
                Sts2Rng& gen = rng[(int)RngStream::CombatCardGeneration];
                Sts2Rng& sh = rng[(int)RngStream::Shuffle];
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = opts[gen.NextInt(static_cast<int>(opts.size()))];
                    nc.costOverride = 0; // SetToFreeThisCombat
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner;
                    const int pos = sh.NextInt(static_cast<int>(drawPile.size()) + 1);
                    drawPile.insert(drawPile.begin() + pos, nc);
                }
                break;
            }
            case StepOp::GENERATE_FROM_POOL_TYPED: {
                // WhiteNoise/Distraction: CardFactory.GetDistinctForCombat over the character
                // pool filtered to one card type; the whole candidate list is shuffled on the
                // CombatCardGeneration stream (same discipline as Infernal Blade).
                int n = st.varIdx >= 0 ? cardVarValue(c, d, st.varIdx) : 1; // constVal is the type
                int pn = 0;
                const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
                std::vector<CardId> opts;
                for (int i = 0; pool && i < pn; ++i) {
                    const CardDef& cd = BattleContext::cardDef(pool[i]);
                    if (st.constVal == 0 || (int)cd.type == st.constVal - 1) opts.push_back(pool[i]);
                }
                if (opts.empty()) break;
                Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    CardId tmp = opts[i];
                    opts[i] = opts[j];
                    opts[j] = tmp;
                }
                for (int i = 0; i < n && i < (int)opts.size() && !battleOver; ++i) {
                    CardInstance nc;
                    nc.id = opts[i];
                    nc.costOverride = 0; // SetToFreeThisTurn
                    nc.uniqueIdx = ++nextUniqueIdx;
                    nc.genCard = true;
                    ++cardsGeneratedByOwner;
                    if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                    else discardPile.push_back(nc);
                }
                break;
            }
            case StepOp::CHOICE_SELECT_TRANSFORM:
            case StepOp::CHOICE_SELECT_EXHAUST:
            case StepOp::CHOICE_SELECT_DISCARD:
            case StepOp::CHOICE_SELECT_TO_HAND:
            case StepOp::CHOICE_SELECT_TO_DRAW_TOP:
            case StepOp::CHOICE_TRANSFORM_TO:
            case StepOp::CHOICE_CLONE_TO_HAND: {
                // Generic CardSelectCmd result application. The route record carries every
                // chosen card (id + upgrade + source occurrence, in game order); resolve each
                // against the source pile recorded with the choice, then apply the step's
                // effect. Resolution is by identity triple, not by name, so duplicate copies
                // land on the right instance (same discipline as Bolas' per-instance check).
                std::vector<CardInstance> src;
                const char* pileName = pendingChoice.sourcePile[0] ? pendingChoice.sourcePile : "Hand";
                std::vector<CardInstance>* pile = nullptr;
                if (std::strcmp(pileName, "Hand") == 0) pile = &hand;
                else if (std::strcmp(pileName, "Draw") == 0) pile = &drawPile;
                else if (std::strcmp(pileName, "Discard") == 0) pile = &discardPile;
                else if (std::strcmp(pileName, "Exhaust") == 0) pile = &exhaustPile;
                if (!pendingChoice.valid || !pile) break;

                // Build the selection: the multi-card list when present, else the single slot.
                std::vector<PendingChoice::SelCard> sel = pendingChoice.cards;
                if (sel.empty() && pendingChoice.cardId[0]) {
                    PendingChoice::SelCard one;
                    std::snprintf(one.cardId, sizeof(one.cardId), "%s", pendingChoice.cardId);
                    one.upgrade = pendingChoice.upgrade;
                    one.sourceOccurrence = pendingChoice.sourceOccurrence;
                    sel.push_back(one);
                }

                // Resolve every chosen instance FIRST (indices shift as we remove them), then
                // clear them out in one pass so occurrences stay valid against the original order.
                // sourceOccurrence is the card's index among same-id copies in the UNTOUCHED
                // pile, so each selection is counted from the pile start — a cursor would
                // restart the count and miss the second of two identical picks.
                std::vector<int> hits;
                {
                    for (const auto& sc : sel) {
                        int seen = 0;
                        for (int i = 0; i < (int)pile->size(); ++i) {
                            if (std::strcmp(CardId_NAMES[(int)(*pile)[i].id], sc.cardId) == 0
                                && (*pile)[i].upgrade == sc.upgrade) {
                                if (seen == sc.sourceOccurrence) { hits.push_back(i); break; }
                                ++seen;
                            }
                        }
                    }
                }
                std::sort(hits.begin(), hits.end());

                for (int idx : hits) {
                    if (idx < 0 || idx >= (int)pile->size()) continue;
                    CardInstance picked = (*pile)[idx];
                    switch (st.op) {
                        case StepOp::CHOICE_SELECT_TRANSFORM: {
                            // CardCmd.Transform: replace with a random card from the
                            // ORIGINAL's transformation pool, excluding its own id.
                            std::vector<CardId> opts = transformOptionsFor(picked.id, characterPool);
                            if (opts.empty()) break;
                            Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                            CardInstance nc;
                            nc.id = opts[r.NextInt((int)opts.size())];
                            nc.uniqueIdx = ++nextUniqueIdx;
                            nc.genCard = true;
                            ++cardsGeneratedByOwner; // CardCmd.Transform logs CardGeneratedEntry
                            (*pile)[idx] = nc; // in-place replacement keeps the pile slot
                            break;
                        }
                        case StepOp::CHOICE_SELECT_EXHAUST:
                            if (pile != &exhaustPile) exhaustCard(picked);
                            else ++cardExhaustsThisTurn;
                            break;
                        case StepOp::CHOICE_SELECT_DISCARD:
                            if (pile != &discardPile) discardPile.push_back(picked);
                            ++cardsDiscardedThisTurn;
                            break;
                        case StepOp::CHOICE_SELECT_TO_HAND:
                            if (pile != &hand && (int)hand.size() < maxHandSize) hand.push_back(picked);
                            break;
                        case StepOp::CHOICE_SELECT_TO_DRAW_TOP:
                            if (pile != &drawPile) drawPile.insert(drawPile.begin(), picked);
                            break;
                        case StepOp::CHOICE_CLONE_TO_HAND: {
                            // CardCmd clone family: N clones of the chosen card join the hand
                            // (CardGenerated). The chosen original stays in its pile.
                            int n = st.varIdx >= 0 ? cardVarValue(c, d, st.varIdx)
                                                   : (st.constVal > 0 ? st.constVal : 1);
                            for (int i = 0; i < n && !battleOver; ++i) {
                                CardInstance clone = picked;
                                clone.genCard = true;
                                clone.stampText.clear();
                                clone.uniqueIdx = ++nextUniqueIdx;
                                ++cardsGeneratedByOwner;
                                if ((int)hand.size() < maxHandSize) hand.push_back(clone);
                                else discardPile.push_back(clone);
                            }
                            break;
                        }
                        case StepOp::CHOICE_TRANSFORM_TO: {
                            // CardCmd.TransformTo<T>: the chosen card becomes a fresh copy of
                            // the step's cardId in the same pile slot (no RNG draw).
                            CardInstance nc;
                            nc.id = st.cardId;
                            // constVal bit 0 = "upgrade the replacement when THIS card is
                            // upgraded" (Charge), not an unconditional upgrade.
                            nc.upgrade = (c.upgrade > 0 && (st.constVal & 1)) ? 1 : 0;
                            nc.uniqueIdx = ++nextUniqueIdx;
                            nc.genCard = true;
                            ++cardsGeneratedByOwner; // CardCmd.Transform logs CardGeneratedEntry
                            (*pile)[idx] = nc;
                            break;
                        }
                        default: break;
                    }
                }
                // The transform op replaced its instance in place; every other op moved the
                // card out, so drop the originals (descending order keeps the indices valid).
                if (st.op != StepOp::CHOICE_SELECT_TRANSFORM
                    && st.op != StepOp::CHOICE_TRANSFORM_TO
                    && st.op != StepOp::CHOICE_CLONE_TO_HAND) {
                    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
                        if (*it >= 0 && *it < (int)pile->size()) pile->erase(pile->begin() + *it);
                }
                break;
            }
            case StepOp::SHUFFLE_TAKE_FROM_DRAW: {
                // SeekerStrike: PileType.Draw.Cards.ToList().StableShuffle(CombatCardSelection)
                // → Take(Cards) only builds the candidate list; the pile is untouched. The
                // shuffle's Fisher-Yates still advances the stream (n-1 draws), so it must run.
                int n = (int)drawPile.size();
                Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                for (int i = n - 1; i > 0; --i) r.NextInt(i + 1);
                break;
            }
            case StepOp::CHANNEL_ORB: {
                // constVal carries the OrbKind ordinal (generator emits it); evoking on a
                // full queue happens inside channelOrb (OrbCmd.Channel).
                // Repeat count: step.hits (plain), or a calc multiplier
                // (Voltaic: Lightning orbs channelled this combat; Tempest/Hibernate/IceLance: X).
                int n = 1;
                if (st.calc == CalcKind::LIGHTNING_CHANNELED) n = lightningChanneled;
                else if (st.calc == CalcKind::X_VALUE) n = xValue >= 0 ? xValue : player.energy;
                else if (st.calc == CalcKind::X_PLUS_UPGRADE)
                    n = (xValue >= 0 ? xValue : player.energy) + c.upgrade; // Tempest
                else if (st.calc == CalcKind::HITTABLE_COUNT) {
                    // Chill: one Frost orb per hittable enemy at play time
                    n = 0;
                    for (const auto& e : enemies)
                        if (!e.removed && e.curHp > 0 && !e.reviving) ++n;
                }
                else if (st.varIdx >= 0) n = cardVarValue(c, d, st.varIdx);
                else if (st.hits > 1) n = st.hits; // fixed repeat count (Glacier: two Frost)
                for (int i = 0; i < n && !battleOver; ++i) channelOrb((OrbKind)st.constVal);
                break;
            }
            case StepOp::CHANNEL_RANDOM: {
                // Chaos: channel a random orb (OrbModel.GetRandomOrb on CombatOrbGeneration;
                // the pool order is exactly the OrbKind enum order)
                int n = st.varIdx >= 0 ? cardVarValue(c, d, st.varIdx)
                                       : (st.constVal > 0 ? st.constVal : 1);
                for (int i = 0; i < n && !battleOver; ++i) {
                    Sts2Rng& rg = rng[(int)RngStream::CombatOrbGeneration];
                    channelOrb((OrbKind)(1 + rg.NextInt(5)));
                }
                break;
            }
            case StepOp::EVOKE_FRONT: {
                // Dualcast/MultiCast/Quadcast family: evoke the front orb N times, dequeuing
                // only on the LAST iteration (game: i == evokeCount-1). N comes from
                // step.hits (Quadcast), a var (IceLance-style repeats), X, or X+upgrade.
                int n;
                if (st.calc == CalcKind::X_PLUS_UPGRADE)
                    n = (xValue >= 0 ? xValue : player.energy) + (c.upgrade > 0 ? 1 : 0);
                else if (st.calc == CalcKind::X_VALUE) n = xValue >= 0 ? xValue : player.energy;
                else if (st.varIdx >= 0 && amt > 0) n = amt;
                else n = st.constVal > 0 ? st.constVal : 1;
                for (int i = 0; i < n && !battleOver; ++i) {
                    if (player.orbs.empty()) break;
                    if (!evokeFront(i == n - 1)) break;
                }
                break;
            }
            case StepOp::EVOKE_ALL_DEQUEUE: {
                // Shatter: evoke every orb present at play time, each one twice —
                // first with dequeue=false (value fires), then with dequeue=true
                // (fires again and leaves). The queue length is captured up front.
                int n = (int)player.orbs.size();
                for (int i = 0; i < n && !battleOver; ++i) {
                    if (player.orbs.empty()) break;
                    evokeFront(/*dequeue=*/false);
                    if (battleOver || player.orbs.empty()) break;
                    evokeFront(/*dequeue=*/true);
                }
                break;
            }
            case StepOp::EVOKE_LAST_N: {
                int n = amt > 0 ? amt : 1;
                for (int i = 0; i < n && !battleOver; ++i)
                    if (!evokeLast()) break;
                break;
            }
            case StepOp::ADD_ORB_SLOTS:
                addOrbSlots(amt);
                break;
            case StepOp::REMOVE_ORB_SLOTS:
                removeOrbSlots(amt);
                break;
            case StepOp::PASSIVE_ALL_DARK: {
                // Darkness: the channel already ran in the preceding step; fire the passive
                // of every Dark orb once (base) or twice (upgraded) — the game loops
                // triggerCount = IsUpgraded ? 2 : 1 over each Dark orb.
                int rep = c.upgrade > 0 ? 2 : 1;
                for (int r = 0; r < rep && !battleOver; ++r)
                    for (size_t oi = 0; oi < player.orbs.size() && !battleOver; ++oi)
                        if (player.orbs[oi].kind == OrbKind::DARK) triggerOrbPassive(player.orbs[oi]);
                break;
            }
            case StepOp::PASSIVE_ALL_LIGHTNING: {
                // TeslaCoil: fire the passive of each Lightning orb, targeting the played
                // card's target (OrbCmd.Passive(choiceContext, orb, cardPlay.Target)).
                // The queue is snapshotted: passives never reorder it.
                int n = lightningOrbCount();
                for (int i = 0, fired = 0; i < (int)player.orbs.size() && fired < n && !battleOver; ++i)
                    if (player.orbs[i].kind == OrbKind::LIGHTNING) {
                        triggerOrbPassive(player.orbs[i], targetEnemyIdx);
                        ++fired;
                    }
                break;
            }
            case StepOp::OSTY_SUMMON: {
                // Dirge loops ResolveEnergyXValue() summons
                int n = st.calc == CalcKind::X_VALUE ? (xValue >= 0 ? xValue : player.energy) : 1;
                for (int i = 0; i < n && !battleOver; ++i) summonOsty(amt);
                break;
            }
            case StepOp::OSTY_ATTACK:
                ostyAttackFrom(amt, targetEnemyIdx, /*allEnemies=*/false);
                break;
            case StepOp::OSTY_ATTACK_ALL:
                ostyAttackFrom(amt, -1, /*allEnemies=*/true);
                break;
            case StepOp::OSTY_KILL:
                killOsty();
                break;
            case StepOp::OSTY_MAXHP_ATTACK: {
                // Unleash: damage = CalculationBase + ExtraDamage * Osty.CurrentHp, only
                // while the pet lives (Unleash.cs multiplier = osty.IsAlive ? CurrentHp : 0)
                if (!player.osty.present || player.osty.curHp <= 0) break;
                int mult = player.osty.curHp;
                int extra = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 1;
                int dmg = amt + extra * mult;
                int ti = targetEnemyIdx;
                if (ti >= 0 && ti < (int)enemies.size()) {
                    if (powerAmount(enemies[ti].powers, PowerId::VULNERABLE) > 0) {
                        Rational v; v.num = dmg; v.mul(3, 2); dmg = (int)v.trunc();
                    }
                    damageEnemy(ti, dmg, /*powered=*/true);
                }
                break;
            }
            case StepOp::OSTY_MAXHP_BLOCK: {
                // Sacrifice: block = MaxHp * 3, computed BEFORE the pet is killed
                // (Sacrifice.cs: Calculate, then Kill, then GainBlock)
                if (!player.osty.present || player.osty.curHp <= 0) break;
                int extra = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 3;
                int blockGain = extra * player.osty.maxHp;
                killOsty();
                gainBlock(blockGain);
                break;
            }
            case StepOp::OSTY_HEAL:
                // CreatureCmd.Heal on the pet (capped at its MaxHp)
                if (player.osty.present && player.osty.curHp > 0)
                    player.osty.curHp = std::min(player.osty.maxHp, player.osty.curHp + amt);
                break;
            case StepOp::FORGE:
                forge(amt);
                break;
            case StepOp::DEATHS_DOOR_BLOCK: {
                int times = 1;
                if (doomAppliedByOwnerThisTurn)
                    times += st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 1;
                for (int i = 0; i < times && !battleOver; ++i) gainBlock(amt);
                break;
            }
            case StepOp::FORGE_HITS_CALC: {
                // BeatIntoShape: CalculatedForge (base + extra × hits this turn) minus the
                // attack's own damage instances × extra.
                int ti = targetEnemyIdx;
                int hits = (ti >= 0 && ti < (int)enemies.size()) ? enemies[ti].hitsTakenThisTurn : 0;
                int base = cardVarValue(c, d, st.varIdx);
                int extra = st.extraVarIdx >= 0 ? cardVarValue(c, d, st.extraVarIdx) : 0;
                int own = st.constVal > 0 ? st.constVal : 1;
                int amount = base + extra * hits - own * extra;
                if (amount > 0) forge(amount);
                break;
            }
            case StepOp::SUMMON_FORTH_BLADES: {
                // SummonForth: every non-exhausted Sovereign Blade in any pile moves to hand
                for (auto* pile : {&discardPile, &drawPile}) {
                    for (auto it = pile->begin(); it != pile->end();) {
                        if (it->id == CardId::SOVEREIGN_BLADE) {
                            if ((int)hand.size() < maxHandSize) {
                                hand.push_back(*it);
                                it = pile->erase(it);
                                continue;
                            }
                        }
                        ++it;
                    }
                }
                break;
            }
            case StepOp::CREATE_SOULS: {
                // Soul.CreateInHand: token cards (0-cost skill, Exhaust, "draw 2") to hand.
                // Dirge: n = X (energy spent); upgraded Dirge upgrades every created Soul.
                int n = (st.calc == CalcKind::X_VALUE) ? (xValue >= 0 ? xValue : player.energy)
                                                       : (amt > 0 ? amt : 1);
                const bool upSoul = (st.cond & STEP_UPGRADE_SOULS) && c.upgrade > 0;
                for (int i = 0; i < n && !battleOver; ++i) {
                    if ((int)hand.size() >= maxHandSize) break;
                    CardInstance soul;
                    soul.id = CardId::SOUL;
                    soul.uniqueIdx = ++nextUniqueIdx;
                    soul.genCard = true;
                    if (upSoul) soul.upgrade = 1;
                    ++cardsGeneratedByOwner;
                    hand.push_back(soul);
                }
                break;
            }
            case StepOp::CREATE_SOULS_TO_DISCARD: {
                int n = (st.calc == CalcKind::X_VALUE) ? (xValue >= 0 ? xValue : player.energy)
                                                       : (amt > 0 ? amt : 1);
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance soul;
                    soul.id = CardId::SOUL;
                    soul.uniqueIdx = ++nextUniqueIdx;
                    soul.genCard = true;
                    if ((st.cond & STEP_UPGRADE_SOULS) && c.upgrade > 0) soul.upgrade = 1;
                    ++cardsGeneratedByOwner;
                    discardPile.push_back(soul);
                }
                break;
            }
            case StepOp::CREATE_SOULS_RANDOM_DRAW: {
                int n = (st.calc == CalcKind::X_VALUE) ? (xValue >= 0 ? xValue : player.energy)
                                                       : (amt > 0 ? amt : 1);
                const bool upSoul = (st.cond & STEP_UPGRADE_SOULS) && c.upgrade > 0;
                Sts2Rng& r = rng[(int)RngStream::Shuffle];
                for (int i = 0; i < n && !battleOver; ++i) {
                    CardInstance soul;
                    soul.id = CardId::SOUL;
                    soul.uniqueIdx = ++nextUniqueIdx;
                    soul.genCard = true;
                    if (upSoul) soul.upgrade = 1;
                    ++cardsGeneratedByOwner;
                    int idx = r.NextInt((int)drawPile.size() + 1);
                    drawPile.insert(drawPile.begin() + idx, soul);
                }
                break;
            }
            case StepOp::SOVEREIGN_BLADE_ATTACK: {
                // SovereignBlade: Damage var hits, Repeat count per hit. SeekingEdgePower
                // retargets to ALL enemies; ParryPower converts it into a block gain equal
                // to the Parry amount (after the attack).
                const CardDef& bd = d;
                int rep = bd.nVars > 4 ? cardVarValue(c, bd, 4) : 1; // Repeat slot
                if (powerAmount(player.powers, PowerId::SEEKING_EDGE) > 0) {
                    for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                        const Monster& t = enemies[i];
                        if (t.removed || t.curHp <= 0 || t.reviving) continue;
                        int dmg = attackDamageFromBase(amt, i);
                        for (int h = 0; h < rep && !battleOver; ++h) {
                            if (enemies[i].removed || enemies[i].curHp <= 0) break;
                            damageEnemy(i, dmg);
                        }
                    }
                } else {
                    int ti = targetEnemyIdx;
                    if (ti >= 0 && ti < (int)enemies.size()) {
                        int dmg = attackDamageFromBase(amt, ti);
                        for (int h = 0; h < rep && !battleOver; ++h) {
                            if (enemies[ti].removed || enemies[ti].curHp <= 0) break;
                            damageEnemy(ti, dmg);
                        }
                    }
                }
                if (int parry = powerAmount(player.powers, PowerId::PARRY))
                    gainBlock(parry); // CalculatedBlock multiplier = Parry amount
                break;
            }
            case StepOp::COST_ADD_THIS_COMBAT:
                // EnergyCost.AddThisCombat(delta): Modded/UpMySleeve change their own cost
                // for the rest of the fight (stamp carries the cost-state ledger)
                addCostModThisCombat(c, amt);
                break;
            case StepOp::UNSUPPORTED:
            case StepOp::NONE:
            default:
                std::fprintf(stderr, "[card] not implemented: %s\n", d.name);
                currentCardDamageBonus = savedDmgBonus;
                currentCardBlockBonus = savedBlkBonus;
                currentDamageBonusVigorous = savedVigorous;
                currentDamageCorrupted = savedCorrupted;
                return false;
        }
        if (battleOver) break;
    }
    // EnchantmentModel.OnPlay: Sown grants Amount energy on the first play, then disables
    // itself; Swift draws Amount the same way.
    if (c.enchant == static_cast<int>(EnchantmentId::SOWN) && !c.enchantDisabled) {
        player.energy += c.enchantAmount;
        c.enchantDisabled = true;
        if (c.deckIdx >= 0) enchantDisabledDeckIdx.push_back(c.deckIdx);
    }
    if (c.enchant == static_cast<int>(EnchantmentId::SWIFT) && !c.enchantDisabled) {
        drawCards(c.enchantAmount, /*naturalDraw=*/false);
        c.enchantDisabled = true;
        if (c.deckIdx >= 0) enchantDisabledDeckIdx.push_back(c.deckIdx);
    }
    // Corrupted: playing the enchanted attack costs 2 unblockable HP.
    if (c.enchant == static_cast<int>(EnchantmentId::CORRUPTED)) {
        player.curHp = std::max(0, player.curHp - 2);
        ++corruptedTriggers;
        if (player.curHp <= 0) {
            battleOver = true;
            playerWon = false;
        }
    }
    // Vigorous: one-shot damage bonus; AfterCardPlayed disables it.
    if (c.enchant == static_cast<int>(EnchantmentId::VIGOROUS) && !c.enchantDisabled) {
        c.enchantDisabled = true;
        if (c.deckIdx >= 0) enchantDisabledDeckIdx.push_back(c.deckIdx);
    }
    currentCardDamageBonus = savedDmgBonus;
    currentCardBlockBonus = savedBlkBonus;
    currentDamageBonusVigorous = savedVigorous;
    currentDamageCorrupted = savedCorrupted;
    return true;
}

// CardCmd.AutoPlay: free play; AnyEnemy target picked via the CombatTargets stream
// when not given; unplayables just move to their result pile.
bool BattleContext::autoplayCard(CardInstance& c, bool forceExhaust) {
    const CardDef& d = c.def();
    if (battleOver) return true;
    if (cardHasFlag(c, CF_UNPLAYABLE)) { discardPile.push_back(c); return true; }
    const int savedPlayIdx = currentPlayUniqueIdx;
    currentPlayUniqueIdx = c.uniqueIdx; // CURL_UP: AfterDamageReceived marks the card source
    const bool savedTainted = lastPlayedTainted;
    lastPlayedTainted = (c.affliction == 3);
    afterimageAtPlayStart = powerAmount(player.powers, PowerId::AFTERIMAGE);
    serpentFormAtPlayStart = powerAmount(player.powers, PowerId::SERPENT_FORM);
    stormAtPlayStart = powerAmount(player.powers, PowerId::STORM);
    // Nested autoplays must not leak their per-play relic flags into the parent card.
    struct PlayFlagGuard {
        BattleContext* bc;
        bool strike, upgraded, enchanted, penNib;
        explicit PlayFlagGuard(BattleContext* b)
            : bc(b), strike(b->currentCardStrike), upgraded(b->currentCardUpgraded),
              enchanted(b->currentCardEnchanted), penNib(b->penNibDoubleThisPlay) {}
        ~PlayFlagGuard() {
            bc->currentCardStrike = strike;
            bc->currentCardUpgraded = upgraded;
            bc->currentCardEnchanted = enchanted;
            bc->penNibDoubleThisPlay = penNib;
        }
    } playFlagGuard(this);
    lastPlayedWasShiv = d.hasTag(CT_SHIV);
    lastPlayedWasDefend = d.hasTag(CT_DEFEND);
    currentCardIsSovereignBlade = (d.id == CardId::SOVEREIGN_BLADE);
    if (d.id == CardId::BOLAS || d.id == CardId::THRUMMING_HATCHET)
        bolasPlayedThisTurn.push_back(c.uniqueIdx); // this instance returns next turn
    // Per-play relic flags (BeforeCardPlayed). Pen Nib counts attack cards and doubles
    // the 10th one; the counter persists across combats (RunState.penNibAttacks).
    currentCardStrike = d.hasTag(CT_STRIKE);
    currentCardUpgraded = c.upgrade > 0;
    currentCardEnchanted = c.enchant != 0;
    penNibDoubleThisPlay = false;
    if (d.type == CardType::ATTACK && hasRelic(RelicId::PEN_NIB)) {
        relicPenNibAttacks = (relicPenNibAttacks + 1) % 10;
        penNibDoubleThisPlay = (relicPenNibAttacks == 0);
    }

    stranglerAmountAtPlayStart.clear();
    for (const auto& em : enemies)
        stranglerAmountAtPlayStart.push_back(powerAmount(em.powers, PowerId::STRANGLE));
    // OblivionPower.BeforeCardPlayed: snapshot the amount in force now; AfterCardPlayed
    // applies that many Doom stacks to the power's owner (the enemy). The card that
    // applies the power is skipped because the power did not exist at its play start.
    oblivionAmountAtPlayStart.clear();
    for (const auto& em : enemies)
        oblivionAmountAtPlayStart.push_back(powerAmount(em.powers, PowerId::OBLIVION));
    // stamp history: auto plays are real play starts; Resources carries the card's
    // nominal cost (corpus HELLRAISER: auto Strike with cost 1 did NOT count as a
    // zero-cost attack start)
    if (d.cost >= 0) recordCardPlayStart(c, c.energyCost(*this));
    else {
        echoReplayThisPlay = powerAmount(player.powers, PowerId::ECHO_FORM) > cardPlayStarts;
        recordCardPlayStart(c, 0);
    }
    int target = -1;
    if (d.target == CardTarget::ANY_ENEMY || d.target == CardTarget::ANY_ALLY) {
        std::vector<int> hittable;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
        if (hittable.empty()) { discardPile.push_back(c); return true; }
        Sts2Rng& r = rng[(int)RngStream::CombatTargets];
        target = hittable[r.NextInt((int)hittable.size())];
    }
    if (!executeCardSteps(c, target)) { unsupportedCard = d.name; return false; }
    for (int rep = 0; rep < c.extraReplays && !battleOver; ++rep) {
        if (!executeCardSteps(c, target)) { unsupportedCard = d.name; return false; }
    }
    // Spiral: the enchanted Basic Strike/Defend plays one extra time.
    if (c.enchant == static_cast<int>(EnchantmentId::SPIRAL) && !battleOver) {
        ++spiralReplays;
        if (!executeCardSteps(c, target)) { unsupportedCard = d.name; return false; }
    }
    if (echoReplayThisPlay && !battleOver) {
        if (!executeCardSteps(c, target)) { unsupportedCard = d.name; return false; }
    }
    // OneTwoPunch also doubles auto-played attacks (ModifyCardPlayCount is global)
    if (d.type == CardType::ATTACK && !battleOver) {
        if (powerAmount(player.powers, PowerId::ONE_TWO_PUNCH) > 0) {
            if (!executeCardSteps(c, target)) { unsupportedCard = d.name; return false; }
            for (auto it = player.powers.begin(); it != player.powers.end();) {
                if (it->id == PowerId::ONE_TWO_PUNCH) {
                    if (--it->amount <= 0) it = player.powers.erase(it);
                    else ++it;
                } else ++it;
            }
        }
    }
    // MasterPlannerPower.AfterCardPlayed: every Skill the owner plays gains Sly
    if (d.type == CardType::SKILL && powerAmount(player.powers, PowerId::MASTER_PLANNER) > 0)
        applySlyKeyword(c);
    // RazorTooth.AfterCardPlayed: an upgradable Attack/Skill upgrades as it leaves play.
    if (hasRelic(RelicId::RAZOR_TOOTH)
        && (d.type == CardType::ATTACK || d.type == CardType::SKILL)
        && c.upgrade < d.maxUpgradeLevel)
        upgradeCardInstance(c);
        if (effectiveCardType(c) == CardType::POWER) { /* gone */ }
    else if (forceExhaust || c.exhaust || cardHasFlag(c, CF_EXHAUST)) {
        // SoulsPower removes the Exhaust keyword: the card discards as usual.
        if (!forceExhaust && c.enchant == static_cast<int>(EnchantmentId::SOULS_POWER)) {
            discardPile.push_back(c);
            ++soulsPowerKept;
        } else {
            exhaustCard(c);
        }
    }
    else if (effectiveCardType(c) == CardType::SKILL
             && powerAmount(player.powers, PowerId::CORRUPTION) > 0) {
        exhaustCard(c); // Corruption routes skills to exhaust
    }
    else if (d.id == CardId::SHINING_STRIKE) {
        drawPile.insert(drawPile.begin(), c); // GetResultLocationForCardPlay: draw top
    }
    else if (d.id == CardId::PARTICLE_WALL) {
        if ((int)hand.size() < maxHandSize) hand.push_back(c); // back to hand
        else discardPile.push_back(c);
    }
    else discardPile.push_back(c);
    lastPlayedWasAttack = (d.type == CardType::ATTACK);
    lastPlayedWasPower = (d.type == CardType::POWER);
    lastPlayedCardWasSkill = (d.type == CardType::SKILL);
    lastPlayedWasShiv = d.hasTag(CT_SHIV);
    lastPlayedWasDefend = d.hasTag(CT_DEFEND); // Accuracy/PhantomBlades (CardTag.Shiv)
    ++totalPlaysFinished; // GoldAxe: CardPlaysFinished
    ++playsFinishedThisTurn; // PhantomBlades HappenedThisTurn window
    if (lastPlayedWasAttack) {
        ++attacksFinishedThisTurn;
        relicAoWThisTurn = true; // ArtOfWar.AfterCardPlayed
    }
    if (lastPlayedWasShiv) ++shivPlaysFinishedThisTurn;
    if (d.type == CardType::SKILL) ++skillPlaysFinishedThisTurn; // LunarBlast
    // BurstPower: the next Skill after this play executes one extra time (ModifyCardPlayCount
    // adds 1 and the power then decrements — modelled in playCard, see burstPending)
    afterPlayerCardPlayedHooks();
    currentPlayUniqueIdx = savedPlayIdx;
    lastPlayedTainted = savedTainted;
    return true;
}

void BattleContext::afterPlayerCardPlayedHooks() {
    // TenderPower.AfterCardPlayed: each card the tender owner plays drains 1 Strength and
    // 1 Dexterity; AfterSideTurnEnd restores the drained total (see endTurn).
    {
        int tender = 0;
        for (auto& p : player.powers)
            if (p.id == PowerId::TENDER) {
                ++tender;
                p.misc = (p.misc < 0 ? 0 : p.misc) + 1;
            }
        if (tender > 0) {
            addPower(player.powers, PowerId::STRENGTH, -1, false);
            addPower(player.powers, PowerId::DEXTERITY, -1, false);
        }
    }
    // CurlUpPower.AfterCardPlayed: the card that dealt powered damage to the owner pays
    // out the stored block (ValueProp.Unpowered) and the power leaves.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        for (auto it = m.powers.begin(); it != m.powers.end();) {
            if (it->id == PowerId::CURL_UP && m.curlUpMarked >= 0
                && m.curlUpMarked == currentPlayUniqueIdx) {
                m.block += it->amount;
                m.curlUpMarked = -1;
                it = m.powers.erase(it);
            } else {
                ++it;
            }
        }
    }
    // VitalSparkPower.AfterCardPlayed: playing a Tainted card gives the player TaintedPower
    // equal to the VitalSpark amount (TaintedPower.ModifyDamageAdditive then boosts every
    // enemy powered attack until the enemy side ends).
    if (lastPlayedTainted) {
        int spark = 0;
        for (const auto& e : enemies)
            if (!e.removed && e.curHp > 0) {
                const int v = powerAmount(e.powers, PowerId::VITAL_SPARK);
                if (v > spark) spark = v;
            }
        if (spark > 0) addPower(player.powers, PowerId::TAINTED, spark, false);
    }
    // SlowPower.AfterCardPlayed: every card play raises the owner's per-turn counter.
    for (auto& m : enemies)
        for (auto& p : m.powers)
            if (p.id == PowerId::SLOW) p.misc = (p.misc < 0 ? 0 : p.misc) + 1;
    // RagePower: owner plays an attack -> gain Amount block (unpowered; Juggernaut fires)
    if (lastPlayedWasAttack)
        if (int amt = powerAmount(player.powers, PowerId::RAGE)) gainBlockUnpowered(amt);
    // ---- relic AfterCardPlayed hooks ----
    if (lastPlayedWasAttack) {
        // Kunai / Shuriken / Ornamental Fan / Kusarigama: every 3rd attack card in a turn.
        ++relicAttacksThisTurn;
        if (relicAttacksThisTurn % 3 == 0) {
            if (hasRelic(RelicId::KUNAI))
                addPower(player.powers, PowerId::DEXTERITY, 1, false);
            if (hasRelic(RelicId::SHURIKEN))
                addPower(player.powers, PowerId::STRENGTH, 1, false);
            if (hasRelic(RelicId::ORNAMENTAL_FAN)) gainBlockUnpowered(4);
            if (hasRelic(RelicId::KUSARIGAMA)) {
                std::vector<int> hit;
                for (int i = 0; i < (int)enemies.size(); ++i)
                    if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                        hit.push_back(i);
                if (!hit.empty()) {
                    const int t = hit[rng[(int)RngStream::CombatTargets].NextInt((int)hit.size())];
                    damageEnemy(t, 6, /*powered=*/false);
                }
            }
        }
        // Nunchaku: every 10th attack card gains 1 energy (counter persists).
        if (hasRelic(RelicId::NUNCHAKU)) {
            relicNunchakuAttacks = (relicNunchakuAttacks + 1) % 10;
            if (relicNunchakuAttacks == 0) player.energy += 1;
        }
    }
    // GamePiece.AfterCardPlayed: every Power draws 1.
    if (lastPlayedWasPower && hasRelic(RelicId::GAME_PIECE))
        drawCards(1, /*naturalDraw=*/false);
    // Permafrost.AfterCardPlayed: the first Power each combat gains 7 unpowered block.
    if (lastPlayedWasPower && hasRelic(RelicId::PERMAFROST) && !relicPermafrostUsed) {
        relicPermafrostUsed = true;
        gainBlockUnpowered(7);
    }
    // MummifiedHand.AfterCardPlayed: after a Power, one random hand card is free this
    // turn (prefer cards that currently cost energy or stars; CombatCardSelection pick).
    if (lastPlayedWasPower && hasRelic(RelicId::MUMMIFIED_HAND) && !hand.empty()) {
        std::vector<int> cands;
        for (int i = 0; i < (int)hand.size(); ++i)
            if (hand[i].energyCost(*this) > 0 || hand[i].starCost_() > 0) cands.push_back(i);
        if (cands.empty())
            for (int i = 0; i < (int)hand.size(); ++i) cands.push_back(i);
        Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
        hand[cands[r.NextInt((int)cands.size())]].freeThisTurn = true;
    }
    // JossPaper: threshold draws resolve after the card finishes (exhaust loops inside
    // card effects must not draw into a hand they are iterating).
    if (hasRelic(RelicId::JOSS_PAPER) && relicJossExhausted >= 5) {
        drawCards(relicJossExhausted / 5, /*naturalDraw=*/false);
        relicJossExhausted %= 5;
    }
    // RainbowRing.AfterCardPlayed: once per turn, an Attack + Skill + Power play grants
    // +1 Strength and +1 Dexterity.
    if (hasRelic(RelicId::RAINBOW_RING) && relicRainbowActivated < 1) {
        if (lastPlayedWasAttack) ++relicRainbowAttacks;
        if (lastPlayedCardWasSkill) ++relicRainbowSkills;
        if (lastPlayedWasPower) ++relicRainbowPowers;
        if (relicRainbowAttacks > 0 && relicRainbowSkills > 0 && relicRainbowPowers > 0) {
            addPower(player.powers, PowerId::STRENGTH, 1, false);
            addPower(player.powers, PowerId::DEXTERITY, 1, false);
            ++relicRainbowActivated;
        }
    }
    // UnsettlingLamp.AfterCardPlayed: the doubling window closes with its card.
    if (hasRelic(RelicId::UNSETTLING_LAMP) && !relicLampFinished
        && relicLampCardIdx == currentPlayUniqueIdx) {
        relicLampFinished = true;
        relicLampCardIdx = -1;
    }
    // Pocketwatch.AfterCardPlayed: every card play counts toward the next draw.
    ++relicPocketwatchThisTurn;
    // Vambrace.AfterCardPlayed: the triggering card's block-gain window closes.
    if (hasRelic(RelicId::VAMBRACE) && relicVambraceCardIdx >= 0
        && relicVambraceCardIdx == currentPlayUniqueIdx) {
        relicVambraceUsed = true;
        relicVambraceCardIdx = -1;
    }
    // TuningFork.AfterCardPlayed: every 10 Skill cards gain 7 block, subtract the threshold.
    if (lastPlayedCardWasSkill && hasRelic(RelicId::TUNING_FORK)) {
        ++relicTuningForkSkills;
        if (relicTuningForkSkills >= 10) {
            gainBlockUnpowered(7);
            relicTuningForkSkills -= 10;
        }
    }
    // Letter Opener: every 3rd Skill card in a player turn deals 5 unpowered damage to all.
    if (lastPlayedCardWasSkill && hasRelic(RelicId::LETTER_OPENER)) {
        ++relicSkillsThisTurn;
        if (relicSkillsThisTurn % 3 == 0) {
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                    damageEnemy(i, 5, /*powered=*/false);
        }
    }
    syncRedSkull();
    // CalamityPower.AfterCardPlayed: after an ATTACK is played, create Amount cards from the
    // owner's combat pool restricted to Attack cards (CardFactory.GetForCombat over the
    // character pool filtered to Type==Attack, then the usual combat-gen filter), each
    // generated on the CombatCardGeneration stream and added to the hand.
    if (lastPlayedWasAttack)
        if (int amt = powerAmount(player.powers, PowerId::CALAMITY)) {
            int pn = 0;
            const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
            if (pool && pn > 0) {
                std::vector<CardId> attacks;
                for (int i = 0; i < pn; ++i)
                    if (BattleContext::cardDef(pool[i]).type == CardType::ATTACK) attacks.push_back(pool[i]);
                if (!attacks.empty()) {
                    Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
                    for (int i = 0; i < amt && !battleOver; ++i) {
                        CardInstance nc;
                        nc.id = attacks[r.NextInt((int)attacks.size())];
                        nc.uniqueIdx = ++nextUniqueIdx;
                        nc.genCard = true;
                        ++cardsGeneratedByOwner;
                        if ((int)hand.size() < maxHandSize) hand.push_back(nc);
                        else discardPile.push_back(nc); // CardPileCmd.Add: full hand -> discard
                    }
                }
            }
        }
    // PanachePower (Instanced).AfterCardPlayed: cards played after the one that applied it
    // decrement that instance's CardsLeft; at 0 it deals its Amount to all enemies and the
    // counter resets to 5. The applying card itself does not count (Data.alreadyApplied),
    // so instances created by THIS play are skipped; instances are marked seen below.
    for (auto& pw : player.powers) {
        if (pw.id != PowerId::PANACHE) continue;
        if (pw.aux < 0) { pw.aux = 5; continue; } // just applied by this play: no tick yet
        if (--pw.aux <= 0) {
            if (pw.amount > 0) {
                for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                    if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                    damageEnemy(i, pw.amount, /*powered=*/false);
                }
            }
            pw.aux = 5;
        }
    }
    // StranglePower.AfterCardPlayed: each card the applier plays makes the strangled enemy
    // take its own Amount as Unblockable|Unpowered damage. The amount is captured at
    // card-play START (BeforeCardPlayed dictionary) so a triggered-by-this-card stack
    // does not fire from the Strangle card itself, and the stacks in force when the play
    // began are the ones that hit.
    for (size_t i = 0; i < enemies.size(); ++i) {
        Monster& m = enemies[i];
        if (m.removed || m.curHp <= 0 || m.reviving) continue;
        // Each Strangle INSTANCE fires its own amount (InstancedPerApplier): with two
        // stacks applied, one card play produces two separate damage events.
        // The snapshot taken at play start holds the per-instance amounts in force then.
        int total = (i < stranglerAmountAtPlayStart.size()) ? stranglerAmountAtPlayStart[i] : 0;
        if (total > 0) {
            m.curHp -= total; // unblockable: block is not consulted
            if (m.curHp <= 0) {
                m.curHp = 0;
                onEnemyDeath((int)i);
                if (battleOver) return;
            }
        }
    }
    // OblivionPower.AfterCardPlayed: after each owner card play, the power's owner (the
    // enemy) gains Doom equal to the Oblivion stacks that were in force when the play
    // STARTED. That dictionary is what keeps the Oblivion card itself from triggering:
    // its own play began before the power existed, so the snapshot is 0 there.
    for (size_t i = 0; i < enemies.size() && !battleOver; ++i) {
        Monster& m = enemies[i];
        if (m.removed || m.curHp <= 0 || m.reviving) continue;
        int doom = (i < oblivionAmountAtPlayStart.size()) ? oblivionAmountAtPlayStart[i] : 0;
        if (doom > 0) addPower(m.powers, PowerId::DOOM, doom, false);
    }
    // StormPower.AfterCardPlayed: playing a POWER card channels Amount Lightning orbs,
    // where Amount is the stack count captured at card-play START (so Storm does not
    // trigger on itself and repeated Storm plays use the stacks that were live).
    if (lastPlayedWasPower && stormAtPlayStart > 0) {
        for (int i = 0; i < stormAtPlayStart && !battleOver; ++i) channelOrb(OrbKind::LIGHTNING);
    }
    // AfterimagePower.AfterCardPlayed: any owner card -> gain block equal to the amount
    // captured at card-play start. Plays that started before the power was applied
    // (e.g. the After Image card itself) do not trigger (BeforeCardPlayed dictionary).
    if (afterimageAtPlayStart > 0) gainBlockUnpowered(afterimageAtPlayStart);
    // SerpentFormPower.AfterCardPlayed: unpowered damage to a random hittable enemy
    if (serpentFormAtPlayStart > 0) {
        std::vector<int> hittable;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
        if (!hittable.empty()) {
            Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
            int t = hittable[rt.NextInt((int)hittable.size())];
            damageEnemy(t, serpentFormAtPlayStart, /*powered=*/false);
        }
    }
    // EnvenomPower.AfterDamageGiven: powered attacks that deal unblocked damage stack Poison;
    // that resolution lives in damageEnemy (it knows the unblocked amount).
    // EnragePower.AfterCardPlayed (TestSubject): every Skill the owner's player plays gives
    // the owner Amount Strength.
    for (auto& m : enemies) {
        if (m.curHp <= 0) continue;
        if (int en = powerAmount(m.powers, PowerId::ENRAGE)) {
            if (lastPlayedCardWasSkill)
                applyPowerToMonsterSelf(m, PowerId::STRENGTH, en);
        }
    }
    // monster AfterCardPlayed hooks: WitheringPresence (cards-played counter on AEONGLASS)
    for (auto& m : enemies) {
        if (m.curHp <= 0) continue;
        for (auto& p : m.powers) {
            if (p.id == PowerId::WITHERING_PRESENCE) {
                if (p.misc < 0) p.misc = p.amount;
                p.misc -= 1;
                if (p.misc <= 0) {
                    if ((int)hand.size() < maxHandSize) {
                        hand.insert(hand.begin(), makeWither(m.addStrength));
                        std::stable_sort(hand.begin(), hand.end(), [](const CardInstance& a, const CardInstance& b) {
                            return a.id == CardId::WITHER && b.id != CardId::WITHER;
                        });
                    }
                    p.misc = p.amount;
                }
            }
        }
    }
    if (battleOver) checkBattleOver();
}

// Potion targeting class (PotionModel.TargetType): 0 self, 1 single enemy, 2 all enemies.
int potionTargetKind(int potionId) {
    switch (static_cast<PotionId>(potionId)) {
        case PotionId::FIRE_POTION:
        case PotionId::WEAK_POTION:
        case PotionId::VULNERABLE_POTION:
            return 1;
        case PotionId::EXPLOSIVE_AMPOULE:
            return 2;
        default:
            return 0;
    }
}

// Drink one potion. Values match the decompiled PotionModel canonical vars.
bool BattleContext::usePotion(int slot, int targetIdx) {
    if (battleOver || slot < 0 || slot >= 3) return false;
    const int pid = player.potions[slot];
    // (Belt Buckle re-application is handled after the belt slot clears below.)
    if (pid == 0) return false;
    player.potions[slot] = 0;
    // BeltBuckle.AfterPotionUsed: the last potion leaving an empty belt grants +2 Dex.
    if (hasRelic(RelicId::BELT_BUCKLE) && !relicBeltBuckleApplied) {
        bool empty = true;
        for (int p : player.potions) if (p != 0) empty = false;
        if (empty) {
            addPower(player.powers, PowerId::DEXTERITY, 2, false);
            relicBeltBuckleApplied = true;
        }
    }
    switch (static_cast<PotionId>(pid)) {
        case PotionId::BLOCK_POTION:
            gainBlockUnpowered(12); // BlockVar(12, Unpowered)
            break;
        case PotionId::ENERGY_POTION:
            player.energy += 2;
            break;
        case PotionId::STRENGTH_POTION:
            applyPowerToPlayer(PowerId::STRENGTH, 2);
            break;
        case PotionId::DEXTERITY_POTION:
            applyPowerToPlayer(PowerId::DEXTERITY, 2);
            break;
        case PotionId::LIQUID_BRONZE:
            applyPowerToPlayer(PowerId::THORNS, 3);
            break;
        case PotionId::BLOOD_POTION:
            player.curHp = std::min(player.maxHp, player.curHp + player.maxHp * 20 / 100);
            break;
        case PotionId::FRUIT_JUICE:
            player.maxHp += 5;
            player.curHp += 5;
            break;
        case PotionId::SWIFT_POTION:
            drawCards(3);
            break;
        case PotionId::FIRE_POTION:
            if (targetIdx >= 0 && targetIdx < (int)enemies.size())
                damageEnemy(targetIdx, 20, /*powered=*/false);
            break;
        case PotionId::POTION_SHAPED_ROCK:
            // DamageVar(15, Unpowered), TargetType.AnyEnemy (Petrified Toad token).
            if (targetIdx >= 0 && targetIdx < (int)enemies.size())
                damageEnemy(targetIdx, 15, /*powered=*/false);
            break;
        case PotionId::WEAK_POTION:
            if (targetIdx >= 0 && targetIdx < (int)enemies.size())
                applyPowerToEnemy(targetIdx, PowerId::WEAK, 3);
            break;
        case PotionId::VULNERABLE_POTION:
            if (targetIdx >= 0 && targetIdx < (int)enemies.size())
                applyPowerToEnemy(targetIdx, PowerId::VULNERABLE, 3);
            break;
        case PotionId::EXPLOSIVE_AMPOULE:
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                    damageEnemy(i, 10, /*powered=*/false);
            break;
        default:
            break; // potion implemented as a no-op (recorded gap)
    }
    // ReptileTrinket.AfterPotionUsed: +3 temporary Strength (internal Strength + power).
    if (hasRelic(RelicId::REPTILE_TRINKET)) {
        addPower(player.powers, PowerId::REPTILE_TRINKET, 3, false);
        applyPowerToPlayer(PowerId::STRENGTH, 3);
    }
    if (battleOver) checkBattleOver();
    return true;
}

void BattleContext::playCard(int handIdx, int targetEnemyIdx) {
    CardInstance c = hand[handIdx];
    const CardDef& d = c.def();
    xValue = -1;
    const int savedPlayIdx = currentPlayUniqueIdx;
    currentPlayUniqueIdx = c.uniqueIdx; // CURL_UP: AfterDamageReceived marks the card source
    const bool savedTainted = lastPlayedTainted;
    lastPlayedTainted = (c.affliction == 3);
    // ChainsOfBindingPower.BeforeCardPlayed: the first Bound card played this turn flags
    // the counter (ShouldPlay then rejects every further Bound card).
    if (c.affliction == 4) ++boundCardsPlayedThisTurn;
    // GalvanicPower.AfterCardPlayed: playing a Galvanized card deals Amount unpowered
    // damage to its owner (GlobeHead's opening affliction).
    const int galvanicOnPlay = c.affliction == 6 ? c.afflictionAmount : 0;

    // DanseMacabrePower.BeforeCardPlayed: a card whose resolved cost is at least the
    // power's Energy dynvar (2) gives the owner Amount unpowered block.
    if (int dm = powerAmount(player.powers, PowerId::DANSE_MACABRE)) {
        if (c.energyCost(*this) >= 2) gainBlockUnpowered(dm);
    }
    // BeforeCardPlayed snapshot: Afterimage/SerpentForm capture their amount when the play
    // STARTED, so a card does not trigger on the power it is itself applying
    // (AfterImagePower/SerpentFormPower Data.amountsForPlayedCards).
    afterimageAtPlayStart = powerAmount(player.powers, PowerId::AFTERIMAGE);
    serpentFormAtPlayStart = powerAmount(player.powers, PowerId::SERPENT_FORM);
    stormAtPlayStart = powerAmount(player.powers, PowerId::STORM);
    lastPlayedWasShiv = d.hasTag(CT_SHIV);
    lastPlayedWasDefend = d.hasTag(CT_DEFEND); // cardSource tag for Shiv-damage modifiers
    currentCardIsSovereignBlade = (d.id == CardId::SOVEREIGN_BLADE); // ConquerorPower
    if (d.id == CardId::BOLAS || d.id == CardId::THRUMMING_HATCHET)
        bolasPlayedThisTurn.push_back(c.uniqueIdx); // this instance returns next turn
    // Per-play relic flags (BeforeCardPlayed). Pen Nib counts attack cards and doubles
    // the 10th one; the counter persists across combats (RunState.penNibAttacks).
    currentCardStrike = d.hasTag(CT_STRIKE);
    currentCardUpgraded = c.upgrade > 0;
    currentCardEnchanted = c.enchant != 0;
    penNibDoubleThisPlay = false;
    if (d.type == CardType::ATTACK && hasRelic(RelicId::PEN_NIB)) {
        relicPenNibAttacks = (relicPenNibAttacks + 1) % 10;
        penNibDoubleThisPlay = (relicPenNibAttacks == 0);
    }

    stranglerAmountAtPlayStart.clear();
    for (const auto& em : enemies)
        stranglerAmountAtPlayStart.push_back(powerAmount(em.powers, PowerId::STRANGLE));
    // OblivionPower.BeforeCardPlayed: snapshot the amount in force now; AfterCardPlayed
    // applies that many Doom stacks to the power's owner (the enemy). The card that
    // applies the power is skipped because the power did not exist at its play start.
    oblivionAmountAtPlayStart.clear();
    for (const auto& em : enemies)
        oblivionAmountAtPlayStart.push_back(powerAmount(em.powers, PowerId::OBLIVION));

    // IntimidatingHelmet.BeforeCardPlayed: a card whose resolved cost is >= 2 grants 4
    // unpowered block before it resolves.
    if (hasRelic(RelicId::INTIMIDATING_HELMET) && c.energyCost(*this) >= 2)
        gainBlockUnpowered(4);
    // stamp history: the play's resolved energy value at start (X = pre-payment energy;
    // FreeAttack/Corruption late modifiers included, power stacks NOT yet decremented)
    int energyVal = 0;
    if (d.hasFlag(CF_ENERGY_X)) energyVal = player.energy;
    else if (d.cost >= 0) energyVal = c.energyCost(*this);
    // EchoFormPower.ModifyCardPlayCount: the first Amount card plays each turn execute one
    // extra time (the replay itself is not a new CardPlaysStarted entry)
    echoReplayThisPlay = powerAmount(player.powers, PowerId::ECHO_FORM) > cardPlayStarts;
    recordCardPlayStart(c, energyVal);

    // FreeAttackPower: the next attack played while stacks remain costs 0 and
    // decrements the counter (BeforeCardPlayed fires before OnPlay)
    int freeAttack = 0;
    if (d.type == CardType::ATTACK)
        freeAttack = powerAmount(player.powers, PowerId::FREE_ATTACK);
    for (auto it = player.powers.begin(); it != player.powers.end();) {
        if (it->id == PowerId::FREE_ATTACK && freeAttack > 0) {
            if (--it->amount <= 0) it = player.powers.erase(it); // Decrement removes at 0
            else ++it;
        } else ++it;
    }

    // RandomEnemy targeting rolls ONE hittable enemy at play start — but only when
    // the effect program actually consumes a single target (Volley yes; per-hit
    // random families like SwordBoomerang roll per hit instead, no upfront draw)
    if (d.target == CardTarget::RANDOM_ENEMY
        && (targetEnemyIdx < 0 || targetEnemyIdx >= (int)enemies.size())) {
        bool needsTarget = false;
        for (int si2 = 0; si2 < (int)d.nSteps; ++si2) {
            switch (d.steps[si2].op) {
                case StepOp::ATTACK: case StepOp::ATTACK_X_HITS: case StepOp::ATTACK_HITS_CALC:
                case StepOp::ATTACK_CALC: case StepOp::ATTACK_FEED: case StepOp::ATTACK_BLOCK_AS_DAMAGE:
                case StepOp::VULN_DOUBLE: case StepOp::DOMINATE: case StepOp::POISON_IF_POISONED:
                case StepOp::EXPOSE: case StepOp::MALAISE_X:
                    needsTarget = true; break;
                default: break;
            }
            if (needsTarget) break;
        }
        if (needsTarget) {
            std::vector<int> hittable;
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
            if (!hittable.empty()) {
                Sts2Rng& r = rng[(int)RngStream::CombatTargets];
                targetEnemyIdx = hittable[r.NextInt((int)hittable.size())];
            }
        }
    }

    // pay resources: X-cost consumes all energy; star costs ride alongside energy.
    // ResolveEnergyXValue is captured BEFORE payment (Whirlwind corpus T4: the hit
    // count uses the pre-payment energy).
    int paidEnergy = 0;
    if (d.hasFlag(CF_ENERGY_X)) {
        // ChemicalX.ModifyXValue: X-cost cards resolve with +2.
        xValue = player.energy + (hasRelic(RelicId::CHEMICAL_X) ? 2 : 0);
        paidEnergy = xValue;
        player.energy = 0;
    } else {
        int cost = c.energyCost(*this);
        if (cost > 0 && d.cost >= 0) { player.energy -= cost; paidEnergy = cost; }
        int stars = c.starCost_();
        if (stars > 0) {
            // energy shortfall is paid in stars at 1 energy = 2 stars (T1 ruling)
            int energyDeficit = 0;
            if (player.energy < 0) { energyDeficit = -player.energy; player.energy = 0; }
            int starNeed = stars + 2 * energyDeficit;
            player.stars = std::max(0, player.stars - starNeed);
        }
    }

    // the card leaves the hand when the play begins: draw effects resolve against the
    // post-removal hand size (corpus OFFERING T1: the third draw would cap at 10 with
    // the played card still counted)
    hand.erase(hand.begin() + handIdx);

    // SurroundedPower.BeforeCardPlayed: a card played at a targeted enemy turns the player
    // toward that side (KaiserCrab arms; the OTHER arm then strikes from behind).
    if (targetEnemyIdx >= 0 && targetEnemyIdx < (int)enemies.size()) {
        const Monster& tgt = enemies[targetEnemyIdx];
        for (auto& p : player.powers)
            if (p.id == PowerId::SURROUNDED) {
                if (p.misc != 1 && powerAmount(tgt.powers, PowerId::BACK_ATTACK_LEFT) > 0)
                    p.misc = 1;
                else if (p.misc == 1 && powerAmount(tgt.powers, PowerId::BACK_ATTACK_RIGHT) > 0)
                    p.misc = 0;
                break;
            }
    }

    if (!executeCardSteps(c, targetEnemyIdx)) {
        unsupportedCard = d.name; // replay harness aborts the run with this
        return;
    }
    // Hidden Gem: BaseReplayCount 2 makes GeneratePlayCount = 3; every extra play runs
    // the same OnPlay program (the result location is applied once, after the loop).
    for (int rep = 0; rep < c.extraReplays && !battleOver; ++rep) {
        if (!executeCardSteps(c, targetEnemyIdx)) {
            unsupportedCard = d.name;
            return;
        }
    }
    // Spiral: the enchanted Basic Strike/Defend plays one extra time.
    if (c.enchant == static_cast<int>(EnchantmentId::SPIRAL) && !battleOver) {
        ++spiralReplays;
        if (!executeCardSteps(c, targetEnemyIdx)) {
            unsupportedCard = d.name;
            return;
        }
    }
    if (echoReplayThisPlay && !battleOver) {
        if (!executeCardSteps(c, targetEnemyIdx)) {
            unsupportedCard = d.name;
            return;
        }
    }

    // OneTwoPunchPower.ModifyCardPlayCount: the played Attack executes one extra time,
    // then the power decrements (AfterModifyingCardPlayCount, removes at 0)
    if (d.type == CardType::ATTACK && !battleOver) {
        if (powerAmount(player.powers, PowerId::ONE_TWO_PUNCH) > 0) {
            if (!executeCardSteps(c, targetEnemyIdx)) {
                unsupportedCard = d.name;
                return;
            }
            for (auto it = player.powers.begin(); it != player.powers.end();) {
                if (it->id == PowerId::ONE_TWO_PUNCH) {
                    if (--it->amount <= 0) it = player.powers.erase(it);
                    else ++it;
                } else ++it;
            }
        }
    }
    // VigorPower.BeforeAttack: consumed by the owner's first powered attack
    if (d.type == CardType::ATTACK) {
        for (auto it = player.powers.begin(); it != player.powers.end();) {
            if (it->id == PowerId::VIGOR) it = player.powers.erase(it);
            else ++it;
        }
    }
    // FreeSkillPower.BeforeCardPlayed: playing a Skill decrements it (one free skill per
    // stack; it has no side-end removal)
    if (d.type == CardType::SKILL && !battleOver) {
        for (auto it = player.powers.begin(); it != player.powers.end();) {
            if (it->id == PowerId::FREE_SKILL) {
                if (--it->amount <= 0) it = player.powers.erase(it);
                else ++it;
            } else ++it;
        }
    }
    // BurstPower.ModifyCardPlayCount: the played Skill executes one extra time, then the
    // power decrements (it also leaves at the owner's side end)
    if (d.type == CardType::SKILL && !battleOver) {
        if (powerAmount(player.powers, PowerId::BURST) > 0) {
            if (!executeCardSteps(c, targetEnemyIdx)) {
                unsupportedCard = d.name;
                return;
            }
            for (auto it = player.powers.begin(); it != player.powers.end();) {
                if (it->id == PowerId::BURST) {
                    if (--it->amount <= 0) it = player.powers.erase(it);
                    else ++it;
                } else ++it;
            }
        }
    }

    // MasterPlannerPower.AfterCardPlayed: every Skill the owner plays gains Sly
    if (d.type == CardType::SKILL && powerAmount(player.powers, PowerId::MASTER_PLANNER) > 0)
        applySlyKeyword(c);

    // OrbitPower.AfterEnergySpent (Instanced): every 4 energy spent grants Amount energy.
    // Runs AFTER the steps so an Orbit applied by this very play also counts its cost
    // (corpus ORBIT T1: one defend after the Orbit → 3 energy counted by T2 start).
    if (paidEnergy > 0 && !battleOver) {
        energySpentThisTurn += paidEnergy;
        for (auto& pw : player.powers) {
            if (pw.id != PowerId::ORBIT) continue;
            if (pw.skipEnergyTick) { pw.skipEnergyTick = false; continue; }
            pw.misc = (pw.misc < 0 ? 0 : pw.misc) + paidEnergy;
            int triggers = pw.misc / 4 - (pw.aux < 0 ? 0 : pw.aux);
            if (triggers > 0) {
                player.energy += pw.amount * triggers;
                pw.aux = (pw.aux < 0 ? 0 : pw.aux) + triggers;
            }
        }
    }

    // NostalgiaPower.ModifyCardPlayResultLocation: the first Amount Attack/Skill plays each
    // turn go to the TOP of the draw pile instead of the discard.
    const bool nostalgiaToDraw = powerAmount(player.powers, PowerId::NOSTALGIA) > 0
        && (d.type == CardType::ATTACK || d.type == CardType::SKILL)
        && attackSkillStarts <= powerAmount(player.powers, PowerId::NOSTALGIA);
    // route to the result pile. Powers leave the combat entirely when played (they are
    // not discarded nor exhausted — corpus AGGRESSION T3: 13-card deck shows 12 in piles
    // after the played power vanished).
    // RazorTooth.AfterCardPlayed: an upgradable Attack/Skill upgrades as it leaves play.
    if (hasRelic(RelicId::RAZOR_TOOTH)
        && (d.type == CardType::ATTACK || d.type == CardType::SKILL)
        && c.upgrade < d.maxUpgradeLevel)
        upgradeCardInstance(c);

    if (effectiveCardType(c) == CardType::POWER) {
        // gone
    } else if (c.exhaust || cardHasFlag(c, CF_EXHAUST)) {
        if (c.enchant == static_cast<int>(EnchantmentId::SOULS_POWER)) {
            discardPile.push_back(c);
            ++soulsPowerKept;
        } else {
            exhaustCard(c);
        }
    } else if (effectiveCardType(c) == CardType::SKILL
               && powerAmount(player.powers, PowerId::CORRUPTION) > 0) {
        // CorruptionPower.ModifyCardPlayResultLocation: skills exhaust instead of discard
        exhaustCard(c);
    } else if (nostalgiaToDraw) {
        drawPile.insert(drawPile.begin(), c); // draw pile top = front
    } else if (d.id == CardId::SHINING_STRIKE) {
        // ShiningStrike.GetResultLocationForCardPlay: instead of the discard, the played
        // card returns to the TOP of the draw pile.
        drawPile.insert(drawPile.begin(), c);
    } else if (d.id == CardId::PARTICLE_WALL) {
        // ParticleWall.GetResultLocationForCardPlay: the played card returns to the hand.
        if ((int)hand.size() < maxHandSize) hand.push_back(c);
        else discardPile.push_back(c);
    } else {
        discardPile.push_back(c);
    }
    lastPlayedWasAttack = (d.type == CardType::ATTACK);
    lastPlayedWasPower = (d.type == CardType::POWER);
    lastPlayedCardWasSkill = (d.type == CardType::SKILL);
    lastPlayedWasShiv = d.hasTag(CT_SHIV);
    lastPlayedWasDefend = d.hasTag(CT_DEFEND);
    ++totalPlaysFinished; // GoldAxe: CardPlaysFinished
    ++playsFinishedThisTurn;
    if (lastPlayedWasAttack) {
        ++attacksFinishedThisTurn;
        relicAoWThisTurn = true; // ArtOfWar.AfterCardPlayed
    }
    if (lastPlayedWasShiv) ++shivPlaysFinishedThisTurn;
    if (d.type == CardType::SKILL) ++skillPlaysFinishedThisTurn; // LunarBlast
    // GalvanicPower.AfterCardPlayed: a Galvanized card that was played deals its Amount as
    // unpowered (blockable) damage to the player.
    if (galvanicOnPlay > 0) damagePlayer(galvanicOnPlay, /*poweredAttack=*/false);
    afterPlayerCardPlayedHooks();
    currentPlayUniqueIdx = savedPlayIdx;
    lastPlayedTainted = savedTainted;
}

// ---------------- turn loop ----------------

void BattleContext::endTurn() {
    if (battleOver) return;

    // StoneCalendar BeforeSideTurnEnd: at the end of player turn 7 deal 52 unpowered
    // damage to every hittable enemy.
    if (turn == 7 && hasRelic(RelicId::STONE_CALENDAR)) {
        for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                damageEnemy(i, 52, /*powered=*/false);
        if (battleOver) return;
    }

    // ---- BeforeSideTurnEndEarly: Plating gains block before end-of-turn damage effects ----
    if (int amt = powerAmount(player.powers, PowerId::PLATING)) gainBlockUnpowered(amt);

    // ---- BeforeSideTurnEnd powers ----
    // TheBombPower (Instanced).BeforeSideTurnEnd: Amount counts down; at 1 it deals its
    // Damage var to all enemies and removes itself.
    // Index-based: damageEnemy may apply powers to the player, so no iterator may be
    // held across the detonation (ASAN caught a stale iterator here in searcher playouts).
    for (int pi = 0; pi < (int)player.powers.size();) {
        if (player.powers[pi].id == PowerId::THE_BOMB) {
            if (player.powers[pi].amount > 1) { --player.powers[pi].amount; ++pi; continue; }
            const int dmg = player.powers[pi].aux >= 0 ? player.powers[pi].aux : 40;
            player.powers.erase(player.powers.begin() + pi);
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                damageEnemy(i, dmg, /*powered=*/false);
            }
            continue;
        }
        if (player.powers[pi].id == PowerId::PANACHE) player.powers[pi].aux = 5;
        ++pi;
    }
    if (battleOver) return;

    // OrichalcumPower-like relic: BeforeSideTurnEnd(participants.Contains(owner)) — if the
    // player still has no block when their side ends, gain the relic's block (unpowered).
    if (relicEndTurnBlock > 0 && player.block == 0) gainBlockUnpowered(relicEndTurnBlock);
    // RippleBasin / CloakClasp / ScreamingFlagon BeforeSideTurnEnd (relic hooks).
    if (hasRelic(RelicId::RIPPLE_BASIN) && attackStartsThisTurn == 0) gainBlockUnpowered(4);
    if (hasRelic(RelicId::CLOAK_CLASP) && !hand.empty())
        gainBlockUnpowered(static_cast<int>(hand.size()));
    if (hasRelic(RelicId::SCREAMING_FLAGON) && hand.empty()) {
        for (int i = 0; i < (int)enemies.size() && !battleOver; ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                damageEnemy(i, 20, /*powered=*/false);
    }
    if (battleOver) return;

    // ---- Orb queue BeforeTurnEnd (CombatManager.DoTurnEnd runs it before the
    // TurnInHand card effects): Lightning/Frost/Dark/Glass passives ----
    orbBeforeTurnEndTriggers();
    if (battleOver) return;

    // ---- end-of-turn-in-hand effects (OnTurnEndInHand family) ----
    // DoTurnEnd moves every turn-end card to the Play pile, resolves it, then adds it to
    // the DISCARD bottom — before FlushPlayerHand. That ordering is visible: the four
    // Infections of a hand flush land before the normal cards (corpus PHROG_PARASITE T9).
    {
        std::vector<CardInstance> turnEndCards, restHand;
        for (auto& c : hand) {
            if (cardHasFlag(c, CF_TURN_END_IN_HAND)) turnEndCards.push_back(c);
            else restHand.push_back(c);
        }
        hand.swap(restHand);
        for (auto& c : turnEndCards) {
            const CardDef& d = c.def();
            if (d.id == CardId::WITHER) {
                int dmg = cardVarValue(c, d, 0) + 3 * c.fakeUpgrade;
                damagePlayer(dmg); // regular damage: block absorbs first
            } else if (d.id == CardId::BURN) {
                int dmg = cardVarValue(c, d, 0);
                damagePlayer(dmg);
            } else if (d.id == CardId::BECKON || d.id == CardId::BAD_LUCK) {
                damagePlayerUnblockable(cardVarValue(c, d, 0)); // Unblockable self damage
            } else if (d.id == CardId::INFECTION || d.id == CardId::TOXIC
                    || d.id == CardId::DECAY) {
                damagePlayer(cardVarValue(c, d, 0)); // Unpowered self damage (blockable)
            } else if (d.id == CardId::REGRET) {
                damagePlayerUnblockable((int)hand.size()); // damage = cards in hand now
            } else if (d.id == CardId::DEBT) {
                player.gold = std::max(0, player.gold - cardVarValue(c, d, 0));
            } else if (d.id == CardId::DOUBT) {
                applyPowerToPlayer(PowerId::WEAK, cardVarValue(c, d, 0));
            } else if (d.id == CardId::SHAME) {
                applyPowerToPlayer(PowerId::FRAIL, cardVarValue(c, d, 0));
            }
            // other turn-end-in-hand cards: unimplemented (harness reports on play/stamp)
            c.stampText.clear(); // the card moved between piles
            discardPile.push_back(c);
            if (battleOver) return;
        }
    }

    // ---- auto post-play phase (AfterAutoPostPlayPhaseEntered) ----
    // IAmInvincible.AfterAutoPostPlayPhaseEntered: when this card sits on top of the draw
    // pile, it auto-plays itself from there (free, no forced exhaust) — its block lands
    // before the enemy phase (corpus I_AM_INVINCIBLE: hp stays at 75 with a 13 block).
    // Every IAmInvincible instance whose own check finds itself on top fires in the same
    // hook pass, so consecutive IIV copies at the top all auto-play (corpus: two copies).
    while (!drawPile.empty() && drawPile.front().id == CardId::I_AM_INVINCIBLE && !battleOver) {
        CardInstance top = drawPile.front();
        drawPile.erase(drawPile.begin());
        if (!autoplayCard(top)) return;
    }
    // StampedePower: play N random playable attacks from hand via the Shuffle stream
    if (int amt = powerAmount(player.powers, PowerId::STAMPEDE)) {
        for (int i = 0; i < amt && !battleOver; ++i) {
            std::vector<int> items;
            for (int j = 0; j < (int)hand.size(); ++j)
                if (hand[j].def().type == CardType::ATTACK && !cardHasFlag(hand[j], CF_UNPLAYABLE))
                    items.push_back(j);
            if (items.empty()) break;
            Sts2Rng& r = rng[(int)RngStream::Shuffle];
            int pick = items[r.NextInt((int)items.size())];
            CardInstance c = hand[pick];
            hand.erase(hand.begin() + pick);
            if (!autoplayCard(c)) return;
        }
    }

    // ---- flush hand ----
    // side-turn-end power removals at the owner's side end: NoDraw/Rage clear;
    // SetupStrike (TemporaryStrength) restores its strength then leaves; OneTwoPunch
    // expires unused (PowerCmd.Remove); Synchronize (TemporaryFocus) retires its Focus
    {
        // DoubleDamagePower.AfterSideTurnEnd: one stack burns per turn (not per attack)
        for (auto it = player.powers.begin(); it != player.powers.end();) {
            if (it->id == PowerId::DOUBLE_DAMAGE) {
                if (--it->amount <= 0) it = player.powers.erase(it);
                else ++it;
            } else ++it;
        }
        int setupRestore = 0;
        for (const auto& p : player.powers)
            if (p.id == PowerId::SETUP_STRIKE) setupRestore += p.amount;
        if (setupRestore) addPower(player.powers, PowerId::STRENGTH, -setupRestore, false);
        // TenderPower.AfterSideTurnEnd(side == Player): the Strength/Dexterity drained this
        // turn comes back and the per-turn counter resets (the power itself stays).
        {
            int tenderRestore = 0;
            for (auto& p : player.powers)
                if (p.id == PowerId::TENDER && p.misc > 0) {
                    tenderRestore += p.misc;
                    p.misc = 0;
                }
            if (tenderRestore) {
                addPower(player.powers, PowerId::STRENGTH, tenderRestore, false);
                addPower(player.powers, PowerId::DEXTERITY, tenderRestore, false);
            }
        }
        int syncRestore = 0;
        for (const auto& p : player.powers)
            if (p.id == PowerId::SYNCHRONIZE || p.id == PowerId::FOCUSED_STRIKE
                || p.id == PowerId::HOTFIX)
                syncRestore += p.amount;
        if (syncRestore) addPower(player.powers, PowerId::FOCUS, -syncRestore, false);
        // Anticipate is a TemporaryDexterityPower: restores its dexterity then leaves
        int anticipRestore = 0;
        for (const auto& p : player.powers)
            if (p.id == PowerId::ANTICIPATE) anticipRestore += p.amount;
        if (anticipRestore) addPower(player.powers, PowerId::DEXTERITY, -anticipRestore, false);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) {
                return p.id == PowerId::NO_DRAW || p.id == PowerId::RAGE
                    || p.id == PowerId::SETUP_STRIKE || p.id == PowerId::ONE_TWO_PUNCH
                    || p.id == PowerId::SYNCHRONIZE || p.id == PowerId::FOCUSED_STRIKE
                    || p.id == PowerId::HOTFIX || p.id == PowerId::ANTICIPATE
                    || p.id == PowerId::SHADOWMELD || p.id == PowerId::BURST
                    || p.id == PowerId::CORROSIVE_WAVE || p.id == PowerId::BORROWED_TIME;
            }), player.powers.end());
        // OblivionPower.AfterSideTurnEnd(side == Player): the debuff leaves at the end of
        // the side that applied it, whether or not the enemy acted.
        for (auto& m : enemies)
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::OBLIVION; }), m.powers.end());
    }
    // Withers resolve their TurnEndInHand hook individually and leave the hand FIRST
    // (corpus AEONGLASS s1002 T6: hand [STRIKE,STRIKE,WITHER] flushes as WITHER,STRIKE,
    // STRIKE); Ethereal cards (Dazed/Void/...) exhaust; Retain cards stay in hand
    // (CombatManager.FlushPlayerHand: cardsToRetain); the rest discard in hand order.
    {
        // RetainHandPower.ShouldFlush == false: with it active NOTHING is flushed this turn
        const bool retainAll = powerAmount(player.powers, PowerId::RETAIN_HAND) > 0;
        std::vector<CardInstance> rest, retained;
        // HexPower.TryModifyKeywordsInCombat grants Ethereal to every Hexed card while the
        // power is in force (the keyword is a runtime modifier, so the stamp's keywords=
        // list stays empty).
        const bool hexActive = powerAmount(player.powers, PowerId::HEX) > 0;
        for (auto& c : hand) {
            // DoTurnEnd exhausts EVERY Ethereal card in hand first — that happens before
            // FlushPlayerHand, so Ethereal wins over Retain (corpus CALL_OF_THE_VOID:
            // SPUR [Ethereal,Retain] is exhausted, not retained).
            if (cardHasFlag(c, CF_ETHEREAL) || c.ephemeral
                || (hexActive && c.affliction == 5)) exhaustCard(c, /*causedByEthereal=*/true);
            else if (retainAll || cardHasFlag(c, CF_RETAIN) || c.retain || c.retainThisTurn)
                retained.push_back(c);
            else if (c.id == CardId::WITHER) discardPile.push_back(c);
            else rest.push_back(c);
        }
        for (auto& c : rest) discardPile.push_back(c);
        hand = retained;
        // ApplySingleTurnRetain covers exactly one flush: consume the flag on the survivors.
        for (auto& c : hand) c.retainThisTurn = false;
    }

    // RetainHandPower.AfterSideTurnEnd: one stack leaves per side end (post-flush)
    for (auto it2 = player.powers.begin(); it2 != player.powers.end();) {
        if (it2->id == PowerId::RETAIN_HAND) {
            if (--it2->amount <= 0) it2 = player.powers.erase(it2);
            else ++it2;
        } else ++it2;
    }

    // DoomPower.AfterSideTurnEnd(side == Player): a player whose Doom stacks reach their
    // current HP dies at the end of their own side. ShouldDoomTrigger only needs the side's
    // first doomed creature to fire, and the kill is not damage (block is irrelevant).
    if (int doom = powerAmount(player.powers, PowerId::DOOM)) {
        if (player.curHp > 0 && player.curHp <= doom) {
            player.curHp = 0;
            battleOver = true;
            playerWon = false;
            return;
        }
    }

    // ConstrictPower.AfterSideTurnEnd(player): the constricted owner takes unpowered
    // damage equal to the stacks at the end of its own side (block still absorbs).
    if (int con = powerAmount(player.powers, PowerId::CONSTRICT)) {
        damagePlayer(con, /*poweredAttack=*/false);
        if (battleOver) return;
    }

    // TangledPower.AfterSideTurnEnd(player): the power leaves at the end of the owner's
    // side and clears every Entangled affliction it applied.
    if (powerAmount(player.powers, PowerId::TANGLED) > 0) {
        auto clear = [](std::vector<CardInstance>& pile) {
            for (auto& c : pile)
                if (c.affliction == 1) { c.affliction = 0; c.afflictionAmount = 0; c.stampText.clear(); }
        };
        clear(hand);
        clear(drawPile);
        clear(discardPile);
        clear(exhaustPile);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::TANGLED; }), player.powers.end());
    }
    // RingingPower.AfterSideTurnEnd(player): clears the Ringing afflictions it applied.
    if (powerAmount(player.powers, PowerId::RINGING) > 0) {
        auto clear = [](std::vector<CardInstance>& pile) {
            for (auto& c : pile)
                if (c.affliction == 2) { c.affliction = 0; c.afflictionAmount = 0; c.stampText.clear(); }
        };
        clear(hand);
        clear(drawPile);
        clear(discardPile);
        clear(exhaustPile);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::RINGING; }), player.powers.end());
    }
    // ChainsOfBindingPower.BeforeSideTurnEnd(player): the Bound afflictions clear at the
    // end of the owner's side and the per-turn counters reset (the power itself stays
    // until its duration runs out).
    if (powerAmount(player.powers, PowerId::CHAINS_OF_BINDING) > 0) {
        clearAffliction(4);
        boundCardsPlayedThisTurn = 0;
        boundAppliedThisTurn = 0;
    }

    // DisintegrationPower.AfterSideTurnEndLate(side == Player): the KnowledgeDemon curse
    // deals its stacks as unpowered (blockable) damage at the end of the player's turn.
    if (int dis = powerAmount(player.powers, PowerId::DISINTEGRATION)) damagePlayer(dis);
    if (battleOver) return;

    // JossPaper.AfterSideTurnEnd(player): ethereal exhausts count at the side end.
    if (hasRelic(RelicId::JOSS_PAPER)) {
        relicJossExhausted += relicJossEthereal;
        relicJossEthereal = 0;
        if (relicJossExhausted >= 5) {
            drawCards(relicJossExhausted / 5, /*naturalDraw=*/false);
            relicJossExhausted %= 5;
        }
    }
    // "Free this turn" (Mummified Hand / Vexing Puzzlebox) expires with the player's turn.
    for (auto* pile : {&hand, &drawPile, &discardPile, &exhaustPile})
        for (auto& c : *pile) {
            c.freeThisTurn = false;
            if (!c.parsedFreeCost) continue;
            c.parsedFreeCost = false;
            c.costBase = -1;
            c.costMods.clear();
            if (c.stampText.empty()) continue;
            std::vector<std::string> parts;
            std::string cur;
            for (char ch : c.stampText) {
                if (ch == '/') { parts.push_back(cur); cur.clear(); }
                else cur += ch;
            }
            parts.push_back(cur);
            if (parts.size() > 1) {
                const size_t colon = parts[1].find(':');
                if (colon != std::string::npos)
                    parts[1] = parts[1].substr(0, colon) + ":"
                             + std::to_string(c.def().cost + c.def().costUp * c.upgrade);
            }
            std::vector<std::string> kept;
            for (auto& part : parts) {
                if (part.rfind("stars=", 0) == 0) continue;
                const size_t cs = part.find("|cost-state=");
                if (cs != std::string::npos) {
                    const size_t next = part.find('|', cs + 1);
                    part = part.substr(0, cs)
                         + (next == std::string::npos ? std::string() : part.substr(next));
                }
                kept.push_back(part);
            }
            c.stampText.clear();
            for (size_t i = 0; i < kept.size(); ++i) {
                if (i) c.stampText += '/';
                c.stampText += kept[i];
            }
        }

    // ParryingShield.AfterSideTurnEnd(player): with at least 10 block, deal 6 unpowered
    // damage to a random hittable enemy (CombatTargets stream).
    if (hasRelic(RelicId::PARRYING_SHIELD) && player.block >= 10) {
        std::vector<int> hit;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving)
                hit.push_back(i);
        if (!hit.empty()) {
            const int t = hit[rng[(int)RngStream::CombatTargets].NextInt((int)hit.size())];
            damageEnemy(t, 6, /*powered=*/false);
        }
    }

    // TemporaryStrengthPower removal: Reptile Trinket's +Strength lasts this turn only.
    if (int rt = powerAmount(player.powers, PowerId::REPTILE_TRINKET)) {
        applyPowerToPlayer(PowerId::STRENGTH, -rt);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::REPTILE_TRINKET; }),
            player.powers.end());
    }

    // ---- enemy phase ----
    inEnemyPhase = true;
    enemyPhase();
    inEnemyPhase = false;
    if (battleOver) return;

    // ---- end of enemy side: powers fire then tick ----
    afterEnemySideTurnEnd();

    // ---- next player turn ----
    startPlayerTurn(false);
}

void BattleContext::enemyPhase() {
    // enemy side turn start: every enemy's block clears (Creature.AfterTurnStart -> ClearBlock)
    for (auto& m : enemies) {
        // BurrowedPower.ShouldClearBlock == false: a burrowed monster keeps its block.
        if (!m.removed && m.curHp > 0 && powerAmount(m.powers, PowerId::BURROWED) == 0)
            m.block = 0;
        // SlowPower.AfterSideTurnStart(owner side): the per-turn card counter resets.
        for (auto& p : m.powers)
            if (p.id == PowerId::SLOW) p.misc = 0;
    }

    // PlatingPower.AfterSideTurnStart (enemy side, RoundNumber > 1): one stack leaves at
    // the start of the owner's own turn (the block payout happens at that turn's end).
    if (turn > 1) {
        for (auto& m : enemies) {
            if (m.removed || m.curHp <= 0) continue;
            for (auto it = m.powers.begin(); it != m.powers.end();) {
                if (it->id == PowerId::PLATING) {
                    if (--it->amount <= 0) it = m.powers.erase(it);
                    else ++it;
                } else ++it;
            }
        }
    }

    // SandpitPower.AfterSideTurnStartLate(enemy side): the escape countdown burns one per
    // enemy turn; reaching 0 removes the power and the player is eaten (force kill).
    {
        bool eaten = false;
        for (auto& em : enemies) {
            if (em.removed) continue;
            for (auto it = em.powers.begin(); it != em.powers.end();) {
                if (it->id == PowerId::SANDPIT) {
                    if (--it->amount <= 0) { it = em.powers.erase(it); eaten = true; }
                    else ++it;
                } else ++it;
            }
        }
        if (eaten) {
            player.curHp = 0;
            battleOver = true;
            playerWon = false;
            return;
        }
    }

    // PoisonPower.AfterSideTurnStart: unblockable unpowered damage = stacks, then -1
    // (TriggerCount = min(Amount, 1 + opponent Accelerant); no Accelerant in the set)
    for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
        Monster& m = enemies[i];
        if (m.removed || m.curHp <= 0 || m.reviving) continue;
        if (int pois = powerAmount(m.powers, PowerId::POISON)) {
            m.curHp -= pois;
            if (m.curHp <= 0) {
                m.curHp = 0;
                onEnemyDeath(i);
            } else {
                for (auto it = m.powers.begin(); it != m.powers.end();) {
                    if (it->id == PowerId::POISON) {
                        if (--it->amount <= 0) it = m.powers.erase(it);
                        else ++it;
                    } else ++it;
                }
            }
        }
    }
    if (battleOver) return;

    // Actors = monsters that exist when the phase begins. Summons added mid-phase
    // (FOGMOG ILLUSION eye) join next phase — the eye spawns behind the fogmog slot,
    // so the game's iteration never reaches it this phase, and it first acts on the
    // following enemy phase (corpus: first Dazed lands in discard during turn 2).
    std::vector<int> actorIds;
    for (auto& m : enemies)
        if (!m.removed && (m.curHp > 0 || m.reviving)) actorIds.push_back(m.combatId);

    for (int id : actorIds) { // re-find by combat id: summons/respawns shift indices
        int idx = -1;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].combatId == id) { idx = i; break; }
        if (idx < 0) continue;
        Monster& m = enemies[idx];
        if (m.curHp <= 0 && !m.reviving) continue;
        executeEnemyIntent(idx);
        if (battleOver) return;
    }
}

void BattleContext::executeEnemyIntent(int idx) {
    // `enemies` can be reallocated MID-INTENT: a hit that kills the attacker through the
    // player's Thorns runs onEnemyDeath, and the Infested/AXEBOT paths push new monsters.
    // No Monster reference may survive the attack loop — work off the index, then re-find
    // the attacker by combat id.
    if (enemies[idx].defIdx >= 0) {
        // Tunneler Bite/Burrow/Below clear IsStunned (the stamp MS field).
        Monster& m0 = enemies[idx];
        const MonsterDef& mdx = m0.def(*this);
        if (m0.curState >= 0 && m0.curState < (int)mdx.states.size()) {
            const char* mid = mdx.states[m0.curState].moveId;
            // IsStunned clears when DIZZY performs (StillDizzyMove) and on Bite/Burrow/Below.
            if (std::strcmp(mid, "BITE_MOVE") == 0 || std::strcmp(mid, "BURROW_MOVE") == 0
                || std::strcmp(mid, "BELOW_MOVE") == 0 || std::strcmp(mid, "DIZZY_MOVE") == 0)
                m0.tunnelerStunned = false;
        }
    }
    if (enemies[idx].defIdx < 0) {
        enemies[idx].moveHistory.push_back(
            enemies[idx].nextMove.empty() ? std::string("null") : enemies[idx].nextMove);
        return;
    }
    const int selfId = enemies[idx].combatId;
    const MonsterDef& md = enemies[idx].def(*this);
    // branch states are resolved at intent-roll time
    const MoveState& st = md.states[enemies[idx].curState];

    // attack
    int hpBefore = player.curHp;
    // TheForgotten DREAD: the printed damage grows with the owner's own Dexterity.
    // TestSubject MULTI_CLAW: the hit count is 3 + ExtraMultiClawCount.
    if (st.damage > 0) {
        const int baseDmg = st.damage + (st.damageAddsSelfDexterity
            ? powerAmount(enemies[idx].powers, PowerId::DEXTERITY) : 0);
        const int hits = st.hitsFromExtraClaw
            ? st.hits + enemies[idx].extraMultiClawCount : st.hits;
        for (int h = 0; h < hits && !battleOver; ++h) {
            if (idx >= (int)enemies.size() || enemies[idx].combatId != selfId) break;
            Monster& cur = enemies[idx];
            if (cur.removed || cur.curHp <= 0) break;
            const int dmg = calcMonsterAttackDamage(cur, baseDmg);
            damagePlayer(dmg, /*poweredAttack=*/true, /*dealer=*/idx);
        }
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (enemies[i].combatId == selfId) { idx = i; break; }
        if (idx >= (int)enemies.size() || enemies[idx].combatId != selfId) return;
    }
    Monster& m = enemies[idx]; // valid from here: nothing below reallocates `enemies`

    // Imbalanced (BowlbugRock): its attack fully blocked -> off balance
    if (st.damage > 0 && powerAmount(m.powers, PowerId::IMBALANCED) > 0) {
        if (player.curHp == hpBefore) m.offBalance = true; // no damage got through
    }

    // debuffs on the player
    if (st.playerPower != PowerId::NONE) applyPowerToPlayer(st.playerPower, st.playerPowerAmount);
    if (st.playerPower2 != PowerId::NONE) applyPowerToPlayer(st.playerPower2, st.playerPower2Amount);
    if (st.playerPower3 != PowerId::NONE) applyPowerToPlayer(st.playerPower3, st.playerPower3Amount);

    // TheLost DEBILITATING_SMOG / TheForgotten MIASMA: the player loses N Strength (or
    // Dexterity) and the owner gains the same amount; Possess* remembers the theft and
    // returns it when the owner dies.
    if (st.stealStrength > 0) {
        applyPowerToPlayer(PowerId::STRENGTH, -st.stealStrength);
        addPower(m.powers, PowerId::STRENGTH, st.stealStrength, false);
        if (powerAmount(m.powers, PowerId::POSSESS_STRENGTH) > 0)
            m.stolenStrength += st.stealStrength;
    }
    if (st.stealDexterity > 0) {
        applyPowerToPlayer(PowerId::DEXTERITY, -st.stealDexterity);
        // MIASMA order: player -Dex, then GainBlock(8, Move) at the owner's CURRENT
        // Dexterity, then the owner takes the Dexterity.
        if (st.selfBlockAddsSelfDexterity)
            m.block += std::max(0, st.selfBlock + powerAmount(m.powers, PowerId::DEXTERITY));
        addPower(m.powers, PowerId::DEXTERITY, st.stealDexterity, false);
        if (powerAmount(m.powers, PowerId::POSSESS_SPEED) > 0)
            m.stolenDexterity += st.stealDexterity;
    }
    // OwlMagistrate VERDICT removes its own SoarPower before the debuff lands.
    if (st.removeSelfSoar)
        m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
            [](const PowerInst& pw) { return pw.id == PowerId::SOAR; }), m.powers.end());
    if (st.markBeetleCharged) m.beetleCharged = true;

    // LIQUIFY_GROUND: FranticEscape status cards — 3 into the draw pile and 3 into the
    // discard pile, each at a random position (Shuffle.NextInt(count + 1)).
    if (st.addFranticToDraw > 0 || st.addFranticToDiscard > 0) {
        auto addOne = [&](std::vector<CardInstance>& pile) {
            CardInstance fe;
            fe.id = CardId::FRANTIC_ESCAPE;
            fe.uniqueIdx = nextUniqueIdx++;
            fe.genCard = true;
            Sts2Rng& r = rng[(int)RngStream::Shuffle];
            pile.insert(pile.begin() + r.NextInt((int)pile.size() + 1), fe);
        };
        for (int i = 0; i < st.addFranticToDraw; ++i) addOne(drawPile);
        for (int i = 0; i < st.addFranticToDiscard; ++i) addOne(discardPile);
    }
    // CURSE_OF_KNOWLEDGE: the route's recorded choice (turnStartChoices effect
    // ApplyKnowledgeCurse, sourceId KNOWLEDGE_DEMON:<combatId>:<curseIndex>) selects which
    // player curse lands.
    if (st.curseChoice) {
        const int idx = std::min(m.curseCounter, 2);
        char srcId[48];
        std::snprintf(srcId, sizeof(srcId), "KNOWLEDGE_DEMON:%d:%d", m.combatId, idx);
        const TurnStartChoice* req = takeTurnStartChoice(srcId);
        std::string pick;
        if (req && !req->cards.empty()) {
            pick = req->cards[0].cardId;
        } else {
            // No recorded route to replay. This is a real player decision between
            // Disintegration and one other curse (KnowledgeDemon._curseOfKnowledgeSets),
            // and nothing on this path can ask, so the first card wins as before.
            // STS2_CURSE_PICK=alt takes the other one instead. Measured on the 40-stamp
            // act-2 boss fixture: Knowledge Demon 2/18 either way (mean end HP 1.0 vs
            // 1.8, mean decisions 31 vs 40), so the choice is not what decides the fight
            // and the old behaviour stays the default.
            static const bool kAlt = [] {
                const char* v = std::getenv("STS2_CURSE_PICK");
                return v && std::string(v) == "alt";
            }();
            if (kAlt) pick = idx == 0 ? "MIND_ROT" : (idx == 1 ? "SLOTH" : "WASTE_AWAY");
        }
        if (pick == "MIND_ROT") applyPowerToPlayer(PowerId::MIND_ROT, 1);
        else if (pick == "SLOTH") applyPowerToPlayer(PowerId::SLOTH, 3);
        else if (pick == "WASTE_AWAY") applyPowerToPlayer(PowerId::WASTE_AWAY, 1);
        else applyPowerToPlayer(PowerId::DISINTEGRATION, 6 + idx);
        ++m.curseCounter;
    }
    // PHEROMONE_SPIT: a full hive (>= 3) only buffs; otherwise it grows the hive and
    // gives +1 strength.
    if (st.hiveSpit) {
        if (powerAmount(m.powers, PowerId::PERSONAL_HIVE) >= 3) {
            addPower(m.powers, PowerId::STRENGTH, 2, false);
        } else {
            addPower(m.powers, PowerId::PERSONAL_HIVE, 1, false);
            addPower(m.powers, PowerId::STRENGTH, 1, false);
        }
    }

    if (st.selfBlock > 0 && !st.selfBlockAddsSelfDexterity) {
        m.block += st.selfBlock;
    }

    if (st.upgradePlayerWithers || st.addWithersToDiscard > 0) {
        auto bumpWither = [](CardInstance& c) {
            if (c.id != CardId::WITHER) return;
            ++c.fakeUpgrade;
            c.stampText.clear();
        };
        for (auto& c : hand) bumpWither(c);
        for (auto& c : drawPile) bumpWither(c);
        for (auto& c : discardPile) bumpWither(c);
        for (auto& c : exhaustPile) bumpWither(c);
        ++m.addStrength;
        for (int i = 0; i < st.addWithersToDiscard; ++i)
            discardPile.push_back(makeWither(m.addStrength));
        int gain = st.selfPowerAmount + (m.addStrength - 1);
        applyPowerToMonsterSelf(m, st.selfPower, gain);
    } else if (st.selfPower != PowerId::NONE && st.selfPowerAllies) {
        // PowerCmd.Apply(GetTeammatesOf(creature), ...) — every living enemy, owner included
        for (auto& o : enemies)
            if (!o.removed && o.curHp > 0)
                applyPowerToMonsterSelf(o, st.selfPower, st.selfPowerAmount);
    } else if (st.selfPower != PowerId::NONE && st.selfPowerTeammates) {
        // Queen BURN_BRIGHT_FOR_ME: PowerCmd.Apply(GetTeammatesOf(owner) minus owner, ...)
        for (auto& o : enemies)
            if (&o != &m && !o.removed && o.curHp > 0)
                applyPowerToMonsterSelf(o, st.selfPower, st.selfPowerAmount);
    } else if (st.selfPower != PowerId::NONE) {
        int amount = st.selfPowerAmount;
        // Axebot BootUpMove: PowerCmd.Apply(Strength, BootUpStrGain * RespawnCount);
        // the fresh body's respawn count is 2 - stockLeft (spawned bodies only).
        if (m.stockLeft < 2 && st.selfPower == PowerId::STRENGTH
            && std::strcmp(st.moveId, "BOOT_UP_MOVE") == 0)
            amount *= (2 - m.stockLeft);
        applyPowerToMonsterSelf(m, st.selfPower, amount);
        // RitualPower.AfterApplied: a ritual applied during the enemy turn skips its
        // first own-side end (WasJustAppliedByEnemy).
        if (st.selfPower == PowerId::RITUAL)
            for (auto& p : m.powers)
                if (p.id == PowerId::RITUAL) p.misc = 1;
    }
    // VitalSparkPower.AfterPowerAmountChanged: every Tainted card takes the new stack count.
    if (st.selfPower == PowerId::VITAL_SPARK) {
        const int v = powerAmount(m.powers, PowerId::VITAL_SPARK);
        auto sync = [v](std::vector<CardInstance>& pile) {
            for (auto& c : pile)
                if (c.affliction == 3) { c.afflictionAmount = v; c.stampText.clear(); }
        };
        sync(hand); sync(drawPile); sync(discardPile); sync(exhaustPile);
    }

    // heal (REATTACH +25 only while another segment lives; REVIVE = full)
    if (st.healSelf != 0) {
        bool othersAlive = false;
        for (auto& o : enemies)
            if (&o != &m && o.curHp > 0 && !o.reviving) othersAlive = true;
        if (!md.reviveOnlyIfOthersAlive || othersAlive) {
            if (st.healSelf < 0) m.curHp = m.maxHp;
            else m.curHp = std::min(m.maxHp, m.curHp + st.healSelf);
            if (m.curHp > 0) m.reviving = false;
        }
    }

    // Dazed generation (EyeWithTeeth DISTRACT): CardPileCmd.AddToCombatAndPreview with
    // CardPilePosition.Bottom — corpus shows them appended AFTER existing discards
    // (turn-4 stamp: BASH,DEFEND,STRIKE,STRIKE,DAZED,DAZED,DAZED)
    for (int i = 0; i < st.addDazedToDiscard; ++i) {
        CardInstance dz;
        dz.id = CardId::DAZED;
        dz.uniqueIdx = nextUniqueIdx++;
        dz.genCard = true;
        discardPile.push_back(dz);
    }
    // Same AddToCombatAndPreview path for other status cards (slime STICKY_SHOT -> Slimed).
    for (int i = 0; i < st.addCardToDiscardCount; ++i) {
        CardInstance sc;
        sc.id = st.addCardToDiscard;
        sc.uniqueIdx = nextUniqueIdx++;
        sc.genCard = true;
        discardPile.push_back(sc);
    }
    // Noisebot NOISE_MOVE: one Dazed to the discard bottom and one at a RANDOM position in
    // the draw pile (CardPilePosition.Random = Shuffle.NextInt(count + 1)).
    if (st.noisebotNoise) {
        CardInstance dz;
        dz.id = CardId::DAZED;
        dz.uniqueIdx = nextUniqueIdx++;
        dz.genCard = true;
        discardPile.push_back(dz);
        CardInstance dz2 = dz;
        dz2.uniqueIdx = nextUniqueIdx++;
        Sts2Rng& r = rng[(int)RngStream::Shuffle];
        drawPile.insert(drawPile.begin() + r.NextInt((int)drawPile.size() + 1), dz2);
    }
    // Guardbot GUARD_MOVE: 15 unpowered block to every Fabricator on the enemy side.
    if (st.guardFabricator) {
        for (auto& o : enemies)
            if (!o.removed && o.curHp > 0 && o.defIdx >= 0
                && std::strcmp(o.def(*this).name, "FABRICATOR") == 0)
                o.block += 15;
    }
    // ThievingHopper THIEVERY: steal one deck card from draw+discard. Candidate order is
    // CardPile.GetCards(player, Draw, Discard) (each pile's own card order), filtered to
    // cards that still have a DeckVersion (combat-generated status cards are skipped) and
    // narrowed by the four rarity priorities in ThievingHopper.cs.
    if (st.stealCard) {
        std::vector<CardInstance*> cand;
        for (auto* pile : {&drawPile, &discardPile})
            for (auto& c : *pile)
                if (!c.genCard) cand.push_back(&c);
        auto narrow = [&](bool (*pred)(const CardDef&)) {
            std::vector<CardInstance*> hit;
            for (auto* c : cand)
                if (pred(c->def())) hit.push_back(c);
            if (!hit.empty()) cand.swap(hit);
        };
        narrow([](const CardDef& d) { return d.rarity == CardRarity::UNCOMMON; });
        narrow([](const CardDef& d) {
            return d.rarity == CardRarity::COMMON || d.rarity == CardRarity::RARE
                || d.rarity == CardRarity::EVENT;
        });
        narrow([](const CardDef& d) {
            return d.rarity == CardRarity::BASIC || d.rarity == CardRarity::STARTER;
        });
        if (!cand.empty()) {
            Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
            CardInstance* pick = cand[r.NextInt((int)cand.size())];
            const int uidPick = pick->uniqueIdx;
            for (auto* pile : {&drawPile, &discardPile})
                pile->erase(std::remove_if(pile->begin(), pile->end(),
                    [uidPick](const CardInstance& c) { return c.uniqueIdx == uidPick; }),
                    pile->end());
            // SwipePower.Steal: one INSTANCED power row per stolen card holds the card.
            addPower(m.powers, PowerId::SWIPE, 1, false);
        }
    }
    // Status cards added straight to the HAND (Myte TOXIC: CardPileCmd.AddToCombatAndPreview
    // targets PileType.Hand).
    for (int i = 0; i < st.addCardToHandCount; ++i) {
        if ((int)hand.size() >= maxHandSize) break;
        CardInstance tc;
        tc.id = st.addCardToHand;
        tc.uniqueIdx = nextUniqueIdx++;
        tc.genCard = true;
        hand.push_back(tc);
    }
    // ToughEgg HATCH_MOVE: turn into a Hatchling — max/current HP rolled on the Niche
    // stream (RunRng.Niche.NextInt(HatchlingMinHp, HatchlingMaxHp + 1) = 19..22) and every
    // power except MinionPower is removed.
    if (md.name && std::strcmp(md.name, "TOUGH_EGG") == 0
        && std::strcmp(st.moveId, "HATCH_MOVE") == 0) {
        const int hatchHp = rng[(int)RngStream::Niche].NextInt(19, 23);
        m.maxHp = hatchHp;
        m.curHp = hatchHp;
        m.eggHatched = true;
        m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
            [](const PowerInst& p) { return p.id != PowerId::MINION; }), m.powers.end());
    }
    // TestSubject MULTI_CLAW: the hit count is 3 + ExtraMultiClawCount at execution, and
    // the counter grows after the attack resolves.
    if (st.hitsFromExtraClaw) m.extraMultiClawCount += 1;
    // TestSubject RESPAWN_MOVE: Respawns++ then the stage-specific revive (Revive() calls
    // SetMaxHp + Heal with the scaled form HP).
    if (st.testSubjectRespawn) {
        m.respawns += 1;
        if (m.respawns == 1) {
            m.maxHp = 200; m.curHp = 200;
            addPower(m.powers, PowerId::PAINFUL_STABS, 1, false);
        } else if (m.respawns == 2) {
            m.maxHp = 300; m.curHp = 300;
            addPower(m.powers, PowerId::NEMESIS, 1, false);
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) {
                    return p.id == PowerId::ADAPTABLE || p.id == PowerId::PAINFUL_STABS;
                }), m.powers.end());
        }
        m.reviving = false;
    }
    // Fabricator FABRICATE_MOVE / FABRICATING_STRIKE_MOVE: spawn one or two bots. Each
    // spawn takes Encounter.GetNextSlot (first free encounter slot) and never repeats the
    // previous spawn (_lastSpawned). The spawn set order is the C# HashSet iteration order
    // (defense {GUARDBOT, NOISEBOT}, aggro {ZAPBOT, STABBOT}), filtered by _lastSpawned
    // and drawn with RunRng.MonsterAi.NextItem. Deferred to the very end of the intent:
    // inserting into `enemies` reallocates the vector and invalidates every Monster&.
    auto fabricateSpawns = [&]() {
        auto spawnFrom = [&](const std::vector<std::string>& order) {
            int ownerIdx = -1;
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].combatId == selfId) { ownerIdx = i; break; }
            if (ownerIdx < 0) return;
            const std::string prev = enemies[ownerIdx].lastSpawned;
            std::vector<std::string> opts;
            for (const auto& n : order)
                if (prev != n) opts.push_back(n);
            if (opts.empty()) return;
            Sts2Rng& r = rng[(int)RngStream::MonsterAi];
            const std::string pick = opts[r.NextInt((int)opts.size())];
            std::vector<std::string> slots;
            {
                std::string cur;
                for (const char* p = md.slotOrder; ; ++p) {
                    if (*p == ',' || *p == '\0') {
                        if (!cur.empty()) slots.push_back(cur);
                        cur.clear();
                        if (*p == '\0') break;
                    } else cur += *p;
                }
            }
            spawnMonster(BattleContext::monsterDefIdx(pick),
                         nextFreeSlot(slots), /*rollIntent=*/false);
            for (int i = 0; i < (int)enemies.size(); ++i)
                if (!enemies[i].removed && enemies[i].combatId == selfId) {
                    enemies[i].lastSpawned = pick;
                    break;
                }
        };
        if (st.fabricateKind == 1) spawnFrom({"GUARDBOT", "NOISEBOT"});
        spawnFrom({"ZAPBOT", "STABBOT"});
    };

    // State-advance bookkeeping (game: transitions happen ONLY at intent-roll time in
    // RollMove; execution just performs the move and clears the must-perform gate).
    m.gate = false;
    // BowlbugRock: a fully blocked HEADBUTT -> HeadbuttMove calls Stun() -> CreatureCmd.Stun
    // forces DIZZY (SetMoveImmediate: never logged, MustPerformOnceBeforeTransitioning).
    if (m.offBalance && std::string(st.moveId) == "HEADBUTT_MOVE") {
        m.curState = 1;             // DIZZY recovery state
        m.nextMove = "STUNNED";     // DIZZY's StunIntent displays as the generic marker
        m.gate = true;
    } else if (std::string(st.moveId) == "DIZZY_MOVE") {
        m.offBalance = false;       // recovered; next roll transitions back to HEADBUTT
    }

    // ThievingHopper ESCAPE_MOVE: CreatureCmd.Escape removes the creature from combat
    // (powers cleared first). The stolen cards leave the run with it.
    if (st.escapeCombat) {
        m.powers.clear();
        m.removed = true;
        m.curHp = 0;
        checkBattleOver();
        return;
    }

    // summon (FOGMOG ILLUSION / Ovicopter eggs) — deferred to the tail: inserting into
    // `enemies` here would shift/destroy `m` mid-function (the old code wrote through a
    // stale alias).
    if (st.summonDefIdx >= 0) {
        const MonsterDef& sd = monsterDefs()[st.summonDefIdx];
        std::vector<std::string> summonSlots;
        std::vector<std::string> slotOrderList;
        {
            std::string cur;
            for (const char* p = md.slotOrder; *p; ++p) {
                if (*p == ',') { slotOrderList.push_back(cur); cur.clear(); }
                else cur += *p;
            }
            if (!cur.empty()) slotOrderList.push_back(cur);
        }
        if (st.summonLastFreeSlot) {
            // Slots.LastOrDefault(s => Enemies.All(c => c.SlotName != s)) — every egg
            // takes the LAST unoccupied slot (egg5, egg4, egg3, ...).
            const std::vector<std::string>& order = slotOrderList;
            for (int i = (int)order.size() - 1; i >= 0; --i) {
                const std::string& sl = order[i];
                bool busy = false;
                for (auto& o : enemies)
                    if (!o.removed && o.slot == sl) busy = true;
                for (auto& t : summonSlots) if (t == sl) busy = true;
                if (!busy) summonSlots.push_back(sl);
            }
        } else {
            bool slotBusy = false;
            for (auto& o : enemies)
                if (o.slot == st.summonSlot && (o.curHp > 0 || o.reviving)) slotBusy = true;
            if (!slotBusy) summonSlots.push_back(st.summonSlot);
        }
        const int count = std::min(st.summonCount, (int)summonSlots.size());
        for (int k = 0; k < count; ++k) {
            // CreateCreature -> SetUniqueMonsterHpValue: one Niche draw per enemy summon
            // (NextInt over the free max-HP set; a singleton set still advances once).
            std::vector<int> cand;
            for (int hp = sd.minHp; hp <= sd.maxHp; ++hp) {
                bool used = false;
                for (auto& o : enemies) if (!o.removed && o.maxHp == hp) used = true;
                if (!used) cand.push_back(hp);
            }
            int hp = sd.maxHp;
            if (cand.empty()) (void)rng[(int)RngStream::Niche].NextInt(0, 1);
            else hp = cand[rng[(int)RngStream::Niche].NextInt((int)cand.size())];
            Monster e;
            e.defIdx = st.summonDefIdx;
            e.name = sd.name;
            e.slot = summonSlots[k];
            e.curHp = e.maxHp = hp;
            e.combatId = ++nextCombatId;
            applyStartPowers(e, sd);
            e.curState = sd.startState;
            e.nextMove = sd.states[e.curState].moveId; // enemy-side summon: no RollMove (CombatManager.AfterCreatureAdded rolls only when CurrentSide==Player)
            e.moveHistory.push_back(e.nextMove);       // machine constructor logs the initial state
            e.gate = true;                             // first roll is a no-op (_performedFirstMove=false)
            if (summonSlots[k] == "illusion") {
                enemies.insert(enemies.begin(), e); // "illusion" slot precedes the fogmog slot
            } else if (st.summonLastFreeSlot) {
                // Enemies stay ordered by encounter slot (the game's Enemies list is the
                // encounter's slot order): egg3/egg4/egg5 come before the ovicopter slot.
                auto slotIndex = [&](const std::string& sl) {
                    for (int q = 0; q < (int)slotOrderList.size(); ++q)
                        if (slotOrderList[q] == sl) return q;
                    return -1;
                };
                const int mine = slotIndex(summonSlots[k]);
                int at = (int)enemies.size();
                for (int q = 0; q < (int)enemies.size(); ++q) {
                    const int theirs = slotIndex(enemies[q].slot);
                    if (theirs >= 0 && mine >= 0 && theirs > mine) { at = q; break; }
                }
                enemies.insert(enemies.begin() + at, e);
            } else {
                enemies.push_back(e);
            }
        }
    }
    // Fabricator spawns run LAST: `enemies` reallocation invalidates every Monster& held
    // above (m included), and the summon block above also inserts into the vector.
    if (st.fabricateKind > 0) fabricateSpawns();
}

void BattleContext::applyStartPowers(Monster& m, const MonsterDef& md) {
    if (md.startPower != PowerId::NONE) addPower(m.powers, md.startPower, md.startPowerAmount, false);
    if (md.startPower2 != PowerId::NONE) addPower(m.powers, md.startPower2, md.startPower2Amount, false);
    if (md.startPlayerPower != PowerId::NONE)
        applyPowerToPlayer(md.startPlayerPower, md.startPlayerPowerAmount);
}

// EncounterModel.GetNextSlot: Slots.FirstOrDefault(s => Enemies.All(c => c.SlotName != s)).
std::string BattleContext::nextFreeSlot(const std::vector<std::string>& slots) const {
    for (const auto& s : slots) {
        bool busy = false;
        for (const auto& o : enemies)
            if (!o.removed && o.slot == s) { busy = true; break; }
        if (!busy) return s;
    }
    return std::string();
}

// CreatureCmd.Add for an enemy summoned mid-combat (Fabricator bots). CreateCreature ->
// SetUniqueMonsterHpValue spends one Niche draw over the free HP values, then the model's
// AfterAddedToRoom powers apply. A mid-enemy-phase summon takes no RollMove: the intent is
// the machine's initial state and the first transition is gated behind one performance.
void BattleContext::spawnMonster(int defIdx, const std::string& slot, bool rollIntent) {
    if (defIdx < 0 || slot.empty()) return;
    const MonsterDef& sd = monsterDef(defIdx);
    std::vector<int> cand;
    for (int hp = sd.minHp; hp <= sd.maxHp; ++hp) {
        bool used = false;
        for (const auto& o : enemies)
            if (!o.removed && o.maxHp == hp) used = true;
        if (!used) cand.push_back(hp);
    }
    int hp = sd.maxHp;
    if (cand.empty()) (void)rng[(int)RngStream::Niche].NextInt(0, 1);
    else hp = cand[rng[(int)RngStream::Niche].NextInt((int)cand.size())];
    Monster e;
    e.defIdx = defIdx;
    e.name = sd.name;
    e.slot = slot;
    e.curHp = e.maxHp = hp;
    e.combatId = ++nextCombatId;
    applyStartPowers(e, sd);
    e.curState = sd.startState;
    e.nextMove = sd.states[e.curState].moveId;
    e.moveHistory.push_back(e.nextMove);
    e.gate = true;
    // Enemies stay ordered by the encounter's slot order (the game's Enemies list).
    const MonsterDef* owner = nullptr;
    for (const auto& o : enemies)
        if (o.defIdx >= 0 && o.def(*this).slotOrder && o.def(*this).slotOrder[0]) {
            owner = &o.def(*this);
            break;
        }
    auto slotIndex = [&](const std::string& sl) {
        if (!owner) return -1;
        std::string cur;
        int idx = 0;
        for (const char* p = owner->slotOrder; ; ++p) {
            if (*p == ',' || *p == '\0') {
                if (cur == sl) return idx;
                ++idx;
                cur.clear();
                if (*p == '\0') break;
            } else cur += *p;
        }
        return -1;
    };
    const int mine = slotIndex(slot);
    int at = (int)enemies.size();
    for (int q = 0; q < (int)enemies.size(); ++q) {
        const int theirs = slotIndex(enemies[q].slot);
        if (theirs >= 0 && mine >= 0 && theirs > mine) { at = q; break; }
    }
    enemies.insert(enemies.begin() + at, e);
    if (rollIntent) resolveInitialIntent(enemies[at]);
}

// CardCmd.ClearAffliction over every pile for one affliction kind.
void BattleContext::clearAffliction(int kind) {
    auto clear = [kind](std::vector<CardInstance>& pile) {
        for (auto& c : pile)
            if (c.affliction == kind) { c.affliction = 0; c.afflictionAmount = 0; c.stampText.clear(); }
    };
    clear(hand);
    clear(drawPile);
    clear(discardPile);
    clear(exhaustPile);
}

CardInstance BattleContext::makeWither(int upgradeCount) {
    CardInstance c;
    c.id = CardId::WITHER;
    c.uniqueIdx = nextUniqueIdx++;
    c.justGenerated = true;
    c.genCard = true;
    c.fakeUpgrade = upgradeCount; // AfterCardGeneratedForCombat: MatchWitherToUpgradeCount
    return c;
}

void BattleContext::afterEnemySideTurnEnd() {
    // BattlewornDummyTimeLimitPower.AfterSideTurnEnd (enemy participants): the countdown
    // ticks each enemy side end; at 1 the dummy escapes and the encounter times out.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        for (auto& p : m.powers) {
            if (p.id != PowerId::BATTLEWORN_DUMMY_TIME_LIMIT) continue;
            if (p.amount > 1) { --p.amount; break; }
            m.removed = true; // CreatureCmd.Escape
            battleTimedOut = true;
            break;
        }
    }
    if (battleTimedOut) {
        checkBattleOver();
        if (battleOver) return;
    }
    // PlatingPower.BeforeSideTurnEndEarly(enemy side): gain block = Amount before the
    // end-of-turn damage effects resolve.
    for (auto& m : enemies)
        if (!m.removed && m.curHp > 0)
            if (int amt = powerAmount(m.powers, PowerId::PLATING)) m.block += amt;
    // SlumberPower.AfterSideTurnEnd(enemy participants): one stack per own side end; at 0
    // the SlumberingBeetle wakes (Plating leaves, IsAwake=true) without being stunned.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        bool woke = false;
        for (auto it = m.powers.begin(); it != m.powers.end();) {
            if (it->id == PowerId::SLUMBER) {
                if (--it->amount <= 0) { it = m.powers.erase(it); woke = true; }
                else ++it;
            } else ++it;
        }
        if (woke) {
            m.isAwake = true;
            m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::PLATING; }),
                m.powers.end());
        }
    }
    // RitualPower.AfterSideTurnEnd(enemy participants): gain Amount Strength at the end
    // of the owner's own side turn, except the first one after it was applied.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        int gain = 0;
        for (auto& p : m.powers) {
            if (p.id != PowerId::RITUAL) continue;
            if (p.misc == 1) { p.misc = 0; continue; }
            gain += p.amount;
        }
        if (gain > 0) addPower(m.powers, PowerId::STRENGTH, gain, false);
    }
    // HighVoltagePower.AfterSideTurnEnd(enemy participants): the owner gains Amount
    // Strength at the end of its own side (Fabricator's Zapbot).
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        if (int hv = powerAmount(m.powers, PowerId::HIGH_VOLTAGE))
            addPower(m.powers, PowerId::STRENGTH, hv, false);
    }
    // NemesisPower.AfterSideTurnEnd(enemy participants): each own side end toggles one
    // Intangible stack on the owner (TestSubject phase 3). The power's Data flag starts
    // false, so the FIRST own side end grants the stack.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        for (auto& p : m.powers) {
            if (p.id != PowerId::NEMESIS) continue;
            if (p.misc == 1) {
                p.misc = 0;
                m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                    [](const PowerInst& q) { return q.id == PowerId::INTANGIBLE; }),
                    m.powers.end());
            } else {
                p.misc = 1;
                addPower(m.powers, PowerId::INTANGIBLE, 1, false);
            }
            break;
        }
    }
    // EscapeArtistPower.AfterSideTurnEnd: display countdown that stops at 1.
    for (auto& m : enemies)
        if (!m.removed && m.curHp > 0)
            for (auto& p : m.powers)
                if (p.id == PowerId::ESCAPE_ARTIST && p.amount > 1) --p.amount;
    // DoomPower.BeforeSideTurnEnd(side == Enemy): enemies whose Doom stacks reach their
    // current HP die as the enemy side ends (not damage — block is irrelevant).
    for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
        Monster& m = enemies[i];
        if (m.removed || m.curHp <= 0 || m.reviving) continue;
        if (int doom = powerAmount(m.powers, PowerId::DOOM)) {
            if (m.curHp <= doom) {
                m.curHp = 0;
                onEnemyDeath(i);
            }
        }
    }
    if (battleOver) return;
    // BeforesSideTurnEnd powers: HailstormPower deals unpowered AoE damage equal to its
    // stacks once the owner has at least one Frost orb (BeforeSideTurnEnd)
    if (int amt = powerAmount(player.powers, PowerId::HAILSTORM)) {
        if (frostOrbCount() >= 1) {
            for (int i = 0; i < (int)enemies.size() && !battleOver; ++i) {
                if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
                damageEnemy(i, amt, /*powered=*/false);
            }
        }
    }
    // AfterSideTurnEnd: ConsumingShadowPower evokes the LAST orb once per stack
    if (int amt = powerAmount(player.powers, PowerId::CONSUMING_SHADOW)) {
        for (int i = 0; i < amt && !battleOver; ++i)
            if (!evokeLast()) break;
    }
    // AfterSideTurnEnd (enemy participants): Strangle/Flanking are enemy powers and leave
    // when the enemy's own side ends; ConquerorPower decrements the same way.
    for (auto& m : enemies) {
        if (m.removed || m.curHp <= 0) continue;
        for (auto it = m.powers.begin(); it != m.powers.end();) {
            if (it->id == PowerId::CONQUEROR || it->id == PowerId::DEBILITATE) {
                if (--it->amount <= 0) it = m.powers.erase(it);
                else ++it;
            } else ++it;
        }
        m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::STRANGLE || p.id == PowerId::FLANKING; }),
            m.powers.end());
    }
    // TaintedPower.AfterSideTurnEnd(side == Enemy): the extra-damage buff leaves the player
    // once the enemy side has acted.
    player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
        [](const PowerInst& p) { return p.id == PowerId::TAINTED; }), player.powers.end());
    // NoBlockPower.AfterSideTurnEnd (side == Enemy): one stack ticks away per enemy side
    for (auto it = player.powers.begin(); it != player.powers.end();) {
        if (it->id == PowerId::NO_BLOCK) {
            if (--it->amount <= 0) it = player.powers.erase(it);
            else ++it;
        } else ++it;
    }
    // FlameBarrier clears when the opposite (enemy) side ends; Colossus decrements
    // (AfterSideTurnEnd side==Enemy, removes at 0)
    for (auto it = player.powers.begin(); it != player.powers.end();) {
        if (it->id == PowerId::FLAME_BARRIER) it = player.powers.erase(it);
        else if (it->id == PowerId::COLOSSUS) {
            if (--it->amount <= 0) it = player.powers.erase(it);
            else ++it;
        }
        else ++it;
    }
    // power triggers (enemy side just ended): Territorial -> gain strength;
    // Mangle (TemporaryStrength IsPositive=false) restores its full stack then leaves
    for (auto& m : enemies) {
        if (m.curHp <= 0) continue;
        if (int amt = powerAmount(m.powers, PowerId::TERRITORIAL))
            applyPowerToMonsterSelf(m, PowerId::STRENGTH, amt);
        for (PowerId wail : {PowerId::MANGLE, PowerId::PIERCING_WAIL, PowerId::DARK_SHACKLES,
                             PowerId::ENFEEBLING_TOUCH, PowerId::MONARCHS_GAZE_STRENGTH_DOWN}) {
            if (int amt = powerAmount(m.powers, wail)) {
                addPower(m.powers, PowerId::STRENGTH, amt, false); // -Sign * Amount
                m.powers.erase(std::remove_if(m.powers.begin(), m.powers.end(),
                    [wail](const PowerInst& p) { return p.id == wail; }), m.powers.end());
            }
        }
    }
    // duration tick: counter debuffs on every creature tick when enemy side ends;
    // entries flagged skipNextTick skip exactly one tick (freshly applied player-side debuffs).
    auto tick = [](std::vector<PowerInst>& list) {
        auto isDuration = [](PowerId id) {
            return id == PowerId::VULNERABLE || id == PowerId::WEAK || id == PowerId::FRAIL
                || id == PowerId::INTANGIBLE;
        };
        for (auto& p : list) {
            if (!isDuration(p.id)) continue;
            if (p.skipNextTick) { p.skipNextTick = false; continue; }
            p.amount -= 1;
        }
        list.erase(std::remove_if(list.begin(), list.end(), [&isDuration](const PowerInst& p) {
            return isDuration(p.id) && p.amount <= 0;
        }), list.end());
    };
    // ShrinkPower.AfterSideTurnEnd: finite stacks tick; -1 (infinite, applied by
    // ShrinkerBeetle) never ticks.
    for (auto it = player.powers.begin(); it != player.powers.end();) {
        if (it->id == PowerId::SHRINK && it->amount > 0) {
            if (it->skipNextTick) { it->skipNextTick = false; ++it; }
            else if (--it->amount <= 0) it = player.powers.erase(it);
            else ++it;
        } else ++it;
    }
    tick(player.powers);
    for (auto& m : enemies) if (m.curHp > 0) tick(m.powers);
}

// RandomBranchState.GetStateWeight: repeat-type gate x cooldown gate x branch weight.
static float branchWeight(const BranchAlt& a, const Monster& m, const MonsterDef& md) {
    const std::string& target = md.states[a.target].moveId;
    const auto& log = m.moveHistory;
    if (a.useOnlyOnce) {
        for (const auto& e : log)
            if (e == target) return 0.0f;
        return a.weight;
    }
    if (a.maxRepeat > 0) {
        const int n = a.maxRepeat;
        if (static_cast<int>(log.size()) >= n) {
            bool allSame = true;
            for (int i = 1; i <= n; ++i) {
                if (log[log.size() - i] != target) { allSame = false; break; }
            }
            if (allSame) return 0.0f;
        }
    }
    if (a.cooldown > 0) {
        const int take = std::min(a.cooldown, static_cast<int>(log.size()));
        for (int i = 1; i <= take; ++i)
            if (log[log.size() - i] == target) return 0.0f;
    }
    return a.weight;
}

// ConditionalBranchState predicates (no RNG draw): 1 = Ovicopter CanLay
// (GetTeammatesOf(creature).Count(IsAlive) <= 3 — the count includes the owner itself:
// corpus OVICOPTER T4 with 3 live eggs picks NUTRITIONAL_PASTE, not LAY_EGGS).
int BattleContext::evalCondPredicate(int pred, const Monster& self) const {
    if (pred == 1) {
        (void)self;
        int alive = 0;
        for (const auto& o : enemies)
            if (!o.removed && o.curHp > 0) ++alive;
        return alive <= 3 ? 1 : 0;
    }
    if (pred == 2) return powerAmount(self.powers, PowerId::SLUMBER) > 0 ? 1 : 0;
    if (pred == 3) return self.curseCounter < 3 ? 1 : 0; // KnowledgeDemon branch
    // FrogKnight HALF_HEALTH: TONGUE_LASH while already charged or at/above half HP,
    // otherwise BEETLE_CHARGE.
    if (pred == 4)
        return (self.beetleCharged || self.curHp >= self.maxHp / 2) ? 1 : 0;
    // LivingShield SHIELD_SLAM_BRANCH: GetAllyCount() > 0 (another living enemy).
    if (pred == 5) {
        for (const auto& o : enemies)
            if (&o != &self && !o.removed && o.curHp > 0) return 1;
        return 0;
    }
    // Fabricator CanFabricate: GetTeammatesOf(owner).Count(IsAlive) < 4 — the count
    // INCLUDES the owner itself.
    if (pred == 6) {
        int alive = 0;
        for (const auto& o : enemies)
            if (!o.removed && o.curHp > 0) ++alive;
        return alive < 4 ? 1 : 0;
    }
    // Queen branch: !HasAmalgamDied.
    if (pred == 7) return self.amalgamDied ? 0 : 1;
    // TestSubject REVIVE_BRANCH: Respawns < 2.
    if (pred == 8) return self.respawns < 2 ? 1 : 0;
    return 0;
}

void BattleContext::resolveInitialIntent(Monster& m) {
    if (m.defIdx < 0) return;
    const MonsterDef& md = m.def(*this);
    int s = m.curState;
    if (s < 0 || s >= (int)md.states.size()) return;
    // Slot-conditional INIT states resolve on the first RollMove at combat setup too
    // (Wriggler INIT in the DenseVegetation event fight).
    if (md.states[s].slotInit && m.slotInitAlt >= 0) {
        m.curState = m.slotInitAlt;
        m.nextMove = md.states[m.curState].moveId;
        if (!md.states[m.curState].noLog) m.moveHistory.push_back(m.nextMove);
        return;
    }
    if (md.states[s].branch.empty() && md.states[s].condPredicate <= 0) return;
    // FindNextMoveState on the first RollMove: resolve branch states until a MoveState
    // lands (the game loops `while (!_currentState.IsMove)`, so a conditional branch may
    // point at a RandomBranchState — Fabricator's fabricateBranch -> RAND).
    for (int hop = 0; hop < 8; ++hop) {
        const MoveState& cur = md.states[s];
        if (!cur.branch.empty()) {
            const auto& alts = cur.branch;
            float total = 0;
            for (auto& a : alts) total += branchWeight(a, m, md);
            float roll = rng[(int)RngStream::MonsterAi].NextFloat(0.0f, total);
            int chosen = -1;
            for (auto& a : alts) {
                const float w = branchWeight(a, m, md);
                if (w <= 0.0f) continue;
                roll -= w;
                chosen = a.target;
                if (roll <= 0) break;
            }
            if (chosen < 0) break;
            s = chosen;
        } else if (cur.condPredicate > 0) {
            s = evalCondPredicate(cur.condPredicate, m) ? cur.condTrue : cur.condFalse;
        } else if (cur.next >= 0) {
            s = cur.next;
        } else {
            break;
        }
        if (md.states[s].branch.empty() && md.states[s].condPredicate <= 0) break;
    }
    if (md.states[s].slotInit && m.slotInitAlt >= 0) s = m.slotInitAlt;
    m.curState = s;
    m.nextMove = md.states[s].moveId;
    if (!md.states[s].noLog) m.moveHistory.push_back(m.nextMove);
}

void BattleContext::startPlayerTurn(bool firstTurn) {
    ++turn;
    // ArtOfWar: AfterSideTurnEnd moves ThisTurn -> LastTurn; AfterEnergyReset reads it.
    relicAoWLastTurn = relicAoWThisTurn;
    relicAoWThisTurn = false;
    // MercuryHourglass / Brimstone AfterSideTurnStart (player side).
    applyRelicTurnStart();
    // Pocketwatch.AfterSideTurnStart: move this turn's play count to last turn.
    relicPocketwatchLastTurn = relicPocketwatchThisTurn;
    relicPocketwatchThisTurn = 0;
    // Pendulum.BeforeHandDraw: every 3rd player turn adds 1 to the hand draw.
    relicPendulumTurns = (relicPendulumTurns + 1) % 3;
    relicBeatingDamageThisTurn = 0;
    relicDemonTongueUsed = false;
    relicRainbowAttacks = 0;
    relicRainbowSkills = 0;
    relicRainbowPowers = 0;
    relicRainbowActivated = 0;

    // RampartPower.AfterSideTurnStart(player side): every TurretOperator in the encounter
    // gains the carrier's Amount as unpowered block.
    for (const auto& shield : enemies) {
        if (shield.removed || shield.curHp <= 0) continue;
        if (!shield.def(*this).rampartShield) continue;
        const int amt = powerAmount(shield.powers, PowerId::RAMPART);
        if (amt <= 0) continue;
        for (auto& o : enemies)
            if (!o.removed && o.curHp > 0
                && std::strcmp(o.def(*this).name, "TURRET_OPERATOR") == 0)
                o.block += amt;
    }

    // Bolas.BeforeHandDraw reads the PREVIOUS player turn's play history
    bolasPlayedLastTurn = bolasPlayedThisTurn;
    bolasPlayedThisTurn.clear();
    // ChainsOfBindingPower.BeforeSideTurnEnd clears the Bound afflictions and the per-turn
    // play counter; the counter starts clean at each player side start.
    boundCardsPlayedThisTurn = 0;
    boundAppliedThisTurn = 0;

    // stamp turn-card-history counters reset (HappenedThisTurn window)
    statusDrawsThisTurn = 0;
    zeroCostAttackStarts = 0;
    cardPlayStarts = 0;
    attackSkillStarts = 0;
    attackStartsThisTurn = 0;
    relicAttacksThisTurn = 0;
    relicSkillsThisTurn = 0;
    cardExhaustsThisTurn = 0;
    hpLostThisTurn = 0;
    cardsDiscardedThisTurn = 0;
    effectDrawsThisTurn = 0;
    energySpentThisTurn = 0;
    attacksFinishedThisTurn = 0;
    skillPlaysFinishedThisTurn = 0;
    blockGainsThisTurn = 0;
    doomAppliedByOwnerThisTurn = false;

    // block clears at side turn start, except the player's very first turn;
    // BlockNextTurnPower (Prolong) returns the stored block when it clears
    // BlurPower.ShouldClearBlock == false: the player's block survives the turn start
    if (!firstTurn && powerAmount(player.powers, PowerId::BLUR) <= 0) {
        // SturdyClamp.ShouldClearBlock: the owner's block is kept but capped at 10.
        if (hasRelic(RelicId::STURDY_CLAMP)) {
            if (player.block > 10) player.block = 10;
        } else {
        int keep = powerAmount(player.powers, PowerId::BLOCK_NEXT_TURN);
        player.block = 0;
        // ToricToughnessPower.AfterBlockCleared: re-grant the stored block and tick down.
        std::vector<int> toricBlocks;
        for (const auto& p : player.powers)
            if (p.id == PowerId::TORIC_TOUGHNESS && p.aux > 0) toricBlocks.push_back(p.aux);
        for (auto it = player.powers.begin(); it != player.powers.end();) {
            if (it->id == PowerId::TORIC_TOUGHNESS) {
                if (--it->amount <= 0) {
                    it = player.powers.erase(it);
                    continue;
                }
            }
            ++it;
        }
        for (int b : toricBlocks) {
            gainBlockUnpowered(b);
            ++toricRegrants;
        }
        if (keep > 0) {
            gainBlockUnpowered(keep);
            player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::BLOCK_NEXT_TURN; }),
                player.powers.end());
        }
        }
        // SelfFormingClayPower.AfterBlockCleared: gain its stacks as block, then remove.
        if (int sfc = powerAmount(player.powers, PowerId::SELF_FORMING_CLAY)) {
            gainBlockUnpowered(sfc);
            player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::SELF_FORMING_CLAY; }),
                player.powers.end());
        }
        // SparklingRouge.AfterBlockCleared: turn 3 grants +1 Strength and +1 Dexterity.
        if (turn == 3 && hasRelic(RelicId::SPARKLING_ROUGE)) {
            addPower(player.powers, PowerId::STRENGTH, 1, false);
            addPower(player.powers, PowerId::DEXTERITY, 1, false);
        }
        // HornCleat / CaptainsWheel AfterBlockCleared: turns 2 and 3 gain block.
        if (turn == 2 && hasRelic(RelicId::HORN_CLEAT)) gainBlockUnpowered(14);
        if (turn == 3 && hasRelic(RelicId::CAPTAINS_WHEEL)) gainBlockUnpowered(18);
    }

    // per-enemy hit counters reset with the turn window
    for (auto& m : enemies) m.hitsTakenThisTurn = 0;

    // ShadowStepPower.AfterSideTurnStart: converts into DoubleDamage, then leaves
    if (int ss = powerAmount(player.powers, PowerId::SHADOW_STEP)) {
        addPower(player.powers, PowerId::DOUBLE_DAMAGE, ss, false);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::SHADOW_STEP; }), player.powers.end());
    }

    // NeurosurgePower.AfterSideTurnStart: the owner gains Doom equal to the stacks at the
    // start of each of their turns (the debuff itself never ticks away).
    if (int ns = powerAmount(player.powers, PowerId::NEUROSURGE))
        addPower(player.powers, PowerId::DOOM, ns, false);

    // CountdownPower.AfterSideTurnStart: one random hittable enemy gains Doom = Amount
    if (int cd = powerAmount(player.powers, PowerId::COUNTDOWN)) {
        std::vector<int> hittable;
        for (int i = 0; i < (int)enemies.size(); ++i)
            if (!enemies[i].removed && enemies[i].curHp > 0 && !enemies[i].reviving) hittable.push_back(i);
        if (!hittable.empty()) {
            Sts2Rng& rt = rng[(int)RngStream::CombatTargets];
            applyPowerToEnemy(hittable[rt.NextInt((int)hittable.size())], PowerId::DOOM, cd);
        }
    }

    // energy reset; PyrePower.ModifyMaxEnergy adds its stacks to the turn's max energy
    // DemesnePower.ModifyMaxEnergy: +Amount max energy each turn
    // IceCream.ShouldPlayerResetEnergy: from turn 2 the leftover energy is kept and the
    // turn's max energy is added (CombatManager.AddMaxEnergyToCurrent), not a reset.
    const int breadEnergy = (hasRelic(RelicId::BREAD) && turn > 1) ? 1 : 0;
    const int turnMaxEnergy = player.maxEnergy + breadEnergy + powerAmount(player.powers, PowerId::PYRE)
                            + powerAmount(player.powers, PowerId::DEMESNE)
                            + powerAmount(player.powers, PowerId::FRIENDSHIP)
                            - powerAmount(player.powers, PowerId::WASTE_AWAY);
    if (hasRelic(RelicId::ICE_CREAM) && turn > 1) player.energy += turnMaxEnergy;
    else player.energy = turnMaxEnergy;
    // Candelabra (turn 2, +2) / Chandelier (turn 3, +3): AfterSideTurnStart energy.
    if (hasRelic(RelicId::CANDELABRA) && turn == 2) player.energy += 2;
    if (hasRelic(RelicId::CHANDELIER) && turn == 3) player.energy += 3;
    // ArtOfWar.AfterEnergyReset: +1 energy when no attack was played on the previous turn.
    if (hasRelic(RelicId::ART_OF_WAR) && turn > 1 && !relicAoWLastTurn) player.energy += 1;
    relicAoWLastTurn = false;
    relicAoWThisTurn = false;

    // HappyFlower.AfterSideTurnStart: every 3rd player turn (+1 energy). The turn-1 tick
    // happened before the first play and is seeded in buildBattle.
    if (hasRelic(RelicId::HAPPY_FLOWER)) {
        relicHappyTurns = (relicHappyTurns + 1) % 3;
        if (relicHappyTurns == 0) player.energy += 1;
    }

    // EnergyNextTurnPower.AfterEnergyReset: gains Amount energy then leaves (Sidestep)
    if (int amt = powerAmount(player.powers, PowerId::ENERGY_NEXT_TURN)) {
        player.energy += amt;
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::ENERGY_NEXT_TURN; }),
            player.powers.end());
    }

    // SummonNextTurnPower.AfterPlayerTurnStart: summon Amount, then the power leaves
    if (int amt = powerAmount(player.powers, PowerId::SUMMON_NEXT_TURN)) {
        summonOsty(amt);
        player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
            [](const PowerInst& p) { return p.id == PowerId::SUMMON_NEXT_TURN; }),
            player.powers.end());
    }

    // BoundPhylactery.AfterEnergyResetLate: the Necrobinder's starter relic re-summons Osty
    // (1 HP) after every energy reset except the very first turn — an alive pet instead
    // gains +1 MaxHp and heals 1. The relic is not stamp-visible, but a root stamp that
    // already carries a pet (osty=<cid>/hp/maxhp) proves the owner has it.
    if (player.ownerHasBoundPhylactery && !firstTurn) summonOsty(1);

    // AfterEnergyReset hooks run in apply order (LightningRod/Spinner channel orbs; the
    // game notes they sit here so an orb evoked to make room keeps its benefit —
    // Plasma's energy is not lost to the reset, Frost's block not lost to the clear)
    {
        // SpinnerPower.AfterEnergyReset: channel a Glass orb per stack
        if (int amt = powerAmount(player.powers, PowerId::SPINNER)) {
            for (int i = 0; i < amt && !battleOver; ++i) channelOrb(OrbKind::GLASS);
        }
        // GenesisPower.AfterEnergyReset: gain Amount stars
        if (int amt = powerAmount(player.powers, PowerId::GENESIS))
            player.stars += amt;
        // StarNextTurnPower.AfterEnergyReset: gain Amount stars, then the power leaves
        if (int amt = powerAmount(player.powers, PowerId::STAR_NEXT_TURN)) {
            player.stars += amt;
            player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
                [](const PowerInst& p) { return p.id == PowerId::STAR_NEXT_TURN; }),
                player.powers.end());
        }
        // LightningRodPower.AfterEnergyReset: channel one Lightning orb, then decrement
        for (int i = 0; i < 1 && !battleOver; ++i) {
            if (powerAmount(player.powers, PowerId::LIGHTNING_ROD) <= 0) break;
            channelOrb(OrbKind::LIGHTNING);
            for (auto it = player.powers.begin(); it != player.powers.end();) {
                if (it->id == PowerId::LIGHTNING_ROD) {
                    if (--it->amount <= 0) it = player.powers.erase(it);
                    else ++it;
                } else ++it;
            }
        }
    }

    // PlatingPower.AfterSideTurnStart: loses one stack each turn after the first
    if (!firstTurn) {
        for (auto it = player.powers.begin(); it != player.powers.end();) {
            if (it->id == PowerId::PLATING) {
                if (--it->amount <= 0) it = player.powers.erase(it);
                else ++it;
            } else ++it;
        }
    }

    // roll enemy intents for the coming enemy phase (RollMove at player turn start =
    // PrepareForNextTurn). This is the ONLY place the state machine advances: transition
    // along follow-ups / resolve RandomBranchStates on the MonsterAi stream (the draw
    // happens at intent time — corpus: fogmog R7 +1 on the turn it enters BRANCH), then
    // log the entered move (MonsterMoveStateMachine.StateLog; self-transitions log too).
    for (auto& m : enemies) {
        if (m.curHp <= 0 && !m.reviving) continue; // gone: keep last intent
        if (m.defIdx < 0) continue;                // unregistered: keep last intent
        if (m.gate) continue; // fresh/forced state must perform before transitioning
        const MonsterDef& md = m.def(*this);
        // Forced STUNNED move (CreatureCmd.Stun with an explicit nextMoveId): after the
        // stunned turn the machine jumps straight to that move and logs it.
        if (m.stunNextState >= 0 && m.stunNextState < (int)md.states.size()) {
            m.curState = m.stunNextState;
            m.nextMove = md.states[m.curState].moveId;
            if (!md.states[m.curState].noLog) m.moveHistory.push_back(m.nextMove);
            m.stunNextState = -1;
            continue;
        }
        int s = m.curState;
        // FindNextMoveState loops `while (!_currentState.IsMove)`: a conditional branch
        // may land on a RandomBranchState (Fabricator fabricateBranch -> RAND) and a
        // branch target may itself be a branch, so keep hopping until a plain move.
        for (int hop = 0; hop < 8; ++hop) {
            if (!md.states[s].branch.empty()) {
                // RandomBranchState.GetNextState: sum valid weights (CannotRepeat vs the
                // last logged move -> weight 0), NextFloat(0, total) — the draw happens
                // even when only one branch remains — then the first state whose
                // cumulative weight brings the roll to <= 0 wins.
                const auto& alts = md.states[s].branch;
                float total = 0;
                for (auto& a : alts) total += branchWeight(a, m, md);
                float roll = rng[(int)RngStream::MonsterAi].NextFloat(0.0f, total);
                int chosen = -1;
                for (auto& a : alts) {
                    const float w = branchWeight(a, m, md);
                    if (w <= 0.0f) continue;
                    roll -= w;
                    chosen = a.target;
                    if (roll <= 0) break;
                }
                if (chosen < 0) break;
                s = chosen;
            } else if (md.states[s].condPredicate > 0) {
                // ConditionalBranchState (Ovicopter SUMMON_BRANCH_STATE): first branch
                // whose predicate holds wins, no RNG draw.
                const MoveState& cs = md.states[s];
                s = evalCondPredicate(cs.condPredicate, m) ? cs.condTrue : cs.condFalse;
            } else if (md.states[s].next >= 0) {
                s = md.states[s].next;
            } else {
                break; // stay on the current move (self-loop without follow pointer)
            }
            if (md.states[s].branch.empty() && md.states[s].condPredicate <= 0) break;
        }
        // ConditionalBranchState (Wriggler INIT): the target depends on the slot name,
        // which the spawn assigned (wriggler1/3 -> bite, wriggler2/4 -> wriggle).
        if (md.states[s].slotInit && m.slotInitAlt >= 0) s = m.slotInitAlt;
        m.curState = s;
        m.nextMove = md.states[s].moveId;
        if (!md.states[s].noLog) m.moveHistory.push_back(m.nextMove);
    }

    // BeforeSideTurnStart powers (fire before the draw): Aggression pulls N shuffled
    // attack cards from discard into the hand and upgrades each (AggressionPower.cs).
    // The shuffle runs on a filtered COPY — untouched cards keep their pile positions.
    if (int amt = powerAmount(player.powers, PowerId::AGGRESSION)) {
        std::vector<int> pool; // indices into discardPile
        for (int i = 0; i < (int)discardPile.size(); ++i)
            if (discardPile[i].def().type == CardType::ATTACK) pool.push_back(i);
        // UnstableShuffle on the filtered list (CombatCardSelection stream), Take(amount)
        Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
        for (int i = (int)pool.size() - 1; i >= 1; --i) {
            int j = r.NextInt(i + 1);
            std::swap(pool[i], pool[j]);
        }
        int take = std::min(amt, (int)pool.size());
        std::vector<CardInstance> taken;
        taken.reserve(take);
        for (int i = 0; i < take; ++i) taken.push_back(discardPile[pool[i]]);
        // remove the taken entries (by index, descending keeps earlier indices valid)
        std::vector<int> sortedIdx(pool.begin(), pool.begin() + take);
        std::sort(sortedIdx.rbegin(), sortedIdx.rend());
        for (int idx : sortedIdx) discardPile.erase(discardPile.begin() + idx);
        for (auto& c : taken) {
            if ((int)hand.size() < maxHandSize) {
                upgradeCardInstance(c);
                hand.push_back(c);
            } else {
                discardPile.push_back(c); // hand full: pulled card stays in discard
            }
        }
    }

    // WraithFormPower.AfterSideTurnStart: lose Amount dexterity every turn
    if (int amt = powerAmount(player.powers, PowerId::WRAITH_FORM))
        applyPowerToPlayer(PowerId::DEXTERITY, -amt);

    // AfterSideTurnStart: DemonForm gains Amount strength every turn
    if (int amt = powerAmount(player.powers, PowerId::DEMON_FORM)) {
        if (amt > 0) applyPowerToPlayer(PowerId::STRENGTH, amt);
        else if (amt < 0) applyPowerToPlayer(PowerId::STRENGTH, amt);
    }
    // AfterPlayerTurnStart: Inferno self-burn (misc carries the SelfDamage dynvar);
    // CrimsonMantle self-burn then gains Amount block
    if (int amt = powerAmount(player.powers, PowerId::INFERNO)) {
        int selfDmg = 1;
        for (auto& p : player.powers)
            if (p.id == PowerId::INFERNO && p.misc > 0) selfDmg = p.misc;
        damagePlayerUnblockable(selfDmg);
    }
    if (int amt = powerAmount(player.powers, PowerId::CRIMSON_MANTLE)) {
        int selfDmg = 1;
        for (auto& p : player.powers)
            if (p.id == PowerId::CRIMSON_MANTLE && p.misc > 0) selfDmg = p.misc;
        damagePlayerUnblockable(selfDmg);
        gainBlock(amt);
    }

    // // AfterPlayerTurnStart: LoopPower fires the FRONT orb's passive once per stack
    if (int amt = powerAmount(player.powers, PowerId::LOOP)) {
        for (int i = 0; i < amt && !battleOver; ++i) {
            if (player.orbs.empty()) break;
            triggerOrbPassive(player.orbs.front());
        }
    }

    // PrepTimePower.AfterSideTurnStart: applies Vigor equal to its stacks
    if (int amt = powerAmount(player.powers, PowerId::PREP_TIME)) applyPowerToPlayer(PowerId::VIGOR, amt);

    // // AfterSideTurnStart: CoolantPower gains block = distinct orb kinds * stacks
    if (int amt = powerAmount(player.powers, PowerId::COOLANT)) {
        int kinds = distinctOrbKinds();
        if (kinds > 0) gainBlockUnpowered(kinds * amt);
    }

    // // AfterSideTurnStart: NoxiousFumesPower poisons every hittable enemy by its stacks
    if (int amt = powerAmount(player.powers, PowerId::NOXIOUS_FUMES)) {
        for (int i = 0; i < (int)enemies.size(); ++i) {
            if (enemies[i].removed || enemies[i].curHp <= 0 || enemies[i].reviving) continue;
            applyPowerToEnemy(i, PowerId::POISON, amt);
        }
    }

    // // AfterSideTurnStart: BlurPower decrements (its ShouldClearBlock keeps this turn's
    // block from clearing; handled where block clears, this only ticks the stack)
    for (auto it = player.powers.begin(); it != player.powers.end();) {
        if (it->id == PowerId::BLUR) {
            if (--it->amount <= 0) it = player.powers.erase(it);
            else ++it;
        } else ++it;
    }

    // RollingBoulderPower.AfterPlayerTurnStart: each instance deals its Damage to all enemies
    for (size_t i = 0; i < player.powers.size() && !battleOver; ++i) {
        const PowerInst& pw = player.powers[i];
        if (pw.id != PowerId::ROLLING_BOULDER) continue;
        // AfterPlayerTurnStart deals the CURRENT Amount, then SetAmount(Amount + Damage).
        // The stamp is emitted at turn start BEFORE this hook, so the growth lands on the
        // following turn's stamp (T2 reads 10 = play 5 + AfterApplied 5; T3 reads 15).
        // The play leaves Amount at 10 (apply 5 + AfterApplied 5). AfterPlayerTurnStart then
        // deals the CURRENT Amount and grows it by the Damage dynvar; the FIRST such trigger
        // (the turn right after the play) does not grow, which is what makes the stamps read
        // 10 at T2 and 15 at T3 while the hits land 5 then 10 then 15.
        // Sequence proven by the stamps: play leaves Amount at 5; each AfterPlayerTurnStart
        // deals the CURRENT Amount then grows by the Damage dynvar, and the harness stamp is
        // taken after the hook — so T2 reads 10 (5 dealt) and T3 reads 15 (10 dealt).
        int dmg = pw.amount; // deal the current Amount, then grow
        for (int e = 0; e < (int)enemies.size() && !battleOver; ++e) {
            if (enemies[e].removed || enemies[e].curHp <= 0 || enemies[e].reviving) continue;
            damageEnemy(e, dmg, /*powered=*/false);
        }
        player.powers[i].amount += (pw.aux >= 0 ? pw.aux : 5);
    }

    // // OrbQueue.AfterTurnStart: Plasma orbs fire their passive (energy) at turn start
    orbAfterTurnStartTriggers();

    // BeforeHandDraw hook pass, in game listener order (powers before cards): the
    // InfiniteBladesPower shivs are created first, then each card's own BeforeHandDraw.

    // SpectrumShiftPower.BeforeHandDraw: shuffle the COLORLESS combat pool and take
    // Amount distinct cards into the hand (CardFactory.GetDistinctForCombat →
    // TakeRandom = full UnstableShuffle on the CombatCardGeneration stream).
    if (int n = powerAmount(player.powers, PowerId::SPECTRUM_SHIFT)) {
        int pn = 0;
        const CardId* pool = sts2CombatGenPool("COLORLESS", &pn);
        if (pool && pn > 0) {
            std::vector<CardId> opts(pool, pool + pn);
            Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
            for (int i = (int)opts.size() - 1; i > 0; --i) {
                int j = r.NextInt(i + 1);
                std::swap(opts[i], opts[j]);
            }
            for (int i = 0; i < n && i < (int)opts.size() && !battleOver; ++i) {
                if ((int)hand.size() >= maxHandSize) break;
                CardInstance nc;
                nc.id = opts[i];
                nc.uniqueIdx = ++nextUniqueIdx;
                nc.genCard = true;
                ++cardsGeneratedByOwner;
                hand.push_back(nc);
            }
        }
    }
    // CallOfTheVoidPower.BeforeHandDraw: for each of Amount cards the game calls
    // CardFactory.GetDistinctForCombat(..., 1, rng) — one FULL UnstableShuffle of the
    // character combat pool per card (monorail: 3 applications → 3 shuffles), and every
    // generated card then carries Ethereal.
    if (int n = powerAmount(player.powers, PowerId::CALL_OF_THE_VOID)) {
        int pn = 0;
        const CardId* pool = sts2CombatGenPool(characterPool.c_str(), &pn);
        if (pool && pn > 0) {
            Sts2Rng& r = rng[(int)RngStream::CombatCardGeneration];
            for (int k = 0; k < n && !battleOver; ++k) {
                if ((int)hand.size() >= maxHandSize) break;
                std::vector<CardId> opts(pool, pool + pn);
                for (int i = (int)opts.size() - 1; i > 0; --i) {
                    int j = r.NextInt(i + 1);
                    std::swap(opts[i], opts[j]);
                }
                CardInstance nc;
                nc.id = opts[0];
                nc.uniqueIdx = ++nextUniqueIdx;
                nc.genCard = true;
                nc.ephemeral = true; // CardCmd.ApplyKeyword(Ethereal)
                ++cardsGeneratedByOwner;
                hand.push_back(nc);
            }
        }
    }
    // SentryModePower.BeforeHandDraw: one SweepingGaze per stack into the hand
    if (int amt = powerAmount(player.powers, PowerId::SENTRY_MODE)) {
        for (int i = 0; i < amt && !battleOver; ++i) {
            if ((int)hand.size() >= maxHandSize) break;
            CardInstance gaze;
            gaze.id = CardId::SWEEPING_GAZE;
            gaze.uniqueIdx = ++nextUniqueIdx;
            gaze.genCard = true;
            ++cardsGeneratedByOwner;
            hand.push_back(gaze);
        }
    }
    // InfiniteBladesPower.BeforeHandDraw: creates Amount Shivs in hand before the draw
    if (int amt = powerAmount(player.powers, PowerId::INFINITE_BLADES)) {
        for (int i = 0; i < amt && !battleOver; ++i) {
            if ((int)hand.size() >= maxHandSize) break;
            CardInstance shiv;
            shiv.id = CardId::SHIV;
            shiv.uniqueIdx = ++nextUniqueIdx;
            shiv.genCard = true;
            ++cardsGeneratedByOwner;
            hand.push_back(shiv);
        }
    }

    // Bolas/ThrummingHatchet.BeforeHandDraw: each card checks its OWN history — it returns
    // to hand only if THIS instance was played last player turn (the played instance sits in
    // the discard pile by now; cards that were never played stay wherever they are). The
    // listener pass walks the piles in AllPiles order, so returns append to the hand in
    // (DrawPile → DiscardPile → ExhaustPile) order; a card already in hand stays in place.
    if (!bolasPlayedLastTurn.empty()) {
        std::vector<CardInstance> returning;
        for (auto* pile : {&drawPile, &discardPile, &exhaustPile}) {
            for (auto it = pile->begin(); it != pile->end();) {
                bool isPlayedInstance = false;
                for (int u : bolasPlayedLastTurn)
                    if (u == it->uniqueIdx) { isPlayedInstance = true; break; }
                if (isPlayedInstance) {
                    returning.push_back(*it);
                    it = pile->erase(it);
                    continue;
                }
                ++it;
            }
        }
        for (auto& c : returning) {
            if ((int)hand.size() < maxHandSize) hand.push_back(c);
            else discardPile.push_back(c); // CardPileCmd.Add: a full hand routes to discard
        }
    }

    // draw (DrawCardsNextTurnPower.ModifyHandDraw adds its stacks: Predator +2 next turn)
    // DemesnePower/ToolsOfTheTradePower.ModifyHandDraw: +Amount cards in the natural draw
    // MindRotPower.ModifyHandDraw: the KnowledgeDemon curse shrinks the natural draw
    // (clamped at 0).
    // Pocketwatch.ModifyHandDraw: +3 when at most 3 cards were played last turn (the
    // source returns count unchanged when the count is above the threshold).
    const int pocketwatchDraw = (hasRelic(RelicId::POCKETWATCH) && turn > 1
                                 && relicPocketwatchLastTurn <= 3) ? 3 : 0;
    const int pendulumDraw = (hasRelic(RelicId::PENDULUM) && relicPendulumTurns == 0) ? 1 : 0;
    drawCards(std::max(0, 5 + powerAmount(player.powers, PowerId::DRAW_CARDS_NEXT_TURN)
                + powerAmount(player.powers, PowerId::DEMESNE)
                + powerAmount(player.powers, PowerId::TOOLS_OF_THE_TRADE)
                + powerAmount(player.powers, PowerId::MACHINE_LEARNING)
                + powerAmount(player.powers, PowerId::TYRANNY)
                - powerAmount(player.powers, PowerId::MIND_ROT)
                + pocketwatchDraw + pendulumDraw), true);
    // AfterSideTurnStart: DrawCardsNextTurnPower leaves once it has been used
    player.powers.erase(std::remove_if(player.powers.begin(), player.powers.end(),
        [](const PowerInst& p) { return p.id == PowerId::DRAW_CARDS_NEXT_TURN; }),
        player.powers.end());

    // FurnacePower.AfterSideTurnStart: forge Amount; the Blade lands after the drawn
    // cards (corpus FURNACE T2: the Sovereign Blade is the last hand entry)
    if (int amt = powerAmount(player.powers, PowerId::FURNACE)) forge(amt);

    // TyrannyPower.AfterPlayerTurnStart: exhaust Amount cards chosen by the route.
    // Resolve every pick against the UNTOUCHED hand (sourceOccurrence counts same-id
    // copies from the hand start), then remove descending; the exhaust pile keeps the
    // selection order.
    if (powerAmount(player.powers, PowerId::TYRANNY) > 0) {
        const TurnStartChoice* req = takeTurnStartChoice("TYRANNY_POWER");
        if (req) {
            std::vector<int> hits;
            for (const auto& rc : req->cards) {
                int seen = 0;
                for (size_t hi = 0; hi < hand.size(); ++hi) {
                    if (std::strcmp(CardId_NAMES[(int)hand[hi].id], rc.cardId) != 0
                        || hand[hi].upgrade != rc.upgrade) continue;
                    if (seen == rc.sourceOccurrence) {
                        if (std::find(hits.begin(), hits.end(), (int)hi) == hits.end())
                            hits.push_back((int)hi);
                        break;
                    }
                    ++seen;
                }
            }
            // The pile keeps the SELECTION order (the route's card list), while removal
            // must go descending so index lookups stay valid.
            std::vector<CardInstance> picked;
            for (int h : hits) picked.push_back(hand[h]);
            std::vector<int> order = hits;
            std::sort(order.begin(), order.end());
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                hand.erase(hand.begin() + *it);
            for (auto& pc : picked) exhaustCard(pc); // selection order
        }
    }

    // ToolsOfTheTradePower.AfterPlayerTurnStart: discard Amount cards chosen by the
    // route. Same discipline as the CHOICE_SELECT_* ops: resolve every pick against the
    // UNTOUCHED hand, remove descending, and let the discard pile keep the selection
    // order (corpus T3/T4/T7).
    if (int tott = powerAmount(player.powers, PowerId::TOOLS_OF_THE_TRADE)) {
        const TurnStartChoice* req = takeTurnStartChoice("TOOLS_OF_THE_TRADE_POWER");
        if (req) {
            std::vector<int> hits;
            for (const auto& rc : req->cards) {
                int seen = 0;
                for (size_t hi = 0; hi < hand.size(); ++hi) {
                    if (std::strcmp(CardId_NAMES[(int)hand[hi].id], rc.cardId) != 0
                        || hand[hi].upgrade != rc.upgrade) continue;
                    if (seen == rc.sourceOccurrence) {
                        if (std::find(hits.begin(), hits.end(), (int)hi) == hits.end())
                            hits.push_back((int)hi);
                        break;
                    }
                    ++seen;
                }
            }
            // The pile keeps the SELECTION order (the route's card list), while removal
            // must go descending so index lookups stay valid.
            std::vector<CardInstance> picked;
            for (int h : hits) picked.push_back(hand[h]);
            std::vector<int> order = hits;
            std::sort(order.begin(), order.end());
            for (auto it = order.rbegin(); it != order.rend(); ++it)
                hand.erase(hand.begin() + *it);
            for (auto& pc : picked) { // selection order
                discardPile.push_back(pc);
                ++cardsDiscardedThisTurn;
            }
        } else if (tott > 0) {
            // no recorded choice: discard the first Amount cards
            for (int i = 0; i < tott && !hand.empty(); ++i) {
                discardPile.push_back(hand.front());
                ++cardsDiscardedThisTurn;
                hand.erase(hand.begin());
            }
        }
    }

    for (auto& c : hand) c.justGenerated = false;

    // AfterPlayerTurnStart hooks (fire after the draw, before the AutoPrePlay phase):
    // EntropyPower transforms Amount chosen cards in hand into random cards of the owner's
    // transformation pool (CardFactory.CreateRandomCardForTransform over
    // GetDefaultTransformationOptions, drawn on the CombatCardSelection stream). The harness
    // logs the chosen hand cards as a turnStartChoices record (sourceId=ENTROPY_POWER);
    // the replay driver queued it, so consume it here.
    if (int amt = powerAmount(player.powers, PowerId::ENTROPY)) {
        const TurnStartChoice* req = takeTurnStartChoice("ENTROPY_POWER");
        if (req) {
            // Resolve every chosen card against the UNTOUCHED hand first: sourceOccurrence
            // counts duplicates in the hand as it stood when the player chose, while a
            // transform replaces a card in place. Matching on the fly would make the second
            // of two identical picks (#1) search a hand that no longer holds #0.
            std::vector<int> hits;
            hits.reserve(req->cards.size());
            for (const auto& rc : req->cards) {
                int seen = 0, hit = -1;
                for (int i = 0; i < (int)hand.size(); ++i) {
                    if (std::strcmp(CardId_NAMES[(int)hand[i].id], rc.cardId) == 0
                        && hand[i].upgrade == rc.upgrade) {
                        if (seen == rc.sourceOccurrence) { hit = i; break; }
                        ++seen;
                    }
                }
                hits.push_back(hit);
            }
            // Transform in the game's order (CardCmd.TransformToRandom loops the chosen
            // list), one CombatCardSelection draw each.
            for (size_t k = 0; k < req->cards.size(); ++k) {
                int hit = hits[k];
                if (hit < 0) continue;
                // GetFilteredTransformationOptions keeps the ORIGINAL card's pool and
                // excludes the original's own id.
                std::vector<CardId> opts = transformOptionsFor(hand[hit].id, characterPool);
                if (opts.empty()) continue;
                Sts2Rng& r = rng[(int)RngStream::CombatCardSelection];
                CardId repl = opts[r.NextInt((int)opts.size())];
                CardInstance nc;
                nc.id = repl;
                nc.uniqueIdx = ++nextUniqueIdx;
                nc.genCard = true;
                ++cardsGeneratedByOwner; // CardCmd.Transform logs CardGeneratedEntry
                hand[hit] = nc; // transform replaces the card in place (same pile slot)
            }
        }
        (void)amt;
    }

    // AutoPrePlay phase (runs after the hand draw, before the player may act):
    // MayhemPower.AfterAutoPrePlayPhaseEntered -> CardPileCmd.AutoPlayFromDrawPile(amount, Top).
    // The hook fires ONCE per phase; the count is the power's Amount at that moment. The cards
    // are pulled off the TOP of the draw pile (shuffling the discard in if it runs dry) and
    // then auto-played for free, in order (no forced exhaust). A Mayhem played this way stacks
    // the power for NEXT turn — it does not re-run the phase.
    if (int amt = powerAmount(player.powers, PowerId::MAYHEM)) {
        std::vector<CardInstance> picked;
        for (int i = 0; i < amt; ++i) {
            if (drawPile.empty()) {
                if (discardPile.empty()) break;
                shuffleDrawFromDiscard();
            }
            if (drawPile.empty()) break;
            picked.push_back(drawPile.front());
            drawPile.erase(drawPile.begin());
        }
        for (auto& pc : picked) {
            if (battleOver) break;
            if (!autoplayCard(pc)) return;
        }
    }
}

} // namespace sts2
