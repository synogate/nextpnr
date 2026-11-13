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

#include "hierarchydock.h"
#include <QApplication>
#include <QCheckBox>
#include <QColorDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QPolygonF>
#include <QProxyStyle>
#include <QPushButton>
#include <QScrollBar>
#include <QSplitter>
#include <QStyleOption>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <mutex>
#include "editcommands.h"
#include "fpgaviewwidget.h"

NEXTPNR_NAMESPACE_BEGIN

namespace {

const int ScopeRole = Qt::UserRole;

enum
{
    COL_NAME,
    COL_CELLS,
    COL_SHARE,
    COL_AREA,
    COL_MODULE,
    COL_COUNT
};

// Distinct colours that read on the dark view (after Kelly / Trubetskoy)
const QColor PALETTE[] = {
        QColor(230, 25, 75),  QColor(60, 180, 75),   QColor(255, 225, 25), QColor(67, 99, 216),   QColor(245, 130, 49),
        QColor(145, 30, 180), QColor(66, 212, 244),  QColor(240, 50, 230), QColor(191, 239, 69),  QColor(250, 190, 212),
        QColor(70, 153, 144), QColor(220, 190, 255), QColor(154, 99, 36),  QColor(255, 250, 200), QColor(170, 255, 195),
        QColor(128, 128, 0),  QColor(255, 216, 177), QColor(128, 0, 0),    QColor(169, 169, 169), QColor(0, 130, 200),
};
const int PALETTE_SIZE = int(sizeof(PALETTE) / sizeof(PALETTE[0]));

QIcon swatch(const QColor &c)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    if (c.isValid()) {
        QPainter p(&pm);
        p.fillRect(0, 0, 12, 12, QColor(c.red(), c.green(), c.blue()));
        p.setPen(Qt::black);
        p.drawRect(0, 0, 11, 11);
    }
    return QIcon(pm);
}

QString percent(double part, double whole)
{
    if (whole <= 0)
        return "";
    return QString::number(100.0 * part / whole, 'f', part * 1000 < whole ? 2 : 1) + " %";
}

// Of the sites of the main bel type in the area of the placed cells, how many the cells use
QString area_use(const BelCensus &census, const ScopeUsage &u)
{
    std::string type = u.mainBelType();
    if (type.empty())
        return "";
    return percent(census.sitesOf(type, u.sites), census.sitesIn(type, u.box));
}

// Expand / collapse arrows in the text colour of the palette
class BranchArrowStyle : public QProxyStyle
{
  public:
    using QProxyStyle::QProxyStyle;
    void drawPrimitive(PrimitiveElement pe, const QStyleOption *opt, QPainter *p, const QWidget *w) const override
    {
        if (pe != PE_IndicatorBranch || !(opt->state & State_Children)) {
            QProxyStyle::drawPrimitive(pe, opt, p, w);
            return;
        }
        QRectF r(opt->rect);
        double h = std::max(3.0, std::min(r.width(), r.height()) / 5.0);
        QPointF c = r.center();
        QPolygonF tri;
        if (opt->state & State_Open)
            tri << QPointF(c.x() - h, c.y() - h / 2) << QPointF(c.x() + h, c.y() - h / 2)
                << QPointF(c.x(), c.y() + h / 2 + h / 2);
        else
            tri << QPointF(c.x() - h / 2, c.y() - h) << QPointF(c.x() + h / 2 + h / 2, c.y())
                << QPointF(c.x() - h / 2, c.y() + h);
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(Qt::NoPen);
        p->setBrush(opt->palette.color(QPalette::Text));
        p->drawPolygon(tri);
        p->restore();
    }
};

} // namespace

