// v2.1

// Nested beam searches: an outer one plans turns ahead, and for each of its
// nodes an inner one decides that turn's rails one cell at a time. Both are
// interruptible, playing the best line found when the turn budget runs out.

// - Rail placement: every affordable cell touching the network, scored by how
//     much it shortens the remaining wishes.
// - Turn scoring (extendGap): per wish, the terrain distance from a newly
//     laid cell to whichever of its two towns is farther.
// - State scoring (evaluate): income difference per turn, minus GAP_PENALTY
//     per cell of true remaining gap, from one multi-source flood fill
//     (openGapTotal) that reads off where two components' floods meet.
// - Disrupt choice: the region where the opponent owns the most connection
//     rails more than we do. Four disrupts ink a region and erase its rails.
// Limit to 30 ms per turn

#undef _GLIBCXX_DEBUG
#pragma GCC optimize("Ofast,unroll-loops,omit-frame-pointer,inline")
#pragma GCC option("arch=native", "tune=native", "no-zero-upper")
#pragma GCC target( \
    "movbe,aes,pclmul,avx,avx2,f16c,fma,sse3,ssse3,sse4.1,sse4.2,rdrnd,popcnt,bmi,bmi2,lzcnt")

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <sstream>
#include <queue>
#include <cstdio>
#include <cmath>
#include <climits>
#include <chrono>

using namespace std;

// ====================
// PROFILING

// Per-function call counts and accumulated microseconds. Printed each turn
// from main() alongside snapshotCommittedChild stats. Counters are cumulative
// over the game; PROFILE_END_TURN() marks a turn boundary so the print can
// also report a per-turn average.

class ProfileScope
{
public:
    int &elapsed;
    std::chrono::steady_clock::time_point start;
    ProfileScope(int &c, int &e) : elapsed(e), start(std::chrono::steady_clock::now()) { c++; }
    ~ProfileScope()
    {
        elapsed += (int)std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - start)
                       .count();
    }
};

// Set to 0 for competition builds: on the hottest helpers the two
// steady_clock reads per call cost more than the work being measured.
#ifndef ENABLE_PROFILING
#define ENABLE_PROFILING 1
#endif

#define DECLARE_PROFILE(name) \
    int callcount_##name = 0; \
    int elapsed_##name = 0;

// Number of game turns elapsed, bumped by PROFILE_END_TURN(). The counters
// above are cumulative over the whole game, so dividing by this gives the
// per-turn averages, which is what actually matters for a time budget that
// is spent turn by turn.
int profileTurnCount = 0;
#define PROFILE_END_TURN() (profileTurnCount++)

#if ENABLE_PROFILING
#define PROFILE(name) ProfileScope _ps_##name(callcount_##name, elapsed_##name)
#else
#define PROFILE(name) ((void)0)
#endif

