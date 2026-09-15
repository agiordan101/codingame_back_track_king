// Debug harness for the Back Track King bot.
//
// It compiles the real engine -- main.cpp is included verbatim, DEBUG_TOOL
// suppressing only its main() -- and watches it through the DBG_* hooks, so
// what the viewer shows is what the bot actually computed, never a
// reimplementation that could drift from it.
//
//   btk-debug live   <outdir>                    play a match, dump every turn
//   btk-debug replay <events.jsonl> <outdir> [n]  replay a logged game
//
// In live mode the tool is a drop-in bot: it reads stdin and writes its move
// to stdout exactly as main.cpp would, and drops <outdir>/turn_NNN.json on the
// side. In replay mode it feeds back the stdin recorded by cg-colosseum, which
// needs no referee and is reproducible.

#define DEBUG_TOOL
#include "../main.cpp"

#include <fstream>
#include <iomanip>
#include <sys/stat.h>

// ====================
// CAPTURE

// Everything one turn of the planner revealed about itself. Filled by the
// DBG_* hooks, flushed to JSON once the move is out.
//
// The planner scores the whole board once per turn, so a turn is one value
// grid -- no rounds, no prefixes: what the viewer draws on a cell is the very
// number the move was read off.
class DebugProbe
{
public:
    int turn = 0;
    bool haveBoard = false;
    Map board;
    vector<pair<int, int>> wishes;
    // The value of every cell, row-major, after the ink discount: the map the
    // rails were chosen on.
    vector<int> values;
    // Per region slot, what inking it would be worth. Indexed by slot, which
    // write() resolves back to a region id through the StaticMap.
    vector<int> regionScore;
    ActionSet decision;
    int disrupt = -1;
    int myScore = 0, foeScore = 0;
    int myId = 0;
    // Wishes already connected this turn, so the viewer can tell a link that
    // still has to be built from one that is already paying out.
    vector<pair<int, int>> active;

    void beginTurn(const Map &b, const vector<pair<int, int>> &w)
    {
        board = b;
        wishes = w;
        haveBoard = true;
        values.clear();
        regionScore.clear();
        decision = ActionSet();
        disrupt = -1;
    }

    void setValues(const vector<int> &v) { values = v; }
    void setRegionScores(const vector<int> &s) { regionScore = s; }

    void endTurn(const ActionSet &action, int d)
    {
        decision = action;
        disrupt = d;
    }

    void write(const string &path) const;
};

static DebugProbe *g_probe = nullptr;

// ---- hook implementations, declared in main.cpp ----

void dbgTurnBegin(const Map &board, const vector<pair<int, int>> &wishes)
{
    if (g_probe)
        g_probe->beginTurn(board, wishes);
}

void dbgValues(const vector<int> &values)
{
    if (g_probe)
        g_probe->setValues(values);
}

void dbgRegionScores(const vector<int> &scoreBySlot)
{
    if (g_probe)
        g_probe->setRegionScores(scoreBySlot);
}

void dbgTurnEnd(const ActionSet &action, int disrupt)
{
    if (g_probe)
        g_probe->endTurn(action, disrupt);
}

// ====================
// SERIALISATION

// Hand-rolled so the tool stays a single translation unit with no dependency
// beyond the standard library, the way main.cpp is.

static void writeIntGrid(ostream &os, const char *name, const Map &board,
                         int (*cell)(const Map &, int, int))
{
    os << "  \"" << name << "\": [";
    for (int y = 0; y < board.height(); y++)
    {
        os << (y ? ",\n    [" : "\n    [");
        for (int x = 0; x < board.width(); x++)
        {
            if (x)
                os << ",";
            os << cell(board, x, y);
        }
        os << "]";
    }
    os << "\n  ],\n";
}

// The planner's flat value array, cut into rows so the viewer indexes it the
// way it indexes terrain and rails.
static void writeFlatGrid(ostream &os, const char *name, int W, int H,
                          const vector<int> &flat)
{
    os << "  \"" << name << "\": [";
    for (int y = 0; y < H; y++)
    {
        os << (y ? ",\n    [" : "\n    [");
        for (int x = 0; x < W; x++)
        {
            if (x)
                os << ",";
            const size_t idx = (size_t)y * W + x;
            os << (idx < flat.size() ? flat[idx] : 0);
        }
        os << "]";
    }
    os << "\n  ],\n";
}

