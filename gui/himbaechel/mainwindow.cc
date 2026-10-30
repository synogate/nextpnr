/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <micko@yosyshq.com>
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

#include "mainwindow.h"

#include <QMessageBox>
#include "../fpgaviewwidget.h"
#include "../timingwidget.h"
#include "partitionwidget.h"

static void initMainResource() { Q_INIT_RESOURCE(nextpnr); }

NEXTPNR_NAMESPACE_BEGIN

MainWindow::MainWindow(std::unique_ptr<Context> context, CommandHandler *handler, QWidget *parent)
        : BaseMainWindow(std::move(context), handler, parent)
{
    initMainResource();

    connect(this, &BaseMainWindow::contextChanged, this, &MainWindow::newContext);
    connect(task, &TaskManager::run_finished, this, &MainWindow::run_finished);

    repairTimer = new QTimer(this);
    connect(repairTimer, &QTimer::timeout, this, &MainWindow::repair_progress);
    connect(task, &TaskManager::taskCanceled, this, [this] { repairTimer->stop(); });

    partitionview = new PartitionWidget(fpgaView);
    rightTabWidget->addTab(partitionview, "Partitions");
    connect(this, &BaseMainWindow::contextChanged, partitionview, &PartitionWidget::newContext);
    connect(this, &BaseMainWindow::updateTreeView, partitionview, &PartitionWidget::designChanged);

    createMenu();
}

MainWindow::~MainWindow() {}

void MainWindow::newContext(Context *ctx)
{
    std::string title = "nextpnr-himbächel - " + ctx->getChipName();
    setWindowTitle(title.c_str());
    // The uarch's repairs after routing are run by their own button, before finishing the routing
    ctx->defer_route_finish = true;
}

void MainWindow::createMenu()
{
    actionRepair = new QAction("Repair", this);
    actionRepair->setIcon(QIcon(":/icons/resources/time_add.png"));
    actionRepair->setStatusTip("Repair the critical paths of the routed design (can be run again)");
    actionRepair->setEnabled(false);
    connect(actionRepair, &QAction::triggered, this, &MainWindow::repair_route);

    actionFinish = new QAction("Finish", this);
    actionFinish->setIcon(QIcon(":/icons/resources/resultset_last.png"));
    actionFinish->setStatusTip("Finish the routing (no repairs after this)");
    actionFinish->setEnabled(false);
    connect(actionFinish, &QAction::triggered, this, &MainWindow::finish_route);

    mainActionBar->insertAction(actionExecutePy, actionRepair);
    mainActionBar->insertAction(actionExecutePy, actionFinish);

    // after Route, before the separator
    QAction *after_route = menuDesign->actions().at(menuDesign->actions().indexOf(actionRoute) + 1);
    menuDesign->insertAction(after_route, actionRepair);
    menuDesign->insertAction(after_route, actionFinish);
}

void MainWindow::repair_route()
{
    Context *c = ctx.get();
    running = "Repairing";
    repairTimer->start(100);
    Q_EMIT task->run([c]() { return c->repairRoute(); });
}

void MainWindow::repair_progress()
{
    // The timing after a round of the repairs: the repair waits without the Context (at its next
    // round) while the paths are analysed again, so that the GUI never waits for it
    if (!ctx->repair_ui_wants) {
        if (ctx->repair_progress)
            ctx->repair_ui_wants = true;
    } else if (ctx->repair_waiting) {
        ctx->repair_progress = false;
        timingview->designChanged();
        ctx->repair_ui_wants = false;
    }
}

void MainWindow::finish_route()
{
    Context *c = ctx.get();
    running = "Finishing the routing of";
    Q_EMIT task->run([c]() { return c->finishRoute(); });
}

void MainWindow::run_finished(bool status)
{
    repairTimer->stop();
    ctx->repair_ui_wants = false;
    ctx->repair_progress = false;
    disableActions();
    if (status) {
        log("%s design successful.\n", running.c_str());
        Q_EMIT updateTreeView();
        updateActions();
    } else
        log("%s design failed.\n", running.c_str());
}

void MainWindow::save_json()
{
    // The JSON holds the finished routing
    if (ctx->route_finish_pending) {
        ctx->finishRoute();
        Q_EMIT updateTreeView();
        disableActions();
        updateActions();
    }
    BaseMainWindow::save_json();
}

void MainWindow::onDisableActions()
{
    actionRepair->setEnabled(false);
    actionFinish->setEnabled(false);
}

void MainWindow::onUpdateActions()
{
    actionRepair->setEnabled(ctx->route_finish_pending);
    actionFinish->setEnabled(ctx->route_finish_pending);
    // A partition written placed only is complete once placed
    if (ctx->settings.count(ctx->id("partition-unrouted")))
        actionRoute->setEnabled(false);
}

bool MainWindow::beforePack()
{
    QString error;
    if (partitionview->apply(error))
        return true;
    QMessageBox::critical(this, "Partitions", error);
    return false;
}

