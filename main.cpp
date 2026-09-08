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

#define DECLARE_PROFILE(name) \
    int callcount_##name = 0; \
    int elapsed_##name = 0;

#define PROFILE(name) ProfileScope _ps_##name(callcount_##name, elapsed_##name)

#define PRINT_PROFILE(name)                                                                                                                                                                             \
    do                                                                                                                                                                                                  \
    {                                                                                                                                                                                                   \
        if (callcount_##name != 0)                                                                                                                                                                      \
            fprintf(stderr, "%-32s avg time : %f ms  \ttotals : %d ms  \t%d calls\n", #name, (double)elapsed_##name / callcount_##name / 1000, (int)((double)elapsed_##name / 1000), callcount_##name); \
    } while (0)

// Profile declarations
DECLARE_PROFILE(mainLoopturn)
DECLARE_PROFILE(beamSearch)
DECLARE_PROFILE(railChoices)
DECLARE_PROFILE(disruptChoice)
DECLARE_PROFILE(simulateTurn)

// ====================
// CONSTANTS

static const int BEAM_WIDTH = 20;
static const int MAX_DEPTH = 10;
static const int PAINT_PER_TURN = 3;

// Wall-clock budget for one turn's search. The referee allows 50 ms per turn
// (1000 ms on the first). The deadline is only tested between expansions, so
// the budget stays well under the limit to absorb one in-flight expansion
// plus the final replay and output.
static const int TURN_BUDGET_MS = 20;
static const int FIRST_TURN_BUDGET_MS = 700;

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
private:
    Grid grid;
    vector<Town> towns;
    unordered_map<int, Region> regionById;

    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // set of cells that contain a town
    set<pair<int, int>> townCells;

    // Lookup table: regionId -> does this region contain a town?
    unordered_map<int, bool> regionHasTown;

    Region &getRegionAt(int x, int y)
    {
        return regionById[grid.get(x, y).regionId];
    }

public:
    // ---- geometry ----

    int width() const { return grid.width; }
    int height() const { return grid.height; }

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
            townCells.insert({townX, townY});

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
    }

    // ---- tile queries ----

    int tileType(int x, int y) const { return grid.get(x, y).type; }
    int tileOwner(int x, int y) const { return grid.get(x, y).tracksOwner; }
    int tileRegion(int x, int y) const { return grid.get(x, y).regionId; }
    bool tileInked(int x, int y) const { return grid.get(x, y).inked; }

    bool isTownCell(int x, int y) const { return townCells.count({x, y}) != 0; }
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
        }
    }

    // ---- rail groups ----

    // Every cell reachable from a town through an unbroken run of rails and
    // towns. This is the "rail group connected to the town" of the spec.
    vector<Coord> railGroupOf(Coord townCell) const
    {
        vector<Coord> group;
        if (!inBounds(townCell.x, townCell.y))
            return group;

        vector<vector<bool>> seen(grid.height, vector<bool>(grid.width, false));
        vector<Coord> stack{townCell};
        seen[townCell.y][townCell.x] = true;

        while (!stack.empty())
        {
            Coord cur = stack.back();
            stack.pop_back();
            group.push_back(cur);

            for (int k = 0; k < 4; k++)
            {
                int nx = cur.x + DIR_X[k], ny = cur.y + DIR_Y[k];
                if (!inBounds(nx, ny) || seen[ny][nx])
                    continue;
                if (!isConnectable(nx, ny))
                    continue;
                seen[ny][nx] = true;
                stack.push_back(Coord(nx, ny));
            }
        }
        return group;
    }

    // ---- connections ----

    // Shortest rail/town path between two towns, honouring the
    // NORTH/EAST/SOUTH/WEST tie-break. Empty if the towns are not linked.
    vector<Coord> connectionPath(Coord from, Coord to) const
    {
        if (!inBounds(from.x, from.y) || !inBounds(to.x, to.y))
            return {};

        vector<vector<int>> dist(grid.height, vector<int>(grid.width, INT_MAX));
        vector<vector<Coord>> parent(grid.height, vector<Coord>(grid.width, Coord(-1, -1)));

        queue<Coord> q;
        dist[from.y][from.x] = 0;
        q.push(from);

        while (!q.empty())
        {
            Coord cur = q.front();
            q.pop();
            if (cur == to)
                break;

            // Neighbours are visited in NORTH/EAST/SOUTH/WEST order, so the
            // first parent recorded for a cell already follows the priority.
            for (int k = 0; k < 4; k++)
            {
                int nx = cur.x + DIR_X[k], ny = cur.y + DIR_Y[k];
                if (!inBounds(nx, ny))
                    continue;
                if (dist[ny][nx] != INT_MAX)
                    continue;
                if (!isConnectable(nx, ny))
                    continue;
                dist[ny][nx] = dist[cur.y][cur.x] + 1;
                parent[ny][nx] = cur;
                q.push(Coord(nx, ny));
            }
        }

        if (dist[to.y][to.x] == INT_MAX)
            return {};

        vector<Coord> path;
        for (Coord cur = to; cur != Coord(-1, -1); cur = parent[cur.y][cur.x])
        {
            path.push_back(cur);
            if (cur == from)
                break;
        }
        reverse(path.begin(), path.end());
        return path;
    }
};

