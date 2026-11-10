#include "ui/DesktopSearchBar/DesktopSearchBar.h"

#include "core/Logger.h"
#include "core/SettingsManager.h"
#include "platform/windows/DesktopIntegration.h"
#include "platform/windows/SingleInstanceGuard.h"
#include "semantic/SemanticEmbeddingEngine.h"
#include "ui/BrandingIcon.h"
#include "ui/SearchResults/SearchPopupWidget.h"
#include "ui/Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QColor>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPointer>
#include <QRunnable>
#include <QScreen>
#include <QShowEvent>
#include <QStyleHints>
#include <QTimer>
#include <QUrl>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>

namespace lumen::ui {

DesktopSearchBar::DesktopSearchBar(QWidget* parent)
    : QWidget(parent, Qt::FramelessWindowHint | Qt::Tool) {
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setAttribute(Qt::WA_QuitOnClose, false);
    setFocusPolicy(Qt::ClickFocus);
    setFixedSize(kBarWidth, kBarHeight);
    setWindowTitle(QStringLiteral("Lumen Desktop Search"));

    m_searchPool.setMaxThreadCount(1);
    m_searchPool.setExpiryTimeout(5000); // Reclaim search worker thread after 5s of inactivity

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search files, documents, and more..."));
    m_searchEdit->setFrame(false);
    m_searchEdit->setFont(ThemeTokens::makeFont(15, QFont::Medium));
    m_searchEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_searchEdit->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_searchEdit->installEventFilter(this);

    m_popupWidget = new SearchPopupWidget(this);
    m_popupWidget->setGeometry(0, kBarHeight + kPopupGap, kPopupWidth, kPopupHeight);
    m_popupWidget->hide();

    updateSearchInputGeometry();

    connect(m_searchEdit, &QLineEdit::textChanged, this, &DesktopSearchBar::onSearchTextChanged);
    connect(m_popupWidget, &SearchPopupWidget::resultActivated, this, &DesktopSearchBar::onResultActivated);

    connect(m_popupWidget, &SearchPopupWidget::searchModeChanged, this, [this](search::SearchMode) {
        const QString trimmed = m_searchEdit->text().trimmed();
        if (!trimmed.isEmpty()) {
            dispatchAsyncSearch(trimmed);
        }
    });

    connect(m_popupWidget, &SearchPopupWidget::previewVisibilityChanged, this, [this](bool visible) {
        if (visible) {
            if (const auto* sel = m_popupWidget->selectedResult()) {
                dispatchInspectorPreviewFetch(*sel);
            }
        }
        updateExpandedGeometry();
    });

    connect(m_popupWidget, &SearchPopupWidget::selectedResultChanged,
            this, [this](const search::FileSearchResult& res) {
                dispatchInspectorPreviewFetch(res);
            });

    connect(m_popupWidget, &SearchPopupWidget::revealInExplorerRequested,
            this, &DesktopSearchBar::revealResultInExplorer);

    connect(m_popupWidget, &SearchPopupWidget::copyPathRequested,
            this, &DesktopSearchBar::copyResultPathToClipboard);

    connect(m_popupWidget, &SearchPopupWidget::findRelatedRequested,
            this, &DesktopSearchBar::dispatchRelatedDocumentsSearch);

    connect(m_popupWidget, &SearchPopupWidget::togglePinRequested,
            this, [this](const search::FileSearchResult& res) {
                core::SettingsManager::instance().togglePinnedFile(res.path, true);
                const QString trimmed = m_searchEdit->text().trimmed();
                if (!trimmed.isEmpty()) {
                    dispatchAsyncSearch(trimmed);
                }
            });

    connect(m_popupWidget, &SearchPopupWidget::exitRelatedModeRequested,
            this, [this]() {
                const QString trimmed = m_searchEdit->text().trimmed();
                if (!trimmed.isEmpty()) {
                    dispatchAsyncSearch(trimmed);
                }
            });

    connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
            this, [this](Qt::ColorScheme) {
                if (core::SettingsManager::instance().appearancePreference().compare(
                        QStringLiteral("system"), Qt::CaseInsensitive) == 0) {
                    refreshAppearanceFromSettings();
                }
            });

    refreshAppearanceFromSettings();
    applySavedOrDefaultPosition();
    bindScreenSignals();

