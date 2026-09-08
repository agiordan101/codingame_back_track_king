// v1.2
// - Find shortest distance amongs desired connections to build
// - Skip connection if active
// - Pick which region to disrupt: one with enemy rails, not yet inked,
//     not containing one of our/their towns (can't disrupt those),
//     preferring the one closest to being inked / with the most rails
// - A* instead of floodfill & Skip dead town

#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <unordered_map>
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
// fprintf(stderr, "%-32s avg time : %f ys  \ttotals : %d ys  \t%d calls\n", #name, (double)elapsed_##name / callcount_##name, elapsed_##name, callcount_##name); \

// Profile declarations
DECLARE_PROFILE(mainLoopturn)

// ====================
// STRUCTURES

class Coord
{
public:
    int x, y;
    Coord(int x = 0, int y = 0) : x(x), y(y) {}
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
        : regionId(r), type(t), tracksOwner(-1), inked(false), instability(0) {}
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

// Cost to cross a terrain type (mirrors Python's COST dict).
// Types not listed (e.g. 3 = POI) are impassable, just like the Python version.
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
        return INT_MAX; // impassable (POI or unknown)
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
    // Best known cost from src to each cell found so far.
    vector<vector<int>> gScore(height, vector<int>(width, INT_MAX));
    gScore[src.y][src.x] = 0;

    auto heuristic = [&](int x, int y)
    {
        return abs(x - dst.x) + abs(y - dst.y);
    };

    // min-heap of (f = g + h, g, x, y)
    priority_queue<tuple<int, int, int, int>, vector<tuple<int, int, int, int>>, greater<>> pq;
    pq.push({heuristic(src.x, src.y), 0, src.x, src.y});

    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};

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
            int nx = x + dx[k], ny = y + dy[k];
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
class Map
{
private:
    Grid grid;
    vector<Town> towns;
    unordered_map<int, Region> regionById;

    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // set of cells that contain a town (cost 0 to cross, like in Python)
    set<pair<int, int>> townCells;

    // Lookup table: regionId -> does this region contain a town?
    // A region containing a town can never be disrupted, so this is
    // checked before ever adding a region to the disrupt candidates.
    unordered_map<int, bool> regionHasTown;

    Region &getRegionAt(int x, int y)
    {
        return regionById[grid.get(x, y).regionId];
    }

public:
    // ---- geometry ----

    int width() const { return grid.width; }
    int height() const { return grid.height; }

    // ---- construction / parsing ----

    // Reads the width/height + per-tile (regionId, type) block, building the
    // grid and the region table.
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

