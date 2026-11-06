// Common Python bindings #included by all arches

readonly_wrapper<Context, decltype(&Context::cells), &Context::cells, wrap_context<CellMap &>>::def_wrap(ctx_cls,
                                                                                                         "cells");
readonly_wrapper<Context, decltype(&Context::nets), &Context::nets, wrap_context<NetMap &>>::def_wrap(ctx_cls, "nets");
readonly_wrapper<Context, decltype(&Context::net_aliases), &Context::net_aliases, wrap_context<AliasMap &>>::def_wrap(
        ctx_cls, "net_aliases");
readonly_wrapper<Context, decltype(&Context::hierarchy), &Context::hierarchy, wrap_context<HierarchyMap &>>::def_wrap(
        ctx_cls, "hierarchy");
readwrite_wrapper<Context, decltype(&Context::top_module), &Context::top_module, conv_to_str<IdString>,
                  conv_from_str<IdString>>::def_wrap(ctx_cls, "top_module");
readonly_wrapper<Context, decltype(&Context::timing_result), &Context::timing_result,
                 wrap_context<TimingResult &>>::def_wrap(ctx_cls, "timing_result");

fn_wrapper_0a<Context, decltype(&Context::getNameDelimiter), &Context::getNameDelimiter, pass_through<char>>::def_wrap(
        ctx_cls, "getNameDelimiter");

fn_wrapper_1a<Context, decltype(&Context::getNetByAlias), &Context::getNetByAlias, deref_and_wrap<NetInfo>,
              conv_from_str<IdString>>::def_wrap(ctx_cls, "getNetByAlias");
fn_wrapper_2a_v<Context, decltype(&Context::addClock), &Context::addClock, conv_from_str<IdString>,
                pass_through<float>>::def_wrap(ctx_cls, "addClock");
fn_wrapper_5a_v<Context, decltype(&Context::createRectangularRegion), &Context::createRectangularRegion,
                conv_from_str<IdString>, pass_through<int>, pass_through<int>, pass_through<int>,
                pass_through<int>>::def_wrap(ctx_cls, "createRectangularRegion");
fn_wrapper_2a_v<Context, decltype(&Context::addBelToRegion), &Context::addBelToRegion, conv_from_str<IdString>,
                conv_from_str<BelId>>::def_wrap(ctx_cls, "addBelToRegion");
fn_wrapper_2a_v<Context, decltype(&Context::constrainCellToRegion), &Context::constrainCellToRegion,
                conv_from_str<IdString>, conv_from_str<IdString>>::def_wrap(ctx_cls, "constrainCellToRegion");

fn_wrapper_2a<Context, decltype(&Context::getNetinfoRouteDelay), &Context::getNetinfoRouteDelay, pass_through<delay_t>,
              addr_and_unwrap<NetInfo>, unwrap_context<PortRef>>::def_wrap(ctx_cls, "getNetinfoRouteDelay");
fn_wrapper_2a<Context, decltype(&Context::getNetinfoSinkWireCount), &Context::getNetinfoSinkWireCount,
              pass_through<size_t>, addr_and_unwrap<NetInfo>, unwrap_context<PortRef>>::def_wrap(ctx_cls,
                                                                                                 "getNetinfoSinkWireCount");
fn_wrapper_3a<Context, decltype(&Context::getNetinfoSinkWire), &Context::getNetinfoSinkWire, conv_to_str<WireId>,
              addr_and_unwrap<NetInfo>, unwrap_context<PortRef>, pass_through<size_t>>::def_wrap(ctx_cls,
                                                                                                 "getNetinfoSinkWire");

// The settings (ctx.settings in C++): what the flow has done, and options such as target_freq
ctx_cls.def(
        "getSetting",
        [](Context &c, const std::string &key) -> py::object {
            auto found = c.settings.find(c.id(key));
            if (found == c.settings.end())
                return py::none();
            return py::str(found->second.is_string ? found->second.as_string() : std::to_string(found->second.as_int64()));
        },
        "The setting as a string (numbers too), or None");
ctx_cls.def(
        "setSetting", [](Context &c, const std::string &key, const std::string &value) { c.settings[c.id(key)] = value; },
        "Sets the setting (a string)");
ctx_cls.def(
        "unsetSetting", [](Context &c, const std::string &key) { c.settings.erase(c.id(key)); }, "Removes the setting");
ctx_cls.def(
        "getSettings",
        [](Context &c) {
            py::dict d;
            for (auto &s : c.settings)
                d[py::str(s.first.str(&c))] =
                        py::str(s.second.is_string ? s.second.as_string() : std::to_string(s.second.as_int64()));
            return d;
        },
        "All settings, as a dict of strings");
