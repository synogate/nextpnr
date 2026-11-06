/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <micko@yosyshq.com>
 *  Copyright (C) 2018  Serge Bazanski <q3k@q3k.org>
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

#include "designwidget.h"
#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QToolBar>
#include <QTreeWidgetItem>
#include "editcommands.h"
#include "elementinfo.h"
#include "netstatus.h"
#include "fpgaviewwidget.h"

NEXTPNR_NAMESPACE_BEGIN

using QtType = QMetaType::Type;

TreeView::TreeView(QWidget *parent) : QTreeView(parent) {}

TreeView::~TreeView() {}

void TreeView::mouseMoveEvent(QMouseEvent *event)
{
    QModelIndex index = indexAt(event->pos());
    if (index != current) {
        current = index;
        Q_EMIT hoverIndexChanged(index);
    }
    QTreeView::mouseMoveEvent(event);
}

void TreeView::leaveEvent(QEvent *event) { Q_EMIT hoverIndexChanged(QModelIndex()); }

DesignWidget::DesignWidget(QWidget *parent) : QWidget(parent), ctx(nullptr)
{
    tabWidget = new QTabWidget();

    // Add tree view
    for (int i = 0; i < 6; i++) {
        treeView[i] = new TreeView();
        treeModel[i] = new TreeModel::Model();
        treeView[i]->setModel(treeModel[i]);
        treeView[i]->setContextMenuPolicy(Qt::CustomContextMenu);
        treeView[i]->setSelectionMode(QAbstractItemView::ExtendedSelection);
        treeView[i]->viewport()->setMouseTracking(true);
        selectionModel[i] = nullptr;
    }

    tabWidget->addTab(treeView[0], "Bels");
    tabWidget->addTab(treeView[1], "Wires");
    tabWidget->addTab(treeView[2], "Pips");
    // Cells tab: the tree with a filter for unplaced cells above it
    QWidget *cellsPage = new QWidget();
    QVBoxLayout *cellsLayout = new QVBoxLayout(cellsPage);
    cellsLayout->setSpacing(2);
    cellsLayout->setContentsMargins(0, 0, 0, 0);
    unplacedOnly = new QCheckBox("Unplaced only");
    unplacedOnly->setToolTip("Show only cells that are not placed (N in edit mode: arm the next one)");
    connect(unplacedOnly, &QCheckBox::toggled, this, [this] {
        if (ctx != nullptr)
            refreshAfterEdit();
    });
    cellsLayout->addWidget(unplacedOnly);
    cellsLayout->addWidget(treeView[3]);
    tabWidget->addTab(cellsPage, "Cells");
    tabWidget->addTab(treeView[4], "Nets");
    tabWidget->addTab(treeView[5], "Groups");

    // Add property view
    variantManager = new QtVariantPropertyManager(this);
    readOnlyManager = new QtVariantPropertyManager(this);
    groupManager = new QtGroupPropertyManager(this);
    variantFactory = new QtVariantEditorFactory(this);
    propertyEditor = new QtTreePropertyBrowser(this);
    propertyEditor->setFactoryForManager(variantManager, variantFactory);
    propertyEditor->setPropertiesWithoutValueMarked(true);
    propertyEditor->show();
    propertyEditor->treeWidget()->setContextMenuPolicy(Qt::CustomContextMenu);
    propertyEditor->treeWidget()->setSelectionMode(QAbstractItemView::ExtendedSelection);
    propertyEditor->treeWidget()->viewport()->setMouseTracking(true);

    searchEdit = new QLineEdit();
    searchEdit->setClearButtonEnabled(true);
    searchEdit->addAction(QIcon(":/icons/resources/zoom.png"), QLineEdit::LeadingPosition);
    searchEdit->setPlaceholderText("Search...");
    connect(searchEdit, &QLineEdit::returnPressed, this, &DesignWidget::onSearchInserted);

    actionFirst = new QAction("", this);
    actionFirst->setIcon(QIcon(":/icons/resources/resultset_first.png"));
    actionFirst->setEnabled(false);
    connect(actionFirst, &QAction::triggered, this, [this] {
        history_ignore = true;
        history_index = 0;
        auto h = history.at(history_index);
        if (tabWidget->currentIndex() != h.first) {
            selectionModel[tabWidget->currentIndex()]->clearSelection();
            tabWidget->setCurrentIndex(h.first);
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::Select);
        } else {
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::ClearAndSelect);
        }
        updateButtons();
    });

    actionPrev = new QAction("", this);
    actionPrev->setIcon(QIcon(":/icons/resources/resultset_previous.png"));
    actionPrev->setEnabled(false);
    connect(actionPrev, &QAction::triggered, this, [this] {
        history_ignore = true;
        history_index--;
        auto h = history.at(history_index);
        if (tabWidget->currentIndex() != h.first) {
            selectionModel[tabWidget->currentIndex()]->clearSelection();
            tabWidget->setCurrentIndex(h.first);
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::Select);
        } else {
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::ClearAndSelect);
        }
        updateButtons();
    });

    actionNext = new QAction("", this);
    actionNext->setIcon(QIcon(":/icons/resources/resultset_next.png"));
    actionNext->setEnabled(false);
    connect(actionNext, &QAction::triggered, this, [this] {
        history_ignore = true;
        history_index++;
        auto h = history.at(history_index);
        if (tabWidget->currentIndex() != h.first) {
            selectionModel[tabWidget->currentIndex()]->clearSelection();
            tabWidget->setCurrentIndex(h.first);
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::Select);
        } else {
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::ClearAndSelect);
        }
        updateButtons();
    });

    actionLast = new QAction("", this);
    actionLast->setIcon(QIcon(":/icons/resources/resultset_last.png"));
    actionLast->setEnabled(false);
    connect(actionLast, &QAction::triggered, this, [this] {
        history_ignore = true;
        history_index = int(history.size() - 1);
        auto h = history.at(history_index);
        if (tabWidget->currentIndex() != h.first) {
            selectionModel[tabWidget->currentIndex()]->clearSelection();
            tabWidget->setCurrentIndex(h.first);
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::Select);
        } else {
            selectionModel[h.first]->setCurrentIndex(h.second, QItemSelectionModel::ClearAndSelect);
        }
        updateButtons();
    });

    actionClear = new QAction("", this);
    actionClear->setIcon(QIcon(":/icons/resources/cross.png"));
    actionClear->setEnabled(true);
    connect(actionClear, &QAction::triggered, this, [this] {
        history_index = -1;
        history.clear();
        int num = tabWidget->currentIndex();
        if (selectionModel[num]->selectedIndexes().size() > 0) {
            QModelIndex index = selectionModel[num]->selectedIndexes().at(0);
            if (index.isValid()) {
                ElementType type = treeModel[num]->nodeFromIndex(index)->type();
                if (type != ElementType::NONE)
                    addToHistory(num, index);
            }
        }
        updateButtons();
    });

    QToolBar *toolbar = new QToolBar();
    toolbar->addAction(actionFirst);
    toolbar->addAction(actionPrev);
    toolbar->addAction(actionNext);
    toolbar->addAction(actionLast);
    toolbar->addAction(actionClear);

    QWidget *topWidget = new QWidget();
    QVBoxLayout *vbox1 = new QVBoxLayout();
    topWidget->setLayout(vbox1);
    vbox1->setSpacing(5);
    vbox1->setContentsMargins(0, 0, 0, 0);
    vbox1->addWidget(searchEdit);
    vbox1->addWidget(tabWidget);

    QWidget *toolbarWidget = new QWidget();
    QHBoxLayout *hbox = new QHBoxLayout;
    hbox->setAlignment(Qt::AlignCenter);
    toolbarWidget->setLayout(hbox);
    hbox->setSpacing(0);
    hbox->setContentsMargins(0, 0, 0, 0);
    hbox->addWidget(toolbar);

    QWidget *btmWidget = new QWidget();

    QVBoxLayout *vbox2 = new QVBoxLayout();
    btmWidget->setLayout(vbox2);
    vbox2->setSpacing(0);
    vbox2->setContentsMargins(0, 0, 0, 0);
    vbox2->addWidget(toolbarWidget);
    vbox2->addWidget(propertyEditor);

    QSplitter *splitter = new QSplitter(Qt::Vertical);
    splitter->setObjectName("splitter_design");
    splitter->addWidget(topWidget);
    splitter->addWidget(btmWidget);

    QGridLayout *mainLayout = new QGridLayout();
    mainLayout->setSpacing(0);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(splitter);
    setLayout(mainLayout);

    // Connection
    connect(propertyEditor->treeWidget(), &QTreeWidget::customContextMenuRequested, this,
            &DesignWidget::prepareMenuProperty);
    connect(propertyEditor->treeWidget(), &QTreeWidget::itemDoubleClicked, this, &DesignWidget::onItemDoubleClicked);
    connect(propertyEditor, &QtTreePropertyBrowser::hoverPropertyChanged, this, &DesignWidget::onHoverPropertyChanged);

    for (int num = 0; num < 6; num++) {
        connect(treeView[num], &TreeView::customContextMenuRequested,
                [this, num](const QPoint &pos) { prepareMenuTree(num, pos); });
        connect(treeView[num], &TreeView::doubleClicked, [this](const QModelIndex &index) { onDoubleClicked(index); });
        if (num == getIndexByElementType(ElementType::CELL)) {
            // A click on the already selected cell does not change the selection: arm it anyway
            connect(treeView[num], &TreeView::clicked, this, [this](const QModelIndex &) {
                if (!arm_ignore)
                    armTreeCell();
            });
        }
        connect(treeView[num], &TreeView::hoverIndexChanged,
                [this, num](QModelIndex index) { onHoverIndexChanged(num, index); });
        selectionModel[num] = treeView[num]->selectionModel();
        connect(selectionModel[num], &QItemSelectionModel::selectionChanged,
                [this, num](const QItemSelection &selected, const QItemSelection &deselected) {
                    onSelectionChanged(num, selected, deselected);
                });
    }

    history_index = -1;
    history_ignore = false;

    highlightColors[0] = QColor("#6495ed");
    highlightColors[1] = QColor("#7fffd4");
    highlightColors[2] = QColor("#98fb98");
    highlightColors[3] = QColor("#ffd700");
    highlightColors[4] = QColor("#cd5c5c");
    highlightColors[5] = QColor("#fa8072");
    highlightColors[6] = QColor("#ff69b4");
    highlightColors[7] = QColor("#da70d6");
}

