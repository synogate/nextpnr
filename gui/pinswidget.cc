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
#include <map>
#include <mutex>
#include <unordered_set>

NEXTPNR_NAMESPACE_BEGIN

namespace {

// What an item of the list stands for
enum
{
    GROUP_ITEM,
    PORT_ITEM,
    CELL_ITEM
};
// The kind, a name (group key, port name, cell name) and the net of a port
const int KindRole = Qt::UserRole, NameRole = Qt::UserRole + 1, NetRole = Qt::UserRole + 2;

enum
{
    COL_NAME,
    COL_TYPE,
    COL_LOCATION,
    COL_NET,
    COL_DETAILS
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

bool isPadKey(const std::string &key)
{
    return key == "PIN_NAME" || key == "LOC" || key == "PIN_NAME_P" || key == "PIN_NAME_N" || key == "PACKAGE_PIN";
}

// The pad an IO cell is constrained to by its parameters, if any
QString padOf(const CellInfo *ci)
{
    for (const char *key : {"PIN_NAME", "LOC", "PIN_NAME_P", "PACKAGE_PIN"})
        for (const dict<IdString, Property> *props : {&ci->params, &ci->attrs})
            for (auto &p : *props)
                if (p.first.str(ci->ctx) == key && p.second.is_string && !p.second.as_string().empty() &&
                    p.second.as_string() != "UNPLACED")
                    return str(p.second.as_string());
    return QString();
}

// The settings of an IO cell: its parameters, leaving out the pad
QString settingsOf(const CellInfo *ci)
{
    QStringList list;
    for (auto &p : ci->params)
        if (!isPadKey(p.first.str(ci->ctx)))
            list << str(p.first.str(ci->ctx)) + "=" + propertyText(p.second);
    return list.join(", ");
}

const char *directionName(PortType type) { return type == PORT_IN ? "input" : type == PORT_OUT ? "output" : "inout"; }

// Where some cells are: the bel of one, else how many
QString whereOf(const std::vector<const CellInfo *> &cells, const Context *ctx, const char *what)
{
    if (cells.empty())
        return QString();
    if (cells.size() == 1)
        return cells.front()->bel != BelId() ? str(ctx->getBelName(cells.front()->bel).str(ctx))
                                             : QString("not placed");
    return QString("%1 cells %2").arg(cells.size()).arg(what);
}

} // namespace

PinsWidget::PinsWidget(QWidget *parent) : QWidget(parent)
{
    summary = new QLabel(this);
    summary->setObjectName("pinsSummary");
    summary->setWordWrap(true);
    filter = new QLineEdit(this);
    filter->setObjectName("pinsFilter");
    filter->setPlaceholderText("Filter (pin, location, net)");
    filter->setClearButtonEnabled(true);

    pinList = new QTreeWidget(this);
    pinList->setObjectName("pinList");
    pinList->setColumnCount(5);
    pinList->setHeaderLabels({"Pin", "Type", "Location", "Net", "Details"});
    pinList->setUniformRowHeights(true);
    pinList->setAllColumnsShowFocus(true);
    pinList->setSelectionMode(QAbstractItemView::SingleSelection);
    pinList->header()->setSectionResizeMode(QHeaderView::Interactive);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);
    layout->addWidget(summary);
    layout->addWidget(filter);
    layout->addWidget(pinList, 1);

    connect(filter, &QLineEdit::textChanged, this, &PinsWidget::applyFilter);
    connect(pinList, &QTreeWidget::itemSelectionChanged, this, &PinsWidget::itemSelected);
    connect(pinList, &QTreeWidget::itemActivated, this, &PinsWidget::itemActivated);
    connect(pinList, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
        if (item->data(COL_NAME, KindRole).toInt() == GROUP_ITEM)
            closedGroups.erase(item->data(COL_NAME, NameRole).toString());
        fitColumns();
    });
    connect(pinList, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
        if (item->data(COL_NAME, KindRole).toInt() == GROUP_ITEM)
            closedGroups.insert(item->data(COL_NAME, NameRole).toString());
        fitColumns();
    });

    summary->setText("No design loaded.");
}

void PinsWidget::setPartitionMode(std::function<bool(Context *)> isPartition)
{
    this->isPartition = isPartition;
    designChanged();
}

