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

// Design scopes (common/kernel/design_scopes.h, the GUI's Hierarchy dock): which instance of the
// HDL hierarchy each cell of a flattened design is put into, and what the cells use of the device.

#include "design_scopes.h"
#include <algorithm>
#include <map>
#include <sstream>
#include "json_frontend.h"
#include "jsonwrite.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

class GateMateDesignScopesTest : public GateMateTest
{
  protected:
    // A LUT2 on these nets (nullptr: none); the output net is created with the name given
    CellInfo *lut(const std::string &name, NetInfo *i0, NetInfo *i1, const std::string &out)
    {
        CellInfo *c = create_cell_ptr(id_CC_LUT2, name);
        c->params[id_INIT] = Property(0b1000, 4);
        if (i0)
            c->connectPort(id_I0, i0);
        if (i1)
            c->connectPort(id_I1, i1);
        if (!out.empty())
            c->connectPort(id_O, net(out));
        return c;
    }

    NetInfo *net(const std::string &name)
    {
        IdString id = ctx->id(name);
        return ctx->nets.count(id) ? ctx->nets.at(id).get() : create_net(name);
    }

    void alias(NetInfo *ni, const std::string &name)
    {
        ni->aliases.push_back(ctx->id(name));
        ctx->net_aliases[ctx->id(name)] = ni->name;
    }

    std::string scope_of(const DesignScopes &ds, const std::string &cell)
    {
        return ds.scopes.at(ds.cell_scope.at(ctx->id(cell))).path;
    }

    DesignScopes::Source source_of(const DesignScopes &ds, const std::string &cell)
    {
        return ds.cell_source.at(ctx->id(cell));
    }
};

TEST_F(GateMateDesignScopesTest, scope_of_name)
{
    using V = std::vector<std::string>;
    EXPECT_EQ(DesignScopes::scopeOfName("cpu.alu.sum"), (V{"cpu", "alu"}));
    EXPECT_EQ(DesignScopes::scopeOfName("sum"), V{});
    EXPECT_EQ(DesignScopes::scopeOfName("$abc$123$n4"), V{});
    // Bit and generate indices stay with their name
    EXPECT_EQ(DesignScopes::scopeOfName("gen[1].u.x[3]"), (V{"gen[1]", "u"}));
    EXPECT_EQ(DesignScopes::scopeOfName("cpu.a.b[2].c"), (V{"cpu", "a", "b[2]"}));
    // A component starting with a digit indexes the memory before it
    EXPECT_EQ(DesignScopes::scopeOfName("cpu.regs.3.1"), V{"cpu"});
    // Names a tool made from another name: not trusted
    EXPECT_EQ(DesignScopes::scopeOfName("x$mult_lower$cpu.mul.p"), V{});
    EXPECT_EQ(DesignScopes::scopeOfName("cpu.$auto$1.y"), V{});
}

