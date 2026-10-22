#pragma once

#include "search/FileSearchEngine.h"

#include <QAbstractListModel>
#include <QListView>
#include <QRectF>
#include <QString>
#include <QVector>
#include <QWidget>

namespace lumen::ui {

class SearchResultListModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Roles {
        FileNameRole = Qt::UserRole + 1,
        ParentDirRole,
        ExtensionRole,
        SizeRole,
        MtimeRole,
        MatchStartRole,
        MatchLengthRole,
        SnippetRole,
        MatchedPageRole,
        MatchTypeRole,
        IsPinnedRole
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
};

class SearchResultItemDelegate;

class SearchPopupWidget : public QWidget {
    Q_OBJECT

public:
    static constexpr int kHeaderHeight = 34;
    static constexpr int kRowHeight = 54;
    static constexpr int kContentRowHeight = 72;
    static constexpr int kPreviewHeight = 112;
    static constexpr int kFooterHeight = 28;
    static constexpr int kMaxVisibleRows = 6;

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
    const search::FileSearchResult* selectedResult() const noexcept;

    void selectNext();
    void selectPrevious();
    bool activateSelected();

    int desiredPopupHeight() const noexcept;
    QString statusHeadline() const;

signals:
    void resultActivated(const lumen::search::FileSearchResult& result);
    void revealInExplorerRequested(const lumen::search::FileSearchResult& result);
    void copyPathRequested(const lumen::search::FileSearchResult& result);
    void findRelatedRequested(const lumen::search::FileSearchResult& result);
    void togglePinRequested(const lumen::search::FileSearchResult& result);
    void searchModeChanged(lumen::search::SearchMode mode);
    void previewVisibilityChanged(bool visible);
    void selectedResultChanged(const lumen::search::FileSearchResult& result);
    void exitRelatedModeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    void updateListLayout();
    void showContextMenuAt(const QPoint& localPos, const search::FileSearchResult& item);
    QRectF modePillRect(int modeIndex) const;
    QRectF previewToggleRect() const;

    QString m_queryText;
    QString m_previewText;
    QString m_relatedSourceFileName;
    double m_lastQueryMs = 0.0;
    search::SearchMode m_searchMode = search::SearchMode::All;
    bool m_previewVisible = false;
    bool m_darkTheme = false;
    bool m_indexingInProgress = false;

    SearchResultListModel* m_model = nullptr;
    QListView* m_listView = nullptr;
    SearchResultItemDelegate* m_delegate = nullptr;
};

} // namespace lumen::ui
