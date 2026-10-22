#include "ui/SearchResults/SearchPopupWidget.h"

#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"

#include <QClipboard>
#include <QColor>
#include <QDateTime>
#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QItemSelectionModel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPen>
#include <QScrollBar>
#include <QStyledItemDelegate>

namespace lumen::ui {

namespace {
QString formatHumanSize(qint64 bytes) {
    if (bytes < 1024) {
        return QStringLiteral("%1 B").arg(bytes);
    }
    if (bytes < 1024 * 1024) {
        return QStringLiteral("%1 KB").arg(bytes / 1024);
    }
    if (bytes < 1024LL * 1024LL * 1024LL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QColor extensionBadgeColor(const QString& extLower, bool darkTheme) {
    if (extLower == QStringLiteral("pdf")) {
        return darkTheme ? QColor(239, 68, 68, 55) : QColor(254, 226, 226);
    }
    if (extLower == QStringLiteral("doc") || extLower == QStringLiteral("docx") ||
        extLower == QStringLiteral("txt") || extLower == QStringLiteral("md")) {
        return darkTheme ? QColor(59, 130, 246, 55) : QColor(219, 234, 254);
    }
    if (extLower == QStringLiteral("xls") || extLower == QStringLiteral("xlsx") ||
        extLower == QStringLiteral("csv")) {
        return darkTheme ? QColor(16, 185, 129, 55) : QColor(209, 250, 229);
    }
    if (extLower == QStringLiteral("png") || extLower == QStringLiteral("jpg") ||
        extLower == QStringLiteral("jpeg") || extLower == QStringLiteral("svg")) {
        return darkTheme ? QColor(168, 85, 247, 55) : QColor(243, 232, 255);
    }
    return darkTheme ? QColor(71, 85, 105, 80) : QColor(241, 245, 249);
}

QColor extensionTextColor(const QString& extLower, bool darkTheme) {
    if (extLower == QStringLiteral("pdf")) {
        return darkTheme ? QColor(252, 165, 165) : QColor(185, 28, 28);
    }
    if (extLower == QStringLiteral("doc") || extLower == QStringLiteral("docx") ||
        extLower == QStringLiteral("txt") || extLower == QStringLiteral("md")) {
        return darkTheme ? QColor(147, 197, 253) : QColor(29, 78, 216);
    }
    if (extLower == QStringLiteral("xls") || extLower == QStringLiteral("xlsx") ||
        extLower == QStringLiteral("csv")) {
        return darkTheme ? QColor(110, 231, 183) : QColor(4, 120, 87);
    }
    if (extLower == QStringLiteral("png") || extLower == QStringLiteral("jpg") ||
        extLower == QStringLiteral("jpeg") || extLower == QStringLiteral("svg")) {
        return darkTheme ? QColor(216, 180, 254) : QColor(109, 40, 217);
    }
    return darkTheme ? QColor(203, 213, 225) : QColor(71, 85, 105);
}
} // namespace

// ------------------------------------------------------------------------------
// SearchResultListModel
// ------------------------------------------------------------------------------
SearchResultListModel::SearchResultListModel(QObject* parent)
    : QAbstractListModel(parent) {}

int SearchResultListModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_results.size());
}

QVariant SearchResultListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= m_results.size()) {
        return {};
    }
    const auto& item = m_results.at(index.row());
    switch (role) {
        case Qt::DisplayRole:
        case FileNameRole:    return item.name;
        case ParentDirRole:   return item.parentDirectory;
        case ExtensionRole:   return item.extension;
        case SizeRole:        return item.size;
        case MtimeRole:       return item.mtime;
        case MatchStartRole:  return item.matchStart;
        case MatchLengthRole: return item.matchLength;
        case SnippetRole:     return item.snippet;
        case MatchedPageRole: return item.matchedPage;
        case MatchTypeRole:   return static_cast<int>(item.matchType);
        case IsPinnedRole:    return item.isPinned;
        default:              return {};
    }
}

void SearchResultListModel::setResults(QVector<search::FileSearchResult> results) {
    beginResetModel();
    m_results = std::move(results);
    endResetModel();
}

void SearchResultListModel::clear() {
    if (m_results.isEmpty()) {
        return;
    }
    beginResetModel();
    m_results.clear();
    endResetModel();
}

const search::FileSearchResult* SearchResultListModel::resultAt(int row) const noexcept {
    if (row < 0 || row >= m_results.size()) {
        return nullptr;
    }
    return &m_results.at(row);
}