DesignWidget::~DesignWidget() {}

void DesignWidget::updateButtons()
{
    int count = int(history.size());
    actionFirst->setEnabled(history_index > 0);
    actionPrev->setEnabled(history_index > 0);
    actionNext->setEnabled(history_index < (count - 1));
    actionLast->setEnabled(history_index < (count - 1));
}

void DesignWidget::addToHistory(int tab, QModelIndex item)
{
    if (!history_ignore) {
        int count = int(history.size());
        for (int i = count - 1; i > history_index; i--)
            history.pop_back();
        history.push_back(std::make_pair(tab, item));
        history_index++;
    }
    history_ignore = false;
    updateButtons();
}

void DesignWidget::newContext(Context *ctx)
{
    if (!ctx)
        return;

    highlightSelected.clear();
    history_ignore = false;
    history_index = -1;
    history.clear();
    updateButtons();

    highlightSelected.clear();
    this->ctx = ctx;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        {
            TreeModel::ElementXYRoot<BelId>::ElementMap belMap;
            for (const auto &bel : ctx->getBels()) {
                auto loc = ctx->getBelLocation(bel);
                belMap[std::pair<int, int>(loc.x, loc.y)].push_back(bel);
            }
            auto belGetter = [](Context *ctx, BelId id) { return ctx->getBelName(id); };

            getTreeByElementType(ElementType::BEL)
                    ->loadData(ctx,
                               std::unique_ptr<TreeModel::ElementXYRoot<BelId>>(
                                       new TreeModel::ElementXYRoot<BelId>(ctx, belMap, belGetter, ElementType::BEL)));
        }

        {
            TreeModel::ElementXYRoot<WireId>::ElementMap wireMap;
#ifdef ARCH_ICE40
            for (int i = 0; i < int(ctx->chip_info->wire_data.size()); i++) {
                const auto wire = &ctx->chip_info->wire_data[i];
                WireId wireid;
                wireid.index = i;
                wireMap[std::pair<int, int>(wire->x, wire->y)].push_back(wireid);
            }
#endif
#ifdef ARCH_ECP5
            for (const auto &wire : ctx->getWires()) {
                wireMap[std::pair<int, int>(wire.location.x, wire.location.y)].push_back(wire);
            }
#endif
#ifdef ARCH_MACHXO2
            for (const auto &wire : ctx->getWires()) {
                wireMap[std::pair<int, int>(wire.location.x, wire.location.y)].push_back(wire);
            }
#endif
#ifdef ARCH_GOWIN
            for (const auto &wire : ctx->getWires()) {
                WireInfo wi = ctx->wire_info(wire);
                wireMap[std::pair<int, int>(wi.x, wi.y)].push_back(wire);
            }
#endif
#ifdef ARCH_HIMBAECHEL
            for (const auto &wire : ctx->getWires()) {
                Loc loc;
                tile_xy(ctx->chip_info, wire.tile, loc.x, loc.y);
                wireMap[std::pair<int, int>(loc.x, loc.y)].push_back(wire);
            }
#endif
            auto wireGetter = [](Context *ctx, WireId id) { return ctx->getWireName(id); };
            getTreeByElementType(ElementType::WIRE)
                    ->loadData(ctx,
                               std::unique_ptr<TreeModel::ElementXYRoot<WireId>>(new TreeModel::ElementXYRoot<WireId>(
                                       ctx, wireMap, wireGetter, ElementType::WIRE)));
        }

        {
            TreeModel::ElementXYRoot<PipId>::ElementMap pipMap;
            for (const auto &pip : ctx->getPips()) {
                auto loc = ctx->getPipLocation(pip);
                pipMap[std::pair<int, int>(loc.x, loc.y)].push_back(pip);
            }
            auto pipGetter = [](Context *ctx, PipId id) { return ctx->getPipName(id); };

            getTreeByElementType(ElementType::PIP)
                    ->loadData(ctx,
                               std::unique_ptr<TreeModel::ElementXYRoot<PipId>>(
                                       new TreeModel::ElementXYRoot<PipId>(ctx, pipMap, pipGetter, ElementType::PIP)));
        }

        getTreeByElementType(ElementType::CELL)
                ->loadData(ctx, std::unique_ptr<TreeModel::IdList>(new TreeModel::IdList(ElementType::CELL)));
        getTreeByElementType(ElementType::NET)
                ->loadData(ctx, std::unique_ptr<TreeModel::IdList>(new TreeModel::IdList(ElementType::NET)));
    }
    updateTree();
}

void DesignWidget::refreshAfterEdit()
{
    updateTree();
    // Rebuild the property view of the current selection (updateTree cleared it)
    bool single = false;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        if (selectionModel[i]->selectedIndexes().size() == 1) {
            history_ignore = true;
            arm_ignore = true;
            onSelectionChanged(i, QItemSelection(), QItemSelection());
            arm_ignore = false;
            history_ignore = false;
            single = true;
            break;
        }
    }
    if (!single) {
        // Several items: their graphics may have moved (cells)
        std::vector<DecalXY> decals;
        for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
            for (auto index : selectionModel[i]->selectedIndexes()) {
                TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
                std::vector<DecalXY> d = getDecals(item->type(), item->id());
                std::move(d.begin(), d.end(), std::back_inserter(decals));
            }
        }
        Q_EMIT selected(decals, false);
        emitSelectionInfo();
    }
}

void DesignWidget::updateTree()
{
    clearProperties();

    QMap<TreeModel::Item *, int>::iterator i = highlightSelected.begin();
    while (i != highlightSelected.end()) {
        QMap<TreeModel::Item *, int>::iterator prev = i;
        ++i;
        if (prev.key()->type() == ElementType::NET && ctx->nets.find(prev.key()->id()[0]) == ctx->nets.end()) {
            highlightSelected.erase(prev);
        }
        if (prev.key()->type() == ElementType::CELL && ctx->cells.find(prev.key()->id()[0]) == ctx->cells.end()) {
            highlightSelected.erase(prev);
        }
    }

    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        bool only_unplaced = unplacedOnly != nullptr && unplacedOnly->isChecked();
        int num_unplaced = 0;
        std::vector<IdStringList> cells;
        for (auto &pair : ctx->cells) {
            bool unplaced = pair.second->bel == BelId();
            num_unplaced += unplaced;
            if (unplaced || !only_unplaced)
                cells.push_back(IdStringList(pair.first));
        }
        std::vector<IdStringList> nets;
        for (auto &pair : ctx->nets) {
            nets.push_back(IdStringList(pair.first));
        }
        if (unplacedOnly != nullptr)
            unplacedOnly->setText(QString("Unplaced only (%1 of %2)").arg(num_unplaced).arg(ctx->cells.size()));

        // Resetting a model clears its selection (silently): keep the selected cells and nets
        for (ElementType type : {ElementType::CELL, ElementType::NET}) {
            int num = getIndexByElementType(type);
            std::vector<IdStringList> keep;
            for (auto index : selectionModel[num]->selectedIndexes())
                keep.push_back(treeModel[num]->nodeFromIndex(index)->id());
            QModelIndex current = selectionModel[num]->currentIndex();
            boost::optional<IdStringList> current_id;
            if (current.isValid())
                current_id = treeModel[num]->nodeFromIndex(current)->id();
            getTreeByElementType(type)->updateElements(type == ElementType::CELL ? cells : nets);
            QSignalBlocker blocker(selectionModel[num]);
            for (auto &id : keep) {
                auto item = treeModel[num]->nodeForId(id);
                if (item)
                    selectionModel[num]->select(treeModel[num]->indexFromNode(*item), QItemSelectionModel::Select);
            }
            if (current_id) {
                auto item = treeModel[num]->nodeForId(*current_id);
                if (item)
                    selectionModel[num]->setCurrentIndex(treeModel[num]->indexFromNode(*item),
                                                         QItemSelectionModel::NoUpdate);
            }
            treeView[num]->viewport()->update();
        }
    }
}
QtProperty *DesignWidget::addTopLevelProperty(const QString &id)
{
    QtProperty *topItem = groupManager->addProperty(id);
    propertyToId[topItem] = id;
    idToProperty[id] = topItem;
    topItem->setSelectable(false);
    propertyEditor->addProperty(topItem);
    return topItem;
}

void DesignWidget::clearProperties()
{
    QMap<QtProperty *, QString>::ConstIterator itProp = propertyToId.constBegin();
    while (itProp != propertyToId.constEnd()) {
        delete itProp.key();
        itProp++;
    }
    propertyToId.clear();
    idToProperty.clear();
}

