#include "thememanager.h"

#include "configmanager.h"
#include "logger.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStyle>
#include <QWidget>

using markdown_editor::core::document::ThemePalette;

namespace {

// 资源路径：两份 QSS 都在 qrc 里（resources/styles/），打包进 exe，不依赖磁盘文件
QString styleSheetResourcePath(ThemeManager::Theme theme)
{
    return (theme == ThemeManager::Theme::Dark) ? QStringLiteral(":/styles/dark.qss")
                                                : QStringLiteral(":/styles/light.qss");
}

// 暗色主题的调色板。QSS 管不到的地方（原生对话框、禁用态文字、交替行底色…）靠它兜住，
// 否则切到暗色后会零星留下几块系统白底 —— 那种"没切干净"比不切还难看。
QPalette darkPalette()
{
    QPalette palette;
    const QColor window(0x0d, 0x11, 0x17);
    const QColor base(0x0d, 0x11, 0x17);
    const QColor alternate(0x16, 0x1b, 0x22);
    const QColor text(0xc9, 0xd1, 0xd9);
    const QColor button(0x21, 0x26, 0x2d);
    const QColor highlight(0x1f, 0x6f, 0xeb);
    const QColor disabled(0x6e, 0x76, 0x81);

    palette.setColor(QPalette::Window, window);
    palette.setColor(QPalette::WindowText, text);
    palette.setColor(QPalette::Base, base);
    palette.setColor(QPalette::AlternateBase, alternate);
    palette.setColor(QPalette::Text, text);
    palette.setColor(QPalette::Button, button);
    palette.setColor(QPalette::ButtonText, text);
    palette.setColor(QPalette::BrightText, QColor(0xff, 0x7b, 0x72));
    palette.setColor(QPalette::Highlight, highlight);
    palette.setColor(QPalette::HighlightedText, QColor(0xff, 0xff, 0xff));
    palette.setColor(QPalette::ToolTipBase, alternate);
    palette.setColor(QPalette::ToolTipText, text);
    palette.setColor(QPalette::PlaceholderText, disabled);
    palette.setColor(QPalette::Link, QColor(0x58, 0xa6, 0xff));
    palette.setColor(QPalette::LinkVisited, QColor(0xd2, 0xa8, 0xff));

    palette.setColor(QPalette::Disabled, QPalette::Text, disabled);
    palette.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);

    return palette;
}

}  // namespace

ThemeManager::ThemeManager(QObject *parent) : QObject(parent)
{
    // 记住系统原本的调色板：切回亮色时要还原成它（而不是我们自己拼一套"亮色"，
    // 那样在 Windows 上会丢掉系统的字体/间隔等细节）。
    m_lightPalette = QApplication::style()->standardPalette();
}

ThemeManager::~ThemeManager() = default;

ThemeManager &ThemeManager::instance()
{
    // 函数内静态局部变量：C++11 起初始化是线程安全的，不需要自己加锁
    static ThemeManager manager;
    return manager;
}

ThemeManager::Theme ThemeManager::theme() const
{
    return m_theme;
}

QString ThemeManager::themeId(Theme theme)
{
    return (theme == Theme::Dark) ? QStringLiteral("dark") : QStringLiteral("light");
}

ThemeManager::Theme ThemeManager::themeFromId(const QString &id)
{
    // 认不出来就当亮色：配置被手改坏了、或者将来加了第三种主题时，
    // "退化成亮色"总比"启动就崩"或者"一片什么都看不清"好。
    return (id.trimmed().compare(QLatin1String("dark"), Qt::CaseInsensitive) == 0) ? Theme::Dark : Theme::Light;
}

QString ThemeManager::styleSheetFor(Theme theme)
{
    const QString path = styleSheetResourcePath(theme);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        LOG_WARN("读不到主题样式表（%1），界面会退化成系统默认外观。"
                 "检查 resources/resources.qrc 里有没有把 styles/ 下的两个 qss 加进去", path);
        return QString();
    }
    return QString::fromUtf8(file.readAll());
}

ThemePalette ThemeManager::editorPalette(Theme theme)
{
    return (theme == Theme::Dark) ? ThemePalette::dark() : ThemePalette::light();
}

ThemePalette ThemeManager::currentPalette() const
{
    if (m_hasCustomTheme) {
        return m_customPalette;
    }
    return editorPalette(m_theme);
}

