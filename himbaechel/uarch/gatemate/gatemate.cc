/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2024  The Project Peppercorn Authors.
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

#include <algorithm>
#include <fstream>
#include <regex>
#include <utility>

#include "gatemate.h"
#include "log.h"
#include "nextpnr_assertions.h"
#include "placer_heap.h"
#include "placer_static.h"
#include "router2.h"
#include "timing.h"
#include "timing_opt.h"

#define GEN_INIT_CONSTIDS
#define HIMBAECHEL_CONSTIDS "uarch/gatemate/constids.inc"

#include "himbaechel_constids.h"

NEXTPNR_NAMESPACE_BEGIN

GateMateImpl::~GateMateImpl() {};

po::options_description GateMateImpl::getUArchOptions()
{
    po::options_description specific("GateMate specific options");
    specific.add_options()("out", po::value<std::string>(), "textual configuration bitstream output file");
    specific.add_options()("ccf", po::value<std::string>(), "name of constraints file");
    specific.add_options()("allow-unconstrained", "allow unconstrained IOs");
    specific.add_options()("fpga_mode", po::value<std::string>(),
                           "performance mode (1:lowpower, 2:economy, 3:speed (default))");
    specific.add_options()("time_mode", po::value<std::string>(), "timing mode (1:best, 2:typical, 3:worst (default))");
    specific.add_options()("strategy", po::value<std::string>(),
                           "multi-die clock placement strategy (mirror (default), full or clk1)");
    specific.add_options()("force_die", po::value<std::string>(), "force specific die (example 1A,1B...)");
    specific.add_options()("clk-cp", "use CP lines for CLK and EN");
    specific.add_options()("no-cpe-cp", "do not use CP lines pass through CPE");
    specific.add_options()("no-bridges", "do not use CPE in bridge mode");
    specific.add_options()("as-partition",
                           "implement the design as a partition: top level ports become unrouted boundary nets instead "
                           "of IO pads, for importing the result with partition");
    specific.add_options()("partition-unrouted",
                           "with as-partition: write the partition placed only, after placing (run with --no-route); "
                           "the design importing it routes its nets");
    specific.add_options()("partition", po::value<std::vector<std::string>>(),
                           "<inst>:<file>[@dx,dy][,key=value..]: replace the blackbox instance <inst> with the placed "
                           "(and routed) partition from <file> (written with as-partition), moved by dx, dy tiles; "
                           "keys load, place and route as partition-load, partition-place and partition-route");
    specific.add_options()("partition-load", po::value<std::string>(),
                           "what imported partitions bring: routing (default for routed files) or placement");
    specific.add_options()("partition-place", po::value<std::string>(),
                           "imported partitions' cells after placing: repair (default; the passes after placement may "
                           "move them) or locked");
    specific.add_options()("partition-route", po::value<std::string>(),
                           "imported partitions' routing: open (default; the router and the repairs may change it), "
                           "repair (only the repairs after routing) or locked");
    specific.add_options()("partition-open",
                           "let the design place cells in the areas of imported partitions too, everywhere but the "
                           "tiles of their cells (by default it keeps out of their whole rectangles)");
    // The options from here on come from the timing experiments (docs/timing_experiments.md). Those
    // marked TEMPORARY gave no gain; they and the code behind them are to go once the comparison has
    // been repeated with the timing model fixed.
    specific.add_options()("lookahead", po::value<std::string>(),
                           "router delay estimate: linear (default) or table (shortest paths on the empty chip)");
    specific.add_options()("place-delay", po::value<std::string>(), "placer delay estimate: linear (default) or table");
    specific.add_options()("lookahead-radius", po::value<int>(), "radius in tiles of the delay tables (default 12)");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("sb-y-cost", po::value<float>(),
                           "extra router cost (ns) of a switch box Y output for non-critical nets (default 0)");
    specific.add_options()(
            "sb-ydiag-cost", po::value<float>(),
            "extra router cost (ns) of a small switch box's diagonal mux for non-critical nets (default 0)");
    specific.add_options()("route-seed", po::value<uint64_t>(),
                           "reseed the random generator before routing, to vary routing on the same placement");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("route-runs", po::value<int>(),
                           "after routing, route again from scratch this many times with learnt criticality, keeping "
                           "the best (default 0)");
    specific.add_options()("route-learn", po::value<std::string>(),
                           "how route-runs learn criticality: max (default), ema, scale or none");
    specific.add_options()("route-learn-step", po::value<float>(), "step of the ema and scale learning (default 0.25)");
    specific.add_options()("route-repair", po::value<int>(),
                           "after routing, rounds of re-routing critical connections on their ideal path (default: until "
                           "timing is met or nothing is left to try; 0 turns it off)");
    // No effect any more (the fixes are the default); kept for the scripts that pass it
    specific.add_options()(
            "timing-fixes",
            "no effect: the fixes of the timing bugs it turned on (tests/BUGS.md 7, 23, 24, 25, 26, 28, 29, 31, 33, "
            "37) are the default now (kept so that scripts using it keep working)");
    // TEMPORARY until measured (docs/timing_experiments.md)
    specific.add_options()("rewrite-crit-in", po::value<std::string>(),
                           "apply mux-chains, fuse-luts and enable-to-lut only to cells driving nets that were "
                           "critical in an earlier run (a place-crit-out file)");
    specific.add_options()("rewrite-crit-min", po::value<float>(),
                           "criticality from rewrite-crit-in a net needs (default 0.9)");
    // TEMPORARY until measured (docs/timing_experiments.md)
    specific.add_options()("rewrite-depth", po::value<float>(),
                           "apply mux-chains, fuse-luts and enable-to-lut only on paths at least this fraction as "
                           "deep (in logic levels) as the deepest");
    // TEMPORARY until measured (docs/timing_experiments.md)
    specific.add_options()("cpe-merge", po::value<int>(),
                           "before packing, merge two levels of LUTs into one CPE where they fit: 1 where the first "
                           "feeds only the second, 2 also with a copy of the first");
    // TEMPORARY until measured (docs/timing_experiments.md)
    specific.add_options()("mux-chains", "before packing, merge chains of two 2:1 multiplexers into 4:1 multiplexers");
    // TEMPORARY until measured (docs/timing_experiments.md)
    specific.add_options()("enable-to-lut", po::value<int>(),
                           "before packing, move the enables of registers fed through a pass-through LUT into that LUT "
                           "(D = EN ? D : Q), for enables feeding at least this many registers");
    specific.add_options()("fuse-luts", "before packing, fold inverters and fuse LUTs into the LUTs they feed");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("net-repair", po::value<int>(),
                           "after routing, rounds of re-routing nets with several critical sinks (default 0)");
    specific.add_options()("place-repair", po::value<int>(),
                           "after routing, rounds of moving cells on the critical paths (LUT trees, registers with "
                           "their LUT, 5-input LUT pairs, 4:1 multiplexers) and of computing LUT2s (or copies) in the "
                           "first level of the LUT2 they feed (default: until timing is met or nothing is left to try; "
                           "0 turns it off)");
    specific.add_options()("repair-margin", po::value<float>(),
                           "percent above each clock's constraint route-repair and place-repair aim for when run "
                           "until timing is met (default 0)");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("replicate-fanout", po::value<int>(),
                           "before routing, copy LUTs driving critical nets of at least this many far-spread sinks");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("place-crit-in", po::value<std::string>(),
                           "file of net criticalities (net, criticality) the placer treats as a minimum");
    specific.add_options()(
            "place-crit-mode", po::value<std::string>(),
            "how the placer uses place-crit-in: max (a minimum criticality, default) or scale (a factor)");
    specific.add_options()("place-crit-out", po::value<std::string>(),
                           "file to write each net's criticality after routing to (its most critical sink)");
    specific.add_options()("report-crit-routing",
                           "after routing, compare the critical paths' routing with the delay tables");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("timing-opt", "after placement, move LUT trees on critical paths (nextpnr's timing_opt)");
    // TEMPORARY: +1.5% at most, see docs/timing_experiments.md; goes unless it pays off after the timing fixes
    specific.add_options()("lut-input-swap", "before routing, swap the input pairs of LUT trees to save plane changes");
    specific.add_options()(
            "lut-swap-rounds", po::value<int>(),
            "after routing, rounds of swapping LUT input pairs or CPE halves on critical paths (default 0)");
    specific.add_options()("lut-swap-moves", po::value<std::string>(),
                           "moves for lut-swap-rounds: pairs, halves or both (default)");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()("place-density-weight", po::value<float>(),
                           "weight of a pin density penalty in the annealing placer (default 0, off)");
    specific.add_options()("place-density-pins", po::value<float>(),
                           "pins per tile above which the density penalty starts (default 8)");
    specific.add_options()("place-density-bin", po::value<int>(),
                           "bin size in tiles of the density penalty (default 4)");
    // TEMPORARY: no gain, see docs/timing_experiments.md; goes with its code after the ablation is redone
    specific.add_options()(
            "router-critical-estimate-weight", po::value<float>(),
            "weight of the delay estimate for the most critical connections (default: as for the others)");
    specific.add_options()("router-estimate-weight", po::value<float>(),
                           "weight of the delay estimate in the router's search (router2 default 1.25)");
    return specific;
}

static int parse_mode(const std::string &val, const std::map<std::string, int> &map, const char *error_msg)
{
    try {
        int i = std::stoi(val);
        if (i >= 1 && i <= 3)
            return i;
    } catch (...) {
        auto it = map.find(val);
        if (it != map.end())
            return it->second;
    }
    log_error("Invalid mode='%s', %s", val.c_str(), error_msg);
}

