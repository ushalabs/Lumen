#include "ui/SearchResults/SearchPopupWidget.h"

#include "core/SettingsManager.h"
#include "database/DatabaseManager.h"
#include "extraction/DocumentExtractor.h"
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
#include <QTimer>
#include <QToolTip>

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
        {QStringLiteral("xlsm"), QStringLiteral("Excel Macro Workbook")},
        {QStringLiteral("xls"), QStringLiteral("Excel Spreadsheet")},
        {QStringLiteral("csv"), QStringLiteral("CSV Data Table")},
        {QStringLiteral("tsv"), QStringLiteral("TSV Data Table")},
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
        {QStringLiteral("go"), QStringLiteral("Go Source File")},
        {QStringLiteral("java"), QStringLiteral("Java Source File")},
        {QStringLiteral("kt"), QStringLiteral("Kotlin Source File")},
        {QStringLiteral("cs"), QStringLiteral("C# Source File")},
        {QStringLiteral("swift"), QStringLiteral("Swift Source File")},
        {QStringLiteral("rb"), QStringLiteral("Ruby Script")},
        {QStringLiteral("php"), QStringLiteral("PHP Script")},
        {QStringLiteral("py"), QStringLiteral("Python Script")},
        {QStringLiteral("js"), QStringLiteral("JavaScript Source File")},
        {QStringLiteral("ts"), QStringLiteral("TypeScript Source File")},
        {QStringLiteral("sh"), QStringLiteral("Shell Script")},
        {QStringLiteral("ps1"), QStringLiteral("PowerShell Script")},
        {QStringLiteral("json"), QStringLiteral("JSON Configuration")},
        {QStringLiteral("toml"), QStringLiteral("TOML Configuration")},
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
        {QStringLiteral("gif"), QStringLiteral("GIF Image")},
        {QStringLiteral("bmp"), QStringLiteral("Bitmap Image")},
        {QStringLiteral("tif"), QStringLiteral("TIFF Image")},
        {QStringLiteral("tiff"), QStringLiteral("TIFF Image")},
        {QStringLiteral("heic"), QStringLiteral("HEIC Image")},
        {QStringLiteral("ico"), QStringLiteral("Windows Icon")},
        {QStringLiteral("mp4"), QStringLiteral("MP4 Video")},
        {QStringLiteral("mkv"), QStringLiteral("MKV Video")},
        {QStringLiteral("mov"), QStringLiteral("MOV Video")},
        {QStringLiteral("avi"), QStringLiteral("AVI Video")},
        {QStringLiteral("webm"), QStringLiteral("WebM Video")},
        {QStringLiteral("wmv"), QStringLiteral("WMV Video")},
        {QStringLiteral("m4v"), QStringLiteral("M4V Video")},
        {QStringLiteral("mp3"), QStringLiteral("MP3 Audio")},
        {QStringLiteral("flac"), QStringLiteral("FLAC Lossless Audio")},
        {QStringLiteral("wav"), QStringLiteral("WAV Audio")},
        {QStringLiteral("m4a"), QStringLiteral("M4A Audio")},
        {QStringLiteral("aac"), QStringLiteral("AAC Audio")},
        {QStringLiteral("ogg"), QStringLiteral("OGG Audio")},
        {QStringLiteral("opus"), QStringLiteral("Opus Audio")},
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
    return extraction::DocumentExtractor::isCodeExtension(extLower);
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
            return item.isDirectory ? QStringLiteral("FOLDERS") : QStringLiteral("FILES");
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
    const QFont mediumFont = ThemeTokens::makeFont(11, QFont::Medium);
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
            // Lightweight pastel wash + thin pink underline (no heavy solid block)
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(236, 22, 128, 18));
            painter->drawRoundedRect(
                QRectF(curX - 1.0, rect.top() + 1.0, advance + 2.0, rect.height() - 2.0), 3.0, 3.0);
            painter->setPen(QPen(QColor(236, 22, 128, 155), 1.3));
            painter->drawLine(QPointF(curX, rect.bottom() - 2.0), QPointF(curX + advance, rect.bottom() - 2.0));
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
        case IsDirectoryRole:        return item.isDirectory;
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
            const QRectF hdrRect(fullRect.left() + 16.0,
                                 fullRect.top() + 5.0,
                                 fullRect.width() - 32.0,
                                 SearchPopupWidget::kGroupHeaderExtraHeight - 5.0);
            painter->setFont(ThemeTokens::makeFont(10, QFont::Medium));
            painter->setPen(theme.textMuted);
            painter->drawText(hdrRect, Qt::AlignLeft | Qt::AlignVCenter, groupTitle);

            painter->setFont(ThemeTokens::makeFont(10, QFont::Normal));
            painter->setPen(theme.textMuted);
            painter->drawText(hdrRect, Qt::AlignRight | Qt::AlignVCenter, QString::number(groupCount));

            fullRect.setTop(fullRect.top() + SearchPopupWidget::kGroupHeaderExtraHeight);
        }

        const QRectF cardRect = fullRect.adjusted(10.0, 2.0, -10.0, -3.0);
        const bool isSelected = (option.state & QStyle::State_Selected) != 0;
        const bool isHovered = (option.state & QStyle::State_MouseOver) != 0;

        // Calm transparent/soft card; selected shows airier pastel tint + sleek 3px pink indicator bar
        if (isSelected) {
            painter->setBrush(theme.brandPinkSelected);
            painter->setPen(QPen(QColor(236, 22, 128, 70), 1.0));
            painter->drawRoundedRect(cardRect, 10.0, 10.0);

            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.brandPink);
            painter->drawRoundedRect(QRectF(cardRect.left() + 1.5, cardRect.top() + 10.0, 3.0, cardRect.height() - 20.0), 1.5, 1.5);
        } else if (isHovered) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.hoverSurface);
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
        const bool isDirectory = index.data(SearchResultListModel::IsDirectoryRole).toBool();

        // 1. Left 32x32 rounded file or folder icon tile
        const QRectF iconBox(cardRect.left() + 12.0, cardRect.top() + 10.0, 32.0, 32.0);
        BrandingIcon::paintFileTypeIcon(painter, iconBox, ext, false, isSelected, isDirectory);

        const qreal textLeft = iconBox.right() + 11.0;
        const qreal rightMargin = cardRect.right() - 12.0;
        const qreal availWidth = std::max(80.0, rightMargin - textLeft);

        // 2. Right-side file size (or Folder badge) in smaller muted gray (#6E6E6E)
        const QFont sizeFont = ThemeTokens::makeFont(11, QFont::Normal);
        const QFontMetrics fmSize(sizeFont);
        const QString sizeStr = isDirectory ? QStringLiteral("Folder") : formatHumanSize(fileSize);
        const int sizeW = fmSize.horizontalAdvance(sizeStr) + 6;
        painter->setFont(sizeFont);
        painter->setPen(theme.textSecondary);
        painter->drawText(QRectF(rightMargin - sizeW, cardRect.top() + 8.0, sizeW, 18.0),
                          Qt::AlignRight | Qt::AlignVCenter,
                          sizeStr);

        int rightReserved = sizeW + 8;

        // 3. Subtle page or pinned tag when applicable
        QString badgeText;
        if (isPinned) {
            badgeText = (matchedPage > 0) ? QStringLiteral("Pinned · p.%1").arg(matchedPage) : QStringLiteral("Pinned");
        } else if (matchedPage > 0) {
            badgeText = QStringLiteral("Page %1").arg(matchedPage);
        }

        const QFont badgeFont = ThemeTokens::makeFont(10, QFont::Medium);
        const QFontMetrics fmBadge(badgeFont);
        const int badgeW = badgeText.isEmpty() ? 0 : (fmBadge.horizontalAdvance(badgeText) + 12);
        const int badgeGap = badgeText.isEmpty() ? 0 : 8;

        // 4. Filename on Line 1 — Stronger typographic weight & scale vs muted #6E6E6E metadata
        const QFont nameFont = ThemeTokens::makeFont(13, QFont::DemiBold);
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
            painter->setBrush(QColor(236, 22, 128, 20));
            painter->drawRoundedRect(QRectF(hlX - 1.0, cardRect.top() + 8.0, hlW + 2.0, 18.0), 3.0, 3.0);
            painter->setPen(QPen(QColor(236, 22, 128, 165), 1.4));
            painter->drawLine(QPointF(hlX, cardRect.top() + 25.0), QPointF(hlX + hlW, cardRect.top() + 25.0));
        }

        painter->setFont(nameFont);
        painter->setPen(theme.textPrimary);
        painter->drawText(QRectF(textLeft, cardRect.top() + 7.0, actualNameW + 4, 20.0),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          elidedName);

        if (!badgeText.isEmpty()) {
            const qreal badgeLeft = std::min(textLeft + actualNameW + 8.0, rightMargin - rightReserved - badgeW);
            const QRectF badgeRect(badgeLeft, cardRect.top() + 9.0, badgeW, 17.0);
            painter->setPen(Qt::NoPen);
            painter->setBrush(theme.brandPinkTint);
            painter->drawRoundedRect(badgeRect, 5.0, 5.0);
            painter->setFont(badgeFont);
            painter->setPen(QColor(180, 28, 102));
            painter->drawText(badgeRect, Qt::AlignCenter, badgeText);
        }

        // 5. Line 2: Clean muted #6E6E6E path
        const QFont pathFont = ThemeTokens::makeFont(11, QFont::Normal);
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
    setMouseTracking(true);
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
    m_listView->viewport()->installEventFilter(this);
    m_listView->setStyleSheet(QStringLiteral(
        "QListView { background: transparent; border: none; outline: none; }"
        "QScrollBar:vertical { width: 13px; background: #F1F2F6; margin: 6px 3px 6px 1px; border-radius: 6px; border: none; }"
        "QScrollBar::handle:vertical { background: #B0B5C3; border-radius: 5px; min-height: 36px; margin: 1px; }"
        "QScrollBar::handle:vertical:hover { background: #EC1680; }"
        "QScrollBar::handle:vertical:pressed { background: #C8106C; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0px; background: transparent; }"));
    m_listView->hide();

    connect(m_listView, &QListView::clicked, this, [this](const QModelIndex& idx) {
        if (!idx.isValid()) {
            clearResultSelection();
            return;
        }
        m_listView->setCurrentIndex(idx);
        m_listView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        update();
    });

    connect(m_listView, &QListView::doubleClicked, this, [this](const QModelIndex& idx) {
        if (const auto* res = m_model->resultAt(idx.row())) {
            emit resultActivated(*res);
        }
    });

    connect(m_listView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, [this](const QItemSelection& selected, const QItemSelection&) {
                m_previewText.clear();
                m_copiedFeedback = false;
                m_copiedFileFeedback = false;
                const QModelIndexList rows = selected.indexes();
                if (!rows.isEmpty()) {
                    if (const auto* res = m_model->resultAt(rows.first().row())) {
                        syncInspectorSnapshot(*res);
                        emit selectedResultChanged(*res);
                        emit selectionActiveChanged(true);
                        update();
                        return;
                    }
                }
                emit selectionActiveChanged(false);
                update();
            });

    connect(m_listView, &QListView::customContextMenuRequested, this, [this](const QPoint& pos) {
        const QModelIndex idx = m_listView->indexAt(pos);
        if (idx.isValid()) {
            m_listView->setCurrentIndex(idx);
            m_listView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
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
    m_lastInspectorPreviewText.clear();
    m_lastInspectorResult.reset();
    m_copiedFeedback = false;
    m_copiedFileFeedback = false;
    m_hoveredActionIdx = -1;
    m_closeDetailsHovered = false;
    m_pathHovered = false;

    m_model->setResults(std::move(results));
    if (m_listView->selectionModel() != nullptr) {
        QSignalBlocker blocker(m_listView->selectionModel());
        m_listView->selectionModel()->clear();
    }
    m_listView->setCurrentIndex(QModelIndex());
    m_listView->scrollToTop();

    if (m_model->rowCount() > 0) {
        m_listView->show();
    } else {
        m_listView->hide();
    }
    updateListLayout();
    emit selectionActiveChanged(false);
    update();
}

void SearchPopupWidget::clearResults() {
    m_lastQueryMs = 0.0;
    m_previewText.clear();
    m_lastInspectorPreviewText.clear();
    m_lastInspectorResult.reset();
    m_relatedSourceFileName.clear();
    m_copiedFeedback = false;
    m_copiedFileFeedback = false;
    m_hoveredActionIdx = -1;
    m_closeDetailsHovered = false;
    m_pathHovered = false;
    if (m_listView->selectionModel() != nullptr) {
        QSignalBlocker blocker(m_listView->selectionModel());
        m_listView->selectionModel()->clear();
    }
    m_listView->setCurrentIndex(QModelIndex());
    m_model->clear();
    m_listView->hide();
    emit selectionActiveChanged(false);
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
    const int next = forward ? ((cur + 1) % 5) : ((cur + 4) % 5);
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
    if (hasSelection()) {
        m_lastInspectorPreviewText = text;
    }
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
    if (!m_listView || !m_listView->selectionModel()) {
        return -1;
    }
    const QModelIndexList rows = m_listView->selectionModel()->selectedRows();
    if (!rows.isEmpty() && rows.first().isValid()) {
        return rows.first().row();
    }
    return -1;
}

bool SearchPopupWidget::hasSelection() const noexcept {
    return selectedRow() >= 0;
}

const search::FileSearchResult* SearchPopupWidget::selectedResult() const noexcept {
    return m_model ? m_model->resultAt(selectedRow()) : nullptr;
}

void SearchPopupWidget::syncInspectorSnapshot(const search::FileSearchResult& item) {
    m_lastInspectorResult = item;
    m_lastInspectorPreviewText = m_previewText;
}

void SearchPopupWidget::clearInspectorSnapshot() {
    if (!hasSelection()) {
        m_lastInspectorResult.reset();
        m_lastInspectorPreviewText.clear();
    }
}

void SearchPopupWidget::showFileCopiedFeedback() {
    m_copiedFileFeedback = true;
    update();
    QTimer::singleShot(1400, this, [this]() {
        m_copiedFileFeedback = false;
        update();
    });
}

void SearchPopupWidget::clearResultSelection() {
    if (!m_listView) {
        return;
    }
    const bool hadSel = hasSelection();
    if (m_listView->selectionModel() != nullptr) {
        m_listView->selectionModel()->clear();
    }
    m_listView->setCurrentIndex(QModelIndex());
    m_hoveredActionIdx = -1;
    m_closeDetailsHovered = false;
    m_pathHovered = false;
    m_copiedFeedback = false;
    m_copiedFileFeedback = false;
    QToolTip::hideText();
    update();
    if (!hadSel) {
        emit selectionActiveChanged(false);
    }
}

void SearchPopupWidget::selectNext() {
    const int count = resultCount();
    if (count <= 0) {
        return;
    }
    const int cur = selectedRow();
    const int nextRow = (cur < 0) ? 0 : ((cur + 1) % count);
    const QModelIndex idx = m_model->index(nextRow, 0);
    m_listView->setCurrentIndex(idx);
    m_listView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_listView->scrollTo(idx);
}

void SearchPopupWidget::selectPrevious() {
    const int count = resultCount();
    if (count <= 0) {
        return;
    }
    const int cur = selectedRow();
    const int prevRow = (cur <= 0) ? (count - 1) : (cur - 1);
    const QModelIndex idx = m_model->index(prevRow, 0);
    m_listView->setCurrentIndex(idx);
    m_listView->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
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
    // Keep left results list constant at kLeftPaneWidth (488px) so sliding details out never reflows items
    return std::max(260, std::min(width(), kLeftPaneWidth));
}

void SearchPopupWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    updateListLayout();
}

void SearchPopupWidget::updateListLayout() {
    if (m_listView != nullptr && m_listView->isVisible()) {
        const int leftW = leftPaneWidth();
        const int listH = std::max(0, height() - kHeaderHeight - 10);
        m_listView->setGeometry(2, kHeaderHeight + 4, leftW - 4, listH);
    }
}

QRectF SearchPopupWidget::modePillRect(int modeIndex) const {
    static const int widths[5] = {54, 60, 74, 78, 86};
    qreal x = 18.0;
    for (int i = 0; i < modeIndex && i < 5; ++i) {
        x += widths[i] + 4.0;
    }
    return QRectF(x, 9.0, widths[modeIndex], 26.0);
}

QRectF SearchPopupWidget::backButtonRect() const {
    return QRectF(width() - 98.0, 9.0, 80.0, 26.0);
}

QRectF SearchPopupWidget::inspectorPaneRect() const {
    const int leftW = leftPaneWidth();
    const int bodyH = std::max(0, height() - kHeaderHeight);
    const int fixedInspW = std::max(360, kFullWorkspaceWidth - kLeftPaneWidth);
    return QRectF(leftW, kHeaderHeight, fixedInspW - 1.0, bodyH);
}

QRectF SearchPopupWidget::inspectorCloseButtonRect(const QRectF& inspRect) const {
    return QRectF(inspRect.right() - 42.0, inspRect.top() + 18.0, 22.0, 22.0);
}

QRectF SearchPopupWidget::inspectorPathBoxRect(const QRectF& inspRect) const {
    const qreal pad = 22.0;
    const qreal hdrBottom = inspRect.top() + 78.0;
    return QRectF(inspRect.left() + pad, hdrBottom + 28.0, inspRect.width() - pad * 2.0, 34.0);
}

QRectF SearchPopupWidget::inspectorCopyPathButtonRect(const QRectF& inspRect) const {
    const QRectF pb = inspectorPathBoxRect(inspRect);
    return QRectF(pb.right() - 29.0, pb.top() + 5.0, 24.0, 24.0);
}

QRectF SearchPopupWidget::inspectorPinButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 48.0;
    const qreal rightEdge = inspRect.right() - 22.0;
    return QRectF(rightEdge - 32.0, barTop, 32.0, 32.0);
}

