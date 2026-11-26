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

#include "exceptionswidget.h"
#include <QApplication>
#include <QFormLayout>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>
#include <sstream>
#include "log.h"
#include "timingwidget.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {
const delay_t NO_SLACK = std::numeric_limits<delay_t>::max();

QString portName(const Context *ctx, const std::pair<IdString, IdString> &port)
{
    return QString("%1.%2").arg(port.first.c_str(ctx), port.second.c_str(ctx));
}

bool matches(const TimingAnalyser::PairConstraints &pc, int exception)
{
    return exception < 0 || std::find(pc.exceptions.begin(), pc.exceptions.end(), exception) != pc.exceptions.end();
}

// (a false path applies wherever it matches: the pair names one of several only)
bool applies(const Context *ctx, const TimingAnalyser::PairConstraints &pc, int exception)
{
    return pc.setup_exception == exception || pc.hold_exception == exception ||
           ctx->timing_exceptions.at(exception).type == TimingException::FALSE_PATH;
}
} // namespace

ExceptionsWidget::ExceptionsWidget(QWidget *parent) : QWidget(parent), ctx(nullptr), dirty(true)
{
    summary = new QLabel();
    summary->setObjectName("exceptionSummary");
    summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    summary->setWordWrap(true);

    exceptionList = new QTreeWidget();
    exceptionList->setObjectName("exceptionList");
    exceptionList->setColumnCount(5);
    exceptionList->setHeaderLabels({"Line", "Paths", "Applied", "Setup WNS [ns]", "Exception"});
    exceptionList->setRootIsDecorated(false);
    exceptionList->setUniformRowHeights(true);
    exceptionList->setAlternatingRowColors(true);
    exceptionList->setSelectionMode(QAbstractItemView::SingleSelection);

    QWidget *topWidget = new QWidget();
    QVBoxLayout *vbox1 = new QVBoxLayout();
    topWidget->setLayout(vbox1);
    vbox1->setSpacing(5);
    vbox1->setContentsMargins(0, 0, 0, 0);
    vbox1->addWidget(summary);
    vbox1->addWidget(exceptionList);

    countSpin = new QSpinBox();
    countSpin->setRange(1, 100000);
    countSpin->setValue(200);
    QFormLayout *form = new QFormLayout();
    form->setContentsMargins(0, 0, 0, 0);
    form->addRow("Paths", countSpin);

    pathList = new QTreeWidget();
    pathList->setObjectName("exceptionPaths");
    pathList->setColumnCount(7);
    pathList->setHeaderLabels({"Setup [ns]", "Hold [ns]", "Cycles", "From", "To", "Clock", "Exceptions"});
    pathList->setRootIsDecorated(false);
    pathList->setUniformRowHeights(true);
    pathList->setAlternatingRowColors(true);
    pathList->setSelectionMode(QAbstractItemView::SingleSelection);

    pathSummary = new QLabel();
    pathSummary->setObjectName("exceptionPathSummary");
    pathSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    pathSummary->setWordWrap(true);

    QWidget *btmWidget = new QWidget();
    QVBoxLayout *vbox2 = new QVBoxLayout();
    btmWidget->setLayout(vbox2);
    vbox2->setSpacing(5);
    vbox2->setContentsMargins(0, 0, 0, 0);
    vbox2->addLayout(form);
    vbox2->addWidget(pathList, 1);
    vbox2->addWidget(pathSummary);

    QSplitter *splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName("splitter_exceptions");
    splitter->addWidget(topWidget);
    splitter->addWidget(btmWidget);

    QVBoxLayout *mainLayout = new QVBoxLayout();
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(splitter);
    setLayout(mainLayout);

    connect(exceptionList, &QTreeWidget::currentItemChanged, this, [this] { updatePaths(); });
    connect(countSpin, &QSpinBox::valueChanged, this, [this] { updatePaths(); });
    connect(pathList, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item) {
        int index = item ? pathList->indexOfTopLevelItem(item) : -1;
        if (index < 0 || index >= int(paths.size())) {
            return;
        }
        const auto &path = paths.at(index);
        QString text = QString("<b>%1</b> -> <b>%2</b>, setup slack %3 ns, hold slack %4 ns")
                               .arg(item->text(3).toHtmlEscaped(), item->text(4).toHtmlEscaped(),
                                    formatDelay(path.timing.setup_slack), formatDelay(path.timing.hold_slack));
        for (int exception : path.constraints.exceptions)
            text += QString("<br>%1<br>&nbsp;&nbsp;&rarr; %2")
                            .arg(TimingText::exceptionName(ctx, exception).toHtmlEscaped(),
                                 TimingText::exceptionStatus(ctx, path.constraints, exception).toHtmlEscaped());
        text += "<br>(the Paths tab shows how the slacks come about)";
        pathSummary->setText(text);
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        Q_EMIT selected(TimingText::pathDecals(ctx, path.path), false);
    });
    connect(pathList, &QTreeWidget::itemDoubleClicked, this, [this] { Q_EMIT zoomSelected(); });
}

