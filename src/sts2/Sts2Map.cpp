#include "sts2/Sts2Map.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace sts2 {

const char* mapPointTypeName(MapPointType t) {
    switch (t) {
        case MapPointType::Unassigned: return "UNASSIGNED";
        case MapPointType::Unknown: return "UNKNOWN";
        case MapPointType::Shop: return "SHOP";
        case MapPointType::Treasure: return "TREASURE";
        case MapPointType::RestSite: return "REST";
        case MapPointType::Monster: return "MONSTER";
        case MapPointType::Elite: return "ELITE";
        case MapPointType::Boss: return "BOSS";
        case MapPointType::Ancient: return "ANCIENT";
    }
    return "?";
}

bool ActMap::inMap(int idx) const {
    if (idx < 0) return false;
    const MapPoint& p = points[idx];
    if (p.type == MapPointType::Boss) return true;
    if (p.col < 0 || p.col >= kWidth || p.row < 0 || p.row >= height) return false;
    if (grid[p.row * kWidth + p.col] == idx) return true;
    return p.type == MapPointType::Ancient;
}

std::vector<int> ActMap::allPoints() const {
    std::vector<int> out;
    for (int c = 0; c < kWidth; ++c)
        for (int r = 0; r < height; ++r) {
            const int idx = grid[r * kWidth + c];
            if (idx >= 0) out.push_back(idx);
        }
    return out;
}

ActConfig actConfigFor(const std::string& actName) {
    if (actName == "HIVE") return {"HIVE", 14, 5, 3};
    if (actName == "UNDERDOCKS") return {"UNDERDOCKS", 15, 5, 3};
    if (actName == "GLORY") return {"GLORY", 13, 5, 3};
    return {"OVERGROWTH", 15, 5, 3};
}

MapPointTypeCounts rollPointTypeCounts(const std::string& actName, Sts2Rng& mapRng,
                                        int ascension) {
    // RNG order matches the C# overrides: rest count first, then the unknown count.
    MapPointTypeCounts c;    if (actName == "GLORY") {
        c.rests = mapRng.NextInt(5, 7);
        c.unknowns = mapRng.NextGaussianInt(12, 1, 10, 14) - 1;
    } else if (actName == "HIVE") {
        c.rests = mapRng.NextGaussianInt(6, 1, 6, 7);
        c.unknowns = mapRng.NextGaussianInt(12, 1, 10, 14) - 1;
    } else if (actName == "UNDERDOCKS") {
        c.rests = mapRng.NextGaussianInt(7, 1, 6, 7);
        c.unknowns = mapRng.NextGaussianInt(12, 1, 10, 14);
    } else {
        c.rests = mapRng.NextGaussianInt(7, 1, 6, 7);
        c.unknowns = mapRng.NextGaussianInt(12, 1, 10, 14);
    }
    // MapPointTypeCounts.NumOfElites: (int)Math.Round(5f * (SwarmingElites ? 1.6f : 1f)).
    c.elites = ascension >= 1 ? 8 : 5;
    c.shops = 3;
    return c;
}

