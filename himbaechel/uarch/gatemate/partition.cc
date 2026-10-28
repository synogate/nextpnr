/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The Project Peppercorn Authors.
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

// Importing placed and routed partitions (--vopt partition), see docs/partitions.md.
//
// A partition is a design implemented on its own with --vopt as-partition, so that its top level ports
// are nets without IO pads: an input port net has no driver, an output port net no users, and neither
// is routed. Another design instantiates it as a blackbox; the import replaces that instance with the
// partition's cells and nets, moved by whole tiles. The cells and the routing of the nets inside the
// partition are locked; the port nets join the nets on the instance's ports and are routed with the
// rest of the design, as are clocks, which reach the partition through its ports.
//
// A partition is written as routed, before postRoute turns bridges and CP lines into cells and renames
// the LUT inputs after the CPE inputs their routing reaches; that happens once the ports are routed.

#include <fstream>
#include <regex>
#include <sstream>

#include <boost/algorithm/string.hpp>

#include "json11.hpp"

#include "gatemate.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

using namespace json11;

namespace {

// The name of bit i of a port or netname written as `base` with `width` bits from `offset`, as the
// JSON frontend names it
std::string bit_name(const std::string &base, int i, int width, int offset, bool upto)
{
    if (width == 1 && offset == 0)
        return base;
    return stringf("%s[%d]", base.c_str(), upto ? offset + width - i - 1 : offset + i);
}

Property parse_property(const Json &val)
{
    if (val.is_number())
        return Property(val.int_value(), 32);
    return Property::from_string(val.string_value());
}

// A bit of a connection: its net number, or -1 if it is a constant or unconnected
int signal_bit(const Json &bit)
{
    if (bit.is_number())
        return bit.int_value();
    if (bit.string_value() != "x" && bit.string_value() != "z")
        log_error("Partitions with constant connections ('%s') are not supported.\n", bit.string_value().c_str());
    return -1;
}

} // namespace

void GateMateImpl::import_partitions()
{
    if (!ctx->args.options.count("partition"))
        return;
    for (auto &spec : ctx->args.options["partition"].as<std::vector<std::string>>())
        import_partition(spec);
    ctx->assignArchInfo();
}

