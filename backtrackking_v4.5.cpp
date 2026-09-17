#define BOT_VERSION "4.5"

// One-turn search. The board is still scored cell by cell, but the move is no
// longer the best cells taken one at a time: every affordable combination of
// rails is played out against every disrupt candidate, and the combination
// whose resulting board scores best is the turn.
//
// A cell's value is built in three layers:
//   1. a flat bonus if its region holds a town -- such a region can never be
//      inked, so a rail laid there is never erased;
//   2. W+H-length for every shortest town-to-town path crossing it, so the
//      short wishes (the ones a turn can actually finish) weigh most;
//   3. scaled down by how close its region is to being inked.
//
// The DISRUPT candidates are read off the same map, between layers 2 and 3:
// the regions where the opponent's rails sit on the most valuable cells. The
// search then judges each of them by the connections the cut actually breaks,
// so a cut that costs us more than it costs him loses on its own.

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

// Planning budget, counted from the moment the turn's input is parsed. The
// referee allows 50 ms; the rest is left to parsing jitter and the reply.
static const int PLAN_BUDGET_MS = 30;

// Ranked cells the combination search draws from, and disrupt regions tried
// alongside them. Together they bound the search at C(20,3) combinations
// times five boards, a few thousand evaluations.
static const int MAX_RAIL_CANDIDATES = 24;
static const int MAX_DISRUPT_CANDIDATES = 4;

// One completed connection outweighs any reachable sum of cell values, so
// both live in a single integer key without ever mixing.
static const long long SCORE_WEIGHT = 10000;

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
void dbgCutTally(int slot, int mine, int foe);
void dbgTurnEnd(const ActionSet &action, int disrupt);