namespace {
struct SnippetSegment {
    QString text;
    bool highlighted = false;
};

QVector<SnippetSegment> parseSnippetSegments(const QString& rawInput) {
    QString rawSnippet = rawInput;
    rawSnippet.replace(QStringLiteral("\r\n"), QStringLiteral(" "));
    rawSnippet.replace(QLatin1Char('\n'), QLatin1Char(' '));
    rawSnippet.replace(QLatin1Char('\r'), QLatin1Char(' '));
    rawSnippet = rawSnippet.simplified();

    QVector<SnippetSegment> segments;
    int pos = 0;
    const int n = rawSnippet.size();
    while (pos < n) {
        const int openIdx = rawSnippet.indexOf(QChar(0x00AB), pos); // '«'
        if (openIdx < 0) {
            segments.push_back({rawSnippet.mid(pos), false});
            break;
        }
        if (openIdx > pos) {
            segments.push_back({rawSnippet.mid(pos, openIdx - pos), false});
        }
        const int closeIdx = rawSnippet.indexOf(QChar(0x00BB), openIdx + 1); // '»'
        if (closeIdx < 0) {
            segments.push_back({rawSnippet.mid(openIdx + 1), true});
            break;
        }
        segments.push_back({rawSnippet.mid(openIdx + 1, closeIdx - openIdx - 1), true});
        pos = closeIdx + 1;
    }
    return segments;
}

void drawHighlightedSnippet(QPainter* painter,
                            const QRectF& rect,
                            const QString& rawSnippet,
                            bool darkTheme) {
    const QVector<SnippetSegment> segments = parseSnippetSegments(rawSnippet);
    if (segments.isEmpty()) {
        return;
    }

    QFont normalFont(QStringLiteral("Segoe UI"), 8, QFont::Normal);
    QFont boldFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold);
    const QFontMetrics fmNormal(normalFont);
    const QFontMetrics fmBold(boldFont);

    const QColor normalColor = darkTheme ? QColor(148, 163, 184) : QColor(71, 85, 105);
    const QColor hlTextColor = darkTheme ? QColor(147, 197, 253) : QColor(29, 78, 216);
    const QColor hlBgColor = darkTheme ? QColor(59, 130, 246, 55) : QColor(219, 234, 254);

    qreal curX = rect.left();
    const qreal maxX = rect.right();

    painter->setPen(Qt::NoPen);
    painter->setBrush(darkTheme ? QColor(96, 165, 250, 140) : QColor(59, 130, 246, 160));
    painter->drawRoundedRect(QRectF(curX, rect.top() + 3.0, 2.5, rect.height() - 6.0), 1.2, 1.2);
    curX += 7.0;

    for (const SnippetSegment& seg : segments) {
        if (seg.text.isEmpty() || curX >= maxX - 8.0) {
            continue;
        }
        const QFontMetrics& fm = seg.highlighted ? fmBold : fmNormal;
        const int availPx = static_cast<int>(maxX - curX);
        QString drawText = seg.text;
        int advance = fm.horizontalAdvance(drawText);
        bool truncated = false;
        if (advance > availPx) {
            drawText = fm.elidedText(drawText, Qt::ElideRight, availPx);
            advance = fm.horizontalAdvance(drawText);
            truncated = true;
        }

        if (seg.highlighted && !drawText.isEmpty()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(hlBgColor);
            painter->drawRoundedRect(QRectF(curX - 1.0, rect.top() + 2.0, advance + 2.0, rect.height() - 4.0), 3.0, 3.0);
            painter->setFont(boldFont);
            painter->setPen(hlTextColor);
        } else {
            painter->setFont(normalFont);
            painter->setPen(normalColor);
        }

        painter->drawText(QRectF(curX, rect.top(), advance + 2.0, rect.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          drawText);
        curX += advance;
        if (truncated) {
            break;
        }
    }
}
} // namespace

