/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <micko@yosyshq.com>
 *  Copyright (C) 2018  Serge Bazanski <q3k@q3k.org>
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

#include <QAction>
#include <QCoreApplication>
#include <QFileDialog>
#include <QGridLayout>
#include <QIcon>
#include <QImageWriter>
#include <QInputDialog>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <fstream>
#include "designwidget.h"
#include "fpgaviewwidget.h"
#include "timingwidget.h"
#include "json_frontend.h"
#include "jsonwrite.h"
#include "log.h"
#include "mainwindow.h"
#include "pythontab.h"
#include "version.h"

static void initBasenameResource() { Q_INIT_RESOURCE(base); }

NEXTPNR_NAMESPACE_BEGIN

BaseMainWindow::BaseMainWindow(std::unique_ptr<Context> context, CommandHandler *handler, QWidget *parent)
        : QMainWindow(parent), handler(handler), ctx(std::move(context)), seed(0)
{
    initBasenameResource();
    qRegisterMetaType<std::string>();

    log_streams.clear();

    setObjectName("BaseMainWindow");
    resize(1024, 768);
    restoreGeometry(QSettings("YosysHQ", "nextpnr").value("mainwindow/geometry").toByteArray());

    task = new TaskManager();

    // Create and deploy widgets on main screen
    QWidget *centralWidget = new QWidget(this);
    QGridLayout *gridLayout = new QGridLayout(centralWidget);
    gridLayout->setSpacing(6);
    gridLayout->setContentsMargins(11, 11, 11, 11);

    QSplitter *splitter_h = new QSplitter(Qt::Horizontal, centralWidget);
    QSplitter *splitter_v = new QSplitter(Qt::Vertical, splitter_h);
    splitter_h->setObjectName("splitter_h");
    splitter_v->setObjectName("splitter_v");
    splitter_h->addWidget(splitter_v);

    gridLayout->addWidget(splitter_h, 0, 0, 1, 1);

    setCentralWidget(centralWidget);

    designview = new DesignWidget();
    timingview = new TimingWidget();
    rightTabWidget = new QTabWidget();
    rightTabWidget->setMinimumWidth(300);
    rightTabWidget->addTab(designview, "Design");
    rightTabWidget->addTab(timingview, "Paths");
    splitter_h->addWidget(rightTabWidget);

    tabWidget = new QTabWidget();

    console = new PythonTab();
    tabWidget->addTab(console, "Console");

    centralTabWidget = new QTabWidget();
    centralTabWidget->setTabsClosable(true);

    fpgaView = new FPGAViewWidget();
    centralTabWidget->addTab(fpgaView, "Device");
    centralTabWidget->tabBar()->setTabButton(0, QTabBar::RightSide, 0);
    centralTabWidget->tabBar()->setTabButton(0, QTabBar::LeftSide, 0);

    splitter_v->addWidget(centralTabWidget);
    splitter_v->addWidget(tabWidget);

    // Connect Worker
    connect(task, &TaskManager::log, this, &BaseMainWindow::writeInfo);
    connect(task, &TaskManager::pack_finished, this, &BaseMainWindow::pack_finished);
    connect(task, &TaskManager::place_finished, this, &BaseMainWindow::place_finished);
    connect(task, &TaskManager::route_finished, this, &BaseMainWindow::route_finished);
    connect(task, &TaskManager::taskCanceled, this, &BaseMainWindow::taskCanceled);
    connect(task, &TaskManager::taskStarted, this, &BaseMainWindow::taskStarted);
    connect(task, &TaskManager::taskPaused, this, &BaseMainWindow::taskPaused);

    // Events for context change
    connect(this, &BaseMainWindow::contextChanged, task, &TaskManager::contextChanged);
    connect(this, &BaseMainWindow::contextChanged, console, &PythonTab::newContext);
    connect(this, &BaseMainWindow::contextChanged, fpgaView, &FPGAViewWidget::newContext);
    connect(this, &BaseMainWindow::contextChanged, designview, &DesignWidget::newContext);
    connect(this, &BaseMainWindow::contextChanged, timingview, &TimingWidget::newContext);

    // Catch close tab events
    connect(centralTabWidget, &QTabWidget::tabCloseRequested, this, &BaseMainWindow::closeTab);

    // Propagate events from design view to device view
    connect(designview, &DesignWidget::selected, fpgaView, &FPGAViewWidget::onSelectedArchItem);
    connect(designview, &DesignWidget::zoomSelected, fpgaView, &FPGAViewWidget::zoomSelected);
    connect(designview, &DesignWidget::highlight, fpgaView, &FPGAViewWidget::onHighlightGroupChanged);
    connect(designview, &DesignWidget::hover, fpgaView, &FPGAViewWidget::onHoverItemChanged);
    connect(timingview, &TimingWidget::selected, fpgaView, &FPGAViewWidget::onSelectedArchItem);
    connect(timingview, &TimingWidget::zoomSelected, fpgaView, &FPGAViewWidget::zoomSelected);

    // Click event on device view
    connect(fpgaView, &FPGAViewWidget::clickedBel, designview, &DesignWidget::onClickedBel);
    connect(fpgaView, &FPGAViewWidget::clickedWire, designview, &DesignWidget::onClickedWire);
    connect(fpgaView, &FPGAViewWidget::clickedPip, designview, &DesignWidget::onClickedPip);

    // Update tree event
    connect(this, &BaseMainWindow::updateTreeView, designview, &DesignWidget::updateTree);
    connect(this, &BaseMainWindow::updateTreeView, timingview, &TimingWidget::designChanged);

    createMenusAndBars();

    QSettings settings("YosysHQ", "nextpnr");
    for (auto splitter : findChildren<QSplitter *>())
        if (!splitter->objectName().isEmpty())
            splitter->restoreState(settings.value("mainwindow/" + splitter->objectName()).toByteArray());
}

