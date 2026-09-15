#include "codehighlighter.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace markdown_editor::core::document {

namespace {

// ============================================================================
// 小工具
// ============================================================================

// 把空格分隔的一长串词切成列表。语言词表都是这个形状，写成字面量最省地方也最好改。
QStringList words(const char *spaceSeparated)
{
    return QString::fromLatin1(spaceSeparated).split(QLatin1Char(' '), Qt::SkipEmptyParts);
}

bool isIdentStart(QChar c)
{
    return c.isLetter() || c == QLatin1Char('_');
}

bool isIdentPart(QChar c)
{
    return c.isLetterOrNumber() || c == QLatin1Char('_');
}

bool isHexDigit(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'a' && u <= 'f') || (u >= 'A' && u <= 'F');
}

// 在 pos 处是不是正好是 needle。手写循环而不是 QString::mid().startsWith()：
// 扫描是逐字符走的，这里不能有分配。
bool matchesAt(const QString &text, int pos, const QString &needle)
{
    if (needle.isEmpty() || pos < 0 || pos + needle.size() > text.size()) {
        return false;
    }
    for (int k = 0; k < needle.size(); ++k) {
        if (text.at(pos + k) != needle.at(k)) {
            return false;
        }
    }
    return true;
}

void pushToken(QVector<CodeToken> &tokens, int start, int length, CodeTokenKind kind)
{
    if (length <= 0 || kind == CodeTokenKind::Plain) {
        return;  // Plain 不进结果：调用方自己把空隙当普通文字输出
    }
    CodeToken token;
    token.start = start;
    token.length = length;
    token.kind = kind;
    tokens.append(token);
}

// 扫描数字：十进制/十六进制/二进制/八进制、下划线分隔、指数、常见后缀（f/L/u/ll…）。
int scanNumber(const QString &code, int start)
{
    const int n = code.size();
    int j = start;

    if (code.at(j) == '0' && j + 1 < n) {
        const QChar next = code.at(j + 1);
        if (next == QLatin1Char('x') || next == QLatin1Char('X') || next == QLatin1Char('b') || next == QLatin1Char('B')
            || next == QLatin1Char('o') || next == QLatin1Char('O')) {
            j += 2;
            while (j < n && (isHexDigit(code.at(j)) || code.at(j) == QLatin1Char('_'))) {
                ++j;
            }
            return j;
        }
    }

    while (j < n && (code.at(j).isDigit() || code.at(j) == QLatin1Char('_') || code.at(j) == QLatin1Char('.'))) {
        ++j;
    }
    if (j < n && (code.at(j) == QLatin1Char('e') || code.at(j) == QLatin1Char('E'))) {
        ++j;
        if (j < n && (code.at(j) == QLatin1Char('+') || code.at(j) == QLatin1Char('-'))) {
            ++j;
        }
        while (j < n && code.at(j).isDigit()) {
            ++j;
        }
    }
    // 后缀：1.0f / 10L / 5u / 3.0d
    while (j < n) {
        const QChar c = code.at(j);
        if (c == QLatin1Char('f') || c == QLatin1Char('F') || c == QLatin1Char('L') || c == QLatin1Char('l')
            || c == QLatin1Char('u') || c == QLatin1Char('U') || c == QLatin1Char('d') || c == QLatin1Char('D')) {
            ++j;
        } else {
            break;
        }
    }
    return j;
}

// 一行注释/块注释/字符串都要用到的"跳到行尾"
int endOfLine(const QString &code, int start)
{
    const int at = code.indexOf(QLatin1Char('\n'), start);
    return (at < 0) ? code.size() : at;
}

// 字符串扫描：返回结束下标（含结束引号的下一位）；未闭合则返回 n
int scanQuotedEnd(const QString &code, int start, bool escapes)
{
    const int n = code.size();
    const QChar quote = code.at(start);
    int j = start + 1;
    while (j < n) {
        const QChar c = code.at(j);
        if (escapes && c == QLatin1Char('\\')) {
            j += 2;
            continue;
        }
        if (c == quote) {
            return j + 1;  // 含结束引号
        }
        ++j;
    }
    return n;  // 未闭合：一直吃到末尾（宁可多着色，也不要漏）
}

// 三引号字符串（Python / Kotlin 的 """ ）：返回结束下标
int scanTripleQuotedEnd(const QString &code, int start)
{
    const int n = code.size();
    const QString fence = code.mid(start, 3);
    const int at = code.indexOf(fence, start + 3);
    return (at < 0) ? n : at + 3;
}

// ============================================================================
// 语言规则表
// ============================================================================

enum class CodeMode {
    Generic,   // C 家族、脚本语言、SQL 这一类：注释/字符串/数字/词表
    Markup,    // HTML / XML
    Css,       // CSS
    KeyValue,  // JSON / YAML / TOML / INI
    Diff,      // diff / patch
};

struct LangSpec
{
    CodeMode mode = CodeMode::Generic;

    QStringList lineComments;      // 例如 {"//"}、{"#"}、{"--"}
    QString blockCommentStart;     // 例如 "/*"
    QString blockCommentEnd;       // 例如 "*/"

    QStringList stringQuotes;      // 例如 { "\"", "'" }
    bool tripleQuotes = false;     // Python/Kotlin 的 """ / '''
    bool backtickStrings = false;  // JS / Go / Shell 的反引号
    bool stringEscapes = true;     // 字符串里 \" 是不是转义（Shell 的单引号不是）

    bool preprocessorHash = false;  // C 系：#include / #define 一直吃到行尾
    bool annotationAt = false;      // @Override / @decorator / @interface
    bool dollarVariables = false;   // $var / ${var} / $(var)
    bool caseInsensitive = false;   // SQL / Dockerfile
    bool hyphenatedIdentifiers = false;  // PowerShell 的 Write-Host 这种带连字符的命令名

    // KeyValue 模式的细分
    bool jsonLike = false;   // 键必须带引号，后面跟 ':'，没有注释
    bool yamlLike = false;   // 键: 值；支持 - 列表与 --- 分隔
    bool tomlLike = false;   // 键 = 值；[section]；# 注释
    bool iniLike = false;    // 键 = 值；[section]；# 与 ; 注释

    QStringList keywords;
    QStringList types;
    QStringList literals;
    QStringList builtins;

    // 查表用的集合（finalize() 里生成；caseInsensitive 时统一转小写）
    QSet<QString> keywordSet;
    QSet<QString> typeSet;
    QSet<QString> literalSet;
    QSet<QString> builtinSet;

    void finalize()
    {
        const auto toSet = [this](const QStringList &list) {
            QSet<QString> set;
            for (const QString &word : list) {
                set.insert(caseInsensitive ? word.toLower() : word);
            }
            return set;
        };
        keywordSet = toSet(keywords);
        typeSet = toSet(types);
        literalSet = toSet(literals);
        builtinSet = toSet(builtins);
    }
};

