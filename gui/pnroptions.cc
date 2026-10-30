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

#include "pnroptions.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <iterator>

NEXTPNR_NAMESPACE_BEGIN

namespace {

const char *step_names[] = {"General", "Pack", "Place", "Route", "Repair", "Finish"};

QWidget *make_widget(const PnrOption &option, QWidget *parent)
{
    switch (option.kind) {
    case PnrOption::Flag:
        return new QCheckBox(parent);
    case PnrOption::Int: {
        auto *spin = new QSpinBox(parent);
        spin->setRange(int(option.min), int(option.max));
        spin->setSingleStep(int(option.singleStep));
        spin->setSpecialValueText(option.special);
        return spin;
    }
    case PnrOption::Float: {
        auto *spin = new QDoubleSpinBox(parent);
        spin->setDecimals(option.decimals);
        spin->setRange(option.min, option.max);
        spin->setSingleStep(option.singleStep);
        spin->setSpecialValueText(option.special);
        return spin;
    }
    case PnrOption::Choice: {
        auto *combo = new QComboBox(parent);
        combo->addItems(option.choices);
        return combo;
    }
    case PnrOption::OpenFile:
    case PnrOption::SaveFile: {
        auto *box = new QWidget(parent);
        auto *layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *edit = new QLineEdit(box);
        edit->setPlaceholderText("none");
        auto *browse = new QToolButton(box);
        browse->setText("...");
        bool save = option.kind == PnrOption::SaveFile;
        QObject::connect(browse, &QToolButton::clicked, box, [box, edit, save, label = option.label] {
            QString file = save ? QFileDialog::getSaveFileName(box, label, edit->text())
                                : QFileDialog::getOpenFileName(box, label, edit->text());
            if (!file.isEmpty())
                edit->setText(file);
        });
        layout->addWidget(edit, 1);
        layout->addWidget(browse);
        return box;
    }
    }
    return nullptr;
}

} // namespace

PnrOptionsDialog::PnrOptionsDialog(std::vector<PnrOption> options, PnrStep open, QWidget *parent) : QDialog(parent)
{
    setWindowTitle("Place and route options");
    resize(640, 700);

    auto *tabs = new QTabWidget(this);
    // Per step: the tab's page and its groups, in the order they first come
    std::vector<QWidget *> pages(std::size(step_names), nullptr);
    std::vector<std::vector<std::pair<QString, QGridLayout *>>> groups(std::size(step_names));

    for (auto &option : options) {
        int step = int(option.step);
        bool editable = option.step == PnrStep::General || option.step >= open;
        if (!pages[step]) {
            pages[step] = new QWidget();
            auto *layout = new QVBoxLayout(pages[step]);
            if (!editable)
                layout->addWidget(new QLabel("This step has run; its options cannot change any more."));
        }
        auto &step_groups = groups[step];
        auto found = std::find_if(step_groups.begin(), step_groups.end(),
                                  [&](const auto &group) { return group.first == option.group; });
        if (found == step_groups.end()) {
            auto *box = new QGroupBox(option.group, pages[step]);
            auto *grid = new QGridLayout(box);
            grid->setColumnStretch(1, 1);
            grid->setVerticalSpacing(2);
            pages[step]->layout()->addWidget(box);
            step_groups.emplace_back(option.group, grid);
            found = std::prev(step_groups.end());
        }
        QGridLayout *grid = found->second;
        int row = grid->rowCount();
        QWidget *widget = make_widget(option, grid->parentWidget());
        widget->setEnabled(editable);
        auto *label = new QLabel(option.label, grid->parentWidget());
        label->setEnabled(editable);
        auto *help = new QLabel(option.help, grid->parentWidget());
        help->setWordWrap(true);
        help->setEnabled(editable);
        help->setStyleSheet("color: gray; font-size: small;");
        grid->addWidget(label, row, 0);
        bool wide = option.kind == PnrOption::OpenFile || option.kind == PnrOption::SaveFile;
        grid->addWidget(widget, row, 1, wide ? Qt::Alignment() : Qt::AlignLeft);
        // Some room below each option's help before the next one
        help->setContentsMargins(0, 0, 0, 6);
        grid->addWidget(help, row + 1, 1);

        Field field{option, widget, QVariant()};
        setValue(field, option.get());
        field.initial = value(field);
        if (!editable)
            field.option.set = nullptr;
        fields.push_back(field);
    }

    // The same label column in all groups
    int label_width = 0;
    for (auto &field : fields)
        if (auto *grid = qobject_cast<QGridLayout *>(field.widget->parentWidget()->layout())) {
            int row, column, rows, columns;
            grid->getItemPosition(grid->indexOf(field.widget), &row, &column, &rows, &columns);
            label_width = std::max(label_width, grid->itemAtPosition(row, 0)->sizeHint().width());
        }
    for (auto &step_groups : groups)
        for (auto &group : step_groups)
            group.second->setColumnMinimumWidth(0, label_width + 12);

    for (int step = 0; step < int(std::size(step_names)); step++) {
        if (!pages[step])
            continue;
        static_cast<QVBoxLayout *>(pages[step]->layout())->addStretch(1);
        auto *scroll = new QScrollArea();
        scroll->setWidget(pages[step]);
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        tabs->addTab(scroll, step_names[step]);
        // Open on the next step to run
        if (step == int(open))
            tabs->setCurrentIndex(tabs->count() - 1);
    }

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                                                 QDialogButtonBox::RestoreDefaults,
                                         this);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, [this] {
        for (auto &field : fields)
            if (field.option.set)
                setValue(field, field.option.def);
    });

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);
}

QVariant PnrOptionsDialog::value(const Field &field) const
{
    switch (field.option.kind) {
    case PnrOption::Flag:
        return static_cast<QCheckBox *>(field.widget)->isChecked();
    case PnrOption::Int:
        return static_cast<QSpinBox *>(field.widget)->value();
    case PnrOption::Float:
        return static_cast<QDoubleSpinBox *>(field.widget)->value();
    case PnrOption::Choice:
        return static_cast<QComboBox *>(field.widget)->currentText();
    case PnrOption::OpenFile:
    case PnrOption::SaveFile:
        return field.widget->findChild<QLineEdit *>()->text().trimmed();
    }
    return QVariant();
}

void PnrOptionsDialog::setValue(Field &field, const QVariant &value)
{
    switch (field.option.kind) {
    case PnrOption::Flag:
        static_cast<QCheckBox *>(field.widget)->setChecked(value.toBool());
        break;
    case PnrOption::Int:
        static_cast<QSpinBox *>(field.widget)->setValue(value.toInt());
        break;
    case PnrOption::Float:
        static_cast<QDoubleSpinBox *>(field.widget)->setValue(value.toDouble());
        break;
    case PnrOption::Choice:
        static_cast<QComboBox *>(field.widget)->setCurrentText(value.toString());
        break;
    case PnrOption::OpenFile:
    case PnrOption::SaveFile:
        field.widget->findChild<QLineEdit *>()->setText(value.toString());
        break;
    }
}

std::vector<std::string> PnrOptionsDialog::apply()
{
    std::vector<std::string> changed;
    for (auto &field : fields) {
        QVariant now = value(field);
        if (!field.option.set || now == field.initial)
            continue;
        field.option.set(now);
        field.initial = now;
        QString text = field.option.kind == PnrOption::Flag ? (now.toBool() ? "on" : "off") : now.toString();
        if (text.isEmpty())
            text = "none";
        changed.push_back((field.option.label + ": " + text).toStdString());
    }
    return changed;
}

NEXTPNR_NAMESPACE_END
