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
#include <QDesktopServices>
#include <QDir>
#include <QEasingCurve>
#include <QEvent>
#include <QFont>
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
    setFixedSize(kIconSize, kIconSize);
    setWindowTitle(QStringLiteral("Lumen Desktop Search"));

    m_searchPool.setMaxThreadCount(1);
    m_searchPool.setExpiryTimeout(5000);

    m_slideAnim = new QVariantAnimation(this);
    m_slideAnim->setDuration(155);
    m_slideAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_slideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_slideProgress = value.toReal();
        if (m_barExpanded && !m_popupExpanded) {
            applyOpenBarGeometry();
        }
        update();
    });

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(QStringLiteral("Search files, documents, and more..."));
    m_searchEdit->setFrame(false);
    m_searchEdit->setFont(ThemeTokens::makeFont(14, QFont::Normal));
    m_searchEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_searchEdit->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_searchEdit->installEventFilter(this);
    m_searchEdit->hide();

    m_popupWidget = new SearchPopupWidget(this);
    m_popupWidget->setGeometry(0, kBarHeight + kPopupGap, kPopupWidth, kPopupHeight);
    m_popupWidget->hide();

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
            } else if (!m_isSearchActive && !m_barExpanded) {
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
        return QPoint(1800, 48);
    }
    const QRect workArea = primary->availableGeometry();
    // Docked cleanly on the right edge of the desktop by default
    const int x = workArea.right() - kIconSize - kRightEdgeMargin;
    const int y = workArea.top() + kTopWorkAreaOffset;
    return clampPositionToScreens(QPoint(x, y), kIconSize, kIconSize);
}

QPoint DesktopSearchBar::clampPositionToScreens(const QPoint& candidatePos, int widgetW, int widgetH) {
    const QList<QScreen*> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return candidatePos;
    }

    const QPoint centerPoint(candidatePos.x() + widgetW / 2, candidatePos.y() + widgetH / 2);
    const QScreen* targetScreen = QGuiApplication::screenAt(centerPoint);
    if (targetScreen == nullptr) {
        targetScreen = QGuiApplication::primaryScreen();
    }
    if (targetScreen == nullptr) {
        targetScreen = screens.first();
    }

    const QRect workArea = targetScreen->availableGeometry();
    const int minX = workArea.left() + 8;
    const int maxX = qMax(minX, workArea.right() - widgetW - 8);
    const int minY = workArea.top() + 8;
    const int maxY = qMax(minY, workArea.bottom() - widgetH - 8);

    return QPoint(qBound(minX, candidatePos.x(), maxX),
                  qBound(minY, candidatePos.y(), maxY));
}

bool DesktopSearchBar::shouldSlideLeftFromAnchor(const QPoint& anchorPos) {
    const QPoint centerPoint(anchorPos.x() + kIconSize / 2, anchorPos.y() + kIconSize / 2);
    const QScreen* screen = QGuiApplication::screenAt(centerPoint);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return true;
    }
    const QRect workArea = screen->availableGeometry();
    const int midX = workArea.left() + workArea.width() / 2;
    return centerPoint.x() >= midX;
}

void DesktopSearchBar::applySavedOrDefaultPosition() {
    auto& settings = core::SettingsManager::instance();
    QPoint targetPos;
    if (settings.hasCustomPosition()) {
        targetPos = clampPositionToScreens(settings.desktopBarPosition(), kIconSize, kIconSize);
    } else {
        targetPos = computeDefaultDesktopPosition();
    }
    m_iconAnchorPos = targetPos;
    m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);

    if (m_popupExpanded) {
        updateExpandedGeometry();
    } else if (m_barExpanded) {
        applyOpenBarGeometry();
    } else {
        setFixedSize(kIconSize, kIconSize);
        move(m_iconAnchorPos);
    }
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

    const auto theme = ThemeTokens::get(false);
    const QString textColor = theme.textPrimary.name();
    const QString placeholderColor = theme.textMuted.name();
    const QString selectionBg = theme.brandPinkStrongFill.name();

    m_searchEdit->setFont(ThemeTokens::makeFont(14, QFont::Normal));
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  selection-background-color: %2;"
        "  selection-color: %1;"
        "}"
        "QLineEdit[ placeholderText ] {"
        "  color: %3;"
        "}")
        .arg(textColor, selectionBg, placeholderColor));

    if (m_popupWidget != nullptr) {
        m_popupWidget->setDarkTheme(false);
    }
    if (!m_isSearchActive && !m_barExpanded) {
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

void DesktopSearchBar::activateAndFocusSearch(bool animateSlide) {
    if (!isVisible()) {
        show();
    }

    m_isSearchActive = true;
    m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);

    const bool wasExpanded = m_barExpanded;
    m_barExpanded = true;

    const HWND hwnd = reinterpret_cast<HWND>(winId());
    platform::windows::DesktopIntegration::activateForSearchInput(hwnd);
    raise();
    activateWindow();

    m_searchEdit->show();
    m_searchEdit->setFocus(Qt::ShortcutFocusReason);

    const QString trimmed = m_searchEdit->text().trimmed();
    if (!trimmed.isEmpty()) {
        m_slideAnim->stop();
        m_slideProgress = 1.0;
        setPopupExpanded(true);
        dispatchAsyncSearch(trimmed);
    } else if (!wasExpanded && animateSlide) {
        m_slideAnim->stop();
        m_slideProgress = 0.0;
        m_slideAnim->setStartValue(0.0);
        m_slideAnim->setEndValue(1.0);
        applyOpenBarGeometry();
        m_slideAnim->start();
    } else {
        m_slideAnim->stop();
        m_slideProgress = 1.0;
        applyOpenBarGeometry();
    }

    emit barVisibilityExpandedChanged(true);
    update();
}

