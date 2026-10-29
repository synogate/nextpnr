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

#ifndef PARTITIONWIDGET_H
#define PARTITIONWIDGET_H

#include <QCheckBox>
#include <QComboBox>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTreeWidget>
#include <QWidget>
#include <map>

#include "fpgaviewwidget.h"
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Partitions (see the uarch's docs, e.g. himbaechel/uarch/gatemate/docs/partitions.md): implementing the
// loaded design as a partition, in a region picked in the device view, and importing partitions into it,
// each moved to where it is picked in the device view. The options are set when packing (apply).
class PartitionWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit PartitionWidget(FPGAViewWidget *view, QWidget *parent = 0);

    // Sets the options of the uarch for packing; false with the reason if they are not complete
    bool apply(QString &error);

  public Q_SLOTS:
    void newContext(Context *ctx);
    // After loading, packing and so on: what can still be changed, and the areas shown
    void designChanged();

  private:
    struct Import
    {
        QString inst, file;
        int dx = 0, dy = 0;
        QString load = "auto", place = "repair", route = "open";
        bool read = false;
        HimbaechelAPI::PartitionFootprint footprint;
        // The offsets checked so far, and why they do not work (empty where they do)
        std::map<std::pair<int, int>, std::string> problems;
    };

    void readOptions();
    void readFootprint(Import &imp);
    const std::string &moveProblem(Import &imp, int dx, int dy);
    // Why the partition can not go there: as moveProblem, or it would overlap another one
    QString placeProblem(Import &imp, int dx, int dy);
    // The nearest offset to (dx, dy) the partition can be moved by, if any within reach
    bool snapOffset(Import &imp, int &dx, int &dy);
    void updateList();
    void updateEditor();
    void updateInfo();
    void updateOverlays();
    void highlightCells();
    void editChanged();
    void addImport();
    void removeImport();
    void browseFile();
    void startPick(int what);
    void stopPick();
    void onPicked(int event, float x, float y);
    bool editable() const;
    // Tiles and the view's coordinates
    int tileX(float x) const;
    int tileY(float y) const;
    FPGAViewWidget::Overlay tileRect(int x0, int y0, int x1, int y1, QColor color, QString label, bool fill) const;

    Context *ctx = nullptr;
    FPGAViewWidget *view;
    bool optionsRead = false;
    bool updating = false;

    // Implementing the design as a partition
    QGroupBox *asPartition;
    QCheckBox *placedOnly;
    QCheckBox *useRegion;
    QSpinBox *regionX0, *regionY0, *regionX1, *regionY1;
    QPushButton *pickRegion;

    // Importing partitions
    QGroupBox *importBox;
    QTreeWidget *importList;
    QPushButton *addButton, *removeButton;
    QComboBox *instance;
    QLineEdit *file;
    QPushButton *browseButton;
    QSpinBox *offsetX, *offsetY;
    QPushButton *pickOffset;
    QComboBox *load, *place, *route;
    QCheckBox *openAreas;
    QLabel *info;
    QCheckBox *showAreas;

    std::vector<Import> imports;
    int current = -1;

    enum
    {
        PICK_NONE,
        PICK_REGION,
        PICK_OFFSET
    } picking = PICK_NONE;
    bool dragging = false;
    int dragX = 0, dragY = 0;
    // Where the partition being placed is shown, and whether it can go there
    int ghostDx = 0, ghostDy = 0;
    bool ghostShown = false, ghostValid = false;
};

NEXTPNR_NAMESPACE_END

#endif // PARTITIONWIDGET_H