BaseMainWindow::~BaseMainWindow() { delete task; }

void BaseMainWindow::closeEvent(QCloseEvent *event)
{
    QSettings settings("YosysHQ", "nextpnr");
    settings.setValue("mainwindow/geometry", saveGeometry());
    for (auto splitter : findChildren<QSplitter *>())
        if (!splitter->objectName().isEmpty())
            settings.setValue("mainwindow/" + splitter->objectName(), splitter->saveState());
    QMainWindow::closeEvent(event);
}

void BaseMainWindow::closeTab(int index) { delete centralTabWidget->widget(index); }

void BaseMainWindow::writeInfo(std::string text) { console->info(text); }

void BaseMainWindow::about()
{
    QString msg;
    QTextStream out(&msg);
    out << "nextpnr-" << NPNR_STRINGIFY_MACRO(ARCHNAME) << "\n";
    out << "Version " << GIT_DESCRIBE_STR;
    QMessageBox::information(this, "About nextpnr", msg);
}

void BaseMainWindow::createMenusAndBars()
{
    // File menu / project toolbar actions
    QAction *actionExit = new QAction("Exit", this);
    actionExit->setIcon(QIcon(":/icons/resources/exit.png"));
    actionExit->setShortcuts(QKeySequence::Quit);
    actionExit->setStatusTip("Exit the application");
    connect(actionExit, &QAction::triggered, this, &BaseMainWindow::close);

    // Help menu actions
    QAction *actionAbout = new QAction("About", this);
    connect(actionAbout, &QAction::triggered, this, &BaseMainWindow::about);

    // Gile menu options
    actionNew = new QAction("New", this);
    actionNew->setIcon(QIcon(":/icons/resources/new.png"));
    actionNew->setShortcuts(QKeySequence::New);
    actionNew->setStatusTip("New project");
    connect(actionNew, &QAction::triggered, this, &BaseMainWindow::new_proj);

    actionLoadJSON = new QAction("Open JSON", this);
    actionLoadJSON->setIcon(QIcon(":/icons/resources/open_json.png"));
    actionLoadJSON->setStatusTip("Open an existing JSON file");
    actionLoadJSON->setEnabled(true);
    connect(actionLoadJSON, &QAction::triggered, this, &BaseMainWindow::open_json);

    actionSaveJSON = new QAction("Save JSON", this);
    actionSaveJSON->setIcon(QIcon(":/icons/resources/save_json.png"));
    actionSaveJSON->setStatusTip("Write to JSON file");
    actionSaveJSON->setEnabled(true);
    connect(actionSaveJSON, &QAction::triggered, this, &BaseMainWindow::save_json);

    // Design menu options
    actionPack = new QAction("Pack", this);
    actionPack->setIcon(QIcon(":/icons/resources/pack.png"));
    actionPack->setStatusTip("Pack current design");
    actionPack->setEnabled(false);
    connect(actionPack, &QAction::triggered, this, &BaseMainWindow::pack);

    actionPlace = new QAction("Place", this);
    actionPlace->setIcon(QIcon(":/icons/resources/place.png"));
    actionPlace->setStatusTip("Place current design");
    actionPlace->setEnabled(false);
    connect(actionPlace, &QAction::triggered, this, &BaseMainWindow::place);

    actionRoute = new QAction("Route", this);
    actionRoute->setIcon(QIcon(":/icons/resources/route.png"));
    actionRoute->setStatusTip("Route current design");
    actionRoute->setEnabled(false);
    connect(actionRoute, &QAction::triggered, task, &TaskManager::route);

    actionOptions = new QAction("Options", this);
    actionOptions->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    actionOptions->setStatusTip("Options of packing, placing and routing");
    connect(actionOptions, &QAction::triggered, this, &BaseMainWindow::options);

    actionExecutePy = new QAction("Execute Python", this);
    actionExecutePy->setIcon(QIcon(":/icons/resources/py.png"));
    actionExecutePy->setStatusTip("Execute Python script");
    actionExecutePy->setEnabled(true);
    connect(actionExecutePy, &QAction::triggered, this, &BaseMainWindow::execute_python);

    // Worker control toolbar actions
    actionPlay = new QAction("Play", this);
    actionPlay->setIcon(QIcon(":/icons/resources/control_play.png"));
    actionPlay->setStatusTip("Continue running task");
    actionPlay->setEnabled(false);
    connect(actionPlay, &QAction::triggered, task, &TaskManager::continue_thread);

    actionPause = new QAction("Pause", this);
    actionPause->setIcon(QIcon(":/icons/resources/control_pause.png"));
    actionPause->setStatusTip("Pause running task");
    actionPause->setEnabled(false);
    connect(actionPause, &QAction::triggered, task, &TaskManager::pause_thread);

    actionStop = new QAction("Stop", this);
    actionStop->setIcon(QIcon(":/icons/resources/control_stop.png"));
    actionStop->setStatusTip("Stop running task");
    actionStop->setEnabled(false);
    connect(actionStop, &QAction::triggered, task, &TaskManager::terminate_thread);

    // Device view control toolbar actions
    QAction *actionZoomIn = new QAction("Zoom In", this);
    actionZoomIn->setIcon(QIcon(":/icons/resources/zoom_in.png"));
    connect(actionZoomIn, &QAction::triggered, fpgaView, &FPGAViewWidget::zoomIn);

    QAction *actionZoomOut = new QAction("Zoom Out", this);
    actionZoomOut->setIcon(QIcon(":/icons/resources/zoom_out.png"));
    connect(actionZoomOut, &QAction::triggered, fpgaView, &FPGAViewWidget::zoomOut);

    QAction *actionZoomSelected = new QAction("Zoom Selected", this);
    actionZoomSelected->setIcon(QIcon(":/icons/resources/shape_handles.png"));
    connect(actionZoomSelected, &QAction::triggered, fpgaView, &FPGAViewWidget::zoomSelected);

    QAction *actionZoomOutbound = new QAction("Zoom Outbound", this);
    actionZoomOutbound->setIcon(QIcon(":/icons/resources/shape_square.png"));
    connect(actionZoomOutbound, &QAction::triggered, fpgaView, &FPGAViewWidget::zoomOutbound);

    actionDisplayBel = new QAction("Enable/Disable Bels", this);
    actionDisplayBel->setIcon(QIcon(":/icons/resources/bel.png"));
    actionDisplayBel->setCheckable(true);
    actionDisplayBel->setChecked(true);
    connect(actionDisplayBel, &QAction::triggered, this, &BaseMainWindow::enableDisableDecals);

    actionDisplayWire = new QAction("Enable/Disable Wires", this);
    actionDisplayWire->setIcon(QIcon(":/icons/resources/wire.png"));
    actionDisplayWire->setCheckable(true);
    actionDisplayWire->setChecked(true);
    connect(actionDisplayWire, &QAction::triggered, this, &BaseMainWindow::enableDisableDecals);

    actionDisplayPip = new QAction("Enable/Disable Pips", this);
    actionDisplayPip->setIcon(QIcon(":/icons/resources/pip.png"));
    actionDisplayPip->setCheckable(true);
#ifdef ARCH_ECP5
    actionDisplayPip->setChecked(false);
#else
    actionDisplayPip->setChecked(true);
#endif
    connect(actionDisplayPip, &QAction::triggered, this, &BaseMainWindow::enableDisableDecals);

    actionDisplayGroups = new QAction("Enable/Disable Groups", this);
    actionDisplayGroups->setIcon(QIcon(":/icons/resources/group.png"));
    actionDisplayGroups->setCheckable(true);
    actionDisplayGroups->setChecked(true);
    connect(actionDisplayGroups, &QAction::triggered, this, &BaseMainWindow::enableDisableDecals);

    actionScreenshot = new QAction("Screenshot", this);
    actionScreenshot->setIcon(QIcon(":/icons/resources/camera.png"));
    actionScreenshot->setStatusTip("Taking a screenshot");
    connect(actionScreenshot, &QAction::triggered, this, &BaseMainWindow::screenshot);

    actionMovie = new QAction("Recording", this);
    actionMovie->setIcon(QIcon(":/icons/resources/film.png"));
    actionMovie->setStatusTip("Saving a movie");
    actionMovie->setCheckable(true);
    actionMovie->setChecked(false);
    connect(actionMovie, &QAction::triggered, this, &BaseMainWindow::saveMovie);

    actionSaveSVG = new QAction("Save SVG", this);
    actionSaveSVG->setIcon(QIcon(":/icons/resources/save_svg.png"));
    actionSaveSVG->setStatusTip("Saving a SVG");
    connect(actionSaveSVG, &QAction::triggered, this, &BaseMainWindow::saveSVG);

    // set initial state
    fpgaView->enableDisableDecals(actionDisplayBel->isChecked(), actionDisplayWire->isChecked(),
                                  actionDisplayPip->isChecked(), actionDisplayGroups->isChecked());

    // Add main menu
    menuBar = new QMenuBar();
    menuBar->setGeometry(QRect(0, 0, 1024, 27));
    setMenuBar(menuBar);
    QMenu *menuFile = new QMenu("&File", menuBar);
    QMenu *menuHelp = new QMenu("&Help", menuBar);
    menuDesign = new QMenu("&Design", menuBar);
    menuBar->addAction(menuFile->menuAction());
    menuBar->addAction(menuDesign->menuAction());
    menuBar->addAction(menuHelp->menuAction());

    // Add File menu actions
    menuFile->addAction(actionNew);
    menuFile->addAction(actionLoadJSON);
    menuFile->addAction(actionSaveJSON);
    menuFile->addSeparator();
    menuFile->addAction(actionExit);

    // Add Design menu actions
    menuDesign->addAction(actionOptions);
    menuDesign->addSeparator();
    menuDesign->addAction(actionPack);
    menuDesign->addAction(actionPlace);
    menuDesign->addAction(actionRoute);
    menuDesign->addSeparator();
    menuDesign->addAction(actionExecutePy);

    // Add Help menu actions
    menuHelp->addAction(actionAbout);

    // Main action bar
    mainActionBar = new QToolBar("Main");
    addToolBar(Qt::TopToolBarArea, mainActionBar);
    mainActionBar->addAction(actionNew);
    mainActionBar->addAction(actionLoadJSON);
    mainActionBar->addAction(actionSaveJSON);
    mainActionBar->addSeparator();
    mainActionBar->addAction(actionOptions);
    mainActionBar->addAction(actionPack);
    mainActionBar->addAction(actionPlace);
    mainActionBar->addAction(actionRoute);
    mainActionBar->addAction(actionExecutePy);

    // Add worker control toolbar
    QToolBar *workerControlToolBar = new QToolBar("Worker");
    addToolBar(Qt::TopToolBarArea, workerControlToolBar);
    workerControlToolBar->addAction(actionPlay);
    workerControlToolBar->addAction(actionPause);
    workerControlToolBar->addAction(actionStop);

    // Add device view control toolbar
    QToolBar *deviceViewToolBar = new QToolBar("Device");
    addToolBar(Qt::TopToolBarArea, deviceViewToolBar);
    deviceViewToolBar->addAction(actionZoomIn);
    deviceViewToolBar->addAction(actionZoomOut);
    deviceViewToolBar->addAction(actionZoomSelected);
    deviceViewToolBar->addAction(actionZoomOutbound);
    deviceViewToolBar->addSeparator();
    deviceViewToolBar->addAction(actionDisplayBel);
    deviceViewToolBar->addAction(actionDisplayWire);
    deviceViewToolBar->addAction(actionDisplayPip);
    deviceViewToolBar->addAction(actionDisplayGroups);
    deviceViewToolBar->addSeparator();
    deviceViewToolBar->addAction(actionScreenshot);
    deviceViewToolBar->addAction(actionMovie);
    deviceViewToolBar->addAction(actionSaveSVG);

    // Add status bar with progress bar
    statusBar = new QStatusBar();
    progressBar = new QProgressBar(statusBar);
    progressBar->setAlignment(Qt::AlignRight);
    progressBar->setMaximumSize(180, 19);
    statusBar->addPermanentWidget(progressBar);
    progressBar->setValue(0);
    progressBar->setEnabled(false);
    setStatusBar(statusBar);
}

