#define BOT_VERSION "5.3"

// A beam search over turns, built on v4's one-turn machinery, with both
// players played out on every turn of every line.
//
// A state is a board a few of our turns ahead. Each level of the beam plays,
// on every state it kept, the cross product of two prunings v4 already had:
//   - the affordable rail combinations drawn from the best RAIL_PRUNING_WIDTH
//     cells of that state's value map,
//   - the MAX_DISRUPT_CANDIDATES regions worth inking on it.
// Every resulting board is scored, the BEAM_WIDTH best are kept, and the next
// level grows from those until the turn's budget runs out. The move played is
// the first turn of the best line found.
//
// Nothing is copied to get there. The planner owns one working board -- owner,
// network, ink and instability as flat byte arrays -- and a move is applied to
// it and rolled back, so a level costs a few thousand byte writes rather than
// a board copy per state. Ranking a level sorts packed integers, never states.
//
// A cell's value is built in three layers, unchanged from v4:
//   1. a flat bonus if its region holds a town -- such a region can never be
//      inked, so a rail laid there is never erased;
//   2. W+H-length for every shortest town-to-town path crossing it, so the
//      short wishes (the ones a turn can actually finish) weigh most;
//   3. scaled down by how close its region is to being inked.
// Layers 1 and 2 depend only on where the ink is, so the map is built once and
// reused by every state that inked nothing new -- which is nearly all of them.
//
// What the beam buys over v4: a rail that pays only next turn. v4 scored the
// board one turn out, so a mountain that completes a link in two turns looked
// like three wasted paint. It also makes DISRUPT honest -- a disrupt raises
// instability by one and only inks at INK_INSTABILITY_THRESHOLD, where v4
// scored every disrupt as if it inked on the spot.
//
// The budget goes to width rather than depth (v5.2): a line is a chain of
// guesses about an opponent we predict about half the time, so the levels the
// beam can still believe deserve the states, and the far ones a tapering
// count of combinations.
//
// Both players are played out on every turn of a line. The opponent reads the
// board through the very same value map -- it is symmetric, built from the
// towns' wishes with no side in it -- so his turn is the head of our own
// ranking: the best cells his three paint can buy, and the region that erases
// most of ours for least of his. A turn resolves the way the referee resolves
// one: his rails, ours beside them, then the disrupts. A cell both players
// paint the same turn belongs to neither -- it carries the path and pays
// nobody.

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
#include <cstring>
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

// ---- the beam ----

// States carried from one level to the next. Wide on purpose since v5.2: once
// the opponent is played out too, a line is a chain of guesses about him, and
// the budget is worth more spread over the turns we are nearly sure of than
// sunk into the ones we invented. Measured against v5.0, wider and shallower
// wins monotonically: w24/d6 68%, w16/d8 60%, w12/d10 58%.
static const int BEAM_WIDTH = 24; // @cg-param
// Turns the beam may look ahead -- a hard ceiling now, not a safety net. Six
// turns of a 56%-accurate prediction is already 0.56^6 of credibility.
static const int MAX_BEAM_DEPTH = 6; // @cg-param

// Ranked cells a state draws its combinations from, and disrupt regions tried
// alongside them. The pool stays wide on purpose: the cells a turn ends up
// buying fall outside the best six often enough that narrowing it here costs
// far more than the depth it buys (measured: -45 points of winrate).
static const int RAIL_PRUNING_WIDTH = 24; // @cg-param
static const int MAX_DISRUPT_CANDIDATES = 3; // @cg-param

// Every set of at most three cells drawn from the candidates, plus the empty
// one: a turn may be worth a disrupt and nothing else.
static const int MAX_COMBOS =
    RAIL_PRUNING_WIDTH * (RAIL_PRUNING_WIDTH - 1) * (RAIL_PRUNING_WIDTH - 2) / 6 +
    RAIL_PRUNING_WIDTH * (RAIL_PRUNING_WIDTH - 1) / 2 + RAIL_PRUNING_WIDTH + 1;

// Combinations a state actually plays, richest first: this is the branching
// factor, and it is what the beam trades for depth. Forming all of them is a
// few thousand adds, but each one played costs a wish solve, so only the head
// of the ranking is kept -- widening the pool above and cutting here beats
// narrowing the pool, which throws the good cells away before they are tried.
//
// The turn we are about to play is exempt: the value sum a combination is
// ranked on does not predict the income it earns, so cutting the ranking short
// at the root loses moves the search would have found (measured: -16 points of
// winrate). Deeper levels pay that price, where a mistake is one line among
// BEAM_WIDTH rather than the move itself.
static const int COMBO_PRUNING_WIDTH = 64; // @cg-param

// How fast that width falls away with depth. A line is a guess about an
// opponent we only half predict, and the guess compounds: what we play out at
// depth 20 rests on him having played our prediction twenty times running.
// Measured: the budget spent that far out is not merely wasted, it contaminates
// the move we actually play. Each level therefore plays the one above's count
// divided by this, down to COMBO_FLOOR; 1 keeps v5.1's flat step.
static const int COMBO_DECAY = 2; // @cg-param
// Below this a level stops being a search and becomes a rollout, so the decay
// stops here rather than at one combination.
static const int COMBO_FLOOR = 32; // @cg-param

// Children a state hands the level per disrupt option. The ranking below a
// state is the same whichever disrupt rides along -- a disrupt that inks
// nothing shifts every key by one constant -- so past the width of the beam
// itself a child could never be kept, and writing it out is pure traffic.
static const int EMIT_PER_DISRUPT = 2 * BEAM_WIDTH;

// One point of income outweighs any reachable sum of cell values, so both live
// in a single integer key without ever mixing. Wider than v4's 10000 because
// the beam sums the cells of a whole line, not of one turn.
static const long long SCORE_WEIGHT = 1 << 24;
// Added to a key before it is packed for sorting, so the pack stays unsigned.
static const long long KEY_BIAS = 1LL << 40;

// Cells the opponent is played out as taking each turn, and the first level
// he is played out on at all (1 = the turn we are about to play, 2 = only the
// turns after it). His turn is a guess: it lands on about half the rails he
// really lays, so conceding him a cell the beam then avoids costs as much as
// it saves. These two bound how far that guess is allowed to reach.
static const int FOE_RAILS_PREDICTED = 3; // @cg-param
static const int FOE_PREDICT_FROM_DEPTH = 1; // @cg-param
// The first level his DISRUPT is played out on, same convention. A wrong
// guess here is cheaper than a wrong rail: unless it inks, it moves no rail
// at all, it only raises a region one step closer to going under.
static const int FOE_DISRUPT_FROM_DEPTH = 1; // @cg-param

// A disrupt only pays when it finally inks, which is often past the horizon.
// Crediting each hit with its share of what the region is worth gives the beam
// the gradient it needs to plan several of them; the credit is paid back on
// the hit that inks, where the income carries the real loss from then on.
// Scaled by INK_SCALE to read in the same units as the discounted cell values
// it is summed with.
static const int DISRUPT_CREDIT_SCALE = INK_SCALE; // @cg-param

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

