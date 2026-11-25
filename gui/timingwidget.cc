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
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include "log.h"

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
    launchCombo->setObjectName("pathLaunch");
    launchCombo->addItem("All", QString());
    latchCombo = new QComboBox();
    latchCombo->setObjectName("pathLatch");
    latchCombo->addItem("All", QString());

    sortCombo = new QComboBox();
    sortCombo->setObjectName("pathSort");
    sortCombo->addItem("Setup slack", SETUP_SLACK);
    sortCombo->addItem("Hold slack", HOLD_SLACK);
    sortCombo->addItem("Delay", DELAY);

    countSpin = new QSpinBox();
    countSpin->setRange(1, 100000);
    countSpin->setValue(32);

    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow("Launch clock edge", launchCombo);
    form->addRow("Latch clock edge", latchCombo);
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
    summary->setObjectName("pathSummary");
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);

    segmentList = new QTreeWidget();
    segmentList->setObjectName("pathSegments");
    segmentList->setColumnCount(6);
    segmentList->setHeaderLabels({"Type", "Delay [ns]", "Total [ns]", "From", "To", "Net"});
    segmentList->setRootIsDecorated(false);
    segmentList->setUniformRowHeights(true);
    segmentList->setAlternatingRowColors(true);
    segmentList->setSelectionMode(QAbstractItemView::SingleSelection);

    // How the slacks come about: the required time from the clocks and the exceptions, the arrival time of the
    // data, and the constraints on the path
    slackList = new QTreeWidget();
    slackList->setObjectName("pathSlack");
    slackList->setColumnCount(3);
    slackList->setHeaderLabels({"Slack", "[ns]", "From"});
    slackList->setUniformRowHeights(true);
    slackList->setSelectionMode(QAbstractItemView::SingleSelection);

    QSplitter *details = new QSplitter(Qt::Vertical);
    details->setObjectName("splitter_path_details");
    details->addWidget(slackList);
    details->addWidget(segmentList);
    details->setStretchFactor(0, 3);
    details->setStretchFactor(1, 2);

    QWidget *btmWidget = new QWidget();
    QVBoxLayout *vbox2 = new QVBoxLayout();
    btmWidget->setLayout(vbox2);
    vbox2->setSpacing(5);
    vbox2->setContentsMargins(0, 0, 0, 0);
    vbox2->addWidget(summary);
    vbox2->addWidget(details, 1);

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
        Q_EMIT selected(TimingText::segmentDecals(ctx, segment), false);
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
    analyser.reset();
    endpoints.clear();
    // (the cells of an unpacked design have no timing: the analysis would end in an error)
    if (!ctx->settings.count(ctx->id("pack"))) {
        error = "The design is not packed: the paths are listed once it is.";
    } else {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        // Catch the error message (log_error) for the summary
        std::ostringstream errors;
        log_streams.push_back(std::make_pair(&errors, LogLevel::ERROR_MSG));
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
        } catch (const log_execution_error_exception &) {
            analyser.reset();
            endpoints.clear();
            QString msg = QString::fromStdString(errors.str()).trimmed();
            error = QString("Timing analysis failed: %1").arg(msg.isEmpty() ? "see the log" : msg);
        } catch (std::exception &e) {
            analyser.reset();
            endpoints.clear();
            error = QString("Timing analysis failed: %1").arg(e.what());
        }
        for (auto it = log_streams.begin(); it != log_streams.end(); ++it)
            if (it->first == &errors) {
                log_streams.erase(it);
                break;
            }
    }

    // The clock edges to filter for, on both sides all of them and async (so that every combination can be
    // chosen), keeping the chosen ones if they are still there. Ordered by clock, rising edge first.
    std::map<std::pair<std::string, int>, QString> clocks;
    auto addClock = [&](const ClockEvent &clock) {
        clocks.emplace(std::make_pair(clockNet(clock).toStdString(), clock.clock == IdString() ? 0 : int(clock.edge)),
                       clockName(clock));
    };
    addClock(ClockEvent{IdString(), RISING_EDGE});
    for (auto &ep : endpoints) {
        addClock(ep.clock_pair.start);
        addClock(ep.clock_pair.end);
    }
    for (QComboBox *combo : {launchCombo, latchCombo}) {
        QString current = combo->currentData().toString();
        combo->blockSignals(true);
        combo->clear();
        combo->addItem("All", QString());
        for (auto &clock : clocks)
            combo->addItem(clock.second, clock.second);
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
    slackList->clear();
    summary->clear();
    if (!analyser)
        return;

    QString launch = launchCombo->currentData().toString(), latch = latchCombo->currentData().toString();
    SortBy sortBy = SortBy(sortCombo->currentData().toInt());

    std::vector<const TimingAnalyser::EndpointTiming *> selected;
    for (auto &ep : endpoints) {
        if ((!launch.isEmpty() && clockName(ep.clock_pair.start) != launch) ||
            (!latch.isEmpty() && clockName(ep.clock_pair.end) != latch))
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
        summary->setText(endpoints.empty() ? "No timing paths." : "No paths between these clock edges.");
    else
        summary->setText(QString("%1 of %2 paths. Select one for its details. %3")
                                 .arg(count)
                                 .arg(selected.size())
                                 .arg(note));
}

void TimingWidget::showPath(int index)
{
    segmentList->clear();
    slackList->clear();
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

    showSlack(index);

    std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    Q_EMIT selected(TimingText::pathDecals(ctx, path.path), false);
}

void TimingWidget::showSlack(int index)
{
    const auto &path = paths.at(index);
    const auto &ep = path.timing;
    bool hold_sorted = SortBy(sortCombo->currentData().toInt()) == HOLD_SLACK;

    // The parts of a path: the data path (clock-to-q or source, logic, routing), the clock-to-clock delay and the
    // setup or hold time. (Its clock skew segment is left out: the clock delays at the two ends come from the
    // analysis, which counts them also where the report has no skew, as at an unclocked end.)
    struct Parts
    {
        delay_t data = 0, clock_to_clock = 0, check = 0;
        int logic = 0, routing = 0;
    };
    auto parts_of = [](const CriticalPath &p) {
        Parts parts;
        for (auto &segment : p.segments) {
            switch (segment.type) {
            case CriticalPath::Segment::Type::CLK_SKEW:
                break;
            case CriticalPath::Segment::Type::CLK_TO_CLK:
                parts.clock_to_clock += segment.delay;
                break;
            case CriticalPath::Segment::Type::SETUP:
                parts.check += segment.delay;
                break;
            case CriticalPath::Segment::Type::HOLD:
                // (the segment is the negated hold time)
                parts.check -= segment.delay;
                break;
            case CriticalPath::Segment::Type::LOGIC:
                parts.logic++;
                parts.data += segment.delay;
                break;
            case CriticalPath::Segment::Type::ROUTING:
                parts.routing++;
                parts.data += segment.delay;
                break;
            default:
                parts.data += segment.delay;
            }
        }
        return parts;
    };

    TimingAnalyser::PairConstraints pc;
    CriticalPath setup_path;
    std::optional<CriticalPath> hold_path;
    TimingAnalyser::PathClockDelays setup_clocks, hold_clocks;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        pc = analyser->get_pair_constraints(ep.domain_pair);
        setup_path = hold_sorted ? analyser->get_endpoint_path(ep, true) : path.path;
        setup_clocks = analyser->get_clock_delays(ep, setup_path);
        if (ep.hold_slack != NO_SLACK) {
            hold_path = hold_sorted ? path.path : analyser->get_endpoint_path(ep, false);
            hold_clocks = analyser->get_clock_delays(ep, *hold_path);
        }
    }

    auto signedDelay = [&](delay_t delay) {
        QString text = formatDelay(delay);
        return delay >= 0 ? "+" + text : text;
    };
    auto row = [&](QTreeWidgetItem *parent, const QString &name, const QString &value, const QString &from) {
        auto item = new QTreeWidgetItem(parent, {name, value, from});
        item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
        item->setToolTip(2, from);
        return item;
    };
    auto bold = [](QTreeWidgetItem *item) {
        QFont font = item->font(0);
        font.setBold(true);
        for (int column = 0; column < 2; column++)
            item->setFont(column, font);
        return item;
    };
    auto slackRow = [&](QTreeWidgetItem *parent, const QString &name, delay_t slack, const QString &from) {
        auto item = bold(row(parent, name, formatDelay(slack), from));
        if (slack < 0)
            item->setForeground(1, Qt::red);
        return item;
    };
    // The arrival time: the clock delay to the launching register and the data path
    auto arrivalRows = [&](QTreeWidgetItem *top, const Parts &p, const TimingAnalyser::PathClockDelays &clocks,
                           const char *which) {
        if (clocks.launch != 0)
            row(top, "Launch clock delay", signedDelay(clocks.launch), "clock network to the launching register");
        row(top, "Data path", signedDelay(p.data),
            QString("%1 path: clock-to-q, %2 logic and %3 routing segment(s) (listed below)")
                    .arg(which)
                    .arg(p.logic)
                    .arg(p.routing));
        delay_t arrival = clocks.launch + p.data;
        bold(row(top, "Arrival time", formatDelay(arrival), QString()));
        return arrival;
    };
    // What the itemised times leave of the analysed slack (none, unless the report differs from the analysis)
    auto restRow = [&](QTreeWidgetItem *top, delay_t rest) {
        if (std::abs(ctx->getDelayNS(rest)) >= 0.0005)
            row(top, "Not itemised", signedDelay(rest), "the analysis differs from the path report by this much");
    };
    QString endpoint = portName(ctx, {ep.endpoint.cell, ep.endpoint.port});
    const ClockEvent &launch = pc.clock_pair.start, &capture = pc.clock_pair.end;
    bool unclocked = launch.clock == IdString() || capture.clock == IdString();

    // Setup: required = the time the clocks give + the multicycle path + capture clock delay - clock to clock -
    // setup time, arrival = launch clock delay + data path, slack = required - arrival
    {
        Parts p = parts_of(setup_path);
        auto top = slackRow(nullptr, "Setup check", ep.setup_slack, "slack = required time - arrival time");
        QString edges;
        if (unclocked)
            edges = "unclocked start or end point: one period of the launch clock, or of the default target";
        else if (launch.clock != capture.clock)
            edges = "different clocks: one period of the launch clock";
        else if (launch.edge == capture.edge)
            edges = "one period of the clock";
        else
            edges = QString("from the %1 to the %2 of the clock")
                            .arg(launch.edge == RISING_EDGE ? "rising" : "falling",
                                 capture.edge == RISING_EDGE ? "rising edge" : "falling edge");
        row(top, "Capture edge", signedDelay(pc.clock_required), edges);
        if (pc.setup_exception >= 0)
            row(top, QString("Multicycle path: %1 cycles").arg(pc.setup_cycles), signedDelay(pc.setup_shift),
                TimingText::exceptionName(ctx, pc.setup_exception));
        if (setup_clocks.capture != 0)
            row(top, "Capture clock delay", signedDelay(setup_clocks.capture),
                "clock network to the capturing register");
        if (p.clock_to_clock != 0)
            row(top, "Clock to clock", signedDelay(-p.clock_to_clock), "delay between the related clocks");
        if (p.check != 0)
            row(top, "Setup time", signedDelay(-p.check), endpoint);
        delay_t required = pc.clock_required + pc.setup_shift + setup_clocks.capture - p.clock_to_clock - p.check;
        bold(row(top, "Required time", formatDelay(required), QString()));
        delay_t arrival = arrivalRows(top, p, setup_clocks, "longest");
        restRow(top, ep.setup_slack - (required - arrival));
        slackRow(top, "Slack", ep.setup_slack, "required time - arrival time");
        slackList->addTopLevelItem(top);
    }

    // Hold: required = the hold edge + capture clock delay + hold time - clock to clock, arrival as for setup
    // (shortest path), slack = arrival - required
    if (hold_path) {
        Parts p = parts_of(*hold_path);
        auto top = slackRow(nullptr, "Hold check", ep.hold_slack, "slack = arrival time - required time");
        QString edge;
        if (pc.hold_exception >= 0)
            edge = TimingText::exceptionName(ctx, pc.hold_exception);
        else if (pc.setup_exception >= 0)
            edge = "one period before the setup edge of the multicycle path (set_multicycle_path -hold moves it "
                   "back)";
        else if (pc.hold_edge != 0)
            edge = "the capture edge before the launch edge (the other edge of the clock)";
        else
            edge = "the launch edge";
        delay_t hold_edge = pc.hold_shift - pc.hold_edge;
        row(top, "Hold edge", signedDelay(hold_edge), edge);
        if (hold_clocks.capture != 0)
            row(top, "Capture clock delay", signedDelay(hold_clocks.capture),
                "clock network to the capturing register");
        if (p.clock_to_clock != 0)
            row(top, "Clock to clock", signedDelay(-p.clock_to_clock), "delay between the related clocks");
        if (p.check != 0)
            row(top, "Hold time", signedDelay(p.check), endpoint);
        delay_t required = hold_edge + hold_clocks.capture + p.check - p.clock_to_clock;
        bold(row(top, "Required time", formatDelay(required), "the earliest time the data may arrive"));
        delay_t arrival = arrivalRows(top, p, hold_clocks, "shortest");
        restRow(top, ep.hold_slack - (arrival - required));
        slackRow(top, "Slack", ep.hold_slack, "arrival time - required time");
        slackList->addTopLevelItem(top);
    } else {
        slackList->addTopLevelItem(row(nullptr, "Hold check", "-",
                                       "not checked: an unclocked start or end point, or unrelated clocks"));
    }

    // The constraints on the path: the clocks and the timing exceptions
    {
        auto top = row(nullptr, "Constraints", QString(), QString());
        // (short names in the first column, which sets the width of the table)
        row(top, "Launch clock", QString(),
            TimingText::clockName(ctx, launch) + ": " + TimingText::clockConstraint(ctx, launch.clock));
        if (capture.clock != launch.clock)
            row(top, "Capture clock", QString(),
                TimingText::clockName(ctx, capture) + ": " + TimingText::clockConstraint(ctx, capture.clock));
        for (int index : pc.exceptions) {
            const TimingException &exc = ctx->timing_exceptions.at(index);
            QString name = TimingText::exceptionName(ctx, index);
            if (name.startsWith("line "))
                name = name.mid(name.indexOf(':') + 2);
            row(top, QString("Line %1").arg(exc.line), QString(),
                TimingText::exceptionStatus(ctx, pc, index) + ": " + name);
        }
        if (pc.exceptions.empty())
            row(top, "Exceptions", QString(), "none (set_multicycle_path, set_false_path)");
        slackList->addTopLevelItem(top);
    }
    slackList->expandAll();
    for (int column = 0; column < 2; column++)
        slackList->resizeColumnToContents(column);
}