void GateMateImpl::init_database(Arch *arch)
{
    const ArchArgs &args = arch->args;
    init_uarch_constids(arch);
    arch->load_chipdb(stringf("gatemate/chipdb-%s.bin", args.device.c_str()));
    arch->set_package("FBGA324");
    dies = std::stoi(args.device.substr(6));
    fpga_mode = 3;
    timing_mode = 3;

    static const std::map<std::string, int> fpga_map = {{"lowpower", 1}, {"economy", 2}, {"speed", 3}};
    static const std::map<std::string, int> timing_map = {{"best", 1}, {"typical", 2}, {"worst", 3}};

    if (args.options.count("fpga_mode"))
        fpga_mode = parse_mode(args.options["fpga_mode"].as<std::string>(), fpga_map,
                               "performance valid values are {1:lowpower, 2:economy, 3:speed}.\n");
    if (args.options.count("time_mode"))
        timing_mode = parse_mode(args.options["time_mode"].as<std::string>(), timing_map,
                                 "timing valid values are {1:best, 2:typical, 3:worst}.\n");

    std::string speed_grade = "";
    switch (timing_mode) {
    case 1:
        speed_grade = "best_";
        break;
    case 2:
        speed_grade = "typ_";
        break;
    default:
        speed_grade = "worst_";
        break;
    }
    log_info("Using timing mode '%s'.\n", timing_mode == 1   ? "BEST"
                                          : timing_mode == 2 ? "TYPICAL"
                                          : timing_mode == 3 ? "WORST"
                                                             : "");

    switch (fpga_mode) {
    case 1:
        speed_grade += "lpr";
        break;
    case 2:
        speed_grade += "eco";
        break;
    default:
        speed_grade += "spd";
    }
    log_info("Using performance mode '%s'.\n", fpga_mode == 1   ? "LOWPOWER"
                                               : fpga_mode == 2 ? "ECONOMY"
                                               : fpga_mode == 3 ? "SPEED"
                                                                : "");
    arch->set_speed_grade(speed_grade);
    use_cp_for_clk = args.options.count("clk-cp") == 1;
    use_cp_for_cpe = args.options.count("no-cpe-cp") == 0;
    use_bridges = args.options.count("no-bridges") == 0;
    partition_unrouted = args.options.count("partition-unrouted") > 0;
    as_partition = args.options.count("as-partition") > 0 || partition_unrouted;
    partition_open = args.options.count("partition-open") > 0;
    auto table = [&](const char *option) {
        if (!args.options.count(option))
            return false;
        std::string value = args.options[option].as<std::string>();
        if (value != "linear" && value != "table")
            log_error("Invalid %s='%s', valid values are linear and table.\n", option, value.c_str());
        return value == "table";
    };
    table_route = table("lookahead");
    table_place = table("place-delay");
    lut_swap = args.options.count("lut-input-swap") > 0;
    timing_opt_after_place = args.options.count("timing-opt") > 0;
    report_crit_routing = args.options.count("report-crit-routing") > 0;
    if (args.options.count("route-repair"))
        route_repair = args.options["route-repair"].as<int>();
    // TEMPORARY: the environment variable lets the bug tests run with the fixes
    if (args.options.count("place-repair"))
        place_repair = args.options["place-repair"].as<int>();
    if (args.options.count("repair-margin"))
        repair_margin = args.options["repair-margin"].as<float>();
    if (args.options.count("net-repair"))
        net_repair = args.options["net-repair"].as<int>();
    if (args.options.count("route-runs"))
        route_runs = args.options["route-runs"].as<int>();
    if (args.options.count("replicate-fanout"))
        replicate_fanout = args.options["replicate-fanout"].as<int>();
    if (args.options.count("route-learn")) {
        route_learn_mode = args.options["route-learn"].as<std::string>();
        if (!pool<std::string>{"max", "ema", "scale", "none"}.count(route_learn_mode))
            log_error("Invalid route-learn='%s', valid values are max, ema, scale and none.\n",
                      route_learn_mode.c_str());
    }
    if (args.options.count("route-learn-step"))
        route_learn_step = args.options["route-learn-step"].as<float>();
    if (args.options.count("lut-swap-rounds"))
        lut_swap_rounds = args.options["lut-swap-rounds"].as<int>();
    if (args.options.count("lut-swap-moves")) {
        std::string moves = args.options["lut-swap-moves"].as<std::string>();
        if (moves != "pairs" && moves != "halves" && moves != "both")
            log_error("Invalid lut-swap-moves='%s', valid values are pairs, halves and both.\n", moves.c_str());
        swap_pairs = moves != "halves";
        swap_halves = moves != "pairs";
    }
    if (args.options.count("sb-y-cost"))
        sb_y_cost = args.options["sb-y-cost"].as<float>();
    if (args.options.count("sb-ydiag-cost"))
        sb_ydiag_cost = args.options["sb-ydiag-cost"].as<float>();
}

void GateMateImpl::init(Context *ctx)
{
    HimbaechelAPI::init(ctx);
    if (ctx->args.options.count("place-crit-in")) {
        std::string filename = ctx->args.options["place-crit-in"].as<std::string>();
        std::ifstream in(filename);
        if (!in)
            log_error("Cannot read '%s'.\n", filename.c_str());
        std::string name;
        float crit;
        while (in >> name >> crit)
            learnt_net_crit[ctx->id(name)] = crit;
        log_info("Read the criticality of %d nets from '%s'.\n", int(learnt_net_crit.size()), filename.c_str());
        learnt_scale = ctx->args.options.count("place-crit-mode") &&
                       ctx->args.options["place-crit-mode"].as<std::string>() == "scale";
    }
    auto &options = ctx->args.options;
    if (table_route || table_place || lut_swap || lut_swap_rounds > 0 || route_repair != 0 || place_repair != 0) {
        int radius = options.count("lookahead-radius") ? options["lookahead-radius"].as<int>() : 12;
        std::string key =
                stringf("%s %s %d%d%d", ctx->chip_info->name.get(), IdString(ctx->speed_grade->name).c_str(ctx),
                        use_cp_for_clk, use_cp_for_cpe, use_bridges);
        lookahead.build(ctx, radius, [this](PipId pip) { return pip_allowed_by_options(pip); }, key);
    }
    if (sb_y_cost > 0 || sb_ydiag_cost > 0) {
        // Every switch box plane has four Y outputs, each driving a whole node, and a small switch
        // box has only one diagonal mux per plane, through which all plane changes and diagonal hops
        // pass; under congestion these run out first.
        static const std::regex y_mux(R"(SB_(SML\.P\d+\.Y\d_int|BIG\.P\d+\.Y\d))"),
                ydiag_mux(R"(SB_SML\.P\d+\.YDIAG_int)");
        for (auto &type : ctx->chip_info->tile_types) {
            scarce_pip_cost.emplace_back(type.pips.ssize(), 0.0f);
            for (int i = 0; i < type.pips.ssize(); i++) {
                std::string dst = IdString(type.wires[type.pips[i].dst_wire].name).str(ctx);
                if (std::regex_match(dst, y_mux))
                    scarce_pip_cost.back()[i] = sb_y_cost;
                else if (std::regex_match(dst, ydiag_mux))
                    scarce_pip_cost.back()[i] = sb_ydiag_cost;
            }
        }
    }
    if (options.count("place-density-weight"))
        ctx->settings[ctx->id("placer1/densityWeight")] = std::to_string(options["place-density-weight"].as<float>());
    if (options.count("place-density-pins"))
        ctx->settings[ctx->id("placer1/densityPins")] = std::to_string(options["place-density-pins"].as<float>());
    if (options.count("place-density-bin"))
        ctx->settings[ctx->id("placer1/densityBin")] = std::to_string(options["place-density-bin"].as<int>());
    if (options.count("router-critical-estimate-weight"))
        ctx->settings[ctx->id("router2/criticalEstimateWeight")] =
                std::to_string(options["router-critical-estimate-weight"].as<float>());
    if (options.count("router-estimate-weight"))
        ctx->settings[ctx->id("router2/estimateWeight")] =
                std::to_string(options["router-estimate-weight"].as<float>());
    for (const auto &pad : ctx->package_info->pads) {
        available_pads.emplace(IdString(pad.package_pin));
        BelId bel = ctx->getBelByName(IdStringList::concat(IdString(pad.tile), IdString(pad.bel)));
        bel_to_pad.emplace(bel, &pad);
        locations.emplace(std::make_pair(IdString(pad.package_pin), tile_extra_data(bel.tile)->die),
                          ctx->getBelLocation(bel));
    }
    available_pads.emplace(ctx->id("SER_CLK"));
    available_pads.emplace(ctx->id("SER_CLK_N"));
    for (auto bel : ctx->getBels()) {
        auto *ptr = bel_extra_data(bel);
        std::map<IdString, const GateMateBelPinConstraintPOD *> pins;
        for (const auto &p : ptr->constraints)
            pins.emplace(IdString(p.name), &p);
        pin_to_constr.emplace(bel, pins);
        if (ctx->getBelType(bel).in(id_CLKIN, id_GLBOUT, id_PLL, id_USR_RSTN, id_CFG_CTRL, id_SERDES)) {
            locations.emplace(std::make_pair(ctx->getBelName(bel)[1], tile_extra_data(bel.tile)->die),
                              ctx->getBelLocation(bel));
        }
    }
    const auto &sp = reinterpret_cast<const GateMateSpeedGradeExtraDataPOD *>(ctx->speed_grade->extra_data.get());
    for (int i = 0; i < sp->timings.ssize(); i++) {
        timing.emplace(IdString(sp->timings[i].name), &sp->timings[i]);
    }
    for (int num = 0; num < 2; num++) {
        int index = (num == 0) ? 0 : 2;
        ram_signal_clk.emplace(ctx->idf("ENA[%d]", index), RamPinInfo{RamPinKind::CTRL, false, num});
        ram_signal_clk.emplace(ctx->idf("ENB[%d]", index), RamPinInfo{RamPinKind::CTRL, true, num});
        ram_signal_clk.emplace(ctx->idf("GLWEA[%d]", index), RamPinInfo{RamPinKind::CTRL, false, num});
        ram_signal_clk.emplace(ctx->idf("GLWEB[%d]", index), RamPinInfo{RamPinKind::CTRL, true, num});
        ram_signal_clk.emplace(ctx->idf("ECC1B_ERRA[%d]", index), RamPinInfo{RamPinKind::ECC_STATUS, false, num});
        ram_signal_clk.emplace(ctx->idf("ECC1B_ERRB[%d]", index), RamPinInfo{RamPinKind::ECC_STATUS, true, num});
        ram_signal_clk.emplace(ctx->idf("ECC2B_ERRA[%d]", index), RamPinInfo{RamPinKind::ECC_STATUS, false, num});
        ram_signal_clk.emplace(ctx->idf("ECC2B_ERRB[%d]", index), RamPinInfo{RamPinKind::ECC_STATUS, true, num});
        for (int i = 0; i < 20; i++) {
            ram_signal_clk.emplace(ctx->idf("WEA[%d]", i + num * 20), RamPinInfo{RamPinKind::BITMASK, false, num});
            ram_signal_clk.emplace(ctx->idf("WEB[%d]", i + num * 20), RamPinInfo{RamPinKind::BITMASK, true, num});
        }

        for (int i = 0; i < 16; i++) {
            ram_signal_clk.emplace(ctx->idf("ADDRA%d[%d]", num, i), RamPinInfo{RamPinKind::ADDR, false, num});
            ram_signal_clk.emplace(ctx->idf("ADDRB%d[%d]", num, i), RamPinInfo{RamPinKind::ADDR, true, num});
        }

        for (int i = 0; i < 20; i++) {
            ram_signal_clk.emplace(ctx->idf("DIA[%d]", i + num * 20), RamPinInfo{RamPinKind::DATA_IN, false, num});
            ram_signal_clk.emplace(ctx->idf("DOA[%d]", i + num * 20), RamPinInfo{RamPinKind::DATA_OUT, false, num});
            ram_signal_clk.emplace(ctx->idf("DIB[%d]", i + num * 20), RamPinInfo{RamPinKind::DATA_IN, true, num});
            ram_signal_clk.emplace(ctx->idf("DOB[%d]", i + num * 20), RamPinInfo{RamPinKind::DATA_OUT, true, num});
        }
    }
    // A FIFO's status outputs, in the read clock's domain (port A) or the write clock's (port B)
    for (const char *flag : {"F_EMPTY[0]", "F_AL_EMPTY[0]", "FRD_ERR[0]"})
        ram_signal_clk.emplace(ctx->id(flag), RamPinInfo{RamPinKind::FIFO_STATUS, false, 0});
    for (const char *flag : {"F_FULL[0]", "F_AL_FULL[0]", "FWR_ERR[0]"})
        ram_signal_clk.emplace(ctx->id(flag), RamPinInfo{RamPinKind::FIFO_STATUS, true, 0});
    for (int i = 0; i < 16; i++) {
        ram_signal_clk.emplace(ctx->idf("FRD_ADDR[%d]", i), RamPinInfo{RamPinKind::FIFO_STATUS, false, 0});
        ram_signal_clk.emplace(ctx->idf("FWR_ADDR[%d]", i), RamPinInfo{RamPinKind::FIFO_STATUS, true, 0});
    }

    const GateMateChipExtraDataPOD *extra =
            reinterpret_cast<const GateMateChipExtraDataPOD *>(ctx->chip_info->extra_data.get());

    const ArchArgs &args = ctx->args;
    std::string die_name;
    if (args.options.count("force_die"))
        die_name = args.options["force_die"].as<std::string>();
    bool found = false;
    int index = 0;
    for (auto &die : extra->dies) {
        IdString name(die.name);
        index_to_die[index] = name;
        die_to_index[name] = index++;
        ctx->createRectangularRegion(name, die.x1, die.y1, die.x2, die.y2);
        if (die_name == name.c_str(ctx)) {
            found = true;
            forced_die = name;
        }
    }
    if (!die_name.empty() && !found)
        log_error("Unable to select forced die '%s'.\n", die_name.c_str());
}

