// obs v8 export for the run layer. See Sts2Obs.h for the block layout.

#include "sts2/Sts2Obs.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "sts2/Sts2ObsVocab.h"
#include "sts2/Sts2Relic.h"

namespace sts2 {

namespace {

constexpr int kBaseSize = 151;
constexpr int kOrbSize = 17;
constexpr int kPowerSize = 534;
constexpr int kDeckSize = OBS_CARD_VOCAB;      // 581
constexpr int kUpgradeSize = OBS_CARD_VOCAB;   // 581
constexpr int kRelicSize = 300;                // RelicId domain (invalid + 299)
constexpr int kMapRows = 15;
constexpr int kMapCols = 7;
constexpr int kMapCell = 11;                   // 9 point types + visited + reachable
constexpr int kMapSize = kMapRows * kMapCols * kMapCell;
constexpr int kMapCandSlots = 5;
constexpr int kMapCandFeats = 3;
constexpr int kCardRewardSlots = 5;
constexpr int kCardRewardFeats = 5;
constexpr int kCardRewardSize = kCardRewardSlots * kCardRewardFeats + 1;
constexpr int kEventOptionSlots = 4;
constexpr int kEventOptionFeats = 3;
constexpr int kEventSize = 1 + kEventOptionSlots * kEventOptionFeats;
constexpr int kShopSlots = 12;
constexpr int kShopFeats = 4;
constexpr int kShopSize = kShopSlots * kShopFeats;
constexpr int kPotionSlots = 5;
constexpr int kPotionFeats = 2;
constexpr int kPotionSize = kPotionSlots * kPotionFeats;
constexpr int kBossRelicSize = 3;
constexpr int kIntentSize = 130;               // 5 enemies x 26
constexpr int kHpSize = 2;

// Python MapPointType order (UNASSIGNED, UNKNOWN, MONSTER, ELITE, BOSS, SHOP, REST,
// TREASURE, ANCIENT) — the engine enum uses a different order.
int obsPointTypeIndex(MapPointType t) {
    switch (t) {
        case MapPointType::Unassigned: return 0;
        case MapPointType::Unknown: return 1;
        case MapPointType::Monster: return 2;
        case MapPointType::Elite: return 3;
        case MapPointType::Boss: return 4;
        case MapPointType::Shop: return 5;
        case MapPointType::RestSite: return 6;
        case MapPointType::Treasure: return 7;
        case MapPointType::Ancient: return 8;
    }
    return 0;
}

// Card damage/block vars for the card-reward descriptor (base values at upgrade 0).
void cardCombatVars(CardId id, float& damage, float& block) {
    damage = 0.0f;
    block = 0.0f;
    const CardDef& d = BattleContext::cardDef(id);
    for (int i = 0; i < d.nSteps; ++i) {
        const CardStep& st = d.steps[i];
        const int val = st.varIdx >= 0 && st.varIdx < d.nVars ? d.vars[st.varIdx].base
                                                             : st.constVal;
        if (st.op == StepOp::ATTACK || st.op == StepOp::ATTACK_ALL) {
            damage = std::max(damage, static_cast<float>(val));
        } else if (st.op == StepOp::GAIN_BLOCK) {
            block = std::max(block, static_cast<float>(val));
        }
    }
}

float rarityNormPotion(PotionRarity r) {
    const int idx = static_cast<int>(r);
    return (idx + 1) / 8.0f; // 6 rarities + 2
}

} // namespace

std::vector<float> runObsV8(const RunState& run) {
    std::vector<float> out(OBS_V8_SIZE, 0.0f);

    // ---- base 151: 131 combat dims stay zero (no combat at run-layer decisions) ----
    int o = kBaseSize - 20;
    out[o + 0] = run.actIndex / 3.0f;
    out[o + 1] = run.totalFloor / 50.0f;
    out[o + 2] = run.actFloor / 20.0f;
    out[o + 3] = run.player.maxHp > 0
        ? static_cast<float>(run.player.hp) / run.player.maxHp : 0.0f;
    out[o + 4] = run.player.gold / 1000.0f;
    out[o + 5] = run.player.deck.size() / 40.0f;
    out[o + 6] = run.relics.size() / 30.0f;
    int potionsHeld = 0;
    for (int p : run.player.potions) potionsHeld += p != 0 ? 1 : 0;
    out[o + 7] = potionsHeld / 3.0f;
    out[o + 8] = 3.0f / 5.0f; // max potion slots
    // phase one-hot (8): map choice, combat, card reward, boss relic, shop, rest, event,
    // treasure.
    int phase = 0;
    if (run.awaitingCardReward) phase = 2;
    else if (run.awaitingShop) phase = 4;
    else if (run.awaitingRest) phase = 5;
    else if (run.awaitingEvent) phase = 6;
    out[o + 9 + phase] = 1.0f;
    out[o + 17] = 0.0f; // ascension
    bool elite = false, boss = false;
    if (run.current >= 0 && run.current < static_cast<int>(run.map.points.size())) {
        elite = run.map.points[run.current].type == MapPointType::Elite;
        boss = run.map.points[run.current].type == MapPointType::Boss;
    }
    out[o + 18] = elite ? 1.0f : 0.0f;
    out[o + 19] = boss ? 1.0f : 0.0f;

    int off = kBaseSize;

    // ---- orbs / powers: zero outside combat ----
    off += kOrbSize;
    off += kPowerSize;

    // ---- deck + upgrades ----
    for (const RunCard& c : run.player.deck) {
        const int id = static_cast<int>(c.id);
        if (id < 0 || id > OBS_CARD_ID_COUNT) continue;
        const int idx = kCardIdToObsIndex[id];
        if (idx < 0) continue;
        out[off + idx] += 1.0f;
        if (c.upgrade > 0) out[off + kDeckSize + idx] += 1.0f;
    }
    for (int i = 0; i < kDeckSize; ++i) {
        out[off + i] /= 3.0f;
        out[off + kDeckSize + i] /= 3.0f;
    }
    off += kDeckSize + kUpgradeSize;

    // ---- relics (one-hot, capped at 5 per slot) ----
    for (int rid : run.relics) {
        if (rid <= 0 || rid >= kRelicSize) continue;
        out[off + rid] = std::min(out[off + rid] + 1.0f, 5.0f);
    }
    off += kRelicSize;

    // ---- map grid ----
    for (const MapPoint& p : run.map.points) {
        if (p.row < 0 || p.row >= kMapRows || p.col < 0 || p.col >= kMapCols) continue;
        const int base = (p.row * kMapCols + p.col) * kMapCell;
        out[off + base + obsPointTypeIndex(p.type)] = 1.0f;
    }
    for (int idx : run.visitedPoints) {
        if (idx < 0 || idx >= static_cast<int>(run.map.points.size())) continue;
        const MapPoint& p = run.map.points[idx];
        if (p.row < 0 || p.row >= kMapRows || p.col < 0 || p.col >= kMapCols) continue;
        out[off + (p.row * kMapCols + p.col) * kMapCell + 9] = 1.0f;
    }
    for (int idx : availablePoints(run)) {
        const MapPoint& p = run.map.points[idx];
        if (p.row < 0 || p.row >= kMapRows || p.col < 0 || p.col >= kMapCols) continue;
        out[off + (p.row * kMapCols + p.col) * kMapCell + 10] = 1.0f;
    }
    off += kMapSize;

    // ---- map candidates (ordered like availablePoints) ----
    {
        const std::vector<int> pts = availablePoints(run);
        for (int i = 0; i < kMapCandSlots && i < static_cast<int>(pts.size()); ++i) {
            const MapPoint& p = run.map.points[pts[i]];
            const int base = off + i * kMapCandFeats;
            out[base + 0] = (p.col + 1) / static_cast<float>(kMapCols);
            out[base + 1] = (p.row + 1) / static_cast<float>(kMapRows);
            out[base + 2] = (obsPointTypeIndex(p.type) + 1) / 10.0f;
        }
    }
    off += kMapCandSlots * kMapCandFeats;

    // ---- card rewards ----
    {
        const int n = static_cast<int>(run.cardOffer.size());
        out[off + kCardRewardSlots * kCardRewardFeats] = std::min(n, 2) / 2.0f;
        for (int i = 0; i < kCardRewardSlots && i < n; ++i) {
            const CardId id = static_cast<CardId>(run.cardOffer[i].first);
            const int idx = (static_cast<int>(id) >= 0 && static_cast<int>(id) <= OBS_CARD_ID_COUNT)
                ? kCardIdToObsIndex[static_cast<int>(id)] : -1;
            const int base = off + i * kCardRewardFeats;
            if (idx >= 0) out[base + 0] = (idx + 1) / (OBS_CARD_VOCAB + 1.0f);
            const CardDef& d = BattleContext::cardDef(id);
            out[base + 1] = std::max(0, d.cost) / 5.0f;
            float dmg = 0.0f, blk = 0.0f;
            cardCombatVars(id, dmg, blk);
            out[base + 2] = dmg / 50.0f;
            out[base + 3] = blk / 50.0f;
            out[base + 4] = d.type == CardType::ATTACK ? 1.0f : 0.0f;
        }
    }
    off += kCardRewardSize;

    // ---- event ----
    if (run.awaitingEvent) {
        // One implemented event: pool index 1 of 1.
        out[off + 0] = 1.0f / 3.0f;
        for (int i = 0; i < kEventOptionSlots && i < static_cast<int>(run.eventOptions.size()); ++i) {
            const std::string& name = run.eventOptions[i];
            uint64_t h = 1469598103934665603ull;
            for (char ch : name) { h ^= static_cast<unsigned char>(ch); h *= 1099511628211ull; }
            const int base = off + 1 + i * kEventOptionFeats;
            out[base + 0] = (i + 1) / 9.0f;
            out[base + 1] = static_cast<float>((h >> 32) & 0xFFFF) / 32768.0f - 1.0f;
            out[base + 2] = static_cast<float>(h & 0xFFFF) / 32768.0f - 1.0f;
        }
    }
    off += kEventSize;

    // ---- shop ----
    if (run.awaitingShop) {
        for (int i = 0; i < kShopSlots && i < static_cast<int>(run.shop.size()); ++i) {
            const ShopEntry& e = run.shop[i];
            if (e.sold) continue;
            const int base = off + i * kShopFeats;
            if (e.removal) {
                out[base + 0] = (OBS_CARD_VOCAB + 1) / (OBS_CARD_VOCAB + 2.0f);
            } else if (e.relic) {
                out[base + 0] = (e.relicId + 1) / (kRelicSize + 2.0f);
                const int r = static_cast<int>(sts2RelicDef(static_cast<RelicId>(e.relicId)).rarity);
                out[base + 2] = (r + 1) / 10.0f;
            } else if (e.potion) {
                out[base + 0] = (e.potionId + 1) / 67.0f;
                out[base + 2] = rarityNormPotion(
                    sts2PotionDef(static_cast<PotionId>(e.potionId)).rarity);
            } else {
                const int idx = kCardIdToObsIndex[e.cardId];
                out[base + 0] = idx >= 0 ? (idx + 1) / (OBS_CARD_VOCAB + 2.0f)
                                         : (OBS_CARD_VOCAB + 1) / (OBS_CARD_VOCAB + 2.0f);
                const int r = static_cast<int>(BattleContext::cardDef(
                    static_cast<CardId>(e.cardId)).rarity);
                out[base + 2] = (r + 1) / 12.0f;
            }
            out[base + 1] = std::min(e.price / 300.0f, 3.0f);
            out[base + 3] = e.onSale ? 1.0f : 0.0f;
        }
    }
    off += kShopSize;

    // ---- held potions ----
    for (int i = 0; i < kPotionSlots; ++i) {
        const int pid = i < 3 ? run.player.potions[i] : 0;
        if (pid == 0) continue;
        const int base = off + i * kPotionFeats;
        out[base + 0] = (pid + 1) / 67.0f;
        out[base + 1] = rarityNormPotion(sts2PotionDef(static_cast<PotionId>(pid)).rarity);
    }
    off += kPotionSize;

    // ---- boss relics / enemy intents: not modelled at run decisions ----
    off += kBossRelicSize;
    off += kIntentSize;

    // ---- absolute HP ----
    out[off + 0] = run.player.hp / 200.0f;
    out[off + 1] = run.player.maxHp / 200.0f;

    for (float& v : out) v = std::max(-1.0f, std::min(10.0f, v));
    return out;
}

} // namespace sts2
