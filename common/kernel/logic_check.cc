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

#include "logic_check.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <istream>
#include <iterator>
#include <map>
#include <ostream>
#include <sstream>

#include "json11.hpp"
#include "log.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// ---------------------------------------------------------------------------------------------------------------------
// Snapshot

namespace {
std::unique_ptr<CellInfo> copy_cell(Context *ctx, const CellInfo *ci)
{
    auto copy = std::make_unique<CellInfo>(ctx, ci->name, ci->type);
    copy->params = ci->params;
    copy->attrs = ci->attrs;
    for (auto &port : ci->ports) {
        PortInfo &p = copy->ports[port.first];
        p.name = port.first;
        p.type = port.second.type;
        p.net = nullptr;
    }
    return copy;
}
} // namespace

LogicSnapshot LogicSnapshot::take(Context *ctx)
{
    LogicSnapshot snap;
    snap.taken_at = int64_t(std::time(nullptr));
    snap.cells.reserve(ctx->cells.size());
    for (auto &cell : ctx->cells) {
        Cell c;
        c.info = copy_cell(ctx, cell.second.get());
        for (auto &port : cell.second->ports)
            if (port.second.net)
                c.port_nets[port.first] = port.second.net->name;
        snap.cells.push_back(std::move(c));
    }
    return snap;
}

void LogicSnapshot::write(std::ostream &out) const
{
    using json11::Json;
    std::vector<Json> jcells;
    for (auto &c : cells) {
        const CellInfo *ci = c.info.get();
        Context *ctx = ci->ctx;
        std::map<std::string, Json> params, attrs, ports;
        for (auto &p : ci->params)
            params[p.first.str(ctx)] = p.second.to_string();
        for (auto &a : ci->attrs)
            attrs[a.first.str(ctx)] = a.second.to_string();
        for (auto &p : ci->ports) {
            auto net = c.port_nets.find(p.first);
            ports[p.first.str(ctx)] = Json::object{
                    {"dir", int(p.second.type)},
                    {"net", net == c.port_nets.end() ? Json() : Json(net->second.str(ctx))},
            };
        }
        jcells.push_back(Json::object{
                {"name", ci->name.str(ctx)},
                {"type", ci->type.str(ctx)},
                {"params", params},
                {"attrs", attrs},
                {"ports", ports},
        });
    }
    Json doc = Json::object{
            {"nextpnr_logic_snapshot", 1},
            {"taken_at", double(taken_at)},
            {"cells", jcells},
    };
    out << doc.dump() << "\n";
}

std::string LogicSnapshot::read(Context *ctx, std::istream &in)
{
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string err;
    json11::Json doc = json11::Json::parse(text, err);
    if (!err.empty())
        return "not a JSON file: " + err;
    if (doc["nextpnr_logic_snapshot"].int_value() != 1)
        return "not a logic reference written by nextpnr";
    std::vector<Cell> read_cells;
    for (auto &jc : doc["cells"].array_items()) {
        Cell c;
        c.info = std::make_unique<CellInfo>(ctx, ctx->id(jc["name"].string_value()), ctx->id(jc["type"].string_value()));
        for (auto &p : jc["params"].object_items())
            c.info->params[ctx->id(p.first)] = Property::from_string(p.second.string_value());
        for (auto &a : jc["attrs"].object_items())
            c.info->attrs[ctx->id(a.first)] = Property::from_string(a.second.string_value());
        for (auto &p : jc["ports"].object_items()) {
            IdString port = ctx->id(p.first);
            PortInfo &pi = c.info->ports[port];
            pi.name = port;
            pi.type = PortType(p.second["dir"].int_value());
            pi.net = nullptr;
            if (p.second["net"].is_string())
                c.port_nets[port] = ctx->id(p.second["net"].string_value());
        }
        read_cells.push_back(std::move(c));
    }
    cells = std::move(read_cells);
    taken_at = int64_t(doc["taken_at"].number_value());
    return {};
}

// ---------------------------------------------------------------------------------------------------------------------
// Check