bool GateMateImpl::isBelLocationValid(BelId bel, bool explain_invalid) const
{
    CellInfo *cell = ctx->getBoundBelCell(bel);
    if (cell == nullptr) {
        return true;
    }
    // The placer keeps out of imported partitions: the bels of their final cells overlap the placer's, and a cell
    // in a gap would be closed in by their locked routing. Cells fixed by packing (next to the IOs) stay.
    if (partition_tiles.count(bel.tile) && cell->belStrength < STRENGTH_FIXED && !partition_cells.count(cell->name))
        return false;
    if (partition_routed_tiles.count(bel.tile) && multiplier_clusters.count(cell->cluster))
        return false;
    // A partition keeps the CPEs next to IOs and the clocking free for the design importing it
    if (as_partition && partition_io_tiles.count(bel.tile) && cell->belStrength < STRENGTH_FIXED)
        return false;

    if (getBelBucketForBel(bel) == id_CPE_FF) {
        Loc loc = ctx->getBelLocation(bel);
        const CellInfo *adj_half = ctx->getBoundBelCell(
                ctx->getBelByLocation(Loc(loc.x, loc.y, loc.z == CPE_FF_L_Z ? CPE_FF_U_Z : CPE_FF_L_Z)));
        if (adj_half) {
            const auto &half_data = fast_cell_info.at(cell->flat_index);
            if (half_data.used) {
                const auto &adj_data = fast_cell_info.at(adj_half->flat_index);
                if (adj_data.used) {
                    if (adj_data.config != half_data.config)
                        return false;
                    if (adj_data.ff_en != half_data.ff_en)
                        return false;
                    if (adj_data.ff_clk != half_data.ff_clk)
                        return false;
                    if (adj_data.ff_sr != half_data.ff_sr)
                        return false;
                }
            }
        }
        return true;
    } else if (getBelBucketForBel(bel) == id_RAM_HALF) {
        Loc loc = ctx->getBelLocation(bel);
        const CellInfo *adj_half =
                ctx->getBoundBelCell(ctx->getBelByLocation(Loc(loc.x, loc.z == RAM_HALF_L_Z ? loc.y - 8 : loc.y + 8,
                                                               loc.z == RAM_HALF_L_Z ? RAM_FULL_Z : RAM_HALF_L_Z)));
        if (adj_half) {
            const auto &half_data = fast_cell_info.at(cell->flat_index);
            if (half_data.used) {
                const auto &adj_data = fast_cell_info.at(adj_half->flat_index);
                if (adj_data.used) {
                    if (adj_data.config != half_data.config)
                        return false;
                }
            }
        }
        return true;
    }

    return true;
}

Loc GateMateImpl::getRelativeConstraint(Loc &root_loc, IdString id) const
{
    Loc child_loc;
    BelId root_bel = ctx->getBelByLocation(root_loc);
    if (pin_to_constr.count(root_bel)) {
        auto &constr = pin_to_constr.at(root_bel);
        if (constr.count(id)) {
            auto &p = constr.at(id);
            child_loc.x = root_loc.x + p->constr_x;
            child_loc.y = root_loc.y + p->constr_y;
            child_loc.z = p->constr_z;
        } else {
            log_error("Constrain info not available for pin '%s'.\n", id.c_str(ctx));
        }
    } else {
        log_error("Bel info not available for constraints.\n");
    }
    return child_loc;
}

bool GateMateImpl::getChildPlacement(const BaseClusterInfo *cluster, Loc root_loc,
                                     std::vector<std::pair<CellInfo *, BelId>> &placement) const
{
    for (auto child : cluster->constr_children) {
        Loc child_loc;
        if (child->constr_z >= PLACE_DB_CONSTR) {
            // Placers also probe root bels that do not have the constraint, which is just not a placement
            auto constr = pin_to_constr.find(ctx->getBelByLocation(root_loc));
            if (constr == pin_to_constr.end())
                return false;
            auto pin = constr->second.find(IdString(child->constr_z - PLACE_DB_CONSTR));
            if (pin == constr->second.end())
                return false;
            child_loc.x = root_loc.x + pin->second->constr_x;
            child_loc.y = root_loc.y + pin->second->constr_y;
            child_loc.z = pin->second->constr_z;
        } else {
            child_loc.x = root_loc.x + child->constr_x;
            child_loc.y = root_loc.y + child->constr_y;
            child_loc.z = child->constr_abs_z ? child->constr_z : (root_loc.z + child->constr_z);
        }
        if (child_loc.x < 0 || child_loc.x >= ctx->getGridDimX())
            return false;
        if (child_loc.y < 0 || child_loc.y >= ctx->getGridDimY())
            return false;
        BelId child_bel = ctx->getBelByLocation(child_loc);
        if (child_bel == BelId() || !this->isValidBelForCellType(child->type, child_bel))
            return false;
        placement.emplace_back(child, child_bel);
        if (!getChildPlacement(child, child_loc, placement))
            return false;
    }
    return true;
}

bool GateMateImpl::getClusterPlacement(ClusterId cluster, BelId root_bel,
                                       std::vector<std::pair<CellInfo *, BelId>> &placement) const
{
    CellInfo *root_cell = get_cluster_root(ctx, cluster);
    placement.clear();
    NPNR_ASSERT(root_bel != BelId());
    Loc root_loc = ctx->getBelLocation(root_bel);
    if (root_cell->constr_abs_z) {
        // Coerce root to absolute z constraint
        root_loc.z = root_cell->constr_z;
        root_bel = ctx->getBelByLocation(root_loc);
    }
    if (root_bel == BelId() || !this->isValidBelForCellType(root_cell->type, root_bel))
        return false;
    placement.emplace_back(root_cell, root_bel);
    return getChildPlacement(root_cell, root_loc, placement);
}

void GateMateImpl::prePlace()
{
    assign_cell_info();
    check_regions();
}

