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
 * "Timing" dock: static timing analysis on demand (runTiming, View/Design > Run timing analysis, T),
 * on the design as it is, also partially placed / routed: arcs to or from unplaced cells are left
 * out (the header says how many cells), unrouted arcs count with the estimated delay (marked
 * "est." in the path). Shows Fmax per clock (and the cross-domain paths) and the critical path of
 * the selected clock as a list of segments with delays; double-click selects the net (routing) or
 * the cell and zooms to it. "Highlight critical path" draws the path in the device view (see
 * FPGAViewWidget::setCriticalPath). After an edit the results are marked as out of date.
 */

#ifndef TIMINGDOCK_H
#define TIMINGDOCK_H

#include <QDockWidget>
#include "nextpnr.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

NEXTPNR_NAMESPACE_BEGIN

class EditStack;

class TimingDock : public QDockWidget
{
    Q_OBJECT

  public:
    TimingDock(EditStack *es, QWidget *parent = nullptr);

  public Q_SLOTS:
    void setContext(Context *ctx);
    // Run the timing analysis now (refused while a task runs)
    void runTiming();
    // The design was edited: the results are out of date
    void designEdited();

  Q_SIGNALS:
    void focusCell(IdString cell);
    void focusNet(IdString net);
    // The path to draw in the device view (empty: none)
    void criticalPathChanged(std::vector<CriticalPath::Segment> segments);

  private:
    void showPath(int index);
    void emitHighlight();

    Context *ctx = nullptr;
    EditStack *es;
    QLabel *header;
    QPushButton *btnRun;
    QCheckBox *highlight;
    QTreeWidget *clocks;
    QTreeWidget *path;
    // The paths of the last analysis: per clock, then cross-domain
    std::vector<CriticalPath> paths;
    // "est." per path / segment: the routing segment's arc is not routed
    std::vector<std::vector<bool>> estimated;
    int current = -1;
    bool stale = false;
    QString summary;
};

NEXTPNR_NAMESPACE_END

#endif // TIMINGDOCK_H
