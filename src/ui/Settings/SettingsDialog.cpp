#include "ui/Settings/SettingsDialog.h"

#include "core/AppPaths.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "ui/BrandingIcon.h"

#include <QDir>
#include <QFileDialog>
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
    setMinimumWidth(470);
    setModal(true);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(18, 18, 18, 18);
    mainLayout->setSpacing(12);

    // 1. Desktop Search Bar group
    auto* desktopGroup = new QGroupBox(QStringLiteral("Desktop Search Bar"), this);
    auto* desktopLayout = new QFormLayout(desktopGroup);
    desktopLayout->setSpacing(8);

    m_desktopBarVisibleCheck = new QCheckBox(QStringLiteral("Show search bar on Windows desktop"), desktopGroup);
    desktopLayout->addRow(QString(), m_desktopBarVisibleCheck);

    m_alwaysOnTopCheck = new QCheckBox(QStringLiteral("Floating Always-on-Top mode (stay above maximized windows)"), desktopGroup);
    desktopLayout->addRow(QString(), m_alwaysOnTopCheck);

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

    // 2. Indexing Scope & Offline AI group
    auto* indexGroup = new QGroupBox(QStringLiteral("Indexing Scope && Offline Intelligence"), this);
    auto* indexLayout = new QVBoxLayout(indexGroup);
    indexLayout->setSpacing(8);

    m_enableContentCheck = new QCheckBox(
        QStringLiteral("Index document contents (.txt, .md, .pdf, .docx)"), indexGroup);
    indexLayout->addWidget(m_enableContentCheck);

    m_enableSemanticCheck = new QCheckBox(
        QStringLiteral("Enable offline semantic AI embeddings (ONNX CPU MiniLM-L6-v2)"), indexGroup);
    indexLayout->addWidget(m_enableSemanticCheck);

    auto* scopeCaption = new QLabel(
        QStringLiteral("Monitored user folders:"),
        indexGroup);
    indexLayout->addWidget(scopeCaption);

    m_watchedFoldersList = new QListWidget(indexGroup);
    m_watchedFoldersList->setMaximumHeight(82);
    indexLayout->addWidget(m_watchedFoldersList);

    auto* folderBtnRow = new QHBoxLayout();
    auto* addFolderBtn = new QPushButton(QStringLiteral("Add Folder..."), indexGroup);
    auto* removeFolderBtn = new QPushButton(QStringLiteral("Remove Selected"), indexGroup);
    auto* rebuildBtn = new QPushButton(QStringLiteral("Rebuild Index"), indexGroup);

    connect(addFolderBtn, &QPushButton::clicked, this, &SettingsDialog::onAddFolderClicked);
    connect(removeFolderBtn, &QPushButton::clicked, this, &SettingsDialog::onRemoveFolderClicked);
    connect(rebuildBtn, &QPushButton::clicked, this, &SettingsDialog::onRebuildIndexClicked);

    folderBtnRow->addWidget(addFolderBtn);
    folderBtnRow->addWidget(removeFolderBtn);
    folderBtnRow->addStretch();
    folderBtnRow->addWidget(rebuildBtn);
    indexLayout->addLayout(folderBtnRow);

    m_dbStatusLabel = new QLabel(indexGroup);
    m_dbStatusLabel->setStyleSheet(QStringLiteral("color: #475569; font-size: 8.5pt;"));
    m_dbStatusLabel->setWordWrap(true);
    indexLayout->addWidget(m_dbStatusLabel);

    mainLayout->addWidget(indexGroup);

    // 3. Dialog buttons
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
    m_alwaysOnTopCheck->setChecked(settings.alwaysOnTopMode());
    m_startWithWindowsCheck->setChecked(settings.startWithWindows());

    const QString pref = settings.appearancePreference().toLower();
    const int idx = m_appearanceCombo->findData(pref);
    m_appearanceCombo->setCurrentIndex(idx >= 0 ? idx : 0);

    m_shortcutEdit->setText(settings.globalShortcut());

    const auto idxSettings = settings.indexingSettings();
    m_enableContentCheck->setChecked(idxSettings.enableContentIndexing);
    m_enableSemanticCheck->setChecked(idxSettings.enableSemanticIndexing);

    m_watchedFoldersList->clear();
    for (const QString& folder : idxSettings.watchedFolders) {
        m_watchedFoldersList->addItem(folder);
    }

    refreshDbStatusLabel();
}

void SettingsDialog::refreshDbStatusLabel() {
    auto& db = database::DatabaseManager::instance();
    m_dbStatusLabel->setText(
        QStringLiteral("Indexed Files: %1  •  Content Docs: %2  •  Semantic Chunks: %3\nDB: %4 (Schema v%5, WAL: %6)")
            .arg(db.fileCount())
            .arg(db.documentContentCount())
            .arg(db.documentChunkCount())
            .arg(core::AppPaths::databaseFilePath())
            .arg(db.schemaVersion())
            .arg(db.journalMode()));
}

void SettingsDialog::onResetPositionClicked() {
    core::SettingsManager::instance().resetDesktopBarPosition(true);
    emit resetPositionRequested();
}

void SettingsDialog::onAddFolderClicked() {
    const QString dir = QFileDialog::getExistingDirectory(
        this,
        QStringLiteral("Select Folder to Index"),
        QDir::homePath(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
    if (!dir.isEmpty()) {
        const QString nativeDir = QDir::toNativeSeparators(dir);
        for (int i = 0; i < m_watchedFoldersList->count(); ++i) {
            if (m_watchedFoldersList->item(i)->text().compare(nativeDir, Qt::CaseInsensitive) == 0) {
                return;
            }
        }
        m_watchedFoldersList->addItem(nativeDir);
    }
}

void SettingsDialog::onRemoveFolderClicked() {
    const int row = m_watchedFoldersList->currentRow();
    if (row >= 0 && m_watchedFoldersList->count() > 1) {
        delete m_watchedFoldersList->takeItem(row);
    }
}

void SettingsDialog::onRebuildIndexClicked() {
    emit rebuildIndexRequested();
    refreshDbStatusLabel();
}

void SettingsDialog::onSaveClicked() {
    auto& settings = core::SettingsManager::instance();
    settings.setDesktopBarVisible(m_desktopBarVisibleCheck->isChecked(), false);
    settings.setAlwaysOnTopMode(m_alwaysOnTopCheck->isChecked(), false);
    settings.setStartWithWindows(m_startWithWindowsCheck->isChecked(), QString(), true);
    settings.setAppearancePreference(m_appearanceCombo->currentData().toString(), false);

    const QString shortcut = m_shortcutEdit->text().trimmed();
    if (!shortcut.isEmpty()) {
        settings.setGlobalShortcut(shortcut, false);
    }

    auto idx = settings.indexingSettings();
    idx.enableContentIndexing = m_enableContentCheck->isChecked();
    idx.enableSemanticIndexing = m_enableSemanticCheck->isChecked();
    QStringList folders;
    for (int i = 0; i < m_watchedFoldersList->count(); ++i) {
        const QString f = m_watchedFoldersList->item(i)->text().trimmed();
        if (!f.isEmpty()) {
            folders.append(f);
        }
    }
    if (!folders.isEmpty()) {
        idx.watchedFolders = folders;
    }
    settings.setIndexingSettings(idx, false);
    settings.save();

    emit settingsApplied();
    accept();
}

} // namespace lumen::ui
