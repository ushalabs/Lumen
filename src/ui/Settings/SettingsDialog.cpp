#include "ui/Settings/SettingsDialog.h"

#include "core/AppPaths.h"
#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "platform/windows/DesktopIntegration.h"
#include "ui/BrandingIcon.h"
#include "ui/Theme.h"

#include <QCheckBox>
#include <QDir>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QProxyStyle>
#include <QPushButton>
#include <QShowEvent>
#include <QStyleFactory>
#include <QStyleOptionButton>
#include <QVBoxLayout>

namespace lumen::ui {

namespace {
class LumenSettingsStyle : public QProxyStyle {
public:
    LumenSettingsStyle()
        : QProxyStyle(QStyleFactory::create(QStringLiteral("Fusion"))) {}

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption* option,
                       QPainter* painter,
                       const QWidget* widget = nullptr) const override {
        if (element == PE_IndicatorCheckBox) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);

            const auto theme = ThemeTokens::get(false);
            const QRectF r = QRectF(option->rect).adjusted(1.0, 1.0, -1.0, -1.0);
            const bool checked = (option->state & State_On) != 0;
            const bool hovered = (option->state & State_MouseOver) != 0;

            if (checked) {
                painter->setPen(QPen(theme.brandPink, 1.2));
                painter->setBrush(QColor(253, 242, 248));
                painter->drawRoundedRect(r, 4.0, 4.0);

                QPainterPath check;
                check.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.52);
                check.lineTo(r.left() + r.width() * 0.44, r.top() + r.height() * 0.72);
                check.lineTo(r.left() + r.width() * 0.76, r.top() + r.height() * 0.30);
                QPen checkPen(theme.brandPink, 1.9, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
                painter->setPen(checkPen);
                painter->setBrush(Qt::NoBrush);
                painter->drawPath(check);
            } else {
                painter->setPen(QPen(hovered ? theme.brandPink : theme.borderColor, 1.2));
                painter->setBrush(QColor(255, 255, 255));
                painter->drawRoundedRect(r, 4.0, 4.0);
            }

            painter->restore();
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }
};

QLabel* createSectionHeader(const QString& title, QWidget* parent) {
    auto* lbl = new QLabel(title, parent);
    lbl->setObjectName(QStringLiteral("sectionHeader"));
    lbl->setFont(ThemeTokens::makeFont(14, QFont::Medium, true));
    return lbl;
}

QFrame* createSectionDivider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("sectionDivider"));
    line->setFrameShape(QFrame::HLine);
    line->setFixedHeight(1);
    return line;
}

QHBoxLayout* createLabeledControlRow(const QString& labelText, QWidget* control, QWidget* parent) {
    auto* row = new QHBoxLayout();
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);
    auto* lbl = new QLabel(labelText, parent);
    lbl->setObjectName(QStringLiteral("rowLabel"));
    lbl->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    row->addWidget(lbl);
    row->addStretch();
    row->addWidget(control);
    return row;
}
} // namespace