TEST_F(GateMateDesignScopesTest, sources_in_order_of_trust)
{
    NetInfo *a = net("a"), *b = net("b");
    // Nets entering an instance have an alias per instance they pass through
    alias(a, "cpu.a");
    alias(a, "cpu.alu.x");

    // 1. the hierarchy nextpnr imported (a netlist it flattened itself)
    ctx->top_module = ctx->id("top");
    ctx->hierarchy[ctx->id("top")].type = ctx->id("top");
    ctx->hierarchy[ctx->id("top/core")].type = ctx->id("core_t");
    CellInfo *h = lut("core.h", a, b, "cpu.alu.h_out"); // the net name would say otherwise
    h->hierpath = ctx->id("top/core");

    // 2. attributes: hdlname names the cell itself too, scopename only its instance
    CellInfo *ha = lut("ha", a, b, "");
    ha->attrs[ctx->id("hdlname")] = Property("cpu alu add0");
    CellInfo *sa = lut("sa", a, b, "");
    sa->attrs[ctx->id("scopename")] = Property("dsp mac");

    // 3. nets: the driven net counts twice, ties go to the deeper instance
    lut("n1", a, b, "cpu.alu.sum");
    lut("n2", b, b, "cpu.alu.sum2"); // b only at the top level: 1 against 2
    lut("n3", net("cpu.alu.sum"), b, "top_out");

    // 4. neighbours: only private nets, next to n1 (over cpu.alu.sum) and to each other
    lut("p1", net("cpu.alu.sum"), nullptr, "$p1");
    lut("p2", net("$p1"), nullptr, "$p2");
    lut("p3", net("$p2"), nullptr, "$p3");

    // nothing to go by
    lut("lone", nullptr, nullptr, "");

    DesignScopes ds = DesignScopes::build(ctx);
    EXPECT_EQ(scope_of(ds, "core.h"), "core");
    EXPECT_EQ(source_of(ds, "core.h"), DesignScopes::SRC_HIERARCHY);
    EXPECT_EQ(ds.scopes.at(ds.find("core")).type, "core_t");
    EXPECT_EQ(ds.scopes.at(0).type, "top");

    EXPECT_EQ(scope_of(ds, "ha"), "cpu.alu");
    EXPECT_EQ(source_of(ds, "ha"), DesignScopes::SRC_ATTRIBUTE);
    EXPECT_EQ(scope_of(ds, "sa"), "dsp.mac");
    EXPECT_EQ(source_of(ds, "sa"), DesignScopes::SRC_ATTRIBUTE);

    EXPECT_EQ(scope_of(ds, "n1"), "cpu.alu");
    EXPECT_EQ(source_of(ds, "n1"), DesignScopes::SRC_NETS);
    EXPECT_EQ(scope_of(ds, "n2"), "cpu.alu");
    EXPECT_EQ(scope_of(ds, "n3"), "") << "drives a top level net, reads one of cpu.alu";

    for (const char *p : {"p2", "p3"}) {
        EXPECT_EQ(scope_of(ds, p), "cpu.alu") << p;
        EXPECT_EQ(source_of(ds, p), DesignScopes::SRC_NEIGHBOURS) << p;
    }
    EXPECT_EQ(source_of(ds, "p1"), DesignScopes::SRC_NETS);

    EXPECT_EQ(scope_of(ds, "lone"), "");
    EXPECT_EQ(source_of(ds, "lone"), DesignScopes::SRC_NONE);

    // Totals over the instances below, the cells of each instance itself
    int cpu = ds.find("cpu"), alu = ds.find("cpu.alu");
    ASSERT_GE(cpu, 0);
    ASSERT_GE(alu, 0);
    EXPECT_EQ(ds.scopes.at(alu).total, 6); // ha n1 n2 p1 p2 p3
    EXPECT_EQ(ds.scopes.at(cpu).total, 6);
    EXPECT_TRUE(ds.scopes.at(cpu).cells.empty());
    EXPECT_EQ(ds.scopes.at(0).total, int(ctx->cells.size()));
    EXPECT_EQ(ds.cellsBelow(cpu).size(), 6u);
    EXPECT_TRUE(ds.contains(cpu, alu));
    EXPECT_FALSE(ds.contains(alu, cpu));
    int counted = 0;
    for (int n : ds.source_count)
        counted += n;
    EXPECT_EQ(counted, int(ctx->cells.size()));
}

TEST_F(GateMateDesignScopesTest, net_into_several_instances_stays_where_they_meet)
{
    // Cells whose only named net is the one they drive, which is visible in several instances: a tie.
    // A clock that enters u_a, u_b and u_b.u_c, which are in different branches: the top level
    lut("clkbuf", nullptr, nullptr, "clk");
    for (const char *a : {"u_a.clk", "u_b.clk", "u_b.u_c.clk"})
        alias(net("clk"), a);
    // The same within u_b: a net driven in u_b entering u_b.u_c and u_b.u_d
    lut("sub", nullptr, nullptr, "u_b.y");
    for (const char *a : {"u_b.u_c.y", "u_b.u_d.y"})
        alias(net("u_b.y"), a);
    // A net passed down a line of instances: the deepest
    lut("deep", nullptr, nullptr, "u_b.z");
    alias(net("u_b.z"), "u_b.u_c.z");
    DesignScopes ds = DesignScopes::build(ctx);
    EXPECT_EQ(scope_of(ds, "clkbuf"), "");
    EXPECT_EQ(scope_of(ds, "sub"), "u_b");
    EXPECT_EQ(scope_of(ds, "deep"), "u_b.u_c");
    EXPECT_EQ(ds.find("u_a"), -1) << "no cell in it";
    EXPECT_EQ(ds.common(ds.find("u_b.u_c"), 0), 0);
    EXPECT_EQ(ds.common(ds.find("u_b.u_c"), ds.find("u_b")), ds.find("u_b"));
}