ctx_cls.def(
        "writeJson",
        [](Context &c, std::string filename) {
            std::ofstream f(filename);
            if (!f || !write_json_file(f, filename, &c))
                throw std::runtime_error("cannot write " + filename);
        },
        "Writes the design (as placed and routed so far) to a JSON file");

// Editing the netlist; errors are Python exceptions (rather than failed assertions)
ctx_cls.def(
        "removeCell",
        [](Context &c, const std::string &name) {
            auto found = c.cells.find(c.id(name));
            if (found == c.cells.end())
                throw std::runtime_error("no cell " + name);
            CellInfo *cell = found->second.get();
            if (cell->cluster != ClusterId())
                throw std::runtime_error(name + " is in a cluster; take it out first (ctx.removeFromCluster(cell) "
                                                "where there is one)");
            if (cell->bel != BelId())
                c.unbindBel(cell->bel);
            std::vector<IdString> ports;
            for (auto &port : cell->ports)
                ports.push_back(port.first);
            for (IdString port : ports)
                cell->disconnectPort(port);
            c.cells.erase(found);
            c.assignArchInfo();
            c.refreshUi();
        },
        "Removes the cell: unbinds its bel and disconnects its ports (the nets stay)");
ctx_cls.def(
        "removeNet",
        [](Context &c, const std::string &name) {
            auto found = c.nets.find(c.id(name));
            if (found == c.nets.end())
                throw std::runtime_error("no net " + name);
            NetInfo *net = found->second.get();
            c.ripupNet(net->name);
            std::vector<std::pair<CellInfo *, IdString>> ports;
            if (net->driver.cell)
                ports.emplace_back(net->driver.cell, net->driver.port);
            for (auto &user : net->users)
                ports.emplace_back(user.cell, user.port);
            for (auto &port : ports)
                port.first->disconnectPort(port.second);
            for (auto it = c.net_aliases.begin(); it != c.net_aliases.end();)
                it = it->second == net->name ? c.net_aliases.erase(it) : std::next(it);
            c.nets.erase(found);
            c.assignArchInfo();
            c.refreshUi();
        },
        "Removes the net: unroutes it and disconnects its driver and users");
ctx_cls.def(
        "renameNet",
        [](Context &c, const std::string &old_name, const std::string &new_name) {
            IdString from = c.id(old_name), to = c.id(new_name);
            if (!c.nets.count(from))
                throw std::runtime_error("no net " + old_name);
            if (c.nets.count(to))
                throw std::runtime_error("there is a net " + new_name + " already");
            c.renameNet(from, to);
            for (auto &alias : c.net_aliases)
                if (alias.second == from)
                    alias.second = to;
            c.assignArchInfo();
            c.refreshUi();
        },
        "Renames the net");
ctx_cls.def(
        "renameCell",
        [](Context &c, const std::string &old_name, const std::string &new_name) {
            IdString from = c.id(old_name), to = c.id(new_name);
            auto found = c.cells.find(from);
            if (found == c.cells.end())
                throw std::runtime_error("no cell " + old_name);
            if (c.cells.count(to))
                throw std::runtime_error("there is a cell " + new_name + " already");
            if (found->second->cluster != ClusterId())
                throw std::runtime_error(old_name + " is in a cluster");
            std::unique_ptr<CellInfo> cell = std::move(found->second);
            c.cells.erase(from);
            cell->name = to;
            c.cells[to] = std::move(cell);
            c.assignArchInfo();
            c.refreshUi();
        },
        "Renames the cell (not one in a cluster)");
auto port_cell = [](Context &c, const std::string &cell, const std::string &port, bool must_exist) {
    auto found = c.cells.find(c.id(cell));
    if (found == c.cells.end())
        throw std::runtime_error("no cell " + cell);
    if (must_exist && !found->second->ports.count(c.id(port)))
        throw std::runtime_error("no port " + cell + "." + port);
    return found->second.get();
};
ctx_cls.def(
        "removePort",
        [port_cell](Context &c, const std::string &cell, const std::string &port) {
            CellInfo *ci = port_cell(c, cell, port, true);
            ci->disconnectPort(c.id(port));
            ci->ports.erase(c.id(port));
            c.assignArchInfo();
        },
        "Disconnects the port and removes it from the cell");
