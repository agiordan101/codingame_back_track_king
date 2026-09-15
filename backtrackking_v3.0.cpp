// v3.0

// Greedy one-turn planner. No search, no lookahead: every turn the board is
// scored once, cell by cell, and the rails go on the best cells the paint can
// afford.
//
// A cell's value is built in three layers:
//   1. a flat bonus if its region holds a town -- such a region can never be
//      inked, so a rail laid there is never erased;
//   2. W+H-length for every shortest town-to-town path crossing it, so the
//      short wishes (the ones a turn can actually finish) weigh most;
//   3. scaled down by how close its region is to being inked.
//
// The DISRUPT target is read off the same map, between layers 2 and 3: the
// region where the opponent's rails sit on the most valuable cells, minus
// what our own rails there would cost us.

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
#include <set>
#include <sstream>
#include <cstdio>
#include <cstdint>
#include <climits>
#include <chrono>

using namespace std;

// ====================
// CONSTANTS

static const int PAINT_PER_TURN = 3;

// Owner marker for a tile carrying no rail.
static const int NO_OWNER = -1;
// Owner marker for a rail both players placed on the same turn.
static const int NEUTRAL_OWNER = 2;

// The statement defines inked as instability >= 4, a DISRUPT adding 1.
static const int INK_INSTABILITY_THRESHOLD = 4;
// Numerator of the (INK_SCALE - instability) / INK_SCALE discount. The
// division is never performed: it is the same for every cell, so dropping it
// leaves the ranking untouched and the values exact.
static const int INK_SCALE = INK_INSTABILITY_THRESHOLD + 1;

// Direction priority: NORTH, EAST, SOUTH, WEST.
static const int DIR_X[4] = {0, 1, 0, -1};
static const int DIR_Y[4] = {-1, 0, 1, 0};

// ====================
// DEBUG HOOKS
//
// Observation points for the external viewer (tools/debug_tool.cpp): it
// watches the planner, never steers it. Without DEBUG_TOOL the call sites
// expand to nothing, so the competition build carries no trace of them.

#ifdef DEBUG_TOOL
class Map;
class Coord;
class ActionSet;

// Defined in tools/debug_tool.cpp.
void dbgTurnBegin(const Map &board, const vector<pair<int, int>> &wishes);
void dbgValues(const vector<int> &values);
void dbgRegionScores(const vector<int> &scoreBySlot);
void dbgTurnEnd(const ActionSet &action, int disrupt);

#define DBG_TURN_BEGIN(board, wishes) dbgTurnBegin(board, wishes)
#define DBG_VALUES(values) dbgValues(values)
#define DBG_REGION_SCORES(scores) dbgRegionScores(scores)
#define DBG_TURN_END(action, disrupt) dbgTurnEnd(action, disrupt)
#else
#define DBG_TURN_BEGIN(board, wishes) ((void)0)
#define DBG_VALUES(values) ((void)0)
#define DBG_REGION_SCORES(scores) ((void)0)
#define DBG_TURN_END(action, disrupt) ((void)0)
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

// Packed to 4 bytes so the board streams through cache: the scoring passes
// read it end to end several times a turn.
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

// A region's fixed description. Ink and instability are per-turn state and
// live on the Map, so the cell list is read only when a region is walked.
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
// MAP

// Board state, split in two: StaticMap holds what the referee fixes once
// (terrain regions, towns), Map what a turn can change (tiles, region ink).

class StaticMap
{
public:
    vector<Town> towns;
    // Regions by dense index; regionSlot maps id -> index, so a sparse
    // referee numbering still works.
    vector<Region> regions;
    vector<int> regionSlot;
    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // Flat per-cell town flag: a set<pair> here cost a tree walk on a lookup
    // every candidate scan performs.
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
    // Public so the debug viewer (tools/debug_tool.cpp) can serialise a board.
    Grid grid;
    // Per-region ink state by slot: flat byte arrays rather than fields on
    // Region, so a region's state is one indexed read.
    vector<char> regionInkedFlag;
    vector<unsigned char> regionInstability;
    // The immutable half, shared rather than owned.
    const StaticMap *stat = nullptr;

    // Slot of the region owning a cell, so hot paths never touch a hash map.
    int regionSlotAt(int x, int y) const
    {
        return stat->slotOfRegion(grid.get(x, y).regionId);
    }

    // ---- geometry ----