// ------------------------------------------------------------------------------
// SearchResultItemDelegate
// ------------------------------------------------------------------------------
class SearchResultItemDelegate : public QStyledItemDelegate {
public:
    explicit SearchResultItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void setDarkTheme(bool dark) { m_darkTheme = dark; }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/, const QModelIndex& index) const override {
        const QString snippet = index.data(SearchResultListModel::SnippetRole).toString();
        const int h = snippet.isEmpty() ? SearchPopupWidget::kRowHeight : SearchPopupWidget::kContentRowHeight;
        return QSize(380, h);
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);

        const QRectF rowRect = QRectF(option.rect).adjusted(6.0, 2.0, -6.0, -2.0);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

        if (isSelected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(59, 130, 246, 48) : QColor(239, 246, 255));
            painter->drawRoundedRect(rowRect, 8.0, 8.0);

            painter->setBrush(m_darkTheme ? QColor(96, 165, 250) : QColor(37, 99, 235));
            painter->drawRoundedRect(QRectF(rowRect.left() + 2.0, rowRect.top() + 8.0, 3.0, rowRect.height() - 16.0), 1.5, 1.5);
        } else if (isHovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(51, 65, 85, 70) : QColor(248, 250, 252));
            painter->drawRoundedRect(rowRect, 8.0, 8.0);
        }

        const QString fileName = index.data(SearchResultListModel::FileNameRole).toString();
        const QString parentDir = index.data(SearchResultListModel::ParentDirRole).toString();
        const QString ext = index.data(SearchResultListModel::ExtensionRole).toString().toLower();
        const qint64 fileSize = index.data(SearchResultListModel::SizeRole).toLongLong();
        const qint64 mtime = index.data(SearchResultListModel::MtimeRole).toLongLong();
        const int matchStart = index.data(SearchResultListModel::MatchStartRole).toInt();
        const int matchLen = index.data(SearchResultListModel::MatchLengthRole).toInt();
        const QString snippet = index.data(SearchResultListModel::SnippetRole).toString();
        const int matchedPage = index.data(SearchResultListModel::MatchedPageRole).toInt();
        const bool isPinned = index.data(SearchResultListModel::IsPinnedRole).toBool();
        const auto matchType = static_cast<search::SearchResultMatchType>(
            index.data(SearchResultListModel::MatchTypeRole).toInt());

        // 1. Left File Icon Badge
        const QRectF iconBox(rowRect.left() + 10.0, rowRect.top() + 9.0, 30.0, 30.0);
        painter->setPen(Qt::NoPen);
        painter->setBrush(extensionBadgeColor(ext, m_darkTheme));
        painter->drawRoundedRect(iconBox, 7.0, 7.0);

        QFont iconFont(QStringLiteral("Segoe UI"), 7, QFont::Bold);
        painter->setFont(iconFont);
        painter->setPen(extensionTextColor(ext, m_darkTheme));
        const QString badgeLabel = ext.isEmpty() ? QStringLiteral("FILE") : ext.left(4).toUpper();
        painter->drawText(iconBox, Qt::AlignCenter, badgeLabel);

        // 2. Top-Right: Optional Content / Semantic / Pinned pill + File size label
        const qreal textLeft = iconBox.right() + 10.0;
        const qreal rightMargin = rowRect.right() - 10.0;
        const qreal availWidth = rightMargin - textLeft;

        QFont metaFont(QStringLiteral("Segoe UI"), 8, QFont::Normal);
        painter->setFont(metaFont);
        const QFontMetrics fmMeta(metaFont);
        const QColor secondaryText = m_darkTheme ? QColor(148, 163, 184) : QColor(100, 116, 139);
        const QString sizeStr = formatHumanSize(fileSize);
        const int sizeWidth = fmMeta.horizontalAdvance(sizeStr) + 6;

        painter->setPen(secondaryText);
        painter->drawText(QRectF(rightMargin - sizeWidth, rowRect.top() + 4.0, sizeWidth, 20.0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          sizeStr);

        int rightReservedWidth = sizeWidth + 6;
        if (matchType != search::SearchResultMatchType::Filename || matchedPage > 0) {
            const bool isSemantic = (matchType == search::SearchResultMatchType::Semantic ||
                                     matchType == search::SearchResultMatchType::Hybrid);
            QString pillLabel;
            if (matchType == search::SearchResultMatchType::Semantic) {
                pillLabel = (matchedPage > 0)
                                ? QStringLiteral("Page %1 · Semantic").arg(matchedPage)
                                : QStringLiteral("Semantic");
            } else if (matchType == search::SearchResultMatchType::Hybrid) {
                pillLabel = (matchedPage > 0)
                                ? QStringLiteral("Page %1 · Hybrid").arg(matchedPage)
                                : QStringLiteral("Hybrid");
            } else {
                pillLabel = (matchedPage > 0)
                                ? QStringLiteral("Page %1").arg(matchedPage)
                                : QStringLiteral("Content");
            }

            QFont pillFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold);
            const QFontMetrics fmPill(pillFont);
            const int pillW = fmPill.horizontalAdvance(pillLabel) + 12;
            const QRectF pillRect(rightMargin - rightReservedWidth - pillW,
                                  rowRect.top() + 6.0,
                                  pillW,
                                  16.0);
            painter->setPen(Qt::NoPen);
            if (isSemantic) {
                painter->setBrush(m_darkTheme ? QColor(139, 92, 246, 55) : QColor(237, 233, 254));
            } else {
                painter->setBrush(m_darkTheme ? QColor(16, 185, 129, 45) : QColor(209, 250, 229));
            }
            painter->drawRoundedRect(pillRect, 4.0, 4.0);
            painter->setFont(pillFont);
            if (isSemantic) {
                painter->setPen(m_darkTheme ? QColor(196, 181, 253) : QColor(109, 40, 217));
            } else {
                painter->setPen(m_darkTheme ? QColor(110, 231, 183) : QColor(4, 120, 87));
            }
            painter->drawText(pillRect, Qt::AlignCenter, pillLabel);
            rightReservedWidth += pillW + 6;
        }

        if (isPinned) {
            const QString pinLabel = QStringLiteral("★ Pinned");
            QFont pillFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold);
            const QFontMetrics fmPill(pillFont);
            const int pillW = fmPill.horizontalAdvance(pinLabel) + 10;
            const QRectF pillRect(rightMargin - rightReservedWidth - pillW,
                                  rowRect.top() + 6.0,
                                  pillW,
                                  16.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(245, 158, 11, 55) : QColor(254, 243, 199));
            painter->drawRoundedRect(pillRect, 4.0, 4.0);
            painter->setFont(pillFont);
            painter->setPen(m_darkTheme ? QColor(253, 224, 71) : QColor(180, 83, 9));
            painter->drawText(pillRect, Qt::AlignCenter, pinLabel);
            rightReservedWidth += pillW + 6;
        }

        // 3. Top Line: Filename (with highlighted match range)
        QFont nameFont(QStringLiteral("Segoe UI"), 9, QFont::DemiBold);
        painter->setFont(nameFont);
        const QFontMetrics fmName(nameFont);
        const int maxNameWidth = std::max(60, static_cast<int>(availWidth - rightReservedWidth));
        const QString elidedName = fmName.elidedText(fileName, Qt::ElideMiddle, maxNameWidth);

        const QColor primaryText = m_darkTheme ? QColor(241, 245, 249) : QColor(15, 23, 42);
        const QColor highlightBg = m_darkTheme ? QColor(59, 130, 246, 75) : QColor(219, 234, 254);

        const QRectF nameRect(textLeft, rowRect.top() + 4.0, maxNameWidth, 20.0);
        if (matchStart >= 0 && matchLen > 0 && elidedName == fileName &&
            matchStart + matchLen <= fileName.size()) {
            const QString prefix = fileName.left(matchStart);
            const int hlX = static_cast<int>(textLeft) + fmName.horizontalAdvance(prefix);
            const int hlW = fmName.horizontalAdvance(fileName.mid(matchStart, matchLen));
            painter->setPen(Qt::NoPen);
            painter->setBrush(highlightBg);
            painter->drawRoundedRect(QRectF(hlX - 1.0, rowRect.top() + 6.0, hlW + 2.0, 16.0), 3.0, 3.0);
        }

        painter->setPen(primaryText);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // 4. Second Line: Elided parent folder path + modification date
        painter->setFont(metaFont);
        painter->setPen(secondaryText);
        const QString dateStr = (mtime > 0)
                                    ? QDateTime::fromSecsSinceEpoch(mtime).toString(QStringLiteral("MMM d, yyyy"))
                                    : QString();
        const int dateWidth = dateStr.isEmpty() ? 0 : (fmMeta.horizontalAdvance(dateStr) + 10);
        const QString elidedDir = fmMeta.elidedText(
            parentDir, Qt::ElideMiddle, static_cast<int>(availWidth - dateWidth));

        painter->drawText(QRectF(textLeft, rowRect.top() + 23.0, availWidth - dateWidth, 18.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedDir);

        if (!dateStr.isEmpty()) {
            painter->drawText(QRectF(rightMargin - dateWidth, rowRect.top() + 23.0, dateWidth, 18.0),
                              Qt::AlignRight | Qt::AlignVCenter,
                              dateStr);
        }

        // 5. Third Line (when content snippet is available): Highlighted snippet
        if (!snippet.isEmpty()) {
            const QRectF snippetRect(textLeft, rowRect.top() + 42.0, availWidth, 19.0);
            drawHighlightedSnippet(painter, snippetRect, snippet, m_darkTheme);
        }

        painter->restore();
    }