QString DesignWidget::getElementTypeName(ElementType type)
{
    if (type == ElementType::NONE)
        return "";
    if (type == ElementType::BEL)
        return "BEL";
    if (type == ElementType::WIRE)
        return "WIRE";
    if (type == ElementType::PIP)
        return "PIP";
    if (type == ElementType::NET)
        return "NET";
    if (type == ElementType::CELL)
        return "CELL";
    return "";
}
ElementType DesignWidget::getElementTypeByName(QString type)
{
    if (type == "BEL")
        return ElementType::BEL;
    if (type == "WIRE")
        return ElementType::WIRE;
    if (type == "PIP")
        return ElementType::PIP;
    if (type == "NET")
        return ElementType::NET;
    if (type == "CELL")
        return ElementType::CELL;
    return ElementType::NONE;
}

TreeModel::Model *DesignWidget::getTreeByElementType(ElementType type)
{
    if (type == ElementType::NONE)
        return nullptr;
    if (type == ElementType::BEL)
        return treeModel[0];
    if (type == ElementType::WIRE)
        return treeModel[1];
    if (type == ElementType::PIP)
        return treeModel[2];
    if (type == ElementType::CELL)
        return treeModel[3];
    if (type == ElementType::NET)
        return treeModel[4];
    return nullptr;
}
int DesignWidget::getIndexByElementType(ElementType type)
{
    if (type == ElementType::NONE)
        return -1;
    if (type == ElementType::BEL)
        return 0;
    if (type == ElementType::WIRE)
        return 1;
    if (type == ElementType::PIP)
        return 2;
    if (type == ElementType::CELL)
        return 3;
    if (type == ElementType::NET)
        return 4;
    if (type == ElementType::GROUP)
        return 5;
    return -1;
}
void DesignWidget::addProperty(QtProperty *topItem, int propertyType, const QString &name, QVariant value,
                               const ElementType &type)
{
    QtVariantProperty *item = readOnlyManager->addProperty(propertyType, name);
    item->setValue(value);
    item->setPropertyId(getElementTypeName(type));
    item->setSelectable(type != ElementType::NONE);
    topItem->addSubProperty(item);
}

QtProperty *DesignWidget::addSubGroup(QtProperty *topItem, const QString &name)
{
    QtProperty *item = groupManager->addProperty(name);
    item->setSelectable(false);
    topItem->addSubProperty(item);
    return item;
}

void DesignWidget::clearAllSelectionModels()
{
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++)
        selectionModel[i]->clearSelection();
}

void DesignWidget::onClickedBel(BelId bel, bool keep)
{
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        item = getTreeByElementType(ElementType::BEL)->nodeForId(ctx->getBelName(bel));
        if (!item)
            return;

        Q_EMIT selected(getDecals(ElementType::BEL, ctx->getBelName(bel)), keep);
    }
    int index = getIndexByElementType(ElementType::BEL);
    if (!keep)
        clearAllSelectionModels();
    if (tabWidget->currentIndex() != index) {
        tabWidget->setCurrentIndex(index);
    }
    selectionModel[index]->setCurrentIndex(getTreeByElementType(ElementType::BEL)->indexFromNode(*item),
                                           keep ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect);
}

static NetInfo *bound_net(Context *ctx, WireId wire) { return ctx->getBoundWireNet(wire); }
static NetInfo *bound_net(Context *ctx, PipId pip) { return ctx->getBoundPipNet(pip); }

void DesignWidget::onClickedWire(WireId wire, bool keep)
{
    if (clickedNet(wire, keep))
        return;
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        item = getTreeByElementType(ElementType::WIRE)->nodeForId(ctx->getWireName(wire));
        if (!item)
            return;

        Q_EMIT selected(getDecals(ElementType::WIRE, ctx->getWireName(wire)), keep);
    }
    int index = getIndexByElementType(ElementType::WIRE);
    if (!keep)
        clearAllSelectionModels();
    if (tabWidget->currentIndex() != index)
        tabWidget->setCurrentIndex(index);
    selectionModel[index]->setCurrentIndex(getTreeByElementType(ElementType::WIRE)->indexFromNode(*item),
                                           keep ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect);
}

void DesignWidget::onClickedPip(PipId pip, bool keep)
{
    if (clickedNet(pip, keep))
        return;
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        item = getTreeByElementType(ElementType::PIP)->nodeForId(ctx->getPipName(pip));
        if (!item)
            return;

        Q_EMIT selected(getDecals(ElementType::PIP, ctx->getPipName(pip)), keep);
    }

    int index = getIndexByElementType(ElementType::PIP);
    if (!keep)
        clearAllSelectionModels();
    if (tabWidget->currentIndex() != index)
        tabWidget->setCurrentIndex(index);
    selectionModel[index]->setCurrentIndex(getTreeByElementType(ElementType::PIP)->indexFromNode(*item),
                                           keep ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect);
}

// For a wire or pip whose click selects its net (see guiSelectsNet): select the net, as a click in
// the net list does
template <typename T> bool DesignWidget::clickedNet(T element, bool keep)
{
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        if (!ctx->guiSelectsNet(element))
            return false;
        NetInfo *net = bound_net(ctx, element);
        if (net == nullptr)
            return false;
        item = getTreeByElementType(ElementType::NET)->nodeForId(IdStringList(net->name));
        if (!item)
            return false;
        Q_EMIT selected(getDecals(ElementType::NET, IdStringList(net->name)), keep);
    }
    int index = getIndexByElementType(ElementType::NET);
    if (!keep)
        clearAllSelectionModels();
    if (tabWidget->currentIndex() != index)
        tabWidget->setCurrentIndex(index);
    selectionModel[index]->setCurrentIndex(getTreeByElementType(ElementType::NET)->indexFromNode(*item),
                                           keep ? QItemSelectionModel::Select : QItemSelectionModel::ClearAndSelect);
    return true;
}

void DesignWidget::showCell(IdString cell)
{
    if (ctx == nullptr)
        return;
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        item = getTreeByElementType(ElementType::CELL)->nodeForId(IdStringList(cell));
    }
    if (!item && unplacedOnly->isChecked() && ctx->cells.count(cell)) {
        // A placed cell, hidden by the filter: show all cells
        unplacedOnly->setChecked(false);
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        item = getTreeByElementType(ElementType::CELL)->nodeForId(IdStringList(cell));
    }
    if (!item)
        return;
    int index = getIndexByElementType(ElementType::CELL);
    arm_ignore = true;
    clearAllSelectionModels();
    if (tabWidget->currentIndex() != index)
        tabWidget->setCurrentIndex(index);
    QModelIndex mi = getTreeByElementType(ElementType::CELL)->indexFromNode(*item);
    selectionModel[index]->setCurrentIndex(mi, QItemSelectionModel::ClearAndSelect);
    treeView[index]->scrollTo(mi);
    arm_ignore = false;
}

void DesignWidget::showNet(IdString net)
{
    if (ctx == nullptr)
        return;
    boost::optional<TreeModel::Item *> item;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        item = getTreeByElementType(ElementType::NET)->nodeForId(IdStringList(net));
    }
    if (!item)
        return;
    int index = getIndexByElementType(ElementType::NET);
    arm_ignore = true;
    clearAllSelectionModels();
    if (tabWidget->currentIndex() != index)
        tabWidget->setCurrentIndex(index);
    QModelIndex mi = getTreeByElementType(ElementType::NET)->indexFromNode(*item);
    selectionModel[index]->setCurrentIndex(mi, QItemSelectionModel::ClearAndSelect);
    treeView[index]->scrollTo(mi);
    arm_ignore = false;
}

IdString DesignWidget::singleTreeCell()
{
    int cells = getIndexByElementType(ElementType::CELL);
    QModelIndexList sel = selectionModel[cells]->selectedIndexes();
    if (sel.size() != 1)
        return IdString();
    // With Ctrl held the other trees keep their selection: then the cell must be all that is selected
    if (QApplication::keyboardModifiers().testFlag(Qt::ControlModifier)) {
        for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++)
            if (i != cells && !selectionModel[i]->selectedIndexes().isEmpty())
                return IdString();
    }
    TreeModel::Item *item = treeModel[cells]->nodeFromIndex(sel.at(0));
    return item->type() == ElementType::CELL ? item->id()[0] : IdString();
}

void DesignWidget::armTreeCell()
{
    if (editStack == nullptr || ctx == nullptr)
        return;
    IdString cell = singleTreeCell();
    if (cell == IdString() || !editStack->editMode()) {
        Q_EMIT armCellRequested(IdString());
        return;
    }
    // armCellRequested reports why if the cell cannot be armed now (e.g. a task is running)
    Q_EMIT armCellRequested(cell);
}

void DesignWidget::setEditStack(EditStack *stack)
{
    editStack = stack;
    if (stack == nullptr)
        return;
    // Turning edit mode on with one cell selected in the Cells tree arms it
    connect(stack, &EditStack::stateChanged, this, [this] {
        bool on = editStack->editMode();
        if (on && !lastEditMode && !arm_ignore)
            armTreeCell();
        lastEditMode = on;
    });
}

void DesignWidget::armSelected()
{
    if (ctx == nullptr || editStack == nullptr)
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot place: " + why);
        return;
    }
    IdString cell;
    int num_selected = 0;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        for (auto index : selectionModel[i]->selectedIndexes()) {
            num_selected++;
            TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
            if (item->type() == ElementType::CELL) {
                cell = item->id()[0];
            } else if (item->type() == ElementType::BEL) {
                BelId bel = ctx->getBelByName(item->id());
                CellInfo *bound = bel != BelId() ? ctx->getBoundBelCell(bel) : nullptr;
                if (bound != nullptr)
                    cell = bound->name;
            }
        }
    }
    if (num_selected != 1 || cell == IdString()) {
        editStack->notify("To place or move a cell, select one cell or one bel with a cell first");
        return;
    }
    Q_EMIT armCellRequested(cell);
}

