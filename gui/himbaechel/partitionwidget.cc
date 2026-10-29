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

#include "partitionwidget.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>
#include <cmath>
#include <set>

NEXTPNR_NAMESPACE_BEGIN

namespace {

// The colours of the partitions, those of the device view's highlight groups (cells are highlighted too)
const char *partition_colors[8] = {"#6495ed", "#7fffd4", "#98fb98", "#ffd700",
                                   "#cd5c5c", "#fa8072", "#ff69b4", "#da70d6"};

void set_option(po::variables_map &options, const std::string &key, const boost::any &value)
{
    options.erase(key);
    options.insert(std::make_pair(key, po::variable_value(value, false)));
}

} // namespace

PartitionWidget::PartitionWidget(FPGAViewWidget *view, QWidget *parent) : QWidget(parent), view(view)
{
    auto spin = [&](QWidget *parent) {
        auto *s = new QSpinBox(parent);
        s->setRange(-999, 999);
        return s;
    };

    // Step 1: this design as a partition
    asPartition = new QGroupBox("Implement this design as a partition");
    asPartition->setCheckable(true);
    asPartition->setChecked(false);
    asPartition->setToolTip("The top level ports become the partition's ports (no IO pads). Pack, place and route,\n"
                            "finish, then Save JSON writes the partition for importing it into another design.");
    placedOnly = new QCheckBox("Placed only (route it with the importing design)");
    placedOnly->setToolTip("Written after placing, without routing (partition-unrouted)");
    useRegion = new QCheckBox("Region");
    useRegion->setToolTip("The partition's area: its cells are placed inside, and its routing stays inside\n"
                          "(bel coordinates, as X..Y.. in the bel names)");
    regionX0 = spin(asPartition);
    regionY0 = spin(asPartition);
    regionX1 = spin(asPartition);
    regionY1 = spin(asPartition);
    pickRegion = new QPushButton("Pick");
    pickRegion->setToolTip("Drag the region open in the device view (Escape cancels)");
    pickRegion->setCheckable(true);
    auto *regionRow = new QHBoxLayout();
    regionRow->addWidget(useRegion);
    regionRow->addWidget(new QLabel("X"));
    regionRow->addWidget(regionX0);
    regionRow->addWidget(new QLabel("Y"));
    regionRow->addWidget(regionY0);
    regionRow->addWidget(new QLabel("to X"));
    regionRow->addWidget(regionX1);
    regionRow->addWidget(new QLabel("Y"));
    regionRow->addWidget(regionY1);
    regionRow->addWidget(pickRegion);
    regionRow->addStretch();
    auto *stepOne = new QVBoxLayout(asPartition);
    stepOne->addWidget(placedOnly);
    stepOne->addLayout(regionRow);

    // Step 2: partitions imported into this design
    importBox = new QGroupBox("Import partitions");
    importList = new QTreeWidget();
    importList->setColumnCount(6);
    importList->setHeaderLabels({"Instance", "File", "Offset", "load", "place", "route"});
    importList->setRootIsDecorated(false);
    importList->setSelectionMode(QAbstractItemView::SingleSelection);
    importList->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    importList->setMinimumHeight(80);
    addButton = new QPushButton("Add");
    addButton->setToolTip("Import a partition in place of a blackbox instance of the design");
    removeButton = new QPushButton("Remove");
    auto *listButtons = new QHBoxLayout();
    listButtons->addWidget(addButton);
    listButtons->addWidget(removeButton);
    listButtons->addStretch();

    instance = new QComboBox();
    instance->setToolTip("The blackbox instance the partition replaces");
    file = new QLineEdit();
    file->setToolTip("The partition, written with as-partition");
    browseButton = new QPushButton("...");
    auto *fileRow = new QHBoxLayout();
    fileRow->addWidget(file);
    fileRow->addWidget(browseButton);
    offsetX = spin(importBox);
    offsetY = spin(importBox);
    pickOffset = new QPushButton("Pick");
    pickOffset->setCheckable(true);
    pickOffset->setToolTip("Place the partition in the device view: it follows the mouse to where it can go, a "
                           "click puts it there (Escape cancels)");
    auto *offsetRow = new QHBoxLayout();
    offsetRow->addWidget(new QLabel("dX"));
    offsetRow->addWidget(offsetX);
    offsetRow->addWidget(new QLabel("dY"));
    offsetRow->addWidget(offsetY);
    offsetRow->addWidget(pickOffset);
    offsetRow->addStretch();
    load = new QComboBox();
    load->addItems({"auto", "routing", "placement"});
    load->setToolTip("What is taken from the file: routing (with the placement), or the placement only.\n"
                     "auto: the routing if the file has some");
    place = new QComboBox();
    place->addItems({"repair", "locked"});
    place->setToolTip("After placing, the cells are open to the passes after placement (repair) or stay (locked)");
    route = new QComboBox();
    route->addItems({"open", "repair", "locked"});
    route->setToolTip("open: the router's starting point, it may change it\n"
                      "repair: locked while routing, then open to the repairs\n"
                      "locked: stays as loaded");
    auto *form = new QFormLayout();
    form->addRow("Instance", instance);
    form->addRow("File", fileRow);
    form->addRow("Offset", offsetRow);
    form->addRow("load", load);
    form->addRow("place", place);
    form->addRow("route", route);
    info = new QLabel();
    info->setWordWrap(true);
    openAreas = new QCheckBox("Open the partitions' areas to the design");
    openAreas->setToolTip("The design may place cells in the free tiles of the partitions' rectangles, where the\n"
                          "partitions' routing leaves a way in (partition-open)");
    auto *stepTwo = new QVBoxLayout(importBox);
    stepTwo->addWidget(importList);
    stepTwo->addLayout(listButtons);
    stepTwo->addLayout(form);
    stepTwo->addWidget(info);
    stepTwo->addWidget(openAreas);

    showAreas = new QCheckBox("Show the areas in the device view");
    showAreas->setChecked(true);
    auto *hint = new QLabel("The options take effect when packing.");
    hint->setWordWrap(true);

    auto *content = new QWidget();
    auto *layout = new QVBoxLayout(content);
    layout->addWidget(hint);
    layout->addWidget(asPartition);
    layout->addWidget(importBox);
    layout->addWidget(showAreas);
    layout->addStretch();
    auto *scroll = new QScrollArea();
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    connect(asPartition, &QGroupBox::toggled, this, &PartitionWidget::updateOverlays);
    connect(useRegion, &QCheckBox::toggled, this, &PartitionWidget::updateOverlays);
    for (auto *s : {regionX0, regionY0, regionX1, regionY1})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] {
            if (!updating) {
                useRegion->setChecked(true);
                updateOverlays();
            }
        });
    connect(pickRegion, &QPushButton::clicked, this, [this](bool on) { on ? startPick(PICK_REGION) : stopPick(); });
    connect(pickOffset, &QPushButton::clicked, this, [this](bool on) { on ? startPick(PICK_OFFSET) : stopPick(); });
    connect(importList, &QTreeWidget::currentItemChanged, this, [this] {
        if (updating)
            return;
        stopPick();
        current = importList->currentItem() ? importList->indexOfTopLevelItem(importList->currentItem()) : -1;
        updateEditor();
        updateOverlays();
    });
    connect(addButton, &QPushButton::clicked, this, &PartitionWidget::addImport);
    connect(removeButton, &QPushButton::clicked, this, &PartitionWidget::removeImport);
    connect(browseButton, &QPushButton::clicked, this, &PartitionWidget::browseFile);
    connect(instance, &QComboBox::currentTextChanged, this, &PartitionWidget::editChanged);
    connect(file, &QLineEdit::editingFinished, this, &PartitionWidget::editChanged);
    for (auto *s : {offsetX, offsetY})
        connect(s, QOverload<int>::of(&QSpinBox::valueChanged), this, &PartitionWidget::editChanged);
    for (auto *c : {load, place, route})
        connect(c, &QComboBox::currentTextChanged, this, &PartitionWidget::editChanged);
    connect(showAreas, &QCheckBox::toggled, this, &PartitionWidget::updateOverlays);
    connect(view, &FPGAViewWidget::picked, this, &PartitionWidget::onPicked);
    connect(view, &FPGAViewWidget::pickCanceled, this, [this] {
        picking = PICK_NONE;
        stopPick();
    });

    updateEditor();
}