QRectF SearchPopupWidget::inspectorRelatedButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 48.0;
    const qreal rightEdge = inspRect.right() - 22.0;
    return QRectF(rightEdge - 72.0, barTop, 32.0, 32.0);
}

QRectF SearchPopupWidget::inspectorCopyFileButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 48.0;
    const qreal rightEdge = inspRect.right() - 22.0;
    return QRectF(rightEdge - 112.0, barTop, 32.0, 32.0);
}

QRectF SearchPopupWidget::inspectorRevealButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 48.0;
    const qreal rightEdge = inspRect.right() - 22.0;
    return QRectF(rightEdge - 152.0, barTop, 32.0, 32.0);
}

QRectF SearchPopupWidget::inspectorOpenButtonRect(const QRectF& inspRect) const {
    const qreal barTop = inspRect.bottom() - 48.0;
    const qreal rightEdge = inspRect.right() - 22.0;
    return QRectF(rightEdge - 192.0, barTop, 32.0, 32.0);
}

int SearchPopupWidget::hoveredActionIndex(const QPointF& pos) const {
    if (resultCount() <= 0 || !hasSelection() || width() <= leftPaneWidth() + 40) {
        return -1;
    }
    const QRectF inspRect = inspectorPaneRect();
    if (!inspRect.contains(pos)) {
        return -1;
    }
    if (inspectorOpenButtonRect(inspRect).contains(pos)) return 0;
    if (inspectorRevealButtonRect(inspRect).contains(pos)) return 1;
    if (inspectorCopyFileButtonRect(inspRect).contains(pos)) return 2;
    if (inspectorRelatedButtonRect(inspRect).contains(pos)) return 3;
    if (inspectorPinButtonRect(inspRect).contains(pos)) return 4;
    return -1;
}

