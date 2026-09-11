// v2.0
// - Beam search over simulated future states
// - Rail placement choices: shortest link between the two rail groups
//     already connected to each town of an unbuilt desired connection
// - Disrupt choice: best region where the opponent owns more connection
//     rails than we do
// - Greedy 3-paint-point rail application, A*-guided, NORTH/EAST/SOUTH/WEST
//     tie-breaking

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
// from main() alongside snapshotCommittedChild stats.

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

#if ENABLE_PROFILING
#define PROFILE(name) ProfileScope _ps_##name(callcount_##name, elapsed_##name)
#else
#define PROFILE(name) ((void)0)
#endif

#define PRINT_PROFILE(name)                                                                                                                                                                             \
    do                                                                                                                                                                                                  \
    {                                                                                                                                                                                                   \
        if (callcount_##name != 0)                                                                                                                                                                      \
            fprintf(stderr, "%-32s avg time : %f ms  \ttotals : %d ms  \t%d calls\n", #name, (double)elapsed_##name / callcount_##name / 1000, (int)((double)elapsed_##name / 1000), callcount_##name); \
    } while (0)

// Profile declarations
DECLARE_PROFILE(beamSearch)
DECLARE_PROFILE(placementCandidates)
DECLARE_PROFILE(planTurnPlacements)
DECLARE_PROFILE(disruptChoice)
DECLARE_PROFILE(simulateTurn)
DECLARE_PROFILE(evaluate)
DECLARE_PROFILE(connectionPath)
DECLARE_PROFILE(railGroupOf)
DECLARE_PROFILE(stateCopy)
DECLARE_PROFILE(openGapTotal)

// ====================
// CONSTANTS

static const int BEAM_WIDTH = 10;
// Width of the intra-turn beam, i.e. how many half-built turns stay alive
// between one rail and the next. It is a separate knob from BEAM_WIDTH
// because the two buy different things and cost very differently: widening
// here multiplies the work spent on a single turn, which comes straight out
// of the depth the outer beam can reach.
#ifndef PLACEMENT_BEAM_WIDTH
#define PLACEMENT_BEAM_WIDTH BEAM_WIDTH
#endif
// Upper bound on how many turn plans one state expands into, i.e. how many
// of the intra-turn beam's survivors the outer beam actually simulates. It
// exists for responsiveness, not for pruning quality: the deadline is only
// tested between expansions, so a node that expands everything overshoots the
// budget by a whole depth's worth of work. Set to 0 to disable, in which case
// the intra-turn beam width (BEAM_WIDTH) is the only bound.
static const int MAX_BRANCHING = 20;
static const int MAX_DEPTH = 10;
// How many of the scanned candidate cells are actually scored each round.
// The scan itself stays deliberately unselective — it returns every legal
// cell touching the network, a few hundred on a dense board — but scoring one
// costs a board copy plus a full rescore, so scoring all of them is far
// beyond a turn's budget. They are ranked by a cheap distance test first and
// only the best survive. Set to 0 to score every candidate.
static const int MAX_CANDIDATES = 20;
static const int PAINT_PER_TURN = 3;

// Wall-clock budget for one turn's search. The referee allows 50 ms per turn
// (1000 ms on the first). The deadline is only tested between expansions, so
// the budget stays well under the limit to absorb one in-flight expansion
// plus the final replay and output.
static const int TURN_BUDGET_MS = 30;
static const int FIRST_TURN_BUDGET_MS = 30;

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
    vector<Connection> partOfActiveConnections;
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

// ====================
// PATH LOOKUP TABLE

// One cached path between two cells: its A* cost and the set of regions it
// crosses. The region list is what makes targeted invalidation possible —
// when a region is inked, only the paths that ran through it are wrong.
class PathInfo
{
public:
    int distance;
    // Regions the path crosses, sorted and deduplicated.
    vector<int> regions;
    PathInfo() : distance(INT_MAX) {}
};

// Cache of cell-to-cell paths, plus the reverse index region -> paths that
// cross it. Both are filled at the same time, so inking a region can drop
// exactly the entries that depended on it instead of clearing everything.
//
// The cached distances describe the terrain (cost and ink), not the rails
// laid during the search, so a single table stays valid for every beam node.
class PathTable
{
public:
    // Key for a cell pair. Paths are symmetric, so the two endpoints are
    // stored in a canonical order and each pair is cached once.
    typedef pair<int, int> CellPair; // (from index, to index)

    // A whole distance field from one destination cell to every other cell,
    // over terrain and ink only (rails excluded, so it survives every beam
    // node). Cached because the rail walk queries it four times per step and
    // it is otherwise recomputed for every choice at every node.
    class DistanceField
    {
    public:
        vector<int> dist; // indexed by cellIndex
        vector<int> regions;
        // Ink generation this field was built against; stale if older than
        // the table's current generation.
        int generation = 0;
        DistanceField() {}
    };

private:
    unordered_map<long long, PathInfo> paths;
    // regionId -> keys of every cached path crossing that region.
    unordered_map<int, vector<long long>> pathsByRegion;

    // Distance fields keyed by destination cell, with the same reverse index
    // so inking a region drops the fields that crossed it.
    unordered_map<int, DistanceField> fields;
    unordered_map<int, vector<int>> fieldsByRegion;

    // Regions known to be inked, and the ink generation. The generation is
    // bumped whenever a new region is inked, so any entry cached earlier is
    // recognised as stale even if the reverse index no longer lists it.
    set<int> inkedRegions;
    int generation = 0;

    int width, height;

    long long makeKey(int fromIdx, int toIdx) const
    {
        // Canonical order: the table is symmetric.
        if (fromIdx > toIdx)
            swap(fromIdx, toIdx);
        return (long long)fromIdx * (long long)(width * height) + toIdx;
    }

public:
    // Statistics, printed with the other per-turn beam numbers.
    int hits = 0, misses = 0, invalidations = 0;
    int fieldHits = 0, fieldMisses = 0;

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
        fields.clear();
        fieldsByRegion.clear();
    }

    void resetStats()
    {
        hits = misses = invalidations = 0;
        fieldHits = fieldMisses = 0;
    }

    // ---- distance fields ----

    const DistanceField *findField(Coord dst)
    {
        auto it = fields.find(cellIndex(dst));
        if (it == fields.end())
        {
            fieldMisses++;
            return nullptr;
        }
        // Built before the latest region was inked: recompute it.
        if (it->second.generation != generation)
        {
            fields.erase(it);
            invalidations++;
            fieldMisses++;
            return nullptr;
        }
        fieldHits++;
        return &it->second;
    }

    // Caches a field and indexes it under every region it reaches.
    //
    // Staleness is tracked with a generation counter rather than by refusing
    // fields that touch inked regions: a field built now already accounts for
    // the ink known now, and only entries created before the latest ink event
    // are wrong. invalidateRegion() bumps the generation and drops those.
    const DistanceField *insertField(Coord dst, DistanceField field)
    {
        int key = cellIndex(dst);
        field.generation = generation;
        for (int r : field.regions)
            fieldsByRegion[r].push_back(key);
        auto res = fields.insert_or_assign(key, move(field));
        return &res.first->second;
    }

    int cellIndex(Coord c) const { return c.y * width + c.x; }

    // Returns the cached entry for a pair, or nullptr on a miss.
    const PathInfo *find(Coord a, Coord b)
    {
        auto it = paths.find(makeKey(cellIndex(a), cellIndex(b)));
        if (it == paths.end())
        {
            misses++;
            return nullptr;
        }
        hits++;
        return &it->second;
    }

    // Stores a computed path and indexes it under every region it crosses.
    void insert(Coord a, Coord b, PathInfo info)
    {
        long long key = makeKey(cellIndex(a), cellIndex(b));
        for (int r : info.regions)
            pathsByRegion[r].push_back(key);
        paths[key] = move(info);
    }

    // Drops every path and field that crossed the region, so the next lookup
    // recomputes it against the new (inked) terrain.
    //
    // The region is also remembered as inked: the reverse index is consumed
    // here, so without that flag a field built later and registered under the
    // same region would never be dropped again.
    void invalidateRegion(int regionId)
    {
        // Only a region that was not already inked changes the terrain, and
        // only then does everything cached earlier become stale.
        if (!inkedRegions.insert(regionId).second)
            return;
        generation++;

        auto it = pathsByRegion.find(regionId);
        if (it != pathsByRegion.end())
        {
            for (long long key : it->second)
            {
                if (paths.erase(key))
                    invalidations++;
            }
            pathsByRegion.erase(it);
        }

        auto fit = fieldsByRegion.find(regionId);
        if (fit != fieldsByRegion.end())
        {
            for (int key : fit->second)
            {
                if (fields.erase(key))
                    invalidations++;
            }
            fieldsByRegion.erase(fit);
        }
    }

    bool isRegionInked(int regionId) const
    {
        return inkedRegions.count(regionId) != 0;
    }

    size_t size() const { return paths.size(); }
    size_t fieldCount() const { return fields.size(); }
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
                vector<Connection> connections;
                if (partStr != "x")
                {
                    stringstream ss(partStr);
                    string conn;
                    while (getline(ss, conn, ','))
                    {
                        int fromTownId, toTownId;
                        sscanf(conn.c_str(), "%d-%d", &fromTownId, &toTownId);
                        connections.emplace_back(fromTownId, toTownId);
                        outActiveConnections[{fromTownId, toTownId}] = true;
                    }
                }
                Tile &tile = grid.get(x, y);
                // An inked region has been erased: whatever the referee
                // reports, it holds no usable rail any more.
                tile.tracksOwner = inked ? NO_OWNER : tracksOwner;
                tile.inked = inked;
                tile.instability = instability;
                tile.partOfActiveConnections = connections;

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
        sort(ids.begin(), ids.end());
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
        PROFILE(railGroupOf);
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
        PROFILE(connectionPath);
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
class PlacementLine
{
public:
    Map board;
    ActionSet action;
    int paintLeft = PAINT_PER_TURN;
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
    mutable vector<vector<int>> gapCells;

    // Set while the search is producing the first legal move of the turn.
    // Everything below tests the clock, and on a big enough board the budget
    // can expire before even one candidate exists — which would make us play
    // WAIT and forfeit the turn's paint. Overrunning slightly beats that, so
    // the clock is ignored until there is something to play.
    bool mustFinish = false;

    bool outOfTime() const
    {
        if (mustFinish)
            return false;
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

    // Ranks two turn plans: the one that leaves the least gap wins, and among
    // plans that leave the same gap, the one that spent more paint — unspent
    // paint is simply lost at the end of the turn.
    static bool betterPlacement(const ActionSet &a, const ActionSet &b)
    {
        if (a.resultingGap != b.resultingGap)
            return a.resultingGap < b.resultingGap;
        return a.cost > b.cost;
    }

    // Every cell a rail could legally go on this turn that actually touches
    // the existing network: the free neighbours of every rail and every town.
    //
    // No attempt is made to work out which of them helps. That used to be the
    // job here — the list was the free neighbours of the two cells bringing
    // one wish's groups closest — but it meant re-deriving both rail groups of
    // every open wish on every intermediate state, two flood fills per wish,
    // which is where the turn's time went. The nested beam already scores
    // every candidate on the board it produces and keeps only the best, so a
    // useless cell is discarded by the search a moment later. Generating it is
    // far cheaper than reasoning about it.
    //
    // `paintLeft` drops terrain the turn can no longer afford, which is also
    // what ends a turn: with no paint left there is no candidate.
    static vector<Coord> placementCandidates(const Map &board,
                                             const vector<pair<int, int>> &wishes,
                                             int paintLeft)
    {
        PROFILE(placementCandidates);

        vector<Coord> cells;
        if (paintLeft <= 0)
            return cells;

        const int W = board.width(), H = board.height();

        // Ranked by a cheap town-to-town test, never by anything that has to
        // walk the rail network: lower is better.
        vector<pair<int, Coord>> ranked;

        for (int y = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++)
            {
                // A cell is a candidate on its own merits, so it is tested
                // once and never through a neighbour: no dedup needed.
                if (!board.canPlaceRail(x, y))
                    continue;
                if (board.railCost(x, y) > paintLeft)
                    continue;

                // Touching the network is the only thing asked of it. A rail
                // floating in open ground joins nothing and can never pay.
                bool touches = false;
                for (int k = 0; k < 4 && !touches; k++)
                {
                    int nx = x + DIR_X[k], ny = y + DIR_Y[k];
                    if (board.inBounds(nx, ny) && board.isConnectable(nx, ny))
                        touches = true;
                }
                if (!touches)
                    continue;

                // How near this cell sits to the straight line between some
                // pair of towns that still want each other. Purely arithmetic
                // on town coordinates — no groups, no flood fill — so it stays
                // as cheap as the scan itself. It decides nothing: a cell that
                // ranks badly is merely scored later, and the beam still has
                // the final say on every cell it does score.
                int bestDetour = INT_MAX;
                for (const auto &wish : wishes)
                {
                    if (!board.hasTown(wish.first) || !board.hasTown(wish.second))
                        continue;
                    Coord ta = board.townCoordOf(wish.first);
                    Coord tb = board.townCoordOf(wish.second);
                    int da = abs(x - ta.x) + abs(y - ta.y);
                    int db = abs(x - tb.x) + abs(y - tb.y);
                    int direct = abs(ta.x - tb.x) + abs(ta.y - tb.y);
                    bestDetour = min(bestDetour, da + db - direct);
                }
                ranked.push_back({bestDetour, Coord(x, y)});
            }
        }

        if (MAX_CANDIDATES > 0 && (int)ranked.size() > MAX_CANDIDATES)
        {
            partial_sort(ranked.begin(), ranked.begin() + MAX_CANDIDATES,
                         ranked.end(),
                         [](const pair<int, Coord> &a, const pair<int, Coord> &b)
                         { return a.first < b.first; });
            ranked.resize(MAX_CANDIDATES);
        }

        cells.reserve(ranked.size());
        for (const auto &r : ranked)
            cells.push_back(r.second);
        return cells;
    }

    // One turn's building, decided one rail at a time rather than as a route
    // planned up front. Each step lists the cells worth laying right now,
    // extends every surviving line with each of them, keeps the `keep` best,
    // and lists again from the boards those rails produced — so a line's
    // second rail is chosen knowing where its first one went, and a rail that
    // opens up a better continuation is free to win on that.
    //
    // A line finishes when its remaining paint can buy nothing; the finished
    // lines come back best first. Nothing here touches the caller's board,
    // because both players' rails have to land on the same turn.
    //
    // The turn's clock is respected down to the individual rail: this is the
    // single most expensive thing the search does, so letting it run to
    // completion once entered is what used to push the turn past its budget.
    // Cutting it short is safe because a line that has laid one rail is
    // already a legal turn — just one that leaves paint unspent — so an
    // aborted run still hands back something playable.
    //
    // `statesSeen` accumulates the intermediate states built, for stats.
    vector<ActionSet> planTurnPlacements(const Map &board,
                                         const vector<pair<int, int>> &wishes,
                                         int owner, int keep, int &statesSeen,
                                         bool needMove = false)
    {
        PROFILE(planTurnPlacements);

        vector<ActionSet> finished;
        bool aborted = false;

        vector<PlacementLine> lines(1);
        {
            PROFILE(stateCopy);
            lines[0].board = board;
        }
        lines[0].paintLeft = PAINT_PER_TURN;

        // Until one rail is down, the turn has nothing to play, so the
        // search is allowed to overrun to get that far. The guarantee is
        // deliberately as small as it can be — one line extended by one
        // rail, not a whole round — because everything it covers is time
        // spent past the budget. Later rails, the other lines and the
        // opponent's planning are all optional, and stay bounded.
        bool guarantee = needMove;

        while (!lines.empty())
        {
            // Armed before any clock test, so the guarantee cannot be
            // skipped past by the very checks it exists to override.
            mustFinish = guarantee;

            // A whole extra rail round is about to start. Everything the
            // earlier rounds finished is already in `finished`.
            if (outOfTime())
            {
                aborted = true;
                break;
            }

            vector<PlacementLine> grown;

            for (PlacementLine &line : lines)
            {
                // Only the line still under the guarantee ignores the clock,
                // and only until it has produced a rail.
                mustFinish = guarantee;

                // Listing a line's candidates re-derives both groups of every
                // open wish, so it is worth a check of its own.
                if (outOfTime())
                {
                    aborted = true;
                    break;
                }

                vector<Coord> candidates =
                    placementCandidates(line.board, wishes, line.paintLeft);

                // Out of paint, or paint left with nowhere useful to spend it:
                // either way this line's turn is over.
                if (candidates.empty())
                {
                    if (!line.action.empty())
                    {
                        finished.push_back(move(line.action));
                        // Moved from, so the abort harvest below skips it.
                        line.action.cells.clear();
                    }
                    // Still nothing to play, so the guarantee stays armed for
                    // whichever line does manage to produce a rail.
                    continue;
                }

                for (const Coord &c : candidates)
                {
                    // One candidate is a board copy plus a full rescore, the
                    // grain the whole planner is built out of: checking here
                    // is what bounds the overrun.
                    if (outOfTime())
                    {
                        aborted = true;
                        break;
                    }

                    PlacementLine child;
                    {
                        PROFILE(stateCopy);
                        child.board = line.board;
                    }
                    child.action = line.action;

                    int cost = line.board.railCost(c.x, c.y);
                    child.paintLeft = line.paintLeft - cost;
                    child.board.placeRail(c.x, c.y, owner);
                    child.action.cells.push_back(c);
                    child.action.cost += cost;
                    child.action.resultingGap = openGapTotal(child.board, wishes);

                    grown.push_back(move(child));

                    // A rail is down: there is a move to play, so the clock
                    // is back in charge for everything that follows.
                    guarantee = false;
                    mustFinish = false;
                }

                mustFinish = false;

                if (aborted)
                    break;
            }

            statesSeen += (int)grown.size();

            if (aborted)
            {
                // Time ran out part-way through a round. Every line still
                // alive is a legal turn already, and so is every child the
                // round did manage to build, so both are kept instead of
                // being thrown away; the final ranking below sorts them out.
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
            sort(grown.begin(), grown.end(),
                 [](const PlacementLine &a, const PlacementLine &b)
                 { return betterPlacement(a.action, b.action); });
            if ((int)grown.size() > keep)
                grown.resize(keep);

            lines = move(grown);
        }

        // Lines finish at different steps — a mountain ends a turn in one
        // rail, plains take three — so the survivors are ranked together
        // here rather than at whichever step produced them.
        sort(finished.begin(), finished.end(), betterPlacement);
        if ((int)finished.size() > keep)
            finished.resize(keep);

        mustFinish = false;

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
        PROFILE(disruptChoice);

        // Collect the cells of every currently active connection once.
        // Interruptible: a partial set just means fewer candidate regions.
        vector<vector<Coord>> connectionPaths;
        for (const auto &wish : wishes)
        {
            if (outOfTime())
                break;

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
            if (outOfTime())
                break;

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
    int openGapTotal(const Map &board, const vector<pair<int, int>> &wishes) const
    {
        PROFILE(openGapTotal);

        // One pass over the board labels every rail/town component, instead of
        // flooding once per town and then comparing every cell of one group
        // against every cell of the other. That cross product was quadratic in
        // the size of the rail network and ran for every candidate cell the
        // scan proposes, which is what made scoring the bottleneck once
        // candidate generation stopped filtering.
        const int W = board.width(), H = board.height();
        const int N = W * H;

        gapLabel.assign(N, -1);
        int components = 0;

        for (int start = 0; start < N; start++)
        {
            if (gapLabel[start] != -1)
                continue;
            if (!board.isConnectable(start % W, start / W))
                continue;

            const int label = components++;
            gapQueue.clear();
            gapQueue.push_back(start);
            gapLabel[start] = label;

            for (size_t head = 0; head < gapQueue.size(); head++)
            {
                const int cur = gapQueue[head];
                const int cx = cur % W, cy = cur / W;
                for (int k = 0; k < 4; k++)
                {
                    const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                        continue;
                    const int nIdx = ny * W + nx;
                    if (gapLabel[nIdx] != -1)
                        continue;
                    if (!board.isConnectable(nx, ny))
                        continue;
                    gapLabel[nIdx] = label;
                    gapQueue.push_back(nIdx);
                }
            }
        }

        // Cells of each component, so a wish only scans the two it needs.
        gapCells.assign(components, {});
        for (int idx = 0; idx < N; idx++)
            if (gapLabel[idx] != -1)
                gapCells[gapLabel[idx]].push_back(idx);

        int gap = 0;

        for (const auto &wish : wishes)
        {
            if (outOfTime())
                break;

            int a = wish.first, b = wish.second;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;

            Coord ca = board.townCoordOf(a), cb = board.townCoordOf(b);
            const int la = gapLabel[ca.y * W + ca.x];
            const int lb = gapLabel[cb.y * W + cb.x];
            // A town with no component at all: nothing to measure.
            if (la == -1 || lb == -1)
                continue;
            // Same component: the towns already touch, so the wish is closed.
            if (la == lb)
                continue;

            int best = INT_MAX;
            for (int ia : gapCells[la])
            {
                const int ax = ia % W, ay = ia / W;
                for (int ib : gapCells[lb])
                    best = min(best, abs(ax - ib % W) + abs(ay - ib / W));
            }
            if (best != INT_MAX)
                gap += best;
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
        PROFILE(evaluate);

        int income = bankedSelf - bankedOther;
        if (turns > 1)
            income /= turns;

        return income - GAP_PENALTY * openGapTotal(board, wishes);
    }

    // ---- game engine turn application ----

    // Plays one full turn on `child`, which starts as a copy of `parent`:
    // both players' rails land simultaneously, then both pick and apply a
    // disrupt, then the payout. Producing state D+1 and banking what it pays
    // are one operation on purpose — a caller that advanced the board without
    // crediting the turn would silently lose that turn's income.
    //
    // Returns the region `selfId` disrupted, or -1. The caller needs it at the
    // root, where it is half of the move actually played.
    int simulateTurn(BeamNode &child, const BeamNode &parent,
                     const vector<pair<int, int>> &wishes,
                     const vector<Coord> &myRails, const vector<Coord> &foeRails,
                     int selfId, int otherId)
    {
        PROFILE(simulateTurn);

        Map &board = child.state;

        // Both players place at the same time: a shared tile becomes neutral.
        for (const Coord &c : myRails)
            if (board.canPlaceRail(c.x, c.y))
                board.placeRail(c.x, c.y, selfId);
        for (const Coord &c : foeRails)
            if (board.canPlaceRail(c.x, c.y) || board.tileOwner(c.x, c.y) == selfId)
                board.placeRail(c.x, c.y, otherId);

        // The turn's rails are down, so the disrupts are picked on the board
        // they produced: a region only becomes worth hitting once the rails
        // that make it valuable are actually on it.
        int myDisrupt = buildDisruptChoice(board, wishes, selfId, otherId);
        int foeDisrupt = buildDisruptChoice(board, wishes, otherId, selfId);

        // Disrupts raise instability and may ink (erasing the region's rails).
        if (myDisrupt != -1)
            board.disruptRegion(myDisrupt);
        if (foeDisrupt != -1)
            board.disruptRegion(foeDisrupt);

        // The turn is over: settle it. Inherit the line's banked points,
        // credit what this now-final board pays both players, and rescore.
        // This runs last because inking above can erase rails, and a rail
        // erased this turn must not be paid for it.
        int gainSelf = 0, gainOther = 0;
        turnIncome(board, wishes, selfId, otherId, gainSelf, gainOther);

        child.bankedSelf = parent.bankedSelf + gainSelf;
        child.bankedOther = parent.bankedOther + gainOther;
        child.turns = parent.turns + 1;
        child.score = evaluate(board, wishes,
                               child.bankedSelf, child.bankedOther, child.turns);

        return myDisrupt;
    }

    // Runs the beam and returns the move to play this turn.
    void run(ActionSet &outAction, int &outDisrupt)
    {
        PROFILE(beamSearch);

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
        static const int IN_FLIGHT_MARGIN_MS = 3;
        deadline = turnStart + std::chrono::milliseconds(budgetMs) -
                   std::chrono::milliseconds(IN_FLIGHT_MARGIN_MS);

        BeamNode root;
        root.state = *startBoard;
        root.active = startActive;
        // Nothing simulated yet, so nothing banked: the root is judged on its
        // open gaps alone. Scored with the clock ignored, because a root whose
        // gaps went unmeasured would be a meaningless baseline for every child
        // that gets compared against it.
        mustFinish = true;
        root.score = evaluate(root.state, wishes, 0, 0, 0);
        mustFinish = false;

        vector<BeamNode> beam{root};

        // Whether any turn planning has run yet this turn. Until one has, the
        // search has no move to fall back on, so the first one is allowed to
        // ignore the clock.
        bool anyPlanYet = false;

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
            if (outOfTime())
                break;

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
                    planTurnPlacements(node.state, wishes, foeId,
                                       1, placementStates);
                vector<Coord> foeRails;
                if (!foeTurns.empty())
                    foeRails = foeTurns.front().cells;

                // Our own turn, kept as several alternatives: each is a
                // different way of spending this turn's paint, and the outer
                // beam picks between them on what the whole turn is worth
                // once the opponent's rails and both disrupts have landed.
                int keep = (MAX_BRANCHING > 0)
                               ? min((int)PLACEMENT_BEAM_WIDTH, MAX_BRANCHING)
                               : (int)PLACEMENT_BEAM_WIDTH;
                // The first depth-0 planning of the turn is the one whose
                // result we fall back on if everything after it is cut short,
                // so it alone may overrun rather than hand back nothing. Once
                // it has run, the rest of the beam is bounded normally.
                bool needMove = (depth == 0 && !anyPlanYet);
                vector<ActionSet> myTurns =
                    planTurnPlacements(node.state, wishes, myId,
                                       keep, placementStates, needMove);
                anyPlanYet = true;
                stats.actionsCreated[depth] += (int)myTurns.size();
                stats.placementStates[depth] += placementStates;

                // The planning above stops on the deadline by itself, but it
                // can stop having produced next to nothing: bail out here
                // rather than expanding a turn it never got to think about.
                if (outOfTime())
                {
                    stats.truncatedByTime++;
                    depthComplete = false;
                    break;
                }

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
                        PROFILE(stateCopy);
                        child.state = node.state;
                        child.active = node.active;
                    }

                    // The plan already names its cells, in the order they were
                    // decided: no replanning, so the board the search scored
                    // is the one that gets played.
                    int myDisrupt = simulateTurn(child, node, wishes,
                                                 action.cells, foeRails,
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

            sort(nextBeam.begin(), nextBeam.end(),
                 [](const BeamNode &a, const BeamNode &b)
                 { return a.score > b.score; });
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

        game.beam.stats.print();
        fprintf(stderr, "%-32s path LT : %d hits / %d misses, fields %d/%d, "
                        "%d invalidated, %d cached\n",
                "pathTable", game.pathTable.hits, game.pathTable.misses,
                game.pathTable.fieldHits, game.pathTable.fieldMisses,
                game.pathTable.invalidations, (int)game.pathTable.fieldCount());

        PRINT_PROFILE(beamSearch);
        PRINT_PROFILE(placementCandidates);
        PRINT_PROFILE(planTurnPlacements);
        PRINT_PROFILE(disruptChoice);
        PRINT_PROFILE(simulateTurn);
        PRINT_PROFILE(evaluate);
        PRINT_PROFILE(connectionPath);
        PRINT_PROFILE(railGroupOf);
        PRINT_PROFILE(stateCopy);
        PRINT_PROFILE(openGapTotal);
    }
}
