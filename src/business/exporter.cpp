#include "exporter.h"

#include "codehighlighter.h"
#include "logger.h"
#include "markdownparser.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUrl>
#include <QWebEnginePage>

using markdown_editor::core::document::CodeHighlighter;
using markdown_editor::core::document::MarkdownParser;

namespace {

// 预览模板里的 <style>…</style>。用 DotMatchesEverything 让 .* 能跨行匹配。
const QRegularExpression &styleBlockRe()
{
    static const QRegularExpression re(QStringLiteral("<style[^>]*>(.*?)</style>"),
                                       QRegularExpression::DotMatchesEverythingOption);
    return re;
}

// <img … src="…" …> 里的 src 值（捕获组 1）。md4c-html 输出的一定是双引号。
const QRegularExpression &imgSrcRe()
{
    static const QRegularExpression re(QStringLiteral("<img\\b[^>]*?\\bsrc\\s*=\\s*\"([^\"]*)\""));
    return re;
}

}  // namespace

Exporter::Exporter(QObject *parent) : QObject(parent)
{
    m_pdfWatchdog.setSingleShot(true);
    connect(&m_pdfWatchdog, &QTimer::timeout, this, &Exporter::onPdfTimeout);
}

// ============================ 样式 ============================

QString Exporter::previewStyleSheet()
{
    QFile file(QStringLiteral(":/html/preview_template.html"));
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN("读不到预览模板（:/html/preview_template.html），导出会用一份极简样式兜底");
        return QStringLiteral("body{font-family:sans-serif;line-height:1.7;}"
                              "pre{background:#f6f8fa;padding:12px;}"
                              "table{border-collapse:collapse;}th,td{border:1px solid #d0d7de;padding:6px 12px;}");
    }

    const QString html = QString::fromUtf8(file.readAll());
    const QRegularExpressionMatch match = styleBlockRe().match(html);
    if (!match.hasMatch()) {
        LOG_WARN("预览模板里没找到 <style> 块，导出会用极简样式兜底");
        return QString();
    }
    return match.captured(1).trimmed();
}

QString Exporter::exportOverrideStyleSheet()
{
    // 这段必须放在预览样式**之后**：同选择器、同优先级时后写的赢。
    return QStringLiteral(
        "/* ---- 导出专用：把预览里「给屏幕看」的东西收回来 ---- */\n"
        "body { padding: 0; margin: 0; background: var(--bg); }\n"
        "#content { max-width: 920px; margin: 0 auto; padding: 28px 36px 40px; }\n"
        "\n"
        "@media print {\n"
        "  /* 打印/导 PDF 时不再限宽，让版面吃满纸张 */\n"
        "  #content { max-width: none; padding: 0; }\n"
        "  h1, h2, h3, h4, h5, h6 { page-break-after: avoid; }\n"
        "  pre, blockquote, table, img, figure { page-break-inside: avoid; }\n"
        "  /* 有些打印设置不画背景色，给代码块补一条边框兜底，免得看起来像「没了」 */\n"
        "  pre { border: 1px solid #d0d7de; }\n"
        "  /* 纸上看不出蓝色，链接改成下划线 */\n"
        "  a { color: inherit; text-decoration: underline; }\n"
        "  /* ★ 打印一律用浅色：暗色主题打出来是一片黑，费墨而且难读。\n"
        "     选择器和 html[data-theme=\"dark\"] 一样具体、但写在后面，所以能压过暗色那套变量。 */\n"
        "  :root, html[data-theme=\"dark\"] {\n"
        "    --bg: #ffffff; --fg: #24292f; --muted: #57606a; --border: #eaecef;\n"
        "    --border-strong: #d0d7de; --stripe: #f6f8fa; --link: #24292f; --quote-bar: #d0d7de;\n"
        "    --code-bg: #f6f8fa; --code-fg: #24292f;\n"
        "    --tok-comment: #6a737d; --tok-keyword: #d73a49; --tok-type: #6f42c1; --tok-literal: #005cc5;\n"
        "    --tok-builtin: #005cc5; --tok-string: #032f62; --tok-number: #005cc5; --tok-function: #6f42c1;\n"
        "    --tok-attr: #005cc5; --tok-tag: #22863a; --tok-operator: #d73a49; --tok-meta: #6a737d;\n"
        "    --tok-add-fg: #22863a; --tok-add-bg: #f0fff4; --tok-del-fg: #b31d28; --tok-del-bg: #ffeef0;\n"
        "  }\n"
        "}\n");
}