static int cellTerrain(const Map &b, int x, int y) { return b.tileType(x, y); }
static int cellRegion(const Map &b, int x, int y) { return b.tileRegion(x, y); }
static int cellRail(const Map &b, int x, int y) { return b.tileOwner(x, y); }
static int cellInked(const Map &b, int x, int y)
{
    return b.isInked(x, y) ? 1 : 0;
}

void DebugProbe::write(const string &path) const
{
    ofstream os(path);
    if (!os)
    {
        fprintf(stderr, "debug_tool: cannot write %s\n", path.c_str());
        return;
    }

    const int W = board.width(), H = board.height();

    os << "{\n";
    os << "  \"turn\": " << turn << ",\n";
    os << "  \"myId\": " << myId << ",\n";
    os << "  \"width\": " << W << ",\n";
    os << "  \"height\": " << H << ",\n";
    os << "  \"scores\": {\"me\": " << myScore << ", \"foe\": " << foeScore
       << "},\n";

    writeIntGrid(os, "terrain", board, cellTerrain);
    writeIntGrid(os, "regions", board, cellRegion);
    writeIntGrid(os, "rails", board, cellRail);
    writeIntGrid(os, "inked", board, cellInked);
    writeFlatGrid(os, "values", W, H, values);

    // Towns, with the connections each one wishes for.
    os << "  \"towns\": [";
    for (size_t i = 0; i < board.allTowns().size(); i++)
    {
        const Town &t = board.allTowns()[i];
        os << (i ? ",\n    " : "\n    ");
        os << "{\"id\": " << t.id << ", \"x\": " << t.coord.x
           << ", \"y\": " << t.coord.y << ", \"wishes\": [";
        for (size_t k = 0; k < t.desiredConnections.size(); k++)
            os << (k ? "," : "") << t.desiredConnections[k];
        os << "]}";
    }
    os << "\n  ],\n";

    // Regions, sorted by id so the viewer can index them directly. Ink,
    // instability and the disrupt score are per-turn state, so they come off
    // the Map and the probe, not off Region.
    const vector<int> &ids = board.allRegionIds();

    os << "  \"regionInfo\": [";
    for (size_t i = 0; i < ids.size(); i++)
    {
        const int slot = board.stat->slotOfRegion(ids[i]);
        const Region &r = board.stat->regions[slot];
        os << (i ? ",\n    " : "\n    ");
        os << "{\"id\": " << r.id
           << ", \"instability\": " << (int)board.regionInstability[slot]
           << ", \"inked\": " << (board.regionInkedFlag[slot] ? "true" : "false")
           << ", \"hasTown\": " << (r.hasTown ? "true" : "false")
           << ", \"cells\": " << r.coords.size()
           << ", \"disruptScore\": "
           << (slot < (int)regionScore.size() ? regionScore[slot] : 0) << "}";
    }
    os << "\n  ],\n";

    os << "  \"wishes\": [";
    for (size_t i = 0; i < wishes.size(); i++)
        os << (i ? "," : "") << "[" << wishes[i].first << ","
           << wishes[i].second << "]";
    os << "],\n";

    os << "  \"active\": [";
    for (size_t i = 0; i < active.size(); i++)
        os << (i ? "," : "") << "[" << active[i].first << ","
           << active[i].second << "]";
    os << "],\n";

    os << "  \"inkThreshold\": " << INK_INSTABILITY_THRESHOLD << ",\n";

    os << "  \"decision\": {\"cells\": [";
    for (size_t i = 0; i < decision.cells.size(); i++)
        os << (i ? ", " : "") << "{\"x\": " << decision.cells[i].x
           << ", \"y\": " << decision.cells[i].y << "}";
    os << "], \"cost\": " << decision.cost
       << ", \"disrupt\": " << disrupt << "}\n";
    os << "}\n";
}


// ====================
// DRIVING THE ENGINE

