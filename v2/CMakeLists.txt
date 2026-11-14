# ta2debug v2（リファクタリング後の新ツリー）。設計書は design/REFACTORING.md。
# ルート CMakeLists.txt から option(TA2_BUILD_V2) で add_subdirectory される。
# v1（ルートの src/ inc/）とは混ぜない: v1 のターゲット・include パス（ta2opts）は使わない。
#
# いまあるのは最下層（v1 から実ロジックを移植済み）だけ:
#   ta2base     … 横断値型（Msg / MsgId）。何にも依存しない
#   ta2platform … L0 Platform/Arch（Arch / Target / ExceptionServer / DtraceCatch / ProcessList / MIG）
# 層の依存方向はライブラリ分割で強制する（下位は上位のヘッダを include パスに持たない）。

# v2 共通のコンパイル設定（v1 の ta2opts 相当。ただし inc/ と capstone は入れない）。
add_library(ta2v2opts INTERFACE)
target_compile_options(ta2v2opts INTERFACE -g -Wall)
if(USE_ASAN)
  target_compile_options(ta2v2opts INTERFACE -fsanitize=address -fno-omit-frame-pointer)
  target_link_options(ta2v2opts INTERFACE -fsanitize=address)
endif()

# ---- ta2base ----
set(TA2BASE_SOURCES
  base/src/Msg.cpp
)
add_library(ta2base STATIC ${TA2BASE_SOURCES})
target_include_directories(ta2base PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/base/include)
target_link_libraries(ta2base PRIVATE ta2v2opts)

# ---- ta2platform ----
# Arch 実装は両方を列挙する（各ファイルは自アーキの #if でガードされ、非ホスト側は空の翻訳単位）。
set(TA2PLATFORM_CXX_SOURCES
  platform/src/Target.cpp
  platform/src/ExceptionServer.cpp
  platform/src/DtraceCatch.cpp
  platform/src/ProcessList.cpp
  platform/src/arch/ArchX86_64.cpp
  platform/src/arch/ArchArm64.cpp
)
# MIG 生成の mach_exc サーバ。生成物（bootstrap_cmds 由来・手を入れない）なので複製せず、
# いまは v1 と同じファイルを参照する。v1 を畳む工程で platform/src/ へ git mv する。
set(TA2PLATFORM_MIG_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/core/mach_excServer.c
)
add_library(ta2platform STATIC ${TA2PLATFORM_CXX_SOURCES} ${TA2PLATFORM_MIG_SOURCES})
target_include_directories(ta2platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/platform/include)
# libdtrace はシステムライブラリ（DtraceCatch 用）。利用側のリンクへ伝播させる。
target_link_libraries(ta2platform PUBLIC ta2base dtrace PRIVATE ta2v2opts)

# ---- 非ホストアーキのコンパイル検査（リンクなし。設計書 §9.3） ----
# x86_64 ホストでは -arch arm64、arm64 ホストでは -arch x86_64 で ta2base / ta2platform の
# C++ ソースをすべてコンパイルし、非ホスト側の Arch 実装が SDK ヘッダに対して壊れていない
# ことを毎ビルドで確かめる。コンパイラを直接起動するので -isysroot を明示的に渡す。
if(CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL "arm64")
  set(_ta2v2_cross_arch x86_64)
else()
  set(_ta2v2_cross_arch arm64)
endif()
set(_ta2v2_isysroot "")
if(CMAKE_OSX_SYSROOT)
  set(_ta2v2_isysroot -isysroot ${CMAKE_OSX_SYSROOT})
endif()
file(GLOB_RECURSE _ta2v2_headers CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/base/include/*
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/include/*)
set(_ta2v2_cross_objs "")
foreach(_src ${TA2BASE_SOURCES} ${TA2PLATFORM_CXX_SOURCES})
  string(REPLACE "/" "_" _obj ${_src})
  set(_obj ${CMAKE_CURRENT_BINARY_DIR}/cross_${_ta2v2_cross_arch}/${_obj}.o)
  add_custom_command(
    OUTPUT ${_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/cross_${_ta2v2_cross_arch}
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -arch ${_ta2v2_cross_arch} ${_ta2v2_isysroot} -g -Wall
            -I${CMAKE_CURRENT_SOURCE_DIR}/base/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/platform/include
            -c ${CMAKE_CURRENT_SOURCE_DIR}/${_src} -o ${_obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_src} ${_ta2v2_headers}
    COMMENT "v2 ${_ta2v2_cross_arch} cross-compile check: ${_src}"
    VERBATIM
  )
  list(APPEND _ta2v2_cross_objs ${_obj})
endforeach()
add_custom_target(ta2v2_cross_check ALL DEPENDS ${_ta2v2_cross_objs})