HierarchyDock::HierarchyDock(FPGAViewWidget *view, EditStack *es, QWidget *parent)
        : QDockWidget("Hierarchy", parent), view(view), es(es)
{
    // (not "HierarchyDock": window layouts saved while the dock opened on the right do not move it back there)
    setObjectName("HierarchyPanel");
    QWidget *w = new QWidget(this);
    QVBoxLayout *layout = new QVBoxLayout(w);
    layout->setContentsMargins(4, 4, 4, 4);

    header = new QLabel(w);
    header->setObjectName("hierarchyHeader");
    header->setWordWrap(true);
    layout->addWidget(header);

    QHBoxLayout *row = new QHBoxLayout();
    filter = new QLineEdit(w);
    filter->setObjectName("hierarchyFilter");
    filter->setPlaceholderText("Filter instances");
    filter->setClearButtonEnabled(true);
    connect(filter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    row->addWidget(filter, 1);
    QPushButton *btnChildren = new QPushButton("Colour children", w);
    btnChildren->setToolTip("Colour each instance below the selected one in its own colour, and nothing else");
    connect(btnChildren, &QPushButton::clicked, this, &HierarchyDock::colourChildren);
    row->addWidget(btnChildren);
    QPushButton *btnClear = new QPushButton("Clear colours", w);
    connect(btnClear, &QPushButton::clicked, this, &HierarchyDock::clearColours);
    row->addWidget(btnClear);
    outlines = new QCheckBox("Outlines", w);
    outlines->setObjectName("hierarchyOutlines");
    outlines->setToolTip("Draw the bounding box of each coloured instance's cells, with its name");
    connect(outlines, &QCheckBox::toggled, this, [this] { updateView(); });
    row->addWidget(outlines);
    layout->addLayout(row);

    QSplitter *split = new QSplitter(Qt::Vertical, w);
    split->setObjectName("hierarchySplit");
    layout->addWidget(split, 1);

    tree = new QTreeWidget(split);
    tree->setObjectName("hierarchyTree");
    tree->setColumnCount(COL_COUNT);
    tree->setHeaderLabels({"Instance", "Cells", "Share", "Area use", "Module"});
    tree->setUniformRowHeights(true);
    tree->setContextMenuPolicy(Qt::CustomContextMenu);
    // The style's own expand arrows do not show on the dark palette
    QStyle *arrows = new BranchArrowStyle(QApplication::style()->name());
    arrows->setParent(tree);
    tree->setStyle(arrows);
    tree->headerItem()->setToolTip(COL_NAME, "Tick an instance to colour its cells in the device view (instances "
                                             "ticked below it keep their own colour)");
    tree->headerItem()->setToolTip(COL_CELLS, "Cells of the instance and the instances below it");
    tree->headerItem()->setToolTip(COL_SHARE, "Share of all cells of the design");
    tree->headerItem()->setToolTip(COL_AREA, "Of the sites (grid locations, GateMate: CPEs) with bels of the "
                                             "instance's main bel type inside the bounding box of its placed cells, "
                                             "the share it uses: low for a spread out instance");
    tree->setToolTip("Double-click / Enter: select the cells of the instance in the device view and zoom to them");
    connect(tree, &QTreeWidget::itemChanged, this, &HierarchyDock::itemChanged);
    connect(tree, &QTreeWidget::currentItemChanged, this, [this] {
        updateUsage();
        if (!quiet && !filling) {
            // (the Cells tab first shows the instance's cells, then they are selected there)
            emitScope();
            highlightCurrent();
        }
    });
    connect(tree, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem *item, int) {
        tree->setCurrentItem(item);
        selectCurrent();
    });
    connect(tree, &QTreeWidget::customContextMenuRequested, this, &HierarchyDock::contextMenu);
    // (the names of deeper instances need more room)
    connect(tree, &QTreeWidget::itemExpanded, this, [this] { tree->resizeColumnToContents(COL_NAME); });

    QWidget *bottom = new QWidget(split);
    QVBoxLayout *bottomLayout = new QVBoxLayout(bottom);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    stats = new QLabel(bottom);
    stats->setObjectName("hierarchyStats");
    stats->setWordWrap(true);
    stats->setTextInteractionFlags(Qt::TextSelectableByMouse);
    bottomLayout->addWidget(stats);
    usage = new QTreeWidget(bottom);
    usage->setObjectName("hierarchyUsage");
    usage->setColumnCount(4);
    usage->setHeaderLabels({"Bel type", "Used", "Device", "Of device"});
    usage->setRootIsDecorated(false);
    usage->setUniformRowHeights(true);
    usage->setToolTip("The bel types the placed cells of the selected instance use, and the unplaced cells by type");
    bottomLayout->addWidget(usage, 1);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 2);

    setWidget(w);

    timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, &HierarchyDock::refresh);
    // A task that ended without a design update (canceled, failed): catch up what it held up
    if (es != nullptr)
        connect(es, &EditStack::stateChanged, this, [this] {
            if (dirty && !busy())
                scheduleRefresh();
        });
    header->setText("No design");
}