void GateMateImpl::import_partition(const std::string &spec)
{
    // <inst>:<file>[@dx,dy]; the file name may contain ':' (drive letters), the instance name not
    size_t colon = spec.find(':');
    if (colon == std::string::npos || colon == 0)
        log_error("Invalid partition '%s', expected <inst>:<file>[@dx,dy].\n", spec.c_str());
    std::string inst_name = spec.substr(0, colon), filename = spec.substr(colon + 1);
    int dx = 0, dy = 0;
    static const std::regex offset_re(R"(@(-?\d+),(-?\d+)$)");
    std::smatch m;
    if (std::regex_search(filename, m, offset_re)) {
        dx = std::stoi(m[1]);
        dy = std::stoi(m[2]);
        filename = filename.substr(0, m.position(0));
    }

    IdString inst_id = ctx->id(inst_name);
    if (!ctx->cells.count(inst_id))
        log_error("Partition instance '%s' not found in the design.\n", inst_name.c_str());
    CellInfo *inst = ctx->cells.at(inst_id).get();
    log_info("Importing partition '%s' (%s) from '%s', moved by (%d, %d)..\n", inst_name.c_str(), inst->type.c_str(ctx),
             filename.c_str(), dx, dy);

    std::ifstream in(filename);
    if (!in)
        log_error("Cannot read partition file '%s'.\n", filename.c_str());
    std::stringstream buf;
    buf << in.rdbuf();
    std::string error;
    Json root = Json::parse(buf.str(), error, JsonParse::COMMENTS);
    if (!error.empty())
        log_error("Failed to parse partition file '%s': %s.\n", filename.c_str(), error.c_str());
    const auto &modules = root["modules"].object_items();
    if (modules.size() != 1)
        log_error("Partition file '%s' is not a placed and routed design (it has %d modules).\n", filename.c_str(),
                  int(modules.size()));
    const Json &mod = modules.begin()->second;
    const auto &settings = mod["settings"].object_items();
    auto setting = [&](const char *name) {
        auto found = settings.find(name);
        return found != settings.end() && parse_property(found->second).as_bool();
    };
    if (!setting("route"))
        log_error("Partition file '%s' is not routed.\n", filename.c_str());
    if (!setting("as-partition"))
        log_error("Partition file '%s' was not implemented with --vopt as-partition.\n", filename.c_str());

    std::string prefix = inst_name + ".";
    auto unique = [&](const std::string &name, bool net) {
        std::string candidate = prefix + name;
        for (int i = 1; net ? ctx->nets.count(ctx->id(candidate)) : ctx->cells.count(ctx->id(candidate)); i++)
            candidate = stringf("%s%s$%d", prefix.c_str(), name.c_str(), i);
        return ctx->id(candidate);
    };

    // Move a bel or pip location; the tile there must be of the same type, so the same index means the
    // same resource
    auto moved_tile = [&](int tile, const std::string &what) {
        int x, y;
        tile_xy(ctx->chip_info, tile, x, y);
        int nx = x + dx, ny = y + dy;
        if (nx < 0 || ny < 0 || nx >= ctx->getGridDimX() || ny >= ctx->getGridDimY())
            log_error("Partition '%s' moved by (%d, %d) leaves the device: %s at X%dY%d would be at X%dY%d.\n",
                      inst_name.c_str(), dx, dy, what.c_str(), x, y, nx, ny);
        int nt = tile_by_xy(ctx->chip_info, nx, ny);
        if (ctx->chip_info->tile_insts[nt].type != ctx->chip_info->tile_insts[tile].type)
            return -1;
        return nt;
    };

    // The nets on the instance's ports, by port name
    dict<IdString, NetInfo *> outer_nets;
    dict<IdString, PortType> outer_dirs;
    for (auto &port : inst->ports) {
        outer_nets[port.first] = port.second.net;
        outer_dirs[port.first] = port.second.type;
    }

    // The constant nets (see detach_partition_ports), among them those of constant outputs
    dict<int, std::string> constant_bits;
    for (auto &nn : mod["netnames"].object_items()) {
        const Json &value = nn.second["attributes"]["GATEMATE_CONSTANT"];
        if (value.is_string())
            for (auto &b : nn.second["bits"].array_items())
                if (b.is_number())
                    constant_bits[b.int_value()] = value.string_value();
    }

    // Partition net numbers to nets of this design. The port nets are the nets outside where connected.
    dict<int, NetInfo *> nets;
    std::vector<std::pair<NetInfo *, NetInfo *>> merge;
    pool<IdString> used_ports;
    for (auto &port : mod["ports"].object_items()) {
        const auto &bits = port.second["bits"].array_items();
        int offset = port.second["offset"].is_number() ? port.second["offset"].int_value() : 0;
        bool upto = port.second["upto"].is_number() && port.second["upto"].int_value();
        std::string dir = port.second["direction"].string_value();
        PortType type = dir == "input" ? PORT_IN : dir == "output" ? PORT_OUT : PORT_INOUT;
        for (int i = 0; i < int(bits.size()); i++) {
            IdString name = ctx->id(bit_name(port.first, i, bits.size(), offset, upto));
            int bit = signal_bit(bits.at(i));
            if (type == PORT_INOUT)
                log_error("Partition '%s' has an inout port '%s', which is not supported.\n", inst_name.c_str(),
                          name.c_str(ctx));
            if (!outer_dirs.count(name))
                continue; // not connected outside, so the instance has no such port
            if (outer_dirs.at(name) != type)
                log_error("Port '%s' of partition '%s' has another direction than on the instance.\n",
                          name.c_str(ctx), inst_name.c_str());
            used_ports.insert(name);
            NetInfo *outer = outer_nets.at(name);
            if (bit < 0 || !outer)
                continue;
            if (type == PORT_OUT && outer->driver.cell && outer->driver.cell != inst)
                log_error("Output '%s' of partition '%s' drives net '%s', which has another driver.\n",
                          name.c_str(ctx), inst_name.c_str(), ctx->nameOf(outer));
            if (constant_bits.count(bit))
                outer->attrs[ctx->id("GATEMATE_CONSTANT")] = constant_bits.at(bit);
            if (nets.count(bit) && nets.at(bit) != outer)
                merge.emplace_back(nets.at(bit), outer); // outputs driven by the same signal, or a feedthrough
            else
                nets[bit] = outer;
        }
    }
    for (auto &port : inst->ports)
        if (port.second.net && !used_ports.count(port.first))
            log_error("Port '%s' of instance '%s' is not a port of the partition.\n", port.first.c_str(ctx),
                      inst_name.c_str());
    for (auto &port : outer_nets)
        if (port.second)
            inst->disconnectPort(port.first);
    ctx->cells.erase(inst_id);
    // Ports on one net inside are one net outside too, which can have one driver at most
    for (auto &m : merge) {
        NetInfo *keep = m.first, *from = m.second;
        if (keep == from)
            continue;
        if (from->driver.cell && keep->driver.cell)
            log_error("Ports of partition '%s' sharing a net connect to nets outside driven by '%s' and '%s'.\n",
                      inst_name.c_str(), ctx->nameOf(keep->driver.cell), ctx->nameOf(from->driver.cell));
        if (from->driver.cell)
            std::swap(keep, from);
        auto users = from->users; // make a copy
        for (auto &usr : users) {
            usr.cell->disconnectPort(usr.port);
            usr.cell->connectPort(usr.port, keep);
        }
        for (auto &port : ctx->ports)
            if (port.second.net == from)
                port.second.net = keep;
        for (auto &n : nets)
            if (n.second == from)
                n.second = keep;
        for (auto &later : merge) {
            if (later.first == from)
                later.first = keep;
            if (later.second == from)
                later.second = keep;
        }
        // (from stays, empty, as names may refer to it)
    }

    // The other nets get the partition's names, preferring names that are not hidden
    dict<int, std::pair<std::string, const Json *>> net_names;
    for (auto &nn : mod["netnames"].object_items()) {
        const auto &bits = nn.second["bits"].array_items();
        for (int i = 0; i < int(bits.size()); i++) {
            int bit = signal_bit(bits.at(i));
            if (bit < 0)
                continue;
            int offset = nn.second["offset"].is_number() ? nn.second["offset"].int_value() : 0;
            bool upto = nn.second["upto"].is_number() && nn.second["upto"].int_value();
            std::string name = bit_name(nn.first, i, bits.size(), offset, upto);
            auto found = net_names.find(bit);
            if (found == net_names.end() || (found->second.first[0] == '$' && name[0] != '$'))
                net_names[bit] = std::make_pair(name, &nn.second);
        }
    }
    // The routing of each net inside the partition, as moved pips
    dict<IdString, std::vector<std::pair<PipId, std::string>>> routing;
    int unmovable_pips = 0;
    auto get_net = [&](int bit) {
        auto found = nets.find(bit);
        if (found != nets.end())
            return found->second;
        auto nn = net_names.find(bit);
        NetInfo *net = ctx->createNet(unique(nn != net_names.end() ? nn->second.first : stringf("$net%d", bit), true));
        nets[bit] = net;
        if (nn != net_names.end()) {
            for (auto &attr : nn->second.second->operator[]("attributes").object_items()) {
                if (attr.first == "ROUTING") {
                    std::vector<std::string> strs;
                    std::string route = attr.second.string_value();
                    boost::split(strs, route, boost::is_any_of(";"));
                    for (size_t i = 0; i + 2 < strs.size(); i += 3) {
                        if (strs[i + 1].empty())
                            continue; // the source wire, bound again from the moved driver
                        PipId pip = ctx->getPipByName(IdStringList::parse(ctx, strs[i + 1]));
                        if (pip == PipId())
                            log_error("Unknown pip '%s' in partition '%s'.\n", strs[i + 1].c_str(), inst_name.c_str());
                        int tile = moved_tile(pip.tile, "pip " + strs[i + 1]);
                        if (tile < 0) {
                            unmovable_pips++;
                            routing[net->name].emplace_back(PipId(), strs[i + 1]);
                            continue;
                        }
                        PipId moved;
                        moved.tile = tile;
                        moved.index = pip.index;
                        routing[net->name].emplace_back(moved, strs[i + 1]);
                    }
                } else {
                    net->attrs[ctx->id(attr.first)] = parse_property(attr.second);
                }
            }
        }
        return net;
    };

    // The cells, placed at their moved bels
    int num_cells = 0;
    std::vector<std::string> unmovable;
    BoundingBox area(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), -1, -1);
    for (auto &cell : mod["cells"].object_items()) {
        CellInfo *ci = ctx->createCell(unique(cell.first, false), ctx->id(cell.second["type"].string_value()));
        num_cells++;
        for (auto &param : cell.second["parameters"].object_items())
            ci->params[ctx->id(param.first)] = parse_property(param.second);
        for (auto &attr : cell.second["attributes"].object_items())
            ci->attrs[ctx->id(attr.first)] = parse_property(attr.second);
        auto bel_attr = ci->attrs.find(id_NEXTPNR_BEL);
        if (bel_attr == ci->attrs.end())
            log_error("Cell '%s' of partition '%s' is not placed.\n", cell.first.c_str(), inst_name.c_str());
        BelId bel = ctx->getBelByNameStr(bel_attr->second.as_string());
        if (bel == BelId())
            log_error("Unknown bel '%s' in partition '%s'.\n", bel_attr->second.as_string().c_str(),
                      inst_name.c_str());
        int tile = moved_tile(bel.tile, "cell " + cell.first);
        if (tile < 0) {
            unmovable.push_back(stringf("%s (%s)", cell.first.c_str(), bel_attr->second.as_string().c_str()));
            continue;
        }
        BelId moved;
        moved.tile = tile;
        moved.index = bel.index;
        if (CellInfo *other = ctx->getBoundBelCell(moved))
            log_error("Cell '%s' of partition '%s' would be placed on %s, which '%s' uses already.\n",
                      cell.first.c_str(), inst_name.c_str(), ctx->nameOfBel(moved), ctx->nameOf(other));
        ctx->bindBel(moved, ci, STRENGTH_LOCKED);
        ci->attrs.erase(id_NEXTPNR_BEL);
        ci->attrs.erase(id_BEL_STRENGTH);
        restore_partition_attrs(ci);
        partition_cells.insert(ci->name);
        Loc loc = ctx->getBelLocation(moved);
        area.x0 = std::min(area.x0, loc.x);
        area.y0 = std::min(area.y0, loc.y);
        area.x1 = std::max(area.x1, loc.x);
        area.y1 = std::max(area.y1, ci->type == id_RAM ? loc.y + 8 : loc.y); // a RAM spans two tiles

        dict<std::string, int> port_offsets; // see save_partition_attrs
        auto offsets_attr = ci->attrs.find(id_GATEMATE_PORT_OFFSETS);
        if (offsets_attr != ci->attrs.end()) {
            std::vector<std::string> entries;
            std::string value = offsets_attr->second.as_string();
            boost::split(entries, value, boost::is_any_of(";"));
            for (auto &entry : entries) {
                size_t eq = entry.find('=');
                if (eq != std::string::npos)
                    port_offsets[entry.substr(0, eq)] = std::stoi(entry.substr(eq + 1));
            }
            ci->attrs.erase(offsets_attr);
        }
        const auto &dirs = cell.second["port_directions"].object_items();
        for (auto &conn : cell.second["connections"].object_items()) {
            const auto &bits = conn.second.array_items();
            auto dir = dirs.find(conn.first);
            std::string d = dir != dirs.end() ? dir->second.string_value() : "input";
            PortType type = d == "input" ? PORT_IN : d == "output" ? PORT_OUT : PORT_INOUT;
            // An unconnected port is written without bits; the passes after routing connect some of them
            if (bits.empty()) {
                IdString port = ctx->id(conn.first);
                if (type == PORT_IN)
                    ci->addInput(port);
                else if (type == PORT_OUT)
                    ci->addOutput(port);
                else
                    ci->addInout(port);
                ci->cell_bel_pins[port].push_back(port);
            }
            auto offset = port_offsets.find(conn.first);
            for (int i = 0; i < int(bits.size()); i++) {
                // The writer can not tell a lone bit of a bus from a single bit port; take the name with a bel pin
                IdString port = offset != port_offsets.end()
                                        ? ctx->idf("%s[%d]", conn.first.c_str(), offset->second + i)
                                        : ctx->id(bit_name(conn.first, i, bits.size(), 0, false));
                if (offset == port_offsets.end() && bits.size() == 1 && ctx->getBelPinWire(moved, port) == WireId() &&
                    ctx->getBelPinWire(moved, ctx->idf("%s[0]", conn.first.c_str())) != WireId())
                    port = ctx->idf("%s[0]", conn.first.c_str());
                // The writer fills the gaps in a bus with bits of no net; there is no port there
                int gap = signal_bit(bits.at(i));
                if (gap >= 0 && !net_names.count(gap) && !nets.count(gap))
                    continue;
                if (type == PORT_IN)
                    ci->addInput(port);
                else if (type == PORT_OUT)
                    ci->addOutput(port);
                else
                    ci->addInout(port);
                ci->cell_bel_pins[port].push_back(port); // as assignArchInfo does, needed for the routing below
                int bit = signal_bit(bits.at(i));
                if (bit < 0)
                    continue;
                NetInfo *net = get_net(bit);
                if (type == PORT_OUT && net->driver.cell)
                    log_error("Net '%s' of partition '%s' has two drivers.\n", ctx->nameOf(net), inst_name.c_str());
                ci->connectPort(port, net);
            }
        }
    }
    // The partition keeps the whole rectangle it covers, with the area its routing was kept in: a cell placed in
    // a gap inside it would be closed in by the partition's locked routing
    auto routed_area = settings.find("partition-area");
    if (routed_area != settings.end()) {
        int x0, y0, x1, y1;
        if (sscanf(routed_area->second.string_value().c_str(), "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4) {
            area.x0 = std::max(0, std::min(area.x0, x0 + dx));
            area.y0 = std::max(0, std::min(area.y0, y0 + dy));
            area.x1 = std::min(ctx->getGridDimX() - 1, std::max(area.x1, x1 + dx));
            area.y1 = std::min(ctx->getGridDimY() - 1, std::max(area.y1, y1 + dy));
        }
    }
    for (int y = area.y0; y <= area.y1; y++)
        for (int x = area.x0; x <= area.x1; x++)
            partition_tiles.insert(tile_by_xy(ctx->chip_info, x, y));
    if (!unmovable.empty()) {
        for (auto &u : unmovable)
            log_info("    can not move %s\n", u.c_str());
        log_error("Partition '%s' can not be moved by (%d, %d): %d cells would land on other kinds of tile.\n",
                  inst_name.c_str(), dx, dy, int(unmovable.size()));
    }

    // The constant nets (see detach_partition_ports) get drivers as the JSON frontend makes them, which packing
    // joins with this design's constants
    for (auto &bit : nets) {
        NetInfo *net = bit.second;
        auto constant = net->attrs.find(ctx->id("GATEMATE_CONSTANT"));
        if (constant == net->attrs.end())
            continue;
        bool one = constant->second.as_string() == "1";
        net->attrs.erase(constant);
        if (net->driver.cell)
            log_error("Constant output of partition '%s' drives net '%s', which has another driver.\n",
                      inst_name.c_str(), ctx->nameOf(net));
        CellInfo *drv = ctx->createCell(unique(stringf("%s$%s", net->name.c_str(ctx), one ? "VCC" : "GND"), false),
                                        ctx->id(one ? "VCC" : "GND"));
        drv->addOutput(ctx->id("Y"));
        drv->connectPort(ctx->id("Y"), net);
    }

    // Bind the routing of the nets inside the partition and check it reaches every sink; a net whose
    // routing can not be moved is routed again
    int locked = 0, rerouted = 0;
    for (auto &r : routing) {
        NetInfo *net = ctx->nets.at(r.first).get();
        std::string why;
        WireId src = net->driver.cell ? ctx->getNetinfoSourceWire(net) : WireId();
        if (src == WireId())
            why = "no source wire";
        else if (NetInfo *other = ctx->getBoundWireNet(src))
            why = stringf("source wire %s used by '%s'", ctx->nameOfWire(src), ctx->nameOf(other));
        else
            ctx->bindWire(src, net, STRENGTH_LOCKED);
        for (auto &pip : r.second) {
            if (!why.empty())
                break;
            // (The GateMate pip checks need the placement, so this only checks the wires are free)
            if (pip.first == PipId())
                why = stringf("pip %s on another kind of tile", pip.second.c_str());
            else if (NetInfo *other = ctx->getBoundWireNet(ctx->getPipDstWire(pip.first)))
                why = stringf("wire %s used by '%s'", ctx->nameOfWire(ctx->getPipDstWire(pip.first)),
                              ctx->nameOf(other));
            else
                ctx->bindPip(pip.first, net, STRENGTH_LOCKED);
        }
        for (auto &usr : net->users) {
            for (WireId sink : ctx->getNetinfoSinkWires(net, usr)) {
                WireId cursor = sink;
                // (moved pips can form a loop where the wires of the new place differ)
                for (size_t steps = 0; why.empty() && cursor != src; steps++) {
                    auto found = net->wires.find(cursor);
                    if (found == net->wires.end() || found->second.pip == PipId() || steps > net->wires.size())
                        why = stringf("no route to %s.%s", ctx->nameOf(usr.cell), usr.port.c_str(ctx));
                    else
                        cursor = ctx->getPipSrcWire(found->second.pip);
                }
            }
        }
        if (why.empty()) {
            for (auto &w : net->wires) {
                partition_routed_tiles.insert(w.first.tile);
                if (w.second.pip != PipId())
                    partition_routed_tiles.insert(w.second.pip.tile);
            }
            locked++;
            continue;
        }
        rerouted++;
        if (ctx->verbose)
            log_info("    routing net '%s' again: %s\n", ctx->nameOf(net), why.c_str());
        ctx->ripupNet(net->name);
    }
    log_info("    %d cells, %d nets with locked routing, %d nets to route again (%d pips on other kinds of "
             "tile).\n",
             num_cells, locked, rerouted, unmovable_pips);
    if (partition_open)
        open_partition_area(area);
}