void DesktopSearchBar::collapsePopupAndDeactivate(bool clearQuery) {
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }
    ++m_previewGeneration;

    m_slideAnim->stop();
    m_slideProgress = 0.0;

    if (clearQuery && !m_searchEdit->text().isEmpty()) {
        m_searchEdit->clear();
    }
    if (m_popupWidget != nullptr) {
        m_popupWidget->setPreviewVisible(false);
        m_popupWidget->setPreviewText(QString());
        m_popupWidget->setRelatedSourceFileName(QString());
    }

    setPopupExpanded(false);
    m_barExpanded = false;
    m_isSearchActive = false;

    m_searchEdit->deselect();
    m_searchEdit->clearFocus();
    m_searchEdit->hide();

    setFixedSize(kIconSize, kIconSize);
    move(m_iconAnchorPos);

    applyRestingWindowZOrder();

    if (semantic::SemanticEmbeddingEngine::instance().isSessionLoaded()) {
        QTimer::singleShot(180000, this, []() {
            semantic::SemanticEmbeddingEngine::instance().checkAndUnloadIfIdle();
        });
    }

    emit barVisibilityExpandedChanged(false);
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

QRectF DesktopSearchBar::iconTileRect() const {
    // Always anchored on the left side whether collapsed or slid open from left or right
    return QRectF(10.0, 10.0, 36.0, 36.0);
}

QRectF DesktopSearchBar::clearButtonHitRect() const {
    return QRectF(width() - 38.0, 17.0, 22.0, 22.0);
}

void DesktopSearchBar::updateSearchInputGeometry() {
    if (!m_searchEdit) {
        return;
    }
    if (!m_barExpanded) {
        m_searchEdit->hide();
        return;
    }
    const bool hasText = !m_searchEdit->text().trimmed().isEmpty();
    const int w = width();
    const int leftOffset = 56;
    const int rightMargin = hasText ? 46 : 18;
    const int editWidth = std::max(0, w - leftOffset - rightMargin);
    if (editWidth < 40) {
        m_searchEdit->setGeometry(leftOffset, 11, 0, 34);
    } else {
        m_searchEdit->show();
        m_searchEdit->setGeometry(leftOffset, 11, editWidth, 34);
    }
}

void DesktopSearchBar::applyOpenBarGeometry() {
    const QPoint centerPoint(m_iconAnchorPos.x() + kIconSize / 2, m_iconAnchorPos.y() + kIconSize / 2);
    const QScreen* screen = QGuiApplication::screenAt(centerPoint);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }

    int targetW = kBarWidth;
    QRect workArea(0, 0, 1920, 1080);
    if (screen != nullptr) {
        workArea = screen->availableGeometry();
        targetW = std::min(kBarWidth, std::max(kIconSize, workArea.width() - 24));
    }

    const int curW = std::max(kIconSize, static_cast<int>(qRound(kIconSize + (targetW - kIconSize) * m_slideProgress)));
    int targetX = m_iconAnchorPos.x();
    if (m_slidesLeft) {
        const int anchorRight = m_iconAnchorPos.x() + kIconSize;
        targetX = qMax(workArea.left() + 8, anchorRight - curW);
    } else {
        targetX = qMin(workArea.right() - curW - 8, m_iconAnchorPos.x());
    }

    if (width() != curW || height() != kBarHeight) {
        setFixedSize(curW, kBarHeight);
    }
    if (pos().x() != targetX || pos().y() != m_iconAnchorPos.y()) {
        move(targetX, m_iconAnchorPos.y());
    }
    updateSearchInputGeometry();
}

