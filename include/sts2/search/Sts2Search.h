// STS2 battle search — full structural port of sts_lightspeed / sts-rl-agent
// BattleScumSearcher2 onto sts2::BattleContext.
//
// The STS1 searcher is bound to the STS1 BattleContext shape (fixed arrays, InputState
// machine, Action bit-packing). This port keeps its search structure intact and adapts
// only the parts that are model-specific:
//   PLAY_CARD(handIdx, targetIdx) | USE_POTION(slot, targetIdx) | END_TURN
//
// Ported verbatim in structure (each item marked in Sts2Search.cpp with the upstream
// function it came from):
//   - node/edge both carry rollout statistics (PlayoutStats); UCT reads the edge
//   - bounded terminal evaluation: win in [0,1] = post-combat HP / root max HP;
//     loss in [-1,0] = -(remaining enemy HP ratio), minions weighted 0.30
//   - playout: 88% best-by-heuristic, 12% uniformly random non-END_TURN action
//   - progressive bias: explorationParameter * sqrt(log(N+1)/(n+1)) plus
//     progressiveBiasParameter * heuristicPrior / (n+1)
//   - root: least-visited edge first (balanced root sampling), UCT below the root
//   - bestActionSequence keeps the highest-scoring complete line seen
//   - time budget and convergence early-exit
//
// Still not ported (recorded, not hidden):
//   - transposition table: upstream keys it with a hand-written field hash; here the only
//     complete key available is emitStamp(), which measured 5.2x wall clock for +0.5
//     floors and was rejected (docs/ledger.md 2026-10-21)
//   - card-selection branches are not enumerated; effects resolve with the route-recorded
//     choice if one was pushed, otherwise with the engine's first-candidate fallback
//   - potion effects outside the implemented set are no-ops (the action is still legal)
//   - per-node snapshots: upstream does not keep them either (its step() also replays
//     from rootState), so this matches upstream rather than falling short of it

#ifndef STS2_SEARCH_H
#define STS2_SEARCH_H

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <random>
#include <vector>

#include "sts2/Sts2Battle.h"

namespace sts2::search {

struct Action {
    enum class Kind : std::uint8_t { PLAY_CARD, USE_POTION, END_TURN } kind = Kind::END_TURN;
    int handIdx = -1;   // PLAY_CARD: hand index; USE_POTION: potion slot
    int targetIdx = -1;

    bool operator==(const Action& o) const {
        return kind == o.kind && handIdx == o.handIdx && targetIdx == o.targetIdx;
    }
};

// All legal actions in the given state. END_TURN is always present.
std::vector<Action> enumerateActions(const BattleContext& bc, bool allowPotions = true);
// Same enumeration into a caller-owned buffer (search hot path: no per-step allocation).
void enumerateActionsInto(const BattleContext& bc, bool allowPotions, std::vector<Action>& out);

// Apply one action in place. Returns false when the action was rejected (unplayable card,
// unsupported effect program); the state may then hold side effects and must be discarded.
bool applyAction(BattleContext& bc, const Action& a);

struct SearchResult {
    std::vector<Action> bestSequence;
    double bestValue = 0;
    int outcomeHp = 0;
    bool reachedTerminal = false;
    long long simulations = 0;
};

class BattleSearcher {
public:
    BattleSearcher(const BattleContext& root, std::uint64_t seed = 1, int maxTurns = 50);

    // Runs up to `simulations` rollouts. `timeBudgetMs` <= 0 means no wall-clock cap;
    // when set, the loop checks it every 100 simulations (upstream's cadence).
    void search(long long simulations, long timeBudgetMs = 0);

    // When false the search ignores potion actions entirely, so a held potion is still
    // available for the fight that decides the run (upstream ScumSearchAgent2::allowPotions).
    bool allowPotions_ = true;

    const SearchResult& result() const { return result_; }
    const BattleContext& rootState() const { return root_; }

