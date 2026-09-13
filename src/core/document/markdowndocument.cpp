#include "markdowndocument.h"

// 本文件只有"纯状态"的读写，所以除了自己的头文件之外什么都不用 include。
// 之前这里 include 过 FileUtils / Logger / MarkdownParser —— 那些职责（读盘、记日志、渲染）
// 现在分别归 core/storage 的 FileManager 和 PreviewRenderer，这里不再需要。

namespace markdown_editor::core::document {

void MarkdownDocument::setMarkdownText(const QString &mdText)
{
    if (m_mdContent == mdText) {
        return;  // 内容没变就不是"修改"（也不清脏：那是 setModified 的事）
    }

    m_mdContent = mdText;
    m_isModified = true;
}

QString MarkdownDocument::getMarkdownText() const
{
    return m_mdContent;
}

void MarkdownDocument::setFilePath(const QString &path)
{
    m_filePath = path;
}

QString MarkdownDocument::getFilePath() const
{
    return m_filePath;
}

bool MarkdownDocument::isModified() const
{
    return m_isModified;
}

void MarkdownDocument::setModified(bool flag)
{
    m_isModified = flag;
}

}  // namespace markdown_editor::core::document
