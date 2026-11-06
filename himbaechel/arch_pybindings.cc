/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2020-23  gatecat <gatecat@ds0.me>
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

#ifndef NO_PYTHON

#include "arch_pybindings.h"
#include "nextpnr.h"
#include "pybindings.h"

#include <algorithm>

NEXTPNR_NAMESPACE_BEGIN

void arch_wrap_python(py::module &m)
{
    using namespace PythonConversion;
    py::class_<ArchArgs>(m, "ArchArgs").def_readwrite("device", &ArchArgs::device);

    py::class_<BelId>(m, "BelId").def_readwrite("index", &BelId::index);

    py::class_<WireId>(m, "WireId").def_readwrite("index", &WireId::index);

    py::class_<PipId>(m, "PipId").def_readwrite("index", &PipId::index);

    auto arch_cls = py::class_<Arch, BaseCtx>(m, "Arch").def(py::init<ArchArgs>());
    auto ctx_cls = py::class_<Context, Arch>(m, "Context")
                           .def("checksum", &Context::checksum)
                           .def("pack", &Context::pack)
                           .def("place", &Context::place)
                           .def("route", &Context::route);

    typedef dict<IdString, std::unique_ptr<CellInfo>> CellMap;
    typedef dict<IdString, std::unique_ptr<NetInfo>> NetMap;
    typedef dict<IdString, IdString> AliasMap;
    typedef dict<IdString, HierarchicalCell> HierarchyMap;

    auto belpin_cls = py::class_<ContextualWrapper<BelPin>>(m, "BelPin");
    readonly_wrapper<BelPin, decltype(&BelPin::bel), &BelPin::bel, conv_to_str<BelId>>::def_wrap(belpin_cls, "bel");
    readonly_wrapper<BelPin, decltype(&BelPin::pin), &BelPin::pin, conv_to_str<IdString>>::def_wrap(belpin_cls, "pin");

    // Useful for debugging routing database
    fn_wrapper_1a<Context, decltype(&Context::get_tile_wire_range), &Context::get_tile_wire_range,
                  wrap_context<TileWireRange>, conv_from_str<WireId>>::def_wrap(ctx_cls, "get_tile_wire_range");

    typedef const std::vector<BelBucketId> &BelBucketRange;
    typedef const std::vector<BelId> &BelRangeForBelBucket;
#include "arch_pybindings_shared.h"

    // Clusters (cell.cluster and the root's constr_children in C++)
    auto find_cell = [](Context &c, const std::string &name) {
        auto found = c.cells.find(c.id(name));
        if (found == c.cells.end())
            throw std::runtime_error("no cell " + name);
        return found->second.get();
    };
    ctx_cls.def(
            "getCellCluster",
            [find_cell](Context &c, const std::string &name) -> py::object {
                CellInfo *cell = find_cell(c, name);
                if (cell->cluster == ClusterId())
                    return py::none();
                return py::str(cell->cluster.str(&c));
            },
            "The name of the cluster (its root cell) the cell is in, or None");
    ctx_cls.def(
            "getClusterChildren",
            [find_cell](Context &c, const std::string &name) {
                py::list list;
                for (CellInfo *child : find_cell(c, name)->constr_children)
                    list.append(py::make_tuple(child->name.str(&c), child->constr_x, child->constr_y, child->constr_z,
                                               child->constr_abs_z));
                return list;
            },
            "The children of a cluster root: (name, dx, dy, dz, abs_z) each");
    ctx_cls.def(
            "getCellRegion",
            [find_cell](Context &c, const std::string &name) -> py::object {
                CellInfo *cell = find_cell(c, name);
                if (!cell->region)
                    return py::none();
                return py::str(cell->region->name.str(&c));
            },
            "The name of the region the cell is constrained to, or None");
    ctx_cls.def(
            "removeFromCluster",
            [find_cell](Context &c, const std::string &name) {
                CellInfo *cell = find_cell(c, name);
                if (cell->cluster == ClusterId())
                    throw std::runtime_error(name + " is in no cluster");
                CellInfo *root = c.getClusterRootCell(cell->cluster);
                if (root == cell && !cell->constr_children.empty())
                    throw std::runtime_error(name + " is the root of its cluster; take its children out first");
                if (cell->bel != BelId())
                    throw std::runtime_error(name + " is placed; unbind it first");
                auto &children = root->constr_children;
                children.erase(std::remove(children.begin(), children.end(), cell), children.end());
                cell->cluster = ClusterId();
                cell->constr_x = cell->constr_y = cell->constr_z = 0;
                cell->constr_abs_z = false;
                c.assignArchInfo();
            },
            "Takes a cell out of its cluster, e.g. to remove it (a root only once it has no children)");

    WRAP_RANGE(m, Bel, conv_to_str<BelId>);
    WRAP_RANGE(m, Wire, conv_to_str<WireId>);
    WRAP_RANGE(m, AllPip, conv_to_str<PipId>);
    WRAP_RANGE(m, UphillPip, conv_to_str<PipId>);
    WRAP_RANGE(m, DownhillPip, conv_to_str<PipId>);
    WRAP_RANGE(m, BelPin, wrap_context<BelPin>);

    WRAP_RANGE(m, TileWire, conv_to_str<WireId>);

    WRAP_MAP_UPTR(m, CellMap, "IdCellMap");
    WRAP_MAP_UPTR(m, NetMap, "IdNetMap");
    WRAP_MAP(m, HierarchyMap, wrap_context<HierarchicalCell &>, "HierarchyMap");
}

NEXTPNR_NAMESPACE_END

#endif // NO_PYTHON
