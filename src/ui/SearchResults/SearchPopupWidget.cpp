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
        return QStringLiteral("PINNED FAVORITES");
    }
    switch (item.matchType) {
        case search::SearchResultMatchType::Semantic:
            return QStringLiteral("SEMANTIC IN-FILE MATCHES");
        case search::SearchResultMatchType::Hybrid:
            return QStringLiteral("HYBRID SEMANTIC & CONTENT MATCHES");
        case search::SearchResultMatchType::Content:
        case search::SearchResultMatchType::FilenameAndContent:
            return QStringLiteral("IN-FILE CONTENT MATCHES");
        case search::SearchResultMatchType::Filename:
        default:
            return QStringLiteral("FILES & DOCUMENTS");
    }
}

QString matchTypeBadgeLabel(const search::FileSearchResult& item) {
    switch (item.matchType) {
        case search::SearchResultMatchType::Semantic:
            if (item.semanticSimilarity > 0.0f) {
                return QStringLiteral("Cosine %1").arg(item.semanticSimilarity, 0, 'f', 2);
            }
            return QStringLiteral("Semantic");
        case search::SearchResultMatchType::Hybrid:
            if (item.semanticSimilarity > 0.0f) {
                return QStringLiteral("Hybrid · %1").arg(item.semanticSimilarity, 0, 'f', 2);
            }
            return QStringLiteral("Hybrid Match");
        case search::SearchResultMatchType::FilenameAndContent:
            return (item.matchedPage > 0)
                       ? QStringLiteral("Page %1 · Content").arg(item.matchedPage)
                       : QStringLiteral("Name + Content");
        case search::SearchResultMatchType::Content:
            return (item.matchedPage > 0)
                       ? QStringLiteral("Page %1 Match").arg(item.matchedPage)
                       : QStringLiteral("Content Match");
        case search::SearchResultMatchType::Filename:
        default:
            return QStringLiteral("Filename Match");
    }
}