void BaseMainWindow::enableDisableDecals()
{
    fpgaView->enableDisableDecals(actionDisplayBel->isChecked(), actionDisplayWire->isChecked(),
                                  actionDisplayPip->isChecked(), actionDisplayGroups->isChecked());
    ctx->refreshUi();
}

void BaseMainWindow::open_json()
{
    QString fileName = QFileDialog::getOpenFileName(this, QString("Open JSON"), QString(), QString("*.json"));
    if (!fileName.isEmpty()) {
        disableActions();
        if (ctx->settings.find(ctx->id("synth")) == ctx->settings.end()) {
            ArchArgs chipArgs = ctx->args;
            std::unique_ptr<Context> new_ctx = std::unique_ptr<Context>(new Context(chipArgs));
#ifdef ARCH_HIMBAECHEL
            new_ctx->uarch->with_gui = true;
            new_ctx->uarch->init(new_ctx.get());
            new_ctx->late_init();
#endif
            Q_EMIT contextChanged(new_ctx.get());
            ctx = std::move(new_ctx);
        }
        handler->load_json(ctx.get(), fileName.toStdString());
        Q_EMIT updateTreeView();
        log("Loading design successful.\n");
        updateActions();
    }
}

void BaseMainWindow::save_json()
{
    QString fileName = QFileDialog::getSaveFileName(this, QString("Save JSON"), QString(), QString("*.json"));
    if (!fileName.isEmpty()) {
        std::string fn = fileName.toStdString();
        std::ofstream f(fn);
        if (write_json_file(f, fn, ctx.get()))
            log("Saving JSON successful.\n");
        else
            log("Saving JSON failed.\n");
    }
}