// The beam and everything it needs: one value map per ink layout, one working
// board mutated by apply/rollback, and flat scratch buffers allocated once.
class Planner
{
public:
    int myId = 0, foeId = 1;
    // When the search must stop. Set by Game from the turn's parse time.
    std::chrono::steady_clock::time_point deadline;
    // Reported to stderr: boards scored this turn, and levels completed.
    int evaluated = 0;
    int depthReached = 0;
    // What each level of the beam cost and what it looked at, indexed by depth
    // (0 unused). A turn's shape is readable level by level rather than as one
    // total that hides where the budget went.
    struct LevelStats
    {
        int states;  // states expanded at this level
        int combos;  // combinations the pruning formed from the candidates
        int kept;    // of those, the ones the width let through
        int scored;  // boards weighed with the heuristic
        int astar;   // A* runs, i.e. value map rebuilds times wishes
        int solves;  // wishes re-solved exactly
        int sweeps;  // distance sweeps
    };
    LevelStats level[MAX_BEAM_DEPTH + 2];
    int rebuilds = 0, solves = 0, sweeps = 0, astarRuns = 0;

    // Totals the turn report prints under the per-level breakdown.
    int totalCombos() const
    {
        int sum = 0;
        for (int d = 1; d <= MAX_BEAM_DEPTH; d++)
            sum += level[d].combos;
        return sum;
    }

    // Everything a turn never changes: geometry, the neighbour table, terrain
    // costs, region cell lists, the uninkable bonus and the wishes resolved to
    // cell indices. Every buffer a turn uses is sized here and never again.
    void init(const Map &board, const vector<pair<int, int>> &wishes)
    {
        W = board.width();
        H = board.height();
        N = W * H;
        R = (int)board.stat->regions.size();

        // Neighbours once and for all, in NESW order: every sweep below then
        // streams a flat list instead of testing four bounds per cell, and the
        // referee's tie-break is built into the order.
        nbrAt.assign(N + 1, 0);
        nbrList.clear();
        nbrList.reserve(4 * N);
        for (int y = 0, idx = 0; y < H; y++)
            for (int x = 0; x < W; x++, idx++)
            {
                nbrAt[idx] = (int)nbrList.size();
                for (int k = 0; k < 4; k++)
                {
                    const int nx = x + DIR_X[k], ny = y + DIR_Y[k];
                    if (nx >= 0 && nx < W && ny >= 0 && ny < H)
                        nbrList.push_back((int16_t)(ny * W + nx));
                }
            }
        nbrAt[N] = (int)nbrList.size();

        cellSlot.assign(N, -1);
        cellCost.assign(N, 0);
        cellTown.assign(N, 0);
        baseValue.assign(N, 0);
        // A rail in a town region can never be erased, so it is worth holding
        // on its own. A quarter of a typical path reward: enough to break a
        // tie between two cells on the same path, not enough to outrank one.
        const int uninkable = (W + H) / 4;
        for (int y = 0, idx = 0; y < H; y++)
            for (int x = 0; x < W; x++, idx++)
            {
                const int slot = board.regionSlotAt(x, y);
                const int cost = terrainCost(board.tileType(x, y));
                cellSlot[idx] = (int16_t)slot;
                // 0 stands for impassable, so one test covers both.
                cellCost[idx] = (int8_t)(cost == INT_MAX ? 0 : cost);
                cellTown[idx] = (uint8_t)board.stat->townCellFlag[idx];
                if (slot >= 0 && board.stat->regionHasTown[slot])
                    baseValue[idx] = uninkable;
            }

        regionTown.assign(R, 0);
        for (int s = 0; s < R; s++)
            regionTown[s] = (uint8_t)board.stat->regionHasTown[s];

        // Region cell lists, flattened: inking one walks its cells to erase
        // the rails, and that is the only place a region is read whole.
        regionAt.assign(R + 1, 0);
        for (int idx = 0; idx < N; idx++)
            if (cellSlot[idx] >= 0)
                regionAt[cellSlot[idx] + 1]++;
        for (int s = 0; s < R; s++)
            regionAt[s + 1] += regionAt[s];
        regionCells.assign(N, 0);
        vector<int> fill(regionAt.begin(), regionAt.end() - 1);
        for (int idx = 0; idx < N; idx++)
            if (cellSlot[idx] >= 0)
                regionCells[fill[cellSlot[idx]]++] = idx;

        // Kept in the referee's own orientation: readTowns emits (declaring
        // town, wanted town), and A* breaks its ties NESW walking outwards
        // from the source, so swapping the two picks a different corridor
        // wherever several shortest paths exist.
        linkSrc.clear();
        linkDst.clear();
        set<pair<int, int>> seen;
        for (const auto &wish : wishes)
        {
            const int from = wish.first, to = wish.second;
            if (from == to || !seen.insert({min(from, to), max(from, to)}).second)
                continue;
            if (!board.hasTown(from) || !board.hasTown(to))
                continue;
            const Coord a = board.townCoordOf(from), b = board.townCoordOf(to);
            linkSrc.push_back(a.y * W + a.x);
            linkDst.push_back(b.y * W + b.x);
        }
        nLinks = (int)linkSrc.size();

        value.assign(N, 0);
        spread.assign(N, 0);
        disruptValue.assign(N, 0);
        wasZero.assign(N, 0);
        owner.assign(N, NO_OWNER);
        net.assign(N, 0);
        cellInked.assign(N, 0);
        regionInst.assign(R, 0);
        rootInst.assign(R, 0);
        regionInked.assign(R, 0);
        regionScore.assign(R, 0);
        inkWords.assign((R + 63) / 64, 0);
        valueInk.assign((R + 63) / 64, 0);
        gScore.assign(N, 0);
        gStamp.assign(N, 0);
        parent.assign(N, -1);
        heap.reserve(N);
        bfsSeen.assign(N, 0);
        bfsParent.assign(N, -1);
        bfsQueue.reserve(N);
        distSrc.assign((size_t)max(nLinks, 1) * N, 0);
        distDst.assign((size_t)max(nLinks, 1) * N, 0);
        wishMine.assign(max(nLinks, 1), 0);
        wishFoe.assign(max(nLinks, 1), 0);
        wishLen.assign(max(nLinks, 1), 0);
        candKey.reserve(N);
        rankKey.reserve(R);
        railUndo.reserve(4 * MAX_BEAM_DEPTH);
        inkUndo.reserve(N);
        frames.reserve(2 * MAX_BEAM_DEPTH + 4);
        pool.reserve((size_t)BEAM_WIDTH * MAX_BEAM_DEPTH + 1);
        kids.reserve((size_t)BEAM_WIDTH * EMIT_PER_DISRUPT *
                     (MAX_DISRUPT_CANDIDATES + 1));
        kidKey.reserve(kids.capacity());
        beam.reserve(BEAM_WIDTH);

        // Zobrist keys: rails laid in a different order, or disrupts spread
        // over different turns, reach the same board, and the beam is worth
        // more when it holds that board once.
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        zCell.resize(N);
        for (int i = 0; i < N; i++)
            zCell[i] = splitmix(seed);
        zRegion.resize((size_t)R * (INK_INSTABILITY_THRESHOLD + 1));
        for (size_t i = 0; i < zRegion.size(); i++)
            zRegion[i] = splitmix(seed);
    }

    // The turn's move: where to lay rail, and which region to DISRUPT (-1 for
    // none) -- the first turn of the best line the beam found.
    void plan(const Map &board, ActionSet &outAction, int &outDisrupt)
    {
        boardRef = &board;
        beginTurn(board);
        runBeam();
        emitMove(outAction, outDisrupt);
        // After the search: it may have spread the map to find candidates,
        // and it is that final map the move was read off.
        DBG_VALUES(viewerValues());
    }

private:
    // ---- fixed for the game ----
    int W = 0, H = 0, N = 0, R = 0, nLinks = 0;
    vector<int> nbrAt;
    vector<int16_t> nbrList;
    vector<int16_t> cellSlot;
    vector<int8_t> cellCost;
    vector<uint8_t> cellTown, regionTown;
    vector<int> regionAt, regionCells;
    vector<int> baseValue;
    vector<int> linkSrc, linkDst;
    vector<uint64_t> zCell, zRegion;
    const Map *boardRef = nullptr;