    int width() const { return grid.width; }
    int height() const { return grid.height; }

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
                outWishes.emplace_back(townId, other);
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
    }

    // ---- tile queries ----

    int tileType(int x, int y) const { return grid.get(x, y).type; }
    int tileOwner(int x, int y) const { return grid.get(x, y).tracksOwner; }
    int tileRegion(int x, int y) const { return grid.get(x, y).regionId; }

    bool isTownCell(int x, int y) const
    {
        return stat->townCellFlag[y * grid.width + x] != 0;
    }
    bool hasRail(int x, int y) const
    {
        return grid.get(x, y).tracksOwner != NO_OWNER;
    }

    // Both tile and region are checked: the referee inks a whole region, and
    // the flag is what the per-region state carries.
    bool isInked(int x, int y) const
    {
        const Tile &tile = grid.get(x, y);
        if (tile.inked)
            return true;
        const int slot = stat->slotOfRegion(tile.regionId);
        return slot >= 0 && regionInkedFlag[slot] != 0;
    }

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

    int railCost(int x, int y) const { return terrainCost(grid.get(x, y).type); }

    // Part of the live network: a town, or a rail outside ink.
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
};

// ====================
// ACTIONS

// One turn's building: the cells to lay rail on, in the order chosen. At most
// PAINT_PER_TURN cells and often fewer, since each costs its terrain -- a
// single mountain (cost 3) spends the whole turn.
class ActionSet
{
public:
    vector<Coord> cells;
    // Paint spent by `cells`, i.e. the sum of their terrain costs.
    int cost = 0;

    bool empty() const { return cells.empty(); }
};

// ====================
// PLANNER

// One value map per turn, and the turn's move read off it. Every buffer is
// flat and allocated once, so a turn is a handful of linear sweeps over the
// board with no allocation at all.
class Planner
{
public:
    int myId = 0, foeId = 1;

    // Board geometry and the layer that never changes: the uninkable bonus.
    // Also resolves each wish to a pair of coordinates, deduplicated -- the
    // referee reports a wish from both of its towns.
    void init(const Map &board, const vector<pair<int, int>> &wishes)
    {
        W = board.width();
        H = board.height();
        N = W * H;

        value.assign(N, 0);
        gScore.assign(N, 0);
        gStamp.assign(N, 0);
        parent.assign(N, -1);
        heap.reserve(N);
        cellSlot.assign(N, -1);

        regionScore.assign(board.stat->regions.size(), 0);
        regionMult.assign(board.stat->regions.size(), INK_SCALE);

        // A rail in a town region can never be erased, so it is worth holding
        // on its own. A quarter of a typical path reward: enough to break a
        // tie between two cells on the same path, not enough to outrank one.
        const int uninkable = (W + H) / 4;
        baseValue.assign(N, 0);
        for (int y = 0, idx = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++, idx++)
            {
                const int slot = board.regionSlotAt(x, y);
                cellSlot[idx] = (int16_t)slot;
                if (slot >= 0 && board.stat->regionHasTown[slot])
                    baseValue[idx] = uninkable;
            }
        }

        links.clear();
        set<pair<int, int>> seen;
        for (const auto &wish : wishes)
        {
            const int a = min(wish.first, wish.second);
            const int b = max(wish.first, wish.second);
            if (a == b || !seen.insert({a, b}).second)
                continue;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;
            links.push_back({board.townCoordOf(a), board.townCoordOf(b)});
        }
    }

    // The turn's move: where to lay rail, and which region to DISRUPT (-1 for
    // none). Every step below reads the value map built by the first one.
    void plan(const Map &board, ActionSet &outAction, int &outDisrupt)
    {
        buildValueMap(board);
        outDisrupt = chooseDisrupt(board);
        // After the disrupt choice on purpose: the discount says how long a
        // rail would survive there, which is a placement question, whereas the
        // disrupt is about what sits there now.
        applyInkDiscount(board);
        DBG_VALUES(value);
        chooseRails(board, outAction);
    }