// ============================ 拼完整 HTML ============================

QString Exporter::escapedTitle(const QString &raw)
{
    QString text = raw;
    text.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    text.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    text.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    text.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return text;
}

QString Exporter::defaultTitleFor(const QString &filePath)
{
    if (filePath.trimmed().isEmpty()) {
        return QStringLiteral("未命名");
    }
    const QString name = QFileInfo(filePath).completeBaseName();
    return name.isEmpty() ? QStringLiteral("未命名") : name;
}

QString Exporter::standaloneHtml(const QString &markdown, const QString &title, const QString &themeId)
{
    // 和预览一样：渲染 → 代码高亮（三条路共用同一个结果）
    const QString body = CodeHighlighter::highlightCodeBlocks(MarkdownParser::parseToHtml(markdown));
    const QString shownTitle = escapedTitle(title.trimmed().isEmpty() ? QStringLiteral("未命名") : title);
    // 只认两个值：它会被写进 HTML 属性，先收窄
    const QString theme = (themeId.compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) ? QStringLiteral("dark")
                                                                                             : QStringLiteral("light");

    // 样式一律内联在 <style> 里（这就是"独立文件"的含义：拷到任何地方、断网也照样好看）。
    // 没有 <script>：导出的文件不需要和 C++ 通话，带上 WebChannel 反而会报错。
    return QStringLiteral("<!DOCTYPE html>\n"
                          "<html lang=\"zh-CN\" data-theme=\"%1\">\n"
                          "<head>\n"
                          "<meta charset=\"utf-8\">\n"
                          "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                          "<meta name=\"generator\" content=\"Markdown 编辑器 (muse-md)\">\n"
                          "<title>%2</title>\n"
                          "<style>\n%3\n\n%4\n</style>\n"
                          "</head>\n"
                          "<body>\n"
                          "<div id=\"content\">\n%5</div>\n"
                          "</body>\n"
                          "</html>\n")
        .arg(theme, shownTitle, previewStyleSheet(), exportOverrideStyleSheet(), body);
}

// ============================ 图片内联 ============================

QString Exporter::mimeTypeForSuffix(const QString &suffix)
{
    const QString s = suffix.toLower();
    if (s == QLatin1String("png")) {
        return QStringLiteral("image/png");
    }
    if (s == QLatin1String("jpg") || s == QLatin1String("jpeg")) {
        return QStringLiteral("image/jpeg");
    }
    if (s == QLatin1String("gif")) {
        return QStringLiteral("image/gif");
    }
    if (s == QLatin1String("webp")) {
        return QStringLiteral("image/webp");
    }
    if (s == QLatin1String("bmp")) {
        return QStringLiteral("image/bmp");
    }
    if (s == QLatin1String("svg")) {
        return QStringLiteral("image/svg+xml");
    }
    if (s == QLatin1String("ico")) {
        return QStringLiteral("image/x-icon");
    }
    return QStringLiteral("application/octet-stream");
}

QString Exporter::encodeDataUri(const QByteArray &bytes, const QString &mimeType)
{
    return QStringLiteral("data:%1;base64,%2").arg(mimeType, QString::fromLatin1(bytes.toBase64()));
}