std::vector<IdString> DesignWidget::selectedCells(bool *on_bels)
{
    std::vector<IdString> cells;
    pool<IdString> seen;
    if (on_bels)
        *on_bels = false;
    if (ctx == nullptr)
        return cells;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        for (auto index : selectionModel[i]->selectedIndexes()) {
            TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
            IdString cell;
            if (item->type() == ElementType::CELL) {
                cell = item->id()[0];
            } else if (item->type() == ElementType::BEL) {
                BelId bel = ctx->getBelByName(item->id());
                CellInfo *bound = bel != BelId() ? ctx->getBoundBelCell(bel) : nullptr;
                if (bound == nullptr)
                    continue;
                cell = bound->name;
                if (on_bels)
                    *on_bels = true;
            }
            if (cell != IdString() && ctx->cells.count(cell) && seen.insert(cell).second)
                cells.push_back(cell);
        }
    }
    return cells;
}

void DesignWidget::selectBels(const std::vector<BelId> &bels)
{
    bool first = true;
    for (BelId bel : bels) {
        if (bel == BelId())
            continue;
        onClickedBel(bel, !first);
        first = false;
    }
}

std::string DesignWidget::moveSelectionBy(int dx, int dy)
{
    if (ctx == nullptr || editStack == nullptr)
        return "no design";
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot move: " + why);
        return why.toStdString();
    }
    bool on_bels = false;
    std::vector<IdString> cells = selectedCells(&on_bels);
    if (cells.empty()) {
        editStack->notify("To move cells, select placed cells (Cells tree) or bels with cells first");
        return "nothing selected";
    }
    QString text = cells.size() == 1 ? QString("Move %1 by (%2, %3)").arg(cells[0].c_str(ctx)).arg(dx).arg(dy)
                                     : QString("Move %1 cells by (%2, %3)").arg(cells.size()).arg(dx).arg(dy);
    std::string err = editStack->run(text, [&](EditBatch &b) { return addMoveCells(b, cells, dx, dy); });
    if (err.empty() && on_bels) {
        // The selected bels are empty now: select those the cells went to
        std::vector<BelId> bels;
        for (IdString c : cells) {
            auto fnd = ctx->cells.find(c);
            if (fnd != ctx->cells.end())
                bels.push_back(fnd->second->bel);
        }
        selectBels(bels);
    }
    return err;
}

void DesignWidget::moveSelectionDialog()
{
    if (ctx == nullptr || editStack == nullptr)
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot move: " + why);
        return;
    }
    std::vector<IdString> cells = selectedCells();
    if (cells.empty()) {
        editStack->notify("To move cells, select placed cells (Cells tree) or bels with cells first");
        return;
    }
    QDialog dialog(this);
    dialog.setWindowTitle("Move selection by");
    QFormLayout *form = new QFormLayout(&dialog);
    form->addRow(new QLabel(QString("Move %1 selected cell(s), clusters as a whole, by grid units:").arg(cells.size())));
    QSpinBox *sx = new QSpinBox(), *sy = new QSpinBox();
    for (QSpinBox *s : {sx, sy})
        s->setRange(-10000, 10000);
    form->addRow("dx", sx);
    form->addRow("dy", sy);
    QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
        return;
    moveSelectionBy(sx->value(), sy->value());
}

void DesignWidget::nudgeSelection(int dx, int dy)
{
    if (ctx == nullptr || editStack == nullptr || (dx == 0 && dy == 0))
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot move: " + why);
        return;
    }
    std::vector<IdString> cells = selectedCells();
    if (cells.empty()) {
        editStack->notify("To move cells with the arrow keys, select placed cells or bels with cells first");
        return;
    }
    // The smallest legal step in that direction (e.g. GateMate CPEs only move by some offsets)
    std::string first_err;
    for (int k = 1; k <= MAX_NUDGE; k++) {
        std::string err = editStack->dryRun([&](EditBatch &b) { return addMoveCells(b, cells, k * dx, k * dy); });
        if (err.empty()) {
            moveSelectionBy(k * dx, k * dy);
            return;
        }
        if (k == 1)
            first_err = err;
    }
    editStack->notify(QString("Cannot move the selection by up to %1 steps of (%2, %3): %4")
                              .arg(MAX_NUDGE)
                              .arg(dx)
                              .arg(dy)
                              .arg(QString::fromStdString(first_err)));
}

void DesignWidget::unlockCells(const QString &text, std::vector<IdString> cells)
{
    if (ctx == nullptr || editStack == nullptr)
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot unlock: " + why);
        return;
    }
    // Clusters as a whole; only placed, locked cells change
    std::vector<IdString> locked;
    int fixed = 0;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        pool<IdString> seen;
        for (IdString name : cells) {
            auto fnd = ctx->cells.find(name);
            if (fnd == ctx->cells.end())
                continue;
            for (CellInfo *c : placementGroup(ctx, fnd->second.get())) {
                if (!seen.insert(c->name).second || c->bel == BelId() || !isCellLocked(c))
                    continue;
                // Bound by the packer to the site the hardware ties them to: they stay
                if (isCellFixed(c))
                    fixed++;
                else
                    locked.push_back(c->name);
            }
        }
    }
    QString kept = fixed ? QString(", %1 FIXED by the packer kept").arg(fixed) : QString();
    if (locked.empty()) {
        editStack->notify(cells.empty() ? "Nothing to unlock: select cells or bels with cells first"
                                        : "Nothing to unlock: no placed locked cells" + kept);
        return;
    }
    editStack->run(QString("%1 (%2 cells%3)").arg(text).arg(locked.size()).arg(kept), [&](EditBatch &b) -> std::string {
        for (IdString name : locked) {
            std::string err = b.add(std::make_unique<SetCellStrengthCmd>(name, STRENGTH_MANUAL));
            if (!err.empty())
                return err;
        }
        return {};
    });
}

void DesignWidget::unlockSelection() { unlockCells("Unlock selection", selectedCells()); }

void DesignWidget::unlockAllCells()
{
    if (ctx == nullptr)
        return;
    std::vector<IdString> cells;
    for (auto &c : ctx->cells)
        cells.push_back(c.first);
    unlockCells("Unlock all cells", cells);
}

void DesignWidget::unlockAllNets()
{
    if (ctx == nullptr || editStack == nullptr)
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot unlock: " + why);
        return;
    }
    std::vector<IdString> nets;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (auto &n : ctx->nets) {
            for (auto &w : n.second->wires) {
                if (w.second.strength > STRENGTH_STRONG) {
                    nets.push_back(n.first);
                    break;
                }
            }
        }
    }
    if (nets.empty()) {
        editStack->notify("Nothing to unlock: no locked routing");
        return;
    }
    editStack->run(QString("Unlock all routing (%1 nets)").arg(nets.size()), [&](EditBatch &b) -> std::string {
        for (IdString name : nets) {
            std::string err = b.add(std::make_unique<SetNetStrengthCmd>(name, STRENGTH_STRONG));
            if (!err.empty())
                return err;
        }
        return {};
    });
}

void DesignWidget::selectNextUnplaced()
{
    if (ctx == nullptr || editStack == nullptr)
        return;
    QString why;
    if (!editStack->canEdit(&why)) {
        editStack->notify("Cannot place: " + why);
        return;
    }
    IdString cell;
    std::vector<BelId> neighbours;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        // Unplaced cells (cluster roots stand for their clusters), by name
        std::vector<std::pair<std::string, IdString>> unplaced;
        for (auto &c : ctx->cells) {
            CellInfo *ci = c.second.get();
            if (ci->bel != BelId())
                continue;
            if (ci->cluster != ClusterId() && ctx->getClusterRootCell(ci->cluster) != ci)
                continue;
            unplaced.emplace_back(ci->name.str(ctx), ci->name);
        }
        if (unplaced.empty()) {
            editStack->notify("All cells are placed");
            return;
        }
        std::sort(unplaced.begin(), unplaced.end());
        cell = unplaced.front().second;
        if (lastUnplaced != IdString()) {
            std::string last = lastUnplaced.str(ctx);
            for (auto &u : unplaced)
                if (u.first > last) {
                    cell = u.second;
                    break;
                }
        }
        // Its placed neighbours
        pool<BelId> seen;
        std::vector<CellInfo *> group = placementGroup(ctx, ctx->cells.at(cell).get());
        pool<IdString> members;
        for (CellInfo *m : group)
            members.insert(m->name);
        auto add = [&](CellInfo *other) {
            if (other != nullptr && other->bel != BelId() && !members.count(other->name) && seen.insert(other->bel).second)
                neighbours.push_back(other->bel);
        };
        for (CellInfo *m : group) {
            for (auto &port : m->ports) {
                NetInfo *net = port.second.net;
                if (net == nullptr || net->users.entries() > 64)
                    continue; // skip clocks, resets and other wide nets
                add(net->driver.cell);
                for (auto &usr : net->users)
                    add(usr.cell);
            }
        }
    }
    lastUnplaced = cell;
    showCell(cell);
    Q_EMIT armCellRequested(cell);
    if (!neighbours.empty())
        Q_EMIT centerOnBels(neighbours);
}