void HierarchyDock::setContext(Context *ctx)
{
    this->ctx = ctx;
    haveCensus = false;
    scopes = DesignScopes();
    items.clear();
    filling = true;
    tree->clear();
    filling = false;
    usage->clear();
    stats->clear();
    dirty = true;
    if (view != nullptr)
        view->setBelFills({}, {});
    Q_EMIT cellScopeChanged(QString(), {});
    if (!isHidden())
        scheduleRefresh();
}

void HierarchyDock::scheduleRefresh()
{
    dirty = true;
    // (also behind another dock's tab: the colours in the device view follow the design)
    if (!isHidden())
        timer->start(REFRESH_DELAY_MS);
}

void HierarchyDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    if (dirty)
        refresh();
    else
        updateView();
}

void HierarchyDock::hideEvent(QHideEvent *event)
{
    QDockWidget::hideEvent(event);
    // The colours belong to the dock: they go when it is closed (not when another dock's tab is in front)
    if (!isHidden())
        return;
    if (view != nullptr)
        view->setBelFills({}, {});
}

void HierarchyDock::refresh()
{
    timer->stop();
    if (ctx == nullptr) {
        header->setText("No design");
        return;
    }
    if (busy()) {
        header->setText("A task is running; the hierarchy follows when it is done");
        dirty = true;
        return;
    }
    dirty = false;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        scopes = DesignScopes::build(ctx);
        if (!haveCensus) {
            census = BelCensus::build(ctx);
            haveCensus = true;
        }
    }
    int n = int(scopes.scopes.size());
    header->setText(QString("%1 instances, %2 cells. Cells put into an instance by its hierarchy or attributes: "
                            "%3, by the names of their nets: %4, by their neighbours: %5, nothing to go by (top "
                            "level): %6")
                            .arg(n - 1)
                            .arg(n ? scopes.scopes[0].total : 0)
                            .arg(scopes.source_count[DesignScopes::SRC_HIERARCHY] +
                                 scopes.source_count[DesignScopes::SRC_ATTRIBUTE])
                            .arg(scopes.source_count[DesignScopes::SRC_NETS])
                            .arg(scopes.source_count[DesignScopes::SRC_NEIGHBOURS])
                            .arg(scopes.source_count[DesignScopes::SRC_NONE]));
    rebuildTree();
    updateView();
}

