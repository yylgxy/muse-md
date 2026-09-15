#ifndef THEMEMANAGER_H
#define THEMEMANAGER_H

#include <QObject>
#include <QPalette>
#include <QString>

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

    // 配置里存主题用的键名
    static QString storageKey();

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

    Theme m_theme = Theme::Light;
    bool m_applied = false;         // 是否已经应用过（决定"同主题是否要真的应用一遍"）
    QPalette m_lightPalette;        // 系统原生调色板：切回亮色时要还原成它
};

#endif // THEMEMANAGER_H