void GateMateImpl::open_partition_area(const BoundingBox &area)
{
    // The free tiles of the area are opened to the design where the partition's locked routing leaves a way to
    // every CPE input: a flood over the free wires from the outputs of all bels outside the partitions' cells has
    // to reach each of them
    pool<int> used;
    for (auto &name : partition_cells) {
        const CellInfo *ci = ctx->cells.at(name).get();
        if (ci->bel == BelId())
            continue;
        used.insert(ci->bel.tile);
        if (ci->type == id_RAM) // the RAM's bel pins span its second half's tile too
            used.insert(rel_tile(ctx->chip_info, ci->bel.tile, 0, 8));
    }
    auto is_cpe = [&](BelId bel) { return ctx->getBelType(bel).str(ctx).rfind("CPE", 0) == 0; };
    pool<WireId> reached;
    std::vector<WireId> queue;
    for (int y = 0; y < ctx->getGridDimY(); y++)
        for (int x = 0; x < ctx->getGridDimX(); x++) {
            if (used.count(tile_by_xy(ctx->chip_info, x, y)))
                continue;
            for (BelId bel : ctx->getBelsByTile(x, y)) {
                for (IdString pin : ctx->getBelPins(bel)) {
                    WireId wire = ctx->getBelPinWire(bel, pin);
                    if (ctx->getBelPinType(bel, pin) == PORT_OUT && wire != WireId() &&
                        ctx->getBoundWireNet(wire) == nullptr && reached.insert(wire).second)
                        queue.push_back(wire);
                }
            }
        }
    while (!queue.empty()) {
        WireId wire = queue.back();
        queue.pop_back();
        for (PipId pip : ctx->getPipsDownhill(wire)) {
            WireId dst = ctx->getPipDstWire(pip);
            if (!reached.count(dst) && ctx->getBoundWireNet(dst) == nullptr && pip_allowed_by_options(pip)) {
                reached.insert(dst);
                queue.push_back(dst);
            }
        }
    }
    int opened = 0, closed = 0;
    for (int y = area.y0; y <= area.y1; y++)
        for (int x = area.x0; x <= area.x1; x++) {
            int tile = tile_by_xy(ctx->chip_info, x, y);
            if (used.count(tile))
                continue;
            bool open = true;
            for (BelId bel : ctx->getBelsByTile(x, y)) {
                if (!is_cpe(bel))
                    continue;
                for (IdString pin : ctx->getBelPins(bel)) {
                    // (RAM_I comes the dedicated way from its RAM)
                    if (ctx->getBelPinType(bel, pin) != PORT_IN || pin.str(ctx).rfind("RAM_I", 0) == 0)
                        continue;
                    WireId wire = ctx->getBelPinWire(bel, pin);
                    if (wire != WireId() && !reached.count(wire)) {
                        open = false;
                        break;
                    }
                }
                if (!open)
                    break;
            }
            if (open) {
                partition_tiles.erase(tile);
                opened++;
            } else {
                closed++;
            }
        }
    log_info("    opened %d free tiles of the area to the design, %d stay closed in by its routing.\n", opened,
             closed);
}

