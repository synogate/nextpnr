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

#include "routedock.h"
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include "autoroute.h"
#include "editcommands.h"
#include "fpgaviewwidget.h"
#include "netstatus.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
const int NameRole = Qt::UserRole;
}

RouteDock::RouteDock(FPGAViewWidget *view, EditStack *es, QWidget *parent)
        : QDockWidget("Route candidates", parent), view(view), es(es)
{
    setObjectName("RouteDock");
    QWidget *w = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);

    header = new QLabel(w);
    header->setWordWrap(true);
    layout->addWidget(header);

    QSplitter *split = new QSplitter(Qt::Vertical, w);
    layout->addWidget(split, 1);

    cands = new QTreeWidget(split);
    cands->setObjectName("routeCandidates");
    cands->setColumnCount(4);
    cands->setHeaderLabels({"To wire", "Pip", "Delay ns", "State"});
    cands->setRootIsDecorated(false);
    cands->setUniformRowHeights(true);
    cands->setToolTip("The head wire and the pips downhill of it. Double-click / Enter binds the pip (as a click in "
                      "the device view) and moves the head");
    connect(cands, &QTreeWidget::itemActivated, this,
            [this](QTreeWidgetItem *item, int) { activateCandidate(cands->indexOfTopLevelItem(item)); });

    QWidget *mid = new QWidget(split);
    QVBoxLayout *midLayout = new QVBoxLayout(mid);
    midLayout->setContentsMargins(0, 0, 0, 0);
    midLayout->addWidget(new QLabel("Pips of this route session (click: continue from there)", mid));
    steps = new QListWidget(mid);
    steps->setObjectName("routeSteps");
    midLayout->addWidget(steps);
    connect(steps, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        Context *ctx = this->view->getContext();
        if (ctx == nullptr || item == nullptr)
            return;
        WireId wire = ctx->getWireByName(IdStringList::parse(ctx, item->data(NameRole).toString().toStdString()));
        if (wire != WireId())
            this->view->setRouteHead(wire);
    });

    QWidget *bottom = new QWidget(split);
    QVBoxLayout *botLayout = new QVBoxLayout(bottom);
    botLayout->setContentsMargins(0, 0, 0, 0);
    botLayout->addWidget(new QLabel("Arcs of the net (double-click: route from the head to the sink)", bottom));
    sinks = new QTreeWidget(bottom);
    sinks->setObjectName("routeSinks");
    sinks->setColumnCount(3);
    sinks->setHeaderLabels({"Sink", "Wire", "State"});
    sinks->setRootIsDecorated(false);
    sinks->setUniformRowHeights(true);
    botLayout->addWidget(sinks);
    auto sinkWire = [this](QTreeWidgetItem *item) {
        Context *ctx = this->view->getContext();
        if (ctx == nullptr || item == nullptr)
            return WireId();
        return ctx->getWireByName(IdStringList::parse(ctx, item->data(0, NameRole).toString().toStdString()));
    };
    connect(sinks, &QTreeWidget::itemActivated, this, [this, sinkWire](QTreeWidgetItem *item, int) {
        WireId wire = sinkWire(item);
        if (wire != WireId())
            this->view->routeToWire(wire);
    });
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setStretchFactor(2, 2);

    QHBoxLayout *toRow = new QHBoxLayout();
    target = new QLineEdit(w);
    target->setObjectName("routeTarget");
    target->setPlaceholderText("wire name, e.g. X10Y20/IM.P01.D0");
    btnTo = new QPushButton("Route to", w);
    btnTo->setToolTip("Bind the shortest free path from the head to this wire (Ctrl+click in the view)");
    toRow->addWidget(target, 1);
    toRow->addWidget(btnTo);
    layout->addLayout(toRow);
    connect(btnTo, &QPushButton::clicked, this, &RouteDock::routeToTyped);
    connect(target, &QLineEdit::returnPressed, this, &RouteDock::routeToTyped);

    QHBoxLayout *row = new QHBoxLayout();
    btnArc = new QPushButton("Route arc", w);
    btnArc->setToolTip("Route the selected arc from the closest wire of the net's routing");
    btnRest = new QPushButton("Route rest", w);
    btnRest->setToolTip("Route all unrouted arcs of the net (existing pips stay)");
    btnLock = new QPushButton("Lock", w);
    btnBack = new QPushButton("Back", w);
    btnBack->setToolTip("Take back the last step of this session (Backspace)");
    btnEnd = new QPushButton("End", w);
    btnEnd->setToolTip("End routing (Esc)");
    for (QPushButton *b : {btnArc, btnRest, btnLock, btnBack, btnEnd})
        row->addWidget(b);
    layout->addLayout(row);
    connect(btnArc, &QPushButton::clicked, this, [this, sinkWire] {
        IdString net = this->view->routingNet();
        WireId wire = sinkWire(sinks->currentItem());
        if (net == IdString())
            return;
        if (wire == WireId()) {
            this->es->notify("Route arc: select an arc in the list first");
            return;
        }
        runRouteArc(this->es, net, wire);
    });
    connect(btnRest, &QPushButton::clicked, this, [this] {
        IdString net = this->view->routingNet();
        if (net != IdString())
            runRouteNetRest(this->es, net);
    });
    connect(btnLock, &QPushButton::clicked, this, [this] {
        IdString net = this->view->routingNet();
        if (net != IdString())
            runLockRouting(this->es, net, btnLock->text().startsWith("Lock"));
    });
    connect(btnBack, &QPushButton::clicked, view, &FPGAViewWidget::routeBack);
    connect(btnEnd, &QPushButton::clicked, view, &FPGAViewWidget::cancelInteraction);

    setWidget(w);
    connect(view, &FPGAViewWidget::routeStateChanged, this, &RouteDock::scheduleRefresh);
    refresh();
}

