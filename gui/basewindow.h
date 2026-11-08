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

#ifndef BASEMAINWINDOW_H
#define BASEMAINWINDOW_H

#include "command.h"
#include "nextpnr.h"
#include "pnroptions.h"
#include "worker.h"

#include <QJsonValue>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QLabel>
#include <QProgressBar>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolBar>
#include "mcpserver.h"

Q_DECLARE_METATYPE(std::string)
Q_DECLARE_METATYPE(NEXTPNR_NAMESPACE_PREFIX DecalXY)

NEXTPNR_NAMESPACE_BEGIN

class PythonTab;
class EditStack;
class DesignWidget;
class TimingWidget;
class FPGAViewWidget;
class RouteDock;
class CheckDock;
class TimingDock;

class BaseMainWindow : public QMainWindow
{
    Q_OBJECT
    // Scripted GUI tests (NEXTPNR_GUI_TEST, see guitest.h)
    friend class GuiTest;

  public:
    explicit BaseMainWindow(std::unique_ptr<Context> context, CommandHandler *handler, QWidget *parent = 0);
    virtual ~BaseMainWindow();
    Context *getContext() { return ctx.get(); }
    EditStack *getEditStack() { return editStack; }
    void updateActions();
    // File > Save as partition... without the file dialog: write the design as a partition file (see
    // preparePartitionWrite); with ask, a message box asks first when that takes something out of the design.
    // Returns "" or the error (also logged).
    std::string savePartition(const QString &fileName, bool ask);

    void notifyChangeContext();
    // After a design file was loaded (Open JSON, --json): its logic reference, see CheckDock
    void afterDesignLoaded(const std::string &filename);
    // Starts the MCP server (see mcpserver.h) on that port; false if it cannot listen there
    bool startMcpServer(int port);

  protected:
    void closeEvent(QCloseEvent *event) override;
    void createMenusAndBars();
    // Window geometry and dock / toolbar layout from the last session (QSettings)
    void restoreWindowState();
    void saveWindowState();
    void disableActions();
    void enableDisableDecals();

    virtual void onDisableActions() {};
    virtual void onUpdateActions() {};
    // Before packing, e.g. to set options; false keeps it from packing
    virtual bool beforePack() { return true; }
    // The options of the place and route flow the options dialog shows, and the next step to run
    virtual std::vector<PnrOption> pnrOptions();
    virtual PnrStep nextStep();
    // After options changed
    virtual void optionsChanged() {}
    // Sets an option of the architecture as --vopt does (null: unset it); false, with the reason in
    // error, if it cannot
    virtual bool setArchOption(const std::string &key, const QJsonValue &value, QString &error);
    // Before the design is saved, e.g. to finish the routing
    virtual void beforeSave() {}
    // Before a hand edit (EditStack::setBeforeEdit), e.g. to finish the routing as well
    virtual void beforeEdit() {}
    // Saves the design to a JSON file, logging how it went
    bool writeJson(const std::string &filename);
    // Loads a design from a JSON file (File > Open JSON) into a fresh context, in place of the design there is;
    // a design saved part way through the flow continues where it was (packed, placed, routed so far)
    bool loadJson(const std::string &filename);
    // The architecture's part of savePartition (HimbaechelAPI::preparePartitionWrite); called with the context lock
    virtual std::string preparePartitionWrite(bool check_only, bool *changes_design)
    {
        return "saving a partition is not supported for this architecture";
    }

  protected Q_SLOTS:
    void writeInfo(std::string text);
    void closeTab(int index);

    virtual void new_proj() = 0;

    void open_json();
    void save_json();
    void pack();
    void save_partition();
    void place();
    void options();

    void execute_python();

    void pack_finished(bool status);
    void place_finished(bool status);
    void route_finished(bool status);

    void taskCanceled();
    void taskStarted();
    void taskPaused();

    void screenshot();
    void saveMovie();
    void saveSVG();

    void about();

    void toggleMcpServer(bool on);
    void editStateChanged();
    void setElidedText(QLabel *label, const QString &text);
    void taskIdle();

    // From the check / timing docks: select the element and zoom to it (an unplaced cell: arm it)
    void focusCell(IdString cell);
    void focusNet(IdString net);
    void focusWire(WireId wire);
    void focusPip(PipId pip);

  Q_SIGNALS:
    void contextChanged(Context *ctx);
    void updateTreeView();

  private:
    // The tools of the MCP server (mcptools.cc)
    void mcpCall(const QString &name, const QJsonObject &args, McpServer::Reply reply);
    void mcpTool(const QString &name, const QJsonObject &args, McpServer::Reply reply);
    void mcpTaskEnded(const char *outcome, bool ok);

  protected:
    // state variables
    CommandHandler *handler;
    std::unique_ptr<Context> ctx;
    TaskManager *task;
    // The seed the options dialog set last, 0 for none
    int seed;
    // Whether a task (pack, place, route, ...) runs, and how many started
    bool taskRunning = false;
    int tasksStarted = 0;

    McpServer *mcp = nullptr;
    int mcpPort = 4747;
    // The run_step tool waits for the task it started, collecting its log
    McpServer::Reply mcpTaskReply;
    std::string mcpTaskLog;
    EditStack *editStack;
    std::string currentProj;

    // main widgets
    QTabWidget *tabWidget;
    QTabWidget *centralTabWidget;
    PythonTab *console;
    DesignWidget *designview;
    TimingWidget *timingview;
    QTabWidget *rightTabWidget;
    FPGAViewWidget *fpgaView;
    RouteDock *routeDock;
    CheckDock *checkDock;
    TimingDock *timingDock;

    // Menus, bars and actions
    QMenuBar *menuBar;
    QMenu *menuDesign;
    QMenu *menuEdit;
    QStatusBar *statusBar;
    QToolBar *mainActionBar;
    QProgressBar *progressBar;
    QLabel *selectionLabel;
    // Shown while a cell is armed for placement by click
    QLabel *placementLabel;

    QAction *actionNew;
    QAction *actionLoadJSON;
    QAction *actionSaveJSON;
    QAction *actionSavePartition;

    QAction *actionPack;
    QAction *actionPlace;
    QAction *actionRoute;
    QAction *actionOptions;

    QAction *actionExecutePy;
    QAction *actionMcp;
    // (Design and View menus; not a step of the flow)
    QAction *actionRunTiming;

    QAction *actionPlay;
    QAction *actionPause;
    QAction *actionStop;

    QAction *actionDisplayBel;
    QAction *actionDisplayWire;
    QAction *actionDisplayPip;
    QAction *actionDisplayGroups;

    QAction *actionScreenshot;
    QAction *actionMovie;
    QAction *actionSaveSVG;

    QAction *actionEditMode;
    QAction *actionUndo;
    QAction *actionRedo;
    QAction *actionPlaceSelected;
    QAction *actionCancelPlacement;
    QAction *actionNextUnplaced;
    QAction *actionMoveSelection;
};

NEXTPNR_NAMESPACE_END

#endif // BASEMAINWINDOW_H