QString ThemeManager::storageKey()
{
    return QStringLiteral("theme");
}

// ============================================================================
// 主题导入导出（C7）
// ============================================================================

QString ThemeManager::themesDirectory() const
{
    // 自定义主题放 AppData（和配置、快照仓库同级），不往用户文档目录塞文件。
    // 根目录直接取 ConfigManager::filePath() 的父目录 —— 这样 themes/ 和 config.ini、
    // history/（快照仓库）落在同一个 <AppData>/Dev/MarkdownEditor/ 下，路径是统一的。
    // 测试里 ConfigManager 指向临时文件时，这里自然也跟着落到临时目录，不会碰用户数据。
    const QString config = ConfigManager::filePath();
    const int slash = qMax(config.lastIndexOf(QLatin1Char('/')), config.lastIndexOf(QLatin1Char('\\')));
    const QString root = (slash >= 0) ? config.left(slash) : QStringLiteral(".");
    return root + QStringLiteral("/themes");
}

bool ThemeManager::exportTheme(const QString &path, QString *error) const
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    if (path.isEmpty()) {
        return fail(QStringLiteral("没有指定导出路径"));
    }

    // 导出的是**当前**配色（可能是自定义主题，也可能就是内置亮/暗）——
    // 用户想分享他正在用的样子，这是最直觉的语义。
    const QJsonObject obj = currentPalette().toJson();
    const QJsonDocument doc(obj);

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("写不进 %1：%2").arg(path, file.errorString()));
    }
    file.write(doc.toJson(QJsonDocument::Indented));
    return true;
}

bool ThemeManager::importTheme(const QString &path, const QString &name, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return false;
    };

    if (path.isEmpty()) {
        return fail(QStringLiteral("没有指定要导入的文件"));
    }

    QString readError;
    const ThemePalette palette = paletteFromFile(path, &readError);
    if (readError.isEmpty() == false) {
        return fail(readError);
    }

    // WCAG 校验：正文和语法色对底色都要能看清（和内置主题同一条硬要求）。
    // 导入一个"看不清"的主题，用户第一眼就会以为软件坏了，所以这里直接拒绝，
    // 并把"哪个颜色不合格、对比度多少"说清楚 —— 让人能改，而不是只给一句"不行"。
    const auto requireContrast = [](const QString &label, const QColor &fg, const QColor &bg,
                                    double minimum, QString *bad) {
        const double ratio = ThemePalette::contrastRatio(fg, bg);
        if (ratio < minimum) {
            *bad = QStringLiteral("%1 对底色对比度 %2，低于 %3")
                       .arg(label)
                       .arg(ratio, 0, 'f', 2)
                       .arg(minimum, 0, 'f', 1);
            return false;
        }
        return true;
    };
    QString bad;
    const QColor &bg = palette.editorBackground;
    if (!requireContrast(QStringLiteral("正文"), palette.editorForeground, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("标题"), palette.heading, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("粗体"), palette.bold, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("斜体"), palette.italic, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("行内代码"), palette.inlineCodeForeground, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("链接"), palette.link, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("引用"), palette.blockQuote, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("列表标记"), palette.listMarker, bg, 4.5, &bad)
        || !requireContrast(QStringLiteral("代码块文字"), palette.codeBlockForeground, bg, 4.5, &bad)) {
        return fail(QStringLiteral("对比度校验不过：%1").arg(bad));
    }

    // 落盘：文件名用 name（传空则用原文件名去后缀），确保安全（去掉路径分隔符）。
    QString base = name;
    if (base.isEmpty()) {
        base = QFileInfo(path).completeBaseName();
    }
    base = base.simplified();
    base.replace(QLatin1Char('/'), QLatin1Char('_'));
    base.replace(QLatin1Char('\\'), QLatin1Char('_'));
    if (base.isEmpty()) {
        base = QStringLiteral("imported");
    }

    const QString dir = themesDirectory();
    if (!QDir().mkpath(dir)) {
        return fail(QStringLiteral("建不出主题目录 %1").arg(dir));
    }
    const QString target = dir + QLatin1Char('/') + base + QStringLiteral(".json");

    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return fail(QStringLiteral("写不进 %1：%2").arg(target, file.errorString()));
    }
    file.write(QJsonDocument(palette.toJson()).toJson(QJsonDocument::Indented));

    m_importedThemePath = target;
    return true;
}