void PartitionWidget::newContext(Context *ctx)
{
    stopPick();
    this->ctx = ctx;
    if (!optionsRead) {
        // What the command line gave, once
        optionsRead = true;
        readOptions();
    }
    designChanged();
}

void PartitionWidget::readOptions()
{
    auto &options = ctx->args.options;
    asPartition->setChecked(options.count("as-partition") || options.count("partition-unrouted"));
    placedOnly->setChecked(options.count("partition-unrouted"));
    openAreas->setChecked(options.count("partition-open"));
    if (options.count("partition-region")) {
        QStringList f = QString::fromStdString(options["partition-region"].as<std::string>()).split(',');
        if (f.size() == 4) {
            updating = true;
            regionX0->setValue(f[0].toInt());
            regionY0->setValue(f[1].toInt());
            regionX1->setValue(f[2].toInt());
            regionY1->setValue(f[3].toInt());
            updating = false;
            useRegion->setChecked(true);
        }
    }
    if (!options.count("partition"))
        return;
    auto option = [&](const char *key, const QString &value) {
        std::string name = std::string("partition-") + key;
        return options.count(name) ? QString::fromStdString(options[name].as<std::string>()) : value;
    };
    // <inst>:<file>[@dx,dy][,key=value..], as the uarch reads it
    static const QRegularExpression option_re(R"(,([a-z]+)=([a-z]+)$)"), offset_re(R"(@(-?\d+),(-?\d+)$)");
    for (auto &spec : options["partition"].as<std::vector<std::string>>()) {
        QString s = QString::fromStdString(spec);
        int colon = s.indexOf(':');
        if (colon <= 0)
            continue;
        Import imp;
        imp.inst = s.left(colon);
        QString rest = s.mid(colon + 1);
        imp.load = option("load", "auto");
        imp.place = option("place", "repair");
        imp.route = option("route", "open");
        std::set<QString> given;
        QRegularExpressionMatch m;
        while ((m = option_re.match(rest)).hasMatch()) {
            QString key = m.captured(1), value = m.captured(2);
            if (given.insert(key).second) {
                if (key == "load")
                    imp.load = value;
                else if (key == "place")
                    imp.place = value;
                else if (key == "route")
                    imp.route = value;
            }
            rest = rest.left(m.capturedStart(0));
        }
        if ((m = offset_re.match(rest)).hasMatch()) {
            imp.dx = m.captured(1).toInt();
            imp.dy = m.captured(2).toInt();
            rest = rest.left(m.capturedStart(0));
        }
        imp.file = rest;
        readFootprint(imp);
        imports.push_back(imp);
    }
    current = imports.empty() ? -1 : 0;
}