// C 风格：// 与 /* */ 注释，双引号/单引号字符串
LangSpec cLike(const char *keywords, const char *types = "", const char *literals = "", const char *builtins = "")
{
    LangSpec spec;
    spec.lineComments = {QStringLiteral("//")};
    spec.blockCommentStart = QStringLiteral("/*");
    spec.blockCommentEnd = QStringLiteral("*/");
    spec.stringQuotes = {QStringLiteral("\""), QStringLiteral("'")};
    spec.keywords = words(keywords);
    spec.types = words(types);
    spec.literals = words(literals);
    spec.builtins = words(builtins);
    return spec;
}

// # 行注释（脚本语言、CMake、Dockerfile、YAML…）
LangSpec hashLike(const char *keywords, const char *types = "", const char *literals = "", const char *builtins = "")
{
    LangSpec spec = cLike(keywords, types, literals, builtins);
    spec.lineComments = {QStringLiteral("#")};
    spec.blockCommentStart.clear();
    spec.blockCommentEnd.clear();
    return spec;
}

// JavaScript 的规则。TypeScript 在它基础上加词表，所以抽成函数（不能在同一段作用域里"复制"）
LangSpec javascriptSpec()
{
    LangSpec js = cLike(
        "async await break case catch class const continue debugger default delete do else export extends finally "
        "for function get if import in instanceof let new of return set static super switch this throw try typeof "
        "var void while with yield",
        "Array ArrayBuffer Boolean Date Error Function Map Number Object Promise Proxy RegExp Set String Symbol "
        "WeakMap BigInt JSON Math",
        "true false null undefined NaN Infinity",
        "console document window require module exports process setTimeout setInterval fetch alert addEventListener "
        "querySelector parseInt parseFloat isNaN stringify assign keys values entries push pop map filter forEach "
        "reduce slice splice join split replace test match");
    js.backtickStrings = true;
    return js;
}

struct Registry
{
    QStringList order;                  // 规范名，按"常用程度"排（下拉框直接用这个顺序）
    QHash<QString, LangSpec> specs;
    QHash<QString, QString> display;    // 规范名 → 给人看的名字
    QHash<QString, QString> canonical;  // 小写输入（含别名）→ 规范名

    void add(const QString &name, const QString &shown, LangSpec spec, const QStringList &aliases = QStringList())
    {
        spec.finalize();
        order << name;
        specs.insert(name, spec);
        display.insert(name, shown);
        canonical.insert(name.toLower(), name);
        for (const QString &alias : aliases) {
            canonical.insert(alias.toLower(), name);
        }
    }
};

