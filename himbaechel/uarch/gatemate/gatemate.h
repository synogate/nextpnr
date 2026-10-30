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

#ifndef HIMBAECHEL_GATEMATE_H
#define HIMBAECHEL_GATEMATE_H

#include "extra_data.h"
#include "himbaechel_api.h"
#include "log.h"
#include "lookahead.h"
#include "nextpnr.h"
#include "util.h"

#include "himbaechel_helpers.h"

NEXTPNR_NAMESPACE_BEGIN

enum MultiDieStrategy
{
    CLOCK_MIRROR,
    REUSE_CLK1,
    FULL_USE,
};

struct TimingAnalyser;

struct GateMateImpl : HimbaechelAPI
{
    ~GateMateImpl();
    po::options_description getUArchOptions() override;
    void init_database(Arch *arch) override;

    void init(Context *ctx) override;

    void pack() override;

    void prePlace() override;
    void postPlace() override;
    void preRoute() override;
    void postRoute() override;

    virtual std::string getDefaultRouter() const override { return "router2"; };

    BoundingBox getRouteBoundingBox(WireId src, WireId dst) const override;
    void expandBoundingBox(BoundingBox &bb) const override;
    bool checkPipAvail(PipId pip) const override;
    bool checkPipAvailForNet(PipId pip, const NetInfo *net) const override;
    // Whether a pip is usable at all with the chosen options, regardless of placement
    bool pip_allowed_by_options(PipId pip) const;