TEST_F(GateMateDesignScopesTest, nets_constrain_neighbours_decide)
{
    // A register inside blk.inner (its nets are seen there only) and the reset mux in front of it,
    // whose only named net is the reset, seen in every instance: the neighbour decides
    NetInfo *rst = net("rst");
    for (const char *a : {"blk.rst", "blk.inner.rst", "other.rst"})
        alias(rst, a);
    lut("ff", net("blk.inner.d"), net("$m"), "blk.inner.q");
    lut("mux", rst, nullptr, "$m");
    // At the top level: the XOR of two outputs of different instances (each seen at the top level
    // and in its instance)
    NetInfo *pa = net("pa"), *qb = net("qb");
    alias(pa, "blk.o");
    alias(qb, "other.o");
    lut("xor", pa, qb, "$x");
    lut("blk_drv", net("blk.k"), nullptr, "pa");
    lut("other_drv", net("other.k"), nullptr, "qb");
    DesignScopes ds = DesignScopes::build(ctx);
    EXPECT_EQ(scope_of(ds, "ff"), "blk.inner");
    EXPECT_EQ(source_of(ds, "ff"), DesignScopes::SRC_NETS);
    EXPECT_EQ(scope_of(ds, "mux"), "blk.inner");
    EXPECT_EQ(source_of(ds, "mux"), DesignScopes::SRC_NEIGHBOURS);
    EXPECT_EQ(scope_of(ds, "xor"), "");
    EXPECT_EQ(source_of(ds, "xor"), DesignScopes::SRC_NETS);
    EXPECT_EQ(scope_of(ds, "blk_drv"), "blk");
    EXPECT_EQ(scope_of(ds, "other_drv"), "other");
}

TEST_F(GateMateDesignScopesTest, saved_and_loaded_design_keeps_aliases_and_scopes)
{
    // Nets with names in instances, the ports of which are seen at the top level too
    NetInfo *a = top_input("a"), *b = top_input("b");
    alias(a, "blk.a");
    alias(b, "blk.inner.b");
    NetInfo *x = lut2("blk.l1", 0b1000, a, a);
    x->aliases.clear();
    ctx->renameNet(x->name, ctx->id("blk.x"));
    alias(x, "blk.x");
    alias(x, "blk.inner.x");
    NetInfo *y = lut2("blk.inner.l2", 0b0110, x, b);
    ctx->renameNet(y->name, ctx->id("y_top"));
    alias(y, "y_top");
    alias(y, "blk.inner.y");
    top_output("y", y);
    place_and_route();
    // (by name: the IdStrings are those of the context, which goes)
    DesignScopes before = DesignScopes::build(ctx);
    std::map<std::string, std::string> scope_before;
    for (auto &cs : before.cell_scope)
        scope_before[cs.first.str(ctx)] = before.scopes.at(cs.second).path;
    // (nets without pins are not loaded back)
    std::map<std::string, std::vector<std::string>> aliases;
    for (auto &n : ctx->nets)
        for (IdString alias : n.second->aliases)
            if (n.second->driver.cell != nullptr || !n.second->users.empty())
                aliases[n.first.str(ctx)].push_back(alias.str(ctx));

    std::ostringstream out;
    std::string filename = "saved.json";
    ASSERT_TRUE(write_json_file(out, filename, ctx));
    init_context();
    std::istringstream in(out.str());
    ASSERT_TRUE(parse_json(in, filename, ctx));

    // The aliases come back (not as an attribute), the net names stay
    for (auto &n : aliases) {
        IdString name = ctx->id(n.first);
        ASSERT_TRUE(ctx->nets.count(name)) << n.first;
        NetInfo *ni = ctx->nets.at(name).get();
        EXPECT_FALSE(ni->attrs.count(ctx->id("nextpnr_aliases"))) << n.first;
        for (const std::string &a : n.second) {
            IdString alias = ctx->id(a);
            EXPECT_NE(std::find(ni->aliases.begin(), ni->aliases.end(), alias), ni->aliases.end())
                    << n.first << " lost " << a;
            EXPECT_EQ(ctx->net_aliases.at(alias), name);
        }
    }
    DesignScopes after = DesignScopes::build(ctx);
    EXPECT_EQ(after.scopes.size(), before.scopes.size());
    for (auto &cs : scope_before) {
        IdString cell = ctx->id(cs.first);
        ASSERT_TRUE(after.cell_scope.count(cell)) << cs.first;
        EXPECT_EQ(after.scopes.at(after.cell_scope.at(cell)).path, cs.second) << cs.first;
    }
    EXPECT_NE(after.find("blk.inner"), -1);
    EXPECT_GE(aliases.size(), 2u);
}