QString Exporter::inlineLocalImages(const QString &html, const QString &baseDir, HtmlResult *result)
{
    QString out;
    int lastEnd = 0;

    QRegularExpressionMatchIterator it = imgSrcRe().globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString src = match.captured(1);

        // 已经自带出处的（data: / http(s):// / qrc: / file:）一律不动：
        // 换掉它们没有好处，反而可能把能用的东西弄坏
        if (src.isEmpty() || src.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)
            || src.contains(QLatin1String("://"))) {
            continue;
        }

        // 去掉锚点和查询串（本地图片不会有，但万一有就别把文件名搞错）
        QString relative = src;
        int cut = relative.indexOf(QLatin1Char('#'));
        const int queryAt = relative.indexOf(QLatin1Char('?'));
        if (queryAt >= 0 && (cut < 0 || queryAt < cut)) {
            cut = queryAt;
        }
        if (cut >= 0) {
            relative.truncate(cut);
        }
        // src 里可能有 %20 这种转义（md4c 会把空格转义）
        relative = QUrl::fromPercentEncoding(relative.toUtf8());

        const QFileInfo info(QDir(baseDir).filePath(relative));
        if (!info.exists() || !info.isFile()) {
            if (result != nullptr) {
                ++result->imagesSkipped;
            }
            LOG_WARN("导出时找不到图片，保持原样: %1", info.absoluteFilePath());
            continue;
        }
        if (info.size() > kMaxInlineImageBytes) {
            if (result != nullptr) {
                ++result->imagesSkipped;
            }
            LOG_WARN("图片太大（%1 字节），不内联: %2", info.size(), info.absoluteFilePath());
            continue;
        }

        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            if (result != nullptr) {
                ++result->imagesSkipped;
            }
            LOG_WARN("图片读不了，不内联: %1（%2）", info.absoluteFilePath(), file.errorString());
            continue;
        }
        const QByteArray bytes = file.readAll();
        file.close();

        // 只换 src 的属性值：其余属性（alt/title/width…）原样保留
        out += html.mid(lastEnd, match.capturedStart(1) - lastEnd);
        out += encodeDataUri(bytes, mimeTypeForSuffix(info.suffix()));
        lastEnd = match.capturedEnd(1);

        if (result != nullptr) {
            ++result->imagesInlined;
        }
    }

    out += html.mid(lastEnd);
    return out;
}

// ============================ 选项 ============================

QPageLayout Exporter::buildPageLayout(const PdfOptions &options)
{
    const qreal margin = qMax<qreal>(0.0, options.marginMm);
    return QPageLayout(QPageSize(options.pageSize), options.orientation, QMarginsF(margin, margin, margin, margin), QPageLayout::Millimeter);
}

QString Exporter::buildDocumentHtml(const QString &markdown,
                                    const QString &baseDir,
                                    const QString &title,
                                    const QString &themeId,
                                    bool inlineImages,
                                    HtmlResult *result) const
{
    QString html = standaloneHtml(markdown, title, themeId);
    if (inlineImages) {
        html = inlineLocalImages(html, baseDir, result);
    }
    if (result != nullptr) {
        result->isEmpty = markdown.trimmed().isEmpty();
    }
    return html;
}

// ============================ HTML 导出 ============================

bool Exporter::exportHtml(const QString &markdown,
                          const QString &baseDir,
                          const QString &targetPath,
                          const HtmlOptions &options,
                          HtmlResult *result,
                          QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        LOG_ERROR("%1", why);
        return false;
    };

    if (targetPath.trimmed().isEmpty()) {
        return fail(QStringLiteral("没有指定导出到哪里"));
    }

    HtmlResult local;
    const QString title = options.title.trimmed().isEmpty() ? defaultTitleFor(targetPath) : options.title;
    const QString html = buildDocumentHtml(markdown, baseDir, title, options.themeId, options.inlineImages, &local);

    // 目录不存在就建：用户在"另存为"里手打一个还不存在的路径很常见
    const QString dir = QFileInfo(targetPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        return fail(QStringLiteral("建不了导出目录：%1").arg(dir));
    }

    // QSaveFile：先写临时文件再原子替换，写一半失败不会留下半个坏文件
    QSaveFile file(targetPath);
    if (!file.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("打不开要写入的文件（%1）：%2").arg(targetPath, file.errorString()));
    }
    const QByteArray bytes = html.toUtf8();
    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();
        return fail(QStringLiteral("写文件失败（可能是磁盘满或没有权限）：%1").arg(targetPath));
    }
    if (!file.commit()) {
        return fail(QStringLiteral("保存失败：%1").arg(file.errorString()));
    }

    local.bytes = bytes.size();
    if (result != nullptr) {
        *result = local;
    }

    LOG_INFO("已导出 HTML: %1（%2 字节，内联图片 %3 张，跳过 %4 张）",
             targetPath,
             local.bytes,
             local.imagesInlined,
             local.imagesSkipped);
    return true;
}

// ============================ PDF 导出 ============================

bool Exporter::isPdfRunning() const
{
    return m_pdfPage != nullptr;
}

