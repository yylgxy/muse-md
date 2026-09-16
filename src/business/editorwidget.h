#ifndef EDITORWIDGET_H
#define EDITORWIDGET_H

#include <QColor>
#include <QPlainTextEdit>
#include <QString>

#include "themepalette.h"  // 值成员，需要完整定义（主题配色）

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;
class QContextMenuEvent;
class QMenu;
class MarkdownHighlighter;  // 全局命名空间的类（命名空间不统一的遗留）

// 编辑器核心控件：在 QPlainTextEdit 之上补齐"写 Markdown 需要的东西"。
//
// 注意：本类刻意放在**全局命名空间**（和 MainWindow、MarkdownHighlighter 一致），
// 原因是 mainwindow.ui 里把 editor 提升成了它 ——
// uic 生成的成员声明是 `EditorWidget *editor;`，只会写简单类名，
// 带命名空间限定的类型名它表达不了（`<class>` 里也只能写标识符）。
// 想放进命名空间的话，就得在 ui 层再包一个全局的空壳类，不值当。
//
// 它自己负责四件事（界面层因此不用重复做）：
//   1. **绑定语法高亮器** —— 高亮器挂在文档上，由本控件创建并持有
//   2. **编辑增强** —— 括号/引号自动闭合、自动缩进、列表与引用回车续行
//   3. **行号栏** —— 左侧绘制行号（当前行加粗），并高亮当前行
//   4. **光标位置信号** —— cursorMoved(行, 列)，行列都从 1 起算，状态栏直接可用
//
// 关于信号命名的说明：基类 QPlainTextEdit **本来就有** cursorPositionChanged()，
// 界面想连它随时可以连（本控件没有遮蔽它）。这里另外发一个带行列号的 cursorMoved()，
// 是因为状态栏要显示"行 12，列 3"，自己从 QTextCursor 换算太啰嗦 ——
// 而把 1 起算这件事放在控件里做一次，界面层就永远不用再想 0/1 转换。
//
// 编辑增强的分工（重要）：
//   判断逻辑（"该补哪个闭合符""回车后新行该加什么前缀"）全是 **static 纯函数**，
//   不碰控件状态，所以能脱离界面单独测；keyPressEvent 只负责"读懂按键 + 调用它们 + 落到文档上"。
//   这样最难写错的部分（Markdown 列表续行那一堆边界）就有了自动化测试。
//
// 缩进一律用**空格**（默认 4 个）：Markdown 里制表符在不同渲染器下宽度不一，
// 列表嵌套和代码块很容易被它搞乱，所以本控件不插入制表符。
class EditorWidget : public QPlainTextEdit
{
    Q_OBJECT

public:
    // 本类在**全局命名空间**（理由见上面那段），配色表在 markdown_editor::core::document 里，
    // 用别名接进来（和 MarkdownHighlighter 里的写法一致）。
    using ThemePalette = markdown_editor::core::document::ThemePalette;

    explicit EditorWidget(QWidget *parent = nullptr);

    // 已绑定的高亮器（本控件创建并持有，界面不用管它的生命周期）
    MarkdownHighlighter *highlighter() const;

    // 行号栏需要的宽度（像素），按总行数的位数算出来。
    // 公开出来是为了让布局/测试能问它，也是行号栏子控件 sizeHint 的来源。
    int lineNumberAreaWidth() const;

    // 视口里 y 坐标处的行号（1 起算，夹在 [1, 总行数] 内）。
    // 行号栏的悬停提示要用它（鼠标停在某个行号上时提示"第 N 行"），
    // 公开出来也让这条映射能被单独测 —— 它和绘制行号用的是同一套几何计算。
    int lineNumberAtY(int y) const;

    // 文档的字符数 —— **等于 toPlainText().size()，但是 O(1)**。
    // 为什么专门开这一个：状态栏的"字符 N"会跟着每次按键刷新，
    // 用 toPlainText().size() 等于每敲一个字就把整篇文档深拷贝一遍
    //（几十万字符的文档上，这笔开销是打字"发黏"的主因之一）。
    // QTextDocument::characterCount() 多算了最后一个块的块结束符，所以这里减 1。
    int characterCount() const;

    int indentWidth() const;
    void setIndentWidth(int spaces);