private:
    bool m_darkTheme = false;
};

// ------------------------------------------------------------------------------
// SearchPopupWidget
// ------------------------------------------------------------------------------
SearchPopupWidget::SearchPopupWidget(QWidget* parent)
    : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::NoFocus);

    m_model = new SearchResultListModel(this);
    m_delegate = new SearchResultItemDelegate(this);

    m_listView = new QListView(this);
    m_listView->setModel(m_model);
    m_listView->setItemDelegate(m_delegate);
    m_listView->setUniformItemSizes(false);
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_listView->setMouseTracking(true);
    m_listView->setFocusPolicy(Qt::NoFocus);
    m_listView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_listView->viewport()->setAutoFillBackground(false);
    m_listView->setStyleSheet(QStringLiteral(
        "QListView { background: transparent; border: none; outline: none; }"
        "QScrollBar:vertical { width: 6px; background: transparent; margin: 4px 2px; }"
        "QScrollBar::handle:vertical { background: rgba(148, 163, 184, 120); border-radius: 3px; min-height: 24px; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"));
    m_listView->hide();

    connect(m_listView, &QListView::clicked, this, [this](const QModelIndex& idx) {
        if (const auto* res = m_model->resultAt(idx.row())) {
            emit resultActivated(*res);
        }
    });

    connect(m_listView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                if (const auto* res = m_model->resultAt(current.row())) {
                    emit selectedResultChanged(*res);
                }
                if (m_previewVisible) {
                    update();
                }
            });

    connect(m_listView, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_listView->indexAt(pos);
        if (idx.isValid()) {
            m_listView->setCurrentIndex(idx);
            if (const auto* res = m_model->resultAt(idx.row())) {
                showContextMenuAt(m_listView->mapTo(this, pos), *res);
            }
        }
    });
}