#define PRINT_PROFILE(name)                                                    \
    do                                                                         \
    {                                                                          \
        int _turns = profileTurnCount > 0 ? profileTurnCount : 1;              \
        fprintf(stderr, "%-32s per turn : %.3f ms  \t%d calls\n", #name,         \
                (double)elapsed_##name / _turns / 1000,                        \
                (int)((double)callcount_##name / _turns));                            \
    } while (0)

// Profile declarations
DECLARE_PROFILE(beamSearch)
DECLARE_PROFILE(placementCandidates)
DECLARE_PROFILE(generateActionSets)
DECLARE_PROFILE(candidateCreation)
DECLARE_PROFILE(connectionPathProfile)
DECLARE_PROFILE(buildDisruptChoice)
DECLARE_PROFILE(simulateTurn)
DECLARE_PROFILE(evaluate)
DECLARE_PROFILE(railGroupOf)
DECLARE_PROFILE(stateCopy)
DECLARE_PROFILE(openGapTotal)
DECLARE_PROFILE(sortRegionIds)
DECLARE_PROFILE(sortRoundLines)
DECLARE_PROFILE(sortFinishedLines)
DECLARE_PROFILE(sortBeam)

// ====================
// CONSTANTS

static const int BEAM_WIDTH = 30;
// Width of the intra-turn beam, i.e. how many half-built turns stay alive
// between one rail and the next. It is a separate knob from BEAM_WIDTH
// because the two buy different things and cost very differently: widening
// here multiplies the work spent on a single turn, which comes straight out
// of the depth the outer beam can reach.
static const int NESTED_BEAM_WIDTH = 40;

// Upper bound on how many turn plans one state expands into, i.e. how many
// of the intra-turn beam's survivors the outer beam actually simulates. It
// exists for responsiveness, not for pruning quality: the deadline is only
// tested between expansions, so a node that expands everything overshoots the
// budget by a whole depth's worth of work. Set to 0 to disable, in which case
// the intra-turn beam width (BEAM_WIDTH) is the only bound.
static const int MAX_BRANCHING = 20;
static const int MAX_DEPTH = 10;
static const int PAINT_PER_TURN = 3;

// Wall-clock budget for one turn's search. The referee allows 50 ms per turn
// (1000 ms on the first). The deadline is only tested between expansions, so
// the budget stays well under the limit to absorb one in-flight expansion
// plus the final replay and output.
static const int TURN_BUDGET_MS = 30;
static const int FIRST_TURN_BUDGET_MS = 900;

// Owner marker for a tile carrying no rail.
static const int NO_OWNER = -1;
// Owner marker for a rail both players placed on the same turn.
static const int NEUTRAL_OWNER = 2;

// Instability a region gains per DISRUPT, and the level at which it gets
// inked (erasing every rail inside it): the statement defines inked as
// instability >= 4.
static const int DISRUPT_INSTABILITY_GAIN = 1;
static const int INK_INSTABILITY_THRESHOLD = 4;

// Direction priority: NORTH, EAST, SOUTH, WEST. Used both for path
// tie-breaking and for choosing which neighbour a rail advances to.
static const int DIR_X[4] = {0, 1, 0, -1};
static const int DIR_Y[4] = {-1, 0, 1, 0};

// ====================
// STRUCTURES

class Coord
{
public:
    int x, y;
    Coord(int x = 0, int y = 0) : x(x), y(y) {}
    bool operator==(const Coord &o) const { return x == o.x && y == o.y; }
    bool operator!=(const Coord &o) const { return !(*this == o); }
};

class Connection
{
public:
    int fromTownId, toTownId;
    Connection(int f = -1, int t = -1) : fromTownId(f), toTownId(t) {}
};

class Tile
{
public:
    int regionId;
    int type;
    int tracksOwner;
    bool inked;
    int instability;
    Tile(int r = 0, int t = 0)
        : regionId(r), type(t), tracksOwner(NO_OWNER), inked(false), instability(0) {}
};

class Town
{
public:
    int id;
    Coord coord;
    vector<int> desiredConnections;
    Town(int id = 0, Coord c = {}, vector<int> d = {})
        : id(id), coord(c), desiredConnections(move(d)) {}
};

class Grid
{
public:
    int width, height;
    vector<Tile> tiles;
    Grid(int w = 0, int h = 0) : width(w), height(h) { tiles.resize(w * h); }
    Tile &get(int x, int y) { return tiles[y * width + x]; }
    const Tile &get(int x, int y) const { return tiles[y * width + x]; }
};

class Region
{
public:
    int id;
    int instability;
    bool inked;
    vector<Coord> coords;
    bool hasTown;
    Region(int id = 0) : id(id), instability(0), inked(false), hasTown(false) {}
};

// Paint cost to place a rail on a terrain type.
static int terrainCost(int type)
{
    switch (type)
    {
    case 0:
        return 1; // PLAINS
    case 1:
        return 2; // RIVER
    case 2:
        return 3; // MOUNTAIN
    default:
        return INT_MAX; // impassable (unknown)
    }
}

// ====================
// PATHFINDING

// Generic 4-directional A* on a width x height grid.
//
// stepCost(x, y) gives the cost of entering cell (x, y); return INT_MAX to
// mark it impassable. Keeping the cost function a parameter is what makes
// this a standalone helper: it knows nothing about tiles, towns or regions.
//
// Returns the shortest path cost from src to dst, or INT_MAX if dst is
// unreachable. The Manhattan heuristic is admissible as long as no step
// costs less than 1 (cheaper steps, e.g. free town cells, only make the
// heuristic more conservative, never overestimating).
template <typename StepCostFn>
static int aStar(Coord src, Coord dst, int width, int height, StepCostFn stepCost)
{
    if (src == dst)
        return 0;

    vector<vector<int>> gScore(height, vector<int>(width, INT_MAX));
    gScore[src.y][src.x] = 0;

    auto heuristic = [&](int x, int y)
    {
        return abs(x - dst.x) + abs(y - dst.y);
    };

    // min-heap of (f = g + h, g, x, y)
    priority_queue<tuple<int, int, int, int>, vector<tuple<int, int, int, int>>, greater<>> pq;
    pq.push({heuristic(src.x, src.y), 0, src.x, src.y});

    while (!pq.empty())
    {
        auto [f, g, x, y] = pq.top();
        pq.pop();

        if (x == dst.x && y == dst.y)
            return g;

        // Stale entry: a shorter path to (x, y) was already found.
        if (g > gScore[y][x])
            continue;

        for (int k = 0; k < 4; k++)
        {
            int nx = x + DIR_X[k], ny = y + DIR_Y[k];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                continue;

            int step = stepCost(nx, ny);
            if (step == INT_MAX)
                continue;

            int ng = g + step;
            if (ng < gScore[ny][nx])
            {
                gScore[ny][nx] = ng;
                int nf = ng + heuristic(nx, ny);
                pq.push({nf, ng, nx, ny});
            }
        }
    }

    return INT_MAX; // dst is unreachable
}

// Same A* as above, but also reports the cells the shortest path runs
// through. `outPath` receives src..dst inclusive on success and is left empty
// when dst is unreachable. Separate from the cost-only version because
// tracking parents costs an extra grid and most callers only want the number.
template <typename StepCostFn>
static int aStarPath(Coord src, Coord dst, int width, int height,
                     StepCostFn stepCost, vector<Coord> &outPath)
{
    outPath.clear();
    if (src == dst)
    {
        outPath.push_back(src);
        return 0;
    }

    vector<vector<int>> gScore(height, vector<int>(width, INT_MAX));
    vector<vector<int>> parent(height, vector<int>(width, -1));
    gScore[src.y][src.x] = 0;

    auto heuristic = [&](int x, int y)
    {
        return abs(x - dst.x) + abs(y - dst.y);
    };

    priority_queue<tuple<int, int, int, int>, vector<tuple<int, int, int, int>>, greater<>> pq;
    pq.push({heuristic(src.x, src.y), 0, src.x, src.y});

    while (!pq.empty())
    {
        auto [f, g, x, y] = pq.top();
        pq.pop();
        (void)f;

        if (x == dst.x && y == dst.y)
        {
            for (int cur = y * width + x; cur != -1;
                 cur = parent[cur / width][cur % width])
                outPath.push_back(Coord(cur % width, cur / width));
            reverse(outPath.begin(), outPath.end());
            return g;
        }

        // Stale entry: a shorter path to (x, y) was already found.
        if (g > gScore[y][x])
            continue;

        for (int k = 0; k < 4; k++)
        {
            int nx = x + DIR_X[k], ny = y + DIR_Y[k];
            if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                continue;

            int step = stepCost(nx, ny);
            if (step == INT_MAX)
                continue;

            int ng = g + step;
            if (ng < gScore[ny][nx])
            {
                gScore[ny][nx] = ng;
                parent[ny][nx] = y * width + x;
                pq.push({ng + heuristic(nx, ny), ng, nx, ny});
            }
        }
    }

    return INT_MAX; // dst is unreachable
}

// ====================
// PATH LOOKUP TABLE

// One cached path between two cells: its distance and the set of regions it
// crosses. The region list is what makes targeted invalidation possible —
// when a region is inked, only the paths that ran through it are wrong.
class PathInfo
{
public:
    int distance;
    // Regions the path crosses, sorted and deduplicated.
    vector<int> regions;
    // Tombstone: an inked region invalidated this entry. The entry is kept so
    // invalidation stays a flat walk over one bucket, and the next find() for
    // the pair recomputes it in place. See PathTable::invalidateRegion.
    bool dead;
    PathInfo() : distance(INT_MAX), dead(false) {}
};

// Cache of cell-to-cell paths, plus the reverse index region -> paths that
// cross it. Both are filled at the same time, so inking a region drops
// exactly the entries that depended on it and nothing else.
//
// The cached distances describe the terrain (cost and ink), not the rails
// laid during the search, so a single table stays valid for every beam node.
class PathTable
{
private:
    unordered_map<long long, PathInfo> paths;
    // regionId -> keys of every cached path crossing that region. A key may
    // appear more than once only across distinct regions, never twice in the
    // same bucket, because PathInfo::regions is deduplicated.
    unordered_map<int, vector<long long>> pathsByRegion;

    // Regions known to be inked. A path computed after a region is inked can
    // never cross it (the region is impassable), so it never registers under
    // that region and no later entry can be missed. Entries are therefore
    // dropped purely by reverse index — no global generation counter, so
    // inking one region leaves every unrelated path cached.
    set<int> inkedRegions;

    int width, height;

    // Reused by find() so a miss does not allocate a fresh path vector.
    vector<Coord> scratchPath;

    long long makeKey(int fromIdx, int toIdx) const
    {
        // Canonical order: the table is symmetric.
        if (fromIdx > toIdx)
            swap(fromIdx, toIdx);
        return (long long)fromIdx * (long long)(width * height) + toIdx;
    }

    // Stores a computed path and indexes it under every region it crosses.
    // Private: entries are only ever created by find() on a miss, so a cached
    // distance can never disagree with what A* would return for the board.
    //
    // insert_or_assign overwrites any tombstone for the pair, clearing `dead`
    // with it. A revived entry re-registers under the regions the new path
    // crosses; those are necessarily un-inked, so the only bucket it could
    // land in twice is one it already sits in from the previous computation.
    // pushing a duplicate would make invalidateRegion visit it twice, which
    // the `dead` check there already absorbs, but the bucket would still grow
    // without bound across repeated revivals — so skip keys already present.
    const PathInfo *insert(Coord a, Coord b, PathInfo info)
    {
        long long key = makeKey(cellIndex(a), cellIndex(b));
        const bool revived = paths.count(key) != 0;
        for (int r : info.regions)
        {
            auto &bucket = pathsByRegion[r];
            if (revived &&
                std::find(bucket.begin(), bucket.end(), key) != bucket.end())
                continue;
            bucket.push_back(key);
        }
        auto res = paths.insert_or_assign(key, move(info));
        return &res.first->second;
    }

public:
    // Statistics, printed with the other per-turn beam numbers.
    int hits = 0, misses = 0, invalidations = 0;
    // Misses that landed on a tombstoned entry rather than an absent one, i.e.
    // recomputes caused by ink. Splits `misses` into cold lookups
    // (misses - deadEncountered) and re-work forced by invalidation.
    int deadEncountered = 0;

    void init(int w, int h)
    {
        width = w;
        height = h;
        clear();
    }

    void clear()
    {
        paths.clear();
        pathsByRegion.clear();
    }

    void resetStats()
    {
        hits = misses = invalidations = 0;
        deadEncountered = 0;
    }

    int cellIndex(Coord c) const { return c.y * width + c.x; }

    // Distance between two cells over terrain and ink, computed once and
    // reused. On a miss the path is built with A* against `board` and cached
    // along with the regions it crosses, so inking one of them drops it.
    //
    // `board` is templated only to keep PathTable independent of Map, which is
    // declared later; it is always the Map the search is running on. The step
    // cost is terrain cost, with inked cells impassable — deliberately blind
    // to rails, so one table stays valid across every beam node.
    //
    // Returns a pointer into the table, or nullptr when dst is unreachable.
    // Unreachability is not cached: it depends only on ink, and any ink event
    // that could change it also invalidates through invalidateRegion().
    template <typename BoardT>
    const PathInfo *find(const BoardT &board, Coord a, Coord b)
    {
        auto it = paths.find(makeKey(cellIndex(a), cellIndex(b)));
        if (it != paths.end())
        {
            if (!it->second.dead)
            {
                hits++;
                return &it->second;
            }
            // Present but invalidated: counted as a miss like any other, and
            // separately as re-work that ink forced.
            deadEncountered++;
        }
        misses++;

        scratchPath.clear();
        const int dist = aStarPath(
            a, b, width, height,
            [&](int x, int y)
            {
                if (board.tileInked(x, y))
                    return INT_MAX;
                return terrainCost(board.tileType(x, y));
            },
            scratchPath);
        if (dist == INT_MAX)
            return nullptr;

        PathInfo info;
        info.distance = dist;
        info.regions.reserve(scratchPath.size());
        for (const Coord &c : scratchPath)
            info.regions.push_back(board.tileRegion(c.x, c.y));
        sort(info.regions.begin(), info.regions.end());
        info.regions.erase(unique(info.regions.begin(), info.regions.end()),
                           info.regions.end());

        return insert(a, b, move(info));
    }

    // Marks every path that crossed the region as stale, so the next lookup
    // recomputes it against the new (inked) terrain. Paths that avoid the
    // region are untouched and stay cached.
    //
    // Entries are tombstoned rather than erased. Erasing meant unregistering
    // each dead key from the buckets of every other region it crossed, which
    // is a linear scan per region per path — on a board where hundreds of
    // paths cross an inked region that dominates the ink event. A flag makes
    // this one pass over a single bucket, and leaves the stale keys in the
    // other buckets harmless: re-inking cannot happen (inkedRegions guards
    // it), and find() rebuilds a dead entry in place on next use.
    void invalidateRegion(int regionId)
    {
        // Only a region that was not already inked changes the terrain.
        if (!inkedRegions.insert(regionId).second)
            return;

        auto it = pathsByRegion.find(regionId);
        if (it == pathsByRegion.end())
            return;

        for (long long key : it->second)
        {
            auto pit = paths.find(key);
            if (pit == paths.end() || pit->second.dead)
                continue; // already stale via another inked region
            pit->second.dead = true;
            invalidations++;
        }
        // The bucket has done its job: every path crossing this region is
        // now dead, and no live path can ever register here again.
        pathsByRegion.erase(it);
    }

    bool isRegionInked(int regionId) const
    {
        return inkedRegions.count(regionId) != 0;
    }

    // Live entries only: tombstones still occupy a slot until the pair is
    // looked up again, and reporting them as cached would overstate the table.
    size_t size() const
    {
        size_t live = 0;
        for (const auto &kv : paths)
            if (!kv.second.dead)
                live++;
        return live;
    }

    // Total slots held, tombstones included. Useful to see how much dead
    // weight the table is carrying between recomputes.
    size_t slotCount() const { return paths.size(); }
};

// ====================
// MAP

// Owns every piece of board state (Grid/Tile, Region, Town) and is the sole
// interface to it. Nothing outside Map touches a Tile, Region, Grid or Town
// container directly: callers go through the methods below, which may hand
// back references to those sub-classes when a caller needs to read them.
//
// A Map is copied wholesale by the beam search to represent a simulated
// future state, so it stays a plain value type.
class Map
{
public:
    Map() {}

    // Copying a Map happens once per beam child, so the scratch buffers are
    // deliberately left behind: they carry no value, only working space, and
    // copying them was pure overhead. Each copy lazily rebuilds its own.
    Map(const Map &o)
        : grid(o.grid), towns(o.towns), regionById(o.regionById),
          townCoord(o.townCoord), townCellFlag(o.townCellFlag),
          regionHasTown(o.regionHasTown), pathTable(o.pathTable) {}

    Map &operator=(const Map &o)
    {
        if (this != &o)
        {
            grid = o.grid;
            towns = o.towns;
            regionById = o.regionById;
            townCoord = o.townCoord;
            townCellFlag = o.townCellFlag;
            regionHasTown = o.regionHasTown;
            pathTable = o.pathTable;
            // scratch* intentionally not copied.
        }
        return *this;
    }

private:
    Grid grid;
    vector<Town> towns;
    unordered_map<int, Region> regionById;

    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // Flat per-cell town flag. A set<pair> lookup here cost a tree walk and
    // was hit millions of times per turn from the path/group scans.
    vector<char> townCellFlag;

    // Lookup table: regionId -> does this region contain a town?
    unordered_map<int, bool> regionHasTown;

    // Shared path cache. Not owned: every simulated Map points at the same
    // table, so a Map copy stays cheap and the cache is filled once.
    PathTable *pathTable = nullptr;

    // Scratch space for the BFS helpers. Mutable and deliberately excluded
    // from the Map's value: copying a beam state must not copy these.
    mutable vector<int> scratchSeen;
    mutable vector<int> scratchParent;
    mutable vector<int> scratchQueue;
    mutable int scratchStamp = 0;

    void ensureScratch() const
    {
        const size_t n = (size_t)grid.width * grid.height;
        if (scratchSeen.size() != n)
        {
            scratchSeen.assign(n, 0);
            scratchParent.assign(n, -1);
            scratchQueue.reserve(n);
            scratchStamp = 0;
        }
    }

    Region &getRegionAt(int x, int y)
    {
        return regionById[grid.get(x, y).regionId];
    }

public:
    // ---- geometry ----

    int width() const { return grid.width; }
    int height() const { return grid.height; }

    void setPathTable(PathTable *table) { pathTable = table; }
    PathTable *paths() const { return pathTable; }

    bool inBounds(int x, int y) const
    {
        return x >= 0 && x < grid.width && y >= 0 && y < grid.height;
    }

    // ---- construction / parsing ----

    // Reads the width/height + per-tile (regionId, type) block.
    void readTerrain(istream &in)
    {
        int w, h;
        in >> w >> h;
        grid = Grid(w, h);
        townCellFlag.assign(w * h, 0);
        regionById.clear();

        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int regionId, type;
                in >> regionId >> type;
                grid.get(x, y) = Tile(regionId, type);
                if (!regionById.count(regionId))
                {
                    regionById.emplace(regionId, Region(regionId));
                }
                regionById[regionId].coords.push_back(Coord(x, y));
            }
        }
    }

    // Reads the town block. Every wish (townId, otherTownId) found is appended
    // to outWishes so the caller keeps its own strategy-level list.
    void readTowns(istream &in, vector<pair<int, int>> &outWishes)
    {
        int townCount;
        in >> townCount;
        for (int i = 0; i < townCount; i++)
        {
            int townId, townX, townY;
            string desiredStr;
            in >> townId >> townX >> townY >> desiredStr;
            vector<int> desired;
            if (desiredStr != "x")
            {
                stringstream ss(desiredStr);
                string tmp;
                while (getline(ss, tmp, ','))
                    desired.push_back(stoi(tmp));
            }
            towns.emplace_back(townId, Coord(townX, townY), desired);
            getRegionAt(townX, townY).hasTown = true;

            townCoord[townId] = Coord(townX, townY);
            townCellFlag[townY * grid.width + townX] = 1;

            for (int other : desired)
            {
                outWishes.emplace_back(townId, other);
            }
        }

        for (auto &kv : regionById)
        {
            regionHasTown[kv.first] = kv.second.hasTown;
        }
    }

    // Reads the per-turn tile state block. Each active connection found is
    // recorded into outActiveConnections for the caller's own bookkeeping.
    void readTurnState(istream &in, map<pair<int, int>, bool> &outActiveConnections)
    {
        for (auto &kv : regionById)
        {
            kv.second.instability = 0;
            kv.second.inked = false;
        }

        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                int tracksOwner, instability;
                string inkedStr, partStr;
                in >> tracksOwner >> instability >> inkedStr >> partStr;
                bool inked = (inkedStr != "0");
                if (partStr != "x")
                {
                    stringstream ss(partStr);
                    string conn;
                    while (getline(ss, conn, ','))
                    {
                        int fromTownId, toTownId;
                        sscanf(conn.c_str(), "%d-%d", &fromTownId, &toTownId);
                        outActiveConnections[{fromTownId, toTownId}] = true;
                    }
                }
                Tile &tile = grid.get(x, y);
                // An inked region has been erased: whatever the referee
                // reports, it holds no usable rail any more.
                tile.tracksOwner = inked ? NO_OWNER : tracksOwner;
                tile.inked = inked;
                tile.instability = instability;

                // Mirror per-tile instability/ink onto the owning region.
                Region &region = regionById[tile.regionId];
                region.instability = max(region.instability, instability);
                if (inked)
                    region.inked = true;
            }
        }

        // Any region the referee reports as inked invalidates the paths that
        // crossed it, exactly as a simulated DISRUPT would.
        if (pathTable)
        {
            for (auto &kv : regionById)
            {
                if (kv.second.inked)
                    pathTable->invalidateRegion(kv.first);
            }
        }
    }

    // ---- tile queries ----

    int tileType(int x, int y) const { return grid.get(x, y).type; }
    int tileOwner(int x, int y) const { return grid.get(x, y).tracksOwner; }
    int tileRegion(int x, int y) const { return grid.get(x, y).regionId; }
    bool tileInked(int x, int y) const { return grid.get(x, y).inked; }

    bool isTownCell(int x, int y) const { return townCellFlag[y * grid.width + x] != 0; }
    bool hasRail(int x, int y) const { return grid.get(x, y).tracksOwner != NO_OWNER; }

    // A rail can be placed only on an empty, non-town, passable tile whose
    // region has not been erased with ink. An inked region is gone for good,
    // so building there is always a wasted (and rejected) action.
    bool canPlaceRail(int x, int y) const
    {
        if (!inBounds(x, y))
            return false;
        if (isTownCell(x, y))
            return false;
        if (hasRail(x, y))
            return false;
        if (isInked(x, y))
            return false;
        return terrainCost(grid.get(x, y).type) != INT_MAX;
    }

    // True when the tile, or the region it belongs to, has been inked. Both
    // are checked because a region can be inked mid-search (by a simulated
    // DISRUPT) between two beam depths.
    bool isInked(int x, int y) const
    {
        const Tile &tile = grid.get(x, y);
        if (tile.inked)
            return true;
        auto it = regionById.find(tile.regionId);
        return it != regionById.end() && it->second.inked;
    }

    int railCost(int x, int y) const { return terrainCost(grid.get(x, y).type); }

    // Places a rail, applying the neutral-owner rule when both players
    // target the same tile on the same turn.
    void placeRail(int x, int y, int owner)
    {
        Tile &tile = grid.get(x, y);
        if (tile.tracksOwner == NO_OWNER)
            tile.tracksOwner = owner;
        else if (tile.tracksOwner != owner)
            tile.tracksOwner = NEUTRAL_OWNER;
    }

    // Speculative rail placement: sets the owner and hands back what was
    // there, so the caller can put it back. Only tracksOwner changes, so an
    // undo is exact — nothing else in the Map depends on it.
    int setRailOwner(int x, int y, int owner)
    {
        Tile &tile = grid.get(x, y);
        const int previous = tile.tracksOwner;
        tile.tracksOwner = owner;
        return previous;
    }

    // A cell is traversable by a connection path if it holds a rail or a town.
    // Rails in an inked region no longer exist, so they never connect.
    bool isConnectable(int x, int y) const
    {
        if (isTownCell(x, y))
            return true;
        return hasRail(x, y) && !isInked(x, y);
    }

    // ---- town queries ----

    bool hasTown(int townId) const { return townCoord.count(townId) != 0; }
    Coord townCoordOf(int townId) const { return townCoord.at(townId); }
    const vector<Town> &allTowns() const { return towns; }

    // ---- region queries ----

    bool regionContainsTown(int regionId) const
    {
        auto it = regionHasTown.find(regionId);
        return it != regionHasTown.end() && it->second;
    }

    bool regionInked(int regionId) const
    {
        auto it = regionById.find(regionId);
        return it != regionById.end() && it->second.inked;
    }

    vector<int> allRegionIds() const
    {
        vector<int> ids;
        ids.reserve(regionById.size());
        for (auto &kv : regionById)
            ids.push_back(kv.first);
        {
            // PROFILE(sortRegionIds);
            sort(ids.begin(), ids.end());
        }
        return ids;
    }

    // Raises a region's instability, inking it (and erasing every rail it
    // contains) once it crosses the threshold.
    void disruptRegion(int regionId)
    {
        auto it = regionById.find(regionId);
        if (it == regionById.end())
            return;
        Region &region = it->second;
        if (region.inked)
            return;

        region.instability += DISRUPT_INSTABILITY_GAIN;
        if (region.instability >= INK_INSTABILITY_THRESHOLD)
        {
            region.inked = true;
            for (const Coord &c : region.coords)
            {
                Tile &tile = grid.get(c.x, c.y);
                tile.inked = true;
                tile.tracksOwner = NO_OWNER;
            }
            // The region just became impassable: every cached path crossing
            // it is stale and must be recomputed on next use.
            if (pathTable)
                pathTable->invalidateRegion(regionId);
        }
    }

    // ---- rail groups ----

    // Every cell reachable from a town through an unbroken run of rails and
    // towns. This is the "rail group connected to the town" of the spec.
    vector<Coord> railGroupOf(Coord townCell) const
    {
        // PROFILE(railGroupOf);
        vector<Coord> group;
        if (!inBounds(townCell.x, townCell.y))
            return group;

        const int W = grid.width, H = grid.height;
        ensureScratch();
        scratchStamp++;
        const int stamp = scratchStamp;

        // Reuses the shared flood-fill scratch: same generation-stamp trick
        // as connectionPathInto, so no allocation per call.
        scratchQueue.clear();
        scratchQueue.push_back(townCell.y * W + townCell.x);
        scratchSeen[townCell.y * W + townCell.x] = stamp;

        for (size_t head = 0; head < scratchQueue.size(); head++)
        {
            const int curIdx = scratchQueue[head];
            const int cx = curIdx % W, cy = curIdx / W;
            group.push_back(Coord(cx, cy));

            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                const int nIdx = ny * W + nx;
                if (scratchSeen[nIdx] == stamp)
                    continue;
                if (!isConnectable(nx, ny))
                    continue;
                scratchSeen[nIdx] = stamp;
                scratchQueue.push_back(nIdx);
            }
        }
        return group;
    }

    // ---- connections ----

    // Shortest rail/town path between two towns, honouring the
    // NORTH/EAST/SOUTH/WEST tie-break. Empty if the towns are not linked.
    vector<Coord> connectionPath(Coord from, Coord to) const
    {
        vector<Coord> path;
        connectionPathInto(from, to, path);
        return path;
    }

    // Same BFS, writing into a caller-owned buffer. The scratch state lives
    // in the Map (mutable, not part of its value) so the hot callers below
    // pay no allocation at all: this runs hundreds of thousands of times per
    // turn and the per-call vector<vector<>> pair used to dominate the turn.
    void connectionPathInto(Coord from, Coord to, vector<Coord> &path) const
    {
        // PROFILE(connectionPathProfile);
        path.clear();
        if (!inBounds(from.x, from.y) || !inBounds(to.x, to.y))
            return;

        const int W = grid.width, H = grid.height;
        ensureScratch();
        // A generation stamp replaces clearing the visited array each call.
        scratchStamp++;
        const int stamp = scratchStamp;

        const int fromIdx = from.y * W + from.x;
        const int toIdx = to.y * W + to.x;

        scratchSeen[fromIdx] = stamp;
        scratchParent[fromIdx] = -1;

        scratchQueue.clear();
        scratchQueue.push_back(fromIdx);

        bool found = (fromIdx == toIdx);
        for (size_t head = 0; head < scratchQueue.size() && !found; head++)
        {
            const int curIdx = scratchQueue[head];
            const int cx = curIdx % W, cy = curIdx / W;

            // Neighbours are visited in NORTH/EAST/SOUTH/WEST order, so the
            // first parent recorded for a cell already follows the priority.
            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                const int nIdx = ny * W + nx;
                if (scratchSeen[nIdx] == stamp)
                    continue;
                if (!isConnectable(nx, ny))
                    continue;
                scratchSeen[nIdx] = stamp;
                scratchParent[nIdx] = curIdx;
                if (nIdx == toIdx)
                {
                    found = true;
                    break;
                }
                scratchQueue.push_back(nIdx);
            }
        }

        if (!found)
            return;

        for (int cur = toIdx; cur != -1; cur = scratchParent[cur])
            path.push_back(Coord(cur % W, cur / W));
        reverse(path.begin(), path.end());
    }
};

