// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QDialog>
class EmuInstance;
class QCheckBox;
class QComboBox;
class QLineEdit;
class QLabel;

class BaristaSettingsDialog : public QDialog
{
public:
    explicit BaristaSettingsDialog(EmuInstance* instance, QWidget* parent);
private:
    void Apply();
    void Refresh();
    EmuInstance* instance;
    QCheckBox* enabled;
    QCheckBox* input;
    QComboBox* screen;
    QLineEdit* socketPath;
    QLabel* status;
};