void DesignWidget::emitSelectionInfo()
{
    if (ctx == nullptr) {
        Q_EMIT selectionInfo(QString());
        Q_EMIT netSelected(IdString());
        return;
    }
    int num_selected = 0;
    TreeModel::Item *item = nullptr;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        for (auto index : selectionModel[i]->selectedIndexes()) {
            num_selected++;
            item = treeModel[i]->nodeFromIndex(index);
        }
    }
    if (num_selected == 0 || item == nullptr) {
        Q_EMIT selectionInfo("No selection");
        Q_EMIT netSelected(IdString());
        return;
    }
    if (num_selected > 1) {
        Q_EMIT selectionInfo(QString("%1 items selected").arg(num_selected));
        Q_EMIT netSelected(IdString());
        return;
    }
    std::string text;
    // The net shown in the device view: the selected net, or the net of the selected wire / pip
    NetInfo *view_net = nullptr;
    IdStringList c = item->id();
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        switch (item->type()) {
        case ElementType::BEL: {
            BelId bel = ctx->getBelByName(c);
            if (bel != BelId())
                text = ElementInfo::belText(ctx, bel, true, false);
            break;
        }
        case ElementType::WIRE: {
            WireId wire = ctx->getWireByName(c);
            if (wire != WireId()) {
                text = ElementInfo::wireText(ctx, wire, true, false);
                view_net = ctx->getBoundWireNet(wire);
            }
            break;
        }
        case ElementType::PIP: {
            PipId pip = ctx->getPipByName(c);
            if (pip != PipId()) {
                text = ElementInfo::pipText(ctx, pip, true, false);
                view_net = ctx->getBoundPipNet(pip);
            }
            break;
        }
        case ElementType::GROUP: {
            GroupId group = ctx->getGroupByName(c);
            if (group != GroupId())
                text = ElementInfo::groupText(ctx, group, false);
            break;
        }
        case ElementType::CELL: {
            auto fnd = ctx->cells.find(c[0]);
            if (fnd == ctx->cells.end())
                break;
            CellInfo *cell = fnd->second.get();
            text = "CELL " + cell->name.str(ctx) + "  |  type: " + cell->type.str(ctx) + "  |  bel: ";
            if (cell->bel != BelId())
                text += ctx->getBelName(cell->bel).str(ctx) + " [" + ElementInfo::strengthName(cell->belStrength) +
                        "]";
            else
                text += "(unplaced)";
            break;
        }
        case ElementType::NET: {
            auto fnd = ctx->nets.find(c[0]);
            if (fnd == ctx->nets.end())
                break;
            NetInfo *net = fnd->second.get();
            text = "NET " + net->name.str(ctx) + "  |  driver: ";
            if (net->driver.cell != nullptr)
                text += net->driver.cell->name.str(ctx) + "." + net->driver.port.str(ctx);
            else
                text += "(none)";
            text += "  |  users: " + std::to_string(net->users.entries()) +
                    "  |  wires: " + std::to_string(net->wires.size());
            view_net = net;
            break;
        }
        default:
            break;
        }
        if (view_net != nullptr) {
            std::string summary = NetStatus::summary(NetStatus::compute(ctx, view_net));
            if (item->type() == ElementType::NET)
                text += "  |  " + summary;
            else
                text += "  |  net " + view_net->name.str(ctx) + ": " + summary;
        }
    }
    Q_EMIT selectionInfo(QString::fromStdString(text));
    Q_EMIT netSelected(view_net != nullptr ? view_net->name : IdString());
}