void RouteDock::scheduleRefresh()
{
    if (refreshPending)
        return;
    refreshPending = true;
    QTimer::singleShot(0, this, [this] {
        refreshPending = false;
        refresh();
    });
}

void RouteDock::refresh()
{
    Context *ctx = view->getContext();
    IdString net = view->routingNet();
    bool routing = ctx != nullptr && net != IdString();
    cands->clear();
    steps->clear();
    sinks->clear();
    for (QWidget *wd : std::initializer_list<QWidget *>{cands, steps, sinks, target, btnTo, btnArc, btnRest, btnLock,
                                                        btnBack, btnEnd})
        wd->setEnabled(routing);
    if (!routing) {
        header->setText("Not routing. In edit mode (E), select a net and press R, or right-click a wire of a net > "
                        "Route net.");
        wasRouting = false;
        return;
    }
    if (!wasRouting) {
        show();
        raise();
    }
    wasRouting = true;

    WireId head = view->routingHead();
    const auto &rc = view->routeCandidates();
    int free = 0;
    for (auto &c : rc)
        free += c.cls == FPGAViewWidget::ROUTE_OK && !c.bound;
    header->setText(QString("Routing <b>%1</b> from <b>%2</b>: %3 of %4 pips free")
                            .arg(QString(net.c_str(ctx)).toHtmlEscaped())
                            .arg(QString(ctx->nameOfWire(head)).toHtmlEscaped())
                            .arg(free)
                            .arg(rc.size()));

    // Head first, then the free pips, the routed ones, the others
    QTreeWidgetItem *headItem = new QTreeWidgetItem(cands, {QString(ctx->nameOfWire(head)), "", "", "head"});
    QFont bold = headItem->font(0);
    bold.setBold(true);
    for (int i = 0; i < 4; i++)
        headItem->setFont(i, bold);
    std::vector<int> order(rc.size());
    for (size_t i = 0; i < rc.size(); i++)
        order[i] = int(i);
    auto rank = [&](const FPGAViewWidget::RouteCand &c) {
        return c.cls == FPGAViewWidget::ROUTE_OK ? (c.bound ? 1 : 0) : c.cls == FPGAViewWidget::ROUTE_OTHER ? 2 : 3;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return rank(rc[a]) < rank(rc[b]); });
    for (int i : order) {
        const auto &c = rc[i];
        QString state;
        QColor color;
        if (c.cls == FPGAViewWidget::ROUTE_OK && c.bound) {
            state = "routed (this net)";
            color = QColor("#2060c0");
        } else if (c.cls == FPGAViewWidget::ROUTE_OK) {
            state = "free";
            color = QColor("#108010");
        } else if (c.cls == FPGAViewWidget::ROUTE_OTHER) {
            state = QString::fromStdString(c.why);
            color = QColor("#c06000");
        } else {
            state = "unavailable: " + QString::fromStdString(c.why);
            color = QColor("#808080");
        }
        QTreeWidgetItem *item = new QTreeWidgetItem(
                cands, {QString(ctx->nameOfWire(c.dst)), QString(ctx->nameOfPip(c.pip)), QString::number(c.delay, 'f', 3),
                        state});
        item->setData(0, NameRole, QString(ctx->nameOfPip(c.pip)));
        for (int k = 0; k < 4; k++)
            item->setForeground(k, color);
        item->setToolTip(3, state);
    }
    for (int k = 0; k < 4; k++)
        cands->resizeColumnToContents(k);

    // Session pips (breadcrumbs)
    int n = 0;
    for (PipId pip : view->routeSessionPips()) {
        WireId dst = ctx->getPipDstWire(pip);
        QListWidgetItem *item = new QListWidgetItem(
                QString("%1. %2  ->  %3").arg(++n).arg(ctx->nameOfPip(pip)).arg(ctx->nameOfWire(dst)), steps);
        item->setData(NameRole, QString(ctx->nameOfWire(dst)));
        if (dst == head) {
            QFont f = item->font();
            f.setBold(true);
            item->setFont(f);
        }
    }

    // Arcs, not routed first
    bool locked = false;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        auto fnd = ctx->nets.find(net);
        if (fnd != ctx->nets.end()) {
            NetInfo *ni = fnd->second.get();
            locked = AutoRoute::isRoutingLocked(ni);
            NetStatus::Status st = NetStatus::compute(ctx, ni);
            std::vector<const NetStatus::Arc *> arcs;
            for (auto &arc : st.arcs)
                arcs.push_back(&arc);
            std::stable_sort(arcs.begin(), arcs.end(), [](const NetStatus::Arc *a, const NetStatus::Arc *b) {
                return (a->state == NetStatus::ArcState::ROUTED) < (b->state == NetStatus::ArcState::ROUTED);
            });
            const size_t max_arcs = 500;
            for (size_t i = 0; i < arcs.size() && i < max_arcs; i++) {
                const NetStatus::Arc *arc = arcs[i];
                QString state = NetStatus::stateName(arc->state);
                if (arc->blocked)
                    state += " (sink wire used by another net)";
                QString wire = arc->sink != WireId() ? QString(ctx->nameOfWire(arc->sink)) : QString();
                QTreeWidgetItem *item = new QTreeWidgetItem(
                        sinks, {QString("%1.%2").arg(arc->cell.c_str(ctx)).arg(arc->port.c_str(ctx)), wire, state});
                item->setData(0, NameRole, wire);
                if (arc->state == NetStatus::ArcState::ROUTED)
                    for (int k = 0; k < 3; k++)
                        item->setForeground(k, QColor("#808080"));
            }
        }
    }
    for (int k = 0; k < 3; k++)
        sinks->resizeColumnToContents(k);
    btnLock->setText(locked ? "Unlock" : "Lock");
    btnLock->setToolTip(locked ? "Unlock the routing of the net (STRONG)"
                               : "Lock the routing of the net (USER): route mode and rip-up refuse to change it");
}

void RouteDock::activateCandidate(int row)
{
    Context *ctx = view->getContext();
    QTreeWidgetItem *item = cands->topLevelItem(row);
    if (ctx == nullptr || item == nullptr)
        return;
    QString name = item->data(0, NameRole).toString();
    if (name.isEmpty())
        return; // the head row
    PipId pip = ctx->getPipByName(IdStringList::parse(ctx, name.toStdString()));
    if (pip != PipId())
        view->routeThroughPip(pip);
}

void RouteDock::routeToTyped()
{
    Context *ctx = view->getContext();
    QString name = target->text().trimmed();
    if (ctx == nullptr || name.isEmpty() || view->routingNet() == IdString())
        return;
    WireId wire;
    try {
        wire = ctx->getWireByName(IdStringList::parse(ctx, name.toStdString()));
    } catch (...) {
        wire = WireId();
    }
    if (wire == WireId()) {
        es->notify("Route to: no wire named " + name);
        return;
    }
    view->routeToWire(wire);
}

NEXTPNR_NAMESPACE_END
