#ifndef LOGGER_H
#define LOGGER_H

#include <QByteArray>
#include <QChar>
#include <QDebug>
#include <QLatin1String>
#include <QString>
#include <QStringView>

#include <string>
#include <type_traits>
#include <utility>

namespace markdown_editor::infrastructure {

// 统一日志工具
// ---------------------------------------------------------------------------
// * 输出到控制台（Qt Creator 的「应用程序输出」面板可见）和文件，两边格式完全一致
// * 每行自动带：时间 + 级别 + 文件名:行号
// * 通过 qInstallMessageHandler 接管 Qt 自己的 qDebug/qWarning/qCritical/qFatal，
//   所以 Qt 内部和第三方库的输出也走同一套格式（Release 下也保留文件/行号，
//   见 src/infrastructure/CMakeLists.txt 里的 QT_MESSAGELOGCONTEXT）
// * 线程安全；每写一行就 flush，程序崩溃也不丢日志
//
// 每行格式：
//   [2026-09-10 21:30:12.345] [INFO ] [mainwindow.cpp:34] 打开文件: D:/note.md
//
// 用法：
//   markdown_editor::infrastructure::Logger::init();      // main() 里 QApplication 之后调一次
//   LOG_INFO("应用启动");
//   LOG_WARN("配置缺失, 使用默认值 %1", value);            // %1 %2 ... 是 QString::arg 占位符
//   LOG_ERROR("保存失败: %1", error);
//   markdown_editor::infrastructure::Logger::shutdown();  // main() 返回前调一次
class Logger
{
public:
    enum class Level
    {
        Debug = 0,
        Info,
        Warning,
        Error,
        Fatal,
    };

    // 安装 Qt 消息处理器并打开日志文件。
    // logFilePath 为空时用默认路径（AppData 下的 logs 目录，见 defaultLogFilePath()）。
    // 可以重复调用：会先关掉上一个日志文件再重新打开。
    static void init(const QString &logFilePath = QString());

    // 关闭日志文件并还原 Qt 原来的消息处理器。
    static void shutdown();

    // 低于该级别的日志不再输出（默认 Debug，即全部输出）。
    static void setMinimumLevel(Level level);
    static Level minimumLevel();

    // 当前日志文件的完整路径；未初始化或文件打开失败时返回空字符串。
    static QString logFilePath();

    // 默认日志路径：<AppData>/logs/<应用名>-<日期>.log
    static QString defaultLogFilePath();

    // 供日志宏调用，一般不要直接使用。
    static void log(Level level, const char *file, int line, const QString &message);

private:
    Logger() = delete;
};

namespace detail {

// __FILE__ 一般是完整路径，这里只取文件名，避免日志行太长。
inline constexpr const char *shortFileName(const char *path)
{
    if (path == nullptr) {
        return "";
    }
    const char *name = path;
    for (const char *p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

// ---------- 参数 -> QString ----------
inline QString toQString(const QString &value) { return value; }
inline QString toQString(QStringView value) { return value.toString(); }
inline QString toQString(const QLatin1String &value) { return QString(value); }
inline QString toQString(const QByteArray &value) { return QString::fromUtf8(value); }
inline QString toQString(const char *value) { return QString::fromUtf8(value != nullptr ? value : ""); }
inline QString toQString(char *value) { return QString::fromUtf8(value != nullptr ? value : ""); }
inline QString toQString(const std::string &value) { return QString::fromUtf8(value.c_str()); }
inline QString toQString(QChar value) { return QString(value); }
inline QString toQString(char value) { return QString(QChar::fromLatin1(value)); }

template <typename T, std::enable_if_t<std::is_arithmetic_v<T>, int> = 0>
inline QString toQString(T value)
{
    if constexpr (std::is_same_v<T, bool>) {
        return value ? QStringLiteral("true") : QStringLiteral("false");
    } else {
        return QString::number(value);
    }
}

// 其它类型（枚举、QPoint、容器、QVariant……）：借 QDebug 的 operator<< 兜底。
template <typename T,
          std::enable_if_t<!std::is_arithmetic_v<T> && !std::is_pointer_v<T> && !std::is_array_v<T>, int> = 0>
inline QString toQString(const T &value)
{
    QString text;
    QDebug debug(&text);
    debug << value;
    // QDebug 在流结束时会补一个空格，这里把首尾多余空格都去掉
    if (text.endsWith(QLatin1Char(' '))) {
        text.chop(1);
    }
    // QDebug 有时会在最前面留一个空格，去掉它
    if (text.startsWith(QLatin1Char(' '))) {
        text.remove(0, 1);
    }
    return text;
}

// ---------- 消息模板 + 参数 -> 最终字符串（%1 %2 ... 依次替换）----------
inline QString format(const QString &text) { return text; }
inline QString format(const char *text) { return QString::fromUtf8(text != nullptr ? text : ""); }
inline QString format(QStringView text) { return text.toString(); }
inline QString format(const QLatin1String &text) { return QString(text); }

template <typename First, typename... Rest>
QString format(const QString &text, First &&first, Rest &&...rest)
{
    return format(text.arg(toQString(std::forward<First>(first))), std::forward<Rest>(rest)...);
}

template <typename First, typename... Rest>
QString format(const char *text, First &&first, Rest &&...rest)
{
    return format(QString::fromUtf8(text != nullptr ? text : "").arg(toQString(std::forward<First>(first))),
                  std::forward<Rest>(rest)...);
}

}  // namespace detail
}  // namespace markdown_editor::infrastructure

// 有些平台的头文件（例如 Linux 的 <syslog.h>）也会定义 LOG_INFO 这类名字，先清掉再定义。
#ifdef LOG_INFO
#  undef LOG_INFO
#endif
#ifdef LOG_WARN
#  undef LOG_WARN
#endif
#ifdef LOG_ERROR
#  undef LOG_ERROR
#endif

#define LOG_INFO(...)                                                                 \
    ::markdown_editor::infrastructure::Logger::log(                                   \
        ::markdown_editor::infrastructure::Logger::Level::Info,                       \
        ::markdown_editor::infrastructure::detail::shortFileName(__FILE__), __LINE__, \
        ::markdown_editor::infrastructure::detail::format(__VA_ARGS__))

#define LOG_WARN(...)                                                                 \
    ::markdown_editor::infrastructure::Logger::log(                                   \
        ::markdown_editor::infrastructure::Logger::Level::Warning,                    \
        ::markdown_editor::infrastructure::detail::shortFileName(__FILE__), __LINE__, \
        ::markdown_editor::infrastructure::detail::format(__VA_ARGS__))

#define LOG_ERROR(...)                                                                \
    ::markdown_editor::infrastructure::Logger::log(                                   \
        ::markdown_editor::infrastructure::Logger::Level::Error,                      \
        ::markdown_editor::infrastructure::detail::shortFileName(__FILE__), __LINE__, \
        ::markdown_editor::infrastructure::detail::format(__VA_ARGS__))

#endif  // LOGGER_H