#include "sts2/search/Sts2Search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace sts2::search {


namespace {

bool targetable(const Monster& m) {
    return !m.removed && m.curHp > 0 && !m.reviving;
}

// Two hand cards are interchangeable for search purposes when every value the play
// resolves from differs in no field. Keeping one edge per equivalence class keeps the
// branching factor at "distinct cards" instead of "copies in hand" (7 STRIKEs in a
// starter deck would otherwise be 7 identical subtrees).
bool sameActionCard(const CardInstance& a, const CardInstance& b, const BattleContext& bc) {
    if (a.id != b.id || a.upgrade != b.upgrade) return false;
    if (a.energyCost(bc) != b.energyCost(bc)) return false;
    if (a.starCost_() != b.starCost_()) return false;
    for (int i = 0; i < 4; ++i)
        if (a.varBump[i] != b.varBump[i]) return false;
    if (a.fakeUpgrade != b.fakeUpgrade) return false;
    return true;
}

} // namespace

namespace {

// Incoming damage the enemy side is about to deal this turn, read from the rolled intents
// (Monster.nextMove -> the state's damage x hits). Powers that scale the printed value are
// approximated by the monster's Strength; the exact figure is not needed for a prior.
int incomingThreat(const BattleContext& bc) {
    int threat = 0;
    for (const Monster& m : bc.enemies) {
        if (m.removed || m.curHp <= 0 || m.defIdx < 0) continue;
        const MonsterDef& md = m.def(bc);
        if (m.curState < 0 || m.curState >= static_cast<int>(md.states.size())) continue;
        const MoveState& st = md.states[m.curState];
        if (st.damage <= 0) continue;
        int strength = 0;
        for (const PowerInst& p : m.powers)
            if (p.id == PowerId::STRENGTH) strength += p.amount;
        int per = st.damage + strength;
        if (per < 0) per = 0;
        threat += per * (st.hits > 0 ? st.hits : 1);
    }
    return threat;
}

// Static rollout heuristic (action prior): score a legal action from its card's first
// attack/block step and the incoming threat. Biases the playout toward sensible orders
// without changing the game model; UCT still explores every expanded edge.
//
// Block is priced against the damage the enemy side has already committed to this turn:
// without that term a greedy playout always attacks and never defends, which caps the
// whole search at the "attack blindly" line (T3 budget sweep showed agreement flat from
// 100 to 16000 sims — a policy defect, not a budget one).
// Per-state constants the action heuristic needs. Hoisted out of the per-action loop:
// the playout scores every legal action at every step, so recomputing the threat roll-up
// and the player power scan per action dominated the cost (measured: +30% wall clock).
struct HeuristicCtx {
    int threat = 0;
    int uncovered = 0;
    int livingEnemies = 1;
    int strength = 0;
    int dexterity = 0;
};

HeuristicCtx makeHeuristicCtx(const BattleContext& bc) {
    HeuristicCtx ctx;
    ctx.threat = incomingThreat(bc);
    ctx.uncovered = std::max(0, ctx.threat - bc.player.block);
    int living = 0;
    for (const Monster& m : bc.enemies)
        if (!m.removed && m.curHp > 0) ++living;
    ctx.livingEnemies = living > 0 ? living : 1;
    for (const PowerInst& p : bc.player.powers) {
        if (p.id == PowerId::STRENGTH) ctx.strength += p.amount;
        if (p.id == PowerId::DEXTERITY) ctx.dexterity += p.amount;
    }
    return ctx;
}

double actionHeuristic(const BattleContext& bc, const HeuristicCtx& ctx,
                          const Action& a) {
    if (a.kind == Action::Kind::END_TURN) return 0.6;
    if (a.kind == Action::Kind::USE_POTION) return 4.0;
    if (a.handIdx < 0 || a.handIdx >= static_cast<int>(bc.hand.size())) return 0.5;
    const CardInstance& c = bc.hand[a.handIdx];
    const CardDef& d = c.def();

    auto stepValue = [&](const CardStep& st) {
        return st.varIdx >= 0 && st.varIdx < d.nVars ? cardVarValue(c, d, st.varIdx)
                                                     : st.constVal;
    };
    auto stepHits = [&](const CardStep& st) {
        if (st.hits > 0) return st.hits;
        if (st.hits < 0) return std::max(0, cardVarValue(c, d, -(st.hits) - 1));
        return 1;
    };

    // Block only pays for the part of the incoming damage not already covered by the block
    // this card would leave behind; excess block is worth a fraction.
    const int uncovered = ctx.uncovered;
    const int livingEnemies = ctx.livingEnemies;
    const int strength = ctx.strength;

    // ---- walk every step, not just the first attack / first block ----
    // The first-cut heuristic read one attack step and one block step, which scores a
    // multi-hit card as its per-hit damage and ignores debuffs, self-buffs, draw and
    // energy. Those are exactly the cards where a rollout most needs a prior.
    double dmgTarget = 0.0;   // damage to the chosen target
    double dmgAoe = 0.0;      // total damage across the enemy side (already multiplied)
    double blk = 0.0;
    double debuff = 0.0;
    double selfBuff = 0.0;
    double draw = 0.0, energy = 0.0, heal = 0.0, selfHurt = 0.0;

    for (int i = 0; i < d.nSteps; ++i) {
        const CardStep& st = d.steps[i];
        const int val = stepValue(st);
        switch (st.op) {
            case StepOp::ATTACK:
            case StepOp::ATTACK_CALC:
            case StepOp::ATTACK_FEED:
            case StepOp::ATTACK_BLOCK_AS_DAMAGE:
            case StepOp::ATTACK_HITS_CALC: {
                const double total = static_cast<double>(std::max(0, val))
                                     * static_cast<double>(stepHits(st));
                dmgTarget += total;
                if (st.op == StepOp::ATTACK_BLOCK_AS_DAMAGE) blk += total;
                break;
            }
            case StepOp::ATTACK_ALL: {
                const double total = static_cast<double>(std::max(0, val))
                                     * static_cast<double>(stepHits(st));
                dmgAoe += total * livingEnemies;
                break;
            }
            case StepOp::ATTACK_ALL_X:
            case StepOp::ATTACK_X_HITS: {
                // X = energy spent; assume the player spends what is left.
                const int x = std::max(1, bc.player.energy);
                const double total = static_cast<double>(std::max(0, val)) * x;
                if (st.op == StepOp::ATTACK_ALL_X) dmgAoe += total * livingEnemies;
                else dmgTarget += total;
                break;
            }
            case StepOp::ATTACK_RANDOM_HITS: {
                const double total = static_cast<double>(std::max(0, val)) * stepHits(st);
                dmgAoe += total;
                break;
            }
            case StepOp::THRASH_ABSORB: {
                dmgTarget += 2.0 * std::max(0, val);
                break;
            }
            case StepOp::GAIN_BLOCK:
            case StepOp::GAIN_BLOCK_CALC:
            case StepOp::GAIN_BLOCK_EXHAUST_DOUBLE:
                blk += std::max(0, val);
                break;
            case StepOp::PROLONG_BLOCK:
                blk += static_cast<double>(std::max(0, bc.player.block)) * 0.5;
                break;
            case StepOp::APPLY_POWER_ENEMY: {
                const double amount = std::max(0, val) * (st.sign < 0 ? -1.0 : 1.0);
                double w = 0.0;
                if (st.power == PowerId::VULNERABLE) w = 0.35;       // +50% taken, a few turns
                else if (st.power == PowerId::WEAK) w = 0.30;        // -25% dealt
                else if (st.power == PowerId::POISON) w = 0.80;      // damage over time
                else w = 0.15;
                debuff += amount * w;
                break;
            }
            case StepOp::APPLY_POWER_ALL_ENEMIES: {
                const double amount = std::max(0, val) * (st.sign < 0 ? -1.0 : 1.0);
                double w = 0.0;
                if (st.power == PowerId::VULNERABLE) w = 0.35;
                else if (st.power == PowerId::WEAK) w = 0.30;
                else if (st.power == PowerId::POISON) w = 0.80;
                else w = 0.15;
                debuff += amount * w * livingEnemies;
                break;
            }
            case StepOp::APPLY_POWER_SELF: {
                const double amount = std::max(0, val) * (st.sign < 0 ? -1.0 : 1.0);
                double w = 0.0;
                if (st.power == PowerId::STRENGTH) w = 0.80;         // permanent damage
                else if (st.power == PowerId::DEXTERITY) w = 0.60;   // permanent block
                else w = 0.40;
                selfBuff += amount * w;
                break;
            }
            case StepOp::VULN_DOUBLE: {
                // Doubles existing Vulnerable on the target.
                if (a.targetIdx >= 0 && a.targetIdx < static_cast<int>(bc.enemies.size())) {
                    for (const PowerInst& p : bc.enemies[a.targetIdx].powers)
                        if (p.id == PowerId::VULNERABLE) debuff += p.amount * 0.35;
                }
                break;
            }
            case StepOp::DOMINATE: {
                debuff += std::max(0, val) * 0.35;
                if (a.targetIdx >= 0 && a.targetIdx < static_cast<int>(bc.enemies.size())) {
                    int vuln = 0;
                    for (const PowerInst& p : bc.enemies[a.targetIdx].powers)
                        if (p.id == PowerId::VULNERABLE) vuln += p.amount;
                    selfBuff += vuln * 0.80; // strength += target's total vuln
                }
                break;
            }
            case StepOp::DRAW:
            case StepOp::DRAW_AND_RETAIN:
            case StepOp::DRAW_TO_MAX:
            case StepOp::DRAW_IF_NO_ATTACK:
            case StepOp::DRAW_UNTIL_NON_ATTACK:
                draw += std::max(0, val);
                break;
            case StepOp::GAIN_ENERGY:
                energy += std::max(0, val);
                break;
            case StepOp::GAIN_STARS:
                energy += std::max(0, val) * 0.5;
                break;
            case StepOp::HEAL_SELF:
                heal += std::max(0, val);
                break;
            case StepOp::SELF_DAMAGE:
                selfHurt += std::max(0, val);
                break;
            default:
                break;
        }
    }

    static const double kBlockDiv = [] {
        const char* v = std::getenv("STS2_BLOCK_W");
        return v ? std::atof(v) : 8.0;
    }();

    double score = 1.0;
    // Damage: single-target value first, then AoE at a discount (spreading damage does
    // not remove a threat as fast as focusing it).
    score += (dmgTarget + 0.6 * dmgAoe) / 8.0;
    if (a.targetIdx >= 0 && a.targetIdx < static_cast<int>(bc.enemies.size())) {
        const Monster& t = bc.enemies[a.targetIdx];
        const double effective = dmgTarget + strength * static_cast<double>(d.nSteps);
        if (t.curHp > 0 && t.curHp <= effective) score += 6.0; // lethal
    }
    // Defending is worth the damage it actually prevents this turn.
    score += std::min(blk, static_cast<double>(uncovered)) / kBlockDiv
             + std::max(0.0, blk - uncovered) / 40.0;
    score += debuff * 0.6 + selfBuff + draw * 0.5 + energy * 1.0 + heal * 0.5
             - selfHurt * 1.0;
    if (d.type == CardType::POWER) score += 2.5;
    score -= 0.15 * std::max(0, c.energyCost(bc));
    return std::max(0.05, score);
}


// Cheap variant for the rollout loop (also usable as the edge prior via
// STS2_PRIOR_HEUR=simple, which reproduces the pre-enrichment scoring for A/B runs). The playout scores every legal action at every
// step, so it is the hot path; the rich scoring above is kept for edge priors, which are
// computed once per edge. STS2_PLAYOUT_HEUR=rich uses the rich one everywhere.
double playoutHeuristic(const BattleContext& bc, const HeuristicCtx& ctx, const Action& a) {
    static const bool kSimple = [] {
        const char* v = std::getenv("STS2_PLAYOUT_HEUR");
        return v && std::string(v) == "simple";
    }();
    if (!kSimple) return actionHeuristic(bc, ctx, a);
    if (a.kind == Action::Kind::END_TURN) return 0.6;
    if (a.kind == Action::Kind::USE_POTION) return 4.0;
    if (a.handIdx < 0 || a.handIdx >= static_cast<int>(bc.hand.size())) return 0.5;
    const CardInstance& c = bc.hand[a.handIdx];
    const CardDef& d = c.def();
    int dmg = 0, blk = 0;
    for (int i = 0; i < d.nSteps; ++i) {
        const CardStep& st = d.steps[i];
        const int val = st.varIdx >= 0 && st.varIdx < d.nVars
            ? cardVarValue(c, d, st.varIdx) : st.constVal;
        if ((st.op == StepOp::ATTACK || st.op == StepOp::ATTACK_ALL) && dmg == 0) dmg = val;
        if (st.op == StepOp::GAIN_BLOCK && blk == 0) blk = val;
    }
    double score = 1.0;
    if (d.type == CardType::ATTACK) {
        score += dmg / 8.0;
        if (a.targetIdx >= 0 && a.targetIdx < static_cast<int>(bc.enemies.size())) {
            const Monster& t = bc.enemies[a.targetIdx];
            const int eff = dmg + ctx.strength * d.nSteps;
            if (t.curHp > 0 && t.curHp <= eff) score += 6.0;
        }
    } else if (d.type == CardType::SKILL) {
        static const double kBlockDiv = [] {
            const char* v = std::getenv("STS2_BLOCK_W");
            return v ? std::atof(v) : 8.0;
        }();
        score += std::min(blk, ctx.uncovered) / kBlockDiv
                 + std::max(0, blk - ctx.uncovered) / 40.0;
    } else if (d.type == CardType::POWER) {
        score += 2.5;
    }
    score -= 0.15 * std::max(0, c.energyCost(bc));
    return std::max(0.05, score);
}


} // namespace

