#ifndef MARKDOWNDOCUMENT_H
#define MARKDOWNDOCUMENT_H

#include <QDateTime>
#include <QString>

namespace markdown_editor::core::document {

// 一个 Markdown 文档的内存模型：内容 + 磁盘路径 + 脏标记 + HTML 缓存。
//
// 三件事的分工（这也是分层的意义）：
//   * 渲染本身交给 MarkdownParser —— 它是**静态工具类、无状态**，所以这里不需要
//     持有它的对象，直接 MarkdownParser::parseToHtml(...) 调用即可（不用 new、不用 delete）。
//     （两个类在同一个命名空间里，所以这里不用写限定名。）
//   * 文件读写交给 FileUtils（统一处理 UTF-8 / BOM / 错误信息），本类只关心文档语义。
//   * 本类不碰 UI：m_isModified 只是"提醒 UI 该问用户要不要保存"，弹窗是 ui 层的事。
//
// HTML 缓存策略：md 内容变时才置脏，真正需要 HTML 时才重新解析（惰性 + 缓存）。
// 注意缓存粒度是**整篇**：UI 做实时预览时要加防抖，否则每敲一个字就全量解析一次。
class MarkdownDocument
{
public:
    // 从磁盘加载（UTF-8，自动剥离 BOM）。
    // 成功：true，并把状态重置为"未修改"。
    // 失败：false，**对象状态完全不变**（原来的内容和路径都还在），原因写进日志。
    bool loadFromFile(const QString &filePath);

    // 另存到指定路径（成功后内部路径也切过去，即"另存为"语义）。
    // 失败：false，内部状态不变（不会把路径改成写不进去的那个）。
    bool saveToFile(const QString &filePath);

    // 保存到当前路径（相当于 Ctrl+S）。
    // 还没有路径的新文档返回 false —— 这时应该由 UI 去弹「另存为」。
    bool save();

    // 设置原始 Markdown 文本。内容与当前完全相同时不置脏
    // （避免"在编辑器里点了一下、输入又删掉"就被判成已修改）。
    void setMarkdownText(const QString &mdText);

    // 原始 Markdown 文本
    QString getMarkdownText() const;

    // 渲染后的 HTML（内部走 MarkdownParser）。
    // 非 const 是有意的：第一次调用会填充缓存，之后 md 没变就直接返回缓存。
    QString getRenderedHtml();

    void setFilePath(const QString &path);
    QString getFilePath() const;

    // 脏标记：有未保存的修改（UI 靠它决定要不要提示保存 / 标题栏加 *）
    bool isModified() const;
    void setModified(bool flag);

    // 文件时间（每次都查一次磁盘）。
    // 注意：路径为空或文件不存在时返回的是**无效**的 QDateTime，用 isValid() 判断再显示。
    QDateTime getCreateTime() const;
    QDateTime getModifyTime() const;

private:
    QString m_filePath;         // md 文件本地路径（空 = 还没保存过的新文档）
    QString m_mdContent;        // 原始 markdown 文本
    QString m_htmlContent;      // 渲染结果的缓存
    bool m_isModified = false;  // 是否有未保存的修改
    bool m_htmlDirty = true;    // md 变过、HTML 缓存已失效
};

}  // namespace markdown_editor::core::document

#endif // MARKDOWNDOCUMENT_H