// ====================
// CHOICES

// A rail placement choice: build from src towards dst.
class RailChoice
{
public:
    Coord src, dst;
    int distance;
    RailChoice(Coord s = {}, Coord d = {}, int dist = INT_MAX)
        : src(s), dst(d), distance(dist) {}
};

// ====================
// GAME

class Game
{
public:
    int myId;
    int foeId;
    Map gameMap;

    int myScore, foeScore;

    // wishes: pairs of town ids that want to be connected
    vector<pair<int, int>> wishes;
    // activeConnections: pairs of town ids being connected
    map<pair<int, int>, bool> activeConnections;

    // Start of the current turn, used to bound the search.
    std::chrono::steady_clock::time_point turnStart;
    // The first turn has a far larger time allowance than the others.
    bool firstTurn = true;

    void init()
    {
        cin >> myId;
        foeId = 1 - myId;
        gameMap.readTerrain(cin);
        activeConnections.clear();
        gameMap.readTowns(cin, wishes);
    }

    void parse()
    {
        cin >> myScore;
        cin >> foeScore;
        activeConnections.clear();
        gameMap.readTurnState(cin, activeConnections);
    }

    // ---- "Rail placement choice"s creation ----

    // For every desired connection not yet built, grow the rail group already
    // attached to each of the two towns, then take the cheapest (Manhattan)
    // pair of cells across the two groups. That pair is the choice.
    static vector<RailChoice> buildRailChoices(const Map &board,
                                               const vector<pair<int, int>> &wishes,
                                               const map<pair<int, int>, bool> &active)
    {
        PROFILE(railChoices);

        vector<RailChoice> choices;

        for (const auto &wish : wishes)
        {
            int a = wish.first, b = wish.second;
            if (!board.hasTown(a) || !board.hasTown(b))
                continue;
            // Already built: nothing to place for this wish.
            if (active.count({a, b}) || active.count({b, a}))
                continue;

            Coord ac = board.townCoordOf(a);
            Coord bc = board.townCoordOf(b);

            vector<Coord> groupA = board.railGroupOf(ac);
            vector<Coord> groupB = board.railGroupOf(bc);
            if (groupA.empty() || groupB.empty())
                continue;

            // Cross-product of both groups: keep the shortest link.
            RailChoice best;
            for (const Coord &ca : groupA)
            {
                for (const Coord &cb : groupB)
                {
                    int d = abs(ca.x - cb.x) + abs(ca.y - cb.y);
                    if (d < best.distance)
                        best = RailChoice(ca, cb, d);
                }
            }

            // distance 0 means the groups already touch: the connection is
            // effectively built, nothing to place.
            if (best.distance != INT_MAX && best.distance > 0)
                choices.push_back(best);
        }

        return choices;
    }