QString matchSourceDescription(const search::FileSearchResult& item) {
    switch (item.matchType) {
        case search::SearchResultMatchType::Semantic:
            return QStringLiteral("Semantic (MiniLM-L6)");
        case search::SearchResultMatchType::Hybrid:
            return QStringLiteral("Hybrid (FTS5 + Vector)");
        case search::SearchResultMatchType::Content:
        case search::SearchResultMatchType::FilenameAndContent:
            return QStringLiteral("SQLite FTS5 BM25");
        case search::SearchResultMatchType::Filename:
        default:
            return QStringLiteral("Filename Index");
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
            painter->setBrush(theme.brandPinkTint);
            painter->drawRoundedRect(
                QRectF(curX - 1.0, rect.top() + 1.0, advance + 2.0, rect.height() - 2.0), 3.0, 3.0);
            painter->setFont(mediumFont);
            painter->setPen(theme.brandPinkLight);
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

        const auto theme = ThemeTokens::get(m_darkTheme);
        const QString groupTitle = index.data(SearchResultListModel::GroupHeaderTitleRole).toString();
        const int groupCount = index.data(SearchResultListModel::GroupHeaderCountRole).toInt();

        QRectF fullRect = QRectF(option.rect);
        if (!groupTitle.isEmpty()) {
            // Draw Gemini uppercase group section header ("SEMANTIC IN-FILE MATCHES   2")
            const QRectF hdrRect(fullRect.left() + 14.0,
                                 fullRect.top() + 4.0,
                                 fullRect.width() - 28.0,
                                 SearchPopupWidget::kGroupHeaderExtraHeight - 4.0);
            painter->setFont(ThemeTokens::makeMonoFont(10, QFont::DemiBold));
            painter->setPen(theme.brandPinkLight);
            painter->drawText(hdrRect, Qt::AlignLeft | Qt::AlignVCenter, groupTitle);

            painter->setFont(ThemeTokens::makeMonoFont(10, QFont::Normal));
            painter->setPen(theme.textMuted);
            painter->drawText(hdrRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(groupCount));

            fullRect.setTop(fullRect.top() + SearchPopupWidget::kGroupHeaderExtraHeight);
        }

        const QRectF cardRect = fullRect.adjusted(8.0, 2.0, -8.0, -3.0);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

        // Card background & pink selection border matching Gemini's `from-rose-500/30 via-pink-500/15`
        if (isSelected) {
            if (m_darkTheme) {
                QLinearGradient selGrad(cardRect.topLeft(), cardRect.topRight());
                selGrad.setColorAt(0.0, QColor(236, 22, 128, 72));
                selGrad.setColorAt(0.55, QColor(236, 22, 128, 32));
                selGrad.setColorAt(1.0, QColor(15, 10, 20, 60));
                painter->setBrush(selGrad);
            } else {
                painter->setBrush(theme.brandPinkSelected);
            }
            painter->setPen(QPen(theme.brandPinkBorder, 1.2));
            painter->drawRoundedRect(cardRect, 10.0, 10.0);
        } else if (isHovered) {
            painter->setPen(QPen(theme.borderSubtle, 1.0));
            painter->setBrush(theme.hoverSurface);
            painter->drawRoundedRect(cardRect, 10.0, 10.0);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(m_darkTheme ? QColor(0, 0, 0, 75) : theme.elevatedBackground);
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
        const float semSim = index.data(SearchResultListModel::SemanticSimilarityRole).toFloat();
        const bool isPinned = index.data(SearchResultListModel::IsPinnedRole).toBool();
        const auto matchType = static_cast<search::SearchResultMatchType>(
            index.data(SearchResultListModel::MatchTypeRole).toInt());

        // 1. Left 32x32 rounded file icon tile
        const QRectF iconBox(cardRect.left() + 10.0, cardRect.top() + 10.0, 32.0, 32.0);
        BrandingIcon::paintFileTypeIcon(painter, iconBox, ext, m_darkTheme, isSelected);

        const qreal textLeft = iconBox.right() + 10.0;
        const qreal rightMargin = cardRect.right() - 10.0;
        const qreal availWidth = std::max(80.0, rightMargin - textLeft);

        // 2. Right-side file size in monospaced font
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

        // 3. Match category pill next to filename (real match type / cosine / page)
        search::FileSearchResult tempItem;
        tempItem.matchType = matchType;
        tempItem.matchedPage = matchedPage;
        tempItem.semanticSimilarity = semSim;
        QString badgeText = matchTypeBadgeLabel(tempItem);
        if (isPinned) {
            badgeText = QStringLiteral("Pinned · ") + badgeText;
        }

        const QFont badgeFont = ThemeTokens::makeMonoFont(9, QFont::Medium);
        const QFontMetrics fmBadge(badgeFont);
        const int badgeW = fmBadge.horizontalAdvance(badgeText) + 12;

        // 4. Filename on Line 1
        const QFont nameFont = ThemeTokens::makeFont(12, QFont::DemiBold);
        const QFontMetrics fmName(nameFont);
        const int maxNameW = std::max(60, static_cast<int>(availWidth - rightReserved - badgeW - 8));
        const QString elidedName = fmName.elidedText(fileName, Qt::ElideMiddle, maxNameW);
        const int actualNameW = fmName.horizontalAdvance(elidedName);

        if (matchStart >= 0 && matchLen > 0 && elidedName == fileName &&
            matchStart + matchLen <= fileName.size()) {
            const QString prefix = fileName.left(matchStart);
            const int hlX = static_cast<int>(textLeft) + fmName.horizontalAdvance(prefix);
            const int hlW = fmName.horizontalAdvance(fileName.mid(matchStart, matchLen));
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.brandPinkTint);
            painter->drawRoundedRect(QRectF(hlX - 1.0, cardRect.top() + 9.0, hlW + 2.0, 16.0), 3.0, 3.0);
        }

        painter->setFont(nameFont);
        painter->setPen(theme.textPrimary);
        painter->drawText(QRectF(textLeft, cardRect.top() + 7.0, actualNameW + 4, 20.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedName);

        // Draw the match badge right after the filename
        const qreal badgeLeft = std::min(textLeft + actualNameW + 8.0, rightMargin - rightReserved - badgeW);
        const QRectF badgeRect(badgeLeft, cardRect.top() + 9.0, badgeW, 16.0);
        painter->setPen(QPen(theme.brandPinkBorder, 1.0));
        painter->setBrush(theme.brandPinkTint);
        painter->drawRoundedRect(badgeRect, 4.0, 4.0);
        painter->setFont(badgeFont);
        painter->setPen(theme.brandPinkLight);
        painter->drawText(badgeRect, Qt::AlignCenter, badgeText);

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
        "QScrollBar:vertical { width: 5px; background: rgba(0, 0, 0, 40); margin: 4px 2px; border-radius: 2px; }"
        "QScrollBar::handle:vertical { background: rgba(236, 22, 128, 95); border-radius: 2px; min-height: 24px; }"
        "QScrollBar::handle:vertical:hover { background: rgba(255, 51, 119, 180); }"
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
    // 58% left results pane / 42% right inspector pane (matching Gemini's 7/12 vs 5/12 grid)
    return std::max(260, (width() * 58) / 100);
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
    static const int widths[4] = {96, 72, 92, 114};
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
    return QRectF(inspRect.right() - 40.0, inspRect.top() + 86.0, 26.0, 22.0);
}

QRectF SearchPopupWidget::inspectorOpenButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 38.0;
    return QRectF(inspRect.left() + 12.0, barTop, 96.0, 28.0);
}

QRectF SearchPopupWidget::inspectorRevealButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 38.0;
    return QRectF(inspRect.left() + 114.0, barTop, 86.0, 28.0);
}

