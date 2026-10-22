#include "ui/Settings/SettingsDialog.h"

#include "core/AppPaths.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "ui/BrandingIcon.h"

#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QPushButton>
#include <QVBoxLayout>

namespace lumen::ui {

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(QStringLiteral("Lumen Settings — UshaLabs"));
    setWindowIcon(BrandingIcon::createAppIcon());
    setMinimumWidth(440);
    setModal(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(18, 18, 18, 18);
    mainLayout->setSpacing(14);

    // Desktop Search Bar group
    auto* desktopGroup = new QGroupBox(QStringLiteral("Desktop Search Bar"), this);
    auto* desktopLayout = new QFormLayout(desktopGroup);
    desktopLayout->setSpacing(10);

    m_desktopBarVisibleCheck = new QCheckBox(QStringLiteral("Show search bar on Windows desktop"), desktopGroup);
    desktopLayout->addRow(QString(), m_desktopBarVisibleCheck);

    m_startWithWindowsCheck = new QCheckBox(QStringLiteral("Start Lumen automatically when Windows starts"), desktopGroup);
    desktopLayout->addRow(QString(), m_startWithWindowsCheck);

    m_appearanceCombo = new QComboBox(desktopGroup);
    m_appearanceCombo->addItem(QStringLiteral("Light (Default)"), QStringLiteral("light"));
    m_appearanceCombo->addItem(QStringLiteral("Dark"), QStringLiteral("dark"));
    m_appearanceCombo->addItem(QStringLiteral("Follow Windows System"), QStringLiteral("system"));
    desktopLayout->addRow(QStringLiteral("Appearance:"), m_appearanceCombo);

    m_shortcutEdit = new QLineEdit(desktopGroup);
    m_shortcutEdit->setPlaceholderText(QStringLiteral("Ctrl+Alt+Space"));
    desktopLayout->addRow(QStringLiteral("Global Shortcut:"), m_shortcutEdit);

    auto* resetPosBtn = new QPushButton(QStringLiteral("Reset Desktop Bar Position"), desktopGroup);
    connect(resetPosBtn, &QPushButton::clicked, this, &SettingsDialog::onResetPositionClicked);
    desktopLayout->addRow(QStringLiteral("Placement:"), resetPosBtn);

    mainLayout->addWidget(desktopGroup);

    // Indexing Scope group (Phase 1 & 2)
    auto* indexGroup = new QGroupBox(QStringLiteral("Indexing Scope (Filename + Document Content)"), this);
    auto* indexLayout = new QVBoxLayout(indexGroup);
    indexLayout->setSpacing(8);

    auto* scopeCaption = new QLabel(
        QStringLiteral("Approved user folders monitored for offline filename and document content (.txt, .md, .pdf, .docx) indexing:"),
        indexGroup);
    scopeCaption->setWordWrap(true);
    indexLayout->addWidget(scopeCaption);

    m_watchedFoldersList = new QListWidget(indexGroup);
    m_watchedFoldersList->setMaximumHeight(90);
    indexLayout->addWidget(m_watchedFoldersList);

    m_dbStatusLabel = new QLabel(indexGroup);
    m_dbStatusLabel->setStyleSheet(QStringLiteral("color: #475569; font-size: 9pt;"));
    m_dbStatusLabel->setWordWrap(true);
    indexLayout->addWidget(m_dbStatusLabel);

    mainLayout->addWidget(indexGroup);

    // Dialog buttons
    auto* btnRow = new QHBoxLayout();
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    auto* saveBtn = new QPushButton(QStringLiteral("Save Changes"), this);
    saveBtn->setDefault(true);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSaveClicked);

    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    mainLayout->addLayout(btnRow);

    loadFromSettings();
}

void SettingsDialog::loadFromSettings() {
    auto& settings = core::SettingsManager::instance();
    m_desktopBarVisibleCheck->setChecked(settings.desktopBarVisible());
    m_startWithWindowsCheck->setChecked(settings.startWithWindows());

    const QString pref = settings.appearancePreference().toLower();
    const int idx = m_appearanceCombo->findData(pref);
    m_appearanceCombo->setCurrentIndex(idx >= 0 ? idx : 0);

    m_shortcutEdit->setText(settings.globalShortcut());

    m_watchedFoldersList->clear();
    for (const QString& folder : settings.indexingSettings().watchedFolders) {
        m_watchedFoldersList->addItem(folder);
    }

    auto& db = database::DatabaseManager::instance();
    m_dbStatusLabel->setText(
        QStringLiteral("Indexed Files: %1  •  Content Documents: %2  •  DB: %3 (Schema v%4, WAL: %5)")
            .arg(db.fileCount())
            .arg(db.documentContentCount())
            .arg(core::AppPaths::databaseFilePath())
            .arg(db.schemaVersion())
            .arg(db.journalMode()));
}

void SettingsDialog::onResetPositionClicked() {
    core::SettingsManager::instance().resetDesktopBarPosition(true);
    emit resetPositionRequested();
}

void SettingsDialog::onSaveClicked() {
    auto& settings = core::SettingsManager::instance();
    settings.setDesktopBarVisible(m_desktopBarVisibleCheck->isChecked(), false);
    settings.setStartWithWindows(m_startWithWindowsCheck->isChecked(), QString(), true);
    settings.setAppearancePreference(m_appearanceCombo->currentData().toString(), false);

    const QString shortcut = m_shortcutEdit->text().trimmed();
    if (!shortcut.isEmpty()) {
        settings.setGlobalShortcut(shortcut, false);
    }
    settings.save();

    emit settingsApplied();
    accept();
}

} // namespace lumen::ui
