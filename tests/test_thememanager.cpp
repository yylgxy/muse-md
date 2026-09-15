// ThemeManager（5.7 亮暗主题系统）的契约测试。
//
// 需要 QApplication：主题是**应用级**的东西（QApplication 的样式表与调色板），
// 而且要构造真的控件才能验证"颜色真的换了"。
//
// 这个测试盯四类东西：
//   1. 主题规则（纯函数）：themeId / themeFromId 的往返、认不出来的值退化成亮色。
//   2. 两套 QSS：真的能从 qrc 读到、选择器清单一致（加了控件忘了在另一套里补会露馅）、
//      而且编辑器底色和 ThemePalette（C++ 配色表）**不打架**（两边不一致了会出现
//      "文字是深色但背景还是深色"这种看得见但很难查的问题）。
//   3. 配色质量：亮暗两套每个字段都不同；正文/语法色对编辑器底色的对比度达标（WCAG AA），
//      行号是次要信息可以低一些但也有下限。颜色这件事写成测试才靠得住。
//   4. 切换行为：setTheme 换掉 QSS/调色板、发一次信号、落盘；**同一个主题重复设置不发信号、
//      不重新应用**（这正是"无闪烁"的一条保证）；编辑器与预览渲染器各自响应。
//
// ConfigManager 指向临时目录，不碰用户真实的 config.ini。
//
// 跑法：ctest -C Debug --output-on-failure

#include "configmanager.h"
#include "editorwidget.h"
#include "markdownhighlighter.h"  // 要显式 rehighlight()（读取格式前先确保高亮跑过）
#include "previewrenderer.h"
#include "thememanager.h"
#include "themepalette.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>

#include <cstdio>

using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::ThemePalette;
// 用类型别名（不能写 using ThemeManager::Theme —— 嵌套枚举不能这样引进来）
using Theme = ThemeManager::Theme;

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

// 从 QSS 里把选择器抠出来（"{" 之前的那一段），用于"两套文件选择器清单必须一致"的检查
QStringList selectorsOf(const QString &qss)
{
    QStringList selectors;
    static const QRegularExpression re(QStringLiteral("([^{}]+)\\{"));
    QRegularExpressionMatchIterator it = re.globalMatch(qss);
    while (it.hasNext()) {
        const QString raw = it.next().captured(1).trimmed();
        // 去掉注释行（QSS 里 /* … */ 也算在 {} 之前的那一坨里）
        QString cleaned = raw;
        cleaned.remove(QRegularExpression(QStringLiteral("/\\*.*?\\*/"),
                                          QRegularExpression::DotMatchesEverythingOption));
        cleaned = cleaned.trimmed();
        if (!cleaned.isEmpty()) {
            selectors << cleaned.simplified();
        }
    }
    return selectors;
}