void GateMateImpl::restore_partition_attrs(CellInfo *ci)
{
    auto mult = ci->attrs.find(id_GATEMATE_MULT);
    if (mult != ci->attrs.end()) {
        std::string role = mult->second.as_string();
        if (role == "a_passthru_lower")
            multiplier_a_passthru_lowers.insert(ci->name);
        else if (role == "a_passthru_upper")
            multiplier_a_passthru_uppers.insert(ci->name);
        else if (role == "zero_driver")
            multiplier_zero_drivers.insert(ci->name);
    }
    auto constr_x = ci->attrs.find(id_GATEMATE_CONSTR_X);
    if (constr_x != ci->attrs.end())
        ci->constr_x = constr_x->second.as_int64();
}

pool<IdString> GateMateImpl::partition_detached_cells() const
{
    // The clocking cells, and the cells feeding nothing but them
    pool<IdString> remove;
    for (auto &cell : ctx->cells)
        if (cell.second->type.in(id_GLBOUT, id_CLKIN))
            remove.insert(cell.first);
    for (bool changed = true; changed;) {
        changed = false;
        for (auto &cell : ctx->cells) {
            if (remove.count(cell.first))
                continue;
            bool drives = false, only_clocking = true;
            for (auto &port : cell.second->ports) {
                if (port.second.type != PORT_OUT || !port.second.net)
                    continue;
                for (auto &usr : port.second.net->users) {
                    drives = true;
                    only_clocking &= remove.count(usr.cell->name) > 0;
                }
            }
            if (drives && only_clocking) {
                remove.insert(cell.first);
                changed = true;
            }
        }
    }
    // The constant drivers
    for (auto name : {"$PACKER_GND_DRV", "$PACKER_VCC_DRV"})
        if (ctx->cells.count(ctx->id(name)))
            remove.insert(ctx->id(name));
    // The registers standing in on the ports (pack_io), with what packing made of them, named after them
    for (auto &cell : ctx->cells)
        for (auto dummy : partition_port_cells)
            if (cell.first.str(ctx).rfind(dummy.str(ctx), 0) == 0)
                remove.insert(cell.first);
    return remove;
}