    platform::windows::DesktopIntegration::installForegroundEventHook(
        [this](HWND /*foregroundHwnd*/, bool isDesktopShell) {
            if (!isVisible() || m_alwaysOnTop) {
                return;
            }
            const HWND myHwnd = reinterpret_cast<HWND>(winId());
            if (isDesktopShell) {
                m_keepAboveShellTemp = true;
                platform::windows::DesktopIntegration::raiseAboveShellDesktopNoActivate(myHwnd);
            } else if (!m_isSearchActive) {
                m_keepAboveShellTemp = false;
                platform::windows::DesktopIntegration::placeOnDesktopBottom(myHwnd);
            }
        });
}

DesktopSearchBar::~DesktopSearchBar() {
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }
    m_searchPool.clear();
    m_searchPool.waitForDone(2000);

    platform::windows::DesktopIntegration::uninstallForegroundEventHook();
    if (testAttribute(Qt::WA_WState_Created)) {
        const HWND hwnd = reinterpret_cast<HWND>(winId());
        platform::windows::DesktopIntegration::unregisterGlobalShortcut(hwnd);
    }
}

QPoint DesktopSearchBar::computeDefaultDesktopPosition() {
    const QScreen* primary = QGuiApplication::primaryScreen();
    if (primary == nullptr) {
        return QPoint(100, 88);
    }
    const QRect workArea = primary->availableGeometry();
    // Centered horizontally near the top of the screen (~88px below top work area edge)
    const int x = workArea.left() + (workArea.width() - kBarWidth) / 2;
    const int y = workArea.top() + kTopWorkAreaOffset;
    return clampPositionToScreens(QPoint(x, y));
}

QPoint DesktopSearchBar::clampPositionToScreens(const QPoint& candidatePos) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return candidatePos;
    }

    const QPoint centerPoint(candidatePos.x() + kBarWidth / 2, candidatePos.y() + kBarHeight / 2);
    const QScreen* targetScreen = QGuiApplication::screenAt(centerPoint);
    if (targetScreen == nullptr) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (targetScreen == nullptr) {
        targetScreen = screens.first();
    }

    const QRect workArea = targetScreen->availableGeometry();
    const int minX = workArea.left() + 8;
    const int maxX = qMax(minX, workArea.right() - kBarWidth - 8);
    const int minY = workArea.top() + 8;
    const int maxY = qMax(minY, workArea.bottom() - kBarHeight - 8);

    const int clampedX = qBound(minX, candidatePos.x(), maxX);
    const int clampedY = qBound(minY, candidatePos.y(), maxY);
    return QPoint(clampedX, clampedY);
}

void DesktopSearchBar::applySavedOrDefaultPosition() {
    auto& settings = core::SettingsManager::instance();
    QPoint targetPos;
    if (settings.hasCustomPosition()) {
        targetPos = clampPositionToScreens(settings.desktopBarPosition());
    } else {
        targetPos = computeDefaultDesktopPosition();
    }
    m_collapsedAnchorPos = targetPos;
    m_expandedXShift = 0;
    move(targetPos);
}

void DesktopSearchBar::applyRestingWindowZOrder() {
    if (!testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    if (m_alwaysOnTop) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        platform::windows::DesktopIntegration::placeOnDesktopBottom(hwnd);
    }
}

void DesktopSearchBar::refreshAppearanceFromSettings() {
    auto& settings = core::SettingsManager::instance();
    m_alwaysOnTop = settings.alwaysOnTopMode();

    const QString pref = settings.appearancePreference().toLower();
    if (pref == QStringLiteral("light")) {
        m_darkTheme = false;
    } else if (pref == QStringLiteral("system")) {
        m_darkTheme = (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    } else {
        // Default to Gemini's dark theme
        m_darkTheme = true;
    }

    const auto theme = ThemeTokens::get(m_darkTheme);
    const QString textColor = theme.textPrimary.name();
    const QString placeholderColor = theme.textSecondary.name();
    const QString selectionBg = theme.brandPink.name();

    m_searchEdit->setFont(ThemeTokens::makeFont(15, QFont::Medium));
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  selection-background-color: %2;"
        "  selection-color: #FFFFFF;"
        "}"
        "QLineEdit[ placeholderText ] {"
        "  color: %3;"
        "}")
        .arg(textColor, selectionBg, placeholderColor));

    if (m_popupWidget != nullptr) {
        m_popupWidget->setDarkTheme(m_darkTheme);
    }
    if (!m_isSearchActive) {
        applyRestingWindowZOrder();
    }
    updateSearchInputGeometry();
    update();
}

