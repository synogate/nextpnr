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

#include "pinswidget.h"

#include <QCollator>
#include <QHeaderView>
#include <QVBoxLayout>
#include <algorithm>
#include <mutex>
#include <set>
#include <unordered_set>

NEXTPNR_NAMESPACE_BEGIN

namespace {

// What an item of the list stands for
enum
{
    PORT_ITEM,
    CELL_ITEM
};
const int KindRole = Qt::UserRole, NameRole = Qt::UserRole + 1, NetRole = Qt::UserRole + 2;

enum
{
    COL_NAME,
    COL_TYPE,
    COL_LOCATION,
    COL_NET,
    COL_SETTINGS
};

QString str(const std::string &s) { return QString::fromStdString(s); }

QString propertyText(const Property &p)
{
    if (p.is_string)
        return str(p.as_string());
    if (p.str.size() <= 64 && p.str.find_first_of("xz") == std::string::npos)
        return QString::number(p.as_int64());
    return str(p.to_string());
}

// The pad an IO cell is constrained to by its parameters, if any
QString padOf(const CellInfo *ci)
{
    for (const char *key : {"PIN_NAME", "LOC", "PIN_NAME_P", "PACKAGE_PIN"}) {
        for (auto &p : ci->params)
            if (p.first.str(ci->ctx) == key && p.second.is_string && !p.second.as_string().empty() &&
                p.second.as_string() != "UNPLACED")
                return str(p.second.as_string());
        for (auto &a : ci->attrs)
            if (a.first.str(ci->ctx) == key && a.second.is_string && !a.second.as_string().empty() &&
                a.second.as_string() != "UNPLACED")
                return str(a.second.as_string());
    }
    return QString();
}

// The settings of an IO cell: its parameters, leaving out the pad
QString settingsOf(const CellInfo *ci)
{
    QStringList list;
    for (auto &p : ci->params) {
        std::string key = p.first.str(ci->ctx);
        if (key == "PIN_NAME" || key == "LOC" || key == "PIN_NAME_P" || key == "PIN_NAME_N" || key == "PACKAGE_PIN")
            continue;
        list << str(key) + "=" + propertyText(p.second);
    }
    return list.join(", ");
}

const char *directionName(PortType type) { return type == PORT_IN ? "input" : type == PORT_OUT ? "output" : "inout"; }

} // namespace

PinsWidget::PinsWidget(QWidget *parent) : QWidget(parent)
{
    summary = new QLabel(this);
    summary->setObjectName("pinsSummary");
    summary->setWordWrap(true);
    filter = new QLineEdit(this);
    filter->setObjectName("pinsFilter");
    filter->setPlaceholderText("Filter (name, location, net)");
    filter->setClearButtonEnabled(true);

    pinList = new QTreeWidget(this);
    pinList->setObjectName("pinList");
    pinList->setColumnCount(5);
    pinList->setHeaderLabels({"Pin", "Type", "Location", "Net", "Settings"});
    pinList->setUniformRowHeights(true);
    pinList->setAllColumnsShowFocus(true);
    pinList->setSelectionMode(QAbstractItemView::SingleSelection);
    pinList->header()->setSectionResizeMode(QHeaderView::Interactive);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->addWidget(summary);
    layout->addWidget(filter);
    layout->addWidget(pinList, 1);

    connect(filter, &QLineEdit::textChanged, this, &PinsWidget::applyFilter);
    connect(pinList, &QTreeWidget::itemSelectionChanged, this, &PinsWidget::itemSelected);
    connect(pinList, &QTreeWidget::itemActivated, this, &PinsWidget::itemActivated);
    connect(pinList, &QTreeWidget::itemExpanded, this, &PinsWidget::fitColumns);
    connect(pinList, &QTreeWidget::itemCollapsed, this, &PinsWidget::fitColumns);

    summary->setText("No design loaded.");
}

void PinsWidget::setPartitionMode(std::function<bool()> isPartition)
{
    this->isPartition = isPartition;
    designChanged();
}

void PinsWidget::setPadName(std::function<std::string(BelId, std::vector<BelId> &)> padName)
{
    this->padName = padName;
    designChanged();
}

void PinsWidget::newContext(Context *ctx)
{
    this->ctx = ctx;
    pinList->clear();
    designChanged();
}

void PinsWidget::designChanged()
{
    dirty = true;
    if (isVisible())
        refresh();
}