namespace {

// ---------------- list extensions (ListExtensions.cs) ----------------

void unstableShuffle(std::vector<int>& v, Sts2Rng& rng) {
    int n = static_cast<int>(v.size());
    while (n > 1) {
        --n;
        const int j = rng.NextInt(n + 1);
        std::swap(v[n], v[j]);
    }
}

void stableShufflePoints(std::vector<int>& v, const ActMap& m, Sts2Rng& rng) {
    std::sort(v.begin(), v.end(), [&](int a, int b) {
        const MapPoint& pa = m.points[a];
        const MapPoint& pb = m.points[b];
        if (pa.col != pb.col) return pa.col < pb.col;
        return pa.row < pb.row;
    });
    unstableShuffle(v, rng);
}

// List<int>.StableShuffle: plain ascending sort, then the same Fisher-Yates.
void stableShuffleInts(std::vector<int>& v, Sts2Rng& rng) {
    std::sort(v.begin(), v.end());
    unstableShuffle(v, rng);
}

// ---------------- graph helpers ----------------

bool hasPoint(const std::vector<int>& v, int idx) {
    return std::find(v.begin(), v.end(), idx) != v.end();
}

void addChild(ActMap& m, int parent, int child) {
    if (hasPoint(m.points[parent].children, child)) return;
    m.points[parent].children.push_back(child);
    m.points[child].parents.push_back(parent);
}

void removeChild(ActMap& m, int parent, int child) {
    auto& ch = m.points[parent].children;
    ch.erase(std::remove(ch.begin(), ch.end(), child), ch.end());
    auto& pa = m.points[child].parents;
    pa.erase(std::remove(pa.begin(), pa.end(), parent), pa.end());
}

int addPoint(ActMap& m, int col, int row) {
    MapPoint p;
    p.col = col;
    p.row = row;
    m.points.push_back(p);
    return static_cast<int>(m.points.size()) - 1;
}

int getOrCreatePoint(ActMap& m, int col, int row) {
    const int existing = m.get(col, row);
    if (existing >= 0) return existing;
    const int idx = addPoint(m, col, row);
    m.grid[row * ActMap::kWidth + col] = idx;
    return idx;
}

bool isRemoved(const ActMap& m, int idx) {
    const MapPoint& p = m.points[idx];
    if (p.col < 0 || p.col >= ActMap::kWidth || p.row < 0 || p.row >= m.height) return true;
    return m.grid[p.row * ActMap::kWidth + p.col] < 0;
}

void removePoint(ActMap& m, int idx) {
    if (std::getenv("STS2_MAP_PHASES"))
        std::fprintf(stderr, "remove pt=%d,%d\n", m.points[idx].col, m.points[idx].row);
    MapPoint& p = m.points[idx];
    if (p.col >= 0 && p.col < ActMap::kWidth && p.row >= 0 && p.row < m.height)
        m.grid[p.row * ActMap::kWidth + p.col] = -1;
    m.starts.erase(std::remove(m.starts.begin(), m.starts.end(), idx), m.starts.end());
    const std::vector<int> children = p.children;
    for (int c : children) removeChild(m, idx, c);
    const std::vector<int> parents = p.parents;
    for (int par : parents) removeChild(m, par, idx);
}

// ---------------- construction ----------------

bool hasInvalidCrossover(const ActMap& m, int current, int targetCol) {
    const MapPoint& cur = m.points[current];
    const int delta = targetCol - cur.col;
    if (delta == 0 || delta == 7) return false;
    const int other = m.get(targetCol, cur.row);
    if (other < 0) return false;
    for (int child : m.points[other].children) {
        const int childDelta = m.points[child].col - m.points[other].col;
        if (childDelta == -delta) return true;
    }
    return false;
}

struct Coord { int col; int row; };

Coord generateNextCoord(ActMap& m, int current, Sts2Rng& rng) {
    const MapPoint& cur = m.points[current];
    const int low = std::max(0, cur.col - 1);
    const int high = std::min(cur.col + 1, 6);
    std::vector<int> options{-1, 0, 1};
    stableShuffleInts(options, rng);
    for (int item : options) {
        int target;
        if (item == -1) target = low;
        else if (item == 0) target = cur.col;
        else target = high;
        if (!hasInvalidCrossover(m, current, target))
            return {target, cur.row + 1};
    }
    throw std::runtime_error("StandardActMap: cannot find next node");
}

void pathGenerate(ActMap& m, int startPoint, Sts2Rng& rng) {
    int current = startPoint;
    while (m.points[current].row < m.height - 1) {
        const Coord next = generateNextCoord(m, current, rng);
        const int idx = getOrCreatePoint(m, next.col, next.row);
        addChild(m, current, idx);
        current = idx;
    }
}

// ---------------- point type assignment ----------------

bool inLowerRestriction(MapPointType t) {
    return t == MapPointType::RestSite || t == MapPointType::Elite;
}
bool inUpperRestriction(MapPointType t) { return t == MapPointType::RestSite; }
bool inParentChildRestriction(MapPointType t) {
    return t == MapPointType::Elite || t == MapPointType::RestSite ||
           t == MapPointType::Treasure || t == MapPointType::Shop;
}
bool inSiblingRestriction(MapPointType t) {
    return t == MapPointType::RestSite || t == MapPointType::Monster ||
           t == MapPointType::Unknown || t == MapPointType::Elite ||
           t == MapPointType::Shop;
}

bool isValidPointType(const ActMap& m, MapPointType type, int idx) {
    const MapPoint& p = m.points[idx];
    if (p.row >= m.height - 3 && inUpperRestriction(type)) return false;
    if (p.row < 6 && inLowerRestriction(type)) return false;

    if (inParentChildRestriction(type)) {
        for (int par : p.parents)
            if (m.points[par].type == type) return false;
        for (int ch : p.children)
            if (m.points[ch].type == type) return false;
    }
    if (inParentChildRestriction(type)) {
        for (int ch : p.children)
            if (m.points[ch].type == type) return false;
    }
    if (inSiblingRestriction(type)) {
        for (int par : p.parents) {
            for (int sib : m.points[par].children) {
                if (sib == idx) continue;
                if (m.points[sib].type == type) return false;
            }
        }
    }
    return true;
}

MapPointType nextValidType(const ActMap& m, std::deque<MapPointType>& queue, int idx) {
    const std::size_t n = queue.size();
    for (std::size_t i = 0; i < n; ++i) {
        const MapPointType t = queue.front();
        queue.pop_front();
        if (isValidPointType(m, t, idx)) return t;
        queue.push_back(t);
    }
    return MapPointType::Unassigned;
}

void assignRemainingTypes(ActMap& m, std::deque<MapPointType>& queue, Sts2Rng& rng) {
    for (int round = 0; round < 3; ++round) {
        if (queue.empty()) return;
        std::vector<int> unassigned;
        for (int idx : m.allPoints())
            if (m.points[idx].type == MapPointType::Unassigned) unassigned.push_back(idx);
        stableShufflePoints(unassigned, m, rng);
        for (int idx : unassigned) {
            if (queue.empty()) break;
            m.points[idx].type = nextValidType(m, queue, idx);
        }
    }
}

void assignPointTypes(ActMap& m, const MapPointTypeCounts& counts, Sts2Rng& rng) {
    for (int c = 0; c < ActMap::kWidth; ++c) {
        const int idx = m.grid[(m.height - 1) * ActMap::kWidth + c];
        if (idx >= 0) {
            m.points[idx].type = MapPointType::RestSite;
            m.points[idx].canBeModified = false;
        }
    }
    for (int c = 0; c < ActMap::kWidth; ++c) {
        const int row = m.height - 7;
        if (row < 0) break;
        const int idx = m.grid[row * ActMap::kWidth + c];
        if (idx >= 0) {
            m.points[idx].type = MapPointType::Treasure;
            m.points[idx].canBeModified = false;
        }
    }
    for (int c = 0; c < ActMap::kWidth; ++c) {
        const int idx = m.grid[1 * ActMap::kWidth + c];
        if (idx >= 0) {
            m.points[idx].type = MapPointType::Monster;
            m.points[idx].canBeModified = false;
        }
    }

    std::deque<MapPointType> queue;
    for (int i = 0; i < counts.rests; ++i) queue.push_back(MapPointType::RestSite);
    for (int i = 0; i < counts.shops; ++i) queue.push_back(MapPointType::Shop);
    for (int i = 0; i < counts.elites; ++i) queue.push_back(MapPointType::Elite);
    for (int i = 0; i < counts.unknowns; ++i) queue.push_back(MapPointType::Unknown);

    assignRemainingTypes(m, queue, rng);

    for (int idx : m.allPoints())
        if (m.points[idx].type == MapPointType::Unassigned)
            m.points[idx].type = MapPointType::Monster;

    m.points[m.boss].type = MapPointType::Boss;
    m.points[m.start].type = MapPointType::Ancient;
}

// ---------------- pruning (MapPathPruning.cs) ----------------

using Path = std::vector<int>;
using Segment = std::vector<int>;

void findAllPaths(const ActMap& m, int current, Path& stack, std::vector<Path>& out) {
    if (m.points[current].type == MapPointType::Boss) {
        out.push_back(stack);
        return;
    }
    for (int child : m.points[current].children) {
        stack.push_back(child);
        findAllPaths(m, child, stack, out);
        stack.pop_back();
    }
}

bool validSegmentStart(const ActMap& m, int idx) {
    return m.points[idx].children.size() > 1 || m.points[idx].row == 0;
}
bool validSegmentEnd(const ActMap& m, int idx) {
    return m.points[idx].parents.size() >= 2;
}

std::string segmentKey(const ActMap& m, const Segment& seg) {
    const MapPoint& a = m.points[seg.front()];
    const MapPoint& b = m.points[seg.back()];
    std::string key;
    if (a.row == 0) {
        key = std::to_string(a.row) + "-" + std::to_string(b.col) + "," +
              std::to_string(b.row) + "-";
    } else {
        key = std::to_string(a.col) + "," + std::to_string(a.row) + "-" +
              std::to_string(b.col) + "," + std::to_string(b.row) + "-";
    }
    for (std::size_t i = 0; i < seg.size(); ++i) {
        if (i) key += ",";
        key += std::to_string(static_cast<int>(m.points[seg[i]].type));
    }
    return key;
}

bool overlappingSegment(const ActMap& m, const Segment& a, const Segment& b) {
    if (a.size() < 3 || b.size() < 3) return false;
    for (std::size_t i = 1; i + 1 < a.size(); ++i) {
        const MapPoint& pa = m.points[a[i]];
        const MapPoint& pb = m.points[b[i]];
        if (pa.col == pb.col && pa.row == pb.row) return true;
    }
    return false;
}

std::vector<std::vector<Segment>> findMatchingSegments(const ActMap& m) {
    Path stack{m.start};
    std::vector<Path> paths;
    findAllPaths(m, m.start, stack, paths);

    std::map<std::string, std::vector<Segment>> segments;
    for (const Path& path : paths) {
        for (std::size_t i = 0; i + 1 < path.size(); ++i) {
            if (!validSegmentStart(m, path[i])) continue;
            for (std::size_t j = 2; j < path.size() - i; ++j) {
                const int end = path[i + j];
                if (!validSegmentEnd(m, end)) continue;
                Segment seg(path.begin() + i, path.begin() + i + j + 1);
                const std::string key = segmentKey(m, seg);
                auto it = segments.find(key);
                if (it == segments.end()) {
                    segments[key] = {seg};
                } else {
                    bool overlaps = false;
                    for (const Segment& existing : it->second)
                        if (overlappingSegment(m, existing, seg)) { overlaps = true; break; }
                    if (!overlaps) it->second.push_back(seg);
                }
            }
        }
    }

    std::vector<std::vector<Segment>> dupes;
    for (auto& kv : segments)
        if (kv.second.size() > 1) dupes.push_back(kv.second);
    return dupes;
}

bool pruneSegment(ActMap& m, const Segment& segment) {
    if (std::getenv("STS2_MAP_PHASES")) {
        std::fprintf(stderr, "segment enter seg=");
        bool fp = true;
        for (int idp : segment) {
            if (!fp) std::fprintf(stderr, ";");
            fp = false;
            std::fprintf(stderr, "%d,%d", m.points[idp].col, m.points[idp].row);
        }
        std::fprintf(stderr, "\n");
    }
    bool result = false;
    for (std::size_t i = 0; i + 1 < segment.size(); ++i) {
        const int idx = segment[i];
        if (!m.inMap(idx)) return true;
        const MapPoint& p = m.points[idx];
        if (p.children.size() > 1 || p.parents.size() > 1) continue;
        bool parentWithOneChild = false;
        for (int par : p.parents)
            if (m.points[par].children.size() == 1 && !isRemoved(m, par)) {
                parentWithOneChild = true;
                break;
            }
        if (parentWithOneChild) continue;

        bool hasJunction = false;
        for (std::size_t k = i; k < segment.size(); ++k) {
            const MapPoint& n = m.points[segment[k]];
            if (n.children.size() > 1 && n.parents.size() == 1) { hasJunction = true; break; }
        }
        if (hasJunction) continue;

        if (m.points[segment.back()].parents.size() == 1) return false;

        bool outsideChildWithOneParent = false;
        for (int ch : p.children) {
            if (hasPoint(segment, ch)) continue;
            if (m.points[ch].parents.size() == 1) { outsideChildWithOneParent = true; break; }
        }
        if (!outsideChildWithOneParent) {
            removePoint(m, idx);
            result = true;
        }
    }
    return result;
}

int pruneAllButLast(ActMap& m, std::vector<Segment>& matches) {
    // C# exits on the PRUNE COUNT, not the index: if the first segments do not prune,
    // the loop keeps going and the last segment is pruned too (MapPathPruning.cs).
    int removed = 0;
    for (const Segment& seg : matches) {
        if (removed == static_cast<int>(matches.size()) - 1) return removed;
        if (pruneSegment(m, seg)) ++removed;
    }
    return removed;
}

bool breakRelationshipInSegment(ActMap& m, const Segment& segment) {
    bool result = false;
    for (std::size_t i = 0; i + 1 < segment.size(); ++i) {
        const int a = segment[i];
        if (m.points[a].children.size() >= 2) {
            const int b = segment[i + 1];
            if (m.points[b].parents.size() != 1) {
                if (std::getenv("STS2_MAP_PHASES"))
                    std::fprintf(stderr, "break rm %d,%d -> %d,%d\n",
                                 m.points[a].col, m.points[a].row, m.points[b].col, m.points[b].row);
                removeChild(m, a, b);
                result = true;
            }
        }
    }
    return result;
}

bool breakRelationshipAny(ActMap& m, std::vector<Segment>& matches) {
    for (const Segment& seg : matches)
        if (breakRelationshipInSegment(m, seg)) return true;
    return false;
}

bool prunePaths(ActMap& m, std::vector<std::vector<Segment>>& matching, Sts2Rng& rng) {
    const bool tr = std::getenv("STS2_MAP_PHASES") != nullptr;
    if (tr) {
        std::string sizes;
        for (auto& g : matching) { if (!sizes.empty()) sizes += ","; sizes += std::to_string(g.size()); }
        std::fprintf(stderr, "prunepaths enter groups=%s before=%d segs=",
                     sizes.c_str(), rng.Counter());
        bool fg = true;
        for (auto& g : matching) {
            if (!fg) std::fprintf(stderr, "/");
            fg = false;
            bool fs = true;
            for (auto& seg : g) {
                if (!fs) std::fprintf(stderr, "|");
                fs = false;
                bool fp = true;
                for (int idp : seg) {
                    if (!fp) std::fprintf(stderr, ";");
                    fp = false;
                    std::fprintf(stderr, "%d,%d", m.points[idp].col, m.points[idp].row);
                }
            }
        }
        std::fprintf(stderr, "\n");
    }
    bool any = false;
    for (auto& group : matching) {
        // C#: matchingSegment.UnstableShuffle(rng) on the segment list. Shuffling the
        // index list consumes the same RNG draws and yields the same permutation.
        std::vector<int> order(group.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = static_cast<int>(i);
        unstableShuffle(order, rng);
        std::vector<Segment> shuffled;
        shuffled.reserve(group.size());
        for (int i : order) shuffled.push_back(group[i]);
        group.swap(shuffled);

        if (pruneAllButLast(m, group) != 0) { any = true; break; }
        // The shipped runtime never calls BreakAParentChildRelationshipInAnySegment during
        // map generation (probed: 0 calls), even though the decompiled source has it. The
        // game build is authoritative, so the break step is not applied.
    }
    if (tr) std::fprintf(stderr, "prunepaths exit after=%d\n", rng.Counter());
    return any;
}

bool repairPointType(ActMap& m, MapPointType type, int target, Sts2Rng& rng) {
    int count = 0;
    for (int idx : m.allPoints())
        if (m.points[idx].type == type) ++count;
    int need = target - count;
    if (need <= 0) return false;

    std::vector<int> candidates;
    for (int idx : m.allPoints())
        if (m.points[idx].type == MapPointType::Monster && m.points[idx].canBeModified)
            candidates.push_back(idx);
    stableShufflePoints(candidates, m, rng);

    bool result = false;
    for (int idx : candidates) {
        if (need == 0) break;
        if (isValidPointType(m, type, idx)) {
            m.points[idx].type = type;
            --need;
            result = true;
        }
    }
    return result;
}

bool repairPrunedPointTypes(ActMap& m, const MapPointTypeCounts& counts, Sts2Rng& rng) {
    bool changed = false;
    changed |= repairPointType(m, MapPointType::Shop, counts.shops, rng);
    changed |= repairPointType(m, MapPointType::Elite, counts.elites, rng);
    changed |= repairPointType(m, MapPointType::RestSite, counts.rests, rng);
    changed |= repairPointType(m, MapPointType::Unknown, counts.unknowns, rng);
    return changed;
}

void pruneAndRepair(ActMap& m, const MapPointTypeCounts& counts, Sts2Rng& rng) {
    const bool trace = std::getenv("STS2_MAP_PHASES") != nullptr;
    for (int i = 0; i < 3; ++i) {
        int guard = 0;
        std::vector<std::vector<Segment>> matching = findMatchingSegments(m);
        int rounds = 0;
        while (prunePaths(m, matching, rng)) {
            ++rounds;
            if (++guard > 50)
                throw std::runtime_error("MapPathPruning: unable to prune matching segments");
            matching = findMatchingSegments(m);
        }
        if (trace) {
            std::fprintf(stderr, "prune iter=%d rounds=%d after=%d\n",
                         i, rounds, rng.Counter());
            if (i == 0 && std::getenv("STS2_MAP_DUMP_PREPAIR")) {
                std::fprintf(stderr, "prerepair ");
                for (int c2 = 0; c2 < ActMap::kWidth; ++c2)
                    for (int r2 = 0; r2 < m.height; ++r2) {
                        const int id2 = m.grid[r2*ActMap::kWidth+c2];
                        if (id2 < 0) continue;
                        std::fprintf(stderr, "(%d,%d,%s,%d)", m.points[id2].col, m.points[id2].row,
                                     mapPointTypeName(m.points[id2].type),
                                     m.points[id2].canBeModified ? 1 : 0);
                    }
                std::fprintf(stderr, "\n");
            }
        }
        const bool changed = repairPrunedPointTypes(m, counts, rng);
        if (trace) std::fprintf(stderr, "repair iter=%d changed=%d after=%d\n",
                                i, changed ? 1 : 0, rng.Counter());
        if (!changed) break;
    }
}

} // namespace

// StandardActMap(hasSecondBoss): SecondBossMapPoint = (boss column, boss row + 1);
// only the first boss links to it.
void addSecondBossPoint(ActMap& m) {
    const MapPoint& boss = m.points[m.boss];
    MapPoint sb;
    sb.col = boss.col;
    sb.row = boss.row + 1;
    sb.type = MapPointType::Boss;
    sb.canBeModified = false;
    m.points.push_back(sb);
    addChild(m, m.boss, static_cast<int>(m.points.size()) - 1);
}

ActMap generateStandardActMap(Sts2Rng& mapRng, int baseRooms, const MapPointTypeCounts& counts,
                              bool enablePruning) {
    ActMap m;
    m.height = baseRooms + 1;
    m.grid.assign(static_cast<std::size_t>(m.height) * ActMap::kWidth, -1);

    m.start = addPoint(m, 3, 0);
    m.boss = addPoint(m, 3, m.height);

    // GenerateMap: seven row-1 starts, each walked to the last row.
    const bool trace = std::getenv("STS2_MAP_PHASES") != nullptr;
    for (int i = 0; i < 7; ++i) {
        int p = getOrCreatePoint(m, mapRng.NextInt(0, 7), 1);
        if (i == 1) {
            while (hasPoint(m.starts, p)) p = getOrCreatePoint(m, mapRng.NextInt(0, 7), 1);
        }
        m.starts.push_back(p);
        pathGenerate(m, p, mapRng);
    }
    for (int c = 0; c < ActMap::kWidth; ++c) {
        const int idx = m.grid[(m.height - 1) * ActMap::kWidth + c];
        if (idx >= 0) addChild(m, idx, m.boss);
    }
    for (int c = 0; c < ActMap::kWidth; ++c) {
        const int idx = m.grid[1 * ActMap::kWidth + c];
        if (idx >= 0) addChild(m, m.start, idx);
    }

    if (trace) std::fprintf(stderr, "phase GenerateMap=%d\n", mapRng.Counter());
    assignPointTypes(m, counts, mapRng);
    if (trace) std::fprintf(stderr, "phase AssignPointTypes=%d\n", mapRng.Counter());
    if (enablePruning) {
        if (trace) {
            const auto st0 = mapRng.CaptureState();
            std::fprintf(stderr, "phase PruneStart=%d:%llu:%llu:%llu:%llu\n",
                         st0.counter, (unsigned long long)st0.state.s0,
                         (unsigned long long)st0.state.s1, (unsigned long long)st0.state.s2,
                         (unsigned long long)st0.state.s3);
        }
        pruneAndRepair(m, counts, mapRng);
    }
    if (trace) std::fprintf(stderr, "phase Prune=%d\n", mapRng.Counter());
    return m;
}

} // namespace sts2