void SearchPopupWidget::setQueryText(const QString& query) {
    if (m_queryText != query) {
        m_queryText = query;
        update();
    }
}

void SearchPopupWidget::setSearchResults(QVector<search::FileSearchResult> results, double elapsedMs) {
    m_lastQueryMs = elapsedMs;
    m_model->setResults(std::move(results));
    if (m_model->rowCount() > 0) {
        m_listView->setCurrentIndex(m_model->index(0, 0));
        m_listView->show();
        if (const auto* res = m_model->resultAt(0)) {
            emit selectedResultChanged(*res);
        }
    } else {
        m_listView->hide();
        m_previewText.clear();
    }
    updateListLayout();
    update();
}

void SearchPopupWidget::clearResults() {
    m_lastQueryMs = 0.0;
    m_previewText.clear();
    m_relatedSourceFileName.clear();
    m_model->clear();
    m_listView->hide();
    update();
}

void SearchPopupWidget::setSearchMode(search::SearchMode mode) {
    if (m_searchMode != mode || !m_relatedSourceFileName.isEmpty()) {
        m_searchMode = mode;
        m_relatedSourceFileName.clear();
        update();
        emit searchModeChanged(m_searchMode);
    }
}

void SearchPopupWidget::cycleSearchMode(bool forward) {
    const int cur = static_cast<int>(m_searchMode);
    const int next = forward ? ((cur + 1) % 4) : ((cur + 3) % 4);
    setSearchMode(static_cast<search::SearchMode>(next));
}

void SearchPopupWidget::setPreviewVisible(bool visible) {
    if (m_previewVisible != visible) {
        m_previewVisible = visible;
        if (!m_previewVisible) {
            m_previewText.clear();
        }
        updateListLayout();
        update();
        emit previewVisibilityChanged(m_previewVisible);
    }
}

void SearchPopupWidget::togglePreview() {
    setPreviewVisible(!m_previewVisible);
}

void SearchPopupWidget::setPreviewText(const QString& text) {
    if (m_previewText != text) {
        m_previewText = text;
        if (m_previewVisible) {
            update();
        }
    }
}

void SearchPopupWidget::setRelatedSourceFileName(const QString& fileName) {
    if (m_relatedSourceFileName != fileName) {
        m_relatedSourceFileName = fileName;
        update();
    }
}

void SearchPopupWidget::setIndexingInProgress(bool indexing) {
    if (m_indexingInProgress != indexing) {
        m_indexingInProgress = indexing;
        update();
    }
}

void SearchPopupWidget::setDarkTheme(bool dark) {
    if (m_darkTheme != dark) {
        m_darkTheme = dark;
        m_delegate->setDarkTheme(dark);
        m_listView->viewport()->update();
        update();
    }
}

int SearchPopupWidget::resultCount() const noexcept {
    return m_model ? m_model->rowCount() : 0;
}

int SearchPopupWidget::selectedRow() const noexcept {
    if (!m_listView || !m_listView->currentIndex().isValid()) {
        return -1;
    }
    return m_listView->currentIndex().row();
}

const search::FileSearchResult* SearchPopupWidget::selectedResult() const noexcept {
    return m_model ? m_model->resultAt(selectedRow()) : nullptr;
}

void SearchPopupWidget::selectNext() {
    const int count = resultCount();
    if (count <= 0) {
        return;
    }
    const int nextRow = (selectedRow() + 1) % count;
    const QModelIndex idx = m_model->index(nextRow, 0);
    m_listView->setCurrentIndex(idx);
    m_listView->scrollTo(idx);
}

void SearchPopupWidget::selectPrevious() {
    const int count = resultCount();
    if (count <= 0) {
        return;
    }
    const int prevRow = (selectedRow() <= 0) ? (count - 1) : (selectedRow() - 1);
    const QModelIndex idx = m_model->index(prevRow, 0);
    m_listView->setCurrentIndex(idx);
    m_listView->scrollTo(idx);
}

bool SearchPopupWidget::activateSelected() {
    if (const auto* res = selectedResult()) {
        emit resultActivated(*res);
        return true;
    }
    return false;
}

int SearchPopupWidget::desiredPopupHeight() const noexcept {
    const int count = resultCount();
    if (count <= 0) {
        return kHeaderHeight + 100;
    }
    const int visibleRows = qMin(count, kMaxVisibleRows);
    int rowsHeight = 0;
    const auto& items = m_model->results();
    for (int i = 0; i < visibleRows && i < items.size(); ++i) {
        rowsHeight += items.at(i).snippet.isEmpty() ? kRowHeight : kContentRowHeight;
    }
    const int previewAdd = m_previewVisible ? kPreviewHeight : 0;
    return kHeaderHeight + 8 + rowsHeight + previewAdd + kFooterHeight;
}

