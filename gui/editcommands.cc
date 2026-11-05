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

#include "editcommands.h"
#include "log.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

CellInfo *findCell(Context *ctx, IdString name)
{
    auto fnd = ctx->cells.find(name);
    return fnd == ctx->cells.end() ? nullptr : fnd->second.get();
}

NetInfo *findNet(Context *ctx, IdString name)
{
    auto fnd = ctx->nets.find(name);
    return fnd == ctx->nets.end() ? nullptr : fnd->second.get();
}

const char *strengthName(PlaceStrength s)
{
    switch (s) {
    case STRENGTH_NONE:
        return "NONE";
    case STRENGTH_WEAK:
        return "WEAK";
    case STRENGTH_STRONG:
        return "STRONG";
    case STRENGTH_PLACER:
        return "PLACER";
    case STRENGTH_FIXED:
        return "FIXED";
    case STRENGTH_LOCKED:
        return "LOCKED";
    case STRENGTH_USER:
        return "USER";
    }
    return "?";
}

std::string q(const std::string &s) { return "'" + s + "'"; }

} // namespace

// ---------------------------------------------------------------------------
// BindBelCmd

std::string BindBelCmd::checkApply(Context *ctx)
{
    if (bel == BelId())
        return "invalid bel";
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(cell.str(ctx)) + " is already placed at " + q(ctx->nameOfBel(ci->bel));
    if (!ctx->checkBelAvail(bel)) {
        CellInfo *other = ctx->getBoundBelCell(bel);
        if (other != nullptr)
            return "bel " + q(ctx->nameOfBel(bel)) + " is occupied by " + q(other->name.str(ctx));
        return "bel " + q(ctx->nameOfBel(bel)) + " is not available";
    }
    if (!ctx->isValidBelForCellType(ci->type, bel))
        return "cell type " + q(ci->type.str(ctx)) + " cannot be placed on bel " + q(ctx->nameOfBel(bel)) +
               " of type " + q(ctx->getBelType(bel).str(ctx));
    return {};
}

void BindBelCmd::apply(Context *ctx) { ctx->bindBel(bel, findCell(ctx, cell), strength); }

std::string BindBelCmd::postApply(Context *ctx)
{
    if (check_location && !ctx->isBelLocationValid(bel))
        return "placing " + q(cell.str(ctx)) + " at " + q(ctx->nameOfBel(bel)) + " violates location constraints";
    return {};
}

std::string BindBelCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    if (ci == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    if (ci->bel != bel)
        return "cell " + q(cell.str(ctx)) + " is no longer placed at " + q(ctx->nameOfBel(bel));
    return {};
}

void BindBelCmd::revert(Context *ctx) { ctx->unbindBel(bel); }

std::string BindBelCmd::describe(Context *ctx) const
{
    return "Place " + cell.str(ctx) + " at " + ctx->nameOfBel(bel);
}

// ---------------------------------------------------------------------------
// UnbindBelCmd

std::string UnbindBelCmd::checkApply(Context *ctx)
{
    if (bel == BelId())
        return "invalid bel";
    if (ctx->getBoundBelCell(bel) == nullptr)
        return "bel " + q(ctx->nameOfBel(bel)) + " is empty";
    return {};
}

void UnbindBelCmd::apply(Context *ctx)
{
    CellInfo *ci = ctx->getBoundBelCell(bel);
    prev_cell = ci->name;
    prev_strength = ci->belStrength;
    ctx->unbindBel(bel);
}

std::string UnbindBelCmd::checkRevert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, prev_cell);
    if (ci == nullptr)
        return "cell " + q(prev_cell.str(ctx)) + " does not exist";
    if (ci->bel != BelId())
        return "cell " + q(prev_cell.str(ctx)) + " has been placed elsewhere";
    if (!ctx->checkBelAvail(bel))
        return "bel " + q(ctx->nameOfBel(bel)) + " is no longer free";
    return {};
}