// Report how full the placement regions are, and stop if one can not hold its cells; the placers
// would otherwise search for a long time or, for a region without any fitting bel, fail badly
void GateMateImpl::check_regions()
{
    dict<std::pair<IdString, BelBucketId>, int> demand;
    dict<std::pair<IdString, BelBucketId>, pool<IdString>> types;
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        if (ci->region && ci->region->constr_bels && !ci->isPseudo()) {
            std::pair<IdString, BelBucketId> key{ci->region->name, ctx->getBelBucketForCellType(ci->type)};
            demand[key]++;
            types[key].insert(ci->type);
        }
    }
    if (demand.empty())
        return;
    // The bels of the region that can take any of the cell types of the bucket
    dict<std::pair<IdString, BelBucketId>, int> capacity;
    for (auto &t : types) {
        int &have = capacity[t.first];
        for (BelId bel : ctx->region.at(t.first.first)->bels)
            for (IdString type : t.second)
                if (ctx->isValidBelForCellType(type, bel)) {
                    have++;
                    break;
                }
    }
    std::vector<std::pair<IdString, BelBucketId>> keys;
    for (auto &d : demand)
        keys.push_back(d.first);
    std::sort(keys.begin(), keys.end(), [&](const auto &a, const auto &b) {
        return a.first == b.first ? a.second.str(ctx) < b.second.str(ctx) : a.first.str(ctx) < b.first.str(ctx);
    });
    log_info("Region utilisation:\n");
    bool over = false;
    for (auto &key : keys) {
        int need = demand.at(key), have = capacity.at(key);
        log_info("    %-30s %12s: %6d/%6d %5.1f%%\n", key.first.c_str(ctx), key.second.c_str(ctx), need, have,
                 have ? 100.0 * need / have : 100.0 * need);
        over |= need > have;
    }
    if (over)
        log_error("A placement region is too small for its cells, see the utilisation above.\n");
}

void GateMateImpl::postPlace()
{
    release_partitions();
    if (timing_opt_after_place) {
        // Only cells outside clusters move, so these are LUT trees not merged with a register
        TimingOptCfg cfg(ctx);
        cfg.cellTypes.insert(id_CPE_L2T4);
        timing_opt(ctx, cfg);
    }
    repack();
    ctx->assignArchInfo();
    used_cpes.resize(ctx->getGridDimX() * ctx->getGridDimY());
    pip_data.resize(ctx->getGridDimX() * ctx->getGridDimY());
    pip_mask.resize(ctx->getGridDimX() * ctx->getGridDimY());

    auto set_param_mask_data = [&](CellInfo *cell, IdString param, uint32_t pip_mask, uint32_t &mask, uint32_t &data) {
        if (cell->params.count(param)) {
            mask |= pip_mask;
            if (int_or_default(cell->params, param, 0))
                data |= pip_mask;
        }
    };
    for (auto &cell : ctx->cells) {
        // We need to skip CPE_MULT since using CP outputs is mandatory
        // even if output is actually not connected
        bool marked_used = cell.second.get()->type == id_CPE_MULT;
        // Can not use FF or latch for OUT2 if CPE is used in bridge mode (C_BR drives DIN2 from MUXOUT)
        if (cell.second.get()->type.in(id_CPE_FF, id_CPE_LATCH) &&
            ctx->getBelLocation(cell.second.get()->bel).z == CPE_FF_U_Z)
            marked_used = true;
        if (marked_used)
            used_cpes[cell.second.get()->bel.tile] = true;

        uint32_t mask = pip_mask[cell.second.get()->bel.tile];
        uint32_t data = pip_data[cell.second.get()->bel.tile];
        if (cell.second.get()->type == id_CPE_MULT) {
            mask |= PipMask::IS_MULT;
            data |= PipMask::IS_MULT;
        }
        if (cell.second.get()->type.in(id_CPE_ADDF, id_CPE_ADDF2)) {
            data |= PipMask::IS_ADDF;
            mask |= PipMask::IS_ADDF;
        }
        if (cell.second.get()->type == id_CPE_COMP) {
            data |= PipMask::IS_COMP;
            mask |= PipMask::IS_COMP;
        }
        set_param_mask_data(cell.second.get(), id_C_SELX, PipMask::C_SELX, mask, data);
        set_param_mask_data(cell.second.get(), id_C_SELY1, PipMask::C_SELY1, mask, data);
        set_param_mask_data(cell.second.get(), id_C_SELY2, PipMask::C_SELY2, mask, data);
        set_param_mask_data(cell.second.get(), id_C_SEL_C, PipMask::C_SEL_C, mask, data);
        set_param_mask_data(cell.second.get(), id_C_SEL_P, PipMask::C_SEL_P, mask, data);
        set_param_mask_data(cell.second.get(), id_C_Y12, PipMask::C_Y12, mask, data);
        set_param_mask_data(cell.second.get(), id_C_CX_I, PipMask::C_CX_I, mask, data);
        set_param_mask_data(cell.second.get(), id_C_CY1_I, PipMask::C_CY1_I, mask, data);
        set_param_mask_data(cell.second.get(), id_C_CY2_I, PipMask::C_CY2_I, mask, data);
        set_param_mask_data(cell.second.get(), id_C_PX_I, PipMask::C_PX_I, mask, data);
        set_param_mask_data(cell.second.get(), id_C_PY1_I, PipMask::C_PY1_I, mask, data);
        set_param_mask_data(cell.second.get(), id_C_PY2_I, PipMask::C_PY2_I, mask, data);
        pip_mask[cell.second.get()->bel.tile] = mask;
        pip_data[cell.second.get()->bel.tile] = data;
    }
    if (partition_unrouted) {
        // The partition is complete once placed: as after routing (postRoute), but with no routing kept
        detach_partition_ports();
        save_partition_attrs();
        ctx->settings[ctx->id("partition-unrouted")] = 1;
        timing_analysis(ctx, false, false, false, false, true);
    }
}
void GateMateImpl::configureRouter2(Router2Cfg &cfg)
{
    if (scarce_pip_cost.empty())
        return;
    int y_pips = 0, ydiag_pips = 0;
    for (auto &type : scarce_pip_cost)
        for (float cost : type) {
            y_pips += cost != 0 && cost == sb_y_cost;
            ydiag_pips += cost != 0 && cost == sb_ydiag_cost && cost != sb_y_cost;
        }
    log_info("Extra router cost for non-critical nets: %.3f ns per switch box Y output, %.3f ns per diagonal mux "
             "(%d and %d pips over all tile types).\n",
             sb_y_cost, sb_ydiag_cost, y_pips, ydiag_pips);
    cfg.get_base_cost = [this](Context *ctx, WireId wire, PipId pip, float crit_weight) {
        float cost = default_base_cost(ctx, wire, pip, crit_weight);
        if (pip != PipId())
            cost += crit_weight * scarce_pip_cost[ctx->chip_info->tile_insts[pip.tile].type][pip.index];
        return cost;
    };
}

bool GateMateImpl::pip_allowed_by_options(PipId pip) const
{
    const auto &extra_data = *pip_extra_data(pip);
    if (!use_cp_for_clk && extra_data.type == PipExtra::PIP_EXTRA_MUX) {
        if (extra_data.value == 1 && IdString(extra_data.name).in(id_C_CLKSEL, id_C_ENSEL))
            return false;
    }
    if (!use_cp_for_cpe && extra_data.type == PipExtra::PIP_EXTRA_MUX && extra_data.resource != 0 &&
        extra_data.resource <= PipMask::C_PY2_I) {
        return false;
    }
    if (!use_bridges && extra_data.type == PipExtra::PIP_EXTRA_MUX &&
        IdString(extra_data.name) == ctx->id("CPE.C_SN")) {
        return false;
    }
    return true;
}

bool GateMateImpl::checkPipAvail(PipId pip) const
{
    if (!pip_allowed_by_options(pip))
        return false;
    const auto &extra_data = *pip_extra_data(pip);
    if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.block != 0)) {
        if (pip_mask[pip.tile] & extra_data.block) {
            return false;
        }
    }
    if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.resource != 0)) {
        if (pip_mask[pip.tile] & extra_data.resource) {
            if ((pip_data[pip.tile] & extra_data.resource) != (extra_data.value ? extra_data.resource : 0)) {
                return false;
            }
        }
    }
    if (extra_data.type != PipExtra::PIP_EXTRA_MUX || (extra_data.flags & MUX_ROUTING) == 0)
        return true;
    if (used_cpes[pip.tile])
        return false;
    return true;
}

void GateMateImpl::preRoute()
{
    if (partition_unrouted)
        log_error("A partition written with --vopt partition-unrouted is not routed; run with --no-route.\n");
    if (as_partition)
        find_partition_free_nets();
    if (replicate_fanout > 0)
        replicate_drivers(replicate_fanout);
    if (ctx->args.options.count("route-seed"))
        ctx->rngseed(ctx->args.options["route-seed"].as<uint64_t>());
    if (lut_swap)
        swap_lut_inputs();
    route_mult();
    route_clock();
    ctx->assignArchInfo();

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->wires.empty())
            continue;
        for (auto &w : ni->wires) {
            if (w.second.pip != PipId()) {
                const auto &extra_data = *pip_extra_data(w.second.pip);
                if (extra_data.type == PipExtra::PIP_EXTRA_MUX && extra_data.resource != 0) {
                    pip_mask[w.second.pip.tile] |= extra_data.resource;
                    pip_data[w.second.pip.tile] |= extra_data.value ? extra_data.resource : 0;
                }
            }
        }
    }
    prerouted_nets.clear();
    // (the routing imported with partitions is the router's to change, as is that of the repairs)
    for (auto &net : ctx->nets)
        if (!net.second->wires.empty() && !partition_nets.count(net.first))
            prerouted_nets.insert(net.first);
}

