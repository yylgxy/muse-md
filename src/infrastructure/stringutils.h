#ifndef STRINGUTILS_H
#define STRINGUTILS_H

// 字符串处理工具（全部为静态函数，不需要实例化）。
class StringUtils
{
public:
    // 以后要加的字符串处理函数写在这里，例如 trim / 大小写转换 / 编码探测
private:
    StringUtils() = delete; // 工具类不允许实例化：所有函数都是静态的，造对象没有意义
};

#endif // STRINGUTILS_H
