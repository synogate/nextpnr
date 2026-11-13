/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "design_scopes.h"
#include <algorithm>
#include <cctype>
#include <iterator>

NEXTPNR_NAMESPACE_BEGIN

namespace {

// A net visible in more instances than this (clocks, resets) says nothing about where a cell is
constexpr int MAX_NET_SCOPES = 8;
// Neighbours over nets with more pins than this do not count
constexpr int MAX_NEIGHBOUR_PINS = 16;
constexpr int MAX_NEIGHBOUR_ROUNDS = 64;

// The path components of a dotted name without the last one; false for names that are not those of
// a net or cell declared in an instance: private names ("$..."), names made by tools that have a '$'
// in an instance name ("x$mult_lower$cpu.mul.p"). A component starting with a digit is an index into
// the one before it, a memory or array ("cpu.regs.3.1" is in "cpu").
bool name_scope(const std::string &name, std::vector<std::string> &out)
{
    out.clear();
    if (name.empty() || name[0] == '$')
        return false;
    std::vector<std::string> parts;
    std::string part;
    int bracket = 0;
    for (char c : name) {
        if (c == '[')
            bracket++;
        else if (c == ']' && bracket > 0)
            bracket--;
        if (c == '.' && bracket == 0) {
            parts.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    for (size_t i = 0; i < parts.size(); i++) {
        const std::string &p = parts[i];
        if (p.empty() || p.find('$') != std::string::npos)
            return false;
        if (std::isdigit((unsigned char)p[0])) {
            if (!out.empty())
                out.pop_back();
            break;
        }
        out.push_back(p);
    }
    return true;
}

std::vector<std::string> split(const std::string &s, char sep)
{
    std::vector<std::string> out;
    std::string part;
    for (char c : s) {
        if (c == sep) {
            if (!part.empty())
                out.push_back(part);
            part.clear();
        } else {
            part += c;
        }
    }
    if (!part.empty())
        out.push_back(part);
    return out;
}

// "fifo2" before "fifo10"
bool natural_less(const std::string &a, const std::string &b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t i1 = i, j1 = j;
            while (i1 < a.size() && std::isdigit((unsigned char)a[i1]))
                i1++;
            while (j1 < b.size() && std::isdigit((unsigned char)b[j1]))
                j1++;
            std::string na = a.substr(i, i1 - i), nb = b.substr(j, j1 - j);
            na.erase(0, std::min(na.find_first_not_of('0'), na.size()));
            nb.erase(0, std::min(nb.find_first_not_of('0'), nb.size()));
            if (na.size() != nb.size())
                return na.size() < nb.size();
            if (na != nb)
                return na < nb;
            i = i1;
            j = j1;
        } else {
            if (a[i] != b[j])
                return a[i] < b[j];
            i++;
            j++;
        }
    }
    return a.size() - i < b.size() - j;
}

} // namespace

std::vector<std::string> DesignScopes::scopeOfName(const std::string &name)
{
    std::vector<std::string> out;
    if (!name_scope(name, out))
        out.clear();
    return out;
}

int DesignScopes::intern(const std::vector<std::string> &path, const std::string &type)
{
    int idx = 0;
    std::string key;
    for (size_t i = 0; i < path.size(); i++) {
        key += (i ? "." : "") + path[i];
        auto found = by_path.find(key);
        if (found != by_path.end()) {
            idx = found->second;
            continue;
        }
        Scope s;
        s.name = path[i];
        s.path = key;
        s.parent = idx;
        s.depth = int(i) + 1;
        int next = int(scopes.size());
        scopes.push_back(s);
        by_path[key] = next;
        idx = next;
    }
    if (!type.empty() && scopes[idx].type.empty())
        scopes[idx].type = type;
    return idx;
}

int DesignScopes::find(const std::string &path) const
{
    if (path.empty())
        return scopes.empty() ? -1 : 0;
    auto found = by_path.find(path);
    return found == by_path.end() ? -1 : found->second;
}

bool DesignScopes::contains(int ancestor, int scope) const
{
    for (int s = scope; s != -1; s = scopes[s].parent)
        if (s == ancestor)
            return true;
    return false;
}

int DesignScopes::common(int a, int b) const
{
    while (scopes[a].depth > scopes[b].depth)
        a = scopes[a].parent;
    while (scopes[b].depth > scopes[a].depth)
        b = scopes[b].parent;
    while (a != b) {
        a = scopes[a].parent;
        b = scopes[b].parent;
    }
    return a;
}

