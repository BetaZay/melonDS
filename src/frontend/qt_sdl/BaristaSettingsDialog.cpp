// SPDX-License-Identifier: GPL-3.0-or-later
#include "BaristaSettingsDialog.h"
#include "EmuInstance.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

BaristaSettingsDialog::BaristaSettingsDialog(EmuInstance* instance, QWidget* parent)
    : QDialog(parent), instance(instance)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("BaristaSettingsDialog");
    setWindowTitle(tr("Barista settings"));
    auto* layout = new QVBoxLayout(this);
    auto* description = new QLabel(tr("Play on a Wii U GamePad through Barista. Start a GamePad session in Barista, then enable the connection here."));
    description->setWordWrap(true);
    layout->addWidget(description);
    enabled = new QCheckBox(tr("Enable Barista connection"));
    input = new QCheckBox(tr("Use GamePad buttons, left stick and touchscreen"));
    screen = new QComboBox;
    screen->addItems({tr("Bottom screen"), tr("Top screen"), tr("Side by side"), tr("Top and bottom")});
    socketPath = new QLineEdit;
    socketPath->setPlaceholderText(QString::fromStdString(Barista::Connector::DefaultSocketPath()));
    socketPath->setClearButtonEnabled(true);
    socketPath->setToolTip(tr("Leave blank to use the default Barista connection. Changes take effect when you choose Apply."));
    auto& config = instance->getGlobalConfig();
    enabled->setChecked(config.GetBool("Barista.Enabled"));
    input->setChecked(config.GetBool("Barista.Input"));
    screen->setCurrentIndex(config.GetInt("Barista.Screen"));
    socketPath->setText(config.GetQString("Barista.SocketPath"));
    layout->addWidget(enabled);
    layout->addWidget(input);
    auto* form = new QFormLayout;
    form->addRow(tr("GamePad display:"), screen);
    form->addRow(tr("Socket path:"), socketPath);
    layout->addLayout(form);
    auto* touchHint = new QLabel(tr("Touch follows the bottom screen. Black borders and the top screen do not accept touch."));
    touchHint->setWordWrap(true);
    layout->addWidget(touchHint);
    status = new QLabel;
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    // Reserve enough room for wrapped paths, counters and stale-input status.
    status->setMinimumHeight(status->fontMetrics().lineSpacing() * 7);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto* reconnect = new QPushButton(tr("Apply and reconnect"));
    layout->addWidget(reconnect, 0, Qt::AlignLeft);
    connect(reconnect, &QPushButton::clicked, this, [this] { Apply(); });
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    layout->addWidget(buttons);
    connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this] { Apply(); });
    connect(buttons, &QDialogButtonBox::accepted, this, [this] { Apply(); accept(); });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    auto* timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, [this] { Refresh(); });
    timer->start(500);
#if !defined(__linux__)
    enabled->setEnabled(false);
    reconnect->setEnabled(false);
    description->setText(tr("Barista AppHook is available on Linux."));
#endif
    resize(520, sizeHint().height());
    Refresh();
}

void BaristaSettingsDialog::Apply()
{
    auto& config = instance->getGlobalConfig();
    config.SetBool("Barista.Enabled", enabled->isChecked());
    config.SetBool("Barista.Input", input->isChecked());
    config.SetInt("Barista.Screen", screen->currentIndex());
    config.SetQString("Barista.SocketPath", socketPath->text().trimmed());
    Config::Save();
    instance->baristaConfigure();
    Refresh();
}

void BaristaSettingsDialog::Refresh()
{
    const auto state = instance->barista.GetStatus();
    if (!state.enabled)
    {
        status->setText(tr("Connection disabled."));
        return;
    }
    QString connection = state.connected ? tr("Connected") : tr("Waiting for Barista");
    QString detail = state.connected ? (state.active ? tr("Streaming") : tr("Idle")) : (state.error.empty() ? tr("Connecting…") : QString::fromStdString(state.error));
    status->setText(tr("%1 — %2\n%3\nFrames: %4 | Audio chunks: %5 | Input reports: %6\nLast input: %7")
        .arg(connection, detail, QString::fromStdString(state.endpoint))
        .arg(qulonglong(state.frames)).arg(qulonglong(state.audioChunks)).arg(qulonglong(state.inputReports))
        .arg(state.inputAgeMs < 0 ? tr("waiting") : tr("%1 ms ago").arg(qlonglong(state.inputAgeMs))));
}
