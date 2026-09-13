#include "fileutils.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

// 本文件的组织方式：**字节级的两个函数是唯一的实现**，文本级的两个只是它的薄包装。
//   readFile      = readFileBytes + 剥 BOM + UTF-8 解码
//   writeFile     = writeFileBytes + UTF-8 编码
// 这样"原子替换、自动建目录、失败不破坏原文件"这些关键保证只有一份代码，
// 不会出现"文本路径修好了、字节路径还带着老 bug"的情况。

bool FileUtils::readFileBytes(const QString &path, QByteArray &bytes, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    const QFileInfo fileInfo(path);

    // 目录不是文件，直接给出更清楚的原因（QFile::open 也会失败，但提示没这么明确）
    if (fileInfo.isDir()) {
        if (error != nullptr) {
            *error = QStringLiteral("是文件夹，不是文件");
        }
        return false;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    const QByteArray data = file.readAll();

    // open 成功不代表一定读完：读的过程中也可能出错（例如设备被拔出、网络盘断开）
    if (file.error() != QFileDevice::NoError) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;  // bytes 此时还没被碰过
    }

    bytes = data;
    return true;
}

bool FileUtils::readFile(const QString &path, QString &content, QString *error)
{
    QByteArray bytes;
    if (!readFileBytes(path, bytes, error)) {
        return false;  // content 没被碰过
    }

    // 去掉 UTF-8 BOM（EF BB BF）：Windows 记事本"另存为 UTF-8"会加上它
    if (bytes.startsWith("\xEF\xBB\xBF")) {
        bytes = bytes.mid(3);
    }

    // 字节 -> UTF-8 文本。注意用 fromUtf8，不要用 QString(bytes)（那是 Latin-1，中文必乱）
    content = QString::fromUtf8(bytes);
    return true;
}

bool FileUtils::writeFileBytes(const QString &path, const QByteArray &bytes, QString *error)
{
    if (error != nullptr) {
        error->clear();
    }

    // 父目录不存在时自动创建。必须在打开文件之前做，否则照样失败。
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty() && !QDir().mkpath(dir)) {
        if (error != nullptr) {
            *error = QStringLiteral("无法创建目录: %1").arg(dir);
        }
        return false;
    }

    // QSaveFile：内容先写到临时文件，commit() 成功才原子地替换原文件。
    // 这样写入失败或中途崩溃时，原文件一个字节都不会被破坏。
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    if (file.write(bytes) != bytes.size()) {
        file.cancelWriting();  // 放弃临时文件，原文件不受影响
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    // commit() 才是"真的写成功了"：它包含校验、flush、替换原文件这些步骤
    if (!file.commit()) {
        if (error != nullptr) {
            *error = file.errorString();
        }
        return false;
    }

    return true;
}

bool FileUtils::writeFile(const QString &path, const QString &content, QString *error)
{
    // 不带 QIODevice::Text：换行符原样写入，不做 \n -> \r\n 转换，
    // 否则"写出去的内容"和 content 就不一致了（读回来会多出 \r）。
    return writeFileBytes(path, content.toUtf8(), error);
}

bool FileUtils::exists(const QString &path)
{
    return QFileInfo::exists(path);
}

QString FileUtils::getSuffix(const QString &path)
{
    // QFileInfo::suffix() 保留原始大小写，这里统一转小写，
    // 让调用方可以直接 if (FileUtils::getSuffix(p) == "md")
    return QFileInfo(path).suffix().toLower();
}
