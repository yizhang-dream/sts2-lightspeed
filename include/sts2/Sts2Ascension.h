#ifndef STS2_ASCENSION_H
#define STS2_ASCENSION_H

namespace sts2 {

// Applies a STS2 ascension level (0-10, AscensionManager semantics) to the shared
// monster-def registry: A8 ToughEnemies HP tables and A9 DeadlyEnemies damage/block/
// power values. Level 0 restores the pristine A0 tables (byte-identical behaviour),
// so calling this with 0 is a no-op by construction.
//
// Every override carries the game's A0 base value; the applier refuses the override
// and reports a mismatch if the shipped registry ever drifts from that base.
void applyAscensionToMonsterDefs(int level);

}

#endif
