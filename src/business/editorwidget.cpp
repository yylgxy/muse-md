#include "editorwidget.h"

#include "markdownhighlighter.h"

#include <QFontMetrics>
#include <QKeyEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QTextFormat>
#include <QtGlobal>

namespace {

// 6 种成对字符的闭合侧。和 EditorWidget::closingFor() 用同一张表，放在这里是为了
// "是不是闭合字符"这个判断也有一处唯一定义（避免两处表迟早对不上）。
bool isCloser(QChar c)
{
    return c == QLatin1Char(')') || c == QLatin1Char(']') || c == QLatin1Char('}')
           || c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`');
}

bool isQuote(QChar c)
{
    return c == QLatin1Char('"') || c == QLatin1Char('\'') || c == QLatin1Char('`');
}

// 引号右边的这个字符是不是"补全了也不碍事"的那种。
// 写成 don't 或者 a"b 的时候不该自动补引号，所以只有右边是空/空白/标点时才补。
bool quoteFriendlyRightSide(QChar next)
{
    return next.isNull() || next.isSpace() || isCloser(next) || next == QLatin1Char(';')
           || next == QLatin1Char(',') || next == QLatin1Char(':') || next == QLatin1Char('.');
}

}  // namespace

// ============================================================================
// 行号栏
// ============================================================================
// 只做两件事：把自己的绘制转交给编辑器、把自己的宽度告诉布局。
// 真正的绘制在 EditorWidget::paintLineNumbers() 里 —— 那里才能用到
// firstVisibleBlock() / blockBoundingGeometry() / contentOffset() 这些 protected 成员。
class EditorWidget::LineNumberArea : public QWidget
{
public:
    explicit LineNumberArea(EditorWidget *editor) : QWidget(editor), m_editor(editor) {}

    QSize sizeHint() const override
    {
        return QSize(m_editor->lineNumberAreaWidth(), 0);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        m_editor->paintLineNumbers(event);
    }

private:
    EditorWidget *m_editor;
};

// ============================================================================
// 构造与外观
// ============================================================================

EditorWidget::EditorWidget(QWidget *parent) : QPlainTextEdit(parent)
{
    // ---- 1. 绑定语法高亮器 ----
    // QSyntaxHighlighter 是挂在**文档**上的，所以父对象设成 document()：
    // 文档被回收时高亮器跟着走，界面层不需要记住它，也不用手工 delete。
    m_highlight = new MarkdownHighlighter(document());

    // ---- 2. 行号栏 ----
    m_lineNumberArea = new LineNumberArea(this);

    // ---- 3. 主题配色 ----
    // 颜色来自 ThemePalette（亮色起步）：行号栏和高亮器都用同一份，
    // 切主题时 ThemeManager 会推进来新的一份（见 setThemePalette）。
    // 编辑器控件的背景/文字色不在这里设 —— 那是 QSS 的事（resources/styles/*.qss）。
    setThemePalette(ThemePalette::light());

    applyIndentWidth();

    // ---- 4. 信号槽：行号栏跟着文档和视口变化 ----
    // blockCountChanged：行数变了 → 行号位数可能变（99 → 100），左侧留白要重算
    // updateRequest：视口要重绘/滚动 → 行号栏跟着重绘/滚动
    // cursorPositionChanged：基类信号，这里用来刷新当前行高亮 + 发 cursorMoved
    connect(this, &QPlainTextEdit::blockCountChanged, this, &EditorWidget::updateLineNumberAreaWidth);
    connect(this, &QPlainTextEdit::updateRequest, this, &EditorWidget::updateLineNumberArea);
    connect(this, &QPlainTextEdit::cursorPositionChanged, this, &EditorWidget::refreshCurrentLine);

    updateLineNumberAreaWidth(0);
    refreshCurrentLine();
}

MarkdownHighlighter *EditorWidget::highlighter() const
{
    return m_highlight;
}

