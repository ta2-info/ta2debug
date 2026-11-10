#pragma once

// 動作中プロセスの一覧。attach 先を選ぶための情報を返すだけで、アーキには依存しない
// （システムの libproc を使う。外部ライブラリは増やさない）。
#include <string>
#include <vector>

#include <sys/types.h>

namespace ta2 {

struct ProcessInfo {
  pid_t pid = 0;
  std::string name;        // 実行ファイルのファイル名部分
  std::string path;        // フルパス（取れなければ空）
  std::string args;        // 起動引数（argv[1..] を空白連結。取れなければ空）
  bool attachable = false; // task_for_pid が成功するか（= アタッチできるか）
};

// pid 昇順。取得できなかったプロセスは name/path が空のまま含む。
// attachable は task_for_pid を試して判定する（root 権限が前提。SIP 保護プロセス等は false）。
std::vector<ProcessInfo> listProcesses();

} // namespace ta2
