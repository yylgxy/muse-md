#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QPalette>
#include <QString>
#include <QStringList>

#include "themepalette.h"  // 编辑器配色的类型出现在接口里（editorPalette()）

// 主题管理器（5.7）：亮 / 暗两套主题，一键全局切换。
//
// 为什么是单例：主题是**进程级的事实**（整个应用要么亮要么暗），不是某个窗口的属性。
// 任何地方想知道"现在是什么主题"、或者想切换，都通过 instance() ——
// 不需要层层传参，也不需要谁去记着"该通知谁"：切换由这里统一发信号。
// （线程注记：QApplication 的样式表本来就是主线程的事，本类不做线程安全保证。）
//
// 一次 setTheme() 到底改了什么（这就是"全局同步切换"的全部内容）：
//   1. QApplication 的样式表  → 菜单栏、工具栏、标签页、状态栏、文件树、面板……（QSS）
//   2. QApplication 的调色板  → QSS 覆盖不到的地方（原生对话框、禁用态等）
//   3. 配置落盘              → 下次启动还是这个主题（ConfigManager，退出不丢）
//   4. 发 themeChanged 信号   → 编辑器的语法配色/行号栏、预览区的 CSS 变量各自响应
//
// ★ "无闪烁"是怎么保证的（不是运气，是三条设计）：
//   * **不重建任何控件**：只换样式表/调色板，控件树不动（重建才会有白屏一闪）；
//   * **不重载预览页面**：预览的颜色全是 CSS 变量，只改 <html data-theme> 属性
//     （重载会闪白、会丢滚动位置、还会让 WebChannel 重连）；
//   * **同一个主题不重复应用**：setTheme(当前主题) 直接返回，不发信号、不重绘。
//
// 配色分两处存，各有各的理由：
//   * 控件外观（菜单/按钮/滚动条…）在 resources/styles/{light,dark}.qss —— QSS 是描述这个的；
//   * 编辑器里"画出来的东西"（语法高亮、行号栏）在 ThemePalette（C++）—— 那是 QTextCharFormat
//     和 QPainter 的事，QSS 管不到。
//   两边的底色必须一致，test_thememanager 会检查它们没打架。
class ThemeManager : public QObject
{
    Q_OBJECT

public:
    enum class Theme {
        Light,
        Dark,
    };
    Q_ENUM(Theme)

    static ThemeManager &instance();

    Theme theme() const;

    // 一键切换：QSS + 调色板 + 落盘 + 发信号。传当前主题时什么都不做。
    void setTheme(Theme theme);

    // 启动时用：读配置里的主题并应用（第一次应用即使和默认值相同也会真的应用，
    // 否则开机就是系统配色而不是我们的主题）。
    void applySavedTheme();

    // ---- 纯函数（能单独测）----

    // 主题 → 配置里存的字符串（"light"/"dark"）
    static QString themeId(Theme theme);
    // 字符串 → 主题。认不出来（配置被手改坏了、老版本写的别的值）当亮色。
    static Theme themeFromId(const QString &id);

    // 主题 → 样式表内容（从 :/styles/light.qss、:/styles/dark.qss 读）。
    // 资源丢了也不崩：返回空字符串并写一条日志（界面会退化成系统默认外观）。
    static QString styleSheetFor(Theme theme);

    // 主题 → 编辑器配色（语法高亮 + 行号栏用）
    static markdown_editor::core::document::ThemePalette editorPalette(Theme theme);

    // 当前应该给编辑器的配色：有自定义主题时返回它，否则返回当前主题的内置配色。
    // 这是"自定义主题真正生效"的那一下 —— 编辑器/行号栏只认这个函数，不关心来源。
    markdown_editor::core::document::ThemePalette currentPalette() const;

    // 配置里存主题用的键名
    static QString storageKey();

    // ---- 主题导入导出（C7）----

    // 导出一份主题 JSON。path 是目标文件路径（不含目录会自动建）。
    // 成功返回 true；失败 false + error（目录建不了、写不进等）。
    bool exportTheme(const QString &path, QString *error = nullptr) const;

    // 导入一份主题 JSON：读文件 → 严格解析（ThemePalette::fromJson）→
    // WCAG 对比度校验（正文/语法色对底色 >= 4.5）→ 存进 <AppData>/themes/<name>.json。
    // name 是导入后显示的名字（传空则用文件名去后缀）。
    // 校验不达标返回 false + error（说清是"哪个颜色对底色对比度不够"），不落盘、不改状态。
    // 成功返回 true，并可通过 importedThemePath() 拿到落盘路径。
    bool importTheme(const QString &path, const QString &name = QString(), QString *error = nullptr);

    // 导入成功后落盘的路径（失败时为空）。
    QString importedThemePath() const;

    // 已经导入的自定义主题：返回它们的"名字 + 落盘路径"（名字 = 文件名去后缀）。
    // 用 QPair（first = 名字，second = 完整路径）而不是自定义结构体 ——
    // 就两个字段，为它单独建个 struct 反而啰嗦。
    QList<QPair<QString, QString>> customThemes() const;

    // 读取某个自定义主题的配色（importTheme / customThemes 的配套）。
    // path 不存在或解析失败返回空 ThemePalette + error。
    static markdown_editor::core::document::ThemePalette
    paletteFromFile(const QString &path, QString *error = nullptr);

    // 应用一个自定义主题：把它的配色交给编辑器/预览（**不改 QSS**，QSS 仍是亮/暗两套里选）。
    // 这是 C7 的取舍：自定义主题只换编辑器配色（用户最想要的那部分），
    // 控件外观（菜单/按钮）仍跟随"亮色/暗色"二选一 —— 因为给每个自定义主题都配一套
    // 完整 QSS 会让"导入主题"变成"导入一整个皮肤包"，复杂度翻倍而收益很小。
    // baseTheme 决定控件外观用哪套 QSS。
    void applyCustomTheme(const markdown_editor::core::document::ThemePalette &palette,
                          Theme baseTheme = Theme::Dark);

    // 清除自定义主题，回到纯亮/暗。调用后 editorPalette() 恢复成内置两套。
    void clearCustomTheme();

    // 是否正在使用某个自定义主题（区别于内置亮/暗）。
    bool hasCustomTheme() const;

signals:
    // 主题变了。编辑器、预览区都连这个信号做各自那部分（它们不需要认识彼此）。
    void themeChanged(ThemeManager::Theme theme);

private:
    explicit ThemeManager(QObject *parent = nullptr);
    ~ThemeManager() override;

    // 单例不该被复制/移动
    ThemeManager(const ThemeManager &) = delete;
    ThemeManager &operator=(const ThemeManager &) = delete;
    ThemeManager(ThemeManager &&) = delete;
    ThemeManager &operator=(ThemeManager &&) = delete;

    // 把样式表和调色板真正应用到 QApplication
    void applyToApplication(Theme theme);

    // 自定义主题的存储目录：<AppData>/Dev/MarkdownEditor/themes/
    QString themesDirectory() const;

    Theme m_theme = Theme::Light;
    bool m_applied = false;         // 是否已经应用过（决定"同主题是否要真的应用一遍"）
    QPalette m_lightPalette;        // 系统原生调色板：切回亮色时要还原成它
    QString m_importedThemePath;    // 最近一次 importTheme 成功后的落盘路径
    bool m_hasCustomTheme = false;  // 是否在用一个自定义主题
    markdown_editor::core::document::ThemePalette m_customPalette;  // 自定义主题的配色（hasCustomTheme 时有效）
};

#endif // THEMEMANAGER_H