int EditorWidget::indentWidth() const
{
    return m_indentWidth;
}

void EditorWidget::setIndentWidth(int spaces)
{
    if (spaces < 1) {
        spaces = 1;
    }
    m_indentWidth = spaces;
    applyIndentWidth();
}

void EditorWidget::applyIndentWidth()
{
    // 制表位宽度按"n 个空格的宽度"算：文档里已经存在的制表符也能显示得合理
    setTabStopDistance(m_indentWidth * fontMetrics().horizontalAdvance(QLatin1Char(' ')));
}

// ============================================================================
// 主题
// ============================================================================

void EditorWidget::setThemePalette(const ThemePalette &palette)
{
    if (m_themePalette.editorBackground == palette.editorBackground
        && m_themePalette.gutterBackground == palette.gutterBackground && m_themePalette.heading == palette.heading
        && m_themePalette.editorForeground == palette.editorForeground) {
        return;  // 同一套配色：不做任何事（主题重复应用不该引起重绘）
    }

    m_themePalette = palette;

    m_gutterBackground = palette.gutterBackground;
    m_lineNumberColor = palette.gutterText;
    m_currentLineNumberColor = palette.currentLineNumberText;
    m_currentLineColor = palette.currentLineHighlight;
    m_currentLineColor.setAlpha(40);  // 当前行高亮要"看得见但不抢眼"

    if (m_highlight != nullptr) {
        m_highlight->setPalette(palette);  // 语法高亮重建规则并重新上一遍色
    }

    // 行号栏和当前行高亮都是自己画的：重绘一次即可
    if (m_lineNumberArea != nullptr) {
        m_lineNumberArea->update();
    }
    refreshCurrentLine();
}

// 返回类型写全限定名：C++ 解析返回类型时还没进入 EditorWidget 的作用域（见上面的别名说明）
markdown_editor::core::document::ThemePalette EditorWidget::themePalette() const
{
    return m_themePalette;
}

// ============================================================================
// 跳转与插入代码块
// ============================================================================

void EditorWidget::goToLine(int line, int column)
{
    const int blocks = document()->blockCount();
    if (blocks <= 0) {
        return;  // 理论上不会发生（空文档也有一个块），但没必要为它冒越界的风险
    }

    // 行：1 起算 → 夹到 [1, 总行数]
    const int blockNumber = qBound(0, line - 1, blocks - 1);
    QTextCursor cursor(document()->findBlockByNumber(blockNumber));

    // 列：1 起算 → 夹到这一行的有效范围。
    // block().length() 含结尾的换行符，所以有效列数是 length()-1（避免把光标放到"行尾之后"）。
    const int maxColumn = qMax(0, cursor.block().length() - 1);
    const int offset = qBound(0, column - 1, maxColumn);
    cursor.movePosition(QTextCursor::NextCharacter, QTextCursor::MoveAnchor, offset);

    setTextCursor(cursor);
    centerCursor();  // 让目标行落在屏幕中间，而不是贴着上边缘
    setFocus();
}

void EditorWidget::insertCodeBlock(const QString &language)
{
    QTextCursor cursor = textCursor();

    // QTextCursor 用 U+2029 表示换行（selectedText() 里不会给 '\n'），所以要换回来
    QString selected = cursor.selectedText();
    selected.replace(QChar::ParagraphSeparator, QLatin1Char('\n'));

    const QString name = language.trimmed();
    const QString fence = name.isEmpty() ? QStringLiteral("```") : QStringLiteral("```") + name;

    // 围栏必须独占一行：插入点在这一行中间时先补一个换行，否则这堆反引号根本不成围栏。
    // 判断要用**插入点**（有选区时是选区起点），不能用 cursor.atBlockStart() —— 有选区时
    // 那个问的是光标的活动端，全选一段文字会得出错误的答案（然后凭空多出一个空行）。
    const int insertAt = cursor.hasSelection() ? cursor.selectionStart() : cursor.position();
    QTextCursor probe(cursor);
    probe.setPosition(insertAt);
    const QString prefix = probe.atBlockStart() ? QString() : QStringLiteral("\n");

    QString block = prefix + fence + QLatin1Char('\n');
    const int codeStart = insertAt + block.size();  // 代码区第一行的行首
    if (selected.isEmpty()) {
        block += QLatin1Char('\n');  // 留一行空行给用户写代码
    } else {
        block += selected;
        if (!selected.endsWith(QLatin1Char('\n'))) {
            block += QLatin1Char('\n');
        }
    }
    block += QStringLiteral("```\n");

    cursor.insertText(block);  // 有选区时这一步会把选区替换掉

    // 光标落到代码区第一行（而不是留在闭合围栏之后）
    QTextCursor target = cursor;
    target.setPosition(codeStart);
    setTextCursor(target);
    setFocus();
}

