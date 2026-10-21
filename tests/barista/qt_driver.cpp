// SPDX-License-Identifier: GPL-3.0-or-later
// Test-only Qt event driver, preloaded into the real application on Linux.
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <dlfcn.h>
#include <cstdio>

int QApplication::exec()
{
    QString root = qEnvironmentVariable("MELONDS_TEST_DIR");
    auto* timer = new QTimer(qApp);
    QObject::connect(timer, &QTimer::timeout, qApp, [root]
    {
        QFile file(root+"/command.json");
        if (!file.open(QIODevice::ReadOnly)) return;
        auto command = QJsonDocument::fromJson(file.readAll()).object();
        file.close(); file.remove();
        QString action = command["action"].toString();
        QString error;
        if (action == "settings")
        {
            QDialog* dialog = nullptr;
            for (QWidget* window : QApplication::topLevelWidgets())
                if (window->objectName() == "BaristaSettingsDialog") dialog = qobject_cast<QDialog*>(window);
            if (!dialog)
            {
                bool found = false;
                for (QWidget* window : QApplication::topLevelWidgets())
                    for (QAction* candidate : window->findChildren<QAction*>())
                        if (!found && candidate->text() == "Barista settings") { candidate->trigger(); found = true; }
                for (QWidget* window : QApplication::topLevelWidgets())
                    if (window->objectName() == "BaristaSettingsDialog") dialog = qobject_cast<QDialog*>(window);
            }
            if (!dialog) error = "Settings dialog missing";
            else
            {
                auto boxes = dialog->findChildren<QCheckBox*>();
                for (auto* box : boxes)
                {
                    if (box->text().startsWith("Enable") && command.contains("enabled")) box->setChecked(command["enabled"].toBool());
                    if (box->text().startsWith("Use GamePad") && command.contains("input")) box->setChecked(command["input"].toBool());
                }
                if (command.contains("mode")) dialog->findChild<QComboBox*>()->setCurrentIndex(command["mode"].toInt());
                if (command.contains("apply")) dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Apply)->click();
                if (command.contains("screenshot"))
                {
                    QString path = command["screenshot"].toString();
                    if (!dialog->grab().save(path)) error = "Cannot save settings screenshot";
                }
                if (command["close"].toBool()) dialog->reject();
            }
        }
        else if (action == "pause" || action == "stop")
        {
            bool found = false;
            for (QWidget* window : QApplication::topLevelWidgets())
                for (QAction* candidate : window->findChildren<QAction*>())
                    if (!found && candidate->text() == (action == "pause" ? "Pause" : "Stop")) { candidate->trigger(); found = true; }
            if (!found) error = "System action missing";
        }
        else if (action == "quit") QApplication::closeAllWindows();
        QFile result(root+"/result.json");
        if (!result.open(QIODevice::WriteOnly)) qFatal("Cannot write UI test result");
        result.write(QJsonDocument(QJsonObject{{"error",error}}).toJson());
    });
    timer->start(25);
    return reinterpret_cast<int(*)()>(dlsym(RTLD_NEXT,"_ZN12QApplication4execEv"))();
}
