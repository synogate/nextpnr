/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2018  Miodrag Milanovic <micko@yosyshq.com>
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

#ifndef DESIGNWIDGET_H
#define DESIGNWIDGET_H

#include <QCheckBox>
#include <QMouseEvent>
#include <QTabWidget>
#include <QTreeView>
#include <QVariant>
#include "nextpnr.h"
#include "qtgroupboxpropertybrowser.h"
#include "qtpropertymanager.h"
#include "qttreepropertybrowser.h"
#include "qtvariantproperty.h"
#include "treemodel.h"

NEXTPNR_NAMESPACE_BEGIN

class EditStack;

class TreeView : public QTreeView
{
    Q_OBJECT

  public:
    explicit TreeView(QWidget *parent = 0);
    ~TreeView();
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

  Q_SIGNALS:
    void hoverIndexChanged(QModelIndex index);

  private:
    QModelIndex current;
};

class DesignWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit DesignWidget(QWidget *parent = 0);
    ~DesignWidget();
    // Command layer for manual edits (see editcommands.h); owned by BaseMainWindow.
    void setEditStack(EditStack *stack);

    // An element of the trees, named as the Python bindings name it (for the MCP server)
    struct Element
    {
        ElementType type;
        std::string name;
    };
    std::vector<Element> selectedElements();
    // Selects these, as clicks in the trees would; the names of those not found go to missing
    void selectElements(const std::vector<Element> &elements, std::vector<std::string> &missing);
    // Highlights these in a group (0 to 7), or takes them out of their group (-1); clear: all of
    // them first
    void highlightElements(const std::vector<Element> &elements, int group, bool clear,
                           std::vector<std::string> &missing);
    static QString elementTypeName(ElementType type);
    static ElementType elementTypeByName(QString type);

  private:
    template <typename T> bool clickedNet(T element, bool keep);
    void clearProperties();
    void emitSelectionInfo();
    QtProperty *addTopLevelProperty(const QString &id);
    QtProperty *addSubGroup(QtProperty *topItem, const QString &name);
    void addProperty(QtProperty *topItem, int propertyType, const QString &name, QVariant value,
                     const ElementType &type = ElementType::NONE);
    QString getElementTypeName(ElementType type);
    ElementType getElementTypeByName(QString type);
    TreeModel::Model *getTreeByElementType(ElementType type);
    int getIndexByElementType(ElementType type);
    int getElementIndex(ElementType type);
    void updateButtons();
    void addToHistory(int tab, QModelIndex item);
    std::vector<DecalXY> getDecals(ElementType type, IdStringList value);
    void updateHighlightGroup(QList<TreeModel::Item *> item, int group);
    void emitHighlights();
    boost::optional<TreeModel::Item *> findElement(const Element &element);
    void clearAllSelectionModels();
    // The selected cells: cells in the Cells tree and cells bound to selected bels (each once);
    // *on_bels is set if any selected bel had a cell
    std::vector<IdString> selectedCells(bool *on_bels = nullptr);
    // Select these bels in the Bels tree (and the view)
    void selectBels(const std::vector<BelId> &bels);
    // The one cell selected in the Cells tree (nothing else selected), else empty
    IdString singleTreeCell();
    // Arm singleTreeCell() for placement by click if in edit mode, else disarm
    void armTreeCell();
    void unlockCells(const QString &text, std::vector<IdString> cells);
  Q_SIGNALS:
    void selected(std::vector<DecalXY> decal, bool keep);
    void highlight(std::vector<DecalXY> decal, int group);
    void hover(DecalXY decal);
    void zoomSelected();
    // A one-line description of the current selection (for the status bar)
    void selectionInfo(QString text);
    // Arm a cell for placement by click in the view (empty: disarm)
    void armCellRequested(IdString cell);
    // Centre the device view on these bels
    void centerOnBels(std::vector<BelId> bels);
    // The net to show in the device view (selected net, or net of the selected wire / pip; empty: none)
    void netSelected(IdString net);

  private Q_SLOTS:
    void prepareMenuProperty(const QPoint &pos);
    void prepareMenuTree(int num, const QPoint &pos);
    void onSelectionChanged(int num, const QItemSelection &selected, const QItemSelection &deselected);
    void onItemDoubleClicked(QTreeWidgetItem *item, int column);
    void onDoubleClicked(const QModelIndex &index);
    void onSearchInserted();
    void onHoverIndexChanged(int num, QModelIndex index);
    void onHoverPropertyChanged(QtBrowserItem *item);
  public Q_SLOTS:
    void newContext(Context *ctx);
    void updateTree();
    // Refresh trees and the property view of the current selection after an edit.
    void refreshAfterEdit();
    void onClickedBel(BelId bel, bool keep);
    void onClickedWire(WireId wire, bool keep);
    void onClickedPip(PipId pip, bool keep);
    // Select a cell in the Cells tree (without arming it for placement)
    void showCell(IdString cell);
    // Select a net in the Nets tree
    void showNet(IdString net);
    // Arm the selected cell, or the cell on the selected bel, for placement by click
    void armSelected();
    // Select and arm the next unplaced cell (by name, after the last one chosen this way) and centre
    // the view on its placed neighbours (via its nets), if any
    void selectNextUnplaced();
    // Ask for an offset and move the selected cells by it (see moveSelectionBy)
    void moveSelectionDialog();
    // Move the selected cells (Cells tree, or the cells on selected bels; clusters as a whole) by
    // (dx, dy) grid units, as one undo entry. Returns "" or the reason for refusing.
    std::string moveSelectionBy(int dx, int dy);
    // Move the selected cells in the direction (dx, dy) by the smallest step (1, 2, ... MAX_NUDGE
    // times the direction) that is legal
    void nudgeSelection(int dx, int dy);
    // Set the placed locked (strength above STRONG) selected cells / all cells to STRENGTH_MANUAL,
    // clusters as a whole, as one undo entry
    void unlockSelection();
    void unlockAllCells();
    // Set the routing strength of all nets with wires above STRONG to STRONG, as one undo entry
    void unlockAllNets();

  private:
    // How many of a large net's users and wires the properties list, and how long an attribute's
    // value may be there (the property browser gets very slow with thousands of entries)
    static constexpr int MAX_LISTED = 100;
    static constexpr size_t MAX_VALUE_LENGTH = 200;

    Context *ctx;
    EditStack *editStack = nullptr;

    QTabWidget *tabWidget;

    TreeView *treeView[6];
    QItemSelectionModel *selectionModel[6];
    TreeModel::Model *treeModel[6];
    QLineEdit *searchEdit;
    QtVariantPropertyManager *variantManager;
    QtVariantPropertyManager *readOnlyManager;
    QtGroupPropertyManager *groupManager;
    QtVariantEditorFactory *variantFactory;
    QtTreePropertyBrowser *propertyEditor;

    QMap<QtProperty *, QString> propertyToId;
    QMap<QString, QtProperty *> idToProperty;

    std::vector<std::pair<int, QModelIndex>> history;
    int history_index;
    bool history_ignore;
    // Selection changes that must not arm a cell for placement (refresh, "show in tree")
    bool arm_ignore = false;
    bool lastEditMode = false;
    // Cells tab: show only unplaced cells
    QCheckBox *unplacedOnly = nullptr;
    // The cell last chosen by selectNextUnplaced
    IdString lastUnplaced;
    static constexpr int MAX_NUDGE = 8;

    QAction *actionFirst;
    QAction *actionPrev;
    QAction *actionNext;
    QAction *actionLast;
    QAction *actionClear;

    QColor highlightColors[8];
    QMap<TreeModel::Item *, int> highlightSelected;

    QString currentSearch;
    QList<QModelIndex> currentSearchIndexes;
    int currentIndex;
    int currentIndexTab;
};

NEXTPNR_NAMESPACE_END

#endif // DESIGNWIDGET_H
