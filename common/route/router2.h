/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2019  gatecat <gatecat@ds0.me>
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

#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

inline float default_base_cost(Context *ctx, WireId wire, PipId pip, float crit_weight)
{
    (void)crit_weight; // unused
    return ctx->getDelayNS(ctx->getPipDelay(pip).maxDelay() + ctx->getWireDelay(wire).maxDelay() +
                           ctx->getDelayEpsilon());
}

struct Router2Cfg
{
    Router2Cfg(Context *ctx);

    // Maximum iterations for backwards routing attempt
    int backwards_max_iter;
    // Maximum iterations for backwards routing attempt for global nets
    int global_backwards_max_iter;
    // Padding added to bounding boxes to account for imperfect routing,
    // congestion, etc
    int bb_margin_x, bb_margin_y;
    // Cost factor added to input pin wires; effectively reduces the
    // benefit of sharing interconnect
    float ipin_cost_adder;
    // Cost factor for "bias" towards center location of net
    float bias_cost_factor;
    // Starting current and historical congestion cost factor
    float init_curr_cong_weight, hist_cong_weight;
    // Current congestion cost multiplier
    float curr_cong_mult;

    // Weight given to delay estimate in A*. Higher values
    // mean faster and more directed routing, at the risk
    // of choosing a less congestion/delay-optimal route
    float estimate_weight;
    // The weight for critical connections; it slides from estimate_weight for uncritical ones (their
    // crit_weight 1) to this one for the most critical (crit_weight 0.05). Defaults to estimate_weight.
    // TEMPORARY: no gain in the GateMate timing experiments (himbaechel/uarch/gatemate/docs/timing_experiments.md)
    float critical_estimate_weight;

    // Print additional performance profiling information
    bool perf_profile = false;

    std::string heatmap;
    std::function<float(Context *ctx, WireId wire, PipId pip, float crit_weight)> get_base_cost = default_base_cost;

    // Give up after this many iterations of the main loop if routing is still overused (for callers
    // that route a small change and can undo it); -1 for no limit
    int max_iterations = -1;

    // If set, adjusts the criticality the router gives a connection (which orders the nets and scales
    // their congestion cost), e.g. by what earlier routing runs learnt. TEMPORARY: only the GateMate
    // routing learning uses it, which gave no gain (himbaechel/uarch/gatemate/docs/timing_experiments.md)
    std::function<float(const PortRef &user, float crit)> adjust_crit;
};

// Returns false if it gave up (see Router2Cfg::max_iterations)
bool router2(Context *ctx, const Router2Cfg &cfg);

NEXTPNR_NAMESPACE_END