void PinsWidget::showEvent(QShowEvent *event)
{
    if (dirty)
        refresh();
    QWidget::showEvent(event);
}

void PinsWidget::refresh()
{
    dirty = false;
    if (!ctx) {
        pinList->clear();
        summary->setText("No design loaded.");
        return;
    }
    bool partition = isPartition && isPartition();

    // What was selected and opened, kept over the new list
    QString current, currentCell;
    if (QTreeWidgetItem *item = pinList->currentItem()) {
        QTreeWidgetItem *top = item->parent() ? item->parent() : item;
        current = top->data(COL_NAME, NameRole).toString();
        if (item->parent())
            currentCell = item->data(COL_NAME, NameRole).toString();
    }
    std::set<QString> expanded;
    for (int i = 0; i < pinList->topLevelItemCount(); i++)
        if (pinList->topLevelItem(i)->isExpanded())
            expanded.insert(pinList->topLevelItem(i)->data(COL_NAME, NameRole).toString());

    std::vector<QTreeWidgetItem *> items;
    int inputs = 0, outputs = 0, inouts = 0, placed = 0, unused = 0, stand_ins = 0;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        // A port's net is left behind when packing takes it apart; only nets still in the design count
        std::unordered_set<const NetInfo *> live;
        for (auto &net : ctx->nets)
            live.insert(net.second.get());
        auto is_buffer = [&](const CellInfo *ci) { return ci->type.str(ctx).rfind("$nextpnr_", 0) == 0; };

        for (auto &port : ctx->ports) {
            IdString name = port.first;
            const PortInfo &pi = port.second;
            switch (pi.type) {
            case PORT_IN:
                inputs++;
                break;
            case PORT_OUT:
                outputs++;
                break;
            default:
                inouts++;
            }

            // The nets of the port: before packing those of the frontend's buffer standing for it, after
            // packing the net of the port
            std::vector<NetInfo *> nets;
            auto add_net = [&](NetInfo *net) {
                if (net && live.count(net) && std::find(nets.begin(), nets.end(), net) == nets.end())
                    nets.push_back(net);
            };
            CellInfo *buffer = nullptr;
            auto fnd = ctx->cells.find(name);
            if (fnd != ctx->cells.end() && is_buffer(fnd->second.get())) {
                buffer = fnd->second.get();
                for (auto &p : buffer->ports)
                    add_net(p.second.net);
            } else {
                add_net(pi.net);
            }

            // The cells on them: in a top-level design the IO cell, in a partition the cells inside the port
            // reaches and the stand-in for the outside
            struct Pin
            {
                CellInfo *cell;
                IdString port;
                NetInfo *net;
                bool stand_in;
            };
            std::vector<Pin> pins;
            std::string stand_in_prefix = name.str(ctx) + "$partition_port";
            auto add_pin = [&](CellInfo *ci, IdString p, NetInfo *net) {
                if (!ci || ci == buffer || is_buffer(ci))
                    return;
                for (auto &have : pins)
                    if (have.cell == ci && have.port == p)
                        return;
                pins.push_back({ci, p, net, ci->name.str(ctx).rfind(stand_in_prefix, 0) == 0});
            };
            for (NetInfo *net : nets) {
                add_pin(net->driver.cell, net->driver.port, net);
                for (auto &usr : net->users)
                    add_pin(usr.cell, usr.port, net);
            }
            // A chip pin: the pad of its IO cell, and the other cells of that IO (listed after it, with no port)
            QString pad;
            if (!partition && padName) {
                size_t n = pins.size();
                for (size_t i = 0; i < n; i++) {
                    if (pins.at(i).cell->bel == BelId())
                        continue;
                    std::vector<BelId> io_bels;
                    std::string pad_name = padName(pins.at(i).cell->bel, io_bels);
                    if (pad.isEmpty())
                        pad = str(pad_name);
                    for (BelId bel : io_bels) {
                        CellInfo *other = ctx->getBoundBelCell(bel);
                        bool listed = false;
                        for (auto &have : pins)
                            listed |= have.cell == other;
                        if (other && !listed)
                            pins.push_back({other, IdString(), nullptr, false});
                    }
                }
            }

            QTreeWidgetItem *item = new QTreeWidgetItem();
            item->setData(COL_NAME, KindRole, PORT_ITEM);
            item->setData(COL_NAME, NameRole, str(name.str(ctx)));
            item->setText(COL_NAME, str(name.str(ctx)));
            item->setText(COL_TYPE, directionName(pi.type));
            if (!nets.empty())
                item->setData(COL_NAME, NetRole, str(nets.front()->name.str(ctx)));

            QStringList net_names, locations, settings;
            int inside = 0;
            const CellInfo *inside_cell = nullptr;
            bool any_placed = false, has_stand_in = false;
            for (auto &pin : pins) {
                QTreeWidgetItem *child = new QTreeWidgetItem(item);
                child->setData(COL_NAME, KindRole, CELL_ITEM);
                child->setData(COL_NAME, NameRole, str(pin.cell->name.str(ctx)));
                child->setText(COL_NAME, pin.port == IdString()
                                                 ? str(pin.cell->name.str(ctx))
                                                 : str(pin.cell->name.str(ctx) + "." + pin.port.str(ctx)));
                child->setText(COL_TYPE, str(pin.cell->type.str(ctx)));
                QString bel = pin.cell->bel != BelId() ? str(ctx->getBelName(pin.cell->bel).str(ctx)) : "not placed";
                child->setText(COL_LOCATION, bel);
                any_placed |= pin.cell->bel != BelId();
                // The nets of the cell besides the port's: what an IO cell connects the pin to inside
                QStringList others;
                if (!partition && pin.net)
                    for (auto &p : pin.cell->ports)
                        if (p.second.net && p.second.net != pin.net && live.count(p.second.net)) {
                            QString n = str(p.second.net->name.str(ctx));
                            others << QString("%1: %2").arg(str(p.first.str(ctx)), n);
                            if (!net_names.contains(n))
                                net_names << n;
                        }
                child->setText(COL_NET, others.join(", "));
                if (pin.stand_in) {
                    child->setText(COL_SETTINGS, "stand-in for the outside (removed when saved)");
                    settings << (pin.cell->bel != BelId() ? QString("stand-in at %1").arg(bel)
                                                          : QString("stand-in, not placed"));
                    has_stand_in = true;
                } else {
                    inside++;
                    inside_cell = pin.cell;
                    if (!partition) {
                        // The pad of the bel, else the one the parameters ask for (before packing: of the buffer)
                        if (pin.net) {
                            QString p = !pad.isEmpty() ? pad : padOf(pin.cell);
                            if (p.isEmpty() && buffer)
                                p = padOf(buffer);
                            QString loc = !p.isEmpty() ? p : pin.cell->bel != BelId() ? bel : "not placed";
                            if (!locations.contains(loc))
                                locations << loc;
                        }
                        QString s = settingsOf(pin.cell);
                        child->setText(COL_SETTINGS, s);
                        if (!s.isEmpty())
                            settings << s;
                        child->setToolTip(COL_SETTINGS, s);
                    }
                }
                child->setToolTip(COL_LOCATION, bel);
            }

            if (partition) {
                for (NetInfo *net : nets)
                    net_names << str(net->name.str(ctx));
                if (inside == 0)
                    locations << "not connected";
                else if (inside == 1)
                    locations << (inside_cell->bel != BelId() ? str(ctx->getBelName(inside_cell->bel).str(ctx))
                                                              : QString("not placed"));
                else
                    locations << QString("%1 cells inside").arg(inside);
            } else {
                if (pins.empty()) {
                    locations << "unused";
                    unused++;
                }
                // An IO cell straight on the pin and nothing else: the nets of the port itself
                if (net_names.isEmpty())
                    for (NetInfo *net : nets)
                        net_names << str(net->name.str(ctx));
            }
            placed += any_placed;
            stand_ins += has_stand_in;
            item->setText(COL_LOCATION, locations.join(", "));
            item->setText(COL_NET, net_names.join(", "));
            item->setText(COL_SETTINGS, settings.join("; "));
            item->setToolTip(COL_SETTINGS, settings.join("\n"));
            item->setToolTip(COL_NET, net_names.join("\n"));
            items.push_back(item);
        }
    }

    // In the natural order of the names (bus bits by number)
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(items.begin(), items.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) {
        return collator.compare(a->text(COL_NAME), b->text(COL_NAME)) < 0;
    });

    pinList->setUpdatesEnabled(false);
    pinList->blockSignals(true);
    pinList->clear();
    QTreeWidgetItem *select = nullptr;
    for (QTreeWidgetItem *item : items) {
        pinList->addTopLevelItem(item);
        QString pname = item->data(COL_NAME, NameRole).toString();
        if (expanded.count(pname))
            item->setExpanded(true);
        if (pname == current) {
            select = item;
            for (int i = 0; i < item->childCount() && !currentCell.isEmpty(); i++)
                if (item->child(i)->data(COL_NAME, NameRole).toString() == currentCell)
                    select = item->child(i);
        }
    }
    if (select)
        pinList->setCurrentItem(select);
    pinList->blockSignals(false);
    fitColumns();
    pinList->setUpdatesEnabled(true);
    applyFilter();

    int total = int(items.size());
    QString dirs = QString("%1 inputs, %2 outputs").arg(inputs).arg(outputs);
    if (inouts)
        dirs += QString(", %1 inouts").arg(inouts);
    if (partition)
        summary->setText(
                QString("<b>Partition ports</b> (the pins between this partition and the design that imports "
                        "it): %1 ports, %2%3.")
                        .arg(total)
                        .arg(dirs)
                        .arg(stand_ins ? QString("; %1 with a stand-in for the outside").arg(stand_ins) : QString()));
    else
        summary->setText(QString("<b>Chip pins</b>: %1 pins, %2; %3 placed%4.")
                                 .arg(total)
                                 .arg(dirs)
                                 .arg(placed)
                                 .arg(unused ? QString(", %1 unused").arg(unused) : QString()));
}

