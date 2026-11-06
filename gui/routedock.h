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
 * "Route candidates" dock: the state of route mode (FPGAViewWidget::startRouting) as lists, so that
 * routing also works where wires have no graphics in the device view:
 *  - the head wire and every pip downhill of it (destination wire, pip, delay, state); double-click
 *    or Enter binds the pip like a click in the view,
 *  - the pips bound in this route session (click: continue from there),
 *  - the arcs of the net (double-click: shortest free path from the head to that sink),
 *  - "Route to" a wire by name, "Route arc" / "Route rest of net", lock / unlock, back, end.
 * Shown when route mode starts; View > Route candidates toggles it.
 */

#ifndef ROUTEDOCK_H
#define ROUTEDOCK_H

#include <QDockWidget>
#include "nextpnr.h"

class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QTreeWidget;

NEXTPNR_NAMESPACE_BEGIN

class EditStack;
class FPGAViewWidget;

class RouteDock : public QDockWidget
{
    Q_OBJECT

  public:
    RouteDock(FPGAViewWidget *view, EditStack *es, QWidget *parent = nullptr);

  public Q_SLOTS:
    // Rebuild the lists (coalesced: runs once from the event loop)
    void scheduleRefresh();

  private:
    void refresh();
    void activateCandidate(int row);
    void routeToTyped();

    FPGAViewWidget *view;
    EditStack *es;
    QLabel *header;
    QTreeWidget *cands;
    QListWidget *steps;
    QTreeWidget *sinks;
    QLineEdit *target;
    QPushButton *btnTo, *btnArc, *btnRest, *btnLock, *btnBack, *btnEnd;
    bool refreshPending = false;
    bool wasRouting = false;
};

NEXTPNR_NAMESPACE_END

#endif // ROUTEDOCK_H