// ====================
// CHOICES

// The two cells that would join a wish's two rail groups, and how far apart
// they are. Not an action: it says where the gap is, and action sets are
// generated to close it.
class GroupLink
{
public:
    Coord src, dst;
    int distance;
    GroupLink(Coord s = {}, Coord d = {}, int dist = INT_MAX)
        : src(s), dst(d), distance(dist) {}
};

// One turn's worth of building: the cells to lay rail on this turn, in the
// order they were decided. A turn buys PAINT_PER_TURN paint and each cell
// costs its terrain, so a set holds at most PAINT_PER_TURN cells and often
// fewer — a single mountain (cost 3) spends the whole turn.
class ActionSet
{
public:
    vector<Coord> cells;
    // Paint spent by `cells`, i.e. the sum of their terrain costs.
    int cost = 0;
    // Gap left across every open wish once these cells are laid. This is how
    // turn plans are ranked against each other, both inside the turn and when
    // the branching cap has to drop some.
    int resultingGap = INT_MAX;

    bool empty() const { return cells.empty(); }
};

// One turn part-way through being decided: the rails chosen so far already
// laid on `board`, and what is left of the turn's paint. The intra-turn beam
// keeps a handful of these alive and extends each by one rail at a time; only
// `action` outlives the planning.
// A turn part-way through being decided. It holds no board of its own: the
// planner keeps a single shared Map and lays this line's `action.cells` on it
// when it needs to, undoing them afterwards. A Tile owns a vector, so copying
// a grid is one heap allocation per cell — far more than replaying three
// rails costs.
class PlacementLine
{
public:
    ActionSet action;
    int paintLeft = PAINT_PER_TURN;
    // Closest this line's rails have come to each open wish so far, so a
    // child only has to fold in the one cell it adds.
    vector<int> bestPerWish;
};

