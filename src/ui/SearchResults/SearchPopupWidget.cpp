#include "ui/SearchResults/SearchPopupWidget.h"

#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "ui/BrandingIcon.h"
#include "ui/Theme.h"

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

QString formatBreadcrumbPath(const QString& rawParentDir) {
    if (rawParentDir.isEmpty()) {
        return QString();
    }
    QString clean = QDir::fromNativeSeparators(rawParentDir);
    const QString home = QDir::fromNativeSeparators(QDir::homePath());
    if (!home.isEmpty() && clean.startsWith(home, Qt::CaseInsensitive)) {
        clean = clean.mid(home.size());
        while (clean.startsWith(QLatin1Char('/'))) {
            clean.remove(0, 1);
        }
        if (clean.isEmpty()) {
            return QStringLiteral("Home");
        }
    }
    clean.replace(QStringLiteral("/"), QStringLiteral(" / "));
    return clean;
}

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
                            const ThemeTokens& theme) {
    const QVector<SnippetSegment> segments = parseSnippetSegments(rawSnippet);
    if (segments.isEmpty()) {
        return;
    }

    const QFont normalFont = ThemeTokens::makeFont(12, QFont::Normal);
    const QFont mediumFont = ThemeTokens::makeFont(12, QFont::DemiBold);
    const QFontMetrics fmNormal(normalFont);
    const QFontMetrics fmMedium(mediumFont);

    const QColor normalColor = theme.textSecondary;
    const QColor hlTextColor = theme.brandPink;
    const QColor hlBgColor = theme.brandPinkTint;

    qreal curX = rect.left();
    const qreal maxX = rect.right();

    for (const SnippetSegment& seg : segments) {
        if (seg.text.isEmpty() || curX >= maxX - 8.0) {
            continue;
        }
        const QFontMetrics& fm = seg.highlighted ? fmMedium : fmNormal;
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
            painter->drawRoundedRect(
                QRectF(curX - 1.0, rect.top() + 1.0, advance + 2.0, rect.height() - 2.0), 3.0, 3.0);
            painter->setFont(mediumFont);
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

        const auto theme = ThemeTokens::get(m_darkTheme);
        // Generous horizontal padding (8px outer margin + 12px inner padding)
        const QRectF rowRect = QRectF(option.rect).adjusted(8.0, 2.0, -8.0, -2.0);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

        // Soft, low-contrast selected row surface (NO thick left pink bar)
        if (isSelected) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.brandPinkSelected);
            painter->drawRoundedRect(rowRect, 8.0, 8.0);
        } else if (isHovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.hoverSurface);
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

        const bool hasSnippet = !snippet.isEmpty();

        // 1. Small, refined 18x18 vector file-type icon (no large colorful badge)
        const qreal iconTop = hasSnippet ? (rowRect.top() + 10.0) : (rowRect.top() + 11.0);
        const QRectF iconRect(rowRect.left() + 12.0, iconTop, 18.0, 18.0);
        BrandingIcon::paintFileTypeIcon(painter, iconRect, ext, m_darkTheme, isSelected);

        const qreal textLeft = iconRect.right() + 10.0;
        const qreal rightMargin = rowRect.right() - 12.0;
        const qreal availWidth = std::max(80.0, rightMargin - textLeft);

        const QFont nameFont = ThemeTokens::makeFont(13, QFont::Medium);
        const QFont metaFont = ThemeTokens::makeFont(11, QFont::Normal);
        const QFontMetrics fmName(nameFont);
        const QFontMetrics fmMeta(metaFont);

        // 2. Right-side subtle metadata on Line 1:
        //    - Show Page N subtly when known (e.g., Page 2)
        //    - Show file size & date ONLY on hover or selection to keep inactive rows calm
        QString rightLine1Meta;
        if (matchedPage > 0) {
            rightLine1Meta = QStringLiteral("Page %1").arg(matchedPage);
        }
        if (isSelected || isHovered) {
            const QString sizeStr = formatHumanSize(fileSize);
            if (rightLine1Meta.isEmpty()) {
                rightLine1Meta = sizeStr;
            } else {
                rightLine1Meta = QStringLiteral("%1  ·  %2").arg(rightLine1Meta, sizeStr);
            }
        }

        int rightReservedWidth = 0;
        if (!rightLine1Meta.isEmpty()) {
            const int metaW = fmMeta.horizontalAdvance(rightLine1Meta) + 6;
            painter->setFont(metaFont);
            painter->setPen(theme.textSecondary);
            painter->drawText(QRectF(rightMargin - metaW, rowRect.top() + 7.0, metaW, 18.0),
                              Qt::AlignRight | Qt::AlignVCenter,
                              rightLine1Meta);
            rightReservedWidth += metaW + 8;
        }

        if (isPinned) {
            const QRectF pinRect(rightMargin - rightReservedWidth - 12.0, rowRect.top() + 10.0, 11.0, 11.0);
            BrandingIcon::paintPinIcon(painter, pinRect, theme.brandPink);
            rightReservedWidth += 18;
        }

        // 3. Line 1: Filename (13px Medium, dark charcoal)
        const int maxNameWidth = std::max(60, static_cast<int>(availWidth - rightReservedWidth));
        const QString elidedName = fmName.elidedText(fileName, Qt::ElideMiddle, maxNameWidth);
        const QRectF nameRect(textLeft, rowRect.top() + 6.0, maxNameWidth, 20.0);

        if (matchStart >= 0 && matchLen > 0 && elidedName == fileName &&
            matchStart + matchLen <= fileName.size()) {
            const QString prefix = fileName.left(matchStart);
            const int hlX = static_cast<int>(textLeft) + fmName.horizontalAdvance(prefix);
            const int hlW = fmName.horizontalAdvance(fileName.mid(matchStart, matchLen));
            painter->setPen(Qt::NoPen);
            painter->setBrush(isSelected ? theme.surfaceBackground : theme.brandPinkTint);
            painter->drawRoundedRect(QRectF(hlX - 1.0, rowRect.top() + 8.0, hlW + 2.0, 16.0), 3.0, 3.0);
        }

        painter->setFont(nameFont);
        painter->setPen(theme.textPrimary);
        painter->drawText(nameRect, Qt::AlignLeft | Qt::AlignVCenter, elidedName);

        // 4. Line 2: Clean breadcrumb folder path (`Documents / Finance`)
        painter->setFont(metaFont);
        painter->setPen(theme.textSecondary);

        int dateReserved = 0;
        if (isSelected || isHovered) {
            const QString dateStr = (mtime > 0)
                                        ? QDateTime::fromSecsSinceEpoch(mtime).toString(QStringLiteral("MMM d, yyyy"))
                                        : QString();
            if (!dateStr.isEmpty()) {
                dateReserved = fmMeta.horizontalAdvance(dateStr) + 10;
                painter->setPen(theme.textMuted);
                painter->drawText(QRectF(rightMargin - dateReserved, rowRect.top() + 26.0, dateReserved, 16.0),
                                  Qt::AlignRight | Qt::AlignVCenter,
                                  dateStr);
                painter->setPen(theme.textSecondary);
            }
        }

        const QString formattedDir = formatBreadcrumbPath(parentDir);
        const QString elidedDir = fmMeta.elidedText(
            formattedDir, Qt::ElideMiddle, std::max(60, static_cast<int>(availWidth - dateReserved)));
        painter->drawText(QRectF(textLeft, rowRect.top() + 26.0, availWidth - dateReserved, 16.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedDir);

        // 5. Line 3 (only for content / semantic results): 1 clean snippet line with subtle highlight
        if (hasSnippet) {
            const QRectF snippetRect(textLeft, rowRect.top() + 45.0, availWidth, 18.0);
            drawHighlightedSnippet(painter, snippetRect, snippet, theme);
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
        "QScrollBar:vertical { width: 5px; background: transparent; margin: 4px 2px; }"
        "QScrollBar::handle:vertical { background: rgba(148, 163, 184, 110); border-radius: 2px; min-height: 24px; }"
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
        // Must equal DesktopSearchBar::kPopupHeight (132) so phase0_tests kExpandedHeight invariant holds
        return kHeaderHeight + 96;
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
    return QStringLiteral("No matching files");
}

void SearchPopupWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateListLayout();
}