ctx_cls.def(
        "renamePort",
        [port_cell](Context &c, const std::string &cell, const std::string &old_name, const std::string &new_name) {
            CellInfo *ci = port_cell(c, cell, old_name, true);
            if (ci->ports.count(c.id(new_name)))
                throw std::runtime_error("there is a port " + cell + "." + new_name + " already");
            ci->renamePort(c.id(old_name), c.id(new_name));
            c.assignArchInfo();
        },
        "Renames a port of the cell, keeping its net");
ctx_cls.def(
        "movePort",
        [port_cell](Context &c, const std::string &cell, const std::string &port, const std::string &other,
                    const std::string &other_port) {
            CellInfo *ci = port_cell(c, cell, port, true);
            ci->movePortTo(c.id(port), port_cell(c, other, other_port, false), c.id(other_port));
            c.assignArchInfo();
        },
        "Moves the port's connection to other.other_port (made if not there); the port stays, unconnected");
ctx_cls.def(
        "copyPort",
        [port_cell](Context &c, const std::string &cell, const std::string &port, const std::string &other,
                    const std::string &other_port) {
            CellInfo *ci = port_cell(c, cell, port, true);
            ci->copyPortTo(c.id(port), port_cell(c, other, other_port, false), c.id(other_port));
            c.assignArchInfo();
        },
        "Connects other.other_port (made if not there) to the port's net too");
ctx_cls.def(
        "connectPorts",
        [port_cell](Context &c, const std::string &cell, const std::string &port, const std::string &other,
                    const std::string &other_port) {
            CellInfo *ci = port_cell(c, cell, port, true);
            ci->connectPorts(c.id(port), port_cell(c, other, other_port, true), c.id(other_port));
            c.assignArchInfo();
        },
        "Connects two ports with a new net (or the port's net, if it has one), the first the driver");

ctx_cls.def(
        "readSdc",
        [](Context &c, std::string filename) {
            std::ifstream f(filename);
            if (!f)
                throw std::runtime_error("cannot read " + filename);
            c.read_sdc(f);
        },
        "Reads timing constraints from an SDC file (as --sdc does)");

fn_wrapper_1a<Context, decltype(&Context::createNet), &Context::createNet, deref_and_wrap<NetInfo>,
              conv_from_str<IdString>>::def_wrap(ctx_cls, "createNet");
fn_wrapper_3a_v<Context, decltype(&Context::connectPort), &Context::connectPort, conv_from_str<IdString>,
                conv_from_str<IdString>, conv_from_str<IdString>>::def_wrap(ctx_cls, "connectPort");
fn_wrapper_2a_v<Context, decltype(&Context::disconnectPort), &Context::disconnectPort, conv_from_str<IdString>,
                conv_from_str<IdString>>::def_wrap(ctx_cls, "disconnectPort");
fn_wrapper_1a_v<Context, decltype(&Context::ripupNet), &Context::ripupNet, conv_from_str<IdString>>::def_wrap(
        ctx_cls, "ripupNet");
fn_wrapper_1a_v<Context, decltype(&Context::lockNetRouting), &Context::lockNetRouting,
                conv_from_str<IdString>>::def_wrap(ctx_cls, "lockNetRouting");

fn_wrapper_2a<Context, decltype(&Context::createCell), &Context::createCell, deref_and_wrap<CellInfo>,
              conv_from_str<IdString>, conv_from_str<IdString>>::def_wrap(ctx_cls, "createCell");
fn_wrapper_2a_v<Context, decltype(&Context::copyBelPorts), &Context::copyBelPorts, conv_from_str<IdString>,
                conv_from_str<BelId>>::def_wrap(ctx_cls, "copyBelPorts");

fn_wrapper_1a<Context, decltype(&Context::getBelType), &Context::getBelType, conv_to_str<IdString>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "getBelType");
fn_wrapper_1a<Context, decltype(&Context::getBelLocation), &Context::getBelLocation, pass_through<Loc>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "getBelLocation");
fn_wrapper_1a<Context, decltype(&Context::checkBelAvail), &Context::checkBelAvail, pass_through<bool>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "checkBelAvail");
fn_wrapper_1a<Context, decltype(&Context::getBelChecksum), &Context::getBelChecksum, pass_through<uint32_t>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "getBelChecksum");
fn_wrapper_3a_v<Context, decltype(&Context::bindBel), &Context::bindBel, conv_from_str<BelId>,
                addr_and_unwrap<CellInfo>, pass_through<PlaceStrength>>::def_wrap(ctx_cls, "bindBel");
fn_wrapper_1a_v<Context, decltype(&Context::unbindBel), &Context::unbindBel, conv_from_str<BelId>>::def_wrap(
        ctx_cls, "unbindBel");
