#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>

namespace lumen::ui {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override = default;

signals:
    void settingsApplied();
    void resetPositionRequested();

private slots:
    void onSaveClicked();
    void onResetPositionClicked();

private:
    void loadFromSettings();

    QCheckBox* m_desktopBarVisibleCheck = nullptr;
    QCheckBox* m_startWithWindowsCheck = nullptr;
    QComboBox* m_appearanceCombo = nullptr;
    QLineEdit* m_shortcutEdit = nullptr;
    QListWidget* m_watchedFoldersList = nullptr;
    QLabel* m_dbStatusLabel = nullptr;
};

} // namespace lumen::ui