void UnbindBelCmd::revert(Context *ctx) { ctx->bindBel(bel, findCell(ctx, prev_cell), prev_strength); }

std::string UnbindBelCmd::describe(Context *ctx) const
{
    if (prev_cell != IdString())
        return "Unplace " + prev_cell.str(ctx) + " from " + ctx->nameOfBel(bel);
    return std::string("Unbind bel ") + ctx->nameOfBel(bel);
}

// ---------------------------------------------------------------------------
// BindPipCmd

std::string BindPipCmd::checkApply(Context *ctx)
{
    if (pip == PipId())
        return "invalid pip";
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetInfo *bound = ctx->getBoundPipNet(pip);
    if (bound != nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is already used by net " + q(bound->name.str(ctx));
    WireId dst = ctx->getPipDstWire(pip);
    NetInfo *dst_net = ctx->getBoundWireNet(dst);
    if (dst_net != nullptr)
        return "wire " + q(ctx->nameOfWire(dst)) + " is already used by net " + q(dst_net->name.str(ctx));
    if (!ctx->checkWireAvail(dst))
        return "wire " + q(ctx->nameOfWire(dst)) + " is not available";
    if (!ctx->checkPipAvailForNet(pip, ni))
        return "pip " + q(ctx->nameOfPip(pip)) + " is not available for net " + q(net.str(ctx));
    return {};
}

void BindPipCmd::apply(Context *ctx) { ctx->bindPip(pip, findNet(ctx, net), strength); }

std::string BindPipCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr || ctx->getBoundPipNet(pip) != ni)
        return "pip " + q(ctx->nameOfPip(pip)) + " is no longer bound to net " + q(net.str(ctx));
    return {};
}

void BindPipCmd::revert(Context *ctx) { ctx->unbindPip(pip); }

std::string BindPipCmd::describe(Context *ctx) const
{
    return "Bind pip " + std::string(ctx->nameOfPip(pip)) + " to " + net.str(ctx);
}

// ---------------------------------------------------------------------------
// UnbindPipCmd

std::string UnbindPipCmd::checkApply(Context *ctx)
{
    if (pip == PipId())
        return "invalid pip";
    if (ctx->getBoundPipNet(pip) == nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is not used";
    return {};
}

void UnbindPipCmd::apply(Context *ctx)
{
    NetInfo *ni = ctx->getBoundPipNet(pip);
    prev_net = ni->name;
    auto fnd = ni->wires.find(ctx->getPipDstWire(pip));
    prev_strength = fnd != ni->wires.end() ? fnd->second.strength : STRENGTH_NONE;
    ctx->unbindPip(pip);
}

std::string UnbindPipCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (ni == nullptr)
        return "net " + q(prev_net.str(ctx)) + " does not exist";
    if (ctx->getBoundPipNet(pip) != nullptr)
        return "pip " + q(ctx->nameOfPip(pip)) + " is used again";
    WireId dst = ctx->getPipDstWire(pip);
    if (ctx->getBoundWireNet(dst) != nullptr || !ctx->checkWireAvail(dst))
        return "wire " + q(ctx->nameOfWire(dst)) + " is used again";
    if (!ctx->checkPipAvailForNet(pip, ni))
        return "pip " + q(ctx->nameOfPip(pip)) + " is not available for net " + q(prev_net.str(ctx));
    return {};
}

void UnbindPipCmd::revert(Context *ctx) { ctx->bindPip(pip, findNet(ctx, prev_net), prev_strength); }

std::string UnbindPipCmd::describe(Context *ctx) const { return "Unbind pip " + std::string(ctx->nameOfPip(pip)); }

// ---------------------------------------------------------------------------
// BindWireCmd

