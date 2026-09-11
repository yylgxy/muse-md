#include "logger.h"

#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QSysInfo>

#include <cstdio>
#include <cstdlib>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace markdown_editor::infrastructure {
namespace {

QMutex g_mutex;
QFile g_file;
QString g_filePath;
Logger::Level g_minimumLevel = Logger::Level::Debug;
QtMessageHandler g_previousHandler = nullptr;
bool g_handlerInstalled = false;

// 单个日志文件超过这个大小就轮转成 <文件名>.1
constexpr qint64 kMaxFileSizeBytes = 5 * 1024 * 1024;

const char *levelName(Logger::Level level)
{
    switch (level) {
    case Logger::Level::Debug:
        return "DEBUG";
    case Logger::Level::Info:
        return "INFO";
    case Logger::Level::Warning:
        return "WARN";
    case Logger::Level::Error:
        return "ERROR";
    case Logger::Level::Fatal:
        return "FATAL";
    }
    return "?";
}

// 级别标签固定 5 个字符宽，多行日志才对得齐
QString levelTag(Logger::Level level)
{
    return QString::fromLatin1(levelName(level)).leftJustified(5, QLatin1Char(' '));
}

QString applicationName()
{
    const QString name = QCoreApplication::applicationName();
    return name.isEmpty() ? QStringLiteral("app") : name;
}

QString defaultFileName()
{
    return QStringLiteral("%1-%2.log")
        .arg(applicationName(), QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd")));
}

// 默认日志目录：先 AppData，再可执行文件目录（AppData 不可写时兜底）
QStringList defaultLogDirs()
{
    QStringList dirs;
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!dataDir.isEmpty()) {
        dirs << dataDir + QStringLiteral("/logs");
    }
    const QString exeDir = QCoreApplication::applicationDirPath();
    if (!exeDir.isEmpty()) {
        dirs << exeDir + QStringLiteral("/logs");
    }
    return dirs;
}

QString buildInfo()
{
    // CMake 的 Debug 不加 NDEBUG，Release/RelWithDebInfo/MinSizeRel 会加
#if defined(NDEBUG)
    const QString config = QStringLiteral("Release");
#else
    const QString config = QStringLiteral("Debug");
#endif

    QString compiler = QStringLiteral("unknown");
#if defined(__clang__)
    compiler = QStringLiteral("Clang %1.%2").arg(__clang_major__).arg(__clang_minor__);
#elif defined(_MSC_VER)
    compiler = QStringLiteral("MSVC %1.%2").arg(_MSC_VER / 100).arg(_MSC_VER % 100);
#elif defined(__GNUC__)
    compiler = QStringLiteral("GCC %1.%2").arg(__GNUC__).arg(__GNUC_MINOR__);
#endif

    return QStringLiteral("%1 / Qt %2 / %3 / %4")
        .arg(config, QString::fromLatin1(qVersion()), QSysInfo::prettyProductName(), compiler);
}

bool openLogFile(const QString &path)
{
    const QString dir = QFileInfo(path).absolutePath();
    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
    }
    g_file.setFileName(path);
    if (!g_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        std::fprintf(stderr, "[Logger] 无法打开日志文件: %s (%s)\n",
                     qPrintable(path), qPrintable(g_file.errorString()));
        return false;
    }
    return true;
}

void rotateIfTooLarge()
{
    if (!g_file.isOpen() || g_file.size() < kMaxFileSizeBytes) {
        return;
    }
    const QString path = g_file.fileName();
    g_file.close();
    const QString backup = path + QStringLiteral(".1");
    QFile::remove(backup);
    if (!QFile::rename(path, backup)) {
        std::fprintf(stderr, "[Logger] 日志轮转失败: %s\n", qPrintable(path));
    }
    openLogFile(path);
}

void writeToDebugger(const QString &text)
{
#ifdef Q_OS_WIN
    // Windows 调试器和 DebugView 能看到；Qt Creator 的「应用程序输出」走下面的 stderr
    ::OutputDebugStringW(reinterpret_cast<const wchar_t *>(text.utf16()));
#else
    Q_UNUSED(text)
#endif
}

