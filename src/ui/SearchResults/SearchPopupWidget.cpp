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
#include <QHash>
#include <QItemSelectionModel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
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
        return QStringLiteral("%1 KB").arg(static_cast<double>(bytes) / 1024.0, 0, 'f', 1);
    }
    if (bytes < 1024LL * 1024LL * 1024LL) {
        return QStringLiteral("%1 MB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0), 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}

QString formatTimestamp(qint64 mtimeEpoch) {
    if (mtimeEpoch <= 0) {
        return QStringLiteral("Unavailable");
    }
    const QDateTime dt = QDateTime::fromSecsSinceEpoch(mtimeEpoch);
    const QDate today = QDate::currentDate();
    if (dt.date() == today) {
        return QStringLiteral("Today, %1").arg(QLocale::system().toString(dt.time(), QLocale::ShortFormat));
    }
    if (dt.date() == today.addDays(-1)) {
        return QStringLiteral("Yesterday, %1").arg(QLocale::system().toString(dt.time(), QLocale::ShortFormat));
    }
    return dt.toString(QStringLiteral("MMM dd, yyyy"));
}

QString humanReadableFileType(const QString& extLower) {
    static const QHash<QString, QString> kTypeMap = {
        {QStringLiteral("pdf"), QStringLiteral("PDF Document")},
        {QStringLiteral("docx"), QStringLiteral("Microsoft Word Document")},
        {QStringLiteral("doc"), QStringLiteral("Microsoft Word Document")},
        {QStringLiteral("xlsx"), QStringLiteral("Excel Spreadsheet")},
        {QStringLiteral("xls"), QStringLiteral("Excel Spreadsheet")},
        {QStringLiteral("csv"), QStringLiteral("CSV Data Table")},
        {QStringLiteral("pptx"), QStringLiteral("PowerPoint Presentation")},
        {QStringLiteral("md"), QStringLiteral("Markdown Document")},
        {QStringLiteral("txt"), QStringLiteral("Plain Text Document")},
        {QStringLiteral("log"), QStringLiteral("Diagnostic Log File")},
        {QStringLiteral("cpp"), QStringLiteral("C++ Source File")},
        {QStringLiteral("cc"), QStringLiteral("C++ Source File")},
        {QStringLiteral("cxx"), QStringLiteral("C++ Source File")},
        {QStringLiteral("h"), QStringLiteral("C/C++ Header File")},
        {QStringLiteral("hpp"), QStringLiteral("C++ Header File")},
        {QStringLiteral("c"), QStringLiteral("C Source File")},
        {QStringLiteral("rs"), QStringLiteral("Rust Source File")},
        {QStringLiteral("py"), QStringLiteral("Python Script")},
        {QStringLiteral("js"), QStringLiteral("JavaScript Source File")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript Source File")},
        {QStringLiteral("json"), QStringLiteral("JSON Configuration")},
        {QStringLiteral("yaml"), QStringLiteral("YAML Configuration")},
        {QStringLiteral("yml"), QStringLiteral("YAML Configuration")},
        {QStringLiteral("xml"), QStringLiteral("XML Document")},
        {QStringLiteral("html"), QStringLiteral("HTML Document")},
        {QStringLiteral("css"), QStringLiteral("CSS Stylesheet")},
        {QStringLiteral("sql"), QStringLiteral("SQL Script")},
        {QStringLiteral("db"), QStringLiteral("SQLite Database")},
        {QStringLiteral("sqlite"), QStringLiteral("SQLite Database")},
        {QStringLiteral("png"), QStringLiteral("PNG Image")},
        {QStringLiteral("jpg"), QStringLiteral("JPEG Image")},
        {QStringLiteral("jpeg"), QStringLiteral("JPEG Image")},
        {QStringLiteral("svg"), QStringLiteral("SVG Vector Graphic")},
        {QStringLiteral("webp"), QStringLiteral("WebP Image")},
        {QStringLiteral("ico"), QStringLiteral("Windows Icon")},
        {QStringLiteral("exe"), QStringLiteral("Windows Executable")},
        {QStringLiteral("dll"), QStringLiteral("Dynamic Link Library")},
        {QStringLiteral("msi"), QStringLiteral("Windows Installer Package")},
        {QStringLiteral("zip"), QStringLiteral("ZIP Archive")}
    };

    const auto it = kTypeMap.constFind(extLower);
    if (it != kTypeMap.constEnd()) {
        return it.value();
    }
    if (extLower.isEmpty()) {
        return QStringLiteral("File");
    }
    return QStringLiteral("%1 File").arg(extLower.toUpper());
}

bool isSourceCodeExtension(const QString& extLower) {
    return extLower == QStringLiteral("cpp") || extLower == QStringLiteral("h") ||
           extLower == QStringLiteral("hpp") || extLower == QStringLiteral("c") ||
           extLower == QStringLiteral("rs") || extLower == QStringLiteral("py") ||
           extLower == QStringLiteral("js") || extLower == QStringLiteral("ts") ||
           extLower == QStringLiteral("json") || extLower == QStringLiteral("sql") ||
           extLower == QStringLiteral("yaml") || extLower == QStringLiteral("yml") ||
           extLower == QStringLiteral("xml") || extLower == QStringLiteral("html") ||
           extLower == QStringLiteral("css");
}

QString sectionCategoryName(const search::FileSearchResult& item) {
    if (item.isPinned) {
        return QStringLiteral("PINNED");
    }
    switch (item.matchType) {
        case search::SearchResultMatchType::Semantic:
            return QStringLiteral("SEMANTIC MATCHES");
        case search::SearchResultMatchType::Hybrid:
        case search::SearchResultMatchType::Content:
        case search::SearchResultMatchType::FilenameAndContent:
            return QStringLiteral("CONTENT MATCHES");
        case search::SearchResultMatchType::Filename:
        default:
            return QStringLiteral("FILES");
    }
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

    const QFont normalFont = ThemeTokens::makeFont(11, QFont::Normal);
    const QFont mediumFont = ThemeTokens::makeFont(11, QFont::DemiBold);
    const QFontMetrics fmNormal(normalFont);
    const QFontMetrics fmMedium(mediumFont);

    qreal curX = rect.left();
    const qreal maxX = rect.right();

    // Subtle 2px pink left accent bar on the snippet line
    painter->setPen(Qt::NoPen);
    painter->setBrush(theme.brandPink);
    painter->drawRoundedRect(QRectF(curX, rect.top() + 3.0, 2.0, rect.height() - 6.0), 1.0, 1.0);
    curX += 6.0;

    for (const SnippetSegment& seg : segments) {
        if (seg.text.isEmpty() || curX >= maxX - 6.0) {
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
            painter->setBrush(theme.brandPinkStrongFill);
            painter->drawRoundedRect(
                QRectF(curX - 1.0, rect.top() + 1.0, advance + 2.0, rect.height() - 2.0), 3.0, 3.0);
            painter->setFont(mediumFont);
            painter->setPen(theme.textPrimary);
        } else {
            painter->setFont(normalFont);
            painter->setPen(theme.textSecondary);
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
    const int row = index.row();
    const auto& item = m_results.at(row);
    switch (role) {
        case Qt::DisplayRole:
        case FileNameRole:           return item.name;
        case FullPathRole:           return item.path;
        case ParentDirRole:          return item.parentDirectory;
        case ExtensionRole:          return item.extension;
        case SizeRole:               return item.size;
        case MtimeRole:              return item.mtime;
        case MatchStartRole:         return item.matchStart;
        case MatchLengthRole:        return item.matchLength;
        case SnippetRole:            return item.snippet;
        case MatchedPageRole:        return item.matchedPage;
        case MatchTypeRole:          return static_cast<int>(item.matchType);
        case SemanticSimilarityRole: return item.semanticSimilarity;
        case IsPinnedRole:           return item.isPinned;
        case GroupHeaderTitleRole:   return (row < m_groupTitles.size()) ? m_groupTitles.at(row) : QString();
        case GroupHeaderCountRole:   return (row < m_groupCounts.size()) ? m_groupCounts.at(row) : 0;
        default:                     return {};
    }
}

void SearchResultListModel::setResults(QVector<search::FileSearchResult> results) {
    beginResetModel();
    m_results = std::move(results);
    m_groupTitles.resize(m_results.size());
    m_groupCounts.resize(m_results.size());

    int i = 0;
    while (i < m_results.size()) {
        const QString cat = sectionCategoryName(m_results.at(i));
        int j = i + 1;
        while (j < m_results.size() && sectionCategoryName(m_results.at(j)) == cat) {
            m_groupTitles[j].clear();
            m_groupCounts[j] = 0;
            ++j;
        }
        m_groupTitles[i] = cat;
        m_groupCounts[i] = (j - i);
        i = j;
    }
    endResetModel();
}

void SearchResultListModel::clear() {
    if (m_results.isEmpty()) {
        return;
    }
    beginResetModel();
    m_results.clear();
    m_groupTitles.clear();
    m_groupCounts.clear();
    endResetModel();
}

const search::FileSearchResult* SearchResultListModel::resultAt(int row) const noexcept {
    if (row < 0 || row >= m_results.size()) {
        return nullptr;
    }
    return &m_results.at(row);
}

// ------------------------------------------------------------------------------
// SearchResultItemDelegate (Gemini Left Pane Grouped Result Cards)
// ------------------------------------------------------------------------------
class SearchResultItemDelegate : public QStyledItemDelegate {
public:
    explicit SearchResultItemDelegate(QObject* parent = nullptr)
        : QStyledItemDelegate(parent) {}

    void setDarkTheme(bool dark) { m_darkTheme = dark; }

    QSize sizeHint(const QStyleOptionViewItem& /*option*/, const QModelIndex& index) const override {
        const QString snippet = index.data(SearchResultListModel::SnippetRole).toString();
        const QString groupTitle = index.data(SearchResultListModel::GroupHeaderTitleRole).toString();
        const int baseH = snippet.isEmpty() ? SearchPopupWidget::kRowHeight : SearchPopupWidget::kContentRowHeight;
        const int extraH = groupTitle.isEmpty() ? 0 : SearchPopupWidget::kGroupHeaderExtraHeight;
        return QSize(480, baseH + extraH);
    }

    void paint(QPainter* painter,
               const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        painter->setRenderHint(QPainter::TextAntialiasing, true);

        const auto theme = ThemeTokens::get(false);
        const QString groupTitle = index.data(SearchResultListModel::GroupHeaderTitleRole).toString();
        const int groupCount = index.data(SearchResultListModel::GroupHeaderCountRole).toInt();

        QRectF fullRect = QRectF(option.rect);
        if (!groupTitle.isEmpty()) {
            const QRectF hdrRect(fullRect.left() + 14.0,
                                 fullRect.top() + 4.0,
                                 fullRect.width() - 28.0,
                                 SearchPopupWidget::kGroupHeaderExtraHeight - 4.0);
            painter->setFont(ThemeTokens::makeFont(10, QFont::DemiBold));
            painter->setPen(theme.textSecondary);
            painter->drawText(hdrRect, Qt::AlignLeft | Qt::AlignVCenter, groupTitle);

            painter->setFont(ThemeTokens::makeMonoFont(10, QFont::Normal));
            painter->setPen(theme.textMuted);
            painter->drawText(hdrRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(groupCount));

            fullRect.setTop(fullRect.top() + SearchPopupWidget::kGroupHeaderExtraHeight);
        }

        const QRectF cardRect = fullRect.adjusted(8.0, 2.0, -8.0, -3.0);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

        // Pure White & Clean Light Pink Result Card
        if (isSelected) {
            painter->setBrush(theme.brandPinkSelected);
            painter->setPen(QPen(theme.brandPink, 1.3));
            painter->drawRoundedRect(cardRect, 10.0, 10.0);
        } else if (isHovered) {
            painter->setPen(QPen(theme.borderColor, 1.0));
            painter->setBrush(theme.brandPinkTint);
            painter->drawRoundedRect(cardRect, 10.0, 10.0);
        } else {
            painter->setPen(QPen(theme.borderSubtle, 1.0));
            painter->setBrush(theme.surfaceBackground);
            painter->drawRoundedRect(cardRect, 10.0, 10.0);
        }

        const QString fileName = index.data(SearchResultListModel::FileNameRole).toString();
        const QString fullPath = index.data(SearchResultListModel::FullPathRole).toString();
        const QString ext = index.data(SearchResultListModel::ExtensionRole).toString().toLower();
        const qint64 fileSize = index.data(SearchResultListModel::SizeRole).toLongLong();
        const int matchStart = index.data(SearchResultListModel::MatchStartRole).toInt();
        const int matchLen = index.data(SearchResultListModel::MatchLengthRole).toInt();
        const QString snippet = index.data(SearchResultListModel::SnippetRole).toString();
        const int matchedPage = index.data(SearchResultListModel::MatchedPageRole).toInt();
        const bool isPinned = index.data(SearchResultListModel::IsPinnedRole).toBool();

        // 1. Left 32x32 rounded light-pink file icon tile
        const QRectF iconBox(cardRect.left() + 10.0, cardRect.top() + 10.0, 32.0, 32.0);
        BrandingIcon::paintFileTypeIcon(painter, iconBox, ext, false, isSelected);

        const qreal textLeft = iconBox.right() + 10.0;
        const qreal rightMargin = cardRect.right() - 10.0;
        const qreal availWidth = std::max(80.0, rightMargin - textLeft);

        // 2. Right-side file size
        const QFont monoSmall = ThemeTokens::makeMonoFont(10, QFont::Normal);
        const QFontMetrics fmMonoSmall(monoSmall);
        const QString sizeStr = formatHumanSize(fileSize);
        const int sizeW = fmMonoSmall.horizontalAdvance(sizeStr) + 6;
        painter->setFont(monoSmall);
        painter->setPen(theme.textMuted);
        painter->drawText(QRectF(rightMargin - sizeW, cardRect.top() + 8.0, sizeW, 18.0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          sizeStr);

        int rightReserved = sizeW + 8;

        // 3. Optional subtle badge ONLY for Pinned or specific PDF Page (no Filename Match / Cosine!)
        QString badgeText;
        if (isPinned) {
            badgeText = (matchedPage > 0) ? QStringLiteral("Pinned · p.%1").arg(matchedPage) : QStringLiteral("Pinned");
        } else if (matchedPage > 0) {
            badgeText = QStringLiteral("Page %1").arg(matchedPage);
        }

        const QFont badgeFont = ThemeTokens::makeFont(10, QFont::Medium);
        const QFontMetrics fmBadge(badgeFont);
        const int badgeW = badgeText.isEmpty() ? 0 : (fmBadge.horizontalAdvance(badgeText) + 14);
        const int badgeGap = badgeText.isEmpty() ? 0 : 8;

        // 4. Filename on Line 1 (Crisp Black)
        const QFont nameFont = ThemeTokens::makeFont(12, QFont::DemiBold);
        const QFontMetrics fmName(nameFont);
        const int maxNameW = std::max(60, static_cast<int>(availWidth - rightReserved - badgeW - badgeGap));
        const QString elidedName = fmName.elidedText(fileName, Qt::ElideMiddle, maxNameW);
        const int actualNameW = fmName.horizontalAdvance(elidedName);

        if (matchStart >= 0 && matchLen > 0 && elidedName == fileName &&
            matchStart + matchLen <= fileName.size()) {
            const QString prefix = fileName.left(matchStart);
            const int hlX = static_cast<int>(textLeft) + fmName.horizontalAdvance(prefix);
            const int hlW = fmName.horizontalAdvance(fileName.mid(matchStart, matchLen));
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.brandPinkStrongFill);
            painter->drawRoundedRect(QRectF(hlX - 1.0, cardRect.top() + 9.0, hlW + 2.0, 16.0), 3.0, 3.0);
        }

        painter->setFont(nameFont);
        painter->setPen(theme.textPrimary);
        painter->drawText(QRectF(textLeft, cardRect.top() + 7.0, actualNameW + 4, 20.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedName);

        if (!badgeText.isEmpty()) {
            const qreal badgeLeft = std::min(textLeft + actualNameW + 8.0, rightMargin - rightReserved - badgeW);
            const QRectF badgeRect(badgeLeft, cardRect.top() + 9.0, badgeW, 17.0);
            painter->setPen(QPen(theme.borderColor, 1.0));
            painter->setBrush(theme.brandPinkStrongFill);
            painter->drawRoundedRect(badgeRect, 5.0, 5.0);
            painter->setFont(badgeFont);
            painter->setPen(theme.textPrimary);
            painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
        }

        // 5. Line 2: Monospaced Windows path
        const QFont pathFont = ThemeTokens::makeMonoFont(10, QFont::Normal);
        const QFontMetrics fmPath(pathFont);
        const QString nativePath = QDir::toNativeSeparators(fullPath);
        const QString elidedPath = fmPath.elidedText(nativePath, Qt::ElideMiddle, static_cast<int>(availWidth));
        painter->setFont(pathFont);
        painter->setPen(theme.textSecondary);
        painter->drawText(QRectF(textLeft, cardRect.top() + 27.0, availWidth, 16.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedPath);

        // 6. Line 3 (when snippet exists): Compact in-row snippet preview
        if (!snippet.isEmpty()) {
            const QRectF snipRect(textLeft, cardRect.top() + 45.0, availWidth, 16.0);
            drawHighlightedSnippet(painter, snipRect, snippet, theme);
        }

        painter->restore();
    }

private:
    bool m_darkTheme = true;
};

// ------------------------------------------------------------------------------
// SearchPopupWidget (Gemini Dual-Pane Workspace: Left Results | Right Inspector)
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
        "QScrollBar:vertical { width: 5px; background: #FDF2F8; margin: 4px 2px; border-radius: 2px; }"
        "QScrollBar::handle:vertical { background: #F472B6; border-radius: 2px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: #EC1680; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; }"));
    m_listView->hide();

    connect(m_listView, &QListView::clicked, this, [this](const QModelIndex& idx) {
        m_listView->setCurrentIndex(idx);
        update();
    });

    connect(m_listView, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (const auto* res = m_model->resultAt(idx.row())) {
            emit resultActivated(*res);
        }
    });

    connect(m_listView->selectionModel(), &QItemSelectionModel::currentRowChanged,
            this, [this](const QModelIndex& current, const QModelIndex&) {
                m_previewText.clear();
                if (const auto* res = m_model->resultAt(current.row())) {
                    emit selectedResultChanged(*res);
                }
                update();
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
    m_previewText.clear();
    m_model->setResults(std::move(results));
    if (m_model->rowCount() > 0) {
        m_listView->setCurrentIndex(m_model->index(0, 0));
        m_listView->show();
        if (const auto* res = m_model->resultAt(0)) {
            emit selectedResultChanged(*res);
        }
    } else {
        m_listView->hide();
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
        update();
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
        // Must equal DesktopSearchBar::kPopupHeight (140) so phase0_tests kExpandedHeight invariant holds
        return kHeaderHeight + 96;
    }
    const int visibleRows = qMin(count, kMaxVisibleRows);
    int rowsHeight = kGroupHeaderExtraHeight;
    const auto& items = m_model->results();
    for (int i = 0; i < visibleRows && i < items.size(); ++i) {
        rowsHeight += items.at(i).snippet.isEmpty() ? kRowHeight : kContentRowHeight;
    }
    const int bodyH = std::max(kSplitBodyMinHeight, rowsHeight + 16);
    const int previewAdd = m_previewVisible ? kPreviewHeight : 0;
    return kHeaderHeight + bodyH + previewAdd + kFooterHeight;
}

QString SearchPopupWidget::statusHeadline() const {
    if (database::DatabaseManager::instance().fileCount() == 0) {
        return QStringLiteral("Your search index is not ready yet.");
    }
    return QStringLiteral("No indexed matches found");
}

int SearchPopupWidget::leftPaneWidth() const noexcept {
    // 54% left results pane / 46% right summary & details pane
    return std::max(260, (width() * 54) / 100);
}

void SearchPopupWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateListLayout();
}

void SearchPopupWidget::updateListLayout() {
    if (m_listView != nullptr && m_listView->isVisible()) {
        const int leftW = leftPaneWidth();
        const int listH = std::max(0, height() - kHeaderHeight - kFooterHeight - 4);
        m_listView->setGeometry(2, kHeaderHeight + 2, leftW - 4, listH);
    }
}

QRectF SearchPopupWidget::modePillRect(int modeIndex) const {
    static const int widths[4] = {80, 72, 86, 92};
    qreal x = 18.0;
    for (int i = 0; i < modeIndex && i < 4; ++i) {
        x += widths[i] + 6.0;
    }
    return QRectF(x, 9.0, widths[modeIndex], 26.0);
}

QRectF SearchPopupWidget::backButtonRect() const {
    return QRectF(width() - 96.0, 9.0, 80.0, 26.0);
}

QRectF SearchPopupWidget::inspectorPaneRect() const {
    const int leftW = leftPaneWidth();
    const int bodyH = std::max(0, height() - kHeaderHeight - kFooterHeight);
    return QRectF(leftW, kHeaderHeight, width() - leftW - 1.0, bodyH);
}

QRectF SearchPopupWidget::inspectorCopyPathButtonRect(const QRectF& inspRect) const {
    return QRectF(inspRect.right() - 14.0 - 44.0, inspRect.top() + 98.0, 40.0, 22.0);
}

QRectF SearchPopupWidget::inspectorOpenButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 39.0;
    return QRectF(inspRect.left() + 14.0, barTop, 84.0, 28.0);
}

QRectF SearchPopupWidget::inspectorRevealButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 39.0;
    return QRectF(inspRect.left() + 106.0, barTop, 84.0, 28.0);
}

QRectF SearchPopupWidget::inspectorRelatedButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 39.0;
    return QRectF(inspRect.left() + 198.0, barTop, 84.0, 28.0);
}

QRectF SearchPopupWidget::inspectorPinButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 39.0;
    const qreal leftX = inspRect.left() + 290.0;
    return QRectF(leftX, barTop, std::max(60.0, inspRect.right() - 14.0 - leftX), 28.0);
}

void SearchPopupWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        const QPointF pos = event->position();
        if (pos.y() <= kHeaderHeight) {
            if (!m_relatedSourceFileName.isEmpty()) {
                if (backButtonRect().contains(pos)) {
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
            }
        } else if (resultCount() > 0) {
            const QRectF inspRect = inspectorPaneRect();
            if (inspRect.contains(pos)) {
                if (const auto* sel = selectedResult()) {
                    if (inspectorCopyPathButtonRect(inspRect).contains(pos)) {
                        emit copyPathRequested(*sel);
                        event->accept();
                        return;
                    }
                    if (inspectorOpenButtonRect(inspRect).contains(pos)) {
                        emit resultActivated(*sel);
                        event->accept();
                        return;
                    }
                    if (inspectorRevealButtonRect(inspRect).contains(pos)) {
                        emit revealInExplorerRequested(*sel);
                        event->accept();
                        return;
                    }
                    if (inspectorRelatedButtonRect(inspRect).contains(pos)) {
                        emit findRelatedRequested(*sel);
                        event->accept();
                        return;
                    }
                    if (inspectorPinButtonRect(inspRect).contains(pos)) {
                        emit togglePinRequested(*sel);
                        event->accept();
                        return;
                    }
                }
            }
        }
    }
    QWidget::mousePressEvent(event);
}

