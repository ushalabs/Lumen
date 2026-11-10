#pragma once

#include "search/FileSearchEngine.h"

#include <QLineEdit>
#include <QPoint>
#include <QRectF>
#include <QThreadPool>
#include <QVariantAnimation>
#include <QWidget>
#include <atomic>
#include <memory>

namespace lumen::ui {

class SearchPopupWidget;

class DesktopSearchBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int kShadowLeft = 9;
    static constexpr int kShadowTop = 5;
    static constexpr int kShadowRight = 9;
    static constexpr int kShadowBottom = 13;
    static constexpr int kHorizontalShadow = kShadowLeft + kShadowRight;
    static constexpr int kVerticalShadow = kShadowTop + kShadowBottom;

    static constexpr int kBarCardHeight = 56;
    static constexpr int kIconSize = 54 + kHorizontalShadow;          // 72px widget -> 54x54 visible card
    static constexpr int kBarWidth = 464 + kHorizontalShadow;         // 482px widget -> 464px visible bar
    static constexpr int kBarHeight = kBarCardHeight + kVerticalShadow; // 74px widget -> 56px visible bar
    static constexpr int kPopupWidth = 900 + kHorizontalShadow;       // 918px widget -> 900px visible workspace
    static constexpr int kPopupGap = 0;
    static constexpr int kPopupHeight = 140;
    static constexpr int kExpandedHeight = kBarHeight + kPopupGap + kPopupHeight;
    static constexpr int kRightEdgeMargin = 20;
    static constexpr int kTopWorkAreaOffset = 44;

    explicit DesktopSearchBar(QWidget* parent = nullptr);
    ~DesktopSearchBar() override;

    static QPoint computeDefaultDesktopPosition();
    static QPoint clampPositionToScreens(const QPoint& candidatePos,
                                         int widgetW = kIconSize,
                                         int widgetH = kIconSize);
    static bool shouldSlideLeftFromAnchor(const QPoint& anchorPos);

    void applySavedOrDefaultPosition();
    void activateAndFocusSearch(bool animateSlide = true);
    void collapsePopupAndDeactivate(bool clearQuery = false);

    bool isBarExpanded() const noexcept { return m_barExpanded; }
    bool isPopupExpanded() const noexcept { return m_popupExpanded; }
    bool slidesLeft() const noexcept { return m_slidesLeft; }
    QPoint iconAnchorPosition() const noexcept { return m_iconAnchorPos; }

    bool isGlobalShortcutRegistered() const noexcept { return m_hotkeyRegistered; }
    QString globalShortcutError() const { return m_hotkeyError; }

    QString searchText() const;
    void setSearchText(const QString& text);

    void setOpenFilesWithShell(bool enable) noexcept { m_openFilesWithShell = enable; }
    bool openFilesWithShell() const noexcept { return m_openFilesWithShell; }

    QLineEdit* lineEdit() const noexcept { return m_searchEdit; }
    SearchPopupWidget* popupWidget() const noexcept { return m_popupWidget; }

    void refreshAppearanceFromSettings();
    bool registerConfiguredGlobalShortcut();
    void notifyIndexUpdated();

    void revealResultInExplorer(const search::FileSearchResult& result);
    void copyResultPathToClipboard(const search::FileSearchResult& result);
    void triggerFindRelatedForSelected();

signals:
    void explorerRestartDetected();
    void positionPersisted(const QPoint& newPos);
    void barVisibilityExpandedChanged(bool barExpanded);
    void popupVisibilityChanged(bool visible);
    void searchCompleted(int resultCount, double elapsedMs);
    void fileResultActivated(const lumen::search::FileSearchResult& result);
    void fileRevealedInExplorer(const lumen::search::FileSearchResult& result);
    void filePathCopied(const QString& path);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onSearchTextChanged(const QString& text);
    void onScreenConfigurationChanged();
    void onResultActivated(const search::FileSearchResult& result);

private:
    void setPopupExpanded(bool expanded);
    void applyOpenBarGeometry();
    void updateExpandedGeometry();
    void updateSearchInputGeometry();
    void syncNativeBackdropBlur();
    void dispatchAsyncSearch(const QString& trimmedQuery);
    void dispatchRelatedDocumentsSearch(const search::FileSearchResult& sourceResult);
    void dispatchInspectorPreviewFetch(const search::FileSearchResult& result);
    void applyRestingWindowZOrder();
    void bindScreenSignals();
    QRectF cardRect() const;
    QRectF iconTileRect() const;
    QRectF clearButtonHitRect() const;

    QLineEdit* m_searchEdit = nullptr;
    SearchPopupWidget* m_popupWidget = nullptr;
    QVariantAnimation* m_slideAnim = nullptr;

    QThreadPool m_searchPool;
    search::FileSearchEngine m_searchEngine;
    std::shared_ptr<std::atomic_bool> m_activeCancelToken;
    quint64 m_queryGeneration = 0;
    quint64 m_previewGeneration = 0;

    qreal m_slideProgress = 0.0;
    bool m_barExpanded = false;
    bool m_popupExpanded = false;
    bool m_slidesLeft = true;
    bool m_isSearchActive = false;
    bool m_clearHovered = false;
    bool m_iconHovered = false;
    bool m_keepAboveShellTemp = false;
    bool m_alwaysOnTop = false;
    bool m_dragging = false;
    bool m_dragMoved = false;
    QPoint m_dragOffset;
    QPoint m_iconAnchorPos;

    bool m_hotkeyRegistered = false;
    bool m_openFilesWithShell = true;
    QString m_hotkeyError;
};

} // namespace lumen::ui
