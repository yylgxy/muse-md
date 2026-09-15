// Exporter（5.6 导出系统）+ ExportDialog 的契约测试。
//
// 需要 QApplication（ExportDialog 是窗口控件）。
//
// 分工决定了怎么测：
//   * **HTML 导出是同步的**，所以能完整验证：文档结构、样式内联、图片打包成 data: URI、
//     原子写入、目录自动创建、失败原因 —— 全部真跑真读文件。
//   * **PDF 导出是异步的**（QWebEnginePage::printToPdf 走 Chromium 的打印管线），
//     受控环境里起不来，所以这里只验证"选项 → QPageLayout"这条纯规则，
//     以及参数不对时的**失败路径**（立刻以信号回来，不会卡住界面）。
//     真正的排版好不好看必须人工看一次（见 README 验收步骤）。
//   * **对话框**只收集设置：直接构造它、读 request()，不用 exec()，所以也不用有人点按钮。
//
// 全程在临时目录里干活，不往用户主目录或项目目录写东西。
//
// 跑法：ctest -C Debug --output-on-failure

#include "exportdialog.h"
#include "exporter.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPageSize>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

#include <cstdio>

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-62s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

void writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(bytes);
        file.close();
    }
}

QString readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    const QString base = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-export-test"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    const QString docs = base + QStringLiteral("/docs");
    QDir().mkpath(docs);

    // ============================ A. 拼 HTML（纯函数）============================
    {
        std::printf("---- A. 独立 HTML 文档 ----\n");

        const QString css = Exporter::previewStyleSheet();
        check(!css.isEmpty(), QStringLiteral("样式: 能从预览模板里抽到那段 CSS"), QStringLiteral("%1 字符").arg(css.size()));
        check(css.contains(QStringLiteral("font-family")) && css.contains(QStringLiteral("blockquote")),
              QStringLiteral("样式: 抽出来的确实是预览用的那份（含正文字体和引用样式）"));
        check(!css.contains(QStringLiteral("<style")), QStringLiteral("样式: 抽的是 <style> 里面的内容，不含标签本身"));

        const QString overrides = Exporter::exportOverrideStyleSheet();
        check(overrides.contains(QStringLiteral("@media print")), QStringLiteral("覆盖规则: 有打印/分页规则"));
        check(overrides.contains(QStringLiteral("padding: 0")),
              QStringLiteral("覆盖规则: 把预览那个 60vh 的下边距收回来（否则导出一整页空白）"));
        check(overrides.contains(QStringLiteral("page-break-inside")), QStringLiteral("覆盖规则: 代码块/表格不跨页断开"));

        const QString html = Exporter::standaloneHtml(QStringLiteral("# 标题\n\n正文 **粗体**\n"), QStringLiteral("我的笔记"));
        check(html.startsWith(QStringLiteral("<!DOCTYPE html>")), QStringLiteral("文档: 以 DOCTYPE 开头（浏览器按标准模式渲染）"));
        check(html.contains(QStringLiteral("<meta charset=\"utf-8\">")), QStringLiteral("文档: 声明了 UTF-8（中文不会乱码）"));
        check(html.contains(QStringLiteral("<title>我的笔记</title>")), QStringLiteral("文档: 标题进 <title>"));
        check(html.contains(QStringLiteral("<h1>标题</h1>")) && html.contains(QStringLiteral("<strong>粗体</strong>")),
              QStringLiteral("文档: Markdown 真的渲染成了 HTML"));
        check(html.contains(css.left(40)), QStringLiteral("文档: 样式内联在文件里（不依赖外部 css）"));
        check(!html.contains(QStringLiteral("stylesheet")), QStringLiteral("文档: 没有 <link> 外部引用"));
        check(!html.contains(QStringLiteral("qwebchannel")) && !html.contains(QStringLiteral("<script")),
              QStringLiteral("文档: 不含预览用的 WebChannel 脚本（导出文件不需要和 C++ 通话）"));
        check(html.trimmed().endsWith(QStringLiteral("</html>")), QStringLiteral("文档: 结构闭合"));

        // 标题转义：文件名叫 <未命名> 也不能把 HTML 弄坏
        check(Exporter::escapedTitle(QStringLiteral("a<b>&\"c\"")) == QStringLiteral("a&lt;b&gt;&amp;&quot;c&quot;"),
              QStringLiteral("escapedTitle: & < > \" 都被转义"), Exporter::escapedTitle(QStringLiteral("a<b>&\"c\"")));
        check(Exporter::standaloneHtml(QStringLiteral("x"), QStringLiteral("<脚本>"))
                  .contains(QStringLiteral("<title>&lt;脚本&gt;</title>")),
              QStringLiteral("文档: 标题里的尖括号不会破坏 HTML"));

        check(Exporter::defaultTitleFor(QStringLiteral("D:/notes/我的笔记.md")) == QStringLiteral("我的笔记"),
              QStringLiteral("defaultTitleFor: 取文件名、去掉扩展名"));
        check(Exporter::defaultTitleFor(QStringLiteral("a.b.md")) == QStringLiteral("a.b"),
              QStringLiteral("defaultTitleFor: 只去掉最后那个扩展名"));
        check(Exporter::defaultTitleFor(QString()) == QStringLiteral("未命名"),
              QStringLiteral("defaultTitleFor: 没路径 -> 未命名"));

        check(Exporter::mimeTypeForSuffix(QStringLiteral("PNG")) == QStringLiteral("image/png"),
              QStringLiteral("mimeTypeForSuffix: 大小写不敏感"));
        check(Exporter::mimeTypeForSuffix(QStringLiteral("jpg")) == QStringLiteral("image/jpeg"),
              QStringLiteral("mimeTypeForSuffix: jpg -> image/jpeg"));
        check(Exporter::mimeTypeForSuffix(QStringLiteral("svg")) == QStringLiteral("image/svg+xml"),
              QStringLiteral("mimeTypeForSuffix: svg"));
        check(Exporter::mimeTypeForSuffix(QStringLiteral("xyz")) == QStringLiteral("application/octet-stream"),
              QStringLiteral("mimeTypeForSuffix: 不认识的后缀 -> 二进制流"));

        check(Exporter::encodeDataUri(QByteArray("abc"), QStringLiteral("image/png"))
                  == QStringLiteral("data:image/png;base64,YWJj"),
              QStringLiteral("encodeDataUri: base64 与格式正确"),
              Exporter::encodeDataUri(QByteArray("abc"), QStringLiteral("image/png")));
    }

    // ============================ B. 图片内联 ============================
    {
        std::printf("---- B. 图片打包进 HTML ----\n");

        const QByteArray pngBytes("fake-png-bytes");
        writeFile(docs + QStringLiteral("/img/pic.png"), pngBytes);
        writeFile(docs + QStringLiteral("/img/big.png"), QByteArray(int(Exporter::kMaxInlineImageBytes) + 16, 'x'));

        const QString markdown = QStringLiteral("![小图](img/pic.png)\n\n"
                                                "![大图](img/big.png)\n\n"
                                                "![找不到](img/missing.png)\n\n"
                                                "![网络图](https://example.com/a.png)\n");

        Exporter::HtmlResult result;
        const QString html = Exporter::inlineLocalImages(Exporter::standaloneHtml(markdown, QStringLiteral("t")),
                                                         docs,
                                                         &result);

        check(result.imagesInlined == 1, QStringLiteral("内联: 只有那张本地小图被内联"),
              QStringLiteral("内联 %1 张").arg(result.imagesInlined));
        check(result.imagesSkipped == 2, QStringLiteral("内联: 太大的 + 找不到的 -> 跳过 2 张"),
              QStringLiteral("跳过 %1 张").arg(result.imagesSkipped));
        check(html.contains(QStringLiteral("data:image/png;base64,")),
              QStringLiteral("内联: 图片变成 data: URI（文件因此是独立的）"));
        check(html.contains(QStringLiteral("data:image/png;base64,") + QString::fromLatin1(pngBytes.toBase64())),
              QStringLiteral("内联: 内容一字不差（base64 正确）"));
        check(!html.contains(QStringLiteral("src=\"img/pic.png\"")),
              QStringLiteral("内联: 原来那个相对路径已经被换掉"));
        check(html.contains(QStringLiteral("src=\"img/big.png\"")),
              QStringLiteral("内联: 太大的图片保持原样（不悄悄丢内容）"));
        check(html.contains(QStringLiteral("src=\"img/missing.png\"")),
              QStringLiteral("内联: 找不到的图片保持原样"));
        check(html.contains(QStringLiteral("src=\"https://example.com/a.png\"")),
              QStringLiteral("内联: 网络图片不动（本来就自带出处）"));

        // 关掉内联：导出结果就是"没做过内联"的那份 HTML（相对路径原样保留）
        const QString plainTarget = base + QStringLiteral("/plain.html");
        Exporter plainExporter;
        Exporter::HtmlOptions plainOptions;
        plainOptions.inlineImages = false;
        plainOptions.title = QStringLiteral("t");
        QString plainError;
        check(plainExporter.exportHtml(markdown, docs, plainTarget, plainOptions, nullptr, &plainError),
              QStringLiteral("内联开关: 关掉之后导出仍然成功"), plainError);
        const QString plainWritten = readFile(plainTarget);
        check(plainWritten == Exporter::standaloneHtml(markdown, QStringLiteral("t")),
              QStringLiteral("内联开关: 关掉之后 HTML 与「不做内联」时逐字节相同"));
        check(plainWritten.contains(QStringLiteral("src=\"img/pic.png\"")),
              QStringLiteral("内联开关: 关掉之后图片仍是相对路径"));
    }

    // ============================ C. HTML 导出（真写文件）============================
    {
        std::printf("---- C. HTML 导出 ----\n");

        Exporter exporter;
        const QString markdown = QStringLiteral("# 导出测试\n\n- 一\n- 二\n\n```cpp\nint main() { return 0; }\n```\n");

        const QString target = base + QStringLiteral("/out/note.html");
        Exporter::HtmlResult result;
        Exporter::HtmlOptions options;
        options.title = QStringLiteral("导出测试");
        QString error;

        check(exporter.exportHtml(markdown, docs, target, options, &result, &error),
              QStringLiteral("导出: 成功"), error);
        check(QFileInfo::exists(target), QStringLiteral("导出: 文件真的写出来了"));
        check(result.bytes > 0 && result.bytes == QFileInfo(target).size(),
              QStringLiteral("导出: 统计的字节数和磁盘上一致"),
              QStringLiteral("%1 字节").arg(result.bytes));
        const QString written = readFile(target);
        check(written.contains(QStringLiteral("<h1>导出测试</h1>")) && written.contains(QStringLiteral("<li>一</li>")),
              QStringLiteral("导出: 正文渲染正确"));
        check(written.contains(QStringLiteral("<pre><code")), QStringLiteral("导出: 代码块渲染正确"));
        check(written.contains(QStringLiteral("font-family")), QStringLiteral("导出: 样式在文件里"));

        // 代码高亮（5.7）也要在导出文件里 —— 预览和导出走的是同一条渲染+高亮路径
        {
            const QString codeMarkdown = QStringLiteral("```cpp\nint main() { return 0; }  // 注释\n```\n");
            const QString codeTarget = base + QStringLiteral("/highlight.html");
            QString codeError;
            check(exporter.exportHtml(codeMarkdown, docs, codeTarget, options, nullptr, &codeError),
                  QStringLiteral("导出: 带代码块的文档导出成功"), codeError);
            const QString codeHtml = readFile(codeTarget);
            check(codeHtml.contains(QStringLiteral("hljs-type\">int</span>"))
                      && codeHtml.contains(QStringLiteral("hljs-comment\">// 注释</span>")),
                  QStringLiteral("导出: 代码块在导出文件里是**带高亮 span** 的（和预览一致）"));
            check(codeHtml.contains(QStringLiteral(".hljs-keyword")),
                  QStringLiteral("导出: 高亮的 CSS 也跟着抽进来了（否则 span 没有颜色）"));
        }

        // 目录不存在要自动建：用户手打一个不存在的路径很常见
        const QString deep = base + QStringLiteral("/a/b/c/deep.html");
        error.clear();
        check(exporter.exportHtml(markdown, docs, deep, options, nullptr, &error),
              QStringLiteral("导出: 目录不存在时自动创建"), error);
        check(QFileInfo::exists(deep), QStringLiteral("导出: 深路径也能写出来"));

        // 失败路径：路径为空
        error.clear();
        check(!exporter.exportHtml(markdown, docs, QString(), options, nullptr, &error),
              QStringLiteral("导出: 没给路径 -> false"));
        check(!error.isEmpty(), QStringLiteral("导出: 并且给出人能看懂的原因"), error);

        // 空文档：可以导出，但结果要标注"内容是空的"（界面好提醒一句）
        Exporter::HtmlResult emptyResult;
        const QString emptyTarget = base + QStringLiteral("/empty.html");
        error.clear();
        check(exporter.exportHtml(QString(), docs, emptyTarget, options, &emptyResult, &error),
              QStringLiteral("导出: 空文档也能导出（不是错误）"), error);
        check(emptyResult.isEmpty, QStringLiteral("导出: 空文档被标注出来（isEmpty = true）"));
        check(readFile(emptyTarget).contains(QStringLiteral("<!DOCTYPE html>")),
              QStringLiteral("导出: 空文档也是一个完整的 HTML 文件"));
    }

    // ============================ D. PDF 选项 → 页面设置（纯函数）============================
    {
        std::printf("---- D. PDF 页面设置 ----\n");

        Exporter::PdfOptions options;  // 默认：A4 纵向 12mm
        QPageLayout layout = Exporter::buildPageLayout(options);
        check(layout.pageSize().id() == QPageSize::A4, QStringLiteral("页面: 默认是 A4"));
        check(layout.orientation() == QPageLayout::Portrait, QStringLiteral("页面: 默认纵向"));
        check(qFuzzyCompare(layout.margins(QPageLayout::Millimeter).left(), 12.0),
              QStringLiteral("页面: 默认边距 12 mm"),
              QStringLiteral("%1 mm").arg(layout.margins(QPageLayout::Millimeter).left()));

        options.pageSize = QPageSize::Letter;
        options.orientation = QPageLayout::Landscape;
        options.marginMm = 25.0;
        layout = Exporter::buildPageLayout(options);
        check(layout.pageSize().id() == QPageSize::Letter, QStringLiteral("页面: 换成 Letter 生效"));
        check(layout.orientation() == QPageLayout::Landscape, QStringLiteral("页面: 横向生效"));
        check(qFuzzyCompare(layout.margins(QPageLayout::Millimeter).top(), 25.0)
                  && qFuzzyCompare(layout.margins(QPageLayout::Millimeter).bottom(), 25.0),
              QStringLiteral("页面: 四边边距都跟着走"));

        options.marginMm = -5.0;  // 负数没有意义：夹到 0，不要给 Chromium 一个非法值
        layout = Exporter::buildPageLayout(options);
        check(qFuzzyCompare(layout.margins(QPageLayout::Millimeter).left(), 0.0),
              QStringLiteral("页面: 负数边距被夹到 0（不给打印管线非法值）"),
              QStringLiteral("%1").arg(layout.margins(QPageLayout::Millimeter).left()));
    }

    // ============================ E. PDF 导出的失败路径（不等 Chromium）============================
    {
        std::printf("---- E. PDF 导出（只测立刻能判定的失败）----\n");

        Exporter exporter;
        QString seenPath = QStringLiteral("占位");
        bool seenOk = true;
        QString seenError;
        // 注意：变量别叫 signals —— 那是 Qt 的宏（展开成 public），会报出莫名其妙的语法错
        int signalCount = 0;
        QObject::connect(&exporter, &Exporter::pdfExported, [&](const QString &path, bool ok, const QString &error) {
            ++signalCount;
            seenPath = path;
            seenOk = ok;
            seenError = error;
        });

        check(!exporter.isPdfRunning(), QStringLiteral("PDF: 一开始没有任务在跑"));

        // 没给路径：应该**立刻**以失败信号回来，而不是起一个 Chromium 页面
        exporter.exportPdf(QStringLiteral("# x"), docs, QString());
        check(signalCount == 1 && !seenOk, QStringLiteral("PDF: 没给路径 -> 立刻失败（不会白等）"));
        check(seenError.contains(QStringLiteral("导出到哪里")), QStringLiteral("PDF: 原因说得清楚"), seenError);
        check(!exporter.isPdfRunning(), QStringLiteral("PDF: 失败之后没有留下「在跑」的状态"));
    }

    // ============================ F. 导出对话框（只收集设置）============================
    {
        std::printf("---- F. 导出对话框 ----\n");

        check(ExportDialog::suggestedPathFor(QStringLiteral("D:/notes/我的笔记.md"), ExportDialog::Format::Html)
                  == QStringLiteral("D:/notes/我的笔记.html"),
              QStringLiteral("建议路径: md -> html（同目录同名）"),
              ExportDialog::suggestedPathFor(QStringLiteral("D:/notes/我的笔记.md"), ExportDialog::Format::Html));
        check(ExportDialog::suggestedPathFor(QStringLiteral("D:/notes/我的笔记.md"), ExportDialog::Format::Pdf)
                  == QStringLiteral("D:/notes/我的笔记.pdf"),
              QStringLiteral("建议路径: md -> pdf"));
        check(ExportDialog::suggestedPathFor(QStringLiteral("a.b.md"), ExportDialog::Format::Html)
                  == QStringLiteral("a.b.html"),
              QStringLiteral("建议路径: 只换最后那个扩展名"));
        check(ExportDialog::suggestedPathFor(QString(), ExportDialog::Format::Pdf) == QStringLiteral("未命名.pdf"),
              QStringLiteral("建议路径: 没路径 -> 未命名.pdf"));
        check(ExportDialog::suggestedPathFor(QStringLiteral("D:/notes/noext"), ExportDialog::Format::Html)
                  == QStringLiteral("D:/notes/noext.html"),
              QStringLiteral("建议路径: 原来没扩展名也能加上"));

        check(ExportDialog::fileFilterFor(ExportDialog::Format::Html).contains(QStringLiteral("*.html")),
              QStringLiteral("文件过滤器: HTML"));
        check(ExportDialog::fileFilterFor(ExportDialog::Format::Pdf).contains(QStringLiteral("*.pdf")),
              QStringLiteral("文件过滤器: PDF"));

        // ---- 对话框本体 ----
        const QString suggested = base + QStringLiteral("/note.html");
        ExportDialog dialog(ExportDialog::Format::Html, suggested);
        ExportDialog::Request request = dialog.request();
        check(request.format == ExportDialog::Format::Html, QStringLiteral("对话框: 初始格式是 HTML"));
        check(request.targetPath == QDir::toNativeSeparators(suggested),
              QStringLiteral("对话框: 路径默认就是建议的那个（native 分隔符，人能读）"), request.targetPath);
        check(request.html.inlineImages, QStringLiteral("对话框: HTML 默认把图片一起打包"));
        check(request.title == QStringLiteral("note"), QStringLiteral("对话框: 标题从文档名推出来"), request.title);
        check(request.pdf.pageSize == QPageSize::A4 && request.pdf.orientation == QPageLayout::Portrait,
              QStringLiteral("对话框: PDF 默认 A4 纵向"));
        check(qFuzzyCompare(request.pdf.marginMm, 12.0), QStringLiteral("对话框: PDF 默认边距 12 mm"));

        // 换成 PDF：路径后缀要自动跟着换，否则用户会得到一个内容是 PDF 的 .html 文件
        request.format = ExportDialog::Format::Pdf;
        request.targetPath = base + QStringLiteral("/note.html");
        request.pdf.pageSize = QPageSize::A3;
        request.pdf.orientation = QPageLayout::Landscape;
        request.pdf.marginMm = 20.0;
        request.html.inlineImages = false;
        dialog.setRequest(request);

        const ExportDialog::Request asPdf = dialog.request();
        check(asPdf.format == ExportDialog::Format::Pdf, QStringLiteral("对话框: setRequest 能切到 PDF"));
        check(asPdf.targetPath.endsWith(QStringLiteral(".pdf")),
              QStringLiteral("对话框: 换格式时路径后缀自动换成 .pdf"), asPdf.targetPath);
        check(asPdf.pdf.pageSize == QPageSize::A3 && asPdf.pdf.orientation == QPageLayout::Landscape,
              QStringLiteral("对话框: 纸张和方向都记住了（不会被静默重置）"));
        check(qFuzzyCompare(asPdf.pdf.marginMm, 20.0), QStringLiteral("对话框: 边距记住了"));
        check(!asPdf.html.inlineImages, QStringLiteral("对话框: HTML 的开关也记住了"));

        // 再切回 HTML：后缀跟着回来
        ExportDialog::Request back = asPdf;
        back.format = ExportDialog::Format::Html;
        dialog.setRequest(back);
        check(dialog.request().targetPath.endsWith(QStringLiteral(".html")),
              QStringLiteral("对话框: 切回 HTML 后缀也回来"), dialog.request().targetPath);
        check(dialog.request().format == ExportDialog::Format::Html, QStringLiteral("对话框: 格式切回 HTML"));
    }

    QDir(base).removeRecursively();

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
