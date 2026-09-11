#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

// 配置管理（全部为静态函数，不需要实例化）。
// 以后实现时内部复用 FileUtils 做读写，不要自己再写一套文件 IO。
class ConfigManager
{
public:
    // 以后要加的配置读写函数写在这里，例如 load / save / value / setValue
private:
    ConfigManager() = delete; // 工具类不允许实例化：所有函数都是静态的，造对象没有意义
};

#endif // CONFIGMANAGER_H