void HierarchyDock::rebuildTree()
{
    // Keep what is expanded and selected
    QSet<QString> expanded;
    QString current;
    for (auto *item : items) {
        if (item == nullptr)
            continue;
        if (item->isExpanded())
            expanded.insert(item->data(COL_NAME, ScopeRole + 1).toString());
    }
    if (tree->currentItem() != nullptr)
        current = tree->currentItem()->data(COL_NAME, ScopeRole + 1).toString();
    bool first = items.empty();
    int scroll = tree->verticalScrollBar()->value();

    filling = true;
    tree->clear();
    items.assign(scopes.scopes.size(), nullptr);
    int total = scopes.scopes.empty() ? 0 : scopes.scopes[0].total;
    std::vector<ScopeUsage> use(scopes.scopes.size());
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (int s = 0; s < int(scopes.scopes.size()); s++)
            use[s] = ScopeUsage::of(ctx, scopes.cellsBelow(s));
    }
    QTreeWidgetItem *currentItem = nullptr;
    for (int s = 0; s < int(scopes.scopes.size()); s++) {
        const auto &sc = scopes.scopes[s];
        QTreeWidgetItem *item = s == 0 ? new QTreeWidgetItem(tree) : new QTreeWidgetItem(items.at(sc.parent));
        items[s] = item;
        QString path = QString::fromStdString(sc.path);
        item->setText(COL_NAME, s == 0 ? "(top)" : QString::fromStdString(sc.name));
        item->setData(COL_NAME, ScopeRole, s);
        item->setData(COL_NAME, ScopeRole + 1, path);
        item->setToolTip(COL_NAME, s == 0 ? "The top level" : path);
        item->setText(COL_MODULE, QString::fromStdString(sc.type));
        item->setText(COL_CELLS, QString::number(sc.total));
        item->setText(COL_SHARE, percent(sc.total, total));
        item->setText(COL_AREA, area_use(census, use[s]));
        for (int c : {COL_CELLS, COL_SHARE, COL_AREA})
            item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
        bool on = colours.contains(path);
        item->setCheckState(COL_NAME, on ? Qt::Checked : Qt::Unchecked);
        item->setIcon(COL_NAME, swatch(on ? colours.value(path) : QColor()));
        if (path == current)
            currentItem = item;
    }
    for (int s = 0; s < int(items.size()); s++)
        items[s]->setExpanded(first ? s == 0 : expanded.contains(items[s]->data(COL_NAME, ScopeRole + 1).toString()));
    filling = false;
    for (int c = 0; c < COL_COUNT; c++)
        tree->resizeColumnToContents(c);
    quiet = true;
    tree->setCurrentItem(currentItem != nullptr ? currentItem : (items.empty() ? nullptr : items[0]));
    quiet = false;
    applyFilter();
    if (!first)
        tree->verticalScrollBar()->setValue(scroll);
    updateUsage();
    // (the cells of the instance may have changed; the Cells tab ignores the same set)
    emitScope();
}

bool HierarchyDock::busy() const { return es != nullptr && es->busy(); }

int HierarchyDock::currentScope() const
{
    QTreeWidgetItem *item = tree->currentItem();
    if (item == nullptr)
        return scopes.scopes.empty() ? -1 : 0;
    int s = item->data(COL_NAME, ScopeRole).toInt();
    return s >= 0 && s < int(scopes.scopes.size()) ? s : -1;
}

void HierarchyDock::updateUsage()
{
    usage->clear();
    int s = currentScope();
    if (ctx == nullptr || s < 0 || filling || busy()) {
        stats->clear();
        return;
    }
    const auto &sc = scopes.scopes[s];
    ScopeUsage u;
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        u = ScopeUsage::of(ctx, scopes.cellsBelow(s));
    }
    QStringList lines;
    QString name = s == 0 ? QString("Top level") : QString::fromStdString(sc.path);
    if (!sc.type.empty())
        name += QString(" (%1)").arg(QString::fromStdString(sc.type));
    lines << QString("<b>%1</b>: %2 cells (%3 directly), %4 placed")
                     .arg(name.toHtmlEscaped())
                     .arg(u.cells)
                     .arg(sc.cells.size())
                     .arg(u.placed);
    if (u.placed > 0) {
        int w = u.box.x1 - u.box.x0 + 1, h = u.box.y1 - u.box.y0 + 1;
        std::string type = u.mainBelType();
        QString t = QString::fromStdString(type).toHtmlEscaped();
        int used = census.sitesOf(type, u.sites), dev = census.sites(type);
        lines << QString("Uses %1 of the %2 sites with %3 bels (%4)").arg(used).arg(dev).arg(t).arg(percent(used, dev));
        lines << QString("Area X %1..%2, Y %3..%4 (%5 %10 %6): uses %7 of the %8 sites with %9 bels in it")
                         .arg(u.box.x0)
                         .arg(u.box.x1)
                         .arg(u.box.y0)
                         .arg(u.box.y1)
                         .arg(w)
                         .arg(h)
                         .arg(area_use(census, u))
                         .arg(census.sitesIn(type, u.box))
                         .arg(t)
                         .arg(QChar(0xd7));
    }
    QStringList regions;
    for (auto &r : u.regions)
        if (!r.first.empty())
            regions << QString("%1 (%2 cells)").arg(QString::fromStdString(r.first).toHtmlEscaped()).arg(r.second);
    if (!regions.isEmpty())
        lines << "Region constraints: " + regions.join(", ");
    stats->setText(lines.join("<br>"));

    for (auto &t : u.by_bel_type) {
        int dev = census.total(t.first);
        auto *item = new QTreeWidgetItem(usage);
        item->setText(0, QString::fromStdString(t.first));
        item->setText(1, QString::number(t.second));
        item->setText(2, QString::number(dev));
        item->setText(3, percent(t.second, dev));
    }
    for (auto &t : u.unplaced_by_cell_type) {
        auto *item = new QTreeWidgetItem(usage);
        item->setText(0, QString("%1 (unplaced)").arg(QString::fromStdString(t.first)));
        item->setText(1, QString::number(t.second));
    }
    for (int i = 0; i < usage->topLevelItemCount(); i++)
        for (int c = 1; c < 4; c++)
            usage->topLevelItem(i)->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
    for (int c = 0; c < 4; c++)
        usage->resizeColumnToContents(c);
}