TEST_F(GateMateDesignScopesTest, instances_without_cells_dropped_children_in_natural_order)
{
    NetInfo *a = net("a");
    // Visible in "empty" and "empty.inner" (aliases), but no cell is there
    alias(a, "empty.inner.a");
    for (const char *f : {"fifo10", "fifo2", "fifo1"})
        lut(std::string(f) + "_q", a, nullptr, std::string(f) + ".q");
    DesignScopes ds = DesignScopes::build(ctx);
    EXPECT_EQ(ds.find("empty"), -1);
    EXPECT_EQ(ds.find("empty.inner"), -1);
    std::vector<std::string> names;
    for (int k : ds.scopes.at(0).children)
        names.push_back(ds.scopes.at(k).name);
    EXPECT_EQ(names, (std::vector<std::string>{"fifo1", "fifo2", "fifo10"}));
    for (int s = 1; s < int(ds.scopes.size()); s++)
        EXPECT_GT(ds.scopes.at(s).total, 0) << ds.scopes.at(s).path;
}

TEST_F(GateMateDesignScopesTest, usage_of_placed_and_unplaced_cells)
{
    NetInfo *a = top_input("a"), *b = top_input("b");
    for (int i = 0; i < 4; i++)
        top_output(stringf("y%d", i), lut2(stringf("blk.l%d", i), 0b1000, a, b));
    std::vector<IdString> cells;
    for (auto &c : ctx->cells)
        cells.push_back(c.first);
    ScopeUsage before = ScopeUsage::of(ctx, cells);
    EXPECT_EQ(before.cells, int(cells.size()));
    EXPECT_EQ(before.placed, 0);
    EXPECT_EQ(before.unplaced_by_cell_type.at("CC_LUT2"), 4);
    EXPECT_EQ(before.mainBelType(), "");

    BelCensus census = BelCensus::build(ctx);
    EXPECT_EQ(census.sites("CPE_LT_L"), 20480) << "one CPE per site";
    EXPECT_EQ(census.total("CPE_LT_L"), 20480);
    EXPECT_EQ(census.total("no such type"), 0);

    place_and_route();
    cells.clear();
    for (auto &c : ctx->cells)
        cells.push_back(c.first);
    ScopeUsage u = ScopeUsage::of(ctx, cells);
    EXPECT_EQ(u.placed, u.cells);
    EXPECT_TRUE(u.unplaced_by_cell_type.empty());
    int n = 0;
    for (auto &t : u.by_bel_type)
        n += t.second;
    EXPECT_EQ(n, u.placed);
    EXPECT_LE(u.box.x0, u.box.x1);
    EXPECT_LE(u.box.y0, u.box.y1);
    std::string main = u.mainBelType();
    ASSERT_FALSE(main.empty());
    // Each site of the main type in use lies in the box
    int used = census.sitesOf(main, u.sites);
    EXPECT_GT(used, 0);
    EXPECT_LE(used, census.sitesIn(main, u.box));
    EXPECT_EQ(u.regions.size(), 1u);
    EXPECT_EQ(u.regions.count(""), 1u);
}