void PinsWidget::setPadName(std::function<std::string(Context *, BelId, std::vector<BelId> &)> padName)
{
    this->padName = padName;
    designChanged();
}

void PinsWidget::setChildPins(std::function<std::vector<ChildPin>(Context *)> childPins)
{
    this->childPins = childPins;
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
    bool partition = isPartition && isPartition(ctx);

    // What was selected and opened, kept over the new list (by the names of the item and its parents)
    auto path = [](QTreeWidgetItem *item) {
        QStringList p;
        for (; item; item = item->parent())
            p.prepend(item->data(COL_NAME, NameRole).toString());
        return p.join('\n');
    };
    QString current = pinList->currentItem() ? path(pinList->currentItem()) : QString();
    std::set<QString> openPorts;
    for (int g = 0; g < pinList->topLevelItemCount(); g++)
        for (int i = 0; i < pinList->topLevelItem(g)->childCount(); i++)
            if (pinList->topLevelItem(g)->child(i)->isExpanded())
                openPorts.insert(path(pinList->topLevelItem(g)->child(i)));

    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);

    std::vector<QTreeWidgetItem *> groups;
    QStringList summaries;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        // A port's net is left behind when packing takes it apart; only nets still in the design count
        std::unordered_set<const NetInfo *> live;
        for (auto &net : ctx->nets)
            live.insert(net.second.get());
        auto is_buffer = [&](const CellInfo *ci) { return ci->type.str(ctx).rfind("$nextpnr_", 0) == 0; };

        auto new_group = [&](const QString &key, const QString &title) {
            QTreeWidgetItem *group = new QTreeWidgetItem();
            group->setData(COL_NAME, KindRole, GROUP_ITEM);
            group->setData(COL_NAME, NameRole, key);
            group->setText(COL_NAME, title);
            QFont font = group->font(COL_NAME);
            font.setBold(true);
            group->setFont(COL_NAME, font);
            groups.push_back(group);
            return group;
        };
        auto new_port = [&](const QString &name, PortType dir, const NetInfo *net) {
            QTreeWidgetItem *item = new QTreeWidgetItem();
            item->setData(COL_NAME, KindRole, PORT_ITEM);
            item->setData(COL_NAME, NameRole, name);
            item->setText(COL_NAME, name);
            item->setText(COL_TYPE, directionName(dir));
            if (net)
                item->setData(COL_NAME, NetRole, str(net->name.str(ctx)));
            return item;
        };
        auto cell_row = [&](QTreeWidgetItem *parent, const CellInfo *ci, IdString port, const QString &details) {
            QTreeWidgetItem *child = new QTreeWidgetItem(parent);
            child->setData(COL_NAME, KindRole, CELL_ITEM);
            child->setData(COL_NAME, NameRole, str(ci->name.str(ctx)));
            child->setText(COL_NAME,
                           port == IdString() ? str(ci->name.str(ctx)) : str(ci->name.str(ctx) + "." + port.str(ctx)));
            child->setText(COL_TYPE, str(ci->type.str(ctx)));
            child->setText(COL_LOCATION,
                           ci->bel != BelId() ? str(ctx->getBelName(ci->bel).str(ctx)) : QString("not placed"));
            child->setText(COL_DETAILS, details);
            child->setToolTip(COL_DETAILS, details);
            return child;
        };
        // In the natural order of the names (bus bits by number)
        auto add_sorted = [&](QTreeWidgetItem *group, std::vector<QTreeWidgetItem *> &ports) {
            std::sort(ports.begin(), ports.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) {
                return collator.compare(a->text(COL_NAME), b->text(COL_NAME)) < 0;
            });
            for (auto *p : ports)
                group->addChild(p);
        };
        auto count_text = [](int in, int out, int inout) {
            QString t = QString("%1 inputs, %2 outputs").arg(in).arg(out);
            if (inout)
                t += QString(", %1 inouts").arg(inout);
            return t;
        };

        // The partitions loaded into the design (children): per port the net outside, the cells inside and outside
        std::map<std::string, std::vector<ChildPin>> children;
        if (childPins)
            for (auto &pin : childPins(ctx))
                children[pin.inst.str(ctx)].push_back(pin);
        std::vector<std::string> child_names;
        for (auto &c : children)
            child_names.push_back(c.first);
        std::sort(child_names.begin(), child_names.end(),
                  [&](const std::string &a, const std::string &b) { return collator.compare(str(a), str(b)) < 0; });
        int child_pins = 0;
        bool any_imported = false;
        for (auto &inst : child_names) {
            auto &pins = children.at(inst);
            bool imported = pins.front().imported;
            any_imported |= imported;
            QTreeWidgetItem *group = new_group("partition " + str(inst), "Partition " + str(inst));
            std::string prefix = inst + ".";
            std::vector<QTreeWidgetItem *> ports;
            int in = 0, out = 0, inout = 0;
            for (auto &pin : pins) {
                (pin.dir == PORT_IN ? in : pin.dir == PORT_OUT ? out : inout)++;
                auto fnd = pin.net != IdString() ? ctx->nets.find(pin.net) : ctx->nets.end();
                const NetInfo *net = fnd != ctx->nets.end() ? fnd->second.get() : nullptr;
                QTreeWidgetItem *item = new_port(str(pin.port.str(ctx)), pin.dir, net);
                std::vector<const CellInfo *> inside, outside;
                if (net) {
                    std::vector<std::pair<const CellInfo *, IdString>> cells;
                    if (net->driver.cell)
                        cells.emplace_back(net->driver.cell, net->driver.port);
                    for (auto &usr : net->users)
                        cells.emplace_back(usr.cell, usr.port);
                    // the cells inside first
                    for (int pass = 0; pass < 2; pass++)
                        for (auto &c : cells) {
                            if (c.first->name.str(ctx) == inst)
                                continue; // the blackbox instance, before it is replaced
                            bool in_part = c.first->name.str(ctx).rfind(prefix, 0) == 0;
                            if (in_part != (pass == 0))
                                continue;
                            (in_part ? inside : outside).push_back(c.first);
                            cell_row(item, c.first, c.second, in_part ? "inside" : "outside");
                        }
                    item->setText(COL_NET, str(net->name.str(ctx)));
                }
                item->setText(COL_LOCATION, !imported        ? QString("not loaded yet")
                                            : inside.empty() ? QString("no cell inside")
                                                             : whereOf(inside, ctx, "inside"));
                item->setText(COL_DETAILS, !net              ? QString("not connected outside")
                                           : outside.empty() ? QString("outside: no cell")
                                                             : "outside: " + whereOf(outside, ctx, "outside"));
                ports.push_back(item);
            }
            add_sorted(group, ports);
            child_pins += int(pins.size());
            group->setText(COL_LOCATION, imported ? QString("%1 pins").arg(pins.size())
                                                  : QString("%1 pins, not loaded yet").arg(pins.size()));
            group->setText(COL_NET, count_text(in, out, inout));
        }
        if (!child_names.empty())
            summaries << QString("%1 partition%2 %3 (%4 pins)")
                                 .arg(child_names.size())
                                 .arg(child_names.size() == 1 ? "" : "s")
                                 .arg(any_imported ? "loaded" : "to load")
                                 .arg(child_pins);

        // The design's own ports: chip pins, or the ports of this partition to the design importing it (parent)
        if (!ctx->ports.empty()) {
            QTreeWidgetItem *group = new_group("own", partition ? "Ports to the parent design" : "Chip pins");
            std::vector<QTreeWidgetItem *> ports;
            int in = 0, out = 0, inout = 0, placed = 0, unused = 0, stand_ins = 0;
            for (auto &port : ctx->ports) {
                IdString name = port.first;
                const PortInfo &pi = port.second;
                (pi.type == PORT_IN ? in : pi.type == PORT_OUT ? out : inout)++;

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
                    const CellInfo *cell;
                    IdString port;
                    const NetInfo *net;
                    bool stand_in;
                };
                std::vector<Pin> pins;
                std::string stand_in_prefix = name.str(ctx) + "$partition_port";
                auto add_pin = [&](const CellInfo *ci, IdString p, const NetInfo *net) {
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
                // A chip pin: the pad of its IO cell, and the other cells of that IO (listed after it, no port)
                QString pad;
                if (!partition && padName) {
                    size_t n = pins.size();
                    for (size_t i = 0; i < n; i++) {
                        if (pins.at(i).cell->bel == BelId())
                            continue;
                        std::vector<BelId> io_bels;
                        std::string pad_name = padName(ctx, pins.at(i).cell->bel, io_bels);
                        if (pad.isEmpty())
                            pad = str(pad_name);
                        for (BelId bel : io_bels) {
                            const CellInfo *other = ctx->getBoundBelCell(bel);
                            bool listed = false;
                            for (auto &have : pins)
                                listed |= have.cell == other;
                            if (other && !listed)
                                pins.push_back({other, IdString(), nullptr, false});
                        }
                    }
                }

                QTreeWidgetItem *item = new_port(str(name.str(ctx)), pi.type, nets.empty() ? nullptr : nets.front());
                QStringList net_names, locations, details;
                std::vector<const CellInfo *> inside;
                bool any_placed = false, has_stand_in = false;
                for (auto &pin : pins) {
                    QString bel = pin.cell->bel != BelId() ? str(ctx->getBelName(pin.cell->bel).str(ctx))
                                                           : QString("not placed");
                    any_placed |= pin.cell->bel != BelId();
                    QString d;
                    if (pin.stand_in) {
                        d = "stand-in for the outside (removed when saved)";
                        details << (pin.cell->bel != BelId() ? QString("stand-in at %1").arg(bel)
                                                             : QString("stand-in, not placed"));
                        has_stand_in = true;
                    } else if (!partition) {
                        d = settingsOf(pin.cell);
                        if (!d.isEmpty())
                            details << d;
                        // The pad of the bel, else the one the parameters ask for (before packing: the buffer's)
                        if (pin.net) {
                            QString p = !pad.isEmpty() ? pad : padOf(pin.cell);
                            if (p.isEmpty() && buffer)
                                p = padOf(buffer);
                            QString loc = !p.isEmpty() ? p : bel;
                            if (!locations.contains(loc))
                                locations << loc;
                        }
                    } else {
                        inside.push_back(pin.cell);
                        d = "inside";
                    }
                    QTreeWidgetItem *child = cell_row(item, pin.cell, pin.port, d);
                    // The nets of an IO cell besides the pin's: what it connects the pin to inside
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
                }
                if (partition) {
                    for (NetInfo *net : nets)
                        net_names << str(net->name.str(ctx));
                    locations << (inside.empty() ? QString("no cell inside") : whereOf(inside, ctx, "inside"));
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
                item->setText(COL_DETAILS, details.join("; "));
                item->setToolTip(COL_DETAILS, details.join("\n"));
                item->setToolTip(COL_NET, net_names.join("\n"));
                ports.push_back(item);
            }
            add_sorted(group, ports);
            int total = int(ports.size());
            group->setText(COL_NET, count_text(in, out, inout));
            if (partition) {
                group->setText(COL_LOCATION, QString("%1 ports").arg(total));
                if (stand_ins)
                    group->setText(COL_DETAILS, QString("%1 with a stand-in for the outside").arg(stand_ins));
                summaries << QString("%1 ports to the parent design").arg(total);
            } else {
                group->setText(COL_LOCATION, QString("%1 pins, %2 placed").arg(total).arg(placed));
                if (unused)
                    group->setText(COL_DETAILS, QString("%1 unused").arg(unused));
                summaries << QString("%1 chip pins").arg(total);
            }
        }
    }

    pinList->setUpdatesEnabled(false);
    pinList->blockSignals(true);
    pinList->clear();
    QTreeWidgetItem *select = nullptr;
    std::function<void(QTreeWidgetItem *)> restore = [&](QTreeWidgetItem *item) {
        QString p = path(item);
        if (p == current)
            select = item;
        if (item->data(COL_NAME, KindRole).toInt() == PORT_ITEM && openPorts.count(p))
            item->setExpanded(true);
        for (int i = 0; i < item->childCount(); i++)
            restore(item->child(i));
    };
    for (QTreeWidgetItem *group : groups) {
        pinList->addTopLevelItem(group);
        group->setExpanded(!closedGroups.count(group->data(COL_NAME, NameRole).toString()));
        restore(group);
    }
    if (select)
        pinList->setCurrentItem(select);
    pinList->blockSignals(false);
    fitColumns();
    pinList->setUpdatesEnabled(true);
    applyFilter();

    summary->setText(summaries.isEmpty() ? QString("No pins.") : summaries.join("; ") + ".");
}