const Registry &registry()
{
    static const Registry instance = [] {
        Registry r;

        // ---------------------------- C 家族 ----------------------------
        {
            LangSpec c = cLike("auto break case const continue default do else enum extern for goto if inline register "
                               "restrict return sizeof static struct switch typedef union volatile while _Bool _Atomic",
                               "char double float int long short signed unsigned void size_t ssize_t ptrdiff_t FILE bool "
                               "int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t",
                               "NULL true false",
                               "printf fprintf sprintf snprintf scanf sscanf malloc calloc realloc free memcpy memset "
                               "memmove strlen strcmp strcpy strcat strstr fopen fclose fread fwrite fseek exit abort");
            c.preprocessorHash = true;
            r.add(QStringLiteral("c"), QStringLiteral("C"), c, {QStringLiteral("h")});
        }
        {
            LangSpec cpp = cLike(
                "alignas alignof and auto break case catch class concept const consteval constexpr constinit const_cast "
                "continue co_await co_return co_yield decltype default delete do dynamic_cast else enum explicit export "
                "extern final for friend goto if inline mutable namespace new noexcept not operator or override private "
                "protected public register reinterpret_cast requires return sizeof static static_assert static_cast struct "
                "switch template this thread_local throw try typedef typeid typename union using virtual volatile while xor "
                "nullptr true false",
                "bool char char8_t char16_t char32_t double float int long short signed unsigned void wchar_t size_t "
                "string wstring vector map set unordered_map unordered_set pair tuple optional variant shared_ptr unique_ptr "
                "weak_ptr function array deque list queue stack ostream istream string_view span",
                "true false nullptr",
                "std cout cin cerr endl printf sprintf malloc free memcpy strlen make_unique make_shared make_pair move "
                "forward swap sort find begin end size push_back emplace_back");
            cpp.preprocessorHash = true;
            r.add(QStringLiteral("cpp"), QStringLiteral("C++"), cpp,
                  {QStringLiteral("c++"), QStringLiteral("cc"), QStringLiteral("cxx"), QStringLiteral("hpp"),
                   QStringLiteral("hh"), QStringLiteral("hxx"), QStringLiteral("h++")});
        }
        {
            LangSpec cs = cLike(
                "abstract as async await base break case catch checked class const continue default delegate do else enum "
                "event explicit extern finally fixed for foreach get goto if implicit in init interface internal is lock "
                "namespace new operator out override params partial private protected public readonly record ref return "
                "sealed set sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using value var "
                "virtual when where while yield",
                "bool byte char decimal double dynamic float int long object sbyte short string uint ulong ushort void "
                "List Dictionary IEnumerable Task Action Func Span Memory",
                "true false null",
                "Console WriteLine ReadLine ToString Parse TryParse Add Remove Contains Length Count");
            r.add(QStringLiteral("csharp"), QStringLiteral("C#"), cs,
                  {QStringLiteral("cs"), QStringLiteral("c#")});
        }
        {
            LangSpec java = cLike(
                "abstract assert break case catch class const continue default do else enum extends final finally for "
                "goto if implements import instanceof interface native new package private protected public record return "
                "sealed static strictfp super switch synchronized this throw throws transient try var volatile while yield",
                "boolean byte char double float int long short void String Object Integer Long Double Float Boolean "
                "Character List Map Set ArrayList HashMap HashSet Optional Stream Thread Exception",
                "true false null",
                "System out println printf Math String valueOf length toString equals hashCode add get put contains");
            java.annotationAt = true;
            r.add(QStringLiteral("java"), QStringLiteral("Java"), java);
        }
        {
            LangSpec kotlin = cLike(
                "abstract actual annotation as break by catch class companion const constructor continue crossinline data "
                "delegate do dynamic else enum expect external final finally for fun get if import in infix init inline "
                "inner interface internal is lateinit noinline object open operator out override package private protected "
                "public reified return sealed set super suspend tailrec this throw try typealias val var vararg when where "
                "while",
                "Int Long Short Byte Double Float Boolean Char String Unit Any Nothing List MutableList Map MutableMap Set "
                "Array Sequence Pair Triple Result",
                "true false null",
                "println print listOf mapOf setOf mutableListOf require check let run apply also with");
            kotlin.annotationAt = true;
            r.add(QStringLiteral("kotlin"), QStringLiteral("Kotlin"), kotlin, {QStringLiteral("kt"), QStringLiteral("kts")});
        }
        {
            LangSpec swift = cLike(
                "associatedtype class deinit enum extension fileprivate func import init inout internal let open operator "
                "private protocol public rethrows static struct subscript typealias var break case continue default defer do "
                "else fallthrough for guard if in repeat return switch where while as catch is throw throws try async await "
                "actor some any",
                "Int Int8 Int16 Int32 Int64 UInt Double Float Bool String Character Array Dictionary Set Optional Any "
                "AnyObject Void Result Error",
                "true false nil self super",
                "print dump map filter reduce append count isEmpty description");
            r.add(QStringLiteral("swift"), QStringLiteral("Swift"), swift);
        }
        {
            LangSpec dart = cLike(
                "abstract as assert async await break case catch class const continue covariant default deferred do dynamic "
                "else enum export extends extension external factory false final finally for get hide if implements import "
                "in interface is late library mixin new null on operator part required rethrow return set show static super "
                "switch sync this throw true try typedef var void while with yield",
                "int double num String bool List Map Set Object dynamic void Future Stream Iterable Duration DateTime",
                "true false null",
                "print toString length add forEach map where toList");
            r.add(QStringLiteral("dart"), QStringLiteral("Dart"), dart);
        }
        {
            LangSpec scala = cLike(
                "abstract case catch class def do else extends false final finally for forSome if implicit import lazy "
                "match new null object override package private protected return sealed super this throw trait try true "
                "type val var while with yield given using enum export",
                "Int Long Double Float Boolean Char String Unit Any AnyRef Nothing List Seq Map Set Option Some None "
                "Vector Array Future Either",
                "true false null",
                "println printf map flatMap filter foldLeft foreach mkString require");
            r.add(QStringLiteral("scala"), QStringLiteral("Scala"), scala);
        }
        {
            LangSpec objc = cLike(
                "auto break case const continue default do else enum extern for goto if inline register restrict return "
                "sizeof static struct switch typedef union volatile while self super nil YES NO id instancetype",
                "char double float int long short signed unsigned void BOOL NSInteger NSUInteger CGFloat NSString NSArray "
                "NSDictionary NSObject NSError",
                "YES NO nil true false NULL",
                "NSLog alloc init retain release autorelease description length objectForKey setObject");
            objc.preprocessorHash = true;
            objc.annotationAt = true;  // @interface / @property / @selector …
            r.add(QStringLiteral("objectivec"), QStringLiteral("Objective-C"), objc,
                  {QStringLiteral("objc"), QStringLiteral("objective-c"), QStringLiteral("mm")});
        }

        // ---------------------------- Web / 脚本 ----------------------------
        {
            r.add(QStringLiteral("javascript"), QStringLiteral("JavaScript"), javascriptSpec(),
                  {QStringLiteral("js"), QStringLiteral("jsx"), QStringLiteral("mjs"), QStringLiteral("cjs"),
                   QStringLiteral("node")});
        }
        {
            LangSpec ts = javascriptSpec();
            ts.keywords << words("interface type enum implements namespace declare readonly abstract as satisfies keyof "
                                 "infer is never unknown any asserts override");
            ts.types << words("Record Partial Required Readonly Pick Omit Exclude Extract PromiseLike Iterable Awaited");
            r.add(QStringLiteral("typescript"), QStringLiteral("TypeScript"), ts,
                  {QStringLiteral("ts"), QStringLiteral("tsx")});
        }
        {
            LangSpec py = hashLike(
                "and as assert async await break class continue def del elif else except finally for from global if import "
                "in is lambda nonlocal not or pass raise return try while with yield match case",
                "bool bytes bytearray complex dict float frozenset int list object set str tuple type range enumerate zip "
                "Any Optional List Dict Tuple Set Iterable Callable",
                "True False None NotImplemented Ellipsis self cls",
                "print len range open super isinstance issubclass getattr setattr hasattr enumerate zip map filter sorted "
                "sum min max abs round any all repr str int float list dict set tuple type id input format join split "
                "strip replace startswith endswith append extend insert remove pop keys values items get update");
            py.tripleQuotes = true;
            py.annotationAt = true;  // @decorator
            r.add(QStringLiteral("python"), QStringLiteral("Python"), py,
                  {QStringLiteral("py"), QStringLiteral("py3"), QStringLiteral("python3")});
        }
        {
            LangSpec rust = cLike(
                "as async await break const continue crate dyn else enum extern fn for if impl in let loop match mod move "
                "mut pub ref return self Self static struct super trait type unsafe use where while",
                "bool char f32 f64 i8 i16 i32 i64 i128 isize str u8 u16 u32 u64 u128 usize String Vec Option Result Box "
                "Rc Arc RefCell Cell HashMap HashSet BTreeMap Cow Path PathBuf",
                "true false None Some Ok Err Self self",
                "println print eprintln format vec! write! assert! assert_eq! panic! todo! unimplemented! len push pop "
                "iter into_iter map filter collect unwrap expect clone to_string");
            r.add(QStringLiteral("rust"), QStringLiteral("Rust"), rust, {QStringLiteral("rs")});
        }
        {
            LangSpec go = cLike(
                "break case chan const continue default defer else fallthrough for func go goto if import interface map "
                "package range return select struct switch type var",
                "bool byte complex64 complex128 error float32 float64 int int8 int16 int32 int64 rune string uint uint8 "
                "uint16 uint32 uint64 uintptr any",
                "true false nil iota",
                "append cap close copy delete len make new panic print println recover fmt Errorf Printf Sprintf Println "
                "string bytes strconv errors sort time context sync");
            r.add(QStringLiteral("go"), QStringLiteral("Go"), go, {QStringLiteral("golang")});
        }
        {
            LangSpec php = cLike(
                "abstract and array as break callable case catch class clone const continue declare default do echo else "
                "elseif empty enddeclare endfor endforeach endif endswitch endwhile enum extends final finally fn for "
                "foreach function global goto if implements include include_once instanceof insteadof interface isset list "
                "match namespace new or print private protected public readonly require require_once return static switch "
                "throw trait try unset use var while xor yield",
                "int float string bool array object mixed void iterable callable self static",
                "true false null TRUE FALSE NULL",
                "echo print count strlen array_map array_filter array_merge var_dump sprintf implode explode trim substr "
                "str_replace in_array isset empty json_encode json_decode preg_match file_get_contents");
            php.dollarVariables = true;
            r.add(QStringLiteral("php"), QStringLiteral("PHP"), php, {QStringLiteral("phtml")});
        }
        {
            LangSpec rb = hashLike(
                "alias and begin break case class def defined? do else elsif end ensure for if in module next not or redo "
                "rescue retry return self super then undef unless until when while yield require require_relative attr_accessor "
                "attr_reader attr_writer lambda proc",
                "Array Hash String Symbol Integer Float Range Struct Module Class Proc Enumerator",
                "true false nil",
                "puts print p require lambda proc new length size each map select reject reduce inject to_s to_i to_sym "
                "freeze frozen? nil? empty? push pop join split gsub sub match");
            r.add(QStringLiteral("ruby"), QStringLiteral("Ruby"), rb, {QStringLiteral("rb")});
        }
        {
            LangSpec bash = hashLike(
                "if then else elif fi case esac for while until do done function in select time coproc break continue return "
                "exit local export readonly declare typeset unset shift source alias eval exec trap set shopt",
                "int string bool array",
                "true false",
                "echo printf cd pwd ls cat grep sed awk cut tr sort uniq head tail find xargs chmod chown mkdir rmdir rm cp "
                "mv touch test read dirname basename sleep kill wait which command type");
            bash.stringQuotes = {QStringLiteral("\""), QStringLiteral("'"), QStringLiteral("`")};
            bash.stringEscapes = true;
            bash.dollarVariables = true;
            r.add(QStringLiteral("bash"), QStringLiteral("Shell"), bash,
                  {QStringLiteral("sh"), QStringLiteral("shell"), QStringLiteral("zsh"), QStringLiteral("ksh"),
                   QStringLiteral("console")});
        }
        {
            LangSpec ps = hashLike(
                "function param begin process end dynamicparam if elseif else switch foreach for while do until try catch "
                "finally throw return break continue in filter class enum using",
                "int string bool array hashtable datetime psobject scriptblock",
                "$true $false $null",
                "Write-Host Write-Output Write-Error Get-ChildItem Get-Content Set-Content Get-Item Select-Object "
                "Where-Object ForEach-Object Sort-Object Measure-Object Test-Path Join-Path Split-Path New-Item Remove-Item "
                "Copy-Item Move-Item Start-Process Stop-Process");
            ps.blockCommentStart = QStringLiteral("<#");
            ps.blockCommentEnd = QStringLiteral("#>");
            ps.dollarVariables = true;
            ps.hyphenatedIdentifiers = true;
            r.add(QStringLiteral("powershell"), QStringLiteral("PowerShell"), ps,
                  {QStringLiteral("ps1"), QStringLiteral("pwsh")});
        }
        {
            LangSpec lua = hashLike(
                "and break do else elseif end false for function goto if in local nil not or repeat return then true until "
                "while",
                "string number boolean table function thread userdata nil",
                "true false nil",
                "print pairs ipairs type tostring tonumber require setmetatable getmetatable rawget rawset error assert "
                "pcall table.insert table.remove string.format math.floor math.max io.open");
            lua.blockCommentStart = QStringLiteral("--[[");
            lua.blockCommentEnd = QStringLiteral("]]");
            lua.lineComments = {QStringLiteral("--")};
            r.add(QStringLiteral("lua"), QStringLiteral("Lua"), lua);
        }
        {
            LangSpec perl = hashLike(
                "my our local sub use require package if elsif else unless while for foreach until do return last next "
                "redo goto and or not eq ne lt gt le ge cmp",
                "scalar array hash",
                "undef",
                "print say printf chomp chop split join push pop shift unshift keys values sort map grep length substr "
                "index sprintf die warn open close");
            perl.dollarVariables = true;
            r.add(QStringLiteral("perl"), QStringLiteral("Perl"), perl, {QStringLiteral("pl"), QStringLiteral("pm")});
        }
        {
            LangSpec rlang = hashLike(
                "if else repeat while function for in next break TRUE FALSE NULL Inf NaN NA return library",
                "numeric character logical integer double list data.frame matrix vector factor",
                "TRUE FALSE NULL NA NaN Inf",
                "print cat paste library require c list data.frame matrix mean median sd sum length seq rep plot read.csv "
                "write.csv names head tail str summary");
            r.add(QStringLiteral("r"), QStringLiteral("R"), rlang, {QStringLiteral("rscript")});
        }

        // ---------------------------- 数据 / 配置 ----------------------------
        {
            LangSpec sql = cLike(
                "select from where insert into values update set delete create table alter drop index view join inner left "
                "right full outer on group by order having limit offset union all distinct as and or not null is in between "
                "like exists case when then else end primary key foreign references default constraint unique check cascade "
                "primary_key begin commit rollback transaction grant revoke with returning",
                "int integer bigint smallint tinyint varchar char text nvarchar date datetime timestamp time decimal numeric "
                "float double real boolean blob json uuid serial",
                "null true false",
                "count sum avg min max coalesce cast now date_part extract concat substring trim upper lower round");
            sql.lineComments = {QStringLiteral("--")};
            sql.stringQuotes = {QStringLiteral("'"), QStringLiteral("\"")};
            sql.caseInsensitive = true;
            r.add(QStringLiteral("sql"), QStringLiteral("SQL"),
                  sql,
                  {QStringLiteral("mysql"), QStringLiteral("postgres"), QStringLiteral("postgresql"),
                   QStringLiteral("sqlite"), QStringLiteral("tsql")});
        }
        {
            LangSpec json;
            json.mode = CodeMode::KeyValue;
            json.jsonLike = true;
            json.stringQuotes = {QStringLiteral("\"")};
            json.literals = words("true false null");
            r.add(QStringLiteral("json"), QStringLiteral("JSON"), json, {QStringLiteral("jsonc")});
        }
        {
            LangSpec yaml;
            yaml.mode = CodeMode::KeyValue;
            yaml.yamlLike = true;
            yaml.lineComments = {QStringLiteral("#")};
            yaml.stringQuotes = {QStringLiteral("\""), QStringLiteral("'")};
            yaml.literals = words("true false null ~ yes no on off");
            r.add(QStringLiteral("yaml"), QStringLiteral("YAML"), yaml,
                  {QStringLiteral("yml")});
        }
        {
            LangSpec toml;
            toml.mode = CodeMode::KeyValue;
            toml.tomlLike = true;
            toml.lineComments = {QStringLiteral("#")};
            toml.stringQuotes = {QStringLiteral("\""), QStringLiteral("'")};
            toml.literals = words("true false");
            r.add(QStringLiteral("toml"), QStringLiteral("TOML"), toml);
        }
        {
            LangSpec ini;
            ini.mode = CodeMode::KeyValue;
            ini.iniLike = true;
            ini.lineComments = {QStringLiteral(";"), QStringLiteral("#")};
            ini.stringQuotes = {QStringLiteral("\""), QStringLiteral("'")};
            ini.literals = words("true false yes no on off");
            r.add(QStringLiteral("ini"), QStringLiteral("INI"), ini,
                  {QStringLiteral("conf"), QStringLiteral("cfg"), QStringLiteral("properties")});
        }
        {
            LangSpec html;
            html.mode = CodeMode::Markup;
            r.add(QStringLiteral("html"), QStringLiteral("HTML / XML"), html,
                  {QStringLiteral("htm"), QStringLiteral("xml"), QStringLiteral("xhtml"), QStringLiteral("svg"),
                   QStringLiteral("xaml"), QStringLiteral("vue"), QStringLiteral("plist")});
        }
        {
            LangSpec css;
            css.mode = CodeMode::Css;
            r.add(QStringLiteral("css"), QStringLiteral("CSS"), css, {QStringLiteral("scss"), QStringLiteral("less")});
        }
        {
            LangSpec cmake = hashLike(
                "cmake_minimum_required project add_executable add_library add_subdirectory target_link_libraries "
                "target_include_directories target_compile_definitions target_compile_options target_sources set unset if "
                "elseif else endif foreach endforeach while endwhile function endfunction macro endmacro return include "
                "find_package find_library find_path find_program option message install configure_file file list string "
                "get_filename_component get_target_property set_target_properties enable_testing add_test mark_as_advanced",
                "STATIC SHARED MODULE INTERFACE OBJECT PRIVATE PUBLIC REQUIRED QUIET EXCLUDE_FROM_ALL",
                "ON OFF TRUE FALSE",
                "AND OR NOT STREQUAL EQUAL LESS GREATER DEFINED EXISTS");
            cmake.caseInsensitive = true;
            cmake.dollarVariables = true;
            r.add(QStringLiteral("cmake"), QStringLiteral("CMake"), cmake);
        }
        {
            LangSpec make = hashLike(
                "include define endef ifeq ifneq ifdef ifndef else endif export override unexport vpath",
                "",
                "",
                "all clean install uninstall test check dist distclean");
            make.dollarVariables = true;
            r.add(QStringLiteral("makefile"), QStringLiteral("Makefile"), make,
                  {QStringLiteral("make"), QStringLiteral("mk"), QStringLiteral("gnumakefile")});
        }
        {
            LangSpec docker = hashLike(
                "from run cmd label maintainer expose env add copy entrypoint volume user workdir arg onbuild stopsignal "
                "healthcheck shell as",
                "",
                "",
                "");
            docker.caseInsensitive = true;  // Dockerfile 的指令大小写不敏感
            docker.dollarVariables = true;
            r.add(QStringLiteral("dockerfile"), QStringLiteral("Dockerfile"), docker,
                  {QStringLiteral("docker")});
        }

        // ---------------------------- diff ----------------------------
        {
            LangSpec diff;
            diff.mode = CodeMode::Diff;
            r.add(QStringLiteral("diff"), QStringLiteral("Diff / Patch"), diff,
                  {QStringLiteral("patch")});
        }

        return r;
    }();
    return instance;
}