PnrStep MainWindow::nextStep()
{
    // The repairs can run until the routing is finished
    if (ctx->route_finish_pending)
        return PnrStep::Repair;
    return BaseMainWindow::nextStep();
}

void MainWindow::optionsChanged() { ctx->uarch->optionsChanged(); }

std::vector<PnrOption> MainWindow::pnrOptions()
{
    std::vector<PnrOption> options = BaseMainWindow::pnrOptions();
    if (ctx->args.device.rfind("CCGM", 0) != 0)
        return options;

    // The GateMate options are those of --vopt, kept in ctx->args.options; an option at its default is left out
    auto &vopts = ctx->args.options;
    auto set_option = [&vopts](const char *key, const boost::any &value) {
        vopts.erase(key);
        vopts.insert(std::make_pair(key, po::variable_value(value, false)));
    };
    auto option = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      PnrOption::Kind kind, QVariant def) {
        PnrOption o{step, group, label, help, kind, def};
        o.get = [&vopts, key, kind, def]() -> QVariant {
            if (!vopts.count(key))
                return def;
            const auto &v = vopts[key];
            if (kind == PnrOption::Int)
                return v.value().type() == typeid(uint64_t) ? qlonglong(v.as<uint64_t>()) : v.as<int>();
            if (kind == PnrOption::Float)
                return v.as<float>();
            return QString::fromStdString(v.as<std::string>());
        };
        o.set = [&vopts, set_option, key, kind, def](const QVariant &v) {
            if (v == def || (kind == PnrOption::Float && v.toDouble() == def.toDouble()))
                vopts.erase(key);
            else if (kind == PnrOption::Int)
                set_option(key, v.toInt());
            else if (kind == PnrOption::Float)
                set_option(key, float(v.toDouble()));
            else
                set_option(key, v.toString().toStdString());
        };
        return o;
    };
    auto number = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      PnrOption::Kind kind, double def, double min, double max, const char *special = "",
                      double singleStep = 1) {
        PnrOption o = option(step, group, label, help, key, kind, def);
        o.min = min;
        o.max = max;
        o.special = special;
        o.singleStep = singleStep;
        if (kind == PnrOption::Float)
            o.decimals = singleStep < 0.1 ? 3 : 2;
        return o;
    };
    auto choice = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      QStringList choices) {
        PnrOption o = option(step, group, label, help, key, PnrOption::Choice, choices.front());
        o.choices = choices;
        return o;
    };
    auto file = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                    PnrOption::Kind kind) { return option(step, group, label, help, key, kind, QString()); };
    // A flag is on when given, or with `inverted` (a --vopt no-...) when not given
    auto flag = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                    bool inverted = false) {
        PnrOption o{step, group, label, help, PnrOption::Flag, inverted};
        o.get = [&vopts, key, inverted] { return QVariant((vopts.count(key) > 0) != inverted); };
        o.set = [&vopts, set_option, key, inverted](const QVariant &v) {
            if (v.toBool() != inverted)
                set_option(key, std::string());
            else
                vopts.erase(key);
        };
        return o;
    };

    const PnrStep pack = PnrStep::Pack, place = PnrStep::Place, route = PnrStep::Route, repair = PnrStep::Repair,
                  finish = PnrStep::Finish;
    auto add = [&](PnrOption o) { options.push_back(o); };

    add(file(pack, "Constraints", "CCF file", "Pin and placement constraints", "ccf", PnrOption::OpenFile));
    add(flag(pack, "Constraints", "Allow unconstrained IOs", "IOs without a pin are placed freely",
             "allow-unconstrained"));
    add(choice(pack, "Constraints", "Multi-die clocks", "How clocks reach several dies: mirror, full use, reuse CLK1",
               "strategy", {"mirror", "full", "clk1"}));
    add(flag(pack, "Logic rewrites", "Fuse LUTs", "Fold inverters and LUTs into the LUTs they feed", "fuse-luts"));
    add(flag(pack, "Logic rewrites", "Mux chains", "Two 2:1 muxes in a row become one 4:1 mux", "mux-chains"));
    PnrOption merge = choice(pack, "Logic rewrites", "CPE merge", "Two levels of LUTs in one CPE; 2: also with a copy",
                             "cpe-merge", {"off", "1", "2"});
    merge.get = [&vopts] { return vopts.count("cpe-merge") ? QString::number(vopts["cpe-merge"].as<int>()) : "off"; };
    merge.set = [&vopts, set_option](const QVariant &v) {
        if (v.toString() == "off")
            vopts.erase("cpe-merge");
        else
            set_option("cpe-merge", v.toInt());
    };
    add(merge);
    add(number(pack, "Logic rewrites", "Enable into LUT", "For enables of at least this many registers",
               "enable-to-lut", PnrOption::Int, 0, 0, 100000, "off"));
    add(number(pack, "Rewrite only where critical", "Minimum depth", "Share of the deepest path's logic levels",
               "rewrite-depth", PnrOption::Float, 0, 0, 1, "everywhere", 0.05));
    add(file(pack, "Rewrite only where critical", "Criticality file", "Nets critical in an earlier run",
             "rewrite-crit-in", PnrOption::OpenFile));
    add(number(pack, "Rewrite only where critical", "Minimum criticality", "From the criticality file",
               "rewrite-crit-min", PnrOption::Float, 0.9, 0, 1, "", 0.05));

    add(choice(place, "GateMate placer", "Delay estimate", "table: shortest paths on the empty chip", "place-delay",
               {"linear", "table"}));
    add(flag(place, "GateMate placer", "Timing optimisation", "Move LUT trees on critical paths afterwards",
             "timing-opt"));
    add(file(place, "Earlier criticality", "Criticality file", "Nets critical in an earlier run",
             "place-crit-in", PnrOption::OpenFile));
    add(choice(place, "Earlier criticality", "Use", "max: as a minimum, scale: as a factor", "place-crit-mode",
               {"max", "scale"}));

    PnrOption route_seed = number(route, "GateMate router", "Seed", "Restarts the random numbers before routing",
                                  "route-seed", PnrOption::Int, -1, -1, 1 << 30, "unchanged");
    route_seed.set = [&vopts, set_option](const QVariant &v) {
        if (v.toInt() < 0)
            vopts.erase("route-seed");
        else
            set_option("route-seed", uint64_t(v.toInt()));
    };
    add(route_seed);
    add(choice(route, "GateMate router", "Delay estimate", "table: shortest paths on the empty chip", "lookahead",
               {"linear", "table"}));
    add(number(route, "GateMate router", "Table radius", "Tiles the delay tables cover (also the placer's)",
               "lookahead-radius", PnrOption::Int, 12, 1, 100));
    add(flag(route, "Routing resources", "CP lines for clock and enable", "Clock and enable over CP lines",
             "clk-cp"));
    add(flag(route, "Routing resources", "CP lines through CPEs", "Pass signals through a CPE's CP lines",
             "no-cpe-cp", true));
    add(flag(route, "Routing resources", "CPE bridges", "Route through CPEs in bridge mode", "no-bridges", true));
    add(number(route, "Routing resources", "Y output cost", "Extra ns per switch box Y output, non-critical nets",
               "sb-y-cost", PnrOption::Float, 0, 0, 10, "", 0.05));
    add(number(route, "Routing resources", "Diagonal mux cost", "Extra ns per diagonal mux, non-critical nets",
               "sb-ydiag-cost", PnrOption::Float, 0, 0, 10, "", 0.05));
    add(flag(route, "Before routing", "Swap LUT inputs", "Input pairs of LUT trees, to save plane changes",
             "lut-input-swap"));
    add(number(route, "Before routing", "Replicate drivers", "Copy LUTs driving critical nets of this many sinks",
               "replicate-fanout", PnrOption::Int, 0, 0, 100000, "off"));

    add(number(repair, "Repairs", "Route repair", "Rounds re-routing critical connections", "route-repair",
               PnrOption::Int, -1, -1, 1000, "until met"));
    add(number(repair, "Repairs", "Place repair", "Rounds moving cells on critical paths", "place-repair",
               PnrOption::Int, -1, -1, 1000, "until met"));
    add(number(repair, "Repairs", "Net repair", "Rounds re-routing nets with several critical sinks", "net-repair",
               PnrOption::Int, 0, 0, 1000, "off"));
    add(number(repair, "Repairs", "Margin", "Percent above the constraint, when repairing until met",
               "repair-margin", PnrOption::Float, 0, 0, 100, "", 0.5));
    add(number(repair, "LUT input swaps", "Rounds", "Swapping LUT inputs on critical paths", "lut-swap-rounds",
               PnrOption::Int, 0, 0, 1000, "off"));
    add(choice(repair, "LUT input swaps", "Moves", "Input pairs, CPE halves or both", "lut-swap-moves",
               {"both", "pairs", "halves"}));
    add(number(repair, "Route again", "Runs", "Route from scratch with learnt criticality, keep the best",
               "route-runs", PnrOption::Int, 0, 0, 100, "off"));
    add(choice(repair, "Route again", "Learning", "How the criticality is learnt", "route-learn",
               {"max", "ema", "scale", "none"}));
    add(number(repair, "Route again", "Learning step", "For ema and scale", "route-learn-step", PnrOption::Float,
               0.25, 0, 1, "", 0.05));
    add(flag(repair, "Reports", "Compare with delay tables", "The critical paths' routing against the estimate",
             "report-crit-routing"));
    add(file(repair, "Reports", "Criticality file", "Write each net's criticality, for later runs",
             "place-crit-out", PnrOption::SaveFile));

    add(file(finish, "Bitstream", "Bitstream file", "Textual configuration, written when finishing", "out",
             PnrOption::SaveFile));
    return options;
}

void MainWindow::new_proj()
{
    QMessageBox::critical(0, "Error",
                          "Creating a new project not supported in himbächel mode, please re-start from command line.");
}

NEXTPNR_NAMESPACE_END
