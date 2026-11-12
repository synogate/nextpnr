/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Claire Xenia Wolf <claire@yosyshq.com>
 *  Copyright (C) 2018  Serge Bazanski <q3k@q3k.org>
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

#ifndef ARCH_API_H
#define ARCH_API_H

#include <algorithm>

#include "basectx.h"
#include "idstring.h"
#include "idstringlist.h"
#include "nextpnr_assertions.h"
#include "nextpnr_namespaces.h"
#include "nextpnr_types.h"

NEXTPNR_NAMESPACE_BEGIN

// Hand routing (GUI): a sink of a net that a route head can take over by changing the cell instead of binding pips,
// as the arch's virtual selects allow (on GateMate the LUT2 input and subtree swaps in front of the LUT inputs). See
// ArchAPI::getRouteRetargets.
struct RouteRetarget
{
    // The pip a click on the candidate picks (the last of `path`, into dst), and all pips drawn for it (none of
    // them is bound by the edit: they stand for the virtual selects)
    PipId pip;
    std::vector<PipId> path;
    WireId dst;
    // The sink that moves to the head wire (its arc is not routed); empty for a virtual pip that moves nothing
    IdString cell, port;
    // What the edit does, and "" if it can be done now, else what is in the way
    std::string text, why;
    // How the arch draws the way as a whole (in the device view's coordinates); empty: as the pips of `path`
    std::vector<GraphicElement> graphics;
};

// The logic of a cell for the logic check (logic_check.h, ArchAPI::evaluateCell): the values of its inputs, 64
// patterns at once (bit i of each word is pattern i).
struct CellEvalInputs
{
    virtual ~CellEvalInputs() = default;
    // The value of an input port; *connected (if given) tells whether a net is on it (the value is then 0)
    virtual uint64_t value(IdString port, bool *connected = nullptr) const = 0;
};

// What ArchAPI::evaluateCell computes for a cell.
struct CellEvalResult
{
    // Combinational outputs: each a function of the inputs only. An output port missing here is a free variable of the
    // check (a register or RAM output, a top-level input): it is the same in two netlists where cell and port names are.
    dict<IdString, uint64_t> outputs;
    // What the cell consumes in a way the check compares between two netlists (a register's data, clock, enable,
    // set/reset, a RAM's or an output buffer's inputs), under names that stay the same over the arch's own conversions
    // (e.g. "CLK" whichever pin the clock arrives on, its polarity applied). Combinational cells have none.
    dict<IdString, uint64_t> sinks;
};

