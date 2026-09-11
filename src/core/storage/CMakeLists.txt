# core/storage：持久化层。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(core_storage STATIC
    storage_placeholder.cpp   # TODO: 有真实源文件后删掉这一行
)

target_link_libraries(core_storage infrastructure Qt6::Core Qt6::Sql)