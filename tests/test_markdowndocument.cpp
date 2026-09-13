// MarkdownDocument 的契约测试。
//
// 4.2.1 之后这个类只剩"纯内存状态"（内容 + 路径 + 脏标志）：
// 读盘/写盘/编码在 core/storage 的 FileManager、渲染在 PreviewRenderer，
// 所以这个测试**完全不碰磁盘**，也不需要临时目录。
//
// 剩下要守住的核心语义只有一条：**脏标志什么时候变**。
// 它决定了标题栏那个 * 会不会出现、关窗口时要不要提示保存 ——
// 误置脏（多弹一次保存提示）比漏置脏（丢内容）好，但也别乱来，
// 所以"内容没变就不置脏"和"保存后不误判"这两条都得测。
//
// 跑法：ctest -C Debug --output-on-failure

#include "markdowndocument.h"

#include <QString>

#include <cstdio>

using markdown_editor::core::document::MarkdownDocument;

namespace {

int g_fail = 0;

void check(bool ok, const QString &what, const QString &detail = QString())
{
    std::printf("%-56s %s", what.toUtf8().constData(), ok ? "PASS" : "FAIL");
    if (!detail.isEmpty()) {
        std::printf("  [%s]", detail.toUtf8().constData());
    }
    std::printf("\n");
    if (!ok) {
        ++g_fail;
    }
}

}  // namespace

int main()
{
    // ---------------- 新文档的初始状态 ----------------
    MarkdownDocument doc;
    check(!doc.isModified(), "新文档: isModified = false");
    check(doc.getFilePath().isEmpty(), "新文档: 没有磁盘路径");
    check(doc.getMarkdownText().isEmpty(), "新文档: 内容为空");

    // ---------------- 内容没变 = 不是修改 ----------------
    doc.setMarkdownText(QString());
    check(!doc.isModified(), "setMarkdownText(空->空): 内容没变，不置脏");

    const QString md = QString::fromUtf8("# 标题 😀\n\n- [x] 任务\n");
    doc.setMarkdownText(md);
    check(doc.isModified(), "setMarkdownText: 内容变了才置脏");
    check(doc.getMarkdownText() == md, "内容逐字符一致（中文 / emoji / 换行）");

    doc.setMarkdownText(md);
    check(doc.isModified(), "重复设置同一内容: 不会顺手清脏（清脏是 setModified 的职责）");

    // ---------------- 保存之后不能再被误判成「改过」 ----------------
    doc.setModified(false);
    check(!doc.isModified(), "setModified(false): 清脏");

    doc.setMarkdownText(md);
    check(!doc.isModified(), "保存后再设同一内容: 仍然不算修改（关键语义）");

    doc.setMarkdownText(md + QStringLiteral("多了一行\n"));
    check(doc.isModified(), "真的改了内容: 又脏了");

    // ---------------- 路径 ----------------
    doc.setFilePath(QStringLiteral("D:/我的文档/笔记/a.md"));
    check(doc.getFilePath() == QStringLiteral("D:/我的文档/笔记/a.md"),
          "setFilePath / getFilePath: 中文路径原样保存");

    doc.setFilePath(QString());
    check(doc.getFilePath().isEmpty(), "setFilePath(空): 回到「没有路径」的新文档状态");

    // ---------------- 两个实例互不影响 ----------------
    MarkdownDocument a;
    MarkdownDocument b;
    a.setMarkdownText(QStringLiteral("A"));
    a.setModified(true);
    check(b.getMarkdownText().isEmpty() && !b.isModified() && b.getFilePath().isEmpty(),
          "两个实例互不影响（没有共享状态）");

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