namespace {

uint64_t mix(uint64_t x)
{
    // splitmix64 finaliser
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t hash_str(const std::string &s)
{
    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned char c : s)
        h = (h ^ c) * 0x100000001b3ULL;
    return h;
}

// The netlist the check works on, the same for the design and the reference: cells with their ports' nets (by index)
struct LNet
{
    IdString name;
    int driver = -1; // cell index, -1 if none
    IdString driver_port;
    std::vector<std::pair<int, IdString>> users;
    // Free variable (not driven by a combinational output): its key ("" if driven combinationally)
    std::string var;
};

struct LCell
{
    const CellInfo *ci = nullptr;
    dict<IdString, int> port_nets; // connected ports -> net index
    bool known = false;            // evaluateCell knows it
    pool<IdString> comb_outputs;   // its combinational outputs that drive a net
    std::vector<IdString> sinks;   // sink names (for unknown cells: its connected inputs)
    bool loop = false;
};

struct LNetlist
{
    Context *ctx;
    std::vector<LCell> cells;
    std::vector<LNet> nets;
    dict<IdString, int> net_index;
    dict<IdString, int> cell_index;
    std::vector<int> order;     // the combinational cells, by components (blocks) in topological order
    std::vector<int> topo_pos;  // per cell: its position in `order`
    dict<std::string, int> var_net; // free variable key -> net
    std::vector<int> loop_cells;

    explicit LNetlist(Context *ctx) : ctx(ctx) {}

    int net(IdString name)
    {
        auto found = net_index.find(name);
        if (found != net_index.end())
            return found->second;
        int idx = int(nets.size());
        nets.emplace_back();
        nets.back().name = name;
        net_index[name] = idx;
        return idx;
    }

    void add_cell(const CellInfo *ci, const dict<IdString, IdString> &port_nets)
    {
        int idx = int(cells.size());
        cells.emplace_back();
        LCell &c = cells.back();
        c.ci = ci;
        cell_index[ci->name] = idx;
        for (auto &pn : port_nets) {
            auto port = ci->ports.find(pn.first);
            if (port == ci->ports.end())
                continue;
            int n = net(pn.second);
            c.port_nets[pn.first] = n;
            if (port->second.type == PORT_OUT && nets[n].driver == -1) {
                nets[n].driver = idx;
                nets[n].driver_port = pn.first;
            } else {
                nets[n].users.emplace_back(idx, pn.first);
            }
        }
    }

    struct Inputs : CellEvalInputs
    {
        const LCell *cell = nullptr;
        const uint64_t *values = nullptr;
        uint64_t value(IdString port, bool *connected) const override
        {
            auto found = cell->port_nets.find(port);
            if (connected)
                *connected = found != cell->port_nets.end();
            return found == cell->port_nets.end() ? 0 : values[found->second];
        }
    };

    // evaluateCell for one word of the values of all nets (indexed by net)
    bool eval(int idx, const uint64_t *values, CellEvalResult &result) const
    {
        Inputs in;
        in.cell = &cells[idx];
        in.values = values;
        result.outputs.clear();
        result.sinks.clear();
        return ctx->evaluateCell(cells[idx].ci, in, result);
    }

