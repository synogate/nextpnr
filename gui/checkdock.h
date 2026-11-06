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
 * "Design check" dock: what is left to do / wrong while placing and routing by hand:
 *  - unplaced cells,
 *  - nets with arcs not routed (or partially: the sink's chain of pips does not reach the source),
 *  - conflicts: a source / sink wire of a net bound to another net, a wire in net->wires bound to
 *    another net, a bound pip the arch no longer allows for the net (checkPipAvailForNet),
 *  - dangling routing: wires driven by a pip whose source wire is not part of the net,
 *  - cells on bels whose location is invalid (isBelLocationValid).
 * Counts in the header, at most MAX_ENTRIES entries per category. Refreshed after edits (debounced,
 * only while the dock is visible, else when it is shown again) and by the Refresh button.
 * Double-click / Enter: select the element and zoom to it (see the focus* signals).
 */

#ifndef CHECKDOCK_H
#define CHECKDOCK_H

#include <QDockWidget>
#include "nextpnr.h"

class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

NEXTPNR_NAMESPACE_BEGIN

class EditStack;

class CheckDock : public QDockWidget
{
    Q_OBJECT

  public:
    CheckDock(EditStack *es, QWidget *parent = nullptr);
    static constexpr int MAX_ENTRIES = 500;
    static constexpr int REFRESH_DELAY_MS = 400;

  public Q_SLOTS:
    void setContext(Context *ctx);
    // Refresh once REFRESH_DELAY_MS after the last call (if visible; else when shown)
    void scheduleRefresh();
    void refresh();

  Q_SIGNALS:
    // An unplaced cell: arm it for placement; a placed one: select it and zoom to it
    void focusCell(IdString cell);
    // Select the net (net view) and zoom to it
    void focusNet(IdString net);
    void focusWire(WireId wire);
    void focusPip(PipId pip);

  private:
    void activate(QTreeWidgetItem *item);

    Context *ctx = nullptr;
    EditStack *es;
    QLabel *header;
    QTreeWidget *list;
    QPushButton *btnRefresh;
    QTimer *timer;
    bool dirty = true;
    // Expanded categories (by index), kept over refreshes
    std::vector<bool> expanded;
    bool firstFill = true;
};

NEXTPNR_NAMESPACE_END

#endif // CHECKDOCK_H