// ============================================================================
// 分词器
// ============================================================================

CodeTokenKind classifyWord(const LangSpec &spec, const QString &word)
{
    const QString key = spec.caseInsensitive ? word.toLower() : word;
    if (spec.keywordSet.contains(key)) {
        return CodeTokenKind::Keyword;
    }
    if (spec.literalSet.contains(key)) {
        return CodeTokenKind::Literal;
    }
    if (spec.typeSet.contains(key)) {
        return CodeTokenKind::Type;
    }
    if (spec.builtinSet.contains(key)) {
        return CodeTokenKind::Builtin;
    }
    return CodeTokenKind::Plain;
}

// 通用模式：注释 → 字符串 → 数字 → 预处理/注解/$变量 → 标识符 → 运算符
QVector<CodeToken> tokenizeGeneric(const QString &code, const LangSpec &spec)
{
    QVector<CodeToken> tokens;
    const int n = code.size();
    int i = 0;

    while (i < n) {
        const QChar c = code.at(i);
        bool handled = false;

        // 1) 行注释
        for (const QString &marker : spec.lineComments) {
            if (matchesAt(code, i, marker)) {
                const int end = endOfLine(code, i);
                pushToken(tokens, i, end - i, CodeTokenKind::Comment);
                i = end;
                handled = true;
                break;
            }
        }
        if (handled) {
            continue;
        }

        // 2) 块注释
        if (!spec.blockCommentStart.isEmpty() && matchesAt(code, i, spec.blockCommentStart)) {
            const int at = code.indexOf(spec.blockCommentEnd, i + spec.blockCommentStart.size());
            const int end = (at < 0) ? n : at + spec.blockCommentEnd.size();
            pushToken(tokens, i, end - i, CodeTokenKind::Comment);
            i = end;
            continue;
        }

        // 3) 三引号字符串（Python/Kotlin）
        if (spec.tripleQuotes && (matchesAt(code, i, QStringLiteral("\"\"\"")) || matchesAt(code, i, QStringLiteral("'''")))) {
            const int end = scanTripleQuotedEnd(code, i);
            pushToken(tokens, i, end - i, CodeTokenKind::String);
            i = end;
            continue;
        }

        // 4) 普通字符串
        bool stringDone = false;
        for (const QString &quote : spec.stringQuotes) {
            if (matchesAt(code, i, quote)) {
                const int end = scanQuotedEnd(code, i, spec.stringEscapes);
                pushToken(tokens, i, end - i, CodeTokenKind::String);
                i = end;
                stringDone = true;
                break;
            }
        }
        if (stringDone) {
            continue;
        }

        // 4b) 反引号字符串（JS/TS 的模板串、Go 的原始串）：单独一条规则，
        //     因为它的"转义"规则和普通字符串不完全一样，而上面那张 quotes 表装不下它
        if (spec.backtickStrings && c == QLatin1Char('`')) {
            const int end = scanQuotedEnd(code, i, true);
            pushToken(tokens, i, end - i, CodeTokenKind::String);
            i = end;
            continue;
        }

        // 5) 数字
        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && code.at(i + 1).isDigit())) {
            const int end = scanNumber(code, i);
            pushToken(tokens, i, end - i, CodeTokenKind::Number);
            i = end;
            continue;
        }

        // 6) 预处理指令（C 系）：#include <x> / #define X 1 —— 整行当 Meta，最省事也最像编辑器
        if (spec.preprocessorHash && c == QLatin1Char('#')) {
            const int end = endOfLine(code, i);
            pushToken(tokens, i, end - i, CodeTokenKind::Meta);
            i = end;
            continue;
        }

        // 7) 注解/装饰器：@Override / @app.route("x")
        if (spec.annotationAt && c == QLatin1Char('@')) {
            int j = i + 1;
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('.') || code.at(j) == QLatin1Char('-'))) {
                ++j;
            }
            if (j > i + 1) {
                pushToken(tokens, i, j - i, CodeTokenKind::Meta);
                i = j;
                continue;
            }
        }

        // 8) $变量 / ${变量} / $(变量)
        if (spec.dollarVariables && c == QLatin1Char('$')) {
            int j = i + 1;
            if (j < n && (code.at(j) == QLatin1Char('{') || code.at(j) == QLatin1Char('('))) {
                const QChar close = (code.at(j) == QLatin1Char('{')) ? QLatin1Char('}') : QLatin1Char(')');
                const int at = code.indexOf(close, j + 1);
                j = (at < 0) ? n : at + 1;
            } else {
                while (j < n && isIdentPart(code.at(j))) {
                    ++j;
                }
            }
            if (j > i + 1) {
                pushToken(tokens, i, j - i, CodeTokenKind::Attribute);
                i = j;
                continue;
            }
        }

        // 9) 标识符：关键字/字面量/类型/内置 → 别的再判断"后面跟括号 = 函数调用"
        if (isIdentStart(c)) {
            int j = i;
            while (j < n && isIdentPart(code.at(j))) {
                ++j;
            }
            // PowerShell 的命令名带连字符（Write-Host、Get-ChildItem）：
            // 只在"连字符后面紧跟字母"时并进来，这样 `a - b` 的减号仍然是运算符
            if (spec.hyphenatedIdentifiers) {
                while (j + 1 < n && code.at(j) == QLatin1Char('-') && isIdentStart(code.at(j + 1))) {
                    ++j;
                    while (j < n && isIdentPart(code.at(j))) {
                        ++j;
                    }
                }
            }
            const QString word = code.mid(i, j - i);
            CodeTokenKind kind = classifyWord(spec, word);
            if (kind == CodeTokenKind::Plain) {
                // 函数/方法名：标识符后面（可跳过空白）跟着 '('；Rust 这种宏允许中间有个 '!'
                int k = j;
                while (k < n && (code.at(k) == QLatin1Char(' ') || code.at(k) == QLatin1Char('\t'))) {
                    ++k;
                }
                if (k < n && code.at(k) == QLatin1Char('!')) {
                    ++k;
                    while (k < n && (code.at(k) == QLatin1Char(' ') || code.at(k) == QLatin1Char('\t'))) {
                        ++k;
                    }
                }
                if (k < n && code.at(k) == QLatin1Char('(')) {
                    kind = CodeTokenKind::Function;
                }
            }
            pushToken(tokens, i, j - i, kind);
            i = j;
            continue;
        }

        // 10) 运算符（连续的一串算一个记号）
        static const QString operatorChars = QStringLiteral("+-*/%=<>!&|^~?");
        if (operatorChars.contains(c)) {
            int j = i;
            while (j < n && operatorChars.contains(code.at(j))) {
                ++j;
            }
            pushToken(tokens, i, j - i, CodeTokenKind::Operator);
            i = j;
            continue;
        }

        ++i;  // 其它字符（括号、分号、空白…）不着色
    }

    return tokens;
}