void BaseMainWindow::screenshot()
{
    QString fileName = QFileDialog::getSaveFileName(this, QString("Save screenshot"), QString(), QString("*.png"));
    if (!fileName.isEmpty()) {
        QImage image = fpgaView->grabFramebuffer();
        if (!fileName.endsWith(".png"))
            fileName += ".png";
        QImageWriter imageWriter(fileName, "png");
        if (imageWriter.write(image))
            log("Saving screenshot successful.\n");
        else
            log("Saving screenshot failed.\n");
    }
}

void BaseMainWindow::saveMovie()
{
    if (actionMovie->isChecked()) {
        QString dir = QFileDialog::getExistingDirectory(this, tr("Select Movie Directory"), QDir::currentPath(),
                                                        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (!dir.isEmpty()) {
            bool ok;
            int frames =
                    QInputDialog::getInt(this, "Recording", tr("Frames to skip (1 frame = 50ms):"), 5, 0, 1000, 1, &ok);
            if (ok) {
                QMessageBox::StandardButton reply =
                        QMessageBox::question(this, "Recording", "Skip identical frames ?",
                                              QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                fpgaView->movieStart(dir, frames, (reply == QMessageBox::Yes));
            } else
                actionMovie->setChecked(false);
        } else
            actionMovie->setChecked(false);
    } else {
        fpgaView->movieStop();
    }
}

void BaseMainWindow::saveSVG()
{
    QString fileName = QFileDialog::getSaveFileName(this, QString("Save SVG"), QString(), QString("*.svg"));
    if (!fileName.isEmpty()) {
        if (!fileName.endsWith(".svg"))
            fileName += ".svg";
        bool ok;
        QString options =
                QInputDialog::getText(this, "Save SVG", tr("Save options:"), QLineEdit::Normal, "scale=500", &ok);
        if (ok) {
            try {
                ctx->writeSVG(fileName.toStdString(), options.toStdString());
                log("Saving SVG successful.\n");
            } catch (const log_execution_error_exception &ex) {
                log("Saving SVG failed.\n");
            }
        }
    }
}

void BaseMainWindow::pack_finished(bool status)
{
    disableActions();
    if (status) {
        log("Packing design successful.\n");
        Q_EMIT updateTreeView();
        updateActions();
    } else {
        log("Packing design failed.\n");
    }
}

void BaseMainWindow::place_finished(bool status)
{
    disableActions();
    if (status) {
        log("Placing design successful.\n");
        Q_EMIT updateTreeView();
        updateActions();
    } else {
        log("Placing design failed.\n");
    }
}
void BaseMainWindow::route_finished(bool status)
{
    disableActions();
    if (status) {
        log("Routing design successful.\n");
        Q_EMIT updateTreeView();
        updateActions();
    } else
        log("Routing design failed.\n");
}

void BaseMainWindow::taskCanceled()
{
    log("CANCELED\n");
    disableActions();
}

void BaseMainWindow::taskStarted()
{
    disableActions();
    actionPause->setEnabled(true);
    actionStop->setEnabled(true);
}

void BaseMainWindow::taskPaused()
{
    disableActions();
    actionPlay->setEnabled(true);
    actionStop->setEnabled(true);
}

void BaseMainWindow::pack()
{
    if (beforePack())
        Q_EMIT task->pack();
}

void BaseMainWindow::place()
{
    if (seed != 0)
        ctx->rngseed(seed);
    Q_EMIT task->place(ctx->setting<bool>("timing_driven", true));
}

PnrStep BaseMainWindow::nextStep()
{
    if (!ctx->settings.count(ctx->id("pack")))
        return PnrStep::Pack;
    if (!ctx->settings.count(ctx->id("place")))
        return PnrStep::Place;
    if (!ctx->settings.count(ctx->id("route")))
        return PnrStep::Route;
    return PnrStep::Finish;
}

std::vector<PnrOption> BaseMainWindow::pnrOptions()
{
    Context *c = ctx.get();
    // The value of a setting, without storing the default as Context::setting does
    auto read = [c](const char *key, QVariant def) {
        auto found = c->settings.find(c->id(key));
        if (found == c->settings.end())
            return def;
        if (!found->second.is_string)
            return QVariant(qlonglong(found->second.as_int64()));
        return QVariant(QString::fromStdString(found->second.as_string()));
    };
    auto option = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      PnrOption::Kind kind, QVariant def) {
        PnrOption o{step, group, label, help, kind, def};
        o.get = [read, key, def] { return read(key, def); };
        o.set = [c, key, kind](const QVariant &v) {
            if (kind == PnrOption::Int)
                c->settings[c->id(key)] = std::to_string(v.toInt());
            else if (kind == PnrOption::Float)
                c->settings[c->id(key)] = std::to_string(v.toDouble());
            else
                c->settings[c->id(key)] = v.toString().toStdString();
        };
        return o;
    };
    auto number = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      PnrOption::Kind kind, double def, double min, double max, double singleStep = 1) {
        PnrOption o = option(step, group, label, help, key, kind, def);
        o.min = min;
        o.max = max;
        o.singleStep = singleStep;
        return o;
    };
    // Unset unless given (at 0): the pass picks its default
    auto optional = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                        const char *special, double max, double singleStep) {
        PnrOption o = number(step, group, label, help, key, PnrOption::Float, 0, 0, max, singleStep);
        o.special = special;
        o.set = [c, key](const QVariant &v) {
            if (v.toDouble() == 0)
                c->settings.erase(c->id(key));
            else
                c->settings[c->id(key)] = std::to_string(v.toDouble());
        };
        return o;
    };
    // A flag stays out of the settings when off: some passes only ask whether it is there
    auto flag = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key) {
        PnrOption o{step, group, label, help, PnrOption::Flag, false};
        o.get = [c, key] { return QVariant(c->settings.count(c->id(key)) > 0); };
        o.set = [c, key](const QVariant &v) {
            if (v.toBool())
                c->settings[c->id(key)] = true;
            else
                c->settings.erase(c->id(key));
        };
        return o;
    };
    auto choice = [&](PnrStep step, const char *group, const char *label, const char *help, const char *key,
                      const std::vector<std::string> &choices, const std::string &def) {
        PnrOption o = option(step, group, label, help, key, PnrOption::Choice, QString::fromStdString(def));
        for (auto &name : choices)
            o.choices << QString::fromStdString(name);
        return o;
    };

    const PnrStep general = PnrStep::General, place = PnrStep::Place, route = PnrStep::Route;
    std::vector<PnrOption> options;

    PnrOption freq = number(general, "Timing", "Target frequency", "MHz, for clocks without a constraint",
                            "target_freq", PnrOption::Float, 12, 0.1, 2000);
    freq.get = [read] { return read("target_freq", 12e6).toDouble() / 1e6; };
    freq.set = [c](const QVariant &v) { c->settings[c->id("target_freq")] = std::to_string(v.toDouble() * 1e6); };
    options.push_back(freq);
    options.push_back(flag(general, "Timing", "Allow failing timing", "Not meeting a constraint is no error",
                           "timing/allowFail"));
    options.push_back(flag(general, "Timing", "Ignore loops", "Combinational loops are left out of the analysis",
                           "timing/ignoreLoops"));
    options.push_back(flag(general, "Timing", "Ignore clock relations",
                           "Paths between different clocks are not checked", "timing/ignoreRelClk"));
    options.push_back(number(general, "Run", "Threads", "For the passes that run in parallel", "threads",
                             PnrOption::Int, 8, 1, 256));

    options.push_back(choice(place, "Placer", "Placer", "heap: analytic, sa: annealing, static: electrostatic",
                             "placer", Arch::availablePlacers, Arch::defaultPlacer));
    PnrOption tmdriv{place, "Placer", "Timing driven", "Weigh the critical connections", PnrOption::Flag, true};
    tmdriv.get = [read] { return read("timing_driven", true).toBool(); };
    tmdriv.set = [c](const QVariant &v) { c->settings[c->id("timing_driven")] = std::to_string(v.toBool()); };
    options.push_back(tmdriv);
    PnrOption seed_option{place, "Placer", "Seed", "Restarts the random numbers before placing", PnrOption::Int, 0};
    seed_option.max = 1 << 30;
    seed_option.special = "unchanged";
    seed_option.get = [this] { return seed; };
    seed_option.set = [this](const QVariant &v) { seed = v.toInt(); };
    options.push_back(seed_option);

    options.push_back(number(place, "Heap placer", "Alpha", "Pull towards the spread-out positions",
                             "placerHeap/alpha", PnrOption::Float, 0.1, 0, 10, 0.05));
    options.push_back(number(place, "Heap placer", "Beta", "Share of the timing weight kept per iteration",
                             "placerHeap/beta", PnrOption::Float, 0.9, 0, 1, 0.05));
    options.push_back(number(place, "Heap placer", "Criticality exponent", "Higher favours the most critical",
                             "placerHeap/criticalityExponent", PnrOption::Int, 2, 0, 20));
    options.push_back(number(place, "Heap placer", "Timing weight", "Weight of the critical connections",
                             "placerHeap/timingWeight", PnrOption::Int, 10, 0, 1000));
    options.push_back(number(place, "Heap placer", "Placement timeout", "Divides the tries per cell in legalising",
                             "placerHeap/cellPlacementTimeout", PnrOption::Int, 8, 1, 1000));
    options.push_back(flag(place, "Heap placer", "No control sets", "Ignore which cells share clock and enable",
                           "placerHeap/noCtrlSet"));
    options.push_back(flag(place, "Heap placer", "Parallel refinement", "Experimental parallel annealing afterwards",
                           "placerHeap/parallelRefine"));

    options.push_back(number(place, "Annealing placer", "Constraint weight", "Weight of relative placement",
                             "placer1/constraintWeight", PnrOption::Float, 10, 0, 1000));
    options.push_back(number(place, "Annealing placer", "Start temperature", "Higher moves more at the start",
                             "placer1/startTemp", PnrOption::Float, 1, 0, 100, 0.1));
    options.push_back(number(place, "Annealing placer", "Density weight", "Penalty on crowded pins, 0: off",
                             "placer1/densityWeight", PnrOption::Float, 0, 0, 100, 0.1));
    options.push_back(number(place, "Annealing placer", "Density pins", "Pins per tile the penalty starts above",
                             "placer1/densityPins", PnrOption::Float, 8, 0, 100));
    options.push_back(number(place, "Annealing placer", "Density bin", "Tiles per side of the bins counted",
                             "placer1/densityBin", PnrOption::Int, 4, 1, 64));

    options.push_back(choice(route, "Router", "Router", "router1: classic, router2: negotiated congestion",
                             "router", Arch::availableRouters, Arch::defaultRouter));
    options.push_back(flag(route, "Router", "Timing-driven rip-up", "Also rip up critical nets", "router/tmg_ripup"));
    options.push_back(flag(route, "Router", "Alternative weights", "Other congestion weights (router2)",
                           "router2/alt-weights"));
    options.push_back(optional(route, "Router", "Estimate weight", "Weight of the delay estimate in the search",
                               "router2/estimateWeight", "default", 10, 0.05));
    options.push_back(optional(route, "Router", "Critical estimate weight",
                               "The same for the most critical connections", "router2/criticalEstimateWeight",
                               "as the others", 10, 0.05));
    return options;
}