fn_wrapper_1a<Context, decltype(&Context::getBoundBelCell), &Context::getBoundBelCell, deref_and_wrap<CellInfo>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "getBoundBelCell");
fn_wrapper_1a<Context, decltype(&Context::getConflictingBelCell), &Context::getConflictingBelCell,
              deref_and_wrap<CellInfo>, conv_from_str<BelId>>::def_wrap(ctx_cls, "getConflictingBelCell");
fn_wrapper_0a<Context, decltype(&Context::getBels), &Context::getBels, wrap_context<BelRange>>::def_wrap(ctx_cls,
                                                                                                         "getBels");

fn_wrapper_2a<Context, decltype(&Context::getBelPinWire), &Context::getBelPinWire, conv_to_str<WireId>,
              conv_from_str<BelId>, conv_from_str<IdString>>::def_wrap(ctx_cls, "getBelPinWire");
fn_wrapper_2a<Context, decltype(&Context::getBelPinType), &Context::getBelPinType, pass_through<PortType>,
              conv_from_str<BelId>, conv_from_str<IdString>>::def_wrap(ctx_cls, "getBelPinType");
fn_wrapper_1a<Context, decltype(&Context::getWireBelPins), &Context::getWireBelPins, wrap_context<BelPinRange>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getWireBelPins");

fn_wrapper_1a<Context, decltype(&Context::getWireChecksum), &Context::getWireChecksum, pass_through<uint32_t>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getWireChecksum");
fn_wrapper_1a<Context, decltype(&Context::getWireType), &Context::getWireType, conv_to_str<IdString>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getWireType");
fn_wrapper_1a<Context, decltype(&Context::getWireDelay), &Context::getWireDelay, pass_through<DelayQuad>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getWireDelay");
fn_wrapper_3a_v<Context, decltype(&Context::bindWire), &Context::bindWire, conv_from_str<WireId>,
                addr_and_unwrap<NetInfo>, pass_through<PlaceStrength>>::def_wrap(ctx_cls, "bindWire");
fn_wrapper_1a_v<Context, decltype(&Context::unbindWire), &Context::unbindWire, conv_from_str<WireId>>::def_wrap(
        ctx_cls, "unbindWire");
fn_wrapper_1a<Context, decltype(&Context::checkWireAvail), &Context::checkWireAvail, pass_through<bool>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "checkWireAvail");
fn_wrapper_1a<Context, decltype(&Context::getBoundWireNet), &Context::getBoundWireNet, deref_and_wrap<NetInfo>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getBoundWireNet");
fn_wrapper_1a<Context, decltype(&Context::getConflictingWireNet), &Context::getConflictingWireNet,
              deref_and_wrap<NetInfo>, conv_from_str<WireId>>::def_wrap(ctx_cls, "getConflictingWireNet");

fn_wrapper_0a<Context, decltype(&Context::getWires), &Context::getWires, wrap_context<WireRange>>::def_wrap(ctx_cls,
                                                                                                            "getWires");

fn_wrapper_0a<Context, decltype(&Context::getPips), &Context::getPips, wrap_context<AllPipRange>>::def_wrap(ctx_cls,
                                                                                                            "getPips");
fn_wrapper_1a<Context, decltype(&Context::getPipChecksum), &Context::getPipChecksum, pass_through<uint32_t>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getPipChecksum");
fn_wrapper_1a<Context, decltype(&Context::getPipLocation), &Context::getPipLocation, pass_through<Loc>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getPipLocation");
fn_wrapper_3a_v<Context, decltype(&Context::bindPip), &Context::bindPip, conv_from_str<PipId>, addr_and_unwrap<NetInfo>,
                pass_through<PlaceStrength>>::def_wrap(ctx_cls, "bindPip");
fn_wrapper_1a_v<Context, decltype(&Context::unbindPip), &Context::unbindPip, conv_from_str<PipId>>::def_wrap(
        ctx_cls, "unbindPip");
fn_wrapper_1a<Context, decltype(&Context::checkPipAvail), &Context::checkPipAvail, pass_through<bool>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "checkPipAvail");
fn_wrapper_1a<Context, decltype(&Context::isPipInverting), &Context::isPipInverting, pass_through<bool>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "isPipInverting");
fn_wrapper_1a<Context, decltype(&Context::getBoundPipNet), &Context::getBoundPipNet, deref_and_wrap<NetInfo>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getBoundPipNet");
fn_wrapper_1a<Context, decltype(&Context::getConflictingPipNet), &Context::getConflictingPipNet,
              deref_and_wrap<NetInfo>, conv_from_str<PipId>>::def_wrap(ctx_cls, "getConflictingPipNet");

