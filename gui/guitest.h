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

/*
 * Scripted GUI test driver.
 *
 * When the environment variable NEXTPNR_GUI_TEST=<script file> is set, the main window runs the
 * script once the event loop has started (window shown, design loaded), logs every step to stderr
 * ("[guitest] ...") and quits: exit code 0 when all steps passed, 1 on the first failing step
 * (the script stops there), 2 if the script cannot be read. Input is injected through Qt's window
 * system interface (like real mouse / keyboard input, incl. shortcuts and keyboard modifiers), so
 * the window needs no focus on the real screen; run it on an invisible desktop with
 * tools/run_gui_test.ps1 (see tools/hidden_desktop.ps1). Actions are injected asynchronously, so
 * modal context menus and dialogs do not block the script: the following steps handle them.
 * A crash is logged with exception code and backtrace (symbolized if the .pdb is next to the exe).
 *
 * Script: one command per line, words separated by blanks, '#' starts a comment line. Names (bels,
 * wires, pips, cells, nets) are written as in the GUI trees. x/y without a unit are FPGA-view
 * widget pixel coordinates (0,0 = top left of the device view). Modifiers: any of ctrl, shift, alt.
 *
 *   delay <ms>                 pause between steps (default 150)
 *   wait <ms>                  pause once
 *   waitidle [timeout_ms]      wait until no pack/place/route task runs (default 600000)
 *   resize <w> <h>             resize the main window
 *   shot <file.png>            main window incl. the GL view and an open popup menu
 *   shotview <file.png>        the device view only
 *   view <x> <y> <dist>        centre the view on world coordinates, camera distance <dist>
 *                              (smaller = closer; the GUI's zoom_)
 *   zoomfit                    whole device
 *   zoombel|zoomwire|zoompip|zoomcell <name> [dist]
 *                              centre on the element (cell: its bel); default distance shows a
 *                              few times its size
 *   move <x> <y>               mouse move (hover; drags with the buttons held by press)
 *   press <x> <y> [left|right|middle] [mods]   / release <x> <y> [...]
 *                              a button down / up alone, for slow drags step by step
 *   click <x> <y> [left|right|middle] [mods]
 *   dclick <x> <y> [mods]      double click (left)
 *   clickbel|clickwire|clickpip|clickcell <name> [left|right] [mods]
 *                              click a point where the element is the first pick candidate (pans
 *                              the view if it is not in view); hoverbel ... only moves there
 *   clickcand|hovercand <dst wire or pip> [left|right] [mods]
 *                              route mode: a point where that candidate pip (downhill of the head)
 *                              is the one a click takes
 *   route                      print the route mode state (net, head, candidates by class)
 *   drag <x1> <y1> <x2> <y2> [left|right|middle] [mods]
 *   dragbel <from_bel> <to_bel> [mods]   (both must fit into the view at the current zoom)
 *   key <keyseq>               e.g. key E, key Ctrl+Z, key Esc, key Shift+Left (QKeySequence
 *                              syntax; an enabled QAction with this shortcut is triggered,
 *                              otherwise the key goes to the focus widget)
 *   type <text>                type text into the focus widget
 *   focus view|tree|console    give the keyboard focus to that widget
 *   action <text>              trigger the QAction with this text (without '&', case-insensitive;
 *                              a prefix is enough if unique)
 *   selectcell <name> [ctrl]   click the cell's row in the Cells tree like a user (arms it in edit
 *                              mode; ctrl: add it to the selection); selectnet <name> selects a net
 *                              in the Nets tree (programmatically, like "Show in tree")
 *   menu <text>                pick the entry of the open popup / context menu starting with text
 *                              (waits up to 10 s for the menu; submenus: menu A > B)
 *   assert_menu <text> / assert_menu_disabled <text>
 *                              the open menu has an enabled / a disabled entry starting with text
 *                              (logs its tooltip); assert_nomenu <text>: has no such entry
 *   menuclose                  close open popup menus
 *   dialog [value]             set the value of the open input dialog (if any) and accept it,
 *                              or accept the open message box / dialog
 *                              (other dialogs: the values, separated by blanks, go into its spin
 *                              boxes in order, e.g. dialog 2 -1 for "Move selection by")
 *   dialog_cancel              reject the open dialog
 *   check <text> on|off        set a checkable button (e.g. a checkbox) starting with text
 *   savejson <file.json>       write the design as JSON (like File > Save), e.g. to check it without the
 *                              GUI afterwards
 *   button <text>              click the visible push button whose text starts with text (fails if
 *                              it is disabled)
 *   settext <objectName> <text> set the text of a line edit (e.g. routeTarget in the route dock)
 *   dock cands|steps|sinks [text]
 *                              route dock lists: without text, log the rows; with text, activate
 *                              the row with a column starting with text (cands / sinks: like a
 *                              double-click) or click the session pip row containing text (steps)
 *   status                     print the status bar texts (message, last message, selection,
 *                              placement)
 *   assert_status <substring>  one of these texts contains substring
 *   assert_nostatus <substring> none of them contains it
 *   py <python statement>      run in the embedded Python interpreter (ctx is the design); prints
 *                              the output, fails on an exception
 *   assert_py <python expr>    fails unless the expression is true
 *   echo <text>                log text
 *   quit [code]                stop here (exit code, default 0)
 */

#ifndef GUITEST_H
#define GUITEST_H

#include <QObject>
#include <QPointF>
#include <QStringList>
#include "nextpnr.h"

class QWidget;
class QMenu;

NEXTPNR_NAMESPACE_BEGIN

class BaseMainWindow;

class GuiTest : public QObject
{
    Q_OBJECT
  public:
    // Starts the script of NEXTPNR_GUI_TEST (if set) once the event loop runs
    static void startIfRequested(BaseMainWindow *win);

  private:
    GuiTest(BaseMainWindow *win, const QString &script);
    void scheduleNext(int ms);
    void step();
    // Executes one command; returns false on failure (error set). May set nextDelay_.
    bool exec(const QStringList &w, const QString &line);
    void finish(int code);
    void log(const QString &s);

    // Helpers
    QWidget *view();
    bool parseMods(const QStringList &w, int from, Qt::MouseButton &button, Qt::KeyboardModifiers &mods);
    void mouse(QWidget *target, QPointF pos, Qt::MouseButtons state, Qt::MouseButton button, int type,
               Qt::KeyboardModifiers mods);
    void click(QPointF pos, Qt::MouseButton button, Qt::KeyboardModifiers mods, bool dbl = false);
    void drag(QPointF a, QPointF b, Qt::MouseButton button, Qt::KeyboardModifiers mods);
    bool elementPoint(const QString &kind, const QString &name, QPointF &pos);
    bool zoomElement(const QString &kind, const QString &name, float dist);
    QMenu *popupMenu();
    QStringList statusTexts();
    bool grabWindow(const QString &file, bool viewOnly);
    bool runPython(const QString &code, QString &out);

    BaseMainWindow *win_;
    QStringList lines_;
    int pc_ = 0;
    int delay_ = 150;
    int nextDelay_ = -1;
    // Waiting: until this time (ms since start), for idle / a popup menu
    qint64 waitUntil_ = 0;
    int waitKind_ = 0; // 0 none, 1 idle, 2 popup menu
    QString error_;
    QString lastMessage_;
    // Buttons held down by press (until release), for the state of mouse moves
    Qt::MouseButtons pressed_ = Qt::NoButton;
    bool done_ = false;
};

NEXTPNR_NAMESPACE_END

#endif // GUITEST_H