std::string BindWireCmd::checkApply(Context *ctx)
{
    if (wire == WireId())
        return "invalid wire";
    if (findNet(ctx, net) == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    NetInfo *bound = ctx->getBoundWireNet(wire);
    if (bound != nullptr)
        return "wire " + q(ctx->nameOfWire(wire)) + " is already used by net " + q(bound->name.str(ctx));
    if (!ctx->checkWireAvail(wire))
        return "wire " + q(ctx->nameOfWire(wire)) + " is not available";
    return {};
}

void BindWireCmd::apply(Context *ctx) { ctx->bindWire(wire, findNet(ctx, net), strength); }

std::string BindWireCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr || ctx->getBoundWireNet(wire) != ni)
        return "wire " + q(ctx->nameOfWire(wire)) + " is no longer bound to net " + q(net.str(ctx));
    auto fnd = ni->wires.find(wire);
    if (fnd == ni->wires.end() || fnd->second.pip != PipId())
        return "wire " + q(ctx->nameOfWire(wire)) + " is now driven by a pip";
    return {};
}

void BindWireCmd::revert(Context *ctx) { ctx->unbindWire(wire); }

std::string BindWireCmd::describe(Context *ctx) const
{
    return "Bind wire " + std::string(ctx->nameOfWire(wire)) + " to " + net.str(ctx);
}

// ---------------------------------------------------------------------------
// UnbindWireCmd

std::string UnbindWireCmd::checkApply(Context *ctx)
{
    if (wire == WireId())
        return "invalid wire";
    if (ctx->getBoundWireNet(wire) == nullptr)
        return "wire " + q(ctx->nameOfWire(wire)) + " is not used";
    return {};
}

void UnbindWireCmd::apply(Context *ctx)
{
    NetInfo *ni = ctx->getBoundWireNet(wire);
    prev_net = ni->name;
    auto fnd = ni->wires.find(wire);
    prev_pip = fnd != ni->wires.end() ? fnd->second.pip : PipId();
    prev_strength = fnd != ni->wires.end() ? fnd->second.strength : STRENGTH_NONE;
    // Unbind through the pip if there is one, so the arch pip hooks (timing, uarch state) run.
    if (prev_pip != PipId())
        ctx->unbindPip(prev_pip);
    else
        ctx->unbindWire(wire);
}

std::string UnbindWireCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (ni == nullptr)
        return "net " + q(prev_net.str(ctx)) + " does not exist";
    if (ctx->getBoundWireNet(wire) != nullptr || !ctx->checkWireAvail(wire))
        return "wire " + q(ctx->nameOfWire(wire)) + " is used again";
    if (prev_pip != PipId() && (ctx->getBoundPipNet(prev_pip) != nullptr || !ctx->checkPipAvailForNet(prev_pip, ni)))
        return "pip " + q(ctx->nameOfPip(prev_pip)) + " is used again";
    return {};
}

void UnbindWireCmd::revert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, prev_net);
    if (prev_pip != PipId())
        ctx->bindPip(prev_pip, ni, prev_strength);
    else
        ctx->bindWire(wire, ni, prev_strength);
}

std::string UnbindWireCmd::describe(Context *ctx) const
{
    return "Unbind wire " + std::string(ctx->nameOfWire(wire));
}

// ---------------------------------------------------------------------------
// SetCellStrengthCmd

std::string SetCellStrengthCmd::checkApply(Context *ctx)
{
    if (findCell(ctx, cell) == nullptr)
        return "cell " + q(cell.str(ctx)) + " does not exist";
    return {};
}

void SetCellStrengthCmd::apply(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    prev_strength = ci->belStrength;
    ci->belStrength = strength;
    if (ci->bel != BelId())
        ctx->refreshUiBel(ci->bel);
}

std::string SetCellStrengthCmd::checkRevert(Context *ctx) { return checkApply(ctx); }

void SetCellStrengthCmd::revert(Context *ctx)
{
    CellInfo *ci = findCell(ctx, cell);
    ci->belStrength = prev_strength;
    if (ci->bel != BelId())
        ctx->refreshUiBel(ci->bel);
}

std::string SetCellStrengthCmd::describe(Context *ctx) const
{
    return "Set strength of " + cell.str(ctx) + " to " + strengthName(strength);
}