    bool isBelLocationValid(BelId bel, bool explain_invalid = false) const override;
    delay_t estimateDelay(WireId src, WireId dst) const override;
    delay_t predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const override;
    void configureRouter2(Router2Cfg &cfg) override;
    // Swap the input pairs of the LUT trees where the delay tables estimate it faster, or of all of
    // them; returns how many were swapped (lut_swap.cc)
    int swap_lut_inputs(bool all = false);
    bool lut_swap = false;
    bool timing_opt_after_place = false;
    // After routing, up to `rounds` rounds of swapping the input pairs of LUT trees on critical paths,
    // each kept only if it improves Fmax (`test_undo`: try any trees, and undo every round); returns
    // how many trees it tried
    int improve_lut_inputs(int rounds, bool test_undo = false);
    int lut_swap_rounds = 0;
    // After routing, route `runs` more times from scratch, with criticality learnt from the runs
    // before, and keep the best (reroute.cc)
    void route_learning(int runs);
    int route_runs = 0;
    void report_critical_routing();
    // The fastest path for a connection ignoring other nets' routing (reroute.cc)
    std::vector<PipId> ideal_path(NetInfo *net, WireId src, WireId sink, delay_t &delay, bool free_only = false,
                                  int margin = -1) const;
    // Whether a bound pip sets the resource of pip (a CPE mux select) to another value (reroute.cc)
    bool resource_taken(PipId pip) const;
    // Route what a repair ripped up, all other routing staying (reroute.cc)
    bool route_ripped(std::vector<std::pair<NetInfo *, WireId>> laid);
    // Bind a connection along its ideal path with STRENGTH_PLACER, displacing other nets (reroute.cc)
    void lay_ideal_path(NetInfo *net, const PortRef &user, std::vector<std::pair<NetInfo *, WireId>> &laid);
    // Whether the routing of net from its source to wire passes an odd number of inverting muxes (reroute.cc)
    bool routed_inverted(const NetInfo *net, WireId wire) const;
    // The fastest path to sink branching off the routing of net, arriving inverted or not as `inverted`
    // says, through free wires or with `displace` also those of other nets that can be ripped up (reroute.cc)
    std::vector<PipId> parity_path(NetInfo *net, WireId sink, bool inverted, bool displace) const;
    // After routing, invert nets and route the data inputs of the 8:1 multiplexers that still arrive inverted
    // again, as they have no way to undo it (reroute.cc)
    void fix_mx8_inputs();
    void find_mx8_ways();
    // Before routing them again, which nets put out the inverse of their signal, so that as many data inputs of
    // 8:1 multiplexers as possible arrive as they need (inverted_nets; reroute.cc)
    void choose_net_polarities();
    // Whether a driver can put out the inverse of its signal, and whether a sink undoes an inversion on the
    // way, as the bitstream does for routing (reroute.cc)
    bool can_invert_driver(const PortRef &driver) const;
    bool undoes_inversion(const PortRef &user) const;
    // The configuration of a register storing the inverse (false where its set and reset cannot swap)
    bool invert_register(dict<IdString, Property> &params) const;
    // Whether the routing to the data input `port` of the 8:1 multiplexer `mux` must invert: when either its net
    // or the multiplexer's output is inverted (with all its inputs)
    bool mx8_input_inverted(const CellInfo *mux, IdString port) const;
    // The nets whose driver puts out the inverse of their signal (all their sinks undo it, or are data inputs of
    // 8:1 multiplexers that need it), set by fix_mx8_inputs()
    pool<IdString> inverted_nets;
    // The wires that are the only way to a data input of an 8:1 multiplexer, with whether the signal on them
    // must be inverted for it to arrive as the multiplexer needs (set by fix_mx8_inputs(), kept to by
    // parity_path())
    dict<WireId, bool> mx8_input_ways;
    // And those to its selects, with their nets
    dict<WireId, const NetInfo *> held_ways;
    // After routing, up to `rounds` repairs of connections on the critical paths, with rounds < 0 until
    // timing is met (reroute.cc)
    void repair_critical_routing(int rounds);
    int route_repair = -1;
    // The simple timing model fixes (--vopt timing-fixes; delay.cc). TEMPORARY until fixed upstream
    // After the routing repair, up to `rounds` moves of LUT trees on the critical paths, with rounds < 0
    // until timing is met (reroute.cc)
    void repair_critical_placement(int rounds);
    // place-repair's two halves; the number of changes kept
    int move_critical_cells(int rounds);
    int fuse_critical_luts(int rounds);
    int place_repair = -1;
    // Percent above each clock's constraint that "until timing is met" means (--vopt repair-margin)
    float repair_margin = 0;
    // After routing, up to `rounds` repairs of whole nets with several sinks on the critical paths
    // (reroute.cc)
    void repair_critical_nets(int rounds);
    int net_repair = 0;
    // LUT trees that can move after placement, the free positions they can move to, and the move;
    // the same for registers with the LUT tree feeding them (reroute.cc)
    bool relocatable_lut_tree(const CellInfo *ci) const;
    bool free_lut_tree_bel(const CellInfo *ci, BelId bel, const CellInfo *leaving = nullptr) const;
    void relocate_lut_tree(CellInfo *ci, BelId bel);
    CellInfo *relocatable_register(const CellInfo *ff) const;
    bool free_register_half(const CellInfo *lut, const CellInfo *ff, BelId lt) const;
    void relocate_register(CellInfo *lut, CellInfo *ff, BelId lt);
    void update_used_cpe(int tile);
    bool reroute_clock_sink(NetInfo *clk, const PortRef &user, WireId old_sink);
    // Before routing, copy the LUT trees driving nets of at least min_fanout sinks spread far apart
    // next to groups of their sinks (with `all`, every such driver, two groups); returns the copies
    // made (replicate.cc)
    int replicate_drivers(int min_fanout, bool all = false);
    int replicate_fanout = 0;
    // Criticality per net learnt from earlier runs (--vopt place-crit-in)
    dict<IdString, float> learnt_net_crit;
    bool learnt_scale = false;
    bool report_crit_routing = false;
    std::string route_learn_mode = "max";
    float route_learn_step = 0.25;
    // Nets routed before router2 (clocks, multipliers), which routing again leaves alone
    pool<IdString> prerouted_nets;
    bool swap_pairs = true, swap_halves = true;
    // Criticality-weighted delay from a LUT tree's inputs to its output, as it is and with the pairs
    // swapped; with `routed`, as it is uses the real routing, otherwise both use the delay tables
    std::pair<float, float> lut_tree_costs(CellInfo *ci, const TimingAnalyser &tmg, bool routed) const;
    // Extra router cost (ns) of entering a scarce switch box mux, per tile type and pip; the router
    // scales it down for critical connections. Empty unless --vopt sb-y-cost or sb-ydiag-cost.
    std::vector<std::vector<float>> scarce_pip_cost;
    float sb_y_cost = 0, sb_ydiag_cost = 0;
    // Delay tables for the router (estimateDelay) and the placer (predictDelay), see lookahead.h
    GateMateLookahead lookahead;
    bool table_route = false, table_place = false;
    bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const override;
    TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const override;
    TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const override;

