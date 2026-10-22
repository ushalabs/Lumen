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
    m_searchEdit->setPlaceholderText(QStringLiteral("Search anything on your PC..."));
    m_searchEdit->setFrame(false);
    m_searchEdit->setGeometry(50, 10, kBarWidth - 50 - 105, 32);
    m_searchEdit->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    m_searchEdit->setAttribute(Qt::WA_MacShowFocusRect, false);
    m_searchEdit->setContextMenuPolicy(Qt::DefaultContextMenu);
    m_searchEdit->installEventFilter(this);

    m_popupWidget = new SearchPopupWidget(this);
    m_popupWidget->setGeometry(0, kBarHeight + kPopupGap, kBarWidth, kPopupHeight);
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
                updatePreviewForResult(*sel);
            }
        }
        updateExpandedGeometry();
    });

    connect(m_popupWidget, &SearchPopupWidget::selectedResultChanged,
            this, [this](const search::FileSearchResult& res) {
                if (m_popupWidget->isPreviewVisible()) {
                    updatePreviewForResult(res);
                }
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
        return QPoint(100, 100);
    }
    const QRect workArea = primary->availableGeometry();
    const int x = workArea.right() - kBarWidth - kRightEdgeMargin + 1;
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
    if (pref == QStringLiteral("dark")) {
        m_darkTheme = true;
    } else if (pref == QStringLiteral("system")) {
        m_darkTheme = (QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark);
    } else {
        m_darkTheme = false;
    }

    const auto theme = ThemeTokens::get(m_darkTheme);
    const QString textColor = theme.textPrimary.name();
    const QString placeholderColor = theme.textSecondary.name();
    const QString selectionBg = theme.brandPink.name();

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
    if (clearQuery && !m_searchEdit->text().isEmpty()) {
        m_searchEdit->clear();
    }
    if (m_popupWidget != nullptr) {
        m_popupWidget->setPreviewVisible(false);
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

void DesktopSearchBar::setPopupExpanded(bool expanded) {
    if (m_popupExpanded == expanded) {
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
        return;
    }
    m_popupExpanded = expanded;
    if (m_popupExpanded) {
        updateExpandedGeometry();
        m_popupWidget->show();
    } else {
        m_popupWidget->hide();
        setFixedSize(kBarWidth, kBarHeight);
    }
    update();
    emit popupVisibilityChanged(m_popupExpanded);
}

void DesktopSearchBar::updateExpandedGeometry() {
    if (!m_popupExpanded || m_popupWidget == nullptr) {
        return;
    }
    const int popH = m_popupWidget->desiredPopupHeight();
    const int totalH = kBarHeight + kPopupGap + popH;
    if (height() != totalH) {
        setFixedSize(kBarWidth, totalH);
    }
    m_popupWidget->setGeometry(0, kBarHeight + kPopupGap, kBarWidth, popH);
}

void DesktopSearchBar::updatePreviewForResult(const search::FileSearchResult& result) {
    if (!m_popupWidget || !m_popupWidget->isPreviewVisible()) {
        return;
    }
    const QString preview = m_searchEngine.fetchDocumentPreviewText(result.id, result.path, 500);
    m_popupWidget->setPreviewText(preview);
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
        m_popupWidget->clearResults();
        m_searchEdit->setGeometry(50, 10, kBarWidth - 50 - 105, 32);
        setPopupExpanded(false);
    } else {
        m_searchEdit->setGeometry(50, 10, kBarWidth - 50 - 46, 32);
        setPopupExpanded(true);
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
    if (settings.hasCustomPosition()) {
        const QPoint clamped = clampPositionToScreens(pos());
        if (clamped != pos()) {
            move(clamped);
        }
    } else {
        move(computeDefaultDesktopPosition());
    }
}

QRectF DesktopSearchBar::clearButtonHitRect() const {
    return QRectF(kBarWidth - 38.0, 14.0, 24.0, 24.0);
}

void DesktopSearchBar::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const auto theme = ThemeTokens::get(m_darkTheme);
    const QRectF barBounds(1.0, 1.0, kBarWidth - 2.0, kBarHeight - 2.0);
    const qreal radius = 14.0;

    // Soft elevated shadow
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.shadowColor);
    painter.drawRoundedRect(barBounds.adjusted(0.0, 2.0, 0.0, 2.0), radius, radius);

    // Surface and interactive focus border
    const bool hasFocus = m_isSearchActive || m_searchEdit->hasFocus();
    const QColor borderColor = hasFocus ? theme.brandPink : theme.borderColor;
    QPen borderPen(borderColor, hasFocus ? 1.6 : 1.0);
    painter.setPen(borderPen);
    painter.setBrush(theme.surfaceBackground);
    painter.drawRoundedRect(barBounds.adjusted(0.0, 0.0, 0.0, -1.0), radius, radius);

    // Left Lumen mark in Downshift pink gradient
    const QRectF iconRect(14.0, 14.0, 24.0, 24.0);
    BrandingIcon::paintLumenMark(&painter, iconRect, m_darkTheme);

    if (!m_searchEdit->text().isEmpty()) {
        const QRectF clearRect = clearButtonHitRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.brandPinkTint);
        painter.drawEllipse(clearRect);

        QPen xPen(theme.brandPink, 1.6);
        xPen.setCapStyle(Qt::RoundCap);
        painter.setPen(xPen);
        const QPointF c = clearRect.center();
        constexpr qreal d = 4.0;
        painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
        painter.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
    } else {
        const QRectF pillRect(kBarWidth - 100.0, 14.0, 86.0, 24.0);
        painter.setPen(QPen(theme.borderColor, 1.0));
        painter.setBrush(theme.elevatedBackground);
        painter.drawRoundedRect(pillRect, 7.0, 7.0);

        QFont pillFont(QStringLiteral("Segoe UI"), 8, QFont::Medium);
        painter.setFont(pillFont);
        painter.setPen(theme.textSecondary);
        painter.drawText(pillRect, Qt::AlignCenter, core::SettingsManager::instance().globalShortcut());
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

        if (!m_searchEdit->text().isEmpty() && clearButtonHitRect().contains(localPos)) {
            m_searchEdit->clear();
            m_searchEdit->setFocus();
            event->accept();
            return;
        }

        if (localPos.y() <= kBarHeight) {
            m_dragging = true;
            m_dragMoved = false;
            m_dragOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
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
        const QPoint newPos = clampPositionToScreens(event->globalPosition().toPoint() - m_dragOffset);
        if ((newPos - pos()).manhattanLength() >= 2) {
            m_dragMoved = true;
            move(newPos);
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
            const QPoint finalPos = clampPositionToScreens(pos());
            move(finalPos);
            core::SettingsManager::instance().setDesktopBarPosition(finalPos, true);
            emit positionPersisted(finalPos);
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
                    if (ctrl && shift && m_popupExpanded && m_popupWidget->resultCount() > 0) {
                        if (const auto* sel = m_popupWidget->selectedResult()) {
                            copyResultPathToClipboard(*sel);
                            return true;
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
            core::Logger::info(QStringLiteral("Desktop"),
                               QStringLiteral("Windows Explorer taskbar recreated; restoring desktop widget styles."));
            const HWND hwnd = reinterpret_cast<HWND>(winId());
            platform::windows::DesktopIntegration::applyDesktopWidgetStyles(hwnd);
            if (!m_isSearchActive) {
                applyRestingWindowZOrder();
            }
            emit explorerRestartDetected();
            return false;
        }

        switch (msg->message) {
            case WM_MOUSEACTIVATE: {
                m_isSearchActive = true;
                if (result) {
                    *result = MA_ACTIVATE;
                }
                return true;
            }
            case WM_ACTIVATE: {
                const WORD state = LOWORD(msg->wParam);
                if (state == WA_INACTIVE) {
                    collapsePopupAndDeactivate(false);
                } else {
                    m_isSearchActive = true;
                    update();
                }
                break;
            }
            case WM_WINDOWPOSCHANGING: {
                WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(msg->lParam);
                if (wp != nullptr && !m_isSearchActive && !m_keepAboveShellTemp && !m_alwaysOnTop) {
                    wp->hwndInsertAfter = HWND_BOTTOM;
                }
                break;
            }
            case WM_SYSCOMMAND: {
                if ((msg->wParam & 0xFFF0) == SC_MINIMIZE) {
                    if (result) {
                        *result = 0;
                    }
                    return true;
                }
                break;
            }
            case WM_HOTKEY: {
                if (static_cast<int>(msg->wParam) == platform::windows::DesktopIntegration::kDefaultHotkeyId) {
                    if (isVisible() && m_isSearchActive && m_searchEdit->hasFocus()) {
                        collapsePopupAndDeactivate(false);
                    } else {
                        activateAndFocusSearch();
                    }
                    if (result) {
                        *result = 0;
                    }
                    return true;
                }
                break;
            }
            case WM_DPICHANGED: {
                onScreenConfigurationChanged();
                break;
            }
            default:
                break;
        }
    }
    return QWidget::nativeEvent(eventType, message, result);
}

} // namespace lumen::ui
