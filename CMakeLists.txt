cmake_minimum_required(VERSION 3.20)

# ta2debug（macOS TUI デバッガ）。設計書は DESIGN.md。
#   cmake -S . -B build -G Ninja
#   ninja -C build
# いまは Arch 抽象の土台のみ（arch_selftest）。モジュールを足すたびに拡張していく。
project(ta2debug LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})

option(USE_ASAN "Build with AddressSanitizer" ON)

# 依存: capstone が唯一の外部ライブラリ（x86_64/arm64 両対応なので arm64 化でも増えない）。
# md5/sha1 は OpenSSL をやめ CommonCrypto（システム）を使う。JSON は同梱 picojson。
find_path(CAPSTONE_INCLUDE_DIR capstone/capstone.h HINTS /usr/local/include /opt/homebrew/include)
find_library(CAPSTONE_LIBRARY capstone HINTS /usr/local/lib /opt/homebrew/lib)
if(NOT CAPSTONE_INCLUDE_DIR OR NOT CAPSTONE_LIBRARY)
  message(FATAL_ERROR "capstone が見つからない (brew install capstone)")
endif()

add_library(ta2opts INTERFACE)
target_compile_options(ta2opts INTERFACE -g -Wall)
target_include_directories(ta2opts INTERFACE ${CMAKE_SOURCE_DIR}/inc ${CAPSTONE_INCLUDE_DIR})
if(USE_ASAN)
  target_compile_options(ta2opts INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(ta2opts INTERFACE -fsanitize=address)
endif()

# Model 層。Arch 実装は両方を列挙してよい（各ファイルは自アーキの #if でガードされ、
# 非ホストのファイルは空の翻訳単位になる）。ホストアーキの実装だけがシンボルを出す。
set(CORE_SOURCES
  src/core/Target.cpp
  src/core/Breakpoints.cpp
  src/core/Disassembler.cpp
  src/core/ExceptionServer.cpp
  src/core/ImageList.cpp
  src/core/Unwind.cpp
  src/core/Symbols.cpp
  src/core/DwarfLines.cpp
  src/core/ProcessList.cpp
  src/core/Session.cpp
  src/core/mach_excServer.c
  src/core/arch/ArchX86_64.cpp
  src/core/arch/ArchArm64.cpp
)

set(UI_SOURCES
  src/ui/Terminal.cpp
  src/ui/Layout.cpp
)

set(APP_SOURCES
  src/app/Expression.cpp
)

# 本体（対話 TUI）
add_executable(ta2debug
  src/app/main.cpp
  ${CORE_SOURCES}
  ${UI_SOURCES}
  ${APP_SOURCES}
)
target_link_libraries(ta2debug PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# いまはセルフテストを作る（UI/コントローラは今後追加）。
add_executable(arch_selftest
  src/selftest/arch_selftest.cpp
  ${CORE_SOURCES}
)
target_link_libraries(arch_selftest PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# Target のセルフテスト（task_for_pid のため実行には root が要る）
add_executable(target_selftest
  src/selftest/target_selftest.cpp
  ${CORE_SOURCES}
)
target_link_libraries(target_selftest PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# エンドツーエンド（ブレイク → reg/dis）のセルフテスト（root が要る）
add_executable(break_selftest
  src/selftest/break_selftest.cpp
  ${CORE_SOURCES}
)
target_link_libraries(break_selftest PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# ImageList / Symbols のセルフテスト（root が要る）
add_executable(symbols_selftest
  src/selftest/symbols_selftest.cpp
  ${CORE_SOURCES}
)
target_link_libraries(symbols_selftest PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# arm64 クロスコンパイル確認。実機は無いが、arm64 実装が SDK ヘッダに対して
# 壊れていないことを毎ビルドで検証する（コンパイルのみ、リンクはしない）。
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -arch arm64 -g -Wall
          -I${CMAKE_SOURCE_DIR}/inc -I${CAPSTONE_INCLUDE_DIR}
          -c ${CMAKE_SOURCE_DIR}/src/core/arch/ArchArm64.cpp
          -o ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o
  DEPENDS src/core/arch/ArchArm64.cpp inc/core/Arch.h
  COMMENT "arm64 cross-compile check: ArchArm64.cpp"
  VERBATIM
)
add_custom_target(arch_arm64_check ALL DEPENDS ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o)
