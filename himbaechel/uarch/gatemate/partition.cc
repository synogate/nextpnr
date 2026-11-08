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
// partition's cells and nets, moved by whole tiles. The cells are locked while placing, and the routing of
// the nets inside the partition is bound; the options load, place and route say what is taken over and what
// may change after. The port nets join the nets on the instance's ports and are routed with the rest of the
// design, as are clocks, which reach the partition through its ports.
//
// A partition is written as routed, before postRoute turns bridges and CP lines into cells and renames
// the LUT inputs after the CPE inputs their routing reaches; that happens once the ports are routed.

#include <fstream>
#include <map>
#include <tuple>
#include <set>
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
    // The options, from --vopt partition-<key> and overridden by ,<key>=<value> at the end of the spec
    dict<std::string, std::string> options;
    for (auto key : {"load", "place", "route"}) {
        std::string option = stringf("partition-%s", key);
        if (ctx->args.options.count(option))
            options[key] = ctx->args.options[option].as<std::string>();
    }
    static const std::regex option_re(R"(,([a-z]+)=([a-z]+)$)");
    std::smatch m;
    pool<std::string> given; // (read from the end, the last one given counts)
    while (std::regex_search(filename, m, option_re)) {
        if (given.insert(m[1]).second)
            options[m[1]] = m[2];
        filename = filename.substr(0, m.position(0));
    }
    auto option = [&](const char *key, std::initializer_list<const char *> values) {
        auto found = options.find(key);
        if (found == options.end())
            return std::string();
        for (auto v : values)
            if (found->second == v)
                return found->second;
        log_error("Invalid %s=%s for partition '%s'.\n", key, found->second.c_str(), inst_name.c_str());
    };
    for (auto &o : options)
        if (o.first != "load" && o.first != "place" && o.first != "route")
            log_error("Unknown option '%s' for partition '%s' (load, place and route).\n", o.first.c_str(),
                      inst_name.c_str());
    std::string load = option("load", {"routing", "placement"});
    bool place_locked = option("place", {"repair", "locked"}) == "locked";
    std::string route = option("route", {"open", "repair", "locked"});
    int dx = 0, dy = 0;
    static const std::regex offset_re(R"(@(-?\d+),(-?\d+)$)");
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
    bool unrouted = setting("partition-unrouted");
    if (!setting("route") && !unrouted)
        log_error("Partition file '%s' is not routed (for a partition placed only, implement it with --vopt "
                  "partition-unrouted).\n",
                  filename.c_str());
    if (unrouted && load == "routing")
        log_error("Partition file '%s' has no routing to load (it was written with partition-unrouted).\n",
                  filename.c_str());
    // Without routing, the route option has nothing to act on
    bool load_routing = !unrouted && load != "placement";
    if (route.empty())
        route = "open";
    log_info("    load=%s place=%s route=%s\n", load_routing ? "routing" : "placement",
             place_locked ? "locked" : "repair", load_routing ? route.c_str() : "-");
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
    std::vector<std::tuple<IdString, PortType, int>> port_bits; // the ports joining a net outside
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
            port_bits.emplace_back(name, type, bit);
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
        // (the pins of partitions imported before, see below)
        IdString pins_key = ctx->id("GATEMATE_PARTITION_PINS");
        auto from_pins = from->attrs.find(pins_key);
        if (from_pins != from->attrs.end()) {
            auto keep_pins = keep->attrs.find(pins_key);
            keep->attrs[pins_key] = keep_pins != keep->attrs.end()
                                            ? Property(keep_pins->second.as_string() + ";" + from_pins->second.as_string())
                                            : from_pins->second;
            from->attrs.erase(from_pins);
        }
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
    // The pins between the partition and the design, kept on the nets outside (and so in a saved design) for the
    // GUI: "<in|out>|<instance>|<port>", separated by ';' where ports of several partitions share a net
    for (auto &pb : port_bits) {
        NetInfo *net = nets.at(std::get<2>(pb));
        std::string entry = stringf("%s|%s|%s", std::get<1>(pb) == PORT_IN ? "in" : "out", inst_name.c_str(),
                                    std::get<0>(pb).c_str(ctx));
        Property &pins = net->attrs[ctx->id("GATEMATE_PARTITION_PINS")];
        pins = Property(pins.is_string && !pins.as_string().empty() ? pins.as_string() + ";" + entry : entry);
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
                    if (!load_routing)
                        continue;
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
    dict<std::string, IdString> cell_names;                     // in the partition -> here
    std::vector<std::pair<CellInfo *, std::string>> cluster_roots; // cells and their cluster roots' names
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
        // Locked while placing, see release_partitions
        ctx->bindBel(moved, ci, STRENGTH_LOCKED);
        auto strength = ci->attrs.find(id_BEL_STRENGTH);
        // (files written before the clusters were stored keep their cells locked: the passes would not see them)
        partition_strength[ci->name] = strength != ci->attrs.end() && setting("partition-clusters") && !place_locked
                                               ? PlaceStrength(strength->second.as_int64())
                                               : STRENGTH_LOCKED;
        ci->attrs.erase(id_NEXTPNR_BEL);
        ci->attrs.erase(id_BEL_STRENGTH);
        restore_partition_attrs(ci);
        partition_cells.insert(ci->name);
        auto cluster = ci->attrs.find(id_GATEMATE_CLUSTER);
        if (cluster != ci->attrs.end()) {
            cluster_roots.emplace_back(ci, cluster->second.as_string());
            ci->attrs.erase(cluster);
        }
        cell_names[cell.first] = ci->name;
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
    for (auto &c : cluster_roots) {
        auto root = cell_names.find(c.second);
        if (root == cell_names.end())
            continue; // (a stand-in, gone)
        CellInfo *ci = c.first;
        ci->cluster = root->second;
        if (ci->name != root->second)
            ctx->cells.at(root->second)->constr_children.push_back(ci);
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
    // routing can not be moved is routed again. With route=open it is bound as the router binds its routing, so
    // the router and the repairs after it may change it, but for the fixed multiplier paths (route_mult), locked
    // as there; otherwise it is locked (and route=repair releases it after routing).
    int locked = 0, rerouted = 0;
    for (auto &r : routing) {
        NetInfo *net = ctx->nets.at(r.first).get();
        std::string why;
        WireId src = net->driver.cell ? ctx->getNetinfoSourceWire(net) : WireId();
        IdString drv = net->driver.cell ? net->driver.cell->name : IdString();
        bool fixed = net->driver.port == id_OUT &&
                     (multiplier_a_passthru_lowers.count(drv) || multiplier_a_passthru_uppers.count(drv) ||
                      multiplier_zero_drivers.count(drv));
        PlaceStrength strength = fixed || route != "open" ? STRENGTH_LOCKED : STRENGTH_WEAK;
        if (src == WireId())
            why = "no source wire";
        else if (NetInfo *other = ctx->getBoundWireNet(src))
            why = stringf("source wire %s used by '%s'", ctx->nameOfWire(src), ctx->nameOf(other));
        else
            ctx->bindWire(src, net, strength);
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
                ctx->bindPip(pip.first, net, strength);
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
            if (!fixed && route == "open")
                partition_nets.insert(net->name);
            else
                partition_locked_nets.insert(net->name);
            if (!fixed && route == "repair")
                partition_repair_nets.insert(net->name);
            locked++;
            continue;
        }
        rerouted++;
        if (ctx->verbose)
            log_info("    routing net '%s' again: %s\n", ctx->nameOf(net), why.c_str());
        ctx->ripupNet(net->name);
    }
    if (!load_routing)
        log_info("    %d cells, placed only: their nets are routed with the design.\n", num_cells);
    else
        log_info("    %d cells, %d nets with imported routing, %d nets to route again (%d pips on other kinds of "
                 "tile).\n",
                 num_cells, locked, rerouted, unmovable_pips);
    if (partition_open)
        open_partition_area(area);
}

