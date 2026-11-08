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

#ifndef PINSWIDGET_H
#define PINSWIDGET_H

#include <QLabel>
#include <QLineEdit>
#include <QTreeWidget>
#include <QWidget>
#include <functional>
#include <set>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// The pins of the design (the Pins page beside the Cells and Nets trees), in groups:
// - per partition loaded into the design (child), the pins between it and the design around it: per port the
//   net outside, the cells inside the partition it reaches and the cells outside;
// - the design's own top-level ports: in a top-level design the chip pins (pad, IO cells and settings), in a
//   design implemented as a partition the ports to the design that imports it (parent), with the cells inside
//   and, until saved, the stand-in for the outside.
// A row selected is selected in the device view; a double click zooms to it (a cell not placed yet: placed by the
// next click, which for an IO cell assigns its pad).
class PinsWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit PinsWidget(QWidget *parent = 0);

    // Set by the architecture's window (called with the context shown, which the window may not have switched
    // to yet). Whether the loaded design is a partition (else a top-level design):
    void setPartitionMode(std::function<bool(Context *)> isPartition);
    // The package pin of an IO bel, empty if none, and the other bels of that IO, whose cells are listed with the
    // pin (e.g. those holding its settings); called with the context locked:
    void setPadName(std::function<std::string(Context *, BelId, std::vector<BelId> &)> padName);
    // The pins of the partitions loaded into the design; called with the context locked:
    struct ChildPin
    {
        IdString inst, port;
        PortType dir = PORT_IN;
        IdString net;
        // false: the partition is not loaded yet (the port of its blackbox instance)
        bool imported = false;
    };
    void setChildPins(std::function<std::vector<ChildPin>(Context *)> childPins);

  Q_SIGNALS:
    void selected(std::vector<DecalXY> decals, bool keep);
    void zoomSelected();
    void focusCell(IdString cell);

  public Q_SLOTS:
    void newContext(Context *ctx);
    // After loading, packing, an edit and so on: lists the pins again
    void designChanged();

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    void refresh();
    void applyFilter();
    void fitColumns();
    void itemSelected();
    void itemActivated(QTreeWidgetItem *item);
    std::vector<DecalXY> itemDecals(QTreeWidgetItem *item);

    Context *ctx = nullptr;
    std::function<bool(Context *)> isPartition;
    std::function<std::string(Context *, BelId, std::vector<BelId> &)> padName;
    std::function<std::vector<ChildPin>(Context *)> childPins;
    // Changed while the page was hidden: listed again when shown
    bool dirty = true;
    // The groups closed by the user (the others are open)
    std::set<QString> closedGroups;

    QLabel *summary;
    QLineEdit *filter;
    QTreeWidget *pinList;
};

NEXTPNR_NAMESPACE_END

#endif // PINSWIDGET_H
