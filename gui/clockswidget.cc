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

#include "clockswidget.h"
#include <QApplication>
#include <QHeaderView>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <map>
#include <sstream>
#include "log.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
const delay_t NO_SLACK = std::numeric_limits<delay_t>::max();

enum ClockColumn
{
    COL_CLOCK,
    COL_TARGET,
    COL_FMAX,
    COL_SETUP,
    COL_TNS,
    COL_HOLD,
    COL_ENDPOINTS,
    COL_CELLS,
    COL_EDGES,
    COL_SKEW,
    COL_DRIVER,
    COL_COUNT
};
} // namespace

ClocksWidget::ClocksWidget(QWidget *parent) : QWidget(parent), ctx(nullptr), dirty(true)
{
    header = new QLabel();
    header->setObjectName("clockHeader");
    header->setWordWrap(true);

    clockList = new QTreeWidget();
    clockList->setObjectName("clockList");
    clockList->setColumnCount(COL_COUNT);
    clockList->setHeaderLabels({"Clock", "Target [MHz]", "Fmax [MHz]", "Setup WNS [ns]", "TNS [ns]", "Hold WNS [ns]",
                                "Endpoints", "Clocked cells", "Edges", "Skew [ns]", "Driver"});
    clockList->setRootIsDecorated(false);
    clockList->setUniformRowHeights(true);
    clockList->setAlternatingRowColors(true);
    clockList->setSelectionMode(QAbstractItemView::SingleSelection);
    clockList->headerItem()->setToolTip(COL_TARGET, "The constraint of the clock net, else the default target (in "
                                                    "italics)");
    clockList->headerItem()->setToolTip(COL_FMAX, "The achieved Fmax of the paths within the clock");
    clockList->headerItem()->setToolTip(COL_SETUP, "The worst setup slack of the endpoints the clock captures");
    clockList->headerItem()->setToolTip(COL_TNS, "The sum of the negative setup slacks of these endpoints");
    clockList->headerItem()->setToolTip(COL_HOLD, "The worst hold slack of these endpoints");
    clockList->headerItem()->setToolTip(COL_ENDPOINTS, "The endpoints the clock captures (failing ones in brackets)");
    clockList->headerItem()->setToolTip(COL_CELLS, "The cells with a clock input on the net");
    clockList->headerItem()->setToolTip(COL_SKEW, "The difference of the routing delays to the clock inputs ("
                                                  "estimated while the net is not routed)");

    QWidget *topWidget = new QWidget();
    QVBoxLayout *vbox1 = new QVBoxLayout();
    topWidget->setLayout(vbox1);
    vbox1->setSpacing(5);
    vbox1->setContentsMargins(0, 0, 0, 0);
    vbox1->addWidget(header);
    vbox1->addWidget(clockList);

    summary = new QLabel();
    summary->setObjectName("clockSummary");
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);
    summary->setAlignment(Qt::AlignTop | Qt::AlignLeft);

    crossingList = new QTreeWidget();
    crossingList->setObjectName("clockCrossings");
    crossingList->setColumnCount(6);
    crossingList->setHeaderLabels(
            {"Launch", "Capture", "Related", "Setup WNS [ns]", "Hold WNS [ns]", "Endpoints"});
    crossingList->setRootIsDecorated(false);
    crossingList->setUniformRowHeights(true);
    crossingList->setAlternatingRowColors(true);
    crossingList->setSelectionMode(QAbstractItemView::NoSelection);
    crossingList->headerItem()->setToolTip(2, "Whether the analysis knows the delay between the two clocks (the "
                                              "same net or derived from one another); paths between unrelated clocks "
                                              "are analysed against the faster clock without a common reference");

    QWidget *btmWidget = new QWidget();
    QVBoxLayout *vbox2 = new QVBoxLayout();
    btmWidget->setLayout(vbox2);
    vbox2->setSpacing(5);
    vbox2->setContentsMargins(0, 0, 0, 0);
    vbox2->addWidget(summary);
    vbox2->addWidget(new QLabel("Paths between clocks (of the selected clock, else all)"));
    vbox2->addWidget(crossingList);

    QSplitter *splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName("splitter_clocks");
    splitter->addWidget(topWidget);
    splitter->addWidget(btmWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(splitter);
    setLayout(mainLayout);

    connect(clockList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        showClock(item ? clockList->indexOfTopLevelItem(item) : -1);
    });
    connect(clockList, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item) {
        int index = clockList->indexOfTopLevelItem(item);
        if (index >= 0 && index < int(clocks.size()))
            Q_EMIT focusNet(clocks.at(index).net);
    });
}