void HierarchyDock::updateView()
{
    if (view == nullptr)
        return;
    if (ctx == nullptr || isHidden() || scopes.scopes.empty()) {
        view->setBelFills({}, {});
        return;
    }
    if (busy())
        return; // (the design changes under the task; the refresh after it updates the colours)
    // The coloured instance each instance's cells take their colour from
    int n = int(scopes.scopes.size());
    std::vector<int> source(n, -1);
    for (int s = 0; s < n; s++) {
        if (colours.contains(QString::fromStdString(scopes.scopes[s].path)))
            source[s] = s;
        else if (s != 0)
            source[s] = source[scopes.scopes[s].parent];
    }
    std::vector<std::pair<BelId, QColor>> fills;
    struct Area
    {
        bool any = false;
        float x0, y0, x1, y1;
    };
    std::vector<Area> areas(n);
    {
        std::lock_guard<std::mutex> lock_ui(ctx->ui_mutex);
        std::lock_guard<std::mutex> lock(ctx->mutex);
        for (auto &cs : scopes.cell_scope) {
            int k = source[cs.second];
            if (k < 0)
                continue;
            auto found = ctx->cells.find(cs.first);
            if (found == ctx->cells.end() || found->second->bel == BelId())
                continue;
            BelId bel = found->second->bel;
            QColor c = colours.value(QString::fromStdString(scopes.scopes[k].path));
            c.setAlpha(FILL_ALPHA);
            fills.emplace_back(bel, c);
            float x0, y0, x1, y1;
            if (outlines->isChecked() && view->belArea(bel, x0, y0, x1, y1)) {
                Area &a = areas[k];
                if (!a.any) {
                    a = Area{true, x0, y0, x1, y1};
                } else {
                    a.x0 = std::min(a.x0, x0);
                    a.y0 = std::min(a.y0, y0);
                    a.x1 = std::max(a.x1, x1);
                    a.y1 = std::max(a.y1, y1);
                }
            }
        }
    }
    std::vector<FPGAViewWidget::Overlay> boxes;
    for (int s = 0; s < n; s++)
        if (areas[s].any) {
            FPGAViewWidget::Overlay o;
            o.x0 = areas[s].x0;
            o.y0 = areas[s].y0;
            o.x1 = areas[s].x1;
            o.y1 = areas[s].y1;
            o.color = colours.value(QString::fromStdString(scopes.scopes[s].path));
            o.label = s == 0 ? QString("(top)") : QString::fromStdString(scopes.scopes[s].path);
            boxes.push_back(o);
        }
    view->setBelFills(fills, boxes);
}

void HierarchyDock::applyFilter()
{
    QString text = filter->text().trimmed();
    if (items.empty())
        return;
    // Visible: matches, or something below matches (children come after their parents)
    int n = int(items.size());
    std::vector<bool> show(n, text.isEmpty());
    if (!text.isEmpty()) {
        for (int s = n - 1; s >= 0; s--) {
            if (QString::fromStdString(scopes.scopes[s].path).contains(text, Qt::CaseInsensitive))
                show[s] = true;
            if (show[s] && s != 0)
                show[scopes.scopes[s].parent] = true;
        }
        show[0] = true;
    }
    for (int s = 0; s < n; s++) {
        items[s]->setHidden(!show[s]);
        if (!text.isEmpty() && show[s] && s != 0)
            items[scopes.scopes[s].parent]->setExpanded(true);
    }
}