void GateMateImpl::find_partition_area()
{
    // The bounding box of the floorplan regions
    for (auto &region : ctx->region) {
        if (region.first.str(ctx).rfind("floorplan:", 0) != 0)
            continue;
        for (BelId bel : region.second->bels) {
            Loc loc = ctx->getBelLocation(bel);
            if (partition_area.x0 < 0) {
                partition_area = BoundingBox(loc.x, loc.y, loc.x, loc.y);
                continue;
            }
            partition_area.x0 = std::min(partition_area.x0, loc.x);
            partition_area.y0 = std::min(partition_area.y0, loc.y);
            partition_area.x1 = std::max(partition_area.x1, loc.x);
            partition_area.y1 = std::max(partition_area.y1, loc.y);
        }
    }
    // The CPEs fixed next to IOs and the clocking (by getRelativeConstraint) are kept free for the design
    // importing the partition, which may use any of them
    for (auto &constr : pin_to_constr) {
        // (the SERDES, configuration and reset bels are left to the partition; their CPEs are those of RAMs too)
        IdString type = ctx->getBelType(constr.first);
        if (constr.second.empty() || !type.in(id_GPIO, id_IOSEL, id_GLBOUT, id_CLKIN, id_PLL))
            continue;
        if (!partition_ccf_tiles.empty() && type.in(id_GPIO, id_IOSEL) && !partition_ccf_tiles.count(constr.first.tile))
            continue; // an IO the importing design does not use
        Loc root = ctx->getBelLocation(constr.first);
        for (auto &pin : constr.second) {
            int x = root.x + pin.second->constr_x, y = root.y + pin.second->constr_y;
            if (x >= 0 && y >= 0 && x < ctx->getGridDimX() && y < ctx->getGridDimY())
                partition_io_tiles.insert(tile_by_xy(ctx->chip_info, x, y));
        }
    }
    for (auto &region : ctx->region) {
        if (region.first.str(ctx).rfind("floorplan:", 0) != 0)
            continue;
        std::vector<BelId> taken;
        for (BelId bel : region.second->bels)
            if (partition_io_tiles.count(bel.tile))
                taken.push_back(bel);
        for (BelId bel : taken)
            region.second->bels.erase(bel);
    }
    if (partition_area.x0 < 0) {
        log_warning("The partition is not floorplanned; it will take the rectangle its cells and routing happen to "
                    "cover.\n");
        return;
    }
    // A CPE's inputs come from the switch boxes around it, so the routing gets a margin of a 2x2 block
    const int margin = 2;
    partition_area.x0 = std::max(0, partition_area.x0 - margin);
    partition_area.y0 = std::max(0, partition_area.y0 - margin);
    partition_area.x1 = std::min(ctx->getGridDimX() - 1, partition_area.x1 + margin);
    partition_area.y1 = std::min(ctx->getGridDimY() - 1, partition_area.y1 + margin);
    log_info("Partition area X%dY%d to X%dY%d; its routing stays inside.\n", partition_area.x0, partition_area.y0,
             partition_area.x1, partition_area.y1);
}

