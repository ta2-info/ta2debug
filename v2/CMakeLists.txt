# ta2debug v2（リファクタリング後の新ツリー）。設計書は design/REFACTORING.md。
# ルート CMakeLists.txt から option(TA2_BUILD_V2) で add_subdirectory される。
# v1（ルートの src/ inc/）とは混ぜない: v1 のターゲット・include パス（ta2opts）は使わない。
#
# いまあるのは L0〜L2（v1 から実ロジックを移植済み）:
#   ta2base     … 横断値型（Msg / MsgId）。何にも依存しない
#   ta2platform … L0 Platform/Arch（Arch / Target / ExceptionServer / DtraceCatch / ProcessList / MIG）
#   ta2core     … L1 Core Engine（Breakpoints / ImageList / Symbols / Annotations / BreakpointStore /
#                 Disassembler / Unwinder / DwarfLines / SyscallTables / SyscallDecoder / Expression /
#                 Backtracer / BranchResolver / MemoryExaminer / SymbolRef / Stepper / DyldNotifier /
#                 SyscallMonitor / SymbolWarmer / ExitMonitor / TargetSession）
#   ta2engine   … L2 Facade/API（Debugger / Command / DebugEvent / EngineInbox / StopProcessor /
#                 BreakpointService / AutoRunner / JobRunner / CommandInterpreter）
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

# ---- ta2core ----
# L1 Core Engine。ta2platform / ta2base の上に積む。capstone を使うのはこの層から
# （Disassembler.cpp だけが capstone ヘッダを include する。公開ヘッダには出さない）。
# ui/ ・ engine/ のヘッダは include パスに入れない（＝依存できない）。
set(TA2CORE_SOURCES
  core/src/Breakpoints.cpp
  core/src/ImageList.cpp
  core/src/Symbols.cpp
  core/src/Annotations.cpp
  core/src/BreakpointStore.cpp
  core/src/Disassembler.cpp
  core/src/Unwinder.cpp
  core/src/DwarfLines.cpp
  core/src/DwarfTypes.cpp
  core/src/SymbolRef.cpp
  core/src/SyscallTables.cpp
  core/src/SyscallDecoder.cpp
  core/src/Expression.cpp
  core/src/Backtracer.cpp
  core/src/BranchResolver.cpp
  core/src/MemoryExaminer.cpp
  core/src/DyldNotifier.cpp
  core/src/ExitMonitor.cpp
  core/src/SymbolWarmer.cpp
  core/src/Stepper.cpp
  core/src/SyscallMonitor.cpp
  core/src/TargetSession.cpp
)
add_library(ta2core STATIC ${TA2CORE_SOURCES})
target_include_directories(ta2core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core/include
                                   PRIVATE ${CAPSTONE_INCLUDE_DIR})
# capstone は利用側のリンクへ伝播させる（静的ライブラリなのでリンクは最終実行ファイルで起きる）。
target_link_libraries(ta2core PUBLIC ta2platform ta2base ${CAPSTONE_LIBRARY} PRIVATE ta2v2opts)

# ---- ta2engine ----
# L2 Facade/API（ヘッドレスなデバッガ本体）。ta2core / ta2platform / ta2base の上に積む。
# ui/ のヘッダは include パスに入れない（＝UI・端末・言語カタログに依存できない）。capstone / dtrace の
# ヘッダも見えない（リンクだけ下位から伝播する）。
set(TA2ENGINE_SOURCES
  engine/src/EngineInbox.cpp
  engine/src/AutoRunner.cpp
  engine/src/BreakpointService.cpp
  engine/src/JobRunner.cpp
  engine/src/StopProcessor.cpp
  engine/src/Debugger.cpp
  engine/src/DebuggerCommands.cpp
  engine/src/DebuggerQueries.cpp
  engine/src/CommandInterpreter.cpp
)
add_library(ta2engine STATIC ${TA2ENGINE_SOURCES})
target_include_directories(ta2engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/engine/include)
target_link_libraries(ta2engine PUBLIC ta2core ta2platform ta2base PRIVATE ta2v2opts)

# ---- 非ホストアーキのコンパイル検査（リンクなし。設計書 §9.3） ----
# x86_64 ホストでは -arch arm64、arm64 ホストでは -arch x86_64 で ta2base / ta2platform / ta2core の
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
  ${CMAKE_CURRENT_SOURCE_DIR}/platform/include/*
  ${CMAKE_CURRENT_SOURCE_DIR}/core/include/*
  ${CMAKE_CURRENT_SOURCE_DIR}/core/src/*.inc
  ${CMAKE_CURRENT_SOURCE_DIR}/engine/include/*)
set(_ta2v2_cross_objs "")
foreach(_src ${TA2BASE_SOURCES} ${TA2PLATFORM_CXX_SOURCES} ${TA2CORE_SOURCES} ${TA2ENGINE_SOURCES})
  string(REPLACE "/" "_" _obj ${_src})
  set(_obj ${CMAKE_CURRENT_BINARY_DIR}/cross_${_ta2v2_cross_arch}/${_obj}.o)
  add_custom_command(
    OUTPUT ${_obj}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_CURRENT_BINARY_DIR}/cross_${_ta2v2_cross_arch}
    COMMAND ${CMAKE_CXX_COMPILER} -std=c++17 -arch ${_ta2v2_cross_arch} ${_ta2v2_isysroot} -g -Wall
            -I${CMAKE_CURRENT_SOURCE_DIR}/base/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/platform/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/core/include
            -I${CMAKE_CURRENT_SOURCE_DIR}/engine/include
            -I${CAPSTONE_INCLUDE_DIR}
            -c ${CMAKE_CURRENT_SOURCE_DIR}/${_src} -o ${_obj}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_src} ${_ta2v2_headers}
    COMMENT "v2 ${_ta2v2_cross_arch} cross-compile check: ${_src}"
    VERBATIM
  )
  list(APPEND _ta2v2_cross_objs ${_obj})
endforeach()
add_custom_target(ta2v2_cross_check ALL DEPENDS ${_ta2v2_cross_objs})