void BaseMainWindow::options()
{
    PnrOptionsDialog dialog(pnrOptions(), nextStep(), this);
    if (dialog.exec() != QDialog::Accepted)
        return;
    auto changed = dialog.apply();
    if (changed.empty())
        return;
    optionsChanged();
    for (auto &line : changed)
        log("Option %s\n", line.c_str());
}

void BaseMainWindow::disableActions()
{
    actionLoadJSON->setEnabled(true);
    actionPack->setEnabled(false);
    actionPlace->setEnabled(false);
    actionRoute->setEnabled(false);
    actionOptions->setEnabled(false);

    actionExecutePy->setEnabled(true);

    actionPlay->setEnabled(false);
    actionPause->setEnabled(false);
    actionStop->setEnabled(false);

    onDisableActions();
}

void BaseMainWindow::updateActions()
{
    if (ctx->settings.find(ctx->id("pack")) == ctx->settings.end())
        actionPack->setEnabled(true);
    else if (ctx->settings.find(ctx->id("place")) == ctx->settings.end()) {
        actionPlace->setEnabled(true);
    } else if (ctx->settings.find(ctx->id("route")) == ctx->settings.end())
        actionRoute->setEnabled(true);
    actionOptions->setEnabled(true);

    onUpdateActions();
}

void BaseMainWindow::execute_python()
{
    QString fileName = QFileDialog::getOpenFileName(this, QString("Execute Python"), QString(), QString("*.py"));
    if (!fileName.isEmpty()) {
        console->execute_python(fileName.toStdString());
    }
}

void BaseMainWindow::notifyChangeContext() { Q_EMIT contextChanged(ctx.get()); }

NEXTPNR_NAMESPACE_END
