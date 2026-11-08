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

// The logic check (common/kernel/logic_check.h) on GateMate: a packed reference against the design after routing,
// after changing the logic by hand, and with a mistake.

#include <sstream>
#include "logic_check.h"
#include "testing.h"

#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {

std::string describe(const LogicCheckResult &res, Context *ctx)
{
    std::ostringstream s;
    s << res.summary() << "\n";
    for (auto &i : res.issues) {
        if (i.kind == LogicIssue::UNPROVEN)
            continue;
        s << "  " << i.message << "\n";
        // The cells between the sink and the proven nets, with their connections
        for (IdString c : i.cone) {
            auto found = ctx->cells.find(c);
            if (found == ctx->cells.end())
                continue;
            s << "    " << c.str(ctx) << " (" << found->second->type.str(ctx) << "):";
            for (auto &p : found->second->ports)
                if (p.second.net)
                    s << " " << p.first.str(ctx) << "=" << p.second.net->name.str(ctx);
            for (auto &p : found->second->params)
                s << " " << p.first.str(ctx) << ":" << p.second.to_string();
            s << "\n";
        }
    }
    return s.str();
}

int count(const LogicCheckResult &res, LogicIssue::Kind kind)
{
    int n = 0;
    for (auto &i : res.issues)
        n += i.kind == kind;
    return n;
}

} // namespace

class GateMateLogicCheckTest : public GateMateTest
{
  protected:
    // x, y, z, w top inputs; a = x & y; b = a ^ z; c = b | w; registers on b and c
    NetInfo *a = nullptr, *b = nullptr, *c = nullptr, *x = nullptr, *y = nullptr, *z = nullptr, *w = nullptr;
    IdString na, nb, nx, ny, nz;
    void build()
    {
        NetInfo *clk = global_clock("clk");
        x = input("x");
        y = input("y");
        z = input("z");
        w = input("w");
        a = lut2("a", 0b1000, x, y);
        b = lut2("b", 0b0110, a, z);
        c = lut2("c", 0b1110, b, w);
        CellInfo *rb = dff("rb", b, clk);
        CellInfo *rc = dff("rc", c, clk);
        top_output("ob", rb->getPort(id_Q));
        top_output("oc", rc->getPort(id_Q));
        na = a->name;
        nb = b->name;
        nx = x->name;
        ny = y->name;
        nz = z->name;
    }
    NetInfo *net(IdString name) { return ctx->nets.at(name).get(); }
    CellInfo *driver(IdString name) { return net(name)->driver.cell; }
};

TEST_F(GateMateLogicCheckTest, packed_design_matches_itself)
{
    init_context();
    build();
    ASSERT_TRUE(packs());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << describe(res, ctx);
    EXPECT_EQ(res.unproven, 0) << describe(res, ctx);
    EXPECT_GT(res.sinks, 0);
    EXPECT_EQ(res.proven, res.sinks) << describe(res, ctx);
}

TEST_F(GateMateLogicCheckTest, routed_design_matches_packed_reference)
{
    init_context();
    build();
    ASSERT_TRUE(packs());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    ctx->place();
    ctx->route();
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << describe(res, ctx);
    EXPECT_EQ(res.proven, res.sinks) << describe(res, ctx);
    EXPECT_EQ(count(res, LogicIssue::NEW_VARIABLE), 0) << describe(res, ctx);
}

TEST_F(GateMateLogicCheckTest, snapshot_survives_write_and_read)
{
    init_context();
    build();
    ASSERT_TRUE(packs());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    std::stringstream file;
    ref.write(file);
    LogicSnapshot back;
    ASSERT_EQ(back.read(ctx, file), "");
    EXPECT_EQ(back.cells.size(), ref.cells.size());
    LogicCheckResult res = checkLogic(ctx, back);
    EXPECT_EQ(res.mismatched, 0) << describe(res, ctx);
    EXPECT_EQ(res.proven, res.sinks) << describe(res, ctx);
}