// ============================================================================
// 行号栏：宽度、位置、绘制
// ============================================================================

int EditorWidget::lineNumberAreaWidth() const
{
    // 位数按总行数算：9 行要 1 位，100 行要 3 位
    int digits = 1;
    for (int max = qMax(1, blockCount()); max >= 10; max /= 10) {
        ++digits;
    }
    // 8 = 左右各留 4 像素的呼吸空间
    return 8 + fontMetrics().horizontalAdvance(QLatin1Char('9')) * digits;
}

void EditorWidget::updateLineNumberAreaWidth(int /*newBlockCount*/)
{
    // 用 viewportMargins 在左侧让出位置：滚动条、光标坐标都会自动避开这块区域，
    // 比手工挪 viewport 稳得多。
    setViewportMargins(lineNumberAreaWidth(), 0, 0, 0);
}

void EditorWidget::updateLineNumberArea(const QRect &rect, int dy)
{
    if (dy != 0) {
        m_lineNumberArea->scroll(0, dy);  // 整体滚动：行号跟着挪，不用重画
    } else {
        m_lineNumberArea->update(0, rect.y(), m_lineNumberArea->width(), rect.height());
    }

    if (rect.contains(viewport()->rect())) {
        updateLineNumberAreaWidth(0);  // 行数位数可能刚刚变了
    }
}

void EditorWidget::resizeEvent(QResizeEvent *event)
{
    QPlainTextEdit::resizeEvent(event);

    // 行号栏贴住内容区左上角，高度跟内容区一样
    const QRect cr = contentsRect();
    m_lineNumberArea->setGeometry(QRect(cr.left(), cr.top(), lineNumberAreaWidth(), cr.height()));
}

void EditorWidget::paintLineNumbers(QPaintEvent *event)
{
    QPainter painter(m_lineNumberArea);
    painter.fillRect(event->rect(), m_gutterBackground);

    QTextBlock block = firstVisibleBlock();
    int blockNumber = block.blockNumber();
    int top = qRound(blockBoundingGeometry(block).translated(contentOffset()).top());
    int bottom = top + qRound(blockBoundingRect(block).height());
    const int currentBlock = textCursor().blockNumber();

    // 只画"落在本次重绘区域里"的那些行，长文档滚动时也不会白画一大堆
    while (block.isValid() && top <= event->rect().bottom()) {
        if (block.isVisible() && bottom >= event->rect().top()) {
            const bool isCurrent = (blockNumber == currentBlock);

            // 当前行的行号加粗：只靠颜色对比，在深色主题下往往看不出来
            QFont font = painter.font();
            font.setBold(isCurrent);
            painter.setFont(font);
            painter.setPen(isCurrent ? m_currentLineNumberColor : m_lineNumberColor);

            painter.drawText(0,
                             top,
                             m_lineNumberArea->width() - 4,
                             fontMetrics().height(),
                             Qt::AlignRight | Qt::AlignVCenter,
                             QString::number(blockNumber + 1));
        }

        block = block.next();
        top = bottom;
        bottom = top + qRound(blockBoundingRect(block).height());
        ++blockNumber;
    }
}