void SearchPopupWidget::showContextMenuAt(const QPoint& localPos, const search::FileSearchResult& item) {
    const auto theme = ThemeTokens::get(false);
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background-color: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px; font-family: 'Segoe UI'; font-size: 9pt; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 5px; color: %2; }"
        "QMenu::item:selected { background-color: %4; color: %2; font-weight: 600; }"
        "QMenu::separator { height: 1px; background: %5; margin: 4px 8px; }")
        .arg(theme.surfaceBackground.name(),
             theme.textPrimary.name(),
             theme.borderColor.name(),
             theme.brandPinkSelected.name(),
             theme.borderSubtle.name()));

    QAction* openAct = menu.addAction(QStringLiteral("Open File\tEnter"));
    QAction* revealAct = menu.addAction(QStringLiteral("Reveal in File Explorer\tCtrl+Enter"));
    QAction* previewAct = menu.addAction(m_previewVisible
                                             ? QStringLiteral("Compact Summary View\tCtrl+P")
                                             : QStringLiteral("Expand Summary View\tCtrl+P"));
    menu.addSeparator();
    QAction* relatedAct = menu.addAction(QStringLiteral("Find Related Documents\tCtrl+R"));
    const bool pinned = core::SettingsManager::instance().isFilePinned(item.path);
    QAction* pinAct = menu.addAction(pinned
                                         ? QStringLiteral("Unpin from Favorites\tCtrl+D")
                                         : QStringLiteral("Pin to Favorites\tCtrl+D"));
    menu.addSeparator();
    QAction* copyPathAct = menu.addAction(QStringLiteral("Copy Path\tCtrl+Shift+C"));
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

    const auto theme = ThemeTokens::get(false);
    const QRectF bounds = QRectF(rect()).adjusted(2.0, 0.0, -2.0, -2.0);

    // Clip interior fills so parent's 16px rounded pink border remains crisp along the bottom corners
    QPainterPath clipPath;
    const qreal r = 14.0;
    clipPath.moveTo(bounds.left(), bounds.top());
    clipPath.lineTo(bounds.right(), bounds.top());
    clipPath.lineTo(bounds.right(), bounds.bottom() - r);
    clipPath.quadTo(bounds.right(), bounds.bottom(), bounds.right() - r, bounds.bottom());
    clipPath.lineTo(bounds.left() + r, bounds.bottom());
    clipPath.quadTo(bounds.left(), bounds.bottom(), bounds.left(), bounds.bottom() - r);
    clipPath.closeSubpath();
    painter.setClipPath(clipPath);

    // -------------------------------------------------------------------------
    // 1. Clean Filter Bar Header (Pure White & Light Pink)
    // -------------------------------------------------------------------------
    const QRectF filterHeaderRect(bounds.left(), 0.0, bounds.width(), kHeaderHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(theme.surfaceBackground);
    painter.drawRect(filterHeaderRect);

    if (!m_relatedSourceFileName.isEmpty()) {
        painter.setFont(ThemeTokens::makeFont(12, QFont::DemiBold));
        painter.setPen(theme.textPrimary);
        const QFontMetrics fm(painter.font());
        const QString banner = fm.elidedText(
            QStringLiteral("Related to: %1").arg(m_relatedSourceFileName),
            Qt::ElideMiddle,
            width() - 125);
        painter.drawText(QRectF(18.0, 8.0, width() - 125.0, 28.0), Qt::AlignLeft | Qt::AlignVCenter, banner);

        const QRectF backRect = backButtonRect();
        painter.setPen(QPen(theme.brandPink, 1.0));
        painter.setBrush(theme.brandPinkSelected);
        painter.drawRoundedRect(backRect, 6.0, 6.0);
        painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
        painter.setPen(theme.textPrimary);
        painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("← Back"));
    } else {
        const QRectF trackRect(14.0, 5.0, 356.0, 34.0);
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.setBrush(theme.brandPinkTint);
        painter.drawRoundedRect(trackRect, 9.0, 9.0);

        static const QString kModeLabels[4] = {
            QStringLiteral("All"),
            QStringLiteral("Files"),
            QStringLiteral("Content"),
            QStringLiteral("Semantic")
        };

        for (int i = 0; i < 4; ++i) {
            const QRectF pRect = modePillRect(i);
            const bool active = (static_cast<int>(m_searchMode) == i);
            if (active) {
                painter.setPen(QPen(theme.brandPink, 1.0));
                painter.setBrush(theme.brandPinkStrongFill);
                painter.drawRoundedRect(pRect, 6.0, 6.0);
                painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
                painter.setPen(theme.textPrimary);
            } else {
                painter.setFont(ThemeTokens::makeFont(11, QFont::Medium));
                painter.setPen(theme.textSecondary);
            }
            painter.drawText(pRect, Qt::AlignCenter, kModeLabels[i]);
        }

        // Right-side simple result count ONLY (NO timing ms, NO SQLite FTS5 Ready!)
        if (!m_queryText.isEmpty()) {
            const QFont countFont = ThemeTokens::makeFont(11, QFont::Medium);
            painter.setFont(countFont);
            const QString countStr = (resultCount() == 1)
                                         ? QStringLiteral("1 result")
                                         : QStringLiteral("%1 results").arg(resultCount());
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(bounds.right() - 150.0, 0.0, 134.0, kHeaderHeight),
                             Qt::AlignRight | Qt::AlignVCenter,
                             countStr);
        }
    }

    // Header bottom hairline divider
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left(), kHeaderHeight), QPointF(bounds.right(), kHeaderHeight));

    const qreal footerTop = bounds.bottom() - kFooterHeight;

    // -------------------------------------------------------------------------
    // 2. Empty State (when typed query has 0 matches)
    // -------------------------------------------------------------------------
    if (resultCount() == 0) {
        const QRectF bodyRect(bounds.left(), kHeaderHeight, bounds.width(), footerTop - kHeaderHeight);
        const QPointF center = bodyRect.center();

        const QFont titleFont = ThemeTokens::makeFont(13, QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(theme.textPrimary);
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() - 22.0, bounds.width() - 48.0, 22.0),
                         Qt::AlignCenter,
                         statusHeadline());

        const QFont subFont = ThemeTokens::makeFont(12, QFont::Normal);
        painter.setFont(subFont);
        painter.setPen(theme.textSecondary);
        const QString subtitle = (database::DatabaseManager::instance().fileCount() == 0)
                                     ? (m_indexingInProgress
                                            ? QStringLiteral("Scanning your folders in the background...")
                                            : QStringLiteral("Index is initializing."))
                                     : QStringLiteral("Try different keywords or switch search mode with Tab");
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() + 2.0, bounds.width() - 48.0, 20.0),
                         Qt::AlignCenter,
                         subtitle);
    } else {
        // ---------------------------------------------------------------------
        // 3. Dual-Column Split: Left Results | Right Summary & File Details
        // ---------------------------------------------------------------------
        const QRectF inspRect = inspectorPaneRect();

        // Pure White right pane & crisp light-pink vertical divider
        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.inspectorBackground);
        painter.drawRect(inspRect);

        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.drawLine(QPointF(inspRect.left(), inspRect.top()), QPointF(inspRect.left(), inspRect.bottom()));

        if (const auto* sel = selectedResult()) {
            const qreal pad = 14.0;
            const qreal contentLeft = inspRect.left() + pad;
            const qreal contentWidth = inspRect.width() - pad * 2.0;
            const QString extLower = sel->extension.toLower();

            // A. Clean File Header (42x42 icon + Filename + Human File Type — NO Match Badge!)
            const QRectF hdrIconRect(contentLeft, inspRect.top() + 14.0, 42.0, 42.0);
            BrandingIcon::paintFileTypeIcon(&painter, hdrIconRect, extLower, false, true);

            const qreal titleLeft = hdrIconRect.right() + 12.0;
            const qreal titleAvailW = inspRect.right() - pad - titleLeft;

            const QFont titleFont = ThemeTokens::makeFont(14, QFont::Bold);
            painter.setFont(titleFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmTitle(titleFont);
            painter.drawText(QRectF(titleLeft, inspRect.top() + 15.0, titleAvailW, 20.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmTitle.elidedText(sel->name, Qt::ElideMiddle, static_cast<int>(titleAvailW)));

            const QFont typeFont = ThemeTokens::makeFont(11, QFont::Normal);
            painter.setFont(typeFont);
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(titleLeft, inspRect.top() + 36.0, titleAvailW, 18.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             humanReadableFileType(extLower));

            // Header divider
            const qreal hdrBottom = inspRect.top() + 68.0;
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(contentLeft, hdrBottom), QPointF(inspRect.right() - pad, hdrBottom));

            // B. Path: (Simple label "Path:" + clean light-pink box + Copy button written in black)
            painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
            painter.setPen(theme.textPrimary);
            painter.drawText(QRectF(contentLeft, hdrBottom + 8.0, contentWidth, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Path:"));

            const QRectF pathBoxRect(contentLeft, hdrBottom + 26.0, contentWidth, 30.0);
            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(pathBoxRect, 8.0, 8.0);

            const QRectF copyBtnRect = inspectorCopyPathButtonRect(inspRect);
            painter.setPen(QPen(theme.brandPink, 1.0));
            painter.setBrush(theme.brandPinkStrongFill);
            painter.drawRoundedRect(copyBtnRect, 5.0, 5.0);
            painter.setFont(ThemeTokens::makeFont(10, QFont::DemiBold));
            painter.setPen(theme.textPrimary);
            painter.drawText(copyBtnRect, Qt::AlignCenter, QStringLiteral("Copy"));

            const QFont pathFont = ThemeTokens::makeMonoFont(10, QFont::Normal);
            painter.setFont(pathFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmPath(pathFont);
            const int pathMaxW = std::max(60, static_cast<int>(pathBoxRect.width() - 56.0));
            const QString nativePath = QDir::toNativeSeparators(sel->path);
            painter.drawText(QRectF(pathBoxRect.left() + 10.0, pathBoxRect.top(), pathMaxW, pathBoxRect.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmPath.elidedText(nativePath, Qt::ElideMiddle, pathMaxW));

            // C. Clean 2-Card Metadata Row (Size & Modified ONLY — NO Match Source, NO Cosine Similarity!)
            const qreal gridTop = pathBoxRect.bottom() + 10.0;
            const qreal cellW = (contentWidth - 10.0) * 0.5;
            const qreal cellH = 42.0;

            auto drawMetaCard = [&](const QRectF& cell, const QString& label, const QString& value) {
                painter.setPen(QPen(theme.borderSubtle, 1.0));
                painter.setBrush(theme.codeBoxBackground);
                painter.drawRoundedRect(cell, 8.0, 8.0);

                painter.setFont(ThemeTokens::makeFont(10, QFont::Normal));
                painter.setPen(theme.textMuted);
                painter.drawText(QRectF(cell.left() + 10.0, cell.top() + 4.0, cell.width() - 20.0, 14.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 label);

                const QFont valFont = ThemeTokens::makeFont(11, QFont::DemiBold);
                painter.setFont(valFont);
                painter.setPen(theme.textPrimary);
                const QFontMetrics fmVal(valFont);
                painter.drawText(QRectF(cell.left() + 10.0, cell.top() + 20.0, cell.width() - 20.0, 17.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 fmVal.elidedText(value, Qt::ElideMiddle, static_cast<int>(cell.width() - 20.0)));
            };

            drawMetaCard(QRectF(contentLeft, gridTop, cellW, cellH),
                         QStringLiteral("Size"),
                         formatHumanSize(sel->size));

            drawMetaCard(QRectF(contentLeft + cellW + 10.0, gridTop, cellW, cellH),
                         QStringLiteral("Modified"),
                         formatTimestamp(sel->mtime));

            // D. Single Clean "Summary:" Box (NO Inspector label, NO repeated filename/ext bar!)
            const qreal summaryHeaderTop = gridTop + cellH + 14.0;
            painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
            painter.setPen(theme.textPrimary);
            painter.drawText(QRectF(contentLeft, summaryHeaderTop, contentWidth * 0.65, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Summary:"));

            if (sel->matchedPage > 0) {
                painter.setFont(ThemeTokens::makeFont(10, QFont::Medium));
                painter.setPen(theme.textSecondary);
                painter.drawText(QRectF(contentLeft, summaryHeaderTop, contentWidth, 16.0),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 QStringLiteral("Page %1").arg(sel->matchedPage));
            }

            const qreal actionBarTop = inspRect.bottom() - 48.0;
            const qreal summaryBoxTop = summaryHeaderTop + 20.0;
            const qreal summaryBoxH = std::max(84.0, actionBarTop - summaryBoxTop - 10.0);
            const QRectF summaryBoxRect(contentLeft, summaryBoxTop, contentWidth, summaryBoxH);

            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(summaryBoxRect, 8.0, 8.0);

            // Body passage directly inside Summary card (clean black text, no duplicate title header!)
            const QRectF passageRect = summaryBoxRect.adjusted(12.0, 10.0, -12.0, -10.0);
            QString bodyText = !m_previewText.isEmpty() ? m_previewText : sel->snippet;
            if (bodyText.isEmpty()) {
                bodyText = QStringLiteral("No text summary available for this file.");
            }
            bodyText.replace(QChar(0x00AB), QString());
            bodyText.replace(QChar(0x00BB), QString());

            const bool isCode = isSourceCodeExtension(extLower);
            painter.setFont(isCode ? ThemeTokens::makeMonoFont(11, QFont::Normal)
                                   : ThemeTokens::makeFont(12, QFont::Normal));
            painter.setPen(theme.textPrimary);
            painter.drawText(passageRect,
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             bodyText.trimmed());

            // E. Clean Light-Pink Action Buttons Written in Black
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(inspRect.left(), actionBarTop), QPointF(inspRect.right(), actionBarTop));

            const QRectF openBtn = inspectorOpenButtonRect(inspRect);
            const QRectF revealBtn = inspectorRevealButtonRect(inspRect);
            const QRectF relatedBtn = inspectorRelatedButtonRect(inspRect);
            const QRectF pinBtn = inspectorPinButtonRect(inspRect);

            auto drawActionBtn = [&](const QRectF& r, const QString& label, bool primaryStyle) {
                painter.setPen(QPen(primaryStyle ? theme.brandPink : theme.borderColor, 1.1));
                painter.setBrush(primaryStyle ? theme.brandPinkStrongFill : theme.brandPinkTint);
                painter.drawRoundedRect(r, 7.0, 7.0);
                painter.setFont(ThemeTokens::makeFont(11, primaryStyle ? QFont::Bold : QFont::Medium));
                painter.setPen(theme.textPrimary);
                painter.drawText(r, Qt::AlignCenter, label);
            };

            drawActionBtn(openBtn, QStringLiteral("Open"), true);
            drawActionBtn(revealBtn, QStringLiteral("Reveal"), false);
            drawActionBtn(relatedBtn, QStringLiteral("Related"), false);
            drawActionBtn(pinBtn, sel->isPinned ? QStringLiteral("Pinned") : QStringLiteral("Pin"), sel->isPinned);
        }
    }

    // -------------------------------------------------------------------------
    // 4. Clean Bottom Footer (NO ESC Collapse clutter!)
    // -------------------------------------------------------------------------
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left(), footerTop), QPointF(bounds.right(), footerTop));

    const QRectF footerRect(bounds.left() + 16.0, footerTop, bounds.width() - 32.0, kFooterHeight);
    painter.setFont(ThemeTokens::makeFont(11, QFont::Normal));
    painter.setPen(theme.textSecondary);
    painter.drawText(footerRect,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("↑↓ Navigate   ·   Enter Open   ·   Ctrl+Enter Reveal   ·   Tab Switch Mode"));

    painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
    painter.setPen(theme.textPrimary);
    painter.drawText(footerRect,
                     Qt::AlignRight | Qt::AlignVCenter,
                     m_indexingInProgress ? QStringLiteral("Indexing in Background")
                                          : QStringLiteral("UshaLabs · Offline"));
}

} // namespace lumen::ui
