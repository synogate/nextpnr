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
#include "../timingwidget.h"

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
}

void MainWindow::new_proj()
{
    QMessageBox::critical(0, "Error",
                          "Creating a new project not supported in himbächel mode, please re-start from command line.");
}

NEXTPNR_NAMESPACE_END