// ---------------------------------------------------------------------------
// SetNetStrengthCmd

std::string SetNetStrengthCmd::checkApply(Context *ctx)
{
    if (findNet(ctx, net) == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    return {};
}

void SetNetStrengthCmd::apply(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    prev.clear();
    for (auto &w : ni->wires) {
        prev.emplace_back(w.first, w.second.strength);
        w.second.strength = strength;
    }
}

std::string SetNetStrengthCmd::checkRevert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    if (ni == nullptr)
        return "net " + q(net.str(ctx)) + " does not exist";
    for (auto &p : prev)
        if (!ni->wires.count(p.first))
            return "routing of net " + q(net.str(ctx)) + " has changed";
    return {};
}

void SetNetStrengthCmd::revert(Context *ctx)
{
    NetInfo *ni = findNet(ctx, net);
    for (auto &p : prev)
        ni->wires.at(p.first).strength = p.second;
}

std::string SetNetStrengthCmd::describe(Context *ctx) const
{
    return "Set routing strength of " + net.str(ctx) + " to " + strengthName(strength);
}

// ---------------------------------------------------------------------------
// EditBatch

std::string EditBatch::add(std::unique_ptr<EditCommand> cmd)
{
    std::string err = cmd->checkApply(ctx);
    if (!err.empty())
        return err;
    cmd->apply(ctx);
    err = cmd->postApply(ctx);
    if (!err.empty()) {
        cmd->revert(ctx);
        return err;
    }
    cmds.push_back(std::move(cmd));
    return {};
}

// ---------------------------------------------------------------------------
// Undo stack entry

namespace {

std::string runGuarded(const std::function<std::string()> &fn)
{
    try {
        return fn();
    } catch (const log_execution_error_exception &) {
        return "error during edit (see log)";
    } catch (const std::exception &e) {
        return std::string("exception during edit: ") + e.what();
    }
}

class MacroUndoCommand : public QUndoCommand
{
  public:
    MacroUndoCommand(Context *ctx, const QString &text, std::vector<std::unique_ptr<EditCommand>> cmds)
            : QUndoCommand(text), ctx(ctx), cmds(std::move(cmds))
    {
    }

    void redo() override
    {
        error.clear();
        if (first) {
            // Already applied by EditStack::run.
            first = false;
            return;
        }
        ctx->lock();
        error = runGuarded([&]() -> std::string {
            for (size_t i = 0; i < cmds.size(); i++) {
                std::string err = cmds[i]->checkApply(ctx);
                if (err.empty()) {
                    cmds[i]->apply(ctx);
                    err = cmds[i]->postApply(ctx);
                    if (!err.empty())
                        cmds[i]->revert(ctx);
                }
                if (!err.empty()) {
                    for (size_t j = i; j-- > 0;)
                        cmds[j]->revert(ctx);
                    return err;
                }
            }
            return {};
        });
        ctx->refreshUi();
        ctx->unlock();
    }

    void undo() override
    {
        error.clear();
        ctx->lock();
        error = runGuarded([&]() -> std::string {
            for (size_t i = cmds.size(); i-- > 0;) {
                std::string err = cmds[i]->checkRevert(ctx);
                if (!err.empty()) {
                    for (size_t j = i + 1; j < cmds.size(); j++)
                        cmds[j]->apply(ctx);
                    return err;
                }
                cmds[i]->revert(ctx);
            }
            return {};
        });
        ctx->refreshUi();
        ctx->unlock();
    }

    std::string error;

  private:
    Context *ctx;
    std::vector<std::unique_ptr<EditCommand>> cmds;
    bool first = true;
};

} // namespace

// ---------------------------------------------------------------------------
// EditStack

EditStack::EditStack(QObject *parent) : QObject(parent)
{
    connect(&stack, &QUndoStack::canUndoChanged, this, &EditStack::stateChanged);
    connect(&stack, &QUndoStack::canRedoChanged, this, &EditStack::stateChanged);
    connect(&stack, &QUndoStack::indexChanged, this, &EditStack::stateChanged);
}