void GateMateImpl::find_partition_free_nets()
{
    // The nets taken out or routed again after routing may leave the area: the clocks, the constants and the
    // ports, whose way in and out the stand-ins outside the area keep free
    partition_free_nets.clear();
    for (auto &clock : partition_clocks)
        partition_free_nets.insert(clock.first);
    for (auto name : partition_detached_cells())
        for (auto &port : ctx->cells.at(name)->ports)
            if (port.second.net)
                partition_free_nets.insert(port.second.net->name);
}

bool GateMateImpl::checkPipAvailForNet(PipId pip, const NetInfo *net) const
{
    if (as_partition && net && !partition_free_nets.count(net->name)) {
        // No bridges through the CPEs kept free for IOs (their inputs get no other load: no cells go there)
        if (partition_io_tiles.count(pip.tile)) {
            const auto &extra_data = *pip_extra_data(pip);
            if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_ROUTING))
                return false;
        }
        WireId dst = ctx->getPipDstWire(pip);
        if (partition_area.x0 >= 0) {
            int x, y;
            tile_xy(ctx->chip_info, pip.tile, x, y);
            if (x < partition_area.x0 || x > partition_area.x1 || y < partition_area.y0 || y > partition_area.y1)
                return false;
            tile_xy(ctx->chip_info, dst.tile, x, y);
            if (x < partition_area.x0 || x > partition_area.x1 || y < partition_area.y0 || y > partition_area.y1)
                return false;
        }
    }
    return checkPipAvail(pip);
}