// ============================================================================
// 当前行高亮 + 光标位置信号
// ============================================================================

void EditorWidget::refreshCurrentLine()
{
    // 当前行高亮用 ExtraSelection：这是 QPlainTextEdit 自带的机制，
    // 不用去覆写 paintEvent（覆写会把光标、选区、输入法那一堆绘制逻辑全打乱）。
    QList<QTextEdit::ExtraSelection> selections;
    if (!isReadOnly()) {
        QTextEdit::ExtraSelection selection;
        selection.format.setBackground(m_currentLineColor);
        selection.format.setProperty(QTextFormat::FullWidthSelection, true);  // 铺满整行，而不是只到行尾
        selection.cursor = textCursor();
        selection.cursor.clearSelection();
        selections.append(selection);
    }
    setExtraSelections(selections);

    // 行号栏里"当前行加粗"要跟着换，重画一次
    if (m_lineNumberArea != nullptr) {
        m_lineNumberArea->update();
    }

    // 行列都从 1 起算：状态栏直接显示，界面层不用再想 0/1 转换
    const QTextCursor cursor = textCursor();
    emit cursorMoved(cursor.blockNumber() + 1, cursor.positionInBlock() + 1);
}

QString EditorWidget::currentLineText() const
{
    return textCursor().block().text();
}

// ============================================================================
// 纯函数：编辑增强的判断逻辑
// ============================================================================

QString EditorWidget::closingFor(QChar typed)
{
    switch (typed.unicode()) {
    case '(':
        return QStringLiteral(")");
    case '[':
        return QStringLiteral("]");
    case '{':
        return QStringLiteral("}");
    case '"':
        return QStringLiteral("\"");
    case '\'':
        return QStringLiteral("'");
    case '`':
        return QStringLiteral("`");
    default:
        return QString();
    }
}

bool EditorWidget::isEmptyPair(QChar before, QChar after)
{
    if (before.isNull() || after.isNull()) {
        return false;
    }
    const QString closer = closingFor(before);
    return !closer.isEmpty() && after == closer.at(0);
}

bool EditorWidget::isBareListMarker(const QString &line)
{
    // 只有标记、没有内容的情况：
    //   "- " / "* " / "+ " / "1. " / "2) " / "- [ ] " 以及 "> "、">> "
    static const QRegularExpression re(
        QStringLiteral("^(?:\\s*(?:[-*+]|\\d+[.)])\\s+(?:\\[[ xX]\\]\\s*)?|\\s*>+\\s*)$"));
    return re.match(line).hasMatch();
}