#define DBG_TURN_BEGIN(board, wishes) dbgTurnBegin(board, wishes)
#define DBG_VALUES(values) dbgValues(values)
#define DBG_REGION_SCORES(scores) dbgRegionScores(scores)
#define DBG_CUT_TALLY(slot, mine, foe) dbgCutTally(slot, mine, foe)
#define DBG_TURN_END(action, disrupt) dbgTurnEnd(action, disrupt)
#else
#define DBG_TURN_BEGIN(board, wishes) ((void)0)
#define DBG_VALUES(values) ((void)0)
#define DBG_REGION_SCORES(scores) ((void)0)
#define DBG_CUT_TALLY(slot, mine, foe) ((void)0)
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

    // Live connections, densely indexed for the turn. Barely a handful exist
    // at once (measured: <=5 live, 10 distinct over a whole game), so every
    // array below is a few dozen bytes and stays in L1.
    //
    // connRails[c] counts the rails carrying connection c, split by owner: a
    // connection is worth to a player what he holds of it.
    vector<pair<int, int>> connPair;   // c -> (minTownId, maxTownId)
    // Rails on connection c owned by player 0 and player 1. Map has no notion
    // of sides, so the planner reads the slot matching its own id.
    vector<int> connRails[2];
    // Connections crossing each cell, as a flat CSR-style pair of arrays:
    // cellConnAt[idx]..cellConnAt[idx+1] indexes into cellConnList. A cell
    // carries one or two connections at most, so this stays tiny and is read
    // straight through when a turn's income is tallied.
    vector<int> cellConnAt, cellConnList;
    // Which connections cross a region, as a slot-major bitmask row. Sized
    // regions x connWords, rebuilt each turn.
    vector<uint64_t> regionConnMask;
    int connCount = 0, connWords = 0;
    // (region slot, connection) seen while parsing, folded into the mask once
    // connCount is final. Kept as a member so the turn allocates nothing.
    vector<pair<int, int>> pendingRegionConn;
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
        connPair.clear();
        connRails[0].clear();
        connRails[1].clear();
        connCount = 0;
        cellConnAt.assign((size_t)grid.width * grid.height + 1, 0);
        cellConnList.clear();
        // Worst case one connection per town pair; grown lazily below.
        vector<int> cellConns;

        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                int tracksOwner, instability;
                string inkedStr, partStr;
                in >> tracksOwner >> instability >> inkedStr >> partStr;
                bool inked = (inkedStr != "0");
                cellConns.clear();
                if (partStr != "x")
                {
                    stringstream ss(partStr);
                    string conn;
                    while (getline(ss, conn, ','))
                    {
                        int fromTownId, toTownId;
                        sscanf(conn.c_str(), "%d-%d", &fromTownId, &toTownId);
                        outActiveConnections[{fromTownId, toTownId}] = true;
                        cellConns.push_back(internConnection(fromTownId, toTownId));
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

                // A connection is worth to a player what he holds of it, and a
                // region threatens every connection its cells carry.
                const int owner = tile.tracksOwner;
                cellConnAt[(size_t)y * grid.width + x] = (int)cellConnList.size();
                for (int c : cellConns)
                {
                    cellConnList.push_back(c);
                    if (owner == 0 || owner == 1)
                        connRails[owner][c]++;
                    if (slot >= 0)
                        pendingRegionConn.push_back({slot, c});
                }
            }
        }

        cellConnAt[(size_t)grid.width * grid.height] = (int)cellConnList.size();

        // connCount is only final here, so the slot-major bitmask is laid out
        // once the width is known.
        connWords = (connCount + 63) / 64;
        regionConnMask.assign(stat->regions.size() * (size_t)connWords, 0);
        for (const auto &rc : pendingRegionConn)
            regionConnMask[(size_t)rc.first * connWords + rc.second / 64] |=
                (uint64_t)1 << (rc.second % 64);
        pendingRegionConn.clear();
    }

    // Dense index for a connection, created on first sight. The referee writes
    // a pair in either order, so it is normalised. A linear scan beats a hash
    // map here: a game holds about ten connections in all.
    int internConnection(int a, int b)
    {
        const pair<int, int> key{min(a, b), max(a, b)};
        for (int c = 0; c < connCount; c++)
            if (connPair[c] == key)
                return c;
        connPair.push_back(key);
        connRails[0].push_back(0);
        connRails[1].push_back(0);
        return connCount++;
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
    // When the search must stop. Set by Game from the turn's parse time.
    std::chrono::steady_clock::time_point deadline;
    // Action sets the last turn actually scored, reported to stderr.
    int evaluated = 0;
    // The running score, as the referee reported it this turn. Set by Game.
    int myScore = 0, foeScore = 0;

    // Board geometry and the layer that never changes: the uninkable bonus.
    // Also resolves each wish to a pair of coordinates, oriented from the town
    // that declared it.
    void init(const Map &board, const vector<pair<int, int>> &wishes)
    {
        W = board.width();
        H = board.height();
        N = W * H;

        value.assign(N, 0);
        spread.assign(N, 0);
        disruptValue.assign(N, 0);
        wasZero.assign(N, 0);
        gScore.assign(N, 0);
        gStamp.assign(N, 0);
        parent.assign(N, -1);
        heap.reserve(N);
        cellSlot.assign(N, -1);
        compMine.assign(N, -1);
        compFoe.assign(N, -1);
        bfsSeen.assign(N, 0);
        fresh.assign(N, 0);
        bfsParent.assign(N, -1);
        bfsQueue.reserve(N);
        floodStack.reserve(N);
        candKey.reserve(N);

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

        // Kept in the referee's own orientation: readTowns emits (declaring
        // town, wanted town), and A* breaks its ties NESW walking outwards
        // from the source, so swapping the two picks a different corridor
        // wherever several shortest paths exist. Sorting the pair by id, as
        // this did, threw that away. No wish is declared by both of its towns,
        // so a pair appears once and there is nothing to deduplicate; a guard
        // stays below in case one ever is.
        links.clear();
        set<pair<int, int>> seen;
        for (const auto &wish : wishes)
        {
            const int from = wish.first, to = wish.second;
            if (from == to || !seen.insert({min(from, to), max(from, to)}).second)
                continue;
            if (!board.hasTown(from) || !board.hasTown(to))
                continue;
            links.push_back({board.townCoordOf(from), board.townCoordOf(to)});
        }
    }

    // The turn's move: where to lay rail, and which region to DISRUPT (-1 for
    // none). Every step below reads the value map built by the first one.
    void plan(const Map &board, ActionSet &outAction, int &outDisrupt)
    {
        buildValueMap(board);
        rankDisruptRegions(board);
        // After the disrupt ranking on purpose: the discount says how long a
        // rail would survive there, which is a placement question, whereas the
        // disrupt is about what sits there now.
        applyInkDiscount(board);
        searchActionSets(board, outAction, outDisrupt);
        // After the search: it may have spread the map to find candidates,
        // and it is that final map the move was read off.
        DBG_VALUES(value);
    }

private:
    int W = 0, H = 0, N = 0;

    // The static layer, the turn's working copy of it, and the snapshot the
    // diffusion reads while it writes into the copy.
    vector<int> baseValue;
    vector<int> value;
    vector<int> spread;
    // The disrupt's own diffused copy, so spreading for it cannot reach the
    // map the rails are ranked on.
    vector<int> disruptValue;
    // Cells left at zero by buildValueMap: the only ones a diffusion may fill.
    vector<char> wasZero;
    // cell -> region slot, flattened once: the per-region sweeps would
    // otherwise chase regionId through StaticMap::regionSlot per cell. int16
    // halves the traffic of the array the sweeps stream alongside the tiles.
    vector<int16_t> cellSlot;

    // Wishes as coordinate pairs, resolved once.
    vector<pair<Coord, Coord>> links;

    // Per-region accumulators, small enough to sit in L1 next to the sweeps.
    vector<int> regionScore;
    vector<int> regionMult;
    // (score, slot) ranked each turn; a handful of entries, allocated once.
    vector<pair<int, int>> candidates;

    // A* scratch. gScore is stamped rather than cleared, so a run touches only
    // the cells it visits.
    vector<int> gScore, parent;
    vector<int> gStamp;
    vector<uint64_t> heap;
    int stamp = 0;

    // ---- search scratch ----

    // A combination of rails: candidate slots and the value they add up to.
    // Eight bytes, so the whole set of them stays in L1.
    struct Combo
    {
        int32_t sum;
        uint8_t n;
        uint8_t slot[3];
    };

    // Ranked placement candidates, in parallel arrays so the combination
    // loops stream three ints rather than chase a struct.
    vector<uint64_t> candKey;
    vector<int> candCell, candCost, candValue;
    vector<Combo> combos;

    // Connection solver scratch: a FIFO sweep stamped rather than cleared,
    // so re-solving a wish touches only the cells it reaches.
    vector<int> bfsSeen, bfsParent, bfsQueue;
    int bfsStamp = 0;
    // The combination's own cells, stamped once per state.
    vector<int> fresh;
    int freshStamp = 0;
    // Per-wish baseline payout and the cells its sweep reached, for the
    // disrupt board being evaluated.
    long long baseMine = 0, baseFoe = 0;
    vector<long long> wishMine, wishFoe;
    vector<char> wishReach;
    // Who each side can reach, one mark per cell: same mark, same network.
    // Redone once per disrupt candidate, never per combination. Plus the
    // explicit stack the sweep runs on.
    vector<int> compMine, compFoe;
    vector<int> floodStack;

    // ---- layer 1 + 2: the value map ----

    void buildValueMap(const Map &board)
    {
        value = baseValue; // same size, so a plain copy of the bytes
        for (const auto &link : links)
            addPathReward(board, link.first, link.second);

        // The cells the paths and the town bonus left untouched. Pinned here,
        // once, so every later diffusion ring writes only into this set: a
        // corridor cell keeps the value its path gave it whatever the spread
        // does, and the ink discount cannot be applied twice to one cell.
        for (int idx = 0; idx < N; idx++)
            wasZero[idx] = (value[idx] == 0) ? 1 : 0;
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
    // the value of the cell it sits on. Only ranks: the search decides.
    void rankDisruptRegions(const Map &board)
    {
        // Scored on a diffused copy, never on `value` itself. A corridor is
        // one cell wide and arbitrary among many equal-length paths, so the
        // opponent builds beside it rather than on it and reads as worthless
        // on the raw map -- one ring outwards is what makes his rails count.
        // The copy is what keeps it off the placement ranking: bleeding the
        // real map lifts a mountain next to a corridor over a plain further
        // along it, and the turn buys one rail where it could buy three.
        disruptValue = value;
        diffuseInto(board, disruptValue);

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
                regionScore[slot] += disruptValue[idx];
            else if (owner == myId)
                regionScore[slot] -= disruptValue[idx];
        }

        DBG_REGION_SCORES(regionScore);

        // The best few by score, handed to the search to be played out.
        // Ranking rather than taking the max: the top region can cost us more
        // than it costs the opponent, and the runner-up is then the move.
        candidates.clear();
        for (size_t slot = 0; slot < regionScore.size(); slot++)
        {
            // A town's region can never be inked, and an inked one is done.
            if (board.regionInkedFlag[slot] || board.stat->regionHasTown[slot])
                continue;
            if (regionScore[slot] > 0)
                candidates.push_back({regionScore[slot], (int)slot});
        }
        sort(candidates.begin(), candidates.end(), greater<pair<int, int>>());
        if ((int)candidates.size() > MAX_DISRUPT_CANDIDATES)
            candidates.resize(MAX_DISRUPT_CANDIDATES);
    }

    // Rails at stake on either side of a cut, for the viewer alone. The veto
    // this used to drive is gone: the search counts the connections a cut
    // really breaks, which is the same judgement made exactly.
    void tallyCut(const Map &board, int slot) const
    {
        int mine = 0, foe = 0;
        if (board.connWords > 0)
        {
            const uint64_t *row =
                &board.regionConnMask[(size_t)slot * board.connWords];
            for (int w = 0; w < board.connWords; w++)
                for (uint64_t bits = row[w]; bits; bits &= bits - 1)
                {
                    const int c = w * 64 + __builtin_ctzll(bits);
                    mine += board.connRails[myId][c];
                    foe += board.connRails[foeId][c];
                }
        }
        DBG_CUT_TALLY(slot, mine, foe);
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

    // ---- the turn's move ----

    // Every affordable rail combination against every disrupt candidate. The
    // resulting board is judged by the connections it completes first and by
    // the value of the cells bought second, so finishing a link always beats
    // hoarding good ground. Stops on the clock, keeping the best seen.
    void searchActionSets(const Map &board, ActionSet &out, int &outDisrupt)
    {
        out.cells.clear();
        out.cost = 0;
        outDisrupt = -1;

        buildCandidates(board);
        // Nothing valued in reach: bleed the map one ring outwards and look
        // again, rather than waste the turn. Late games are almost entirely
        // made of these turns, the shortest paths being long since built.
        while (candCell.empty() && diffuseValues(board))
            buildCandidates(board);
        buildCombos();

        long long bestKey = LLONG_MIN;
        Combo bestCombo{0, 0, {0, 0, 0}};
        int bestSlot = -1;
        bool outOfTime = false;
        evaluated = 0;

        // -1 is the no-disrupt board, and it comes first so a turn that runs
        // out of clock still holds a move it has actually evaluated.
        for (int d = -1; d < (int)candidates.size() && !outOfTime; d++)
        {
            // Laying out a disrupt board costs one solve per wish, so the
            // clock is checked before committing to another one rather than
            // only inside the loop it feeds.
            if (d >= 0 && outOfBudget())
                break;
            const int slot = (d < 0) ? -1 : candidates[d].second;
            applyDisrupt(board, slot);

            for (const Combo &combo : combos)
            {
                // Read every single state: one of them can cost a solve per
                // wish, so any stride at all overshoots the budget by far more
                // than the clock reads it saves.
                if (evaluated > 0 && outOfBudget())
                {
                    outOfTime = true;
                    break;
                }
                const long long key = scoreTurn(board, combo, slot);
                evaluated++;
                if (isBetterTurn(key, bestKey, slot, bestSlot))
                {
                    bestKey = key;
                    bestCombo = combo;
                    bestSlot = slot;
                }
            }
        }

        applyRails(board, bestCombo, bestSlot, out);
        if (bestSlot >= 0)
            outDisrupt = board.stat->regions[bestSlot].id;
    }

    // Inks `slot` (-1 for no disrupt): every rail in that region is erased, on
    // both sides, so each player's network is relabelled on the board the cut
    // leaves behind. Returns the income the opponent still collects there --
    // the only part of his score a turn of ours can move.
    void applyDisrupt(const Map &board, int slot)
    {
        if (slot >= 0)
            tallyCut(board, slot);
        baselineIncome(board, slot);
    }

    // The one place the turn's deadline is read. The budget is a hard limit:
    // the search stops to meet it, it is never widened to fit the search.
    bool outOfBudget() const
    {
        return std::chrono::steady_clock::now() >= deadline;
    }

    // Writes the winning combination out as the turn's rails. A rail in the
    // region we ink is erased on the spot, so the scoring dropped it and the
    // move must not carry it either.
    void applyRails(const Map &board, const Combo &combo, int disruptSlot,
                    ActionSet &out) const
    {
        for (int i = 0; i < combo.n; i++)
        {
            const int idx = candCell[combo.slot[i]];
            if (disruptSlot >= 0 && cellSlot[idx] == disruptSlot)
                continue;
            const int x = idx % W, y = idx / W;
            out.cells.push_back(Coord(x, y));
            out.cost += board.railCost(x, y);
        }
    }

    // A disrupt is free, so an equal board is reason enough to take one: the
    // score only sees the connection a cut breaks today, never the
    // instability that inks the region later. Candidates come best-scored
    // first, so the first to tie wins.
    static bool isBetterTurn(long long key, long long bestKey, int slot,
                             int bestSlot)
    {
        if (key != bestKey)
            return key > bestKey;
        return slot >= 0 && bestSlot < 0;
    }

    // The cells worth considering, ranked as the greedy ranked its first pick:
    // value, then contact with the network, then cheap terrain, then scan
    // order. Only the head is kept -- the tail could never enter a best combo.
    void buildCandidates(const Map &board)
    {
        const ActionSet none;
        candKey.clear();
        for (int y = 0, idx = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++, idx++)
            {
                // Cheapest filter first, and it streams the value array.
                if (value[idx] <= 0)
                    continue;
                if (!board.canPlaceRail(x, y))
                    continue;
                const int cost = board.railCost(x, y);
                if (cost > PAINT_PER_TURN)
                    continue;

                // value | touches | cheapness | scan order, most significant
                // first, so the whole tie-break is one compare. The low 20
                // bits hold the cell, capping the board at 2^20.
                candKey.push_back(((uint64_t)value[idx] << 23) |
                                  ((uint64_t)touchesNetwork(board, none, x, y) << 22) |
                                  ((uint64_t)(3 - cost) << 20) |
                                  (uint64_t)(N - 1 - idx));
            }
        }

        const size_t keep =
            min(candKey.size(), (size_t)MAX_RAIL_CANDIDATES);
        partial_sort(candKey.begin(), candKey.begin() + keep, candKey.end(),
                     greater<uint64_t>());

        candCell.clear();
        candCost.clear();
        candValue.clear();
        for (size_t i = 0; i < keep; i++)
        {
            const int idx = N - 1 - (int)(candKey[i] & 0xFFFFF);
            candCell.push_back(idx);
            candCost.push_back(board.railCost(idx % W, idx / W));
            candValue.push_back(value[idx]);
        }
    }

    // Every set of candidates the turn's paint can afford, richest first. The
    // empty set is one of them: a turn may be worth a disrupt and nothing else.
    // Generated widest first, so at equal value more rails win the tie.
    void buildCombos()
    {
        combos.clear();
        const int K = (int)candCell.size();

        for (int i = 0; i < K; i++)
            for (int j = i + 1; j < K; j++)
            {
                if (candCost[i] + candCost[j] > PAINT_PER_TURN)
                    continue; // and no third cell can fit either
                for (int k = j + 1; k < K; k++)
                    if (candCost[i] + candCost[j] + candCost[k] <= PAINT_PER_TURN)
                        combos.push_back(Combo{
                            candValue[i] + candValue[j] + candValue[k], 3,
                            {(uint8_t)i, (uint8_t)j, (uint8_t)k}});
            }
        for (int i = 0; i < K; i++)
            for (int j = i + 1; j < K; j++)
                if (candCost[i] + candCost[j] <= PAINT_PER_TURN)
                    combos.push_back(Combo{candValue[i] + candValue[j], 2,
                                           {(uint8_t)i, (uint8_t)j, 0}});
        for (int i = 0; i < K; i++)
            combos.push_back(Combo{candValue[i], 1, {(uint8_t)i, 0, 0}});
        combos.push_back(Combo{0, 0, {0, 0, 0}});

        // Stable, so equal-valued sets keep the order above: the search reads
        // them best first and a cut-off loses only the weakest.
        stable_sort(combos.begin(), combos.end(),
                    [](const Combo &a, const Combo &b) { return a.sum > b.sum; });
    }

    // Spreads the map one ring outwards: every cell still at zero takes half
    // of each valued neighbour, summed -- so a cell between two of them gets
    // both halves. Reads a snapshot and writes into `value`, so a cell filled
    // by this pass cannot feed the next one within it: one call is one ring.
    //
    // Terminating is free: only zero cells are ever written, so the valued set
    // only grows, and the halving kills the frontier once it drops under 2.
    // Returns false when nothing moved, which is what ends the caller's loop.
    bool diffuseValues(const Map &board) { return diffuseInto(board, value); }

    // The pass itself, on whichever map the caller owns.
    bool diffuseInto(const Map &board, vector<int> &target)
    {
        spread = target;
        bool changed = false;

        for (int y = 0, idx = 0; y < H; y++)
        {
            for (int x = 0; x < W; x++, idx++)
            {
                // Only ever a cell the value map itself left at zero, and only
                // while it is still empty: a corridor is never overwritten,
                // and a ring already laid is never raised by the next one.
                if (!wasZero[idx] || spread[idx] != 0)
                    continue;
                // Ink and impassable ground can never carry a rail, so value
                // must not flow through them either.
                if (board.isInked(x, y) || board.railCost(x, y) == INT_MAX)
                    continue;

                int sum = 0;
                for (int k = 0; k < 4; k++)
                {
                    const int nx = x + DIR_X[k], ny = y + DIR_Y[k];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                        continue;
                    sum += spread[ny * W + nx] / 2;
                }
                if (sum > 0)
                {
                    target[idx] = sum;
                    changed = true;
                }
            }
        }
        return changed;
    }

    // ---- scoring a turn ----

    // The turn's points: connections we complete minus the opponent's, at
    // SCORE_WEIGHT apiece, plus the value of the cells bought. The two never
    // mix -- no reachable sum of cell values comes near one connection.
    long long scoreTurn(const Map &board, const Combo &combo, int disruptSlot)
    {
        int cells[3];
        int n = 0;
        long long sum = 0;
        for (int i = 0; i < combo.n; i++)
        {
            const int idx = candCell[combo.slot[i]];
            // A rail laid in the region we then ink buys nothing at all.
            if (disruptSlot >= 0 && cellSlot[idx] == disruptSlot)
                continue;
            cells[n++] = idx;
            sum += candValue[combo.slot[i]];
        }
        // Score at the end of this turn: what each side already banked, plus
        // what the resulting board pays out.
        long long gainMine = 0, gainFoe = 0;
        collectIncome(board, disruptSlot, cells, n, gainMine, gainFoe);
        const long long lead = (myScore + gainMine) - (foeScore + gainFoe);
        return lead * SCORE_WEIGHT + sum;
    }

    // Is this cell walkable for a connection: a rail or a town, with the
    // disrupt's region erased and the turn's own rails already laid.
    // Walkable for a connection: a rail or a town, the disrupt's region
    // erased and the turn's own rails already laid. freshStamp marks those,
    // so the check is one compare instead of a scan per visited cell.
    bool onNetwork(const Map &board, int idx, int disruptSlot) const
    {
        if (fresh[idx] == freshStamp)
            return true;
        if (disruptSlot >= 0 && cellSlot[idx] == disruptSlot)
            return false;
        if (board.stat->townCellFlag[idx])
            return true;
        return board.grid.tiles[idx].tracksOwner != NO_OWNER;
    }

    // The turn's payout, for both players at once. Every wish is re-solved on
    // the board the move leaves behind: connections the ink broke stop paying,
    // and a rail that completes one starts paying the same turn. Ties go NESW
    // from the asking town, which a FIFO sweep in that neighbour order gives.
    // Income for one wish on the current board, added into mine/foe. Returns
    // the number of cells the sweep reached, so a caller can tell whether a
    // combination could possibly have changed this wish.
    // Income for one wish on the board the move leaves behind, added into
    // mine/foe. Ties go NESW from the asking town, which a FIFO sweep in that
    // neighbour order gives for free.
    void payWish(const Map &board, int disruptSlot,
                 const pair<Coord, Coord> &link, long long &mine,
                 long long &foe)
    {
        const int src = link.first.y * W + link.first.x;
        const int dst = link.second.y * W + link.second.x;
        if (src == dst)
            return;

        bfsStamp++;
        bfsQueue.clear();
        bfsQueue.push_back(src);
        bfsSeen[src] = bfsStamp;
        bfsParent[src] = -1;
        bool found = false;
        for (size_t head = 0; head < bfsQueue.size() && !found; head++)
        {
            const int cur = bfsQueue[head];
            const int cx = cur % W, cy = cur / W;
            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                const int nIdx = ny * W + nx;
                if (bfsSeen[nIdx] == bfsStamp)
                    continue;
                if (!onNetwork(board, nIdx, disruptSlot))
                    continue;
                bfsSeen[nIdx] = bfsStamp;
                bfsParent[nIdx] = cur;
                if (nIdx == dst)
                {
                    found = true;
                    break;
                }
                bfsQueue.push_back(nIdx);
            }
        }
        if (!found)
            return; // no path: the wish pays nobody

        // One point per rail owned on the path. Towns carry no rail.
        for (int cur = dst; cur != -1; cur = bfsParent[cur])
        {
            if (board.stat->townCellFlag[cur])
                continue;
            if (fresh[cur] == freshStamp)
            {
                mine++; // laid this turn, so ours
                continue;
            }
            const int owner = board.grid.tiles[cur].tracksOwner;
            if (owner == myId)
                mine++;
            else if (owner == foeId)
                foe++;
        }
    }

    // The board's payout with no rail of ours added, plus, per wish, the set
    // of cells its sweep could see. Done once per disrupt board: a wish whose
    // sweep never reached a cell the combination touches cannot change, so
    // most wishes are settled here and never re-solved.
    void baselineIncome(const Map &board, int disruptSlot)
    {
        freshStamp++; // no fresh cells in the baseline
        baseMine = 0;
        baseFoe = 0;
        wishMine.assign(links.size(), 0);
        wishFoe.assign(links.size(), 0);
        wishReach.assign(links.size() * (size_t)N, 0);
        for (size_t w = 0; w < links.size(); w++)
        {
            long long m = 0, f = 0;
            payWish(board, disruptSlot, links[w], m, f);
            wishMine[w] = m;
            wishFoe[w] = f;
            baseMine += m;
            baseFoe += f;
            // Cells the sweep touched, plus their neighbours: a rail laid on
            // the frontier extends the reachable area, so it can shorten a
            // path or create one where none existed. Anything further out
            // cannot touch this wish at all.
            char *row = &wishReach[w * (size_t)N];
            for (int idx = 0; idx < N; idx++)
                row[idx] = 0;
            for (int idx = 0; idx < N; idx++)
            {
                if (bfsSeen[idx] != bfsStamp)
                    continue;
                row[idx] = 1;
                const int cx = idx % W, cy = idx / W;
                for (int k = 0; k < 4; k++)
                {
                    const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                        row[ny * W + nx] = 1;
                }
            }
        }
    }

    // The turn's payout for both players. Only the wishes the combination can
    // reach are re-solved; the rest keep the baseline they were given.
    void collectIncome(const Map &board, int disruptSlot, const int *cells,
                       int n, long long &mineOut, long long &foeOut)
    {
        long long mine = baseMine, foe = baseFoe;
        freshStamp++;
        for (int i = 0; i < n; i++)
            fresh[cells[i]] = freshStamp;

        for (size_t w = 0; w < links.size(); w++)
        {
            const char *row = &wishReach[w * (size_t)N];
            bool touched = false;
            for (int i = 0; i < n && !touched; i++)
                touched = row[cells[i]] != 0;
            if (!touched)
                continue; // the new rails are out of this wish's world

            long long m = 0, f = 0;
            payWish(board, disruptSlot, links[w], m, f);
            mine += m - wishMine[w];
            foe += f - wishFoe[w];
        }
        mineOut = mine;
        foeOut = foe;
    }

    // Works out who a player can reach from where, once the disrupt has
    // erased its region. Two towns are connected for him -- and so pay him --
    // exactly when they come back with the same mark. Towns are always
    // reachable, whoever owns the rails around them.
    void mapReachability(const Map &board, int p, int disruptSlot,
                         vector<int> &comp)
    {
        comp.assign(N, -1);
        int next = 0;
        for (int seed = 0; seed < N; seed++)
        {
            if (comp[seed] != -1 || !inNetwork(board, p, seed, disruptSlot))
                continue;
            const int id = next++;
            comp[seed] = id;
            floodStack.clear();
            floodStack.push_back(seed);
            while (!floodStack.empty())
            {
                const int cur = floodStack.back();
                floodStack.pop_back();
                const int cx = cur % W, cy = cur / W;
                for (int k = 0; k < 4; k++)
                {
                    const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                    if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                        continue;
                    const int nIdx = ny * W + nx;
                    if (comp[nIdx] != -1 || !inNetwork(board, p, nIdx, disruptSlot))
                        continue;
                    comp[nIdx] = id;
                    floodStack.push_back(nIdx);
                }
            }
        }
    }

    bool inNetwork(const Map &board, int p, int idx, int disruptSlot) const
    {
        if (disruptSlot >= 0 && cellSlot[idx] == disruptSlot)
            return false;
        if (board.stat->townCellFlag[idx])
            return true;
        return board.grid.tiles[idx].tracksOwner == p;
    }

    // Wishes both of whose towns sit in one component of `comp`, i.e. the
    // connections that player actually holds and is paid for.
    int countConnectedWishes(const vector<int> &comp) const
    {
        int total = 0;
        for (const auto &link : links)
        {
            const int a = link.first.y * W + link.first.x;
            const int b = link.second.y * W + link.second.x;
            if (comp[a] >= 0 && comp[a] == comp[b])
                total++;
        }
        return total;
    }

    // The connections we would hold with `cells` laid on top of our network.
    // A union-find over just the new cells and the components they touch --
    // at most fifteen nodes, on the stack -- so trying a combination never
    // costs a pass over the board.
    int countMyWishesWith(const int *cells, int n) const
    {
        int parent[16], compOf[16];
        int cnt = n;
        for (int i = 0; i < n; i++)
        {
            parent[i] = i;
            compOf[i] = -1;
        }

        for (int i = 0; i < n; i++)
        {
            const int cx = cells[i] % W, cy = cells[i] / W;
            for (int k = 0; k < 4; k++)
            {
                const int nx = cx + DIR_X[k], ny = cy + DIR_Y[k];
                if (nx < 0 || nx >= W || ny < 0 || ny >= H)
                    continue;
                const int nIdx = ny * W + nx;
                // Another cell of the same combination: they touch, so the
                // rails they buy form one piece of network.
                for (int j = 0; j < n; j++)
                    if (cells[j] == nIdx)
                        unite(parent, i, j);
                const int c = compMine[nIdx];
                if (c >= 0)
                    unite(parent, i, intern(parent, compOf, cnt, c));
            }
        }

        int total = 0;
        for (const auto &link : links)
        {
            const int ca = compMine[link.first.y * W + link.first.x];
            const int cb = compMine[link.second.y * W + link.second.x];
            if (ca < 0 || cb < 0)
                continue;
            if (ca == cb)
            {
                total++;
                continue;
            }
            const int na = nodeOf(compOf, cnt, ca);
            const int nb = nodeOf(compOf, cnt, cb);
            if (na >= 0 && nb >= 0 && find(parent, na) == find(parent, nb))
                total++;
        }
        return total;
    }

    static int find(int *parent, int a)
    {
        while (parent[a] != a)
        {
            parent[a] = parent[parent[a]];
            a = parent[a];
        }
        return a;
    }

    static void unite(int *parent, int a, int b)
    {
        a = find(parent, a);
        b = find(parent, b);
        if (a != b)
            parent[b] = a;
    }

    // Node standing for base component c, created on first sight. A dozen at
    // most ever exist, so a linear scan beats any index.
    static int intern(int *parent, int *compOf, int &cnt, int c)
    {
        for (int i = 0; i < cnt; i++)
            if (compOf[i] == c)
                return i;
        parent[cnt] = cnt;
        compOf[cnt] = c;
        return cnt++;
    }

    static int nodeOf(const int *compOf, int cnt, int c)
    {
        for (int i = 0; i < cnt; i++)
            if (compOf[i] == c)
                return i;
        return -1;
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
        // The first read blocks until the referee writes the turn, so it
        // returns about when the turn's clock started. Reading the board is
        // part of the budget, hence the mark here rather than after it.
        cin >> myScore;
        turnStart = std::chrono::steady_clock::now();
        cin >> foeScore;
        activeConnections.clear();
        gameMap.readTurnState(cin, activeConnections);
    }

    void gameTurn()
    {
        DBG_TURN_BEGIN(gameMap, wishes);

        ActionSet action;
        int disrupt = -1;
        planner.deadline =
            turnStart + std::chrono::milliseconds(PLAN_BUDGET_MS);
        planner.myScore = myScore;
        planner.foeScore = foeScore;
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
        fprintf(stderr,
                "turn : %lld us / %d ms  %d actionsets  %d rails (%d paint)  "
                "disrupt %d\n",
                us, PLAN_BUDGET_MS, planner.evaluated,
                (int)action.cells.size(), action.cost, disrupt);
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