    // Classify cells and nets, order the combinational cells. Issues go to `issues` (loops) if given.
    void prepare(std::vector<LogicIssue> *issues)
    {
        std::vector<uint64_t> zeros(nets.size(), 0);
        CellEvalResult result;
        for (int i = 0; i < int(cells.size()); i++) {
            LCell &c = cells[i];
            c.known = eval(i, zeros.data(), result);
            if (c.known) {
                for (auto &o : result.outputs) {
                    auto pn = c.port_nets.find(o.first);
                    if (pn != c.port_nets.end() && nets[pn->second].driver == i &&
                        nets[pn->second].driver_port == o.first)
                        c.comb_outputs.insert(o.first);
                }
                for (auto &s : result.sinks)
                    c.sinks.push_back(s.first);
            } else {
                for (auto &pn : c.port_nets) {
                    auto port = c.ci->ports.find(pn.first);
                    if (port != c.ci->ports.end() && port->second.type != PORT_OUT)
                        c.sinks.push_back(pn.first);
                }
            }
            std::sort(c.sinks.begin(), c.sinks.end(),
                      [&](IdString a, IdString b) { return a.str(ctx) < b.str(ctx); });
        }
        // Order of the combinational cells: their strongly connected components (Tarjan) in topological order. A cell
        // depends on the combinational drivers of its inputs. Cells with several independent paths through them (pass
        // throughs, CP lines) can depend on each other both ways without a loop in the logic: such a component is
        // evaluated over and over until nothing changes. It is a real loop where that does not settle, or settles on
        // something that depends on where it started; then the outputs of its cells are free variables.
        auto comb_driver = [&](int n) {
            const LNet &net = nets[n];
            if (net.driver < 0 || !cells[net.driver].comb_outputs.count(net.driver_port))
                return -1;
            return net.driver;
        };
        auto deps_of = [&](int i) {
            std::vector<int> deps;
            for (auto &pn : cells[i].port_nets) {
                if (pn.first == nets[pn.second].driver_port && nets[pn.second].driver == i)
                    continue;
                int d = comb_driver(pn.second);
                if (d >= 0 && std::find(deps.begin(), deps.end(), d) == deps.end())
                    deps.push_back(d);
            }
            return deps;
        };
        build_blocks(deps_of);
        // The components that are cycles: do they settle?
        std::vector<int> loops;
        {
            std::vector<uint64_t> a(nets.size()), b(nets.size());
            uint64_t s = 0x5eed;
            for (auto &blk : blocks) {
                if (!blk.cyclic)
                    continue;
                bool settles = true;
                for (int round = 0; round < 4 && settles; round++) {
                    for (size_t n = 0; n < nets.size(); n++)
                        a[n] = b[n] = mix(s++);
                    for (int k = blk.start; k < blk.start + blk.len; k++)
                        for (IdString o : cells[order[k]].comb_outputs) {
                            a[cells[order[k]].port_nets.at(o)] = 0;
                            b[cells[order[k]].port_nets.at(o)] = ~0ULL;
                        }
                    CellEvalResult r;
                    settles = settle(blk, a.data(), r) && settle(blk, b.data(), r);
                    for (int k = blk.start; k < blk.start + blk.len && settles; k++)
                        for (IdString o : cells[order[k]].comb_outputs) {
                            int n = cells[order[k]].port_nets.at(o);
                            if (a[n] != b[n])
                                settles = false;
                        }
                }
                if (!settles)
                    for (int k = blk.start; k < blk.start + blk.len; k++)
                        loops.push_back(order[k]);
            }
        }
        for (int i : loops) {
            cells[i].loop = true;
            loop_cells.push_back(i);
            if (issues) {
                LogicIssue issue;
                issue.kind = LogicIssue::LOOP;
                issue.cell = cells[i].ci->name;
                issue.message = "cell " + cells[i].ci->name.str(ctx) +
                                " is in a combinational loop: its outputs are taken as free variables";
                issue.cone.push_back(cells[i].ci->name);
                issues->push_back(issue);
            }
            cells[i].comb_outputs.clear();
        }
        if (!loops.empty())
            build_blocks(deps_of);
        sink_cells.clear();
        for (int i = 0; i < int(cells.size()); i++)
            if (!cells[i].sinks.empty())
                sink_cells.push_back(i);
        // Free variables
        for (int n = 0; n < int(nets.size()); n++) {
            LNet &net = nets[n];
            if (net.driver >= 0 && cells[net.driver].comb_outputs.count(net.driver_port))
                continue;
            if (net.driver >= 0)
                net.var = cells[net.driver].ci->name.str(ctx) + "\x01" + net.driver_port.str(ctx);
            else
                net.var = "\x02" + net.name.str(ctx);
            var_net[net.var] = n;
        }
    }

    struct Block
    {
        int start = 0, len = 0; // in `order`
        bool cyclic = false;
    };
    std::vector<Block> blocks;
    std::vector<int> block_of;   // per cell, -1 if not combinational
    std::vector<int> sink_cells; // the cells with sinks