// HTML / XML：标签名、属性名、属性值、注释、DOCTYPE、实体
QVector<CodeToken> tokenizeMarkup(const QString &code)
{
    QVector<CodeToken> tokens;
    const int n = code.size();
    int i = 0;

    while (i < n) {
        if (matchesAt(code, i, QStringLiteral("<!--"))) {
            const int at = code.indexOf(QStringLiteral("-->"), i + 4);
            const int end = (at < 0) ? n : at + 3;
            pushToken(tokens, i, end - i, CodeTokenKind::Comment);
            i = end;
            continue;
        }

        if (matchesAt(code, i, QStringLiteral("<!"))) {  // <!DOCTYPE html>
            const int at = code.indexOf(QLatin1Char('>'), i);
            const int end = (at < 0) ? n : at + 1;
            pushToken(tokens, i, end - i, CodeTokenKind::Meta);
            i = end;
            continue;
        }

        if (code.at(i) == QLatin1Char('<') && i + 1 < n
            && (isIdentStart(code.at(i + 1)) || code.at(i + 1) == QLatin1Char('/'))) {
            int j = i + 1;
            if (code.at(j) == QLatin1Char('/')) {
                ++j;
            }
            const int tagStart = j;
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-') || code.at(j) == QLatin1Char(':'))) {
                ++j;
            }
            pushToken(tokens, tagStart, j - tagStart, CodeTokenKind::Tag);

            // 属性区：属性名 / = / 属性值。遇到 '>' 或 '/>' 结束。
            while (j < n && code.at(j) != QLatin1Char('>')) {
                const QChar c = code.at(j);
                if (isIdentStart(c) || c == QLatin1Char('-') || c == QLatin1Char(':')) {
                    const int attrStart = j;
                    while (j < n
                           && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-') || code.at(j) == QLatin1Char(':'))) {
                        ++j;
                    }
                    pushToken(tokens, attrStart, j - attrStart, CodeTokenKind::Attribute);
                } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                    const int end = scanQuotedEnd(code, j, false);
                    pushToken(tokens, j, end - j, CodeTokenKind::String);
                    j = end;
                } else {
                    ++j;
                }
            }
            i = (j < n) ? j + 1 : n;
            continue;
        }

        if (code.at(i) == QLatin1Char('&')) {  // &nbsp; &#160;
            const int at = code.indexOf(QLatin1Char(';'), i);
            if (at > 0 && at - i <= 10) {
                pushToken(tokens, i, at - i + 1, CodeTokenKind::Literal);
                i = at + 1;
                continue;
            }
        }

        ++i;
    }

    return tokens;
}

