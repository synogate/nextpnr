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

// For the Paths and the Exceptions tab
namespace TimingText {
// The decals of a path's cells and of the routing of its nets (to select it in the device view), and of a segment
std::vector<DecalXY> pathDecals(Context *ctx, const CriticalPath &path);
std::vector<DecalXY> segmentDecals(Context *ctx, const CriticalPath::Segment &segment);
// "posedge <net>", or "async"
QString clockName(Context *ctx, const ClockEvent &clock);
// The constraint of a clock net: "'name': period 10.000 ns (100.00 MHz), high 5.000 ns, low 5.000 ns", or the
// default target if it has none
QString clockConstraint(Context *ctx, IdString clock);
// "line 3: set_multicycle_path 2 -from ..." for an index of ctx->timing_exceptions
QString exceptionName(Context *ctx, int index);
// What an exception matching the paths of a domain pair does to them: applies to setup / hold, is overridden by
// another one, or is not applied (false paths, paths of unclocked points)
QString exceptionStatus(Context *ctx, const TimingAnalyser::PairConstraints &pc, int index);
} // namespace TimingText

// Lists the worst timing paths, optionally only from one launch clock edge or to one latch clock edge (or async),
// sorted by setup slack, hold slack or delay. A selected path is selected in the device view; below it, how its
// slacks come about (the required time from the clocks and the timing exceptions, the arrival time of the data,
// the constraints on it) and its segments.
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
    // The slack calculation of a path and the constraints on it
    void showSlack(int index);
    QString formatDelay(delay_t delay);
    // The clock net (for ordering), and that with the edge (also for filtering)
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
    QTreeWidget *slackList;
    QTreeWidget *segmentList;
};

NEXTPNR_NAMESPACE_END

#endif // TIMINGWIDGET_H