    // ---- the working board: the only state apply/rollback touches ----
    // Byte per cell rather than the referee's tiles: the sweeps below stream
    // three of these arrays at once, and all three fit in L1 together.
    vector<int8_t> owner;
    // 1 where a connection may walk: a town, or a live rail.
    vector<uint8_t> net;
    vector<uint8_t> cellInked;
    vector<uint8_t> regionInst, regionInked;
    // Instability the turn started at, so a state knows what it added.
    vector<uint8_t> rootInst;
    // Inked regions as a bitmask: the value map's cache key.
    vector<uint64_t> inkWords;

    // ---- rollback ----
    // A frame per move applied, and the cells it changed. A rail always lands
    // on an empty cell, so undoing one needs no more than its index; an ink
    // erases whatever sat in the region, so it keeps the owners it wiped.
    struct Frame
    {
        int32_t railBase, inkBase;
        int16_t slot;
        uint8_t inked;
    };
    vector<Frame> frames;
    vector<int32_t> railUndo;
    vector<int32_t> inkUndo; // cell << 4 | (owner + 2)

    // ---- the value map, cached per ink layout ----
    vector<int> value, spread, disruptValue;
    vector<uint8_t> wasZero;
    vector<uint64_t> valueInk;
    bool valueValid = false;

    // A* scratch. gScore is stamped rather than cleared, so a run touches only
    // the cells it visits.
    vector<int> gScore, parent, gStamp;
    vector<uint64_t> heap;
    int stamp = 0;

    // ---- pruning scratch ----
    vector<int> regionScore;
    vector<uint64_t> rankKey, candKey;
    vector<int> candCell, candCost, candValue;
    // The opponent's turn as we play it out: the best cells of the very same
    // ranking, taken in order while his three paint last.
    int foeRail[PAINT_PER_TURN], foeRailValue[PAINT_PER_TURN];
    int nFoeRails = 0, foeBought = 0;
    int foeDisruptSlot = -1;
    int disruptCand[MAX_DISRUPT_CANDIDATES];
    int nDisrupt = 0;
    // A combination is a packed (count, candidate slots) word; the sort moves
    // its key alone, never the combination.
    uint32_t comboPack[MAX_COMBOS];
    uint64_t comboKey[MAX_COMBOS];
    int nCombos = 0, nPlayed = 0;

    // ---- income scratch ----
    // A FIFO sweep stamped rather than cleared, so re-solving a wish touches
    // only the cells it reaches.
    vector<int> bfsSeen, bfsParent, bfsQueue;
    int bfsStamp = 0;
    // Per-wish payout on the board being evaluated, the length of the walk it
    // pays for, and the distance of every cell to each of its two towns. Those
    // distances are what tells a combination apart from the wishes it cannot
    // touch, without solving a single one of them.
    long long baseMine = 0, baseFoe = 0;
    vector<int> wishMine, wishFoe, wishLen;
    vector<uint16_t> distSrc, distDst;
    static const int UNREACHED = 0xFFFF;
    // The new rails paired up: every walk through them enters at one and
    // leaves at another, so a wish's two distance arrays bound it from below.
    int pairFrom[PAINT_PER_TURN * PAINT_PER_TURN];
    int pairTo[PAINT_PER_TURN * PAINT_PER_TURN];
    int pairGap[PAINT_PER_TURN * PAINT_PER_TURN];
    int nPairs = 0;

    // What the best combinations played on a state's board paid. Only the head
    // of the ranking can ever survive the level's own cut, so a state keeps
    // that many and no more -- sized by EMIT_PER_DISRUPT rather than by the
    // thousands of combinations 24 cells can form, which is the difference
    // between a buffer that streams through L2 and one that sits in L1.
    static const int PLAY_KEPT = EMIT_PER_DISRUPT;
    int playMine[PLAY_KEPT], playFoe[PLAY_KEPT], playValue[PLAY_KEPT];
    uint64_t playHash[PLAY_KEPT], playKey[PLAY_KEPT];
    uint16_t playCells[PLAY_KEPT][PAINT_PER_TURN];
    uint8_t playN[PLAY_KEPT];
    int playedCount = 0;
    // Rank of the weakest kept entry, so a new one is compared before it is
    // written rather than after everything has been stored and sorted.
    int playWorst = 0;

    // ---- the beam ----

    // One state: the move that made it, what the line earned so far, and the
    // key its level is ranked on. Nothing here is ever sorted -- the ranking
    // sorts packed integers that point back at these.
    struct State
    {
        int32_t parent;   // index in the pool, -1 at the root
        int32_t root;     // the depth-1 ancestor: the move we would play
        uint64_t hash;    // rails laid and regions raised, for duplicates
        int64_t key;      // income difference, cells bought, disrupt credit
        int32_t gainMine, gainFoe; // income banked since the turn's board
        int32_t valueSum; // cells bought over the line, discounted
        int32_t phi;      // credit for the disrupts the line has landed
        uint16_t cells[PAINT_PER_TURN];
        uint16_t foe[PAINT_PER_TURN]; // what the opponent painted that turn
        uint8_t n, foeN;
        int16_t slot;    // region we disrupted, -1 for none
        int16_t foeSlot; // region he disrupted that turn, -1 for none
    };

    vector<State> pool;  // kept states, every level appended
    vector<State> kids;  // the level being grown
    vector<uint64_t> kidKey;
    vector<int> beam;    // pool indices of the level in hand
    // Frames one replayed turn pushes: his rails, ours, his disrupt, ours.
    static const int FRAMES_PER_TURN = 4;
    int pathStack[MAX_BEAM_DEPTH + 1];
    int pathLen = 0;
    int curDepth = 0;
    int bestRootIdx = -1;

    // The board as the referee left it, loaded into the flat arrays the search
    // works on. Everything the beam changes afterwards is rolled back, so this
    // runs once a turn.
    void beginTurn(const Map &board)
    {
        fill(inkWords.begin(), inkWords.end(), 0);
        for (int s = 0; s < R; s++)
        {
            regionInst[s] = (uint8_t)min((int)board.regionInstability[s], 15);
            rootInst[s] = regionInst[s];
            regionInked[s] = board.regionInkedFlag[s] ? 1 : 0;
            if (regionInked[s])
                inkWords[s >> 6] |= (uint64_t)1 << (s & 63);
        }
        for (int idx = 0; idx < N; idx++)
        {
            const int slot = cellSlot[idx];
            const bool inked = board.grid.tiles[idx].inked ||
                               (slot >= 0 && regionInked[slot]);
            const int o = inked ? NO_OWNER : board.grid.tiles[idx].tracksOwner;
            cellInked[idx] = inked ? 1 : 0;
            owner[idx] = (int8_t)o;
            net[idx] = (cellTown[idx] || o != NO_OWNER) ? 1 : 0;
        }
        valueValid = false;
        frames.clear();
        railUndo.clear();
        inkUndo.clear();
        evaluated = 0;
        depthReached = 0;
        rebuilds = 0;
        solves = 0;
        sweeps = 0;
        astarRuns = 0;
        memset(level, 0, sizeof(level));
        pathLen = 0;
    }

