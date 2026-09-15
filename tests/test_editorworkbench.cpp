// EditorWorkbench（5.3 分屏预览布局）的契约测试。
//
// 需要 QApplication（QWidget 依赖它）。
//
// 这个测试**刻意不创建 QWebEngineView** —— 那要 Chromium 运行时，受限环境（无管道/无沙箱权限）里
// 起不来。工作台的 setup() 接受任意 QWidget 当预览侧，所以：
//   * 分屏、三种显示模式、比例记忆、双向同步桥的接线 —— 全都能真验证；
//   * 渲染管线依然存在，只是没附着页面，它的调用会安全地变成空操作（这正是 PreviewRenderer
//     当初那批防御性检查的用处）。
// 真正"页面里显示出来了"那一步要靠人工跑 GUI。
//
// 跑法：ctest -C Debug --output-on-failure

#include "editorwidget.h"
#include "editorworkbench.h"
#include "previewrenderer.h"  // 要直接调 renderer()->updateContent() 等，需要完整类型
#include "syncbridge.h"       // 要连 bridge() 的信号，需要完整类型
#include "tabmanager.h"

#include <QApplication>
#include <QLabel>
#include <QList>
#include <QScrollBar>
#include <QString>
#include <QTextCursor>
#include <QWidget>

#include <cstdio>