// CSS：注释、@规则、选择器、属性名、颜色、数字+单位
//
// 选择器和属性名怎么区分？不能只看"在不在花括号里"：@media 里面还套着一层花括号，
// 那样会把 `.box` 当成属性名。这里换成"语句开头"这个更贴 CSS 的判断：
//   * 一段语句的开头（文件开头、{ 之后、; 之后、} 之后）上出现的标识符：
//       后面跟 ':' → 属性名（color: …）
//       否则       → 选择器（.box / div / @media 里的东西）
//   * 语句中间的标识符（margin: 8px auto 里的 auto）→ 普通文字，免得花花绿绿
QVector<CodeToken> tokenizeCss(const QString &code)
{
    QVector<CodeToken> tokens;
    const int n = code.size();
    int i = 0;
    bool atStatementStart = true;

    while (i < n) {
        const QChar c = code.at(i);

        if (matchesAt(code, i, QStringLiteral("/*"))) {
            const int at = code.indexOf(QStringLiteral("*/"), i + 2);
            const int end = (at < 0) ? n : at + 2;
            pushToken(tokens, i, end - i, CodeTokenKind::Comment);
            i = end;
            continue;
        }

        if (c == QLatin1Char('@')) {  // @media / @import / @keyframes
            int j = i + 1;
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-'))) {
                ++j;
            }
            pushToken(tokens, i, j - i, CodeTokenKind::Meta);
            atStatementStart = false;
            i = j;
            continue;
        }

        if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            const int end = scanQuotedEnd(code, i, true);
            pushToken(tokens, i, end - i, CodeTokenKind::String);
            atStatementStart = false;
            i = end;
            continue;
        }

        if (c == QLatin1Char('#')) {  // #fff 颜色 / #main 选择器
            int j = i + 1;
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-'))) {
                ++j;
            }
            bool looksLikeColor = (j - i - 1) > 0 && (j - i - 1) <= 8;
            for (int k = i + 1; looksLikeColor && k < j; ++k) {
                if (!isHexDigit(code.at(k))) {
                    looksLikeColor = false;
                }
            }
            pushToken(tokens, i, j - i, looksLikeColor ? CodeTokenKind::Number : CodeTokenKind::Tag);
            atStatementStart = false;
            i = j;
            continue;
        }

        if (c.isDigit() || (c == QLatin1Char('.') && i + 1 < n && code.at(i + 1).isDigit())) {
            int j = scanNumber(code, i);
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('%'))) {  // px / em / %
                ++j;
            }
            pushToken(tokens, i, j - i, CodeTokenKind::Number);
            atStatementStart = false;
            i = j;
            continue;
        }

        if (c == QLatin1Char('{') || c == QLatin1Char('}') || c == QLatin1Char(';')) {
            atStatementStart = true;  // 语句边界：后面那个标识符要么是选择器、要么是属性名
            ++i;
            continue;
        }

        if (isIdentStart(c) || c == QLatin1Char('-')) {
            int j = i;
            while (j < n && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-'))) {
                ++j;
            }
            if (atStatementStart) {
                int k = j;
                while (k < n && code.at(k).isSpace()) {
                    ++k;
                }
                const bool isProperty = (k < n && code.at(k) == QLatin1Char(':'));
                pushToken(tokens, i, j - i, isProperty ? CodeTokenKind::Attribute : CodeTokenKind::Tag);
                atStatementStart = false;
            }
            i = j;
            continue;
        }

        ++i;
    }

    return tokens;
}