    // One level at a time: every state in hand is grown, the best BEAM_WIDTH
    // children become the next level, and the clock ends it. A level that was
    // cut short is still ranked -- its states are all one turn deep, so they
    // compare, and the best parents were grown first.
    void runBeam()
    {
        pool.clear();
        State root;
        memset(&root, 0, sizeof(root));
        root.parent = -1;
        root.root = -1;
        root.slot = -1;
        root.foeSlot = -1;
        root.foeN = 0;
        pool.push_back(root);
        beam.assign(1, 0);
        bestRootIdx = -1;

        for (int depth = 1; depth <= MAX_BEAM_DEPTH; depth++)
        {
            kids.clear();
            curDepth = depth;
            bool alive = true;
            for (size_t b = 0; b < beam.size() && alive; b++)
            {
                level[depth].states++;
                alive = expandState(beam[b]);
            }
            if (kids.empty())
                break;
            selectBeam();
            if (beam.empty())
                break;
            bestRootIdx = pool[beam[0]].root;
            depthReached = depth;
            if (!alive)
                break;
        }
    }

    // Grows one state: its board is replayed, the turn's candidates ranked on
    // it, and every combination played against every disrupt. Returns false
    // when the clock ran out, which ends the level where it stands.
    bool expandState(int nodeIdx)
    {
        // Laying a state out costs as much as a few dozen boards scored.
        if (evaluated > 0 && outOfBudget())
            return false;
        applyPath(nodeIdx);
        syncValueMap();
        rankDisrupts();
        buildCandidates();
        // Nothing valued in reach: bleed the map one ring outwards and look
        // again, rather than waste the turn. Late games are almost entirely
        // made of these turns, the shortest paths being long since built.
        while (candCell.empty() && diffuseRing())
            buildCandidates();
        buildCombos();
        // His turn is settled before ours is built, so our combinations are
        // free to contest the very cells he is taking -- the board then makes
        // those neutral, which is how denying him is paid for.
        // His turn in the order the referee resolves one: rails first, ours
        // beside them, then the disrupts. His ink can therefore erase a rail
        // he just laid, exactly as ours can.
        pickFoeRails();
        pushRails(foeRail, nFoeRails, foeId);
        pickFoeDisrupt();
        pushDisrupt(foeDisruptSlot);
        if (nodeIdx == 0)
            reportRootDebug();

        // Every combination on the state's own board. A disrupt that inks
        // nothing leaves that board -- and so every payout -- untouched, so
        // its children are these same numbers plus a credit, and no wish is
        // solved for them at all. They are taken first, while the payouts are
        // still the ones they need.
        baselineIncome();
        bool alive = playCombos(nodeIdx, -1, 0, 0);
        // Only the best-ranked of them is worth a child: the others leave the
        // same board behind and earn a smaller credit, so whatever the rails,
        // a state holding one is beaten by the same state holding this one.
        // Regions are ranked by that credit, so the first is it.
        for (int d = 0; d < nDisrupt; d++)
        {
            const int slot = disruptCand[d];
            if (regionInst[slot] + 1 >= INK_INSTABILITY_THRESHOLD)
                continue;
            emitPlayed(nodeIdx, slot, disruptHash(slot), creditStep(slot));
            break;
        }

        // The disrupts that ink: the region's rails go, so the board really
        // does change and every combination is played again on it. The credit
        // the line earned for raising the region is paid back here -- from now
        // on the income carries the loss for real.
        for (int d = 0; d < nDisrupt && alive; d++)
        {
            const int slot = disruptCand[d];
            if (regionInst[slot] + 1 < INK_INSTABILITY_THRESHOLD)
                continue;
            const int credit =
                -creditStep(slot) * (regionInst[slot] - rootInst[slot]);
            const uint64_t slotHash = disruptHash(slot);
            pushDisrupt(slot);
            baselineIncome();
            alive = playCombos(nodeIdx, slot, slotHash, credit);
            popFrame();
        }

        popFrame(); // his disrupt
        popFrame(); // his rails
        undoPath();
        return alive;
    }

    // Plays every combination on the board as it stands, keeping what each
    // paid, then emits the best of them as children. Only the head can ever
    // survive the level's ranking, so the tail is never written out.
    bool playCombos(int nodeIdx, int slot, uint64_t slotHash, int credit)
    {
        int cells[PAINT_PER_TURN];
        playedCount = 0;
        playWorst = 0;
        int scored = 0;
        for (int c = 0; c < nPlayed; c++)
        {
            // Read every single state: one of them can cost a solve per wish,
            // so any stride at all overshoots the budget by far more than the
            // clock reads it saves.
            if (evaluated > 0 && outOfBudget())
                break;

            const uint32_t pack = comboPack[0xFFFFFF - (comboKey[c] & 0xFFFFFF)];
            const int n = (int)(pack & 0xFF);
            int used = 0, bought = 0, denied = 0;
            uint64_t railHash = 0;
            for (int i = 0; i < n; i++)
            {
                const int cand = (int)((pack >> (8 * (i + 1))) & 0xFF);
                const int idx = candCell[cand];
                const bool clash = foeTakesThisTurn(idx) && owner[idx] == foeId;
                // A rail in the region this disrupt just inked buys nothing.
                if (!clash && !canPlace(idx))
                    continue;
                cells[used++] = idx;
                // Ground held is a difference, like the income above it: a
                // cell we share holds nothing for either of us, so taking one
                // he wanted is worth what it stops him holding.
                if (clash)
                    denied += foeValueOf(idx);
                else
                    bought += candValue[cand];
                railHash ^= zCell[idx];
            }

            pushRails(cells, used, myId);
            int gainMine = 0, gainFoe = 0;
            collectIncome(cells, used, gainMine, gainFoe);
            popFrame();
            evaluated++;
            level[curDepth].scored++;

            bought -= foeBought - denied;
            // What the level will rank this child on, bar the constants its
            // parent and its disrupt add: enough to order them here. The tie
            // goes to the combination played first, which is the richer one,
            // so the order stays the one buildCombos laid down.
            const long long key =
                (long long)(gainMine - gainFoe) * SCORE_WEIGHT + bought;
            // key | scoring order | slot, in 41 + 15 + 8 bits. The order
            // breaks ties in favour of the richer combination, and the slot
            // rides along because sorting moves a key away from the entry it
            // describes. Only the first 2^15 scored can be told apart by the
            // tie-break, which is well past what any level plays.
            const uint64_t rank = (uint64_t)min(scored, 0x7FFF);
            const uint64_t ranked =
                ((uint64_t)(key + KEY_BIAS) << 15) | (0x7FFF - rank);
            scored++;
            if (playedCount == PLAY_KEPT &&
                ranked <= (playKey[playWorst] >> 8))
                continue; // weaker than everything kept: nothing to write

            const int at = playedCount < PLAY_KEPT ? playedCount++ : playWorst;
            playKey[at] = (ranked << 8) | (uint64_t)at;
            playMine[at] = gainMine;
            playFoe[at] = gainFoe;
            playValue[at] = bought;
            playHash[at] = railHash;
            playN[at] = (uint8_t)used;
            for (int i = 0; i < used; i++)
                playCells[at][i] = (uint16_t)cells[i];
            // The new weakest, found over a handful of entries that are all
            // in L1 by now.
            playWorst = 0;
            for (int i = 1; i < playedCount; i++)
                if ((playKey[i] >> 8) < (playKey[playWorst] >> 8))
                    playWorst = i;
        }

        sort(playKey, playKey + playedCount, greater<uint64_t>());
        emitPlayed(nodeIdx, slot, slotHash, credit);
        return scored == nPlayed;
    }

