// T4 map entry: generate a StandardActMap from a run seed and print it.
//
//   map dump  <seed> [act]
//   map bench <seedStart> <count> [act]   (determinism + shape stats)

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "sts2/Sts2Map.h"

using namespace sts2;

namespace {

std::string hashMap(const ActMap& m) {
    // Topology + point-type fingerprint in game traversal order (col-major grid).
    std::string s;
    for (int c = 0; c < ActMap::kWidth; ++c) {
        for (int r = 0; r < m.height; ++r) {
            const int idx = m.grid[r * ActMap::kWidth + c];
            if (idx < 0) continue;
            const MapPoint& p = m.points[idx];
            s += std::to_string(p.col) + "," + std::to_string(p.row) + ":" +
                 mapPointTypeName(p.type) + ">";
            std::vector<int> ch = p.children;
            std::sort(ch.begin(), ch.end(), [&](int a, int b) {
                const MapPoint& pa = m.points[a];
                const MapPoint& pb = m.points[b];
                if (pa.col != pb.col) return pa.col < pb.col;
                return pa.row < pb.row;
            });
            for (int x : ch)
                s += std::to_string(m.points[x].col) + "," + std::to_string(m.points[x].row) + ";";
            s += "|";
        }
    }
    return s;
}

void dump(const ActMap& m) {
    std::printf("act height=%d start=(%d,%d) boss=(%d,%d)\n", m.height,
                m.points[m.start].col, m.points[m.start].row,
                m.points[m.boss].col, m.points[m.boss].row);
    for (int c = 0; c < ActMap::kWidth; ++c) {
        for (int r = m.height - 1; r >= 0; --r) {
            const int idx = m.grid[r * ActMap::kWidth + c];
            if (idx < 0) continue;
            const MapPoint& p = m.points[idx];
            std::printf("  (%d,%2d) %-9s ->", p.col, p.row, mapPointTypeName(p.type));
            for (int x : p.children) {
                const MapPoint& q = m.points[x];
                std::printf(" (%d,%d)", q.col, q.row);
            }
            std::printf("\n");
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: map dump <seed> [act] | map bench <seedStart> <count> [act]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    const std::string act = argc > (cmd == "dump" ? 3 : 4) ? argv[(cmd == "dump" ? 3 : 4)]
                                                          : "OVERGROWTH";
    const ActConfig cfg = actConfigFor(act);

    if (cmd == "dump") {
        const std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
        const bool prune = !(argc > 3 && std::string(argv[3]) == "noprune");
        Sts2Rng rng(seed, "act_1_map");
        const MapPointTypeCounts counts = rollPointTypeCounts(act, rng);
        ActMap m = generateStandardActMap(rng, cfg.baseRooms, counts, prune);
        dump(m);
        std::printf("hash=%zu\n", std::hash<std::string>{}(hashMap(m)));
        return 0;
    }

    if (cmd == "json") {
        const std::uint64_t seed = std::strtoull(argv[2], nullptr, 10);
        const bool prune = !(argc > 3 && std::string(argv[3]) == "noprune");
        Sts2Rng rng(seed, "act_1_map");
        const MapPointTypeCounts counts = rollPointTypeCounts(act, rng);
        ActMap m = generateStandardActMap(rng, cfg.baseRooms, counts, prune);
        const auto st = rng.CaptureState();
        std::printf("{\"rng\":\"%d:%llu:%llu:%llu:%llu\",\"start\":[%d,%d],\"boss\":[%d,%d],\"points\":[",
                    st.counter, (unsigned long long)st.state.s0, (unsigned long long)st.state.s1,
                    (unsigned long long)st.state.s2, (unsigned long long)st.state.s3,
                    m.points[m.start].col, m.points[m.start].row,
                    m.points[m.boss].col, m.points[m.boss].row);
        bool first = true;
        for (int c = 0; c < ActMap::kWidth; ++c) {
            for (int r = 0; r < m.height; ++r) {
                const int idx = m.grid[r * ActMap::kWidth + c];
                if (idx < 0) continue;
                const MapPoint& p = m.points[idx];
                if (!first) std::printf(",");
                first = false;
                std::printf("{\"col\":%d,\"row\":%d,\"type\":\"%s\",\"children\":[",
                            p.col, p.row, mapPointTypeName(p.type));
                bool fc = true;
                for (int ch : p.children) {
                    if (!fc) std::printf(",");
                    fc = false;
                    std::printf("[%d,%d]", m.points[ch].col, m.points[ch].row);
                }
                std::printf("]}");
            }
        }
        std::printf("]}\n");
        return 0;
    }

    if (cmd == "bench") {
        if (argc < 4) return 2;
        const std::uint64_t seedStart = std::strtoull(argv[2], nullptr, 10);
        const int count = std::atoi(argv[3]);
        int bad = 0;
        for (int i = 0; i < count; ++i) {
            const std::uint64_t seed = seedStart + i;
            Sts2Rng r1(seed, "act_1_map");
            const MapPointTypeCounts c1 = rollPointTypeCounts(act, r1);
            ActMap m1 = generateStandardActMap(r1, cfg.baseRooms, c1);
            Sts2Rng r2(seed, "act_1_map");
            const MapPointTypeCounts c2 = rollPointTypeCounts(act, r2);
            ActMap m2 = generateStandardActMap(r2, cfg.baseRooms, c2);
            const bool same = hashMap(m1) == hashMap(m2);
            // Structural sanity: every non-terminal point reaches the boss.
            int unreachable = 0;
            for (int idx : m1.allPoints()) {
                bool reaches = false;
                std::vector<int> stack{idx};
                std::vector<char> seen(m1.points.size(), 0);
                while (!stack.empty()) {
                    const int n = stack.back();
                    stack.pop_back();
                    if (n == m1.boss) { reaches = true; break; }
                    if (seen[n]++) continue;
                    for (int ch : m1.points[n].children) stack.push_back(ch);
                }
                if (!reaches) ++unreachable;
            }
            if (!same || unreachable) {
                std::printf("seed %llu same=%d unreachable=%d\n",
                            (unsigned long long)seed, same ? 1 : 0, unreachable);
                ++bad;
            }
        }
        std::printf("== %d seeds, deterministic failures=%d ==\n", count, bad);
        return bad == 0 ? 0 : 1;
    }

    return 2;
}