void ClocksWidget::newContext(Context *ctx)
{
    this->ctx = ctx;
    designChanged();
}

void ClocksWidget::designChanged()
{
    dirty = true;
    if (isVisible())
        analyse();
}

void ClocksWidget::showEvent(QShowEvent *event)
{
    if (dirty)
        analyse();
    QWidget::showEvent(event);
}

void ClocksWidget::analyse()
{
    dirty = false;
    // The selected clock stays selected
    IdString keep;
    int current = clockList->indexOfTopLevelItem(clockList->currentItem());
    if (current >= 0 && current < int(clocks.size()))
        keep = clocks.at(current).net;

    clocks.clear();
    crossings.clear();
    clockList->clear();
    crossingList->clear();
    summary->clear();
    header->clear();
    note.clear();
    if (!ctx)
        return;
    // (the cells of an unpacked design have no timing: the analysis would end in an error)
    if (!ctx->settings.count(ctx->id("pack"))) {
        header->setText("The design is not packed: the clocks are listed once it is.");
        return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    QString error;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        std::unique_ptr<TimingAnalyser> analyser;
        std::vector<TimingAnalyser::EndpointTiming> endpoints;
        // Catch the error message (log_error) for the header
        std::ostringstream errors;
        log_streams.push_back(std::make_pair(&errors, LogLevel::ERROR_MSG));
        try {
            analyser = std::make_unique<TimingAnalyser>(ctx);
            analyser->setup_only = false;
            analyser->with_clock_skew = true;
            // (as in the Paths tab: the CP line cells that routing adds make loops)
            analyser->ignore_loops = true;
            analyser->setup(false, false, true);
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

        // The clocks: the clock nets of the analysis (that clock cells) and the constrained nets
        std::map<IdString, int> index;
        auto clockOf = [&](IdString net) -> Clock & {
            auto found = index.find(net);
            if (found == index.end()) {
                found = index.emplace(net, int(clocks.size())).first;
                clocks.emplace_back();
                clocks.back().net = net;
                clocks.back().setup_wns = clocks.back().hold_wns = NO_SLACK;
            }
            return clocks.at(found->second);
        };
        if (analyser)
            for (auto &ev : analyser->get_clock_events())
                if (ctx->nets.count(ev.clock)) {
                    Clock &clock = clockOf(ev.clock);
                    (ev.edge == FALLING_EDGE ? clock.negedge : clock.posedge) = true;
                }
        for (auto &net : ctx->nets)
            if (net.second->clkconstr)
                clockOf(net.first);

        // The endpoints each clock captures, with their worst slacks; the paths between clocks
        dict<IdString, dict<CellPortKey, std::pair<delay_t, delay_t>>> captured;
        std::map<std::pair<std::pair<std::string, int>, std::pair<std::string, int>>, Crossing> crossing_map;
        auto clock_delays = analyser ? analyser->get_clock_delays() : dict<std::pair<IdString, IdString>, delay_t>();
        for (auto &ep : endpoints) {
            const ClockEvent &launch = ep.clock_pair.start, &capture = ep.clock_pair.end;
            if (capture.clock == IdString())
                continue;
            auto &slacks = captured[capture.clock]
                                   .emplace(ep.endpoint, std::make_pair(NO_SLACK, NO_SLACK))
                                   .first->second;
            slacks.first = std::min(slacks.first, ep.setup_slack);
            slacks.second = std::min(slacks.second, ep.hold_slack);

            if (launch.clock == IdString() || launch == capture)
                continue;
            auto key = std::make_pair(std::make_pair(std::string(launch.clock.c_str(ctx)), int(launch.edge)),
                                      std::make_pair(std::string(capture.clock.c_str(ctx)), int(capture.edge)));
            auto found = crossing_map.find(key);
            if (found == crossing_map.end()) {
                Crossing c;
                c.launch = launch;
                c.capture = capture;
                c.related = launch.clock == capture.clock ||
                            clock_delays.count(std::make_pair(launch.clock, capture.clock));
                c.setup_slack = c.hold_slack = NO_SLACK;
                found = crossing_map.emplace(key, c).first;
            }
            found->second.endpoints++;
            found->second.setup_slack = std::min(found->second.setup_slack, ep.setup_slack);
            found->second.hold_slack = std::min(found->second.hold_slack, ep.hold_slack);
        }
        for (auto &c : crossing_map)
            crossings.push_back(c.second);

        float default_mhz = ctx->setting<float>("target_freq") / 1e6f;
        for (Clock &clock : clocks) {
            const NetInfo *ni = ctx->nets.at(clock.net).get();

            // the constraint
            if (ni->clkconstr) {
                clock.constrained = true;
                double period = ctx->getDelayNS(ni->clkconstr->period.minDelay());
                clock.target_mhz = period > 0 ? 1000.0 / period : 0;
                clock.constraint = QString("period %1 ns (%2 MHz), high %3 ns, low %4 ns")
                                           .arg(period, 0, 'f', 3)
                                           .arg(clock.target_mhz, 0, 'f', 2)
                                           .arg(ctx->getDelayNS(ni->clkconstr->high.minDelay()), 0, 'f', 3)
                                           .arg(ctx->getDelayNS(ni->clkconstr->low.minDelay()), 0, 'f', 3);
            } else {
                clock.target_mhz = default_mhz;
                clock.constraint = QString("none, default target %1 MHz").arg(default_mhz, 0, 'f', 2);
            }
            if (analyser) {
                auto &fmax = analyser->get_timing_result().clock_fmax;
                auto found = fmax.find(clock.net);
                if (found != fmax.end())
                    clock.fmax_mhz = found->second.achieved;
            }

            // the endpoints it captures
            auto found = captured.find(clock.net);
            if (found != captured.end())
                for (auto &ep : found->second) {
                    clock.endpoints++;
                    clock.setup_wns = std::min(clock.setup_wns, ep.second.first);
                    clock.hold_wns = std::min(clock.hold_wns, ep.second.second);
                    if (ep.second.first < 0) {
                        clock.setup_tns += ep.second.first;
                        clock.failing++;
                    }
                }

            // the driver
            if (ni->driver.cell) {
                const CellInfo *drv = ni->driver.cell;
                clock.driver = QString("%1 (%2)").arg(drv->type.c_str(ctx), ni->driver.port.c_str(ctx));
                clock.driver_details = QString("%1.%2 (%3)%4")
                                               .arg(drv->name.c_str(ctx), ni->driver.port.c_str(ctx),
                                                    drv->type.c_str(ctx),
                                                    drv->bel != BelId()
                                                            ? QString(" at %1").arg(ctx->nameOfBel(drv->bel))
                                                            : QString(", not placed"));
            } else {
                clock.driver = "-";
                clock.driver_details = "none";
            }

            // the clock inputs: their routing and the delays to them
            bool first = true;
            for (auto &usr : ni->users) {
                clock.users++;
                int count = 0;
                if (ctx->getPortTimingClass(usr.cell, usr.port, count) != TMG_CLOCK_INPUT)
                    continue;
                clock.clocked_cells++;
                clock.clock_sinks++;
                bool routed = ni->driver.cell != nullptr && ctx->getNetinfoSinkWireCount(ni, usr) > 0;
                for (size_t i = 0; routed && i < ctx->getNetinfoSinkWireCount(ni, usr); i++)
                    routed = ni->wires.count(ctx->getNetinfoSinkWire(ni, usr, i)) > 0;
                if (routed)
                    clock.routed_sinks++;
                if (ni->driver.cell == nullptr || usr.cell->bel == BelId() || ni->driver.cell->bel == BelId())
                    continue;
                delay_t delay = ctx->getNetinfoRouteDelay(ni, usr);
                clock.min_insertion = first ? delay : std::min(clock.min_insertion, delay);
                clock.max_insertion = first ? delay : std::max(clock.max_insertion, delay);
                first = false;
            }
            if (first)
                clock.min_insertion = clock.max_insertion = NO_SLACK;

            for (auto &cd : clock_delays)
                if (cd.first.first == clock.net && cd.first.second != clock.net)
                    clock.related << QString("%1 (%2 ns)").arg(cd.first.second.c_str(ctx), formatDelay(cd.second));
        }
    }

    // The busiest clocks first
    std::stable_sort(clocks.begin(), clocks.end(), [](const Clock &a, const Clock &b) {
        return a.clocked_cells != b.clocked_cells ? a.clocked_cells > b.clocked_cells : a.endpoints > b.endpoints;
    });

    QList<QTreeWidgetItem *> items;
    int failing_clocks = 0;
    for (const Clock &clock : clocks) {
        QString edges = clock.posedge && clock.negedge ? "both" : clock.negedge ? "neg" : clock.posedge ? "pos" : "-";
        QString skew = clock.min_insertion == NO_SLACK ? "-" : formatDelay(clock.max_insertion - clock.min_insertion);
        if (clock.min_insertion != NO_SLACK && clock.routed_sinks < clock.clock_sinks)
            skew += " est.";
        QString endpoints = QString::number(clock.endpoints);
        if (clock.failing > 0)
            endpoints += QString(" (%1)").arg(clock.failing);
        auto item = new QTreeWidgetItem(
                {QString(clock.net.c_str(ctx)), QString::number(clock.target_mhz, 'f', 2),
                 clock.fmax_mhz > 0 ? QString::number(clock.fmax_mhz, 'f', 2) : QString("-"),
                 formatDelay(clock.setup_wns), clock.endpoints > 0 ? formatDelay(clock.setup_tns) : QString("-"),
                 formatDelay(clock.hold_wns), endpoints, QString::number(clock.clocked_cells), edges, skew,
                 clock.driver});
        for (int column = COL_TARGET; column <= COL_CELLS; column++)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        item->setTextAlignment(COL_SKEW, Qt::AlignRight | Qt::AlignVCenter);
        if (!clock.constrained) {
            QFont font = item->font(COL_TARGET);
            font.setItalic(true);
            item->setFont(COL_TARGET, font);
        }
        bool fails = false;
        if (clock.fmax_mhz > 0 && clock.fmax_mhz < clock.target_mhz) {
            item->setForeground(COL_FMAX, Qt::red);
            fails = true;
        }
        if (clock.setup_wns < 0) {
            item->setForeground(COL_SETUP, Qt::red);
            item->setForeground(COL_TNS, Qt::red);
            fails = true;
        }
        if (clock.hold_wns < 0) {
            item->setForeground(COL_HOLD, Qt::red);
            fails = true;
        }
        failing_clocks += fails;
        items.append(item);
    }
    clockList->addTopLevelItems(items);
    for (int column = 0; column < COL_COUNT; column++)
        clockList->resizeColumnToContents(column);

    if (!error.isEmpty())
        header->setText(error);
    else if (clocks.empty())
        header->setText("No clocks.");
    else
        header->setText(QString("%1 clock%2, %3 failing. Double click a clock to show its net. %4")
                                .arg(clocks.size())
                                .arg(clocks.size() == 1 ? "" : "s")
                                .arg(failing_clocks)
                                .arg(note));

    int select = -1;
    for (size_t i = 0; i < clocks.size(); i++)
        if (clocks.at(i).net == keep)
            select = int(i);
    if (select >= 0)
        clockList->setCurrentItem(clockList->topLevelItem(select));
    else
        showClock(-1);
    QApplication::restoreOverrideCursor();
}

void ClocksWidget::showClock(int index)
{
    crossingList->clear();
    const Clock *clock = index >= 0 && index < int(clocks.size()) ? &clocks.at(index) : nullptr;

    QList<QTreeWidgetItem *> items;
    for (const Crossing &c : crossings) {
        if (clock && c.launch.clock != clock->net && c.capture.clock != clock->net)
            continue;
        auto item = new QTreeWidgetItem({clockName(c.launch), clockName(c.capture),
                                         c.launch.clock == c.capture.clock ? "same clock"
                                         : c.related                       ? "yes"
                                                                           : "no",
                                         formatDelay(c.setup_slack), formatDelay(c.hold_slack),
                                         QString::number(c.endpoints)});
        for (int column = 3; column < 6; column++)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        if (c.setup_slack < 0)
            item->setForeground(3, Qt::red);
        if (c.hold_slack < 0)
            item->setForeground(4, Qt::red);
        if (!c.related)
            item->setForeground(2, Qt::darkYellow);
        items.append(item);
    }
    crossingList->addTopLevelItems(items);
    for (int column = 0; column < 6; column++)
        crossingList->resizeColumnToContents(column);

    if (!clock) {
        summary->setText(clocks.empty() ? QString() : QString("Select a clock for its details."));
        return;
    }

    QString fmax = clock->fmax_mhz > 0 ? QString("%1 MHz, %2")
                                                 .arg(clock->fmax_mhz, 0, 'f', 2)
                                                 .arg(clock->fmax_mhz >= clock->target_mhz ? "PASS" : "FAIL")
                                       : QString("none (no paths within the clock)");
    QString routing;
    if (clock->clock_sinks == 0)
        routing = "no clock inputs";
    else
        routing = QString("%1 of %2 clock inputs routed").arg(clock->routed_sinks).arg(clock->clock_sinks);
    if (clock->min_insertion != NO_SLACK)
        routing += QString(", delay to them %1 .. %2 ns%3")
                           .arg(formatDelay(clock->min_insertion), formatDelay(clock->max_insertion),
                                clock->routed_sinks < clock->clock_sinks ? " (estimated)" : "");
    QString setup = clock->endpoints == 0
                            ? QString("none")
                            : QString("%1, setup WNS %2 ns, TNS %3 ns (%4 failing), hold WNS %5 ns")
                                      .arg(clock->endpoints)
                                      .arg(formatDelay(clock->setup_wns), formatDelay(clock->setup_tns))
                                      .arg(clock->failing)
                                      .arg(formatDelay(clock->hold_wns));
    summary->setText(QString("<b>%1</b><br>"
                             "Constraint: %2<br>"
                             "Fmax: %3<br>"
                             "Captured endpoints: %4<br>"
                             "Driver: %5<br>"
                             "Users: %6, of them %7 clocked cells<br>"
                             "Routing: %8<br>"
                             "Related clocks: %9")
                             .arg(QString(clock->net.c_str(ctx)).toHtmlEscaped(), clock->constraint.toHtmlEscaped(),
                                  fmax, setup.toHtmlEscaped(), clock->driver_details.toHtmlEscaped())
                             .arg(clock->users)
                             .arg(clock->clocked_cells)
                             .arg(routing.toHtmlEscaped(),
                                  clock->related.isEmpty() ? QString("none")
                                                           : clock->related.join(", ").toHtmlEscaped()));
}

QString ClocksWidget::formatDelay(delay_t delay) const
{
    if (delay == NO_SLACK)
        return "-";
    return QString::number(ctx->getDelayNS(delay), 'f', 3);
}

QString ClocksWidget::clockName(const ClockEvent &clock) const
{
    if (clock.clock == IdString())
        return "async";
    return QString("%1 %2").arg(clock.edge == FALLING_EDGE ? "negedge" : "posedge", clock.clock.c_str(ctx));
}

NEXTPNR_NAMESPACE_END