    // The kept combinations under one disrupt: for a disrupt that inks
    // nothing this is the same board and the same payouts, one credit apart.
    void emitPlayed(int nodeIdx, int slot, uint64_t slotHash, int credit)
    {
        int cells[PAINT_PER_TURN];
        for (int i = 0; i < playedCount; i++)
        {
            const int c = (int)(playKey[i] & 0xFF);
            for (int j = 0; j < playN[c]; j++)
                cells[j] = playCells[c][j];
            emitChild(nodeIdx, cells, playN[c], playValue[c], playHash[c], slot,
                      slotHash, credit, playMine[c], playFoe[c]);
        }
    }

    // A disrupt earns its share of what the region is worth to ink, so a line
    // that keeps hitting one is graded instead of waiting for the fourth hit.
    int creditStep(int slot) const
    {
        return regionScore[slot] * DISRUPT_CREDIT_SCALE /
               INK_INSTABILITY_THRESHOLD;
    }

    uint64_t disruptHash(int slot) const
    {
        return zRegion[(size_t)slot * (INK_INSTABILITY_THRESHOLD + 1) +
                       min(regionInst[slot] + 1, INK_INSTABILITY_THRESHOLD)];
    }

    // A state's worth: the income the line has earned over the opponent's,
    // then the cells it bought and the disrupts it landed. The first term
    // never mixes with the others -- no sum of cell values comes near a point.
    void emitChild(int nodeIdx, const int *cells, int n, int bought,
                   uint64_t railHash, int slot, uint64_t slotHash, int credit,
                   int gainMine, int gainFoe)
    {
        const State &from = pool[nodeIdx];
        State k;
        k.parent = nodeIdx;
        // The root's own children are the moves we may play, so each is the
        // root of its line; deeper ones inherit the one they came from.
        k.root = (from.parent < 0) ? -1 : from.root;
        k.gainMine = from.gainMine + gainMine;
        k.gainFoe = from.gainFoe + gainFoe;
        k.valueSum = from.valueSum + bought;
        k.phi = from.phi + credit;
        k.hash = from.hash ^ railHash ^ (slot >= 0 ? slotHash : 0);
        k.slot = (int16_t)slot;
        k.n = (uint8_t)n;
        for (int i = 0; i < n; i++)
            k.cells[i] = (uint16_t)cells[i];
        for (int i = n; i < PAINT_PER_TURN; i++)
            k.cells[i] = 0;
        k.foeSlot = (int16_t)foeDisruptSlot;
        k.foeN = (uint8_t)nFoeRails;
        for (int i = 0; i < PAINT_PER_TURN; i++)
            k.foe[i] = (uint16_t)(i < nFoeRails ? foeRail[i] : 0);
        k.key = (long long)(k.gainMine - k.gainFoe) * SCORE_WEIGHT +
                k.valueSum + k.phi;
        kids.push_back(k);
    }

    // The level's best, duplicates dropped. Only packed (key, index) integers
    // are sorted; a state is written once, when it is kept.
    void selectBeam()
    {
        kidKey.clear();
        for (size_t i = 0; i < kids.size(); i++)
            kidKey.push_back(((uint64_t)(kids[i].key + KEY_BIAS) << 20) |
                             (uint64_t)(0xFFFFF - i));
        // Four times the width is looked at, which leaves room for the
        // duplicates dropped below without ordering the whole level.
        const size_t look = min(kids.size(), (size_t)BEAM_WIDTH * 4);
        partial_sort(kidKey.begin(), kidKey.begin() + look, kidKey.end(),
                     greater<uint64_t>());

        beam.clear();
        for (size_t i = 0; i < look && (int)beam.size() < BEAM_WIDTH; i++)
        {
            const State &k = kids[0xFFFFF - (kidKey[i] & 0xFFFFF)];
            bool twin = false;
            for (size_t b = 0; b < beam.size() && !twin; b++)
                twin = pool[beam[b]].hash == k.hash;
            if (twin)
                continue;
            pool.push_back(k);
            const int at = (int)pool.size() - 1;
            if (pool[at].root < 0)
                pool[at].root = at;
            beam.push_back(at);
        }
    }

    // The first turn of the best line, written out as the turn's move.
    void emitMove(ActionSet &out, int &outDisrupt)
    {
        out.cells.clear();
        out.cost = 0;
        outDisrupt = -1;
        if (bestRootIdx < 0)
            return;
        const State &s = pool[bestRootIdx];
        for (int i = 0; i < s.n; i++)
        {
            const int idx = s.cells[i];
            out.cells.push_back(Coord(idx % W, idx / W));
            out.cost += cellCost[idx];
        }
        if (s.slot >= 0)
            outDisrupt = boardRef->stat->regions[s.slot].id;
    }

    // The one place the turn's deadline is read. The budget is a hard limit:
    // the search stops to meet it, it is never widened to fit the search.
    bool outOfBudget() const
    {
        return std::chrono::steady_clock::now() >= deadline;
    }

    // ---- applying and rolling back a move ----

    // Replays a state's line onto the working board, oldest move first.
    void applyPath(int nodeIdx)
    {
        pathLen = 0;
        for (int i = nodeIdx; i > 0; i = pool[i].parent)
            pathStack[pathLen++] = i;
        int cells[PAINT_PER_TURN];
        for (int i = pathLen; i-- > 0;)
        {
            const State &s = pool[pathStack[i]];
            // His whole turn first, then ours, in the order the expansion
            // played them out -- so a cell they shared comes back neutral and
            // a region either of them sank comes back inked, exactly as it was.
            nFoeRails = s.foeN;
            for (int j = 0; j < s.foeN; j++)
                foeRail[j] = s.foe[j];
            pushRails(foeRail, s.foeN, foeId);
            for (int j = 0; j < s.n; j++)
                cells[j] = s.cells[j];
            pushRails(cells, s.n, myId);
            pushDisrupt(s.foeSlot);
            pushDisrupt(s.slot);
        }
    }

    void undoPath()
    {
        for (int i = FRAMES_PER_TURN * pathLen; i-- > 0;)
            popFrame();
        pathLen = 0;
    }

    // A rail needs an empty, non-town, passable cell outside the ink.
    bool canPlace(int idx) const
    {
        return !cellTown[idx] && owner[idx] == NO_OWNER && !cellInked[idx] &&
               cellCost[idx] > 0;
    }

    // Lays one side's rails. Both players paint at the same time, so a cell
    // the opponent is taking this turn goes neutral rather than to us: the
    // rail carries the path and pays neither of us.
    void pushRails(const int *cells, int n, int side)
    {
        Frame f;
        f.railBase = (int32_t)railUndo.size();
        f.inkBase = (int32_t)inkUndo.size();
        f.slot = -1;
        f.inked = 0;
        for (int i = 0; i < n; i++)
        {
            const int idx = cells[i];
            const int8_t was = owner[idx];
            if (side == myId && was == foeId && foeTakesThisTurn(idx))
                owner[idx] = (int8_t)NEUTRAL_OWNER;
            else if (canPlace(idx))
                owner[idx] = (int8_t)side;
            else
                continue; // the ink took it, or a rail was already there
            net[idx] = 1;
            railUndo.push_back((idx << 4) | (was + 2));
        }
        frames.push_back(f);
    }

    // The cells the opponent is painting on the turn being played out. At most
    // three, so a scan beats any index.
    bool foeTakesThisTurn(int idx) const
    {
        for (int i = 0; i < nFoeRails; i++)
            if (foeRail[i] == idx)
                return true;
        return false;
    }