QString ThemeManager::importedThemePath() const
{
    return m_importedThemePath;
}

QList<QPair<QString, QString>> ThemeManager::customThemes() const
{
    QList<QPair<QString, QString>> result;
    const QDir dir(themesDirectory());
    if (!dir.exists()) {
        return result;
    }
    const QFileInfoList files = dir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QFileInfo &info : files) {
        result.append(qMakePair(info.completeBaseName(), info.absoluteFilePath()));
    }
    return result;
}

ThemePalette ThemeManager::paletteFromFile(const QString &path, QString *error)
{
    const auto fail = [error](const QString &why) {
        if (error != nullptr) {
            *error = why;
        }
        return ThemePalette();
    };

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        fail(QStringLiteral("读不到 %1：%2").arg(path, file.errorString()));
        return ThemePalette();
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        fail(QStringLiteral("%1 不是有效的主题 JSON：%2").arg(path, parseError.errorString()));
        return ThemePalette();
    }

    ThemePalette palette;
    if (!ThemePalette::fromJson(doc.object(), &palette, error)) {
        return ThemePalette();
    }
    return palette;
}

void ThemeManager::applyCustomTheme(const ThemePalette &palette, Theme baseTheme)
{
    // 先切 QSS（控件外观用亮/暗二选一），再记住自定义配色。
    // 顺序：先 setTheme 把 QSS/调色板铺好，再设 m_hasCustomTheme + 存配色，
    // 最后发一个 themeChanged 让编辑器/预览拿新配色重绘。
    setTheme(baseTheme);

    m_customPalette = palette;
    m_hasCustomTheme = true;

    // 复用 themeChanged 信号把自定义配色推给编辑器/预览。
    // 注意：setTheme 已经发过一次 themeChanged（baseTheme），这里再发一次，
    // 消费方（编辑器）拿到的 currentPalette() 已经是最新的自定义配色 ——
    // 第一次是"QSS 换好了"，第二次是"配色换成自定义的了"，各管一半。
    emit themeChanged(m_theme);

    LOG_INFO("已应用自定义主题（控件外观跟随 %1）", themeId(baseTheme));
}

void ThemeManager::clearCustomTheme()
{
    if (!m_hasCustomTheme) {
        return;
    }
    m_hasCustomTheme = false;
    m_customPalette = ThemePalette();
    emit themeChanged(m_theme);
    LOG_INFO("已清除自定义主题，回到内置%1主题", themeId(m_theme));
}

bool ThemeManager::hasCustomTheme() const
{
    return m_hasCustomTheme;
}

void ThemeManager::setTheme(Theme theme)
{
    if (m_applied && theme == m_theme) {
        // 已经应用过、而且主题没变：**什么都不做**。
        // 这条不是省事：重复应用会触发整棵控件树的重新 polish，表现就是"点一下闪一下"。
        return;
    }

    m_theme = theme;
    applyToApplication(theme);
    m_applied = true;

    // 落盘：下次启动还是这个主题。写配置失败不影响本次切换（只记日志）。
    ConfigManager::setValue(storageKey(), themeId(theme));

    emit themeChanged(theme);
    LOG_INFO("主题已切换: %1", themeId(theme));
}

void ThemeManager::applySavedTheme()
{
    const Theme saved = themeFromId(ConfigManager::value(storageKey()).toString());
    // 即使和当前默认值相同也要走一遍 setTheme()：m_applied 还是 false 时会真的应用，
    // 否则启动时用的就是系统配色（而不是我们的亮色主题）。
    setTheme(saved);
}

void ThemeManager::applyToApplication(Theme theme)
{
    // 1) 调色板：先设它，再设样式表。顺序反过来的话，QSS 应用过程中控件可能
    //    先用旧调色板重绘一次（能看出来的闪一下）。
    if (theme == Theme::Dark) {
        QApplication::setPalette(darkPalette());
    } else {
        QApplication::setPalette(m_lightPalette);
    }

    // 2) 样式表：一次设给 QApplication，整棵控件树一次换完 ——
    //    逐个控件 setStyleSheet 才会出现"这块换了那块还没换"的中间态。
    //    注意：setStyleSheet() 不是静态函数（styleSheet() 才是），得通过 qApp 调。
    qApp->setStyleSheet(styleSheetFor(theme));
}