SettingsDialog::SettingsDialog(QWidget* parent)
    : QDialog(parent, Qt::Dialog | Qt::WindowTitleHint | Qt::WindowCloseButtonHint | Qt::Tool) {
    setWindowTitle(QStringLiteral("Lumen Settings"));
    setWindowIcon(BrandingIcon::createAppIcon());
    setFixedWidth(520);
    setModal(false);

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(24, 22, 24, 20);
    mainLayout->setSpacing(10);

    // -------------------------------------------------------------------------
    // Section 1: General
    // -------------------------------------------------------------------------
    mainLayout->addWidget(createSectionHeader(QStringLiteral("General"), this));

    m_desktopBarVisibleCheck = new QCheckBox(QStringLiteral("Show Lumen icon on the Windows desktop"), this);
    m_desktopBarVisibleCheck->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addWidget(m_desktopBarVisibleCheck);

    m_alwaysOnTopCheck = new QCheckBox(QStringLiteral("Keep desktop icon above active windows"), this);
    m_alwaysOnTopCheck->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addWidget(m_alwaysOnTopCheck);

    m_startWithWindowsCheck = new QCheckBox(QStringLiteral("Launch Lumen automatically when Windows starts"), this);
    m_startWithWindowsCheck->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addWidget(m_startWithWindowsCheck);

    m_shortcutEdit = new QLineEdit(this);
    m_shortcutEdit->setPlaceholderText(QStringLiteral("Ctrl+Alt+Space"));
    m_shortcutEdit->setFixedWidth(180);
    m_shortcutEdit->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addLayout(createLabeledControlRow(QStringLiteral("Global activation shortcut"), m_shortcutEdit, this));

    auto* resetPosBtn = new QPushButton(QStringLiteral("Reset to Default Position"), this);
    resetPosBtn->setFixedWidth(180);
    resetPosBtn->setFont(ThemeTokens::makeFont(12, QFont::Medium));
    connect(resetPosBtn, &QPushButton::clicked, this, &SettingsDialog::onResetPositionClicked);
    mainLayout->addLayout(createLabeledControlRow(QStringLiteral("Desktop icon position"), resetPosBtn, this));

    mainLayout->addSpacing(4);
    mainLayout->addWidget(createSectionDivider(this));
    mainLayout->addSpacing(2);

    // -------------------------------------------------------------------------
    // Section 2: Search & Indexing
    // -------------------------------------------------------------------------
    mainLayout->addWidget(createSectionHeader(QStringLiteral("Search & Indexing"), this));

    m_enableContentCheck = new QCheckBox(
        QStringLiteral("Index document text (.txt, .md, .pdf, .docx)"), this);
    m_enableContentCheck->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addWidget(m_enableContentCheck);

    m_enableSemanticCheck = new QCheckBox(
        QStringLiteral("Enable offline semantic search (local MiniLM-L6-v2 embeddings)"), this);
    m_enableSemanticCheck->setFont(ThemeTokens::makeFont(13, QFont::Normal));
    mainLayout->addWidget(m_enableSemanticCheck);

    auto* scopeCaption = new QLabel(QStringLiteral("Indexed folders"), this);
    scopeCaption->setObjectName(QStringLiteral("subCaption"));
    scopeCaption->setFont(ThemeTokens::makeFont(12, QFont::Medium));
    mainLayout->addWidget(scopeCaption);

    m_watchedFoldersList = new QListWidget(this);
    m_watchedFoldersList->setFixedHeight(88);
    m_watchedFoldersList->setFont(ThemeTokens::makeFont(12, QFont::Normal));
    mainLayout->addWidget(m_watchedFoldersList);

    auto* folderBtnRow = new QHBoxLayout();
    folderBtnRow->setContentsMargins(0, 0, 0, 0);
    folderBtnRow->setSpacing(8);
    auto* addFolderBtn = new QPushButton(QStringLiteral("Add Folder..."), this);
    auto* removeFolderBtn = new QPushButton(QStringLiteral("Remove"), this);
    auto* rebuildBtn = new QPushButton(QStringLiteral("Rebuild Index"), this);
    addFolderBtn->setFont(ThemeTokens::makeFont(12, QFont::Medium));
    removeFolderBtn->setFont(ThemeTokens::makeFont(12, QFont::Medium));
    rebuildBtn->setFont(ThemeTokens::makeFont(12, QFont::Medium));

    connect(addFolderBtn, &QPushButton::clicked, this, &SettingsDialog::onAddFolderClicked);
    connect(removeFolderBtn, &QPushButton::clicked, this, &SettingsDialog::onRemoveFolderClicked);
    connect(rebuildBtn, &QPushButton::clicked, this, &SettingsDialog::onRebuildIndexClicked);

    folderBtnRow->addWidget(addFolderBtn);
    folderBtnRow->addWidget(removeFolderBtn);
    folderBtnRow->addStretch();
    folderBtnRow->addWidget(rebuildBtn);
    mainLayout->addLayout(folderBtnRow);

    mainLayout->addSpacing(4);
    mainLayout->addWidget(createSectionDivider(this));
    mainLayout->addSpacing(2);

    // -------------------------------------------------------------------------
    // Section 4: Storage & Privacy
    // -------------------------------------------------------------------------
    mainLayout->addWidget(createSectionHeader(QStringLiteral("Storage & Privacy"), this));

    m_dbStatusLabel = new QLabel(this);
    m_dbStatusLabel->setObjectName(QStringLiteral("metaLabel"));
    m_dbStatusLabel->setFont(ThemeTokens::makeFont(12, QFont::Normal));
    m_dbStatusLabel->setWordWrap(true);
    mainLayout->addWidget(m_dbStatusLabel);

    m_dbPathLabel = new QLabel(this);
    m_dbPathLabel->setObjectName(QStringLiteral("mutedLabel"));
    m_dbPathLabel->setFont(ThemeTokens::makeFont(11, QFont::Normal));
    m_dbPathLabel->setWordWrap(true);
    mainLayout->addWidget(m_dbPathLabel);

    mainLayout->addSpacing(6);
    mainLayout->addWidget(createSectionDivider(this));
    mainLayout->addSpacing(4);

    // -------------------------------------------------------------------------
    // Footer: Brand Signature & Save / Cancel Actions
    // -------------------------------------------------------------------------
    auto* btnRow = new QHBoxLayout();
    btnRow->setContentsMargins(0, 0, 0, 0);
    btnRow->setSpacing(8);

    m_signatureLabel = new QLabel(this);
    m_signatureLabel->setTextFormat(Qt::RichText);
    btnRow->addWidget(m_signatureLabel);
    btnRow->addStretch();

    auto* cancelBtn = new QPushButton(QStringLiteral("Cancel"), this);
    auto* saveBtn = new QPushButton(QStringLiteral("Save Changes"), this);
    cancelBtn->setFont(ThemeTokens::makeFont(12, QFont::Medium));
    saveBtn->setFont(ThemeTokens::makeFont(12, QFont::DemiBold));
    saveBtn->setDefault(true);

    connect(cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    connect(saveBtn, &QPushButton::clicked, this, &SettingsDialog::onSaveClicked);

    btnRow->addWidget(cancelBtn);
    btnRow->addWidget(saveBtn);
    mainLayout->addLayout(btnRow);

    loadFromSettings();
}

void SettingsDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    platform::windows::DesktopIntegration::applyDesktopWidgetStyles(reinterpret_cast<HWND>(winId()));
}