void GateMateImpl::reassign_bridges(NetInfo *start_net, const dict<WireId, PipMap> &net_wires, WireId start_wire,
                                    dict<WireId, IdString> &wire_to_net, int &num)
{
    // Processing list, holds parameters to implement the equivalent of recursive calls.
    // This avoids a stack overflow when recursion becomes deep, as the function
    // has a relatively large stack footprint.
    struct record
    {
        NetInfo *net;
        WireId wire;
    };
    std::vector<record> to_process;
    // Insert start record.
    to_process.push_back({start_net, start_wire});
    // For as long as there are pending records, process them.
    while (!to_process.empty()) {
        // Get the next record to process.
        record cur = to_process.back();
        to_process.pop_back();

        wire_to_net.insert({cur.wire, cur.net->name});

        for (auto pip : ctx->getPipsDownhill(cur.wire)) {
            auto dst = ctx->getPipDstWire(pip);

            // Ignore wires not part of the net
            auto it = net_wires.find(dst);
            if (it == net_wires.end())
                continue;
            // Ignore pips if the wire is driven by another pip.
            if (pip != it->second.pip)
                continue;
            // Ignore wires already visited.
            if (wire_to_net.count(dst))
                continue;

            const auto &extra_data = *pip_extra_data(pip);
            // If not a bridge, just recurse.
            if (extra_data.type != PipExtra::PIP_EXTRA_MUX || !(extra_data.flags & MUX_ROUTING)) {
                // Insert in processing list (recurse).
                to_process.push_back({cur.net, dst});
                continue;
            }

            // We have a bridge that needs to be translated to a bel.
            IdString name = ctx->idf("%s$bridge%d", cur.net->name.c_str(ctx), num);

            IdStringList id = ctx->getPipName(pip);
            Loc loc = ctx->getPipLocation(pip);
            BelId bel = ctx->getBelByLocation({loc.x, loc.y, CPE_BRIDGE_Z});
            CellInfo *cell = ctx->createCell(name, id_CPE_BRIDGE);
            ctx->bindBel(bel, cell, PlaceStrength::STRENGTH_FIXED);
            cell->params[id_C_BR] = Property(Property::State::S1, 1);
            cell->params[id_C_SN] = Property(extra_data.value, 3);

            NetInfo *new_net = ctx->createNet(ctx->idf("%s$muxout", name.c_str(ctx)));
            IdString in_port = ctx->idf("IN%d", extra_data.value + 1);

            auto add_port = [&](const IdString id, PortType dir) {
                cell->ports[id].name = id;
                cell->ports[id].type = dir;
                cell->cell_bel_pins[id] = std::vector{id};
            };

            add_port(in_port, PORT_IN);
            add_port(id_MUXOUT, PORT_OUT);

            cell->connectPort(in_port, cur.net);
            cell->connectPort(id_MUXOUT, new_net);
            pass_backtrace[cell->name][id_MUXOUT] = in_port;

            num++;
            // Insert in processing list (recurse).
            to_process.push_back({new_net, dst});
        }
    }
}

void GateMateImpl::reassign_cplines(NetInfo *ni, const dict<WireId, PipMap> &net_wires, WireId wire,
                                    dict<WireId, IdString> &wire_to_net, int &num, IdString in_port)
{
    wire_to_net.insert({wire, ni->name});

    for (auto pip : ctx->getPipsDownhill(wire)) {
        auto dst = ctx->getPipDstWire(pip);

        // Ignore wires not part of the net
        auto it = net_wires.find(dst);
        if (it == net_wires.end())
            continue;
        // Ignore pips if the wire is driven by another pip.
        if (pip != it->second.pip)
            continue;
        // Ignore wires already visited.
        if (wire_to_net.count(dst))
            continue;

        const auto &extra_data = *pip_extra_data(pip);
        // If not a CP line pip, just recurse.
        if (extra_data.type != PipExtra::PIP_EXTRA_MUX || extra_data.resource == 0) {
            reassign_cplines(ni, net_wires, dst, wire_to_net, num, in_port);
            continue;
        }

        // We have a bridge that needs to be translated to a bel.
        IdStringList id = ctx->getPipName(pip);
        Loc loc = ctx->getPipLocation(pip);
        BelId bel = ctx->getBelByLocation({loc.x, loc.y, CPE_CPLINES_Z});
        CellInfo *cell = ctx->getBoundBelCell(bel);
        if (!cell) {
            IdString name = ctx->idf("cplines$%s", id[0].c_str(ctx));
            cell = ctx->createCell(name, id_CPE_CPLINES);
            auto add_port = [&](const IdString id, PortType dir) {
                cell->ports[id].name = id;
                cell->ports[id].type = dir;
                cell->cell_bel_pins[id] = std::vector{id};
            };

            add_port(id_OUT1, PORT_IN);
            add_port(id_OUT2, PORT_IN);
            add_port(id_COMPOUT, PORT_IN);

            add_port(id_CINX, PORT_IN);
            add_port(id_PINX, PORT_IN);
            add_port(id_CINY1, PORT_IN);
            add_port(id_PINY1, PORT_IN);
            add_port(id_CINY2, PORT_IN);
            add_port(id_PINY2, PORT_IN);

            add_port(id_COUTX, PORT_OUT);
            add_port(id_POUTX, PORT_OUT);
            add_port(id_COUTY1, PORT_OUT);
            add_port(id_POUTY1, PORT_OUT);
            add_port(id_COUTY2, PORT_OUT);
            add_port(id_POUTY2, PORT_OUT);

            ctx->bindBel(bel, cell, PlaceStrength::STRENGTH_FIXED);
        }

        cell->setParam(ctx->getGroupName(ctx->getResourceKeyForPip(pip))[1],
                       Property(extra_data.value, extra_data.bits));

        // We have to discover the ports needed by this config.
        auto input_port_map = dict<IdString, IdString>{
                {ctx->id("CPE.CINX"), id_CINX},
                {ctx->id("CPE.CINY1"), id_CINY1},
                {ctx->id("CPE.CINY2"), id_CINY2},
                {ctx->id("CPE.PINX"), id_PINX},
                {ctx->id("CPE.PINY1"), id_PINY1},
                {ctx->id("CPE.PINY2"), id_PINY2},
                {ctx->id("CPE.OUT1_IN_int"), id_OUT1},
                {ctx->id("CPE.OUT2_IN_int"), id_OUT2},
                {ctx->id("CPE.COMPOUT_IN_int"), id_COMPOUT},
        };

        auto input_port_name = input_port_map.find(ctx->getWireName(ctx->getPipSrcWire(pip))[1]);
        if (input_port_name != input_port_map.end()) {
            if (cell->getPort(input_port_name->second) == nullptr) {
                cell->connectPort(input_port_name->second, ni);
                in_port = input_port_name->second;
            } else
                NPNR_ASSERT(cell->getPort(input_port_name->second) == ni);
        }

        auto output_port_map =
                dict<IdString, IdString>{{ctx->id("CPE.COUTX"), id_COUTX},   {ctx->id("CPE.COUTY1"), id_COUTY1},
                                         {ctx->id("CPE.COUTY2"), id_COUTY2}, {ctx->id("CPE.POUTX"), id_POUTX},
                                         {ctx->id("CPE.POUTY1"), id_POUTY1}, {ctx->id("CPE.POUTY2"), id_POUTY2}};

        auto output_port_name = output_port_map.find(ctx->getWireName(ctx->getPipDstWire(pip))[1]);
        if (output_port_name != output_port_map.end()) {
            NetInfo *new_net =
                    ctx->createNet(ctx->idf("%s$%s", cell->name.c_str(ctx), output_port_name->second.c_str(ctx)));

            cell->addOutput(output_port_name->second);
            cell->connectPort(output_port_name->second, new_net);
            pass_backtrace[cell->name][output_port_name->second] = in_port;

            num++;

            reassign_cplines(new_net, net_wires, dst, wire_to_net, num, in_port);
        } else {
            // this is an internal resource pip; recurse anyway.
            reassign_cplines(ni, net_wires, dst, wire_to_net, num, in_port);
        }
    }
}

