#include <cstdio>
#include <string>
#include "sts2/Sts2Random.h"

// RNG 对照验证：与 Python 参考实现（游戏反编译源逐行转写）对同一向量。
// 用法: parity rngcheck
int runRngCheck() {
    using sts2::MegaRandom;
    using sts2::Sts2Rng;

    MegaRandom mr(12345ULL);
    std::printf("ulong:");
    for (int i = 0; i < 5; ++i) std::printf(" %llu", (unsigned long long)mr.NextULong());
    std::printf("\n");

    MegaRandom mr2(12345ULL);
    std::printf("double:");
    for (int i = 0; i < 5; ++i) std::printf(" %.17g", mr2.NextDouble());
    std::printf("\n");

    MegaRandom mr3(12345ULL);
    std::printf("next100:");
    for (int i = 0; i < 5; ++i) std::printf(" %d", mr3.Next(100));
    std::printf("\n");

    for (const char* name : {"shuffle", "combat_card_generation", "monster_ai", "niche"}) {
        std::printf("%s %llu\n", name, (unsigned long long)sts2::xxhash64(std::string(name)));
    }

    Sts2Rng named(12345ULL, "shuffle");
    std::printf("named_next100:");
    for (int i = 0; i < 5; ++i) std::printf(" %d", named.NextInt(100));
    std::printf(" counter=%d\n", named.Counter());
    return 0;
}