bool PartitionWidget::editable() const { return ctx && !ctx->cells.empty() && !ctx->settings.count(ctx->id("pack")); }

void PartitionWidget::designChanged()
{
    if (!ctx)
        return;
    bool supported = ctx->uarch->hasPartitions();
    setEnabled(supported);
    bool edit = supported && editable();
    asPartition->setEnabled(edit);
    importBox->setEnabled(edit);
    if (!edit)
        stopPick();
    // The design's blackbox instances (none once packed)
    updating = true;
    QString kept = instance->currentText();
    instance->clear();
    if (edit)
        for (IdString inst : ctx->uarch->getPartitionInstances())
            instance->addItem(QString::fromStdString(inst.str(ctx)));
    updating = false;
    updateList();
    updateEditor();
    updateOverlays();
    highlightCells();
}

void PartitionWidget::readFootprint(Import &imp)
{
    imp.problems.clear();
    imp.footprint = HimbaechelAPI::PartitionFootprint();
    imp.read = !imp.file.isEmpty() && ctx && ctx->uarch->readPartitionFootprint(imp.file.toStdString(), imp.footprint);
}

const std::string &PartitionWidget::moveProblem(Import &imp, int dx, int dy)
{
    auto found = imp.problems.find({dx, dy});
    if (found != imp.problems.end())
        return found->second;
    return imp.problems[{dx, dy}] = ctx->uarch->partitionMoveProblem(imp.footprint, dx, dy);
}

