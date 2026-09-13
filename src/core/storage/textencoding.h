#ifndef TEXTENCODING_H
#define TEXTENCODING_H

namespace markdown_editor::core::storage {

// 文本编码。
//
// 这个枚举原先定义在 FileManager 里。4.2.3 把它单独挪出来，原因是 **include 方向**：
//   * cachemanager.h 要描述"缓存了哪个文件、内容是什么文本、是什么编码"，
//     但它不该去 include filemanager.h（那样 filemanager.h 再 include cachemanager.h 就循环了）；
//   * 把编码这个概念独立成头文件，两个模块都能用，谁也不依赖谁。
//
// FileManager 里保留了 `using Encoding = Encoding;` 这个别名，
// 所以外面原来的写法（FileManager::Encoding::Utf8）完全不受影响。
//
// 放在 core/storage 是因为"编码"是文件层面的概念：它描述磁盘上那串字节怎么解释，
// 文档模型（core/document）不需要知道这些。
enum class Encoding {
    Utf8,       // UTF-8 无 BOM（默认；新建文档用它）
    Utf8Bom,    // UTF-8 带 BOM（Windows 记事本"另存为 UTF-8"会加）
    Utf16LE,    // 小端 UTF-16（记事本里的"Unicode"）
    Utf16BE,    // 大端 UTF-16
    Local8Bit,  // 本机 ANSI 代码页（中文 Windows = GBK / CP936），留给老的 GBK 文档
};

}  // namespace markdown_editor::core::storage

#endif // TEXTENCODING_H