QString SearchPopupWidget::statusHeadline() const {
    if (database::DatabaseManager::instance().fileCount() == 0) {
        return QStringLiteral("Your search index is not ready yet.");
    }
    return QStringLiteral("No matching files found.");
}

void SearchPopupWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateListLayout();
}

void SearchPopupWidget::updateListLayout() {
    if (m_listView != nullptr && m_listView->isVisible()) {
        const int previewAdd = (m_previewVisible && resultCount() > 0) ? kPreviewHeight : 0;
        const int listH = std::max(0, height() - kHeaderHeight - previewAdd - kFooterHeight - 6);
        m_listView->setGeometry(2, kHeaderHeight + 2, width() - 4, listH);
    }
}

QRectF SearchPopupWidget::modePillRect(int modeIndex) const {
    static const int widths[4] = {38, 46, 62, 68};
    qreal x = 12.0;
    for (int i = 0; i < modeIndex && i < 4; ++i) {
        x += widths[i] + 5.0;
    }
    return QRectF(x, 6.0, widths[modeIndex], 20.0);
}

QRectF SearchPopupWidget::previewToggleRect() const {
    return QRectF(width() - 72.0, 6.0, 60.0, 20.0);
}

void SearchPopupWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF pos = event->position();
        if (pos.y() <= kHeaderHeight) {
            if (!m_relatedSourceFileName.isEmpty()) {
                if (previewToggleRect().contains(pos)) {
                    m_relatedSourceFileName.clear();
                    emit exitRelatedModeRequested();
                    event->accept();
                    return;
                }
            } else {
                for (int i = 0; i < 4; ++i) {
                    if (modePillRect(i).contains(pos)) {
                        setSearchMode(static_cast<search::SearchMode>(i));
                        event->accept();
                        return;
                    }
                }
                if (previewToggleRect().contains(pos)) {
                    togglePreview();
                    event->accept();
                    return;
                }
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void SearchPopupWidget::showContextMenuAt(const QPoint& localPos, const search::FileSearchResult& item) {
    QMenu menu(this);
    menu.setStyleSheet(m_darkTheme
                           ? QStringLiteral("QMenu { background-color: #1E222A; color: #F1F5F9; border: 1px solid #384152; border-radius: 6px; padding: 4px; font-family: 'Segoe UI'; font-size: 9pt; }"
                                            "QMenu::item { padding: 5px 20px 5px 12px; border-radius: 4px; }"
                                            "QMenu::item:selected { background-color: #2563EB; color: #FFFFFF; }"
                                            "QMenu::separator { height: 1px; background: #334155; margin: 4px 6px; }")
                           : QStringLiteral("QMenu { background-color: #FFFFFF; color: #0F172A; border: 1px solid #CBD5E1; border-radius: 6px; padding: 4px; font-family: 'Segoe UI'; font-size: 9pt; }"
                                            "QMenu::item { padding: 5px 20px 5px 12px; border-radius: 4px; }"
                                            "QMenu::item:selected { background-color: #EFF6FF; color: #1D4ED8; }"
                                            "QMenu::separator { height: 1px; background: #E2E8F0; margin: 4px 6px; }"));

    QAction* openAct = menu.addAction(QStringLiteral("Open File\tEnter"));
    QAction* revealAct = menu.addAction(QStringLiteral("Reveal in File Explorer\tCtrl+Enter"));
    QAction* previewAct = menu.addAction(m_previewVisible
                                             ? QStringLiteral("Hide Quick Preview\tCtrl+P")
                                             : QStringLiteral("Show Quick Preview\tCtrl+P"));
    menu.addSeparator();
    QAction* relatedAct = menu.addAction(QStringLiteral("Find Similar Documents\tCtrl+R"));
    const bool pinned = core::SettingsManager::instance().isFilePinned(item.path);
    QAction* pinAct = menu.addAction(pinned
                                         ? QStringLiteral("Unpin from Favorites\tCtrl+D")
                                         : QStringLiteral("Pin to Favorites\tCtrl+D"));
    menu.addSeparator();
    QAction* copyPathAct = menu.addAction(QStringLiteral("Copy Full Path\tCtrl+Shift+C"));
    QAction* copyNameAct = menu.addAction(QStringLiteral("Copy Filename"));

    QAction* chosen = menu.exec(mapToGlobal(localPos));
    if (chosen == openAct) {
        emit resultActivated(item);
    } else if (chosen == revealAct) {
        emit revealInExplorerRequested(item);
    } else if (chosen == previewAct) {
        togglePreview();
    } else if (chosen == relatedAct) {
        emit findRelatedRequested(item);
    } else if (chosen == pinAct) {
        emit togglePinRequested(item);
    } else if (chosen == copyPathAct) {
        emit copyPathRequested(item);
    } else if (chosen == copyNameAct) {
        if (auto* cb = QGuiApplication::clipboard()) {
            cb->setText(item.name);
        }
    }
}

void SearchPopupWidget::paintEvent(QPaintEvent* /*event*/) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const qreal cornerRadius = 11.0;

    // Subtle elevation shadow
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_darkTheme ? QColor(0, 0, 0, 90) : QColor(15, 23, 42, 18));
    painter.drawRoundedRect(bounds.adjusted(0.0, 1.5, 0.0, 1.5), cornerRadius, cornerRadius);

    // Card surface and border
    const QColor surfaceColor = m_darkTheme ? QColor(30, 34, 42, 248) : QColor(255, 255, 255, 252);
    const QColor borderColor = m_darkTheme ? QColor(56, 65, 82) : QColor(218, 224, 233);
    const QColor primaryText = m_darkTheme ? QColor(241, 245, 249) : QColor(30, 41, 59);
    const QColor secondaryText = m_darkTheme ? QColor(148, 163, 184) : QColor(100, 116, 139);
    const QColor badgeBg = m_darkTheme ? QColor(37, 99, 235, 38) : QColor(239, 246, 255);
    const QColor badgeBorder = m_darkTheme ? QColor(59, 130, 246, 80) : QColor(191, 219, 254);
    const QColor accentBlue = m_darkTheme ? QColor(96, 165, 250) : QColor(37, 99, 235);

    QPen borderPen(borderColor, 1.0);
    painter.setPen(borderPen);
    painter.setBrush(surfaceColor);
    painter.drawRoundedRect(bounds.adjusted(0.0, 0.0, 0.0, -1.5), cornerRadius, cornerRadius);

    // 1. Top Search Mode / Related Header Bar
    QFont pillFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold);
    painter.setFont(pillFont);

    if (!m_relatedSourceFileName.isEmpty()) {
        painter.setPen(m_darkTheme ? QColor(196, 181, 253) : QColor(109, 40, 217));
        const QFontMetrics fm(pillFont);
        const QString banner = fm.elidedText(
            QStringLiteral("Similar to: %1").arg(m_relatedSourceFileName),
            Qt::ElideMiddle,
            width() - 95);
        painter.drawText(QRectF(14.0, 6.0, width() - 95.0, 20.0), Qt::AlignLeft | Qt::AlignVCenter, banner);

        const QRectF backRect = previewToggleRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_darkTheme ? QColor(51, 65, 85) : QColor(241, 245, 249));
        painter.drawRoundedRect(backRect, 5.0, 5.0);
        painter.setPen(primaryText);
        painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("× Back"));
    } else {
        static const QString kModeNames[4] = {
            QStringLiteral("All"),
            QStringLiteral("Files"),
            QStringLiteral("Content"),
            QStringLiteral("Semantic")
        };
        for (int i = 0; i < 4; ++i) {
            const QRectF pRect = modePillRect(i);
            const bool active = (static_cast<int>(m_searchMode) == i);
            if (active) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(m_darkTheme ? QColor(37, 99, 235, 90) : QColor(219, 234, 254));
                painter.drawRoundedRect(pRect, 5.0, 5.0);
                painter.setPen(m_darkTheme ? QColor(147, 197, 253) : QColor(29, 78, 216));
            } else {
                painter.setPen(secondaryText);
            }
            painter.drawText(pRect, Qt::AlignCenter, kModeNames[i]);
        }

        // Right-side Quick Preview toggle pill
        const QRectF prevRect = previewToggleRect();
        if (m_previewVisible) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(m_darkTheme ? QColor(16, 185, 129, 65) : QColor(209, 250, 229));
            painter.drawRoundedRect(prevRect, 5.0, 5.0);
            painter.setPen(m_darkTheme ? QColor(110, 231, 183) : QColor(4, 120, 87));
        } else {
            painter.setPen(QPen(m_darkTheme ? QColor(51, 65, 85) : QColor(226, 232, 240), 1.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(prevRect, 5.0, 5.0);
            painter.setPen(secondaryText);
        }
        painter.drawText(prevRect, Qt::AlignCenter, QStringLiteral("Preview"));
    }

    // Header divider line
    painter.setPen(QPen(m_darkTheme ? QColor(45, 53, 68) : QColor(241, 245, 249), 1.0));
    painter.drawLine(QPointF(bounds.left() + 10.0, kHeaderHeight),
                     QPointF(bounds.right() - 10.0, kHeaderHeight));

    // 2. Empty State
    if (resultCount() == 0) {
        const qreal emptyTop = kHeaderHeight + 14.0;
        const QRectF iconCircle(bounds.left() + 18.0, emptyTop + 4.0, 32.0, 32.0);
        painter.setPen(QPen(badgeBorder, 1.0));
        painter.setBrush(badgeBg);
        painter.drawEllipse(iconCircle);

        QFont badgeFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
        painter.setFont(badgeFont);
        painter.setPen(accentBlue);
        painter.drawText(iconCircle, Qt::AlignCenter, QStringLiteral("i"));

        QFont titleFont(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(primaryText);
        const QRectF titleRect(iconCircle.right() + 14.0,
                               emptyTop,
                               bounds.width() - (iconCircle.right() - bounds.left()) - 18.0,
                               22.0);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, statusHeadline());

        QFont subFont(QStringLiteral("Segoe UI"), 8.5, QFont::Normal);
        painter.setFont(subFont);
        painter.setPen(secondaryText);
        const QRectF subRect(iconCircle.right() + 14.0,
                             titleRect.bottom() + 1.0,
                             titleRect.width(),
                             20.0);
        const QString subtitle = (database::DatabaseManager::instance().fileCount() == 0)
                                     ? (m_indexingInProgress
                                            ? QStringLiteral("Scanning approved folders in the background...")
                                            : QStringLiteral("Local file indexing is initializing."))
                                     : QStringLiteral("Try another query or filter (ext:pdf, type:doc, modified:7d).");
        painter.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, subtitle);
    }

    // 3. Optional Quick Preview Pane
    const qreal footerTop = bounds.bottom() - kFooterHeight - 1.0;
    if (m_previewVisible && resultCount() > 0) {
        const qreal previewTop = footerTop - kPreviewHeight;
        painter.setPen(QPen(m_darkTheme ? QColor(45, 53, 68) : QColor(226, 232, 240), 1.0));
        painter.drawLine(QPointF(bounds.left() + 10.0, previewTop),
                         QPointF(bounds.right() - 10.0, previewTop));

        const QRectF prevBox(bounds.left() + 10.0, previewTop + 6.0, bounds.width() - 20.0, kPreviewHeight - 12.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_darkTheme ? QColor(22, 26, 34, 210) : QColor(248, 250, 252));
        painter.drawRoundedRect(prevBox, 7.0, 7.0);

        if (const auto* sel = selectedResult()) {
            QFont hdrFont(QStringLiteral("Segoe UI"), 8, QFont::DemiBold);
            painter.setFont(hdrFont);
            painter.setPen(primaryText);
            const QFontMetrics fmHdr(hdrFont);
            const QString nativePath = QDir::toNativeSeparators(sel->path);
            const QString elidedPath = fmHdr.elidedText(nativePath, Qt::ElideMiddle, static_cast<int>(prevBox.width() - 16.0));
            painter.drawText(QRectF(prevBox.left() + 8.0, prevBox.top() + 4.0, prevBox.width() - 16.0, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elidedPath);

            QFont bodyFont(QStringLiteral("Segoe UI"), 8, QFont::Normal);
            painter.setFont(bodyFont);
            painter.setPen(secondaryText);
            const QString body = m_previewText.isEmpty()
                                     ? (sel->snippet.isEmpty() ? QStringLiteral("No text preview available.") : sel->snippet)
                                     : m_previewText;
            QString cleanBody = body;
            cleanBody.replace(QChar(0x00AB), QString());
            cleanBody.replace(QChar(0x00BB), QString());
            painter.drawText(QRectF(prevBox.left() + 8.0, prevBox.top() + 22.0, prevBox.width() - 16.0, prevBox.height() - 26.0),
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             cleanBody);
        }
    }

    // 4. Bottom divider & status footer bar
    painter.setPen(QPen(m_darkTheme ? QColor(45, 53, 68) : QColor(241, 245, 249), 1.0));
    painter.drawLine(QPointF(bounds.left() + 12.0, footerTop), QPointF(bounds.right() - 12.0, footerTop));

    QFont footerFont(QStringLiteral("Segoe UI"), 8, QFont::Normal);
    painter.setFont(footerFont);
    painter.setPen(secondaryText);
    const QRectF footerRect(bounds.left() + 14.0, footerTop + 1.0, bounds.width() - 28.0, kFooterHeight - 2.0);

    QString leftFooter = QStringLiteral("UshaLabs Lumen • Offline Mode");
    if (resultCount() > 0) {
        leftFooter = QStringLiteral("%1 %2 • %3 ms")
                         .arg(resultCount())
                         .arg(resultCount() == 1 ? QStringLiteral("result") : QStringLiteral("results"))
                         .arg(m_lastQueryMs, 0, 'f', 1);
    }
    painter.drawText(footerRect, Qt::AlignLeft | Qt::AlignVCenter, leftFooter);

    const QString rightFooter = (resultCount() > 0)
                                    ? QStringLiteral("Tab Mode  •  ^P Preview  •  ↵ Open")
                                    : QStringLiteral("Esc to close");
    painter.drawText(footerRect, Qt::AlignRight | Qt::AlignVCenter, rightFooter);
}

} // namespace lumen::ui