QString PartitionWidget::placeProblem(Import &imp, int dx, int dy)
{
    const std::string &problem = moveProblem(imp, dx, dy);
    if (!problem.empty())
        return QString::fromStdString(problem);
    // The cells of two partitions can not share a tile
    std::set<std::pair<int, int>> tiles;
    for (const Loc &loc : imp.footprint.tiles)
        tiles.insert({loc.x + dx, loc.y + dy});
    for (auto &other : imports) {
        if (&other == &imp || !other.read)
            continue;
        for (const Loc &loc : other.footprint.tiles)
            if (tiles.count({loc.x + other.dx, loc.y + other.dy}))
                return QString("its cells would share tiles with those of %1").arg(other.inst);
    }
    return QString();
}

bool PartitionWidget::snapOffset(Import &imp, int &dx, int &dy)
{
    // Rings around (dx, dy), nearest first; the kinds of tile repeat every few tiles (32 by 16 with RAMs)
    const int reach = 32;
    int best_dx = 0, best_dy = 0;
    float best = -1;
    for (int r = 0; r <= reach; r++) {
        if (best >= 0 && best < r)
            break; // no ring further out has a nearer offset
        for (int y = -r; y <= r; y++)
            for (int x = -r; x <= r; x++) {
                if (std::max(std::abs(x), std::abs(y)) != r)
                    continue;
                float d = std::hypot(float(x), float(y));
                if ((best >= 0 && d >= best) || !placeProblem(imp, dx + x, dy + y).isEmpty())
                    continue;
                best = d;
                best_dx = dx + x;
                best_dy = dy + y;
            }
    }
    if (best < 0)
        return false;
    dx = best_dx;
    dy = best_dy;
    return true;
}

void PartitionWidget::updateList()
{
    updating = true;
    importList->clear();
    for (auto &imp : imports) {
        auto *item = new QTreeWidgetItem(importList);
        item->setText(0, imp.inst);
        item->setText(1, QFileInfo(imp.file).fileName());
        item->setToolTip(1, imp.file);
        item->setText(2, QString("%1,%2").arg(imp.dx).arg(imp.dy));
        item->setText(3, imp.load);
        item->setText(4, imp.place);
        item->setText(5, imp.route);
        QColor color(partition_colors[(&imp - imports.data()) % 8]);
        item->setForeground(0, color);
    }
    if (current >= 0 && current < importList->topLevelItemCount())
        importList->setCurrentItem(importList->topLevelItem(current));
    updating = false;
}

void PartitionWidget::updateEditor()
{
    bool has = current >= 0 && current < int(imports.size());
    for (QWidget *w : std::initializer_list<QWidget *>{instance, file, browseButton, offsetX, offsetY, pickOffset,
                                                        load, place, route, removeButton})
        w->setEnabled(has);
    updating = true;
    if (has) {
        Import &imp = imports.at(current);
        if (instance->findText(imp.inst) < 0)
            instance->addItem(imp.inst);
        instance->setCurrentText(imp.inst);
        file->setText(imp.file);
        offsetX->setValue(imp.dx);
        offsetY->setValue(imp.dy);
        load->setCurrentText(imp.load);
        place->setCurrentText(imp.place);
        route->setCurrentText(imp.route);
    } else {
        file->clear();
        offsetX->setValue(0);
        offsetY->setValue(0);
    }
    updating = false;
    updateInfo();
}