void Exporter::exportPdf(const QString &markdown,
                         const QString &baseDir,
                         const QString &targetPath,
                         const PdfOptions &options,
                         const QString &title)
{
    if (m_pdfPage != nullptr) {
        emit pdfExported(targetPath, false, QStringLiteral("已经有一个 PDF 正在生成，等它结束再导"));
        return;
    }
    if (targetPath.trimmed().isEmpty()) {
        emit pdfExported(QString(), false, QStringLiteral("没有指定导出到哪里"));
        return;
    }

    const QString dir = QFileInfo(targetPath).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        emit pdfExported(targetPath, false, QStringLiteral("建不了导出目录：%1").arg(dir));
        return;
    }

    // 临时文件放系统临时目录：不往用户的笔记目录里塞东西。
    // 后缀必须是 .html —— Chromium 按内容类型决定怎么加载，给它一个奇怪的后缀会变成下载。
    const QString pattern = QDir(QDir::tempPath()).filePath(QStringLiteral("md-export-XXXXXX.html"));
    m_pdfTempFile = new QTemporaryFile(pattern, this);
    if (!m_pdfTempFile->open()) {
        finishPdf(false, QStringLiteral("建不了临时文件：%1").arg(m_pdfTempFile->errorString()));
        return;
    }

    // PDF 一律内联图片：临时文件在系统临时目录，相对路径的图片在那儿必然找不到
    HtmlResult ignored;
    const QString html = buildDocumentHtml(markdown,
                                           baseDir,
                                           title.trimmed().isEmpty() ? defaultTitleFor(targetPath) : title,
                                           options.themeId,
                                           true,
                                           &ignored);
    m_pdfTempFile->write(html.toUtf8());
    m_pdfTempFile->flush();

    m_pdfTargetPath = targetPath;
    m_pdfLayout = buildPageLayout(options);

    m_pdfPage = new QWebEnginePage(this);
    connect(m_pdfPage, &QWebEnginePage::loadFinished, this, &Exporter::onPdfLoadFinished);
    connect(m_pdfPage, &QWebEnginePage::pdfPrintingFinished, this, &Exporter::onPdfPrintingFinished);

    LOG_INFO("开始生成 PDF: %1（纸张 %2，方向 %3，边距 %4 mm）",
             targetPath,
             m_pdfLayout.pageSize().name(),
             m_pdfLayout.orientation() == QPageLayout::Landscape ? QStringLiteral("横向") : QStringLiteral("纵向"),
             options.marginMm);

    m_pdfWatchdog.start(kPdfTimeoutMs);
    m_pdfPage->setUrl(QUrl::fromLocalFile(m_pdfTempFile->fileName()));
}

void Exporter::onPdfLoadFinished(bool ok)
{
    if (m_pdfPage == nullptr) {
        return;  // 已经被超时/取消收掉了
    }
    if (!ok) {
        // 页面没加载成功还硬打，只会输出一份空白 PDF —— 那比直接报错更让人困惑
        finishPdf(false, QStringLiteral("导出用的页面加载失败（不想输出一份空白 PDF）"));
        return;
    }
    if (m_pdfTempFile != nullptr) {
        m_pdfTempFile->close();  // 页面已经读完了，早点放手
    }
    m_pdfPage->printToPdf(m_pdfTargetPath, m_pdfLayout);
}

void Exporter::onPdfPrintingFinished(const QString &path, bool ok)
{
    finishPdf(ok, ok ? QString() : QStringLiteral("Chromium 没能把 PDF 写到：%1").arg(path));
}

void Exporter::onPdfTimeout()
{
    finishPdf(false,
              QStringLiteral("生成 PDF 超时（%1 秒）。通常是 Chromium 没能启动或页面卡住了，"
                             "可以先试试导出 HTML。")
                  .arg(kPdfTimeoutMs / 1000));
}

void Exporter::finishPdf(bool ok, const QString &error)
{
    const QString path = m_pdfTargetPath;

    m_pdfWatchdog.stop();
    if (m_pdfPage != nullptr) {
        m_pdfPage->deleteLater();
        m_pdfPage = nullptr;
    }
    if (m_pdfTempFile != nullptr) {
        m_pdfTempFile->deleteLater();
        m_pdfTempFile = nullptr;
    }
    m_pdfTargetPath.clear();

    if (ok) {
        LOG_INFO("已导出 PDF: %1", path);
    } else {
        LOG_ERROR("导出 PDF 失败: %1（%2）", path, error);
    }
    emit pdfExported(path, ok, error);
}