private:
    int W = 0, H = 0, N = 0;

    // The static layer, and the turn's working copy of it.
    vector<int> baseValue;
    vector<int> value;
    // cell -> region slot, flattened once: the per-region sweeps would
    // otherwise chase regionId through StaticMap::regionSlot per cell. int16
    // halves the traffic of the array the sweeps stream alongside the tiles.
    vector<int16_t> cellSlot;

    // Wishes as coordinate pairs, resolved once.
    vector<pair<Coord, Coord>> links;

    // Per-region accumulators, small enough to sit in L1 next to the sweeps.
    vector<int> regionScore;
    vector<int> regionMult;

    // A* scratch. gScore is stamped rather than cleared, so a run touches only
    // the cells it visits.
    vector<int> gScore, parent;
    vector<int> gStamp;
    vector<uint64_t> heap;
    int stamp = 0;

    // ---- layer 1 + 2: the value map ----

    void buildValueMap(const Map &board)
    {
        value = baseValue; // same size, so a plain copy of the bytes
        for (const auto &link : links)
            addPathReward(board, link.first, link.second);
    }

    // Shortest terrain path between two towns, ink impassable, every cell of
    // it rewarded W+H-cost -- so a short connection, the kind a turn can
    // actually finish, weighs more than a long one. The path is never stored:
    // the parent chain is walked straight back from the destination.
    void addPathReward(const Map &board, Coord src, Coord dst)
    {
        const int srcIdx = src.y * W + src.x;
        const int dstIdx = dst.y * W + dst.x;
        if (srcIdx == dstIdx)
            return;

        stamp++;
        heap.clear();
        gScore[srcIdx] = 0;
        gStamp[srcIdx] = stamp;
        parent[srcIdx] = -1;
        heap.push_back(packHeap(heuristic(srcIdx, dst), srcIdx));

        int total = -1;
        while (!heap.empty())
        {
            pop_heap(heap.begin(), heap.end(), greater<uint64_t>());
            const uint64_t top = heap.back();
            heap.pop_back();

            const int cur = (int)(top & 0xFFFFFFFFu);
            const int g = gScore[cur];
            // Stale entry: a shorter path to `cur` was found after this push.
            if ((int)(top >> 32) != g + heuristic(cur, dst))
                continue;
            if (cur == dstIdx)
            {
                total = g;
                break;
            }

            const int cx = cur % W, cy = cur / W;
            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                // Ink is impassable: a path through it could never be built.
                if (board.isInked(nx, ny))
                    continue;
                const int step = terrainCost(board.tileType(nx, ny));
                if (step == INT_MAX)
                    continue;

                const int nIdx = ny * W + nx;
                const int ng = g + step;
                if (gStamp[nIdx] == stamp && ng >= gScore[nIdx])
                    continue;
                gScore[nIdx] = ng;
                gStamp[nIdx] = stamp;
                parent[nIdx] = cur;
                heap.push_back(packHeap(ng + heuristic(nIdx, dst), nIdx));
                push_heap(heap.begin(), heap.end(), greater<uint64_t>());
            }
        }

        if (total < 0)
            return; // walled apart by ink: nothing to steer towards

        // Never zero, so a path longer than the board still marks its cells.
        const int reward = max(1, W + H - total);
        for (int cur = dstIdx; cur != -1; cur = parent[cur])
            value[cur] += reward;
    }

    int heuristic(int idx, Coord dst) const
    {
        return abs(idx % W - dst.x) + abs(idx / W - dst.y);
    }

    // (f, cell) in one word, so the heap compares a single 64-bit integer.
    static uint64_t packHeap(int f, int idx)
    {
        return ((uint64_t)(uint32_t)f << 32) | (uint32_t)idx;
    }

    // ---- the disrupt target ----

    // Inking a region erases every rail in it, so a region is worth hitting
    // by what the opponent holds there minus what we do, each rail counted at
    // the value of the cell it sits on. Returns -1 when nothing is worth it.
    int chooseDisrupt(const Map &board)
    {
        fill(regionScore.begin(), regionScore.end(), 0);

        // One streaming pass: tiles, values and slots are all read in order,
        // and the scatter lands in an array of a few dozen entries.
        for (int idx = 0; idx < N; idx++)
        {
            const int slot = cellSlot[idx];
            if (slot < 0)
                continue;
            const int owner = board.grid.tiles[idx].tracksOwner;
            if (owner == foeId)
                regionScore[slot] += value[idx];
            else if (owner == myId)
                regionScore[slot] -= value[idx];
        }

        DBG_REGION_SCORES(regionScore);

        int best = -1, bestScore = 0;
        for (size_t slot = 0; slot < regionScore.size(); slot++)
        {
            // A town's region can never be inked, and an inked one is done.
            if (board.regionInkedFlag[slot] || board.stat->regionHasTown[slot])
                continue;
            if (regionScore[slot] > bestScore)
            {
                bestScore = regionScore[slot];
                best = board.stat->regions[slot].id;
            }
        }
        return best;
    }

    // ---- layer 3: the ink discount ----

    // A cell is worth (INK_SCALE - instability) / INK_SCALE of its value: a
    // region one disrupt from ink is a rail about to be erased. The division
    // is common to every cell, so it is dropped and the ranking kept exact.
    void applyInkDiscount(const Map &board)
    {
        for (size_t slot = 0; slot < regionMult.size(); slot++)
        {
            const int level = min((int)board.regionInstability[slot],
                                  INK_INSTABILITY_THRESHOLD);
            regionMult[slot] = INK_SCALE - level;
        }
        for (int idx = 0; idx < N; idx++)
        {
            const int slot = cellSlot[idx];
            if (slot >= 0)
                value[idx] *= regionMult[slot];
        }
    }

    // ---- the turn's rails ----

    // Best cells first, one at a time: the second rail is chosen knowing the
    // first, which is what lets a turn grow a line rather than three stubs.
    void chooseRails(const Map &board, ActionSet &out)
    {
        out.cells.clear();
        out.cost = 0;

        int paintLeft = PAINT_PER_TURN;
        while (paintLeft > 0 && (int)out.cells.size() < PAINT_PER_TURN)
        {
            const int idx = bestCell(board, paintLeft, out);
            if (idx < 0)
                break;
            const int x = idx % W, y = idx / W;
            const int cost = board.railCost(x, y);
            out.cells.push_back(Coord(x, y));
            out.cost += cost;
            paintLeft -= cost;
        }
    }

    // The highest-valued affordable cell, or -1. Ties are broken towards the
    // network, then towards cheap terrain, then by scan order -- and the tie
    // is the normal case, since a path rewards all of its cells equally.
    // Growing from the network is what turns those equal cells into a line.
    int bestCell(const Map &board, int paintLeft, const ActionSet &placed) const
    {
        uint64_t bestKey = 0;
        int best = -1;

        for (int y = 0, idx = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++, idx++)
            {
                // Cheapest filter first, and it streams the value array.
                if (value[idx] <= 0)
                    continue;
                if (!board.canPlaceRail(x, y) || takenThisTurn(placed, x, y))
                    continue;
                const int cost = board.railCost(x, y);
                if (cost > paintLeft)
                    continue;

                // value | touches | cheapness | scan order, most
                // significant first, so the whole tie-break is one compare.
                // The low 20 bits hold the cell, capping the board at 2^20.
                const uint64_t key =
                    ((uint64_t)value[idx] << 23) |
                    ((uint64_t)touchesNetwork(board, placed, x, y) << 22) |
                    ((uint64_t)(3 - cost) << 20) |
                    (uint64_t)(N - 1 - idx);
                if (key > bestKey)
                {
                    bestKey = key;
                    best = idx;
                }
            }
        }
        return best;
    }

    // Rails picked earlier this turn are not on the board yet, so they are
    // carried here: at most PAINT_PER_TURN of them, hence the linear scan.
    static bool takenThisTurn(const ActionSet &placed, int x, int y)
    {
        for (const Coord &c : placed.cells)
            if (c.x == x && c.y == y)
                return true;
        return false;
    }

    static int touchesNetwork(const Map &board, const ActionSet &placed,
                              int x, int y)
    {
        for (int k = 0; k < 4; k++)
        {
            const int nx = x + DIR_X[k], ny = y + DIR_Y[k];
            if (board.inBounds(nx, ny) && board.isConnectable(nx, ny))
                return 1;
            if (takenThisTurn(placed, nx, ny))
                return 1;
        }
        return 0;
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

    int myScore, foeScore;

    // wishes: pairs of town ids that want to be connected
    vector<pair<int, int>> wishes;
    // activeConnections: pairs of town ids being connected
    map<pair<int, int>, bool> activeConnections;

    Planner planner;

    // Start of the current turn, reported to stderr.
    std::chrono::steady_clock::time_point turnStart;

    void init()
    {
        cin >> myId;
        foeId = 1 - myId;
        gameMap.readTerrain(cin, staticMap);
        activeConnections.clear();
        gameMap.readTowns(cin, staticMap, wishes);

        planner.myId = myId;
        planner.foeId = foeId;
        planner.init(gameMap, wishes);
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
        DBG_TURN_BEGIN(gameMap, wishes);

        ActionSet action;
        int disrupt = -1;
        planner.plan(gameMap, action, disrupt);

        DBG_TURN_END(action, disrupt);

        vector<string> actions;
        for (const Coord &c : action.cells)
            actions.push_back("PLACE_TRACKS " + to_string(c.x) + " " +
                              to_string(c.y));
        if (disrupt != -1)
            actions.push_back("DISRUPT " + to_string(disrupt));

        if (!actions.empty())
        {
            stringstream msg;
            msg << "MESSAGE " << action.cells.size() << " rails";
            if (disrupt != -1)
                msg << " D" << disrupt;
            actions.push_back(msg.str());

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

        const long long us =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - turnStart)
                .count();
        fprintf(stderr, "planner : %lld us  %d rails (%d paint)  disrupt %d\n",
                us, (int)action.cells.size(), action.cost, disrupt);
    }
};

// Returns false once the referee's stream ends, so a piped game (a replay,
// a local match) stops instead of spinning on a dead stdin.
bool mainLoopturn(Game &game)
{
    game.parse();
    if (!cin)
        return false;
    game.gameTurn();
    return true;
}

// The debug tool includes this file to drive the very same engine, and brings
// its own entry point.
#ifndef DEBUG_TOOL
int main()
{
    Game game;
    game.init();
    while (mainLoopturn(game))
        ;
}
#endif // !DEBUG_TOOL
