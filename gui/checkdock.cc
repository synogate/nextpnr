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

#include "checkdock.h"
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include "editcommands.h"
#include "netstatus.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
const int KindRole = Qt::UserRole, NameRole = Qt::UserRole + 1;
enum Kind
{
    KIND_NONE,
    KIND_CELL,
    KIND_NET,
    KIND_WIRE,
    KIND_PIP
};
enum Category
{
    CAT_UNPLACED,
    CAT_UNROUTED,
    CAT_CONFLICT,
    CAT_DANGLING,
    CAT_INVALID,
    CAT_COUNT
};
const char *categoryName[CAT_COUNT] = {"Unplaced cells", "Nets not fully routed", "Conflicts", "Dangling routing",
                                       "Cells on invalid locations"};

struct Entry
{
    Kind kind;
    QString name; // of the element (cell, net, wire, pip)
    QString text, detail;
};
} // namespace

CheckDock::CheckDock(EditStack *es, QWidget *parent) : QDockWidget("Design check", parent), es(es)
{
    setObjectName("CheckDock");
    QWidget *w = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);

    QHBoxLayout *top = new QHBoxLayout();
    header = new QLabel(w);
    header->setObjectName("checkHeader");
    header->setWordWrap(true);
    header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    btnRefresh = new QPushButton("Refresh", w);
    btnRefresh->setToolTip("Check the design again now (it is also checked shortly after every edit)");
    top->addWidget(header, 1);
    top->addWidget(btnRefresh, 0, Qt::AlignTop);
    layout->addLayout(top);
    connect(btnRefresh, &QPushButton::clicked, this, &CheckDock::refresh);

    list = new QTreeWidget(w);
    list->setObjectName("checkList");
    list->setColumnCount(2);
    list->setHeaderLabels({"Item", "Problem"});
    list->setUniformRowHeights(true);
    list->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    list->setColumnWidth(0, 220);
    list->setToolTip("Double-click / Enter: select the element and zoom to it (an unplaced cell is armed for "
                     "placement, a net shown in the net view)");
    layout->addWidget(list, 1);
    connect(list, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) { activate(item); });

    setWidget(w);
    expanded.assign(CAT_COUNT, false);

    timer = new QTimer(this);
    timer->setSingleShot(true);
    timer->setInterval(REFRESH_DELAY_MS);
    connect(timer, &QTimer::timeout, this, &CheckDock::refresh);
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        if (visible && dirty)
            timer->start();
    });
    header->setText("No design");
}

void CheckDock::setContext(Context *c)
{
    ctx = c;
    firstFill = true;
    scheduleRefresh();
}

void CheckDock::scheduleRefresh()
{
    dirty = true;
    if (isVisible())
        timer->start();
}

