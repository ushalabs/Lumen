#include "ui/SearchResults/SearchPopupWidget.h"

#include "database/DatabaseManager.h"

#include <QColor>
#include <QDateTime>
#include <QFont>
#include <QFontMetrics>
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

QVector<SnippetSegment> parseSnippetSegments(const QString& rawSnippet) {
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

    // Small quote/snippet accent bar on the left
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
            painter->setBrush(m_darkTheme ? QColor(37, 99, 235, 50) : QColor(239, 246, 255));
            painter->drawRoundedRect(rowRect, 8.0, 8.0);

            // Left selection indicator bar
            painter->setBrush(m_darkTheme ? QColor(96, 165, 250) : QColor(37, 99, 235));
            painter->drawRoundedRect(QRectF(rowRect.left() + 2.0, rowRect.top() + 10.0, 3.0, rowRect.height() - 20.0),
                                     1.5, 1.5);
        } else if (isHovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(255, 255, 255, 12) : QColor(248, 250, 252));
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

        // 2. Top-Right: Optional Content / Page pill + File size label
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
            const QString pillLabel = (matchedPage > 0)
                                          ? QStringLiteral("Page %1").arg(matchedPage)
                                          : QStringLiteral("Content");
            QFont pillFont(QStringLiteral("Segoe UI"), 7, QFont::DemiBold);
            const QFontMetrics fmPill(pillFont);
            const int pillW = fmPill.horizontalAdvance(pillLabel) + 12;
            const QRectF pillRect(rightMargin - rightReservedWidth - pillW,
                                  rowRect.top() + 6.0,
                                  pillW,
                                  16.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(16, 185, 129, 45) : QColor(209, 250, 229));
            painter->drawRoundedRect(pillRect, 4.0, 4.0);
            painter->setFont(pillFont);
            painter->setPen(m_darkTheme ? QColor(110, 231, 183) : QColor(4, 120, 87));
            painter->drawText(pillRect, Qt::AlignCenter, pillLabel);
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
    } else {
        m_listView->hide();
    }
    updateListLayout();
    update();
}

void SearchPopupWidget::clearResults() {
    m_lastQueryMs = 0.0;
    m_model->clear();
    m_listView->hide();
    update();
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
        return 112;
    }
    const int visibleRows = qMin(count, kMaxVisibleRows);
    int rowsHeight = 0;
    const auto& items = m_model->results();
    for (int i = 0; i < visibleRows && i < items.size(); ++i) {
        rowsHeight += items.at(i).snippet.isEmpty() ? kRowHeight : kContentRowHeight;
    }
    return 10 + rowsHeight + kFooterHeight;
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
        m_listView->setGeometry(2, 6, width() - 4, height() - kFooterHeight - 8);
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

    if (resultCount() == 0) {
        const QRectF iconCircle(bounds.left() + 18.0, bounds.top() + 22.0, 32.0, 32.0);
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
                               bounds.top() + 18.0,
                               bounds.width() - (iconCircle.right() - bounds.left()) - 18.0,
                               22.0);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, statusHeadline());

        QFont subFont(QStringLiteral("Segoe UI"), 9, QFont::Normal);
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
                                     : QStringLiteral("Try another partial filename or extension filter (ext:pdf).");
        painter.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, subtitle);
    }

    // Bottom divider & status footer bar
    const qreal footerTop = bounds.bottom() - kFooterHeight - 1.0;
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
                                    ? QStringLiteral("↑↓ Navigate  •  ↵ Open  •  Esc Close")
                                    : QStringLiteral("Esc to dismiss");
    painter.drawText(footerRect, Qt::AlignRight | Qt::AlignVCenter, rightFooter);
}

} // namespace lumen::ui