// ====================
// BEAM SEARCH STATE

// Per-turn beam search statistics, printed to stderr at the end of the
// turn. Reset by BeamSearch::run() on every call.
class BeamStats
{
public:
    // Deepest depth level actually expanded. The current state is depth 0,
    // so a value of N means N plies were simulated beyond it.
    int maxDepth;
    // Every child state created this turn, across all depths.
    int totalStates;

    // Per depth:
    //  - actionsCreated  : turn plans the intra-turn beam handed back for us
    //                      to play. Every one of them is expanded.
    //  - placementStates : boards the intra-turn beam built to find them,
    //                      ours and the opponent's together. This is where
    //                      the turn's time actually goes.
    //  - statesPerDepth  : child states produced, before the Bwidth cut.
    vector<int> actionsCreated;
    vector<int> placementStates;
    vector<int> statesPerDepth;
    // Nodes whose expansion was cut short by the time budget.
    int truncatedByTime;
    // Turn plannings the budget cut short. Their plans are still played, but
    // they leave paint unspent, so a nonzero count here means the search is
    // handing back turns it had not finished thinking about.
    int placementsTruncated;

    BeamStats() { reset(); }

    void reset()
    {
        maxDepth = 0;
        totalStates = 0;
        truncatedByTime = 0;
        placementsTruncated = 0;
        actionsCreated.assign(MAX_DEPTH, 0);
        placementStates.assign(MAX_DEPTH, 0);
        statesPerDepth.assign(MAX_DEPTH, 0);
    }

    int totalActionsCreated() const
    {
        int sum = 0;
        for (int d = 0; d < maxDepth; d++)
            sum += actionsCreated[d];
        return sum;
    }

    int totalPlacementStates() const
    {
        int sum = 0;
        for (int d = 0; d < maxDepth; d++)
            sum += placementStates[d];
        return sum;
    }

    // Mean number of turn plans expanded per state, i.e. the outer beam's
    // branching factor. The states expanded at a depth are the ones the
    // previous depth produced, capped by the beam width; depth 0 expands
    // the single root.
    double avgActionsPerState() const
    {
        int states = 0;
        for (int d = 0; d < maxDepth; d++)
            states += (d == 0) ? 1 : min(statesPerDepth[d - 1], BEAM_WIDTH);
        if (states == 0)
            return 0.0;
        return (double)totalActionsCreated() / states;
    }

    void print() const
    {
        fprintf(stderr, "%-32s max depth : %d / %d  (current state = depth 0)\n",
                "beamStats", maxDepth, MAX_DEPTH);
        fprintf(stderr, "%-32s avg actions / state : %.2f\n", "beamStats",
                avgActionsPerState());
        fprintf(stderr, "%-32s turn plans expanded : %d\n", "beamStats",
                totalActionsCreated());
        fprintf(stderr, "%-32s intra-turn states : %d\n", "beamStats",
                totalPlacementStates());
        fprintf(stderr, "%-32s total states : %d\n", "beamStats", totalStates);
        if (truncatedByTime)
            fprintf(stderr, "%-32s time-truncated expansions : %d\n", "beamStats",
                    truncatedByTime);
        if (placementsTruncated)
            fprintf(stderr, "%-32s time-truncated turn plans : %d\n", "beamStats",
                    placementsTruncated);

        fprintf(stderr, "%-32s %-7s %12s %12s %14s\n", "beamStats",
                "depth", "turn plans", "intra-turn", "states created");
        for (int d = 0; d < maxDepth; d++)
            fprintf(stderr, "%-32s %-7d %12d %12d %14d\n", "beamStats",
                    d + 1, actionsCreated[d], placementStates[d],
                    statesPerDepth[d]);
    }
};

