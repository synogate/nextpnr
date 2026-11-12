/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  The nextpnr Authors.
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

// Hand routing into a finished LUT cell (ArchAPI::getRouteRetargets / applyRouteRetarget, the GUI's RetargetSinkCmd):
// a net routed to another physical input of a half than its unrouted sink there takes that input, the inputs and
// tables permuted as the virtual LUT2 input and subtree swaps would; only where no routed input has to move along.

#include <map>
#include <vector>
#include "editcommands.h"
#include "testing.h"
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"
#include "himbaechel_constids.h"

USING_NEXTPNR_NAMESPACE

namespace {
// The function of a finished L2T4 half for each assignment of values to `nets` (bit i: the i-th net)
std::vector<int> half_function(const CellInfo *cell, const std::vector<NetInfo *> &nets)
{
    auto table = [&](IdString name) { return int_or_default(cell->params, name, 0); };
    auto eval = [](int lut, int a, int b) { return (lut >> (a | (b << 1))) & 1; };
    std::vector<int> r;
    for (int v = 0; v < (1 << nets.size()); v++) {
        auto value = [&](IdString port) {
            const NetInfo *net = cell->getPort(port);
            for (size_t i = 0; i < nets.size(); i++)
                if (net != nullptr && nets[i] == net)
                    return (v >> i) & 1;
            return 0;
        };
        r.push_back(eval(table(id_INIT_L10), eval(table(id_INIT_L00), value(id_IN1), value(id_IN2)),
                         eval(table(id_INIT_L01), value(id_IN3), value(id_IN4))));
    }
    return r;
}

struct CellState
{
    std::map<std::string, std::string> ports, params;
    bool operator==(const CellState &o) const { return ports == o.ports && params == o.params; }
};
} // namespace

class GateMateRetargetTest : public GateMateTest
{
  protected:
    CellState state(const CellInfo *ci)
    {
        CellState s;
        for (auto &p : ci->ports)
            s.ports[p.first.str(ctx)] = p.second.net ? p.second.net->name.str(ctx) : "-";
        for (auto &p : ci->params)
            s.params[p.first.str(ctx)] = p.second.to_string();
        return s;
    }
    // The offer of getRouteRetargets at `head` for cell.port, if any
    const RouteRetarget *offer(const std::vector<RouteRetarget> &offers, const CellInfo *ci, IdString port)
    {
        for (auto &r : offers)
            if (r.cell == ci->name && r.port == port)
                return &r;
        return nullptr;
    }
};