void CheckDock::refresh()
{
    timer->stop();
    if (ctx == nullptr) {
        header->setText("No design");
        list->clear();
        return;
    }
    if (es != nullptr && es->busy()) {
        // The task changes the design: check when it is done
        header->setText("A task is running; the check follows when it is done");
        dirty = true;
        return;
    }
    dirty = false;
    QElapsedTimer clock;
    clock.start();
    std::vector<Entry> entries[CAT_COUNT];
    int count[CAT_COUNT] = {0};
    auto add = [&](int cat, Kind kind, const QString &name, const QString &text, const QString &detail) {
        count[cat]++;
        if (int(entries[cat].size()) < MAX_ENTRIES)
            entries[cat].push_back(Entry{kind, name, text, detail});
    };
    int cells = 0, nets = 0;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        pool<BelId> checked;
        for (auto &c : ctx->cells) {
            CellInfo *ci = c.second.get();
            cells++;
            QString name = ci->name.c_str(ctx);
            if (ci->bel == BelId()) {
                add(CAT_UNPLACED, KIND_CELL, name, name, QString("unplaced (%1)").arg(ci->type.c_str(ctx)));
                continue;
            }
            if (checked.insert(ci->bel).second && !ctx->isBelLocationValid(ci->bel))
                add(CAT_INVALID, KIND_CELL, name, name,
                    QString("invalid location %1 (%2)").arg(ctx->nameOfBel(ci->bel)).arg(ci->type.c_str(ctx)));
        }
        for (auto &n : ctx->nets) {
            NetInfo *ni = n.second.get();
            nets++;
            if (ni->driver.cell == nullptr && ni->users.entries() == 0 && ni->wires.empty())
                continue;
            QString name = ni->name.c_str(ctx);
            NetStatus::Status st = NetStatus::compute(ctx, ni);
            if (st.partial + st.unrouted > 0)
                add(CAT_UNROUTED, KIND_NET, name, name, QString::fromStdString(NetStatus::summary(st)));
            if (st.source != WireId()) {
                NetInfo *bound = ctx->getBoundWireNet(st.source);
                if (bound != nullptr && bound != ni)
                    add(CAT_CONFLICT, KIND_WIRE, ctx->nameOfWire(st.source), ctx->nameOfWire(st.source),
                        QString("source wire of %1 used by %2").arg(name).arg(bound->name.c_str(ctx)));
            }
            for (auto &arc : st.arcs)
                if (arc.blocked)
                    add(CAT_CONFLICT, KIND_WIRE, ctx->nameOfWire(arc.sink), ctx->nameOfWire(arc.sink),
                        QString("sink wire of %1 (%2.%3) used by %4")
                                .arg(name)
                                .arg(arc.cell.c_str(ctx))
                                .arg(arc.port.c_str(ctx))
                                .arg(ctx->nameOf(ctx->getBoundWireNet(arc.sink))));
            pool<WireId> arc_sinks;
            for (auto &arc : st.arcs)
                if (arc.sink != WireId())
                    arc_sinks.insert(arc.sink);
            for (auto &w : ni->wires) {
                NetInfo *bound = ctx->getBoundWireNet(w.first);
                if (bound != ni) {
                    add(CAT_CONFLICT, KIND_WIRE, ctx->nameOfWire(w.first), ctx->nameOfWire(w.first),
                        QString("wire of %1 bound to %2").arg(name).arg(bound ? bound->name.c_str(ctx) : "nothing"));
                    continue;
                }
                PipId pip = w.second.pip;
                if (pip == PipId())
                    continue;
                if (!ctx->checkPipAvailForNet(pip, ni)) {
                    NetInfo *pbound = ctx->getBoundPipNet(pip);
                    QString why = pbound != nullptr && pbound != ni
                                          ? QString("used by %1").arg(pbound->name.c_str(ctx))
                                          : QString("not available (resource / mode taken)");
                    add(CAT_CONFLICT, KIND_PIP, ctx->nameOfPip(pip), ctx->nameOfPip(pip),
                        QString("pip of %1 %2").arg(name).arg(why));
                }
                // (routing below a sink wire whose arc is unrouted waits for the arc, see NetStatus::belowSinkWires)
                if (w.first != st.source && !ni->wires.count(ctx->getPipSrcWire(pip)) &&
                    !arc_sinks.count(ctx->getPipSrcWire(pip)))
                    add(CAT_DANGLING, KIND_WIRE, ctx->nameOfWire(w.first), ctx->nameOfWire(w.first),
                        QString("of %1: driven from %2, which is not part of the net")
                                .arg(name)
                                .arg(ctx->nameOfWire(ctx->getPipSrcWire(pip))));
            }
            // Stubs: branches of the routing that end without reaching a sink wire (e.g. the routing to a cell
            // that was moved or unplaced). (GateMate routes on below a CPE's sink pin wire, hence the walk up;
            // a CP-line / bridge net ends at the pip into the source wire of the net split off from it.)
            if (ni->wires.size() > 1) {
                pool<WireId> sinks, drives;
                pool<IdString> users;
                for (auto &usr : ni->users)
                    if (usr.cell != nullptr)
                        users.insert(usr.cell->name);
                for (auto &arc : st.arcs)
                    if (arc.sink != WireId())
                        sinks.insert(arc.sink);
                for (auto &w : ni->wires)
                    if (w.second.pip != PipId())
                        drives.insert(ctx->getPipSrcWire(w.second.pip));
                for (auto &w : ni->wires) {
                    if (drives.count(w.first) || w.first == st.source)
                        continue;
                    bool ok = false;
                    WireId cursor = w.first;
                    for (size_t limit = ni->wires.size() + 1; limit > 0 && !ok; limit--) {
                        auto fnd = ni->wires.find(cursor);
                        ok = sinks.count(cursor) > 0;
                        if (fnd == ni->wires.end())
                            break;
                        if (fnd->second.pip == PipId())
                            break;
                        cursor = ctx->getPipSrcWire(fnd->second.pip);
                    }
                    // a pin wire of a cell of the net (a pin the cell port was renamed from, e.g. GateMate RAM
                    // ports after routing)
                    for (auto bp : ctx->getWireBelPins(w.first)) {
                        if (ok)
                            break;
                        CellInfo *bound = ctx->getBoundBelCell(bp.bel);
                        ok = bound != nullptr && users.count(bound->name);
                    }
                    for (PipId p : ctx->getPipsDownhill(w.first)) {
                        if (ok)
                            break;
                        WireId dst = ctx->getPipDstWire(p);
                        NetInfo *other = ctx->getBoundWireNet(dst);
                        ok = other != nullptr && other != ni && ctx->getNetinfoSourceWire(other) == dst;
                    }
                    if (!ok)
                        add(CAT_DANGLING, KIND_WIRE, ctx->nameOfWire(w.first), ctx->nameOfWire(w.first),
                            QString("of %1: the routing ends here without reaching a sink").arg(name));
                }
            }
        }
    }
    qint64 ms = clock.elapsed();

    // Keep the expanded categories (first fill: expand those with few entries)
    if (!firstFill && list->topLevelItemCount() == CAT_COUNT)
        for (int i = 0; i < CAT_COUNT; i++)
            expanded[i] = list->topLevelItem(i)->isExpanded();
    list->setUpdatesEnabled(false);
    list->clear();
    for (int i = 0; i < CAT_COUNT; i++) {
        QTreeWidgetItem *cat = new QTreeWidgetItem(list);
        cat->setText(0, QString("%1 (%2)").arg(categoryName[i]).arg(count[i]));
        cat->setData(0, KindRole, int(KIND_NONE));
        QFont f = cat->font(0);
        f.setBold(count[i] > 0);
        cat->setFont(0, f);
        if (count[i] > 0 && i != CAT_UNPLACED)
            cat->setForeground(0, QBrush(QColor(i == CAT_UNROUTED ? "#d08000" : "#e04040")));
        for (auto &e : entries[i]) {
            QTreeWidgetItem *item = new QTreeWidgetItem(cat);
            item->setText(0, e.text);
            item->setText(1, e.detail);
            item->setToolTip(0, e.text);
            item->setToolTip(1, e.detail);
            item->setData(0, KindRole, int(e.kind));
            item->setData(0, NameRole, e.name);
        }
        if (count[i] > int(entries[i].size())) {
            QTreeWidgetItem *more = new QTreeWidgetItem(cat);
            more->setText(0, QString("... and %1 more").arg(count[i] - int(entries[i].size())));
            more->setData(0, KindRole, int(KIND_NONE));
        }
        if (firstFill)
            expanded[i] = count[i] > 0 && count[i] <= 50;
        cat->setExpanded(expanded[i]);
    }
    list->setUpdatesEnabled(true);
    firstFill = false;

    QStringList parts;
    parts << QString("Unplaced: %1").arg(count[CAT_UNPLACED]) << QString("Unrouted nets: %1").arg(count[CAT_UNROUTED])
          << QString("Conflicts: %1").arg(count[CAT_CONFLICT]) << QString("Dangling: %1").arg(count[CAT_DANGLING])
          << QString("Invalid locations: %1").arg(count[CAT_INVALID]);
    bool clean = true;
    for (int i = 0; i < CAT_COUNT; i++)
        clean &= count[i] == 0;
    header->setText(QString("%1%2\n%3 cells, %4 nets checked in %5 ms")
                            .arg(clean ? "All placed and routed. " : "")
                            .arg(parts.join(", "))
                            .arg(cells)
                            .arg(nets)
                            .arg(ms));
}

void CheckDock::activate(QTreeWidgetItem *item)
{
    if (item == nullptr || ctx == nullptr)
        return;
    int kind = item->data(0, KindRole).toInt();
    std::string name = item->data(0, NameRole).toString().toStdString();
    switch (kind) {
    case KIND_CELL: {
        IdString id = ctx->id(name);
        if (ctx->cells.count(id))
            Q_EMIT focusCell(id);
        break;
    }
    case KIND_NET: {
        IdString id = ctx->id(name);
        if (ctx->nets.count(id))
            Q_EMIT focusNet(id);
        break;
    }
    case KIND_WIRE: {
        WireId wire = ctx->getWireByName(IdStringList::parse(ctx, name));
        if (wire != WireId())
            Q_EMIT focusWire(wire);
        break;
    }
    case KIND_PIP: {
        PipId pip = ctx->getPipByName(IdStringList::parse(ctx, name));
        if (pip != PipId())
            Q_EMIT focusPip(pip);
        break;
    }
    default:
        break;
    }
}

NEXTPNR_NAMESPACE_END
