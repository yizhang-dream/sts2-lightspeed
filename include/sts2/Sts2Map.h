// STS2 act map — faithful port of MegaCrit.Sts2.Core.Map.StandardActMap +
// MapPathPruning. The game source is the authority; the port keeps the exact RNG call
// order (std::sort-then-Fisher-Yates StableShuffle, gaussian integer draws) because the
// map topology is part of the run distribution the policy is trained on.
//
// MapPostProcessing (CenterGrid/SpreadAdjacentMapPoints/StraightenPaths) only moves the
// visual coordinates after pruning and does not change parents/children, so run logic
// does not need it.

#ifndef STS2_MAP_H
#define STS2_MAP_H

#include <cstdint>
#include <string>
#include <vector>

#include "sts2/Sts2Random.h"

namespace sts2 {

// Numeric values must match the game enum: MapPostProcessing key strings include
// (int)PointType and pruning compares those keys.
enum class MapPointType : int {
    Unassigned = 0,
    Unknown = 1,
    Shop = 2,
    Treasure = 3,
    RestSite = 4,
    Monster = 5,
    Elite = 6,
    Boss = 7,
    Ancient = 8,
};

const char* mapPointTypeName(MapPointType t);

struct MapPointTypeCounts {
    int unknowns = 12;
    int rests = 7;
    int elites = 5;
    int shops = 3;
};

struct MapPoint {
    int col = 0;
    int row = 0;
    MapPointType type = MapPointType::Unassigned;
    bool canBeModified = true;
    std::vector<int> children; // indices into ActMap::points
    std::vector<int> parents;
};

struct ActMap {
    static constexpr int kWidth = 7;

    int height = 0;
    std::vector<int> grid;      // row-major: grid[row * kWidth + col] = point index or -1
    std::vector<MapPoint> points;
    std::vector<int> starts;    // row-1 entry points
    int start = -1;             // Ancient point (3, 0)
    int boss = -1;              // Boss point (3, height)

    int get(int col, int row) const {
        if (col == 3 && row == 0) return start;
        if (col == 3 && row == height) return boss;
        if (col < 0 || col >= kWidth || row < 0 || row >= height) return -1;
        return grid[row * kWidth + col];
    }
    bool inMap(int idx) const;
    // Game order: columns outer, rows inner.
    std::vector<int> allPoints() const;

    MapPoint& point(int idx) { return points[idx]; }
    const MapPoint& point(int idx) const { return points[idx]; }
};

// Act data needed by the map generator (from the decompiled ActModel subclasses).
struct ActConfig {
    const char* name = "OVERGROWTH";
    int baseRooms = 15;
    int elites = 5;
    int shops = 3;
};

ActConfig actConfigFor(const std::string& actName);
MapPointTypeCounts rollPointTypeCounts(const std::string& actName, Sts2Rng& mapRng,
                                        int ascension = 0);
// A10 DoubleBoss: add the second boss node past the first (final act only).
void addSecondBossPoint(ActMap& m);

// Rng(seed, "act_<n>_map") in the game; the caller passes that stream in.
ActMap generateStandardActMap(Sts2Rng& mapRng, int baseRooms, const MapPointTypeCounts& counts,
                              bool enablePruning = true);

} // namespace sts2

#endif // STS2_MAP_H