bool DesktopSearchBar::registerConfiguredGlobalShortcut() {
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    const QString shortcut = core::SettingsManager::instance().globalShortcut();
    m_hotkeyError.clear();
    m_hotkeyRegistered = platform::windows::DesktopIntegration::registerGlobalShortcut(
        hwnd,
        shortcut,
        platform::windows::DesktopIntegration::kDefaultHotkeyId,
        &m_hotkeyError);
    return m_hotkeyRegistered;
}

void DesktopSearchBar::notifyIndexUpdated() {
    const QString trimmed = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (m_popupExpanded && !trimmed.isEmpty()) {
        dispatchAsyncSearch(trimmed);
    }
}

void DesktopSearchBar::activateAndFocusSearch() {
    if (!isVisible()) {
        show();
    }

    m_isSearchActive = true;
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    platform::windows::DesktopIntegration::activateForSearchInput(hwnd);
    raise();
    activateWindow();
    m_searchEdit->setFocus(Qt::ShortcutFocusReason);
    m_searchEdit->selectAll();

    const QString trimmed = m_searchEdit->text().trimmed();
    if (!trimmed.isEmpty()) {
        setPopupExpanded(true);
        dispatchAsyncSearch(trimmed);
    }
    update();
}

void DesktopSearchBar::collapsePopupAndDeactivate(bool clearQuery) {
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }
    ++m_previewGeneration;
    if (clearQuery && !m_searchEdit->text().isEmpty()) {
        m_searchEdit->clear();
    }
    if (m_popupWidget != nullptr) {
        m_popupWidget->setPreviewVisible(false);
        m_popupWidget->setPreviewText(QString());
        m_popupWidget->setRelatedSourceFileName(QString());
    }
    setPopupExpanded(false);
    m_searchEdit->deselect();
    m_searchEdit->clearFocus();
    m_isSearchActive = false;

    applyRestingWindowZOrder();

    if (semantic::SemanticEmbeddingEngine::instance().isSessionLoaded()) {
        QTimer::singleShot(180000, this, []() {
            semantic::SemanticEmbeddingEngine::instance().checkAndUnloadIfIdle();
        });
    }
    update();
}

QString DesktopSearchBar::searchText() const {
    return m_searchEdit ? m_searchEdit->text() : QString();
}

void DesktopSearchBar::setSearchText(const QString& text) {
    if (m_searchEdit != nullptr) {
        m_searchEdit->setText(text);
    }
}

QRectF DesktopSearchBar::clearButtonHitRect() const {
    if (m_popupExpanded) {
        return QRectF(width() - 88.0, 17.0, 22.0, 22.0);
    }
    return QRectF(width() - 40.0, 17.0, 22.0, 22.0);
}

QRectF DesktopSearchBar::escButtonHitRect() const {
    return QRectF(width() - 56.0, 16.0, 42.0, 24.0);
}

void DesktopSearchBar::updateSearchInputGeometry() {
    if (!m_searchEdit) {
        return;
    }
    const int leftOffset = 56;
    const bool hasText = !m_searchEdit->text().trimmed().isEmpty();
    int rightReserve = 128;
    if (m_popupExpanded) {
        rightReserve = hasText ? 98 : 68;
    } else if (hasText) {
        rightReserve = 48;
    }
    const int editWidth = std::max(140, width() - leftOffset - rightReserve);
    m_searchEdit->setGeometry(leftOffset, 11, editWidth, 34);
}

void DesktopSearchBar::setPopupExpanded(bool expanded) {
    if (m_popupExpanded == expanded) {
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
        return;
    }

    if (expanded) {
        m_collapsedAnchorPos = pos();
        m_popupExpanded = true;
        updateExpandedGeometry();
        m_popupWidget->show();
    } else {
        const QPoint restorePos = QPoint(pos().x() + m_expandedXShift, pos().y());
        m_popupExpanded = false;
        m_expandedXShift = 0;
        m_popupWidget->hide();
        setFixedSize(kBarWidth, kBarHeight);
        move(restorePos);
        updateSearchInputGeometry();
    }
    update();
    emit popupVisibilityChanged(m_popupExpanded);
}

