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

#include "timingdock.h"
#include <QCheckBox>
#include <QElapsedTimer>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSplitter>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <sstream>
#include "editcommands.h"
#include "log.h"
#include "netstatus.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
const int IndexRole = Qt::UserRole, KindRole = Qt::UserRole + 1, NameRole = Qt::UserRole + 2;
const int KIND_CELL = 1, KIND_NET = 2;

QString eventName(Context *ctx, const ClockEvent &e)
{
    if (e.clock == IdString() || e.clock == ctx->id("$async$"))
        return "<async>";
    return QString("%1 %2").arg(e.edge == FALLING_EDGE ? "negedge" : "posedge").arg(e.clock.c_str(ctx));
}
} // namespace

TimingDock::TimingDock(EditStack *es, QWidget *parent) : QDockWidget("Timing", parent), es(es)
{
    setObjectName("TimingDock");
    QWidget *w = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);

    QHBoxLayout *top = new QHBoxLayout();
    btnRun = new QPushButton("Run timing analysis", w);
    btnRun->setToolTip("Analyse the design as it is (T): unplaced cells are left out, unrouted arcs count with "
                       "their estimated delay");
    highlight = new QCheckBox("Highlight critical path", w);
    highlight->setObjectName("timingHighlight");
    highlight->setToolTip("Draw the critical path of the selected clock in the device view (red)");
    top->addWidget(btnRun);
    top->addWidget(highlight);
    top->addStretch(1);
    layout->addLayout(top);
    connect(btnRun, &QPushButton::clicked, this, &TimingDock::runTiming);
    connect(highlight, &QCheckBox::toggled, this, &TimingDock::emitHighlight);

    header = new QLabel("Not analysed yet", w);
    header->setObjectName("timingHeader");
    header->setWordWrap(true);
    header->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(header);

    QSplitter *split = new QSplitter(Qt::Vertical, w);
    layout->addWidget(split, 1);

    clocks = new QTreeWidget(split);
    clocks->setObjectName("timingClocks");
    clocks->setColumnCount(4);
    clocks->setHeaderLabels({"Clock / path", "Fmax MHz", "Target MHz", "Delay ns"});
    clocks->setRootIsDecorated(false);
    clocks->setUniformRowHeights(true);
    clocks->setToolTip("Select a clock (or cross-domain path) to show its critical path below");
    connect(clocks, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        showPath(item != nullptr ? item->data(0, IndexRole).toInt() : -1);
    });
    connect(clocks, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        showPath(item != nullptr ? item->data(0, IndexRole).toInt() : -1);
    });

    path = new QTreeWidget(split);
    path->setObjectName("timingPath");
    path->setColumnCount(5);
    path->setHeaderLabels({"Type", "Delay ns", "Total ns", "Cell / net", "From -> to"});
    path->setRootIsDecorated(false);
    path->setUniformRowHeights(true);
    path->setToolTip("The critical path, from the start point. Double-click: select the net (routing) or the cell "
                     "and zoom to it. est.: the arc is not routed, its delay is estimated");
    connect(path, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        if (item == nullptr || ctx == nullptr)
            return;
        IdString name = ctx->id(item->data(0, NameRole).toString().toStdString());
        if (item->data(0, KindRole).toInt() == KIND_NET)
            Q_EMIT focusNet(name);
        else if (item->data(0, KindRole).toInt() == KIND_CELL)
            Q_EMIT focusCell(name);
    });
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);

    setWidget(w);
}

void TimingDock::setContext(Context *c)
{
    ctx = c;
    paths.clear();
    estimated.clear();
    current = -1;
    clocks->clear();
    path->clear();
    header->setText("Not analysed yet");
    emitHighlight();
}

void TimingDock::designEdited()
{
    if (paths.empty() && summary.isEmpty())
        return;
    if (!stale) {
        stale = true;
        header->setText(summary + "\n(the design changed since: run the analysis again)");
    }
}

