# 第三方库总入口：只放"原样引入、不改动"的外部源码。
#
# ---------------------------------------------------------------------------
# md4c 0.5.3  —— C 语言写的 CommonMark 解析器 + HTML 渲染器
#   上游:   https://github.com/mity/md4c
#   来源:   Debian 原始发布包 md4c_0.5.3.orig.tar.gz（即上游 release 快照）
#   许可:   MIT，见 md4c/LICENSE.md（保留原文件即为合规）
#   引入日期: 2026-09-10
#
# ★ 为什么不直接 add_subdirectory(md4c) 用上游的 CMakeLists：
#   1) 上游会把自己源码的 CRT 改成 /MT（静态运行库）—— 见 md4c/CMakeLists.txt 里对
#      CMAKE_C_FLAGS_* 的改写。而本工程（Qt）用的是 /MD(d)（动态运行库），
#      两种运行库混进同一个可执行文件会报 LNK2038 "RuntimeLibrary 不匹配"。
#   2) 上游还带 install/export 规则和 md2html 命令行工具，我们都不需要。
#   3) 自己列源文件符合本项目"显式列出源文件、不用 GLOB"的约定。
#   代价：升级 md4c 时如果上游新增/改名了源文件，这里要跟着改
#        （对照 md4c/src/CMakeLists.txt 看它到底编哪些文件）。
#
# ★ 本文件里的 .c 文件需要 C 语言已启用，见根 CMakeLists.txt 的
#   project(... LANGUAGES CXX C)。否则 CMake 会报
#   "CMake can not determine linker language for target"。
# ---------------------------------------------------------------------------

# ---- 解析器本体 ----
add_library(md4c STATIC
    md4c/src/md4c.c
    md4c/src/md4c.h
)

# PUBLIC：谁链接 md4c，谁就能 #include <md4c.h>
target_include_directories(md4c PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/md4c/src)

# Debug 构建里打开 md4c 自己的内部断言（上游也是这么做的）
target_compile_definitions(md4c PRIVATE $<$<CONFIG:Debug>:DEBUG>)

# ---- HTML 渲染器（依赖上面的解析器）----
add_library(md4c-html STATIC
    md4c/src/md4c-html.c
    md4c/src/md4c-html.h
    md4c/src/entity.c
    md4c/src/entity.h
)

target_include_directories(md4c-html PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/md4c/src)

# PUBLIC：链接 md4c-html 就自动连上 md4c
target_link_libraries(md4c-html PUBLIC md4c)

if(MSVC)
    # md4c 用了 sprintf/sscanf 这类"不安全"函数，MSVC 默认会刷警告，按上游做法关掉。
    # 用 PRIVATE：只影响第三方库自己的编译，不要污染本工程的警告设置。
    target_compile_definitions(md4c PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_definitions(md4c-html PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
