// v2.8

// Two nested beam searches. The outer one plans turns ahead; for each of its
// nodes the inner one (generateActionSets) decides that turn's rails one cell
// at a time. Both are interruptible: when the 30 ms budget runs out, the best
// line found so far is played.
//
// A state is scored by evaluate(): income per turn, minus the gap still to be
// closed on the unbuilt wishes, which is what steers the search towards
// connections that do not exist yet.
//
// The opponent is simulated on the same board and their rails land with ours.
// Only their best line is kept, and past depth 0 it is planned narrow -- see
// nestedWidthFor().

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

// Per-function call counts and accumulated microseconds, printed each turn
// from main(). Counters are cumulative; PROFILE_END_TURN() marks a turn
// boundary so the print can also report a per-turn average.

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

// Turns elapsed, so the cumulative counters above can be shown per turn.
int profileTurnCount = 0;
#define PROFILE_END_TURN() (profileTurnCount++)

#if ENABLE_PROFILING
#define PROFILE(name) ProfileScope _ps_##name(callcount_##name, elapsed_##name)
#else
#define PROFILE(name) ((void)0)
#endif

#define PRINT_PROFILE(name)                                              \
    do                                                                   \
    {                                                                    \
        int _turns = profileTurnCount > 0 ? profileTurnCount : 1;        \
        fprintf(stderr, "%-32s per turn : %.3f ms  \t%d calls\n", #name, \
                (double)elapsed_##name / _turns / 1000,                  \
                (int)((double)callcount_##name / _turns));               \
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

static const int BEAM_WIDTH = 10;
// Width of the intra-turn beam: how many half-built turns stay alive between
// one rail and the next. Separate from BEAM_WIDTH because widening here
// multiplies the work per turn, costing the outer beam its depth.
static const int MY_NESTED_BEAM_WIDTH = 50;
static const int MY_NESTED_BEAM_WIDTH_GREEDY = 10;
static const int OPP_NESTED_BEAM_WIDTH = 20;
static const int OPP_NESTED_BEAM_WIDTH_GREEDY = 5;

// How many turns ahead the outer beam may look. Rarely reached: the clock
// usually stops the search first.
static const int MAX_DEPTH = 10;
static const int PAINT_PER_TURN = 3;

// Wall-clock budget for one turn's search, kept under the referee's 50 ms
// (1000 ms on the first) to absorb the expansion still in flight when the
// deadline fires, plus scoring and output.
#ifndef TURN_BUDGET_MS
static const int TURN_BUDGET_MS = 30;
#endif
#ifndef FIRST_TURN_BUDGET_MS
static const int FIRST_TURN_BUDGET_MS = 900;
#endif

// Owner marker for a tile carrying no rail.
static const int NO_OWNER = -1;
// Owner marker for a rail both players placed on the same turn.
static const int NEUTRAL_OWNER = 2;

// Instability a region gains per DISRUPT, and the level at which it gets
// inked (erasing every rail inside it): the statement defines inked as
// instability >= 4.
static const int DISRUPT_INSTABILITY_GAIN = 1;
static const int INK_INSTABILITY_THRESHOLD = 4;

// Which wishes are connected, one bit per index into the search's `wishes`.
// A word rather than a map, since every beam node carries one. Boards run to
// ~10 wishes; indices past 64 read as never connected.
typedef unsigned long long ActiveMask;
static const size_t ACTIVE_MASK_BITS = 64;

// Direction priority: NORTH, EAST, SOUTH, WEST. Used both for path
// tie-breaking and for choosing which neighbour a rail advances to.
static const int DIR_X[4] = {0, 1, 0, -1};
static const int DIR_Y[4] = {-1, 0, 1, 0};

// ====================
// DEBUG HOOKS
//
// Observation points for the external viewer (tools/debug_tool.cpp): it
// watches the search, never steers it. Without DEBUG_TOOL the call sites
// expand to nothing, so the competition build carries no trace of them.

#ifdef DEBUG_TOOL
class Map;
class Coord;
class ActionSet;

// Defined in tools/debug_tool.cpp.
void dbgTurnBegin(const Map &board, const vector<pair<int, int>> &wishes);
void dbgCandidate(int owner, const vector<Coord> &prefix, const Coord &cand,
                  int cost, int gap);
void dbgTurnEnd(const ActionSet &action, int disrupt);
void dbgBaseline(const vector<int> &baseline);

#define DBG_TURN_BEGIN(board, wishes) dbgTurnBegin(board, wishes)
#define DBG_CANDIDATE(owner, prefix, cand, cost, gap) \
    dbgCandidate(owner, prefix, cand, cost, gap)
#define DBG_TURN_END(action, disrupt) dbgTurnEnd(action, disrupt)
#define DBG_BASELINE(baseline) dbgBaseline(baseline)
#else
#define DBG_TURN_BEGIN(board, wishes) ((void)0)
#define DBG_CANDIDATE(owner, prefix, cand, cost, gap) ((void)0)
#define DBG_TURN_END(action, disrupt) ((void)0)
#define DBG_BASELINE(baseline) ((void)0)
#endif

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

// Packed to 4 bytes so a board fits in cache: a beam state is copied per
// child and scanned by every flood-fill. Accessors return int, so callers
// see no difference.
class Tile
{
public:
    int16_t regionId;
    uint8_t type;
    int8_t tracksOwner : 4;
    uint8_t inked : 1;
    uint8_t instability : 3;
    Tile(int r = 0, int t = 0)
        : regionId((int16_t)r), type((uint8_t)t), tracksOwner(NO_OWNER),
          inked(0), instability(0) {}
};
static_assert(sizeof(Tile) == 4, "Tile must stay 4 bytes");

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

// A region's fixed description. Ink and instability are per-board state and
// live on the Map, so a beam copy does not drag the cell lists along.
class Region
{
public:
    int id;
    vector<Coord> coords;
    bool hasTown;
    Region(int id = 0) : id(id), hasTown(false) {}
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

// Generic 4-directional A*. stepCost(x, y) is the cost of entering a cell,
// INT_MAX for impassable; returns the shortest cost src->dst, or INT_MAX if
// unreachable. Manhattan stays admissible while no step costs under 1.
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

// Same A*, but also reports the path: `outPath` gets src..dst inclusive, or
// stays empty when unreachable. Separate from the cost-only version because
// tracking parents costs an extra grid most callers do not want.
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
    // Tombstone: ink invalidated this entry; the next find() recomputes it.
    bool dead;
    PathInfo() : distance(INT_MAX), dead(false) {}
};

// Cache of cell-to-cell paths with a reverse index region -> paths crossing
// it, so inking a region drops exactly the entries that depended on it. The
// distances describe terrain, not rails, so one table serves every beam node.
class PathTable
{
private:
    unordered_map<long long, PathInfo> paths;
    // regionId -> keys of every cached path crossing it. Never holds a key
    // twice in one bucket, since PathInfo::regions is deduplicated.
    unordered_map<int, vector<long long>> pathsByRegion;

    // Regions known to be inked. A path computed after inking cannot cross
    // one, so the reverse index alone is enough to drop stale entries and
    // unrelated paths stay cached.
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
    // Reviving a tombstone re-registers it, so keys already in a bucket are
    // skipped: otherwise buckets grow without bound across repeated revivals.
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
    // Misses that hit a tombstone rather than an absent entry, splitting
    // `misses` into cold lookups and re-work forced by ink.
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

    // Distance between two cells over terrain and ink, cached with the
    // regions it crosses. Blind to rails, so one table serves every node.
    // Returns nullptr when unreachable; that case is not cached.
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
            // Present but invalidated: a miss, and re-work that ink forced.
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

    // Marks every path crossing the region stale; paths that avoid it stay
    // cached. Tombstoned rather than erased, because unregistering each key
    // from its other buckets would be a linear scan per region per path.
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
        // Every path here is dead now, and no live one can register again.
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

    // Total slots held, tombstones included.
    size_t slotCount() const { return paths.size(); }
};

// ====================
// MAP

// Board state, split in two: StaticMap holds what the referee fixes once
// (terrain regions, towns), Map what a turn can change (tiles, region ink).
// Both are the sole interface to it -- nothing outside touches the containers.

// The immutable half, held once and shared by pointer so that copying a beam
// state stays cheap.
class StaticMap
{
public:
    vector<Town> towns;
    // Regions by dense index; slotOfRegion maps id -> index, so a sparse
    // referee numbering still works.
    vector<Region> regions;
    vector<int> regionSlot;
    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // Flat per-cell town flag: a set<pair> here cost a tree walk, and the
    // path/group scans hit it millions of times per turn.
    vector<char> townCellFlag;
    // Per-slot: does this region contain a town?
    vector<char> regionHasTown;
    // Region ids, ascending, so allRegionIds() need not sort per call.
    vector<int> sortedRegionIds;

    // Slot for a region id, or -1 when the id is unknown.
    int slotOfRegion(int regionId) const
    {
        if (regionId < 0 || regionId >= (int)regionSlot.size())
            return -1;
        return regionSlot[regionId];
    }
};

class Map
{
public:
    Map() {}

    // Copied once per beam child, so it carries only what a turn changes:
    // tiles and per-region ink. The rest is shared through `stat`, and the
    // scratch buffers are left behind for each copy to rebuild lazily.
    Map(const Map &o)
        : grid(o.grid), regionInkedFlag(o.regionInkedFlag),
          regionInstability(o.regionInstability), stat(o.stat),
          pathTable(o.pathTable) {}

    Map &operator=(const Map &o)
    {
        if (this != &o)
        {
            grid = o.grid;
            regionInkedFlag = o.regionInkedFlag;
            regionInstability = o.regionInstability;
            stat = o.stat;
            pathTable = o.pathTable;
            // scratch* intentionally not copied.
        }
        return *this;
    }

    // Public so the debug viewer (tools/debug_tool.cpp) can serialise a board.
public:
    Grid grid;
    // Per-region ink state by slot: flat byte arrays rather than fields on
    // Region, so a beam copy is two small memcpys.
    vector<char> regionInkedFlag;
    vector<unsigned char> regionInstability;
    // The immutable half, shared by every simulated Map rather than owned.
    const StaticMap *stat = nullptr;

    // Shared path cache, likewise not owned, so it is filled once.
    PathTable *pathTable = nullptr;

    // Scratch for the BFS helpers, excluded from the Map's value: copying a
    // beam state must not copy these.
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

    // Slot of the region owning a cell, so hot paths never touch a hash map.
    int regionSlotAt(int x, int y) const
    {
        return stat->slotOfRegion(grid.get(x, y).regionId);
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

    // Reads the width/height + per-tile (regionId, type) block, filling the
    // shared immutable half. `target` outlives every Map built from it.
    void readTerrain(istream &in, StaticMap &target)
    {
        int w, h;
        in >> w >> h;
        grid = Grid(w, h);
        stat = &target;
        target.townCellFlag.assign(w * h, 0);
        target.regions.clear();
        target.regionSlot.clear();

        for (int y = 0; y < h; y++)
        {
            for (int x = 0; x < w; x++)
            {
                int regionId, type;
                in >> regionId >> type;
                grid.get(x, y) = Tile(regionId, type);

                if (regionId >= (int)target.regionSlot.size())
                    target.regionSlot.resize(regionId + 1, -1);
                if (target.regionSlot[regionId] < 0)
                {
                    target.regionSlot[regionId] = (int)target.regions.size();
                    target.regions.push_back(Region(regionId));
                }
                target.regions[target.regionSlot[regionId]].coords.push_back(
                    Coord(x, y));
            }
        }

        target.regionHasTown.assign(target.regions.size(), 0);
        target.sortedRegionIds.clear();
        target.sortedRegionIds.reserve(target.regions.size());
        for (const Region &r : target.regions)
            target.sortedRegionIds.push_back(r.id);
        sort(target.sortedRegionIds.begin(), target.sortedRegionIds.end());

        regionInkedFlag.assign(target.regions.size(), 0);
        regionInstability.assign(target.regions.size(), 0);
    }

    // Reads the town block, appending every wish found to outWishes.
    void readTowns(istream &in, StaticMap &target,
                   vector<pair<int, int>> &outWishes)
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
            target.towns.emplace_back(townId, Coord(townX, townY), desired);
            const int slot = regionSlotAt(townX, townY);
            if (slot >= 0)
            {
                target.regions[slot].hasTown = true;
                target.regionHasTown[slot] = 1;
            }

            target.townCoord[townId] = Coord(townX, townY);
            target.townCellFlag[townY * grid.width + townX] = 1;

            for (int other : desired)
            {
                outWishes.emplace_back(townId, other);
            }
        }
    }

    // Reads the per-turn tile block, recording active connections.
    void readTurnState(istream &in, map<pair<int, int>, bool> &outActiveConnections)
    {
        regionInkedFlag.assign(stat->regions.size(), 0);
        regionInstability.assign(stat->regions.size(), 0);

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
                // An inked region is erased, whatever the referee reports.
                tile.tracksOwner = (int8_t)(inked ? NO_OWNER : tracksOwner);
                tile.inked = inked ? 1 : 0;
                // Saturates at the field's 0..7: nothing reads past the ink
                // threshold anyway.
                tile.instability = (uint8_t)min(instability, 7);

                // Mirror onto the owning region, clamped to the byte.
                const int slot = stat->slotOfRegion(tile.regionId);
                if (slot >= 0)
                {
                    regionInstability[slot] = (unsigned char)max(
                        (int)regionInstability[slot], min(instability, 255));
                    if (inked)
                        regionInkedFlag[slot] = 1;
                }
            }
        }

        // Ink the referee reports invalidates paths, as a DISRUPT would.
        if (pathTable)
        {
            for (size_t slot = 0; slot < regionInkedFlag.size(); slot++)
            {
                if (regionInkedFlag[slot])
                    pathTable->invalidateRegion(stat->regions[slot].id);
            }
        }
    }

    // ---- tile queries ----

    int tileType(int x, int y) const { return grid.get(x, y).type; }
    int tileOwner(int x, int y) const { return grid.get(x, y).tracksOwner; }
    int tileRegion(int x, int y) const { return grid.get(x, y).regionId; }
    bool tileInked(int x, int y) const { return grid.get(x, y).inked; }

    bool isTownCell(int x, int y) const { return stat->townCellFlag[y * grid.width + x] != 0; }
    bool hasRail(int x, int y) const { return grid.get(x, y).tracksOwner != NO_OWNER; }

    // A rail needs an empty, non-town, passable tile in an un-inked region.
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

    // Both tile and region are checked: a DISRUPT can ink a region between
    // two beam depths.
    bool isInked(int x, int y) const
    {
        const Tile &tile = grid.get(x, y);
        if (tile.inked)
            return true;
        const int slot = stat->slotOfRegion(tile.regionId);
        return slot >= 0 && regionInkedFlag[slot] != 0;
    }

    int railCost(int x, int y) const { return terrainCost(grid.get(x, y).type); }

    // Places a rail, neutralising a tile both players target this turn.
    void placeRail(int x, int y, int owner)
    {
        Tile &tile = grid.get(x, y);
        if (tile.tracksOwner == NO_OWNER)
            tile.tracksOwner = (int8_t)owner;
        else if (tile.tracksOwner != owner)
            tile.tracksOwner = NEUTRAL_OWNER;
    }

    // Speculative placement: hands back the previous owner so the caller can
    // undo it exactly.
    int setRailOwner(int x, int y, int owner)
    {
        Tile &tile = grid.get(x, y);
        const int previous = tile.tracksOwner;
        tile.tracksOwner = (int8_t)owner;
        return previous;
    }

    // Traversable by a connection path: a town, or a rail outside ink.
    bool isConnectable(int x, int y) const
    {
        if (isTownCell(x, y))
            return true;
        return hasRail(x, y) && !isInked(x, y);
    }

    // ---- town queries ----

    bool hasTown(int townId) const { return stat->townCoord.count(townId) != 0; }
    Coord townCoordOf(int townId) const { return stat->townCoord.at(townId); }
    const vector<Town> &allTowns() const { return stat->towns; }

    // ---- region queries ----

    bool regionContainsTown(int regionId) const
    {
        const int slot = stat->slotOfRegion(regionId);
        return slot >= 0 && stat->regionHasTown[slot] != 0;
    }

    bool regionInked(int regionId) const
    {
        const int slot = stat->slotOfRegion(regionId);
        return slot >= 0 && regionInkedFlag[slot] != 0;
    }

    // Sorted once at parse time, so a caller iterating regions costs nothing.
    const vector<int> &allRegionIds() const { return stat->sortedRegionIds; }

    // Raises a region's instability, inking it (and erasing every rail it
    // contains) once it crosses the threshold.
    void disruptRegion(int regionId)
    {
        const int slot = stat->slotOfRegion(regionId);
        if (slot < 0 || regionInkedFlag[slot])
            return;

        regionInstability[slot] += DISRUPT_INSTABILITY_GAIN;
        if (regionInstability[slot] >= INK_INSTABILITY_THRESHOLD)
        {
            regionInkedFlag[slot] = 1;
            for (const Coord &c : stat->regions[slot].coords)
            {
                Tile &tile = grid.get(c.x, c.y);
                tile.inked = 1;
                tile.tracksOwner = NO_OWNER;
            }
            // Now impassable, so every path crossing it is stale.
            if (pathTable)
                pathTable->invalidateRegion(regionId);
        }
    }

    // ---- rail groups ----

    // Every cell reachable from a town through rails and towns: the spec's
    // "rail group connected to the town".
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

        // Shared flood-fill scratch, stamped as in connectionPathInto.
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

    // Shortest rail/town path between two towns, NESW tie-broken. Empty
    // when they are not linked.
    vector<Coord> connectionPath(Coord from, Coord to) const
    {
        vector<Coord> path;
        connectionPathInto(from, to, path);
        return path;
    }

    // Same BFS into a caller-owned buffer, using the Map's scratch so it
    // never allocates: this runs hundreds of thousands of times per turn.
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

            // NESW order, so a cell's first parent already has priority.
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

// The two cells that would join a wish's rail groups, and their distance.
// Not an action: it says where the gap is, for action sets to close.
class GroupLink
{
public:
    Coord src, dst;
    int distance;
    GroupLink(Coord s = {}, Coord d = {}, int dist = INT_MAX)
        : src(s), dst(d), distance(dist) {}
};

// One turn's building: the cells to lay rail on, in the order decided. At
// most PAINT_PER_TURN cells and often fewer, since each costs its terrain --
// a single mountain (cost 3) spends the whole turn.
class ActionSet
{
public:
    vector<Coord> cells;
    // Paint spent by `cells`, i.e. the sum of their terrain costs.
    int cost = 0;
    // How well these rails serve the two wishes they serve best, higher
    // better. A ratio, so a board with few open wishes does not outrank a
    // busy one. See extendManhattanGap.
    int closedGap = 0;

    bool empty() const { return cells.empty(); }
};

// A turn part-way through being decided, holding no board of its own: the
// planner lays this line's cells on one shared Map and undoes them after,
// which beats copying a grid per line. Only `action` outlives the planning.
class PlacementLine
{
public:
    ActionSet action;
    int paintLeft = PAINT_PER_TURN;
    // Closest this line has come to each open wish's towns, so a child folds
    // in only the cell it adds. Town-A approaches, then town-B, then the
    // running paying-path total.
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

    // Per depth: turn plans handed back and expanded; boards the intra-turn
    // beam built to find them (ours and the opponent's, where the time goes);
    // and child states produced before the Bwidth cut.
    vector<int> actionsCreated;
    vector<int> placementStates;
    vector<int> statesPerDepth;
    // Nodes whose expansion was cut short by the time budget.
    int truncatedByTime;
    // Turn plannings the budget cut short: still played, but with paint
    // left unspent.
    int placementsTruncated;

    // Microseconds run past the deadline: the breaks stop the sweeps, but
    // work already in flight still has to finish.
    long long overrunUs;

    BeamStats() { reset(); }

    void reset()
    {
        maxDepth = 0;
        totalStates = 0;
        truncatedByTime = 0;
        placementsTruncated = 0;
        overrunUs = 0;
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

    // The outer beam's branching factor. A depth expands what the previous
    // one produced, capped by the beam width; depth 0 expands the root.
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
        if (overrunUs > 0)
            fprintf(stderr, "%-32s ran %.2f ms past the deadline\n",
                    "beamStats", (double)overrunUs / 1000.0);

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
    // Bit i: wishes[i] is connected on this state's board.
    ActiveMask active;
    int score;
    // Points banked along this line, as the referee pays them out. `turns`
    // is how many turns produced them, keeping depths comparable.
    int bankedSelf, bankedOther;
    int turns;
    // The root of this line: the rails we would actually place this turn.
    ActionSet rootAction;
    int rootDisrupt;

    BeamNode()
        : active(0), score(0), bankedSelf(0), bankedOther(0), turns(0),
          rootDisrupt(-1) {}
};

// ====================
// BEAM SEARCH

// The search itself: Game hands it the position through setup(), run()
// explores forward, and the best line's root move is what Game plays.
class BeamSearch
{
public:
    // ---- position under search, set once per turn by setup() ----
    int myId = 0;
    int foeId = 0;
    // Only read, to seed the root: the search never mutates it.
    const Map *startBoard = nullptr;
    ActiveMask startActive = 0;
    // One entry per pair. The referee lists a wish from both of its towns,
    // and keeping both would double every income, gap and flood-fill.
    vector<pair<int, int>> wishes;
    // Turn clock, so the search can stop before the referee's limit.
    std::chrono::steady_clock::time_point turnStart;
    bool firstTurn = true;
    // Set by run() before anything reads it; a member because the planning
    // below is deep enough to test it from inside.
    std::chrono::steady_clock::time_point deadline;

    // Scratch for openGapTotal, reused so the hottest function in the search
    // never allocates.
    mutable vector<int> gapLabel;
    mutable vector<int> gapQueue;
    // Generation stamps, so the per-cell buffers are never re-zeroed: a value
    // counts as present only when its stamp matches the run. Re-zeroing them
    // was the bulk of openGapTotal's cost.
    mutable vector<int> gapLabelStamp;
    mutable vector<int> gapDistStamp;
    // Stamped separately: labelling and flooding are two runs per call, and
    // bumping one must not invalidate the other.
    mutable int gapStamp = 0;
    mutable int gapDistRun = 0;
    // Components that reached a cell, so meeting floods are found by walking
    // arrivals rather than scanning every component at every cell.
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
    // (score, index) pairs the beam is ranked through, and the survivors it
    // hands back. Members so the two keep their capacity across depths.
    vector<pair<int, int>> rankScratch;
    vector<BeamNode> keptScratch;
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

        wishes.clear();
        set<pair<int, int>> seenWish;
        for (const auto &wish : turnWishes)
        {
            int a = wish.first, b = wish.second;
            auto key = a < b ? make_pair(a, b) : make_pair(b, a);
            if (seenWish.insert(key).second)
                wishes.push_back(key);
        }

        // Folded onto wish indices; the referee names a connection in
        // whichever direction it found it, so both orderings are looked up.
        startActive = 0;
        for (size_t i = 0; i < wishes.size() && i < ACTIVE_MASK_BITS; i++)
        {
            const pair<int, int> &w = wishes[i];
            if (turnActive.count(w) != 0 ||
                turnActive.count({w.second, w.first}) != 0)
                startActive |= (ActiveMask)1 << i;
        }

        turnStart = start;
        firstTurn = isFirstTurn;
    }

    // ---- rail group linking ----

    // Cheapest (Manhattan) pair of cells across the rail groups of towns `a`
    // and `b`: INT_MAX when either has no group, 0 when they already share a
    // cell and are therefore linked.
    static GroupLink closestGroupLink(const Map &board, int a, int b)
    {
        GroupLink best;
        if (!board.hasTown(a) || !board.hasTown(b))
            return best;

        vector<Coord> groupA = board.railGroupOf(board.townCoordOf(a));
        vector<Coord> groupB = board.railGroupOf(board.townCoordOf(b));
        if (groupA.empty() || groupB.empty())
            return best;

        // Checked on a cell set, so a shared cell is caught even when the
        // cross product below would not make it the minimum.
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

    // A planning call's wishes resolved to coordinates once. Also fixes the
    // indexing: missing towns are dropped here, so townA[i], townB[i] and a
    // line's bestPerWish[i] all agree on what `i` means.
    class WishGeometry
    {
    public:
        vector<Coord> townA, townB;
        // Straight-line length of each open wish, and SCORE_SCALE / that.
        vector<int> baseline;
        vector<int> weight;

        // Owner of each cell on an already-paying connection, else PAY_NONE.
        static constexpr int PAY_NONE = -2;
        vector<int> payOwner;
        int width = 0;

        bool hasPayMap() const { return !payOwner.empty(); }
        int payAt(int x, int y) const { return payOwner[(size_t)y * width + x]; }
    };

    // Fixed-point unit: the wish score is a ratio that would collapse to
    // zero in integers.
    static const int SCORE_SCALE = 1 << 20;

    // Addressed by index, setup() having folded the referee's ordering away.
    static bool isActiveWish(ActiveMask active, size_t index)
    {
        return index < ACTIVE_MASK_BITS &&
               (active & ((ActiveMask)1 << index)) != 0;
    }

    static WishGeometry wishGeometry(const Map &board,
                                     const vector<pair<int, int>> &wishes,
                                     ActiveMask active)
    {
        WishGeometry geo;
        geo.width = board.width();

        vector<Coord> path;
        for (size_t wi = 0; wi < wishes.size(); wi++)
        {
            const pair<int, int> &wish = wishes[wi];
            if (!board.hasTown(wish.first) || !board.hasTown(wish.second))
                continue;
            Coord ta = board.townCoordOf(wish.first);
            Coord tb = board.townCoordOf(wish.second);

            // Already connected: this turn cannot make it any more connected,
            // but it pays every turn, so its route is recorded instead.
            if (isActiveWish(active, wi))
            {
                board.connectionPathInto(ta, tb, path);
                if (path.empty())
                    continue;
                if (geo.payOwner.empty())
                    geo.payOwner.assign((size_t)board.width() * board.height(),
                                        WishGeometry::PAY_NONE);
                for (const Coord &c : path)
                    geo.payOwner[(size_t)c.y * geo.width + c.x] =
                        board.tileOwner(c.x, c.y);
                continue;
            }

            const int d = abs(ta.x - tb.x) + abs(ta.y - tb.y);
            // Two towns on the same cell are already connected, and would
            // divide by zero below.
            if (d == 0)
                continue;
            geo.townA.push_back(ta);
            geo.townB.push_back(tb);
            geo.baseline.push_back(d);
            geo.weight.push_back(SCORE_SCALE / d);
        }
        return geo;
    }

    // What a point of per-turn income is worth against a cell of gap.
    static const int PAY_WEIGHT = SCORE_SCALE / 4;

    // Rewards joining a connection that already pays -- all this term has to
    // go on once every wish is connected. A rail beside a paying path scores
    // 1, or 2 on a foe cell: rerouting moves a point rather than adding one.
    static int payScore(const WishGeometry &geo, Coord laid, int foe)
    {
        if (!geo.hasPayMap())
            return 0;

        const int W = geo.width;
        const int H = (int)(geo.payOwner.size() / (size_t)W);

        int score = 0;
        for (int k = 0; k < 4; k++)
        {
            const int nx = laid.x + DIR_X[k], ny = laid.y + DIR_Y[k];
            if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                continue;
            const int on = geo.payAt(nx, ny);
            if (on == WishGeometry::PAY_NONE)
                continue;
            // Taking a cell off the opponent moves a point rather than just
            // adding one, so it counts double.
            score += (on == foe) ? 2 : 1;
        }
        return score * PAY_WEIGHT;
    }

    // Ranks a line by its two best open wishes, each scored
    // SCORE_SCALE / (walk * baseline) -- straightness, over baseline^2 to
    // favour the short wishes a turn can finish. Higher is better.
    static int extendManhattanGap(const WishGeometry &geo, Coord laid,
                                  int foe, vector<int> &bestPerWish)
    {
        const size_t n = geo.baseline.size();

        // Accumulated, not recomputed from the newest cell: scoring only the
        // last rail would make a three-rail line look no better than a
        // one-rail one and leave paint unspent.
        int &payTotal = bestPerWish[2 * n];
        payTotal += payScore(geo, laid, foe);

        int banked = payTotal;
        int best = 0, second = 0;
        for (size_t i = 0; i < n; i++)
        {
            int da = abs(laid.x - geo.townA[i].x) + abs(laid.y - geo.townA[i].y);
            int db = abs(laid.x - geo.townB[i].x) + abs(laid.y - geo.townB[i].y);

            // Each town's own nearest rail, tracked apart so a line extending
            // towards one of them keeps improving. Sharing one rail saturates.
            int &nearA = bestPerWish[i];
            int &nearB = bestPerWish[n + i];
            nearA = min(nearA, da);
            nearB = min(nearB, db);

            const int walk = nearA + nearB;

            // Bridged, so at its ceiling: bank it and free the slot, or it
            // would flatten the turn's later rails.
            if (walk <= 2)
            {
                banked += geo.weight[i];
                continue;
            }

            const int score = geo.weight[i] / walk;
            if (score > best)
            {
                second = best;
                best = score;
            }
            else if (score > second)
                second = score;
        }
        // With no open wish, payScore is the only term left.
        return banked + best + second;
    }

    static bool betterPlacement(const ActionSet &a, const ActionSet &b)
    {
        if (a.closedGap != b.closedGap)
            return a.closedGap > b.closedGap;
        return a.cost > b.cost;
    }

    // Every affordable cell touching the network; the beam scores them all.
    // Fills `cells` rather than returning it, to avoid a malloc per round.
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

    // One turn's rails, decided one at a time: each round extends every line
    // by a cell and keeps the `keep` best, so a second rail is chosen knowing
    // the first. Interruptible, best first; `statesSeen` counts states.
    vector<ActionSet> generateActionSets(const Map &startBoard,
                                         const vector<pair<int, int>> &wishes,
                                         ActiveMask active,
                                         int owner, int keep, int &statesSeen)
    {
        // PROFILE(generateActionSets);

        vector<ActionSet> finished;
        bool aborted = false;

        // Whoever is not planning this turn. The planner runs for both
        // players, so it cannot just read myId/foeId.
        const int rival = (owner == myId) ? foeId : myId;

        // Resolved once so scoring never goes back to the town map.
        const WishGeometry geo = wishGeometry(startBoard, wishes, active);

        DBG_BASELINE(geo.baseline);

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
        // Nothing approached yet; the trailing pay slot starts at zero.
        lines[0].bestPerWish.assign(geo.baseline.size() * 2 + 1, INT_MAX);
        lines[0].bestPerWish.back() = 0;

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
                    child.action.closedGap =
                        extendManhattanGap(geo, c, rival, child.bestPerWish);

                    DBG_CANDIDATE(owner, line.action.cells, c, cost,
                                  child.action.closedGap);

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

            // Reduce to the beam width, ordering an index permutation so a
            // swap moves an int rather than a line's vectors.
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

    // Best region to disrupt, or -1: un-inked, town-free, and where the
    // opponent owns strictly more connection rails than we do.
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

    // Weight of one cell of remaining gap: worth less than a point of income,
    // which is banked now rather than on completion.
    static const int GAP_PENALTY = 1;

    // Points each player earns this turn: an active connection pays 1 per
    // rail they own along its path. `outActive` falls out of the same walk,
    // since a wish pays exactly when it is connected.
    void turnIncome(Map &board, const vector<pair<int, int>> &wishes,
                    int selfId, int otherId, int &outSelf, int &outOther,
                    ActiveMask &outActive) const
    {
        outSelf = 0;
        outOther = 0;
        outActive = 0;

        for (size_t wi = 0; wi < wishes.size(); wi++)
        {
            int a = wishes[wi].first, b = wishes[wi].second;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;
            vector<Coord> path = board.connectionPath(board.townCoordOf(a), board.townCoordOf(b));
            if (path.empty())
                continue;

            if (wi < ACTIVE_MASK_BITS)
                outActive |= (ActiveMask)1 << wi;

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
                    recordPair(label, o, components, gapDist[slot] + gapDist[(size_t)o * N + nIdx]);

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

    // How far every unconnected wish is from paying out, summed. The only
    // forward-looking term: income cannot see a connection that does not
    // exist yet, so without this the search has no gradient towards one.
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

    // The heuristic proper. Banked points are already paid for, so all that
    // is left is steering towards connections that do not exist yet.
    //
    // Averaged over the turns that produced it: the beam ranks states from
    // different depths together, and a cumulative total would let a deeper
    // one win on depth alone.
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

    // Both players' rails land together, so a shared tile goes neutral.
    // A step of its own, since disrupts are picked on the board it produces.
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

    // Applies both disrupts to a board whose rails are down, then settles the
    // turn. Advancing and banking are one operation on purpose: doing the
    // first alone would silently lose that turn's points.
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
        turnIncome(board, wishes, selfId, otherId, gainSelf, gainOther,
                   child.active);

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

        DBG_TURN_BEGIN(*startBoard, wishes);

        // Set first: the root's own scoring already tests it.
        int budgetMs = firstTurn ? FIRST_TURN_BUDGET_MS : TURN_BUDGET_MS;
        deadline = turnStart + std::chrono::milliseconds(budgetMs);

        BeamNode root;
        root.state = *startBoard;
        root.active = startActive;
        // Nothing banked yet, so the root scores on its open gaps alone.
        root.score = evaluate(root.state, wishes, 0, 0, 0);

        vector<BeamNode> beam{root};

        // A depth is always entered and abandoned mid-way when the deadline
        // hits. Its states stay usable because evaluate() averages income per
        // turn, so a partial depth cannot win on depth alone.
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

                // The opponent plans from the same board, so their turn is
                // planned once and held fixed across our alternatives -- they
                // play one turn, not one per alternative of ours.
                int placementStates = 0;
                int foeWidth = depth == 0 ? OPP_NESTED_BEAM_WIDTH : OPP_NESTED_BEAM_WIDTH_GREEDY;
                vector<ActionSet> foeTurns =
                    generateActionSets(node.state, wishes, node.active, foeId,
                                       foeWidth,
                                       placementStates);
                vector<Coord> foeRails;
                if (!foeTurns.empty())
                    foeRails = foeTurns.front().cells;

                int myWidth = depth == 0 ? MY_NESTED_BEAM_WIDTH : MY_NESTED_BEAM_WIDTH_GREEDY;
                vector<ActionSet> myTurns =
                    generateActionSets(node.state, wishes, node.active, myId,
                                       myWidth, placementStates);
                stats.actionsCreated[depth] += (int)myTurns.size();
                stats.placementStates[depth] += placementStates;

                // Nothing to build: still play the turn so the line evolves
                // through the opponent's rails and the disrupts. It prints as
                // WAIT, which run() avoids if any playable line exists.
                if (myTurns.empty())
                    myTurns.push_back(ActionSet());

                for (const ActionSet &action : myTurns)
                {
                    // The expensive step, so the budget is checked per turn.
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
                        // active is not copied: simulateTurn rebuilds it.
                    }

                    // No replanning: the board scored is the one played.
                    placeTurnRails(child.state, action.cells, foeRails,
                                   myId, foeId);

                    // Picked on the board the rails just produced: a region
                    // is only worth hitting once those rails are on it.
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

            // Counts every child built, including those the cut discards.
            stats.statesPerDepth[depth] = (int)nextBeam.size();
            stats.totalStates += (int)nextBeam.size();

            if (nextBeam.empty())
                break;

            // This depth produced states, so it counts as expanded.
            stats.maxDepth = depth + 1;

            // A partial depth's children do not replace the previous beam,
            // so the two are merged. Never at depth 0: the root's do-nothing
            // candidate would win whenever the opponent out-earns us.
            if (!depthComplete && depth > 0)
            {
                for (BeamNode &node : beam)
                    nextBeam.push_back(move(node));
            }

            {
                // PROFILE(sortBeam);
                // Ranked through an index rather than by moving nodes: a
                // BeamNode carries a Map, so a swap moves many pointers where
                // a (score, index) pair is 8 bytes.
                //
                // The index is the tie-break, so equal scores keep generation
                // order: the move is picked out of this ranking below.
                rankScratch.clear();
                rankScratch.reserve(nextBeam.size());
                for (int i = 0; i < (int)nextBeam.size(); i++)
                    rankScratch.push_back({nextBeam[i].score, i});

                const int keep = min((int)rankScratch.size(), BEAM_WIDTH);
                auto better = [](const pair<int, int> &a,
                                 const pair<int, int> &b)
                {
                    if (a.first != b.first)
                        return a.first > b.first;
                    return a.second < b.second;
                };
                // Only the survivors are ordered. partial_sort beats
                // nth_element + sort at the hundred-odd entries seen here.
                partial_sort(rankScratch.begin(), rankScratch.begin() + keep,
                             rankScratch.end(), better);

                keptScratch.clear();
                keptScratch.reserve(keep);
                for (int i = 0; i < keep; i++)
                    keptScratch.push_back(move(nextBeam[rankScratch[i].second]));
                beam.swap(keptScratch);
            }

            // Out of time, and this depth's results are already merged in.
            if (!depthComplete)
                break;
        }

        if (!beam.empty())
        {
            // The front is the best line, but it may be the empty-turn
            // placeholder, which prints as WAIT. Prefer the best line that
            // plays something; fall back to the front only when none does.
            const BeamNode *best = &beam.front();
            if (best->rootAction.empty() && best->rootDisrupt == -1)
            {
                for (const BeamNode &node : beam)
                {
                    if (!node.rootAction.empty() || node.rootDisrupt != -1)
                    {
                        best = &node;
                        break;
                    }
                }
            }

            outAction = best->rootAction;
            outDisrupt = best->rootDisrupt;
        }

        // How far past the deadline the in-flight work ran; zero when the
        // search finished inside the budget.
        {
            const auto now = std::chrono::steady_clock::now();
            if (now > deadline)
                stats.overrunUs =
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        now - deadline)
                        .count();
        }

        DBG_TURN_END(outAction, outDisrupt);
    }
};

// ====================
// GAME

class Game
{
public:
    int myId;
    int foeId;
    // The immutable half, owned here so it outlives every Map pointing at it.
    StaticMap staticMap;
    Map gameMap;

    // Shared by every simulated Map, so paths are computed once.
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
        gameMap.readTerrain(cin, staticMap);
        pathTable.init(gameMap.width(), gameMap.height());
        gameMap.setPathTable(&pathTable);
        activeConnections.clear();
        gameMap.readTowns(cin, staticMap, wishes);
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

// The debug tool includes this file to drive the very same engine, and brings
// its own entry point.
#ifndef DEBUG_TOOL
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
#endif // !DEBUG_TOOL
