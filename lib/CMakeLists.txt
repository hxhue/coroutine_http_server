add_library(coro STATIC src/status_message.cpp)
target_include_directories(coro
                           PRIVATE ${CMAKE_SOURCE_DIR}/extern/http_status_code)
target_include_directories(coro PUBLIC ${CMAKE_CURRENT_LIST_DIR}/include)