// The LUT tree computing b takes over a: L00 = x & y, L01 passes z on, L10 = XOR. The cell of a goes.
TEST_F(GateMateLogicCheckTest, merged_luts_are_proven_equal)
{
    init_context();
    build();
    ASSERT_TRUE(packs());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    // (b drives a register and other logic: the packer computes it in an upper half, next to the register, and its
    // lower half passes it on to c; the cell that reads a is the upper one)
    CellInfo *ca = driver(na);
    ASSERT_NE(ca, nullptr);
    ASSERT_EQ(net(na)->users.entries(), 1u);
    CellInfo *cb = (*net(na)->users.begin()).cell;
    ASSERT_NE(ca, cb);
    ASSERT_EQ(cb->type, id_CPE_L2T4);
    NetInfo *x = net(nx), *y = net(ny), *z = net(nz);
    // (the packer may have put a LUT2 straight on L10: D0_10 / D1_10)
    std::vector<IdString> inputs;
    for (auto &p : cb->ports)
        if (p.second.type == PORT_IN)
            inputs.push_back(p.first);
    for (IdString p : inputs)
        cb->disconnectPort(p);
    for (IdString p : {id_D0_00, id_D1_00, id_D0_01, id_D1_01})
        if (!cb->ports.count(p))
            cb->addInput(p);
    cb->connectPort(id_D0_00, x);
    cb->connectPort(id_D1_00, y);
    cb->connectPort(id_D0_01, z);
    cb->params[id_INIT_L00] = Property(0b1000, 4);
    cb->params[id_INIT_L01] = Property(0b1010, 4); // D0_01
    cb->params[id_INIT_L10] = Property(0b0110, 4);
    // (b's cell may be the lower half the packer added to pass the upper half's result on through COMBIN and L20:
    // L20 passes L10 now)
    if (cb->params.count(id_INIT_L20))
        cb->params[id_INIT_L20] = Property(0b1100, 4);
    // Remove a (its cell no longer drives anything)
    IdString a_cell = ca->name;
    std::vector<IdString> ports;
    for (auto &p : ca->ports)
        ports.push_back(p.first);
    for (IdString p : ports)
        ca->disconnectPort(p);
    ctx->cells.erase(a_cell);
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << describe(res, ctx);
    EXPECT_EQ(res.proven, res.sinks) << describe(res, ctx);
    EXPECT_EQ(count(res, LogicIssue::MISSING_SINK), 0) << describe(res, ctx);

    // A mistake: XNOR instead of XOR shows on both registers (c is computed from b)
    cb->params[id_INIT_L10] = Property(0b1001, 4);
    res = checkLogic(ctx, ref);
    EXPECT_GE(res.mismatched, 2) << describe(res, ctx);
    bool found = false;
    for (auto &i : res.issues)
        if (i.kind == LogicIssue::MISMATCH && i.cell == ctx->id("rb")) {
            found = true;
            EXPECT_FALSE(i.cone.empty());
            EXPECT_NE(i.message.find("reference"), std::string::npos) << i.message;
        }
    EXPECT_TRUE(found) << describe(res, ctx);
}

// Inverting a net by name changes its function but not what the registers get when its users compensate: the net is
// not proven, the registers are.
TEST_F(GateMateLogicCheckTest, inverted_intermediate_net_is_compensated)
{
    init_context();
    build();
    ASSERT_TRUE(packs());
    LogicSnapshot ref = LogicSnapshot::take(ctx);
    CellInfo *ca = driver(na);
    ASSERT_NE(ca, nullptr);
    ASSERT_EQ(ca->type, id_CPE_L2T4);
    // a's tree: invert L10's output by inverting its table
    int l10 = int(ca->params.at(id_INIT_L10).as_int64());
    ca->params[id_INIT_L10] = Property(~l10 & 0xf, 4);
    LogicCheckResult res = checkLogic(ctx, ref);
    EXPECT_GE(res.mismatched, 1) << describe(res, ctx);
    // Compensate in every user of a: invert the input in their first-level LUT2
    for (auto &usr : net(na)->users) {
        CellInfo *u = usr.cell;
        IdString port = usr.port;
        std::string pn = port.str(ctx);
        ASSERT_EQ(pn.substr(0, 1), "D") << pn;
        IdString init = ctx->id("INIT_L" + pn.substr(3, 2));
        int t = int(u->params.at(init).as_int64());
        // D0 selects bit pairs (0,1) / (2,3); D1 selects (0,2) / (1,3)
        int swapped = pn[1] == '0' ? (((t & 0b0101) << 1) | ((t & 0b1010) >> 1))
                                   : (((t & 0b0011) << 2) | ((t & 0b1100) >> 2));
        u->params[init] = Property(swapped, 4);
    }
    res = checkLogic(ctx, ref);
    EXPECT_EQ(res.mismatched, 0) << describe(res, ctx);
    EXPECT_EQ(res.proven, res.sinks) << describe(res, ctx);
}
