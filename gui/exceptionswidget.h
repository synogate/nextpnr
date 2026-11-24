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

#ifndef EXCEPTIONSWIDGET_H
#define EXCEPTIONSWIDGET_H

#include <QLabel>
#include <QSpinBox>
#include <QTreeWidget>
#include <memory>
#include "nextpnr.h"
#include "timing.h"

NEXTPNR_NAMESPACE_BEGIN

// The timing constraints beyond the clocks: the timing exceptions of the SDC file (set_multicycle_path,
// set_false_path), each with the number of paths its -from and -to lists match and the number it applies to, and
// below them the paths of the selected exception (or of all), with what the exceptions do to each. A selected
// path is selected in the device view.
class ExceptionsWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit ExceptionsWidget(QWidget *parent = 0);
    ~ExceptionsWidget();

  protected:
    void showEvent(QShowEvent *event) override;

  Q_SIGNALS:
    void selected(std::vector<DecalXY> decal, bool keep);
    void zoomSelected();

  public Q_SLOTS:
    void newContext(Context *ctx);
    void designChanged();

  private:
    void analyse();
    // The paths of the selected exception (-1: of all exceptions)
    void updatePaths();
    QString formatDelay(delay_t delay);

    struct Path
    {
        TimingAnalyser::EndpointTiming timing;
        TimingAnalyser::PairConstraints constraints;
        CriticalPath path;
    };

    Context *ctx;
    std::unique_ptr<TimingAnalyser> analyser;
    // The end points of the analysis that exceptions match, with their constraints
    std::vector<std::pair<TimingAnalyser::EndpointTiming, TimingAnalyser::PairConstraints>> matched;
    bool dirty;
    std::vector<Path> paths;

    QLabel *summary;
    QTreeWidget *exceptionList;
    QSpinBox *countSpin;
    QTreeWidget *pathList;
    QLabel *pathSummary;
};

NEXTPNR_NAMESPACE_END

#endif // EXCEPTIONSWIDGET_H
