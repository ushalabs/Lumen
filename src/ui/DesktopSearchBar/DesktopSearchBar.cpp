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
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointer>
#include <QRegion>
#include <QResizeEvent>
#include <QRunnable>
#include <QScreen>
#include <QShowEvent>
#include <QSignalBlocker>
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
    setMouseTracking(true);
    setFocusPolicy(Qt::ClickFocus);
    setFixedSize(kIconSize, kIconSize);
    setWindowTitle(QStringLiteral("Lumen Desktop Search"));

    m_searchPool.setMaxThreadCount(1);
    m_searchPool.setExpiryTimeout(5000);

    m_slideAnim = new QVariantAnimation(this);
    m_slideAnim->setDuration(145);
    m_slideAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_slideAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_slideProgress = value.toReal();
        if (m_barExpanded && !m_popupExpanded) {
            applyOpenBarGeometry();
        }
        update();
    });
    connect(m_slideAnim, &QVariantAnimation::finished, this, [this]() {
        if (m_closingBar) {
            finishBarCollapse();
        } else if (m_barExpanded && !m_popupExpanded) {
            m_slideProgress = 1.0;
            applyOpenBarGeometry();
            update();
        }
    });

    m_popupAnim = new QVariantAnimation(this);
    m_popupAnim->setDuration(140);
    m_popupAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_popupAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_popupProgress = value.toReal();
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
        update();
    });
    connect(m_popupAnim, &QVariantAnimation::finished, this, [this]() {
        if (m_closingPopup) {
            m_closingPopup = false;
            m_popupExpanded = false;
            m_popupProgress = 0.0;
            m_detailsAnim->stop();
            m_detailsProgress = 0.0;
            m_detailsExpanded = false;
            m_popupWidget->clearMask();
            m_popupWidget->hide();
            m_popupWidget->clearInspectorSnapshot();
            m_popupWidget->clearResults();
            emit popupVisibilityChanged(false);

            if (m_closeBarAfterPopup) {
                m_closeBarAfterPopup = false;
                startBarSlideClose();
            } else {
                m_slideProgress = 1.0;
                applyOpenBarGeometry();
                update();
            }
        } else if (m_popupExpanded) {
            m_popupProgress = 1.0;
            updateExpandedGeometry();
        }
    });

    m_detailsAnim = new QVariantAnimation(this);
    m_detailsAnim->setDuration(150);
    m_detailsAnim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_detailsAnim, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
        m_detailsProgress = value.toReal();
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
        update();
    });
    connect(m_detailsAnim, &QVariantAnimation::finished, this, [this]() {
        m_detailsProgress = m_detailsExpanded ? 1.0 : 0.0;
        if (!m_detailsExpanded && m_popupWidget != nullptr) {
            m_popupWidget->clearInspectorSnapshot();
        }
        if (m_popupExpanded) {
            updateExpandedGeometry();
        }
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
    m_popupWidget->setGeometry(kShadowLeft, kShadowTop + kBarCardHeight, kBarWidth - kHorizontalShadow, kPopupHeight);
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

    connect(m_popupWidget, &SearchPopupWidget::selectionActiveChanged,
            this, [this](bool hasSel) {
                setDetailsExpanded(hasSel);
            });

    connect(m_popupWidget, &SearchPopupWidget::revealInExplorerRequested,
            this, &DesktopSearchBar::revealResultInExplorer);

    connect(m_popupWidget, &SearchPopupWidget::copyFileRequested,
            this, &DesktopSearchBar::copyResultFileToClipboard);

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
            } else if (!m_isSearchActive && !m_barExpanded && !m_closingBar && !m_closingPopup) {
                m_keepAboveShellTemp = false;
                platform::windows::DesktopIntegration::placeOnDesktopBottom(myHwnd);
            }
        });

    platform::windows::DesktopIntegration::applyDesktopWidgetStyles(reinterpret_cast<HWND>(winId()));
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
        return QPoint(1800, 44);
    }
    const QRect workArea = primary->availableGeometry();
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
    const int minX = workArea.left() + 4;
    const int maxX = qMax(minX, workArea.right() - widgetW - 4);
    const int minY = workArea.top() + 4;
    const int maxY = qMax(minY, workArea.bottom() - widgetH - 4);

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

    m_searchEdit->setFont(ThemeTokens::makeFont(14, QFont::Normal));
    m_searchEdit->setStyleSheet(QStringLiteral(
        "QLineEdit {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  selection-background-color: #FCE7F3;"
        "  selection-color: %1;"
        "}"
        "QLineEdit[ placeholderText ] {"
        "  color: %2;"
        "}")
        .arg(textColor, placeholderColor));

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

    const bool wasClosing = m_closingBar || m_closingPopup;
    m_closingBar = false;
    m_closingPopup = false;
    m_closeBarAfterPopup = false;
    m_clearQueryOnClose = false;

    m_isSearchActive = true;
    m_slidesLeft = shouldSlideLeftFromAnchor(m_iconAnchorPos);

    const bool wasExpanded = m_barExpanded && !wasClosing;
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
    } else if ((!wasExpanded || wasClosing) && animateSlide) {
        m_slideAnim->stop();
        if (!wasClosing) {
            m_slideProgress = 0.0;
        }
        m_slideAnim->setDuration(145);
        m_slideAnim->setStartValue(m_slideProgress);
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

void DesktopSearchBar::startBarSlideClose() {
    m_closingBar = true;
    m_barExpanded = true;
    m_searchEdit->deselect();
    m_searchEdit->clearFocus();

    m_slideAnim->stop();
    m_slideAnim->setDuration(135);
    m_slideAnim->setStartValue(m_slideProgress);
    m_slideAnim->setEndValue(0.0);
    applyOpenBarGeometry();
    m_slideAnim->start();
    update();
}

void DesktopSearchBar::finishBarCollapse() {
    m_closingBar = false;
    m_closingPopup = false;
    m_closeBarAfterPopup = false;
    m_popupExpanded = false;
    m_detailsExpanded = false;
    m_barExpanded = false;
    m_isSearchActive = false;
    m_clearHovered = false;
    m_slideProgress = 0.0;
    m_popupProgress = 0.0;
    m_detailsProgress = 0.0;

    if (m_clearQueryOnClose && m_searchEdit != nullptr && !m_searchEdit->text().isEmpty()) {
        const QSignalBlocker blocker(m_searchEdit);
        m_searchEdit->clear();
        if (m_popupWidget != nullptr) {
            m_popupWidget->setQueryText(QString());
        }
    }
    m_clearQueryOnClose = false;

    if (m_popupWidget != nullptr) {
        m_popupWidget->setPreviewVisible(false);
        m_popupWidget->setPreviewText(QString());
        m_popupWidget->setRelatedSourceFileName(QString());
        m_popupWidget->clearMask();
        m_popupWidget->hide();
        m_popupWidget->clearInspectorSnapshot();
        m_popupWidget->clearResults();
    }

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

void DesktopSearchBar::collapsePopupAndDeactivate(bool clearQuery, bool animateSlide) {
    if (m_activeCancelToken) {
        m_activeCancelToken->store(true, std::memory_order_relaxed);
    }
    ++m_previewGeneration;

    if (!animateSlide) {
        m_slideAnim->stop();
        if (m_popupAnim != nullptr) {
            m_popupAnim->stop();
        }
        if (m_detailsAnim != nullptr) {
            m_detailsAnim->stop();
        }
        m_clearQueryOnClose = clearQuery;
        finishBarCollapse();
        return;
    }

    if (!m_barExpanded && !m_popupExpanded && !m_closingBar && !m_closingPopup) {
        return;
    }
    if (m_closingBar && !m_popupExpanded) {
        if (clearQuery) {
            m_clearQueryOnClose = true;
        }
        return;
    }

    m_clearQueryOnClose = clearQuery;
    m_isSearchActive = false;
    m_clearHovered = false;

    if (m_popupExpanded) {
        m_closeBarAfterPopup = true;
        setPopupExpanded(false);
        return;
    }

    startBarSlideClose();
}

QString DesktopSearchBar::searchText() const {
    return m_searchEdit ? m_searchEdit->text() : QString();
}

void DesktopSearchBar::setSearchText(const QString& text) {
    if (m_searchEdit != nullptr) {
        m_searchEdit->setText(text);
    }
}

QRectF DesktopSearchBar::cardRect() const {
    return QRectF(rect()).adjusted(kShadowLeft, kShadowTop, -kShadowRight, -kShadowBottom);
}

QRectF DesktopSearchBar::iconTileRect() const {
    const QRectF c = cardRect();
    // Pure pink Lumen emblem fills the entire 56x56 left cap with zero outer white padding or border
    return QRectF(c.left(), c.top(), kBarCardHeight, kBarCardHeight);
}

QRectF DesktopSearchBar::clearButtonHitRect() const {
    const QRectF c = cardRect();
    return QRectF(c.right() - 36.0, c.top() + 17.0, 22.0, 22.0);
}

void DesktopSearchBar::updateSearchInputGeometry() {
    if (!m_searchEdit) {
        return;
    }
    if (!m_barExpanded) {
        m_searchEdit->hide();
        return;
    }
    const QRectF c = cardRect();
    const bool hasText = !m_searchEdit->text().trimmed().isEmpty();
    const int leftOffset = static_cast<int>(c.left()) + kBarCardHeight + 14;
    const int rightReserve = hasText ? 44 : 18;
    const int editWidth = std::max(0, static_cast<int>(c.width()) - (kBarCardHeight + 14) - rightReserve);
    if (editWidth < 40) {
        m_searchEdit->setGeometry(leftOffset, static_cast<int>(c.top()) + 11, 0, 34);
    } else {
        m_searchEdit->show();
        m_searchEdit->setGeometry(leftOffset, static_cast<int>(c.top()) + 11, editWidth, 34);
    }
}

void DesktopSearchBar::syncNativeBackdropBlur() {
    if (!testAttribute(Qt::WA_WState_Created)) {
        return;
    }
    const qreal dpr = devicePixelRatioF();
    const QRectF c = cardRect();
    platform::windows::DesktopIntegration::updateRoundedBackdropBlur(
        reinterpret_cast<HWND>(winId()),
        qRound(c.left() * dpr),
        qRound(c.top() * dpr),
        qRound(c.right() * dpr),
        qRound(c.bottom() * dpr),
        qRound(16.0 * dpr));
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
        targetW = std::min(kBarWidth, std::max(kIconSize, workArea.width() - 16));
    }

    const int curW = std::max(kIconSize, static_cast<int>(qRound(kIconSize + (targetW - kIconSize) * m_slideProgress)));
    int targetX = m_iconAnchorPos.x();
    if (m_slidesLeft) {
        const int anchorRight = m_iconAnchorPos.x() + kIconSize;
        targetX = qMax(workArea.left() + 4, anchorRight - curW);
    } else {
        targetX = qMin(workArea.right() - curW - 4, m_iconAnchorPos.x());
    }

    if (width() != curW || height() != kBarHeight) {
        setFixedSize(curW, kBarHeight);
    }
    if (pos().x() != targetX || pos().y() != m_iconAnchorPos.y()) {
        move(targetX, m_iconAnchorPos.y());
    }
    updateSearchInputGeometry();
}

void DesktopSearchBar::setDetailsExpanded(bool expanded) {
    if (m_detailsExpanded == expanded &&
        ((expanded && m_detailsProgress >= 0.999) || (!expanded && m_detailsProgress <= 0.001))) {
        return;
    }
    m_detailsExpanded = expanded;
    if (!m_popupExpanded) {
        m_detailsAnim->stop();
        m_detailsProgress = 0.0;
        return;
    }
    m_detailsAnim->stop();
    m_detailsAnim->setDuration(150);
    m_detailsAnim->setStartValue(m_detailsProgress);
    m_detailsAnim->setEndValue(expanded ? 1.0 : 0.0);
    m_detailsAnim->start();
}

void DesktopSearchBar::setPopupExpanded(bool expanded) {
    if (expanded) {
        const bool needOpen = !m_popupExpanded || m_closingPopup;
        m_closingPopup = false;
        m_closingBar = false;
        m_closeBarAfterPopup = false;
        m_slideAnim->stop();
        m_slideProgress = 1.0;
        m_barExpanded = true;
        m_popupExpanded = true;
        m_searchEdit->show();

        if (needOpen) {
            m_popupAnim->stop();
            m_popupAnim->setDuration(140);
            m_popupAnim->setStartValue(qBound(0.0, m_popupProgress, 1.0));
            m_popupAnim->setEndValue(1.0);
            m_popupAnim->start();
            updateExpandedGeometry();
            m_popupWidget->show();
        } else {
            updateExpandedGeometry();
        }
    } else {
        if (!m_popupExpanded || m_closingPopup) {
            return;
        }
        m_closingPopup = true;
        if (m_detailsExpanded || m_detailsProgress > 0.001) {
            m_detailsExpanded = false;
            m_detailsAnim->stop();
            m_detailsAnim->setDuration(125);
            m_detailsAnim->setStartValue(m_detailsProgress);
            m_detailsAnim->setEndValue(0.0);
            m_detailsAnim->start();
        }
        m_popupAnim->stop();
        m_popupAnim->setDuration(130);
        m_popupAnim->setStartValue(qBound(0.0, m_popupProgress, 1.0));
        m_popupAnim->setEndValue(0.0);
        m_popupAnim->start();
    }
    update();
    if (!m_closingPopup) {
        emit popupVisibilityChanged(m_popupExpanded);
    }
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
    int startW = kBarWidth;
    int fullW = kPopupWidth;
    if (screen != nullptr) {
        workArea = screen->availableGeometry();
        startW = std::min(kBarWidth, std::max(kIconSize, workArea.width() - 16));
        fullW = std::min(kPopupWidth, std::max(startW, workArea.width() - 16));
    }

    const int targetPopH = m_popupWidget->desiredPopupHeight();
    const int targetTotalH = kBarHeight + kPopupGap + targetPopH;
    const qreal popP = qBound(0.0, m_popupProgress, 1.0);
    const qreal detP = qBound(0.0, m_detailsProgress, 1.0);

    const int curW = std::max(startW, startW + static_cast<int>(qRound((fullW - startW) * detP)));
    const int curTotalH = std::max(kBarHeight, kBarHeight + static_cast<int>(qRound((targetTotalH - kBarHeight) * popP)));
    const int curPopH = std::max(0, curTotalH - kBarHeight - kPopupGap);

    int targetWinX = m_iconAnchorPos.x();
    if (m_slidesLeft) {
        const int anchorRight = m_iconAnchorPos.x() + kIconSize;
        targetWinX = anchorRight - curW;
    }
    const int minX = workArea.left() + 4;
    const int maxX = qMax(minX, workArea.right() - curW - 4);
    targetWinX = qBound(minX, targetWinX, maxX);

    int targetWinY = m_iconAnchorPos.y();
    if (targetWinY + curTotalH > workArea.bottom() - 4) {
        targetWinY = qMax(workArea.top() + 4, workArea.bottom() - curTotalH - 4);
    }

    if (width() != curW || height() != curTotalH) {
        setFixedSize(curW, curTotalH);
    }
    if (pos().x() != targetWinX || pos().y() != targetWinY) {
        move(targetWinX, targetWinY);
    }

    updateSearchInputGeometry();

    const int visibleInnerW = std::max(0, curW - kHorizontalShadow);
    m_popupWidget->setGeometry(kShadowLeft,
                               kShadowTop + kBarCardHeight + kPopupGap,
                               visibleInnerW,
                               targetPopH);

    if (curPopH < targetPopH) {
        if (visibleInnerW > 0 && curPopH > 0) {
            m_popupWidget->setMask(QRegion(0, 0, visibleInnerW, curPopH));
        } else {
            m_popupWidget->setMask(QRegion(0, 0, 1, 1));
        }
    } else {
        m_popupWidget->clearMask();
    }
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
        m_closeBarAfterPopup = false;
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

void DesktopSearchBar::copyResultFileToClipboard(const search::FileSearchResult& result) {
    if (result.path.isEmpty()) {
        return;
    }
    const QString nativePath = QDir::toNativeSeparators(result.path);
    if (!QFileInfo::exists(nativePath)) {
        return;
    }
    const HWND owner = testAttribute(Qt::WA_WState_Created) ? reinterpret_cast<HWND>(winId()) : nullptr;
    if (platform::windows::DesktopIntegration::copyFileOrFolderToClipboard(nativePath, owner)) {
        if (m_popupWidget != nullptr) {
            m_popupWidget->showFileCopiedFeedback();
        }
        emit fileCopiedToClipboard(nativePath);
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
    const QRectF cRect = cardRect();
    constexpr qreal radius = 16.0;

    // 1. Multi-layer soft ambient drop shadow (0 6px 22px rgba(0, 0, 0, 0.06))
    static const struct ShadowStep {
        qreal spreadX;
        qreal spreadBottom;
        qreal dy;
        int alpha;
    } kShadowSteps[5] = {
        {7.0, 9.0, 5.5, 4},
        {5.5, 7.0, 4.2, 6},
        {4.0, 5.5, 3.0, 8},
        {2.5, 4.0, 2.0, 10},
        {1.0, 2.5, 1.0, 12}
    };
    painter.setPen(Qt::NoPen);
    for (const auto& step : kShadowSteps) {
        painter.setBrush(QColor(0, 0, 0, step.alpha));
        const QRectF sRect = cRect.adjusted(-step.spreadX, -step.spreadX * 0.45 + step.dy,
                                            step.spreadX, step.spreadBottom);
        painter.drawRoundedRect(sRect, radius + step.spreadX * 0.65, radius + step.spreadX * 0.65);
    }

    const bool searchFocused = m_barExpanded && (m_isSearchActive || m_searchEdit->hasFocus());
    const qreal pinkW = kBarCardHeight;
    const QRectF pinkRect(cRect.left(), cRect.top(), pinkW, kBarCardHeight);

    if (!m_barExpanded) {
        // Collapsed state: pure pink Lumen icon only (no white margin, no outer border)
        BrandingIcon::paintLumenMark(&painter, cRect, radius, radius, radius, radius);
        return;
    }

    const QColor cardBorder = (!m_popupExpanded && searchFocused)
                                  ? QColor(236, 22, 128, 175)
                                  : theme.borderColor;
    const qreal seamX = cRect.left() + pinkW;

    // 2. White search bar + optional results workspace cleanly attached to the right edge of the pink Lumen icon
    const qreal dividerY = cRect.top() + kBarCardHeight;
    const qreal popSpan = qMax(0.0, cRect.bottom() - dividerY);
    if (!m_popupExpanded || popSpan <= 3.0) {
        if (cRect.width() > pinkW + 2.0) {
            const qreal fillLeft = qMax(cRect.left() + 4.0, seamX - 6.0);
            const qreal rightEdgeR = qMin(radius, cRect.bottom() - cRect.top());
            QPainterPath fillPath;
            fillPath.moveTo(fillLeft, cRect.top());
            fillPath.lineTo(cRect.right() - radius, cRect.top());
            fillPath.arcTo(QRectF(cRect.right() - 2.0 * radius, cRect.top(), 2.0 * radius, 2.0 * radius), 90.0, -90.0);
            fillPath.lineTo(cRect.right(), dividerY - rightEdgeR * 0.5);
            fillPath.arcTo(QRectF(cRect.right() - 2.0 * radius, dividerY - 2.0 * radius, 2.0 * radius, 2.0 * radius), 0.0, -90.0);
            fillPath.lineTo(fillLeft, dividerY);
            fillPath.closeSubpath();

            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.surfaceBackground);
            painter.drawPath(fillPath);

            // Stroke border strictly on the white search bar (never wrapping around the pink Lumen side)
            QPainterPath whiteOutline;
            whiteOutline.moveTo(seamX, cRect.top());
            whiteOutline.lineTo(cRect.right() - radius, cRect.top());
            whiteOutline.arcTo(QRectF(cRect.right() - 2.0 * radius, cRect.top(), 2.0 * radius, 2.0 * radius), 90.0, -90.0);
            whiteOutline.lineTo(cRect.right(), dividerY - radius);
            whiteOutline.arcTo(QRectF(cRect.right() - 2.0 * radius, dividerY - 2.0 * radius, 2.0 * radius, 2.0 * radius), 0.0, -90.0);
            whiteOutline.lineTo(seamX, dividerY);

            if (searchFocused && !m_popupExpanded) {
                painter.save();
                painter.setClipRect(QRectF(seamX, 0.0, width() - seamX, height()));
                QPen glowPen(QColor(236, 22, 128, 34), 4.0);
                glowPen.setCapStyle(Qt::FlatCap);
                painter.setPen(glowPen);
                painter.setBrush(Qt::NoBrush);
                painter.drawPath(whiteOutline);
                painter.restore();
            }
            QPen borderPen(cardBorder, 1.0);
            borderPen.setCapStyle(Qt::FlatCap);
            painter.setPen(borderPen);
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(whiteOutline);
        }

        // Paint pure pink Lumen badge cleanly on top with smooth right-corner morph during slide in/out
        const qreal rightCapR = (cRect.width() <= pinkW + 2.0)
                                    ? radius
                                    : qMax(0.0, radius * (1.0 - qMin(1.0, (cRect.width() - pinkW) / 14.0)));
        BrandingIcon::paintLumenMark(&painter, pinkRect, radius, rightCapR, rightCapR, radius);
    } else {
        const qreal bottomR = qMin(radius, popSpan * 0.5);

        QPainterPath whiteFillPath;
        whiteFillPath.moveTo(seamX - 4.0, cRect.top());
        whiteFillPath.lineTo(cRect.right() - radius, cRect.top());
        whiteFillPath.arcTo(QRectF(cRect.right() - 2.0 * radius, cRect.top(), 2.0 * radius, 2.0 * radius), 90.0, -90.0);
        whiteFillPath.lineTo(cRect.right(), cRect.bottom() - bottomR);
        whiteFillPath.arcTo(QRectF(cRect.right() - 2.0 * bottomR, cRect.bottom() - 2.0 * bottomR, 2.0 * bottomR, 2.0 * bottomR), 0.0, -90.0);
        whiteFillPath.lineTo(cRect.left() + bottomR, cRect.bottom());
        whiteFillPath.arcTo(QRectF(cRect.left(), cRect.bottom() - 2.0 * bottomR, 2.0 * bottomR, 2.0 * bottomR), 270.0, -90.0);
        whiteFillPath.lineTo(cRect.left(), dividerY - 4.0);
        whiteFillPath.lineTo(seamX - 4.0, dividerY - 4.0);
        whiteFillPath.closeSubpath();

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.surfaceBackground);
        painter.drawPath(whiteFillPath);

        QPainterPath whiteOutline;
        whiteOutline.moveTo(seamX, cRect.top());
        whiteOutline.lineTo(cRect.right() - radius, cRect.top());
        whiteOutline.arcTo(QRectF(cRect.right() - 2.0 * radius, cRect.top(), 2.0 * radius, 2.0 * radius), 90.0, -90.0);
        whiteOutline.lineTo(cRect.right(), cRect.bottom() - bottomR);
        whiteOutline.arcTo(QRectF(cRect.right() - 2.0 * bottomR, cRect.bottom() - 2.0 * bottomR, 2.0 * bottomR, 2.0 * bottomR), 0.0, -90.0);
        whiteOutline.lineTo(cRect.left() + bottomR, cRect.bottom());
        whiteOutline.arcTo(QRectF(cRect.left(), cRect.bottom() - 2.0 * bottomR, 2.0 * bottomR, 2.0 * bottomR), 270.0, -90.0);
        whiteOutline.lineTo(cRect.left(), dividerY);

        painter.setPen(QPen(cardBorder, 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(whiteOutline);

        // Paint pure pink Lumen badge cleanly in top-left corner with zero border
        BrandingIcon::paintLumenMark(&painter, pinkRect, radius, 0.0, 0.0, 0.0);
    }

    // 4. Expanded bar clear button & neutral hairline divider + active search focus indicator
    if (m_barExpanded) {
        if (!m_searchEdit->text().isEmpty() && cRect.width() > 140.0) {
            const QRectF clearRect = clearButtonHitRect();
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_clearHovered ? QColor(236, 22, 128, 24) : theme.elevatedBackground);
            painter.drawEllipse(clearRect);

            QPen xPen(m_clearHovered ? theme.brandPink : theme.textSecondary, 1.45);
            xPen.setCapStyle(Qt::RoundCap);
            painter.setPen(xPen);
            const QPointF c = clearRect.center();
            constexpr qreal d = 3.2;
            painter.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
            painter.drawLine(QPointF(c.x() + d, c.y() - d), QPointF(c.x() - d, c.y() + d));
        }

        if (m_popupExpanded && popSpan > 3.0) {
            // Neutral #EFEFEF divider across top search row starting at the white search section
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(seamX, dividerY),
                             QPointF(cRect.right() - 1.0, dividerY));

            // Dynamic 2px soft pink focus accent fading smoothly across active search input area
            if (searchFocused) {
                const qreal lineLeft = seamX + 14.0;
                const qreal lineRight = cRect.right() - 56.0;
                if (lineRight > lineLeft + 20.0) {
                    QLinearGradient glowGrad(lineLeft, dividerY, lineRight, dividerY);
                    glowGrad.setColorAt(0.0, QColor(236, 22, 128, 42));
                    glowGrad.setColorAt(0.55, QColor(236, 22, 128, 20));
                    glowGrad.setColorAt(1.0, QColor(236, 22, 128, 0));
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(glowGrad);
                    painter.drawRoundedRect(
                        QRectF(lineLeft, dividerY - 1.5, lineRight - lineLeft, 3.0), 1.5, 1.5);

                    QLinearGradient lineGrad(lineLeft, dividerY, lineRight, dividerY);
                    lineGrad.setColorAt(0.0, QColor(236, 22, 128, 170));
                    lineGrad.setColorAt(0.55, QColor(236, 22, 128, 80));
                    lineGrad.setColorAt(1.0, QColor(236, 22, 128, 0));
                    painter.setBrush(lineGrad);
                    painter.drawRoundedRect(
                        QRectF(lineLeft, dividerY - 0.8, lineRight - lineLeft, 1.6), 0.8, 0.8);
                }
            }
        }
    }
}

void DesktopSearchBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    syncNativeBackdropBlur();
}

