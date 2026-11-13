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

// Hierarchy dock: the instance tree of the design (see design_scopes.h for how cells are put into
// instances), the cells of chosen instances coloured in the device view, and what an instance uses
// of the device.

#ifndef HIERARCHYDOCK_H
#define HIERARCHYDOCK_H

#include <QColor>
#include <QDockWidget>
#include <QMap>
#include <QSet>
#include "design_scopes.h"
#include "nextpnr.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

NEXTPNR_NAMESPACE_BEGIN

class EditStack;
class FPGAViewWidget;

class HierarchyDock : public QDockWidget
{
    Q_OBJECT

  public:
    HierarchyDock(FPGAViewWidget *view, EditStack *es, QWidget *parent = nullptr);
    static constexpr int REFRESH_DELAY_MS = 400;
    // Alpha of the cell colours in the device view
    static constexpr int FILL_ALPHA = 170;

  public Q_SLOTS:
    void setContext(Context *ctx);
    // Refresh REFRESH_DELAY_MS after the last call (while the dock is open, also behind another tab; else when
    // shown)
    void scheduleRefresh();
    void refresh();
    // Colour the children of the current instance, each in its own colour, and nothing else
    void colourChildren();
    void clearColours();
    // Select the cells of the current instance in the device view and zoom to them
    void selectCurrent();

  Q_SIGNALS:
    // The instance selected (its dotted path, "" for the top level) and its cells, for the Cells tab
    void cellScopeChanged(const QString &name, const std::vector<IdString> &cells);
    // Select these cells as if chosen by hand in the Cells tab (none: clear the selection)
    void selectCellsRequested(const std::vector<IdString> &cells);

  protected:
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void rebuildTree();
    void updateUsage();
    void updateView();
    void applyFilter();
    void setColoured(int scope, bool on);
    void itemChanged(QTreeWidgetItem *item, int column);
    void contextMenu(const QPoint &pos);
    QColor nextColour() const;
    int currentScope() const;
    // A task (pack, place, route) changes the design: hands off
    bool busy() const;
    // The cells of the current instance (none for the top level)
    std::vector<IdString> currentCells() const;
    // Select them as if by hand (without zooming)
    void highlightCurrent();
    void emitScope();
    // While the tree is rebuilt: no highlighting, no scope signals
    bool quiet = false;

    Context *ctx = nullptr;
    FPGAViewWidget *view;
    EditStack *es;
    DesignScopes scopes;
    BelCensus census;
    bool haveCensus = false;
    // Colours by instance path, kept over refreshes and reloads of the same design
    QMap<QString, QColor> colours;
    // Items by scope index
    std::vector<QTreeWidgetItem *> items;
    bool filling = false;

    QLabel *header;
    QLineEdit *filter;
    QCheckBox *outlines;
    QTreeWidget *tree;
    QLabel *stats;
    QTreeWidget *usage;
    QTimer *timer;
    bool dirty = true;
};

NEXTPNR_NAMESPACE_END

#endif // HIERARCHYDOCK_H
