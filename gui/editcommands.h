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
 * Command layer for manual editing of placement and routing in the GUI.
 *
 * Every change of design state made from the GUI must go through EditStack,
 * so that it can be undone / redone.
 *
 * Usage
 * -----
 *   EditStack *es = ...;   // owned by BaseMainWindow, given to DesignWidget::setEditStack
 *                          // and FPGAViewWidget::setEditStack
 *   QString why;
 *   if (!es->canEdit(&why)) { ... }   // edit mode off or a pack/place/route task is running
 *
 *   // One user action = one undo entry. The lambda runs with the context lock
 *   // held (do NOT take ctx->lock()/ui_mutex yourself inside, and do not call run()
 *   // while holding it). Each batch.add() validates the command and, if valid,
 *   // applies it immediately (so later commands see the new state). Returning a
 *   // non-empty string from the lambda aborts: everything applied so far in this
 *   // batch is reverted and nothing is pushed. Exceptions (log_error) also abort.
 *   std::string err = es->run("Move cell X", [&](EditBatch &b) {
 *       std::string e = b.add(std::make_unique<UnbindBelCmd>(old_bel));
 *       if (!e.empty()) return e;
 *       return b.add(std::make_unique<BindBelCmd>(cell->name, new_bel, STRENGTH_USER));
 *   });
 *   // err is empty on success; on failure it is also shown in the status bar
 *   // (EditStack::message signal). Shortcut for a single command: es->execute(cmd).
 *
 *   A batch that ends up empty (no command added) pushes nothing.
 *
 * After every successful run/undo/redo the stack calls ctx->refreshUi() (the
 * FPGAViewWidget picks this up on its next render tick) and emits designChanged(),
 * which BaseMainWindow connects to DesignWidget::refreshAfterEdit (tree and
 * property view refresh).
 *
 * Undo/redo re-validate (checkRevert / checkApply). If the design has been changed
 * behind the stack's back (e.g. Python console) and an entry can no longer be
 * undone, the entry is rolled back to its previous state, the whole stack is
 * cleared and a message is emitted.
 *
 * The stack is cleared on context change / JSON load and when a TaskManager pass
 * starts.
 *
 * Writing new commands: derive from EditCommand. checkApply() must not modify
 * anything; apply() records whatever it needs to revert exactly (previous
 * owner / strength) and then mutates; postApply() may veto the result after the
 * fact (the batch then reverts it); checkRevert()/revert() undo it. Cells and
 * nets are referenced by name (IdString), never by pointer.
 */

#ifndef EDITCOMMANDS_H
#define EDITCOMMANDS_H

#include <QObject>
#include <QString>
#include <QUndoStack>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

class EditCommand
{
  public:
    virtual ~EditCommand() = default;
    // Return "" if the command can be applied in the current state, else a human-readable reason.
    virtual std::string checkApply(Context *ctx) = 0;
    // Apply the change; record what is needed for revert().
    virtual void apply(Context *ctx) = 0;
    // Called right after apply(); a non-empty result makes the batch revert this command and abort.
    virtual std::string postApply(Context *ctx) { return {}; }
    // Return "" if the command can be reverted in the current state.
    virtual std::string checkRevert(Context *ctx) = 0;
    virtual void revert(Context *ctx) = 0;
    virtual std::string describe(Context *ctx) const = 0;
};

// Bind a cell (by name) to a bel. The cell must be unplaced, the bel free and of a valid type.
// If check_location is set, isBelLocationValid(bel) is checked after binding.
class BindBelCmd : public EditCommand
{
  public:
    BindBelCmd(IdString cell, BelId bel, PlaceStrength strength, bool check_location = true)
            : cell(cell), bel(bel), strength(strength), check_location(check_location)
    {
    }
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string postApply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    IdString cell;
    BelId bel;
    PlaceStrength strength;
    bool check_location;
};

// Unbind whatever cell is bound to a bel. Revert restores the cell with its previous strength.
class UnbindBelCmd : public EditCommand
{
  public:
    explicit UnbindBelCmd(BelId bel) : bel(bel) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    BelId bel;
    IdString prev_cell;
    PlaceStrength prev_strength = STRENGTH_NONE;
};

// Bind a pip (and thereby its destination wire) to a net.
class BindPipCmd : public EditCommand
{
  public:
    BindPipCmd(IdString net, PipId pip, PlaceStrength strength) : net(net), pip(pip), strength(strength) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    IdString net;
    PipId pip;
    PlaceStrength strength;
};

// Unbind a pip (and its destination wire) from its net.
class UnbindPipCmd : public EditCommand
{
  public:
    explicit UnbindPipCmd(PipId pip) : pip(pip) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    PipId pip;
    IdString prev_net;
    PlaceStrength prev_strength = STRENGTH_NONE;
};