void DesktopSearchBar::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    const HWND hwnd = reinterpret_cast<HWND>(winId());
    platform::windows::DesktopIntegration::applyDesktopWidgetStyles(hwnd);
    syncNativeBackdropBlur();
    if (!m_isSearchActive && !m_barExpanded) {
        applyRestingWindowZOrder();
    }
    if (!m_hotkeyRegistered) {
        registerConfiguredGlobalShortcut();
    }
}

void DesktopSearchBar::leaveEvent(QEvent* event) {
    if (m_clearHovered || m_iconHovered) {
        m_clearHovered = false;
        m_iconHovered = false;
        update();
    }
    QWidget::leaveEvent(event);
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

        if (localPos.y() <= kShadowTop + kBarCardHeight) {
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

    const QPointF pos = event->position();
    const bool clearHov = m_barExpanded && !m_searchEdit->text().isEmpty() && clearButtonHitRect().contains(pos);
    const bool iconHov = !m_barExpanded && cardRect().contains(pos);
    if (clearHov != m_clearHovered || iconHov != m_iconHovered) {
        m_clearHovered = clearHov;
        m_iconHovered = iconHov;
        update();
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
            if (!m_barExpanded || m_closingBar) {
                activateAndFocusSearch(true);
            } else if (iconTileRect().contains(event->position())) {
                collapsePopupAndDeactivate(true);
            } else {
                if (m_popupExpanded && m_popupWidget && m_popupWidget->hasSelection()) {
                    m_popupWidget->clearResultSelection();
                }
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
            if (!m_searchEdit->text().trimmed().isEmpty()) {
                setPopupExpanded(true);
            }
            if (m_popupExpanded && m_popupWidget && m_popupWidget->hasSelection()) {
                m_popupWidget->clearResultSelection();
            }
            update();
        } else if (event->type() == QEvent::FocusIn) {
            m_isSearchActive = true;
            if (!m_searchEdit->text().trimmed().isEmpty()) {
                setPopupExpanded(true);
            }
            update();
        } else if (event->type() == QEvent::FocusOut) {
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
                        if (m_popupWidget->activateSelected()) {
                            return true;
                        }
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
                        if (const auto* sel = m_popupWidget->selectedResult()) {
                            if (shift) {
                                copyResultPathToClipboard(*sel);
                                return true;
                            }
                            if (!m_searchEdit->hasSelectedText()) {
                                copyResultFileToClipboard(*sel);
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
            syncNativeBackdropBlur();
            if (!m_isSearchActive && !m_barExpanded) {
                applyRestingWindowZOrder();
            }
            emit explorerRestartDetected();
            return false;
        }

        if (msg->message == WM_HOTKEY &&
            static_cast<int>(msg->wParam) == platform::windows::DesktopIntegration::kDefaultHotkeyId) {
            if (m_barExpanded && !m_closingBar) {
                collapsePopupAndDeactivate(true);
            } else {
                activateAndFocusSearch(true);
            }
            if (result) {
                *result = 0;
            }
            return true;
        }

        if (msg->message == WM_WINDOWPOSCHANGING && !m_alwaysOnTop && !m_isSearchActive && !m_barExpanded &&
            !m_closingBar && !m_closingPopup && !m_keepAboveShellTemp) {
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