void GateMateImpl::postRoute()
{
    release_partition_routing();
    if (route_runs > 0)
        route_learning(route_runs);
    if (net_repair > 0)
        repair_critical_nets(net_repair);
    if (route_repair != 0)
        repair_critical_routing(route_repair);
    if (place_repair != 0) {
        repair_critical_placement(place_repair);
        // The moved trees' new connections can often be repaired too
        if (net_repair > 0)
            repair_critical_nets(net_repair);
        if (route_repair != 0)
            repair_critical_routing(route_repair);
    }
    if (lut_swap_rounds > 0)
        improve_lut_inputs(lut_swap_rounds);
    if (report_crit_routing)
        report_critical_routing();
    if (ctx->args.options.count("place-crit-out")) {
        std::string filename = ctx->args.options["place-crit-out"].as<std::string>();
        std::ofstream out(filename);
        if (!out)
            log_error("Cannot write '%s'.\n", filename.c_str());
        TimingAnalyser tmg(ctx);
        tmg.setup();
        tmg.run();
        for (auto &net : ctx->nets) {
            float crit = 0;
            for (auto &user : net.second->users)
                crit = std::max(crit, tmg.get_criticality(CellPortKey(user)));
            if (crit > 0 && net.first.str(ctx).find_first_of(" \t\n") == std::string::npos)
                out << net.first.str(ctx) << "\t" << crit << "\n";
        }
    }
    if (as_partition) {
        // A partition stays as routed: what follows turns bridges and CP lines into cells and the LUT inputs
        // into the CPE inputs the routing reaches, which needs the routing of the ports, done on import
        detach_partition_ports();
        save_partition_attrs();
        // The timing results kept for the report name the stand-ins just taken out (the paths through the ports
        // are in the log above)
        timing_analysis(ctx, false, false, false, false, true);
        if (ctx->args.options.count("out"))
            log_warning("No bitstream is written for a partition.\n");
        return;
    }
    int num = 0;

    pool<IdString> nets_with_bridges;
    pool<IdString> nets_with_cplines;

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        for (auto &w : ni->wires) {
            if (w.second.pip != PipId()) {
                const auto &extra_data = *pip_extra_data(w.second.pip);
                if (extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_ROUTING)) {
                    nets_with_bridges.insert(ni->name);
                }
            }
        }
    }

    for (auto net_name : nets_with_bridges) {
        auto *ni = ctx->nets.at(net_name).get();
        auto net_wires = ni->wires; // copy wires to preserve across unbind/rebind.
        auto wire_to_net = dict<WireId, IdString>{};
        auto wire_to_port = dict<WireId, std::vector<PortRef>>{};

        for (auto &usr : ni->users)
            for (auto sink_wire : ctx->getNetinfoSinkWires(ni, usr)) {
                auto result = wire_to_port.find(sink_wire);
                if (result == wire_to_port.end())
                    wire_to_port.insert({sink_wire, std::vector<PortRef>{usr}});
                else
                    result->second.push_back(usr);
            }

        // traverse the routing tree to assign bridge nets to wires.
        reassign_bridges(ni, net_wires, ctx->getNetinfoSourceWire(ni), wire_to_net, num);

        for (auto &pair : net_wires)
            ctx->unbindWire(pair.first);

        for (auto &pair : net_wires) {
            auto wire = pair.first;
            auto pip = pair.second.pip;
            auto strength = pair.second.strength;
            auto *net = ctx->nets.at(wire_to_net.at(wire)).get();
            if (pip == PipId())
                ctx->bindWire(wire, net, strength);
            else
                ctx->bindPip(pip, net, strength);

            if (wire_to_port.count(wire)) {
                for (auto sink : wire_to_port.at(wire)) {
                    NPNR_ASSERT(sink.cell != nullptr && sink.port != IdString());
                    sink.cell->disconnectPort(sink.port);
                    sink.cell->connectPort(sink.port, net);
                }
            }
        }
    }

    num = 0;

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        for (auto &w : ni->wires) {
            if (w.second.pip != PipId()) {
                const auto &extra_data = *pip_extra_data(w.second.pip);
                if (extra_data.type == PipExtra::PIP_EXTRA_MUX && extra_data.resource != 0) {
                    nets_with_cplines.insert(ni->name);
                }
            }
        }
    }

    for (auto net_name : nets_with_cplines) {
        auto *ni = ctx->nets.at(net_name).get();
        auto net_wires = ni->wires; // copy wires to preserve across unbind/rebind.
        auto wire_to_net = dict<WireId, IdString>{};
        auto wire_to_port = dict<WireId, std::vector<PortRef>>{};

        for (auto &usr : ni->users)
            for (auto sink_wire : ctx->getNetinfoSinkWires(ni, usr)) {
                auto result = wire_to_port.find(sink_wire);
                if (result == wire_to_port.end())
                    wire_to_port.insert({sink_wire, std::vector<PortRef>{usr}});
                else
                    result->second.push_back(usr);
            }

        // traverse the routing tree to assign bridge nets to wires.
        reassign_cplines(ni, net_wires, ctx->getNetinfoSourceWire(ni), wire_to_net, num, IdString());

        for (auto &pair : net_wires)
            ctx->unbindWire(pair.first);

        for (auto &pair : net_wires) {
            auto wire = pair.first;
            auto pip = pair.second.pip;
            auto strength = pair.second.strength;
            auto *net = ctx->nets.at(wire_to_net.at(wire)).get();
            if (pip == PipId())
                ctx->bindWire(wire, net, strength);
            else
                ctx->bindPip(pip, net, strength);

            if (wire_to_port.count(wire)) {
                for (auto sink : wire_to_port.at(wire)) {
                    NPNR_ASSERT(sink.cell != nullptr && sink.port != IdString());
                    sink.cell->disconnectPort(sink.port);
                    sink.cell->connectPort(sink.port, net);
                }
            }
        }
    }

    dict<IdString, int> cfg;
    dict<IdString, IdString> port_mapping;
    auto add_input = [&](IdString orig_port, IdString port, bool merged) -> bool {
        static dict<IdString, IdString> convert_port = {
                {ctx->id("CPE.IN1"), id_IN1},     {ctx->id("CPE.IN2"), id_IN2},     {ctx->id("CPE.IN3"), id_IN3},
                {ctx->id("CPE.IN4"), id_IN4},     {ctx->id("CPE.IN5"), id_IN1},     {ctx->id("CPE.IN6"), id_IN2},
                {ctx->id("CPE.IN7"), id_IN3},     {ctx->id("CPE.IN8"), id_IN4},     {ctx->id("CPE.PINY1"), id_PINY1},
                {ctx->id("CPE.PINY2"), id_PINY2}, {ctx->id("CPE.CINY2"), id_CINY2}, {ctx->id("CPE.CLK"), id_CLK},
                {ctx->id("CPE.EN"), id_EN},       {ctx->id("CPE.CINX"), id_CINX},   {ctx->id("CPE.PINX"), id_PINX}};
        static dict<IdString, IdString> convert_port_merged = {
                {ctx->id("CPE.IN1"), id_IN1},     {ctx->id("CPE.IN2"), id_IN2},     {ctx->id("CPE.IN3"), id_IN3},
                {ctx->id("CPE.IN4"), id_IN4},     {ctx->id("CPE.IN5"), id_IN5},     {ctx->id("CPE.IN6"), id_IN6},
                {ctx->id("CPE.IN7"), id_IN7},     {ctx->id("CPE.IN8"), id_IN8},     {ctx->id("CPE.PINY1"), id_PINY1},
                {ctx->id("CPE.PINY2"), id_PINY2}, {ctx->id("CPE.CINY2"), id_CINY2}, {ctx->id("CPE.CLK"), id_CLK},
                {ctx->id("CPE.EN"), id_EN},       {ctx->id("CPE.CINX"), id_CINX},   {ctx->id("CPE.PINX"), id_PINX}};
        if (convert_port.count(port)) {
            port_mapping.emplace(orig_port, merged ? convert_port_merged[port] : convert_port[port]);
            return true;
        };
        return false;
    };
    auto check_input = [&](CellInfo *cell, IdString port, bool merged) {
        if (cell->getPort(port)) {
            NetInfo *net = cell->getPort(port);
            WireId src = ctx->getBelPinWire(cell->bel, port);
            // In current chip db real CPE input is max 4 pips away
            for (int i = 0; i < 4; i++) {
                if (net->wires.count(src)) {
                    auto &p = net->wires.at(src);
                    src = ctx->getPipSrcWire(p.pip);
                    const auto &extra_data = *pip_extra_data(p.pip);
                    if (extra_data.type == PipExtra::PIP_EXTRA_MUX) {
                        cfg.emplace(IdString(extra_data.name), extra_data.value);
                        if (add_input(port, ctx->getWireName(src)[1], merged))
                            break;
                    }
                }
            }
        }
    };
    auto swap_lut2_inputs = [&](int lut) -> int {
        // bit permutation: [3,1,2,0]
        return ((lut & 0b1000)) |      // b3 -> bit 3
               ((lut & 0b0010) << 1) | // b1 -> bit 2
               ((lut & 0b0100) >> 1) | // b2 -> bit 1
               ((lut & 0b0001));       // b0 -> bit 0
    };

    log_info("Update configuration based on routing..\n");
    for (auto &cell : ctx->cells) {
        if (cell.second->type.in(id_CPE_L2T4)) {
            cfg.clear();
            port_mapping.clear();

            int l00 = int_or_default(cell.second->params, id_INIT_L00, 0);
            int l01 = int_or_default(cell.second->params, id_INIT_L01, 0);
            int l10 = int_or_default(cell.second->params, id_INIT_L10, 0);

            check_input(cell.second.get(), id_D0_00, false);
            check_input(cell.second.get(), id_D1_00, false);
            check_input(cell.second.get(), id_D0_01, false);
            check_input(cell.second.get(), id_D1_01, false);
            check_input(cell.second.get(), id_D0_10, false);
            check_input(cell.second.get(), id_D1_10, false);
            if (cfg.count(id_LUT2_11) || cfg.count(id_LUT2_10)) {
                // A LUT2 alone in the half computes l10 from D0_10 (a) and D1_10 (b). Each input reaches the
                // half through a first-level LUT j (L00/L02: 0, L01/L03: 1) at its physical input p (IN1/3/5/7:
                // 0, TI2/4/6/8: 1); read both from the routing. Both through the same LUT: that LUT computes
                // and L10 passes it on. Otherwise each passes its input on and L10 computes.
                auto trace = [&](IdString port, int &j, int &p) {
                    j = p = -1;
                    NetInfo *net = cell.second->getPort(port);
                    if (!net)
                        return;
                    WireId w = ctx->getBelPinWire(cell.second->bel, port);
                    for (int step = 0; step < 2 && net->wires.count(w) && net->wires.at(w).pip != PipId(); step++) {
                        PipId pip = net->wires.at(w).pip;
                        std::string src = ctx->getPipName(pip)[2].str(ctx); // CPE.D0_01_int, CPE.TI4_int, ...
                        if (step == 0 && src.size() > 8 && src.compare(0, 5, "CPE.D") == 0)
                            j = (src[8] - '0') & 1;
                        if (step == 1)
                            p = src.compare(0, 6, "CPE.TI") == 0 ? 1 : 0;
                        w = ctx->getPipSrcWire(pip);
                    }
                };
                int ja, pa, jb, pb;
                trace(id_D0_10, ja, pa);
                trace(id_D1_10, jb, pb);
                auto pass = [](int p) { return p == 1 ? 0b1100 : 0b1010; }; // LUT_D1 : LUT_D0
                int *first[2] = {&l00, &l01};
                if (ja >= 0 && ja == jb) {
                    NPNR_ASSERT(pa != pb);
                    *first[ja] = pa == 0 ? l10 : swap_lut2_inputs(l10);
                    l10 = pass(ja);
                } else {
                    if (ja >= 0)
                        *first[ja] = pass(pa);
                    if (jb >= 0)
                        *first[jb] = pass(pb);
                    // L10's physical inputs are L00 then L01: swapped where a comes through L01 or b through L00
                    if (ja == 1 || jb == 0)
                        l10 = swap_lut2_inputs(l10);
                }
                // A first-level LUT computing on its own inputs (place-repair's fusing) where the other passes D0_10
                // or D1_10 on: its inputs swap like a tree's
                const IdString tree_ports[2][2] = {{id_D0_00, id_D1_00}, {id_D0_01, id_D1_01}};
                const IdString tree_mux[2] = {cfg.count(id_LUT2_11) ? id_LUT2_02 : id_LUT2_00,
                                              cfg.count(id_LUT2_11) ? id_LUT2_03 : id_LUT2_01};
                for (int t = 0; t < 2; t++) {
                    if (!cell.second->getPort(tree_ports[t][0]) && !cell.second->getPort(tree_ports[t][1]))
                        continue;
                    NPNR_ASSERT(ja != t && jb != t);
                    if (cfg.count(tree_mux[t]) && cfg.at(tree_mux[t]) == 1)
                        *first[t] = swap_lut2_inputs(*first[t]);
                    for (IdString port : tree_ports[t])
                        if (cell.second->getPort(port))
                            cell.second->renamePort(port, port_mapping[port]);
                }
                cell.second->params[id_INIT_L00] = Property(l00, 4);
                cell.second->params[id_INIT_L01] = Property(l01, 4);
                cell.second->params[id_INIT_L10] = Property(l10, 4);

                cell.second->renamePort(id_D0_10, port_mapping[id_D0_10]);
                cell.second->renamePort(id_D1_10, port_mapping[id_D1_10]);
            } else {
                if (cfg.count(id_LUT2_00) && cfg.at(id_LUT2_00) == 1)
                    l00 = swap_lut2_inputs(l00);
                if (cfg.count(id_LUT2_01) && cfg.at(id_LUT2_01) == 1)
                    l01 = swap_lut2_inputs(l01);
                if (cfg.count(id_LUT2_02) && cfg.at(id_LUT2_02) == 1)
                    l00 = swap_lut2_inputs(l00);
                if (cfg.count(id_LUT2_03) && cfg.at(id_LUT2_03) == 1)
                    l01 = swap_lut2_inputs(l01);

                cell.second->params[id_INIT_L00] = Property(l00, 4);
                cell.second->params[id_INIT_L01] = Property(l01, 4);
                cell.second->params[id_INIT_L10] = Property(l10, 4);

                cell.second->renamePort(id_D0_00, port_mapping[id_D0_00]);
                cell.second->renamePort(id_D1_00, port_mapping[id_D1_00]);
                cell.second->renamePort(id_D0_01, port_mapping[id_D0_01]);
                cell.second->renamePort(id_D1_01, port_mapping[id_D1_01]);
            }
            if (cfg.count(id_C_I1) && cfg.at(id_C_I1) == 1)
                cell.second->params[id_C_I1] = Property(1, 1);
            if (cfg.count(id_C_I2) && cfg.at(id_C_I2) == 1)
                cell.second->params[id_C_I2] = Property(1, 1);
            if (cfg.count(id_C_I3) && cfg.at(id_C_I3) == 1)
                cell.second->params[id_C_I3] = Property(1, 1);
            if (cfg.count(id_C_I4) && cfg.at(id_C_I4) == 1)
                cell.second->params[id_C_I4] = Property(1, 1);
        }
        if (cell.second->type.in(id_CPE_MX4, id_CPE_ADDF, id_CPE_ADDF2)) {
            cfg.clear();
            port_mapping.clear();

            int l00 = int_or_default(cell.second->params, id_INIT_L00, 0);
            int l01 = int_or_default(cell.second->params, id_INIT_L01, 0);
            int l02 = int_or_default(cell.second->params, id_INIT_L02, 0);
            int l03 = int_or_default(cell.second->params, id_INIT_L03, 0);

            check_input(cell.second.get(), id_D0_00, true);
            check_input(cell.second.get(), id_D1_00, true);
            check_input(cell.second.get(), id_D0_01, true);
            check_input(cell.second.get(), id_D1_01, true);
            check_input(cell.second.get(), id_D0_02, true);
            check_input(cell.second.get(), id_D1_02, true);
            check_input(cell.second.get(), id_D0_03, true);
            check_input(cell.second.get(), id_D1_03, true);

            if (cfg.count(id_LUT2_00) && cfg.at(id_LUT2_00) == 1)
                l00 = swap_lut2_inputs(l00);
            if (cfg.count(id_LUT2_01) && cfg.at(id_LUT2_01) == 1)
                l01 = swap_lut2_inputs(l01);
            if (cfg.count(id_LUT2_02) && cfg.at(id_LUT2_02) == 1)
                l02 = swap_lut2_inputs(l02);
            if (cfg.count(id_LUT2_03) && cfg.at(id_LUT2_03) == 1)
                l03 = swap_lut2_inputs(l03);

            cell.second->params[id_INIT_L00] = Property(l00, 4);
            cell.second->params[id_INIT_L01] = Property(l01, 4);
            cell.second->params[id_INIT_L02] = Property(l02, 4);
            cell.second->params[id_INIT_L03] = Property(l03, 4);

            cell.second->renamePort(id_D0_00, port_mapping[id_D0_00]);
            cell.second->renamePort(id_D1_00, port_mapping[id_D1_00]);
            cell.second->renamePort(id_D0_01, port_mapping[id_D0_01]);
            cell.second->renamePort(id_D1_01, port_mapping[id_D1_01]);
            cell.second->renamePort(id_D0_02, port_mapping[id_D0_02]);
            cell.second->renamePort(id_D1_02, port_mapping[id_D1_02]);
            cell.second->renamePort(id_D0_03, port_mapping[id_D0_03]);
            cell.second->renamePort(id_D1_03, port_mapping[id_D1_03]);

            if (cfg.count(id_C_I1) && cfg.at(id_C_I1) == 1)
                cell.second->params[id_C_I1] = Property(1, 1);
            if (cfg.count(id_C_I2) && cfg.at(id_C_I2) == 1)
                cell.second->params[id_C_I2] = Property(1, 1);
            if (cfg.count(id_C_I3) && cfg.at(id_C_I3) == 1)
                cell.second->params[id_C_I3] = Property(1, 1);
            if (cfg.count(id_C_I4) && cfg.at(id_C_I4) == 1)
                cell.second->params[id_C_I4] = Property(1, 1);
        }
        if (cell.second->type.in(id_CPE_FF, id_CPE_FF_L, id_CPE_FF_U, id_CPE_LATCH)) {
            cfg.clear();
            port_mapping.clear();
            check_input(cell.second.get(), id_CLK_INT, false);
            check_input(cell.second.get(), id_EN_INT, false);
            if (cfg.count(id_C_CLKSEL) && cfg.at(id_C_CLKSEL) == 1) {
                uint8_t val = int_or_default(cell.second->params, id_C_CPE_CLK, 0) & 1;
                cell.second->params[id_C_CPE_CLK] = Property(val ? 3 : 0, 2);
                cell.second->params[id_C_CLKSEL] = Property(1, 1);
            }
            if (cfg.count(id_C_ENSEL) && cfg.at(id_C_ENSEL) == 1) {
                uint8_t val = int_or_default(cell.second->params, id_C_CPE_EN, 0) & 1;
                cell.second->params[id_C_CPE_EN] = Property(val ? 3 : 0, 2);
                cell.second->params[id_C_ENSEL] = Property(1, 1);
            }
            ff_routed_ports[cell.first] = {port_mapping[id_CLK_INT], port_mapping[id_EN_INT]};
            cell.second->renamePort(id_CLK_INT, port_mapping[id_CLK_INT]);
            cell.second->renamePort(id_EN_INT, port_mapping[id_EN_INT]);
        }
    }
    ctx->assignArchInfo();

    const ArchArgs &args = ctx->args;
    if (args.options.count("out")) {
        write_bitstream(args.device, args.options["out"].as<std::string>());
    }
}