std::vector<Action> enumerateActions(const BattleContext& bc, bool allowPotions) {
    std::vector<Action> out;
    enumerateActionsInto(bc, allowPotions, out);
    return out;
}

void enumerateActionsInto(const BattleContext& bc, bool allowPotions, std::vector<Action>& out) {
    out.clear();
    if (bc.battleOver) return;

    bool ringingActive = false;
    for (const auto& p : bc.player.powers)
        if (p.id == PowerId::RINGING && p.amount > 0) ringingActive = true;

    for (int i = 0; i < static_cast<int>(bc.hand.size()); ++i) {
        const CardInstance& c = bc.hand[i];
        const CardDef& d = c.def();
        if (cardHasFlag(c, CF_UNPLAYABLE)) continue;
        // RingingPower.ShouldPlay: only the first card play of the turn is allowed.
        if (ringingActive && c.affliction == 2 && bc.cardPlayStarts > 0) continue;
        // ChainsOfBindingPower.ShouldPlay: only one Bound card may be played per turn.
        if (c.affliction == 4 && bc.boundCardsPlayedThisTurn > 0) continue;

        const bool xCost = d.hasFlag(CF_ENERGY_X);
        if (!xCost && d.cost >= 0 && c.energyCost(bc) > bc.player.energy) continue;
        if (c.starCost_() > bc.player.stars) continue;

        bool duplicate = false;
        for (int j = 0; j < i && !duplicate; ++j)
            duplicate = sameActionCard(c, bc.hand[j], bc);
        if (duplicate) continue;

        if (d.target == CardTarget::ANY_ENEMY) {
            bool any = false;
            for (int t = 0; t < static_cast<int>(bc.enemies.size()); ++t) {
                if (!targetable(bc.enemies[t])) continue;
                any = true;
                out.push_back({Action::Kind::PLAY_CARD, i, t});
            }
            if (!any) continue;
        } else {
            out.push_back({Action::Kind::PLAY_CARD, i, -1});
        }
    }
    // Potions are free actions: one candidate per usable slot (per target for single-target
    // potions). Slots the player cannot reach (empty) contribute nothing. `allowPotions`
    // lets the caller hold potions for the fights where they decide the outcome (upstream's
    // ScumSearchAgent2 exposes the same switch as `allowPotions`).
    for (int slot = 0; slot < 3 && allowPotions; ++slot) {
        const int pid = bc.player.potions[slot];
        if (pid == 0) continue;
        if (potionTargetKind(pid) == 1) {
            for (int t = 0; t < static_cast<int>(bc.enemies.size()); ++t) {
                if (!targetable(bc.enemies[t])) continue;
                out.push_back({Action::Kind::USE_POTION, slot, t});
            }
        } else {
            out.push_back({Action::Kind::USE_POTION, slot, -1});
        }
    }
    out.push_back({Action::Kind::END_TURN, -1, -1});
}