void GateMateImpl::release_partitions()
{
    // After placing, the partitions' cells may move as any other: they get back the strength they had
    for (auto &s : partition_strength) {
        auto found = ctx->cells.find(s.first);
        if (found != ctx->cells.end() && found->second->belStrength == STRENGTH_LOCKED)
            found->second->belStrength = s.second;
    }
    // The cells of the design may go into the partitions' areas now; the partitions' routing gives way to them
    partition_tiles.clear();
}

void GateMateImpl::release_partition_routing()
{
    // route=repair: the routing the router kept is the repairs' to change, as the router's own
    for (auto name : partition_repair_nets) {
        auto found = ctx->nets.find(name);
        if (found == ctx->nets.end())
            continue;
        for (auto &w : found->second->wires)
            if (w.second.strength == STRENGTH_LOCKED)
                w.second.strength = STRENGTH_WEAK;
        prerouted_nets.erase(name);
        partition_locked_nets.erase(name);
    }
    partition_repair_nets.clear();
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
    auto constr = ci->attrs.find(id_GATEMATE_CONSTR);
    if (constr != ci->attrs.end()) {
        int x, y, z, abs_z;
        if (sscanf(constr->second.as_string().c_str(), "%d,%d,%d,%d", &x, &y, &z, &abs_z) == 4) {
            ci->constr_x = x;
            ci->constr_y = y;
            ci->constr_z = z;
            ci->constr_abs_z = abs_z;
        }
        ci->attrs.erase(constr);
    }
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

void GateMateImpl::add_partition_region(const std::string &spec)
{
    // 10,10,21,21 or, as the bel names write them, X10,Y10,X21,Y21
    static const std::regex format(R"(\s*[Xx]?(\d+)\s*,\s*[Yy]?(\d+)\s*,\s*[Xx]?(\d+)\s*,\s*[Yy]?(\d+)\s*)");
    std::smatch m;
    if (!std::regex_match(spec, m, format))
        log_error("Invalid partition-region '%s', expected X0,Y0,X1,Y1 (for example 10,10,21,21).\n", spec.c_str());
    int x0 = std::stoi(m[1]), y0 = std::stoi(m[2]), x1 = std::stoi(m[3]), y1 = std::stoi(m[4]);
    if (x0 > x1)
        std::swap(x0, x1);
    if (y0 > y1)
        std::swap(y0, y1);
    if (x0 < 0 || y0 < 0 || x1 >= ctx->getGridDimX() || y1 >= ctx->getGridDimY())
        log_error("Partition region '%s' is out of range.\n", spec.c_str());
    // A floorplan region as the CCF makes them, so it is the partition's area too
    IdString region = ctx->id("floorplan:partition-region");
    ctx->createRectangularRegion(region, x0, y0, x1, y1);
    int scopes = 0;
    for (IdString name : scopenames)
        if (scope_regions.emplace(name, region).second)
            scopes++;
    log_info("Partition region X%dY%d to X%dY%d for %d scopes.\n", x0, y0, x1, y1, scopes);
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
    partition_detached = true;
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

// The GUI's File > Save as partition: what step 1 of the partition flow (--vopt as-partition) writes, from the
// design as it is now: placed (and routed, by hand or by Route) in a session started with --vopt as-partition. The
// stand-ins, buffers and constant drivers go as at the end of step 1 (postRoute, or postPlace with
// partition-unrouted); a design without routing is written as with partition-unrouted.
std::string GateMateImpl::preparePartitionWrite(bool check_only, bool *changes_design)
{
    if (changes_design)
        *changes_design = false;
    if (!as_partition)
        return "the design is not set up as a partition: check 'Implement this design as a partition' in the "
               "Partitions tab before packing (or start nextpnr with --vopt as-partition) on the partition's JSON "
               "synthesized without IO pads, then pack and place it";
    if (!ctx->settings.count(ctx->id("pack")))
        return "the design is not packed yet";
    for (auto &cell : ctx->cells)
        if (cell.second->bel == BelId() && !cell.second->isPseudo())
            return "cell '" + cell.first.str(ctx) + "' is not placed: a partition is written placed";
    if (!partition_detached) {
        if (changes_design)
            *changes_design = true;
        if (check_only)
            return {};
        detach_partition_ports();
        bool routed = false;
        for (auto &net : ctx->nets)
            for (auto &wire : net.second->wires)
                routed |= wire.second.pip != PipId();
        // (routed by hand without Route, the import still takes the routing there is: the router completes it)
        ctx->settings[ctx->id(routed ? "route" : "partition-unrouted")] = 1;
        pip_masks_ready = false;
    } else if (check_only) {
        return {};
    }
    save_partition_attrs();
    return {};
}

void GateMateImpl::save_partition_attrs()
{
    ctx->settings[ctx->id("partition-clusters")] = 1;
    // (the rest as for any packed design written, see archStateToAttributes: the import needs the clusters, the
    // multiplier roles and the partition's area)
    archStateToAttributes();
}

// Saving and loading a packed design part way through the flow (GUI File > Save JSON / Open JSON, --write and
// --json ... --no-pack): the JSON holds the netlist, the placement and the routing (NEXTPNR_BEL, ROUTING); what the
// packer worked out beyond that goes into attributes here and is read back after loading, so that placing and
// routing go on as in the session that saved the design. The partition import reads the same attributes.
namespace {
const char *const region_setting_prefix = "gatemate-region:";

// The tiles of a region as rectangles "x0,y0,x1,y1;..." (the regions of GateMate designs are made of whole tiles)
std::string encode_region_tiles(const Context *ctx, const Region *region)
{
    std::map<int, std::set<int>> rows;
    for (BelId bel : region->bels) {
        Loc loc = ctx->getBelLocation(bel);
        rows[loc.y].insert(loc.x);
    }
    // Runs of x in each row; rows with the same runs one after another make one rectangle
    std::map<int, std::vector<std::pair<int, int>>> runs;
    for (auto &row : rows)
        for (int x : row.second) {
            auto &r = runs[row.first];
            if (!r.empty() && r.back().second == x - 1)
                r.back().second = x;
            else
                r.emplace_back(x, x);
        }
    std::string out;
    for (auto it = runs.begin(); it != runs.end();) {
        int y0 = it->first, y1 = y0;
        auto next = std::next(it);
        while (next != runs.end() && next->first == y1 + 1 && next->second == it->second) {
            y1 = next->first;
            ++next;
        }
        for (auto &run : it->second)
            out += stringf("%s%d,%d,%d,%d", out.empty() ? "" : ";", run.first, y0, run.second, y1);
        it = next;
    }
    return out;
}
} // namespace

void GateMateImpl::archStateToAttributes()
{
    if (!ctx->settings.count(ctx->id("pack")))
        return;
    const IdString a_region = ctx->id("GATEMATE_REGION"), a_port_cell = ctx->id("GATEMATE_PORT_CELL"),
                   a_mult_cluster = ctx->id("GATEMATE_MULT_CLUSTER"), a_ddr = ctx->id("GATEMATE_DDR"),
                   a_clock = ctx->id("GATEMATE_PARTITION_CLOCK"), a_global = ctx->id("GATEMATE_GLOBAL");
    pool<IdString> regions;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        for (IdString a : {id_GATEMATE_MULT, id_GATEMATE_CONSTR_X, id_GATEMATE_CLUSTER, id_GATEMATE_CONSTR,
                           id_GATEMATE_PORT_OFFSETS, a_region, a_port_cell, a_mult_cluster})
            ci->attrs.erase(a);
        // What the bitstream and the routing of the multipliers take from the packer
        if (multiplier_a_passthru_lowers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("a_passthru_lower");
        else if (multiplier_a_passthru_uppers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("a_passthru_upper");
        else if (multiplier_zero_drivers.count(ci->name))
            ci->attrs[id_GATEMATE_MULT] = std::string("zero_driver");
        if (ci->type == id_CPE_MULT)
            ci->attrs[id_GATEMATE_CONSTR_X] = Property(ci->constr_x, 32);
        if (multiplier_clusters.count(ci->name))
            ci->attrs[a_mult_cluster] = Property(1, 1);
        // The clusters (a register merged with its LUT, chains, ...): the root, the offsets and the place among
        // the root's children
        if (ci->cluster != ClusterId()) {
            int index = -1;
            CellInfo *root = ctx->getClusterRootCell(ci->cluster);
            if (root != ci) {
                auto &children = root->constr_children;
                index = int(std::find(children.begin(), children.end(), ci) - children.begin());
            }
            ci->attrs[id_GATEMATE_CLUSTER] = ci->cluster.str(ctx);
            ci->attrs[id_GATEMATE_CONSTR] = stringf("%d,%d,%d,%d,%d", ci->constr_x, ci->constr_y, ci->constr_z,
                                                    int(ci->constr_abs_z), index);
        }
        if (ci->region) {
            ci->attrs[a_region] = ci->region->name.str(ctx);
            regions.insert(ci->region->name);
        }
        // The stand-ins on the ports of a partition, with what packing made of them (see partition_detached_cells)
        for (auto dummy : partition_port_cells)
            if (cell.first.str(ctx).rfind(dummy.str(ctx), 0) == 0) {
                ci->attrs[a_port_cell] = Property(1, 1);
                break;
            }
    }
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        for (IdString a : {a_ddr, a_clock, a_global})
            ni->attrs.erase(a);
        if (ddr_nets.count(ni->name))
            ni->attrs[a_ddr] = Property(1, 1);
        auto clock = partition_clocks.find(ni->name);
        if (clock != partition_clocks.end())
            ni->attrs[a_clock] = clock->second.str(ctx);
        auto global = global_signals.find(ni);
        if (global != global_signals.end())
            ni->attrs[a_global] = Property(global->second, 32);
    }
    // The regions the cells are constrained to, but for the dies (made with the chip)
    std::vector<IdString> old_regions;
    for (auto &setting : ctx->settings)
        if (setting.first.str(ctx).rfind(region_setting_prefix, 0) == 0)
            old_regions.push_back(setting.first);
    for (auto name : old_regions)
        ctx->settings.erase(name);
    for (auto name : regions)
        if (!die_to_index.count(name))
            ctx->settings[ctx->idf("%s%s", region_setting_prefix, name.c_str(ctx))] =
                    encode_region_tiles(ctx, ctx->region.at(name).get());
    if (partition_area.x0 >= 0)
        ctx->settings[ctx->id("partition-area")] = stringf("%d,%d,%d,%d", partition_area.x0, partition_area.y0,
                                                           partition_area.x1, partition_area.y1);
    if (!partition_io_tiles.empty()) {
        std::vector<int> tiles(partition_io_tiles.begin(), partition_io_tiles.end());
        std::sort(tiles.begin(), tiles.end());
        std::string value;
        for (int tile : tiles)
            value += stringf("%s%d", value.empty() ? "" : ",", tile);
        ctx->settings[ctx->id("partition-io-tiles")] = value;
    }
    ctx->settings[ctx->id("gatemate-strategy")] = Property(int(strategy), 32);
}

void GateMateImpl::attributesToArchState()
{
    if (!ctx->settings.count(ctx->id("pack")))
        return;
    auto setting = [&](const char *name) {
        auto found = ctx->settings.find(ctx->id(name));
        return found != ctx->settings.end() ? found->second.as_string() : std::string();
    };
    // What pack() reads from the options, and how far the partition flow got (see preparePartitionWrite)
    as_partition |= ctx->settings.count(ctx->id("as-partition")) > 0;
    partition_unrouted |= ctx->settings.count(ctx->id("partition-unrouted")) > 0;
    partition_detached = ctx->settings.count(ctx->id("partition-clusters")) > 0;
    if (ctx->settings.count(ctx->id("gatemate-strategy")))
        strategy = MultiDieStrategy(ctx->settings.at(ctx->id("gatemate-strategy")).as_int64());
    int x0, y0, x1, y1;
    if (sscanf(setting("partition-area").c_str(), "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4)
        partition_area = BoundingBox(x0, y0, x1, y1);
    {
        std::vector<std::string> tiles;
        std::string value = setting("partition-io-tiles");
        boost::split(tiles, value, boost::is_any_of(","));
        for (auto &tile : tiles)
            if (!tile.empty())
                partition_io_tiles.insert(std::stoi(tile));
    }
    for (auto &s : ctx->settings) {
        std::string key = s.first.str(ctx);
        if (key.rfind(region_setting_prefix, 0) != 0)
            continue;
        IdString name = ctx->id(key.substr(strlen(region_setting_prefix)));
        ctx->createRectangularRegion(name, 0, 0, -1, -1); // empty, filled below
        std::vector<std::string> rects;
        std::string value = s.second.as_string();
        boost::split(rects, value, boost::is_any_of(";"));
        for (auto &rect : rects)
            if (sscanf(rect.c_str(), "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4)
                for (int y = std::max(0, y0); y <= std::min(y1, ctx->getGridDimY() - 1); y++)
                    for (int x = std::max(0, x0); x <= std::min(x1, ctx->getGridDimX() - 1); x++)
                        for (BelId bel : ctx->getBelsByTile(x, y))
                            ctx->addBelToRegion(name, bel);
    }

    const IdString a_region = ctx->id("GATEMATE_REGION"), a_port_cell = ctx->id("GATEMATE_PORT_CELL"),
                   a_mult_cluster = ctx->id("GATEMATE_MULT_CLUSTER"), a_ddr = ctx->id("GATEMATE_DDR"),
                   a_clock = ctx->id("GATEMATE_PARTITION_CLOCK"), a_global = ctx->id("GATEMATE_GLOBAL");
    struct ClusterMember
    {
        CellInfo *cell;
        IdString root;
        int index;
    };
    std::vector<ClusterMember> members;
    clkin.clear();
    glbout.clear();
    pll.clear();
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        auto constr = ci->attrs.find(id_GATEMATE_CONSTR);
        int index = std::numeric_limits<int>::max(), x, y, z, abs_z;
        if (constr != ci->attrs.end() &&
            sscanf(constr->second.as_string().c_str(), "%d,%d,%d,%d,%d", &x, &y, &z, &abs_z, &index) < 5)
            index = std::numeric_limits<int>::max(); // (written without it: in the order of the cells)
        restore_partition_attrs(ci);
        auto cluster = ci->attrs.find(id_GATEMATE_CLUSTER);
        if (cluster != ci->attrs.end())
            members.push_back({ci, ctx->id(cluster->second.as_string()), index});
        auto region = ci->attrs.find(a_region);
        if (region != ci->attrs.end()) {
            IdString name = ctx->id(region->second.as_string());
            if (ctx->region.count(name))
                ctx->constrainCellToRegion(ci->name, name);
            else
                log_warning("Cell '%s' is constrained to region '%s', which the design does not define.\n",
                            ctx->nameOf(ci), name.c_str(ctx));
        }
        if (ci->attrs.count(a_port_cell))
            partition_port_cells.insert(ci->name);
        if (ci->attrs.count(a_mult_cluster))
            multiplier_clusters.insert(ci->name);
        if (ci->type == id_CLKIN)
            clkin.push_back(ci);
        else if (ci->type == id_GLBOUT)
            glbout.push_back(ci);
        else if (ci->type == id_PLL)
            pll.push_back(ci);
        for (IdString a :
             {id_GATEMATE_MULT, id_GATEMATE_CONSTR_X, id_GATEMATE_CLUSTER, a_region, a_port_cell, a_mult_cluster})
            ci->attrs.erase(a);
    }
    std::stable_sort(members.begin(), members.end(),
                     [](const ClusterMember &a, const ClusterMember &b) { return a.index < b.index; });
    for (auto &m : members) {
        auto root = ctx->cells.find(m.root);
        if (root == ctx->cells.end()) {
            log_warning("The cluster root '%s' of cell '%s' is missing.\n", m.root.c_str(ctx), ctx->nameOf(m.cell));
            continue;
        }
        m.cell->cluster = m.root;
        if (m.cell->name != m.root)
            root->second->constr_children.push_back(m.cell);
    }
    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->attrs.count(a_ddr))
            ddr_nets.insert(ni->name);
        auto clock = ni->attrs.find(a_clock);
        if (clock != ni->attrs.end())
            partition_clocks[ni->name] = ctx->id(clock->second.as_string());
        auto global = ni->attrs.find(a_global);
        if (global != ni->attrs.end())
            global_signals[ni] = int(global->second.as_int64());
        for (IdString a : {a_ddr, a_clock, a_global})
            ni->attrs.erase(a);
    }
    if (dies > 1)
        log_warning("A packed multi-die design was loaded: placing it again does not reassign the global clocks "
                    "between the dies as the session that packed it would.\n");
}

std::vector<IdString> GateMateImpl::getPartitionInstances() const
{
    // Before packing, every cell is a GateMate primitive (CC_*), one of nextpnr's or yosys' own ($*) or a constant
    // driver the frontend adds, but for the blackbox instances
    std::vector<IdString> instances;
    for (auto &cell : ctx->cells) {
        std::string type = cell.second->type.str(ctx);
        if (type.rfind("CC_", 0) != 0 && type[0] != '$' && type != "GND" && type != "VCC")
            instances.push_back(cell.first);
    }
    std::sort(instances.begin(), instances.end(),
              [&](IdString a, IdString b) { return a.str(ctx) < b.str(ctx); });
    return instances;
}

std::vector<HimbaechelAPI::PartitionPin> GateMateImpl::getPartitionPins() const
{
    std::vector<PartitionPin> pins;
    if (!ctx->settings.count(ctx->id("pack"))) {
        // Not imported yet: the ports of the blackbox instances
        for (IdString inst : getPartitionInstances()) {
            for (auto &port : ctx->cells.at(inst)->ports) {
                PartitionPin pin;
                pin.inst = inst;
                pin.port = port.first;
                pin.dir = port.second.type;
                if (port.second.net)
                    pin.net = port.second.net->name;
                pins.push_back(pin);
            }
        }
        return pins;
    }
    // As import_partition marked the nets outside
    IdString key = ctx->id("GATEMATE_PARTITION_PINS");
    for (auto &net : ctx->nets) {
        auto found = net.second->attrs.find(key);
        if (found == net.second->attrs.end() || !found->second.is_string)
            continue;
        std::vector<std::string> entries;
        std::string value = found->second.as_string();
        boost::split(entries, value, boost::is_any_of(";"));
        for (auto &entry : entries) {
            size_t a = entry.find('|'), b = a == std::string::npos ? a : entry.find('|', a + 1);
            if (b == std::string::npos)
                continue;
            PartitionPin pin;
            pin.dir = entry.substr(0, a) == "in" ? PORT_IN : PORT_OUT;
            pin.inst = ctx->id(entry.substr(a + 1, b - a - 1));
            pin.port = ctx->id(entry.substr(b + 1));
            pin.net = net.first;
            pin.imported = true;
            pins.push_back(pin);
        }
    }
    return pins;
}

bool GateMateImpl::readPartitionFootprint(const std::string &filename, PartitionFootprint &footprint) const
{
    // As import_partition reads it, but only where the cells are
    std::ifstream in(filename);
    if (!in) {
        footprint.error = "can not be read";
        return false;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string error;
    Json root = Json::parse(buf.str(), error, JsonParse::COMMENTS);
    if (!error.empty()) {
        footprint.error = "is no JSON: " + error;
        return false;
    }
    const auto &modules = root["modules"].object_items();
    if (modules.size() != 1) {
        footprint.error = "is not a placed design";
        return false;
    }
    const Json &mod = modules.begin()->second;
    const auto &settings = mod["settings"].object_items();
    auto setting = [&](const char *name) {
        auto found = settings.find(name);
        return found != settings.end() && parse_property(found->second).as_bool();
    };
    if (!setting("as-partition")) {
        footprint.error = "was not implemented as a partition";
        return false;
    }
    footprint.routed = setting("route") && !setting("partition-unrouted");
    if (!footprint.routed && !setting("partition-unrouted")) {
        footprint.error = "is not routed (nor written placed only)";
        return false;
    }
    for (auto &port : mod["ports"].object_items())
        footprint.ports.push_back(port.first);
    pool<Loc> tiles;
    BoundingBox &area = footprint.area;
    area = BoundingBox(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), -1, -1);
    for (auto &cell : mod["cells"].object_items()) {
        const Json &bel_name = cell.second["attributes"]["NEXTPNR_BEL"];
        BelId bel = bel_name.is_string() ? ctx->getBelByNameStr(bel_name.string_value()) : BelId();
        if (bel == BelId()) {
            footprint.error = stringf("cell '%s' is not placed", cell.first.c_str());
            return false;
        }
        footprint.cells++;
        Loc loc = ctx->getBelLocation(bel);
        tiles.insert(Loc(loc.x, loc.y, 0));
        area.x0 = std::min(area.x0, loc.x);
        area.y0 = std::min(area.y0, loc.y);
        area.x1 = std::max(area.x1, loc.x);
        area.y1 = std::max(area.y1, ctx->getBelType(bel) == id_RAM ? loc.y + 8 : loc.y); // a RAM spans two tiles
    }
    auto routed_area = settings.find("partition-area");
    int x0, y0, x1, y1;
    if (routed_area != settings.end() &&
        sscanf(routed_area->second.string_value().c_str(), "%d,%d,%d,%d", &x0, &y0, &x1, &y1) == 4) {
        area.x0 = std::min(area.x0, x0);
        area.y0 = std::min(area.y0, y0);
        area.x1 = std::max(area.x1, x1);
        area.y1 = std::max(area.y1, y1);
    }
    if (footprint.cells == 0) {
        footprint.error = "has no cells";
        return false;
    }
    footprint.tiles.assign(tiles.begin(), tiles.end());
    return true;
}

std::string GateMateImpl::partitionMoveProblem(const PartitionFootprint &footprint, int dx, int dy) const
{
    // As import_partition moves the cells: each onto the same kind of tile
    int unmovable = 0;
    for (const Loc &loc : footprint.tiles) {
        int nx = loc.x + dx, ny = loc.y + dy;
        if (nx < 0 || ny < 0 || nx >= ctx->getGridDimX() || ny >= ctx->getGridDimY())
            return "it would leave the device";
        if (ctx->chip_info->tile_insts[tile_by_xy(ctx->chip_info, nx, ny)].type !=
            ctx->chip_info->tile_insts[tile_by_xy(ctx->chip_info, loc.x, loc.y)].type)
            unmovable++;
    }
    if (unmovable)
        return stringf("cells in %d tiles would land on other kinds of tile", unmovable);
    return "";
}

NEXTPNR_NAMESPACE_END