    void drawBel(std::vector<GraphicElement> &g, GraphicElement::style_t style, IdString bel_type, Loc loc) override;

    bool getClusterPlacement(ClusterId cluster, BelId root_bel,
                             std::vector<std::pair<CellInfo *, BelId>> &placement) const override;

    IdString getBelBucketForCellType(IdString cell_type) const override;
    bool isValidBelForCellType(IdString cell_type, BelId bel) const override;
    BelBucketId getBelBucketForBel(BelId bel) const override;

    Loc getRelativeConstraint(Loc &root_loc, IdString id) const;

    void configurePlacerHeap(PlacerHeapCfg &cfg) override;
    void configurePlacerStatic(PlacerStaticCfg &cfg) override;

    bool isPipInverting(PipId pip) const override;

    GroupId getResourceKeyForPip(PipId pip) const override;
    int getResourceValueForPip(PipId pip) const override;
    bool isGroupResource(GroupId group) const override;

    const GateMateTileExtraDataPOD *tile_extra_data(int tile) const;
    const GateMateBelExtraDataPOD *bel_extra_data(BelId bel) const;
    const GateMatePipExtraDataPOD *pip_extra_data(PipId pip) const;

    int get_dff_config(CellInfo *dff) const;
    int get_ram_config(CellInfo *ram) const;

    std::set<IdString> available_pads;
    std::map<BelId, const PadInfoPOD *> bel_to_pad;
    pool<IdString> ddr_nets;
    dict<std::pair<IdString, int>, Loc> locations;
    int dies;
    int preferred_die;
    pool<IdString> multiplier_a_passthru_lowers;
    pool<IdString> multiplier_a_passthru_uppers;
    pool<IdString> multiplier_zero_drivers;
    pool<ClusterId> multiplier_clusters;
    std::vector<bool> used_cpes;
    std::vector<uint32_t> pip_data;
    std::vector<uint32_t> pip_mask;
    int fpga_mode;
    int timing_mode;
    std::map<const NetInfo *, int> global_signals;
    dict<std::pair<IdString, int>, NetInfo *> global_mapping;
    dict<std::pair<IdString, int>, IdString> global_clk_mapping;
    std::vector<CellInfo *> clkin;
    std::vector<CellInfo *> glbout;
    std::vector<CellInfo *> pll;
    pool<IdString> ignore;
    MultiDieStrategy strategy;
    dict<int, IdString> index_to_die;
    dict<IdString, int> die_to_index;
    dict<IdString, dict<IdString, IdString>> pass_backtrace;
    pool<IdString> scopenames;
    dict<IdString, IdString> scope_regions; // scope name -> floorplanning region

    // Partitions (partition.cc). With as_partition the top level ports stay as unrouted nets instead of
    // IO pads, so the result can be imported into another design with --vopt partition.
    bool as_partition = false;
    // With partition_unrouted the partition is written placed only, after placing; importing it routes its nets
    bool partition_unrouted = false;
    // Replace the blackbox instances named by --vopt partition=<inst>:<file>[@dx,dy] with the placed and
    // routed partitions from those files, moved by (dx, dy) tiles and locked
    void import_partitions();
    // Store what the bitstream needs beyond the netlist and routing as cell attributes, for a later import
    void save_partition_attrs();
    // A partition's global clock nets (by name) and the port nets clocking them, and taking the global buffers
    // out again after routing, leaving the clock sinks on the port nets
    dict<IdString, IdString> partition_clocks;
    // Registers standing in for the design outside a partition on its ports while it is routed
    pool<IdString> partition_port_cells;
    void detach_partition_ports();
    // The cells detach_partition_ports takes out: stand-ins, clocking and constant drivers
    pool<IdString> partition_detached_cells() const;
    // The area of a partition (its floorplan regions), outside which the routing it keeps must not go, and the
    // nets that may, as they are taken out after routing
    BoundingBox partition_area{-1, -1, -1, -1};
    pool<IdString> partition_free_nets;
    // The tiles of the CPEs fixed next to IOs and clocking, which a partition keeps free
    pool<int> partition_io_tiles;
    // The IOs a partition's CCF (that of the design importing it) names; only their CPEs are kept free then
    pool<int> partition_ccf_tiles;
    void find_partition_area();
    void find_partition_free_nets();
    // With partition_open the design may place cells in the free tiles of the partitions' areas too
    bool partition_open = false;
    // Cells imported from partitions, and the tiles of the rectangles they cover, where no other cell may go
    // while placing
    pool<IdString> partition_cells;
    pool<int> partition_tiles;
    // The placement strength the partition's cells get after placing (place=repair: the one they had when it was
    // implemented, so the passes after placement may change them; place=locked: locked); locked while placing
    dict<IdString, PlaceStrength> partition_strength;
    void release_partitions();
    // Nets of partitions whose routing was imported: route=open, weakly bound, open to the router and the
    // repairs; route=repair, locked for the router and released to the repairs after it (release_partition_routing);
    // route=locked, locked throughout. The passes before routing leave the locked ones as they are.
    pool<IdString> partition_nets;
    pool<IdString> partition_repair_nets;
    pool<IdString> partition_locked_nets;
    void release_partition_routing();
    // Tiles the locked routing of partitions passes through, where the fixed multiplier paths may be taken
    pool<int> partition_routed_tiles;

