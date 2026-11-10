#pragma once

#include <QCheckBox>
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
    void rebuildIndexRequested();

protected:
    void showEvent(QShowEvent* event) override;

private slots:
    void onSaveClicked();
    void onResetPositionClicked();
    void onAddFolderClicked();
    void onRemoveFolderClicked();
    void onRebuildIndexClicked();

private:
    void loadFromSettings();
    void refreshDbStatusLabel();
    void applyThemeStyles();

    QCheckBox* m_desktopBarVisibleCheck = nullptr;
    QCheckBox* m_alwaysOnTopCheck = nullptr;
    QCheckBox* m_startWithWindowsCheck = nullptr;
    QLineEdit* m_shortcutEdit = nullptr;

    QCheckBox* m_enableContentCheck = nullptr;
    QCheckBox* m_enableSemanticCheck = nullptr;
    QListWidget* m_watchedFoldersList = nullptr;
    QLabel* m_dbStatusLabel = nullptr;
    QLabel* m_dbPathLabel = nullptr;
    QLabel* m_signatureLabel = nullptr;
};

} // namespace lumen::ui