    // The strongly connected components of the combinational cells (Tarjan), into order, blocks and topo_pos
    template <typename F> void build_blocks(F deps_of)
    {
        order.clear();
        blocks.clear();
        int n = int(cells.size());
        std::vector<std::vector<int>> deps(n);
        for (int i = 0; i < n; i++)
            if (!cells[i].comb_outputs.empty())
                deps[i] = deps_of(i);
        std::vector<int> index(n, -1), low(n, 0), stack;
        std::vector<bool> on_stack(n, false);
        int counter = 0;
        // (iterative: the netlists can be deep)
        std::vector<std::pair<int, size_t>> call;
        for (int root = 0; root < n; root++) {
            if (cells[root].comb_outputs.empty() || index[root] >= 0)
                continue;
            call.emplace_back(root, 0);
            while (!call.empty()) {
                int v = call.back().first;
                if (index[v] < 0) {
                    index[v] = low[v] = counter++;
                    stack.push_back(v);
                    on_stack[v] = true;
                }
                size_t &next = call.back().second;
                if (next < deps[v].size()) {
                    int w = deps[v][next++];
                    if (index[w] < 0)
                        call.emplace_back(w, 0);
                    else if (on_stack[w])
                        low[v] = std::min(low[v], index[w]);
                    continue;
                }
                if (low[v] == index[v]) {
                    Block blk;
                    blk.start = int(order.size());
                    int w;
                    do {
                        w = stack.back();
                        stack.pop_back();
                        on_stack[w] = false;
                        order.push_back(w);
                    } while (w != v);
                    blk.len = int(order.size()) - blk.start;
                    blk.cyclic = blk.len > 1 || std::find(deps[v].begin(), deps[v].end(), v) != deps[v].end();
                    blocks.push_back(blk);
                }
                call.pop_back();
                if (!call.empty())
                    low[call.back().first] = std::min(low[call.back().first], low[v]);
            }
        }
        // (Tarjan emits a component after all it depends on: that is the topological order already)
        block_of.assign(n, -1);
        for (int b = 0; b < int(blocks.size()); b++)
            for (int k = blocks[b].start; k < blocks[b].start + blocks[b].len; k++)
                block_of[order[k]] = b;
        topo_pos.assign(n, -1);
        for (int k = 0; k < int(order.size()); k++)
            topo_pos[order[k]] = k;
    }

    bool in_cycle(int cell) const { return block_of[cell] >= 0 && blocks[block_of[cell]].cyclic; }

    // Evaluate the combinational outputs of a cell into values (indexed by net); returns whether anything changed
    bool eval_comb(int i, uint64_t *values, CellEvalResult &result) const
    {
        const LCell &c = cells[i];
        eval(i, values, result);
        bool changed = false;
        for (IdString o : c.comb_outputs) {
            auto v = result.outputs.find(o);
            uint64_t value = v == result.outputs.end() ? 0 : v->second;
            uint64_t &slot = values[c.port_nets.at(o)];
            changed |= slot != value;
            slot = value;
        }
        return changed;
    }

    // Evaluate a component until nothing changes (at most len + 2 passes); false if it does not settle
    bool settle(const Block &blk, uint64_t *values, CellEvalResult &result) const
    {
        int passes = blk.cyclic ? blk.len + 2 : 1;
        for (int p = 0; p < passes; p++) {
            bool changed = false;
            for (int k = blk.start; k < blk.start + blk.len; k++)
                changed |= eval_comb(order[k], values, result);
            if (!changed || !blk.cyclic)
                return true;
        }
        return false;
    }

    bool is_comb_net(int n) const { return nets[n].var.empty(); }
};

std::string sink_key(Context *ctx, IdString cell, IdString sink) { return cell.str(ctx) + "\x01" + sink.str(ctx); }

// Simulation of a whole netlist: values[net * words + w], sinks[sink * words + w]
struct Simulation
{
    int words = 0;
    std::vector<uint64_t> values;
    std::vector<uint64_t> sink_values;
    dict<std::string, int> sink_index; // sink key -> index
    std::vector<std::pair<int, IdString>> sinks; // index -> (cell, sink name)

    void run(const LNetlist &nl, int words_, uint64_t seed)
    {
        Context *ctx = nl.ctx;
        words = words_;
        size_t n_nets = nl.nets.size();
        for (int i : nl.sink_cells)
            for (IdString s : nl.cells[i].sinks) {
                sink_index[sink_key(ctx, nl.cells[i].ci->name, s)] = int(sinks.size());
                sinks.emplace_back(i, s);
            }
        values.assign(n_nets * words, 0);
        sink_values.assign(sinks.size() * words, 0);
        std::vector<uint64_t> var_seed(n_nets, 0);
        for (size_t n = 0; n < n_nets; n++)
            if (!nl.nets[n].var.empty())
                var_seed[n] = hash_str(nl.nets[n].var) ^ seed;
        std::vector<uint64_t> cur(n_nets, 0);
        CellEvalResult result;
        for (int w = 0; w < words; w++) {
            for (size_t n = 0; n < n_nets; n++)
                cur[n] = nl.nets[n].var.empty() ? 0 : mix(var_seed[n] + uint64_t(w) * 0x632be59bd9b4e019ULL);
            for (auto &blk : nl.blocks)
                nl.settle(blk, cur.data(), result); // (the cycles that do not settle were made free variables)
            int sink_base = 0;
            for (int i : nl.sink_cells) {
                const LCell &c = nl.cells[i];
                nl.eval(i, cur.data(), result);
                for (IdString s : c.sinks) {
                    uint64_t v;
                    if (c.known) {
                        auto found = result.sinks.find(s);
                        v = found == result.sinks.end() ? 0 : found->second;
                    } else {
                        auto pn = c.port_nets.find(s);
                        v = pn == c.port_nets.end() ? 0 : cur[pn->second];
                    }
                    sink_values[size_t(sink_base) * words + w] = v;
                    sink_base++;
                }
            }
            for (size_t n = 0; n < n_nets; n++)
                values[n * words + w] = cur[n];
        }
    }