TEST_F(GateMateRetargetTest, hand_routing_moves_an_unrouted_lut_input_through_the_virtual_swaps)
{
    init_context("CCGM1A1", {"--allow-unconstrained", "--no-cpe-cp", "--no-bridges"});
    NetInfo *clk = global_clock("clk");
    std::vector<NetInfo *> in;
    for (int i = 0; i < 8; i++)
        in.push_back(input(stringf("i%d", i)));
    // Several trees of four different nets (asymmetric tables: a wrong permutation changes the function); the test
    // takes one whose four inputs all come from the routing (the router may feed a second input from a carry line)
    for (int n = 0; n < 6; n++) {
        NetInfo *y = l2t4(stringf("t%d", n), 0b0010, 0b1011, 0b0100, in[n % 8], in[(n + 1) % 8], in[(n + 3) % 8],
                          in[(n + 6) % 8]);
        output(stringf("q%d", n), dff(stringf("fq%d", n), y, clk)->getPort(id_Q));
    }
    place_and_route();
    ASSERT_FALSE(ctx->route_finish_pending);
    CellInfo *t = nullptr;
    for (int n = 0; n < 6 && t == nullptr; n++) {
        CellInfo *c = ctx->cells.at(ctx->id(stringf("t%d", n))).get();
        bool plain = c->type == id_CPE_L2T4;
        for (IdString p : {id_IN1, id_IN2, id_IN3, id_IN4})
            plain &= c->getPort(p) != nullptr;
        if (plain)
            t = c;
    }
    ASSERT_NE(t, nullptr) << "no LUT tree with its four inputs from the routing";
    std::vector<NetInfo *> nets;
    for (IdString p : {id_IN1, id_IN2, id_IN3, id_IN4})
        nets.push_back(t->getPort(p));
    const std::vector<int> function = half_function(t, nets);
    auto pin = [&](IdString p) { return ctx->getBelPinWire(t->bel, p); };

    EditStack es;
    es.setContext(ctx);
    es.setEditMode(true);
    auto unroute = [&](IdString port) {
        IdString net = t->getPort(port)->name;
        WireId sink = pin(port);
        ASSERT_EQ(es.run("unroute", [&](EditBatch &b) { return addUnrouteArc(b, net, sink); }), "");
        ASSERT_FALSE(t->getPort(port)->wires.count(sink));
    };
    auto retarget = [&](IdString port, WireId head) {
        return es.run("retarget",
                      [&](EditBatch &b) { return b.add(std::make_unique<RetargetSinkCmd>(t->name, port, head)); });
    };
    NetInfo *n = t->getPort(id_IN1);
    unroute(id_IN1);
    const CellState base = state(t);

    // The other input of the same LUT2 (A/B swap of L00): refused while IN2 is routed with its own net
    {
        auto offers = ctx->getRouteRetargets(n, pin(id_IN2));
        const RouteRetarget *r = offer(offers, t, id_IN1);
        ASSERT_NE(r, nullptr);
        EXPECT_NE(r->why.find("IN2"), std::string::npos) << r->why;
        EXPECT_FALSE(r->path.empty());
        EXPECT_NE(retarget(id_IN1, pin(id_IN2)), "");
        EXPECT_TRUE(state(t) == base);
    }
    // (the subtree swap moves IN3 and IN4, routed: refused too)
    {
        auto offers = ctx->getRouteRetargets(n, pin(id_IN3));
        const RouteRetarget *r = offer(offers, t, id_IN1);
        ASSERT_NE(r, nullptr);
        EXPECT_NE(r->why, "");
    }
    // At its own input nothing moves, and the virtual selects downhill are refused (no binding them by hand)
    {
        auto offers = ctx->getRouteRetargets(n, pin(id_IN1));
        EXPECT_EQ(offer(offers, t, id_IN1), nullptr);
        int refused = 0;
        for (auto &r : offers)
            if (r.cell == IdString()) {
                EXPECT_NE(r.why, "");
                refused++;
            }
        EXPECT_GT(refused, 0);
    }

    // With IN2 unrouted: the A/B swap is offered and keeps the function; undo restores the cell
    unroute(id_IN2);
    const CellState unrouted2 = state(t);
    {
        auto offers = ctx->getRouteRetargets(n, pin(id_IN2));
        const RouteRetarget *r = offer(offers, t, id_IN1);
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->why, "");
        EXPECT_EQ(retarget(id_IN1, pin(id_IN2)), "");
        EXPECT_EQ(t->getPort(id_IN2), n);
        EXPECT_EQ(t->getPort(id_IN1), nets[1]);
        EXPECT_EQ(half_function(t, nets), function);
        es.undo();
        EXPECT_TRUE(state(t) == unrouted2);
        EXPECT_EQ(half_function(t, nets), function);
    }

    // With all of the half's inputs unrouted: the subtree swap (IN1 -> IN3) and subtree plus A/B swap (IN1 -> IN4)
    unroute(id_IN3);
    unroute(id_IN4);
    const CellState unrouted = state(t);
    for (IdString to : {id_IN3, id_IN4}) {
        SCOPED_TRACE(to.c_str(ctx));
        auto offers = ctx->getRouteRetargets(n, pin(to));
        const RouteRetarget *r = offer(offers, t, id_IN1);
        ASSERT_NE(r, nullptr);
        EXPECT_EQ(r->why, "");
        EXPECT_EQ(r->path.size(), to == id_IN3 ? 2u : 3u); // (the 2:1 mux of a second input in front)
        // Drawn as one line: from the head's input along its bus into the 4:1 mux of the logical input the sink is
        // (not into the mux of its own LUT2, where the pip alone goes), and across that mux
        {
            std::vector<const GraphicElement *> lines;
            for (auto &el : r->graphics)
                if (el.type == GraphicElement::TYPE_LINE || el.type == GraphicElement::TYPE_ARROW)
                    lines.push_back(&el);
            ASSERT_GE(lines.size(), 3u);
            for (size_t i = 0; i + 1 < lines.size(); i++)
                EXPECT_TRUE(std::abs(lines[i]->x2 - lines[i + 1]->x1) < 1e-4f &&
                            std::abs(lines[i]->y2 - lines[i + 1]->y1) < 1e-4f)
                        << "a gap after line " << i << " (" << lines[i]->x2 << ", " << lines[i]->y2 << ") - ("
                        << lines[i + 1]->x1 << ", " << lines[i + 1]->y1 << ")";
        }
        EXPECT_EQ(retarget(id_IN1, pin(to)), "");
        EXPECT_EQ(t->getPort(to), n);
        EXPECT_EQ(half_function(t, nets), function);
        // (nothing virtual bound)
        for (auto &w : n->wires)
            EXPECT_TRUE(w.second.pip == PipId() || std::find(r->path.begin(), r->path.end(), w.second.pip) ==
                                                           r->path.end());
        es.undo();
        EXPECT_TRUE(state(t) == unrouted);
    }
    // Undoing the rip-ups brings back the routed design
    while (es.undoStack()->canUndo())
        es.undo();
    EXPECT_TRUE(ctx->checkRoutedDesign());
    EXPECT_EQ(half_function(t, nets), function);
}