bool applyAction(BattleContext& bc, const Action& a) {
    if (a.kind == Action::Kind::USE_POTION) return bc.usePotion(a.handIdx, a.targetIdx);
    if (bc.battleOver) return false;
    if (a.kind == Action::Kind::END_TURN) {
        bc.endTurn();
        return true;
    }
    if (a.handIdx < 0 || a.handIdx >= static_cast<int>(bc.hand.size())) return false;
    if (a.targetIdx >= static_cast<int>(bc.enemies.size())) return false;
    if (a.targetIdx >= 0 && !targetable(bc.enemies[a.targetIdx])) return false;
    bc.unsupportedCard.clear();
    bc.playCard(a.handIdx, a.targetIdx);
    return bc.unsupportedCard.empty();
}


bool BattleSearcher::isTerminal(const BattleContext& bc) { return bc.battleOver; }

// Terminal and capped evaluation, bounded to [-1, 1] — ported from
// sts-rl-agent BattleEvaluator::evaluateEndState (MIT).
//
// Upstream's comment explains the bound: UCT assumes a bounded reward, and an unbounded
// "100 * (35 + hp)" style score lets one lucky branch dominate exploration. The win term
// is post-combat HP over the ROOT's max HP, so it is monotonic in remaining HP and stays
// in [0, 1] even after healing or a Lizard-Tail-style revive.
//
// The loss term keeps a dense signal instead of a flat -1: minus the remaining enemy HP
// ratio relative to the root, so a rollout that nearly killed the encounter still ranks
// above one that barely scratched it. Minions (MinionPower: death never blocks the win
// condition) are weighted separately — the encounter leader stays the primary objective,
// but removing an existing minion must still be visible in a losing line.
double BattleSearcher::evaluateFrom(const BattleContext& root, const BattleContext& bc) {
    auto hpOf = [](const Monster& m) { return static_cast<double>(std::max(0, m.curHp)); };
    auto maxOf = [](const Monster& m) { return static_cast<double>(std::max(0, m.maxHp)); };

    if (bc.battleOver && bc.playerWon) {
        const double rootMaxHp = static_cast<double>(std::max(1, root.player.maxHp));
        const double left = static_cast<double>(std::max(1, bc.player.curHp));
        return std::clamp(left / rootMaxHp, 0.0, 1.0);
    }

    double rootNonMinionMax = 0.0, rootMinionMax = 0.0;
    double nowNonMinionHp = 0.0, nowMinionHp = 0.0;
    int rootLivingMinions = 0, nowLivingMinions = 0;
    for (const Monster& m : root.enemies) {
        if (m.removed) continue;
        if (m.defIdx >= 0 && m.def(root).minion) {
            rootMinionMax += maxOf(m);
            ++rootLivingMinions;
        } else {
            rootNonMinionMax += maxOf(m);
        }
    }
    for (const Monster& m : bc.enemies) {
        if (m.removed) continue;
        if (m.defIdx >= 0 && m.def(bc).minion) {
            nowMinionHp += hpOf(m);
            ++nowLivingMinions;
        } else {
            nowNonMinionHp += hpOf(m);
        }
    }
    if (rootNonMinionMax <= 0.0 && rootMinionMax <= 0.0) return -1.0;

    const double nonMinionRatio = rootNonMinionMax > 0.0
        ? std::clamp(nowNonMinionHp / rootNonMinionMax, 0.0, 1.0) : 0.0;
    if (rootMinionMax <= 0.0) return -nonMinionRatio;

    const double minionHpRatio = std::clamp(nowMinionHp / rootMinionMax, 0.0, 1.0);
    const double minionAliveRatio = rootLivingMinions > 0
        ? std::clamp(static_cast<double>(nowLivingMinions) /
                     static_cast<double>(rootLivingMinions), 0.0, 1.0)
        : minionHpRatio;
    // Dense HP-progress signal while minions live, plus a bounded bonus for actually
    // removing one (upstream kMinionKillBonus).
    constexpr double kMinionKillBonus = 0.25;
    const double minionRatio = std::clamp(
        minionHpRatio - kMinionKillBonus * (1.0 - minionAliveRatio), 0.0, 1.0);
    if (rootNonMinionMax <= 0.0) return -minionRatio;

    // Upstream kExistingMinionWeight: the encounter leader dominates, minions carry 0.30.
    constexpr double kExistingMinionWeight = 0.30;
    const double hpRatio = (1.0 - kExistingMinionWeight) * nonMinionRatio
                           + kExistingMinionWeight * minionRatio;
    return -std::clamp(hpRatio, 0.0, 1.0);
}