    bool same_net(int n, const Simulation &other, int m) const
    {
        for (int w = 0; w < words; w++)
            if (values[size_t(n) * words + w] != other.values[size_t(m) * other.words + w])
                return false;
        return true;
    }
    // First differing (word, bit) of two sinks, or false
    bool sink_diff(int s, const Simulation &other, int t, int &word, int &bit) const
    {
        for (int w = 0; w < words; w++) {
            uint64_t d = sink_values[size_t(s) * words + w] ^ other.sink_values[size_t(t) * other.words + w];
            if (d) {
                word = w;
                bit = 0;
                while (!((d >> bit) & 1))
                    bit++;
                return true;
            }
        }
        return false;
    }
    bool net_bit(int n, int word, int bit) const { return (values[size_t(n) * words + word] >> bit) & 1; }
    bool sink_bit(int s, int word, int bit) const { return (sink_values[size_t(s) * words + word] >> bit) & 1; }
};

// A cone: the combinational cells from a root (a net, or a sink of a cell) back to the leaves (nets with a variable)
struct Cone
{
    std::vector<int> cells; // topological order
    std::vector<int> leaves; // nets
    bool ok = true;
    bool cyclic = false; // has cells of a cyclic component: evaluated until nothing changes
};

struct Prover
{
    Context *ctx;
    const LogicCheckOptions &opts;
    LNetlist &des, &ref;
    // Variables shared by both netlists: the proven pairs of nets (design net -> variable, reference net -> variable)
    std::vector<int> des_var, ref_var; // per net, -1 if not proven
    int n_vars = 0;
    std::vector<uint64_t> des_scratch, ref_scratch;

    Prover(Context *ctx, const LogicCheckOptions &opts, LNetlist &des, LNetlist &ref)
            : ctx(ctx), opts(opts), des(des), ref(ref)
    {
        des_var.assign(des.nets.size(), -1);
        ref_var.assign(ref.nets.size(), -1);
        des_scratch.assign(des.nets.size(), 0);
        ref_scratch.assign(ref.nets.size(), 0);
    }

    void pair(int d, int r)
    {
        des_var[d] = ref_var[r] = n_vars++;
    }

    // Leaves: nets with a variable (`vars`), or free variables of the netlist (with vars_only: only those). The root
    // is a net (root_net) or a sink of root_cell (root_sink): a sink of a cell evaluateCell does not know is the value
    // of the input of that name only, so of its other inputs none is in the cone.
    Cone cone(const LNetlist &nl, const std::vector<int> &vars, int root_cell, int root_net, bool vars_only,
              IdString root_sink = IdString()) const
    {
        Cone c;
        pool<int> in_cone, leaf_set;
        std::vector<int> stack{root_cell};
        in_cone.insert(root_cell);
        while (!stack.empty() && c.ok) {
            int i = stack.back();
            stack.pop_back();
            c.cells.push_back(i);
            if (int(c.cells.size()) > opts.max_cone_cells) {
                c.ok = false;
                break;
            }
            const LCell &cell = nl.cells[i];
            bool only_sink = i == root_cell && root_net < 0 && !cell.known && root_sink != IdString();
            for (auto &pn : cell.port_nets) {
                int n = pn.second;
                if (only_sink && pn.first != root_sink)
                    continue;
                if (nl.nets[n].driver == i && nl.nets[n].driver_port == pn.first)
                    continue; // its own output
                bool leaf = !nl.is_comb_net(n) || (!vars_only && vars[n] >= 0 && n != root_net);
                if (leaf) {
                    if (leaf_set.insert(n).second)
                        c.leaves.push_back(n);
                    continue;
                }
                int d = nl.nets[n].driver;
                if (in_cone.insert(d).second)
                    stack.push_back(d);
            }
        }
        std::sort(c.cells.begin(), c.cells.end(), [&](int a, int b) { return nl.topo_pos[a] < nl.topo_pos[b]; });
        for (int i : c.cells)
            c.cyclic |= nl.in_cycle(i);
        return c;
    }