std::vector<IdString> DesignScopes::cellsBelow(int scope) const
{
    std::vector<IdString> out;
    std::vector<int> stack{scope};
    while (!stack.empty()) {
        int s = stack.back();
        stack.pop_back();
        out.insert(out.end(), scopes[s].cells.begin(), scopes[s].cells.end());
        stack.insert(stack.end(), scopes[s].children.begin(), scopes[s].children.end());
    }
    return out;
}

DesignScopes DesignScopes::build(const Context *ctx)
{
    DesignScopes ds;
    Scope top;
    top.type = ctx->top_module == IdString() ? "" : ctx->top_module.str(ctx);
    ds.scopes.push_back(top);
    ds.by_path[""] = 0;

    auto assign = [&](const CellInfo *ci, int scope, Source src) {
        ds.cell_scope[ci->name] = scope;
        ds.cell_source[ci->name] = src;
    };

    // 1. and 2.: the hierarchy nextpnr imported, the attributes yosys left
    const IdString id_scopename = ctx->id("scopename"), id_hdlname = ctx->id("hdlname");
    for (auto &c : ctx->cells) {
        const CellInfo *ci = c.second.get();
        if (ci->hierpath != IdString() && ci->hierpath != ctx->top_module && ctx->hierarchy.count(ci->hierpath)) {
            std::vector<std::string> parts = split(ci->hierpath.str(ctx), '/');
            if (parts.size() > 1) {
                parts.erase(parts.begin());
                // The module types of the instances on the way
                std::vector<std::string> prefix;
                std::string hpath = split(ci->hierpath.str(ctx), '/').front();
                int idx = 0;
                for (auto &p : parts) {
                    prefix.push_back(p);
                    hpath += "/" + p;
                    auto h = ctx->hierarchy.find(ctx->id(hpath));
                    idx = ds.intern(prefix, h == ctx->hierarchy.end() ? "" : h->second.type.str(ctx));
                }
                assign(ci, idx, SRC_HIERARCHY);
                continue;
            }
        }
        auto sn = ci->attrs.find(id_scopename);
        if (sn != ci->attrs.end() && sn->second.is_string) {
            std::vector<std::string> parts = split(sn->second.as_string(), ' ');
            if (!parts.empty()) {
                assign(ci, ds.intern(parts), SRC_ATTRIBUTE);
                continue;
            }
        }
        auto hn = ci->attrs.find(id_hdlname);
        if (hn != ci->attrs.end() && hn->second.is_string) {
            std::vector<std::string> parts = split(hn->second.as_string(), ' ');
            if (parts.size() > 1) {
                parts.pop_back();
                assign(ci, ds.intern(parts), SRC_ATTRIBUTE);
                continue;
            }
        }
    }

    // 3.: the instances in which the nets of a cell are visible
    dict<IdString, std::vector<int>> net_scopes;
    std::vector<std::string> parts;
    for (auto &n : ctx->nets) {
        const NetInfo *ni = n.second.get();
        std::vector<int> found;
        auto add = [&](IdString name) {
            if (name_scope(name.str(ctx), parts)) {
                int s = ds.intern(parts);
                if (std::find(found.begin(), found.end(), s) == found.end())
                    found.push_back(s);
            }
        };
        add(ni->name);
        for (IdString alias : ni->aliases)
            if (alias != ni->name)
                add(alias);
        if (!found.empty()) {
            std::sort(found.begin(), found.end());
            net_scopes[ni->name] = std::move(found);
        }
    }
    // Ties: on one line of descent the deepest instance (a net passed down into it); in different
    // branches the instance they have in common (a net fanning out to several instances, such as a
    // clock or a reset from the top level)
    auto resolve = [&](const std::vector<int> &tied) {
        int best = tied.front();
        for (int t : tied)
            if (ds.scopes[t].depth > ds.scopes[best].depth)
                best = t;
        bool line = std::all_of(tied.begin(), tied.end(), [&](int t) { return ds.contains(t, best); });
        if (!line)
            for (int t : tied)
                best = ds.common(best, t);
        return best;
    };
    auto best_of = [&](const dict<int, int> &score) {
        int best_score = 0;
        for (auto &sc : score)
            best_score = std::max(best_score, sc.second);
        std::vector<int> tied;
        for (auto &sc : score)
            if (sc.second == best_score && best_score > 0)
                tied.push_back(sc.first);
        return tied.empty() ? -1 : resolve(tied);
    };
    // A cell is in an instance in which all its named nets are visible. Where that leaves several on
    // one line of descent, the deepest; in different branches, the neighbours decide among them;
    // where it leaves none (a name lost on the way), the instance in which most of them are visible.
    dict<IdString, std::vector<int>> candidates;
    for (auto &c : ctx->cells) {
        const CellInfo *ci = c.second.get();
        if (ds.cell_scope.count(ci->name))
            continue;
        std::vector<int> allowed;
        bool any = false;
        dict<int, int> score;
        pool<IdString> seen;
        for (auto &port : ci->ports) {
            if (port.second.net == nullptr || !seen.insert(port.second.net->name).second)
                continue;
            auto ns = net_scopes.find(port.second.net->name);
            if (ns == net_scopes.end())
                continue;
            if (!any) {
                allowed = ns->second;
                any = true;
            } else {
                std::vector<int> both;
                std::set_intersection(allowed.begin(), allowed.end(), ns->second.begin(), ns->second.end(),
                                      std::back_inserter(both));
                allowed = std::move(both);
            }
            // (the net a cell drives is most often declared where the cell is)
            if (int(ns->second.size()) <= MAX_NET_SCOPES)
                for (int s : ns->second)
                    score[s] += port.second.type == PORT_OUT ? 2 : 1;
        }
        if (!any)
            continue;
        if (allowed.size() == 1) {
            assign(ci, allowed.front(), SRC_NETS);
        } else if (allowed.size() > 1) {
            // On one line of descent: the deepest (the ports of an instance are seen above it too)
            int deepest = resolve(allowed);
            if (std::all_of(allowed.begin(), allowed.end(), [&](int t) { return ds.contains(t, deepest); }))
                assign(ci, deepest, SRC_NETS);
            else
                candidates[ci->name] = std::move(allowed);
        } else {
            int best = best_of(score);
            if (best != -1)
                assign(ci, best, SRC_NETS);
        }
    }

    // 4.: the neighbours, in rounds (each round sees only what the rounds before decided), within the
    // instances a cell's nets allow: a neighbour's instance counts as the deepest of them it is in
    for (int round = 0; round < MAX_NEIGHBOUR_ROUNDS; round++) {
        std::vector<std::pair<const CellInfo *, int>> decided;
        for (auto &c : ctx->cells) {
            const CellInfo *ci = c.second.get();
            if (ds.cell_scope.count(ci->name))
                continue;
            auto cand = candidates.find(ci->name);
            dict<int, int> score;
            pool<IdString> seen;
            for (auto &port : ci->ports) {
                const NetInfo *ni = port.second.net;
                if (ni == nullptr || !seen.insert(ni->name).second || int(ni->users.entries()) + 1 > MAX_NEIGHBOUR_PINS)
                    continue;
                auto vote = [&](const CellInfo *other) {
                    if (other == nullptr || other == ci)
                        return;
                    auto s = ds.cell_scope.find(other->name);
                    if (s == ds.cell_scope.end())
                        return;
                    int scope = s->second;
                    if (cand != candidates.end()) {
                        int in = -1;
                        for (int t : cand->second)
                            if (ds.contains(t, scope) && (in == -1 || ds.scopes[t].depth > ds.scopes[in].depth))
                                in = t;
                        scope = in;
                    }
                    if (scope != -1)
                        score[scope]++;
                };
                vote(ni->driver.cell);
                for (auto &usr : ni->users)
                    vote(usr.cell);
            }
            int best = best_of(score);
            if (best != -1)
                decided.emplace_back(ci, best);
        }
        if (decided.empty())
            break;
        for (auto &d : decided)
            assign(d.first, d.second, SRC_NEIGHBOURS);
    }
    // Neither a single instance nor neighbours: as the nets allow
    for (auto &cand : candidates)
        if (!ds.cell_scope.count(cand.first))
            assign(ctx->cells.at(cand.first).get(), resolve(cand.second), SRC_NETS);
    for (auto &c : ctx->cells)
        if (!ds.cell_scope.count(c.first))
            assign(c.second.get(), 0, SRC_NONE);

    // Totals, then without the instances no cell is in
    std::vector<int> total(ds.scopes.size(), 0);
    for (auto &cs : ds.cell_scope)
        total[cs.second]++;
    for (int s = int(ds.scopes.size()) - 1; s > 0; s--) // parents come before their children
        total[ds.scopes[s].parent] += total[s];
    std::vector<int> remap(ds.scopes.size(), -1);
    std::vector<Scope> kept;
    for (int s = 0; s < int(ds.scopes.size()); s++) {
        if (s != 0 && total[s] == 0)
            continue;
        remap[s] = int(kept.size());
        kept.push_back(ds.scopes[s]);
        kept.back().total = total[s];
        if (s != 0)
            kept.back().parent = remap[ds.scopes[s].parent];
    }
    ds.scopes = std::move(kept);
    ds.by_path.clear();
    for (int s = 0; s < int(ds.scopes.size()); s++) {
        ds.by_path[ds.scopes[s].path] = s;
        if (s != 0)
            ds.scopes[ds.scopes[s].parent].children.push_back(s);
    }
    for (auto &cs : ds.cell_scope) {
        cs.second = remap[cs.second];
        ds.scopes[cs.second].cells.push_back(cs.first);
    }
    for (auto &src : ds.cell_source)
        ds.source_count[src.second]++;
    for (auto &s : ds.scopes)
        std::sort(s.children.begin(), s.children.end(),
                  [&](int a, int b) { return natural_less(ds.scopes[a].name, ds.scopes[b].name); });
    return ds;
}