void TimingDock::runTiming()
{
    if (ctx == nullptr)
        return;
    show();
    raise();
    if (es != nullptr && es->busy()) {
        header->setText("A task is running: run the timing analysis when it is done");
        return;
    }
    QElapsedTimer clock;
    clock.start();
    TimingResult result;
    int unplaced = 0, cells = 0, unrouted = 0;
    QString error;
    paths.clear();
    estimated.clear();
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (auto &c : ctx->cells) {
            cells++;
            unplaced += c.second->bel == BelId();
        }
        // Catch the error message (log_error) for the header
        std::ostringstream errors;
        log_streams.push_back(std::make_pair(&errors, LogLevel::ERROR_MSG));
        try {
            TimingAnalyser tmg(ctx);
            tmg.setup_only = false;
            tmg.with_clock_skew = true;
            tmg.setup(false, false, true);
            result = tmg.get_timing_result();
        } catch (const log_execution_error_exception &) {
            std::string msg = errors.str();
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == ' '))
                msg.pop_back();
            error = "the timing analysis failed: " + QString::fromStdString(msg.empty() ? "see the log" : msg);
        }
        for (auto it = log_streams.begin(); it != log_streams.end(); ++it)
            if (it->first == &errors) {
                log_streams.erase(it);
                break;
            }
        if (error.isEmpty()) {
            for (auto &cp : result.clock_paths)
                paths.push_back(cp.second);
            for (auto &cp : result.xclock_paths)
                paths.push_back(cp);
            // Which routing segments are estimated (their arc is not routed)
            dict<IdString, NetStatus::Status> status;
            for (auto &p : paths) {
                std::vector<bool> est;
                for (auto &seg : p.segments) {
                    bool e = false;
                    if (seg.type == CriticalPath::Segment::Type::ROUTING && ctx->nets.count(seg.net)) {
                        if (!status.count(seg.net))
                            status.emplace(seg.net, NetStatus::compute(ctx, ctx->nets.at(seg.net).get()));
                        e = true;
                        for (auto &arc : status.at(seg.net).arcs)
                            if (arc.cell == seg.to.first && arc.port == seg.to.second &&
                                arc.state == NetStatus::ArcState::ROUTED)
                                e = false;
                        unrouted += e;
                    }
                    est.push_back(e);
                }
                estimated.push_back(est);
            }
        }
    }
    qint64 ms = clock.elapsed();

    clocks->clear();
    path->clear();
    current = -1;
    stale = false;
    if (!error.isEmpty()) {
        summary = "Timing: " + error;
        header->setText(summary);
        emitHighlight();
        return;
    }
    // One row per clock (Fmax), then the cross-domain paths
    size_t idx = 0;
    QStringList fmaxText;
    for (auto &cp : result.clock_paths) {
        QTreeWidgetItem *item = new QTreeWidgetItem(clocks);
        item->setText(0, cp.first.c_str(ctx));
        delay_t total = 0;
        for (auto &seg : cp.second.segments)
            total += seg.delay;
        if (result.clock_fmax.count(cp.first)) {
            const ClockFmax &f = result.clock_fmax.at(cp.first);
            item->setText(1, QString::number(f.achieved, 'f', 2));
            item->setText(2, QString::number(f.constraint, 'f', 2));
            if (f.achieved < f.constraint)
                item->setForeground(1, QBrush(QColor("#c00000")));
            fmaxText << QString("%1 %2 MHz").arg(cp.first.c_str(ctx)).arg(f.achieved, 0, 'f', 2);
        }
        item->setText(3, QString::number(ctx->getDelayNS(total), 'f', 3));
        item->setData(0, IndexRole, int(idx++));
    }
    for (auto &cp : result.xclock_paths) {
        QTreeWidgetItem *item = new QTreeWidgetItem(clocks);
        item->setText(0, QString("%1 -> %2")
                                 .arg(eventName(ctx, cp.clock_pair.start))
                                 .arg(eventName(ctx, cp.clock_pair.end)));
        delay_t total = 0;
        for (auto &seg : cp.segments)
            total += seg.delay;
        item->setText(3, QString::number(ctx->getDelayNS(total), 'f', 3));
        item->setData(0, IndexRole, int(idx++));
    }
    for (int i = 0; i < 4; i++)
        clocks->resizeColumnToContents(i);

    QStringList notes;
    if (paths.empty())
        notes << "no timing paths (no clocked or constrained logic among the placed cells)";
    else
        notes << (fmaxText.isEmpty() ? QString("no clock with an Fmax") : "Fmax: " + fmaxText.join(", "));
    if (unplaced > 0)
        notes << QString("%1 of %2 cells unplaced: their arcs are left out (timing needs all cells placed)")
                         .arg(unplaced)
                         .arg(cells);
    if (unrouted > 0)
        notes << QString("%1 unrouted arcs on the paths shown: estimated delays (est.)").arg(unrouted);
    notes << QString("analysed in %1 ms").arg(ms);
    summary = notes.join("\n");
    header->setText(summary);
    if (clocks->topLevelItemCount() > 0)
        clocks->setCurrentItem(clocks->topLevelItem(0));
    else
        emitHighlight();
}

void TimingDock::showPath(int index)
{
    path->clear();
    current = index >= 0 && index < int(paths.size()) ? index : -1;
    if (current >= 0 && ctx != nullptr) {
        const CriticalPath &p = paths.at(current);
        delay_t total = 0;
        for (size_t i = 0; i < p.segments.size(); i++) {
            const auto &seg = p.segments.at(i);
            total += seg.delay;
            bool est = estimated.at(current).at(i);
            QTreeWidgetItem *item = new QTreeWidgetItem(path);
            item->setText(0, QString::fromStdString(CriticalPath::Segment::type_to_str(seg.type)));
            bool routing = seg.type == CriticalPath::Segment::Type::ROUTING;
            IdString name = routing ? seg.net
                            : (seg.type == CriticalPath::Segment::Type::SETUP ||
                               seg.type == CriticalPath::Segment::Type::HOLD)
                                    ? seg.to.first
                                    : seg.from.first;
            item->setText(3, name.c_str(ctx));
            item->setToolTip(3, name.c_str(ctx));
            QString ft = QString("%1.%2 -> %3.%4")
                                 .arg(seg.from.first.c_str(ctx))
                                 .arg(seg.from.second.c_str(ctx))
                                 .arg(seg.to.first.c_str(ctx))
                                 .arg(seg.to.second.c_str(ctx));
            if (!routing)
                ft = QString("%1 -> %2").arg(seg.from.second.c_str(ctx)).arg(seg.to.second.c_str(ctx));
            item->setText(4, ft);
            item->setToolTip(4, ft);
            item->setText(1, QString::number(ctx->getDelayNS(seg.delay), 'f', 3) + (est ? " est." : ""));
            item->setText(2, QString::number(ctx->getDelayNS(total), 'f', 3));
            if (est)
                item->setForeground(1, QBrush(QColor("#c07000")));
            item->setData(0, KindRole, routing ? KIND_NET : KIND_CELL);
            item->setData(0, NameRole, QString(name.c_str(ctx)));
        }
        for (int i = 0; i < 5; i++)
            path->resizeColumnToContents(i);
    }
    emitHighlight();
}

void TimingDock::emitHighlight()
{
    std::vector<CriticalPath::Segment> segs;
    if (highlight->isChecked() && current >= 0 && current < int(paths.size()))
        segs = paths.at(current).segments;
    Q_EMIT criticalPathChanged(segs);
}

NEXTPNR_NAMESPACE_END
