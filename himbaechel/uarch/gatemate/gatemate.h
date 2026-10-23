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

#include <array>
#include <mutex>

#include "extra_data.h"
#include "himbaechel_api.h"
#include "log.h"
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

struct PlacedAnchor; // gfx.cc

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
    bool checkPipAvailForNet(PipId pip, const NetInfo *net) const override { return checkPipAvail(pip); };

    bool isBelLocationValid(BelId bel, bool explain_invalid = false) const override;
    delay_t estimateDelay(WireId src, WireId dst) const override;
    delay_t predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const override;
    bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const override;
    TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const override;
    TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const override;

    void drawBel(std::vector<GraphicElement> &g, GraphicElement::style_t style, IdString bel_type, Loc loc) override;
    using HimbaechelAPI::drawPip;
    void drawTileWire(std::vector<GraphicElement> &g, GraphicElement::style_t style, WireId tile_wire) override;
    void drawPip(std::vector<GraphicElement> &g, GraphicElement::style_t style, PipId pip) override;
    bool guiDrawsUnusedRouting() const override;
    std::vector<std::pair<DecalXY, BoundingBox>> getDetailDecals() const override;
    void drawDetail(std::vector<GraphicElement> &g, DecalId decal) override;
    std::vector<PipId> getDetailPips(DecalId decal, float x, float y) const override;
    float getGuiColumnX(int x) const override;
    std::vector<std::pair<IdString, std::string>> getBelAttrs(BelId bel) const override;
    std::vector<std::pair<IdString, std::string>> getWireAttrs(WireId wire) const override;
    std::vector<std::pair<IdString, std::string>> getPipAttrs(PipId pip) const override;
    // Labels of the configuration of the used bels for the GUI (gui_attrs.cc). Built after placement
    // and after routing, while the context is locked; read while drawing, which may happen in
    // another thread.
    void update_gui_labels();
    std::vector<std::string> gui_labels(BelId bel) const;
    // The pad of an IO tile, or nullptr
    const PadInfoPOD *tile_pad(Loc loc) const;
    // Lanes and channel tracks for the used routing in the GUI (gfx.cc), assigned after routing so
    // that no two signals share one; with the labels
    void update_gui_routing();
    float gui_lane(int tile, int index, float lane) const;
    float gui_track(WireId node, int channel, float track) const;
    // Whether a pip from this wire arriving from another tile is used
    bool gui_arrival_used(int tile, int index) const;
    bool fixed_pip(PipId pip) const;
    // Where the GUI draws a tile wire, and where the drawing of a pip starts (at its source, or for
    // a source arriving from another tile, at its driver) and ends; for checking the drawings
    std::pair<float, float> gui_position(int tile, int index) const;
    std::array<float, 4> gui_pip_ends(PipId pip) const;
    // gfx.cc helpers: a tile wire's anchor with its assigned lane; a pip's or a wire's drawing, with
    // the assigned lanes and tracks or the default ones
    PlacedAnchor placed_routed(int tile, int index) const;
    void draw_pip(std::vector<GraphicElement> &g, GraphicElement::style_t style, PipId pip, bool assigned) const;
    void draw_route_from_driver(std::vector<GraphicElement> &g, GraphicElement::style_t style, WireId tile_wire,
                                const PlacedAnchor &to, bool assigned) const;

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
    pool<IdString> scopenames_used;

  private:
    mutable std::mutex gui_labels_mutex_;
    dict<BelId, std::vector<std::string>> gui_labels_;
    dict<std::pair<int, int>, float> gui_lanes_;
    dict<std::pair<WireId, int>, float> gui_tracks_;
    pool<std::pair<int, int>> gui_arrivals_;

    bool getChildPlacement(const BaseClusterInfo *cluster, Loc root_loc,
                           std::vector<std::pair<CellInfo *, BelId>> &placement) const;

    void write_bitstream(const std::string &device, const std::string &filename);

    void parse_ccf(const std::string &filename);

    void assign_cell_info();
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
    std::map<IdString, const GateMateTimingExtraDataPOD *> timing;

    enum class RamPinKind
    {
        DATA_IN,
        DATA_OUT,
        BITMASK,
        ADDR,
        CTRL,
        ECC_STATUS,
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