BelCensus BelCensus::build(const Context *ctx)
{
    BelCensus bc;
    for (BelId bel : ctx->getBels()) {
        auto &t = bc.types[ctx->getBelType(bel).str(ctx)];
        t.total++;
        Loc l = ctx->getBelLocation(bel);
        t.sites.insert(std::make_pair(l.x, l.y));
    }
    return bc;
}

int BelCensus::total(const std::string &type) const
{
    auto t = types.find(type);
    return t == types.end() ? 0 : t->second.total;
}

int BelCensus::sites(const std::string &type) const
{
    auto t = types.find(type);
    return t == types.end() ? 0 : int(t->second.sites.size());
}

int BelCensus::sitesIn(const std::string &type, const BoundingBox &box) const
{
    auto t = types.find(type);
    if (t == types.end())
        return 0;
    int n = 0;
    for (auto &xy : t->second.sites)
        if (xy.first >= box.x0 && xy.first <= box.x1 && xy.second >= box.y0 && xy.second <= box.y1)
            n++;
    return n;
}

int BelCensus::sitesOf(const std::string &type, const pool<std::pair<int, int>> &used) const
{
    auto t = types.find(type);
    if (t == types.end())
        return 0;
    int n = 0;
    for (auto &xy : used)
        if (t->second.sites.count(xy))
            n++;
    return n;
}