    // ============================ 主题（5.7）============================

    // 换一套配色：行号栏的颜色立刻更新，语法高亮也重新上一遍色。
    // 编辑器控件的**背景/文字色**由 QSS 管（见 resources/styles/*.qss），
    // 这里只管"画出来的东西"（行号、当前行高亮）和"高亮格式的颜色"。
    // 与当前配色相同时直接返回 —— 主题被重复应用不该造成多余重绘（闪烁就是这么来的）。
    void setThemePalette(const ThemePalette &palette);
    ThemePalette themePalette() const;

    // ============================ 跳转 ============================

    // 把光标移到第 line 行第 column 列，**行列都从 1 起算**（和状态栏显示的一致），
    // 并把这一行居中显示、让编辑器拿到焦点。
    // 越界会被夹到合法范围（行夹到 [1, 总行数]，列夹到该行的有效范围），不会崩也不会跳空。
    // "点预览里的某一块"和"点全文搜索的结果"两条路都走这里 —— 跳行只有一份实现。
    void goToLine(int line, int column = 1);

    // 插入一个代码块围栏（```语言 … ```），光标停在代码区第一行。
    // language 就是 Markdown 里写在围栏后面的那个语言名（python/cpp/…），
    // 它同时也是高亮器认的语言名 —— "选语言"和"显示高亮"靠的就是这一个词。
    // 有选中内容时，选区直接变成代码块里的代码（"选中一段 → 插入代码块"最自然）；
    // 光标在行中间时，会先补一个换行，保证围栏独占一行（否则围栏根本不成立）。
    void insertCodeBlock(const QString &language);

    // ============================ 查找与替换 ============================
    // 机制放在控件里（而不是放进查找对话框）有两个理由：
    //   1. 它们全是"对文档的操作"，本质属于编辑器；
    //   2. 替换的边界情况最容易写错（大小写、环绕、替换文本里又含搜索词……），
    //      放在这里就能脱离界面单独测。

    // 从光标之后往下找；找不到且 wrap=true 时从头再来一遍（"到底了回到开头"，符合直觉）。
    // 找到就把那一段选中并滚到可见位置，返回 true。
    bool findNext(const QString &text, bool caseSensitive = false, bool wrap = true);

    // 同 findNext，但往上找（找不到且 wrap=true 时从文档末尾再来）
    bool findPrevious(const QString &text, bool caseSensitive = false, bool wrap = true);

    // 替换"当前选中的那一处"：选中内容正好等于 text 时才替换，返回是否真的换了。
    // （先查找再替换的用法下，这样不会误替换别处。）
    bool replaceCurrent(const QString &text, const QString &replacement, bool caseSensitive = false);

    // 全文替换，返回替换了几处。整件事包在一个 edit block 里，**一次撤销**就能全退回。
    int replaceAll(const QString &text, const QString &replacement, bool caseSensitive = false);

    // 数一数全文有几处（给"共 N 处"这类提示用）
    int countOccurrences(const QString &text, bool caseSensitive = false) const;

    // ============================ 右键菜单 ============================
    // 造一份编辑器右键菜单（调用方负责 delete）：
    // 基类的标准项（撤销/重做/剪切/复制/粘贴/删除/全选）+ 我们自己的"查找/替换"和"插入代码块"。
    // 公开出来是为了让"菜单里有哪些动作、什么时候禁用"也能被测到（和 FileTreeView 一个做法）。
    QMenu *createContextMenu();

    // ============================ 纯函数（能单独测）============================

    // 输入 opener 时该补上的闭合字符；不是成对字符就返回空字符串。
    // 支持 ( [ { " ' ` 六种。
    static QString closingFor(QChar typed);

    // 这两个相邻字符是不是"一个空的字符对"（用于退格时整对删除）。
    static bool isEmptyPair(QChar before, QChar after);

    // 回车后新行应该插入什么前缀（含缩进）。规则：
    //   "- 项目"      → "- "          （无序列表续行）
    //   "1. 项目"     → "2. "         （有序列表自动 +1）
    //   "3) 项目"     → "4) "
    //   "- [x] 任务"  → "- [ ] "      （任务列表续行，新的一项默认未勾选）
    //   "> 引用"      → "> "          （引用续行，多层 "> >" 也照搬）
    //   "    缩进行"  → "    "        （普通段落就只保持缩进）
    // 缩进（前导空格/制表符）原样保留，所以嵌套列表不会串位。
    static QString continuationPrefixFor(const QString &lineBefore);