BoundingBox GateMateImpl::getRouteBoundingBox(WireId src, WireId dst) const
{
    int x0, y0, x1, y1;
    auto expand = [&](int x, int y) {
        x0 = std::min(x0, x);
        x1 = std::max(x1, x);
        y0 = std::min(y0, y);
        y1 = std::max(y1, y);
    };
    tile_xy(ctx->chip_info, src.tile, x0, y0);
    x1 = x0;
    y1 = y0;
    int dx, dy;
    tile_xy(ctx->chip_info, dst.tile, dx, dy);
    expand(dx, dy);

    return {(x0 & 0xfffe), (y0 & 0xfffe), (x1 & 0xfffe) + 1, (y1 & 0xfffe) + 1};
}

void GateMateImpl::expandBoundingBox(BoundingBox &bb) const
{
    bb.x0 = std::max((bb.x0 & 0xfffe) - 4, 0);
    bb.y0 = std::max((bb.y0 & 0xfffe) - 4, 0);
    bb.x1 = std::min((bb.x1 & 0xfffe) + 5, ctx->getGridDimX());
    bb.y1 = std::min((bb.y1 & 0xfffe) + 5, ctx->getGridDimY());
}

GroupId GateMateImpl::getResourceKeyForPip(PipId pip) const
{
    const auto &extra_data = *pip_extra_data(pip);
    if (extra_data.type != PipExtra::PIP_EXTRA_MUX || extra_data.group_index == 0)
        return GroupId();

    return GroupId(pip.tile, extra_data.group_index);
}

int GateMateImpl::getResourceValueForPip(PipId pip) const
{
    const auto &extra_data = *pip_extra_data(pip);
    if (extra_data.type != PipExtra::PIP_EXTRA_MUX || extra_data.resource == 0)
        return 0;
    return extra_data.value;
}

bool GateMateImpl::isGroupResource(GroupId group) const { return ctx->getGroupType(group) == id_RESOURCE; }

void GateMateImpl::configurePlacerHeap(PlacerHeapCfg &cfg)
{
    cfg.chainRipup = true;
    cfg.placeAllAtOnce = true;
    if (!learnt_net_crit.empty())
        // What earlier runs learnt (--vopt place-crit-in): at least the criticality the net had after
        // routing, or (place-crit-mode=scale) the criticality times the net's learnt importance
        cfg.adjust_crit = [this](const PortRef &user, float crit) {
            NetInfo *net = user.cell ? user.cell->getPort(user.port) : nullptr;
            auto found = net ? learnt_net_crit.find(net->name) : learnt_net_crit.end();
            if (found == learnt_net_crit.end())
                return crit;
            return learnt_scale ? std::min(1.0f, crit * found->second) : std::max(crit, found->second);
        };
}