ExceptionsWidget::~ExceptionsWidget() {}

void ExceptionsWidget::newContext(Context *ctx)
{
    this->ctx = ctx;
    designChanged();
}

void ExceptionsWidget::designChanged()
{
    dirty = true;
    analyser.reset();
    matched.clear();
    paths.clear();
    if (isVisible())
        analyse();
}

void ExceptionsWidget::showEvent(QShowEvent *event)
{
    if (dirty)
        analyse();
    QWidget::showEvent(event);
}

void ExceptionsWidget::analyse()
{
    dirty = false;
    if (!ctx)
        return;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    int current = exceptionList->currentItem() ? exceptionList->currentItem()->data(0, Qt::UserRole).toInt() : -1;
    analyser.reset();
    matched.clear();
    QString error;
    int count = int(ctx->timing_exceptions.size());
    if (count > 0 && !ctx->settings.count(ctx->id("pack"))) {
        error = "The design is not packed: the paths of the exceptions are listed once it is.";
    } else if (count > 0) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        // Catch the error message (log_error) for the summary
        std::ostringstream errors;
        log_streams.push_back(std::make_pair(&errors, LogLevel::ERROR_MSG));
        try {
            analyser = std::make_unique<TimingAnalyser>(ctx);
            analyser->setup_only = false;
            analyser->with_clock_skew = true;
            // (as the Paths tab: the CP line cells of the routing have loops)
            analyser->ignore_loops = true;
            analyser->setup();
            dict<int, TimingAnalyser::PairConstraints> pairs;
            for (auto &ep : analyser->get_endpoint_timings()) {
                auto pair = pairs.find(ep.domain_pair);
                if (pair == pairs.end())
                    pair = pairs.emplace(ep.domain_pair, analyser->get_pair_constraints(ep.domain_pair)).first;
                if (!pair->second.exceptions.empty())
                    matched.emplace_back(ep, pair->second);
            }
        } catch (const log_execution_error_exception &) {
            analyser.reset();
            matched.clear();
            QString msg = QString::fromStdString(errors.str()).trimmed();
            error = QString("Timing analysis failed: %1").arg(msg.isEmpty() ? "see the log" : msg);
        } catch (std::exception &e) {
            analyser.reset();
            matched.clear();
            error = QString("Timing analysis failed: %1").arg(e.what());
        }
        for (auto it = log_streams.begin(); it != log_streams.end(); ++it)
            if (it->first == &errors) {
                log_streams.erase(it);
                break;
            }
    }

    // The exceptions, with the paths they match and apply to
    exceptionList->blockSignals(true);
    exceptionList->clear();
    if (count > 0) {
        auto all = new QTreeWidgetItem({"", QString::number(matched.size()), "", "", "All paths with exceptions"});
        all->setData(0, Qt::UserRole, -1);
        exceptionList->addTopLevelItem(all);
    }
    int false_paths = 0;
    for (int i = 0; i < count; i++) {
        const TimingException &exc = ctx->timing_exceptions.at(i);
        false_paths += exc.type == TimingException::FALSE_PATH;
        int paths_matched = 0, paths_applied = 0;
        // (the paths of a false path for setup have no setup slack)
        delay_t wns = NO_SLACK;
        for (auto &m : matched) {
            if (!matches(m.second, i))
                continue;
            paths_matched++;
            if (applies(ctx, m.second, i)) {
                paths_applied++;
                wns = std::min(wns, m.first.setup_slack);
            }
        }
        QString name = TimingText::exceptionName(ctx, i);
        if (name.startsWith("line "))
            name = name.mid(name.indexOf(':') + 2);
        auto item = new QTreeWidgetItem({exc.line > 0 ? QString::number(exc.line) : QString(),
                                         analyser ? QString::number(paths_matched) : QString("-"),
                                         analyser ? QString::number(paths_applied) : QString("-"), formatDelay(wns),
                                         name});
        item->setData(0, Qt::UserRole, i);
        item->setToolTip(4, name);
        for (int column = 0; column < 4; column++)
            item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
        if (wns != NO_SLACK && wns < 0)
            item->setForeground(3, Qt::red);
        if (exc.type == TimingException::FALSE_PATH)
            item->setForeground(4, Qt::darkGray);
        exceptionList->addTopLevelItem(item);
    }
    for (int column = 0; column < 4; column++)
        exceptionList->resizeColumnToContents(column);
    // (the exception selected before, else all)
    for (int i = 0; i < exceptionList->topLevelItemCount(); i++)
        if (exceptionList->topLevelItem(i)->data(0, Qt::UserRole).toInt() == current)
            exceptionList->setCurrentItem(exceptionList->topLevelItem(i));
    if (!exceptionList->currentItem() && exceptionList->topLevelItemCount() > 0)
        exceptionList->setCurrentItem(exceptionList->topLevelItem(0));
    exceptionList->blockSignals(false);

    if (!error.isEmpty())
        summary->setText(error);
    else if (count == 0)
        summary->setText("No timing exceptions: every path is timed by its clocks alone. Exceptions are "
                         "set_multicycle_path and set_false_path in the SDC file (--sdc).");
    else
        summary->setText(QString("%1 timing exception(s) on %2 path(s). Paths: the end points (per launch clock) "
                                 "that the -from and -to lists match; applied: those it sets the required time "
                                 "of.%3")
                                 .arg(count)
                                 .arg(matched.size())
                                 .arg(false_paths > 0 ? " The paths of a false path are not timed: they have no "
                                                        "slack ('-')."
                                                      : ""));
    updatePaths();
    QApplication::restoreOverrideCursor();
}

