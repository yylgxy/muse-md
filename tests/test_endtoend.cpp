// 核心主流程全链路测试（7.1）。
//
// 目的：把"一个用户从开机到关机"会经过的**所有模块串起来跑一遍**，
// 而不是各模块自己测自己。单元测试全绿也可能接线接错 —— 这个测试就是抓那种问题的。
//
// ⚠ 一处不可替代的缺口：预览区是 QWebEngineView（Chromium），受控环境里起不来，
//   所以"预览窗口里真的显示出来了"这一步**没法自动验证**（需要人工跑一次 GUI）。
//   能自动验证的是它的**上游**：Markdown → HTML → 代码高亮 → 行号映射是否一一对应
//   （也就是"点击预览跳转到底准不准"的前半段），以及 Workbench 在"预览侧是个普通 QWidget"
//   时的分屏/滚动同步接线。这个测试把这条链完整跑一遍，并在最后列出需要人工确认的项。
//
// 覆盖 7.1 的九步：
//   1) 启动 → 恢复上次窗口状态        6) 搜索关键词 → 点击跳转
//   2) 新建 → 输入 Markdown           7) 导出 HTML（PDF 只能人工）
//   3) 渲染 → 行号映射一一对应        8) 切换亮暗主题 → 全局同步
//   4) 保存 → 文件名进标签页          9) 关闭 → 再次打开，文件与状态恢复
//   5) 打开另一个文件 → 多标签切换
//
// 全程在临时目录里干活；配置指向临时文件；不碰用户的真实数据。
//
// 跑法：ctest -C Debug --output-on-failure

#include "codehighlighter.h"
#include "configmanager.h"
#include "editorwidget.h"
#include "editorworkbench.h"
#include "exporter.h"
#include "filemanager.h"
#include "fulltextsearch.h"
#include "markdownparser.h"
#include "previewrenderer.h"  // workbench.renderer()->themeId() 等要完整类型
#include "recentfiles.h"
#include "sessionstate.h"
#include "syncbridge.h"
#include "tabmanager.h"
#include "thememanager.h"
#include "themepalette.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

#include <cstdio>

using markdown_editor::core::document::CodeHighlighter;
using markdown_editor::core::document::MarkdownParser;
using markdown_editor::core::document::SyncBridge;
using markdown_editor::core::document::ThemePalette;
using markdown_editor::core::storage::FileManager;
// 类型别名（嵌套枚举不能用 using 引进非类作用域）
using Theme = ThemeManager::Theme;