void GateMateImpl::configurePlacerStatic(PlacerStaticCfg &cfg)
{
    cfg.glbBufTypes.insert(id_CLKIN);
    cfg.glbBufTypes.insert(id_GLBOUT);
    cfg.glbBufTypes.insert(id_PLL);
    cfg.glbBufTypes.insert(id_USR_RSTN);
    cfg.glbBufTypes.insert(id_CFG_CTRL);
    cfg.glbBufTypes.insert(id_SERDES);

    {
        cfg.cell_groups.emplace_back();
        auto &comb = cfg.cell_groups.back();
        comb.name = ctx->id("COMB");
        comb.bel_area[id_CPE_LT_U] = StaticRect(1.0f, 0.5f);
        comb.bel_area[id_CPE_LT_L] = StaticRect(1.0f, 0.5f);

        comb.bel_area[id_CPE_CPLINES] = StaticRect(0.0f, 0.0f);
        comb.bel_area[id_CPE_COMP] = StaticRect(0.0f, 0.0f);
        comb.bel_area[id_CPE_RAMIO_U] = StaticRect(0.0f, 0.0f);
        comb.bel_area[id_CPE_RAMIO_L] = StaticRect(0.0f, 0.0f);

        comb.cell_area[id_CPE_LT_U] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_LT_L] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_LT] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_L2T4] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_DUMMY] = StaticRect(1.0f, 0.5f);

        comb.cell_area[id_CPE_CPLINES] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_COMP] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_RAMIO] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_RAMI] = StaticRect(1.0f, 0.5f);
        comb.cell_area[id_CPE_RAMO] = StaticRect(1.0f, 0.5f);

        comb.zero_area_cells.insert(id_CPE_CPLINES);
        comb.zero_area_cells.insert(id_CPE_COMP);
        comb.zero_area_cells.insert(id_CPE_RAMIO);
        comb.zero_area_cells.insert(id_CPE_RAMI);
        comb.zero_area_cells.insert(id_CPE_RAMO);

        comb.spacer_rect = StaticRect(1.0f, 0.5f);
    }

    {
        cfg.cell_groups.emplace_back();
        auto &ff = cfg.cell_groups.back();
        ff.name = ctx->id("FF");

        ff.bel_area[id_CPE_FF_U] = StaticRect(1.0f, 0.5f);
        ff.bel_area[id_CPE_FF_L] = StaticRect(1.0f, 0.5f);

        ff.cell_area[id_CPE_FF_U] = StaticRect(1.0f, 0.5f);
        ff.cell_area[id_CPE_FF_L] = StaticRect(1.0f, 0.5f);
        ff.cell_area[id_CPE_FF] = StaticRect(1.0f, 0.5f);
        ff.cell_area[id_CPE_LATCH] = StaticRect(1.0f, 0.5f);

        ff.spacer_rect = StaticRect(1.0f, 0.5f);
    }

    {
        cfg.cell_groups.emplace_back();
        auto &ram = cfg.cell_groups.back();
        ram.name = ctx->id("RAM");

        ram.bel_area[id_RAM] = StaticRect(1.0f, 2.0f);
        ram.bel_area[id_RAM_HALF_L] = StaticRect(1.0f, 2.0f);

        ram.cell_area[id_RAM] = StaticRect(1.0f, 2.0f);
        ram.cell_area[id_RAM_HALF] = StaticRect(1.0f, 2.0f);
        ram.cell_area[id_RAM_HALF_DUMMY] = StaticRect(1.0f, 2.0f);

        ram.spacer_rect = StaticRect(1.0f, 2.0f);
    }
}

int GateMateImpl::get_dff_config(CellInfo *dff) const
{
    int val = 0;
    val |= int_or_default(dff->params, id_C_CPE_EN, 0);
    val <<= 2;
    val |= int_or_default(dff->params, id_C_CPE_CLK, 0);
    val <<= 2;
    val |= int_or_default(dff->params, id_C_CPE_RES, 0);
    val <<= 2;
    val |= int_or_default(dff->params, id_C_CPE_SET, 0);
    val <<= 2;
    val |= int_or_default(dff->params, id_C_EN_SR, 0);
    val <<= 1;
    val |= int_or_default(dff->params, id_C_L_D, 0);
    val <<= 1;
    val |= int_or_default(dff->params, id_FF_INIT, 0);
    return val;
}

int GateMateImpl::get_ram_config(CellInfo *ram) const
{
    int val = 0;
    val |= int_or_default(ram->params, id_RAM_cfg_ecc_enable, 0);
    val <<= 2;
    val |= int_or_default(ram->params, id_RAM_cfg_sram_mode, 0);
    return val;
}

void GateMateImpl::assign_cell_info()
{
    fast_cell_info.resize(ctx->cells.size());
    for (auto &cell : ctx->cells) {
        CellInfo *ci = cell.second.get();
        auto &fc = fast_cell_info.at(ci->flat_index);
        fc = GateMateCellInfo();
        if (getBelBucketForCellType(ci->type) == id_CPE_FF) {
            fc.ff_en = ci->getPort(id_EN_INT);
            fc.ff_clk = ci->getPort(id_CLK_INT);
            fc.ff_sr = ci->getPort(id_SR);
            fc.config = get_dff_config(ci);
            fc.used = true;
        }
        if (getBelBucketForCellType(ci->type) == id_RAM_HALF) {
            fc.config = get_ram_config(ci);
            fc.used = true;
        }
    }
}

// Bel bucket functions
IdString GateMateImpl::getBelBucketForCellType(IdString cell_type) const
{
    if (cell_type.in(id_CPE_IBUF, id_CPE_OBUF, id_CPE_TOBUF, id_CPE_IOBUF, id_CPE_LVDS_IBUF, id_CPE_LVDS_TOBUF,
                     id_CPE_LVDS_OBUF, id_CPE_LVDS_IOBUF))
        return id_GPIO;
    else if (cell_type.in(id_CPE_LT_U, id_CPE_LT_L, id_CPE_LT, id_CPE_L2T4))
        return id_CPE_LT;
    else if (cell_type.in(id_CPE_FF_U, id_CPE_FF_L, id_CPE_FF, id_CPE_LATCH))
        return id_CPE_FF;
    else if (cell_type.in(id_CPE_RAMIO, id_CPE_RAMI, id_CPE_RAMO))
        return id_CPE_RAMIO;
    else if (cell_type.in(id_RAM, id_RAM_HALF, id_RAM_HALF_DUMMY))
        return id_RAM_HALF;
    else
        return cell_type;
}

BelBucketId GateMateImpl::getBelBucketForBel(BelId bel) const
{
    IdString bel_type = ctx->getBelType(bel);
    if (bel_type.in(id_CPE_LT_U, id_CPE_LT_L))
        return id_CPE_LT;
    else if (bel_type.in(id_CPE_FF_U, id_CPE_FF_L))
        return id_CPE_FF;
    else if (bel_type.in(id_CPE_RAMIO_U, id_CPE_RAMIO_L))
        return id_CPE_RAMIO;
    else if (bel_type.in(id_RAM, id_RAM_HALF_L))
        return id_RAM_HALF;
    return bel_type;
}

bool GateMateImpl::isValidBelForCellType(IdString cell_type, BelId bel) const
{
    IdString bel_type = ctx->getBelType(bel);
    if (bel_type == id_GPIO)
        return cell_type.in(id_CPE_IBUF, id_CPE_OBUF, id_CPE_TOBUF, id_CPE_IOBUF, id_CPE_LVDS_IBUF, id_CPE_LVDS_TOBUF,
                            id_CPE_LVDS_OBUF, id_CPE_LVDS_IOBUF);
    else if (bel_type == id_CPE_LT_U)
        return cell_type.in(id_CPE_LT_U, id_CPE_LT, id_CPE_L2T4, id_CPE_DUMMY);
    else if (bel_type == id_CPE_LT_L)
        return cell_type.in(id_CPE_LT_L, id_CPE_LT, id_CPE_L2T4, id_CPE_DUMMY);
    else if (bel_type == id_CPE_FF_U)
        return cell_type.in(id_CPE_FF_U, id_CPE_FF, id_CPE_LATCH);
    else if (bel_type == id_CPE_FF_L)
        return cell_type.in(id_CPE_FF_L, id_CPE_FF, id_CPE_LATCH);
    else if (bel_type.in(id_CPE_RAMIO_U, id_CPE_RAMIO_L))
        return cell_type.in(id_CPE_RAMIO, id_CPE_RAMI, id_CPE_RAMO);
    else if (bel_type == id_RAM)
        return cell_type.in(id_RAM_HALF, id_RAM);
    else if (bel_type == id_RAM_HALF_L)
        return cell_type.in(id_RAM_HALF, id_RAM_HALF_DUMMY);
    else
        return (bel_type == cell_type);
}

bool GateMateImpl::isPipInverting(PipId pip) const
{
    const auto &extra_data = *pip_extra_data(pip);
    return extra_data.type == PipExtra::PIP_EXTRA_MUX && (extra_data.flags & MUX_INVERT);
}

const GateMateTileExtraDataPOD *GateMateImpl::tile_extra_data(int tile) const
{
    return reinterpret_cast<const GateMateTileExtraDataPOD *>(ctx->chip_info->tile_insts[tile].extra_data.get());
}

const GateMateBelExtraDataPOD *GateMateImpl::bel_extra_data(BelId bel) const
{
    return reinterpret_cast<const GateMateBelExtraDataPOD *>(chip_bel_info(ctx->chip_info, bel).extra_data.get());
}

const GateMatePipExtraDataPOD *GateMateImpl::pip_extra_data(PipId pip) const
{
    return reinterpret_cast<const GateMatePipExtraDataPOD *>(chip_pip_info(ctx->chip_info, pip).extra_data.get());
}

struct GateMateArch : HimbaechelArch
{
    GateMateArch() : HimbaechelArch("gatemate") {};
    bool match_device(const std::string &device) override
    {
        return device.size() > 6 && device.substr(0, 6) == "CCGM1A";
    }
    std::unique_ptr<HimbaechelAPI> create(const std::string &device) override
    {
        return std::make_unique<GateMateImpl>();
    }
} gateMateArch;

NEXTPNR_NAMESPACE_END