    // Terminal-state score in [-1, 1] (upstream BattleEvaluator::evaluateEndState):
    // a win is post-combat HP over the root's max HP, clamped to [0, 1]; a loss is minus
    // the remaining enemy HP ratio (the encounter leader dominates, existing minions
    // carry 0.30). A capped playout falls back to the same loss formula.
    static double evaluate(const BattleContext& bc);
    // Same evaluation but relative to a fixed root state, which is what UCT needs: the
    // denominator must not change between rollouts of one search.
    static double evaluateFrom(const BattleContext& root, const BattleContext& bc);
    static bool isTerminal(const BattleContext& bc);

private:
    // Rollout statistics carried by both nodes and edges (upstream PlayoutStats keeps
    // them identical so the backup path and the root shortcuts agree).
    struct Stats {
        long long count = 0;
        double sum = 0;
        double maxValue = -1e300;
        void record(double v) {
            ++count;
            sum += v;
            if (v > maxValue) maxValue = v;
        }
    };

    struct Edge;
    struct Node {
        Stats stats;
        std::vector<Edge> edges;
        // State captured when this node was first reached (expansion's scratch, moved
        // in). Lets a simulation materialize at the leaf instead of replaying the whole
        // action prefix from the root every step. Root uses root_ instead.
        // Behaviour-identical by construction: the snapshot holds exactly the state the
        // replay used to re-derive, including the battle RNG counters.
        std::unique_ptr<BattleContext> snap;
        // (ownership marker lives on Edge::ownsSnap)
    };
    struct Edge {
        Action action;
        // Static prior from the action heuristic, computed once at expansion. Feeds the
        // progressive-bias term and the leaf first-move choice.
        double prior = 0.0;
        Stats stats;
        // Shared so two action orders reaching the same position can point at one node
        // (transposition table).
        std::shared_ptr<Node> child = std::make_shared<Node>();
        // True when THIS edge's scratch became child->snap. Snapshots are only valid
        // materialization points for paths that arrive through their owning edge;
        // transposition-merged children are reached through edges that do not own the
        // snapshot, so the walk replays the short suffix instead of loading it.
        bool ownsSnap = false;
    };

    void step();
    void playout(BattleContext& state, std::vector<Action>& actions);
    // Reused across simulations so vector assignment hits existing capacity instead of
    // allocating per step (the per-sim clone is the last big allocator on the hot path).
    BattleContext work_;
    BattleContext expandScratch_;   // capacity-reused per expand edge (key computation only)
    std::vector<Action> playoutActs_;
    std::vector<Action> expandActs_;
    void updateFromPlayout(const std::vector<Node*>& stack,
                           const std::vector<int>& edgeIdx,
                           const std::vector<Action>& actions,
                           const BattleContext& endState);
    void expand(Node& node, const BattleContext& bc);
    double edgeScore(const Node& parent, int edgeIdx) const;
    int selectBestEdge(const Node& node);
    int selectExpansionEdge(const Node& node);

    // Transposition key for a player decision state, built incrementally from the fields
    // that decide future play (see Sts2Search.cpp). Returns false when the state is not a
    // clean decision point (a pending choice is queued), where merging would be unsafe.
    static bool stateKey(const BattleContext& bc, std::uint64_t& out);

    BattleContext root_;
    Node rootNode_;
    std::mt19937_64 rng_;
    int maxTurns_;
    // Score range seen so far; UCT normalises edge means into [0, 1] with it.
    double minValue_ = 1e300;
    double maxValue_ = -1e300;
    // Best complete winning line and best positional fallback, tracked separately so a
    // positional score can never masquerade as a win (they live on different scales).
    double bestWinValue_ = -1e300;
    double bestCappedValue_ = -1e300;
    // Transposition table: state key -> node expanded for that state. Enabled by
    // STS2_DEDUP (default on); the key is a field hash, not a string, so it is cheap
    // enough to build at every clean decision node.
    std::unordered_map<std::uint64_t, std::shared_ptr<Node>> transposition_;
    SearchResult result_;
};

} // namespace sts2::search

#endif // STS2_SEARCH_H