// Bind a wire to a net without a driving pip (e.g. a source wire).
class BindWireCmd : public EditCommand
{
  public:
    BindWireCmd(IdString net, WireId wire, PlaceStrength strength) : net(net), wire(wire), strength(strength) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    IdString net;
    WireId wire;
    PlaceStrength strength;
};

// Unbind a wire from its net (also releases the pip driving it, if any; revert rebinds that pip).
class UnbindWireCmd : public EditCommand
{
  public:
    explicit UnbindWireCmd(WireId wire) : wire(wire) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    WireId wire;
    IdString prev_net;
    PipId prev_pip;
    PlaceStrength prev_strength = STRENGTH_NONE;
};

// Undo what the arch's post-route pass changed on a cell for its routing (ArchAPI::prepareCellForMove: on
// GateMate the pins renamed to the CPE inputs the routing reached and the inputs fed through CP-line cells), so
// a cell of a routed design can be routed again at another place. Revert restores the cell's ports (names,
// directions, nets), parameters and bel pin mapping as they were.
class PrepareCellForMoveCmd : public EditCommand
{
  public:
    explicit PrepareCellForMoveCmd(IdString cell) : cell(cell) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    struct PortState
    {
        IdString name;
        PortType type;
        IdString net;
    };
    static std::vector<PortState> ports(const CellInfo *ci);
    IdString cell;
    std::vector<PortState> prev_ports, new_ports;
    dict<IdString, Property> prev_params;
    dict<IdString, std::vector<IdString>> prev_bel_pins;
};

// Change the placement strength of a cell.
class SetCellStrengthCmd : public EditCommand
{
  public:
    SetCellStrengthCmd(IdString cell, PlaceStrength strength) : cell(cell), strength(strength) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    IdString cell;
    PlaceStrength strength;
    PlaceStrength prev_strength = STRENGTH_NONE;
};

// Change the routing strength of all wires currently bound to a net.
class SetNetStrengthCmd : public EditCommand
{
  public:
    SetNetStrengthCmd(IdString net, PlaceStrength strength) : net(net), strength(strength) {}
    std::string checkApply(Context *ctx) override;
    void apply(Context *ctx) override;
    std::string checkRevert(Context *ctx) override;
    void revert(Context *ctx) override;
    std::string describe(Context *ctx) const override;

  private:
    IdString net;
    PlaceStrength strength;
    std::vector<std::pair<WireId, PlaceStrength>> prev;
};

// Placement strengths of manual edits. A cell placed or moved by hand gets STRENGTH_MANUAL: the
// placers may still move it (they keep only cells above STRENGTH_STRONG). "Lock" sets
// STRENGTH_MANUAL_LOCKED, "Unlock" STRENGTH_MANUAL. A cell counts as locked when its strength is
// above STRENGTH_STRONG (also LOCKED cells from constraints): a locked cell is never displaced by a
// swap, but can still be moved explicitly, and then keeps its strength.
// STRENGTH_FIXED cells were bound by the packer to a site the hardware ties them to (e.g. on GateMate
// the CPE next to an IO or RAM port, whose LUT half and RAMIO half only connect inside that CPE, the
// IO/RAM cells themselves, clocking and the CP-line cells of the routing): the placers never move them,
// so neither can the GUI (no move, no unlock).
constexpr PlaceStrength STRENGTH_MANUAL = STRENGTH_STRONG;
constexpr PlaceStrength STRENGTH_MANUAL_LOCKED = STRENGTH_USER;
inline bool isCellLocked(const CellInfo *ci) { return ci->belStrength > STRENGTH_STRONG; }
inline bool isCellFixed(const CellInfo *ci) { return ci->bel != BelId() && ci->belStrength == STRENGTH_FIXED; }
// Why a cell cannot be moved by hand ("" if it can)
std::string cellFixedReason(Context *ctx, const CellInfo *ci);

// The cells that are placed together with ci: its whole cluster (root first), or ci alone.
std::vector<CellInfo *> placementGroup(Context *ctx, CellInfo *ci);

class EditBatch;

