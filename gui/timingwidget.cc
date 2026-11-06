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

#include "timingwidget.h"
#include <QApplication>
#include <QFormLayout>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <optional>
#include <set>

NEXTPNR_NAMESPACE_BEGIN

namespace {
const delay_t NO_SLACK = std::numeric_limits<delay_t>::max();

QString portName(const Context *ctx, const std::pair<IdString, IdString> &port)
{
    return QString("%1.%2").arg(port.first.c_str(ctx), port.second.c_str(ctx));
}
} // namespace

TimingWidget::TimingWidget(QWidget *parent) : QWidget(parent), ctx(nullptr), dirty(true)
{
    launchCombo = new QComboBox();
    launchCombo->addItem("All", QString());
    latchCombo = new QComboBox();
    latchCombo->addItem("All", QString());

    sortCombo = new QComboBox();
    sortCombo->addItem("Setup slack", SETUP_SLACK);
    sortCombo->addItem("Hold slack", HOLD_SLACK);
    sortCombo->addItem("Delay", DELAY);

    countSpin = new QSpinBox();
    countSpin->setRange(1, 100000);
    countSpin->setValue(32);

    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow("Launch clock", launchCombo);
    form->addRow("Latch clock", latchCombo);
    form->addRow("Sort by", sortCombo);
    form->addRow("Paths", countSpin);

    pathList = new QTreeWidget();
    pathList->setObjectName("pathList");
    pathList->setColumnCount(6);
    pathList->setHeaderLabels({"Setup [ns]", "Hold [ns]", "Delay [ns]", "From", "To", "Clock"});
    pathList->setRootIsDecorated(false);
    pathList->setUniformRowHeights(true);
    pathList->setAlternatingRowColors(true);
    pathList->setSelectionMode(QAbstractItemView::SingleSelection);

    QWidget *topWidget = new QWidget();
    QVBoxLayout *vbox1 = new QVBoxLayout();
    topWidget->setLayout(vbox1);
    vbox1->setSpacing(5);
    vbox1->setContentsMargins(0, 0, 0, 0);
    vbox1->addLayout(form);
    vbox1->addWidget(pathList);

    summary = new QLabel();
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);

    segmentList = new QTreeWidget();
    segmentList->setColumnCount(6);
    segmentList->setHeaderLabels({"Type", "Delay [ns]", "Total [ns]", "From", "To", "Net"});
    segmentList->setRootIsDecorated(false);
    segmentList->setUniformRowHeights(true);
    segmentList->setAlternatingRowColors(true);
    segmentList->setSelectionMode(QAbstractItemView::SingleSelection);

    QWidget *btmWidget = new QWidget();
    QVBoxLayout *vbox2 = new QVBoxLayout();
    btmWidget->setLayout(vbox2);
    vbox2->setSpacing(5);
    vbox2->setContentsMargins(0, 0, 0, 0);
    vbox2->addWidget(summary);
    vbox2->addWidget(segmentList);

    QSplitter *splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName("splitter_paths");
    splitter->addWidget(topWidget);
    splitter->addWidget(btmWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(splitter);
    setLayout(mainLayout);

    connect(launchCombo, &QComboBox::currentIndexChanged, this, [this] { updatePaths(); });
    connect(latchCombo, &QComboBox::currentIndexChanged, this, [this] { updatePaths(); });
    connect(sortCombo, &QComboBox::currentIndexChanged, this, [this] { updatePaths(); });
    connect(countSpin, &QSpinBox::valueChanged, this, [this] { updatePaths(); });
    connect(pathList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        showPath(item ? pathList->indexOfTopLevelItem(item) : -1);
    });
    connect(pathList, &QTreeWidget::itemDoubleClicked, this, [this] { Q_EMIT zoomSelected(); });
    connect(segmentList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        int index = pathList->indexOfTopLevelItem(pathList->currentItem());
        if (!item || index < 0)
            return;
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        auto &segment = paths.at(index).path.segments.at(segmentList->indexOfTopLevelItem(item));
        Q_EMIT selected(getSegmentDecals(segment), false);
    });
    connect(segmentList, &QTreeWidget::itemDoubleClicked, this, [this] { Q_EMIT zoomSelected(); });
}

TimingWidget::~TimingWidget() {}

void TimingWidget::newContext(Context *ctx)
{
    this->ctx = ctx;
    designChanged();
}

void TimingWidget::designChanged()
{
    dirty = true;
    analyser.reset();
    endpoints.clear();
    if (isVisible())
        analyse();
}

void TimingWidget::showEvent(QShowEvent *event)
{
    if (dirty)
        analyse();
    QWidget::showEvent(event);
}