void PartitionWidget::updateInfo()
{
    if (current < 0 || current >= int(imports.size())) {
        info->setText(imports.empty() ? "Add a partition to import it in place of a blackbox instance." : "");
        return;
    }
    Import &imp = imports.at(current);
    if (imp.file.isEmpty()) {
        info->setText("Choose the partition's file.");
        return;
    }
    if (!imp.read) {
        info->setText(QString("<span style='color:#cd5c5c'>%1 %2.</span>")
                              .arg(QFileInfo(imp.file).fileName(), QString::fromStdString(imp.footprint.error)));
        return;
    }
    auto &fp = imp.footprint;
    QString text = QString("<b>%1</b>: %2 cells, %3, %4 ports, area X%5Y%6 to X%7Y%8")
                           .arg(QFileInfo(imp.file).fileName())
                           .arg(fp.cells)
                           .arg(fp.routed ? "placed and routed" : "placed only")
                           .arg(fp.ports.size())
                           .arg(fp.area.x0 + imp.dx)
                           .arg(fp.area.y0 + imp.dy)
                           .arg(fp.area.x1 + imp.dx)
                           .arg(fp.area.y1 + imp.dy);
    if (!fp.routed && imp.load == "routing")
        text += "<br><span style='color:#cd5c5c'>The file has no routing to load.</span>";
    if (editable()) {
        QString problem = placeProblem(imp, imp.dx, imp.dy);
        if (!problem.isEmpty())
            text += QString("<br><span style='color:#cd5c5c'>Can not be moved by (%1, %2): %3.</span>")
                            .arg(imp.dx)
                            .arg(imp.dy)
                            .arg(problem);
    }
    info->setText(text);
}

void PartitionWidget::editChanged()
{
    if (updating || current < 0 || current >= int(imports.size()))
        return;
    Import &imp = imports.at(current);
    imp.inst = instance->currentText();
    if (imp.file != file->text()) {
        imp.file = file->text();
        readFootprint(imp);
    }
    imp.dx = offsetX->value();
    imp.dy = offsetY->value();
    imp.load = load->currentText();
    imp.place = place->currentText();
    imp.route = route->currentText();
    updateList();
    updateInfo();
    updateOverlays();
}

void PartitionWidget::addImport()
{
    Import imp;
    // The first instance without a partition yet
    for (int i = 0; i < instance->count(); i++) {
        QString name = instance->itemText(i);
        if (std::none_of(imports.begin(), imports.end(), [&](const Import &other) { return other.inst == name; })) {
            imp.inst = name;
            break;
        }
    }
    // The file of the one selected, as one file can be imported several times
    if (current >= 0 && current < int(imports.size())) {
        imp.file = imports.at(current).file;
        imp.load = imports.at(current).load;
        imp.place = imports.at(current).place;
        imp.route = imports.at(current).route;
        readFootprint(imp);
    }
    imports.push_back(imp);
    current = int(imports.size()) - 1;
    updateList();
    updateEditor();
    updateOverlays();
    if (imp.file.isEmpty())
        browseFile();
}

void PartitionWidget::removeImport()
{
    if (current < 0 || current >= int(imports.size()))
        return;
    stopPick();
    imports.erase(imports.begin() + current);
    current = std::min(current, int(imports.size()) - 1);
    updateList();
    updateEditor();
    updateOverlays();
}

void PartitionWidget::browseFile()
{
    QString name = QFileDialog::getOpenFileName(this, "Partition", file->text(), "*.json");
    if (name.isEmpty())
        return;
    file->setText(name);
    editChanged();
}

void PartitionWidget::startPick(int what)
{
    if (what == PICK_OFFSET && (current < 0 || !imports.at(current).read)) {
        pickOffset->setChecked(false);
        return;
    }
    picking = what == PICK_REGION ? PICK_REGION : PICK_OFFSET;
    pickRegion->setChecked(picking == PICK_REGION);
    pickOffset->setChecked(picking == PICK_OFFSET);
    dragging = false;
    ghostShown = false;
    view->setPicking(true);
}