std::vector<DecalXY> TimingText::pathDecals(Context *ctx, const CriticalPath &path)
{
    std::vector<DecalXY> decals;
    for (auto &segment : path.segments) {
        auto segment_decals = segmentDecals(ctx, segment);
        decals.insert(decals.end(), segment_decals.begin(), segment_decals.end());
    }
    return decals;
}

std::vector<DecalXY> TimingText::segmentDecals(Context *ctx, const CriticalPath::Segment &segment)
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

QString TimingWidget::clockName(const ClockEvent &clock) { return TimingText::clockName(ctx, clock); }

QString TimingText::clockName(Context *ctx, const ClockEvent &clock)
{
    if (clock.clock == IdString())
        return "async";
    return QString("%1 %2").arg(clock.edge == FALLING_EDGE ? "negedge" : "posedge", clock.clock.c_str(ctx));
}

QString TimingText::clockConstraint(Context *ctx, IdString clock)
{
    if (clock == IdString())
        return "unclocked (async)";
    auto ns = [&](delay_t delay) { return QString::number(ctx->getDelayNS(delay), 'f', 3); };
    auto net = ctx->nets.find(clock);
    if (net == ctx->nets.end() || !net->second->clkconstr)
        return QString("no clock constraint, the default target of %1 MHz")
                .arg(ctx->setting<float>("target_freq") / 1e6, 0, 'f', 2);
    const ClockConstraint &cc = *net->second->clkconstr;
    QString name = cc.name != IdString() && cc.name != clock ? QString("'%1': ").arg(cc.name.c_str(ctx)) : QString();
    return QString("%1period %2 ns (%3 MHz), high %4 ns, low %5 ns")
            .arg(name, ns(cc.period.minDelay()))
            .arg(1000 / ctx->getDelayNS(cc.period.minDelay()), 0, 'f', 2)
            .arg(ns(cc.high.minDelay()), ns(cc.low.minDelay()));
}