    // Variable identity of a leaf: the shared variable of a proven pair, else something only this side has
    // (a free variable only in one netlist): encoded as -2 - net (design) / large offsets (reference)
    long long leaf_id(bool is_des, int n) const
    {
        int v = is_des ? des_var[n] : ref_var[n];
        if (v >= 0)
            return v;
        // A free variable with the same key in both is the same variable
        const LNetlist &nl = is_des ? des : ref;
        if (!nl.nets[n].var.empty()) {
            const LNetlist &other = is_des ? ref : des;
            auto found = other.var_net.find(nl.nets[n].var);
            if (found != other.var_net.end()) {
                int ov = is_des ? ref_var[found->second] : des_var[found->second];
                if (ov >= 0)
                    return ov;
            }
        }
        return is_des ? -2 - (long long)n : -2 - (long long)des.nets.size() - n;
    }

    enum Outcome
    {
        EQUAL,
        DIFFERENT,
        TOO_BIG
    };

    // Root: a net (root_net >= 0, driven by root_cell), or a sink of root_cell (root_sink)
    struct Root
    {
        int cell = -1, net = -1;
        IdString sink;
    };

    uint64_t root_value(const LNetlist &nl, const Root &root, std::vector<uint64_t> &scratch, CellEvalResult &result,
                        const Cone &cone) const
    {
        int passes = cone.cyclic ? int(cone.cells.size()) + 2 : 1;
        for (int p = 0; p < passes; p++) {
            bool changed = false;
            for (int i : cone.cells)
                if (!nl.cells[i].comb_outputs.empty())
                    changed |= nl.eval_comb(i, scratch.data(), result);
            if (!changed)
                break;
        }
        if (root.net >= 0)
            return scratch[root.net];
        const LCell &c = nl.cells[root.cell];
        if (!c.known) {
            auto pn = c.port_nets.find(root.sink);
            return pn == c.port_nets.end() ? 0 : scratch[pn->second];
        }
        nl.eval(root.cell, scratch.data(), result);
        auto found = result.sinks.find(root.sink);
        return found == result.sinks.end() ? 0 : found->second;
    }

    Outcome compare(const Root &dr, const Root &rr, bool vars_only, Cone *des_cone_out = nullptr)
    {
        Cone dc = cone(des, des_var, dr.cell, dr.net, vars_only, dr.sink);
        if (des_cone_out)
            *des_cone_out = dc;
        if (!dc.ok)
            return TOO_BIG;
        Cone rc = cone(ref, ref_var, rr.cell, rr.net, vars_only, rr.sink);
        if (!rc.ok)
            return TOO_BIG;
        // The variables of both cones
        std::vector<long long> ids;
        std::vector<int> des_slot(dc.leaves.size()), ref_slot(rc.leaves.size());
        auto slot = [&](long long id) {
            auto found = std::find(ids.begin(), ids.end(), id);
            if (found != ids.end())
                return int(found - ids.begin());
            ids.push_back(id);
            return int(ids.size()) - 1;
        };
        for (size_t k = 0; k < dc.leaves.size(); k++)
            des_slot[k] = slot(leaf_id(true, dc.leaves[k]));
        for (size_t k = 0; k < rc.leaves.size(); k++)
            ref_slot[k] = slot(leaf_id(false, rc.leaves[k]));
        int nv = int(ids.size());
        if (nv > opts.max_cut)
            return TOO_BIG;
        static const uint64_t masks[6] = {0xAAAAAAAAAAAAAAAAULL, 0xCCCCCCCCCCCCCCCCULL, 0xF0F0F0F0F0F0F0F0ULL,
                                          0xFF00FF00FF00FF00ULL, 0xFFFF0000FFFF0000ULL, 0xFFFFFFFF00000000ULL};
        int words = nv <= 6 ? 1 : 1 << (nv - 6);
        CellEvalResult result;
        for (int w = 0; w < words; w++) {
            auto var_word = [&](int v) -> uint64_t {
                if (v < 6)
                    return masks[v];
                return ((w >> (v - 6)) & 1) ? ~0ULL : 0ULL;
            };
            for (size_t k = 0; k < dc.leaves.size(); k++)
                des_scratch[dc.leaves[k]] = var_word(des_slot[k]);
            for (size_t k = 0; k < rc.leaves.size(); k++)
                ref_scratch[rc.leaves[k]] = var_word(ref_slot[k]);
            uint64_t a = root_value(des, dr, des_scratch, result, dc);
            uint64_t b = root_value(ref, rr, ref_scratch, result, rc);
            uint64_t valid = nv >= 6 ? ~0ULL : ((1ULL << (1 << nv)) - 1);
            if ((a ^ b) & valid)
                return DIFFERENT;
        }
        return EQUAL;
    }
};

std::string net_list(Context *ctx, const LNetlist &nl, const Simulation &sim, const std::vector<int> &nets, int word,
                     int bit)
{
    std::vector<std::string> parts;
    for (int n : nets)
        parts.push_back(nl.nets[n].name.str(ctx) + "=" + (sim.net_bit(n, word, bit) ? "1" : "0"));
    std::sort(parts.begin(), parts.end());
    std::string s;
    size_t shown = std::min<size_t>(parts.size(), 16);
    for (size_t k = 0; k < shown; k++)
        s += (k ? ", " : "") + parts[k];
    if (shown < parts.size())
        s += stringf(", ... (%d more)", int(parts.size() - shown));
    return s;
}

} // namespace