EditStack::~EditStack() {}

void EditStack::setContext(Context *new_ctx)
{
    ctx = new_ctx;
    clear();
}

bool EditStack::canEdit(QString *why) const
{
    QString reason;
    if (ctx == nullptr)
        reason = "No design loaded";
    else if (task_busy)
        reason = "Cannot edit while a pack/place/route task is running";
    else if (!edit_mode)
        reason = "Edit mode is off (press E to enable it)";
    if (why)
        *why = reason;
    return reason.isEmpty();
}

void EditStack::setEditMode(bool on)
{
    if (edit_mode == on)
        return;
    edit_mode = on;
    Q_EMIT message(on ? "Edit mode on" : "Edit mode off");
    Q_EMIT stateChanged();
}

void EditStack::setBusy(bool on)
{
    if (task_busy == on)
        return;
    task_busy = on;
    // A pass changes the design behind our back: the history is no longer valid.
    if (on)
        stack.clear();
    Q_EMIT stateChanged();
}

void EditStack::clear()
{
    stack.clear();
    Q_EMIT stateChanged();
}

std::string EditStack::run(const QString &text, const std::function<std::string(EditBatch &)> &fn)
{
    QString why;
    if (!canEdit(&why)) {
        Q_EMIT message(why);
        return why.toStdString();
    }
    EditBatch batch(ctx);
    ctx->lock();
    std::string err = runGuarded([&]() { return fn(batch); });
    if (!err.empty()) {
        std::string rerr = runGuarded([&]() -> std::string {
            for (size_t i = batch.cmds.size(); i-- > 0;)
                batch.cmds[i]->revert(ctx);
            return {};
        });
        if (!rerr.empty())
            err += "; rollback failed: " + rerr;
        batch.cmds.clear();
    }
    bool changed = !batch.cmds.empty();
    if (changed || !err.empty())
        ctx->refreshUi();
    ctx->unlock();

    if (!err.empty()) {
        Q_EMIT message(text + " failed: " + QString::fromStdString(err));
        Q_EMIT designChanged();
        return err;
    }
    if (changed) {
        stack.push(new MacroUndoCommand(ctx, text, std::move(batch.cmds)));
        Q_EMIT message(text);
        Q_EMIT designChanged();
    }
    return {};
}

std::string EditStack::execute(std::unique_ptr<EditCommand> cmd, const QString &text)
{
    QString t = text;
    if (t.isEmpty() && ctx != nullptr)
        t = QString::fromStdString(cmd->describe(ctx));
    // fn is called exactly once by run(), so moving out of the captured reference is fine.
    return run(t, [&](EditBatch &b) { return b.add(std::move(cmd)); });
}

void EditStack::undoRedo(bool is_undo)
{
    QString why;
    if (!canEdit(&why)) {
        Q_EMIT message(why);
        return;
    }
    int idx = is_undo ? stack.index() - 1 : stack.index();
    if (idx < 0 || idx >= stack.count()) {
        Q_EMIT message(is_undo ? "Nothing to undo" : "Nothing to redo");
        return;
    }
    auto *cmd = static_cast<MacroUndoCommand *>(const_cast<QUndoCommand *>(stack.command(idx)));
    QString text = cmd->text();
    if (is_undo)
        stack.undo();
    else
        stack.redo();
    if (!cmd->error.empty()) {
        Q_EMIT message(QString(is_undo ? "Cannot undo '" : "Cannot redo '") + text +
                       "': " + QString::fromStdString(cmd->error) + " - edit history cleared");
        stack.clear();
    } else {
        Q_EMIT message(QString(is_undo ? "Undo: " : "Redo: ") + text);
    }
    Q_EMIT designChanged();
}

void EditStack::undo() { undoRedo(true); }

void EditStack::redo() { undoRedo(false); }

NEXTPNR_NAMESPACE_END