class BeamNode
{
public:
    Map state;
    map<pair<int, int>, bool> active;
    int score;
    // Points banked along this line: every simulated turn adds what each
    // player earned from the connections active at that moment, the way
    // the referee pays them out. `turns` is how many turns produced them,
    // used to keep lines of different depths comparable.
    int bankedSelf, bankedOther;
    int turns;
    // The action set played at the root of this line, i.e. the rails we
    // would actually place this turn.
    ActionSet rootAction;
    int rootDisrupt;

    BeamNode()
        : score(0), bankedSelf(0), bankedOther(0), turns(0),
          rootDisrupt(-1) {}
};

// ====================
// BEAM SEARCH

// The search itself. Game hands it the turn's starting position through
// setup(), run() explores forward from it, and the best line's root move is
// what Game plays. Everything the search needs to score and advance a state
// lives here; Game keeps only the I/O and the real board.
class BeamSearch
{
public:
    // ---- position under search, set once per turn by setup() ----
    int myId = 0;
    int foeId = 0;
    // The turn's real position. Only ever read, to seed the root node, which
    // takes its own copy: the search never mutates the caller's board.
    const Map *startBoard = nullptr;
    map<pair<int, int>, bool> startActive;
    // The turn's wishes, one entry per pair. The referee lists a wish from
    // both of its towns, so (a,b) and (b,a) both arrive; keeping both would
    // count every connection's income and every gap twice, and would make the
    // intra-turn beam flood-fill each town twice per state for nothing.
    vector<pair<int, int>> wishes;
    // Turn clock, so the search can stop before the referee's limit.
    std::chrono::steady_clock::time_point turnStart;
    bool firstTurn = true;
    // When the search has to be finished. Set by run() before anything reads
    // it, and a member rather than a local because the planning below is deep
    // enough to need to test it from inside.
    std::chrono::steady_clock::time_point deadline;

    // Scratch for openGapTotal's component labelling, reused across calls so
    // the hottest function in the search does not allocate.
    mutable vector<int> gapLabel;
    mutable vector<int> gapQueue;
    // Generation stamps, so the per-cell buffers are never re-zeroed: a value
    // counts as present only when its stamp matches the current run. Without
    // this, every call memset components*N ints (15k+ on a dense board) before
    // doing any work, which was the bulk of openGapTotal's cost.
    mutable vector<int> gapLabelStamp;
    mutable vector<int> gapDistStamp;
    // Labels and distances are stamped separately: one openGapTotal call is a
    // single labelling run followed by a single flood run, and bumping one
    // must not invalidate the other.
    mutable int gapStamp = 0;
    mutable int gapDistRun = 0;
    // Components that have reached a cell, so meeting floods are found by
    // walking arrivals rather than scanning every component at every cell.
    mutable vector<int> gapArrivalHead;
    mutable vector<int> gapArrivalNext;
    // Cells whose arrival list was used, so only those get reset.
    mutable vector<int> gapTouched;
    // Per-component distance fields, laid out [component * N + cell].
    mutable vector<int> gapDist;
    // BFS frontier of (cell, component) pairs, packed as cell * components + component.
    mutable vector<int> gapFrontier;
    // Shortest route found between each ordered pair of components.
    mutable vector<int> gapPairBest;
    // Index permutation used to rank a round's lines without copying boards.
    vector<int> grownOrder;
    // The single board every planning call mutates in place, reused across
    // calls so its buffers are allocated once and not per turn.
    Map scratchBoard;
    // Planner scratch, reused across calls for the same reason.
    vector<int> scratchUndo;
    vector<Coord> scratchCandidates;

    bool outOfTime() const
    {
        return std::chrono::steady_clock::now() >= deadline;
    }

    BeamStats stats;

    void setup(int selfId, int otherId, const Map &turnBoard,
               const map<pair<int, int>, bool> &turnActive,
               const vector<pair<int, int>> &turnWishes,
               std::chrono::steady_clock::time_point start, bool isFirstTurn)
    {
        myId = selfId;
        foeId = otherId;
        startBoard = &turnBoard;
        startActive = turnActive;

        wishes.clear();
        set<pair<int, int>> seenWish;
        for (const auto &wish : turnWishes)
        {
            int a = wish.first, b = wish.second;
            auto key = a < b ? make_pair(a, b) : make_pair(b, a);
            if (seenWish.insert(key).second)
                wishes.push_back(key);
        }

        turnStart = start;
        firstTurn = isFirstTurn;
    }

    // ---- rail group linking ----

    // Cheapest (Manhattan) pair of cells across the two rail groups attached
    // to towns `a` and `b`. Returns a GroupLink with distance INT_MAX when
    // either town is missing or has no group, and distance 0 when the two
    // groups share a cell, i.e. the towns are already linked.
    static GroupLink closestGroupLink(const Map &board, int a, int b)
    {
        GroupLink best;
        if (!board.hasTown(a) || !board.hasTown(b))
            return best;

        vector<Coord> groupA = board.railGroupOf(board.townCoordOf(a));
        vector<Coord> groupB = board.railGroupOf(board.townCoordOf(b));
        if (groupA.empty() || groupB.empty())
            return best;

        // Same group: distance is 0 and no pair of distinct cells needs to be
        // scanned. Detected on a cell set rather than trusting the cross
        // product, so a shared cell is caught even when it is not the minimum.
        set<pair<int, int>> cellsA;
        for (const Coord &ca : groupA)
            cellsA.insert({ca.x, ca.y});
        for (const Coord &cb : groupB)
            if (cellsA.count({cb.x, cb.y}))
                return GroupLink(cb, cb, 0);

        // Cross-product of both groups: keep the shortest link.
        for (const Coord &ca : groupA)
        {
            for (const Coord &cb : groupB)
            {
                int d = abs(ca.x - cb.x) + abs(ca.y - cb.y);
                if (d < best.distance)
                    best = GroupLink(ca, cb, d);
            }
        }
        return best;
    }

    // ---- turn planning ----

    // The wishes one planning call can act on, resolved to coordinates up
    // front. Purely a cache: every field is derivable from `wishes` plus the
    // board, and it exists only because the alternative is two std::map
    // lookups per wish per candidate on the hottest path in the search.
    //
    // It also fixes the indexing. Towns that no longer exist are dropped here
    // once, so `townA[i]`, `townB[i]` and a line's `bestPerWish[i]` all agree
    // on what `i` means; walking `wishes` directly would have to re-skip those
    // entries and the indices would drift.
    class WishGeometry
    {
    public:
        vector<Coord> townA, townB;
        // Straight-line distance of each wish with nothing built: the value a
        // line's per-wish best starts at.
        vector<int> baseline;
    };

    static WishGeometry wishGeometry(const Map &board,
                                     const vector<pair<int, int>> &wishes)
    {
        WishGeometry geo;
        for (const auto &wish : wishes)
        {
            if (!board.hasTown(wish.first) || !board.hasTown(wish.second))
                continue;
            Coord ta = board.townCoordOf(wish.first);
            Coord tb = board.townCoordOf(wish.second);
            geo.townA.push_back(ta);
            geo.townB.push_back(tb);
            geo.baseline.push_back(abs(ta.x - tb.x) + abs(ta.y - tb.y));
        }
        return geo;
    }

    // Straight-line stand-in for the real gap, used to rank turns while they
    // are still being built: for each open wish, the shortest Manhattan
    // distance from either of its towns to a cell the line has laid this
    // turn. No rail groups and no flood fill.
    //
    // It is deliberately not the true gap — it cannot see whether a rail
    // actually joins anything. That accuracy is not worth its price here,
    // because every surviving line is rescored with the real heuristic once
    // the turn is played, and a line that only looked good under this
    // approximation is discarded there.
    //
    // Folds one newly laid cell into a line's per-wish bests and returns the
    // new total. A child differs from its parent by exactly one cell, so the
    // whole score is never recomputed: each candidate costs one pass over the
    // wishes rather than one pass over wishes times cells laid.
    static int extendManhattanGap(const WishGeometry &geo, Coord laid,
                                  vector<int> &bestPerWish)
    {
        int total = 0;
        for (size_t i = 0; i < geo.baseline.size(); i++)
        {
            int da = abs(laid.x - geo.townA[i].x) + abs(laid.y - geo.townA[i].y);
            int db = abs(laid.x - geo.townB[i].x) + abs(laid.y - geo.townB[i].y);
            bestPerWish[i] = min(bestPerWish[i], max(da, db));
            total += bestPerWish[i];
        }
        return total;
    }

    // Ranks two turn plans: the one that leaves the least gap wins, and among
    // plans that leave the same gap, the one that spent more paint — unspent
    // paint is simply lost at the end of the turn.
    static bool betterPlacement(const ActionSet &a, const ActionSet &b)
    {
        if (a.resultingGap != b.resultingGap)
            return a.resultingGap < b.resultingGap;
        return a.cost > b.cost;
    }