void GateMateImpl::detach_partition_ports()
{
    for (auto &clock : partition_clocks) {
        if (!ctx->nets.count(clock.first) || !ctx->nets.count(clock.second))
            log_error("Lost the clock net '%s' of the partition.\n", clock.first.c_str(ctx));
        NetInfo *glob = ctx->nets.at(clock.first).get();
        NetInfo *port = ctx->nets.at(clock.second).get();
        ctx->ripupNet(glob->name);
        auto users = glob->users; // make a copy
        for (auto &usr : users) {
            usr.cell->disconnectPort(usr.port);
            usr.cell->connectPort(usr.port, port);
        }
    }
    // The constants come from the importing design's constant drivers, which are placed with its other cells
    // (the packer places these anywhere, outside the partition's region)
    for (auto &constant : {std::make_pair("$PACKER_GND_DRV", "0"), std::make_pair("$PACKER_VCC_DRV", "1")}) {
        auto found = ctx->cells.find(ctx->id(constant.first));
        if (found == ctx->cells.end())
            continue;
        for (auto &port : found->second->ports)
            if (NetInfo *net = port.second.net)
                net->attrs[ctx->id("GATEMATE_CONSTANT")] = std::string(constant.second);
    }
    // The routing of every net they touch goes too: it is the way in and out the importing design routes again
    for (auto name : partition_detached_cells()) {
        CellInfo *ci = ctx->cells.at(name).get();
        for (auto &port : ci->ports) {
            NetInfo *net = port.second.net;
            if (!net)
                continue;
            ctx->ripupNet(net->name);
            ci->disconnectPort(port.first);
        }
        ctx->unbindBel(ci->bel);
        ctx->cells.erase(name);
    }
    // Nets left connecting nothing
    std::vector<IdString> empty;
    for (auto &net : ctx->nets)
        if (!net.second->driver.cell && net.second->users.empty() && net.second->wires.empty())
            empty.push_back(net.first);
    for (auto name : empty) {
        bool is_port = false;
        for (auto &port : ctx->ports)
            is_port |= port.second.net && port.second.net->name == name;
        if (!is_port)
            ctx->nets.erase(name);
    }
}

