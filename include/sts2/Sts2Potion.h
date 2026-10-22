// STS2 potion model (T4 run layer): id/rarity/pool table generated from the decompiled
// source. Potions can be bought and carried in the three run slots; drinking them is not
// wired yet (the run layer has no UsePotion action).

#ifndef STS2_POTION_H
#define STS2_POTION_H

#include <cstdint>

#include "sts2/Sts2Ids.h"

namespace sts2 {

// PotionRarity order matches the game enum (None, Common, Uncommon, Rare, Event, Token).
enum class PotionRarity : std::uint8_t {
    NONE = 0, COMMON, UNCOMMON, RARE, EVENT, TOKEN,
};

struct PotionDef {
    PotionId id = PotionId::INVALID;
    const char* name = "NONE";
    PotionRarity rarity = PotionRarity::NONE;
    bool ironcladPool = false;
    bool sharedPool = false;
    bool canBeGeneratedInCombat = true;
};

const PotionId* sts2PotionShopPool(int* count); // Ironclad + shared, pool order
const PotionDef& sts2PotionDef(PotionId id);
const char* sts2PotionName(PotionId id);

} // namespace sts2

#endif // STS2_POTION_H