    // Every affordable cell touching the network. No attempt to judge which
    // helps: the beam scores them all and keeps the best.
    // Fills `cells` rather than returning it: this runs once per line per
    // round, and a fresh vector each time is a malloc/free pair for nothing.
    static void placementCandidates(const Map &board, int paintLeft,
                                    vector<Coord> &cells)
    {
        // PROFILE(placementCandidates);

        cells.clear();
        if (paintLeft <= 0)
            return;

        const int W = board.width(), H = board.height();

        for (int y = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++)
            {
                if (!board.canPlaceRail(x, y))
                    continue;
                if (board.railCost(x, y) > paintLeft)
                    continue;

                // A rail in open ground joins nothing and can never pay.
                bool touches = false;
                for (int k = 0; k < 4 && !touches; k++)
                {
                    int nx = x + DIR_X[k], ny = y + DIR_Y[k];
                    if (board.inBounds(nx, ny) && board.isConnectable(nx, ny))
                        touches = true;
                }
                if (touches)
                    cells.push_back(Coord(x, y));
            }
        }
    }

    // One turn's rails, decided one at a time: each round extends every
    // surviving line by one cell and keeps the `keep` best, so a line's second
    // rail is chosen knowing where its first went. A line ends when its paint
    // buys nothing; results come back best first.
    //
    // Interruptible: a line that has laid one rail is already a legal turn,
    // just one with paint left over. `statesSeen` counts states built.
    vector<ActionSet> generateActionSets(const Map &startBoard,
                                         const vector<pair<int, int>> &wishes,
                                         int owner, int keep, int &statesSeen)
    {
        // PROFILE(generateActionSets);

        vector<ActionSet> finished;
        bool aborted = false;

        // Resolved once so scoring never goes back to the town map.
        const WishGeometry geo = wishGeometry(startBoard, wishes);

        // One board for the whole planning, mutated in place. A line's rails
        // are laid before it is worked on and undone straight after, so every
        // line sees the same starting position without anyone copying a grid.
        Map &board = scratchBoard;
        {
            // PROFILE(stateCopy);
            board = startBoard;
        }

        // Owners displaced by the rails currently laid, innermost last.
        vector<int> &undo = scratchUndo;
        // Reused across every line and round, so the planner allocates nothing
        // per candidate scan.
        vector<Coord> &candidates = scratchCandidates;

        auto applyLine = [&](const PlacementLine &line)
        {
            undo.clear();
            for (const Coord &c : line.action.cells)
                undo.push_back(board.setRailOwner(c.x, c.y, owner));
        };
        auto undoLine = [&](const PlacementLine &line)
        {
            for (size_t i = line.action.cells.size(); i-- > 0;)
                board.setRailOwner(line.action.cells[i].x,
                                   line.action.cells[i].y, undo[i]);
        };

        vector<PlacementLine> lines(1);
        lines[0].paintLeft = PAINT_PER_TURN;
        lines[0].bestPerWish = geo.baseline;

        vector<PlacementLine> grown;

        while (!lines.empty())
        {
            grown.clear();

            for (PlacementLine &line : lines)
            {
                if (outOfTime())
                {
                    aborted = true;
                    break;
                }

                applyLine(line);

                placementCandidates(board, line.paintLeft, candidates);

                // Nothing affordable left: this line's turn is over.
                if (candidates.empty())
                {
                    undoLine(line);
                    if (!line.action.empty())
                    {
                        finished.push_back(move(line.action));
                        // Moved from, so the abort harvest skips it.
                        line.action.cells.clear();
                    }
                    continue;
                }

                for (const Coord &c : candidates)
                {
                    // PROFILE(candidateCreation);

                    PlacementLine child;
                    child.action = line.action;

                    const int cost = board.railCost(c.x, c.y);
                    child.paintLeft = line.paintLeft - cost;
                    child.action.cells.push_back(c);
                    child.action.cost += cost;

                    child.bestPerWish = line.bestPerWish;
                    child.action.resultingGap =
                        extendManhattanGap(geo, c, child.bestPerWish);

                    grown.push_back(move(child));
                }

                undoLine(line);

                if (aborted)
                    break;
            }

            statesSeen += (int)grown.size();

            if (aborted)
            {
                // Keep whatever is already playable rather than drop it.
                for (PlacementLine &line : lines)
                    if (!line.action.empty())
                        finished.push_back(move(line.action));
                for (PlacementLine &child : grown)
                    finished.push_back(move(child.action));
                break;
            }

            if (grown.empty())
                break;

            // Reduce to the beam width before spending another rail on them.
            //
            // Ordering an index permutation rather than the lines themselves,
            // so a swap moves an int instead of a line's vectors.
            {
                // PROFILE(sortRoundLines);
                const int survivors = min<int>(keep, (int)grown.size());

                grownOrder.resize(grown.size());
                for (size_t i = 0; i < grown.size(); i++)
                    grownOrder[i] = (int)i;

                partial_sort(grownOrder.begin(), grownOrder.begin() + survivors,
                             grownOrder.end(),
                             [&grown](int a, int b)
                             { return betterPlacement(grown[a].action,
                                                      grown[b].action); });

                lines.clear();
                lines.reserve(survivors);
                for (int i = 0; i < survivors; i++)
                    lines.push_back(move(grown[grownOrder[i]]));
            }
        }

        // Lines finish at different rounds, so rank the survivors together.
        {
            // PROFILE(sortFinishedLines);
            sort(finished.begin(), finished.end(), betterPlacement);
        }
        if ((int)finished.size() > keep)
            finished.resize(keep);

        if (aborted)
            stats.placementsTruncated++;

        return finished;
    }

    // ---- "Disrupt choice" creation ----

    // A region is a candidate when it is not inked, holds no town, carries at
    // least one opponent rail, and — counting unique rails per player across
    // the active connections running through it — the opponent owns strictly
    // more than we do. Only the single best region is returned (-1 if none).
    int buildDisruptChoice(Map &board,
                           const vector<pair<int, int>> &wishes,
                           int selfId, int otherId) const
    {
        // PROFILE(buildDisruptChoice);

        // Collect the cells of every currently active connection once.
        // Interruptible: a partial set just means fewer candidate regions.
        vector<vector<Coord>> connectionPaths;
        for (const auto &wish : wishes)
        {
            int a = wish.first, b = wish.second;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;
            vector<Coord> path = board.connectionPath(board.townCoordOf(a), board.townCoordOf(b));
            if (!path.empty())
                connectionPaths.push_back(move(path));
        }

        // Per region, count each player's unique rails lying on a connection.
        unordered_map<int, set<pair<int, int>>> selfCells, otherCells;
        for (const auto &path : connectionPaths)
        {
            for (const Coord &c : path)
            {
                if (!board.hasRail(c.x, c.y))
                    continue;
                int owner = board.tileOwner(c.x, c.y);
                int region = board.tileRegion(c.x, c.y);
                if (owner == selfId)
                    selfCells[region].insert({c.x, c.y});
                else if (owner == otherId)
                    otherCells[region].insert({c.x, c.y});
            }
        }

        int best = -1;
        int bestOtherCount = 0;
        int bestDiff = 0;

        for (int regionId : board.allRegionIds())
        {
            if (board.regionInked(regionId))
                continue;
            if (board.regionContainsTown(regionId))
                continue;

            int otherCount = (int)otherCells[regionId].size();
            int selfCount = (int)selfCells[regionId].size();
            if (otherCount == 0)
                continue; // needs opponent rails
            if (otherCount <= selfCount)
                continue; // must hurt them more than us

            int diff = otherCount - selfCount;
            if (diff > bestDiff || (diff == bestDiff && otherCount > bestOtherCount))
            {
                best = regionId;
                bestDiff = diff;
                bestOtherCount = otherCount;
            }
        }

        return best;
    }

    // ---- heuristic ----

    // Weight of one cell of remaining gap on an unbuilt wish. A gap cell is
    // worth rather less than a point of per-turn income: closing a gap only
    // pays off once the connection completes, while income is banked now.
    static const int GAP_PENALTY = 1;

    // Points the two players earn *this turn*: each active connection pays a
    // player 1 point per rail they own along its path. Called once per
    // simulated turn and accumulated into the node, mirroring how the referee
    // awards points, so a state's banked income is a real running total
    // rather than a snapshot recomputed from the final board.
    // Interruptible: an unmeasured wish pays nobody. That loses income on
    // both sides of the same subtraction, so the comparison between the two
    // players stays roughly fair even on a turn that ran out of clock.
    void turnIncome(Map &board, const vector<pair<int, int>> &wishes,
                    int selfId, int otherId, int &outSelf, int &outOther) const
    {
        outSelf = 0;
        outOther = 0;

        for (const auto &wish : wishes)
        {
            int a = wish.first, b = wish.second;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;
            vector<Coord> path = board.connectionPath(board.townCoordOf(a), board.townCoordOf(b));
            if (path.empty())
                continue;

            for (const Coord &c : path)
            {
                int owner = board.tileOwner(c.x, c.y);
                if (owner == selfId)
                    outSelf++;
                else if (owner == otherId)
                    outOther++;
            }
        }
    }

    // How far every still-unconnected wish is from paying out, as the summed
    // distance between the two rail groups already attached to its towns.
    // This is the only forward-looking term: the income above cannot see a
    // connection that does not exist yet, so without this the search has no
    // gradient to follow towards building one.
    // One wish costs two flood fills plus a cross product over the two groups
    // they find, so on a board with many towns and large rail fields a single
    // call is milliseconds, not microseconds. It is therefore interruptible:
    // the wishes already measured stand and the rest are treated as closed,
    // which understates the gap. That biases a score the search is about to
    // stop trusting anyway, and is much cheaper than overrunning the turn.
    // Labels every rail/town cell with the component it belongs to, -1
    // elsewhere. Returns the number of components.
    int labelComponents(const Map &board) const
    {
        const int W = board.width(), H = board.height();
        const int N = W * H;

        if ((int)gapLabel.size() != N)
        {
            gapLabel.assign(N, -1);
            gapLabelStamp.assign(N, 0);
        }
        gapStamp++;
        const int stamp = gapStamp;

        int components = 0;

        for (int start = 0; start < N; start++)
        {
            if (gapLabelStamp[start] == stamp ||
                !board.isConnectable(start % W, start / W))
                continue;

            const int label = components++;
            gapQueue.clear();
            gapQueue.push_back(start);
            gapLabel[start] = label;
            gapLabelStamp[start] = stamp;

            for (size_t head = 0; head < gapQueue.size(); head++)
            {
                const int cx = gapQueue[head] % W, cy = gapQueue[head] / W;
                for (int k = 0; k < 4; k++)
                {
                    const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                        continue;
                    const int nIdx = ny * W + nx;
                    if (gapLabelStamp[nIdx] == stamp || !board.isConnectable(nx, ny))
                        continue;
                    gapLabel[nIdx] = label;
                    gapLabelStamp[nIdx] = stamp;
                    gapQueue.push_back(nIdx);
                }
            }
        }
        return components;
    }

    // Component at `idx`, or -1 if none. Stale stamps read as absent.
    int labelAt(int idx) const
    {
        return gapLabelStamp[idx] == gapStamp ? gapLabel[idx] : -1;
    }

    // Records a route of `total` cells between two components, keeping the
    // shortest seen. Symmetric.
    void recordPair(int a, int b, int components, int total) const
    {
        int &slot = gapPairBest[(size_t)a * components + b];
        if (total < slot)
        {
            slot = total;
            gapPairBest[(size_t)b * components + a] = total;
        }
    }

    // Every component floods outwards at once over buildable ground. Where two
    // floods meet, their distances sum to a shortest connecting route, so one
    // pass settles every pair. Fills gapPairBest.
    void floodComponentDistances(const Map &board, int components) const
    {
        const int W = board.width(), H = board.height();
        const int N = W * H;

        const size_t need = (size_t)components * N;
        if (gapDist.size() < need)
        {
            gapDist.resize(need);
            gapDistStamp.assign(need, 0);
        }
        gapDistRun++;
        const int stamp = gapDistRun;

        // Arrival lists: for each cell, the components that have reached it,
        // as an intrusive singly-linked list over gapArrivalNext.
        if ((int)gapArrivalHead.size() != N)
            gapArrivalHead.assign(N, -1);
        if (gapArrivalNext.size() < need)
            gapArrivalNext.resize(need);

        gapPairBest.assign((size_t)components * components, INT_MAX);
        gapFrontier.clear();
        gapTouched.clear();

        for (int idx = 0; idx < N; idx++)
        {
            const int label = labelAt(idx);
            if (label == -1)
                continue;
            const size_t slot = (size_t)label * N + idx;
            gapDist[slot] = 0;
            gapDistStamp[slot] = stamp;
            gapArrivalNext[slot] = gapArrivalHead[idx];
            if (gapArrivalHead[idx] == -1)
                gapTouched.push_back(idx);
            gapArrivalHead[idx] = label;
            gapFrontier.push_back(idx * components + label);
        }

        for (size_t head = 0; head < gapFrontier.size(); head++)
        {
            const int label = gapFrontier[head] % components;
            const int cur = gapFrontier[head] / components;
            const int d = gapDist[(size_t)label * N + cur];
            const int cx = cur % W, cy = cur / W;

            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                const int nIdx = ny * W + nx;
                const int other = labelAt(nIdx);

                // Reached another component: adjacent, so the route ends here.
                if (other != -1)
                {
                    if (other != label)
                        recordPair(label, other, components, d);
                    continue;
                }
                // Ink and impassable terrain stop a flood, so a reported gap
                // is a route that could really be built.
                if (!board.canPlaceRail(nx, ny))
                    continue;

                const size_t slot = (size_t)label * N + nIdx;
                if (gapDistStamp[slot] == stamp)
                    continue;
                gapDist[slot] = d + 1;
                gapDistStamp[slot] = stamp;
                gapFrontier.push_back(nIdx * components + label);

                // Only the floods that have actually arrived here, rather than
                // every component in the board.
                for (int o = gapArrivalHead[nIdx]; o != -1;
                     o = gapArrivalNext[(size_t)o * N + nIdx])
                    recordPair(label, o, components, gapDist[slot] +
                                                         gapDist[(size_t)o * N + nIdx]);

                gapArrivalNext[slot] = gapArrivalHead[nIdx];
                if (gapArrivalHead[nIdx] == -1)
                    gapTouched.push_back(nIdx);
                gapArrivalHead[nIdx] = label;
            }
        }

        // Reset only what was used, so the next call starts clean without
        // touching the whole board.
        for (int idx : gapTouched)
            gapArrivalHead[idx] = -1;
    }

    // Distance still separating the two towns of `wish`, or -1 when there is
    // nothing to measure: a town with no component, or both on the same one.
    int wishGap(const Map &board, const pair<int, int> &wish, int components) const
    {
        if (!board.hasTown(wish.first) || !board.hasTown(wish.second))
            return -1;

        const int W = board.width();
        Coord ca = board.townCoordOf(wish.first), cb = board.townCoordOf(wish.second);
        const int la = labelAt(ca.y * W + ca.x), lb = labelAt(cb.y * W + cb.x);
        if (la == -1 || lb == -1 || la == lb)
            return -1;

        const int best = gapPairBest[(size_t)la * components + lb];
        // Walled apart by ink or impassable terrain: nothing to steer towards.
        return best == INT_MAX ? -1 : best;
    }

    int openGapTotal(const Map &board, const vector<pair<int, int>> &wishes) const
    {
        // PROFILE(openGapTotal);

        const int components = labelComponents(board);
        if (components == 0)
            return 0;
        floodComponentDistances(board, components);

        int gap = 0;
        for (const auto &wish : wishes)
        {
            const int d = wishGap(board, wish, components);
            if (d >= 0)
                gap += d;
        }
        return gap;
    }

    // The heuristic proper. `bankedSelf`/`bankedOther` are the points the two
    // players have actually accumulated over the `turns` simulated so far, so
    // the rails each player owns on existing shortest paths are already paid
    // for and must not be counted again here — all that is left is to steer
    // the search towards creating the connections that do not exist yet.
    //
    // The banked total is averaged over the turns that produced it. The beam
    // ranks states from different depths against each other when a depth is
    // cut short by the clock, and a raw cumulative total would make a deeper
    // state win on depth alone; a per-turn rate stays comparable.
    int evaluate(const Map &board, const vector<pair<int, int>> &wishes,
                 int bankedSelf, int bankedOther, int turns) const
    {
        // PROFILE(evaluate);

        int income = bankedSelf - bankedOther;
        if (turns > 1)
            income /= turns;

        return income - GAP_PENALTY * openGapTotal(board, wishes);
    }

    // ---- game engine turn application ----

    // Both players' rails land at the same time: a shared tile becomes
    // neutral. Disrupts are picked on the board this produces, so it is a step
    // of its own.
    static void placeTurnRails(Map &board,
                               const vector<Coord> &myRails,
                               const vector<Coord> &foeRails,
                               int selfId, int otherId)
    {
        for (const Coord &c : myRails)
            if (board.canPlaceRail(c.x, c.y))
                board.placeRail(c.x, c.y, selfId);
        for (const Coord &c : foeRails)
            if (board.canPlaceRail(c.x, c.y) || board.tileOwner(c.x, c.y) == selfId)
                board.placeRail(c.x, c.y, otherId);
    }

    // Applies both disrupts to a board whose rails are already down, then
    // settles the turn: credit what it pays both players and rescore.
    // Advancing the board and banking its income are one operation on purpose
    // — a caller that did the first without the second would silently lose
    // that turn's points.
    void simulateTurn(BeamNode &child, const BeamNode &parent,
                      const vector<pair<int, int>> &wishes,
                      int myDisrupt, int foeDisrupt,
                      int selfId, int otherId)
    {
        // PROFILE(simulateTurn);

        Map &board = child.state;

        // Disrupts raise instability and may ink (erasing the region's rails).
        if (myDisrupt != -1)
            board.disruptRegion(myDisrupt);
        if (foeDisrupt != -1)
            board.disruptRegion(foeDisrupt);

        // Last, because inking above can erase rails and a rail erased this
        // turn must not be paid for it.
        int gainSelf = 0, gainOther = 0;
        turnIncome(board, wishes, selfId, otherId, gainSelf, gainOther);

        child.bankedSelf = parent.bankedSelf + gainSelf;
        child.bankedOther = parent.bankedOther + gainOther;
        child.turns = parent.turns + 1;
        child.score = evaluate(board, wishes,
                               child.bankedSelf, child.bankedOther, child.turns);
    }

    // Runs the beam and returns the move to play this turn.
    void run(ActionSet &outAction, int &outDisrupt)
    {
        // PROFILE(beamSearch);

        outAction = ActionSet();
        outDisrupt = -1;
        stats.reset();

        // The deadline is set before anything else, because everything else
        // -- the root's own scoring included -- now tests it. Leaving it at
        // the previous turn's value would make the whole turn read as already
        // out of time.
        //
        // The beam deepens only while there is time left in the turn: on big
        // boards a full MAX_DEPTH sweep overruns the limit, so we keep the
        // best line found so far instead of forfeiting the turn. The first
        // turn gets the referee's larger allowance.
        int budgetMs = firstTurn ? FIRST_TURN_BUDGET_MS : TURN_BUDGET_MS;
        // The deadline is tested down to the individual rail placement and to
        // the individual wish, so the work still in flight when it fires is
        // small -- but the turn also has to survive scoring, ranking and
        // printing after the search returns. The target is pulled in by that
        // much so TURN_BUDGET_MS is the bound actually observed.
        deadline = turnStart + std::chrono::milliseconds(budgetMs);

        BeamNode root;
        root.state = *startBoard;
        root.active = startActive;
        // Nothing banked yet, so the root scores on its open gaps alone.
        root.score = evaluate(root.state, wishes, 0, 0, 0);

        vector<BeamNode> beam{root};

        // A depth no longer has to fit entirely in the remaining time: it is
        // always entered, and abandoned mid-way when the deadline hits. The
        // states it did produce are still usable, because evaluate() averages
        // banked income over the turns that produced it, so a partial depth's
        // children can be compared directly against the previous depth's
        // survivors without the deeper ones winning on depth alone.
        // Whether a depth completed decides how its states are used below.
        bool depthComplete = true;

        for (int depth = 0; depth < MAX_DEPTH; depth++)
        {
            vector<BeamNode> nextBeam;
            depthComplete = true;

            for (BeamNode &node : beam)
            {
                if (outOfTime())
                {
                    stats.truncatedByTime++;
                    depthComplete = false;
                    break;
                }

                // The opponent builds from the same board we do and their
                // rails land on the same turn as ours, so their turn is
                // planned once here and held fixed across our alternatives.
                // Only their best line is kept: they play one turn, they do
                // not get to pick from several after seeing ours.
                int placementStates = 0;
                vector<ActionSet> foeTurns =
                    generateActionSets(node.state, wishes, foeId,
                                       NESTED_BEAM_WIDTH, placementStates);
                vector<Coord> foeRails;
                if (!foeTurns.empty())
                    foeRails = foeTurns.front().cells;

                vector<ActionSet> myTurns =
                    generateActionSets(node.state, wishes, myId,
                                       NESTED_BEAM_WIDTH, placementStates);
                stats.actionsCreated[depth] += (int)myTurns.size();
                stats.placementStates[depth] += placementStates;

                // Nothing left to build: still play the turn, so the line
                // keeps evolving through the opponent's rails and the
                // disrupts.
                if (myTurns.empty())
                    myTurns.push_back(ActionSet());

                for (const ActionSet &action : myTurns)
                {
                    // Expanding a turn is the expensive step, so the budget
                    // is checked here too rather than once per node.
                    if (outOfTime())
                    {
                        stats.truncatedByTime++;
                        depthComplete = false;
                        break;
                    }

                    BeamNode child;
                    {
                        // PROFILE(stateCopy);
                        child.state = node.state;
                        child.active = node.active;
                    }

                    // The plan already names its cells: no replanning, so the
                    // board the search scored is the one that gets played.
                    placeTurnRails(child.state, action.cells, foeRails,
                                   myId, foeId);

                    // Picked on the board the rails just produced: a region is
                    // only worth hitting once the rails that make it valuable
                    // are on it.
                    int myDisrupt =
                        buildDisruptChoice(child.state, wishes, myId, foeId);
                    int foeDisrupt =
                        buildDisruptChoice(child.state, wishes, foeId, myId);

                    simulateTurn(child, node, wishes, myDisrupt, foeDisrupt,
                                 myId, foeId);

                    if (depth == 0)
                    {
                        child.rootAction = action;
                        child.rootDisrupt = myDisrupt;
                    }
                    else
                    {
                        child.rootAction = node.rootAction;
                        child.rootDisrupt = node.rootDisrupt;
                    }

                    nextBeam.push_back(move(child));
                }

                if (!depthComplete)
                    break;
            }

            // Every child built at this depth counts as a state encountered,
            // even the ones the Bwidth cut discards below.
            stats.statesPerDepth[depth] = (int)nextBeam.size();
            stats.totalStates += (int)nextBeam.size();

            if (nextBeam.empty())
                break;

            // This depth produced states, so it counts as expanded.
            stats.maxDepth = depth + 1;

            // 8. Keep the Bwidth best states.
            //
            // A partial depth only expanded some of the parents, so its
            // children are not a complete replacement for the previous beam:
            // dropping the unexpanded parents would throw away lines that
            // might still be the best available. The two sets are merged
            // instead and ranked together, which is sound because evaluate()
            // returns a per-turn rate rather than a depth-dependent total.
            if (!depthComplete)
            {
                for (BeamNode &node : beam)
                    nextBeam.push_back(move(node));
            }

            {
                // PROFILE(sortBeam);
                sort(nextBeam.begin(), nextBeam.end(),
                     [](const BeamNode &a, const BeamNode &b)
                     { return a.score > b.score; });
            }
            if ((int)nextBeam.size() > BEAM_WIDTH)
                nextBeam.resize(BEAM_WIDTH);

            beam = move(nextBeam);

            // The depth ran out of time: its results are already merged in,
            // and there is nothing left in the budget for another one.
            if (!depthComplete)
                break;
        }

        if (!beam.empty())
        {
            const BeamNode &best = beam.front();
            outAction = best.rootAction;
            outDisrupt = best.rootDisrupt;
        }
    }
};

