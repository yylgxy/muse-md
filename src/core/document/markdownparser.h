#ifndef MARKDOWNPARSER_H
#define MARKDOWNPARSER_H

#include <QString>

namespace markdown_editor::core::document {

// Markdown -> HTML 解析器（静态工具类，不需要也不能实例化）。
//
// 实现基于 md4c（third_party/md4c，纯 C 的 CommonMark 解析器）+ md4c-html 渲染器。
// md4c 只作为本模块的**实现细节**存在：CMake 里是 PRIVATE 链接，头文件不暴露 md4c 类型，
// 别的模块也不该直接 #include <md4c-html.h>（那样会绕过这一层封装）。
//
// 支持的语法：
//   * CommonMark 全量基础语法（标题/段落/强调/列表/引用/代码块/链接/图片/分隔线……）
//   * GFM 扩展：表格、任务列表、删除线、裸 URL/邮箱自动链接
//     对应 md4c 的 MD_DIALECT_GITHUB，正是本阶段需要的范围
//
// 输出的是 HTML **片段**（相当于 <body> 里的内容），不含 <html>/<head>/<body> 外壳，
// 也不带任何样式。页面外壳、CSS、滚动位置这些由预览层（ui）自己拼。
//
// 已知取舍（想改的话从这里入手）：
//   * 不做 HTML 净化：CommonMark 规定 Markdown 里的原始 HTML 原样透传，所以
//     `<script>` 会进入输出。当前只渲染本地文件、风险有限；等要渲染来源不可信的
//     内容时，必须加净化或改用 MD_FLAG_NOHTML 方言。
//   * 不做缓存：每次调用都重新解析（md4c 很快，配合 ui 层防抖足够；真不够再加）。
class MarkdownParser
{
public:
    // 把 Markdown 文本渲染成 HTML 片段。
    // 编码：输入交给 md4c 前先转 UTF-8，输出再从 UTF-8 转回 QString（md4c 只认 UTF-8 字节）。
    // 成功：返回 HTML 片段；空输入或纯空白输入返回空字符串（这是正常结果，不是失败）。
    // 失败：返回空字符串，并写一条 LOG_ERROR。
    //       实际上很难触发：md4c 只在输入不是合法 UTF-8 时报错，而 QString::toUtf8()
    //       产生的一定是合法 UTF-8。
    // 注意：输出末尾通常带一个换行（md4c 的块级元素后会补 '\n'），
    //       做精确断言时记得先 trimmed()。
    static QString parseToHtml(const QString &markdown);

private:
    MarkdownParser() = delete;
};

}  // namespace markdown_editor::core::document

#endif // MARKDOWNPARSER_H