void PinsWidget::fitColumns()
{
    // To what is shown (rows opened too), within reason
    for (int c = 0; c < COL_DETAILS; c++) {
        pinList->resizeColumnToContents(c);
        if (pinList->columnWidth(c) > 360)
            pinList->setColumnWidth(c, 360);
    }
}

void PinsWidget::applyFilter()
{
    QString text = filter->text().trimmed();
    for (int g = 0; g < pinList->topLevelItemCount(); g++) {
        QTreeWidgetItem *group = pinList->topLevelItem(g);
        bool group_hit = text.isEmpty() || group->text(COL_NAME).contains(text, Qt::CaseInsensitive);
        bool any = false;
        for (int i = 0; i < group->childCount(); i++) {
            QTreeWidgetItem *item = group->child(i);
            bool show = group_hit;
            for (int c = 0; c <= COL_NET && !show; c++)
                show = item->text(c).contains(text, Qt::CaseInsensitive);
            item->setHidden(!show);
            any |= show;
        }
        group->setHidden(!any && !group_hit);
    }
}

std::vector<DecalXY> PinsWidget::itemDecals(QTreeWidgetItem *item)
{
    std::vector<DecalXY> decals;
    std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
    std::lock_guard<std::mutex> lock(ctx->mutex);
    std::unordered_set<std::string> seen;
    auto add_cell = [&](const QString &name) {
        if (!seen.insert(name.toStdString()).second)
            return;
        auto fnd = ctx->cells.find(ctx->id(name.toStdString()));
        if (fnd != ctx->cells.end() && fnd->second->bel != BelId() && decals.size() < 4096)
            decals.push_back(ctx->getBelDecal(fnd->second->bel));
    };
    auto add_port = [&](QTreeWidgetItem *port) {
        for (int i = 0; i < port->childCount(); i++)
            add_cell(port->child(i)->data(COL_NAME, NameRole).toString());
        // The routing of the port's net, where there is any
        QString net = port->data(COL_NAME, NetRole).toString();
        if (!net.isEmpty()) {
            auto fnd = ctx->nets.find(ctx->id(net.toStdString()));
            if (fnd != ctx->nets.end())
                for (auto &w : fnd->second->wires)
                    if (decals.size() < 4096)
                        decals.push_back(ctx->getWireDecal(w.first));
        }
    };
    switch (item->data(COL_NAME, KindRole).toInt()) {
    case CELL_ITEM:
        add_cell(item->data(COL_NAME, NameRole).toString());
        break;
    case PORT_ITEM:
        add_port(item);
        break;
    default:
        for (int i = 0; i < item->childCount(); i++)
            add_port(item->child(i));
    }
    return decals;
}

void PinsWidget::itemSelected()
{
    QList<QTreeWidgetItem *> sel = pinList->selectedItems();
    if (!ctx || sel.isEmpty())
        return;
    Q_EMIT selected(itemDecals(sel.front()), false);
}

void PinsWidget::itemActivated(QTreeWidgetItem *item)
{
    if (!ctx || !item)
        return;
    // A cell not placed yet: placed by the next click
    if (item->data(COL_NAME, KindRole).toInt() == CELL_ITEM) {
        IdString cell = ctx->id(item->data(COL_NAME, NameRole).toString().toStdString());
        bool unplaced;
        {
            std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
            std::lock_guard<std::mutex> lock(ctx->mutex);
            auto fnd = ctx->cells.find(cell);
            unplaced = fnd != ctx->cells.end() && fnd->second->bel == BelId();
        }
        if (unplaced) {
            Q_EMIT focusCell(cell);
            return;
        }
    }
    std::vector<DecalXY> decals = itemDecals(item);
    if (decals.empty())
        return;
    Q_EMIT selected(decals, false);
    Q_EMIT zoomSelected();
}

NEXTPNR_NAMESPACE_END