void DesktopSearchBar::updateExpandedGeometry() {
    if (!m_popupExpanded || m_popupWidget == nullptr) {
        return;
    }

    const QPoint anchorPos = QPoint(pos().x() + m_expandedXShift, pos().y());
    const QPoint centerPoint(anchorPos.x() + kBarWidth / 2, anchorPos.y() + kBarHeight / 2);
    const QScreen* screen = QGuiApplication::screenAt(centerPoint);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }

    int popW = kPopupWidth;
    if (screen != nullptr) {
        const QRect workArea = screen->availableGeometry();
        popW = std::min(kPopupWidth, std::max(kBarWidth, workArea.width() - 24));
    }

    const int popH = m_popupWidget->desiredPopupHeight();
    const int totalH = kBarHeight + kPopupGap + popH;

    // Expand symmetrically around the center of the collapsed bar, clamped within usable screen bounds
    int targetWinX = anchorPos.x() - (popW - kBarWidth) / 2;
    if (screen != nullptr) {
        const QRect workArea = screen->availableGeometry();
        const int minX = workArea.left() + 12;
        const int maxX = qMax(minX, workArea.right() - popW - 12);
        targetWinX = qBound(minX, targetWinX, maxX);
    }

    m_expandedXShift = anchorPos.x() - targetWinX;

    if (width() != popW || height() != totalH) {
        setFixedSize(popW, totalH);
    }
    if (pos().x() != targetWinX) {
        move(targetWinX, anchorPos.y());
    }

    updateSearchInputGeometry();
    m_popupWidget->setGeometry(0, kBarHeight + kPopupGap, popW, popH);
}

void DesktopSearchBar::dispatchInspectorPreviewFetch(const search::FileSearchResult& result) {
    if (!m_popupWidget) {
        return;
    }
    const quint64 pGen = ++m_previewGeneration;
    const qint64 fileId = result.id;
    const QString filePath = result.path;

    if (filePath.isEmpty()) {
        m_popupWidget->setPreviewText(QString());
        return;
    }

    QPointer<DesktopSearchBar> safeThis(this);
    m_searchPool.start(QRunnable::create([safeThis, fileId, filePath, pGen]() {
        if (!safeThis) {
            return;
        }
        const QString preview = safeThis->m_searchEngine.fetchDocumentPreviewText(fileId, filePath, 800);
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, preview, pGen]() {
                if (!safeThis || pGen != safeThis->m_previewGeneration || !safeThis->m_popupWidget) {
                    return;
                }
                safeThis->m_popupWidget->setPreviewText(preview);
            },
            Qt::QueuedConnection);
    }));
}

void DesktopSearchBar::dispatchAsyncSearch(const QString& trimmedQuery) {
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }

    if (m_popupWidget != nullptr) {
        m_popupWidget->setRelatedSourceFileName(QString());
    }

    const quint64 gen = ++m_queryGeneration;
    auto cancelToken = std::make_shared<std::atomic_bool>(false);
    m_activeCancelToken = cancelToken;
    const search::SearchMode mode = m_popupWidget ? m_popupWidget->searchMode() : search::SearchMode::All;
    const search::SearchFilters filters = search::FileSearchEngine::filtersForSearchMode(mode);

    QPointer<DesktopSearchBar> safeThis(this);
    m_searchPool.start(QRunnable::create([safeThis, trimmedQuery, filters, gen, cancelToken]() {
        if (!safeThis || cancelToken->load(std::memory_order_relaxed)) {
            return;
        }
        double elapsedMs = 0.0;
        QVector<search::FileSearchResult> results = safeThis->m_searchEngine.searchFiles(
            trimmedQuery, filters, 50, cancelToken, &elapsedMs);

        if (cancelToken->load(std::memory_order_relaxed) || !safeThis) {
            return;
        }

        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, results = std::move(results), elapsedMs, gen]() mutable {
                if (!safeThis || gen != safeThis->m_queryGeneration) {
                    return;
                }
                if (safeThis->m_searchEdit->text().trimmed().isEmpty()) {
                    return;
                }
                const int count = static_cast<int>(results.size());
                safeThis->m_popupWidget->setSearchResults(std::move(results), elapsedMs);
                safeThis->updateExpandedGeometry();
                emit safeThis->searchCompleted(count, elapsedMs);
            },
            Qt::QueuedConnection);
    }));
}