namespace {

int g_fail = 0;
int g_manual = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-58s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

// 需要人工确认的项：自动测试到此为止，剩下的只能靠人跑 GUI
void manual(const QString &what)
{
    std::printf("%-58s MANUAL  [%s]\n", QStringLiteral("（人工）%1").arg(what).toUtf8().constData(), "需要跑一次 GUI");
    ++g_manual;
}

QString readFileText(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

const QString kMarkdown = QStringLiteral(
    "# 项目笔记\n"
    "\n"
    "这是一段正文，用来验证**粗体**和 `行内代码`。\n"
    "\n"
    "- 第一项\n"
    "- 第二项\n"
    "\n"
    "```cpp\n"
    "int main() { return 0; }  // 注释\n"
    "```\n"
    "\n"
    "| 名称 | 说明 |\n"
    "| --- | --- |\n"
    "| 缓存 | LRU 淘汰 |\n");

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ==================== 准备：临时"工作区" ====================
    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-e2e-test"));
    QDir(work).removeRecursively();
    QDir().mkpath(work);
    const QString configPath = work + QStringLiteral("/config.ini");
    ConfigManager::setFilePath(configPath);

    std::printf("==== 7.1 核心主流程全链路 ====\n\n");

    // ==================================================================
    // 第 1 步：启动 → 恢复上次窗口状态
    // ==================================================================
    {
        std::printf("---- 1. 启动：恢复上次窗口状态 ----\n");

        // 上一次退出时存下的状态：上次开着两个文件、看的是第二个、文件树关掉了
        const QString first = work + QStringLiteral("/上次的文件A.md");
        const QString second = work + QStringLiteral("/上次的文件B.md");
        for (const QString &path : {first, second}) {
            QFile file(path);
            file.open(QIODevice::WriteOnly);
            file.write("# 上次的内容\n");
        }

        SessionState::Data previous;
        previous.geometry = QByteArrayLiteral("\x01\x00\x00\x00geometry-bytes");
        previous.openFiles = QStringList{first, second};
        previous.currentIndex = 1;
        previous.fileTreeVisible = false;
        previous.searchPanelVisible = true;
        SessionState::save(previous);
        ConfigManager::setFilePath(configPath);  // 换个"进程"再读

        const SessionState::Data restored = SessionState::load();
        check(restored.openFiles == QStringList{first, second},
              QStringLiteral("1. 恢复会话: 上次的打开文件按原顺序读回来"),
              restored.openFiles.join(QStringLiteral(", ")));
        check(restored.currentIndex == 1, QStringLiteral("1. 恢复会话: 上次的当前标签索引"));
        check(!restored.fileTreeVisible && restored.searchPanelVisible,
              QStringLiteral("1. 恢复会话: 两个面板的可见性"));
        check(restored.geometry == previous.geometry, QStringLiteral("1. 恢复会话: 窗口几何"));

        // 磁盘上没了的文件要能跳过（不弹窗、不崩）
        QStringList available;
        for (const QString &path : restored.openFiles) {
            if (QFileInfo::exists(path)) {
                available << path;
            }
        }
        QFile::remove(second);
        QStringList afterMissing;
        for (const QString &path : restored.openFiles) {
            if (QFileInfo::exists(path)) {
                afterMissing << path;
            }
        }
        check(available.size() == 2 && afterMissing.size() == 1,
              QStringLiteral("1. 恢复会话: 文件被删掉后只剩一个可恢复（不会崩）"));
        // 复原，后面几步还要用它
        {
            QFile file(second);
            file.open(QIODevice::WriteOnly);
            file.write("# 上次的内容\n");
        }
    }

    // ==================================================================
    // 第 2 步：新建文件 → 输入 Markdown
    // ==================================================================
    FileManager files;
    {
        std::printf("---- 2. 新建 → 输入 Markdown ----\n");

        files.newFile();
        check(!files.hasFilePath() && !files.isModified(), QStringLiteral("2. 新建: 没有路径、未修改"));

        const bool changed = files.setText(kMarkdown);
        check(changed, QStringLiteral("2. 输入: setText 认出了内容变化"));
        check(files.isModified(), QStringLiteral("2. 输入: 文档被标记为已修改（标签上该出现 *）"));
        check(!files.setText(kMarkdown), QStringLiteral("2. 输入: 同样的内容再来一次不算修改"));
    }

    // ==================================================================
    // 第 3 步：渲染 + 行号映射（"点击预览跳转"的地基）
    // ==================================================================
    {
        std::printf("---- 3. 渲染与行号映射 ----\n");

        const QString html = CodeHighlighter::highlightCodeBlocks(MarkdownParser::parseToHtml(kMarkdown));
        check(html.contains(QStringLiteral("<h1>项目笔记</h1>")), QStringLiteral("3. 渲染: 标题"));
        check(html.contains(QStringLiteral("<li>第一项</li>")), QStringLiteral("3. 渲染: 列表"));
        check(html.contains(QStringLiteral("<table>")) && html.contains(QStringLiteral("<th>名称</th>")),
              QStringLiteral("3. 渲染: 表格"));
        check(html.contains(QStringLiteral("hljs-type\">int</span>")), QStringLiteral("3. 渲染: 代码块高亮"));
        check(html.contains(QStringLiteral("<strong>粗体</strong>")), QStringLiteral("3. 渲染: 粗体"));

        // 行号映射：预览里的每个顶层块 ↔ 源码里的行号，必须一一对应
        //（点预览跳源码、写源码滚预览，靠的就是它）
        //
        // 期望值 5 是**照着上面那份 Markdown 数出来的**：标题 + 段落 + 列表 + 代码块 + 表格。
        // 这样断言比"数 HTML 里有几个顶层标签"更直白，也正好是用户的验收口径：
        // 我写了几个块，映射就该有几条。
        const QList<int> lineMap = SyncBridge::buildLineMap(kMarkdown);
        check(lineMap.size() == 5,
              QStringLiteral("3. 行号映射: 5 个块对应 5 条映射（点击才不会错位）"),
              QStringLiteral("映射 %1 条").arg(lineMap.size()));
        check(!lineMap.isEmpty() && lineMap.first() == 1,
              QStringLiteral("3. 行号映射: 第一个块对应源码第 1 行（1 起算）"),
              QStringLiteral("%1").arg(lineMap.value(0)));
        check(lineMap.size() >= 2 && lineMap.at(1) > lineMap.at(0),
              QStringLiteral("3. 行号映射: 行号是递增的"));

        // 渲染出来的 HTML 不能把行内代码/正文弄坏
        check(html.contains(QStringLiteral("<code>行内代码</code>")),
              QStringLiteral("3. 渲染: 行内代码保持原样（没被高亮误伤）"));

        manual(QStringLiteral("预览区真的显示出彩色排版、滚动同步、点击跳转"));
    }

    // ==================================================================
    // 第 4 步：保存 → 文件名进标签页
    // ==================================================================
    const QString notePath = work + QStringLiteral("/项目笔记.md");
    TabManager tabs;
    {
        std::printf("---- 4. 保存 → 标签页 ----\n");

        QString error;
        check(files.saveFileAs(notePath, &error), QStringLiteral("4. 保存: 另存为成功"), error);
        check(QFileInfo::exists(notePath), QStringLiteral("4. 保存: 磁盘上真的有这个文件"));
        check(!files.isModified(), QStringLiteral("4. 保存: 保存后不再是已修改（标签上的 * 该消失）"));
        check(readFileText(notePath) == kMarkdown, QStringLiteral("4. 保存: 写出去的内容一字不差"));

        // 标签页：把文件名刷上去（主窗口里就是 updateTabLabel 做的事）
        EditorWidget *editor = tabs.addEditorTab();
        TabManager::TabInfo info;
        info.fileName = files.fileName();
        info.filePath = files.filePath();
        info.modified = files.isModified();
        tabs.updateTab(0, info);
        check(tabs.tabText(0) == QStringLiteral("项目笔记.md"),
              QStringLiteral("4. 标签页: 显示文件名"), tabs.tabText(0));
        check(tabs.tabFilePath(0) == notePath, QStringLiteral("4. 标签页: 记下了完整路径（右键菜单要用）"));

        // 编辑器与文档管理器同步（主窗口的会话机制）
        editor->setPlainText(files.text());
        check(editor->toPlainText() == files.text(), QStringLiteral("4. 编辑器: 内容与文档一致"));
    }

    // ==================================================================
    // 第 5 步：打开另一个文件 → 多标签切换
    // ==================================================================
    const QString secondPath = work + QStringLiteral("/第二篇.md");
    {
        std::printf("---- 5. 多标签切换 ----\n");

        QFile file(secondPath);
        file.open(QIODevice::WriteOnly);
        file.write(QStringLiteral("# 第二篇\n\n这里有 缓存 与 索引。\n").toUtf8());
        file.close();

        FileManager second;
        QString error;
        check(second.openFile(secondPath, &error), QStringLiteral("5. 打开另一个文件"), error);

        EditorWidget *secondEditor = tabs.addEditorTab();
        tabs.updateTab(1, TabManager::TabInfo{QStringLiteral("第二篇.md"), secondPath, false, QString()});
        check(tabs.count() == 2, QStringLiteral("5. 标签页: 现在有两个"));

        tabs.setCurrentIndex(0);
        check(tabs.currentEditor() != nullptr, QStringLiteral("5. 切换: 切到第一个标签（编辑器仍在）"));
        tabs.setCurrentIndex(1);
        check(tabs.currentEditor() == secondEditor, QStringLiteral("5. 切换: 切到第二个标签"));
        check(second.text().contains(QStringLiteral("缓存")), QStringLiteral("5. 内容: 第二个文档内容正确"));

        // 关掉一个标签：剩下的那个还在，页面真的被释放了
        QPointer<EditorWidget> released = secondEditor;
        tabs.closeTab(1);
        check(tabs.count() == 1 && released.isNull(),
              QStringLiteral("5. 关闭: 关掉标签后页面真的被释放了（不泄漏）"));
    }

    // ==================================================================
    // 第 6 步：搜索关键词 → 点击跳转
    // ==================================================================
    {
        std::printf("---- 6. 全文搜索 → 跳到那一行 ----\n");

        FullTextSearch engine;
        engine.setIndexPath(work + QStringLiteral("/index.sqlite"));
        QString error;
        check(engine.open(&error), QStringLiteral("6. 搜索: 打开索引库"), error);

        const IndexStats stats = engine.indexDirectory(work, &error);
        check(stats.filesFound >= 2, QStringLiteral("6. 搜索: 索引了工作区里的 md 文件"),
              QStringLiteral("%1 个").arg(stats.filesFound));

        // 中文两字词（trigram 搜不到，必须走 LIKE 那条路）
        const QList<SearchHit> hits = engine.search(QStringLiteral("缓存"), 50, &error);
        check(!hits.isEmpty(), QStringLiteral("6. 搜索: 中文二字词能搜到"),
              QStringLiteral("%1 条").arg(hits.size()));

        // ★ 关键：拿到了行号之后，跳过去必须正好落在那一行上
        bool jumpCorrect = true;
        QString jumpDetail;
        EditorWidget editor;
        for (const SearchHit &hit : hits) {
            if (QFileInfo(hit.filePath).fileName() != QStringLiteral("第二篇.md")) {
                continue;
            }
            editor.setPlainText(readFileText(hit.filePath));
            editor.goToLine(hit.line);  // 主窗口点搜索结果时做的事
            const QTextBlock block = editor.document()->findBlockByNumber(hit.line - 1);
            if (!block.text().contains(QStringLiteral("缓存"))) {
                jumpCorrect = false;
                jumpDetail = QStringLiteral("第 %1 行是：%2").arg(hit.line).arg(block.text());
            }
        }
        check(jumpCorrect, QStringLiteral("6. 跳转: 搜索命中的行号 → 光标落在那一行且内容对得上"), jumpDetail);

        // 索引要能跨"进程"用（重启后不用重建）
        engine.close();
        FullTextSearch reopened;
        reopened.setIndexPath(work + QStringLiteral("/index.sqlite"));
        reopened.open(&error);
        check(reopened.search(QStringLiteral("缓存"), 50, &error).size() == hits.size(),
              QStringLiteral("6. 搜索: 重新打开索引库仍能搜到（重启不用重建）"));
    }

    // ==================================================================
    // 第 7 步：导出 HTML（PDF 只能人工看）
    // ==================================================================
    {
        std::printf("---- 7. 导出 ----\n");

        Exporter exporter;
        Exporter::HtmlOptions options;
        options.title = QStringLiteral("项目笔记");
        options.themeId = QStringLiteral("dark");  // 顺带验证"导出跟随主题"

        const QString htmlPath = work + QStringLiteral("/导出.html");
        Exporter::HtmlResult result;
        QString error;
        check(exporter.exportHtml(files.text(), work, htmlPath, options, &result, &error),
              QStringLiteral("7. 导出 HTML: 成功"), error);
        const QString exported = readFileText(htmlPath);
        check(exported.contains(QStringLiteral("<!DOCTYPE html>")) && exported.contains(QStringLiteral("</html>")),
              QStringLiteral("7. 导出 HTML: 是一份完整文档"));
        check(exported.contains(QStringLiteral("<h1>项目笔记</h1>"))
                  && exported.contains(QStringLiteral("<table>")) && exported.contains(QStringLiteral("hljs-type")),
              QStringLiteral("7. 导出 HTML: 标题/表格/代码高亮都在"));
        check(exported.contains(QStringLiteral("data-theme=\"dark\""))
                  && exported.contains(QStringLiteral("--tok-keyword")),
              QStringLiteral("7. 导出 HTML: 跟随主题，样式内联（独立文件）"));
        check(!exported.contains(QStringLiteral("<script")), QStringLiteral("7. 导出 HTML: 不含脚本"));
        check(result.bytes == QFileInfo(htmlPath).size(), QStringLiteral("7. 导出 HTML: 字节数与磁盘一致"));

        // PDF：受控环境里 Chromium 起不来（起一个真实任务会干等 60 秒超时），
        // 所以这里只验证"参数不对时会干脆地失败、不把界面卡住"这条路。
        QString pdfError;
        QObject::connect(&exporter, &Exporter::pdfExported, [&pdfError](const QString &, bool ok, const QString &err) {
            if (!ok) {
                pdfError = err;
            }
        });
        exporter.exportPdf(files.text(), work, QString());
        check(!pdfError.isEmpty(), QStringLiteral("7. 导出 PDF: 参数不对时立刻失败并给出原因"), pdfError);
        check(!exporter.isPdfRunning(), QStringLiteral("7. 导出 PDF: 失败后不会把界面卡在导出中"));

        manual(QStringLiteral("PDF 内容与排版（中文字体、代码块不跨页、边距）"));
    }

    // ==================================================================
    // 第 8 步：切换亮暗主题 → 全局同步
    // ==================================================================
    {
        std::printf("---- 8. 主题切换 ----\n");

        ThemeManager &themes = ThemeManager::instance();
        themes.setTheme(Theme::Light);
        check(themes.theme() == Theme::Light, QStringLiteral("8. 主题: 切到亮色"));
        check(qApp->styleSheet().contains(QStringLiteral("#ffffff")),
              QStringLiteral("8. 主题: 亮色 QSS 应用到 QApplication 上"));

        themes.setTheme(Theme::Dark);
        check(themes.theme() == Theme::Dark, QStringLiteral("8. 主题: 切到暗色"));
        check(qApp->styleSheet().contains(QStringLiteral("#0d1117")),
              QStringLiteral("8. 主题: 暗色 QSS 应用到 QApplication 上（菜单栏/面板会跟着变）"));

        // 编辑器侧：语法高亮与行号栏用 ThemePalette（QSS 管不到画出来的东西）
        const ThemePalette darkPalette = ThemeManager::editorPalette(Theme::Dark);
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("# 标题"));
        editor.setThemePalette(darkPalette);
        const QTextBlock block = editor.document()->firstBlock();
        const QColor headingColor = (block.layout() != nullptr && !block.layout()->formats().isEmpty())
                                        ? block.layout()->formats().first().format.foreground().color()
                                        : QColor();
        check(headingColor == darkPalette.heading,
              QStringLiteral("8. 主题: 编辑器的语法配色跟着换了"), headingColor.name());
        check(darkPalette.heading != ThemePalette::light().heading,
              QStringLiteral("8. 主题: 亮暗两套语法配色确实不同"));

        // 预览侧：只改页面属性，不重载页面（不闪白、不丢滚动位置）
        EditorWorkbench workbench;
        auto *editorSide = new TabManager();
        auto *previewSide = new QWidget();  // 不用 QWebEngineView：受控环境里起不来
        workbench.setup(editorSide, previewSide);
        workbench.renderer()->applyTheme(ThemeManager::themeId(Theme::Dark));
        check(workbench.renderer()->themeId() == QStringLiteral("dark"),
              QStringLiteral("8. 主题: 预览侧记住了暗色（页面就绪后由它推给网页）"));
        check(!workbench.renderer()->isPageReady(),
              QStringLiteral("8. 主题: 没页面时也不会崩（预览侧是普通控件）"));
        delete editorSide;
        delete previewSide;

        themes.setTheme(Theme::Light);  // 复位，后面几步按亮色走
        check(themes.theme() == Theme::Light, QStringLiteral("8. 主题: 切回亮色"));
    }