void SearchPopupWidget::mouseMoveEvent(QMouseEvent* event) {
    const QPointF pos = event->position();
    bool needUpdate = false;

    int newModeHover = -1;
    if (pos.y() <= kHeaderHeight && m_relatedSourceFileName.isEmpty()) {
        for (int i = 0; i < 5; ++i) {
            if (modePillRect(i).contains(pos)) {
                newModeHover = i;
                break;
            }
        }
    }
    if (newModeHover != m_hoveredModeIdx) {
        m_hoveredModeIdx = newModeHover;
        needUpdate = true;
    }

    bool newPathHover = false;
    bool newCloseHover = false;
    int newActionHover = -1;
    if (resultCount() > 0 && hasSelection() && width() > leftPaneWidth() + 40) {
        const QRectF inspRect = inspectorPaneRect();
        newPathHover = inspectorPathBoxRect(inspRect).contains(pos);
        newCloseHover = inspectorCloseButtonRect(inspRect).contains(pos);
        newActionHover = hoveredActionIndex(pos);
    }

    if (newPathHover != m_pathHovered ||
        newCloseHover != m_closeDetailsHovered ||
        newActionHover != m_hoveredActionIdx) {
        m_pathHovered = newPathHover;
        m_closeDetailsHovered = newCloseHover;
        m_hoveredActionIdx = newActionHover;
        needUpdate = true;

        if (m_hoveredActionIdx >= 0) {
            const auto* sel = selectedResult();
            const bool isDir = (sel != nullptr && sel->isDirectory);
            const QString kTips[5] = {
                isDir ? QStringLiteral("Open folder (Enter)") : QStringLiteral("Open file (Enter)"),
                QStringLiteral("Reveal in File Explorer (Ctrl+Enter)"),
                m_copiedFileFeedback
                    ? (isDir ? QStringLiteral("Folder copied!") : QStringLiteral("File copied!"))
                    : (isDir ? QStringLiteral("Copy folder (Ctrl+C)") : QStringLiteral("Copy file (Ctrl+C)")),
                QStringLiteral("Find related documents (Ctrl+R)"),
                QStringLiteral("Pin / Unpin favorite (Ctrl+D)")
            };
            QToolTip::showText(event->globalPosition().toPoint(), kTips[m_hoveredActionIdx], this);
        } else if (m_closeDetailsHovered) {
            QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("Close details"), this);
        } else if (m_pathHovered) {
            QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("Copy path (Ctrl+Shift+C)"), this);
        } else {
            QToolTip::hideText();
        }
    }

    if (needUpdate) {
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void SearchPopupWidget::leaveEvent(QEvent* event) {
    if (m_pathHovered || m_closeDetailsHovered || m_hoveredActionIdx != -1 || m_hoveredModeIdx != -1) {
        m_pathHovered = false;
        m_closeDetailsHovered = false;
        m_hoveredActionIdx = -1;
        m_hoveredModeIdx = -1;
        QToolTip::hideText();
        update();
    }
    QWidget::leaveEvent(event);
}

bool SearchPopupWidget::eventFilter(QObject* watched, QEvent* event) {
    if (m_listView && watched == m_listView->viewport() &&
        event->type() == QEvent::MouseButtonPress) {
        auto* mouseEvt = static_cast<QMouseEvent*>(event);
        if (mouseEvt->button() == Qt::LeftButton) {
            const QModelIndex idx = m_listView->indexAt(mouseEvt->position().toPoint());
            if (!idx.isValid() && hasSelection()) {
                clearResultSelection();
                return true;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
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
                for (int i = 0; i < 5; ++i) {
                    if (modePillRect(i).contains(pos)) {
                        setSearchMode(static_cast<search::SearchMode>(i));
                        event->accept();
                        return;
                    }
                }
            }
        } else if (resultCount() > 0 && hasSelection() && width() > leftPaneWidth() + 40) {
            const QRectF inspRect = inspectorPaneRect();
            if (inspRect.contains(pos)) {
                if (inspectorCloseButtonRect(inspRect).contains(pos)) {
                    clearResultSelection();
                    event->accept();
                    return;
                }
                if (const auto* sel = selectedResult()) {
                    if (inspectorCopyPathButtonRect(inspRect).contains(pos) ||
                        inspectorPathBoxRect(inspRect).contains(pos)) {
                        emit copyPathRequested(*sel);
                        m_copiedFeedback = true;
                        update();
                        QTimer::singleShot(1400, this, [this]() {
                            m_copiedFeedback = false;
                            update();
                        });
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
                    if (inspectorCopyFileButtonRect(inspRect).contains(pos)) {
                        emit copyFileRequested(*sel);
                        m_copiedFileFeedback = true;
                        update();
                        QTimer::singleShot(1400, this, [this]() {
                            m_copiedFileFeedback = false;
                            update();
                        });
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

    QAction* openAct = menu.addAction(item.isDirectory
                                          ? QStringLiteral("Open Folder\tEnter")
                                          : QStringLiteral("Open File\tEnter"));
    QAction* revealAct = menu.addAction(QStringLiteral("Reveal in File Explorer\tCtrl+Enter"));
    QAction* copyItemAct = menu.addAction(item.isDirectory
                                              ? QStringLiteral("Copy Folder\tCtrl+C")
                                              : QStringLiteral("Copy File\tCtrl+C"));
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
    } else if (chosen == copyItemAct) {
        emit copyFileRequested(item);
        m_copiedFileFeedback = true;
        update();
        QTimer::singleShot(1400, this, [this]() {
            m_copiedFileFeedback = false;
            update();
        });
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
    const QRectF bounds = QRectF(rect()).adjusted(1.0, 0.0, -1.0, -1.0);

    // Clip cleanly inside DesktopSearchBar's 16px rounded frosted card
    QPainterPath clipPath;
    const qreal r = 15.0;
    clipPath.moveTo(bounds.left(), bounds.top());
    clipPath.lineTo(bounds.right(), bounds.top());
    clipPath.lineTo(bounds.right(), bounds.bottom() - r);
    clipPath.quadTo(bounds.right(), bounds.bottom(), bounds.right() - r, bounds.bottom());
    clipPath.lineTo(bounds.left() + r, bounds.bottom());
    clipPath.quadTo(bounds.left(), bounds.bottom(), bounds.left(), bounds.bottom() - r);
    clipPath.closeSubpath();
    painter.setClipPath(clipPath);

    // -------------------------------------------------------------------------
    // 1. Filter Bar Header (Neutral Track + Dynamic Pink Focus Accents)
    // -------------------------------------------------------------------------
    if (!m_relatedSourceFileName.isEmpty()) {
        painter.setFont(ThemeTokens::makeFont(12, QFont::Medium));
        painter.setPen(theme.textPrimary);
        const QFontMetrics fm(painter.font());
        const QString banner = fm.elidedText(
            QStringLiteral("Related to: %1").arg(m_relatedSourceFileName),
            Qt::ElideMiddle,
            width() - 125);
        painter.drawText(QRectF(18.0, 8.0, width() - 125.0, 28.0), Qt::AlignLeft | Qt::AlignVCenter, banner);

        const QRectF backRect = backButtonRect();
        painter.setPen(QPen(theme.borderColor, 1.0));
        painter.setBrush(theme.elevatedBackground);
        painter.drawRoundedRect(backRect, 6.0, 6.0);
        painter.setFont(ThemeTokens::makeFont(11, QFont::Medium));
        painter.setPen(theme.textPrimary);
        painter.drawText(backRect, Qt::AlignCenter, QStringLiteral("← Back"));
    } else {
        const QRectF trackRect(14.0, 5.0, 376.0, 34.0);
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.setBrush(theme.elevatedBackground);
        painter.drawRoundedRect(trackRect, 9.0, 9.0);

        static const QString kModeLabels[5] = {
            QStringLiteral("All"),
            QStringLiteral("Files"),
            QStringLiteral("Folders"),
            QStringLiteral("Content"),
            QStringLiteral("Semantic")
        };

        for (int i = 0; i < 5; ++i) {
            const QRectF pRect = modePillRect(i);
            const bool active = (static_cast<int>(m_searchMode) == i);
            const bool hovered = (m_hoveredModeIdx == i);
            if (active) {
                painter.setPen(QPen(QColor(236, 22, 128, 130), 1.0));
                painter.setBrush(QColor(255, 255, 255, 245));
                painter.drawRoundedRect(pRect, 6.0, 6.0);
                painter.setFont(ThemeTokens::makeFont(11, QFont::Medium));
                painter.setPen(theme.textPrimary);
            } else {
                if (hovered) {
                    painter.setPen(Qt::NoPen);
                    painter.setBrush(QColor(236, 22, 128, 16));
                    painter.drawRoundedRect(pRect, 6.0, 6.0);
                }
                painter.setFont(ThemeTokens::makeFont(11, QFont::Normal));
                painter.setPen(theme.textSecondary);
            }
            painter.drawText(pRect, Qt::AlignCenter, kModeLabels[i]);
        }

        if (!m_queryText.isEmpty()) {
            const QFont countFont = ThemeTokens::makeFont(11, QFont::Normal);
            painter.setFont(countFont);
            const QString countStr = (resultCount() == 1)
                                         ? QStringLiteral("1 result")
                                         : QStringLiteral("%1 results").arg(resultCount());
            painter.setPen(theme.textSecondary);
            const qreal countLeft = std::max(trackRect.right() + 6.0, bounds.right() - 140.0);
            const qreal countW = std::max(0.0, bounds.right() - 16.0 - countLeft);
            if (countW >= 40.0) {
                painter.drawText(QRectF(countLeft, 0.0, countW, kHeaderHeight),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 countStr);
            }
        }
    }

    // Neutral #EFEFEF divider
    painter.setPen(QPen(theme.borderSubtle, 1.0));
    painter.drawLine(QPointF(bounds.left(), kHeaderHeight), QPointF(bounds.right(), kHeaderHeight));

    // -------------------------------------------------------------------------
    // 2. Empty State
    // -------------------------------------------------------------------------
    if (resultCount() == 0) {
        const QRectF bodyRect(bounds.left(), kHeaderHeight, bounds.width(), bounds.bottom() - kHeaderHeight);
        const QPointF center = bodyRect.center();

        const QFont titleFont = ThemeTokens::makeFont(13, QFont::Medium);
        painter.setFont(titleFont);
        painter.setPen(theme.textPrimary);
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() - 20.0, bounds.width() - 48.0, 22.0),
                         Qt::AlignCenter,
                         statusHeadline());

        const QFont subFont = ThemeTokens::makeFont(12, QFont::Normal);
        painter.setFont(subFont);
        painter.setPen(theme.textSecondary);
        const QString subtitle = (database::DatabaseManager::instance().fileCount() == 0)
                                     ? (m_indexingInProgress
                                            ? QStringLiteral("Scanning your folders in the background...")
                                            : QStringLiteral("Index is initializing."))
                                     : QStringLiteral("Try different keywords or switch filter mode.");
        painter.drawText(QRectF(bounds.left() + 24.0, center.y() + 4.0, bounds.width() - 48.0, 20.0),
                         Qt::AlignCenter,
                         subtitle);
    } else if (width() > leftPaneWidth() + 4) {
        // ---------------------------------------------------------------------
        // 3. Sliding Right Inspector Panel (revealed only when a row is selected)
        // ---------------------------------------------------------------------
        const QRectF inspRect = inspectorPaneRect();

        painter.setPen(Qt::NoPen);
        painter.setBrush(theme.inspectorBackground);
        painter.drawRect(inspRect);

        // Clean neutral #EFEFEF vertical divider
        painter.setPen(QPen(theme.borderSubtle, 1.0));
        painter.drawLine(QPointF(inspRect.left(), inspRect.top()), QPointF(inspRect.left(), inspRect.bottom()));

        const auto* activeSel = selectedResult();
        const auto* sel = activeSel ? activeSel : (m_lastInspectorResult ? &(*m_lastInspectorResult) : nullptr);
        if (sel != nullptr) {
            const qreal pad = 22.0;
            const qreal contentLeft = inspRect.left() + pad;
            const qreal contentWidth = inspRect.width() - pad * 2.0;
            const QString extLower = sel->extension.toLower();

            // Subtle Close-Details (X) button in top-right of inspector
            const QRectF closeBtn = inspectorCloseButtonRect(inspRect);
            painter.setPen(QPen(m_closeDetailsHovered ? QColor(236, 22, 128, 130) : Qt::transparent, 1.0));
            painter.setBrush(m_closeDetailsHovered ? QColor(236, 22, 128, 22) : Qt::transparent);
            painter.drawRoundedRect(closeBtn, 6.0, 6.0);
            QPen xPen(m_closeDetailsHovered ? theme.brandPink : theme.textMuted, 1.45, Qt::SolidLine, Qt::RoundCap);
            painter.setPen(xPen);
            const QPointF xc = closeBtn.center();
            painter.drawLine(QPointF(xc.x() - 3.5, xc.y() - 3.5), QPointF(xc.x() + 3.5, xc.y() + 3.5));
            painter.drawLine(QPointF(xc.x() + 3.5, xc.y() - 3.5), QPointF(xc.x() - 3.5, xc.y() + 3.5));

            // A. Typographic Hierarchy: Larger Bold Filename + Muted File/Folder Type Subtitle
            const QRectF hdrIconRect(contentLeft, inspRect.top() + 18.0, 44.0, 44.0);
            BrandingIcon::paintFileTypeIcon(&painter, hdrIconRect, extLower, false, true, sel->isDirectory);

            const qreal titleLeft = hdrIconRect.right() + 14.0;
            const qreal titleAvailW = std::max(60.0, closeBtn.left() - 10.0 - titleLeft);

            const QFont titleFont = ThemeTokens::makeFont(15, QFont::DemiBold);
            painter.setFont(titleFont);
            painter.setPen(theme.textPrimary);
            const QFontMetrics fmTitle(titleFont);
            painter.drawText(QRectF(titleLeft, inspRect.top() + 19.0, titleAvailW, 22.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmTitle.elidedText(sel->name, Qt::ElideMiddle, static_cast<int>(titleAvailW)));

            const QFont typeFont = ThemeTokens::makeFont(11, QFont::Normal);
            painter.setFont(typeFont);
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(titleLeft, inspRect.top() + 42.0, titleAvailW, 18.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             sel->isDirectory ? QStringLiteral("File Folder") : humanReadableFileType(extLower));

            const qreal hdrBottom = inspRect.top() + 78.0;
            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.drawLine(QPointF(contentLeft, hdrBottom), QPointF(inspRect.right() - pad, hdrBottom));

            // B. Unified Path Box (Hover-Only Clipboard Icon, NO boxed "Copy" pill!)
            painter.setFont(ThemeTokens::makeFont(11, QFont::Normal));
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(contentLeft, hdrBottom + 12.0, contentWidth, 14.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Path:"));

            const QRectF pathBoxRect = inspectorPathBoxRect(inspRect);
            const bool showCopyIcon = m_pathHovered || m_copiedFeedback;
            painter.setPen(QPen(showCopyIcon ? QColor(236, 22, 128, 125) : theme.borderSubtle, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(pathBoxRect, 8.0, 8.0);

            const int pathRightReserve = showCopyIcon ? 38 : 14;
            const QFont pathFont = ThemeTokens::makeFont(11, QFont::Normal);
            painter.setFont(pathFont);
            painter.setPen(theme.textSecondary);
            const QFontMetrics fmPath(pathFont);
            const int pathMaxW = std::max(60, static_cast<int>(pathBoxRect.width() - pathRightReserve - 12.0));
            const QString nativePath = QDir::toNativeSeparators(sel->path);
            painter.drawText(QRectF(pathBoxRect.left() + 12.0, pathBoxRect.top(), pathMaxW, pathBoxRect.height()),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             fmPath.elidedText(nativePath, Qt::ElideMiddle, pathMaxW));

            if (showCopyIcon) {
                const QRectF copyBtnRect = inspectorCopyPathButtonRect(inspRect);
                painter.setPen(Qt::NoPen);
                painter.setBrush(QColor(236, 22, 128, 22));
                painter.drawRoundedRect(copyBtnRect, 5.0, 5.0);
                BrandingIcon::paintClipboardIcon(
                    &painter,
                    copyBtnRect.adjusted(4.0, 4.0, -4.0, -4.0),
                    theme.brandPink,
                    m_copiedFeedback);
            }

            // C. Spacious Metadata Cards (Size & Modified in muted contrast hierarchy)
            const qreal gridTop = pathBoxRect.bottom() + 12.0;
            const qreal cellW = (contentWidth - 12.0) * 0.5;
            const qreal cellH = 46.0;

            auto drawMetaCard = [&](const QRectF& cell, const QString& label, const QString& value) {
                painter.setPen(QPen(theme.borderSubtle, 1.0));
                painter.setBrush(theme.codeBoxBackground);
                painter.drawRoundedRect(cell, 8.0, 8.0);

                painter.setFont(ThemeTokens::makeFont(10, QFont::Normal));
                painter.setPen(theme.textMuted);
                painter.drawText(QRectF(cell.left() + 12.0, cell.top() + 6.0, cell.width() - 24.0, 14.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 label);

                const QFont valFont = ThemeTokens::makeFont(12, QFont::Medium);
                painter.setFont(valFont);
                painter.setPen(theme.textSecondary);
                const QFontMetrics fmVal(valFont);
                painter.drawText(QRectF(cell.left() + 12.0, cell.top() + 22.0, cell.width() - 24.0, 18.0),
                                 Qt::AlignLeft | Qt::AlignVCenter,
                                 fmVal.elidedText(value, Qt::ElideMiddle, static_cast<int>(cell.width() - 24.0)));
            };

            drawMetaCard(QRectF(contentLeft, gridTop, cellW, cellH),
                         sel->isDirectory ? QStringLiteral("Kind") : QStringLiteral("Size"),
                         sel->isDirectory ? QStringLiteral("Folder") : formatHumanSize(sel->size));

            drawMetaCard(QRectF(contentLeft + cellW + 12.0, gridTop, cellW, cellH),
                         QStringLiteral("Modified"),
                         formatTimestamp(sel->mtime));

            // D. Generously Padded "Summary:" Box
            const qreal summaryHeaderTop = gridTop + cellH + 16.0;
            painter.setFont(ThemeTokens::makeFont(11, QFont::Normal));
            painter.setPen(theme.textSecondary);
            painter.drawText(QRectF(contentLeft, summaryHeaderTop, contentWidth * 0.65, 16.0),
                             Qt::AlignLeft | Qt::AlignVCenter,
                             QStringLiteral("Summary:"));

            if (sel->matchedPage > 0) {
                painter.setFont(ThemeTokens::makeFont(10, QFont::Normal));
                painter.setPen(theme.textMuted);
                painter.drawText(QRectF(contentLeft, summaryHeaderTop, contentWidth, 16.0),
                                 Qt::AlignRight | Qt::AlignVCenter,
                                 QStringLiteral("Page %1").arg(sel->matchedPage));
            }

            const qreal iconRowTop = inspRect.bottom() - 48.0;
            const qreal summaryBoxTop = summaryHeaderTop + 20.0;
            const qreal summaryBoxH = std::max(84.0, iconRowTop - summaryBoxTop - 14.0);
            const QRectF summaryBoxRect(contentLeft, summaryBoxTop, contentWidth, summaryBoxH);

            painter.setPen(QPen(theme.borderSubtle, 1.0));
            painter.setBrush(theme.codeBoxBackground);
            painter.drawRoundedRect(summaryBoxRect, 10.0, 10.0);

            const QRectF passageRect = summaryBoxRect.adjusted(14.0, 12.0, -14.0, -12.0);
            const QString activePreview = activeSel ? m_previewText : m_lastInspectorPreviewText;
            QString bodyText = !activePreview.isEmpty() ? activePreview : sel->snippet;
            if (bodyText.isEmpty()) {
                bodyText = sel->isDirectory
                               ? QStringLiteral("Folder contents preview unavailable.")
                               : QStringLiteral("No text summary available for this file.");
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

            // E. Streamlined Icon-Only Bottom-Right Action Control Group
            const QRectF openBtn = inspectorOpenButtonRect(inspRect);
            const QRectF revealBtn = inspectorRevealButtonRect(inspRect);
            const QRectF copyFileBtn = inspectorCopyFileButtonRect(inspRect);
            const QRectF relatedBtn = inspectorRelatedButtonRect(inspRect);
            const QRectF pinBtn = inspectorPinButtonRect(inspRect);

            auto drawIconBtnBg = [&](const QRectF& r, int idx, bool isPrimaryOrActive) {
                const bool hov = (m_hoveredActionIdx == idx);
                const QColor bg = (hov || isPrimaryOrActive)
                                      ? QColor(236, 22, 128, hov ? 32 : 18)
                                      : theme.elevatedBackground;
                const QColor brd = hov
                                       ? QColor(236, 22, 128, 145)
                                       : (isPrimaryOrActive ? QColor(236, 22, 128, 80) : theme.borderSubtle);
                painter.setPen(QPen(brd, 1.0));
                painter.setBrush(bg);
                painter.drawRoundedRect(r, 8.0, 8.0);
            };

            drawIconBtnBg(openBtn, 0, true);
            BrandingIcon::paintExternalOpenIcon(&painter, openBtn.adjusted(7.0, 7.0, -7.0, -7.0), theme.brandPink);

            drawIconBtnBg(revealBtn, 1, false);
            BrandingIcon::paintFolderIcon(
                &painter,
                revealBtn.adjusted(7.0, 7.0, -7.0, -7.0),
                m_hoveredActionIdx == 1 ? theme.brandPink : theme.iconPrimary);

            drawIconBtnBg(copyFileBtn, 2, m_copiedFileFeedback);
            BrandingIcon::paintClipboardIcon(
                &painter,
                copyFileBtn.adjusted(7.0, 7.0, -7.0, -7.0),
                (m_copiedFileFeedback || m_hoveredActionIdx == 2) ? theme.brandPink : theme.iconPrimary,
                m_copiedFileFeedback);

            drawIconBtnBg(relatedBtn, 3, false);
            BrandingIcon::paintRelatedIcon(
                &painter,
                relatedBtn.adjusted(7.0, 7.0, -7.0, -7.0),
                m_hoveredActionIdx == 3 ? theme.brandPink : theme.iconPrimary);

            drawIconBtnBg(pinBtn, 4, sel->isPinned);
            BrandingIcon::paintPinIcon(
                &painter,
                pinBtn.adjusted(7.0, 7.0, -7.0, -7.0),
                (sel->isPinned || m_hoveredActionIdx == 4) ? theme.brandPink : theme.iconPrimary,
                sel->isPinned);
        }
    }
}

} // namespace lumen::ui