void DesignWidget::onSelectionChanged(int num, const QItemSelection &, const QItemSelection &)
{
    emitSelectionInfo();
    // In edit mode, selecting exactly one cell in the Cells tree arms it for placement by click;
    // selecting anything else disarms (clearing another tree's selection does not)
    if (!arm_ignore && editStack != nullptr) {
        if (num == getIndexByElementType(ElementType::CELL))
            armTreeCell();
        else if (!selectionModel[num]->selectedIndexes().isEmpty())
            Q_EMIT armCellRequested(IdString());
    }
    int num_selected = 0;
    std::vector<DecalXY> decals;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        num_selected += selectionModel[i]->selectedIndexes().size();
        for (auto index : selectionModel[i]->selectedIndexes()) {
            TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
            std::vector<DecalXY> d = getDecals(item->type(), item->id());
            std::move(d.begin(), d.end(), std::back_inserter(decals));
        }
    }

    // Keep other tree seleciton only if Control is pressed
    if (num_selected > 1 && QApplication::keyboardModifiers().testFlag(Qt::ControlModifier) == true) {
        Q_EMIT selected(decals, false);
        return;
    }

    // For deselect and multiple select just send all
    if (selectionModel[num]->selectedIndexes().size() != 1) {
        Q_EMIT selected(decals, false);
        return;
    }

    QModelIndex index = selectionModel[num]->selectedIndexes().at(0);
    if (!index.isValid())
        return;
    TreeModel::Item *clickItem = treeModel[num]->nodeFromIndex(index);

    ElementType type = clickItem->type();
    if (type == ElementType::NONE)
        return;

    // Clear other tab selections
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++)
        if (i != num)
            selectionModel[i]->clearSelection();

    addToHistory(num, index);

    clearProperties();

    IdStringList c = clickItem->id();
    Q_EMIT selected(getDecals(type, c), false);

    if (type == ElementType::BEL) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        BelId bel = ctx->getBelByName(c);
        QtProperty *topItem = addTopLevelProperty("Bel");

        addProperty(topItem, QtType::QString, "Name", ctx->nameOfBel(bel));
        addProperty(topItem, QtType::QString, "Type", ctx->getBelType(bel).c_str(ctx));
        addProperty(topItem, QtType::Bool, "Available", ctx->checkBelAvail(bel));
        addProperty(topItem, QtType::QString, "Bound Cell", ctx->nameOf(ctx->getBoundBelCell(bel)), ElementType::CELL);
        addProperty(topItem, QtType::QString, "Bound Cell Type",
                    ctx->getBoundBelCell(bel) ? ctx->getBoundBelCell(bel)->type.c_str(ctx) : "");
        addProperty(topItem, QtType::QString, "Conflicting Cell", ctx->nameOf(ctx->getConflictingBelCell(bel)),
                    ElementType::CELL);

        QtProperty *attrsItem = addSubGroup(topItem, "Attributes");
        for (auto &item : ctx->getBelAttrs(bel)) {
            addProperty(attrsItem, QtType::QString, item.first.c_str(ctx), item.second.c_str());
        }

        QtProperty *belpinsItem = addSubGroup(topItem, "Ports");
        for (const auto &item : ctx->getBelPins(bel)) {
            QtProperty *portInfoItem = addSubGroup(belpinsItem, item.c_str(ctx));
            addProperty(portInfoItem, QtType::QString, "Name", item.c_str(ctx));
            addProperty(portInfoItem, QtType::Int, "Type", int(ctx->getBelPinType(bel, item)));
            WireId wire = ctx->getBelPinWire(bel, item);
            addProperty(portInfoItem, QtType::QString, "Wire", ctx->nameOfWire(wire), ElementType::WIRE);
        }
    } else if (type == ElementType::WIRE) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        WireId wire = ctx->getWireByName(c);
        QtProperty *topItem = addTopLevelProperty("Wire");

        addProperty(topItem, QtType::QString, "Name", ctx->nameOfWire(wire));
        addProperty(topItem, QtType::QString, "Type", ctx->getWireType(wire).c_str(ctx));
        addProperty(topItem, QtType::Bool, "Available", ctx->checkWireAvail(wire));
        addProperty(topItem, QtType::QString, "Bound Net", ctx->nameOf(ctx->getBoundWireNet(wire)), ElementType::NET);
        addProperty(topItem, QtType::QString, "Conflicting Wire", ctx->nameOfWire(ctx->getConflictingWireWire(wire)),
                    ElementType::WIRE);
        addProperty(topItem, QtType::QString, "Conflicting Net", ctx->nameOf(ctx->getConflictingWireNet(wire)),
                    ElementType::NET);

        QtProperty *attrsItem = addSubGroup(topItem, "Attributes");
        for (auto &item : ctx->getWireAttrs(wire)) {
            addProperty(attrsItem, QtType::QString, item.first.c_str(ctx), item.second.c_str());
        }

        DelayQuad delay = ctx->getWireDelay(wire);

        QtProperty *delayItem = addSubGroup(topItem, "Delay");
        addProperty(delayItem, QtType::Double, "Min Rise", delay.minRiseDelay());
        addProperty(delayItem, QtType::Double, "Max Rise", delay.maxRiseDelay());
        addProperty(delayItem, QtType::Double, "Min Fall", delay.minFallDelay());
        addProperty(delayItem, QtType::Double, "Max Fall", delay.maxFallDelay());

        QtProperty *belpinsItem = addSubGroup(topItem, "BelPins");
        for (const auto &item : ctx->getWireBelPins(wire)) {
            QString belname = "";
            if (item.bel != BelId())
                belname = ctx->nameOfBel(item.bel);
            QString pinname = item.pin.c_str(ctx);

            QtProperty *dhItem = addSubGroup(belpinsItem, belname + "-" + pinname);
            addProperty(dhItem, QtType::QString, "Bel", belname, ElementType::BEL);
            addProperty(dhItem, QtType::QString, "PortPin", pinname);
        }

        int counter = 0;
        QtProperty *pipsDownItem = addSubGroup(topItem, "Pips Downhill");
        for (const auto &item : ctx->getPipsDownhill(wire)) {
            addProperty(pipsDownItem, QtType::QString, "", ctx->nameOfPip(item), ElementType::PIP);
            counter++;
            if (counter == 50) {
                addProperty(pipsDownItem, QtType::QString, "Warning", "Too many items...", ElementType::NONE);
                break;
            }
        }

        counter = 0;
        QtProperty *pipsUpItem = addSubGroup(topItem, "Pips Uphill");
        for (const auto &item : ctx->getPipsUphill(wire)) {
            addProperty(pipsUpItem, QtType::QString, "", ctx->nameOfPip(item), ElementType::PIP);
            counter++;
            if (counter == 50) {
                addProperty(pipsUpItem, QtType::QString, "Warning", "Too many items...", ElementType::NONE);
                break;
            }
        }
    } else if (type == ElementType::PIP) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        PipId pip = ctx->getPipByName(c);
        QtProperty *topItem = addTopLevelProperty("Pip");

        addProperty(topItem, QtType::QString, "Name", ctx->nameOfPip(pip));
        addProperty(topItem, QtType::QString, "Type", ctx->getPipType(pip).c_str(ctx));
        addProperty(topItem, QtType::Bool, "Available", ctx->checkPipAvail(pip));
        addProperty(topItem, QtType::QString, "Bound Net", ctx->nameOf(ctx->getBoundPipNet(pip)), ElementType::NET);
        if (ctx->getConflictingPipWire(pip) != WireId()) {
            addProperty(topItem, QtType::QString, "Conflicting Wire", ctx->nameOfWire(ctx->getConflictingPipWire(pip)),
                        ElementType::WIRE);
        } else {
            addProperty(topItem, QtType::QString, "Conflicting Wire", "", ElementType::NONE);
        }
        addProperty(topItem, QtType::QString, "Conflicting Net", ctx->nameOf(ctx->getConflictingPipNet(pip)),
                    ElementType::NET);
        addProperty(topItem, QtType::QString, "Src Wire", ctx->nameOfWire(ctx->getPipSrcWire(pip)), ElementType::WIRE);
        addProperty(topItem, QtType::QString, "Dest Wire", ctx->nameOfWire(ctx->getPipDstWire(pip)), ElementType::WIRE);

        QtProperty *attrsItem = addSubGroup(topItem, "Attributes");
        for (auto &item : ctx->getPipAttrs(pip)) {
            addProperty(attrsItem, QtType::QString, item.first.c_str(ctx), item.second.c_str());
        }

        DelayQuad delay = ctx->getPipDelay(pip);

        QtProperty *delayItem = addSubGroup(topItem, "Delay");
        addProperty(delayItem, QtType::Double, "Min Rise", delay.minRiseDelay());
        addProperty(delayItem, QtType::Double, "Max Rise", delay.maxRiseDelay());
        addProperty(delayItem, QtType::Double, "Min Fall", delay.minFallDelay());
        addProperty(delayItem, QtType::Double, "Max Fall", delay.maxFallDelay());
    } else if (type == ElementType::NET) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        NetInfo *net = ctx->nets.at(c[0]).get();

        QtProperty *topItem = addTopLevelProperty("Net");

        addProperty(topItem, QtType::QString, "Name", net->name.c_str(ctx));

        QtProperty *driverItem = addSubGroup(topItem, "Driver");
        addProperty(driverItem, QtType::QString, "Port", net->driver.port.c_str(ctx));
        if (net->driver.cell)
            addProperty(driverItem, QtType::QString, "Cell", net->driver.cell->name.c_str(ctx), ElementType::CELL);
        else
            addProperty(driverItem, QtType::QString, "Cell", "", ElementType::CELL);

        // (a large net, such as a clock, has thousands of users and wires: only the first ones are
        // listed, as for a wire's pips, since the property browser gets very slow with so many)
        QtProperty *usersItem = addSubGroup(topItem, "Users");
        int counter = 0;
        for (auto &item : net->users) {
            if (counter++ == MAX_LISTED) {
                addProperty(usersItem, QtType::QString, "Warning",
                            QString("%1 more users not listed").arg(int(net->users.entries()) - MAX_LISTED),
                            ElementType::NONE);
                break;
            }
            QtProperty *portItem = addSubGroup(usersItem, item.port.c_str(ctx));

            addProperty(portItem, QtType::QString, "Port", item.port.c_str(ctx));
            if (item.cell)
                addProperty(portItem, QtType::QString, "Cell", item.cell->name.c_str(ctx), ElementType::CELL);
            else
                addProperty(portItem, QtType::QString, "Cell", "", ElementType::CELL);
        }

        QtProperty *attrsItem = addSubGroup(topItem, "Attributes");
        for (auto &item : net->attrs) {
            // (such as the ROUTING of a loaded design, which can be megabytes)
            std::string value = item.second.is_string ? item.second.as_string() : item.second.to_string();
            if (value.size() > MAX_VALUE_LENGTH)
                value = value.substr(0, MAX_VALUE_LENGTH) + "... (" + std::to_string(value.size()) + " characters)";
            addProperty(attrsItem, QtType::QString, item.first.c_str(ctx), value.c_str());
        }

        NetStatus::Status st = NetStatus::compute(ctx, net);
        QtProperty *routingItem = addSubGroup(topItem, "Routing");
        addProperty(routingItem, QtType::QString, "Status", QString::fromStdString(NetStatus::summary(st)));
        QString source = st.source != WireId() ? ctx->nameOfWire(st.source) : "";
        addProperty(routingItem, QtType::QString, "Source Wire", source, ElementType::WIRE);
        QtProperty *arcsItem = addSubGroup(routingItem, "Arcs");
        for (auto &arc : st.arcs) {
            QString name = QString("%1.%2").arg(arc.cell.c_str(ctx)).arg(arc.port.c_str(ctx));
            if (arc.phys_idx > 0)
                name += QString(" [%1]").arg(arc.phys_idx);
            QtProperty *arcItem = addSubGroup(arcsItem, name);
            std::string state = NetStatus::stateName(arc.state);
            if (arc.blocked)
                state += " (sink wire used by " + std::string(ctx->nameOf(ctx->getBoundWireNet(arc.sink))) + ")";
            addProperty(arcItem, QtType::QString, "State", QString::fromStdString(state));
            QString sink = arc.sink != WireId() ? ctx->nameOfWire(arc.sink) : "";
            addProperty(arcItem, QtType::QString, "Sink Wire", sink, ElementType::WIRE);
            if (!arc.path.empty())
                addProperty(arcItem, QtType::Int, "Wires", int(arc.path.size()));
        }

        QtProperty *wiresItem = addSubGroup(topItem, "Wires");
        counter = 0;
        for (auto &item : net->wires) {
            if (counter++ == MAX_LISTED) {
                addProperty(wiresItem, QtType::QString, "Warning",
                            QString("%1 more wires not listed").arg(int(net->wires.size()) - MAX_LISTED),
                            ElementType::NONE);
                break;
            }
            auto name = ctx->nameOfWire(item.first);

            QtProperty *wireItem = addSubGroup(wiresItem, name);
            addProperty(wireItem, QtType::QString, "Wire", name, ElementType::WIRE);

            if (item.second.pip != PipId())
                addProperty(wireItem, QtType::QString, "Pip", ctx->nameOfPip(item.second.pip), ElementType::PIP);
            else
                addProperty(wireItem, QtType::QString, "Pip", "", ElementType::PIP);

            addProperty(wireItem, QtType::Int, "Strength", (int)item.second.strength);
        }

    } else if (type == ElementType::CELL) {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        CellInfo *cell = ctx->cells.at(c[0]).get();

        QtProperty *topItem = addTopLevelProperty("Cell");

        addProperty(topItem, QtType::QString, "Name", cell->name.c_str(ctx));
        addProperty(topItem, QtType::QString, "Type", cell->type.c_str(ctx));
        if (cell->bel != BelId())
            addProperty(topItem, QtType::QString, "Bel", ctx->nameOfBel(cell->bel), ElementType::BEL);
        else
            addProperty(topItem, QtType::QString, "Bel", "", ElementType::BEL);
        addProperty(topItem, QtType::Int, "Bel strength", int(cell->belStrength));

        QtProperty *cellPortsItem = addSubGroup(topItem, "Ports");
        for (auto &item : cell->ports) {
            PortInfo p = item.second;

            QtProperty *portInfoItem = addSubGroup(cellPortsItem, p.name.c_str(ctx));
            addProperty(portInfoItem, QtType::QString, "Name", p.name.c_str(ctx));
            addProperty(portInfoItem, QtType::Int, "Type", int(p.type));
            if (p.net)
                addProperty(portInfoItem, QtType::QString, "Net", p.net->name.c_str(ctx), ElementType::NET);
            else
                addProperty(portInfoItem, QtType::QString, "Net", "", ElementType::NET);
        }

        QtProperty *cellAttrItem = addSubGroup(topItem, "Attributes");
        for (auto &item : cell->attrs) {
            addProperty(cellAttrItem, QtType::QString, item.first.c_str(ctx),
                        item.second.is_string ? item.second.as_string().c_str() : item.second.to_string().c_str());
        }

        QtProperty *cellParamsItem = addSubGroup(topItem, "Parameters");
        for (auto &item : cell->params) {
            addProperty(cellParamsItem, QtType::QString, item.first.c_str(ctx),
                        item.second.is_string ? item.second.as_string().c_str() : item.second.to_string().c_str());
        }
    }
}