void PartitionWidget::stopPick()
{
    if (picking != PICK_NONE)
        view->setPicking(false);
    picking = PICK_NONE;
    dragging = false;
    ghostShown = false;
    pickRegion->setChecked(false);
    pickOffset->setChecked(false);
    updateOverlays();
}

void PartitionWidget::onPicked(int event, float x, float y)
{
    if (!ctx || picking == PICK_NONE)
        return;
    int tx = tileX(x), ty = tileY(y);
    if (picking == PICK_REGION) {
        if (event == FPGAViewWidget::PICK_PRESS) {
            dragging = true;
            dragX = tx;
            dragY = ty;
        }
        if (!dragging || event == FPGAViewWidget::PICK_MOVE)
            return;
        updating = true;
        regionX0->setValue(std::min(dragX, tx));
        regionY0->setValue(std::min(dragY, ty));
        regionX1->setValue(std::max(dragX, tx));
        regionY1->setValue(std::max(dragY, ty));
        updating = false;
        useRegion->setChecked(true);
        updateOverlays();
        if (event == FPGAViewWidget::PICK_RELEASE)
            stopPick();
        return;
    }
    // The partition follows the mouse (its middle), snapped to where it can go
    Import &imp = imports.at(current);
    const BoundingBox &area = imp.footprint.area;
    int dx = tx - (area.x0 + area.x1) / 2, dy = ty - (area.y0 + area.y1) / 2;
    ghostValid = snapOffset(imp, dx, dy);
    ghostDx = dx;
    ghostDy = dy;
    ghostShown = true;
    if (event == FPGAViewWidget::PICK_PRESS && ghostValid) {
        updating = true;
        offsetX->setValue(dx);
        offsetY->setValue(dy);
        updating = false;
        editChanged();
        stopPick();
        return;
    }
    updateOverlays();
}

int PartitionWidget::tileX(float x) const
{
    // The column drawn there, or the nearest one left of it (a RAM column is drawn beside its CPE column)
    int best = 0;
    for (int col = 0; col < ctx->getGridDimX(); col++)
        if (ctx->getGuiColumnX(col) <= x)
            best = col;
    return best;
}

int PartitionWidget::tileY(float y) const
{
    return std::max(0, std::min(ctx->getGridDimY() - 1, int(std::floor(y))));
}

FPGAViewWidget::Overlay PartitionWidget::tileRect(int x0, int y0, int x1, int y1, QColor color, QString label,
                                                  bool fill) const
{
    FPGAViewWidget::Overlay o;
    o.x0 = ctx->getGuiColumnX(std::min(x0, x1));
    o.x1 = ctx->getGuiColumnX(std::max(x0, x1)) + 1;
    o.y0 = std::min(y0, y1);
    o.y1 = std::max(y0, y1) + 1;
    o.color = color;
    o.label = label;
    o.fill = fill;
    return o;
}

