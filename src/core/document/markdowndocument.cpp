#include "markdowndocument.h"

// 按"模块导出的头文件"来包含，不要写 ../infrastructure/xxx.h 这种相对路径：
// infrastructure 在 CMake 里是 PUBLIC 链接 + PUBLIC include 目录，
// 所以 fileutils.h / logger.h 在这里直接可用；写死相对路径会在以后调整目录结构时断掉，
// 也隐藏了"我依赖 infrastructure"这个事实。
#include "fileutils.h"
#include "logger.h"
#include "markdownparser.h"

#include <QFileInfo>

namespace markdown_editor::core::document {

// 注意：FileUtils 目前是**全局命名空间**的类（Logger 在 markdown_editor::infrastructure），
// 所以这里直接写 FileUtils:: 就行；LOG_* 宏内部已经带完整限定名。
// 命名空间不统一的问题在报告里单独提了。

bool MarkdownDocument::loadFromFile(const QString &filePath)
{
    QString content;
    QString error;

    // FileUtils 的契约：失败时把原因放进 error，并且**不会动 content**。
    // 这里必须把 error 带上，否则 UI 只能说"打开失败"，用户不知道为什么
    // （文件不存在？没权限？路径是个目录？）
    if (!FileUtils::readFile(filePath, content, &error)) {
        LOG_ERROR("加载文档失败: %1 (%2)", filePath, error);
        return false;
    }

    m_filePath = filePath;
    m_mdContent = content;
    m_htmlContent.clear();  // 旧文档的 HTML 缓存立刻释放，别留着占内存
    m_isModified = false;
    m_htmlDirty = true;     // 内容换了，缓存作废

    LOG_INFO("已加载文档: %1 (%2 字节)", filePath, content.toUtf8().size());
    return true;
}

bool MarkdownDocument::saveToFile(const QString &filePath)
{
    QString error;

    if (!FileUtils::writeFile(filePath, m_mdContent, &error)) {
        LOG_ERROR("保存文档失败: %1 (%2)", filePath, error);
        return false;   // 失败时不动路径和脏标记：文档还是"没保存过"的状态
    }

    m_filePath = filePath;
    m_isModified = false;

    LOG_INFO("已保存文档: %1", filePath);
    return true;
}

bool MarkdownDocument::save()
{
    if (m_filePath.isEmpty()) {
        LOG_WARN("文档还没有磁盘路径, 应该走「另存为」");
        return false;
    }
    return saveToFile(m_filePath);
}

void MarkdownDocument::setMarkdownText(const QString &mdText)
{
    if (m_mdContent == mdText) {
        return;  // 内容没变就不是"修改"
    }

    m_mdContent = mdText;
    m_isModified = true;
    m_htmlDirty = true;  // md 变了，旧 HTML 不能再用
}

QString MarkdownDocument::getMarkdownText() const
{
    return m_mdContent;
}

QString MarkdownDocument::getRenderedHtml()
{
    // 惰性渲染：只有 md 变过才重新解析，否则直接给缓存
    if (m_htmlDirty) {
        // MarkdownParser 的方法是静态的，不需要（也不允许）创建对象
        m_htmlContent = MarkdownParser::parseToHtml(m_mdContent);
        m_htmlDirty = false;
    }
    return m_htmlContent;
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

QDateTime MarkdownDocument::getCreateTime() const
{
    return QFileInfo(m_filePath).birthTime();
}

QDateTime MarkdownDocument::getModifyTime() const
{
    return QFileInfo(m_filePath).lastModified();
}

}  // namespace markdown_editor::core::document