QRectF SearchPopupWidget::inspectorRelatedButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 38.0;
    return QRectF(inspRect.left() + 206.0, barTop, 92.0, 28.0);
}

QRectF SearchPopupWidget::inspectorPinButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 38.0;
    return QRectF(inspRect.left() + 304.0, barTop, std::max(54.0, inspRect.right() - 12.0 - (inspRect.left() + 304.0)), 28.0);
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
    const auto theme = ThemeTokens::get(m_darkTheme);
    QMenu menu(this);
    menu.setStyleSheet(QStringLiteral(
        "QMenu { background-color: %1; color: %2; border: 1px solid %3; border-radius: 8px; padding: 5px; font-family: 'Segoe UI'; font-size: 9pt; }"
        "QMenu::item { padding: 6px 22px 6px 12px; border-radius: 5px; }"
        "QMenu::item:selected { background-color: %4; color: %5; font-weight: 600; }"
        "QMenu::separator { height: 1px; background: %6; margin: 4px 8px; }")
        .arg(theme.elevatedBackground.name(),
             theme.textPrimary.name(),
             theme.brandPinkBorder.name(),
             theme.brandPinkSelected.name(),
             theme.brandPinkLight.name(),
             theme.borderSubtle.name()));

    QAction* openAct = menu.addAction(QStringLiteral("Open File\tEnter"));
    QAction* revealAct = menu.addAction(QStringLiteral("Reveal in File Explorer\tCtrl+Enter"));
    QAction* previewAct = menu.addAction(m_previewVisible
                                             ? QStringLiteral("Compact Inspector View\tCtrl+P")
                                             : QStringLiteral("Expand Inspector View\tCtrl+P"));
    menu.addSeparator();
    QAction* relatedAct = menu.addAction(QStringLiteral("Find Related Documents\tCtrl+R"));
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
    const QRectF bounds = QRectF(rect()).adjusted(1.0, 0.0, -1.0, -1.0);

    // -------------------------------------------------------------------------
    // 1. Filter Bar & Real-Time Query Metrics Header (Gemini style)
    // -------------------------------------------------------------------------
    const QRectF filterHeaderRect(bounds.left(), 0.0, bounds.width(), kHeaderHeight);
    painter.setPen(Qt::NoPen);
    painter.setBrush(m_darkTheme ? QColor(0, 0, 0, 105) : theme.elevatedBackground);
    painter.drawRect(filterHeaderRect);

    if (!m_relatedSourceFileName.isEmpty()) {
        painter.setFont(ThemeTokens::makeFont(12, QFont::DemiBold));
        painter.setPen(theme.brandPinkLight);
        const QFontMetrics fm(painter.font());
        const QString banner = fm.elidedText(
            QStringLiteral("Related Documents to: %1").arg(m_relatedSourceFileName),
            Qt::ElideMiddle,
            width() - 125);
        painter.drawText(QRectF(18.0, 8.0, width() - 125.0, 28.0), Qt::AlignLeft | Qt::AlignVCenter, banner);

        const QRectF backRect = backButtonRect();
        painter.setPen(QPen(theme.brandPinkBorder, 1.0));
        painter.setBrush(theme.brandPinkTint);
        painter.drawRoundedRect(backRect, 6.0, 6.0);
        painter.setFont(ThemeTokens::makeMonoFont(11, QFont::Medium));
        painter.setPen(theme.brandPinkLight);
        painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("← Back"));
    } else {
        // Segmented dark pill track containing the 4 real Lumen search modes
        const QRectF trackRect(14.0, 5.0, 400.0, 34.0);
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.setBrush(m_darkTheme ? QColor(0, 0, 0, 130) : theme.surfaceBackground);
        painter.drawRoundedRect(trackRect, 9.0, 9.0);

        static const QString kModeLabels[4] = {
            QStringLiteral("All Matches"),
            QStringLiteral("Files"),
            QStringLiteral("In-File Text"),
            QStringLiteral("Semantic")
        };

        for (int i = 0; i < 4; ++i) {
            const QRectF pRect = modePillRect(i);
            const bool active = (static_cast<int>(m_searchMode) == i);
            if (active) {
                painter.setPen(QPen(theme.brandPinkBorder, 1.0));
                painter.setBrush(theme.brandPinkSelected);
                painter.drawRoundedRect(pRect, 6.0, 6.0);
                painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
                painter.setPen(theme.brandPinkLight);
            } else {
                painter.setFont(ThemeTokens::makeFont(11, QFont::Medium));
                painter.setPen(theme.textSecondary);
            }
            painter.drawText(pRect, Qt::AlignCenter, kModeLabels[i]);
        }

        // Right-side real latency & real SQLite WAL / FTS5 status
        const QFont metricFont = ThemeTokens::makeMonoFont(11, QFont::Medium);
        painter.setFont(metricFont);
        const QString metricStr = (resultCount() > 0)
                                      ? QStringLiteral("%1 %2 (~%3 ms)")
                                            .arg(resultCount())
                                            .arg(resultCount() == 1 ? QStringLiteral("result") : QStringLiteral("results"))
                                            .arg(m_lastQueryMs, 0, 'f', 2)
                                      : (m_queryText.isEmpty()
                                             ? QStringLiteral("%1 indexed files")
                                                   .arg(database::DatabaseManager::instance().fileCount())
                                             : QStringLiteral("0 results"));

        const QString dbReadyStr = QStringLiteral("  •  SQLite FTS5 Ready");
        const QFontMetrics fmMetric(metricFont);
        const int dbW = fmMetric.horizontalAdvance(dbReadyStr);
        const int metricW = fmMetric.horizontalAdvance(metricStr);

        painter.setPen(QColor(52, 211, 153)); // Emerald-400 status
        painter.drawText(QRectF(bounds.right() - dbW - 16.0, 0.0, dbW, kHeaderHeight),
                         Qt::AlignRight | Qt::AlignVCenter,
                         dbReadyStr);

        painter.setPen(theme.brandPinkLight);
        painter.drawText(QRectF(bounds.right() - dbW - metricW - 18.0, 0.0, metricW + 4, kHeaderHeight),
                         Qt::AlignRight | Qt::AlignVCenter,
                         metricStr);
    }

    // Header bottom divider
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left(), kHeaderHeight), QPointF(bounds.right(), kHeaderHeight));

    const qreal footerTop = bounds.bottom() - kFooterHeight;

    // -------------------------------------------------------------------------
    // 2. Empty State (when resultCount() == 0)
    // -------------------------------------------------------------------------
    if (resultCount() == 0) {
        const QRectF bodyRect(bounds.left(), kHeaderHeight, bounds.width(), footerTop - kHeaderHeight);
        const QPointF center = bodyRect.center();

        const QFont titleFont = ThemeTokens::makeFont(13, QFont::DemiBold);
        painter.setFont(titleFont);
        painter.setPen(theme.textPrimary);
        const QString headline = m_queryText.isEmpty()
                                     ? QStringLiteral("Start typing to search your files, content, and concepts")
                                     : statusHeadline();
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() - 22.0, bounds.width() - 48.0, 22.0),
                         Qt::AlignCenter,
                         headline);

        const QFont subFont = ThemeTokens::makeFont(12, QFont::Normal);
        painter.setFont(subFont);
        painter.setPen(theme.textSecondary);
        const QString subtitle = (database::DatabaseManager::instance().fileCount() == 0)
                                     ? (m_indexingInProgress
                                            ? QStringLiteral("Scanning approved folders in the background...")
                                            : QStringLiteral("Local file index is initializing."))
                                     : QStringLiteral("Use Tab to switch modes or filters like ext:pdf, type:code, modified:7d");
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() + 2.0, bounds.width() - 48.0, 20.0),
                         Qt::AlignCenter,
                         subtitle);
    } else {
        // ---------------------------------------------------------------------
        // 3. Dual-Column Split: Left Results | Right Real File Inspector
        // ---------------------------------------------------------------------
        const QRectF inspRect = inspectorPaneRect();

        // Inspector background & vertical divider
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

            // A. Inspector File Header (42x42 icon + Filename + Match Badge + Human-readable Type)
            const QRectF hdrIconRect(contentLeft, inspRect.top() + 12.0, 42.0, 42.0);
            BrandingIcon::paintFileTypeIcon(&painter, hdrIconRect, extLower, m_darkTheme, true);

            const qreal titleLeft = hdrIconRect.right() + 10.0;
            const qreal titleAvailW = inspRect.right() - pad - titleLeft;

            const QString matchLabel = matchTypeBadgeLabel(*sel);
            const QFont badgeFont = ThemeTokens::makeMonoFont(10, QFont::Medium);
            const QFontMetrics fmBadge(badgeFont);
            const int badgeW = fmBadge.horizontalAdvance(matchLabel) + 14;
            const QRectF matchPillRect(inspRect.right() - pad - badgeW, inspRect.top() + 13.0, badgeW, 18.0);

            painter.setPen(QPen(theme.brandPinkBorder, 1.0));
            painter.setBrush(theme.brandPinkTint);
            painter.drawRoundedRect(matchPillRect, 9.0, 9.0);
            painter.setFont(badgeFont);
            painter.setPen(theme.brandPinkLight);
            painter.drawText(matchPillRect, Qt::AlignCenter, matchLabel);

            const QFont titleFont = ThemeTokens::makeFont(13, QFont::Bold);
            painter.setFont(titleFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmTitle(titleFont);
            const int maxTitleW = std::max(60, static_cast<int>(titleAvailW - badgeW - 8.0));
            painter.drawText(QRectF(titleLeft, inspRect.top() + 12.0, maxTitleW, 20.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmTitle.elidedText(sel->name, Qt::ElideMiddle, maxTitleW));

            const QFont typeFont = ThemeTokens::makeMonoFont(11, QFont::Normal);
            painter.setFont(typeFont);
            painter.setPen(theme.brandPinkLight);
            painter.drawText(QRectF(titleLeft, inspRect.top() + 33.0, titleAvailW, 18.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             humanReadableFileType(extLower));

            // Header divider
            const qreal hdrBottom = inspRect.top() + 62.0;
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(contentLeft, hdrBottom), QPointF(inspRect.right() - pad, hdrBottom));

            // B. Windows Full Path Card + Working Copy Button
            painter.setFont(ThemeTokens::makeMonoFont(10, QFont::Normal));
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(contentLeft, hdrBottom + 6.0, contentWidth, 14.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("WINDOWS FULL PATH:"));

            const QRectF pathBoxRect(contentLeft, hdrBottom + 22.0, contentWidth, 26.0);
            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(pathBoxRect, 6.0, 6.0);

            const QRectF copyBtnRect = inspectorCopyPathButtonRect(inspRect);
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.brandPinkTint);
            painter.drawRoundedRect(copyBtnRect, 4.0, 4.0);
            painter.setFont(ThemeTokens::makeMonoFont(9, QFont::DemiBold));
            painter.setPen(theme.brandPinkLight);
            painter.drawText(copyBtnRect, Qt::AlignCenter, QStringLiteral("CPY"));

            const QFont pathFont = ThemeTokens::makeMonoFont(10, QFont::Normal);
            painter.setFont(pathFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmPath(pathFont);
            const int pathMaxW = std::max(60, static_cast<int>(pathBoxRect.width() - 44.0));
            const QString nativePath = QDir::toNativeSeparators(sel->path);
            painter.drawText(QRectF(pathBoxRect.left() + 8.0, pathBoxRect.top(), pathMaxW, pathBoxRect.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmPath.elidedText(nativePath, Qt::ElideMiddle, pathMaxW));

            // C. 2x2 Real Metadata Grid (File Size, Last Modified, Match Mode, Location / Page)
            const qreal gridTop = pathBoxRect.bottom() + 8.0;
            const qreal cellW = (contentWidth - 8.0) * 0.5;
            const qreal cellH = 38.0;

            auto drawMetaCard = [&](const QRectF& cell, const QString& label, const QString& value, bool accentVal) {
                painter.setPen(QPen(theme.borderSubtle, 1.0));
                painter.setBrush(theme.codeBoxBackground);
                painter.drawRoundedRect(cell, 6.0, 6.0);

                painter.setFont(ThemeTokens::makeMonoFont(9, QFont::Normal));
                painter.setPen(theme.textMuted);
                painter.drawText(QRectF(cell.left() + 8.0, cell.top() + 4.0, cell.width() - 16.0, 13.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 label);

                const QFont valFont = ThemeTokens::makeMonoFont(10, QFont::Medium);
                painter.setFont(valFont);
                painter.setPen(accentVal ? theme.brandPinkLight : theme.textPrimary);
                const QFontMetrics fmVal(valFont);
                painter.drawText(QRectF(cell.left() + 8.0, cell.top() + 18.0, cell.width() - 16.0, 15.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 fmVal.elidedText(value, Qt::ElideMiddle, static_cast<int>(cell.width() - 16.0)));
            };

            drawMetaCard(QRectF(contentLeft, gridTop, cellW, cellH),
                         QStringLiteral("File Size:"),
                         formatHumanSize(sel->size),
                         false);

            drawMetaCard(QRectF(contentLeft + cellW + 8.0, gridTop, cellW, cellH),
                         QStringLiteral("Last Modified:"),
                         formatTimestamp(sel->mtime),
                         false);

            drawMetaCard(QRectF(contentLeft, gridTop + cellH + 6.0, cellW, cellH),
                         QStringLiteral("Match Source:"),
                         matchSourceDescription(*sel),
                         true);

            QString fourthLabel = QStringLiteral("Location:");
            QString fourthValue = QDir::toNativeSeparators(sel->parentDirectory);
            if (sel->semanticSimilarity > 0.0f) {
                fourthLabel = QStringLiteral("Cosine Similarity:");
                fourthValue = QStringLiteral("%1 (384-D)").arg(sel->semanticSimilarity, 0, 'f', 3);
            } else if (sel->matchedPage > 0) {
                fourthLabel = QStringLiteral("Document Page:");
                fourthValue = QStringLiteral("Page %1").arg(sel->matchedPage);
            }
            drawMetaCard(QRectF(contentLeft + cellW + 8.0, gridTop + cellH + 6.0, cellW, cellH),
                         fourthLabel,
                         fourthValue,
                         sel->semanticSimilarity > 0.0f);

            // D. Content / Source Code Inspector Box
            const qreal snipHeaderTop = gridTop + cellH * 2.0 + 14.0;
            painter.setFont(ThemeTokens::makeMonoFont(10, QFont::DemiBold));
            painter.setPen(theme.brandPinkLight);
            painter.drawText(QRectF(contentLeft, snipHeaderTop, contentWidth * 0.65, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("IN-FILE CONTENT INSPECTOR:"));

            if (sel->matchedPage > 0) {
                painter.setFont(ThemeTokens::makeMonoFont(10, QFont::Normal));
                painter.setPen(theme.textSecondary);
                painter.drawText(QRectF(contentLeft, snipHeaderTop, contentWidth, 16.0),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 QStringLiteral("Page %1").arg(sel->matchedPage));
            }

            const qreal actionBarTop = inspRect.bottom() - 44.0;
            const qreal codeBoxTop = snipHeaderTop + 18.0;
            const qreal codeBoxH = std::max(64.0, actionBarTop - codeBoxTop - 6.0);
            const QRectF codeBoxRect(contentLeft, codeBoxTop, contentWidth, codeBoxH);

            painter.setPen(QPen(theme.borderColor, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(codeBoxRect, 8.0, 8.0);

            // Top sub-bar inside content box (filename + extension badge)
            const QRectF codeSubHeader(codeBoxRect.left(), codeBoxRect.top(), codeBoxRect.width(), 22.0);
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(codeSubHeader.left(), codeSubHeader.bottom()),
                             QPointF(codeSubHeader.right(), codeSubHeader.bottom()));

            painter.setFont(ThemeTokens::makeMonoFont(10, QFont::Medium));
            painter.setPen(theme.brandPinkLight);
            const QFontMetrics fmSub(painter.font());
            painter.drawText(QRectF(codeSubHeader.left() + 10.0, codeSubHeader.top(), codeSubHeader.width() - 70.0, 22.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmSub.elidedText(sel->name, Qt::ElideMiddle, static_cast<int>(codeSubHeader.width() - 75.0)));

            painter.setPen(theme.textMuted);
            painter.drawText(QRectF(codeSubHeader.left(), codeSubHeader.top(), codeSubHeader.width() - 10.0, 22.0),
                             Qt::AlignRight | Qt::AlignVCenter,
                             extLower.isEmpty() ? QStringLiteral("FILE") : extLower.toUpper());

            // Body passage inside content box
            const QRectF passageRect = codeBoxRect.adjusted(10.0, 26.0, -10.0, -6.0);
            QString bodyText = !m_previewText.isEmpty() ? m_previewText : sel->snippet;
            if (bodyText.isEmpty()) {
                bodyText = QStringLiteral("Preview unavailable for this file type.");
            }
            bodyText.replace(QChar(0x00AB), QString());
            bodyText.replace(QChar(0x00BB), QString());

            const bool isCode = isSourceCodeExtension(extLower);
            painter.setFont(isCode ? ThemeTokens::makeMonoFont(11, QFont::Normal)
                                   : ThemeTokens::makeFont(11, QFont::Normal));
            painter.setPen(theme.textPrimary);
            painter.drawText(passageRect,
                             Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                             bodyText.trimmed());

            // E. Bottom Action Toolbar inside Inspector (Open, Reveal, Related, Pin)
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(inspRect.left(), actionBarTop), QPointF(inspRect.right(), actionBarTop));

            const QRectF openBtn = inspectorOpenButtonRect(inspRect);
            const QRectF revealBtn = inspectorRevealButtonRect(inspRect);
            const QRectF relatedBtn = inspectorRelatedButtonRect(inspRect);
            const QRectF pinBtn = inspectorPinButtonRect(inspRect);

            // Primary pink Open button
            painter.setPen(Qt::NoPen);
            painter.setBrush(theme.brandPink);
            painter.drawRoundedRect(openBtn, 7.0, 7.0);
            painter.setFont(ThemeTokens::makeFont(11, QFont::DemiBold));
            painter.setPen(QColor(255, 255, 255));
            painter.drawText(openBtn, Qt::AlignCenter, QStringLiteral("Open ↵"));

            // Secondary action buttons
            auto drawSecondaryBtn = [&](const QRectF& r, const QString& label, bool activeTint = false) {
                painter.setPen(QPen(activeTint ? theme.brandPinkBorder : theme.borderColor, 1.0));
                painter.setBrush(activeTint ? theme.brandPinkTint : theme.elevatedBackground);
                painter.drawRoundedRect(r, 7.0, 7.0);
                painter.setFont(ThemeTokens::makeFont(11, QFont::Medium));
                painter.setPen(activeTint ? theme.brandPinkLight : theme.textPrimary);
                painter.drawText(r, Qt::AlignCenter, label);
            };

            drawSecondaryBtn(revealBtn, QStringLiteral("Reveal ^↵"));
            drawSecondaryBtn(relatedBtn, QStringLiteral("Related ^R"));
            drawSecondaryBtn(pinBtn, sel->isPinned ? QStringLiteral("Pinned ★") : QStringLiteral("Pin ^D"), sel->isPinned);
        }
    }

    // -------------------------------------------------------------------------
    // 4. Bottom Keyboard Navigation & Offline Privacy Bar
    // -------------------------------------------------------------------------
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left(), footerTop), QPointF(bounds.right(), footerTop));

    const QRectF footerRect(bounds.left() + 16.0, footerTop, bounds.width() - 32.0, kFooterHeight);
    painter.setFont(ThemeTokens::makeMonoFont(11, QFont::Normal));
    painter.setPen(theme.textSecondary);
    painter.drawText(footerRect,
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("↑↓ Navigate   ↵ Open   ^↵ Reveal   ^C Copy Path   Tab Mode   ESC Collapse"));

    painter.setFont(ThemeTokens::makeMonoFont(11, QFont::DemiBold));
    painter.setPen(theme.brandPinkLight);
    painter.drawText(footerRect,
                     Qt::AlignRight | Qt::AlignVCenter,
                     m_indexingInProgress ? QStringLiteral("● Indexing in Background")
                                          : QStringLiteral("● 100% Offline"));
}

} // namespace lumen::ui