std::string ScopeUsage::mainBelType() const
{
    std::string best;
    int n = 0;
    for (auto &t : by_bel_type)
        if (t.second > n) {
            best = t.first;
            n = t.second;
        }
    return best;
}

ScopeUsage ScopeUsage::of(const Context *ctx, const std::vector<IdString> &cells)
{
    ScopeUsage u;
    for (IdString name : cells) {
        auto found = ctx->cells.find(name);
        if (found == ctx->cells.end())
            continue;
        const CellInfo *ci = found->second.get();
        u.cells++;
        u.regions[ci->region ? ci->region->name.str(ctx) : ""]++;
        if (ci->bel == BelId()) {
            u.unplaced_by_cell_type[ci->type.str(ctx)]++;
            continue;
        }
        u.placed++;
        u.by_bel_type[ctx->getBelType(ci->bel).str(ctx)]++;
        Loc l = ctx->getBelLocation(ci->bel);
        u.sites.insert(std::make_pair(l.x, l.y));
        if (u.placed == 1) {
            u.box = BoundingBox(l.x, l.y, l.x, l.y);
        } else {
            u.box.x0 = std::min(u.box.x0, l.x);
            u.box.y0 = std::min(u.box.y0, l.y);
            u.box.x1 = std::max(u.box.x1, l.x);
            u.box.y1 = std::max(u.box.y1, l.y);
        }
    }
    return u;
}

NEXTPNR_NAMESPACE_END
