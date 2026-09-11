#ifndef FILEUTILS_H
#define FILEUTILS_H

#include <QString>

// 文件读写工具。
// 全部是静态函数，不需要（也不允许）创建对象，直接写 FileUtils::readFile(...)。
//
// 三条全局约定：
//   1. 文本一律按 UTF-8 处理：读取时解码并自动剥离开头的 BOM（EF BB BF），写入时不写 BOM。
//      —— 目的就是避免 Windows 上"同一个文件在记事本里正常、在程序里乱码"这类问题。
//      换行符不做任何转换：\n 原样写入、\r\n 原样读出，保证"读进来再写回去"内容不变
//      （这也是不用 QIODevice::Text 的原因）。
//   2. 路径统一用 QString，正斜杠 / 反斜杠都能用（内部交给 QFileInfo、QFile 等 Qt 类处理）。
//   3. 会失败的函数用 bool 返回，并通过可选的 error 出参带出失败原因（可直接展示给用户）。
//      出参约定：必填的输出用引用（QString &），可选的输出用指针（QString *，传 nullptr 表示不要）。
class FileUtils
{
public:
    // 读取文本文件。
    // 编码：UTF-8，并剥离开头的 BOM。
    // 成功：返回 true，content 为文件全部内容（文件本身为空 → 空字符串，仍然算成功）。
    // 失败：返回 false，content 不会被修改；error 非 nullptr 时写入失败原因。
    //       error 在成功时会被清空，避免残留上一轮的旧消息。
    // 失败情形：文件不存在 / 路径是目录 / 无读取权限 / 读取过程中出错（如设备被拔出）。
    static bool readFile(const QString &path, QString &content, QString *error = nullptr);

    // 覆盖写入文本文件（UTF-8，不写 BOM）。
    // 成功：返回 true；文件内容被整体替换为 content（是覆盖，不是追加）；父目录不存在时自动创建。
    // 失败：返回 false，error 非 nullptr 时写入失败原因（成功时清空 error）；此时原文件内容保持不变
    //       （实现用 QSaveFile：先写临时文件，commit() 成功才替换，避免写一半崩溃/断电把用户文档毁掉）。
    // 失败情形：无写权限 / 文件被其他程序独占 / 磁盘已满 / 路径不合法。
    static bool writeFile(const QString &path, const QString &content, QString *error = nullptr);

    // 判断路径是否存在（文件或目录都算存在）。
    // 注意：只回答"在不在"，不代表可读或可写；想区分文件还是目录，请另外加 isFile()/isDir()。
    static bool exists(const QString &path);

    // 取文件后缀名：不含点、统一小写。
    // 规则：只看文件名里最后一个点之后的部分；没有点则返回空字符串（空字符串不是错误）。
    //   "D:/a/b.md"         → "md"
    //   "D:/a/b.MD"         → "md"        （统一转小写，调用方可以直接 if (suffix == "md")）
    //   "D:/a/notes.tar.gz" → "gz"        （只取最后一段；要 "tar.gz" 请用 QFileInfo::completeSuffix()）
    //   "D:/v1.2/readme"    → ""          （目录名里的点不算后缀）
    //   "noext" / "a." / "" → ""
    //   ".gitignore"        → "gitignore" ← Qt 的规则就是这样（点开头的名字也当成后缀）；
    //                                       如果你认为隐藏文件没有后缀，需要在实现里特判
    //                                       fileName 以点开头的名字。
    static QString getSuffix(const QString &path);

private:
    // 工具类不允许实例化：所有函数都是静态的，造一个对象没有任何意义。
    // = delete 是明确告诉编译器"这个函数不存在"，谁写 FileUtils f; 谁就编译报错。
    FileUtils() = delete;
};

#endif // FILEUTILS_H