// ====================
// GAME

class Game
{
public:
    int myId;
    int foeId;
    Map gameMap;

    // Shared by every simulated Map, so paths are computed once per terrain
    // state instead of once per beam node.
    PathTable pathTable;

    int myScore, foeScore;

    // wishes: pairs of town ids that want to be connected
    vector<pair<int, int>> wishes;
    // activeConnections: pairs of town ids being connected
    map<pair<int, int>, bool> activeConnections;

    // The search, reused across turns so its buffers survive.
    BeamSearch beam;

    // Start of the current turn, used to bound the search.
    std::chrono::steady_clock::time_point turnStart;
    // The first turn has a far larger time allowance than the others.
    bool firstTurn = true;

    void init()
    {
        cin >> myId;
        foeId = 1 - myId;
        gameMap.readTerrain(cin);
        pathTable.init(gameMap.width(), gameMap.height());
        gameMap.setPathTable(&pathTable);
        activeConnections.clear();
        gameMap.readTowns(cin, wishes);
    }

    void parse()
    {
        cin >> myScore;
        cin >> foeScore;
        activeConnections.clear();
        gameMap.readTurnState(cin, activeConnections);

        turnStart = std::chrono::steady_clock::now();
    }

    void gameTurn()
    {
        pathTable.resetStats();

        ActionSet action;
        int disrupt = -1;

        beam.setup(myId, foeId, gameMap, activeConnections, wishes,
                   turnStart, firstTurn);
        beam.run(action, disrupt);

        vector<string> actions;

        // The engine resolves every PLACE_TRACKS before any DISRUPT, so the
        // commands are emitted in that same order.
        // The search already decided the exact cells, so they are emitted
        // verbatim rather than replanned from a pair of endpoints.
        for (const Coord &c : action.cells)
            actions.push_back("PLACE_TRACKS " + to_string(c.x) + " " + to_string(c.y));
        int placed = (int)action.cells.size();

        if (disrupt != -1)
            actions.push_back("DISRUPT " + to_string(disrupt));

        if (!actions.empty())
        {
            stringstream msg;
            msg << "MESSAGE " << placed << " rails";
            if (disrupt != -1)
                msg << " D" << disrupt;
            actions.push_back(msg.str());
        }

        if (!actions.empty())
        {
            for (int i = 0; i < (int)actions.size(); i++)
            {
                if (i)
                    cout << ";";
                cout << actions[i];
            }
            cout << endl;
        }
        else
        {
            cout << "WAIT" << endl;
        }

        firstTurn = false;
    }
};