// 真正写出一行：过滤、格式化、加锁都在这里
void writeLogLine(Logger::Level level, const char *file, int line, const QString &message)
{
    if (static_cast<int>(level) < static_cast<int>(g_minimumLevel)) {
        return;
    }

    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"));

    QString where = QStringLiteral("qt");
    if (file != nullptr && *file != '\0') {
        where = QString::fromUtf8(file);
        if (line > 0) {
            where += QLatin1Char(':') + QString::number(line);
        }
    }

    const QString text =
        QStringLiteral("[%1] [%2] [%3] %4").arg(timestamp, levelTag(level), where, message);

    QMutexLocker locker(&g_mutex);

    // 控制台：Qt Creator 的「应用程序输出」面板抓的就是 stdout/stderr
    const QByteArray utf8 = text.toUtf8();
    std::fwrite(utf8.constData(), 1, static_cast<size_t>(utf8.size()), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);

    writeToDebugger(text);

    if (g_file.isOpen()) {
        g_file.write(utf8);
        g_file.write("\n", 1);
        g_file.flush();  // 逐行 flush：崩溃或被强杀也保得住日志
        rotateIfTooLarge();
    }
}

// 接管 Qt 自己的 qDebug/qInfo/qWarning/qCritical/qFatal
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Logger::Level level = Logger::Level::Debug;
    switch (type) {
    case QtDebugMsg:
        level = Logger::Level::Debug;
        break;
    case QtInfoMsg:
        level = Logger::Level::Info;
        break;
    case QtWarningMsg:
        level = Logger::Level::Warning;
        break;
    case QtCriticalMsg:
        level = Logger::Level::Error;
        break;
    case QtFatalMsg:
        level = Logger::Level::Fatal;
        break;
    }

    writeLogLine(level, context.file, context.line, message);

    if (type == QtFatalMsg) {
        QMutexLocker locker(&g_mutex);
        if (g_file.isOpen()) {
            g_file.flush();
        }
        std::abort();
    }
}

}  // namespace

void Logger::init(const QString &logFilePath)
{
    QString openedPath;

    {
        QMutexLocker locker(&g_mutex);

        if (!g_handlerInstalled) {
            g_previousHandler = qInstallMessageHandler(messageHandler);
            g_handlerInstalled = true;
        }

        if (g_file.isOpen()) {
            g_file.flush();
            g_file.close();
        }
        g_filePath.clear();

        QStringList candidates;
        if (!logFilePath.isEmpty()) {
            candidates << logFilePath;
        } else {
            const QString fileName = defaultFileName();
            const QStringList dirs = defaultLogDirs();
            for (const QString &dir : dirs) {
                candidates << dir + QLatin1Char('/') + fileName;
            }
        }

        for (const QString &candidate : candidates) {
            if (openLogFile(candidate)) {
                openedPath = QFileInfo(candidate).absoluteFilePath();
                g_filePath = openedPath;
                break;
            }
        }
    }

    // 注意：上面那把锁必须先释放，写日志本身也要加同一把锁
    QString title = applicationName();
    const QString version = QCoreApplication::applicationVersion();
    if (!version.isEmpty()) {
        title += QLatin1Char(' ') + version;
    }

    writeLogLine(Level::Info, detail::shortFileName(__FILE__), __LINE__,
                 QStringLiteral("================ %1 启动 ================").arg(title));
    if (openedPath.isEmpty()) {
        writeLogLine(Level::Warning, detail::shortFileName(__FILE__), __LINE__,
                     QStringLiteral("日志文件不可用，日志只输出到控制台"));
    } else {
        writeLogLine(Level::Info, detail::shortFileName(__FILE__), __LINE__,
                     QStringLiteral("日志文件: %1").arg(openedPath));
    }
    writeLogLine(Level::Info, detail::shortFileName(__FILE__), __LINE__,
                 QStringLiteral("构建信息: %1").arg(buildInfo()));
}

void Logger::shutdown()
{
    QMutexLocker locker(&g_mutex);

    if (g_handlerInstalled) {
        qInstallMessageHandler(g_previousHandler);
        g_handlerInstalled = false;
        g_previousHandler = nullptr;
    }
    if (g_file.isOpen()) {
        g_file.flush();
        g_file.close();
    }
    g_filePath.clear();
}

void Logger::setMinimumLevel(Level level)
{
    QMutexLocker locker(&g_mutex);
    g_minimumLevel = level;
}

Logger::Level Logger::minimumLevel()
{
    QMutexLocker locker(&g_mutex);
    return g_minimumLevel;
}

QString Logger::logFilePath()
{
    QMutexLocker locker(&g_mutex);
    return g_filePath;
}

QString Logger::defaultLogFilePath()
{
    const QStringList dirs = defaultLogDirs();
    if (dirs.isEmpty()) {
        return QString();
    }
    return dirs.first() + QLatin1Char('/') + defaultFileName();
}

void Logger::log(Level level, const char *file, int line, const QString &message)
{
    writeLogLine(level, file, line, message);
}

}  // namespace markdown_editor::infrastructure