void DesktopSearchBar::dispatchRelatedDocumentsSearch(const search::FileSearchResult& sourceResult) {
    if (sourceResult.id <= 0) {
        return;
    }
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }

    const quint64 gen = ++m_queryGeneration;
    const qint64 srcId = sourceResult.id;
    const QString srcName = sourceResult.name;

    QPointer<DesktopSearchBar> safeThis(this);
    m_searchPool.start(QRunnable::create([safeThis, srcId, srcName, gen]() {
        if (!safeThis) {
            return;
        }
        double elapsedMs = 0.0;
        QVector<search::FileSearchResult> related = safeThis->m_searchEngine.findRelatedDocuments(
            srcId, 20, &elapsedMs);

        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, related = std::move(related), srcName, elapsedMs, gen]() mutable {
                if (!safeThis || gen != safeThis->m_queryGeneration) {
                    return;
                }
                const int count = static_cast<int>(related.size());
                safeThis->m_popupWidget->setRelatedSourceFileName(srcName);
                safeThis->m_popupWidget->setSearchResults(std::move(related), elapsedMs);
                safeThis->updateExpandedGeometry();
                emit safeThis->searchCompleted(count, elapsedMs);
            },
            Qt::QueuedConnection);
    }));
}

void DesktopSearchBar::onSearchTextChanged(const QString& text) {
    const QString trimmed = text.trimmed();
    m_popupWidget->setQueryText(trimmed);

    if (trimmed.isEmpty()) {
        if (m_activeCancelToken) {
            m_activeCancelToken->store(true, std::memory_order_relaxed);
        }
        ++m_queryGeneration;
        ++m_previewGeneration;
        m_popupWidget->clearResults();
        setPopupExpanded(false);
        updateSearchInputGeometry();
    } else {
        setPopupExpanded(true);
        updateSearchInputGeometry();
        dispatchAsyncSearch(trimmed);
    }
    update();
}

void DesktopSearchBar::onResultActivated(const search::FileSearchResult& result) {
    const QString trimmed = m_searchEdit ? m_searchEdit->text().trimmed() : QString();
    if (!trimmed.isEmpty()) {
        core::SettingsManager::instance().addRecentSearch(trimmed, true);
    }
    emit fileResultActivated(result);
    if (m_openFilesWithShell && !result.path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(result.path));
    }
    collapsePopupAndDeactivate(false);
}

void DesktopSearchBar::revealResultInExplorer(const search::FileSearchResult& result) {
    if (result.path.isEmpty()) {
        return;
    }
    emit fileRevealedInExplorer(result);
    if (m_openFilesWithShell) {
        const QString nativePath = QDir::toNativeSeparators(result.path);
        const QString param = QStringLiteral("/select,\"%1\"").arg(nativePath);
        const std::wstring wParam = param.toStdWString();
        ::ShellExecuteW(nullptr, L"open", L"explorer.exe", wParam.c_str(), nullptr, SW_SHOWNORMAL);
    }
}

void DesktopSearchBar::copyResultPathToClipboard(const search::FileSearchResult& result) {
    if (result.path.isEmpty()) {
        return;
    }
    const QString nativePath = QDir::toNativeSeparators(result.path);
    if (auto* cb = QGuiApplication::clipboard()) {
        cb->setText(nativePath);
    }
    emit filePathCopied(nativePath);
}

void DesktopSearchBar::triggerFindRelatedForSelected() {
    if (m_popupWidget != nullptr) {
        if (const auto* sel = m_popupWidget->selectedResult()) {
            dispatchRelatedDocumentsSearch(*sel);
        }
    }
}