    // ---- "Disrupt choice" creation ----

    // A region is a candidate when it is not inked, holds no town, carries at
    // least one opponent rail, and — counting unique rails per player across
    // the active connections running through it — the opponent owns strictly
    // more than we do. Only the single best region is returned (-1 if none).
    static int buildDisruptChoice(Map &board,
                                  const vector<pair<int, int>> &wishes,
                                  int selfId, int otherId)
    {
        PROFILE(disruptChoice);

        // Collect the cells of every currently active connection once.
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

    // ---- "Rail placement choice" application ----

    // Walks from the choice's source towards its destination, spending the
    // turn's paint. At each step the neighbour with the smallest A* distance
    // to the destination wins, ties broken NORTH/EAST/SOUTH/WEST.
    //
    // The cells to fill are returned rather than written, so both players'
    // rails can be applied simultaneously (neutral-owner rule).
    static vector<Coord> planRailPlacements(const Map &board, const RailChoice &choice)
    {
        vector<Coord> placements;
        if (choice.distance == INT_MAX)
            return placements;

        // Cells claimed so far this turn, so the walk does not reuse one.
        set<pair<int, int>> claimed;

        // Distance from every cell to the destination, computed once with a
        // single Dijkstra from the destination instead of one A* per
        // candidate neighbour. Costs are symmetric (entering a cell costs the
        // same either way), so a reverse search gives the same distances at a
        // fraction of the work — this is the hot path of the whole search.
        const int W = board.width(), H = board.height();
        vector<vector<int>> distToDst(H, vector<int>(W, INT_MAX));

        {
            priority_queue<tuple<int, int, int>, vector<tuple<int, int, int>>, greater<>> pq;
            distToDst[choice.dst.y][choice.dst.x] = 0;
            pq.push({0, choice.dst.x, choice.dst.y});

            while (!pq.empty())
            {
                auto [d, x, y] = pq.top();
                pq.pop();
                if (d > distToDst[y][x])
                    continue;

                for (int k = 0; k < 4; k++)
                {
                    int nx = x + DIR_X[k], ny = y + DIR_Y[k];
                    if (!board.inBounds(nx, ny))
                        continue;
                    // Only cells we could actually build on are traversable.
                    if (!board.canPlaceRail(nx, ny))
                        continue;

                    int nd = d + board.railCost(nx, ny);
                    if (nd < distToDst[ny][nx])
                    {
                        distToDst[ny][nx] = nd;
                        pq.push({nd, nx, ny});
                    }
                }
            }
        }

        Coord cur = choice.src;
        int paint = PAINT_PER_TURN;

        while (paint > 0)
        {
            int bestK = -1;
            int bestDist = INT_MAX;

            for (int k = 0; k < 4; k++)
            {
                int nx = cur.x + DIR_X[k], ny = cur.y + DIR_Y[k];
                if (!board.inBounds(nx, ny))
                    continue;
                if (!board.canPlaceRail(nx, ny) || claimed.count({nx, ny}))
                    continue;
                if (board.railCost(nx, ny) > paint)
                    continue; // not enough paint left for this terrain

                int d = distToDst[ny][nx];
                if (d == INT_MAX)
                    continue;
                // Strictly-less keeps the earlier (higher priority) direction.
                if (d < bestDist)
                {
                    bestDist = d;
                    bestK = k;
                }
            }

            if (bestK == -1)
                break; // nowhere useful left to build

            Coord next(cur.x + DIR_X[bestK], cur.y + DIR_Y[bestK]);
            paint -= board.railCost(next.x, next.y);
            claimed.insert({next.x, next.y});
            placements.push_back(next);

            // Reached the far group: the connection is joined.
            if (next == choice.dst)
                break;
            cur = next;
        }

        return placements;
    }

    // ---- heuristic ----

    // Connection points are awarded per turn: each active connection pays a
    // player 1 point per rail they own along its path. The heuristic is our
    // income minus the opponent's.
    static int evaluate(Map &board, const vector<pair<int, int>> &wishes,
                        int selfId, int otherId)
    {
        int selfPoints = 0, otherPoints = 0;

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
                    selfPoints++;
                else if (owner == otherId)
                    otherPoints++;
            }
        }

