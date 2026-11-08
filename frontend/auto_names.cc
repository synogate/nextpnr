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

#include "auto_names.h"

#include <algorithm>
#include <cctype>
#include <tuple>

#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

// "foo[3]" -> "foo"
std::string strip_index(const std::string &name)
{
    if (name.empty() || name.back() != ']')
        return name;
    size_t open = name.rfind('[');
    return open == std::string::npos ? name : name.substr(0, open);
}

bool all_digits(const std::string &s, size_t from, size_t to)
{
    if (from >= to)
        return false;
    for (size_t i = from; i < to; i++)
        if (!std::isdigit(static_cast<unsigned char>(s[i])))
            return false;
    return true;
}

// How good a net name is, lower is better:
//  0 a name from the HDL
//  1 a placeholder a generator wrote into the HDL ("s_unnamed_5", "in_unnamed_2")
//  2 a name a tool made up for an expression (GHDL "n1234_o", yosys "_0123_")
//  3 private (has a '$')
int name_quality(const std::string &name)
{
    if (name.empty() || name.find('$') != std::string::npos)
        return 3;
    std::string base = strip_index(name);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos)
        base = base.substr(dot + 1);
    // GHDL: n<digits>_<letters>
    if (base.size() >= 4 && base[0] == 'n') {
        size_t us = base.find('_');
        if (us != std::string::npos && all_digits(base, 1, us) && us + 1 < base.size() &&
            std::all_of(base.begin() + us + 1, base.end(), [](char c) { return c >= 'a' && c <= 'z'; }))
            return 2;
    }
    // yosys: _<digits>_
    if (base.size() >= 3 && base.front() == '_' && base.back() == '_' && all_digits(base, 1, base.size() - 1))
        return 2;
    if (base.find("unnamed") != std::string::npos)
        return 1;
    return 0;
}

struct AutoNamer
{
    Context *ctx;
    explicit AutoNamer(Context *ctx) : ctx(ctx) {}

    // Order of preference for the names of a net: a top level port, the quality above, higher up in the hierarchy,
    // alphabetical
    std::tuple<int, int, long, std::string> net_key(IdString name) const
    {
        const std::string &s = name.str(ctx);
        return std::make_tuple(ctx->ports.count(name) ? 0 : 1, name_quality(s), long(std::count(s.begin(), s.end(), '.')),
                               s);
    }

    int rename_nets()
    {
        std::vector<std::pair<NetInfo *, IdString>> todo;
        for (auto &n : ctx->nets) {
            NetInfo *ni = n.second.get();
            IdString best = ni->name;
            auto best_key = net_key(best);
            for (IdString alias : ni->aliases) {
                if (alias == best || name_quality(alias.str(ctx)) == 3)
                    continue;
                auto key = net_key(alias);
                if (key < best_key) {
                    best = alias;
                    best_key = key;
                }
            }
            if (best != ni->name)
                todo.emplace_back(ni, best);
        }
        int renamed = 0;
        for (auto &t : todo) {
            NetInfo *ni = t.first;
            IdString old_name = ni->name;
            if (ctx->nets.count(t.second))
                continue;
            ctx->renameNet(old_name, t.second);
            if (std::find(ni->aliases.begin(), ni->aliases.end(), old_name) == ni->aliases.end())
                ni->aliases.push_back(old_name);
            for (IdString alias : ni->aliases)
                ctx->net_aliases[alias] = ni->name;
            for (auto &h : ctx->hierarchy) {
                auto found = h.second.nets_by_gname.find(old_name);
                if (found == h.second.nets_by_gname.end())
                    continue;
                IdString local = found->second;
                h.second.nets_by_gname.erase(found);
                h.second.nets_by_gname[ni->name] = local;
                h.second.nets[local] = ni->name;
            }
            ++renamed;
        }
        return renamed;
    }

    // A cell yosys named automatically: its name inside its module starts with '$'
    bool is_auto(const CellInfo *ci) const
    {
        auto h = ctx->hierarchy.find(ci->hierpath);
        if (h != ctx->hierarchy.end()) {
            auto local = h->second.leaf_cells_by_gname.find(ci->name);
            if (local != h->second.leaf_cells_by_gname.end())
                return local->second.c_str(ctx)[0] == '$';
        }
        return ci->name.c_str(ctx)[0] == '$';
    }

    // The best public name of the nets the cell drives; empty if they are all private. A cell that drives several
    // bits of a bus (a multiplier) is named after the bus. A cell on a top level port is named after the port.
    std::string direct_name(const CellInfo *ci) const
    {
        std::vector<const NetInfo *> outs;
        for (auto &port : ci->ports) {
            const NetInfo *net = port.second.net;
            if (net == nullptr)
                continue;
            // an IO buffer is named after its pad: it drives the port net, or is all the port net feeds
            if (ctx->ports.count(net->name) && (port.second.type != PORT_IN || net->users.entries() == 1))
                return net->name.str(ctx);
            if (port.second.type == PORT_OUT)
                outs.push_back(net);
        }
        const NetInfo *best = nullptr;
        std::tuple<int, int, long, std::string> best_key;
        for (const NetInfo *net : outs) {
            if (name_quality(net->name.str(ctx)) == 3)
                continue;
            auto key = net_key(net->name);
            if (best == nullptr || key < best_key) {
                best = net;
                best_key = key;
            }
        }
        if (best == nullptr)
            return "";
        const std::string &name = best->name.str(ctx);
        std::string base = strip_index(name);
        if (base != name)
            for (const NetInfo *net : outs)
                if (net != best && strip_index(net->name.str(ctx)) == base)
                    return base;
        return name;
    }