void DesktopSearchBar::setPopupExpanded(bool expanded) {
    if (m_popupExpanded == expanded) {
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
        return;
    }

    if (expanded) {
        m_slideAnim->stop();
        m_slideProgress = 1.0;
        m_barExpanded = true;
        m_popupExpanded = true;
        m_searchEdit->show();
        updateExpandedGeometry();
        m_popupWidget->show();
    } else {
        m_popupExpanded = false;
        m_popupWidget->hide();
        if (m_barExpanded) {
            m_slideProgress = 1.0;
            applyOpenBarGeometry();
        } else {
            setFixedSize(kIconSize, kIconSize);
            move(m_iconAnchorPos);
            m_searchEdit->hide();
        }
    }
    update();
    emit popupVisibilityChanged(m_popupExpanded);
}

void DesktopSearchBar::updateExpandedGeometry() {
    if (!m_popupExpanded || m_popupWidget == nullptr) {
        return;
    }

    const QPoint centerPoint(m_iconAnchorPos.x() + kIconSize / 2, m_iconAnchorPos.y() + kIconSize / 2);
    const QScreen* screen = QGuiApplication::screenAt(centerPoint);
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }

    QRect workArea(0, 0, 1920, 1080);
    int popW = kPopupWidth;
    if (screen != nullptr) {
        workArea = screen->availableGeometry();
        popW = std::min(kPopupWidth, std::max(kBarWidth, workArea.width() - 24));
    }

    const int popH = m_popupWidget->desiredPopupHeight();
    const int totalH = kBarHeight + kPopupGap + popH;

    int targetWinX = m_iconAnchorPos.x();
    if (m_slidesLeft) {
        const int anchorRight = m_iconAnchorPos.x() + kIconSize;
        targetWinX = anchorRight - popW;
    }
    const int minX = workArea.left() + 12;
    const int maxX = qMax(minX, workArea.right() - popW - 12);
    targetWinX = qBound(minX, targetWinX, maxX);

    int targetWinY = m_iconAnchorPos.y();
    if (targetWinY + totalH > workArea.bottom() - 8) {
        targetWinY = qMax(workArea.top() + 8, workArea.bottom() - totalH - 8);
    }

    if (width() != popW || height() != totalH) {
        setFixedSize(popW, totalH);
    }
    if (pos().x() != targetWinX || pos().y() != targetWinY) {
        move(targetWinX, targetWinY);
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
        // Nothing shown underneath when query is empty
        setPopupExpanded(false);
        updateSearchInputGeometry();
    } else {
        // Explicit user query -> immediately finish slide if animating and open results
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
    collapsePopupAndDeactivate(true);
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
    if (settings.hasCustomPosition()) {
        m_iconAnchorPos = clampPositionToScreens(m_iconAnchorPos, kIconSize, kIconSize);
    } else {
        m_iconAnchorPos = computeDefaultDesktopPosition();
    }
    m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);

    if (m_popupExpanded) {
        updateExpandedGeometry();
    } else if (m_barExpanded) {
        applyOpenBarGeometry();
    } else if (pos() != m_iconAnchorPos) {
        move(m_iconAnchorPos);
    }
}

void DesktopSearchBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const auto theme = ThemeTokens::get(false);
    const QRectF fullBounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const qreal radius = m_barExpanded ? 16.0 : 16.0;

    // 1. Clean pure white surface with soft pink border
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.shadowColor);
    painter.drawRoundedRect(fullBounds.adjusted(0.0, 2.0, 0.0, 2.0), radius, radius);

    const bool hasFocus = m_barExpanded || m_isSearchActive || m_popupExpanded;
    const QColor borderColor = hasFocus ? theme.brandPink : theme.borderColor;
    QPen borderPen(borderColor, hasFocus ? 1.5 : 1.2);
    painter.setPen(borderPen);
    painter.setBrush(theme.surfaceBackground);
    painter.drawRoundedRect(fullBounds.adjusted(0.0, 0.0, 0.0, -0.5), radius, radius);

    // 2. Lumen Icon Mark (centered in collapsed tile, or docked on Left/Right side when slid open)
    BrandingIcon::paintLumenMark(&painter, iconTileRect(), false);

    // 3. When bar is expanded, draw optional clean Clear button (NO ESC button!)
    if (m_barExpanded) {
        if (!m_searchEdit->text().isEmpty() && width() > 140) {
            const QRectF clearRect = clearButtonHitRect();
            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.brandPinkTint);
            painter.drawEllipse(clearRect);

            QPen xPen(theme.textPrimary, 1.5);
            xPen.setCapStyle(Qt::RoundCap);
            painter.setPen(xPen);
            const QPointF c = clearRect.center();
            constexpr qreal d = 3.5;
            painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
            painter.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
        }

        if (m_popupExpanded) {
            // Subtle pink hairline separating search header from expanded results panel
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(fullBounds.left() + 1.0, kBarHeight),
                             QPointF(fullBounds.right() - 1.0, kBarHeight));
        }
    }
}

void DesktopSearchBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    platform::windows::DesktopIntegration::applyDesktopWidgetStyles(hwnd);
    if (!m_isSearchActive && !m_barExpanded) {
        applyRestingWindowZOrder();
    }
    if (!m_hotkeyRegistered) {
        registerConfiguredGlobalShortcut();
    }
}

void DesktopSearchBar::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF localPos = event->position();

        if (m_barExpanded && !m_searchEdit->text().isEmpty() && clearButtonHitRect().contains(localPos)) {
            m_searchEdit->clear();
            m_searchEdit->setFocus();
            event->accept();
            return;
        }

        if (localPos.y() <= kBarHeight) {
            m_dragging = true;
            m_dragMoved = false;
            m_dragOffset = event->globalPosition().toPoint() - m_iconAnchorPos;
            event->accept();
            return;
        }
    }
    QWidget::mousePressEvent(event);
}

void DesktopSearchBar::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const QPoint newAnchor = clampPositionToScreens(
            event->globalPosition().toPoint() - m_dragOffset,
            kIconSize,
            kIconSize);
        if ((newAnchor - m_iconAnchorPos).manhattanLength() >= 4) {
            m_dragMoved = true;
            m_iconAnchorPos = newAnchor;
            m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);
            if (!m_barExpanded) {
                move(m_iconAnchorPos);
            } else if (m_popupExpanded) {
                updateExpandedGeometry();
            } else {
                applyOpenBarGeometry();
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
            m_iconAnchorPos = clampPositionToScreens(m_iconAnchorPos, kIconSize, kIconSize);
            m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);
            if (!m_barExpanded) {
                move(m_iconAnchorPos);
                applyRestingWindowZOrder();
            } else if (m_popupExpanded) {
                updateExpandedGeometry();
            } else {
                applyOpenBarGeometry();
            }
            core::SettingsManager::instance().setDesktopBarPosition(m_iconAnchorPos, true);
            emit positionPersisted(m_iconAnchorPos);
        } else {
            // Pure click (no drag)
            if (!m_barExpanded) {
                activateAndFocusSearch(true);
            } else if (iconTileRect().contains(event->position())) {
                collapsePopupAndDeactivate(true);
            } else {
                m_searchEdit->setFocus(Qt::MouseFocusReason);
            }
        }
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void DesktopSearchBar::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        collapsePopupAndDeactivate(true);
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
            // Important: clicking the search bar shows NOTHING underneath until user enters a query!
            if (!m_searchEdit->text().trimmed().isEmpty()) {
                setPopupExpanded(true);
            }
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
                    collapsePopupAndDeactivate(true);
                    return true;
                case Qt::Key_Tab:
                case Qt::Key_Backtab:
                    if (m_popupExpanded) {
                        m_popupWidget->cycleSearchMode(keyEvent->key() != Qt::Key_Backtab && !shift);
                        return true;
                    }
                    break;
                case Qt::Key_Down:
                    if (m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        m_popupWidget->selectNext();
                        return true;
                    }
                    break;
                case Qt::Key_Up:
                    if (m_popupExpanded && m_popupWidget->resultCount() > 0) {
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
            activateAndFocusSearch(true);
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
            if (!m_isSearchActive && !m_barExpanded) {
                applyRestingWindowZOrder();
            }
            emit explorerRestartDetected();
            return false;
        }

        if (msg->message == WM_HOTKEY &&
            static_cast<int>(msg->wParam) == platform::windows::DesktopIntegration::kDefaultHotkeyId) {
            if (m_barExpanded) {
                collapsePopupAndDeactivate(true);
            } else {
                activateAndFocusSearch(true);
            }
            if (result) {
                *result = 0;
            }
            return true;
        }

        if (msg->message == WM_WINDOWPOSCHANGING && !m_alwaysOnTop && !m_isSearchActive && !m_barExpanded && !m_keepAboveShellTemp) {
            auto* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
            if (wp != nullptr && (wp->flags & SWP_NOZORDER) == 0) {
                wp->hwndInsertAfter = HWND_BOTTOM;
            }
        }

        if (msg->message == WM_ACTIVATEAPP && msg->wParam == FALSE) {
            if (m_isSearchActive || m_barExpanded || m_popupExpanded) {
                collapsePopupAndDeactivate(true);
            }
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace lumen::ui