QColor HierarchyDock::nextColour() const
{
    QSet<QRgb> used;
    for (auto &c : colours)
        used.insert(c.rgb());
    for (int i = 0; i < PALETTE_SIZE; i++)
        if (!used.contains(PALETTE[i].rgb()))
            return PALETTE[i];
    return PALETTE[colours.size() % PALETTE_SIZE];
}

void HierarchyDock::setColoured(int scope, bool on)
{
    if (scope < 0 || scope >= int(scopes.scopes.size()))
        return;
    QString path = QString::fromStdString(scopes.scopes[scope].path);
    if (on && !colours.contains(path))
        colours[path] = nextColour();
    else if (!on)
        colours.remove(path);
    if (scope < int(items.size()) && items[scope] != nullptr) {
        filling = true;
        items[scope]->setCheckState(COL_NAME, on ? Qt::Checked : Qt::Unchecked);
        items[scope]->setIcon(COL_NAME, swatch(on ? colours.value(path) : QColor()));
        filling = false;
    }
}

void HierarchyDock::itemChanged(QTreeWidgetItem *item, int column)
{
    if (filling || column != COL_NAME || item == nullptr)
        return;
    setColoured(item->data(COL_NAME, ScopeRole).toInt(), item->checkState(COL_NAME) == Qt::Checked);
    updateView();
}

void HierarchyDock::colourChildren()
{
    int s = currentScope();
    if (s < 0)
        return;
    // Only the instances that are there now: colours of instances of another design stay
    for (int i = 0; i < int(scopes.scopes.size()); i++)
        setColoured(i, false);
    for (int k : scopes.scopes[s].children)
        setColoured(k, true);
    // An instance without children: colour itself
    if (scopes.scopes[s].children.empty())
        setColoured(s, true);
    if (s < int(items.size()))
        items[s]->setExpanded(true);
    updateView();
}

void HierarchyDock::clearColours()
{
    for (int i = 0; i < int(scopes.scopes.size()); i++)
        setColoured(i, false);
    colours.clear();
    updateView();
}

std::vector<IdString> HierarchyDock::currentCells() const
{
    int s = currentScope();
    if (s <= 0)
        return {};
    return scopes.cellsBelow(s);
}

void HierarchyDock::emitScope()
{
    int s = currentScope();
    if (s <= 0 || busy())
        Q_EMIT cellScopeChanged(QString(), {});
    else
        Q_EMIT cellScopeChanged(QString::fromStdString(scopes.scopes[s].path), currentCells());
}

void HierarchyDock::highlightCurrent()
{
    if (ctx == nullptr || busy())
        return;
    Q_EMIT selectCellsRequested(currentCells());
}

void HierarchyDock::selectCurrent()
{
    if (currentScope() <= 0 || ctx == nullptr || view == nullptr || busy())
        return;
    highlightCurrent();
    view->zoomSelected();
}

void HierarchyDock::contextMenu(const QPoint &pos)
{
    QTreeWidgetItem *item = tree->itemAt(pos);
    if (item == nullptr)
        return;
    tree->setCurrentItem(item);
    int s = item->data(COL_NAME, ScopeRole).toInt();
    QString path = item->data(COL_NAME, ScopeRole + 1).toString();
    QMenu menu(this);
    QAction *colour = menu.addAction("Colour");
    colour->setCheckable(true);
    colour->setChecked(colours.contains(path));
    connect(colour, &QAction::toggled, this, [this, s](bool on) {
        setColoured(s, on);
        updateView();
    });
    menu.addAction("Change colour...", this, [this, s, path] {
        QColor c = QColorDialog::getColor(colours.value(path, nextColour()), this, "Instance colour");
        if (!c.isValid())
            return;
        colours[path] = c;
        setColoured(s, true);
        updateView();
    });
    menu.addAction("Colour children", this, &HierarchyDock::colourChildren);
    menu.addAction("Clear colours", this, &HierarchyDock::clearColours);
    menu.addSeparator();
    menu.addAction("Select cells", this, &HierarchyDock::selectCurrent);
    menu.exec(tree->viewport()->mapToGlobal(pos));
}

NEXTPNR_NAMESPACE_END