  private:
    bool getChildPlacement(const BaseClusterInfo *cluster, Loc root_loc,
                           std::vector<std::pair<CellInfo *, BelId>> &placement) const;

    void write_bitstream(const std::string &device, const std::string &filename);

    void parse_ccf(const std::string &filename);
    void import_partition(const std::string &spec);
    void restore_partition_attrs(CellInfo *ci);
    void open_partition_area(const BoundingBox &area);

    void assign_cell_info();
    void check_regions();
    void route_clock();
    void route_mult();
    void reassign_bridges(NetInfo *start_net, const dict<WireId, PipMap> &net_wires, WireId start_wire,
                          dict<WireId, IdString> &wire_to_net, int &num);
    void reassign_cplines(NetInfo *net, const dict<WireId, PipMap> &net_wires, WireId wire,
                          dict<WireId, IdString> &wire_to_net, int &num, IdString in_port);
    void repack();

    bool get_delay_from_tmg_db(IdString id, DelayQuad &delay) const;
    void get_setuphold_from_tmg_db(IdString id_setup, IdString id_hold, DelayPair &setup, DelayPair &hold) const;
    void get_setuphold_from_tmg_db(IdString id_setuphold, DelayPair &setup, DelayPair &hold) const;

    struct GateMateCellInfo
    {
        // slice info
        const NetInfo *ff_en = nullptr, *ff_clk = nullptr, *ff_sr = nullptr;
        int config = 0;
        int signal_used = -1;
        bool used = false;
    };
    std::vector<GateMateCellInfo> fast_cell_info;
    std::map<BelId, std::map<IdString, const GateMateBelPinConstraintPOD *>> pin_to_constr;
    // The pins the clock and enable inputs (CLK_INT, EN_INT) of the registers are renamed to after
    // routing, for the timing model
    dict<IdString, std::pair<IdString, IdString>> ff_routed_ports;
    IdString ff_clock_port(const CellInfo *cell) const;
    IdString ff_enable_port(const CellInfo *cell) const;
    // The delays of the global clock network from a global clock driver's wire to the clock inputs it
    // reaches, the way route_clock() routes them (computed when first needed, for all contexts with the
    // same device and settings)
    const dict<WireId, delay_t> &clock_tree_delays(WireId src) const;
    std::map<IdString, const GateMateTimingExtraDataPOD *> timing;

    enum class RamPinKind
    {
        DATA_IN,
        DATA_OUT,
        BITMASK,
        ADDR,
        CTRL,
        ECC_STATUS,
        FIFO_STATUS, // a FIFO's flags and pointers: the read side on port A, the write side on B
    };
    struct RamPinInfo
    {
        RamPinKind kind;
        bool port_b; // physical A/B pin side
        int half;    // 0 or 1 (bit index / 20, resp. control set 0/1)
    };
    // Resolve the clock domain (0=a0, 1=a1, 2=b0, 3=b1) a RAM pin is timed
    // against, depending on the cell mode.
    int ram_clock_index(const CellInfo *cell, const RamPinInfo &pin) const;
    dict<IdString, RamPinInfo> ram_signal_clk;
    IdString forced_die;
    bool use_cp_for_clk;
    bool use_cp_for_cpe;
    bool use_bridges;
};

NEXTPNR_NAMESPACE_END
#endif