void TimingWidget::analyse()
{
    dirty = false;
    if (!ctx)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    note.clear();
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        try {
            analyser = std::make_unique<TimingAnalyser>(ctx);
            analyser->setup_only = false;
            analyser->with_clock_skew = true;
            // the CP line cells that routing adds have arcs from every input to every output, which make loops
            analyser->ignore_loops = true;
            analyser->setup();
            endpoints = analyser->get_endpoint_timings();
            if (analyser->have_loops)
                note = "Combinational loops ignored.";
        } catch (std::exception &e) {
            analyser.reset();
            endpoints.clear();
            error = QString("Timing analysis failed: %1").arg(e.what());
        }
    }

    // The clocks to filter for, on both sides all of them and async (so that every combination can be
    // chosen), keeping the chosen ones if they are still there
    std::set<std::string> clocks{"async"};
    for (auto &ep : endpoints) {
        clocks.insert(clockNet(ep.clock_pair.start).toStdString());
        clocks.insert(clockNet(ep.clock_pair.end).toStdString());
    }
    for (QComboBox *combo : {launchCombo, latchCombo}) {
        QString current = combo->currentData().toString();
        combo->blockSignals(true);
        combo->clear();
        combo->addItem("All", QString());
        for (auto &clock : clocks)
            combo->addItem(QString::fromStdString(clock), QString::fromStdString(clock));
        combo->setCurrentIndex(std::max(0, combo->findData(current)));
        combo->blockSignals(false);
    }

    updatePaths();
    if (!error.isEmpty())
        summary->setText(error);
    QApplication::restoreOverrideCursor();
}

void TimingWidget::updatePaths()
{
    // The selected path stays selected (by its endpoint) if it is still listed
    int current = pathList->indexOfTopLevelItem(pathList->currentItem());
    std::optional<CellPortKey> keep;
    if (current >= 0 && current < int(paths.size()))
        keep = paths.at(current).timing.endpoint;

    paths.clear();
    pathList->clear();
    segmentList->clear();
    summary->clear();
    if (!analyser)
        return;

    QString launch = launchCombo->currentData().toString(), latch = latchCombo->currentData().toString();
    SortBy sortBy = SortBy(sortCombo->currentData().toInt());

    std::vector<const TimingAnalyser::EndpointTiming *> selected;
    for (auto &ep : endpoints) {
        if ((!launch.isEmpty() && clockNet(ep.clock_pair.start) != launch) ||
            (!latch.isEmpty() && clockNet(ep.clock_pair.end) != latch))
            continue;
        if (sortBy == HOLD_SLACK && ep.hold_slack == NO_SLACK)
            continue;
        selected.push_back(&ep);
    }
    auto less = [sortBy](const TimingAnalyser::EndpointTiming *a, const TimingAnalyser::EndpointTiming *b) {
        switch (sortBy) {
        case HOLD_SLACK:
            return a->hold_slack < b->hold_slack;
        case DELAY:
            return a->delay > b->delay;
        default:
            return a->setup_slack < b->setup_slack;
        }
    };
    size_t count = std::min(selected.size(), size_t(countSpin->value()));
    std::partial_sort(selected.begin(), selected.begin() + count, selected.end(), less);

    QList<QTreeWidgetItem *> items;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (size_t i = 0; i < count; i++) {
            const auto &ep = *selected.at(i);
            // the hold slack is that of the shortest path, the others of the longest
            paths.push_back(Path{ep, analyser->get_endpoint_path(ep, sortBy != HOLD_SLACK)});

            QString from;
            for (auto &segment : paths.back().path.segments)
                if (segment.type == CriticalPath::Segment::Type::CLK_TO_Q ||
                    segment.type == CriticalPath::Segment::Type::SOURCE) {
                    from = portName(ctx, segment.from);
                    break;
                }
            QString clocks = clockName(ep.clock_pair.start);
            if (ep.clock_pair.end.clock != ep.clock_pair.start.clock || ep.clock_pair.end.edge != ep.clock_pair.start.edge)
                clocks += " -> " + clockName(ep.clock_pair.end);

            auto item = new QTreeWidgetItem({formatDelay(ep.setup_slack), formatDelay(ep.hold_slack),
                                             formatDelay(ep.delay), from,
                                             portName(ctx, {ep.endpoint.cell, ep.endpoint.port}), clocks});
            for (int column = 0; column < 3; column++)
                item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            if (ep.setup_slack < 0)
                item->setForeground(0, Qt::red);
            if (ep.hold_slack < 0)
                item->setForeground(1, Qt::red);
            items.append(item);
        }
    }
    pathList->addTopLevelItems(items);
    for (int column = 0; column < 3; column++)
        pathList->resizeColumnToContents(column);
    if (keep) {
        auto found = std::find_if(paths.begin(), paths.end(),
                                  [&](const Path &path) { return path.timing.endpoint == *keep; });
        if (found != paths.end()) {
            pathList->setCurrentItem(pathList->topLevelItem(found - paths.begin()));
            return; // with its details as summary
        }
        Q_EMIT TimingWidget::selected({}, false);
    }

    if (selected.empty())
        summary->setText(endpoints.empty() ? "No timing paths." : "No paths between these clocks.");
    else
        summary->setText(QString("%1 of %2 paths. Select one for its details. %3")
                                 .arg(count)
                                 .arg(selected.size())
                                 .arg(note));
}

