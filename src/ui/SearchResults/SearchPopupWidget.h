#pragma once

#include "search/FileSearchEngine.h"

#include <QAbstractListModel>
#include <QListView>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>
#include <optional>

namespace lumen::ui {

class SearchResultListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        FullPathRole,
        ParentDirRole,
        ExtensionRole,
        SizeRole,
        MtimeRole,
        MatchStartRole,
        MatchLengthRole,
        SnippetRole,
        MatchedPageRole,
        MatchTypeRole,
        SemanticSimilarityRole,
        IsPinnedRole,
        IsDirectoryRole,
        GroupHeaderTitleRole,
        GroupHeaderCountRole
    };

    explicit SearchResultListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    void setResults(QVector<search::FileSearchResult> results);
    void clear();

    const QVector<search::FileSearchResult>& results() const noexcept { return m_results; }
    const search::FileSearchResult* resultAt(int row) const noexcept;

private:
    QVector<search::FileSearchResult> m_results;
    QVector<QString> m_groupTitles;
    QVector<int> m_groupCounts;
};

class SearchResultItemDelegate;

class SearchPopupWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeaderHeight = 44;
    static constexpr int kGroupHeaderExtraHeight = 24;
    static constexpr int kRowHeight = 62;
    static constexpr int kContentRowHeight = 76;
    static constexpr int kSplitBodyMinHeight = 420;
    static constexpr int kPreviewHeight = 48;
    static constexpr int kFooterHeight = 0;
    static constexpr int kMaxVisibleRows = 6;
    static constexpr int kLeftPaneWidth = 488;
    static constexpr int kFullWorkspaceWidth = 908;

    explicit SearchPopupWidget(QWidget* parent = nullptr);
    ~SearchPopupWidget() override = default;

    void setQueryText(const QString& query);
    QString queryText() const { return m_queryText; }

    void setSearchResults(QVector<search::FileSearchResult> results, double elapsedMs);
    void clearResults();

    void setSearchMode(search::SearchMode mode);
    search::SearchMode searchMode() const noexcept { return m_searchMode; }
    void cycleSearchMode(bool forward = true);

    void setPreviewVisible(bool visible);
    bool isPreviewVisible() const noexcept { return m_previewVisible; }
    void togglePreview();

    void setPreviewText(const QString& text);
    QString previewText() const { return m_previewText; }

    void setRelatedSourceFileName(const QString& fileName);
    QString relatedSourceFileName() const { return m_relatedSourceFileName; }

    void setIndexingInProgress(bool indexing);
    bool isIndexingInProgress() const noexcept { return m_indexingInProgress; }

    void setDarkTheme(bool dark);
    bool isDarkTheme() const noexcept { return m_darkTheme; }

    int resultCount() const noexcept;
    int selectedRow() const noexcept;
    bool hasSelection() const noexcept;
    const search::FileSearchResult* selectedResult() const noexcept;
    void clearResultSelection();
    void clearInspectorSnapshot();
    void showFileCopiedFeedback();

    void selectNext();
    void selectPrevious();
    bool activateSelected();

    int desiredPopupHeight() const noexcept;
    QString statusHeadline() const;

signals:
    void resultActivated(const lumen::search::FileSearchResult& result);
    void revealInExplorerRequested(const lumen::search::FileSearchResult& result);
    void copyFileRequested(const lumen::search::FileSearchResult& result);
    void copyPathRequested(const lumen::search::FileSearchResult& result);
    void findRelatedRequested(const lumen::search::FileSearchResult& result);
    void togglePinRequested(const lumen::search::FileSearchResult& result);
    void searchModeChanged(lumen::search::SearchMode mode);
    void previewVisibilityChanged(bool visible);
    void selectedResultChanged(const lumen::search::FileSearchResult& result);
    void selectionActiveChanged(bool hasSelection);
    void exitRelatedModeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void updateListLayout();
    void syncInspectorSnapshot(const search::FileSearchResult& item);
    void showContextMenuAt(const QPoint& localPos, const search::FileSearchResult& item);
    int leftPaneWidth() const noexcept;
    QRectF modePillRect(int modeIndex) const;
    QRectF backButtonRect() const;
    QRectF inspectorPaneRect() const;
    QRectF inspectorCloseButtonRect(const QRectF& inspRect) const;
    QRectF inspectorPathBoxRect(const QRectF& inspRect) const;
    QRectF inspectorCopyPathButtonRect(const QRectF& inspRect) const;
    QRectF inspectorOpenButtonRect(const QRectF& inspRect) const;
    QRectF inspectorRevealButtonRect(const QRectF& inspRect) const;
    QRectF inspectorCopyFileButtonRect(const QRectF& inspRect) const;
    QRectF inspectorRelatedButtonRect(const QRectF& inspRect) const;
    QRectF inspectorPinButtonRect(const QRectF& inspRect) const;
    int hoveredActionIndex(const QPointF& pos) const;

    QString m_queryText;
    QString m_previewText;
    QString m_lastInspectorPreviewText;
    QString m_relatedSourceFileName;
    std::optional<search::FileSearchResult> m_lastInspectorResult;
    double m_lastQueryMs = 0.0;
    search::SearchMode m_searchMode = search::SearchMode::All;
    bool m_previewVisible = false;
    bool m_darkTheme = false;
    bool m_indexingInProgress = false;
    bool m_pathHovered = false;
    bool m_closeDetailsHovered = false;
    bool m_copiedFeedback = false;
    bool m_copiedFileFeedback = false;
    int m_hoveredActionIdx = -1;
    int m_hoveredModeIdx = -1;

    SearchResultListModel* m_model = nullptr;
    QListView* m_listView = nullptr;
    SearchResultItemDelegate* m_delegate = nullptr;
};

} // namespace lumen::ui