        // Build the regionId -> hasTown lookup table now that every town's
        // region has been flagged (getRegionAt(...).hasTown = true above).
        for (auto &kv : regionById)
        {
            regionHasTown[kv.first] = kv.second.hasTown;
        }
    }

    // Reads the per-turn tile state block. Each active connection found is
    // recorded into outActiveConnections for the caller's own bookkeeping.
    void readTurnState(istream &in, map<pair<int, int>, bool> &outActiveConnections)
    {
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
                tile.tracksOwner = tracksOwner;
                tile.inked = inked;
                tile.instability = instability;
                tile.partOfActiveConnections = connections;
            }
        }
    }

    // ---- town queries ----

    bool hasTown(int townId) const { return townCoord.count(townId) != 0; }

    // Precondition: hasTown(townId).
    Coord townCoordOf(int townId) const { return townCoord.at(townId); }

    const vector<Town> &allTowns() const { return towns; }

    // ---- region queries ----

    bool regionContainsTown(int regionId) const
    {
        auto it = regionHasTown.find(regionId);
        return it != regionHasTown.end() && it->second;
    }

    // Per-region aggregates recomputed from the current tile state:
    //   outInstability[r] - instability of region r
    //   outInked         - regions with at least one inked tile
    //   outFoeRails[r]   - number of tiles in r carrying foeId's rails
    void aggregateRegions(int foeId,
                          unordered_map<int, int> &outInstability,
                          set<int> &outInked,
                          unordered_map<int, int> &outFoeRails)
    {
        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                Tile &tile = grid.get(x, y);
                int r = tile.regionId;
                outInstability[r] = tile.instability;
                if (tile.inked)
                    outInked.insert(r);
                if (tile.tracksOwner == foeId)
                {
                    outFoeRails[r] = outFoeRails[r] + 1;
                }
            }
        }
    }

    // ---- pathfinding ----

    // Shortest path cost between two towns' cells, using this map's terrain.
    // Returns INT_MAX if dst is unreachable from src.
    int aStar(Coord src, Coord dst)
    {
        return ::aStar(src, dst, grid.width, grid.height,
                       [&](int x, int y)
                       {
                           // Town cells cost 0 to cross (like Python).
                           if (townCells.count({x, y}))
                               return 0;
                           return terrainCost(grid.get(x, y).type);
                       });
    }
};

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

    // Cheapest desired connection we've decided to build, if any.
    bool hasTargetPair = false;
    Coord targetA, targetB;

    // Region we are currently trying to disrupt (persists across turns), -1 if none.
    int targetRegion = -1;

    void init()
    {
        cin >> myId;
        foeId = 1 - myId;
        gameMap.readTerrain(cin);
        activeConnections.clear();
        gameMap.readTowns(cin, wishes);

        computeBestWish();
    }

    // Equivalent of the Python "cheapest desired connection" computation.
    void computeBestWish()
    {
        bool found = false;
        int bestCost = INT_MAX;
        int bestA = -1, bestB = -1;

        for (auto &wish : wishes)
        {
            int a = wish.first, b = wish.second;
            if (!gameMap.hasTown(a) || !gameMap.hasTown(b))
                continue;

            // Skip wish if link already made
            if (activeConnections.count({a, b}) || activeConnections.count({b, a}))
            {
                // cerr << "Skipping already active connection: " << a << "-" << b << endl;
                continue;
            }

            Coord ac = gameMap.townCoordOf(a);
            Coord bc = gameMap.townCoordOf(b);

            int cost = gameMap.aStar(ac, bc);
            // cerr << "Considering wish: " << a << "-" << b << " with cost " << cost << endl;

            if (cost == INT_MAX)
                continue;

            if (!found || cost < bestCost)
            {
                // cerr << "New best wish: " << a << "-" << b << " with cost " << cost << endl;
                found = true;
                bestCost = cost;
                bestA = a;
                bestB = b;
            }
        }

        if (found)
        {
            hasTargetPair = true;
            targetA = gameMap.townCoordOf(bestA);
            targetB = gameMap.townCoordOf(bestB);
        }
        else
        {
            hasTargetPair = false;
        }
    }

    void parse()
    {
        cin >> myScore;
        cin >> foeScore;
        activeConnections.clear();
        gameMap.readTurnState(cin, activeConnections);
    }

    void gameTurn()
    {
        vector<string> actions;

        computeBestWish();

        // --- Aggregate instability / inked / enemy rails per region ---
        unordered_map<int, int> inst;
        set<int> inkedRegions;
        unordered_map<int, int> foeRails;

        gameMap.aggregateRegions(foeId, inst, inkedRegions, foeRails);

        // --- Pick which region to disrupt: one with enemy rails, not yet inked,
        //     not containing one of our/their towns (can't disrupt those),
        //     preferring the one closest to being inked / with the most rails.
        //     The goal is to push a region's instability high enough that it
        //     gets erased with ink, wiping out every enemy rail inside it. ---
        vector<int> candidates;
        for (auto &kv : foeRails)
        {
            int r = kv.first;
            if (inkedRegions.count(r))
                continue;
            if (gameMap.regionContainsTown(r))
                continue; // regions with a town can't be disrupted
            candidates.push_back(r);
        }
        set<int> candidateSet(candidates.begin(), candidates.end());

        if (targetRegion != -1 && !candidateSet.count(targetRegion))
        {
            targetRegion = -1;
        }

        if (!candidates.empty())
        {
            auto pickBest = [&](const vector<int> &cands)
            {
                int best = cands[0];
                for (int r : cands)
                {
                    if (make_pair(inst[r], foeRails[r]) > make_pair(inst[best], foeRails[best]))
                    {
                        best = r;
                    }
                }
                return best;
            };

            if (targetRegion == -1)
            {
                targetRegion = pickBest(candidates);
            }
            else
            {
                int other = pickBest(candidates);
                // Only switch targets if the other region is clearly further along.
                if (make_pair(inst[other], foeRails[other]) >
                    make_pair(inst[targetRegion], foeRails[targetRegion] + 1))
                {
                    targetRegion = other;
                }
            }
        }

        if (targetRegion != -1)
        {
            actions.push_back("DISRUPT " + to_string(targetRegion));

            stringstream msg;
            msg << "MESSAGE R" << targetRegion << " inst " << inst[targetRegion]
                << " (" << foeRails[targetRegion] << " rails)";
            actions.push_back(msg.str());
        }

        if (hasTargetPair)
        {
            stringstream ap;
            ap << "AUTOPLACE " << targetA.x << " " << targetA.y << " "
               << targetB.x << " " << targetB.y;
            actions.push_back(ap.str());
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
    }
};

void mainLoopturn(Game &game)
{
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
    }
    cout << "prout" << endl;
}