    // "<sink>$<port>"; a chain through the same port (a carry chain) counts up instead: "x$CI", "x$CIx2", "x$CIx3"
    static std::string derive(const std::string &sink, const std::string &port)
    {
        std::string tail = "$" + port;
        if (sink.size() >= tail.size() && sink.compare(sink.size() - tail.size(), tail.size(), tail) == 0)
            return sink + "x2";
        size_t pos = sink.rfind(tail + "x");
        if (pos != std::string::npos) {
            size_t num = pos + tail.size() + 1;
            if (all_digits(sink, num, sink.size()))
                return sink.substr(0, num) + std::to_string(std::stol(sink.substr(num)) + 1);
        }
        return sink + tail;
    }

    // "CC_DFF" -> "dff", "$lut" -> "lut"
    std::string type_suffix(const CellInfo *ci) const
    {
        std::string type = ci->type.str(ctx);
        if (!type.empty() && type[0] == '$')
            type = type.substr(1);
        size_t us = type.find('_');
        if (us != std::string::npos && us + 1 < type.size())
            type = type.substr(us + 1);
        for (auto &c : type)
            c = std::tolower(static_cast<unsigned char>(c));
        return type;
    }

    IdString uniquify(const std::string &name, const CellInfo *ci, pool<IdString> &taken) const
    {
        auto is_free = [&](IdString id) { return !taken.count(id) && !ctx->cells.count(id); };
        IdString id = ctx->id(name);
        if (!is_free(id)) {
            std::string typed = name + "$" + type_suffix(ci);
            id = ctx->id(typed);
            for (int i = 2; !is_free(id); i++)
                id = ctx->id(typed + "$" + std::to_string(i));
        }
        taken.insert(id);
        return id;
    }

    void run()
    {
        int nets_renamed = rename_nets();

        pool<IdString> taken, auto_cells;
        std::vector<CellInfo *> autos;
        for (auto &cell : ctx->cells) {
            if (is_auto(cell.second.get())) {
                autos.push_back(cell.second.get());
                auto_cells.insert(cell.first);
            } else {
                taken.insert(cell.first);
            }
        }

        // old name -> new name, and how many cells away the cell is from one named after its own net
        dict<IdString, IdString> new_name;
        dict<IdString, int> level;
        std::vector<CellInfo *> pending;
        for (CellInfo *ci : autos) {
            std::string name = direct_name(ci);
            if (name.empty()) {
                pending.push_back(ci);
            } else {
                new_name[ci->name] = uniquify(name, ci, taken);
                level[ci->name] = 0;
            }
        }
        int direct = int(new_name.size());

        // The rest is named after the closest named cell it feeds, round by round outwards
        for (int round = 1; !pending.empty(); round++) {
            std::vector<std::pair<CellInfo *, std::string>> assigned;
            std::vector<CellInfo *> rest;
            for (CellInfo *ci : pending) {
                bool found = false;
                std::tuple<int, std::string, std::string> best;
                for (auto &port : ci->ports) {
                    if (port.second.type != PORT_OUT || port.second.net == nullptr)
                        continue;
                    for (auto &usr : port.second.net->users) {
                        if (usr.cell == nullptr || usr.cell == ci)
                            continue;
                        std::tuple<int, std::string, std::string> cand;
                        if (!auto_cells.count(usr.cell->name)) {
                            cand = std::make_tuple(0, usr.cell->name.str(ctx), usr.port.str(ctx));
                        } else {
                            auto named = new_name.find(usr.cell->name);
                            if (named == new_name.end())
                                continue;
                            cand = std::make_tuple(level.at(usr.cell->name), named->second.str(ctx), usr.port.str(ctx));
                        }
                        if (!found || cand < best) {
                            best = cand;
                            found = true;
                        }
                    }
                }
                if (found)
                    assigned.emplace_back(ci, derive(std::get<1>(best), std::get<2>(best)));
                else
                    rest.push_back(ci);
            }
            if (assigned.empty())
                break;
            for (auto &a : assigned) {
                new_name[a.first->name] = uniquify(a.second, a.first, taken);
                level[a.first->name] = round;
            }
            pending.swap(rest);
        }

        for (auto &r : new_name) {
            std::unique_ptr<CellInfo> cell = std::move(ctx->cells.at(r.first));
            ctx->cells.erase(r.first);
            cell->name = r.second;
            auto h = ctx->hierarchy.find(cell->hierpath);
            if (h != ctx->hierarchy.end()) {
                auto local = h->second.leaf_cells_by_gname.find(r.first);
                if (local != h->second.leaf_cells_by_gname.end()) {
                    h->second.leaf_cells.erase(local->second);
                    h->second.leaf_cells_by_gname.erase(local);
                }
            }
            ctx->cells[r.second] = std::move(cell);
        }
        // puts the renamed cells back into the hierarchy
        ctx->fixupHierarchy();

        log_info("Renamed %d of %d automatically named cells after the nets they drive (%d directly, %d after the "
                 "cell they feed), %d nets after their best alias.\n",
                 int(new_name.size()), int(autos.size()), direct, int(new_name.size()) - direct, nets_renamed);
    }
};

} // namespace

void rename_auto_cells(Context *ctx) { AutoNamer(ctx).run(); }

NEXTPNR_NAMESPACE_END