// The routing that only makes sense with the cell on its bel: what a net routes below one of the cell's sink wires
// (NetStatus::belowSinkWires, e.g. on GateMate the CPE-internal routing of a design routed before, part of the
// cell's configuration) and the whole routing of the nets the cell drives (it grows from the source wire at the
// cell's bel). The routing of the arcs into the cell, up to its sink wires, stays (the router reuses it).
std::vector<WireId> cellRouting(Context *ctx, const CellInfo *ci);
// Add the commands unbinding the cell on `bel` to a batch, together with cellRouting (also when locked), so moving
// or unplacing a cell leaves nothing of it in the old site; undo restores both.
std::string addUnbindBel(EditBatch &b, BelId bel);
// Add the commands placing `cell` at `bel` to a batch: place it if unplaced, else move it. If the
// bel is taken by another unlocked cell outside a cluster, the two are swapped (an unplaced cell
// displaces the other, which ends up unplaced). A cluster root moves with its whole cluster
// (getClusterPlacement), to free bels only. Cell type, region and, at the end, location
// validity of every bel involved are checked. Returns "" or the reason for refusing.
std::string addPlaceCell(EditBatch &b, IdString cell, BelId bel);
// Add the commands moving placed cells by (dx, dy) grid units to a batch, all or nothing: each
// cell goes to the bel at (x + dx, y + dy, z), which must exist and be of the same bel type;
// clusters move as a whole (getClusterPlacement at the shifted root). Target bels must be free or
// taken by cells that move too. Strengths are kept for locked cells, else STRENGTH_MANUAL.
// Returns "" or the reason for refusing.
std::string addMoveCells(EditBatch &b, const std::vector<IdString> &cells, int dx, int dy);
// Undo text for addPlaceCell: "Place X at B", "Move X to B", "Swap X with Y", ...
QString describePlaceCell(Context *ctx, IdString cell, BelId bel);

// Rip-up (see netstatus.h for the arc model). The binding of the net's source wire is kept, as
// after routing (the routers bind it themselves when missing, and rip up only what is not locked).
// Locked routing (a wire bound above STRENGTH_STRONG, see "Lock routing") is refused: unlock first.
// Remove all routing of a net: every wire except the source wire.
std::string addUnrouteNet(EditBatch &b, IdString net);
// Remove the routing serving only the arc to `sink` (a sink wire of the net): from the sink wire
// up its path while a wire drives nothing else of the net (NetStatus::arcOnlyWires).
std::string addUnrouteArc(EditBatch &b, IdString net, WireId sink);
// Remove a bound pip, its destination wire and everything of the net downstream of it.
std::string addRemovePip(EditBatch &b, PipId pip);

// Before writing the design (File > Save JSON): store the current placement and routing in the
// attributes the JSON carries (archInfoToAttributes, which only the placer / router call), and drop
// the stale placement attributes of cells unplaced by hand. The caller holds the context lock.
void prepareDesignForWrite(Context *ctx);

// A set of commands applied as one unit (one undo entry). Only constructed by EditStack::run.
class EditBatch
{
  public:
    explicit EditBatch(Context *ctx) : ctx(ctx) {}
    Context *getContext() const { return ctx; }
    // Validate and apply cmd now. Returns "" on success, else the reason (nothing of cmd applied).
    std::string add(std::unique_ptr<EditCommand> cmd);
    bool empty() const { return cmds.empty(); }
    size_t size() const { return cmds.size(); }
    // Revert (newest first) and drop the commands added after the first n
    void rollbackTo(size_t n);

  private:
    friend class EditStack;
    Context *ctx;
    std::vector<std::unique_ptr<EditCommand>> cmds;
};

class EditStack : public QObject
{
    Q_OBJECT

  public:
    explicit EditStack(QObject *parent = nullptr);
    ~EditStack();

    void setContext(Context *ctx); // also clears the stack
    Context *getContext() const { return ctx; }

    bool editMode() const { return edit_mode; }
    bool busy() const { return task_busy; }
    // True if edits are allowed now; otherwise *why gets the reason.
    bool canEdit(QString *why = nullptr) const;

    // Run fn as one undoable action (see top of file). Returns "" on success, else the error.
    std::string run(const QString &text, const std::function<std::string(EditBatch &)> &fn);
    // Single command as one undoable action, text defaults to cmd->describe().
    std::string execute(std::unique_ptr<EditCommand> cmd, const QString &text = QString());
    // Like run(), but always reverted afterwards and nothing pushed or signalled (the pending UI
    // refresh state of the context is restored too): whether fn would succeed now. Does not need
    // edit mode, only no running task. Returns "" or the reason.
    std::string dryRun(const std::function<std::string(EditBatch &)> &fn);
    // Show a status message (same channel as the stack's own messages).
    void notify(const QString &msg) { Q_EMIT message(msg); }

    QUndoStack *undoStack() { return &stack; }

  public Q_SLOTS:
    void setEditMode(bool on);
    void setBusy(bool on);
    void undo();
    void redo();
    void clear();

  Q_SIGNALS:
    // Design state changed by run/undo/redo.
    void designChanged();
    // Human-readable status message (errors, refusals).
    void message(const QString &msg);
    // editMode / busy / undo-redo availability changed.
    void stateChanged();

  private:
    void undoRedo(bool is_undo);

    Context *ctx = nullptr;
    QUndoStack stack;
    bool edit_mode = false;
    bool task_busy = false;
};

NEXTPNR_NAMESPACE_END

#endif // EDITCOMMANDS_H
