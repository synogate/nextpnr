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
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// The pins of the design, its top-level ports: in a top-level design the chip pins (the IO cell of each port, its
// pad and settings, and the net inside), in a design implemented as a partition the ports between it and the
// design that imports it (the cells inside each port reaches, and the stand-in for the outside while it is
// placed and routed on its own). A pin selected is selected in the device view; a double click shows it there
// (an IO cell not placed yet: placed by the next click, which assigns its pad).
class PinsWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit PinsWidget(QWidget *parent = 0);

    // Whether the loaded design is a partition (set by the architecture's window); without, a top-level design
    void setPartitionMode(std::function<bool()> isPartition);
    // The package pin of an IO bel, empty if none, and the other bels of that IO, whose cells are listed with the
    // pin (e.g. those holding its settings). Set by the architecture's window; called with the context locked.
    void setPadName(std::function<std::string(BelId, std::vector<BelId> &)> padName);

  Q_SIGNALS:
    void selected(std::vector<DecalXY> decals, bool keep);
    void focusCell(IdString cell);
    void focusNet(IdString net);

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

    Context *ctx = nullptr;
    std::function<bool()> isPartition;
    std::function<std::string(BelId, std::vector<BelId> &)> padName;
    // Changed while the tab was hidden: listed again when shown
    bool dirty = true;

    QLabel *summary;
    QLineEdit *filter;
    QTreeWidget *pinList;
};

NEXTPNR_NAMESPACE_END

#endif // PINSWIDGET_H