void GateMateImpl::save_partition_attrs()
{
    if (partition_area.x0 >= 0)
        ctx->settings[ctx->id("partition-area")] = stringf("%d,%d,%d,%d", partition_area.x0, partition_area.y0,
                                                           partition_area.x1, partition_area.y1);
    // What the bitstream takes from the packer beyond the netlist and the routing
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (multiplier_a_passthru_lowers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("a_passthru_lower");
        else if (multiplier_a_passthru_uppers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("a_passthru_upper");
        else if (multiplier_zero_drivers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("zero_driver");
        if (ci->type == id_CPE_MULT)
            ci->attrs[id_GATEMATE_CONSTR_X] = Property(ci->constr_x, 32);
        // The JSON writer writes the ports name[i] of a cell as a bus from the lowest i, without that index
        dict<std::string, int> offsets;
        for (auto &port : ci->ports) {
            std::string name = port.first.str(ctx);
            size_t open = name.find_last_of('[');
            if (name.back() != ']' || open == std::string::npos)
                continue;
            int index = std::stoi(name.substr(open + 1));
            std::string base = name.substr(0, open);
            auto found = offsets.find(base);
            if (found == offsets.end() || index < found->second)
                offsets[base] = index;
        }
        std::string value;
        for (auto &offset : offsets)
            if (offset.second != 0)
                value += stringf("%s%s=%d", value.empty() ? "" : ";", offset.first.c_str(), offset.second);
        if (!value.empty())
            ci->attrs[id_GATEMATE_PORT_OFFSETS] = value;
    }
}

NEXTPNR_NAMESPACE_END
