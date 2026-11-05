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