// The specification of the Arch API (pure virtual)
template <typename R> struct ArchAPI : BaseCtx
{
    // Basic config
    virtual IdString archId() const = 0;
    virtual std::string getChipName() const = 0;
    virtual typename R::ArchArgsT archArgs() const = 0;
    virtual IdString archArgsToId(typename R::ArchArgsT args) const = 0;
    virtual int getGridDimX() const = 0;
    virtual int getGridDimY() const = 0;
    virtual int getTileBelDimZ(int x, int y) const = 0;
    virtual int getTilePipDimZ(int x, int y) const = 0;
    virtual char getNameDelimiter() const = 0;
    // Bel methods
    virtual typename R::AllBelsRangeT getBels() const = 0;
    virtual IdStringList getBelName(BelId bel) const = 0;
    virtual BelId getBelByName(IdStringList name) const = 0;
    virtual uint32_t getBelChecksum(BelId bel) const = 0;
    virtual void bindBel(BelId bel, CellInfo *cell, PlaceStrength strength) = 0;
    virtual void unbindBel(BelId bel) = 0;
    virtual Loc getBelLocation(BelId bel) const = 0;
    virtual BelId getBelByLocation(Loc loc) const = 0;
    virtual typename R::TileBelsRangeT getBelsByTile(int x, int y) const = 0;
    virtual bool getBelGlobalBuf(BelId bel) const = 0;
    virtual bool checkBelAvail(BelId bel) const = 0;
    virtual CellInfo *getBoundBelCell(BelId bel) const = 0;
    virtual CellInfo *getConflictingBelCell(BelId bel) const = 0;
    virtual IdString getBelType(BelId bel) const = 0;
    virtual bool getBelHidden(BelId bel) const = 0;
    virtual typename R::BelAttrsRangeT getBelAttrs(BelId bel) const = 0;
    virtual WireId getBelPinWire(BelId bel, IdString pin) const = 0;
    virtual PortType getBelPinType(BelId bel, IdString pin) const = 0;
    virtual typename R::BelPinsRangeT getBelPins(BelId bel) const = 0;
    virtual typename R::CellBelPinRangeT getBelPinsForCellPin(const CellInfo *cell_info, IdString pin) const = 0;
    // Wire methods
    virtual typename R::AllWiresRangeT getWires() const = 0;
    virtual WireId getWireByName(IdStringList name) const = 0;
    virtual IdStringList getWireName(WireId wire) const = 0;
    virtual IdString getWireType(WireId wire) const = 0;
    virtual typename R::WireAttrsRangeT getWireAttrs(WireId) const = 0;
    virtual typename R::DownhillPipRangeT getPipsDownhill(WireId wire) const = 0;
    virtual typename R::UphillPipRangeT getPipsUphill(WireId wire) const = 0;
    virtual typename R::WireBelPinRangeT getWireBelPins(WireId wire) const = 0;
    virtual uint32_t getWireChecksum(WireId wire) const = 0;
    virtual void bindWire(WireId wire, NetInfo *net, PlaceStrength strength) = 0;
    virtual void unbindWire(WireId wire) = 0;
    virtual bool checkWireAvail(WireId wire) const = 0;
    virtual NetInfo *getBoundWireNet(WireId wire) const = 0;
    virtual WireId getConflictingWireWire(WireId wire) const = 0;
    virtual NetInfo *getConflictingWireNet(WireId wire) const = 0;
    virtual DelayQuad getWireDelay(WireId wire) const = 0;
    virtual IdString getWireConstantValue(WireId wire) const = 0;
    // Pip methods
    virtual typename R::AllPipsRangeT getPips() const = 0;
    virtual PipId getPipByName(IdStringList name) const = 0;
    virtual IdStringList getPipName(PipId pip) const = 0;
    virtual IdString getPipType(PipId pip) const = 0;
    virtual typename R::PipAttrsRangeT getPipAttrs(PipId) const = 0;
    virtual uint32_t getPipChecksum(PipId pip) const = 0;
    virtual void bindPip(PipId pip, NetInfo *net, PlaceStrength strength) = 0;
    virtual void unbindPip(PipId pip) = 0;
    virtual bool checkPipAvail(PipId pip) const = 0;
    virtual bool checkPipAvailForNet(PipId pip, const NetInfo *net) const = 0;
    virtual NetInfo *getBoundPipNet(PipId pip) const = 0;
    virtual WireId getConflictingPipWire(PipId pip) const = 0;
    virtual NetInfo *getConflictingPipNet(PipId pip) const = 0;
    // The placement or routing was changed outside the flow (by hand, in the GUI): recompute whatever
    // the availability checks of bels and pips derive from it (called with the context lock held)
    virtual void notifyDesignEdited() = 0;
    // A cell of a routed design is moved or unplaced by hand: undo what the arch's post-route pass changed on
    // the cell for its routing at the old place (e.g. pins renamed to the inputs the routing reached), so the
    // router can route it again elsewhere. Returns "" (also when there is nothing to undo) or why it can not be
    // done; with check_only nothing changes. Called with the context lock held.
    virtual std::string prepareCellForMove(CellInfo *cell, bool check_only) = 0;
    // Hand routing: what a route head `wire` of `net` offers besides its downhill pips. Each entry either moves an
    // unrouted sink of the net to the head wire by changing its cell (ports and configuration, the function kept;
    // applyRouteRetarget), or (cell empty) marks a pip downhill of the head that hand routing must not bind, with
    // why. Called with the context lock held.
    virtual std::vector<RouteRetarget> getRouteRetargets(const NetInfo *net, WireId wire) const = 0;
    // Move the sink `port` of `cell` to the input whose sink wire is `wire`, as getRouteRetargets offers it.
    // Returns "" or why it can not be done; with check_only nothing changes. Called with the context lock held.
    virtual std::string applyRouteRetarget(CellInfo *cell, IdString port, WireId wire, bool check_only) = 0;
    // Saving and loading a design part way through the flow (JSON): the arch's own state beyond the netlist,
    // placement and routing (e.g. clusters, what the packer worked out) as cell, net and design attributes before
    // writing (archStateToAttributes, from write_json_file), and back after loading (attributesToArchState, from
    // attributesToArchInfo, before the cells are bound)
    virtual void archStateToAttributes() = 0;
    virtual void attributesToArchState() = 0;
    // The logic check (logic_check.h): evaluate a cell for 64 input patterns at once. It may only use the cell's type,
    // parameters, attributes and the names and directions of its ports (in a reference netlist the ports have no
    // nets: connection goes through `inputs`). Returns false where the arch does not know the cell's logic; the check
    // then compares every input of it under its port name and takes every output as a free variable.
    virtual bool evaluateCell(const CellInfo *cell, const CellEvalInputs &inputs, CellEvalResult &result) const = 0;
    virtual WireId getPipSrcWire(PipId pip) const = 0;
    virtual WireId getPipDstWire(PipId pip) const = 0;
    virtual DelayQuad getPipDelay(PipId pip) const = 0;
    virtual Loc getPipLocation(PipId pip) const = 0;
    virtual bool isPipInverting(PipId pip) const = 0;
    // Group methods
    virtual GroupId getGroupByName(IdStringList name) const = 0;
    virtual IdStringList getGroupName(GroupId group) const = 0;
    virtual typename R::AllGroupsRangeT getGroups() const = 0;
    virtual typename R::GroupBelsRangeT getGroupBels(GroupId group) const = 0;
    virtual typename R::GroupWiresRangeT getGroupWires(GroupId group) const = 0;
    virtual typename R::GroupPipsRangeT getGroupPips(GroupId group) const = 0;
    virtual typename R::GroupGroupsRangeT getGroupGroups(GroupId group) const = 0;
    // Delay Methods
    virtual delay_t predictDelay(BelId src_bel, IdString src_pin, BelId dst_bel, IdString dst_pin) const = 0;
    virtual delay_t getDelayEpsilon() const = 0;
    virtual delay_t getRipupDelayPenalty() const = 0;
    virtual float getDelayNS(delay_t v) const = 0;
    virtual delay_t getDelayFromNS(float ns) const = 0;
    virtual uint32_t getDelayChecksum(delay_t v) const = 0;
    virtual delay_t estimateDelay(WireId src, WireId dst) const = 0;
    virtual BoundingBox getRouteBoundingBox(WireId src, WireId dst) const = 0;
    virtual bool getArcDelayOverride(const NetInfo *net_info, const PortRef &sink, DelayQuad &delay) const = 0;
    // Decal methods
    virtual typename R::DecalGfxRangeT getDecalGraphics(DecalId decal) const = 0;
    virtual DecalXY getBelDecal(BelId bel) const = 0;
    virtual DecalXY getWireDecal(WireId wire) const = 0;
    virtual DecalXY getPipDecal(PipId pip) const = 0;
    virtual DecalXY getGroupDecal(GroupId group) const = 0;
    // Whether the GUI draws wires and pips that no net uses. If not, it only visits the wires and pips
    // of nets, which matters for architectures with very large routing graphs.
    virtual bool guiDrawsUnusedRouting() const = 0;
    // Decals the GUI draws besides those of the bels, wires, pips and groups: details shown only
    // when zoomed in (every element has a min_scale of at least 150), each with the tiles it covers
    // (inclusive). The GUI draws them only for the area in view, and they cannot be selected.
    virtual std::vector<std::pair<DecalXY, BoundingBox>> getDetailDecals() const = 0;
    // For selecting in the GUI: the pips drawn in a detail decal near a point (in the decal's
    // coordinates). The GUI picks the closest of them, by their own decals.
    virtual std::vector<PipId> getDetailPips(DecalId decal, float x, float y) const = 0;
    // Where the GUI draws tile column x: the x of its left edge, a column being one unit wide.
    // Columns may be spread out, to make room for something drawn between them.
    virtual float getGuiColumnX(int x) const = 0;
    // Whether clicking a used wire or pip in the GUI selects its net rather than the wire or pip:
    // for a net that is hard to make out by its wires, like a global clock reaching every tile
    virtual bool guiSelectsNet(WireId wire) const = 0;
    virtual bool guiSelectsNet(PipId pip) const = 0;
    // Cell timing methods
    virtual bool getCellDelay(const CellInfo *cell, IdString fromPort, IdString toPort, DelayQuad &delay) const = 0;
    virtual TimingPortClass getPortTimingClass(const CellInfo *cell, IdString port, int &clockInfoCount) const = 0;
    virtual TimingClockingInfo getPortClockingInfo(const CellInfo *cell, IdString port, int index) const = 0;
    // Placement validity checks
    virtual bool isValidBelForCellType(IdString cell_type, BelId bel) const = 0;
    virtual IdString getBelBucketName(BelBucketId bucket) const = 0;
    virtual BelBucketId getBelBucketByName(IdString name) const = 0;
    virtual BelBucketId getBelBucketForBel(BelId bel) const = 0;
    virtual BelBucketId getBelBucketForCellType(IdString cell_type) const = 0;
    virtual bool isBelLocationValid(BelId bel, bool explain_invalid = false) const = 0;
    virtual typename R::CellTypeRangeT getCellTypes() const = 0;
    virtual typename R::BelBucketRangeT getBelBuckets() const = 0;
    virtual typename R::BucketBelRangeT getBelsInBucket(BelBucketId bucket) const = 0;
    // Cluster methods
    virtual CellInfo *getClusterRootCell(ClusterId cluster) const = 0;
    virtual BoundingBox getClusterBounds(ClusterId cluster) const = 0;
    virtual Loc getClusterOffset(const CellInfo *cell) const = 0;
    virtual bool isClusterStrict(const CellInfo *cell) const = 0;
    virtual bool getClusterPlacement(ClusterId cluster, BelId root_bel,
                                     std::vector<std::pair<CellInfo *, BelId>> &placement) const = 0;
    // Routing methods
    virtual void expandBoundingBox(BoundingBox &bb) const = 0;
    // Resource methods
    virtual GroupId getResourceKeyForPip(PipId pip) const = 0;
    virtual int getResourceValueForPip(PipId pip) const = 0;
    virtual bool isGroupResource(GroupId group) const = 0;

    // Flow methods
    virtual bool pack() = 0;
    virtual bool place() = 0;
    virtual bool route() = 0;
    virtual void assignArchInfo() = 0;
};

NEXTPNR_NAMESPACE_END

#endif /* ARCH_API_H */
