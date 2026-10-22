// obs v8 export for the C++ run layer (3566 dims).
//
// Layout (matches sts2ai/obs/v2.py encode_extensions + the 151-dim base obs):
//   [0..150]    base: 131 combat dims (zero at run-layer decisions) + 20 run dims
//   [151..167]  orbs (zeros outside combat)
//   [168..701]  powers (zeros outside combat)
//   [702..1282] deck: frozen 581-card vocabulary counts / 3
//   [1283..1863] deck upgrades: upgraded counts / 3
//   [1864..2163] relics: one-hot over RelicId (300)
//   [2164..3318] map grid: 15 rows x 7 cols x [9 type one-hot + visited + reachable]
//   [3319..3333] map candidates: 5 slots x [col, row, type]
//   [3334..3359] card rewards: 5 slots x [id, cost, damage, block, is_attack] + count
//   [3360..3372] event: id + 4 options x [pool index, hash1, hash2]
//   [3373..3420] shop: 12 slots x [id, price, rarity, on_sale]
//   [3421..3430] held potions: 5 slots x [id, rarity]
//   [3431..3433] boss relic choices (zeros: not modelled)
//   [3434..3563] enemy intents (zeros at run decisions)
//   [3564..3565] absolute HP block [hp/200, maxHp/200]
//
// The frozen 581-card vocabulary comes from Sts2ObsVocab.h; the event/option pools are
// the engine's small tables, so those two blocks are shape-compatible rather than
// index-identical with the Python line.

#ifndef STS2_OBS_H
#define STS2_OBS_H

#include <vector>

#include "sts2/Sts2Run.h"

namespace sts2 {

constexpr int OBS_V8_SIZE = 3566;

std::vector<float> runObsV8(const RunState& run);

} // namespace sts2

#endif // STS2_OBS_H
