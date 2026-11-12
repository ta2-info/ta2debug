cmake_minimum_required(VERSION 3.20)

# macOS SDK（-isysroot）。別の Xcode / コマンドラインツール環境では SDK を自動発見できず
# ビルドが通らないことがあるので、未指定なら xcrun で SDK パスを解決して CMAKE_OSX_SYSROOT に
# 入れる（CMake がこれを -isysroot として全コンパイル・リンクに渡す）。
# 明示指定（-DCMAKE_OSX_SYSROOT=... や CMAKE_OSX_SYSROOT 環境変数）があればそれを尊重する。
# project() より前に設定する必要がある（コンパイラ判定で使われるため）。
if(APPLE AND NOT CMAKE_OSX_SYSROOT AND NOT DEFINED ENV{CMAKE_OSX_SYSROOT})
  find_program(XCRUN_EXECUTABLE xcrun)
  if(XCRUN_EXECUTABLE)
    execute_process(
      COMMAND ${XCRUN_EXECUTABLE} --sdk macosx --show-sdk-path
      OUTPUT_VARIABLE _ta2_sdk_path
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
      RESULT_VARIABLE _ta2_sdk_res)
    if(_ta2_sdk_res EQUAL 0 AND _ta2_sdk_path AND IS_DIRECTORY "${_ta2_sdk_path}")
      set(CMAKE_OSX_SYSROOT "${_ta2_sdk_path}" CACHE PATH "macOS SDK path passed as -isysroot")
      message(STATUS "macOS SDK (isysroot): ${CMAKE_OSX_SYSROOT}")
    endif()
  endif()
endif()

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
# Carbon: 起動時に IME を半角へ切り替える TIS 入力ソース API 用（システムフレームワーク）。
find_library(CARBON_LIBRARY Carbon)
if(NOT CAPSTONE_INCLUDE_DIR OR NOT CAPSTONE_LIBRARY)
  message(FATAL_ERROR "capstone が見つからない (brew install capstone)")
endif()

add_library(ta2opts INTERFACE)
target_compile_options(ta2opts INTERFACE -g -Wall)
# libdtrace（システムライブラリ。実体は dyld 共有キャッシュ、SDK の .tbd でリンク）。
# syscall をカーネルのディスパッチ表計測で検知する DtraceCatch 用。全ターゲットへ伝播。
target_link_libraries(ta2opts INTERFACE dtrace)
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
  src/core/Annotations.cpp
  src/core/DtraceCatch.cpp
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
target_link_libraries(ta2debug PRIVATE ta2opts ${CAPSTONE_LIBRARY} ${CARBON_LIBRARY})

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

# DtraceCatch（libdtrace 相乗りの syscall 検知）のセルフテスト（root が要る）
add_executable(dtrace_selftest
  src/selftest/dtrace_selftest.cpp
  ${CORE_SOURCES}
)
target_link_libraries(dtrace_selftest PRIVATE ta2opts ${CAPSTONE_LIBRARY})

# arm64 クロスコンパイル確認。実機は無いが、arm64 実装が SDK ヘッダに対して
# 壊れていないことを毎ビルドで検証する（コンパイルのみ、リンクはしない）。
# この custom_command はコンパイラを直接起動するため、本体ターゲットと違い CMake が
# -isysroot を自動では付けてくれない。SDK を自動発見できない環境（別 Xcode 等）では
# ここだけヘッダ not found で落ちるので、解決済みの CMAKE_OSX_SYSROOT を明示的に渡す。
set(_ta2_arm64_isysroot "")
if(CMAKE_OSX_SYSROOT)
  set(_ta2_arm64_isysroot -isysroot ${CMAKE_OSX_SYSROOT})
endif()
add_custom_command(
  OUTPUT ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o
  COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -arch arm64 ${_ta2_arm64_isysroot} -g -Wall
          -I${CMAKE_SOURCE_DIR}/inc -I${CAPSTONE_INCLUDE_DIR}
          -c ${CMAKE_SOURCE_DIR}/src/core/arch/ArchArm64.cpp
          -o ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o
  DEPENDS src/core/arch/ArchArm64.cpp inc/core/Arch.h
  COMMENT "arm64 cross-compile check: ArchArm64.cpp"
  VERBATIM
)
add_custom_target(arch_arm64_check ALL DEPENDS ${CMAKE_BINARY_DIR}/ArchArm64.arm64.o)
