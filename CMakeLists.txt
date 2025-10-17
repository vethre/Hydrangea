cmake_minimum_required(VERSION 3.21)
project(Hydrangea C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(cjson STATIC third_party/cjson/cJSON.c)

# Де шукати пакети
list(APPEND CMAKE_PREFIX_PATH
  "C:/SDL2-2.30.9/x86_64-w64-mingw32"
  "C:/SDL2_image-2.8.2/x86_64-w64-mingw32"
  "C:/SDL2_ttf-2.22.0/x86_64-w64-mingw32"
  "C:/SDL2_mixer-2.8.1/x86_64-w64-mingw32"
)

find_package(SDL2 REQUIRED CONFIG)
find_package(SDL2_image REQUIRED CONFIG)
find_package(SDL2_ttf REQUIRED CONFIG)
find_package(SDL2_mixer REQUIRED CONFIG)

add_executable(hydrangea
  src/main.c
  src/game.c
)

target_include_directories(hydrangea PRIVATE
  "C:/SDL2-2.30.9/x86_64-w64-mingw32/include"
  "C:/SDL2_image-2.8.2/x86_64-w64-mingw32/include"
  "C:/SDL2_ttf-2.22.0/x86_64-w64-mingw32/include"
  "C:/SDL2_mixer-2.8.1/x86_64-w64-mingw32/include"
)

target_include_directories(cjson PUBLIC third_party/cjson)

target_link_libraries(hydrangea PRIVATE
  SDL2::SDL2main
  SDL2::SDL2
  SDL2_image::SDL2_image
  SDL2_ttf::SDL2_ttf
  SDL2_mixer::SDL2_mixer
)

target_link_libraries(hydrangea PRIVATE cjson)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(hydrangea PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Скопіювати потрібні DLL поруч із exe
if (WIN32)
  add_custom_command(TARGET hydrangea POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "$<TARGET_FILE:SDL2::SDL2>"
      "$<TARGET_FILE:SDL2_image::SDL2_image>"
      "$<TARGET_FILE:SDL2_ttf::SDL2_ttf>"
      "$<TARGET_FILE:SDL2_mixer::SDL2_mixer>"
      "$<TARGET_FILE_DIR:hydrangea>"
  )
  # Копіюємо всю теку assets поруч із exe
  add_custom_command(TARGET hydrangea POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:hydrangea>/assets"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_FILE_DIR:hydrangea>/assets"
  )

endif()
