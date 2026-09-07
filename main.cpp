// v1.0 -
// - Find shortest distance amongs desired connections to build
// - Skip connection if active
// - Pick which region to disrupt: one with enemy rails, not yet inked,
//     not containing one of our/their towns (can't disrupt those),
//     preferring the one closest to being inked / with the most rails

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

using namespace std;

struct Coord
{
    int x, y;
    Coord(int x = 0, int y = 0) : x(x), y(y) {}
};

struct Connection
{
    int fromTownId, toTownId;
    Connection(int f = -1, int t = -1) : fromTownId(f), toTownId(t) {}
};

struct Tile
{
    int regionId;
    int type;
    int tracksOwner;
    bool inked;
    int instability;
    vector<Connection> partOfActiveConnections;
    Tile(int r = 0, int t = 0)
        : regionId(r), type(t), tracksOwner(-1), inked(false), instability(0) {}
};

struct Town
{
    int id;
    Coord coord;
    vector<int> desiredConnections;
    Town(int id = 0, Coord c = {}, vector<int> d = {})
        : id(id), coord(c), desiredConnections(move(d)) {}
};

struct Grid
{
    int width, height;
    vector<Tile> tiles;
    Grid(int w = 0, int h = 0) : width(w), height(h) { tiles.resize(w * h); }
    Tile &get(int x, int y) { return tiles[y * width + x]; }
};

struct Region
{
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

struct Game
{
    int myId;
    int foeId;
    Grid grid;
    vector<Town> towns;
    unordered_map<int, Region> regionById;

    int myScore, foeScore;

    // wishes: pairs of town ids that want to be connected
    vector<pair<int, int>> wishes;
    // activeConnections: pairs of town ids being connected
    map<pair<int, int>, bool> activeConnections;

    // quick lookup: town id -> coord
    unordered_map<int, Coord> townCoord;
    // set of cells that contain a town (cost 0 to cross, like in Python)
    set<pair<int, int>> townCells;

    // Lookup table: regionId -> does this region contain a town?
    // A region containing a town can never be disrupted, so this is
    // checked before ever adding a region to the disrupt candidates.
    unordered_map<int, bool> regionHasTown;

    // Cheapest desired connection we've decided to build, if any.
    bool hasTargetPair = false;
    Coord targetA, targetB;

    // Region we are currently trying to disrupt (persists across turns), -1 if none.
    int targetRegion = -1;

    void init()
    {
        cin >> myId;
        foeId = 1 - myId;
        int width, height;
        cin >> width >> height;
        grid = Grid(width, height);
        activeConnections.clear();

        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int regionId, type;
                cin >> regionId >> type;
                Tile tile(regionId, type);
                grid.get(x, y) = tile;
                if (!regionById.count(regionId))
                {
                    regionById.emplace(regionId, Region(regionId));
                }
                Region &region = regionById[regionId];
                Coord coord(x, y);
                region.coords.push_back(coord);
            }
        }

        int townCount;
        cin >> townCount;
        for (int i = 0; i < townCount; i++)
        {
            int townId, townX, townY;
            string desiredStr;
            cin >> townId >> townX >> townY >> desiredStr;
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
                wishes.emplace_back(townId, other);
            }
        }

        // Build the regionId -> hasTown lookup table now that every town's
        // region has been flagged (getRegionAt(...).hasTown = true above).
        for (auto &kv : regionById)
        {
            regionHasTown[kv.first] = kv.second.hasTown;
        }

        computeBestWish();
    }

    Region &getRegionAt(int x, int y)
    {
        return regionById[grid.get(x, y).regionId];
    }

    // Dijkstra from src over the grid. Town cells cost 0 to cross (like Python).
    vector<vector<int>> dijkstra(Coord src)
    {
        int width = grid.width, height = grid.height;
        vector<vector<int>> dist(height, vector<int>(width, INT_MAX));
        dist[src.y][src.x] = 0;

        // min-heap of (dist, x, y)
        priority_queue<tuple<int, int, int>, vector<tuple<int, int, int>>, greater<>> pq;
        pq.push({0, src.x, src.y});

        const int dx[4] = {0, 1, 0, -1};
        const int dy[4] = {-1, 0, 1, 0};

        while (!pq.empty())
        {
            auto [d, x, y] = pq.top();
            pq.pop();
            if (d > dist[y][x])
                continue;

            for (int k = 0; k < 4; k++)
            {
                int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || nx >= width || ny < 0 || ny >= height)
                    continue;

                int step;
                if (townCells.count({nx, ny}))
                {
                    step = 0;
                }
                else
                {
                    step = terrainCost(grid.get(nx, ny).type);
                }
                if (step == INT_MAX)
                    continue;

                int nd = d + step;
                if (nd < dist[ny][nx])
                {
                    dist[ny][nx] = nd;
                    pq.push({nd, nx, ny});
                }
            }
        }
        return dist;
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
            if (!townCoord.count(a) || !townCoord.count(b))
                continue;

            // Skip wish if link already made
            if (activeConnections.count({a, b}) || activeConnections.count({b, a}))
            {
                cerr << "Skipping already active connection: " << a << "-" << b << endl;
                continue;
            }

            Coord ac = townCoord[a];
            Coord bc = townCoord[b];

            auto dist = dijkstra(ac);
            int cost = dist[bc.y][bc.x];
            cerr << "Considering wish: " << a << "-" << b << " with cost " << cost << endl;
            
            if (cost == INT_MAX)
            {
                cost = 1000 + abs(ac.x - bc.x) + abs(ac.y - bc.y);
            }
            if (!found || cost < bestCost)
            {
                cerr << "New best wish: " << a << "-" << b << " with cost " << cost << endl;
                found = true;
                bestCost = cost;
                bestA = a;
                bestB = b;
            }
        }

        if (found)
        {
            hasTargetPair = true;
            targetA = townCoord[bestA];
            targetB = townCoord[bestB];
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
        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                int tracksOwner, instability;
                string inkedStr, partStr;
                cin >> tracksOwner >> instability >> inkedStr >> partStr;
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
                        activeConnections[{fromTownId, toTownId}] = true;
                        cerr << "Rails x=" << x << " y=" << y << ": Active connection: " << fromTownId << "-" << toTownId << endl;
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

    void gameTurn()
    {
        vector<string> actions;

        computeBestWish();

        // --- Aggregate instability / inked / enemy rails per region ---
        unordered_map<int, int> inst;
        set<int> inkedRegions;
        unordered_map<int, int> foeRails;

        for (int y = 0; y < grid.height; y++)
        {
            for (int x = 0; x < grid.width; x++)
            {
                Tile &tile = grid.get(x, y);
                int r = tile.regionId;
                inst[r] = tile.instability;
                if (tile.inked)
                    inkedRegions.insert(r);
                if (tile.tracksOwner == foeId)
                {
                    foeRails[r] = foeRails[r] + 1;
                }
            }
        }

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
            if (regionHasTown[r])
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

int main()
{
    Game game;
    game.init();
    while (true)
    {
        game.parse();
        game.gameTurn();
    }
    cout << "prout" << endl;
}