// 把一行的"键"扫出来：不带引号的标识符（YAML/TOML/INI）或带引号的字符串（JSON）
int scanKeyEnd(const QString &code, int start, int lineEnd)
{
    if (code.at(start) == QLatin1Char('"') || code.at(start) == QLatin1Char('\'')) {
        const int end = scanQuotedEnd(code, start, true);
        return qMin(end, lineEnd);
    }
    int j = start;
    while (j < lineEnd && (isIdentPart(code.at(j)) || code.at(j) == QLatin1Char('-') || code.at(j) == QLatin1Char('.'))) {
        ++j;
    }
    return j;
}

// JSON / YAML / TOML / INI
QVector<CodeToken> tokenizeKeyValue(const QString &code, const LangSpec &spec)
{
    QVector<CodeToken> tokens;
    const int n = code.size();
    int lineStart = 0;

    while (lineStart <= n) {
        int lineEnd = code.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0) {
            lineEnd = n;
        }

        int i = lineStart;
        while (i < lineEnd && code.at(i).isSpace()) {
            ++i;
        }

        // YAML：--- / ... 文档分隔，"- " 列表项
        if (spec.yamlLike && i < lineEnd) {
            if (matchesAt(code, i, QStringLiteral("---")) || matchesAt(code, i, QStringLiteral("..."))) {
                pushToken(tokens, i, 3, CodeTokenKind::Meta);
                i += 3;
            } else if (code.at(i) == QLatin1Char('-') && (i + 1 == lineEnd || code.at(i + 1).isSpace())) {
                pushToken(tokens, i, 1, CodeTokenKind::Operator);
                ++i;
            }
        }

        // TOML / INI 的 [section]
        if ((spec.tomlLike || spec.iniLike) && i < lineEnd && code.at(i) == QLatin1Char('[')) {
            const int at = code.indexOf(QLatin1Char(']'), i);
            const int close = (at < 0 || at > lineEnd) ? lineEnd : at;
            pushToken(tokens, i + 1, close - i - 1, CodeTokenKind::Tag);
            i = (close < lineEnd) ? close + 1 : lineEnd;
        }

        // 行首的键（YAML/TOML/INI 通常不带引号）
        if (i < lineEnd && (spec.yamlLike || spec.tomlLike || spec.iniLike)
            && (isIdentStart(code.at(i)) || code.at(i) == QLatin1Char('"'))) {
            const int keyEnd = scanKeyEnd(code, i, lineEnd);
            int k = keyEnd;
            while (k < lineEnd && code.at(k).isSpace()) {
                ++k;
            }
            if (k < lineEnd && (code.at(k) == QLatin1Char(':') || code.at(k) == QLatin1Char('='))) {
                pushToken(tokens, i, keyEnd - i, CodeTokenKind::Attribute);
                pushToken(tokens, k, 1, CodeTokenKind::Operator);
                i = k + 1;
            }
        }

        // 行内剩余部分：注释 / 字符串 / 数字 / 字面量
        while (i < lineEnd) {
            const QChar c = code.at(i);
            bool handled = false;

            for (const QString &marker : spec.lineComments) {
                if (matchesAt(code, i, marker)) {
                    pushToken(tokens, i, lineEnd - i, CodeTokenKind::Comment);
                    i = lineEnd;
                    handled = true;
                    break;
                }
            }
            if (handled) {
                break;
            }

            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                const int end = qMin(scanQuotedEnd(code, i, true), lineEnd);
                // JSON 里"后面跟冒号"的字符串也是键（{"a": 1, "b": 2} 的第二个键就在行中间）
                int k = end;
                while (k < lineEnd && code.at(k).isSpace()) {
                    ++k;
                }
                const bool isKey = (spec.jsonLike || spec.yamlLike) && k < lineEnd && code.at(k) == QLatin1Char(':');
                pushToken(tokens, i, end - i, isKey ? CodeTokenKind::Attribute : CodeTokenKind::String);
                i = end;
                continue;
            }

            if (c.isDigit() || (c == QLatin1Char('-') && i + 1 < lineEnd && code.at(i + 1).isDigit())) {
                const int end = qMin(scanNumber(code, i), lineEnd);
                pushToken(tokens, i, end - i, CodeTokenKind::Number);
                i = end;
                continue;
            }

            if (isIdentStart(c)) {
                int j = i;
                while (j < lineEnd && isIdentPart(code.at(j))) {
                    ++j;
                }
                const QString word = code.mid(i, j - i);
                const QString key = spec.caseInsensitive ? word.toLower() : word;
                if (spec.literalSet.contains(key)) {
                    pushToken(tokens, i, j - i, CodeTokenKind::Literal);
                }
                i = j;
                continue;
            }

            ++i;
        }

        if (lineEnd >= n) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return tokens;
}

// diff / patch
QVector<CodeToken> tokenizeDiff(const QString &code)
{
    QVector<CodeToken> tokens;
    const int n = code.size();
    int lineStart = 0;

    while (lineStart <= n) {
        int lineEnd = code.indexOf(QLatin1Char('\n'), lineStart);
        if (lineEnd < 0) {
            lineEnd = n;
        }

        if (lineStart < lineEnd) {
            const QChar first = code.at(lineStart);
            const bool triple = matchesAt(code, lineStart, QStringLiteral("+++")) || matchesAt(code, lineStart, QStringLiteral("---"));
            const bool hunk = matchesAt(code, lineStart, QStringLiteral("@@"));
            const bool header = matchesAt(code, lineStart, QStringLiteral("diff ")) || matchesAt(code, lineStart, QStringLiteral("index "))
                                || matchesAt(code, lineStart, QStringLiteral("Index:"));

            if (triple || hunk || header) {
                pushToken(tokens, lineStart, lineEnd - lineStart, CodeTokenKind::Meta);
            } else if (first == QLatin1Char('+')) {
                pushToken(tokens, lineStart, lineEnd - lineStart, CodeTokenKind::DiffAdd);
            } else if (first == QLatin1Char('-')) {
                pushToken(tokens, lineStart, lineEnd - lineStart, CodeTokenKind::DiffDel);
            }
        }

        if (lineEnd >= n) {
            break;
        }
        lineStart = lineEnd + 1;
    }

    return tokens;
}

}  // namespace

// ============================================================================
// 对外接口
// ============================================================================

QStringList CodeHighlighter::supportedLanguages()
{
    return registry().order;
}

QString CodeHighlighter::displayNameFor(const QString &language)
{
    const QString canonical = normalizeLanguage(language);
    if (canonical.isEmpty()) {
        return QString();
    }
    return registry().display.value(canonical);
}

