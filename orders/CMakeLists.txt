# Сборка сервиса Task 1 и небольшого набора проверок на C++17.
cmake_minimum_required(VERSION 3.16)
project(soa_orders LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Для работы с базой нужны заголовки и библиотека libpq, для работников — поддержка потоков.
find_package(PostgreSQL REQUIRED)
find_package(Threads REQUIRED)
# Три собственных исходника собираются в одну программу; HTTP и JSON подключаются заголовками.
add_executable(orders src/main.cpp src/database.cpp src/service.cpp)
target_include_directories(orders PRIVATE vendor src)
target_link_libraries(orders PRIVATE PostgreSQL::PostgreSQL Threads::Threads)
# На Windows дополнительно подключаем системные библиотеки сети и криптографии.
if(WIN32)
  target_link_libraries(orders PRIVATE ws2_32 crypt32)
  target_compile_definitions(orders PRIVATE _WIN32_WINNT=0x0A00)
endif()
# Включаем предупреждения компилятора; для MSVC также явно задаём UTF-8.
if(MSVC)
  target_compile_options(orders PRIVATE /W4 /utf-8)
else()
  target_compile_options(orders PRIVATE -Wall -Wextra -Wpedantic)
endif()
# Регистрируем отдельный тест, который можно запустить командой CTest.
enable_testing()
add_executable(core_tests tests/core_tests.cpp)
target_include_directories(core_tests PRIVATE src vendor)
target_link_libraries(core_tests PRIVATE Threads::Threads)
add_test(NAME core_tests COMMAND core_tests)