double BattleSearcher::evaluate(const BattleContext& bc) { return evaluateFrom(bc, bc); }

BattleSearcher::BattleSearcher(const BattleContext& root, std::uint64_t seed, int maxTurns)
    : root_(root), rng_(seed), maxTurns_(maxTurns) {
    rootNode_.stats.count = 1;
    result_.outcomeHp = root_.player.curHp;
}

void BattleSearcher::search(long long simulations, long timeBudgetMs) {
    if (isTerminal(root_)) {
        // Terminal root: nothing to search, report the state as its own best line.
        const double v = evaluateFrom(root_, root_);
        result_.bestValue = v;
        result_.reachedTerminal = true;
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    for (long long i = 0; i < simulations; ++i) {
        // Upstream cadence: check every 100 simulations, and always run the first so a
        // zero or tiny budget still yields a usable root tree.
        if (timeBudgetMs > 0 && i > 0 && i % 100 == 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            if (elapsed.count() >= timeBudgetMs) break;
        }
        step();
    }
    result_.simulations += simulations;
}

void BattleSearcher::step() {
    std::vector<Node*> stack{&rootNode_};
    std::vector<int> edgeIdx;
    std::vector<Action> actions;

    // Selection: walk the tree by statistics only. Every expanded node was verified
    // non-terminal when created, and each walk edge was applied successfully when it
    // was created, so selection needs no state.
    Node* node = &rootNode_;
    while (!node->edges.empty()) {
        const int idx = selectBestEdge(*node);
        actions.push_back(node->edges[idx].action);
        stack.push_back(node->edges[idx].child.get());
        edgeIdx.push_back(idx);
        node = node->edges[idx].child.get();
    }

    // Materialize from the deepest snapshot this path OWNS (the edge whose scratch
    // became the child's snap), then replay the remaining suffix of edges exactly as
    // the pre-snapshot code did. Behaviour-identical by construction: the suffix starts
    // from the state its creator derived, and every apply below it is the same
    // deterministic call the full replay used to make.
    int k = static_cast<int>(edgeIdx.size()) - 1;
    while (k >= 0 && !stack[k]->edges[edgeIdx[k]].ownsSnap) --k;
    if (k >= 0) work_ = *stack[k + 1]->snap;
    else work_ = root_;
    for (int j = k + 1; j < static_cast<int>(edgeIdx.size()); ++j) {
        if (!applyAction(work_, stack[j]->edges[edgeIdx[j]].action)) {
            updateFromPlayout(stack, edgeIdx, actions, work_);
            return;
        }
    }

    if (isTerminal(work_) || work_.turn > maxTurns_) {
        updateFromPlayout(stack, edgeIdx, actions, work_);
        return;
    }
    expand(*node, work_);
    if (node->edges.empty()) {
        updateFromPlayout(stack, edgeIdx, actions, work_);
        return;
    }
    const int idx = selectExpansionEdge(*node);
    actions.push_back(node->edges[idx].action);
    Edge& e = node->edges[idx];
    if (e.ownsSnap) {
        work_ = *e.child->snap;   // this edge produced the snapshot: safe to load
    } else {
        if (!applyAction(work_, e.action)) {
            updateFromPlayout(stack, edgeIdx, actions, work_);
            return;
        }
        if (!e.child->snap) {
            e.child->snap = std::make_unique<BattleContext>(work_);
            e.ownsSnap = true;
        }
    }
    stack.push_back(e.child.get());
    edgeIdx.push_back(idx);
    playout(work_, actions);
    updateFromPlayout(stack, edgeIdx, actions, work_);
}

void BattleSearcher::playout(BattleContext& state, std::vector<Action>& actions) {
    while (!isTerminal(state)) {
        if (state.turn > maxTurns_) return;
        enumerateActionsInto(state, allowPotions_, playoutActs_);
        const std::vector<Action>& acts = playoutActs_;
        if (acts.empty()) return;

        // Rollout policy, ported from BattleScumSearcher2::playoutRandom (MIT): mostly
        // the best heuristic action, but a bounded fraction of uniformly random
        // non-END_TURN moves. Upstream's 0.12 exists so a playout cannot lock onto one
        // line forever; without it every rollout of a node repeats the same greedy path
        // and the simulation budget buys far less information.
        static const double kRandomRolloutChance = [] {
            const char* v = std::getenv("STS2_ROLLOUT_RANDOM");
            return v ? std::atof(v) : 0.12;
        }();
        int nonEndTurn = 0;
        for (const Action& a : acts)
            if (a.kind != Action::Kind::END_TURN) ++nonEndTurn;
        std::size_t pick = 0;
        if (nonEndTurn > 0 && std::uniform_real_distribution<double>(0.0, 1.0)(rng_)
                                  < kRandomRolloutChance) {
            std::uniform_int_distribution<int> dist(0, nonEndTurn - 1);
            int rank = dist(rng_);
            for (std::size_t i = 0; i < acts.size(); ++i) {
                if (acts[i].kind == Action::Kind::END_TURN) continue;
                if (rank-- == 0) { pick = i; break; }
            }
        } else {
            // Only the greedy branch needs the state roll-up; the random branch adopts
            // whichever non-END_TURN action it drew.
            const HeuristicCtx hctx = makeHeuristicCtx(state);
            double best = -1e300;
            int ties = 0;
            for (std::size_t i = 0; i < acts.size(); ++i) {
                const double s = playoutHeuristic(state, hctx, acts[i]);
                if (s > best + 1e-12) { best = s; pick = i; ties = 1; }
                else if (std::fabs(s - best) <= 1e-12) {
                    // Upstream pickBestTiedIndex: break ties at random so equal-scored
                    // actions share the budget instead of always taking the first.
                    ++ties;
                    if (std::uniform_int_distribution<int>(0, ties - 1)(rng_) == 0) pick = i;
                }
            }
        }
        const Action a = acts[pick];
        actions.push_back(a);
        if (!applyAction(state, a)) return;
    }
}

void BattleSearcher::updateFromPlayout(const std::vector<Node*>& stack,
                                       const std::vector<int>& edgeIdx,
                                       const std::vector<Action>& actions,
                                       const BattleContext& endState) {
    // Evaluate against the fixed root, not against the end state itself: UCT needs one
    // denominator for every rollout in a search.
    const double value = evaluateFrom(root_, endState);

    if (endState.battleOver && endState.playerWon) {
        // Keep the highest-scoring complete win seen (upstream bestActionSequence).
        if (value > bestWinValue_) {
            bestWinValue_ = value;
            result_.bestSequence = actions;
            result_.bestValue = value;
            result_.outcomeHp = endState.player.curHp;
            result_.reachedTerminal = true;
        }
    } else if (!result_.reachedTerminal && value > bestCappedValue_) {
        // No winning line yet: fall back to the best positional line so the caller still
        // has something to play.
        bestCappedValue_ = value;
        result_.bestSequence = actions;
        result_.bestValue = value;
        result_.outcomeHp = endState.player.curHp;
    }
    if (value < minValue_) minValue_ = value;
    if (value > maxValue_) maxValue_ = value;

    for (Node* n : stack) n->stats.record(value);
    // Edge statistics drive root balancing and the progressive-bias denominator.
    for (std::size_t depth = 1; depth < stack.size(); ++depth) {
        Node& parent = *stack[depth - 1];
        const int idx = edgeIdx[depth - 1];
        if (idx >= 0 && idx < static_cast<int>(parent.edges.size()))
            parent.edges[idx].stats.record(value);
    }
}

// ---------- transposition key ----------
//
// Ported in spirit from sts-rl-agent BattleScumSearcher2::buildStateKey (MIT), which hashes
// fields rather than building a string. Two deliberate differences:
//
//  * upstream gates on `actionQueue.size == 0 && cardQueue.size == 0 && inputState ==
//    PLAYER_NORMAL`; the STS2 equivalent of those queues is pendingChoice /
//    pendingTurnStartChoices, and a state that still owes a choice is not a clean decision
//    point, so it gets no key at all;
//  * upstream hashes its six RNG streams. We hash all of ours, because STS2 draw order and
//    enemy intent rolls come out of them and two states that differ only in RNG state can
//    diverge on the very next action.
//
// Only fields that decide future play are included. Bookkeeping the engine carries for
// stamping/diagnostics is left out on purpose: including it would split identical positions
// and defeat the table.

namespace {

inline void mix(std::uint64_t& h, std::uint64_t v) {
    // splitmix64 finaliser: cheap and well distributed for sequentially structured input.
    v += 0x9E3779B97F4A7C15ULL;
    v = (v ^ (v >> 30)) * 0xBF58476D1CE4E5B9ULL;
    v = (v ^ (v >> 27)) * 0x94D049BB133111EBULL;
    h ^= v + (h << 6) + (h >> 2);
}

inline void mixInt(std::uint64_t& h, int v) {
    mix(h, static_cast<std::uint64_t>(static_cast<std::int64_t>(v)));
}
inline void mixBool(std::uint64_t& h, bool v) { mix(h, v ? 0x9E37ULL : 0x1F2EULL); }

// Card identity as it matters to future play: id, upgrade, cost overrides, var bumps and
// the flags that change what the card does. uniqueIdx is deliberately excluded — it is an
// instance id, and two otherwise identical cards are interchangeable for search.
inline void mixCard(std::uint64_t& h, const CardInstance& c) {
    mixInt(h, static_cast<int>(c.id));
    mixInt(h, c.upgrade);
    mixInt(h, c.costOverride);
    mixInt(h, c.starCostOverride);
    mixBool(h, c.freeThisTurn);
    mixBool(h, c.exhaust);
    mixBool(h, c.retain);
    mixBool(h, c.retainThisTurn);
    mixBool(h, c.ephemeral);
    mixBool(h, c.sly);
    mixInt(h, c.fakeUpgrade);
    mixInt(h, c.affliction);
    mixInt(h, c.afflictionAmount);
    mixInt(h, c.extraReplays);
    mixInt(h, c.enchant);
    for (int i = 0; i < 4; ++i) mixInt(h, c.varBump[i]);
    mixInt(h, c.privateAccum);
}

inline void mixPile(std::uint64_t& h, const std::vector<CardInstance>& pile) {
    // Order matters: draw order decides future hands.
    mixInt(h, static_cast<int>(pile.size()));
    for (const CardInstance& c : pile) mixCard(h, c);
}

inline void mixPowers(std::uint64_t& h, const std::vector<PowerInst>& powers) {
    mixInt(h, static_cast<int>(powers.size()));
    for (const PowerInst& p : powers) {
        mixInt(h, static_cast<int>(p.id));
        mixInt(h, p.amount);
    }
}

} // namespace

bool BattleSearcher::stateKey(const BattleContext& bc, std::uint64_t& out) {
    // A queued choice means the state is mid-resolution: merging it would let an edge from
    // one selection context be reused in another.
    if (bc.pendingChoice.valid || !bc.pendingTurnStartChoices.empty()) return false;
    if (bc.battleOver) return false;

    std::uint64_t h = 0xC0FFEEULL;
    mixInt(h, bc.turn);

    // ---- player ----
    mixInt(h, bc.player.curHp);
    mixInt(h, bc.player.maxHp);
    mixInt(h, bc.player.block);
    mixInt(h, bc.player.energy);
    mixInt(h, bc.player.maxEnergy);
    mixInt(h, bc.player.stars);
    mixInt(h, bc.player.orbCapacity);
    mixPowers(h, bc.player.powers);
    for (int i = 0; i < 3; ++i) mixInt(h, bc.player.potions[i]);
    mixInt(h, static_cast<int>(bc.player.orbs.size()));
    for (const OrbInstance& o : bc.player.orbs) {
        mixInt(h, static_cast<int>(o.kind));
        mixInt(h, o.darkEvoke);
        mixInt(h, o.glassPassive);
        mixInt(h, o.upgrades);
    }
    mixBool(h, bc.player.osty.present);
    if (bc.player.osty.present) {
        mixInt(h, bc.player.osty.curHp);
        mixInt(h, bc.player.osty.maxHp);
        mixPowers(h, bc.player.osty.powers);
    }

    // ---- enemies: order matters (targets are indices) ----
    mixInt(h, static_cast<int>(bc.enemies.size()));
    for (const Monster& m : bc.enemies) {
        mixBool(h, m.removed);
        mixInt(h, m.combatId);
        mixInt(h, m.curHp);
        mixInt(h, m.maxHp);
        mixInt(h, m.block);
        mixInt(h, m.curState);
        mixInt(h, m.hitsTakenThisTurn);
        mixBool(h, m.reviving);
        mixBool(h, m.gate);
        mixPowers(h, m.powers);
        // The rolled intent is part of the state: the same HP at the same turn with a
        // different next move is a different decision.
        for (char ch : m.nextMove) mixInt(h, static_cast<unsigned char>(ch));
    }

    // ---- card piles ----
    mixPile(h, bc.hand);
    mixPile(h, bc.drawPile);
    mixPile(h, bc.discardPile);
    mixPile(h, bc.exhaustPile);

    // ---- per-turn counters that gate relic/power behaviour ----
    mixInt(h, bc.relicAttacksThisTurn);
    mixInt(h, bc.relicSkillsThisTurn);
    mixInt(h, bc.relicHappyTurns);
    mixInt(h, bc.relicPenNibAttacks);
    mixInt(h, bc.relicNunchakuAttacks);
    mixInt(h, bc.relicPocketwatchThisTurn);
    mixInt(h, bc.relicTuningForkSkills);
    mixInt(h, bc.relicRainbowAttacks);
    mixInt(h, bc.relicRainbowSkills);
    mixBool(h, bc.relicAoWThisTurn);
    mixBool(h, bc.relicAoWLastTurn);
    mixBool(h, bc.redSkullActive);
    mixInt(h, bc.relicBeatingDamageThisTurn);

    // ---- RNG: a state that differs only here diverges on the next draw ----
    for (const Sts2Rng& r : bc.rng) {
        const Sts2Rng::Serializable s = r.CaptureState();
        mixInt(h, s.counter);
        mix(h, s.state.s0);
        mix(h, s.state.s1);
        mix(h, s.state.s2);
        mix(h, s.state.s3);
    }

    out = h;
    return true;
}

void BattleSearcher::expand(Node& node, const BattleContext& bc) {
    // Transposition table: the child node for a state is created once and shared, so two
    // action orders that reach the same position (A then B vs B then A on one turn) pool
    // their simulations instead of each learning the position from scratch.
    static const bool kDedup = [] {
        const char* v = std::getenv("STS2_DEDUP");
        return !(v && std::string(v) == "0");
    }();
    const HeuristicCtx hctx = makeHeuristicCtx(bc);
    enumerateActionsInto(bc, allowPotions_, expandActs_);
    for (const Action& a : expandActs_) {
        // Copy-assign into the reused buffer: steady state hits reserved capacity
        // instead of allocating a fresh context per candidate edge. The resulting
        // state only feeds the transposition key; the child snapshot is created at
        // first traversal from work_, so nothing here outlives the loop.
        expandScratch_ = bc;
        if (!applyAction(expandScratch_, a)) continue;
        const BattleContext& scratch = expandScratch_;
        Edge e;
        e.action = a;
        static const bool kRichPrior = [] {
            const char* v = std::getenv("STS2_PRIOR_HEUR");
            return !(v && std::string(v) == "simple");
        }();
        e.prior = kRichPrior ? actionHeuristic(bc, hctx, a)
                             : playoutHeuristic(bc, hctx, a);
        if (kDedup) {
            std::uint64_t key = 0;
            // Only clean decision points get merged; a state still owing a card choice is
            // left as its own node.
            if (stateKey(scratch, key)) {
                const auto it = transposition_.find(key);
                if (it != transposition_.end()) {
                    e.child = it->second;
                } else {
                    transposition_.emplace(key, e.child);
                }
            }
        }
        node.edges.push_back(std::move(e));
    }
}

double BattleSearcher::edgeScore(const Node& parent, int idx) const {
    const Edge& e = parent.edges[idx];
    // Unexplored edges must outrank any explored one (upstream
    // unexploredNodeValueParameter = 100).
    if (e.stats.count == 0) return 100.0;
    const double mean = e.stats.sum / static_cast<double>(e.stats.count);
    const double span = maxValue_ - minValue_;
    const double quality = span > 1e-9 ? (mean - minValue_) / span : 0.0;
    // Upstream explorationParameter = sqrt(2); the factor 3 is the T3-tuned value that
    // matched the frozen solver on the 65-position corpus.
    const double explore = 3.0 * std::sqrt(2.0) *
        std::sqrt(std::log(static_cast<double>(parent.stats.count) + 1.0) /
                  static_cast<double>(e.stats.count));
    // Progressive bias (upstream progressiveBiasParameter = 0.35): the static prior pulls
    // early visits toward sensible actions, and decays as the edge accumulates samples.
    static const double kBias = [] {
        const char* v = std::getenv("STS2_PROGRESSIVE_BIAS");
        return v ? std::atof(v) : 0.35;
    }();
    const double bias = kBias * e.prior / static_cast<double>(e.stats.count + 1);
    return quality + explore + bias;
}

// Root action selection: least-visited edge first (upstream selectBestEdgeToSearch's root
// branch). Every root action gets a comparable sample count before the comparison that
// actually decides the move is made; below the root, plain UCT. STS2_ROOT_BALANCE=0
// disables it for A/B runs.
int BattleSearcher::selectBestEdge(const Node& node) {
    static const bool kBalanceRoot = [] {
        const char* v = std::getenv("STS2_ROOT_BALANCE");
        return !(v && std::string(v) == "0");
    }();
    if (kBalanceRoot && &node == &rootNode_ && node.edges.size() > 1) {
        int least = 0;
        for (int i = 1; i < static_cast<int>(node.edges.size()); ++i)
            if (node.edges[i].stats.count < node.edges[least].stats.count) least = i;
        return least;
    }
    int best = 0;
    double bestScore = edgeScore(node, 0);
    for (int i = 1; i < static_cast<int>(node.edges.size()); ++i) {
        const double s = edgeScore(node, i);
        if (s > bestScore) { best = i; bestScore = s; }
    }
    return best;
}

// Leaf first move: the highest static prior (upstream selectFirstActionForLeafNode),
// ties broken at random. STS2_LEAF_PRIOR=0 restores uniform selection.
int BattleSearcher::selectExpansionEdge(const Node& node) {
    static const bool kUsePrior = [] {
        const char* v = std::getenv("STS2_LEAF_PRIOR");
        return !(v && std::string(v) == "0");
    }();
    if (!kUsePrior || node.edges.size() < 2) {
        std::uniform_int_distribution<int> dist(0, static_cast<int>(node.edges.size()) - 1);
        return dist(rng_);
    }
    double best = -1e300;
    int pick = 0;
    int ties = 0;
    for (int i = 0; i < static_cast<int>(node.edges.size()); ++i) {
        const double s = node.edges[i].prior;
        if (s > best + 1e-12) { best = s; pick = i; ties = 1; }
        else if (std::fabs(s - best) <= 1e-12) {
            ++ties;
            if (std::uniform_int_distribution<int>(0, ties - 1)(rng_) == 0) pick = i;
        }
    }
    return pick;
}

} // namespace sts2::search