QString TimingText::exceptionName(Context *ctx, int index)
{
    const TimingException &exc = ctx->timing_exceptions.at(index);
    QString source = QString::fromStdString(exc.source);
    if (source.isEmpty())
        source = exc.type == TimingException::FALSE_PATH ? "set_false_path"
                                                         : QString("set_multicycle_path %1").arg(exc.multiplier);
    return exc.line > 0 ? QString("line %1: %2").arg(exc.line).arg(source) : source;
}

QString TimingText::exceptionStatus(Context *ctx, const TimingAnalyser::PairConstraints &pc, int index)
{
    const TimingException &exc = ctx->timing_exceptions.at(index);
    if (exc.type == TimingException::FALSE_PATH)
        return "false path: not applied, the timing analysis does not support false paths yet";
    if (index == pc.setup_exception)
        return QString("applies to setup: %1 clock cycles").arg(pc.setup_cycles);
    if (index == pc.hold_exception)
        return QString("applies to hold: the hold check %1 cycle(s) back").arg(exc.multiplier);
    if (pc.clock_pair.start.clock == IdString() || pc.clock_pair.end.clock == IdString())
        return "no effect: the path starts or ends at an unclocked point";
    int winner = exc.hold ? pc.hold_exception : pc.setup_exception;
    if (winner >= 0)
        return QString("overridden by line %1").arg(ctx->timing_exceptions.at(winner).line);
    return "not applied";
}

NEXTPNR_NAMESPACE_END
