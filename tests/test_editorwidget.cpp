// EditorWidget（5.1 编辑器核心组件）的契约测试。
//
// 需要 **QApplication**（不是 QCoreApplication）：QWidget 依赖它。
//
// 测试方式：**发真实的按键事件**（QApplication::sendEvent + QKeyEvent），
// 然后断言文档内容 —— 测的是"用户敲了这些键之后文本会变成什么"，
// 而不是"内部调了哪个函数"。这样自动缩进、括号闭合这些行为才能被真正钉住。
//
// 跑法：ctest -C Debug --output-on-failure

#include "editorwidget.h"
#include "markdownhighlighter.h"  // 要调用 highlighter()->rehighlight()，需要完整类型

#include <QApplication>
#include <QAction>
#include <QImage>
#include <QKeyEvent>
#include <QList>
#include <QMenu>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextEdit>
#include <QTextLayout>

#include <cstdio>

// EditorWidget 是全局命名空间的类（理由见 editorwidget.h），所以这里不需要 using。

namespace {

int g_fail = 0;

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

// 发一个按键事件给编辑器：text 是"这个键输入的字符"（Enter 传 "\r"，Tab 传 "\t"……）
void pressKey(EditorWidget &editor,
              Qt::Key key,
              const QString &text = QString(),
              Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent event(QEvent::KeyPress, key, mods, text);
    QApplication::sendEvent(&editor, &event);
}

// 把文本设进去，并把光标放到末尾（模拟"刚写完这一行"）
void setTextAndGoToEnd(EditorWidget &editor, const QString &text)
{
    editor.setPlainText(text);
    QTextCursor cursor = editor.textCursor();
    cursor.movePosition(QTextCursor::End);
    editor.setTextCursor(cursor);
}

void moveCursorTo(EditorWidget &editor, int position)
{
    QTextCursor cursor = editor.textCursor();
    cursor.setPosition(position);
    editor.setTextCursor(cursor);
}

// 离屏渲染一个控件，用来验证"行号真的画出来了"
QImage renderWidget(QWidget &widget, const QSize &size)
{
    widget.resize(size);
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    widget.render(&painter);
    return image;
}

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ============================ A. 纯函数：闭合字符表 ============================
    {
        check(EditorWidget::closingFor(QLatin1Char('(')) == QStringLiteral(")"),
              QStringLiteral("closingFor: ( -> )"));
        check(EditorWidget::closingFor(QLatin1Char('[')) == QStringLiteral("]"),
              QStringLiteral("closingFor: [ -> ]"));
        check(EditorWidget::closingFor(QLatin1Char('{')) == QStringLiteral("}"),
              QStringLiteral("closingFor: { -> }"));
        check(EditorWidget::closingFor(QLatin1Char('"')) == QStringLiteral("\""),
              QStringLiteral("closingFor: 双引号 -> 双引号"));
        check(EditorWidget::closingFor(QLatin1Char('\'')) == QStringLiteral("'"),
              QStringLiteral("closingFor: 单引号 -> 单引号"));
        check(EditorWidget::closingFor(QLatin1Char('`')) == QStringLiteral("`"),
              QStringLiteral("closingFor: 反引号 -> 反引号"));
        check(EditorWidget::closingFor(QLatin1Char('x')).isEmpty(),
              QStringLiteral("closingFor: 普通字符 -> 空（不补全）"));

        check(EditorWidget::isEmptyPair(QLatin1Char('('), QLatin1Char(')')),
              QStringLiteral("isEmptyPair: () 是空对"));
        check(EditorWidget::isEmptyPair(QLatin1Char('"'), QLatin1Char('"')),
              QStringLiteral("isEmptyPair: 两个引号也是空对"));
        check(!EditorWidget::isEmptyPair(QLatin1Char('('), QLatin1Char(']')),
              QStringLiteral("isEmptyPair: 括号不配对 -> 不是"));
        check(!EditorWidget::isEmptyPair(QLatin1Char('a'), QLatin1Char(')')),
              QStringLiteral("isEmptyPair: 左边不是成对字符 -> 不是"));
    }