using markdown_editor::core::document::PreviewRenderer;
using markdown_editor::core::document::SyncBridge;

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

}  // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // ============================ A. setup 的参数校验 ============================
    {
        EditorWorkbench workbench;
        QWidget side;

        check(!workbench.setup(nullptr, &side), QStringLiteral("setup: 编辑器侧为空 -> false"));
        check(!workbench.setup(&side, nullptr), QStringLiteral("setup: 预览侧为空 -> false"));
        check(workbench.editorSide() == nullptr && workbench.previewSide() == nullptr,
              QStringLiteral("setup 失败时记不住任何一侧"));

        // 渲染管线和同步桥是工作台自己的东西：没 setup 也该拿得到（这样调用方不用判空）
        check(workbench.renderer() != nullptr, QStringLiteral("renderer(): 没 setup 也不为空"));
        check(workbench.bridge() != nullptr, QStringLiteral("bridge(): 没 setup 也不为空"));
        check(workbench.currentEditor() == nullptr, QStringLiteral("currentEditor(): 初始为空"));
    }

    // ============================ B. 分屏与三种显示模式 ============================
    {
        EditorWorkbench workbench;
        auto *tabs = new TabManager();          // 用真的 TabManager 当编辑器侧
        auto *previewSide = new QWidget();      // 预览侧给个普通控件（不碰 Chromium）

        check(workbench.setup(tabs, previewSide), QStringLiteral("setup: 正常挂上两侧"));
        check(workbench.editorSide() == tabs && workbench.previewSide() == previewSide,
              QStringLiteral("setup: 记住的是传进来的那两个控件"));
        check(workbench.previewView() == nullptr,
              QStringLiteral("setup: 预览侧不是 QWebEngineView 时 previewView() 为空（但模式照样能用）"));

        check(workbench.viewMode() == EditorWorkbench::ViewMode::Split, QStringLiteral("模式: 默认是左右分屏"));
        // 注意用 isHidden() 而不是 isVisible()：这个测试不显示窗口，
        // isVisible() 对"窗口没显示的子控件"永远是 false，会得出假结论。
        check(!tabs->isHidden() && !previewSide->isHidden(), QStringLiteral("模式: 分屏时两侧都没有被隐藏"));

        // 先给它一个真实尺寸，再定比例 —— 没布局过的 QSplitter 会把 sizes 算成 0。
        // 注意：实际尺寸会扣掉分隔条那几像素（{300,500} 会变成 {298,497}），
        // 所以这里只验"比例按 3:5 生效"（左窄右宽），不验精确数值。
        workbench.resize(800, 400);
        workbench.setSizes({300, 500});
        const QList<int> before = workbench.sizes();
        check(before.size() == 2 && before.at(0) > 0 && before.at(1) > before.at(0),
              QStringLiteral("比例: setSizes 生效（左窄右宽）"),
              QStringLiteral("%1/%2").arg(before.value(0)).arg(before.value(1)));

        int modeSignals = 0;
        QObject::connect(&workbench, &EditorWorkbench::viewModeChanged, [&](EditorWorkbench::ViewMode) {
            ++modeSignals;
        });

        workbench.setViewMode(EditorWorkbench::ViewMode::EditorOnly);
        check(workbench.viewMode() == EditorWorkbench::ViewMode::EditorOnly && modeSignals == 1,
              QStringLiteral("模式: 仅编辑 -> 状态与信号都对"));
        check(previewSide->isHidden() && !tabs->isHidden(),
              QStringLiteral("模式: 仅编辑时预览被隐藏、编辑器没被隐藏"));

        workbench.setViewMode(EditorWorkbench::ViewMode::EditorOnly);
        check(modeSignals == 1, QStringLiteral("模式: 重复设置同一个模式不发信号"));

        workbench.setViewMode(EditorWorkbench::ViewMode::PreviewOnly);
        check(tabs->isHidden() && !previewSide->isHidden(),
              QStringLiteral("模式: 仅预览时编辑器被隐藏、预览没被隐藏"));

        workbench.setViewMode(EditorWorkbench::ViewMode::Split);
        check(!tabs->isHidden() && !previewSide->isHidden(), QStringLiteral("模式: 切回分屏两侧都回来"));
        const QList<int> after = workbench.sizes();
        check(after == before, QStringLiteral("模式: ★分屏比例被记住了（不会变成一边倒）"),
              QStringLiteral("%1/%2").arg(after.value(0)).arg(after.value(1)));
        check(modeSignals == 3, QStringLiteral("模式: 三次真实切换一共发了 3 个信号"));

        delete tabs;
        delete previewSide;
    }

    // ============================ C. 双向同步桥的接线 ============================
    {
        EditorWorkbench workbench;
        auto *tabs = new TabManager();
        auto *previewSide = new QWidget();
        workbench.setup(tabs, previewSide);
        workbench.resize(800, 400);

        // 两个编辑器（不用真的开标签，工作台只认 EditorWidget）
        auto *first = new EditorWidget();
        auto *second = new EditorWidget();
        QStringList longText;
        for (int i = 1; i <= 60; ++i) {
            longText << QStringLiteral("第 %1 行").arg(i);
        }
        first->setPlainText(longText.join(QLatin1Char('\n')));
        second->setPlainText(QStringLiteral("另一个文档"));
        first->resize(400, 100);
        second->resize(400, 100);

        // ---- 编辑器滚动 → 桥发信号 ----
        QList<int> scrolledLines;
        QObject::connect(workbench.bridge(), &SyncBridge::editorScrolled, [&](int line) {
            scrolledLines.append(line);
        });

        workbench.setCurrentEditor(first);
        check(workbench.currentEditor() == first, QStringLiteral("同步: setCurrentEditor 记住了当前编辑器"));

        // 强制一个滚动范围（窗口没显示时滚动条未必有范围），再滚动它
        first->verticalScrollBar()->setRange(0, 50);
        first->verticalScrollBar()->setValue(20);
        check(!scrolledLines.isEmpty(), QStringLiteral("同步: 编辑器滚动 → 桥发了 editorScrolled"),
              QStringLiteral("%1 次").arg(scrolledLines.size()));

        if (!scrolledLines.isEmpty()) {
            // 行号要和我们自己按同样规则算出来的一致（blockNumber() + 1）
            const int expected = first->cursorForPosition(QPoint(0, 0)).blockNumber() + 1;
            check(scrolledLines.last() == expected,
                  QStringLiteral("同步: 行号是 1 起算、且算得对"),
                  QStringLiteral("桥=%1 期望=%2").arg(scrolledLines.last()).arg(expected));
            check(scrolledLines.last() >= 1 && scrolledLines.last() <= 60,
                  QStringLiteral("同步: 行号落在文档范围内"), QString::number(scrolledLines.last()));
        }

        // ---- 切到另一个编辑器后，旧的那个不该再带动预览 ----
        workbench.setCurrentEditor(second);
        scrolledLines.clear();
        first->verticalScrollBar()->setValue(40);
        check(scrolledLines.isEmpty(), QStringLiteral("同步: ★切走之后，后台编辑器的滚动不再带动预览"));

        second->verticalScrollBar()->setRange(0, 5);  // 让它的滚动条也有范围
        second->verticalScrollBar()->setValue(1);
        check(!scrolledLines.isEmpty(), QStringLiteral("同步: 当前编辑器滚动照常有反应"));

        // ---- 预览点击 → 编辑器跳行（桥的槽本来就是给 JS 调用的，这里直接调）----
        workbench.setCurrentEditor(first);
        QList<int> clicked;
        QObject::connect(&workbench, &EditorWorkbench::editorLineClicked, [&](int line) { clicked.append(line); });

        workbench.bridge()->reportPreviewClick(3);
        check(clicked == QList<int>{3}, QStringLiteral("同步: 预览点击 -> 发出 editorLineClicked(3)"),
              QStringLiteral("%1").arg(clicked.value(0)));
        check(first->textCursor().blockNumber() == 2,
              QStringLiteral("同步: ★光标跳到了第 3 行（1 起算 -> blockNumber 2）"),
              QStringLiteral("blockNumber=%1").arg(first->textCursor().blockNumber()));

        // 越界行号要被夹住，不能崩
        workbench.bridge()->reportPreviewClick(99999);
        check(first->textCursor().blockNumber() == first->document()->blockCount() - 1,
              QStringLiteral("同步: 越界行号被夹到最后一行（不崩）"));

        workbench.bridge()->reportPreviewClick(0);
        check(first->textCursor().blockNumber() == 0, QStringLiteral("同步: 小于 1 的行号被夹到第一行"));

        delete first;
        delete second;
        delete tabs;
        delete previewSide;
    }

    // ============================ D. showContent：内容交给渲染管线 ============================
    {
        EditorWorkbench workbench;
        auto *tabs = new TabManager();
        auto *previewSide = new QWidget();
        workbench.setup(tabs, previewSide);

        PreviewRenderer *renderer = workbench.renderer();
        check(renderer != nullptr, QStringLiteral("内容: 拿得到渲染管线"));

        renderer->flush();  // 清掉可能残留的待处理内容
        workbench.showContent(QStringLiteral("# 标题"), QStringLiteral("D:/notes"), true);
        check(workbench.previewBaseDir() == QStringLiteral("D:/notes"),
              QStringLiteral("内容: forceReload 时记住新的 baseUrl 目录"), workbench.previewBaseDir());
        check(renderer->hasPendingUpdate(),
              QStringLiteral("内容: 交给渲染管线后会走防抖（waiting 状态）"));

        // 目录不变 -> 立刻推（页面不重载，切标签不会闪白）
        workbench.showContent(QStringLiteral("# 标题（改过）"), QStringLiteral("D:/notes"), false);
        check(!renderer->hasPendingUpdate(), QStringLiteral("内容: 目录没变时立刻推送，不排队等防抖"));

        // 目录变了 -> 重新走防抖 + 重载模板那条路
        workbench.showContent(QStringLiteral("# 另一个文档"), QStringLiteral("D:/other"), false);
        check(workbench.previewBaseDir() == QStringLiteral("D:/other"),
              QStringLiteral("内容: 目录变了会自动识别出来"), workbench.previewBaseDir());
        check(renderer->hasPendingUpdate(), QStringLiteral("内容: 换了目录 -> 重新加载模板并重新排队"));

        delete tabs;
        delete previewSide;
    }

    // ============================ E. 三块的分屏（5.4.1 加了文件树侧边栏之后）============================
    // 这一段是回归测试：中央区从两块变成三块（文件树 + 编辑器 + 预览）之后，
    // "初始比例该由谁给"就变了 —— 工作台只知道两块，所以比例只能由调用方用
    // setSplitSizes() 设。**它必须在 show() 之前设也照样生效**（主窗口就是在构造函数里设的），
    // 而且三块都不能被挤成 0 宽 —— 那正是"预览不见了"这类问题的成因。
    {
        EditorWorkbench workbench;

        // 模仿 uic 生成代码的做法：三块都是 splitter 的子控件，用 addWidget 排进去
        auto *fileTreeSide = new QWidget();
        auto *tabs = new TabManager();
        auto *previewSide = new QWidget();
        workbench.addWidget(fileTreeSide);
        workbench.addWidget(tabs);
        workbench.addWidget(previewSide);

        check(workbench.count() == 3, QStringLiteral("三块: splitter 里有三个部件"),
              QString::number(workbench.count()));
        check(workbench.setup(tabs, previewSide), QStringLiteral("三块: setup 成功（侧边栏不参与，也不该参与）"));

        // 主窗口的顺序：setup() 之后立刻设初始比例，此时窗口还没显示
        workbench.setSplitSizes({220, 490, 490});
        check(workbench.sizes().size() == 3, QStringLiteral("三块: setSplitSizes 在 show() 之前就接受了"),
              QStringLiteral("%1").arg(workbench.sizes().value(0)));

        workbench.resize(1200, 800);
        workbench.show();
        QCoreApplication::processEvents();

        const QList<int> shown = workbench.sizes();
        check(shown.size() == 3, QStringLiteral("三块: 显示之后还是三块"));
        check(shown.value(0) > 0 && shown.value(1) > 0 && shown.value(2) > 0,
              QStringLiteral("三块: **每一块都有正宽度**（谁都不许是 0）"),
              QStringLiteral("%1 / %2 / %3").arg(shown.value(0)).arg(shown.value(1)).arg(shown.value(2)));
        check(shown.value(1) > shown.value(0) && shown.value(2) > shown.value(0),
              QStringLiteral("三块: 编辑器侧和预览侧都比侧边栏宽（比例按 220/490/490 缩放）"));
        check(!previewSide->isHidden(), QStringLiteral("三块: 预览侧是显示状态（不是被藏起来）"));
        check(!tabs->isHidden() && !fileTreeSide->isHidden(),
              QStringLiteral("三块: 编辑器侧和侧边栏也是显示状态"));

        // 数量对不上时必须拒绝：宁可不动，也不能把某一块压成 0
        const QList<int> before = workbench.sizes();
        workbench.setSplitSizes({300, 500});
        check(workbench.sizes() == before, QStringLiteral("三块: 给两个数（块数对不上）时拒绝，不改动"));

        // 切到"仅编辑"再切回分屏：三块的比例都要回来
        workbench.setViewMode(EditorWorkbench::ViewMode::EditorOnly);
        check(previewSide->isHidden(), QStringLiteral("三块: 仅编辑时预览被藏起来"));
        workbench.setViewMode(EditorWorkbench::ViewMode::Split);
        const QList<int> restored = workbench.sizes();
        check(restored.size() == 3 && restored.value(1) > 0 && restored.value(2) > 0,
              QStringLiteral("三块: 切回来之后三块都还在且都有宽度"),
              QStringLiteral("%1 / %2 / %3").arg(restored.value(0)).arg(restored.value(1)).arg(restored.value(2)));
        check(!previewSide->isHidden(), QStringLiteral("三块: 切回分屏后预览重新显示"));

        workbench.hide();
        delete fileTreeSide;
        delete tabs;
        delete previewSide;
    }

    if (g_fail == 0) {
        std::printf("\n=== EditorWorkbench 契约测试：全部通过 ===\n");
    } else {
        std::printf("\n=== EditorWorkbench 契约测试：%d 项失败 ===\n", g_fail);
    }
    return g_fail == 0 ? 0 : 1;
}