QColor colorOfFirstFormat(const EditorWidget &editor)
{
    const QTextBlock block = editor.document()->firstBlock();
    const QTextLayout *layout = block.layout();
    if (layout == nullptr || layout->formats().isEmpty()) {
        return QColor();
    }
    return layout->formats().first().format.foreground().color();
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 配置指向临时文件：主题会被落盘，不能写到用户真实的 config.ini 里
    const QString base = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-theme-test"));
    QDir(base).removeRecursively();
    QDir().mkpath(base);
    ConfigManager::setFilePath(base + QStringLiteral("/config.ini"));

    // ============================ A. 主题规则（纯函数）============================
    {
        std::printf("---- A. 主题规则 ----\n");

        check(ThemeManager::themeId(Theme::Light) == QStringLiteral("light"),
              QStringLiteral("themeId: 亮色 -> light"));
        check(ThemeManager::themeId(Theme::Dark) == QStringLiteral("dark"),
              QStringLiteral("themeId: 暗色 -> dark"));
        check(ThemeManager::themeFromId(QStringLiteral("dark")) == Theme::Dark,
              QStringLiteral("themeFromId: dark"));
        check(ThemeManager::themeFromId(QStringLiteral("DARK")) == Theme::Dark,
              QStringLiteral("themeFromId: 大小写不敏感"));
        check(ThemeManager::themeFromId(QStringLiteral("  light ")) == Theme::Light,
              QStringLiteral("themeFromId: 去掉空白"));
        check(ThemeManager::themeFromId(QStringLiteral("紫色")) == Theme::Light,
              QStringLiteral("themeFromId: 认不出来 -> 亮色（配置被改坏了也不能崩）"));
        check(ThemeManager::themeFromId(QString()) == Theme::Light, QStringLiteral("themeFromId: 空 -> 亮色"));
        check(ThemeManager::themeFromId(ThemeManager::themeId(Theme::Dark)) == Theme::Dark
                  && ThemeManager::themeFromId(ThemeManager::themeId(Theme::Light)) == Theme::Light,
              QStringLiteral("themeId/themeFromId: 两个方向都能往返"));
        check(ThemeManager::storageKey() == QStringLiteral("theme"),
              QStringLiteral("storageKey: 配置键名固定为 theme"), ThemeManager::storageKey());

        // 单例：拿两次是同一个对象
        check(&ThemeManager::instance() == &ThemeManager::instance(),
              QStringLiteral("单例: instance() 每次都返回同一个对象"));
    }

    // ============================ B. 两套 QSS 资源 ============================
    {
        std::printf("---- B. 两套 QSS ----\n");

        const QString lightCss = ThemeManager::styleSheetFor(Theme::Light);
        const QString darkCss = ThemeManager::styleSheetFor(Theme::Dark);

        check(!lightCss.isEmpty() && !darkCss.isEmpty(),
              QStringLiteral("资源: 两份 QSS 都能从 qrc 读到（忘了加进 .qrc 时这里会失败）"),
              QStringLiteral("light %1 字符 / dark %2 字符").arg(lightCss.size()).arg(darkCss.size()));
        check(lightCss != darkCss, QStringLiteral("资源: 两份内容确实不一样"));

        // 控件覆盖：两套都要说到这些……否则切主题会留下没被覆盖的系统色块（看着像没切干净）
        const QStringList requiredSelectors = {QStringLiteral("QMenuBar"),   QStringLiteral("QMenu"),
                                               QStringLiteral("QToolBar"),   QStringLiteral("QTabBar::tab"),
                                               QStringLiteral("QPlainTextEdit"), QStringLiteral("QTreeView"),
                                               QStringLiteral("QLineEdit"),  QStringLiteral("QPushButton"),
                                               QStringLiteral("QStatusBar"), QStringLiteral("QDockWidget"),
                                               QStringLiteral("QScrollBar"), QStringLiteral("QToolTip"),
                                               QStringLiteral("QSplitter::handle")};
        QStringList missing;
        for (const QString &selector : requiredSelectors) {
            if (!lightCss.contains(selector)) {
                missing << selector;
            }
            if (!darkCss.contains(selector)) {
                missing << (QStringLiteral("dark:") + selector);
            }
        }
        check(missing.isEmpty(),
              QStringLiteral("覆盖: 两套 QSS 都覆盖了 %1 类关键控件").arg(requiredSelectors.size()),
              missing.join(QStringLiteral(", ")));

        // 选择器清单一致：加了控件忘了在另一套里补，切换时就会出现"这里还是旧样式"
        QStringList lightSelectors = selectorsOf(lightCss);
        QStringList darkSelectors = selectorsOf(darkCss);
        lightSelectors.sort();
        darkSelectors.sort();
        check(lightSelectors == darkSelectors,
              QStringLiteral("结构: 两套 QSS 的选择器清单完全一致"),
              QStringLiteral("light %1 条 / dark %2 条").arg(lightSelectors.size()).arg(darkSelectors.size()));

        // ★ 契约：QSS 里的编辑器底色必须等于 ThemePalette 的底色。
        //   两处不一致的表现是"文字换了色、背景还是旧色"，非常容易被看成渲染 bug。
        const ThemePalette lightPalette = ThemePalette::light();
        const ThemePalette darkPalette = ThemePalette::dark();
        check(lightCss.contains(lightPalette.editorBackground.name()),
              QStringLiteral("契约: 亮色 QSS 的编辑器底色 = ThemePalette::light()"),
              lightPalette.editorBackground.name());
        check(darkCss.contains(darkPalette.editorBackground.name()),
              QStringLiteral("契约: 暗色 QSS 的编辑器底色 = ThemePalette::dark()"),
              darkPalette.editorBackground.name());
        check(lightCss.contains(lightPalette.editorForeground.name()),
              QStringLiteral("契约: 亮色 QSS 的编辑器文字色 = ThemePalette::light()"));
        check(darkCss.contains(darkPalette.editorForeground.name()),
              QStringLiteral("契约: 暗色 QSS 的编辑器文字色 = ThemePalette::dark()"));
    }

    // ============================ C. 配色质量（两套互不相同 + 对比度）============================
    {
        std::printf("---- C. 配色质量 ----\n");

        const ThemePalette light = ThemePalette::light();
        const ThemePalette dark = ThemePalette::dark();

        // 每个字段都必须不同：漏一个的表现就是"切了主题但某处还是旧颜色"
        struct Field
        {
            const char *name;
            QColor light;
            QColor dark;
        };
        const QList<Field> fields = {
            {"editorBackground", light.editorBackground, dark.editorBackground},
            {"editorForeground", light.editorForeground, dark.editorForeground},
            {"gutterBackground", light.gutterBackground, dark.gutterBackground},
            {"gutterText", light.gutterText, dark.gutterText},
            {"currentLineNumberText", light.currentLineNumberText, dark.currentLineNumberText},
            {"currentLineHighlight", light.currentLineHighlight, dark.currentLineHighlight},
            {"heading", light.heading, dark.heading},
            {"bold", light.bold, dark.bold},
            {"italic", light.italic, dark.italic},
            {"inlineCodeForeground", light.inlineCodeForeground, dark.inlineCodeForeground},
            {"inlineCodeBackground", light.inlineCodeBackground, dark.inlineCodeBackground},
            {"link", light.link, dark.link},
            {"blockQuote", light.blockQuote, dark.blockQuote},
            {"listMarker", light.listMarker, dark.listMarker},
            {"codeBlockForeground", light.codeBlockForeground, dark.codeBlockForeground},
            {"codeBlockBackground", light.codeBlockBackground, dark.codeBlockBackground},
        };
        QStringList same;
        for (const Field &field : fields) {
            if (field.light == field.dark) {
                same << QString::fromLatin1(field.name);
            }
        }
        check(same.isEmpty(), QStringLiteral("配色: 亮暗两套的 %1 个字段**每一个**都不同").arg(fields.size()),
              same.join(QStringLiteral(", ")));

        // 亮色底要比暗色底亮（反了就是把两套配色接反了）
        check(ThemePalette::relativeLuminance(light.editorBackground)
                  > ThemePalette::relativeLuminance(dark.editorBackground),
              QStringLiteral("配色: 亮色编辑器底色确实比暗色亮"));
        check(ThemePalette::relativeLuminance(dark.editorBackground) < 0.1,
              QStringLiteral("配色: 暗色底色确实是深色（不是灰）"),
              QStringLiteral("亮度 %1").arg(ThemePalette::relativeLuminance(dark.editorBackground), 0, 'f', 3));
        check(ThemePalette::relativeLuminance(light.editorBackground) > 0.7,
              QStringLiteral("配色: 亮色底色确实接近白色"));

        // 对比度（WCAG）：正文和所有语法色对底色都要能看清
        const auto requireContrast = [](const QString &label, const QColor &fg, const QColor &bg, double minimum) {
            const double ratio = ThemePalette::contrastRatio(fg, bg);
            check(ratio >= minimum,
                  QStringLiteral("对比度: %1 >= %2").arg(label).arg(minimum, 0, 'f', 1),
                  QStringLiteral("%1:1（%2 / %3）")
                      .arg(ratio, 0, 'f', 2)
                      .arg(fg.name(), bg.name()));
        };

        requireContrast(QStringLiteral("亮色 正文/底色"), light.editorForeground, light.editorBackground, 7.0);
        requireContrast(QStringLiteral("暗色 正文/底色"), dark.editorForeground, dark.editorBackground, 7.0);
        requireContrast(QStringLiteral("亮色 行号/行号栏"), light.gutterText, light.gutterBackground, 2.5);
        requireContrast(QStringLiteral("暗色 行号/行号栏"), dark.gutterText, dark.gutterBackground, 2.5);
        requireContrast(QStringLiteral("亮色 当前行号/行号栏"), light.currentLineNumberText, light.gutterBackground, 4.5);
        requireContrast(QStringLiteral("暗色 当前行号/行号栏"), dark.currentLineNumberText, dark.gutterBackground, 4.5);

        // 语法色：逐个对着**编辑器底色**算（它们是在正文里出现的）
        const QList<QPair<QString, QColor>> lightSyntax = {
            {QStringLiteral("亮色 标题"), light.heading},
            {QStringLiteral("亮色 粗体"), light.bold},
            {QStringLiteral("亮色 斜体"), light.italic},
            {QStringLiteral("亮色 行内代码"), light.inlineCodeForeground},
            {QStringLiteral("亮色 链接"), light.link},
            {QStringLiteral("亮色 引用"), light.blockQuote},
            {QStringLiteral("亮色 列表标记"), light.listMarker},
            {QStringLiteral("亮色 代码块文字"), light.codeBlockForeground},
        };
        for (const auto &pair : lightSyntax) {
            requireContrast(pair.first, pair.second, light.editorBackground, 4.5);
        }
        const QList<QPair<QString, QColor>> darkSyntax = {
            {QStringLiteral("暗色 标题"), dark.heading},
            {QStringLiteral("暗色 粗体"), dark.bold},
            {QStringLiteral("暗色 斜体"), dark.italic},
            {QStringLiteral("暗色 行内代码"), dark.inlineCodeForeground},
            {QStringLiteral("暗色 链接"), dark.link},
            {QStringLiteral("暗色 引用"), dark.blockQuote},
            {QStringLiteral("暗色 列表标记"), dark.listMarker},
            {QStringLiteral("暗色 代码块文字"), dark.codeBlockForeground},
        };
        for (const auto &pair : darkSyntax) {
            requireContrast(pair.first, pair.second, dark.editorBackground, 4.5);
        }

        check(ThemeManager::editorPalette(Theme::Dark).editorBackground == dark.editorBackground,
              QStringLiteral("editorPalette(): 暗色 -> ThemePalette::dark()"));
        check(ThemeManager::editorPalette(Theme::Light).editorBackground == light.editorBackground,
              QStringLiteral("editorPalette(): 亮色 -> ThemePalette::light()"));
    }

    // ============================ D. 一键切换（真的应用 + 发信号 + 落盘）============================
    {
        std::printf("---- D. 一键切换 ----\n");

        ThemeManager &manager = ThemeManager::instance();

        int changes = 0;
        Theme lastSeen = Theme::Light;
        QObject::connect(&manager, &ThemeManager::themeChanged, [&](Theme theme) {
            ++changes;
            lastSeen = theme;
        });

        // 启动：读配置（测试里是空配置）→ 亮色
        manager.applySavedTheme();
        check(manager.theme() == Theme::Light, QStringLiteral("启动: 没有配置时用亮色"));
        check(changes == 1 && lastSeen == Theme::Light,
              QStringLiteral("启动: 应用了一次并发了一次信号（界面靠它同步）"),
              QStringLiteral("changes=%1").arg(changes));
        check(qApp->styleSheet().contains(QStringLiteral("QPlainTextEdit")),
              QStringLiteral("启动: QSS 真的设到 QApplication 上了"));
        check(qApp->styleSheet() == ThemeManager::styleSheetFor(Theme::Light),
              QStringLiteral("启动: 设上去的就是亮色那份"));

        // 切到暗色
        manager.setTheme(Theme::Dark);
        check(manager.theme() == Theme::Dark, QStringLiteral("切换: 当前主题变成暗色"));
        check(changes == 2, QStringLiteral("切换: 发了一次 themeChanged"), QStringLiteral("changes=%1").arg(changes));
        check(qApp->styleSheet() == ThemeManager::styleSheetFor(Theme::Dark),
              QStringLiteral("切换: QApplication 的样式表换成暗色那份"));
        check(QApplication::palette().color(QPalette::Window).lightness() < 100,
              QStringLiteral("切换: 应用的调色板也变暗了（QSS 管不到的地方靠它）"),
              QApplication::palette().color(QPalette::Window).name());

        // ★ 重复设置同一个主题：不发信号、不重新应用（这条就是"无闪烁"的保证之一）
        const int before = changes;
        manager.setTheme(Theme::Dark);
        check(changes == before, QStringLiteral("重复设置同一主题: 不发信号"),
              QStringLiteral("changes=%1").arg(changes));

        // 落盘：换一个"新进程"（重新打开配置）也能读回暗色
        ConfigManager::setFilePath(base + QStringLiteral("/config.ini"));
        check(ThemeManager::themeFromId(ConfigManager::value(ThemeManager::storageKey()).toString()) == Theme::Dark,
              QStringLiteral("落盘: 配置里存的是暗色（重启后还是它）"),
              ConfigManager::value(ThemeManager::storageKey()).toString());

        // 切回亮色：调色板要还原成系统原生那份（而不是我们拼的假亮色）
        manager.setTheme(Theme::Light);
        check(manager.theme() == Theme::Light, QStringLiteral("切回: 当前主题是亮色"));
        check(qApp->styleSheet() == ThemeManager::styleSheetFor(Theme::Light),
              QStringLiteral("切回: 样式表换回亮色那份"));
        check(changes == before + 1, QStringLiteral("切回: 又发了一次信号"));
        check(QApplication::palette().color(QPalette::Window).lightness() > 200,
              QStringLiteral("切回: 调色板变回亮色"),
              QApplication::palette().color(QPalette::Window).name());
    }

    // ============================ E. 编辑器与预览各自响应 ============================
    {
        std::printf("---- E. 编辑器与预览 ----\n");

        // ---- 编辑器：语法色真的换掉 ----
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("# 标题\n\n正文 **粗体**\n"));

        editor.setThemePalette(ThemePalette::light());
        editor.highlighter()->rehighlight();  // 读格式之前先确保高亮跑过一遍
        const QColor lightHeading = colorOfFirstFormat(editor);
        check(lightHeading.isValid() && lightHeading == ThemePalette::light().heading,
              QStringLiteral("编辑器: 亮色下标题用亮色的配色"),
              lightHeading.name());

        editor.setThemePalette(ThemePalette::dark());
        editor.highlighter()->rehighlight();
        const QColor darkHeading = colorOfFirstFormat(editor);
        check(darkHeading == ThemePalette::dark().heading,
              QStringLiteral("编辑器: 切暗色后标题颜色真的变了"),
              darkHeading.name());
        check(darkHeading != lightHeading, QStringLiteral("编辑器: 两次的标题颜色不同"));
        check(editor.themePalette().editorBackground == ThemePalette::dark().editorBackground,
              QStringLiteral("编辑器: themePalette() 能读回当前配色"));

        // 同一套配色重复应用：不做无谓的重绘（闪烁就是这么来的）
        editor.setThemePalette(ThemePalette::dark());
        check(editor.themePalette().heading == ThemePalette::dark().heading,
              QStringLiteral("编辑器: 重复应用同一套配色不出问题"));

        // 新建的编辑器默认亮色（主窗口会给它刷成当前主题）
        EditorWidget fresh;
        check(fresh.themePalette().editorBackground == ThemePalette::light().editorBackground,
              QStringLiteral("编辑器: 新控件默认从亮色起步"));

        // ---- 预览：只改页面属性，不重载页面 ----
        PreviewRenderer renderer;
        check(renderer.themeId() == QStringLiteral("light"), QStringLiteral("预览: 默认亮色"));

        renderer.applyTheme(QStringLiteral("dark"));
        check(renderer.themeId() == QStringLiteral("dark"),
              QStringLiteral("预览: applyTheme(dark) 记住了主题"));
        check(!renderer.isPageReady(),
              QStringLiteral("预览: 没附着页面时也不会崩，而且没变成「就绪」（不假装能推内容）"));

        renderer.applyTheme(QStringLiteral("紫"));
        check(renderer.themeId() == QStringLiteral("light"),
              QStringLiteral("预览: 只认 dark/light，别的值一律当亮色"));

        renderer.applyTheme(QStringLiteral("DARK"));
        check(renderer.themeId() == QStringLiteral("dark"), QStringLiteral("预览: 大小写不敏感"));

        // 关键点：切换主题**不重载页面**（不调 loadTemplate）——
        // 这里能观察到的证据是"内容仍然待推送"，也就是页面状态没被重置。
        renderer.setDebounceInterval(5000);
        renderer.updateContent(QStringLiteral("# 内容"));
        check(renderer.hasPendingUpdate(),
              QStringLiteral("预览: 切主题不影响内容推送（页面没被重载）"));
        renderer.applyTheme(QStringLiteral("light"));
        check(renderer.hasPendingUpdate(),
              QStringLiteral("预览: 再切一次也不影响（没有白屏闪一下）"));
        check(renderer.themeId() == QStringLiteral("light"), QStringLiteral("预览: 切回亮色"));

        // ---- 预览模板：颜色必须是变量，而且带 applyTheme() ----
        QFile file(QStringLiteral(":/html/preview_template.html"));
        const bool opened = file.open(QIODevice::ReadOnly);
        const QString html = opened ? QString::fromUtf8(file.readAll()) : QString();
        check(opened, QStringLiteral("模板: 能读到预览模板"));
        check(html.contains(QStringLiteral("--bg:")) && html.contains(QStringLiteral("--fg:")),
              QStringLiteral("模板: 定义了主题变量"));
        check(html.contains(QStringLiteral("html[data-theme=\"dark\"]")),
              QStringLiteral("模板: 有暗色那一套变量"));
        check(html.contains(QStringLiteral("function applyTheme(")),
              QStringLiteral("模板: 有 applyTheme()（C++ 就是调它来切主题的）"));
        check(html.contains(QStringLiteral("data-theme")),
              QStringLiteral("模板: 用 data-theme 属性切换（不是靠重新加载页面）"));
        check(!html.contains(QStringLiteral("background: #ffffff")),
              QStringLiteral("模板: 页面底色不再是写死的白色（否则暗色下会白底黑字）"));
        check(html.contains(QStringLiteral(".hljs-keyword")) && html.contains(QStringLiteral("var(--tok-keyword)")),
              QStringLiteral("模板: 代码高亮的颜色也走变量（亮暗一起切）"));
        const int varUses = html.count(QStringLiteral("var(--"));
        check(varUses >= 20, QStringLiteral("模板: 变量被大量使用（说明颜色确实都收进来了）"),
              QStringLiteral("%1 处").arg(varUses));
    }

    QDir(base).removeRecursively();
    ConfigManager::setFilePath(QString());

    std::printf("\n%s（失败 %d 项）\n", g_fail == 0 ? "全部通过" : "有失败项", g_fail);
    return g_fail == 0 ? 0 : 1;
}