    // Raises a region's instability by one, and inks it when that reaches the
    // threshold -- which is when its rails are erased, both players' alike.
    void pushDisrupt(int slot)
    {
        Frame f;
        f.railBase = (int32_t)railUndo.size();
        f.inkBase = (int32_t)inkUndo.size();
        f.slot = (int16_t)slot;
        f.inked = 0;
        if (slot >= 0)
        {
            regionInst[slot]++;
            if (regionInst[slot] >= INK_INSTABILITY_THRESHOLD &&
                !regionInked[slot])
            {
                f.inked = 1;
                inkRegion(slot);
            }
        }
        frames.push_back(f);
    }

    void inkRegion(int slot)
    {
        regionInked[slot] = 1;
        inkWords[slot >> 6] |= (uint64_t)1 << (slot & 63);
        for (int p = regionAt[slot], e = regionAt[slot + 1]; p < e; p++)
        {
            const int idx = regionCells[p];
            inkUndo.push_back((idx << 4) | (owner[idx] + 2));
            owner[idx] = NO_OWNER;
            cellInked[idx] = 1;
            // A region holding a town can never be inked, so no town is lost.
            net[idx] = 0;
        }
    }

    void popFrame()
    {
        const Frame f = frames.back();
        frames.pop_back();
        for (int i = (int)railUndo.size(); i-- > f.railBase;)
        {
            const int packed = railUndo[i];
            const int idx = packed >> 4;
            const int8_t was = (int8_t)((packed & 15) - 2);
            owner[idx] = was;
            net[idx] = (cellTown[idx] || was != NO_OWNER) ? 1 : 0;
        }
        railUndo.resize(f.railBase);
        if (f.inked)
        {
            regionInked[f.slot] = 0;
            inkWords[f.slot >> 6] &= ~((uint64_t)1 << (f.slot & 63));
            for (int i = (int)inkUndo.size(); i-- > f.inkBase;)
            {
                const int packed = inkUndo[i];
                const int idx = packed >> 4;
                const int8_t o = (int8_t)((packed & 15) - 2);
                owner[idx] = o;
                cellInked[idx] = 0;
                net[idx] = (cellTown[idx] || o != NO_OWNER) ? 1 : 0;
            }
            inkUndo.resize(f.inkBase);
        }
        if (f.slot >= 0)
            regionInst[f.slot]--;
    }

    // ---- layers 1 and 2: the value map ----

    // Layers 1 and 2 read the terrain and the ink, never the rails, so the map
    // only has to be rebuilt when a state inks a region the cached one did
    // not -- which no line does until its fourth disrupt lands.
    void syncValueMap()
    {
        if (valueValid && inkWords == valueInk)
            return;
        valueInk = inkWords;
        valueValid = true;
        rebuilds++;
        level[curDepth].astar += nLinks;
        astarRuns += nLinks;

        value = baseValue; // same size, so a plain copy of the bytes
        for (int w = 0; w < nLinks; w++)
            addPathReward(linkSrc[w], linkDst[w]);
        // The cells the paths and the town bonus left untouched. Pinned here,
        // once, so every later diffusion ring writes only into this set: a
        // corridor cell keeps the value its path gave it whatever the spread
        // does, and the ink discount cannot be applied twice to one cell.
        for (int idx = 0; idx < N; idx++)
            wasZero[idx] = (value[idx] == 0) ? 1 : 0;

        // The disrupt ranking reads its own diffused copy. A corridor is one
        // cell wide and arbitrary among many equal-length paths, so the
        // opponent builds beside it rather than on it and reads as worthless
        // on the raw map -- one ring outwards is what makes his rails count.
        // The copy is what keeps it off the placement ranking: bleeding the
        // real map lifts a mountain next to a corridor over a plain further
        // along it, and the turn buys one rail where it could buy three.
        disruptValue = value;
        diffuseInto(disruptValue);
    }