void ExceptionsWidget::updatePaths()
{
    paths.clear();
    pathList->clear();
    pathSummary->clear();
    if (!analyser || !exceptionList->currentItem())
        return;
    int exception = exceptionList->currentItem()->data(0, Qt::UserRole).toInt();

    std::vector<const std::pair<TimingAnalyser::EndpointTiming, TimingAnalyser::PairConstraints> *> selected;
    for (auto &m : matched)
        if (matches(m.second, exception))
            selected.push_back(&m);
    size_t count = std::min(selected.size(), size_t(countSpin->value()));
    std::partial_sort(selected.begin(), selected.begin() + count, selected.end(),
                      [](auto *a, auto *b) { return a->first.setup_slack < b->first.setup_slack; });

    QList<QTreeWidgetItem *> items;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (size_t i = 0; i < count; i++) {
            const auto &ep = selected.at(i)->first;
            const auto &pc = selected.at(i)->second;
            paths.push_back(Path{ep, pc, analyser->get_endpoint_path(ep, true)});

            QString from;
            for (auto &segment : paths.back().path.segments)
                if (segment.type == CriticalPath::Segment::Type::CLK_TO_Q ||
                    segment.type == CriticalPath::Segment::Type::SOURCE) {
                    from = portName(ctx, segment.from);
                    break;
                }
            QString clocks = TimingText::clockName(ctx, ep.clock_pair.start);
            if (!(ep.clock_pair.end == ep.clock_pair.start))
                clocks += " -> " + TimingText::clockName(ctx, ep.clock_pair.end);
            // What the exceptions (the selected one, else all) do to the path
            QStringList status;
            for (int e : pc.exceptions)
                if (exception < 0 || e == exception)
                    status << QString("line %1: %2")
                                      .arg(ctx->timing_exceptions.at(e).line)
                                      .arg(TimingText::exceptionStatus(ctx, pc, e));
            auto item = new QTreeWidgetItem({formatDelay(ep.setup_slack), formatDelay(ep.hold_slack),
                                             QString::number(pc.setup_cycles), from,
                                             portName(ctx, {ep.endpoint.cell, ep.endpoint.port}), clocks,
                                             status.join("; ")});
            item->setToolTip(6, status.join("\n"));
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
    pathSummary->setText(selected.empty() ? QString("No paths: the -from and -to lists match no timed path.")
                                          : QString("%1 of %2 paths, worst setup slack first. Select one for "
                                                    "what the exceptions do to it.")
                                                    .arg(count)
                                                    .arg(selected.size()));
}

QString ExceptionsWidget::formatDelay(delay_t delay)
{
    if (delay == NO_SLACK)
        return "-";
    return QString::number(ctx->getDelayNS(delay), 'f', 3);
}

NEXTPNR_NAMESPACE_END