        return selfPoints - otherPoints;
    }

    // ---- game engine turn application ----

    // Applies both players' rail creations simultaneously, then the disrupts,
    // then inking, producing state D+1 in place.
    static void simulateTurn(Map &board,
                             const vector<Coord> &myRails, const vector<Coord> &foeRails,
                             int myDisrupt, int foeDisrupt,
                             int selfId, int otherId)
    {
        PROFILE(simulateTurn);

        // Both players place at the same time: a shared tile becomes neutral.
        for (const Coord &c : myRails)
            if (board.canPlaceRail(c.x, c.y))
                board.placeRail(c.x, c.y, selfId);
        for (const Coord &c : foeRails)
            if (board.canPlaceRail(c.x, c.y) || board.tileOwner(c.x, c.y) == selfId)
                board.placeRail(c.x, c.y, otherId);

        // Disrupts raise instability and may ink (erasing the region's rails).
        if (myDisrupt != -1)
            board.disruptRegion(myDisrupt);
        if (foeDisrupt != -1)
            board.disruptRegion(foeDisrupt);
    }

    // ---- beam search ----

    class BeamNode
    {
    public:
        Map state;
        map<pair<int, int>, bool> active;
        int score;
        // The rail choice played at the root of this line, i.e. the move we
        // would actually output this turn.
        bool hasRootChoice;
        RailChoice rootChoice;
        int rootDisrupt;

        BeamNode() : score(0), hasRootChoice(false), rootDisrupt(-1) {}
    };

    // Runs the beam and returns the move to play this turn.
    void beamSearch(bool &outHasRail, RailChoice &outRail, int &outDisrupt)
    {
        PROFILE(beamSearch);

        outHasRail = false;
        outDisrupt = -1;

        BeamNode root;
        root.state = gameMap;
        root.active = activeConnections;
        root.score = evaluate(root.state, wishes, myId, foeId);

        vector<BeamNode> beam{root};

        // The beam deepens only while there is time left in the turn: on big
        // boards a full MAX_DEPTH sweep overruns the limit, so we keep the
        // best line found so far instead of forfeiting the turn. The first
        // turn gets the referee's larger allowance.
        int budgetMs = firstTurn ? FIRST_TURN_BUDGET_MS : TURN_BUDGET_MS;
        auto deadline = turnStart + std::chrono::milliseconds(budgetMs);

        for (int depth = 0; depth < MAX_DEPTH; depth++)
        {
            if (std::chrono::steady_clock::now() >= deadline)
                break;

            vector<BeamNode> nextBeam;

            for (BeamNode &node : beam)
            {
                if (std::chrono::steady_clock::now() >= deadline)
                    break;

                // 2. Copy current_state in turn_state (node.state is turn_state).
                // 3. Generate rail choices.
                vector<RailChoice> railChoices =
                    buildRailChoices(node.state, wishes, node.active);

                // 4. Generate both players' best disrupt choices.
                int myDisrupt = buildDisruptChoice(node.state, wishes, myId, foeId);
                int foeDisrupt = buildDisruptChoice(node.state, wishes, foeId, myId);

                // The opponent replies with their own best (shortest) rail
                // choice, held fixed across our alternatives.
                vector<Coord> foeRails;
                if (!railChoices.empty())
                {
                    const RailChoice *foeBest = &railChoices[0];
                    for (const RailChoice &rc : railChoices)
                        if (rc.distance < foeBest->distance)
                            foeBest = &rc;
                    foeRails = planRailPlacements(node.state, *foeBest);
                }

                if (railChoices.empty())
                {
                    // No rail to build: still simulate disrupts so the line
                    // keeps evolving.
                    BeamNode child;
                    child.state = node.state;
                    child.active = node.active;
                    child.hasRootChoice = node.hasRootChoice;
                    child.rootChoice = node.rootChoice;
                    child.rootDisrupt = (depth == 0) ? myDisrupt : node.rootDisrupt;

                    simulateTurn(child.state, {}, foeRails, myDisrupt, foeDisrupt, myId, foeId);
                    child.score = evaluate(child.state, wishes, myId, foeId);
                    nextBeam.push_back(move(child));
                    continue;
                }

                // 7. Iterate over rail choices.
                for (const RailChoice &choice : railChoices)
                {
                    // Expanding a choice is the expensive step, so the budget
                    // is checked here too rather than once per node.
                    if (std::chrono::steady_clock::now() >= deadline)
                        break;

                    BeamNode child;
                    child.state = node.state; // copy turn_state
                    child.active = node.active;

                    if (depth == 0)
                    {
                        child.hasRootChoice = true;
                        child.rootChoice = choice;
                        child.rootDisrupt = myDisrupt;
                    }
                    else
                    {
                        child.hasRootChoice = node.hasRootChoice;
                        child.rootChoice = node.rootChoice;
                        child.rootDisrupt = node.rootDisrupt;
                    }

                    vector<Coord> myRails = planRailPlacements(child.state, choice);
                    simulateTurn(child.state, myRails, foeRails, myDisrupt, foeDisrupt,
                                 myId, foeId);
                    child.score = evaluate(child.state, wishes, myId, foeId);

                    nextBeam.push_back(move(child));
                }
            }

            if (nextBeam.empty())
                break;

            // 8. Keep the Bwidth best states.
            sort(nextBeam.begin(), nextBeam.end(),
                 [](const BeamNode &a, const BeamNode &b)
                 { return a.score > b.score; });
            if ((int)nextBeam.size() > BEAM_WIDTH)
                nextBeam.resize(BEAM_WIDTH);

            beam = move(nextBeam);
        }

        if (!beam.empty())
        {
            const BeamNode &best = beam.front();
            outHasRail = best.hasRootChoice;
            outRail = best.rootChoice;
            outDisrupt = best.rootDisrupt;
        }
    }

    void gameTurn()
    {
        // Timed from after the input read, so blocking on stdin does not
        // count against the search budget.
        turnStart = std::chrono::steady_clock::now();

        bool hasRail = false;
        RailChoice rail;
        int disrupt = -1;

        beamSearch(hasRail, rail, disrupt);

        vector<string> actions;

        // The engine resolves every PLACE_TRACKS before any DISRUPT, so the
        // commands are emitted in that same order.
        int placed = 0;
        if (hasRail)
        {
            // Turn the chosen link into concrete rail placements for this turn.
            vector<Coord> placements = planRailPlacements(gameMap, rail);
            for (const Coord &c : placements)
            {
                actions.push_back("PLACE_TRACKS " + to_string(c.x) + " " + to_string(c.y));
            }
            placed = (int)placements.size();
        }

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
            cout << "\n";
        }
        else
        {
            cout << "WAIT\n";
        }

        firstTurn = false;
    }
};

void mainLoopturn(Game &game)
{
    if (!game.firstTurn)
        PROFILE(mainLoopturn);

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

        PRINT_PROFILE(mainLoopturn);
        PRINT_PROFILE(beamSearch);
        PRINT_PROFILE(railChoices);
        PRINT_PROFILE(disruptChoice);
        PRINT_PROFILE(simulateTurn);
    }
}