QString CodeHighlighter::normalizeLanguage(const QString &info)
{
    // 围栏信息串可能还带着别的词（```cpp title=x）：md4c 只会把第一个词放进 class，
    // 但为了稳妥，这里也只取第一个词。
    QString key = info.trimmed().toLower();
    const int spaceAt = key.indexOf(QRegularExpression(QStringLiteral("\\s")));
    if (spaceAt > 0) {
        key.truncate(spaceAt);
    }
    if (key.isEmpty()) {
        return QString();
    }

    // 明确表示"不要高亮"的写法
    if (key == QLatin1String("plaintext") || key == QLatin1String("text") || key == QLatin1String("txt")
        || key == QLatin1String("none") || key == QLatin1String("plain")) {
        return QString();
    }

    const Registry &reg = registry();
    const auto it = reg.canonical.constFind(key);
    return (it == reg.canonical.constEnd()) ? QString() : it.value();
}

bool CodeHighlighter::isSupported(const QString &language)
{
    return !normalizeLanguage(language).isEmpty();
}

QVector<CodeToken> CodeHighlighter::tokenize(const QString &code, const QString &language)
{
    if (code.isEmpty()) {
        return {};
    }

    const QString canonical = normalizeLanguage(language);
    if (canonical.isEmpty()) {
        return {};  // 纯文本或没见过的语言：不着色（不是错误）
    }

    const Registry &reg = registry();
    const auto it = reg.specs.constFind(canonical);
    if (it == reg.specs.constEnd()) {
        return {};
    }
    const LangSpec &spec = it.value();

    switch (spec.mode) {
    case CodeMode::Markup:
        return tokenizeMarkup(code);
    case CodeMode::Css:
        return tokenizeCss(code);
    case CodeMode::KeyValue:
        return tokenizeKeyValue(code, spec);
    case CodeMode::Diff:
        return tokenizeDiff(code);
    case CodeMode::Generic:
        break;
    }
    return tokenizeGeneric(code, spec);
}

QString CodeHighlighter::cssClassNameFor(CodeTokenKind kind)
{
    switch (kind) {
    case CodeTokenKind::Plain:
        return QString();
    case CodeTokenKind::Comment:
        return QStringLiteral("hljs-comment");
    case CodeTokenKind::Keyword:
        return QStringLiteral("hljs-keyword");
    case CodeTokenKind::Type:
        return QStringLiteral("hljs-type");
    case CodeTokenKind::Literal:
        return QStringLiteral("hljs-literal");
    case CodeTokenKind::Builtin:
        return QStringLiteral("hljs-built_in");
    case CodeTokenKind::String:
        return QStringLiteral("hljs-string");
    case CodeTokenKind::Number:
        return QStringLiteral("hljs-number");
    case CodeTokenKind::Function:
        return QStringLiteral("hljs-function");
    case CodeTokenKind::Attribute:
        return QStringLiteral("hljs-attr");
    case CodeTokenKind::Tag:
        return QStringLiteral("hljs-tag");
    case CodeTokenKind::Operator:
        return QStringLiteral("hljs-operator");
    case CodeTokenKind::Meta:
        return QStringLiteral("hljs-meta");
    case CodeTokenKind::DiffAdd:
        return QStringLiteral("hljs-diff-add");
    case CodeTokenKind::DiffDel:
        return QStringLiteral("hljs-diff-del");
    }
    return QString();
}

QString CodeHighlighter::escapeHtml(const QString &text)
{
    QString out = text;
    out.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    out.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    out.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    out.replace(QLatin1Char('"'), QStringLiteral("&quot;"));
    return out;
}

QString CodeHighlighter::unescapeHtml(const QString &text)
{
    // 一次扫完，认识常见的几种实体（md4c-html 输出的是 &amp; &lt; &gt; &quot;）
    QString out;
    out.reserve(text.size());

    int i = 0;
    const int n = text.size();
    while (i < n) {
        if (text.at(i) == QLatin1Char('&')) {
            const int semi = text.indexOf(QLatin1Char(';'), i);
            if (semi > 0 && semi - i <= 10) {
                const QString entity = text.mid(i + 1, semi - i - 1);
                if (entity == QLatin1String("amp")) {
                    out += QLatin1Char('&');
                } else if (entity == QLatin1String("lt")) {
                    out += QLatin1Char('<');
                } else if (entity == QLatin1String("gt")) {
                    out += QLatin1Char('>');
                } else if (entity == QLatin1String("quot")) {
                    out += QLatin1Char('"');
                } else if (entity == QLatin1String("apos") || entity == QLatin1String("#39")) {
                    out += QLatin1Char('\'');
                } else if (entity.startsWith(QLatin1Char('#'))) {
                    bool ok = false;
                    const uint code = entity.mid(1).toUInt(&ok, 10);
                    if (ok && code > 0) {
                        out += QChar(code);
                    } else {
                        out += text.mid(i, semi - i + 1);  // 不认识就原样留着
                    }
                } else {
                    out += text.mid(i, semi - i + 1);
                }
                i = semi + 1;
                continue;
            }
        }
        out += text.at(i);
        ++i;
    }
    return out;
}

QString CodeHighlighter::highlightToHtml(const QString &code, const QString &language)
{
    const QVector<CodeToken> tokens = tokenize(code, language);
    if (tokens.isEmpty()) {
        return escapeHtml(code);  // 不高亮：也要转义（安全底线）
    }

    QString out;
    out.reserve(code.size() * 3 / 2);
    int pos = 0;
    for (const CodeToken &token : tokens) {
        if (token.start > pos) {
            out += escapeHtml(code.mid(pos, token.start - pos));
        }
        out += QStringLiteral("<span class=\"%1\">%2</span>")
                   .arg(cssClassNameFor(token.kind), escapeHtml(code.mid(token.start, token.length)));
        pos = token.start + token.length;
    }
    if (pos < code.size()) {
        out += escapeHtml(code.mid(pos));
    }
    return out;
}

QString CodeHighlighter::highlightCodeBlocks(const QString &html)
{
    // md4c 的输出形状（实测）：<pre><code class="language-python">…</code></pre>
    // 代码里的 & < > " 已经被 md4c 转义过，所以这个正则不会被代码内容骗到。
    static const QRegularExpression blockRe(
        QStringLiteral("<pre><code class=\"language-([^\"]+)\">([\\s\\S]*?)</code></pre>"),
        QRegularExpression::DotMatchesEverythingOption);

    QString out;
    int last = 0;
    QRegularExpressionMatchIterator it = blockRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();

        const QString canonical = normalizeLanguage(match.captured(1));
        if (canonical.isEmpty()) {
            continue;  // 不认识的语言/纯文本：原样留着，一个字都不动
        }

        const QString code = unescapeHtml(match.captured(2));
        out += html.mid(last, match.capturedStart() - last);
        // 保留 language-xxx（别的工具还认它），另加 hljs 作为"已被我们着色"的标记
        out += QStringLiteral("<pre><code class=\"language-%1 hljs\">%2</code></pre>")
                   .arg(escapeHtml(canonical), highlightToHtml(code, canonical));
        last = match.capturedEnd();
    }
    out += html.mid(last);
    return out;
}

}  // namespace markdown_editor::core::document