// Created up front rather than failing a turn at a time: a missing output
// directory is a typo or a cleaned tree, not a reason to lose the run.
static void ensureDir(const string &dir)
{
    struct stat st;
    if (stat(dir.c_str(), &st) == 0)
        return;
    if (mkdir(dir.c_str(), 0755) != 0)
        fprintf(stderr, "debug_tool: cannot create %s\n", dir.c_str());
}

static string turnPath(const string &dir, int turn)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "/turn_%03d.json", turn);
    return dir + buf;
}

// One turn of the real bot, with the probe watching. Game::gameTurn() writes
// the move to stdout itself, which is what makes live mode a usable bot.
static void runTurn(Game &game, DebugProbe &probe, const string &outdir,
                    int turn)
{
    probe.turn = turn;
    // Set before the turn runs: the hooks fire from inside gameTurn().
    probe.myId = game.myId;
    game.gameTurn();
    probe.myScore = game.myScore;
    probe.foeScore = game.foeScore;

    // The referee reports a live connection from both of its towns; the pair
    // is canonicalised the way BeamSearch::setup() does so the viewer can
    // match it against a wish.
    probe.active.clear();
    for (const auto &kv : game.activeConnections)
    {
        if (!kv.second)
            continue;
        const int a = min(kv.first.first, kv.first.second);
        const int b = max(kv.first.first, kv.first.second);
        if (find(probe.active.begin(), probe.active.end(), make_pair(a, b)) ==
            probe.active.end())
            probe.active.push_back({a, b});
    }

    if (probe.haveBoard)
        probe.write(turnPath(outdir, turn));
}

static int runLive(const string &outdir)
{
    ensureDir(outdir);

    DebugProbe probe;
    g_probe = &probe;

    Game game;
    game.init();
    for (int turn = 1;; turn++)
    {
        game.parse();
        if (!cin)
            break;
        runTurn(game, probe, outdir, turn);
    }
    return 0;
}

// ---- replay ----

// Minimal reader for the "data" string of a cg-colosseum event line. The file
// is one JSON object per line with just {"type": ..., "data": ...}, so a full
// parser would be overkill -- but the escapes have to be honoured, since the
// payload is newline-separated game input.
static bool parseEventLine(const string &line, string &type, string &data)
{
    auto field = [&](const char *key, string &out) -> bool {
        const string pat = string("\"") + key + "\":";
        size_t p = line.find(pat);
        if (p == string::npos)
            return false;
        p = line.find('"', p + pat.size());
        if (p == string::npos)
            return false;
        out.clear();
        for (size_t i = p + 1; i < line.size(); i++)
        {
            const char c = line[i];
            if (c == '"')
                return true;
            if (c != '\\')
            {
                out.push_back(c);
                continue;
            }
            if (++i >= line.size())
                return false;
            switch (line[i])
            {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u':
            {
                // Game input is ASCII; decode the code point and keep the low
                // byte rather than dragging in a UTF-8 encoder.
                if (i + 4 >= line.size())
                    return false;
                out.push_back((char)stoi(line.substr(i + 1, 4), nullptr, 16));
                i += 4;
                break;
            }
            default: out.push_back(line[i]); break;
            }
        }
        return false;
    };

    return field("type", type) && field("data", data);
}