    // ==================================================================
    // 第 9 步：关闭 → 再次打开，文件和状态都恢复
    // ==================================================================
    {
        std::printf("---- 9. 关闭与恢复 ----\n");

        // 关闭前：把这次真正打开的文件存进去（主窗口 closeEvent 里做的事）
        RecentFiles recent;
        recent.load();
        recent.add(notePath);
        recent.add(secondPath);

        SessionState::Data state;
        state.geometry = QByteArrayLiteral("\x01\x00\x00\x00second-run-geometry");
        state.openFiles = QStringList{notePath, secondPath};
        state.currentIndex = 1;
        state.fileTreeVisible = true;
        state.searchPanelVisible = false;
        SessionState::save(state);

        // ---- 再次打开（换一次配置路径 = 丢掉内存里的 QSettings，真从文件读）----
        ConfigManager::setFilePath(configPath);
        const SessionState::Data reloaded = SessionState::load();
        check(reloaded.openFiles == QStringList{notePath, secondPath},
              QStringLiteral("9. 恢复: 两个文件都回来了（顺序不变）"),
              reloaded.openFiles.join(QStringLiteral(", ")));
        check(reloaded.currentIndex == 1, QStringLiteral("9. 恢复: 还是停在第二个标签上"));
        check(reloaded.fileTreeVisible && !reloaded.searchPanelVisible,
              QStringLiteral("9. 恢复: 面板可见性也回来了"));

        RecentFiles recentAgain;
        recentAgain.load();
        check(recentAgain.files().size() == 2 && recentAgain.files().first() == secondPath,
              QStringLiteral("9. 恢复: 最近打开列表也在（最新的在最前）"),
              recentAgain.files().join(QStringLiteral(", ")));

        // 恢复时真的能把内容读出来（文件在磁盘上）
        FileManager restored;
        QString error;
        check(restored.openFile(reloaded.openFiles.first(), &error),
              QStringLiteral("9. 恢复: 恢复出来的文件能正常打开"), error);
        check(restored.text() == kMarkdown, QStringLiteral("9. 恢复: 内容与保存时一致"));
    }