fn_wrapper_1a<Context, decltype(&Context::getPipsDownhill), &Context::getPipsDownhill, wrap_context<DownhillPipRange>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getPipsDownhill");
fn_wrapper_1a<Context, decltype(&Context::getPipsUphill), &Context::getPipsUphill, wrap_context<UphillPipRange>,
              conv_from_str<WireId>>::def_wrap(ctx_cls, "getPipsUphill");

fn_wrapper_1a<Context, decltype(&Context::getPipSrcWire), &Context::getPipSrcWire, conv_to_str<WireId>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getPipSrcWire");
fn_wrapper_1a<Context, decltype(&Context::getPipDstWire), &Context::getPipDstWire, conv_to_str<WireId>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getPipDstWire");
fn_wrapper_1a<Context, decltype(&Context::getPipDelay), &Context::getPipDelay, pass_through<DelayQuad>,
              conv_from_str<PipId>>::def_wrap(ctx_cls, "getPipDelay");

fn_wrapper_0a<Context, decltype(&Context::getChipName), &Context::getChipName, pass_through<std::string>>::def_wrap(
        ctx_cls, "getChipName");
fn_wrapper_0a<Context, decltype(&Context::archId), &Context::archId, conv_to_str<IdString>>::def_wrap(ctx_cls,
                                                                                                      "archId");

fn_wrapper_2a_v<Context, decltype(&Context::writeSVG), &Context::writeSVG, pass_through<std::string>,
                pass_through<std::string>>::def_wrap(ctx_cls, "writeSVG");

fn_wrapper_2a<Context, decltype(&Context::isBelLocationValid), &Context::isBelLocationValid, pass_through<bool>,
              conv_from_str<BelId>, pass_through<bool>>::def_wrap(ctx_cls, "isBelLocationValid");

// const\_range\<BelBucketId\> getBelBuckets() const
fn_wrapper_0a<Context, decltype(&Context::getBelBuckets), &Context::getBelBuckets,
              wrap_context<BelBucketRange>>::def_wrap(ctx_cls, "getBelBuckets");
// BelBucketId getBelBucketForBel(BelId bel) const
fn_wrapper_1a<Context, decltype(&Context::getBelBucketForBel), &Context::getBelBucketForBel, conv_to_str<BelBucketId>,
              conv_from_str<BelId>>::def_wrap(ctx_cls, "getBelBucketForBel");
// BelBucketId getBelBucketForCellType(IdString cell\_type) const
fn_wrapper_1a<Context, decltype(&Context::getBelBucketForCellType), &Context::getBelBucketForCellType,
              conv_to_str<BelBucketId>, conv_from_str<IdString>>::def_wrap(ctx_cls, "getBelBucketForCellType");
// const\_range\<BelId\> getBelsInBucket(BelBucketId bucket) const
fn_wrapper_1a<Context, decltype(&Context::getBelsInBucket), &Context::getBelsInBucket,
              wrap_context<BelRangeForBelBucket>, conv_from_str<BelBucketId>>::def_wrap(ctx_cls, "getBelsInBucket");
// bool isValidBelForCellType(IdString cell\_type, BelId bel) const
fn_wrapper_2a<Context, decltype(&Context::isValidBelForCellType), &Context::isValidBelForCellType, pass_through<bool>,
              conv_from_str<IdString>, conv_from_str<BelId>>::def_wrap(ctx_cls, "isValidBelForCellType");

fn_wrapper_1a<Context, decltype(&Context::getDelayFromNS), &Context::getDelayFromNS, pass_through<delay_t>,
              pass_through<double>>::def_wrap(ctx_cls, "getDelayFromNS");

fn_wrapper_1a<Context, decltype(&Context::getDelayNS), &Context::getDelayNS, pass_through<double>,
              pass_through<delay_t>>::def_wrap(ctx_cls, "getDelayNS");

fn_wrapper_3a_v<Context, decltype(&Context::createRegionPlug), &Context::createRegionPlug, conv_from_str<IdString>,
                conv_from_str<IdString>, pass_through<Loc>>::def_wrap(ctx_cls, "createRegionPlug");
fn_wrapper_4a_v<Context, decltype(&Context::addPlugPin), &Context::addPlugPin, conv_from_str<IdString>,
                conv_from_str<IdString>, pass_through<PortType>, conv_from_str<WireId>>::def_wrap(ctx_cls,
                                                                                                  "addPlugPin");
