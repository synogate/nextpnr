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

#ifndef PNROPTIONS_H
#define PNROPTIONS_H

#include <QDialog>
#include <QStringList>
#include <QVariant>
#include <functional>
#include <vector>
#include "nextpnr.h"

NEXTPNR_NAMESPACE_BEGIN

// The steps of the flow an option belongs to; it can be changed until its step has run
enum class PnrStep
{
    General,
    Pack,
    Place,
    Route,
    Repair,
    Finish
};

// An option of the place and route flow, as the options dialog shows it
struct PnrOption
{
    enum Kind
    {
        Flag,
        Int,
        Float,
        Choice,
        OpenFile,
        SaveFile
    };
    PnrStep step;
    QString group, label, help;
    Kind kind;
    QVariant def;
    // Int, Float: the range; at min, special (if not empty) is shown instead of the number
    double min = 0, max = 0, singleStep = 1;
    int decimals = 2;
    QString special;
    QStringList choices; // Choice: the values
    std::function<QVariant()> get;
    std::function<void(const QVariant &)> set;
};

class PnrOptionsDialog : public QDialog
{
    Q_OBJECT

  public:
    // Options of the steps before `open` have run and cannot change any more
    PnrOptionsDialog(std::vector<PnrOption> options, PnrStep open, QWidget *parent = nullptr);
    // Sets the changed options; returns their labels and new values for the log
    std::vector<std::string> apply();

  private:
    struct Field
    {
        PnrOption option;
        QWidget *widget;
        QVariant initial;
    };
    QVariant value(const Field &field) const;
    void setValue(Field &field, const QVariant &value);
    std::vector<Field> fields;
};

NEXTPNR_NAMESPACE_END

#endif // PNROPTIONS_H
