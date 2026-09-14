#ifndef EDITORWIDGET_H
#define EDITORWIDGET_H

#include <QColor>
#include <QPlainTextEdit>
#include <QString>

class QKeyEvent;
class QPaintEvent;
class QResizeEvent;
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
    explicit EditorWidget(QWidget *parent = nullptr);

    // 已绑定的高亮器（本控件创建并持有，界面不用管它的生命周期）
    MarkdownHighlighter *highlighter() const;

    // 行号栏需要的宽度（像素），按总行数的位数算出来。
    // 公开出来是为了让布局/测试能问它，也是行号栏子控件 sizeHint 的来源。
    int lineNumberAreaWidth() const;

    int indentWidth() const;
    void setIndentWidth(int spaces);

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

signals:
    // 光标位置变了。行、列**都从 1 起算**（人类数的第几行第几列）。
    void cursorMoved(int line, int column);

protected:
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void updateLineNumberAreaWidth(int newBlockCount);
    void updateLineNumberArea(const QRect &rect, int dy);
    void refreshCurrentLine();

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

    // 颜色从调色板取，亮色/暗色主题都不用改代码
    QColor m_gutterBackground;
    QColor m_lineNumberColor;
    QColor m_currentLineNumberColor;
    QColor m_currentLineColor;
};

#endif // EDITORWIDGET_H