    // 这一行是不是"只有列表/引用标记、没有内容"。
    // 是的话再按回车表示"我不想继续这个列表了"，调用方应该把标记清掉再换行。
    static bool isBareListMarker(const QString &line);

    // ============================ 大文档快速模式（7.2 性能）============================

    // 超过这个字符数就进入"快速模式"。
    // 为什么是字符数而不是字节：文档在内存里就是 QString，判断成本是 O(1)；
    // 30 万字符大约相当于 1MB 的中文 UTF-8 文件、或 600KB 的英文文件 ——
    // 也就是"规格里说的 1MB 文件"，而正常笔记（几万字）离它还远。
    static constexpr int kFastModeThresholdChars = 300000;

    // 纯规则：这个长度的文档要不要进快速模式（能单独测）。
    // 退出快速模式的门槛比进入低 10%（迟滞）：否则文档正好卡在阈值上时，
    // 每敲一个字都会在高亮/不高亮之间来回抖。
    static bool prefersFastMode(int characterCount, bool currentlyFast = false);

    bool isFastMode() const;

    // 语法高亮的总开关。快速模式下会把它关掉 —— 编辑大文件时唯一真正贵的就是它：
    // QSyntaxHighlighter 要把每一行拿十几条正则过一遍，几十万字符的文档上
    // 每敲一个字都会明显卡顿，而"看得清语法"在那种规模的文档里意义不大。
    void setHighlightingEnabled(bool enabled);
    bool isHighlightingEnabled() const;

signals:
    // 光标位置变了。行、列**都从 1 起算**（人类数的第几行第几列）。
    void cursorMoved(int line, int column);

    // 右键菜单里选了"查找/替换…"：对话框归主窗口管（编辑器不自己弹窗）
    void findRequested();
    // 右键菜单里选了"插入代码块…"：语言列表在高亮器那边，所以也交给主窗口
    void insertCodeBlockRequested();

    // 进入/退出快速模式（主窗口据此在状态栏提示一句，否则用户会以为"高亮坏了"）
    void fastModeChanged(bool fast);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void refreshCurrentLine();
    // 文档长度变了：决定要不要进/出快速模式（用字符数是 O(1)，不碰真正的文本）
    void refreshFastMode();

private:
    // 行号栏。定义在 .cpp 里：C++11 起**嵌套类拥有和成员一样的访问权限**，
    // 所以它能直接调用私有的 paintLineNumbers()，不需要 friend，也不用把内部方法公开出去。
    class LineNumberArea;

    void paintLineNumbers(QPaintEvent *event);  // 由行号栏的 paintEvent 转交过来
    void applyIndentWidth();                    // 把缩进宽度同步到制表位显示宽度
    QString currentLineText() const;

    // 四类编辑增强。返回 true = 这个按键已经被处理掉了，不要再往下传。
    bool handleIndent(QKeyEvent *event);
    bool handleAutoPair(QKeyEvent *event);
    bool handleSmartBackspace(QKeyEvent *event);
    void handleReturn();

    LineNumberArea *m_lineNumberArea = nullptr;
    MarkdownHighlighter *m_highlight = nullptr;

    int m_indentWidth = 4;

    // 大文档快速模式：true = 已经关掉语法高亮（见 kFastModeThresholdChars）
    bool m_fastMode = false;
    // 高亮的"用户意图"：即使处于快速模式，这个值也记着用户的开关状态，
    // 退出快速模式时能恢复成他原来要的样子。
    bool m_highlightingWanted = true;

    // 主题配色（默认亮色；切换由 ThemeManager 通过 setThemePalette() 推进来）
    ThemePalette m_themePalette;

    // 行号栏与当前行用的颜色（由 m_themePalette 派生，改主题时一起更新）
    QColor m_gutterBackground;
    QColor m_lineNumberColor;
    QColor m_currentLineNumberColor;
    QColor m_currentLineColor;
};

#endif // EDITORWIDGET_H
