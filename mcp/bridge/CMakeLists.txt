cmake_minimum_required(VERSION 3.16)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/version.cmake)

set(CMAKE_CXX_STANDARD 20)

project(
    tracy-mcp-bridge
    LANGUAGES C CXX
    VERSION ${TRACY_VERSION_STRING}
)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/config.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/vendor.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/server.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/GitRef.cmake)

set(PROGRAM_FILES
    src/bridge.cpp
    src/McpStreamableHttp.cpp
)

add_executable(${PROJECT_NAME} ${PROGRAM_FILES} ${COMMON_FILES} ${SERVER_FILES})
add_git_ref(${PROJECT_NAME})
target_include_directories( ${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_LIST_DIR}/third_party )
target_compile_definitions( ${PROJECT_NAME} PRIVATE TRACY_MCP_BRIDGE_STANDALONE=1 )
target_link_libraries( ${PROJECT_NAME} PRIVATE TracyServer nlohmann_json::nlohmann_json )
if( WIN32 )
    target_link_libraries( ${PROJECT_NAME} PRIVATE ws2_32 )
endif()
set_property(DIRECTORY ${CMAKE_CURRENT_LIST_DIR} PROPERTY VS_STARTUP_PROJECT ${PROJECT_NAME})

install(TARGETS ${PROJECT_NAME} DESTINATION ${CMAKE_INSTALL_BINDIR})