QString EditorWidget::continuationPrefixFor(const QString &lineBefore)
{
    // 1) 先把这一行的缩进原样取出来（空格/制表符都保留）
    //    保缩进是"自动缩进"的核心：普通段落换行后要对齐上一行
    int indentEnd = 0;
    while (indentEnd < lineBefore.size()
           && (lineBefore.at(indentEnd) == QLatin1Char(' ') || lineBefore.at(indentEnd) == QLatin1Char('\t'))) {
        ++indentEnd;
    }
    const QString indent = lineBefore.left(indentEnd);
    const QString rest = lineBefore.mid(indentEnd);

    // 2) 引用：把行首连续的 "> " 标记原样取出来，续行照搬层数。
    //    注意两种写法都要认："> > 文字"（Markdown 习惯，标记间有空格）和 ">>>文字"（连写）。
    if (rest.startsWith(QLatin1Char('>'))) {
        QString markers;
        int i = 0;
        while (i < rest.size() && rest.at(i) == QLatin1Char('>')) {
            markers += QLatin1Char('>');
            ++i;
            if (i < rest.size() && rest.at(i) == QLatin1Char(' ')) {
                markers += QLatin1Char(' ');  // 标记后面的那个空格属于标记
                ++i;
            }
        }
        return indent + markers;
    }

    // 3) 任务列表：新的一项默认未勾选
    static const QRegularExpression taskRe(QStringLiteral("^(?:[-*+]|\\d+[.)])\\s+\\[[ xX]\\]\\s+"));
    if (taskRe.match(rest).hasMatch()) {
        return indent + QStringLiteral("- [ ] ");
    }

    // 4) 有序列表：序号自动 +1，并且沿用原来的分隔符（"." 或 ")"）
    static const QRegularExpression orderedRe(QStringLiteral("^(\\d+)([.)])\\s+"));
    const QRegularExpressionMatch ordered = orderedRe.match(rest);
    if (ordered.hasMatch()) {
        const int next = ordered.captured(1).toInt() + 1;
        return indent + QString::number(next) + ordered.captured(2) + QLatin1Char(' ');
    }

    // 5) 无序列表："- " 原样续上（连 "- " 里的空格数量也照搬）
    static const QRegularExpression bulletRe(QStringLiteral("^[-*+]\\s+"));
    const QRegularExpressionMatch bullet = bulletRe.match(rest);
    if (bullet.hasMatch()) {
        return indent + bullet.captured(0);
    }

    // 6) 普通段落：只保持缩进
    return indent;
}

// ============================================================================
// 键盘处理
// ============================================================================

void EditorWidget::keyPressEvent(QKeyEvent *event)
{
    // 顺序有讲究：先处理"只可能是特殊按键"的（Tab / 回车），再处理"靠输入字符判断"的，
    // 这样 Enter、Tab 不会被括号补全的逻辑误伤。
    if (handleIndent(event)) {
        return;
    }

    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (event->modifiers().testFlag(Qt::ControlModifier) || event->modifiers().testFlag(Qt::ShiftModifier)) {
            // Shift+回车 / Ctrl+回车：只换行，不做列表续行 —— 用户想"插入一个普通换行"时的出口
            QPlainTextEdit::keyPressEvent(event);
            return;
        }
        handleReturn();
        return;
    }

    if (handleAutoPair(event)) {
        return;
    }

    if (handleSmartBackspace(event)) {
        return;
    }

    QPlainTextEdit::keyPressEvent(event);
}

void EditorWidget::handleReturn()
{
    QTextCursor cursor = textCursor();
    const QString line = cursor.block().text();

    // 只有列表标记、没有内容时再按回车 → 把标记清掉（等于"退出这个列表"）
    if (isBareListMarker(line)) {
        cursor.beginEditBlock();
        cursor.movePosition(QTextCursor::StartOfBlock);
        cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
        cursor.removeSelectedText();
        cursor.insertText(QStringLiteral("\n"));
        cursor.endEditBlock();
        setTextCursor(cursor);
        return;
    }

    // 其余情况：换行 + 前缀（缩进 / 列表标记 / 引用）
    const QString prefix = continuationPrefixFor(line);
    cursor.beginEditBlock();
    cursor.insertText(QStringLiteral("\n") + prefix);
    cursor.endEditBlock();
    setTextCursor(cursor);
}

