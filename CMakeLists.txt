cmake_minimum_required(VERSION 3.16)

# ── 分层 sdkconfig defaults ────────────────────────────────────────────
# sdkconfig.defaults 入库，只放可公开的构建配置。
# sdkconfig.defaults.local 不入库（.gitignore），放本机/本设备私有配置：
# Wi-Fi 凭证、服务端地址与密码 —— doc-conventions.md 禁止凭证与内部端点入仓。
# 文件不存在时（CI、新 clone）自动跳过，构建不受影响，只是设备连不上网。
#
# 显式设置的 SDKCONFIG_DEFAULTS（环境变量，tools/validate.sh --firmware
# 和 CI 就是这么传的）仍然优先；这里只负责在未指定时补上本仓的 defaults，
# 以及在文件存在时追加私有层。
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    if(DEFINED ENV{SDKCONFIG_DEFAULTS})
        set(SDKCONFIG_DEFAULTS "$ENV{SDKCONFIG_DEFAULTS}")
    else()
        set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
    endif()
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(mrrc-terminal)
