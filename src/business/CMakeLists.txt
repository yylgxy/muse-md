# business 层：业务逻辑。
# 源文件必须显式列出，不要用 file(GLOB_RECURSE)（见根 CMakeLists.txt 顶部说明）。
add_library(business STATIC
    business_placeholder.cpp   # TODO: 有真实源文件后删掉这一行
)

target_link_libraries(business core_document core_storage)