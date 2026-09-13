#ifndef MARKDOWNDOCUMENT_H
#define MARKDOWNDOCUMENT_H

#include <QString>

namespace markdown_editor::core::document {

// 一个 Markdown 文档的**内存状态**：内容 + 磁盘路径 + 脏标记。
//
// 边界（4.2.1 之后收窄过，这条线很重要）：本类是**纯内存**的，一行磁盘代码都没有。
//   * 读盘 / 写盘 / 编码检测 / 只读判断 / 文件时间戳 → core/storage 的 FileManager
//     （那边处理 UTF-8 与 GBK、BOM、原子替换、权限，这些都不是"文档"该关心的事）
//   * Markdown → HTML                                → PreviewRenderer 调 MarkdownParser
//     （那边有 300ms 防抖，缓存也应该跟着数据流走，而不是在这里再存一份）
//
// 为什么要把这些移出去：同一个职责有两份实现时，两份迟早会不一致 ——
// 之前这里有一套"只认 UTF-8"的读写、FileManager 有一套认编码的，谁该调用哪套全靠记性；
// 这里还缓存过 HTML，而预览走的是另一条渲染路径，缓存等于白算。
// 现在的原则很简单：**一个职责只有一条路径**。
//
// 剩下的三件事都是"不碰外部世界"的纯状态：
//   * setMarkdownText() 只在内容**真的变了**时置脏（"输入又删掉"不该算已修改）
//   * 脏标记只是"提醒 UI 该问用户要不要保存"，弹窗是 ui 层的事，这里不弹
//   * 路径为空 = 还没保存过的新文档
class MarkdownDocument
{
public:
    // 设置原始 Markdown 文本。
    // 内容与当前**完全相同**时直接返回：既不置脏、也不清脏 ——
    // "内容没变"是中性事件，不能顺手改状态（清脏是 setModified(false) 的职责）。
    void setMarkdownText(const QString &mdText);

    // 原始 Markdown 文本
    QString getMarkdownText() const;

    void setFilePath(const QString &path);
    QString getFilePath() const;

    // 脏标记：有未保存的修改（UI 靠它决定要不要提示保存 / 标题栏加 *）
    bool isModified() const;
    void setModified(bool flag);

private:
    QString m_filePath;         // md 文件本地路径（空 = 还没保存过的新文档）
    QString m_mdContent;        // 原始 markdown 文本
    bool m_isModified = false;  // 是否有未保存的修改
};

}  // namespace markdown_editor::core::document

#endif // MARKDOWNDOCUMENT_H