static int runReplay(const string &eventsPath, const string &outdir,
                     int wantTurn)
{
    ensureDir(outdir);

    ifstream f(eventsPath);
    if (!f)
    {
        fprintf(stderr, "debug_tool: cannot open %s\n", eventsPath.c_str());
        return 1;
    }

    // The recorded stdin, in order. The first event carries the init block and
    // the first turn's state together; every later one is a single turn.
    vector<string> inputs;
    vector<string> outputs;
    string line;
    while (getline(f, line))
    {
        if (line.empty())
            continue;
        string type, data;
        if (!parseEventLine(line, type, data))
            continue;
        if (type == "in")
            inputs.push_back(data);
        else if (type == "out")
            outputs.push_back(data);
    }

    if (inputs.empty())
    {
        fprintf(stderr, "debug_tool: no input events in %s\n",
                eventsPath.c_str());
        return 1;
    }

    // Asking for more turns than the game holds means "all of it", not a
    // mistake: a caller replaying a batch of games cannot know each length.
    const int lastTurn = (int)inputs.size();
    if (wantTurn > lastTurn)
        fprintf(stderr, "debug_tool: game is %d turns, %d asked -- replaying "
                        "all of it\n",
                lastTurn, wantTurn);
    const int stopTurn = wantTurn > 0 ? min(wantTurn, lastTurn) : lastTurn;

    // Game reads cin directly, so the recording is handed to it as cin rather
    // than threading a stream through the engine's signatures.
    string all;
    for (int i = 0; i < stopTurn; i++)
        all += inputs[i];
    istringstream feed(all);
    streambuf *const savedIn = cin.rdbuf(feed.rdbuf());

    // The move the bot prints on replay is noise on stdout; it is compared
    // against the recording below instead.
    ostringstream sink;
    streambuf *const savedOut = cout.rdbuf(sink.rdbuf());

    DebugProbe probe;
    g_probe = &probe;

    Game game;
    game.init();
    for (int turn = 1; turn <= stopTurn; turn++)
    {
        game.parse();
        runTurn(game, probe, outdir, turn);
    }

    cin.rdbuf(savedIn);
    cout.rdbuf(savedOut);

    // What the logged bot played, next to what this build plays on the same
    // input. The planner reads the board and answers -- no clock, no search --
    // so a replay of a game this very version logged reproduces it exactly;
    // any difference means the log came from another version. The comparison
    // is on the set of actions rather than the printed line, since the order a
    // turn's rails are chosen in carries no meaning for the referee.
    if (stopTurn <= (int)outputs.size())
    {
        istringstream replayed(sink.str());
        string line, lastLine, logged = outputs[stopTurn - 1];
        while (getline(replayed, line))
            if (!line.empty())
                lastLine = line;
        while (!logged.empty() &&
               (logged.back() == '\n' || logged.back() == '\r'))
            logged.pop_back();

        // The disrupt is part of the move, so it joins the set: without it a
        // turn that lays no rail would compare equal whatever it inks.
        auto moveSet = [](const string &s) {
            set<pair<int, int>> out;
            size_t p = 0;
            while ((p = s.find("PLACE_TRACKS ", p)) != string::npos)
            {
                int x, y;
                if (sscanf(s.c_str() + p, "PLACE_TRACKS %d %d", &x, &y) == 2)
                    out.insert({x, y});
                p += 13;
            }
            if ((p = s.find("DISRUPT ", 0)) != string::npos)
            {
                int region;
                if (sscanf(s.c_str() + p, "DISRUPT %d", &region) == 1)
                    out.insert({-1, region});
            }
            return out;
        };

        fprintf(stderr, "turn %d  logged : %s\n", stopTurn, logged.c_str());
        fprintf(stderr, "turn %d  replay : %s\n", stopTurn, lastLine.c_str());

        if (lastLine == logged)
            fprintf(stderr, "turn %d  -> identical\n", stopTurn);
        else if (moveSet(lastLine) == moveSet(logged))
            fprintf(stderr, "turn %d  -> same move, different order\n",
                    stopTurn);
        else
            fprintf(stderr,
                    "turn %d  -> differs. The planner is deterministic, so the "
                    "log was written\n"
                    "            by a different version of the bot.\n",
                    stopTurn);
    }

    fprintf(stderr, "debug_tool: wrote turns 1..%d to %s\n", stopTurn,
            outdir.c_str());
    return 0;
}

static int usage()
{
    fprintf(stderr,
            "usage:\n"
            "  btk-debug live   <outdir>\n"
            "  btk-debug replay <events.jsonl> <outdir> [turn]\n");
    return 1;
}

int main(int argc, char **argv)
{
    ios::sync_with_stdio(false);

    if (argc < 3)
        return usage();

    const string mode = argv[1];
    if (mode == "live")
        return runLive(argv[2]);
    if (mode == "replay")
    {
        if (argc < 4)
            return usage();
        const int turn = argc > 4 ? atoi(argv[4]) : 0;
        return runReplay(argv[2], argv[3], turn);
    }
    return usage();
}