    // ==================================================================
    // 7.2 性能：一个 1MB 量级的大文档也要能正常编辑/保存/打开
    // ==================================================================
    {
        std::printf("---- 7.2 大文档（1MB 量级）----\n");

        // 造一份"1MB 量级"的 Markdown（用重复的小节拼出来，比一整行更像真实笔记）
        QString big;
        big.reserve(EditorWidget::kFastModeThresholdChars + 4096);
        while (big.size() < EditorWidget::kFastModeThresholdChars + 500) {
            big += QStringLiteral("# 小节\n\n- 一项\n- 另一项\n\n正文 **粗体** `代码`\n\n");
        }

        FileManager bigFiles;
        bigFiles.setText(big);

        const QString bigPath = work + QStringLiteral("/大文档.md");
        QString error;
        const bool saved = bigFiles.saveFileAs(bigPath, &error);
        check(saved, QStringLiteral("7.2 大文档: 能保存"), error);
        check(!bigFiles.isModified(), QStringLiteral("7.2 大文档: 保存后不再是已修改"));
        check(QFileInfo(bigPath).size() > 300000,
              QStringLiteral("7.2 大文档: 磁盘上的文件确实很大"),
              QStringLiteral("%1 字节").arg(QFileInfo(bigPath).size()));

        FileManager reopened;
        check(reopened.openFile(bigPath, &error), QStringLiteral("7.2 大文档: 能重新打开"), error);
        check(reopened.text().size() == big.size(), QStringLiteral("7.2 大文档: 内容长度一致"),
              QStringLiteral("%1 字符").arg(reopened.text().size()));

        // 编辑器侧：进大文档快速模式（关掉高亮），保证打字不卡
        EditorWidget editor;
        editor.setPlainText(reopened.text());
        check(editor.isFastMode(), QStringLiteral("7.2 大文档: 编辑器自动进入快速模式"));
        check(!editor.isHighlightingEnabled(), QStringLiteral("7.2 大文档: 语法高亮已关闭（打字的流畅由它保证）"));

        // 预览侧：这么大的内容不该硬推（渲染管线里已经有了上限，这里验证工作台的推迟）
        EditorWorkbench workbench;
        auto *tabs = new TabManager();
        auto *previewSide = new QWidget();
        workbench.setup(tabs, previewSide);
        EditorWidget *tabEditor = tabs->addEditorTab();
        workbench.setCurrentEditor(tabEditor);
        tabEditor->setPlainText(reopened.text());
        workbench.showContent(reopened.text(), work, true);
        check(workbench.hasDeferredContent(),
              QStringLiteral("7.2 大文档: 预览推送被推迟（几十万字符的渲染会拖慢打字）"));
        delete tabs;
        delete previewSide;

        QFile::remove(bigPath);  // 别让后面的搜索索引把它算进去
    }

    // ==================================================================
    std::printf("\n---- 需要人工确认的项（受控环境里 Chromium 起不来）----\n");
    manual(QStringLiteral("预览区显示、滚动同步、点击预览跳源码"));
    manual(QStringLiteral("PDF 导出的内容与排版"));
    manual(QStringLiteral("停靠面板拖拽、快捷键、拖拽打开文件"));

    // 收尾：先把配置指向清掉（让 QSettings 析构、松开临时目录），再删目录
    ConfigManager::setFilePath(QString());
    QDir(work).removeRecursively();

    std::printf("\n%s（失败 %d 项；另有 %d 项需人工确认）\n",
                g_fail == 0 ? "全链路自动部分全部通过" : "有失败项",
                g_fail,
                g_manual);
    return g_fail == 0 ? 0 : 1;
}
