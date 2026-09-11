# 测试目录。
#
# 跑法：在构建目录里执行
#     ctest -C Debug --output-on-failure
# 或者直接运行 bin/Debug/test_xxx.exe（退出码 0 = 全部通过）。
#
# 注意：新增测试文件时同样要显式列进 add_executable（不要用 file(GLOB)，
#       理由见根 CMakeLists.txt 顶部说明）。

# ---------------- FileUtils 契约测试 ----------------
add_executable(test_fileutils
    test_fileutils.cpp
)
target_link_libraries(test_fileutils
    infrastructure
    Qt6::Core
)
add_test(NAME fileutils COMMAND test_fileutils)

# ---------------- MarkdownParser 契约测试 ----------------
# 这个测试只链接 core_document，测试代码里没有一行 md4c 代码 ——
# md4c 是 core_document 的实现细节（CMake 里 PRIVATE 链接），拿不到它的头文件，
# 这正是分层想要的效果。
add_executable(test_markdownparser
    test_markdownparser.cpp
)
target_link_libraries(test_markdownparser
    core_document
    Qt6::Core
)
add_test(NAME markdownparser COMMAND test_markdownparser)

# Windows 上测试程序要加载 Qt 的 DLL，而 ctest 是在裸环境里启动程序的，
# 不会自动带上 Qt 的 bin 目录 —— 不设置这里会直接以 0xC0000135（找不到 DLL）失败。
# path_list_prepend 会按平台分隔符把目录加到 PATH 前面；
# 不要手写 "PATH=$<TARGET_FILE_DIR:Qt6::Core>;$ENV{PATH}"，那样分号会被当成列表分隔符。
set_tests_properties(fileutils markdownparser PROPERTIES
    ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
)