void SearchPopupWidget::updateListLayout() {
    if (m_listView != nullptr && m_listView->isVisible()) {
        const int previewAdd = (m_previewVisible && resultCount() > 0) ? kPreviewHeight : 0;
        const int listH = std::max(0, height() - kHeaderHeight - previewAdd - kFooterHeight - 6);
        m_listView->setGeometry(2, kHeaderHeight + 3, width() - 4, listH);
    }
}

QRectF SearchPopupWidget::modePillRect(int modeIndex) const {
    static const int widths[4] = {34, 44, 62, 68};
    qreal x = 14.0;
    for (int i = 0; i < modeIndex && i < 4; ++i) {
        x += widths[i] + 8.0;
    }
    return QRectF(x, 6.0, widths[modeIndex], kHeaderHeight - 6.0);
}

QRectF SearchPopupWidget::previewToggleRect() const {
    return QRectF(width() - 78.0, 7.0, 64.0, 22.0);
}

QRectF SearchPopupWidget::previewOpenButtonRect(const QRectF& previewPaneRect) const {
    return QRectF(previewPaneRect.right() - 188.0, previewPaneRect.top() + 8.0, 56.0, 24.0);
}

QRectF SearchPopupWidget::previewRevealButtonRect(const QRectF& previewPaneRect) const {
    return QRectF(previewPaneRect.right() - 126.0, previewPaneRect.top() + 8.0, 112.0, 24.0);
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
        } else if (m_previewVisible && resultCount() > 0) {
            const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
            const qreal footerTop = bounds.bottom() - kFooterHeight - 1.0;
            const qreal previewTop = footerTop - kPreviewHeight;
            const QRectF prevPane(bounds.left() + 14.0, previewTop, bounds.width() - 28.0, kPreviewHeight);
            if (const auto* sel = selectedResult()) {
                if (previewOpenButtonRect(prevPane).contains(pos)) {
                    emit resultActivated(*sel);
                    event->accept();
                    return;
                }
                if (previewRevealButtonRect(prevPane).contains(pos)) {
                    emit revealInExplorerRequested(*sel);
                    event->accept();
                    return;
                }
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void SearchPopupWidget::showContextMenuAt(const QPoint& localPos, const search::FileSearchResult& item) {
    const auto theme = ThemeTokens::get(m_darkTheme);
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background-color: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px; font-family: 'Segoe UI'; font-size: 9pt; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 5px; }"
        "QMenu::item:selected { background-color: %4; color: %5; font-weight: 600; }"
        "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
        .arg(theme.surfaceBackground.name(),
             theme.textPrimary.name(),
             theme.borderColor.name(),
             theme.brandPinkSelected.name(),
             theme.brandPink.name(),
             theme.borderSubtle.name()));

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

    const auto theme = ThemeTokens::get(m_darkTheme);
    const QRectF bounds = QRectF(rect()).adjusted(1.0, 1.0, -1.0, -1.0);
    const qreal cornerRadius = 14.0;

    // Minimal soft shadow
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.shadowColor);
    painter.drawRoundedRect(bounds.adjusted(0.0, 1.5, 0.0, 1.5), cornerRadius, cornerRadius);

    // Outer container surface and subtle border
    QPen borderPen(theme.borderColor, 1.0);
    painter.setPen(borderPen);
    painter.setBrush(theme.surfaceBackground);
    painter.drawRoundedRect(bounds.adjusted(0.0, 0.0, 0.0, -0.5), cornerRadius, cornerRadius);

    // 1. Top Search Mode Navigation (subtle Linear-style text tabs with crisp underline indicator)
    const QFont tabActiveFont = ThemeTokens::makeFont(12, QFont::DemiBold);
    const QFont tabInactiveFont = ThemeTokens::makeFont(12, QFont::Normal);

    if (!m_relatedSourceFileName.isEmpty()) {
        painter.setFont(tabActiveFont);
        painter.setPen(theme.textPrimary);
        const QFontMetrics fm(tabActiveFont);
        const QString banner = fm.elidedText(
            QStringLiteral("Similar to %1").arg(m_relatedSourceFileName),
            Qt::ElideMiddle,
            width() - 104);
        painter.drawText(QRectF(16.0, 6.0, width() - 104.0, 24.0), Qt::AlignLeft | Qt::AlignVCenter, banner);

        const QRectF backRect = previewToggleRect();
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.setBrush(theme.elevatedBackground);
        painter.drawRoundedRect(backRect, 5.0, 5.0);
        painter.setFont(tabInactiveFont);
        painter.setPen(theme.textSecondary);
        painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("← Back"));
    } else {
        static const QString kModeNames[4] = {
            QStringLiteral("All"),
            QStringLiteral("Files"),
            QStringLiteral("Content"),
            QStringLiteral("Semantic")
        };
        for (int i = 0; i < 4; ++i) {
            const QRectF tRect = modePillRect(i);
            const bool active = (static_cast<int>(m_searchMode) == i);
            painter.setFont(active ? tabActiveFont : tabInactiveFont);
            painter.setPen(active ? theme.textPrimary : theme.textSecondary);
            painter.drawText(QRectF(tRect.left(), 6.0, tRect.width(), 22.0), Qt::AlignCenter, kModeNames[i]);

            if (active) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(theme.brandPink);
                painter.drawRoundedRect(
                    QRectF(tRect.left() + 4.0, kHeaderHeight - 2.0, tRect.width() - 8.0, 2.0), 1.0, 1.0);
            }
        }

        // Right-side contextual Preview toggle
        const QRectF prevRect = previewToggleRect();
        painter.setFont(ThemeTokens::makeFont(11, m_previewVisible ? QFont::DemiBold : QFont::Normal));
        if (m_previewVisible) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.brandPinkSelected);
            painter.drawRoundedRect(prevRect, 5.0, 5.0);
            painter.setPen(theme.brandPink);
        } else {
            painter.setPen(theme.textSecondary);
        }
        painter.drawText(prevRect, Qt::AlignCenter, QStringLiteral("Preview"));
    }

    // Hairline header separator
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left() + 14.0, kHeaderHeight),
                     QPointF(bounds.right() - 14.0, kHeaderHeight));

    // 2. Empty / Loading State (clean typography, no decorative circles)
    if (resultCount() == 0) {
        const qreal emptyTop = kHeaderHeight + 18.0;
        const QFont titleFont = ThemeTokens::makeFont(13, QFont::Medium);
        painter.setFont(titleFont);
        painter.setPen(theme.textPrimary);
        const QRectF titleRect(bounds.left() + 18.0, emptyTop, bounds.width() - 36.0, 20.0);
        painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, statusHeadline());

        const QFont subFont = ThemeTokens::makeFont(12, QFont::Normal);
        painter.setFont(subFont);
        painter.setPen(theme.textSecondary);
        const QRectF subRect(bounds.left() + 18.0, titleRect.bottom() + 3.0, bounds.width() - 36.0, 18.0);
        const QString subtitle = (database::DatabaseManager::instance().fileCount() == 0)
                                     ? (m_indexingInProgress
                                            ? QStringLiteral("Indexing folders in the background...")
                                            : QStringLiteral("Local file index is initializing."))
                                     : QStringLiteral("Try a shorter keyword or press Tab to switch search modes.");
        painter.drawText(subRect, Qt::AlignLeft | Qt::AlignVCenter, subtitle);
    }

    // 3. Optional Quick Preview Section (clean horizontal separator, no nested rounded card)
    const qreal footerTop = bounds.bottom() - kFooterHeight - 1.0;
    if (m_previewVisible && resultCount() > 0) {
        const qreal previewTop = footerTop - kPreviewHeight;
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.drawLine(QPointF(bounds.left() + 14.0, previewTop),
                         QPointF(bounds.right() - 14.0, previewTop));

        const QRectF prevPane(bounds.left() + 16.0, previewTop + 2.0, bounds.width() - 32.0, kPreviewHeight - 4.0);

        if (const auto* sel = selectedResult()) {
            const QRectF openBtn = previewOpenButtonRect(prevPane);
            const QRectF revealBtn = previewRevealButtonRect(prevPane);

            // Action buttons in preview header: Open & Reveal
            const QFont btnFont = ThemeTokens::makeFont(11, QFont::Medium);
            painter.setFont(btnFont);

            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.brandPink);
            painter.drawRoundedRect(openBtn, 5.0, 5.0);
            painter.setPen(QColor(255, 255, 255));
            painter.drawText(openBtn, Qt::AlignCenter, QStringLiteral("Open"));

            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.elevatedBackground);
            painter.drawRoundedRect(revealBtn, 5.0, 5.0);
            painter.setPen(theme.textPrimary);
            painter.drawText(revealBtn, Qt::AlignCenter, QStringLiteral("Reveal in Explorer"));

            // Full filename & location metadata
            const QFont hdrFont = ThemeTokens::makeFont(13, QFont::DemiBold);
            painter.setFont(hdrFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmHdr(hdrFont);
            const int maxTitleW = std::max(100, static_cast<int>(openBtn.left() - prevPane.left() - 12.0));
            const QString elidedName = fmHdr.elidedText(sel->name, Qt::ElideMiddle, maxTitleW);
            painter.drawText(QRectF(prevPane.left(), prevPane.top() + 6.0, maxTitleW, 18.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elidedName);

            const QFont locFont = ThemeTokens::makeFont(11, QFont::Normal);
            painter.setFont(locFont);
            painter.setPen(theme.textSecondary);
            const QFontMetrics fmLoc(locFont);
            QString locMeta = formatBreadcrumbPath(sel->parentDirectory);
            if (sel->matchedPage > 0) {
                locMeta = QStringLiteral("%1  ·  Page %2").arg(locMeta).arg(sel->matchedPage);
            }
            locMeta = QStringLiteral("%1  ·  %2").arg(locMeta, formatHumanSize(sel->size));
            const QString elidedLoc = fmLoc.elidedText(locMeta, Qt::ElideMiddle, maxTitleW);
            painter.drawText(QRectF(prevPane.left(), prevPane.top() + 24.0, maxTitleW, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             elidedLoc);

            // Readable extracted document passage
            const QFont bodyFont = ThemeTokens::makeFont(12, QFont::Normal);
            painter.setFont(bodyFont);
            painter.setPen(theme.textPrimary);
            const QString body = m_previewText.isEmpty()
                                     ? (sel->snippet.isEmpty() ? QStringLiteral("No document text preview available for this file.") : sel->snippet)
                                     : m_previewText;
            QString cleanBody = body;
            cleanBody.replace(QChar(0x00AB), QString());
            cleanBody.replace(QChar(0x00BB), QString());
            cleanBody = cleanBody.simplified();

            painter.drawText(
                QRectF(prevPane.left(), prevPane.top() + 46.0, prevPane.width(), prevPane.height() - 50.0),
                Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                cleanBody);
        }
    }

    // 4. Minimal Utility Footer
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left() + 14.0, footerTop), QPointF(bounds.right() - 14.0, footerTop));

    const QFont footerFont = ThemeTokens::makeFont(11, QFont::Normal);
    painter.setFont(footerFont);
    painter.setPen(theme.textSecondary);
    const QRectF footerRect(bounds.left() + 16.0, footerTop + 1.0, bounds.width() - 32.0, kFooterHeight - 2.0);

    QString leftFooter = m_indexingInProgress
                             ? QStringLiteral("Indexing files...")
                             : QStringLiteral("Ready");
    if (resultCount() > 0) {
        leftFooter = QStringLiteral("%1 %2  ·  %3 ms")
                         .arg(resultCount())
                         .arg(resultCount() == 1 ? QStringLiteral("result") : QStringLiteral("results"))
                         .arg(m_lastQueryMs, 0, 'f', 1);
    }
    painter.drawText(footerRect, Qt::AlignLeft | Qt::AlignVCenter, leftFooter);

    const QString rightFooter = (resultCount() > 0)
                                    ? QStringLiteral("↵ Open   ^↵ Reveal   ^P Preview   Tab Mode")
                                    : QStringLiteral("Esc Close");
    painter.setPen(theme.textMuted);
    painter.drawText(footerRect, Qt::AlignRight | Qt::AlignVCenter, rightFooter);
}

} // namespace lumen::ui