std::string LogicCheckResult::summary() const
{
    if (sinks == 0 && issues.empty())
        return "no logic to compare";
    std::string s = stringf("%d of %d sinks proven equal", proven, sinks);
    if (mismatched)
        s += stringf(", %d DIFFERENT", mismatched);
    if (unproven)
        s += stringf(", %d only simulated", unproven);
    int other = 0;
    for (auto &i : issues)
        if (i.kind != LogicIssue::MISMATCH && i.kind != LogicIssue::UNPROVEN)
            other++;
    if (other)
        s += stringf(", %d other issues", other);
    return s;
}

LogicCheckResult checkLogic(Context *ctx, const LogicSnapshot &snap, const LogicCheckOptions &opts)
{
    auto t0 = std::chrono::steady_clock::now();
    LogicCheckResult res;

    LNetlist des(ctx), ref(ctx);
    for (auto &cell : ctx->cells) {
        dict<IdString, IdString> port_nets;
        for (auto &port : cell.second->ports)
            if (port.second.net)
                port_nets[port.first] = port.second.net->name;
        des.add_cell(cell.second.get(), port_nets);
    }
    for (auto &c : snap.cells)
        ref.add_cell(c.info.get(), c.port_nets);
    des.prepare(&res.issues);
    ref.prepare(nullptr);

    Simulation dsim, rsim;
    dsim.run(des, opts.sim_words, opts.seed);
    rsim.run(ref, opts.sim_words, opts.seed);

    // Free variables of the design the reference does not have (only those something uses)
    for (int n = 0; n < int(des.nets.size()); n++) {
        const LNet &net = des.nets[n];
        if (net.var.empty() || net.users.empty() || ref.var_net.count(net.var))
            continue;
        LogicIssue issue;
        issue.kind = LogicIssue::NEW_VARIABLE;
        issue.net = net.name;
        if (net.driver >= 0) {
            issue.cell = des.cells[net.driver].ci->name;
            issue.sink = net.driver_port;
            issue.message = "net " + net.name.str(ctx) + " (from " + issue.cell.str(ctx) + "." +
                            net.driver_port.str(ctx) +
                            ") is a free variable the reference does not have: what depends on it cannot match";
            issue.cone.push_back(issue.cell);
        } else {
            issue.message = "net " + net.name.str(ctx) +
                            " has no driver and is not in the reference: what depends on it cannot match";
        }
        res.issues.push_back(issue);
    }

    Prover prover(ctx, opts, des, ref);
    // Free variables in both: proven pairs
    for (auto &v : des.var_net) {
        auto found = ref.var_net.find(v.first);
        if (found != ref.var_net.end())
            prover.pair(v.second, found->second);
    }
    // Nets of the design with a net of the same name in the reference, in topological order
    for (int i : des.order) {
        const LCell &c = des.cells[i];
        std::vector<IdString> outs(c.comb_outputs.begin(), c.comb_outputs.end());
        for (IdString o : outs) {
            int d = c.port_nets.at(o);
            auto rn = ref.net_index.find(des.nets[d].name);
            if (rn == ref.net_index.end())
                continue;
            int r = rn->second;
            if (prover.ref_var[r] >= 0 || !ref.is_comb_net(r) || !dsim.same_net(d, rsim, r))
                continue;
            Prover::Root dr{i, d, IdString()}, rr{ref.nets[r].driver, r, IdString()};
            auto outcome = prover.compare(dr, rr, false);
            if (outcome != Prover::EQUAL)
                outcome = prover.compare(dr, rr, true);
            if (outcome == Prover::EQUAL) {
                prover.pair(d, r);
                res.nets_proven++;
            }
        }
    }

    // Sinks
    pool<std::string> des_sink_keys;
    for (size_t s = 0; s < dsim.sinks.size(); s++) {
        int i = dsim.sinks[s].first;
        IdString sink = dsim.sinks[s].second;
        IdString cell = des.cells[i].ci->name;
        std::string key = sink_key(ctx, cell, sink);
        des_sink_keys.insert(key);
        auto found = rsim.sink_index.find(key);
        if (found == rsim.sink_index.end()) {
            LogicIssue issue;
            issue.kind = LogicIssue::NEW_SINK;
            issue.cell = cell;
            issue.sink = sink;
            issue.message = cell.str(ctx) + " " + sink.str(ctx) + " is not in the reference";
            issue.cone.push_back(cell);
            res.issues.push_back(issue);
            continue;
        }
        res.sinks++;
        int t = found->second;
        int ri = rsim.sinks[t].first;
        Prover::Root dr{i, -1, sink}, rr{ri, -1, sink};
        int word, bit;
        if (dsim.sink_diff(int(s), rsim, t, word, bit)) {
            Cone dc = prover.cone(des, prover.des_var, i, -1, false, sink);
            LogicIssue issue;
            issue.kind = LogicIssue::MISMATCH;
            issue.cell = cell;
            issue.sink = sink;
            issue.message = cell.str(ctx) + " " + sink.str(ctx) + ": reference " + (rsim.sink_bit(t, word, bit) ? "1" : "0") +
                            ", design " + (dsim.sink_bit(int(s), word, bit) ? "1" : "0") + " for " +
                            net_list(ctx, des, dsim, dc.leaves, word, bit);
            for (int ci : dc.cells)
                issue.cone.push_back(des.cells[ci].ci->name);
            res.issues.push_back(issue);
            res.mismatched++;
            continue;
        }
        Cone dc;
        auto outcome = prover.compare(dr, rr, false, &dc);
        if (outcome != Prover::EQUAL)
            outcome = prover.compare(dr, rr, true);
        if (outcome == Prover::EQUAL) {
            res.proven++;
            continue;
        }
        LogicIssue issue;
        issue.kind = LogicIssue::UNPROVEN;
        issue.cell = cell;
        issue.sink = sink;
        issue.message = cell.str(ctx) + " " + sink.str(ctx) +
                        stringf(": agrees in %d random patterns, not proven (", opts.sim_words * 64) +
                        (outcome == Prover::TOO_BIG ? std::string("cone too wide") : std::string("its cut nets are not independent")) +
                        stringf(", %d inputs)", int(dc.leaves.size()));
        for (int ci : dc.cells)
            issue.cone.push_back(des.cells[ci].ci->name);
        res.issues.push_back(issue);
        res.unproven++;
    }
    for (auto &k : rsim.sink_index) {
        if (des_sink_keys.count(k.first))
            continue;
        auto &s = rsim.sinks[k.second];
        LogicIssue issue;
        issue.kind = LogicIssue::MISSING_SINK;
        issue.cell = ref.cells[s.first].ci->name;
        issue.sink = s.second;
        issue.message = issue.cell.str(ctx) + " " + issue.sink.str(ctx) + " of the reference is not in the design";
        res.issues.push_back(issue);
    }
    std::stable_sort(res.issues.begin(), res.issues.end(),
                     [](const LogicIssue &a, const LogicIssue &b) { return a.kind < b.kind; });
    res.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return res;
}

NEXTPNR_NAMESPACE_END
