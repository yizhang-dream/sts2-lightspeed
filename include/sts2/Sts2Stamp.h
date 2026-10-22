// ContinuationStamp I/O — the state contract between the game/CombatSolver and this engine.
//
// Stamp grammar (from CombatSolver src/Runtime/ContinuationStamp.cs + real samples):
//   turn=N;hp=;max_hp=;block=;energy=;stars=;gold=;osty=cid/hp/maxhp;
//   E<i>=combatId/MONSTER/slot/hp/maxhp/block/NEXT_MOVE;AI<i>=m1,m2,...;MS<i>=name/int;
//   H=card,card,...;D=...;C=...;X=...;Y=0/0/0/0;FlameHp=0;AttackStarts=0;O=cap:orbs;
//   potions=p1,p2,p3;relicCounters=;mirrorRelics=;P=...powers...;R=rng9;max_hand_size=10
// Card entry: NAME+U/False:cost/False:star/replay/exhaust/sly/retain/deck/flag/-:0:0/-:0[Var=v,...]/private=-/keywords=[]/baselib=-
// Rng entry: nine streams joined by '/', each "counter:s0:s1:s2:s3".

#ifndef STS2_STAMP_H
#define STS2_STAMP_H

#include <string>
#include <vector>
#include "sts2/Sts2Battle.h"

namespace sts2 {

// Parse a ContinuationStamp into a BattleContext (registry monsters resolved by name;
// unknown monster/card names are kept as-is on the instance for diagnostics).
bool parseStamp(const std::string& stamp, BattleContext& bc, std::string* err = nullptr);

// Emit a stamp text from a BattleContext, matching the game field order so that the
// same string-diff tooling works on both sides.
std::string emitStamp(const BattleContext& bc);

// A card rendered as its stamp entry (used by emitStamp and by pile-level diffs).
std::string emitCardEntry(const CardInstance& c);

// Parse one pile entry list ("H=..." payload) into CardInstances.
bool parsePileEntries(const std::string& payload, std::vector<CardInstance>& out, std::string* err = nullptr);

// Parse "counter:s0:s1:s2:s3".
bool parseRngState(const std::string& s, Sts2Rng& rng, std::string* err = nullptr);
std::string emitRngState(const Sts2Rng& rng);

// Power name mapping (P=... entries look like "1:TERRITORIAL_POWER=1/0[]").
const char* powerName(PowerId id);
PowerId powerIdFromGameName(const std::string& name);

} // namespace sts2

#endif // STS2_STAMP_H