void PartitionWidget::updateOverlays()
{
    std::vector<FPGAViewWidget::Overlay> overlays;
    if (!ctx || !showAreas->isChecked()) {
        view->setOverlays(overlays);
        return;
    }
    bool packed = ctx->settings.count(ctx->id("pack"));
    // This design's region as a partition
    if (asPartition->isChecked() && useRegion->isChecked())
        overlays.push_back(tileRect(regionX0->value(), regionY0->value(), regionX1->value(), regionY1->value(),
                                    QColor("#ff6600"), "partition region", false));
    // The imported partitions where they go (once packed, where they went)
    for (int i = 0; i < int(imports.size()); i++) {
        Import &imp = imports.at(i);
        if (!imp.read)
            continue;
        bool ghost = ghostShown && i == current;
        int dx = ghost ? ghostDx : imp.dx, dy = ghost ? ghostDy : imp.dy;
        bool valid = ghost ? ghostValid : packed || placeProblem(imp, dx, dy).isEmpty();
        QColor color = valid ? QColor(partition_colors[i % 8]) : QColor("#ff3030");
        // Pale, so that the nets drawn under it stay visible
        QColor tiles = QColor::fromHsv(color.hue(), color.saturation() / 2, color.value(), i == current ? 55 : 30);
        // The tiles of its cells, and its area
        for (const Loc &loc : imp.footprint.tiles)
            overlays.push_back(tileRect(loc.x + dx, loc.y + dy, loc.x + dx, loc.y + dy, tiles, "", true));
        const BoundingBox &a = imp.footprint.area;
        overlays.push_back(tileRect(a.x0 + dx, a.y0 + dy, a.x1 + dx, a.y1 + dy, color,
                                    QString("%1 (%2,%3)").arg(imp.inst).arg(dx).arg(dy), false));
    }
    view->setOverlays(overlays);
}

void PartitionWidget::highlightCells()
{
    // Once imported, the cells of each partition (named <instance>.<cell>) in its colour
    for (int i = 0; i < 8; i++) {
        std::vector<DecalXY> decals;
        if (ctx && ctx->settings.count(ctx->id("pack")) && i < int(imports.size()) && showAreas->isChecked()) {
            std::string prefix = imports.at(i).inst.toStdString() + ".";
            for (auto &cell : ctx->cells)
                if (cell.second->bel != BelId() && cell.first.str(ctx).rfind(prefix, 0) == 0)
                    decals.push_back(ctx->getBelDecal(cell.second->bel));
        }
        view->onHighlightGroupChanged(decals, i);
    }
}

bool PartitionWidget::apply(QString &error)
{
    if (!ctx || !ctx->uarch->hasPartitions())
        return true;
    stopPick();
    auto &options = ctx->args.options;
    for (auto key : {"as-partition", "partition-unrouted", "partition-region", "partition", "partition-open",
                     "partition-load", "partition-place", "partition-route"})
        options.erase(key);
    if (asPartition->isChecked()) {
        set_option(options, "as-partition", std::string());
        if (placedOnly->isChecked())
            set_option(options, "partition-unrouted", std::string());
        if (useRegion->isChecked())
            set_option(options, "partition-region",
                       stringf("%d,%d,%d,%d", regionX0->value(), regionY0->value(), regionX1->value(),
                               regionY1->value()));
    }
    std::vector<std::string> specs;
    std::set<QString> used;
    for (auto &imp : imports) {
        if (imp.inst.isEmpty() || imp.file.isEmpty()) {
            error = "A partition to import has no instance or no file.";
            return false;
        }
        if (!used.insert(imp.inst).second) {
            error = QString("Instance '%1' has two partitions.").arg(imp.inst);
            return false;
        }
        if (!imp.read) {
            error = QString("The partition for '%1': %2 %3.")
                            .arg(imp.inst, imp.file, QString::fromStdString(imp.footprint.error));
            return false;
        }
        QString problem = placeProblem(imp, imp.dx, imp.dy);
        if (!problem.isEmpty()) {
            error = QString("The partition for '%1' can not be moved by (%2, %3): %4.")
                            .arg(imp.inst)
                            .arg(imp.dx)
                            .arg(imp.dy)
                            .arg(problem);
            return false;
        }
        std::string spec = stringf("%s:%s@%d,%d", imp.inst.toUtf8().constData(), imp.file.toUtf8().constData(),
                                   imp.dx, imp.dy);
        if (imp.load != "auto")
            spec += ",load=" + imp.load.toStdString();
        spec += ",place=" + imp.place.toStdString() + ",route=" + imp.route.toStdString();
        specs.push_back(spec);
    }
    if (!specs.empty())
        set_option(options, "partition", specs);
    if (openAreas->isChecked())
        set_option(options, "partition-open", std::string());
    return true;
}

NEXTPNR_NAMESPACE_END