void mainLoopturn(Game &game)
{
    game.parse();
    game.gameTurn();
}

int main()
{
    Game game;
    game.init();
    while (true)
    {
        mainLoopturn(game);
        PROFILE_END_TURN();

        game.beam.stats.print();
        fprintf(stderr, "%-32s path LT : %d hits / %d misses (%d dead), "
                        "%d invalidated, %d cached\n",
                "pathTable", game.pathTable.hits, game.pathTable.misses,
                game.pathTable.deadEncountered, game.pathTable.invalidations,
                (int)game.pathTable.size());

        // PRINT_PROFILE(beamSearch);
        // PRINT_PROFILE(placementCandidates);
        // PRINT_PROFILE(generateActionSets);
        // PRINT_PROFILE(candidateCreation);
        // PRINT_PROFILE(buildDisruptChoice);
        // PRINT_PROFILE(simulateTurn);
        // PRINT_PROFILE(evaluate);
        // PRINT_PROFILE(connectionPathProfile);
        // PRINT_PROFILE(railGroupOf);
        // PRINT_PROFILE(stateCopy);
        // PRINT_PROFILE(openGapTotal);
        // PRINT_PROFILE(sortRegionIds);
        // PRINT_PROFILE(sortRoundLines);
        // PRINT_PROFILE(sortFinishedLines);
        // PRINT_PROFILE(sortBeam);
    }
}
