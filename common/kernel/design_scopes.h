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

/*
 * Design scopes: which instance of the HDL hierarchy each cell of a (flattened) design belongs to.
 *
 * A flattened netlist keeps little of its hierarchy: yosys gives the cells it keeps an `hdlname` or
 * `scopename` attribute, but the cells that techmapping and abc create have neither. Their nets
 * keep more: a public net name of a flattened design is the dotted path of the instance it was
 * declared in ("cpu0.alu.sum"), and a net has one alias per instance it passes through. So a cell
 * is put, in this order of trust, into
 *   1. the instance of ctx->hierarchy it was imported into (a netlist that nextpnr flattened),
 *   2. the instance its `scopename` / `hdlname` attribute names,
 *   3. the instance in which all its named nets are visible; of several on one line of descent
 *      the deepest (the ports of an instance are visible above it too),
 *   4. among the instances in which all its named nets are visible (all instances if it has none),
 *      the one most of its neighbours (over nets of small fanout) were put in,
 *   5. among those instances, the one they have in common (a cell on a clock or reset net from
 *      the top level),
 * and otherwise into the top level. Where no instance sees all named nets of a cell (a name lost
 * on the way), 3. is the instance in which most of them are visible (the nets it drives count
 * twice; ties as in 5.).
 */

#ifndef DESIGN_SCOPES_H
#define DESIGN_SCOPES_H

#include <map>
#include <string>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

struct DesignScopes
{
    enum Source
    {
        SRC_HIERARCHY,
        SRC_ATTRIBUTE,
        SRC_NETS,
        SRC_NEIGHBOURS,
        SRC_NONE, // nothing to go by: the top level
        SRC_COUNT
    };

    struct Scope
    {
        std::string name; // instance name, "" for the top level
        std::string path; // dotted path of instance names, "" for the top level
        std::string type; // module name if known
        int parent = -1, depth = 0;
        std::vector<int> children;   // by name
        std::vector<IdString> cells; // the cells put into this instance itself
        int total = 0;               // cells of this instance and the instances below it
    };

    // scopes[0] is the top level
    std::vector<Scope> scopes;
    dict<IdString, int> cell_scope;
    dict<IdString, Source> cell_source;
    int source_count[SRC_COUNT] = {0};

    // From the design as it is now (the caller holds the context lock)
    static DesignScopes build(const Context *ctx);

    // Index of the scope with this dotted path, -1 if none
    int find(const std::string &path) const;
    // Whether `scope` is `ancestor` or below it
    bool contains(int ancestor, int scope) const;
    // The deepest scope both are in
    int common(int a, int b) const;
    // The cells of a scope and the scopes below it
    std::vector<IdString> cellsBelow(int scope) const;
    // The path components of a dotted name, without the last one (the object's own name); names in
    // brackets stay whole ("gen[1].u.x" -> gen[1], u); empty for private names ("$...")
    static std::vector<std::string> scopeOfName(const std::string &name);

  private:
    dict<std::string, int> by_path;
    int intern(const std::vector<std::string> &path, const std::string &type = "");
};

// The bels of the device by type, for utilisation figures. A site is a grid location (x, y) that has
// bels of the type (GateMate: a CPE, whose halves are bels of several types).
struct BelCensus
{
    struct PerType
    {
        int total = 0;
        pool<std::pair<int, int>> sites;
    };
    std::map<std::string, PerType> types;
    static BelCensus build(const Context *ctx);
    int total(const std::string &type) const;
    int sites(const std::string &type) const;
    // Sites with bels of this type inside the box (grid coordinates, inclusive)
    int sitesIn(const std::string &type, const BoundingBox &box) const;
    // Of these sites, the ones with bels of this type
    int sitesOf(const std::string &type, const pool<std::pair<int, int>> &used) const;
};

// What the cells of a scope (and the scopes below it) use
struct ScopeUsage
{
    int cells = 0, placed = 0;
    std::map<std::string, int> by_bel_type;           // placed cells
    std::map<std::string, int> unplaced_by_cell_type; // unplaced cells
    BoundingBox box;                                  // of the placed cells, grid coordinates
    pool<std::pair<int, int>> sites;                  // the sites of the placed cells
    std::map<std::string, int> regions;               // region constraints ("" none) -> cells
    static ScopeUsage of(const Context *ctx, const std::vector<IdString> &cells);
    // The bel type most placed cells are on, "" if none is placed
    std::string mainBelType() const;
};

NEXTPNR_NAMESPACE_END

#endif // DESIGN_SCOPES_H