void DesktopSearchBar::bindScreenSignals() {
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this](QScreen* screen) {
        if (screen != nullptr) {
            connect(screen, &QScreen::availableGeometryChanged, this, &DesktopSearchBar::onScreenConfigurationChanged);
            connect(screen, &QScreen::logicalDotsPerInchChanged, this, &DesktopSearchBar::onScreenConfigurationChanged);
        }
        onScreenConfigurationChanged();
    });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this](QScreen*) {
        onScreenConfigurationChanged();
    });
    connect(qGuiApp, &QGuiApplication::primaryScreenChanged, this, [this](QScreen*) {
        onScreenConfigurationChanged();
    });

    for (QScreen* screen : QGuiApplication::screens()) {
        if (screen != nullptr) {
            connect(screen, &QScreen::availableGeometryChanged, this, &DesktopSearchBar::onScreenConfigurationChanged);
            connect(screen, &QScreen::logicalDotsPerInchChanged, this, &DesktopSearchBar::onScreenConfigurationChanged);
        }
    }
}

void DesktopSearchBar::onScreenConfigurationChanged() {
    auto& settings = core::SettingsManager::instance();
    const QPoint currentAnchor = QPoint(pos().x() + m_expandedXShift, pos().y());
    QPoint nextAnchor;
    if (settings.hasCustomPosition()) {
        nextAnchor = clampPositionToScreens(currentAnchor);
    } else {
        nextAnchor = computeDefaultDesktopPosition();
    }
    m_collapsedAnchorPos = nextAnchor;
    if (m_popupExpanded) {
        move(nextAnchor.x() - m_expandedXShift, nextAnchor.y());
        updateExpandedGeometry();
    } else if (pos() != nextAnchor) {
        move(nextAnchor);
    }
}

void DesktopSearchBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const auto theme = ThemeTokens::get(m_darkTheme);
    const QRectF fullBounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const qreal radius = 16.0;

    // 1. Unified morphing outer container (collapsed 540x56 pill -> expanded 920px modal workspace)
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.shadowColor);
    painter.drawRoundedRect(fullBounds.adjusted(0.0, 2.0, 0.0, 2.0), radius, radius);

    const bool hasFocus = m_isSearchActive || m_searchEdit->hasFocus() || m_popupExpanded;
    const QColor borderColor = hasFocus ? theme.brandPinkBorder : theme.borderColor;
    QPen borderPen(borderColor, hasFocus ? 1.5 : 1.0);
    painter.setPen(borderPen);
    painter.setBrush(theme.surfaceBackground);
    painter.drawRoundedRect(fullBounds.adjusted(0.0, 0.0, 0.0, -0.5), radius, radius);

    // 2. Mandatory existing Lumen application icon on the left (32x32)
    const QRectF iconRect(14.0, 12.0, 32.0, 32.0);
    BrandingIcon::paintLumenMark(&painter, iconRect, m_darkTheme);

    // 3. Right-hand controls inside the top search header
    if (m_popupExpanded) {
        if (!m_searchEdit->text().isEmpty()) {
            const QRectF clearRect = clearButtonHitRect();
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.hoverSurface);
            painter.drawEllipse(clearRect);

            QPen xPen(theme.textSecondary, 1.5);
            xPen.setCapStyle(Qt::RoundCap);
            painter.setPen(xPen);
            const QPointF c = clearRect.center();
            constexpr qreal d = 3.5;
            painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
            painter.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
        }

        // ESC badge button matching Gemini reference
        const QRectF escRect = escButtonHitRect();
        painter.setPen(QPen(theme.brandPinkBorder, 1.0));
        painter.setBrush(theme.brandPinkTint);
        painter.drawRoundedRect(escRect, 6.0, 6.0);

        painter.setFont(ThemeTokens::makeMonoFont(11, QFont::DemiBold));
        painter.setPen(theme.brandPinkLight);
        painter.drawText(escRect, Qt::AlignCenter, QStringLiteral("ESC"));

        // Hairline divider separating top search bar from the expanded workspace
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.drawLine(QPointF(fullBounds.left() + 1.0, kBarHeight),
                         QPointF(fullBounds.right() - 1.0, kBarHeight));
    } else if (!m_searchEdit->text().isEmpty()) {
        const QRectF clearRect = clearButtonHitRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.hoverSurface);
        painter.drawEllipse(clearRect);

        QPen xPen(theme.textSecondary, 1.5);
        xPen.setCapStyle(Qt::RoundCap);
        painter.setPen(xPen);
        const QPointF c = clearRect.center();
        constexpr qreal d = 3.5;
        painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
        painter.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
    } else {
        // Actual configured shortcut indicator on the right
        const QString shortcutText = core::SettingsManager::instance().globalShortcut();
        const QFont hintFont = ThemeTokens::makeMonoFont(11, QFont::Medium);
        painter.setFont(hintFont);
        const QFontMetrics fm(hintFont);
        const int textW = fm.horizontalAdvance(shortcutText);
        const int pillW = qBound(56, textW + 16, 114);
        const QRectF pillRect(fullBounds.right() - pillW - 14.0, 16.0, pillW, 24.0);

        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.setBrush(theme.elevatedBackground);
        painter.drawRoundedRect(pillRect, 6.0, 6.0);

        painter.setPen(theme.textSecondary);
        painter.drawText(pillRect, Qt::AlignCenter, shortcutText);
    }
}

void DesktopSearchBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    platform::windows::DesktopIntegration::applyDesktopWidgetStyles(hwnd);
    if (!m_isSearchActive) {
        applyRestingWindowZOrder();
    }
    if (!m_hotkeyRegistered) {
        registerConfiguredGlobalShortcut();
    }
}

void DesktopSearchBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF localPos = event->position();

        if (m_popupExpanded && escButtonHitRect().contains(localPos)) {
            collapsePopupAndDeactivate(false);
            event->accept();
            return;
        }

        if (!m_searchEdit->text().isEmpty() && clearButtonHitRect().contains(localPos)) {
            m_searchEdit->clear();
            m_searchEdit->setFocus();
            event->accept();
            return;
        }

        if (localPos.y() <= kBarHeight) {
            m_dragging = true;
            m_dragMoved = false;
            const QPoint currentAnchor(pos().x() + m_expandedXShift, pos().y());
            m_dragOffset = event->globalPosition().toPoint() - currentAnchor;
            m_isSearchActive = true;
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            platform::windows::DesktopIntegration::activateForSearchInput(hwnd);
            m_searchEdit->setFocus(Qt::MouseFocusReason);
            update();
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void DesktopSearchBar::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const QPoint currentAnchor(pos().x() + m_expandedXShift, pos().y());
        const QPoint newAnchor = clampPositionToScreens(event->globalPosition().toPoint() - m_dragOffset);
        if ((newAnchor - currentAnchor).manhattanLength() >= 2) {
            m_dragMoved = true;
            m_collapsedAnchorPos = newAnchor;
            if (m_popupExpanded) {
                move(newAnchor.x() - m_expandedXShift, newAnchor.y());
                updateExpandedGeometry();
            } else {
                move(newAnchor);
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void DesktopSearchBar::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && m_dragging) {
        m_dragging = false;
        if (m_dragMoved) {
            const QPoint currentAnchor(pos().x() + m_expandedXShift, pos().y());
            const QPoint finalAnchor = clampPositionToScreens(currentAnchor);
            m_collapsedAnchorPos = finalAnchor;
            if (m_popupExpanded) {
                move(finalAnchor.x() - m_expandedXShift, finalAnchor.y());
                updateExpandedGeometry();
            } else {
                move(finalAnchor);
            }
            core::SettingsManager::instance().setDesktopBarPosition(finalAnchor, true);
            emit positionPersisted(finalAnchor);
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DesktopSearchBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        collapsePopupAndDeactivate(false);
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool DesktopSearchBar::eventFilter(QObject* watched, QEvent* event) {
    if (watched == m_searchEdit) {
        if (event->type() == QEvent::MouseButtonPress) {
            m_isSearchActive = true;
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            platform::windows::DesktopIntegration::activateForSearchInput(hwnd);
            setPopupExpanded(true);
            updateSearchInputGeometry();
            update();
        } else if (event->type() == QEvent::FocusIn) {
            m_isSearchActive = true;
            if (!m_searchEdit->text().trimmed().isEmpty()) {
                setPopupExpanded(true);
            }
            update();
        } else if (event->type() == QEvent::KeyPress) {
            auto* keyEvent = static_cast<QKeyEvent*>(event);
            const Qt::KeyboardModifiers mods = keyEvent->modifiers();
            const bool ctrl = (mods & Qt::ControlModifier) != 0;
            const bool shift = (mods & Qt::ShiftModifier) != 0;

            switch (keyEvent->key()) {
                case Qt::Key_Escape:
                    collapsePopupAndDeactivate(false);
                    return true;
                case Qt::Key_Tab:
                case Qt::Key_Backtab:
                    if (m_popupExpanded) {
                        m_popupWidget->cycleSearchMode(keyEvent->key() != Qt::Key_Backtab && !shift);
                        return true;
                    }
                    break;
                case Qt::Key_Down:
                    if (!m_popupExpanded) {
                        setPopupExpanded(true);
                        return true;
                    }
                    if (m_popupWidget->resultCount() > 0) {
                        m_popupWidget->selectNext();
                        return true;
                    }
                    break;
                case Qt::Key_Up:
                    if (!m_popupExpanded) {
                        setPopupExpanded(true);
                        return true;
                    }
                    if (m_popupWidget->resultCount() > 0) {
                        m_popupWidget->selectPrevious();
                        return true;
                    }
                    break;
                case Qt::Key_Return:
                case Qt::Key_Enter:
                    if (m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        if (ctrl) {
                            if (const auto* sel = m_popupWidget->selectedResult()) {
                                revealResultInExplorer(*sel);
                                return true;
                            }
                        }
                        m_popupWidget->activateSelected();
                        return true;
                    }
                    break;
                case Qt::Key_P:
                    if (ctrl && m_popupExpanded) {
                        m_popupWidget->togglePreview();
                        return true;
                    }
                    break;
                case Qt::Key_R:
                    if (ctrl && m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        triggerFindRelatedForSelected();
                        return true;
                    }
                    break;
                case Qt::Key_D:
                    if (ctrl && m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        if (const auto* sel = m_popupWidget->selectedResult()) {
                            core::SettingsManager::instance().togglePinnedFile(sel->path, true);
                            const QString trimmed = m_searchEdit->text().trimmed();
                            if (!trimmed.isEmpty()) {
                                dispatchAsyncSearch(trimmed);
                            }
                            return true;
                        }
                    }
                    break;
                case Qt::Key_C:
                    if (ctrl && m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        if (shift || !m_searchEdit->hasSelectedText()) {
                            if (const auto* sel = m_popupWidget->selectedResult()) {
                                copyResultPathToClipboard(*sel);
                                return true;
                            }
                        }
                    }
                    break;
                default:
                    break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

bool DesktopSearchBar::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    if (eventType == "windows_generic_MSG" || eventType == "windows_dispatcher_MSG") {
        MSG* msg = static_cast<MSG*>(message);
        if (msg == nullptr) {
            return false;
        }

        if (msg->message == platform::windows::SingleInstanceGuard::activationMessageId() &&
            msg->message != 0) {
            core::Logger::info(QStringLiteral("App"),
                               QStringLiteral("Received activation request from secondary instance."));
            activateAndFocusSearch();
            if (result) {
                *result = 1;
            }
            return true;
        }

        if (msg->message == platform::windows::DesktopIntegration::taskbarCreatedMessageId() &&
            msg->message != 0) {
            core::Logger::info(QStringLiteral("Platform"),
                               QStringLiteral("Windows Explorer restart detected (TaskbarCreated). Restoring desktop z-order."));
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            platform::windows::DesktopIntegration::applyDesktopWidgetStyles(hwnd);
            if (!m_isSearchActive) {
                applyRestingWindowZOrder();
            }
            emit explorerRestartDetected();
            return false;
        }

        if (msg->message == WM_HOTKEY &&
            static_cast<int>(msg->wParam) == platform::windows::DesktopIntegration::kDefaultHotkeyId) {
            activateAndFocusSearch();
            if (result) {
                *result = 0;
            }
            return true;
        }

        if (msg->message == WM_WINDOWPOSCHANGING && !m_alwaysOnTop && !m_isSearchActive && !m_keepAboveShellTemp) {
            auto* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
            if (wp != nullptr && (wp->flags & SWP_NOZORDER) == 0) {
                wp->hwndInsertAfter = HWND_BOTTOM;
            }
        }

        if (msg->message == WM_ACTIVATEAPP && msg->wParam == FALSE) {
            if (m_isSearchActive || m_popupExpanded) {
                collapsePopupAndDeactivate(false);
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace lumen::ui
