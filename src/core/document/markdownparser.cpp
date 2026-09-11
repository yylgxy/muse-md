#include "markdownparser.h"

#include "logger.h"

#include <QByteArray>

// md4c 是 C 库，头文件里自己带了 extern "C"，这里直接包含即可。
// md4c 的头文件目录由 third_party/CMakeLists.txt 里的
// target_include_directories(md4c-html PUBLIC ...) 导出。
#include <md4c-html.h>

namespace markdown_editor::core::document {
namespace {

// md4c 的输出回调：C 函数指针，不能用带捕获的 lambda。
// 所以"往哪儿写"通过最后一个 userdata 参数传进来 —— 这里是一个 QByteArray。
// 注意：md4c 的接口是 (指针, 长度) 成对给的，不保证结尾有 '\0'，必须按长度 append。
void appendHtmlChunk(const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    auto *output = static_cast<QByteArray *>(userdata);
    output->append(text, static_cast<qsizetype>(size));
}

}  // namespace

QString MarkdownParser::parseToHtml(const QString &markdown)
{
    // md4c 只吃 UTF-8 字节。这里特意落到具名变量：
    //   1) 接口本身就是"指针 + 长度"，不是以 NUL 结尾的字符串；
    //   2) 避免把临时 QByteArray 的指针存起来（那是悬垂指针的经典写法）。
    const QByteArray input = markdown.toUtf8();

    QByteArray html;
    html.reserve(input.size() * 2);  // 经验值：渲染出的 HTML 一般比 Markdown 长

    const int result = md_html(input.constData(),
                               static_cast<MD_SIZE>(input.size()),
                               appendHtmlChunk,
                               &html,
                               MD_DIALECT_GITHUB,           // CommonMark + GFM: 表格/任务列表/删除线/裸链接
                               MD_HTML_FLAG_SKIP_UTF8_BOM); // 万一输入带了 BOM，也不让它渲染成 U+FEFF

    if (result != 0) {
        // md_html 只在 md_parse 失败时返回非 0（实际只有"输入不是合法 UTF-8"这种情况）
        LOG_ERROR("Markdown 解析失败, md_html 返回 %1", result);
        return QString();
    }

    return QString::fromUtf8(html);
}

}  // namespace markdown_editor::core::document
