// The maps as the bot sees them (#77): each map's collision grid (.att, docs/formats/att.md) and
// objects (.obj, docs/formats/obj.md), read from the extracted bundle, turned into platforms,
// ladders and the moves between them. A route on one map is a list of those moves; between maps
// the bot follows the portal links of chapter 1 (k_links, from the portal scripts). Read only.
// Header-only, like game_driver.h.
#ifndef MAPLETHIEF_TESTS_WORLD_MAP_H
#define MAPLETHIEF_TESTS_WORLD_MAP_H

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <queue>
#include <string>
#include <tuple>
#include <vector>

namespace world_map {

using u8 = std::uint8_t;

// The .att flags (docs/formats/att.md, Values).
constexpr u8 k_air = 0x01, k_foothold = 0x02, k_ladder = 0x04, k_ladder_top = 0x08, k_wall = 0x10;
constexpr int k_cell = 8; // an attribute cell is 8 x 8 px

inline std::vector<u8> read_bytes(const std::filesystem::path &p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// An object of the map: a portal (kind 0-3), an NPC (4-59) or a mob spawn (60+), its anchor
// (bottom centre of its tile) and its script name.
struct Object {
    int kind = 0;
    std::string name;
    int x = 0, y = 0;
};

// A platform: a run of standable cells in one row. The hero stands on it at y = row * 8 - 1,
// x from x0 to x1 (pixels).
struct Platform {
    int row = 0, c0 = 0, c1 = 0; // cells c0..c1 inclusive
    int y() const { return row * k_cell - 1; }
    int x0() const { return c0 * k_cell; }
    int x1() const { return c1 * k_cell + k_cell - 1; }
};

// A ladder or rope: two cells wide (c, c + 1), rows r0..r1. Its middle is the x the game snaps
// the hero to.
struct Ladder {
    int c = 0, r0 = 0, r1 = 0;
    int x() const { return c * k_cell + k_cell; }
    int top() const { return r0 * k_cell; }
    int bottom() const { return r1 * k_cell + k_cell - 1; }
};

// A move from one platform to another: how and from where.
enum class Move { walk, jump, fall, climb_up, climb_down };
inline const char *move_name(Move m) {
    switch (m) {
    case Move::walk:
        return "walk";
    case Move::jump:
        return "jump";
    case Move::fall:
        return "fall";
    case Move::climb_up:
        return "climb up";
    case Move::climb_down:
        return "climb down";
    }
    return "?";
}
struct Edge {
    int to = 0;
    Move move = Move::walk;
    int x = 0;   // where on the platform the move starts
    int dir = 0; // -1 / +1: the direction held for a jump or a fall (0: straight up)
    int ladder = -1;
    int cost = 0;
};

struct Map {
    int area = -1, map = -1;
    int w = 0, h = 0; // in cells
    std::vector<u8> att;
    std::vector<Object> objects;
    std::vector<Platform> platforms;
    std::vector<Ladder> ladders;
    std::vector<std::vector<Edge>> edges;
    bool ok = false;

    u8 at(int cx, int cy) const {
        if (cx < 0 || cx >= w || cy < 0) {
            return k_wall;
        }
        if (cy >= h) {
            return k_foothold; // past the bottom: the game's invisible floor
        }
        return att[static_cast<std::size_t>(cy * w + cx)];
    }
    bool standable(int cx, int cy) const { return (at(cx, cy) & k_foothold) != 0; }
    bool blocked(int cx, int cy) const { return (at(cx, cy) & k_wall) != 0; }

    // The platform under pixel (x, y): the first standable cell at or below y in that column,
    // within `below` pixels. -1 if none.
    int platform_at(int x, int y, int below = 4) const {
        const int cx = x / k_cell;
        for (int cy = std::max(0, (y + 1) / k_cell); cy <= (y + 1 + below) / k_cell && cy < h;
             cy++) {
            for (std::size_t i = 0; i < platforms.size(); i++) {
                const Platform &p = platforms[i];
                if (p.row == cy && cx >= p.c0 && cx <= p.c1) {
                    return static_cast<int>(i);
                }
            }
        }
        return -1;
    }
    // The first platform below (x, y) (falling straight down from there).
    int platform_below(int x, int y) const {
        const int cx = x / k_cell;
        for (int cy = std::max(0, y / k_cell + 1); cy < h; cy++) {
            if (standable(cx, cy)) {
                return platform_at(x, cy * k_cell - 1, 0);
            }
        }
        return -1;
    }
    const Object *object(const std::string &name) const {
        for (const Object &o : objects) {
            if (o.name == name) {
                return &o;
            }
        }
        return nullptr;
    }
    const Object *portal(int kind) const {
        for (const Object &o : objects) {
            if (o.kind == kind && kind < 4) {
                return &o;
            }
        }
        return nullptr;
    }
};

// Jumps the bot plans with (measured on the first maps: a jump rises about 50 px; walking while
// in the air carries the hero a few cells sideways). Kept inside what always works.
constexpr int k_jump_up = 44;    // px a jump reaches up onto a platform
constexpr int k_jump_side = 40;  // px a running jump crosses sideways over a gap
constexpr int k_fall_reach = 24; // px a walk off an edge drifts sideways before landing
// A ladder or rope is grabbed higher than a platform is reached: up in the air looks for a ladder
// cell a quarter of the hero's height (+0x34) above its feet (CHero::KeyUpDown 0x63665 through
// 0x61474), so a jump reaches a ladder's foot hanging up to about 52 px above the floor (1.4's
// ladder to portal140 hangs 48 px up).
constexpr int k_ladder_reach = 52;

inline void build(Map &m) {
    // Platforms: runs of foothold cells per row (one-way floors; a wall corner counts).
    for (int r = 0; r < m.h; r++) {
        int c = 0;
        while (c < m.w) {
            if (!m.standable(c, r)) {
                c++;
                continue;
            }
            int e = c;
            while (e + 1 < m.w && m.standable(e + 1, r)) {
                e++;
            }
            m.platforms.push_back({r, c, e});
            c = e + 1;
        }
    }
    // Ladders: two-cell columns of ladder cells.
    std::vector<std::vector<bool>> seen(static_cast<std::size_t>(m.h),
                                        std::vector<bool>(static_cast<std::size_t>(m.w)));
    for (int r = 0; r < m.h; r++) {
        for (int c = 0; c + 1 < m.w; c++) {
            if (seen[r][c] || (m.at(c, r) & k_ladder) == 0 || (m.at(c + 1, r) & k_ladder) == 0) {
                continue;
            }
            int e = r;
            while (e + 1 < m.h && (m.at(c, e + 1) & k_ladder) != 0) {
                e++;
            }
            for (int k = r; k <= e; k++) {
                seen[k][c] = seen[k][c + 1] = true;
            }
            m.ladders.push_back({c, r, e});
            c++;
        }
    }
    const int n = static_cast<int>(m.platforms.size());
    m.edges.assign(static_cast<std::size_t>(n), {});
    auto add = [&m](int from, Edge e) {
        if (from >= 0 && e.to >= 0 && from != e.to) {
            m.edges[static_cast<std::size_t>(from)].push_back(e);
        }
    };
    // Ladders: up from the floor under the ladder's foot to the platform at its top, and down.
    for (std::size_t li = 0; li < m.ladders.size(); li++) {
        const Ladder &l = m.ladders[li];
        const int top = (m.at(l.c, l.r0) & k_foothold) != 0
                            ? m.platform_at(l.x(), l.r0 * k_cell - 1, 0)
                            : m.platform_at(l.x(), l.r0 * k_cell - 1, 3 * k_cell);
        const int foot = m.platform_below(l.x(), l.bottom() - 2);
        if (foot < 0) {
            continue;
        }
        const int gap = m.platforms[static_cast<std::size_t>(foot)].y() - l.bottom();
        if (gap > k_ladder_reach) { // hangs too high to grab from the floor
            continue;
        }
        const int cost = (l.r1 - l.r0) * 3 + 20;
        add(foot, {top, Move::climb_up, l.x(), 0, static_cast<int>(li), cost});
        add(top, {foot, Move::climb_down, l.x(), 0, static_cast<int>(li), cost});
    }
    for (int i = 0; i < n; i++) {
        const Platform &p = m.platforms[static_cast<std::size_t>(i)];
        // Falls off each end, unless a wall stands there. fall_to: where each end's fall lands
        // (left, right), -1 for none.
        int fall_to[2] = {-1, -1};
        for (const int dir : {-1, 1}) {
            const int c = dir < 0 ? p.c0 - 1 : p.c1 + 1;
            // A wall over the end cell itself, with a foothold on top of it two cells up: walking
            // on, the game steps the hero up onto that foothold (3.6: platform 2's left end lies
            // under platform 0's foothold-and-wall corner; the hero walked up, never off).
            const int end = dir < 0 ? p.c0 : p.c1;
            if (p.row >= 2 && m.blocked(end, p.row - 1) && m.standable(end, p.row - 2)) {
                const int up =
                    m.platform_at(end * k_cell + k_cell / 2, (p.row - 2) * k_cell - 1, 0);
                add(i, {up, Move::walk, dir < 0 ? p.x0() + 2 : p.x1() - 2, dir, -1, 4});
                continue;
            }
            if (c < 0 || c >= m.w || m.blocked(c, p.row - 1) || m.blocked(c, p.row - 2) ||
                m.standable(c, p.row - 1)) { // a wall, or a stair up (the stair edge)
                continue;
            }
            const int edge_x = dir < 0 ? p.x0() : p.x1();
            for (const int drift : {k_cell, k_fall_reach}) {
                const int to = m.platform_below(edge_x + dir * drift, p.y());
                if (to >= 0 && to != i) {
                    const int drop = m.platforms[static_cast<std::size_t>(to)].y() - p.y();
                    fall_to[dir < 0 ? 0 : 1] = to;
                    add(i, {to, Move::fall, edge_x - dir * 2, dir, -1, 10 + drop / 8});
                    break;
                }
            }
        }
        // Jumps onto a platform above (through it: footholds are one-way) or across a gap.
        for (int j = 0; j < n; j++) {
            if (j == i) {
                continue;
            }
            const Platform &q = m.platforms[static_cast<std::size_t>(j)];
            const int rise = p.y() - q.y();
            if (rise > k_jump_up || rise < -k_cell * 40) {
                continue;
            }
            // A stair: q one cell higher, touching or over p's end. Walking on climbs it.
            if (rise > 0 && rise <= k_cell && q.c1 >= p.c0 - 1 && q.c0 <= p.c1 + 1 &&
                (q.c0 <= p.c0 || q.c1 >= p.c1)) {
                const int dir = q.c0 <= p.c0 ? -1 : 1;
                add(i, {j, Move::walk, dir < 0 ? p.x0() + 2 : p.x1() - 2, dir, -1, 2});
                continue;
            }
            // Overlap: a straight jump up from under q (q above p only).
            const int lo = std::max(p.x0(), q.x0()) + 6, hi = std::min(p.x1(), q.x1()) - 6;
            if (rise > 4 && lo <= hi) {
                // No wall in the way of the jump.
                const int x = (lo + hi) / 2;
                bool clear = true;
                for (int r = q.row; r < p.row; r++) {
                    clear = clear && !m.blocked(x / k_cell, r);
                }
                if (clear) {
                    add(i, {j, Move::jump, x, 0, -1, 30 + rise / 4});
                }
                continue;
            }
            // Side by side: a running jump from p's end to q's near end.
            for (const int dir : {-1, 1}) {
                const int from_x = dir < 0 ? p.x0() + 4 : p.x1() - 4;
                const int land_x = dir < 0 ? q.x1() : q.x0();
                const int gap = (land_x - from_x) * dir;
                if (gap < -4 || gap > k_jump_side || rise < -k_cell * 40) {
                    continue;
                }
                // A drop down: the fall edge covers it, unless that fall lands elsewhere (3.4:
                // platform 0's left end is 9 px from platform 1, 16 px lower; the fall one cell
                // out passes it and lands on 6, so 1 and the portal on 3 had no way in).
                if (rise <= 4 && gap < k_cell * 2 && fall_to[dir < 0 ? 0 : 1] == j) {
                    continue;
                }
                if (rise <= 4 && gap < 0) {
                    continue;
                }
                // No wall between (one cell above p's floor up to the jump's top).
                bool clear = true;
                for (int x = from_x; (land_x - x) * dir > 0 && clear; x += dir * 4) {
                    for (int r = std::min(p.row, q.row) - 6; r < std::max(p.row, q.row); r++) {
                        if (r >= 0 && m.blocked(x / k_cell, r) &&
                            !(r == q.row && (m.at(x / k_cell, r) & k_foothold))) {
                            clear = false;
                        }
                    }
                }
                // The hero comes down on q, not on a platform above it in the way.
                const int land_in = land_x + dir * 2;
                clear = clear && m.platform_below(land_in, std::min(p.y(), q.y()) - k_cell) == j;
                if (clear) {
                    add(i, {j, Move::jump, from_x, dir, -1, 30 + std::max(0, rise) / 4 + gap / 8});
                }
            }
        }
    }
}

inline Map load(int area, int map) {
    Map m;
    m.area = area;
    m.map = map;
    const std::filesystem::path dir = std::filesystem::path(MT_BUNDLE_DIR) / "map";
    const std::string stem = std::to_string(area) + std::to_string(map);
    const std::vector<u8> mapf = read_bytes(dir / (stem + ".map"));
    const std::vector<u8> att = read_bytes(dir / (stem + ".att"));
    const std::vector<u8> obj = read_bytes(dir / (stem + ".obj"));
    if (mapf.size() < 5 || att.size() < 8 || obj.empty()) {
        return m;
    }
    const int tw = static_cast<std::int16_t>(mapf[0] | mapf[1] << 8);
    const int th = static_cast<std::int16_t>(mapf[2] | mapf[3] << 8);
    m.w = 2 * tw;
    m.h = 2 * th;
    m.att.assign(static_cast<std::size_t>(m.w * m.h), k_air);
    std::size_t i = 0;
    for (std::size_t pos = 8; pos + 1 < att.size(); pos += 2) {
        for (int k = 0; k < att[pos + 1] && i < m.att.size(); k++) {
            m.att[i++] = att[pos];
        }
    }
    // Objects: u8 count, then i16 cell, u8 kind, u8 len, name.
    std::size_t pos = 1;
    for (int k = 0; k < obj[0] && pos + 4 <= obj.size(); k++) {
        const int cell = static_cast<std::int16_t>(obj[pos] | obj[pos + 1] << 8);
        const int kind = obj[pos + 2];
        const int len = obj[pos + 3];
        std::string name(reinterpret_cast<const char *>(obj.data() + pos + 4),
                         std::min<std::size_t>(len, obj.size() - pos - 4));
        pos += 4 + static_cast<std::size_t>(len);
        if (cell == -1) {
            continue; // the map script
        }
        m.objects.push_back({kind, name, (cell % tw) * 16 + 8, (cell / tw + 1) * 16 - 1});
    }
    build(m);
    m.ok = true;
    return m;
}

inline const Map &get(int area, int map) {
    static std::map<std::pair<int, int>, std::unique_ptr<Map>> cache;
    auto &slot = cache[{area, map}];
    if (!slot) {
        slot = std::make_unique<Map>(load(area, map));
    }
    return *slot;
}

// The cheapest list of moves from platform `from` to platform `to` (Dijkstra; walking along a
// platform costs its distance / 8). Empty when there is no way or from == to.
struct Step {
    Edge edge;
    int from = 0;
};
inline std::vector<Step> route(const Map &m, int from, int from_x, int to) {
    const int n = static_cast<int>(m.platforms.size());
    if (from < 0 || to < 0 || from >= n || to >= n || from == to) {
        return {};
    }
    std::vector<int> dist(static_cast<std::size_t>(n), INT_MAX), at_x(static_cast<std::size_t>(n));
    std::vector<std::pair<int, int>> prev(static_cast<std::size_t>(n), {-1, -1});
    using Q = std::pair<int, int>;
    std::priority_queue<Q, std::vector<Q>, std::greater<Q>> q;
    dist[static_cast<std::size_t>(from)] = 0;
    at_x[static_cast<std::size_t>(from)] = from_x;
    q.push({0, from});
    while (!q.empty()) {
        const auto [d, u] = q.top();
        q.pop();
        if (d != dist[static_cast<std::size_t>(u)]) {
            continue;
        }
        const std::vector<Edge> &es = m.edges[static_cast<std::size_t>(u)];
        for (std::size_t k = 0; k < es.size(); k++) {
            const Edge &e = es[k];
            const int walk = std::abs(e.x - at_x[static_cast<std::size_t>(u)]) / 8;
            const int nd = d + walk + e.cost;
            if (nd < dist[static_cast<std::size_t>(e.to)]) {
                dist[static_cast<std::size_t>(e.to)] = nd;
                prev[static_cast<std::size_t>(e.to)] = {u, static_cast<int>(k)};
                int land = e.x;
                if (e.move == Move::climb_up || e.move == Move::climb_down) {
                    land = e.x;
                } else if (e.move == Move::fall) {
                    land = e.x + e.dir * k_fall_reach;
                } else if (e.move == Move::jump) {
                    land = e.x + e.dir * k_jump_side / 2;
                }
                at_x[static_cast<std::size_t>(e.to)] = land;
                q.push({nd, e.to});
            }
        }
    }
    if (dist[static_cast<std::size_t>(to)] == INT_MAX) {
        return {};
    }
    std::vector<Step> out;
    for (int v = to; v != from;) {
        const auto [u, k] = prev[static_cast<std::size_t>(v)];
        out.push_back({m.edges[static_cast<std::size_t>(u)][static_cast<std::size_t>(k)], u});
        v = u;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

// Chapter 1's portal links (script/chapter1 and script/common portal scripts: changeMap(area,
// map, portal)): from map (area, map), the portal object of `kind` leads to (to_area, to_map).
// Portals whose script checks a quest (portal201 main quest 9, portal240 side quest 7,
// portal212 side quest 25, portal371 main quest 18) are listed; the bot only takes them once
// the quest allows it.
struct Link {
    int area, map, kind, to_area, to_map;
};
inline const std::vector<Link> &links() {
    static const std::vector<Link> k = {
        {0, 0, 0, 0, 1}, {0, 0, 1, 0, 3}, {0, 1, 1, 0, 2}, {0, 1, 0, 0, 0}, {0, 2, 1, 1, 1},
        {0, 2, 0, 0, 1}, {0, 3, 0, 0, 0}, {0, 3, 1, 0, 4}, {0, 4, 0, 0, 3}, {0, 4, 1, 2, 4},
        {1, 0, 0, 1, 2}, {1, 0, 1, 1, 3}, {1, 1, 1, 1, 2}, {1, 1, 0, 0, 2}, {1, 2, 1, 1, 0},
        {1, 2, 0, 1, 1}, {1, 3, 0, 1, 0}, {1, 3, 1, 1, 4}, {1, 4, 0, 1, 3}, {1, 4, 1, 2, 1},
        {2, 0, 0, 2, 3}, {2, 0, 1, 2, 2}, {2, 1, 2, 2, 6}, {2, 1, 0, 1, 4}, {2, 1, 1, 2, 2},
        {2, 2, 1, 2, 1}, {2, 2, 0, 2, 0}, {2, 3, 0, 2, 4}, {2, 3, 1, 2, 0}, {2, 4, 3, 2, 5},
        {2, 4, 0, 0, 4}, {2, 4, 1, 2, 3}, {2, 4, 2, 3, 1}, {2, 5, 0, 2, 4}, {2, 6, 1, 2, 7},
        {2, 6, 0, 2, 1}, {2, 7, 0, 2, 6}, {3, 0, 0, 3, 2}, {3, 0, 1, 3, 3}, {3, 1, 0, 2, 4},
        {3, 1, 1, 3, 2}, {3, 2, 0, 3, 1}, {3, 2, 1, 3, 0}, {3, 3, 0, 3, 0}, {3, 3, 1, 3, 4},
        {3, 4, 0, 3, 3}, {3, 4, 1, 3, 5}, {3, 5, 0, 3, 4}, {3, 5, 1, 3, 6}, {3, 6, 0, 3, 5},
        {3, 6, 1, 3, 7}, {3, 7, 0, 3, 6}, {3, 7, 1, 3, 8},
    };
    return k;
}

// The portal kinds to take from (area, map) to reach (to_area, to_map): the first hop of a
// breadth-first search over links() that skips the `closed` links. -1 when unreachable.
inline int next_portal(int area, int map, int to_area, int to_map,
                       const std::vector<std::pair<int, int>> &closed = {}) {
    using M = std::pair<int, int>;
    std::map<M, std::pair<M, int>> prev;
    std::deque<M> q{{area, map}};
    prev[{area, map}] = {{-1, -1}, -1};
    while (!q.empty()) {
        const M u = q.front();
        q.pop_front();
        if (u == M{to_area, to_map}) {
            M v = u;
            int kind = -1;
            while (prev[v].first != M{-1, -1}) {
                kind = prev[v].second;
                v = prev[v].first;
            }
            return kind;
        }
        for (const Link &l : links()) {
            const M from{l.area, l.map}, to{l.to_area, l.to_map};
            if (from != u || prev.count(to) ||
                std::find(closed.begin(), closed.end(), M{l.area * 100 + l.map, l.kind}) !=
                    closed.end()) {
                continue;
            }
            prev[to] = {u, l.kind};
            q.push_back(to);
        }
    }
    return -1;
}

inline std::string describe(const Map &m) {
    std::string s;
    char buf[160];
    for (std::size_t i = 0; i < m.platforms.size(); i++) {
        const Platform &p = m.platforms[i];
        std::snprintf(buf, sizeof buf, "platform %zu: y %d x %d..%d:", i, p.y(), p.x0(), p.x1());
        s += buf;
        for (const Edge &e : m.edges[i]) {
            std::snprintf(buf, sizeof buf, " [%s@%d%s -> %d]", move_name(e.move), e.x,
                          e.dir < 0 ? "<" : (e.dir > 0 ? ">" : ""), e.to);
            s += buf;
        }
        s += "\n";
    }
    return s;
}

} // namespace world_map

#endif // MAPLETHIEF_TESTS_WORLD_MAP_H