void TimingWidget::showPath(int index)
{
    segmentList->clear();
    if (index < 0 || index >= int(paths.size())) {
        summary->clear();
        return;
    }
    const auto &path = paths.at(index);

    QList<QTreeWidgetItem *> items;
    delay_t total = 0, logic = 0, routing = 0;
    for (auto &segment : path.path.segments) {
        total += segment.delay;
        bool is_routing = segment.type == CriticalPath::Segment::Type::ROUTING ||
                          segment.type == CriticalPath::Segment::Type::CLK_TO_CLK ||
                          segment.type == CriticalPath::Segment::Type::CLK_SKEW;
        (is_routing ? routing : logic) += segment.delay;
        auto item = new QTreeWidgetItem(
                {QString::fromStdString(CriticalPath::Segment::type_to_str(segment.type)), formatDelay(segment.delay),
                 formatDelay(total), portName(ctx, segment.from), portName(ctx, segment.to),
                 segment.net != IdString() ? QString(segment.net.c_str(ctx)) : QString()});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        items.append(item);
    }
    segmentList->addTopLevelItems(items);
    for (int column = 0; column < 3; column++)
        segmentList->resizeColumnToContents(column);

    const auto &ep = path.timing;
    summary->setText(QString("<b>%1</b> -> <b>%2</b><br>"
                             "Clock %3 -> %4, required %5 ns<br>"
                             "Setup slack %6 ns, hold slack %7 ns<br>"
                             "%8 path: %9 ns logic, %10 ns routing")
                             .arg(pathList->topLevelItem(index)->text(3).toHtmlEscaped(),
                                  pathList->topLevelItem(index)->text(4).toHtmlEscaped(),
                                  clockName(ep.clock_pair.start).toHtmlEscaped(),
                                  clockName(ep.clock_pair.end).toHtmlEscaped(), formatDelay(path.path.max_delay),
                                  formatDelay(ep.setup_slack), formatDelay(ep.hold_slack),
                                  SortBy(sortCombo->currentData().toInt()) == HOLD_SLACK ? "Shortest" : "Longest",
                                  formatDelay(logic), formatDelay(routing)));

    std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    Q_EMIT selected(getPathDecals(path.path), false);
}

std::vector<DecalXY> TimingWidget::getPathDecals(const CriticalPath &path)
{
    std::vector<DecalXY> decals;
    for (auto &segment : path.segments) {
        auto segment_decals = getSegmentDecals(segment);
        decals.insert(decals.end(), segment_decals.begin(), segment_decals.end());
    }
    return decals;
}

std::vector<DecalXY> TimingWidget::getSegmentDecals(const CriticalPath::Segment &segment)
{
    std::vector<DecalXY> decals;
    // The cells
    for (IdString cell_name : {segment.from.first, segment.to.first}) {
        auto cell = ctx->cells.find(cell_name);
        if (cell != ctx->cells.end() && cell->second->bel != BelId())
            decals.push_back(ctx->getBelDecal(cell->second->bel));
    }
    if (segment.type != CriticalPath::Segment::Type::ROUTING || !ctx->nets.count(segment.net) ||
        !ctx->cells.count(segment.to.first))
        return decals;

    // The route from the driver to this sink: from the sink wire back over the pips that drive each
    // wire. Only the pips: a wire's decal can show the whole node, with the branches of the net to
    // other sinks.
    const NetInfo *net = ctx->nets.at(segment.net).get();
    PortRef sink{ctx->cells.at(segment.to.first).get(), segment.to.second};
    for (size_t phys_idx = 0; phys_idx < ctx->getNetinfoSinkWireCount(net, sink); phys_idx++) {
        WireId wire = ctx->getNetinfoSinkWire(net, sink, phys_idx);
        for (size_t steps = 0; wire != WireId() && steps < net->wires.size(); steps++) {
            auto it = net->wires.find(wire);
            if (it == net->wires.end())
                break;
            PipId pip = it->second.pip;
            if (pip == PipId())
                break;
            decals.push_back(ctx->getPipDecal(pip));
            wire = ctx->getPipSrcWire(pip);
        }
    }
    return decals;
}

QString TimingWidget::formatDelay(delay_t delay)
{
    if (delay == NO_SLACK)
        return "-";
    return QString::number(ctx->getDelayNS(delay), 'f', 3);
}

QString TimingWidget::clockNet(const ClockEvent &clock)
{
    if (clock.clock == IdString())
        return "async";
    return clock.clock.c_str(ctx);
}

QString TimingWidget::clockName(const ClockEvent &clock)
{
    if (clock.clock == IdString())
        return "async";
    return QString("%1 %2").arg(clock.edge == FALLING_EDGE ? "negedge" : "posedge", clock.clock.c_str(ctx));
}

NEXTPNR_NAMESPACE_END
