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

#ifndef CLOCKSWIDGET_H
#define CLOCKSWIDGET_H

#include <QLabel>
#include <QTreeWidget>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// Lists the clocks of the design with their properties: constraint, achieved Fmax, setup / hold slack of the
// paths they capture, clocked cells, edges, driver, routing and skew; below it the details of the selected clock
// and the paths between clocks. A double click on a clock shows its net.
class ClocksWidget : public QWidget
{
    Q_OBJECT

  public:
    explicit ClocksWidget(QWidget *parent = 0);

  protected:
    void showEvent(QShowEvent *event) override;

  Q_SIGNALS:
    void focusNet(IdString net);

  public Q_SLOTS:
    void newContext(Context *ctx);
    void designChanged();

  private:
    struct Crossing
    {
        ClockEvent launch, capture;
        bool related = false;
        int endpoints = 0;
        delay_t setup_slack, hold_slack;
    };

    struct Clock
    {
        IdString net;
        // the constraint (MHz) and whether it is that of the net (else the default target)
        double target_mhz = 0;
        bool constrained = false;
        QString constraint;
        // the achieved Fmax of the paths within the clock (0: none)
        double fmax_mhz = 0;
        // over the endpoints this clock captures (the worst slack of each endpoint)
        delay_t setup_wns, hold_wns, setup_tns = 0;
        int endpoints = 0, failing = 0;
        bool posedge = false, negedge = false;
        int clocked_cells = 0, users = 0;
        QString driver, driver_details;
        // routing of the clock pins
        int routed_sinks = 0, clock_sinks = 0;
        delay_t min_insertion = 0, max_insertion = 0;
        // clock-to-clock delays to related clocks
        QStringList related;
    };

    void analyse();
    void showClock(int index);
    QString formatDelay(delay_t delay) const;
    QString clockName(const ClockEvent &clock) const;

    Context *ctx;
    bool dirty;
    QString note;
    std::vector<Clock> clocks;
    std::vector<Crossing> crossings;

    QLabel *header;
    QTreeWidget *clockList;
    QLabel *summary;
    QTreeWidget *crossingList;
};

NEXTPNR_NAMESPACE_END

#endif // CLOCKSWIDGET_H