std::vector<DecalXY> DesignWidget::getDecals(ElementType type, IdStringList value)
{
    std::vector<DecalXY> decals;
    switch (type) {
    case ElementType::BEL: {
        BelId bel = ctx->getBelByName(value);
        if (bel != BelId()) {
            decals.push_back(ctx->getBelDecal(bel));
        }
    } break;
    case ElementType::WIRE: {
        WireId wire = ctx->getWireByName(value);
        if (wire != WireId()) {
            decals.push_back(ctx->getWireDecal(wire));
        }
    } break;
    case ElementType::PIP: {
        PipId pip = ctx->getPipByName(value);
        if (pip != PipId()) {
            decals.push_back(ctx->getPipDecal(pip));
        }
    } break;
    case ElementType::NET: {
        NetInfo *net = ctx->nets.at(value[0]).get();
        for (auto &item : net->wires) {
            decals.push_back(ctx->getWireDecal(item.first));
            if (item.second.pip != PipId()) {
                decals.push_back(ctx->getPipDecal(item.second.pip));
            }
        }
    } break;
    case ElementType::CELL: {
        CellInfo *cell = ctx->cells.at(value[0]).get();
        if (cell->bel != BelId()) {
            decals.push_back(ctx->getBelDecal(cell->bel));
        }
    } break;
    default:
        break;
    }
    return decals;
}

void DesignWidget::updateHighlightGroup(QList<TreeModel::Item *> items, int group)
{
    const bool shouldClear = items.size() == 1;
    for (auto item : items) {
        if (highlightSelected.contains(item)) {
            if (shouldClear && highlightSelected[item] == group) {
                highlightSelected.remove(item);
            } else
                highlightSelected[item] = group;
        } else
            highlightSelected.insert(item, group);
    }
    emitHighlights();
}

void DesignWidget::emitHighlights()
{
    std::vector<DecalXY> decals[8];

    for (auto it : highlightSelected.toStdMap()) {
        std::vector<DecalXY> d = getDecals(it.first->type(), it.first->id());
        std::move(d.begin(), d.end(), std::back_inserter(decals[it.second]));
    }
    for (int i = 0; i < 8; i++)
        Q_EMIT highlight(decals[i], i);
}

void DesignWidget::prepareMenuProperty(const QPoint &pos)
{
    QTreeWidget *tree = propertyEditor->treeWidget();
    QList<TreeModel::Item *> items;
    for (auto itemContextMenu : tree->selectedItems()) {
        QtBrowserItem *browserItem = propertyEditor->itemToBrowserItem(itemContextMenu);
        if (!browserItem)
            continue;
        QtProperty *selectedProperty = browserItem->property();
        ElementType type = getElementTypeByName(selectedProperty->propertyId());
        if (type == ElementType::NONE)
            continue;
        IdStringList value = IdStringList::parse(ctx, selectedProperty->valueText().toStdString());
        auto node = getTreeByElementType(type)->nodeForId(value);
        if (!node)
            continue;
        items.append(*node);
    }
    int selectedIndex = -1;
    if (items.size() == 1) {
        TreeModel::Item *item = items.at(0);
        if (highlightSelected.contains(item))
            selectedIndex = highlightSelected[item];
    }

    QMenu menu(this);
    QAction *selectAction = new QAction("&Select", this);
    connect(selectAction, &QAction::triggered, this, [this, items] {
        std::vector<DecalXY> decals;
        for (auto clickItem : items) {
            std::vector<DecalXY> d = getDecals(clickItem->type(), clickItem->id());
            std::move(d.begin(), d.end(), std::back_inserter(decals));
        }
        Q_EMIT selected(decals, false);
    });
    menu.addAction(selectAction);

    QMenu *subMenu = menu.addMenu("Highlight");
    QActionGroup *group = new QActionGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < 8; i++) {
        QPixmap pixmap(32, 32);
        pixmap.fill(QColor(highlightColors[i]));
        QAction *action = new QAction(QIcon(pixmap), ("Group " + std::to_string(i)).c_str(), this);
        action->setCheckable(true);
        subMenu->addAction(action);
        group->addAction(action);
        if (selectedIndex == i)
            action->setChecked(true);
        connect(action, &QAction::triggered, this, [this, i, items] { updateHighlightGroup(items, i); });
    }
    menu.exec(tree->mapToGlobal(pos));
}

void DesignWidget::prepareMenuTree(int num, const QPoint &pos)
{
    int selectedIndex = -1;

    if (selectionModel[num]->selectedIndexes().size() == 0)
        return;

    QList<TreeModel::Item *> items;
    for (int i = 0; i <= getIndexByElementType(ElementType::GROUP); i++) {
        for (auto index : selectionModel[i]->selectedIndexes()) {
            TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
            items.append(item);
        }
    }
    if (items.size() == 1) {
        TreeModel::Item *item = items.at(0);
        if (highlightSelected.contains(item))
            selectedIndex = highlightSelected[item];
    }
    QMenu menu(this);
    QMenu *subMenu = menu.addMenu("Highlight");
    QActionGroup *group = new QActionGroup(this);
    group->setExclusive(true);
    for (int i = 0; i < 8; i++) {
        QPixmap pixmap(32, 32);
        pixmap.fill(QColor(highlightColors[i]));
        QAction *action = new QAction(QIcon(pixmap), ("Group " + std::to_string(i)).c_str(), this);
        action->setCheckable(true);
        subMenu->addAction(action);
        group->addAction(action);
        if (selectedIndex == i)
            action->setChecked(true);
        connect(action, &QAction::triggered, this, [this, i, items] { updateHighlightGroup(items, i); });
    }

    // Edit actions on cells (a cell in a cluster stands for its whole cluster)
    if (editStack != nullptr && !editStack->busy()) {
        std::vector<IdString> cells, placed;
        pool<IdString> seen;
        bool any_locked = false, any_unlocked = false, any_fixed = false;
        for (auto item : items) {
            if (item->type() != ElementType::CELL)
                continue;
            auto fnd = ctx->cells.find(item->id()[0]);
            if (fnd == ctx->cells.end())
                continue;
            cells.push_back(fnd->first);
            for (CellInfo *c : placementGroup(ctx, fnd->second.get())) {
                if (!seen.insert(c->name).second || c->bel == BelId())
                    continue;
                if (isCellFixed(c)) {
                    any_fixed = true;
                    continue;
                }
                placed.push_back(c->name);
                (isCellLocked(c) ? any_locked : any_unlocked) = true;
            }
        }
        QString why;
        bool can_edit = editStack->canEdit(&why);
        menu.setToolTipsVisible(true);
        auto editAction = [&](const QString &text) {
            QAction *action = menu.addAction(text);
            action->setEnabled(can_edit);
            if (!can_edit)
                action->setToolTip(why);
            return action;
        };
        if (!cells.empty())
            menu.addSeparator();
        if (cells.size() == 1) {
            IdString cell = cells.front();
            QAction *place = editAction(ctx->cells.at(cell)->bel != BelId() ? "Move by click...\tP"
                                                                              : "Place by click...\tP");
            connect(place, &QAction::triggered, this, [this, cell] { Q_EMIT armCellRequested(cell); });
            std::string fixed = cellFixedReason(ctx, ctx->cells.at(cell).get());
            if (!fixed.empty()) {
                place->setEnabled(false);
                place->setToolTip(QString::fromStdString(fixed));
            }
        }
        if (any_fixed) {
            QAction *note = menu.addAction(placed.empty() ? "FIXED by the packer: cannot be moved or unlocked"
                                                          : "(cells FIXED by the packer are left out)");
            note->setEnabled(false);
        }
        auto setStrength = [this, placed](const QString &text, PlaceStrength strength) {
            editStack->run(text, [&](EditBatch &b) -> std::string {
                for (auto name : placed) {
                    std::string err = b.add(std::make_unique<SetCellStrengthCmd>(name, strength));
                    if (!err.empty())
                        return err;
                }
                return {};
            });
        };
        QString cells_text = placed.size() == 1 ? QString::fromStdString(placed[0].str(ctx))
                                                : QString("%1 cells").arg(placed.size());
        if (any_unlocked) {
            QAction *lock = editAction("Lock " + cells_text);
            connect(lock, &QAction::triggered, this,
                    [setStrength, cells_text] { setStrength("Lock " + cells_text, STRENGTH_MANUAL_LOCKED); });
        }
        if (any_locked) {
            QAction *unlock = editAction("Unlock " + cells_text);
            connect(unlock, &QAction::triggered, this,
                    [setStrength, cells_text] { setStrength("Unlock " + cells_text, STRENGTH_MANUAL); });
        }
        if (!placed.empty()) {
            QAction *move = editAction("Move selection by...\tM");
            connect(move, &QAction::triggered, this, &DesignWidget::moveSelectionDialog);
            QAction *unplace =
                    editAction(placed.size() == 1 ? "Unplace" : QString("Unplace %1 cells").arg(placed.size()));
            connect(unplace, &QAction::triggered, this, [this, placed] {
                QString text = placed.size() == 1 ? QString("Unplace ") + placed[0].c_str(ctx)
                                                  : QString("Unplace %1 cells").arg(placed.size());
                editStack->run(text, [&](EditBatch &b) -> std::string {
                    for (auto name : placed) {
                        auto fnd = ctx->cells.find(name);
                        if (fnd == ctx->cells.end() || fnd->second->bel == BelId())
                            continue;
                        std::string err = b.add(std::make_unique<UnbindBelCmd>(fnd->second->bel));
                        if (!err.empty())
                            return err;
                    }
                    return {};
                });
            });
        }
    }
    // Rip-up on nets
    if (editStack != nullptr && !editStack->busy()) {
        std::vector<IdString> nets;
        for (auto item : items)
            if (item->type() == ElementType::NET && ctx->nets.count(item->id()[0]))
                nets.push_back(item->id()[0]);
        if (!nets.empty()) {
            QString why;
            bool can_edit = editStack->canEdit(&why);
            menu.setToolTipsVisible(true);
            menu.addSeparator();
            QString what = nets.size() == 1 ? QString("net %1").arg(nets[0].c_str(ctx))
                                            : QString("%1 nets").arg(nets.size());
            QAction *unroute = menu.addAction("Unroute " + what);
            unroute->setEnabled(can_edit);
            if (!can_edit)
                unroute->setToolTip(why);
            connect(unroute, &QAction::triggered, this, [this, nets, what] {
                editStack->run("Unroute " + what, [&](EditBatch &b) -> std::string {
                    for (IdString net : nets) {
                        NetInfo *ni = ctx->nets.at(net).get();
                        // Nets without routing besides the source wire are skipped
                        WireId src = ctx->getNetinfoSourceWire(ni);
                        bool routed = false;
                        for (auto &w : ni->wires)
                            routed |= w.first != src;
                        if (!routed)
                            continue;
                        std::string err = addUnrouteNet(b, net);
                        if (!err.empty())
                            return err;
                    }
                    return {};
                });
            });
            if (nets.size() == 1) {
                IdString net = nets[0];
                NetInfo *ni = ctx->nets.at(net).get();
                std::vector<std::pair<QString, WireId>> arcs;
                {
                    std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
                    std::lock_guard<std::mutex> lock(ctx->mutex);
                    NetStatus::Status st = NetStatus::compute(ctx, ni);
                    for (auto &arc : st.arcs)
                        if (!arc.path.empty() && !NetStatus::arcOnlyWires(ctx, ni, st, arc.sink).empty())
                            arcs.emplace_back(QString("%1.%2").arg(arc.cell.c_str(ctx)).arg(arc.port.c_str(ctx)),
                                              arc.sink);
                }
                if (!arcs.empty()) {
                    QMenu *sub = menu.addMenu(QString("Unroute arc (%1)").arg(arcs.size()));
                    sub->setEnabled(can_edit);
                    const size_t max_arcs = 40;
                    for (size_t i = 0; i < arcs.size() && i < max_arcs; i++) {
                        QString name = arcs[i].first;
                        WireId sink = arcs[i].second;
                        QAction *a = sub->addAction("to " + name);
                        connect(a, &QAction::triggered, this, [this, net, sink, name] {
                            editStack->run("Unroute arc to " + name,
                                           [&](EditBatch &b) { return addUnrouteArc(b, net, sink); });
                        });
                    }
                    if (arcs.size() > max_arcs)
                        sub->addAction(QString("... %1 more").arg(arcs.size() - max_arcs))->setEnabled(false);
                }
            }
        }
    }
    menu.exec(treeView[num]->mapToGlobal(pos));
}

