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

#ifndef TIMINGWIDGET_H
#define TIMINGWIDGET_H

#include <QComboBox>
#include <QLabel>
#include <QSpinBox>
#include <QTreeWidget>
#include <memory>
#include "nextpnr.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

// Lists the worst timing paths, optionally only from one launch clock or to one latch clock (or async),
// sorted by setup slack, hold slack or delay. A selected path is selected in the device view and its segments are listed below.
class TimingWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit TimingWidget(QWidget *parent = 0);
    ~TimingWidget();

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    // Runs the timing analysis again if the design has changed
    void analyse();
    void updatePaths();
    void showPath(int index);
    std::vector<DecalXY> getPathDecals(const CriticalPath &path);
    std::vector<DecalXY> getSegmentDecals(const CriticalPath::Segment &segment);
    QString formatDelay(delay_t delay);
    // The clock net (also for filtering), and that with the edge
    QString clockNet(const ClockEvent &clock);
    QString clockName(const ClockEvent &clock);

  Q_SIGNALS:
    void selected(std::vector<DecalXY> decal, bool keep);
    void zoomSelected();

  public Q_SLOTS:
    void newContext(Context *ctx);
    void designChanged();

  private:
    enum SortBy
    {
        SETUP_SLACK,
        HOLD_SLACK,
        DELAY
    };

    struct Path
    {
        TimingAnalyser::EndpointTiming timing;
        CriticalPath path;
    };

    Context *ctx;
    std::unique_ptr<TimingAnalyser> analyser;
    std::vector<TimingAnalyser::EndpointTiming> endpoints;
    bool dirty;
    // A remark on the analysis, shown with the paths
    QString note;

    std::vector<Path> paths;

    QComboBox *launchCombo;
    QComboBox *latchCombo;
    QComboBox *sortCombo;
    QSpinBox *countSpin;
    QTreeWidget *pathList;
    QLabel *summary;
    QTreeWidget *segmentList;
};

NEXTPNR_NAMESPACE_END

#endif // TIMINGWIDGET_H
