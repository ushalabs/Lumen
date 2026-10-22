#pragma once

#include "search/FileSearchEngine.h"

#include <QLineEdit>
#include <QPoint>
#include <QRectF>
#include <QThreadPool>
#include <QWidget>
#include <atomic>
#include <memory>

namespace lumen::ui {

class SearchPopupWidget;

class DesktopSearchBar : public QWidget {
    Q_OBJECT

public:
    static constexpr int kBarWidth = 400;
    static constexpr int kBarHeight = 50;
    static constexpr int kPopupGap = 6;
    static constexpr int kPopupHeight = 112;
    static constexpr int kExpandedHeight = kBarHeight + kPopupGap + kPopupHeight;
    static constexpr int kRightEdgeMargin = 30;
    static constexpr int kTopWorkAreaOffset = 100;

    explicit DesktopSearchBar(QWidget* parent = nullptr);
    ~DesktopSearchBar() override;

    static QPoint computeDefaultDesktopPosition();
    static QPoint clampPositionToScreens(const QPoint& candidatePos);

    void applySavedOrDefaultPosition();
    void activateAndFocusSearch();
    void collapsePopupAndDeactivate(bool clearQuery = false);

    bool isPopupExpanded() const noexcept { return m_popupExpanded; }
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

signals:
    void explorerRestartDetected();
    void positionPersisted(const QPoint& newPos);
    void popupVisibilityChanged(bool visible);
    void searchCompleted(int resultCount, double elapsedMs);
    void fileResultActivated(const lumen::search::FileSearchResult& result);

protected:
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private slots:
    void onSearchTextChanged(const QString& text);
    void onScreenConfigurationChanged();
    void onResultActivated(const lumen::search::FileSearchResult& result);

private:
    void setPopupExpanded(bool expanded);
    void updateExpandedGeometry();
    void dispatchAsyncSearch(const QString& trimmedQuery);
    void bindScreenSignals();
    QRectF clearButtonHitRect() const;

    QLineEdit* m_searchEdit = nullptr;
    SearchPopupWidget* m_popupWidget = nullptr;

    QThreadPool m_searchPool;
    search::FileSearchEngine m_searchEngine;
    std::shared_ptr<std::atomic_bool> m_activeCancelToken;
    quint64 m_queryGeneration = 0;

    bool m_popupExpanded = false;
    bool m_isSearchActive = false;
    bool m_keepAboveShellTemp = false;
    bool m_dragging = false;
    bool m_dragMoved = false;
    QPoint m_dragOffset;

    bool m_darkTheme = false;
    bool m_hotkeyRegistered = false;
    bool m_openFilesWithShell = true;
    QString m_hotkeyError;
};

} // namespace lumen::ui