void DesignWidget::onItemDoubleClicked(QTreeWidgetItem *item, int column)
{
    QtProperty *selectedProperty = propertyEditor->itemToBrowserItem(item)->property();
    ElementType type = getElementTypeByName(selectedProperty->propertyId());
    if (type == ElementType::NONE)
        return;

    IdStringList value = IdStringList::parse(ctx, selectedProperty->valueText().toStdString());
    auto it = getTreeByElementType(type)->nodeForId(value);
    if (it) {
        int num = getIndexByElementType(type);
        clearAllSelectionModels();
        if (tabWidget->currentIndex() != num)
            tabWidget->setCurrentIndex(num);
        selectionModel[num]->setCurrentIndex(getTreeByElementType(type)->indexFromNode(*it),
                                             QItemSelectionModel::ClearAndSelect);
    }
}

void DesignWidget::onDoubleClicked(const QModelIndex &index) { Q_EMIT zoomSelected(); }

void DesignWidget::onSearchInserted()
{
    if (currentSearch == searchEdit->text() && currentIndexTab == tabWidget->currentIndex()) {
        currentIndex++;
        if (currentIndex >= currentSearchIndexes.size())
            currentIndex = 0;
    } else {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);

        currentSearch = searchEdit->text();
        currentSearchIndexes = treeModel[tabWidget->currentIndex()]->search(searchEdit->text());
        currentIndex = 0;
        currentIndexTab = tabWidget->currentIndex();
    }
    if (currentSearchIndexes.size() > 0 && currentIndex < currentSearchIndexes.size())
        selectionModel[tabWidget->currentIndex()]->setCurrentIndex(currentSearchIndexes.at(currentIndex),
                                                                   QItemSelectionModel::ClearAndSelect);
}

void DesignWidget::onHoverIndexChanged(int num, QModelIndex index)
{
    if (index.isValid()) {
        TreeModel::Item *item = treeModel[num]->nodeFromIndex(index);
        if (item->type() != ElementType::NONE) {
            std::vector<DecalXY> decals = getDecals(item->type(), item->id());
            if (decals.size() > 0)
                Q_EMIT hover(decals.at(0));
            return;
        }
    }
    Q_EMIT hover(DecalXY());
}

void DesignWidget::onHoverPropertyChanged(QtBrowserItem *item)
{
    if (item != nullptr) {
        QtProperty *selectedProperty = item->property();
        ElementType type = getElementTypeByName(selectedProperty->propertyId());
        if (type != ElementType::NONE) {
            IdStringList value = IdStringList::parse(ctx, selectedProperty->valueText().toStdString());
            if (value != IdStringList()) {
                auto node = getTreeByElementType(type)->nodeForId(value);
                if (node) {
                    std::vector<DecalXY> decals = getDecals((*node)->type(), (*node)->id());
                    if (decals.size() > 0)
                        Q_EMIT hover(decals.at(0));
                    return;
                }
            }
        }
    }
    Q_EMIT hover(DecalXY());
}
QString DesignWidget::elementTypeName(ElementType type)
{
    switch (type) {
    case ElementType::BEL:
        return "bel";
    case ElementType::WIRE:
        return "wire";
    case ElementType::PIP:
        return "pip";
    case ElementType::NET:
        return "net";
    case ElementType::CELL:
        return "cell";
    default:
        return "";
    }
}

ElementType DesignWidget::elementTypeByName(QString type)
{
    for (auto t : {ElementType::BEL, ElementType::WIRE, ElementType::PIP, ElementType::NET, ElementType::CELL})
        if (type.toLower() == elementTypeName(t))
            return t;
    return ElementType::NONE;
}

boost::optional<TreeModel::Item *> DesignWidget::findElement(const Element &element)
{
    TreeModel::Model *tree = getTreeByElementType(element.type);
    if (ctx == nullptr || tree == nullptr)
        return boost::none;
    // (a net or cell name is one IdString, even with a slash in it)
    bool single = element.type == ElementType::NET || element.type == ElementType::CELL;
    try {
        return tree->nodeForId(single ? IdStringList(ctx->id(element.name)) : IdStringList::parse(ctx, element.name));
    } catch (const std::exception &) {
        // (a name the Arch does not know)
        return boost::none;
    }
}

std::vector<DesignWidget::Element> DesignWidget::selectedElements()
{
    std::vector<Element> elements;
    if (ctx == nullptr)
        return elements;
    for (int i = 0; i <= getIndexByElementType(ElementType::NET); i++)
        for (auto index : selectionModel[i]->selectedIndexes()) {
            TreeModel::Item *item = treeModel[i]->nodeFromIndex(index);
            if (item->type() != ElementType::NONE)
                elements.push_back({item->type(), item->id().str(ctx)});
        }
    return elements;
}

void DesignWidget::selectElements(const std::vector<Element> &elements, std::vector<std::string> &missing)
{
    std::vector<std::pair<int, QModelIndex>> found;
    for (auto &element : elements) {
        auto item = findElement(element);
        if (!item) {
            missing.push_back(element.name);
            continue;
        }
        int num = getIndexByElementType(element.type);
        found.push_back({num, treeModel[num]->indexFromNode(*item)});
    }
    if (found.size() <= 1) {
        // as a click
        clearAllSelectionModels();
        if (found.empty())
            return;
        tabWidget->setCurrentIndex(found[0].first);
        selectionModel[found[0].first]->setCurrentIndex(found[0].second, QItemSelectionModel::ClearAndSelect);
        return;
    }
    // Several, maybe in several trees: selected without the trees knowing (each would clear the
    // others), then shown all together, as a selection with Control does
    int trees = getIndexByElementType(ElementType::NET);
    for (int i = 0; i <= trees; i++)
        selectionModel[i]->blockSignals(true);
    clearAllSelectionModels();
    for (auto &f : found)
        selectionModel[f.first]->select(f.second, QItemSelectionModel::Select);
    for (int i = 0; i <= trees; i++) {
        selectionModel[i]->blockSignals(false);
        treeView[i]->viewport()->update();
    }
    tabWidget->setCurrentIndex(found[0].first);
    clearProperties();
    std::vector<DecalXY> decals;
    for (auto &f : found) {
        TreeModel::Item *item = treeModel[f.first]->nodeFromIndex(f.second);
        std::vector<DecalXY> d = getDecals(item->type(), item->id());
        std::move(d.begin(), d.end(), std::back_inserter(decals));
    }
    Q_EMIT selected(decals, false);
}

void DesignWidget::highlightElements(const std::vector<Element> &elements, int group, bool clear,
                                     std::vector<std::string> &missing)
{
    if (clear)
        highlightSelected.clear();
    for (auto &element : elements) {
        auto item = findElement(element);
        if (!item)
            missing.push_back(element.name);
        else if (group < 0)
            highlightSelected.remove(*item);
        else
            highlightSelected[*item] = group;
    }
    emitHighlights();
}

NEXTPNR_NAMESPACE_END