void PinsWidget::fitColumns()
{
    // To what is shown (rows opened too), within reason
    for (int c = 0; c < COL_SETTINGS; c++) {
        pinList->resizeColumnToContents(c);
        if (pinList->columnWidth(c) > 360)
            pinList->setColumnWidth(c, 360);
    }
}

void PinsWidget::applyFilter()
{
    QString text = filter->text().trimmed();
    for (int i = 0; i < pinList->topLevelItemCount(); i++) {
        QTreeWidgetItem *item = pinList->topLevelItem(i);
        bool show = text.isEmpty();
        for (int c = 0; c <= COL_NET && !show; c++)
            show = item->text(c).contains(text, Qt::CaseInsensitive);
        item->setHidden(!show);
    }
}

void PinsWidget::itemSelected()
{
    QList<QTreeWidgetItem *> sel = pinList->selectedItems();
    if (!ctx || sel.isEmpty())
        return;
    QTreeWidgetItem *item = sel.front();
    std::vector<DecalXY> decals;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        auto add_cell = [&](const QString &name) {
            auto fnd = ctx->cells.find(ctx->id(name.toStdString()));
            if (fnd != ctx->cells.end() && fnd->second->bel != BelId())
                decals.push_back(ctx->getBelDecal(fnd->second->bel));
        };
        if (item->data(COL_NAME, KindRole).toInt() == CELL_ITEM) {
            add_cell(item->data(COL_NAME, NameRole).toString());
        } else {
            for (int i = 0; i < item->childCount(); i++)
                add_cell(item->child(i)->data(COL_NAME, NameRole).toString());
            // The routing of the port's net, where there is any
            QString net = item->data(COL_NAME, NetRole).toString();
            if (!net.isEmpty()) {
                auto fnd = ctx->nets.find(ctx->id(net.toStdString()));
                if (fnd != ctx->nets.end())
                    for (auto &w : fnd->second->wires)
                        if (decals.size() < 4096)
                            decals.push_back(ctx->getWireDecal(w.first));
            }
        }
    }
    Q_EMIT selected(decals, false);
}

void PinsWidget::itemActivated(QTreeWidgetItem *item)
{
    if (!ctx || !item)
        return;
    if (item->data(COL_NAME, KindRole).toInt() == CELL_ITEM) {
        Q_EMIT focusCell(ctx->id(item->data(COL_NAME, NameRole).toString().toStdString()));
        return;
    }
    // A chip pin: its IO cell (one not placed yet is placed by the next click); a partition port: its net
    bool partition = isPartition && isPartition();
    if (!partition)
        for (int i = 0; i < item->childCount(); i++)
            if (item->child(i)->text(COL_SETTINGS).indexOf("stand-in") != 0) {
                Q_EMIT focusCell(ctx->id(item->child(i)->data(COL_NAME, NameRole).toString().toStdString()));
                return;
            }
    QString net = item->data(COL_NAME, NetRole).toString();
    if (!net.isEmpty())
        Q_EMIT focusNet(ctx->id(net.toStdString()));
}

NEXTPNR_NAMESPACE_END
