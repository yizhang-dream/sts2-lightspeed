// AUTO-GENERATED lookup tables. See Sts2Ids.h.
#include "sts2/Sts2Ids.h"
#include <cstring>

namespace sts2 {

    int lookupCardId(const char* name) {
        for (int i = 1; i <= CardId_COUNT; ++i)
            if (std::strcmp(CardId_NAMES[i], name) == 0) return i;
        return 0;
    }

    int lookupPotionId(const char* name) {
        for (int i = 1; i <= PotionId_COUNT; ++i)
            if (std::strcmp(PotionId_NAMES[i], name) == 0) return i;
        return 0;
    }

    int lookupRelicId(const char* name) {
        for (int i = 1; i <= RelicId_COUNT; ++i)
            if (std::strcmp(RelicId_NAMES[i], name) == 0) return i;
        return 0;
    }

    int lookupEncounterId(const char* name) {
        for (int i = 1; i <= EncounterId_COUNT; ++i)
            if (std::strcmp(EncounterId_NAMES[i], name) == 0) return i;
        return 0;
    }

} // namespace sts2