    // Shortest terrain path between two towns, ink impassable, every cell of
    // it rewarded W+H-cost -- so a short connection, the kind a turn can
    // actually finish, weighs more than a long one. The path is never stored:
    // the parent chain is walked straight back from the destination.
    void addPathReward(int srcIdx, int dstIdx)
    {
        if (srcIdx == dstIdx)
            return;
        const int dx = dstIdx % W, dy = dstIdx / W;

        stamp++;
        heap.clear();
        gScore[srcIdx] = 0;
        gStamp[srcIdx] = stamp;
        parent[srcIdx] = -1;
        heap.push_back(packHeap(heuristic(srcIdx, dx, dy), srcIdx));

        int total = -1;
        while (!heap.empty())
        {
            pop_heap(heap.begin(), heap.end(), greater<uint64_t>());
            const uint64_t top = heap.back();
            heap.pop_back();

            const int cur = (int)(top & 0xFFFFFFFFu);
            const int g = gScore[cur];
            // Stale entry: a shorter path to `cur` was found after this push.
            if ((int)(top >> 32) != g + heuristic(cur, dx, dy))
                continue;
            if (cur == dstIdx)
            {
                total = g;
                break;
            }

            for (int p = nbrAt[cur], e = nbrAt[cur + 1]; p < e; p++)
            {
                const int nIdx = nbrList[p];
                // Ink is impassable: a path through it could never be built.
                if (cellInked[nIdx])
                    continue;
                const int step = cellCost[nIdx];
                if (step == 0)
                    continue;
                const int ng = g + step;
                if (gStamp[nIdx] == stamp && ng >= gScore[nIdx])
                    continue;
                gScore[nIdx] = ng;
                gStamp[nIdx] = stamp;
                parent[nIdx] = cur;
                heap.push_back(packHeap(ng + heuristic(nIdx, dx, dy), nIdx));
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

    int heuristic(int idx, int dx, int dy) const
    {
        return abs(idx % W - dx) + abs(idx / W - dy);
    }

    // (f, cell) in one word, so the heap compares a single 64-bit integer.
    static uint64_t packHeap(int f, int idx)
    {
        return ((uint64_t)(uint32_t)f << 32) | (uint32_t)idx;
    }

    // Spreads a map one ring outwards: every cell still at zero takes half of
    // each valued neighbour, summed. Reads a snapshot and writes into the
    // target, so a cell filled by this pass cannot feed the next one within
    // it: one call is one ring. Returns false when nothing moved.
    bool diffuseInto(vector<int> &target)
    {
        spread = target;
        bool changed = false;
        for (int idx = 0; idx < N; idx++)
        {
            // Only ever a cell the value map itself left at zero, and only
            // while it is still empty: a corridor is never overwritten, and a
            // ring already laid is never raised by the next one.
            if (!wasZero[idx] || spread[idx] != 0)
                continue;
            // Ink and impassable ground can never carry a rail, so value must
            // not flow through them either.
            if (cellInked[idx] || cellCost[idx] == 0)
                continue;
            int sum = 0;
            for (int p = nbrAt[idx], e = nbrAt[idx + 1]; p < e; p++)
                sum += spread[nbrList[p]] / 2;
            if (sum > 0)
            {
                target[idx] = sum;
                changed = true;
            }
        }
        return changed;
    }

    // Rings laid on the placement map stay: they only fill cells the paths
    // left empty, so a state deeper in the line reads the ring an earlier one
    // needed instead of laying it again.
    bool diffuseRing() { return diffuseInto(value); }

    // ---- the disrupt candidates ----

    // Inking a region erases every rail in it, so a region is worth hitting by
    // what the opponent holds there minus what we do, each rail counted at the
    // value of the cell it sits on. Only ranks: the beam decides.
    void rankDisrupts()
    {
        fill(regionScore.begin(), regionScore.end(), 0);
        // One streaming pass: owners, values and slots are all read in order,
        // and the scatter lands in an array of a few dozen entries.
        for (int idx = 0; idx < N; idx++)
        {
            const int o = owner[idx];
            if (o == NO_OWNER)
                continue;
            const int slot = cellSlot[idx];
            if (slot < 0)
                continue;
            if (o == foeId)
                regionScore[slot] += disruptValue[idx];
            else if (o == myId)
                regionScore[slot] -= disruptValue[idx];
        }

        rankKey.clear();
        for (int s = 0; s < R; s++)
        {
            // A town's region can never be inked, and an inked one is done.
            if (regionInked[s] || regionTown[s])
                continue;
            if (regionScore[s] > 0)
                rankKey.push_back(((uint64_t)regionScore[s] << 16) |
                                  (uint64_t)s);
        }
        const size_t keep = min(rankKey.size(), (size_t)MAX_DISRUPT_CANDIDATES);
        partial_sort(rankKey.begin(), rankKey.begin() + keep, rankKey.end(),
                     greater<uint64_t>());
        nDisrupt = (int)keep;
        for (size_t i = 0; i < keep; i++)
            disruptCand[i] = (int)(rankKey[i] & 0xFFFF);
    }

    // ---- the rail candidates ----

    // The cells worth considering, ranked by value, then contact with the
    // network, then cheap terrain, then scan order. Only the head is kept --
    // the tail could never enter a best combination.
    void buildCandidates()
    {
        candKey.clear();
        for (int idx = 0; idx < N; idx++)
        {
            // Cheapest filter first, and it streams the value array.
            if (value[idx] <= 0 || !canPlace(idx))
                continue;
            const int cost = cellCost[idx];
            if (cost > PAINT_PER_TURN)
                continue;
            // Layer 3, applied here rather than in the map: the discount moves
            // with a region's instability, and the beam raises that as it
            // plans, while the map underneath it stays good for the whole line.
            const int slot = cellSlot[idx];
            const int worn = min((int)regionInst[slot], INK_INSTABILITY_THRESHOLD);
            const int v = value[idx] * (INK_SCALE - worn);
            if (v <= 0)
                continue;
            // value | touches | cheapness | scan order, most significant
            // first, so the whole tie-break is one compare. The low 20 bits
            // hold the cell, capping the board at 2^20.
            candKey.push_back(((uint64_t)v << 23) |
                              ((uint64_t)touchesNetwork(idx) << 22) |
                              ((uint64_t)(3 - cost) << 20) |
                              (uint64_t)(N - 1 - idx));
        }

        const size_t keep = min(candKey.size(), (size_t)RAIL_PRUNING_WIDTH);
        partial_sort(candKey.begin(), candKey.begin() + keep, candKey.end(),
                     greater<uint64_t>());

        candCell.clear();
        candCost.clear();
        candValue.clear();
        for (size_t i = 0; i < keep; i++)
        {
            const int idx = N - 1 - (int)(candKey[i] & 0xFFFFF);
            candCell.push_back(idx);
            candCost.push_back(cellCost[idx]);
            candValue.push_back((int)(candKey[i] >> 23));
        }
    }

    // The same ranking read from his side: a region is worth to him what he
    // would erase of ours minus what he would lose of his, which is exactly
    // the score below negated. So his target is its smallest entry.
    void pickFoeDisrupt()
    {
        foeDisruptSlot = -1;
        if (curDepth < FOE_DISRUPT_FROM_DEPTH)
            return;
        int best = 0;
        for (int s = 0; s < R; s++)
        {
            if (regionInked[s] || regionTown[s])
                continue;
            if (regionScore[s] < best)
            {
                best = regionScore[s];
                foeDisruptSlot = s;
            }
        }
    }

    // The opponent reads the same board we do -- the value map is symmetric,
    // built from the towns' wishes with no side in it -- so his turn is the
    // head of our own ranking, cut where his three paint run out.
    void pickFoeRails()
    {
        nFoeRails = 0;
        foeBought = 0;
        if (curDepth < FOE_PREDICT_FROM_DEPTH)
            return;
        int paint = PAINT_PER_TURN;
        for (size_t i = 0; i < candCell.size() && nFoeRails < FOE_RAILS_PREDICTED; i++)
        {
            if (candCost[i] > paint)
                continue; // he cannot afford this one, the next may be cheaper
            paint -= candCost[i];
            foeRailValue[nFoeRails] = candValue[i];
            foeBought += candValue[i];
            foeRail[nFoeRails++] = candCell[i];
        }
    }

    int foeValueOf(int idx) const
    {
        for (int i = 0; i < nFoeRails; i++)
            if (foeRail[i] == idx)
                return foeRailValue[i];
        return 0;
    }

    int touchesNetwork(int idx) const
    {
        for (int p = nbrAt[idx], e = nbrAt[idx + 1]; p < e; p++)
            if (net[nbrList[p]])
                return 1;
        return 0;
    }

    // Every set of candidates the turn's paint can afford, richest first. The
    // empty set is one of them: a turn may be worth a disrupt and nothing else.
    // Generated widest first, so at equal value more rails win the tie.
    void buildCombos()
    {
        nCombos = 0;
        const int K = (int)candCell.size();

        for (int i = 0; i < K; i++)
            for (int j = i + 1; j < K; j++)
            {
                if (candCost[i] + candCost[j] > PAINT_PER_TURN)
                    continue; // and no third cell can fit either
                for (int k = j + 1; k < K; k++)
                    if (candCost[i] + candCost[j] + candCost[k] <= PAINT_PER_TURN)
                        addCombo(3, i, j, k,
                                 candValue[i] + candValue[j] + candValue[k]);
            }
        for (int i = 0; i < K; i++)
            for (int j = i + 1; j < K; j++)
                if (candCost[i] + candCost[j] <= PAINT_PER_TURN)
                    addCombo(2, i, j, 0, candValue[i] + candValue[j]);
        for (int i = 0; i < K; i++)
            addCombo(1, i, 0, 0, candValue[i]);
        addCombo(0, 0, 0, 0, 0);

        // Only the richest are played, best first, ties keeping the order
        // above: a cut-off then loses the weakest of what was kept.
        nPlayed = min(nCombos, widthAtDepth(curDepth));
        level[curDepth].kept += nPlayed;
        partial_sort(comboKey, comboKey + nPlayed, comboKey + nCombos,
                     greater<uint64_t>());
    }

    // Combinations level `depth` may play: everything at the root, then a
    // width that decays towards COMBO_FLOOR as the line gets more speculative.
    static int widthAtDepth(int depth)
    {
        if (depth <= 1)
            return INT_MAX;
        int width = COMBO_PRUNING_WIDTH;
        for (int d = 2; d < depth && width > COMBO_FLOOR; d++)
            width /= COMBO_DECAY;
        return max(width, COMBO_FLOOR);
    }

    void addCombo(int n, int a, int b, int c, int sum)
    {
        comboPack[nCombos] =
            (uint32_t)(n | (a << 8) | (b << 16) | (c << 24));
        comboKey[nCombos] =
            ((uint64_t)sum << 24) | (uint64_t)(0xFFFFFF - nCombos);
        nCombos++;
        level[curDepth].combos++;
    }

    // ---- the income a board pays ----

    // Every wish solved on the board as it stands, and both its towns swept
    // for distances. One sweep pays the wish, the pair of them tells any
    // combination, at a handful of adds, whether it could change this wish at
    // all -- which is what keeps the beam off ten solves a board.
    void baselineIncome()
    {
        baseMine = 0;
        baseFoe = 0;
        for (int w = 0; w < nLinks; w++)
        {
            const int src = linkSrc[w], dst = linkDst[w];
            uint16_t *ds = &distSrc[(size_t)w * N];
            uint16_t *dd = &distDst[(size_t)w * N];
            int mine = 0, foe = 0;
            if (src != dst)
            {
                sweep(src, ds, true);
                wishLen[w] = ds[dst];
                if (ds[dst] != UNREACHED)
                    payPath(dst, mine, foe);
                sweep(dst, dd, false);
            }
            else
                wishLen[w] = 0;
            wishMine[w] = mine;
            wishFoe[w] = foe;
            baseMine += mine;
            baseFoe += foe;
        }
    }

    // The turn's payout for both players. A wish is solved again only when the
    // new rails offer it a walk no longer than the one it already has -- a
    // longer one is never chosen, so its payout cannot have moved.
    void collectIncome(const int *cells, int n, int &mineOut, int &foeOut)
    {
        long long mine = baseMine, foe = baseFoe;
        buildPairs(cells, n);
        for (int w = 0; w < nLinks; w++)
        {
            const uint16_t *ds = &distSrc[(size_t)w * N];
            const uint16_t *dd = &distDst[(size_t)w * N];
            int best = UNREACHED;
            for (int p = 0; p < nPairs; p++)
            {
                const int in = ds[pairFrom[p]], out = dd[pairTo[p]];
                if (in == UNREACHED || out == UNREACHED)
                    continue;
                const int len = in + pairGap[p] + out;
                if (len < best)
                    best = len;
            }
            // Equal length counts: the walk is picked NESW, so a rail that
            // only ties can still take the wish and be paid for it. No walk
            // through the new rails at all, and the wish is untouched -- which
            // is what settles the wishes nobody has connected yet.
            if (best == UNREACHED || best > wishLen[w])
                continue;

            int m = 0, f = 0;
            payWish(w, m, f);
            mine += m - wishMine[w];
            foe += f - wishFoe[w];
        }
        mineOut = (int)mine;
        foeOut = (int)foe;
    }

    // A walk through the new rails enters at one of them and leaves at one of
    // them, and whatever it does in between costs at least the grid distance.
    // Every pair, so the smallest of them bounds any such walk from below.
    void buildPairs(const int *cells, int n)
    {
        nPairs = 0;
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n; j++)
            {
                const int a = cells[i], b = cells[j];
                pairFrom[nPairs] = a;
                pairTo[nPairs] = b;
                pairGap[nPairs] = abs(a % W - b % W) + abs(a / W - b / W);
                nPairs++;
            }
    }

    // Distances from a town over the live network, counted in steps. A cell
    // off the network gets one more than the nearest network cell that reached
    // it -- what a rail laid there would cost the walk -- so the same array
    // answers for the cells a combination is about to buy.
    void sweep(int from, uint16_t *dist, bool keepParent)
    {
        sweeps++;
        level[curDepth].sweeps++;
        memset(dist, 0xFF, (size_t)N * sizeof(uint16_t));
        bfsStamp++;
        bfsQueue.clear();
        bfsQueue.push_back(from);
        bfsSeen[from] = bfsStamp;
        dist[from] = 0;
        if (keepParent)
            bfsParent[from] = -1;

        for (size_t head = 0; head < bfsQueue.size(); head++)
        {
            const int cur = bfsQueue[head];
            const uint16_t next = (uint16_t)(dist[cur] + 1);
            for (int p = nbrAt[cur], e = nbrAt[cur + 1]; p < e; p++)
            {
                const int nIdx = nbrList[p];
                if (!net[nIdx])
                {
                    if (dist[nIdx] > next)
                        dist[nIdx] = next;
                    continue;
                }
                if (bfsSeen[nIdx] == bfsStamp)
                    continue;
                bfsSeen[nIdx] = bfsStamp;
                dist[nIdx] = next;
                if (keepParent)
                    bfsParent[nIdx] = cur;
                bfsQueue.push_back(nIdx);
            }
        }
    }

    // One wish on the board as it stands: the shortest walk over towns and
    // live rails pays each player one point per rail he owns on it. Ties go
    // NESW from the asking town, which a FIFO sweep in that neighbour order
    // gives for free. Stops at the destination, unlike the sweep above.
    void payWish(int w, int &mine, int &foe)
    {
        const int src = linkSrc[w], dst = linkDst[w];
        if (src == dst)
            return;

        solves++;
        level[curDepth].solves++;
        bfsStamp++;
        bfsQueue.clear();
        bfsQueue.push_back(src);
        bfsSeen[src] = bfsStamp;
        bfsParent[src] = -1;

        bool found = false;
        for (size_t head = 0; head < bfsQueue.size() && !found; head++)
        {
            const int cur = bfsQueue[head];
            for (int p = nbrAt[cur], e = nbrAt[cur + 1]; p < e; p++)
            {
                const int nIdx = nbrList[p];
                if (bfsSeen[nIdx] == bfsStamp || !net[nIdx])
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
        if (found)
            payPath(dst, mine, foe);
    }

    // One point per rail owned on the walk the sweep just laid out. Towns
    // carry no rail, and a rail both players laid is neutral, so it pays
    // neither.
    void payPath(int dst, int &mine, int &foe)
    {
        for (int cur = dst; cur != -1; cur = bfsParent[cur])
        {
            const int o = owner[cur];
            if (o == myId)
                mine++;
            else if (o == foeId)
                foe++;
        }
    }

    static uint64_t splitmix(uint64_t &state)
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // ---- the viewer's observation points ----

    // Nothing at all in a competition build: the hooks are empty macros, so
    // neither call below is compiled in.
    void reportRootDebug()
    {
#ifdef DEBUG_TOOL
        DBG_REGION_SCORES(regionScore);
        for (int d = 0; d < nDisrupt; d++)
            tallyCut(disruptCand[d]);
#endif
    }

    // The map the move was read off, discount included: the viewer draws this.
    vector<int> viewerValues() const
    {
        vector<int> out(N, 0);
        for (int idx = 0; idx < N; idx++)
        {
            const int slot = cellSlot[idx];
            const int worn =
                slot < 0 ? 0
                         : min((int)regionInst[slot], INK_INSTABILITY_THRESHOLD);
            out[idx] = value[idx] * (INK_SCALE - worn);
        }
        return out;
    }

    // Rails at stake on either side of a cut, for the viewer alone.
    void tallyCut(int slot) const
    {
        const Map &board = *boardRef;
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
            // How far the beam got and how many boards it weighed: the two
            // numbers a replay cannot show.
            stringstream msg;
            msg << "MESSAGE d" << planner.depthReached << " "
                << planner.evaluated << " states | " << action.cells.size()
                << " rails";
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
        // Level by level, so a turn shows where its budget went rather than
        // one total that hides it.
        fprintf(stderr, "  depth  states    formed     kept   scored     A*"
                        "   solves   sweeps\n");
        for (int d = 1; d <= planner.depthReached; d++)
        {
            const auto &l = planner.level[d];
            fprintf(stderr, "  %5d  %6d  %8d  %7d  %7d  %5d  %7d  %7d\n", d,
                    l.states, l.combos, l.kept, l.scored, l.astar, l.solves,
                    l.sweeps);
        }
        fprintf(stderr,
                "turn : %lld us / %d ms  depth %d  %d states scored  "
                "%d combos formed  %d A*  %d solves  %d sweeps  "
                "%d rails (%d paint)  disrupt %d\n",
                us, PLAN_BUDGET_MS, planner.depthReached, planner.evaluated,
                planner.totalCombos(), planner.astarRuns, planner.solves,
                planner.sweeps, (int)action.cells.size(), action.cost,
                disrupt);
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