void SettingsDialog::applyThemeStyles() {
    const auto theme = ThemeTokens::get(false);

    for (auto* cb : {m_desktopBarVisibleCheck, m_alwaysOnTopCheck, m_startWithWindowsCheck, m_enableContentCheck, m_enableSemanticCheck}) {
        if (cb != nullptr) {
            cb->setStyle(new LumenSettingsStyle());
        }
    }

    m_signatureLabel->setText(
        QStringLiteral("<span style='font-size:13px;font-weight:500;color:%1'>Usha</span>"
                       "<span style='font-size:13px;font-weight:500;color:%2'>Labs</span>"
                       "<span style='font-size:11px;color:%3'>  ·  100% Offline</span>")
            .arg(theme.textPrimary.name(), theme.brandPink.name(), theme.textSecondary.name()));

    setStyleSheet(QStringLiteral(
        "QDialog { background-color: %1; color: %2; }"
        "QLabel#sectionHeader { color: %2; padding-top: 2px; padding-bottom: 2px; }"
        "QLabel#rowLabel { color: %2; }"
        "QLabel#subCaption { color: %3; padding-top: 2px; }"
        "QLabel#metaLabel { color: %3; }"
        "QLabel#mutedLabel { color: %4; }"
        "QFrame#sectionDivider { background-color: %5; border: none; }"
        "QPushButton {"
        "  background-color: %6;"
        "  border: 1px solid %7;"
        "  border-radius: 6px;"
        "  padding: 5px 12px;"
        "  color: %2;"
        "  min-height: 18px;"
        "}"
        "QPushButton:hover { background-color: %8; border-color: %9; color: %2; }"
        "QPushButton:pressed { background-color: %10; }"
        "QPushButton:default { background-color: %10; color: %2; border: 1px solid %9; font-weight: 500; }"
        "QPushButton:default:hover { background-color: %8; border-color: %9; color: %2; }"
        "QLineEdit {"
        "  background-color: %1;"
        "  border: 1px solid %7;"
        "  border-radius: 6px;"
        "  padding: 4px 8px;"
        "  color: %2;"
        "  selection-background-color: %10;"
        "  selection-color: %2;"
        "}"
        "QLineEdit:focus { border-color: %9; }"
        "QCheckBox { spacing: 8px; color: %2; padding: 2px 0px; }"
        "QListWidget { background-color: %6; border: 1px solid %7; border-radius: 6px; padding: 4px; color: %2; outline: none; }"
        "QListWidget::item { padding: 3px 6px; border-radius: 4px; }"
        "QListWidget::item:selected { background-color: %10; color: %2; }")
        .arg(QStringLiteral("#FFFFFF"),            // %1 Pure White dialog surface
             theme.textPrimary.name(),            // %2 Dark text
             theme.textSecondary.name(),          // %3 Muted gray (#6E6E6E)
             theme.textMuted.name(),              // %4 Subtle gray
             QStringLiteral("#EFEFEF"),           // %5 Neutral hairline divider
             QStringLiteral("#F8F8FA"),           // %6 Calm neutral container/button fill
             QStringLiteral("#E6E6EB"),           // %7 Subtle neutral border
             QStringLiteral("#FDF2F8"),           // %8 Soft pastel pink hover
             theme.brandPink.name(),              // %9 Active focus/brand pink
             QStringLiteral("#FCE7F3")));         // %10 Active pastel pink primary button fill
}

void SettingsDialog::loadFromSettings() {
    auto& settings = core::SettingsManager::instance();
    m_desktopBarVisibleCheck->setChecked(settings.desktopBarVisible());
    m_alwaysOnTopCheck->setChecked(settings.alwaysOnTopMode());
    m_startWithWindowsCheck->setChecked(settings.startWithWindows());
    m_shortcutEdit->setText(settings.globalShortcut());

    const auto idxSettings = settings.indexingSettings();
    m_enableContentCheck->setChecked(idxSettings.enableContentIndexing);
    m_enableSemanticCheck->setChecked(idxSettings.enableSemanticIndexing);

    m_watchedFoldersList->clear();
    for (const QString& folder : idxSettings.watchedFolders) {
        m_watchedFoldersList->addItem(folder);
    }

    refreshDbStatusLabel();
    applyThemeStyles();
}

void SettingsDialog::refreshDbStatusLabel() {
    auto& db = database::DatabaseManager::instance();
    m_dbStatusLabel->setText(
        QStringLiteral("%1 indexed files   ·   %2 content documents")
            .arg(db.fileCount())
            .arg(db.documentContentCount()));
    m_dbPathLabel->setText(
        QStringLiteral("Index location: %1")
            .arg(QDir::toNativeSeparators(core::AppPaths::databaseFilePath())));
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
    settings.setAppearancePreference(QStringLiteral("light"), false);

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
