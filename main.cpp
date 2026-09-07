#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <sstream>

using namespace std;

struct Coord {
    int x, y;
    Coord(int x = 0, int y = 0) : x(x), y(y) {}
};

struct Connection {
    int fromTownId, toTownId;
    Connection(int f = -1, int t = -1) : fromTownId(f), toTownId(t) {}
};

struct Tile {
    int regionId;
    int type;
    int tracksOwner;
    bool inked;
    int instability;
    vector<Connection> partOfActiveConnections;
    Tile(int r = 0, int t = 0)
            : regionId(r), type(t), tracksOwner(-1), inked(false), instability(0) {}
};

struct Town {
    int id;
    Coord coord;
    vector<int> desiredConnections;
    Town(int id = 0, Coord c = {}, vector<int> d = {})
            : id(id), coord(c), desiredConnections(move(d)) {}
};

struct Grid {
    int width, height;
    vector<Tile> tiles;
    Grid(int w = 0, int h = 0) : width(w), height(h) { tiles.resize(w * h); }
    Tile &get(int x, int y) { return tiles[y * width + x]; }
};

struct Region {
    int id;
    int instability;
    bool inked;
    vector<Coord> coords;
    bool hasTown;
    Region(int id = 0) : id(id), instability(0), inked(false), hasTown(false) {}
};

struct Game {
    int myId;
    Grid grid;
    vector<Town> towns;
    unordered_map<int, Region> regionById;

    int myScore, foeScore;

    void init() {
        cin >> myId;
        int width, height;
        cin >> width >> height;
        grid = Grid(width, height);

        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int regionId, type;
                cin >> regionId >> type;
                Tile tile(regionId, type);
                grid.get(x, y) = tile;
                if (!regionById.count(regionId)) {
                    regionById.emplace(regionId, Region(regionId));
                }
                Region &region = regionById[regionId];
                Coord coord(x, y);
                region.coords.push_back(coord);
            }
        }

        int townCount;
        cin >> townCount;
        for (int i = 0; i < townCount; i++) {
            int townId, townX, townY;
            string desiredStr;
            cin >> townId >> townX >> townY >> desiredStr;
            vector<int> desired;
            if (desiredStr != "x") {
                stringstream ss(desiredStr);
                string tmp;
                while (getline(ss, tmp, ','))
                    desired.push_back(stoi(tmp));
            }
            towns.emplace_back(townId, Coord(townX, townY), desired);
            getRegionAt(townX, townY).hasTown = true;
        }
    }

    Region &getRegionAt(int x, int y) {
        return regionById[grid.get(x, y).regionId];
    }

    void parse() {
        cin >> myScore;
        cin >> foeScore;
        for (int y = 0; y < grid.height; y++) {
            for (int x = 0; x < grid.width; x++) {
                int tracksOwner, instability;
                string inkedStr, partStr;
                cin >> tracksOwner >> instability >> inkedStr >> partStr;
                bool inked = (inkedStr != "0");
                vector<Connection> connections;
                if (partStr != "x") {
                    stringstream ss(partStr);
                    string conn;
                    while (getline(ss, conn, ',')) {
                        int fromTownId, toTownId;
                        sscanf(conn.c_str(), "%d-%d", &fromTownId, &toTownId);
                        connections.emplace_back(fromTownId, toTownId);
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

    void gameTurn() {
        vector<string> actions;

        // TODO: Game logic here


        if (!actions.empty()) {
            for (int i = 0; i < (int)actions.size(); i++) {
                if (i)
                    cout << ";";
                cout << actions[i];
            }
            cout << "\n";
        } else {
            cout << "WAIT\n";
        }
    }
};

int main() {
    Game game;
    game.init();
    while (true) {
        game.parse();
        game.gameTurn();
    }
    cout << "prout" << endl;
}