    // ============================ B. 纯函数：回车后的续行前缀 ============================
    {
        check(EditorWidget::continuationPrefixFor(QStringLiteral("- 项目")) == QStringLiteral("- "),
              QStringLiteral("续行: - 项目 -> - "));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("* 项目")) == QStringLiteral("* "),
              QStringLiteral("续行: * 项目 -> * "));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("1. 项目")) == QStringLiteral("2. "),
              QStringLiteral("续行: 1. -> 2. （序号 +1）"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("9) 项目")) == QStringLiteral("10) "),
              QStringLiteral("续行: 9) -> 10) （两位数也对）"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("- [x] 已完成")) == QStringLiteral("- [ ] "),
              QStringLiteral("续行: 任务列表 -> 新的一项未勾选"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("> 引用")) == QStringLiteral("> "),
              QStringLiteral("续行: > 引用 -> > "));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("> > 嵌套引用")) == QStringLiteral("> > "),
              QStringLiteral("续行: 多层引用照搬层数"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("    缩进段落")) == QStringLiteral("    "),
              QStringLiteral("续行: 普通段落只保持缩进"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("  - 嵌套列表")) == QStringLiteral("  - "),
              QStringLiteral("续行: 缩进的列表项保持缩进"));
        check(EditorWidget::continuationPrefixFor(QStringLiteral("普通段落")).isEmpty(),
              QStringLiteral("续行: 顶格段落 -> 空前缀"));

        check(EditorWidget::isBareListMarker(QStringLiteral("- "))
                  && EditorWidget::isBareListMarker(QStringLiteral("1. "))
                  && EditorWidget::isBareListMarker(QStringLiteral("> "))
                  && EditorWidget::isBareListMarker(QStringLiteral("- [ ] "))
                  && EditorWidget::isBareListMarker(QStringLiteral("  - [x] ")),
              QStringLiteral("裸标记: 只有标记没内容 -> true"));
        check(!EditorWidget::isBareListMarker(QStringLiteral("- 有内容"))
                  && !EditorWidget::isBareListMarker(QString())
                  && !EditorWidget::isBareListMarker(QStringLiteral("正文")),
              QStringLiteral("裸标记: 有内容 / 空行 / 正文 -> false"));
    }

    // ============================ C. 括号自动闭合（真按键）============================
    {
        EditorWidget editor;

        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_ParenLeft, QStringLiteral("("));
        check(editor.toPlainText() == QStringLiteral("()"), QStringLiteral("自动闭合: 输入 ( -> ()"),
              editor.toPlainText());
        check(editor.textCursor().position() == 1, QStringLiteral("自动闭合: 光标停在括号中间"),
              QString::number(editor.textCursor().position()));

        pressKey(editor, Qt::Key_ParenRight, QStringLiteral(")"));
        check(editor.toPlainText() == QStringLiteral("()"), QStringLiteral("自动闭合: 输入 ) 不会变成 ))"));
        check(editor.textCursor().position() == 2, QStringLiteral("自动闭合: 光标跳过那个 )"));

        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_BracketLeft, QStringLiteral("["));
        check(editor.toPlainText() == QStringLiteral("[]"), QStringLiteral("自动闭合: [ -> []"));
        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_BraceLeft, QStringLiteral("{"));
        check(editor.toPlainText() == QStringLiteral("{}"), QStringLiteral("自动闭合: { -> {}"));

        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_QuoteDbl, QStringLiteral("\""));
        check(editor.toPlainText() == QStringLiteral("\"\""), QStringLiteral("自动闭合: 行尾输入引号会补全"));

        // 引号补全要克制：光标夹在单词中间时不该自动补
        setTextAndGoToEnd(editor, QStringLiteral("dont"));
        moveCursorTo(editor, 3);  // 光标在 n 和 t 之间
        pressKey(editor, Qt::Key_Apostrophe, QStringLiteral("'"));
        check(editor.toPlainText() == QStringLiteral("don't"),
              QStringLiteral("自动闭合: 单词中间输入引号不补（写 don't 不会被插成 don''t）"),
              editor.toPlainText());

        // 有选中内容 -> 用括号包起来，并且原来那段仍然选中
        setTextAndGoToEnd(editor, QStringLiteral("重点文字"));
        editor.selectAll();
        pressKey(editor, Qt::Key_ParenLeft, QStringLiteral("("));
        check(editor.toPlainText() == QStringLiteral("(重点文字)"),
              QStringLiteral("自动闭合: 选中内容被括号包住"), editor.toPlainText());
        check(editor.textCursor().hasSelection()
                  && editor.textCursor().selectedText() == QStringLiteral("重点文字"),
              QStringLiteral("自动闭合: 包完之后原来那段仍然选中"));

        // 退格：空对一次删掉两个
        setTextAndGoToEnd(editor, QStringLiteral("()"));
        moveCursorTo(editor, 1);
        pressKey(editor, Qt::Key_Backspace);
        check(editor.toPlainText().isEmpty(), QStringLiteral("退格: 空对一次删掉两个"), editor.toPlainText());

        // 退格：不是空对就按默认行为，只删一个
        setTextAndGoToEnd(editor, QStringLiteral("(a)"));
        moveCursorTo(editor, 2);
        pressKey(editor, Qt::Key_Backspace);
        check(editor.toPlainText() == QStringLiteral("()"),
              QStringLiteral("退格: 不是空对时只删一个字符"), editor.toPlainText());

        // 不在成对字符上时，一切照旧（不该被我们的逻辑吞掉）
        setTextAndGoToEnd(editor, QStringLiteral("abc"));
        pressKey(editor, Qt::Key_X, QStringLiteral("x"));
        check(editor.toPlainText() == QStringLiteral("abcx"),
              QStringLiteral("普通字符: 原样插入（没有被编辑增强吞掉）"), editor.toPlainText());
    }

    // ============================ D. 自动缩进与列表续行（真按键）============================
    {
        EditorWidget editor;

        setTextAndGoToEnd(editor, QStringLiteral("    缩进过的行"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("    缩进过的行\n    "),
              QStringLiteral("自动缩进: 回车后新行保持上一行的缩进"), editor.toPlainText());

        setTextAndGoToEnd(editor, QStringLiteral("- 第一项"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("- 第一项\n- "),
              QStringLiteral("列表续行: - 第一项 -> 新行 - "), editor.toPlainText());

        setTextAndGoToEnd(editor, QStringLiteral("1. 第一步"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("1. 第一步\n2. "),
              QStringLiteral("列表续行: 序号自动 +1"), editor.toPlainText());

        setTextAndGoToEnd(editor, QStringLiteral("- [ ] 待办"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("- [ ] 待办\n- [ ] "),
              QStringLiteral("列表续行: 任务列表新项默认未勾选"), editor.toPlainText());

        setTextAndGoToEnd(editor, QStringLiteral("> 引用内容"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("> 引用内容\n> "),
              QStringLiteral("列表续行: 引用自动续上 > "), editor.toPlainText());

        // 空列表项上再回车 = 退出列表（标记被清掉）
        setTextAndGoToEnd(editor, QStringLiteral("- 有内容\n- "));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"));
        check(editor.toPlainText() == QStringLiteral("- 有内容\n\n"),
              QStringLiteral("列表退出: 空列表项上回车会把标记清掉"), editor.toPlainText());

        // Shift+回车 = 只换行，不做续行（想插入普通换行时的出口）
        setTextAndGoToEnd(editor, QStringLiteral("- 第一项"));
        pressKey(editor, Qt::Key_Return, QStringLiteral("\r"), Qt::ShiftModifier);
        check(editor.toPlainText() == QStringLiteral("- 第一项\n"),
              QStringLiteral("Shift+回车: 只换行，不续列表"), editor.toPlainText());

        // Tab / Shift+Tab：插入的是空格，不是制表符
        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_Tab, QStringLiteral("\t"));
        check(editor.toPlainText() == QStringLiteral("    "),
              QStringLiteral("缩进: Tab 插入 4 个空格（不是制表符）"), editor.toPlainText());
        check(editor.indentWidth() == 4, QStringLiteral("缩进: 默认宽度 = 4"));

        setTextAndGoToEnd(editor, QStringLiteral("    四个空格"));
        moveCursorTo(editor, 6);
        pressKey(editor, Qt::Key_Backtab, QString(), Qt::ShiftModifier);
        check(editor.toPlainText() == QStringLiteral("四个空格"),
              QStringLiteral("缩进: Shift+Tab 去掉行首 4 个空格"), editor.toPlainText());

        // 选中多行 + Tab：整块缩进
        setTextAndGoToEnd(editor, QStringLiteral("第一行\n第二行"));
        editor.selectAll();
        pressKey(editor, Qt::Key_Tab, QStringLiteral("\t"));
        check(editor.toPlainText() == QStringLiteral("    第一行\n    第二行"),
              QStringLiteral("缩进: 选中多行时整块缩进"), editor.toPlainText());

        // 缩进宽度可配置
        editor.setIndentWidth(2);
        check(editor.indentWidth() == 2, QStringLiteral("缩进: setIndentWidth 生效"));
        setTextAndGoToEnd(editor, QString());
        pressKey(editor, Qt::Key_Tab, QStringLiteral("\t"));
        check(editor.toPlainText() == QStringLiteral("  "), QStringLiteral("缩进: 宽度改成 2 之后 Tab 插 2 个空格"));
        editor.setIndentWidth(0);  // 非法值要被夹到 1
        check(editor.indentWidth() == 1, QStringLiteral("缩进: 非法宽度被夹到 1"));
    }

    // ============================ E. 行号栏 ============================
    {
        EditorWidget editor;

        editor.setPlainText(QStringLiteral("1\n2\n3\n4\n5\n6\n7\n8\n9"));
        const int widthOneDigit = editor.lineNumberAreaWidth();

        editor.setPlainText(QStringLiteral("1\n2\n3\n4\n5\n6\n7\n8\n9\n10"));
        const int widthTwoDigits = editor.lineNumberAreaWidth();

        check(widthOneDigit > 0, QStringLiteral("行号栏: 宽度大于 0"), QString::number(widthOneDigit));
        check(widthTwoDigits > widthOneDigit,
              QStringLiteral("行号栏: 行数从 9 涨到 10 时位数变多、宽度变宽"),
              QStringLiteral("%1 -> %2").arg(widthOneDigit).arg(widthTwoDigits));

        // 视口左边要真的让出这块位置（否则行号会盖住文字）。
        // 注意 viewportMargins() 是 protected 的，外部调不到 —— 所以从视口自身的
        // 位置来验证（父容器左侧确实留了地方），这也更接近"用户看得见的结果"。
        editor.setPlainText(QStringLiteral("第一行\n第二行\n第三行"));
        check(editor.viewport()->pos().x() >= editor.lineNumberAreaWidth(),
              QStringLiteral("行号栏: 视口左边确实让出了空间"),
              QStringLiteral("viewport.x=%1, gutter=%2")
                  .arg(editor.viewport()->pos().x())
                  .arg(editor.lineNumberAreaWidth()));

        // 离屏渲染：行号栏区域里除了底色还应该有别的东西（也就是数字）
        const QImage shot = renderWidget(editor, QSize(400, 200));
        const int strip = editor.lineNumberAreaWidth();
        QList<QRgb> colors;
        int inkPixels = 0;
        QRgb background = 0;
        for (int y = 2; y < shot.height() - 2; ++y) {
            for (int x = 1; x < strip - 1; ++x) {
                const QRgb pixel = shot.pixel(x, y);
                if (colors.isEmpty()) {
                    background = pixel;
                }
                if (!colors.contains(pixel)) {
                    colors.append(pixel);
                }
                if (pixel != background) {
                    ++inkPixels;
                }
            }
        }
        check(inkPixels > 0,
              QStringLiteral("行号栏: 渲染出来确实画了东西（数字），不是一整块纯色"),
              QStringLiteral("非底色像素 %1 个，颜色种类 %2").arg(inkPixels).arg(colors.size()));
    }

    // ============================ F. 当前行高亮 ============================
    {
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("第一行\n第二行\n第三行"));

        moveCursorTo(editor, 0);
        const QList<QTextEdit::ExtraSelection> first = editor.extraSelections();
        check(first.size() >= 1, QStringLiteral("当前行高亮: 有一条 ExtraSelection"),
              QString::number(first.size()));
        if (!first.isEmpty()) {
            check(first.first().format.background().style() != Qt::NoBrush,
                  QStringLiteral("当前行高亮: 那条选区带背景色"));
            check(first.first().cursor.blockNumber() == 0,
                  QStringLiteral("当前行高亮: 跟着光标走（第 1 块）"));
        }

        // 光标换行之后，高亮应该跟过去，而且仍然只有一条（不会越积越多）
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::Down);
        editor.setTextCursor(cursor);
        const QList<QTextEdit::ExtraSelection> second = editor.extraSelections();
        check(second.size() == 1, QStringLiteral("当前行高亮: 换行后仍然只有一条"), QString::number(second.size()));
        check(!second.isEmpty() && second.first().cursor.blockNumber() == 1,
              QStringLiteral("当前行高亮: 换行后跟到第 2 块"));
    }

    // ============================ G. cursorMoved 信号（状态栏用）============================
    {
        EditorWidget editor;
        int lastLine = -1;
        int lastColumn = -1;
        int count = 0;
        QObject::connect(&editor, &EditorWidget::cursorMoved, [&](int line, int column) {
            lastLine = line;
            lastColumn = column;
            ++count;
        });

        editor.setPlainText(QStringLiteral("abc\ndef"));
        QTextCursor cursor = editor.textCursor();
        cursor.movePosition(QTextCursor::End);
        editor.setTextCursor(cursor);

        check(count > 0, QStringLiteral("cursorMoved: 光标移动时会发信号"), QString::number(count));
        check(lastLine == 2 && lastColumn == 4,
              QStringLiteral("cursorMoved: 行列都从 1 起算（文档末尾 = 第 2 行第 4 列）"),
              QStringLiteral("line=%1 column=%2").arg(lastLine).arg(lastColumn));

        moveCursorTo(editor, 0);
        check(lastLine == 1 && lastColumn == 1, QStringLiteral("cursorMoved: 回到开头 = 第 1 行第 1 列"),
              QStringLiteral("line=%1 column=%2").arg(lastLine).arg(lastColumn));
    }

    // ============================ H. goToLine（5.5：预览点击和搜索结果都走它）============================
    {
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("第一行\n第二行\n第三行\n第四行"));
        editor.resize(400, 300);

        int lastLine = -1;
        int lastColumn = -1;
        QObject::connect(&editor, &EditorWidget::cursorMoved, [&](int line, int column) {
            lastLine = line;
            lastColumn = column;
        });

        editor.goToLine(3);
        check(editor.textCursor().blockNumber() == 2, QStringLiteral("goToLine: 3 行 -> 第 3 个块（0 起算的 2）"),
              QStringLiteral("block=%1").arg(editor.textCursor().blockNumber()));
        check(editor.textCursor().positionInBlock() == 0, QStringLiteral("goToLine: 默认落在行首"));
        check(lastLine == 3 && lastColumn == 1, QStringLiteral("goToLine: 会让状态栏显示第 3 行第 1 列"),
              QStringLiteral("line=%1 column=%2").arg(lastLine).arg(lastColumn));

        editor.goToLine(2, 3);
        check(editor.textCursor().blockNumber() == 1 && editor.textCursor().positionInBlock() == 2,
              QStringLiteral("goToLine: 指定列也能定位（1 起算）"),
              QStringLiteral("block=%1 pos=%2").arg(editor.textCursor().blockNumber()).arg(editor.textCursor().positionInBlock()));

        // 越界：夹到合法范围，不崩也不跳空
        editor.goToLine(999);
        check(editor.textCursor().blockNumber() == 3, QStringLiteral("goToLine: 行号超过总行数 -> 夹到最后一行"),
              QStringLiteral("block=%1").arg(editor.textCursor().blockNumber()));
        editor.goToLine(0);
        check(editor.textCursor().blockNumber() == 0, QStringLiteral("goToLine: 行号 0 -> 夹到第一行"));
        editor.goToLine(-5);
        check(editor.textCursor().blockNumber() == 0, QStringLiteral("goToLine: 负行号 -> 还是第一行（不崩）"));
        editor.goToLine(1, 999);
        check(editor.textCursor().positionInBlock() == 3,
              QStringLiteral("goToLine: 列超过这一行的长度 -> 夹到行尾（不会跑到下一行去）"),
              QStringLiteral("pos=%1 len=%2").arg(editor.textCursor().positionInBlock()).arg(editor.textCursor().block().length()));

        // 空文档：什么也不做，但也不能崩
        EditorWidget emptyEditor;
        emptyEditor.goToLine(5);
        check(emptyEditor.textCursor().blockNumber() == 0, QStringLiteral("goToLine: 空文档里跳转 -> 还是第 1 行（不崩）"));
    }

    // ============================ I. 插入代码块（5.7：选语言 → 围栏）============================
    {
        EditorWidget editor;

        // 空文档：插一个 python 围栏，光标落在中间那行（等着用户写代码）
        editor.clear();
        editor.insertCodeBlock(QStringLiteral("python"));
        check(editor.toPlainText() == QStringLiteral("```python\n\n```\n"),
              QStringLiteral("insertCodeBlock: 插入 ```lang 围栏，中间留一行空行"),
              editor.toPlainText().replace(QLatin1Char('\n'), QStringLiteral("\\n")));
        check(editor.textCursor().blockNumber() == 1 && editor.textCursor().positionInBlock() == 0,
              QStringLiteral("insertCodeBlock: 光标停在代码区第一行"),
              QStringLiteral("block=%1 col=%2").arg(editor.textCursor().blockNumber()).arg(editor.textCursor().positionInBlock()));

        // 没有语言名：就是一个普通围栏（高亮器会按"没语言"处理）
        editor.clear();
        editor.insertCodeBlock(QString());
        check(editor.toPlainText() == QStringLiteral("```\n\n```\n"),
              QStringLiteral("insertCodeBlock: 语言为空时不写语言名"));

        // 光标在行中间：必须补一个换行，否则围栏不成立
        editor.setPlainText(QStringLiteral("abc"));
        QTextCursor midCursor = editor.textCursor();
        midCursor.movePosition(QTextCursor::End);
        editor.setTextCursor(midCursor);
        editor.insertCodeBlock(QStringLiteral("cpp"));
        check(editor.toPlainText() == QStringLiteral("abc\n```cpp\n\n```\n"),
              QStringLiteral("insertCodeBlock: 光标在行中间时先换行（围栏独占一行）"),
              editor.toPlainText().replace(QLatin1Char('\n'), QStringLiteral("\\n")));

        // 有选中内容：选区直接变成代码块里的代码
        editor.setPlainText(QStringLiteral("x = 1"));
        QTextCursor selectAll = editor.textCursor();
        selectAll.select(QTextCursor::Document);
        editor.setTextCursor(selectAll);
        editor.insertCodeBlock(QStringLiteral("python"));
        check(editor.toPlainText() == QStringLiteral("```python\nx = 1\n```\n"),
              QStringLiteral("insertCodeBlock: 选中的内容被包进代码块（不多空行、不丢内容）"),
              editor.toPlainText().replace(QLatin1Char('\n'), QStringLiteral("\\n")));

        // 不认识的语言也照插不误：高亮器会忽略它，但文档结构仍然是对的
        editor.clear();
        editor.insertCodeBlock(QStringLiteral("klingon"));
        check(editor.toPlainText().contains(QStringLiteral("```klingon")),
              QStringLiteral("insertCodeBlock: 不认识的语言照样插（高亮器那边会原样显示）"));

        // 连插两个：第二个插在光标处 —— 插完光标停在上一块的代码行（为了立刻能打字），
        // 所以"接着再插一个"要先自己把光标移到文档末尾。这里按这个正常用法验证。
        editor.clear();
        editor.insertCodeBlock(QStringLiteral("python"));
        QTextCursor endCursor = editor.textCursor();
        endCursor.movePosition(QTextCursor::End);
        editor.setTextCursor(endCursor);
        editor.insertCodeBlock(QStringLiteral("go"));
        check(editor.toPlainText() == QStringLiteral("```python\n\n```\n```go\n\n```\n"),
              QStringLiteral("insertCodeBlock: 连插两个（光标在末尾时）互不干扰"),
              editor.toPlainText().replace(QLatin1Char('\n'), QStringLiteral("\\n")));
    }

    // ============================ J. 查找与替换（编辑菜单要的）============================
    {
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("Alpha beta\nbeta gamma\nBETA delta\n"));

        // ---- findNext：从光标往后找、找完回到开头 ----
        editor.moveCursor(QTextCursor::Start);
        check(editor.findNext(QStringLiteral("beta")), QStringLiteral("查找: 找到了（默认不区分大小写）"));
        check(editor.textCursor().selectedText() == QStringLiteral("beta"),
              QStringLiteral("查找: 命中的那一段被选中"), editor.textCursor().selectedText());
        const int firstStart = editor.textCursor().selectionStart();

        check(editor.findNext(QStringLiteral("beta")), QStringLiteral("查找: 再找下一个"));
        check(editor.textCursor().selectionStart() > firstStart,
              QStringLiteral("查找: 第二次命中在第一次之后"));

        check(editor.findNext(QStringLiteral("beta")), QStringLiteral("查找: 第三处（大小写不同的 BETA 也算）"));
        check(editor.textCursor().selectedText() == QStringLiteral("BETA"),
              QStringLiteral("查找: 命中的是 BETA"), editor.textCursor().selectedText());

        check(editor.findNext(QStringLiteral("beta")), QStringLiteral("查找: 到底之后再找会回到开头（wrap）"));
        check(editor.textCursor().selectionStart() == firstStart,
              QStringLiteral("查找: 环绕之后命中的正是第一处"));

        // 关掉环绕：到底就找不到
        editor.moveCursor(QTextCursor::End);
        check(!editor.findNext(QStringLiteral("beta"), false, false),
              QStringLiteral("查找: wrap=false 时走到末尾就返回 false"));

        check(!editor.findNext(QStringLiteral("根本没有这个词")), QStringLiteral("查找: 找不到 -> false"));
        check(!editor.findNext(QString()), QStringLiteral("查找: 空关键词 -> false（不会死循环）"));

        // 区分大小写
        editor.moveCursor(QTextCursor::Start);
        check(editor.findNext(QStringLiteral("BETA"), true), QStringLiteral("查找: 区分大小写时能找到 BETA"));
        check(editor.textCursor().selectedText() == QStringLiteral("BETA"),
              QStringLiteral("查找: 区分大小写命中的是 BETA"));

        // ---- findPrevious ----
        editor.moveCursor(QTextCursor::End);
        check(editor.findPrevious(QStringLiteral("beta")), QStringLiteral("向上查找: 找到了"));
        check(editor.textCursor().selectedText() == QStringLiteral("BETA"),
              QStringLiteral("向上查找: 从末尾往上第一处是 BETA"), editor.textCursor().selectedText());

        // ---- 替换当前 ----
        editor.setPlainText(QStringLiteral("one two three"));
        editor.moveCursor(QTextCursor::Start);
        check(!editor.replaceCurrent(QStringLiteral("two"), QStringLiteral("2")),
              QStringLiteral("替换: 没选中任何东西时不替换"));
        editor.findNext(QStringLiteral("two"));
        check(editor.replaceCurrent(QStringLiteral("two"), QStringLiteral("2")),
              QStringLiteral("替换: 选中内容正好是要找的词 -> 替换成功"));
        check(editor.toPlainText() == QStringLiteral("one 2 three"),
              QStringLiteral("替换: 文档内容正确"), editor.toPlainText());
        check(!editor.replaceCurrent(QStringLiteral("完全没有"), QStringLiteral("x")),
              QStringLiteral("替换: 选中的不是要找的词 -> 不误替换"));

        // ---- 统计与全部替换 ----
        EditorWidget all;
        all.setPlainText(QStringLiteral("aa bb aa cc AA\n"));
        check(all.countOccurrences(QStringLiteral("aa")) == 3,
              QStringLiteral("统计: 不区分大小写时 aa 出现 3 次"),
              QStringLiteral("%1 次").arg(all.countOccurrences(QStringLiteral("aa"))));
        check(all.countOccurrences(QStringLiteral("aa"), true) == 2,
              QStringLiteral("统计: 区分大小写时只有 2 次"));

        const int replaced = all.replaceAll(QStringLiteral("aa"), QStringLiteral("x"));
        check(replaced == 3, QStringLiteral("全部替换: 替换了 3 处"), QStringLiteral("%1 处").arg(replaced));
        check(all.toPlainText() == QStringLiteral("x bb x cc x\n"),
              QStringLiteral("全部替换: 内容正确"), all.toPlainText());
        check(all.document()->isUndoAvailable(), QStringLiteral("全部替换: 可撤销"));
        all.undo();
        check(all.toPlainText() == QStringLiteral("aa bb aa cc AA\n"),
              QStringLiteral("全部替换: 一次撤销就全部退回（整批是一个 edit block）"), all.toPlainText());

        // ★ 替换文本里又含有搜索词：不能原地打转（"aa" -> "aaa"）
        EditorWidget tricky;
        tricky.setPlainText(QStringLiteral("aa\n"));
        check(tricky.replaceAll(QStringLiteral("aa"), QStringLiteral("aaa")) == 1,
              QStringLiteral("全部替换: 替换文本里含搜索词时不会死循环"));
        check(tricky.toPlainText() == QStringLiteral("aaa\n"),
              QStringLiteral("全部替换: 结果正确"), tricky.toPlainText());

        check(all.replaceAll(QString(), QStringLiteral("x")) == 0,
              QStringLiteral("全部替换: 空关键词 -> 0（不会死循环）"));
        check(all.countOccurrences(QString()) == 0, QStringLiteral("统计: 空关键词 -> 0"));
    }

    // ============================ K. 右键菜单（6.2）============================
    {
        EditorWidget editor;
        editor.setPlainText(QStringLiteral("一些内容"));

        QMenu *menu = editor.createContextMenu();

        // 注意：Qt 标准右键菜单里的动作文本**内嵌了快捷键文字和制表符**
        //（实际形如 "&Copy\tCtrl+C"），所以这里必须用子串查找，不能精确相等。
        const auto findAction = [](QMenu *m, const QString &needle) -> QAction * {
            for (QAction *action : m->actions()) {
                if (action->text().contains(needle)) {
                    return action;
                }
            }
            return nullptr;
        };

        QStringList texts;
        for (QAction *action : menu->actions()) {
            texts << (action->isSeparator() ? QStringLiteral("---") : action->text());
        }

        // 基类的标准项（撤销/重做/复制/粘贴…）必须还在 —— 自己拼菜单很容易漏掉它们
        check(findAction(menu, QStringLiteral("Undo")) != nullptr,
              QStringLiteral("右键菜单: 保留了基类的撤销项"), texts.join(QStringLiteral(" / ")));
        check(findAction(menu, QStringLiteral("Copy")) != nullptr
                  && findAction(menu, QStringLiteral("Paste")) != nullptr,
              QStringLiteral("右键菜单: 保留了基类的复制/粘贴项"));
        check(findAction(menu, QStringLiteral("查找/替换…")) != nullptr
                  && findAction(menu, QStringLiteral("插入代码块…")) != nullptr,
              QStringLiteral("右键菜单: 有我们自己的查找/替换与插入代码块"));

        // 点"查找/替换"和"插入代码块"要发信号（对话框归主窗口管，编辑器不自己弹）
        int findSignals = 0;
        int codeSignals = 0;
        QObject::connect(&editor, &EditorWidget::findRequested, [&findSignals] { ++findSignals; });
        QObject::connect(&editor, &EditorWidget::insertCodeBlockRequested, [&codeSignals] { ++codeSignals; });

        findAction(menu, QStringLiteral("查找/替换…"))->trigger();
        findAction(menu, QStringLiteral("插入代码块…"))->trigger();
        check(findSignals == 1, QStringLiteral("右键菜单: 点了查找 -> 发出 findRequested"),
              QStringLiteral("%1 次").arg(findSignals));
        check(codeSignals == 1, QStringLiteral("右键菜单: 点了插入代码块 -> 发出 insertCodeBlockRequested"),
              QStringLiteral("%1 次").arg(codeSignals));
        delete menu;

        // 没选中内容时，标准菜单里的"复制"应该是禁用的（说明我们没把它接管成"永远可用"）
        EditorWidget blank;
        blank.setPlainText(QStringLiteral("abc"));
        QMenu *blankMenu = blank.createContextMenu();
        QAction *copyAction = findAction(blankMenu, QStringLiteral("Copy"));
        check(copyAction != nullptr && !copyAction->isEnabled(),
              QStringLiteral("右键菜单: 没选中内容时复制被禁用（基类的状态还在）"));
        delete blankMenu;
    }

    // ============================ L. 语法高亮器已绑定 ============================
    {
        EditorWidget editor;
        check(editor.highlighter() != nullptr, QStringLiteral("高亮: 控件自己绑定了高亮器"));

        editor.setPlainText(QStringLiteral("# 标题"));
        if (editor.highlighter() != nullptr) {
            editor.highlighter()->rehighlight();
            const QTextBlock block = editor.document()->firstBlock();
            const QTextLayout *layout = block.layout();
            check(layout != nullptr && !layout->formats().isEmpty(),
                  QStringLiteral("高亮: 标题行确实被上了格式（说明高亮器真的在跑）"),
                  QStringLiteral("%1 段格式").arg(layout != nullptr ? layout->formats().size() : 0));
        }
    }

    if (g_fail == 0) {
        std::printf("\n=== EditorWidget 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== EditorWidget 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
