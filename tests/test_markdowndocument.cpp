// MarkdownDocument 的契约测试：把 markdowndocument.h 里写的约定验一遍。
//
// 跑法：ctest -C Debug --output-on-failure   或直接运行 bin/Debug/test_markdowndocument.exe
//
// 测试文件都写在系统临时目录里，不碰你的真实笔记。

#include "fileutils.h"
#include "markdowndocument.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>

#include <cstdio>

using markdown_editor::core::document::MarkdownDocument;
// FileUtils 在全局命名空间，直接用即可

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

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    const QString work = QDir(QDir::tempPath()).filePath(QStringLiteral("md-editor-document-test"));
    QDir(work).removeRecursively();
    QDir().mkpath(work);

    const QString path = work + QStringLiteral("/note.md");
    const QString md = QString::fromUtf8("# 标题\n\n正文 **加粗**\n");

    // ---------------- 新文档的初始状态 ----------------
    MarkdownDocument doc;
    check(!doc.isModified(), "新文档: isModified = false");
    check(doc.getFilePath().isEmpty(), "新文档: 没有磁盘路径");
    check(doc.getMarkdownText().isEmpty(), "新文档: 内容为空");
    check(doc.getRenderedHtml().trimmed().isEmpty(), "新文档: 渲染结果为空");
    check(!doc.save(), "新文档 save(): 没有路径 -> false（UI 该去弹另存为）");

    // ---------------- 改内容 -> 脏标记 + 渲染 ----------------
    doc.setMarkdownText(md);
    check(doc.isModified(), "setMarkdownText: 置脏");

    const QString html1 = doc.getRenderedHtml();
    check(html1.contains(QStringLiteral("<h1>标题</h1>")), "getRenderedHtml: 出 h1", html1.trimmed());
    check(html1.contains(QStringLiteral("<strong>加粗</strong>")), "getRenderedHtml: 出 strong");
    check(doc.getRenderedHtml() == html1, "缓存: 内容没变时返回同一份结果");

    // ---------------- 另存为 ----------------
    check(doc.saveToFile(path), "saveToFile: 成功");
    check(!doc.isModified(), "saveToFile 之后: 不再脏");
    check(doc.getFilePath() == path, "saveToFile 之后: 路径已更新");
    check(QFileInfo::exists(path), "saveToFile: 磁盘上文件已生成");

    QString onDisk;
    check(FileUtils::readFile(path, onDisk) && onDisk == md,
          "磁盘内容与内存完全一致（含换行与中文）");

    // ---------------- 再次修改 -> 缓存必须失效 ----------------
    doc.setMarkdownText(QString::fromUtf8("# 新标题\n"));
    check(doc.isModified(), "再次修改: 又脏了");
    const QString html2 = doc.getRenderedHtml();
    check(html2.contains(QStringLiteral("新标题")), "缓存失效: 重新渲染出新内容");
    check(!html2.contains(QStringLiteral("加粗")), "缓存失效: 旧内容不再出现");

    // ---------------- save() 走当前路径 ----------------
    check(doc.save(), "save(): 保存到当前路径");
    check(FileUtils::readFile(path, onDisk) && onDisk.contains(QStringLiteral("新标题")),
          "save(): 磁盘内容已更新");
    check(!doc.isModified(), "save() 之后: 不再脏");

    // ---------------- 从磁盘加载 ----------------
    MarkdownDocument loaded;
    check(loaded.loadFromFile(path), "loadFromFile: 成功");
    check(loaded.getMarkdownText() == doc.getMarkdownText(), "loadFromFile: 内容一致");
    check(!loaded.isModified(), "loadFromFile: 刚载入不算修改");
    check(loaded.getFilePath() == path, "loadFromFile: 路径已记录");
    check(loaded.getRenderedHtml().contains(QStringLiteral("新标题")), "loadFromFile: 能渲染");

    // ---------------- 加载失败不能破坏已有状态 ----------------
    MarkdownDocument doc2;
    const QString keep = QStringLiteral("原有内容");
    doc2.setMarkdownText(keep);
    check(!doc2.loadFromFile(work + QStringLiteral("/nope.md")), "loadFromFile: 文件不存在 -> false");
    check(doc2.getMarkdownText() == keep, "loadFromFile 失败时: 原内容没被破坏");

    // ---------------- 中文 + emoji + 任务列表往返 ----------------
    MarkdownDocument doc3;
    const QString cn = QString::fromUtf8("# 中文 😀\n\n- [x] 任务\n");
    doc3.setMarkdownText(cn);
    check(doc3.saveToFile(work + QStringLiteral("/cn.md")), "中文文档: 保存成功");

    MarkdownDocument doc4;
    check(doc4.loadFromFile(work + QStringLiteral("/cn.md")) && doc4.getMarkdownText() == cn,
          "中文文档: 读回来与原文逐字符一致");
    const QString cnHtml = doc4.getRenderedHtml();
    check(cnHtml.contains(QStringLiteral("😀")) && cnHtml.contains(QStringLiteral("type=\"checkbox\"")),
          "中文文档: 渲染保留 emoji 和任务列表", cnHtml.trimmed());

    // ---------------- 时间接口 ----------------
    check(doc4.getModifyTime().isValid(), "getModifyTime: 文件存在 -> 有效时间");
    MarkdownDocument noFile;
    check(!noFile.getModifyTime().isValid(), "getModifyTime: 没有文件 -> 无效时间（用 isValid 判断）");
    check(!noFile.getCreateTime().isValid(), "getCreateTime: 没有文件 -> 无效时间");

    QDir(work).removeRecursively();

    std::printf("\nFAIL count = %d\n", g_fail);
    return g_fail == 0 ? 0 : 1;
}
