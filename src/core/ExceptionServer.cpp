#include "core/ExceptionServer.h"

#include <mach/mach_error.h>

// MIG 生成のディスパッチャ。メッセージを catch_mach_exception_raise* へ振り分ける。
extern "C" boolean_t mach_exc_server(mach_msg_header_t* InHeadP, mach_msg_header_t* OutHeadP);

namespace ta2 {
namespace {
// 受信中の ExceptionServer（1 つだけ有効）。catch 関数から参照する。
ExceptionServer* g_active = nullptr;
} // namespace

ExceptionServer::~ExceptionServer() {
  uninstall();
}

bool ExceptionServer::install(task_t task, Handler handler, std::string& err) {
  if (running_) { err = "すでに install 済み"; return false; }

  task_ = task;
  handler_ = std::move(handler);

  kern_return_t kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port_);
  if (kr != KERN_SUCCESS) { err = std::string("mach_port_allocate: ") + mach_error_string(kr); return false; }
  kr = mach_port_insert_right(mach_task_self(), port_, port_, MACH_MSG_TYPE_MAKE_SEND);
  if (kr != KERN_SUCCESS) { err = std::string("mach_port_insert_right: ") + mach_error_string(kr); return false; }

  // 現在の例外ポートを保存して差し替える。
  //   BREAKPOINT … ソフト/ハードブレイク・単一ステップ
  //   BAD_ACCESS … 不正アドレス参照（segfault 相当）
  //   BAD_INSTRUCTION … 不正・特権命令
  //   ARITHMETIC … 0 除算など算術例外
  savedCount_ = 32;
  const exception_mask_t mask = EXC_MASK_BREAKPOINT | EXC_MASK_BAD_ACCESS |
                                EXC_MASK_BAD_INSTRUCTION | EXC_MASK_ARITHMETIC;
  kr = task_swap_exception_ports(
      task_, mask, port_,
      (exception_behavior_t)(EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES), THREAD_STATE_NONE,
      savedMasks_, &savedCount_, savedPorts_, savedBehaviors_, savedFlavors_);
  if (kr != KERN_SUCCESS) { err = std::string("task_swap_exception_ports: ") + mach_error_string(kr); return false; }

  g_active = this;
  running_ = true;
  thread_ = std::thread([this] {
    while (running_) {
      // 1 メッセージずつ処理（uninstall でダミーを送って抜けさせる）
      mach_msg_server_once(mach_exc_server, 4096, port_, MACH_MSG_OPTION_NONE);
    }
  });
  return true;
}

void ExceptionServer::uninstall() {
  if (!running_ && port_ == MACH_PORT_NULL) return;

  running_ = false;

  // 例外ポートを元へ戻す（これ以降の例外は本来のハンドラへ）
  restoreOriginalPorts();

  // 受信スレッドを mach_msg_server_once から抜けさせるためダミーメッセージを送る
  if (port_ != MACH_PORT_NULL) {
    mach_msg_header_t msg = {};
    msg.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    msg.msgh_size = sizeof(msg);
    msg.msgh_remote_port = port_;
    msg.msgh_local_port = MACH_PORT_NULL;
    msg.msgh_id = 0;
    mach_msg(&msg, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(msg), 0,
             MACH_PORT_NULL, 100 /*ms*/, MACH_PORT_NULL);
  }

  if (thread_.joinable()) thread_.join();

  if (port_ != MACH_PORT_NULL) {
    mach_port_mod_refs(mach_task_self(), port_, MACH_PORT_RIGHT_RECEIVE, -1);
    port_ = MACH_PORT_NULL;
  }
  if (g_active == this) g_active = nullptr;
  task_ = 0;
  savedCount_ = 0;
}

void ExceptionServer::restoreOriginalPorts() {
  if (task_ == 0) return;
  for (mach_msg_type_number_t i = 0; i < savedCount_; i++) {
    task_set_exception_ports(task_, savedMasks_[i], savedPorts_[i],
                             savedBehaviors_[i], savedFlavors_[i]);
  }
}

kern_return_t ExceptionServer::dispatch(thread_act_t thread, exception_type_t exception,
                                        const int64_t* codes, int codeCount) {
  if (handler_) return handler_(thread, exception, codes, codeCount);
  return KERN_FAILURE; // ハンドラ未設定 → 本来の処理に委ねる
}

// --- MIG が呼ぶ catch 関数（C リンケージ） ---
// EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES なので raise が呼ばれる。state 付き 2 種は未使用。
extern "C" kern_return_t catch_mach_exception_raise(
    mach_port_t /*exception_port*/, mach_port_t thread, mach_port_t /*task*/,
    exception_type_t exception, mach_exception_data_t code, mach_msg_type_number_t codeCnt) {
  if (g_active) return g_active->dispatch(thread, exception, code, (int)codeCnt);
  return KERN_FAILURE;
}

extern "C" kern_return_t catch_mach_exception_raise_state(
    mach_port_t, exception_type_t, const mach_exception_data_t, mach_msg_type_number_t,
    int*, const thread_state_t, mach_msg_type_number_t, thread_state_t, mach_msg_type_number_t*) {
  return MACH_RCV_INVALID_TYPE; // 使わない
}

extern "C" kern_return_t catch_mach_exception_raise_state_identity(
    mach_port_t, mach_port_t, mach_port_t, exception_type_t, mach_exception_data_t,
    mach_msg_type_number_t, int*, thread_state_t, mach_msg_type_number_t,
    thread_state_t, mach_msg_type_number_t*) {
  return MACH_RCV_INVALID_TYPE; // 使わない
}

} // namespace ta2