bool EditorWidget::handleAutoPair(QKeyEvent *event)
{
    if (isReadOnly() || event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::AltModifier)) {
        return false;
    }

    const QString text = event->text();
    if (text.size() != 1) {
        return false;
    }
    const QChar typed = text.at(0);

    QTextCursor cursor = textCursor();

    // 光标右边紧邻的那个字符（到文档末尾时拿到的是空 QChar，不会越界）
    const QChar next = document()->characterAt(cursor.position());

    // ① 输入的是闭合字符、右边正好也是它 → 只把光标往后跳一格。
    //    这就是"输入 ) 时不会变成 ))"的机制，也是选区包好之后能直接接着写的原因。
    if (isCloser(typed) && next == typed) {
        cursor.movePosition(QTextCursor::NextCharacter);
        setTextCursor(cursor);
        return true;
    }

    const QString closer = closingFor(typed);
    if (closer.isEmpty()) {
        return false;
    }

    // ② 引号只在"右边是空/空白/标点"时才补全：写 don't、a"b 时不至于插出一堆多余引号
    if (isQuote(typed) && !quoteFriendlyRightSide(next)) {
        return false;
    }

    // ③ 有选中内容 → 用这一对把它包起来，并保持原来那段仍被选中
    if (cursor.hasSelection()) {
        const int start = cursor.selectionStart();
        const int end = cursor.selectionEnd();

        cursor.beginEditBlock();
        QTextCursor tail = cursor;
        tail.setPosition(end);
        tail.insertText(closer);  // 先补右边，避免影响左边的位置
        QTextCursor head = cursor;
        head.setPosition(start);
        head.insertText(typed);  // 再补左边
        head.setPosition(start + 1);
        head.setPosition(end + 1, QTextCursor::KeepAnchor);  // 仍然选中原来那段
        head.endEditBlock();
        setTextCursor(head);
        return true;
    }

    // ④ 普通情况：插入一对，光标停在中间
    cursor.beginEditBlock();
    cursor.insertText(typed + closer);
    cursor.movePosition(QTextCursor::PreviousCharacter);
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

bool EditorWidget::handleSmartBackspace(QKeyEvent *event)
{
    if (event->key() != Qt::Key_Backspace || event->modifiers().testFlag(Qt::ControlModifier)) {
        return false;
    }

    QTextCursor cursor = textCursor();
    if (cursor.hasSelection()) {
        return false;  // 有选区就走默认行为（删掉选中的内容）
    }

    const int pos = cursor.position();
    if (pos <= 0) {
        return false;
    }

    const QChar before = document()->characterAt(pos - 1);
    const QChar after = document()->characterAt(pos);
    if (!isEmptyPair(before, after)) {
        return false;
    }

    // 光标正好夹在一个空对中间 → 一次删掉两个（否则得按两次退格，很烦）
    cursor.beginEditBlock();
    cursor.deletePreviousChar();
    cursor.deleteChar();
    cursor.endEditBlock();
    setTextCursor(cursor);
    return true;
}

bool EditorWidget::handleIndent(QKeyEvent *event)
{
    const bool isTabKey = (event->key() == Qt::Key_Tab || event->key() == Qt::Key_Backtab);
    if (!isTabKey || event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::AltModifier)) {
        return false;
    }

    const bool unindent = (event->key() == Qt::Key_Backtab) || event->modifiers().testFlag(Qt::ShiftModifier);

    QTextCursor cursor = textCursor();
    QTextDocument *doc = document();

    // 选中多行时整块处理，没选中就处理当前行
    const int firstBlock = doc->findBlock(cursor.selectionStart()).blockNumber();
    const int lastBlock = doc->findBlock(cursor.selectionEnd()).blockNumber();

    cursor.beginEditBlock();
    for (int n = firstBlock; n <= lastBlock; ++n) {
        QTextCursor lineCursor(doc->findBlockByNumber(n));
        lineCursor.movePosition(QTextCursor::StartOfBlock);

        if (unindent) {
            // 行首最多去掉 indentWidth 个空格；如果一个都没去掉、而首字符是制表符，就去掉它
            int removed = 0;
            while (removed < m_indentWidth
                   && doc->characterAt(lineCursor.position()) == QLatin1Char(' ')) {
                lineCursor.deleteChar();
                ++removed;
            }
            if (removed == 0 && doc->characterAt(lineCursor.position()) == QLatin1Char('\t')) {
                lineCursor.deleteChar();
            }
        } else {
            lineCursor.insertText(QString(m_indentWidth, QLatin1Char(' ')));
        }
    }
    cursor.endEditBlock();

    setTextCursor(cursor);  // 光标/选区会被文档的修改自动调整，这里只是同步回去
    return true;
}

