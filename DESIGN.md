# ta2debug 設計書

macOS 向けの TUI デバッガ。`DEBUGGER/` の実装をゼロから作り直したもの。
機能は踏襲しつつ、肥大化していた `Debugger` クラスを責務ごとに分割し、
「モデル（デバッグ機構）」「ビュー（描画）」「コントローラ（入力・コマンド）」を明確に分ける。

---

## 1. 目的と前提

### 大前提（この 2 点は他のすべてに優先する）

- **A. アーキテクチャ移植性**: 現在は x86_64 のみ対応だが、いずれ Apple シリコン（arm64）にも対応する。
  その際は逆アセンブラ・レジスタ・デバッグ API（スレッド状態 / ブレイク / ステップ）がすべて変わる。
  そこで**アーキ依存はすべて `Arch` 抽象の裏に隔離する**（§4）。アーキ非依存のコード（コマンド・画面・式・
  バックトレース等）には、レジスタ名や命令バイト（INT3 など）を直書きしない。
- **B. 依存最小**: 外部ライブラリを増やさない。可能なら減らす。詳細は §11。
  - **capstone** が唯一の外部ライブラリ。x86_64 と arm64 の両方を扱えるので、arm64 対応でも増えない。
  - md5/sha1 は OpenSSL をやめ、システムの **CommonCrypto**（libSystem 同梱）を使う → 外部ライブラリを 1 つ削減。
  - JSON は同梱の **picojson**（ヘッダ 1 つ、インストール不要）のみ。
  - それ以外はシステムフレームワーク / libSystem だけに依存する。各外部部品は 1 モジュールの裏に隔離する。

### その他の前提

- **用途**: 自分でビルドしたプログラムの動作解析・学習用のデバッガ。lldb に近い操作感。
- **アタッチ方式**: lldb（ptrace / debugserver）と違い、`task_for_pid()` でタスクポートを取り、
  Mach 例外ポートを差し替えて例外を受け取る。**対象プロセスはアタッチ後も動き続ける**（停止させない）。
- **対象**: SIP 無効環境で root 実行。
- **扱う対象は自作プログラムに限る**。システムプロセスや、保護を回避する目的の対象は扱わない。

### 旧版からの引き継ぎ（重要な設計判断・バグ修正）

作り直しでも必ず維持する。詳細は「§7 主要な設計判断」。

1. コードページへの書き込みは `mach_vm_protect(R|W|VM_PROT_COPY)` → 書き戻し。
   保護属性は `mach_vm_region_recurse` で**実ページ**の値を取る（共有キャッシュのサブマップ対策）。
2. `dlopen` 検知: `dyld_all_image_infos.infoArrayChangeTimestamp` を見て一覧を読み直す。
3. `Ctrl+C`: 実行中はブレイク、`auto` 中は停止、ブレイク中は無視。全スレッドにトラップフラグ。
4. シグナル（TERM/HUP/INT/QUIT）受信時はデタッチしてから終了（対象を道連れにしない）。
5. 対象終了検知は kqueue（Mach 例外ポートに通常終了は届かない）。
6. `auto` 中の画面更新は 0.2 秒に 1 回へ間引く。

---

## 2. 全体アーキテクチャ

3 層に分ける。層をまたぐ依存は上から下への一方向のみ。

```
┌─────────────────────────────────────────────────────────┐
│ Controller 層   入力とコマンドのディスパッチ               │
│   App(メインループ/イベントキュー) Screen(画面状態) Command │
├─────────────────────────────────────────────────────────┤
│ View 層         モデルを読んで描画するだけ(状態を持たない) │
│   DisassemblyView RegisterView BacktraceView ThreadsView   │
│   各種リスト画面の描画 / Layout(領域計算) / Terminal        │
├─────────────────────────────────────────────────────────┤
│ Model 層        デバッグ機構。UI を一切知らない            │
│   Target ExceptionServer Breakpoints Stepper ImageList     │
│   Symbols Disassembler  ── すべて Arch 抽象に依存 ──        │
└─────────────────────────────────────────────────────────┘
          │
          └─ Arch 抽象（ArchX86_64 / 将来 ArchArm64）
```

旧版との最大の違い: 旧 `Debugger` は「アタッチ」「例外処理」「メモリ」「シンボル」
「逆アセンブル」「画面描画」まで全部持っていた（約 2700 行の神クラス）。
ta2debug では描画をモデルから完全に追い出し、モデルは「状態を問い合わせる API」だけを提供する。
ビューがその API を呼んで描く。さらにアーキ依存を `Arch` に隔離する。

---

## 3. モジュール一覧と責務

### Model 層（`src/core`, `inc/core`）

| モジュール | 責務 |
|---|---|
| `Target` | `task_for_pid` でのアタッチ/デタッチ、suspend/resume、メモリ R/W、スレッド列挙、レジスタ取得/設定。スレッド状態の flavor は `Arch` に委ねる。タスクポートの所有者。 |
| `ExceptionServer` | Mach 例外ポートの差し替え・復元、`mach_msg_server` を回す専用スレッド、例外の受信。受け取った例外を `StopEvent` にして `Target` 経由で通知。MIG 生成の `mach_exc*` はここだけが使う。 |
| `Breakpoints` | ソフトブレイク（元バイトの保存/復元）とハードウェアブレイクの管理・ヒット判定。命令バイトとハードウェアブレイクの設定方法は `Arch` から得る。 |
| `Stepper` | 単一ステップの制御、`step`/`next`（call は次命令に一時ブレイク）/`auto`（call/ret/syscall で停止、`autoskip` 対応）。ステップの仕組みは `Arch` に委ねる。 |
| `ImageList` | dyld イメージ一覧の取得、Mach-O ロードコマンド解析（LC_SEGMENT_64/SYMTAB/UUID/DYLD_INFO）、`dlopen` 検知による再読込、アドレス→ライブラリ/セグメント解決、VM リージョン列挙。 |
| `Symbols` | ライブラリごとのシンボル収集（JSON + LC_SYMTAB + スタブ）、C++ デマングル、アドレス↔名前解決、UUID キーのキャッシュ。 |
| `Disassembler` | capstone のラッパ。1 命令デコード、call/分岐先アドレス抽出。アーキに応じて capstone のモードを切り替える。 |
| `Arch` | **アーキ依存の一切を隠す抽象（§4）**。`ArchX86_64` が今の実装、将来 `ArchArm64` を足す。 |
| `TargetExitMonitor` | kqueue(EVFILT_PROC/NOTE_EXIT) で対象終了を監視し通知。 |

`Target` が中心で、他のモデルは `Target`（タスクポート + メモリ R/W）と `Arch` を参照する。
`Target` は「状態が変わった」ことを **観測者（`StopObserver`）** に通知するだけで、UI は知らない。

### View 層（`src/ui`, `inc/ui`）

| モジュール | 責務 |
|---|---|
| `Terminal` | raw モード、代替画面、カーソル、`(col,row)` への文字列出力、行クリア。 |
| `Layout` | ブレイク画面の 5 レイアウト計算、表示幅計算、幅に合わせた切り詰め（`…`）。 |
| `PaneView`（基底） | 与えられた矩形に見出し＋行を描く。はみ出しは切り、余りは空白で消す。 |
| `DisassemblyView` ほか | `RegisterView` / `BacktraceView` / `ThreadsView`。モデルを読んで各領域を描く。レジスタ名やフレーム巻き戻しの規約は `Arch` から得る（直書きしない）。 |
| `ListView`（基底） | 選択・スクロール付きの一覧描画（プロセス/スレッド/ライブラリ/シンボル/リージョン で再利用）。 |
| `StatusLine` | 最下行の状態表示（pid/state/レイアウト/非表示/停止待ち）。 |

### Controller 層（`src/app`, `inc/app`）

| モジュール | 責務 |
|---|---|
| `App` | エントリ。イベントキュー（`EventQueue`）のメインループ、画面(`Screen`)の切り替え、シグナル待ちスレッド。 |
| `EventQueue` | スレッド間イベント受け渡し（条件変数）。キー入力・モデル通知を直列化してメインスレッドで処理。 |
| `InputThread` | STDIN を読み、`KeyMapper` でキーに変換して `EventQueue` へ。 |
| `KeyMapper` | バイト列 → キーコード + 修飾。 |
| `Screen`（基底） | 画面状態ごとのキー処理と再描画。Idle/Running/Break/ProcessList/ThreadList/LibraryList/SymbolList/RegionList/MemoryDump。 |
| `LineEditor` | `:` コマンド入力行（カーソル移動・削除・補完・貼り付け）。 |
| `CommandRegistry` | コマンド登録とディスパッチ、直前コマンドの繰り返し。 |
| `Command`（基底） | 各 `:` コマンド。`Expression`（式評価）を共通で使う。 |
| `Expression` | 再帰下降パーサ。数値/レジスタ/`lib`/`ライブラリ名!シンボル`/括弧/四則。レジスタ名は `Arch` から得る。`Symbols`+`ImageList` を参照。 |
| `Settings` | 実行対象パス、表示アドレス、選択スレッド、Intel 記法、相対アドレス、`autoskip` 名、既定ライブラリ、ブレイク画面レイアウトと表示 ON/OFF。 |

---

## 4. アーキテクチャ抽象（Arch）と移植性

大前提 A を実現する核。アーキ依存を 1 つのインタフェース `Arch` に集め、`ArchX86_64` が今の実装、
将来 `ArchArm64` を足すだけで arm64 に対応できるようにする。アタッチ時に対象の CPU タイプを見て、
使う `Arch` 実装を選ぶ。

### `Arch` が隠すもの

| 項目 | x86_64（現在） | arm64（将来） | 隠蔽する場所 |
|---|---|---|---|
| スレッド状態 flavor | `x86_THREAD_STATE64` | `ARM_THREAD_STATE64` | `Arch` / `Target` |
| レジスタ集合（名前と値） | rax〜r15, rip, rflags, cs/fs/gs, 32bit 別名 | x0〜x30, pc, sp, cpsr, w 別名 | `Arch::registers()` |
| PC / SP / FP | rip / rsp / rbp | pc / sp / x29 | `Arch` |
| ソフトブレイク命令 | `0xCC`（INT3, 1 byte） | `0xD4200000`（BRK #0, 4 byte） | `Arch::softBreakBytes()` |
| 単一ステップ | RFLAGS の TF(0x100) | `ARM_DEBUG_STATE64` の SS ビット（MDSCR） | `Arch` / `Stepper` |
| ハードウェアブレイク | DR0–3, DR7（`x86_DEBUG_STATE64`） | DBGBVR/DBGBCR（`ARM_DEBUG_STATE64`） | `Arch` / `Breakpoints` |
| call/分岐判定（next/auto） | capstone group CALL / ret / syscall | BL/BLR / ret / svc | `Disassembler` + `Arch` |
| フレーム巻き戻し（bt） | rbp チェーン | x29 チェーン | `Arch::unwind()` |
| 逆アセンブラのモード | `CS_ARCH_X86` + `CS_MODE_64` | `CS_ARCH_ARM64` | `Disassembler` |

### 規約

- **アーキ非依存コードはレジスタ名・命令バイト・DR 番号などを直書きしない**。
  必要なら `Arch` に問い合わせる。例: `RegisterView` と `Expression` は `Arch::registers()` から
  名前と値の一覧を得る（`rax` 等を固定で持たない）。`Breakpoints` は `Arch::softBreakBytes()` を書く。
- `Stepper` の「1 命令進める」は `Arch::enableSingleStep(thread, on)` を呼ぶ（x86 は TF、arm64 は SS）。
- `next` の「call の次で止める」判定と `auto` の停止条件は、`Disassembler` の命令分類（`Arch` が
  アーキに応じて解釈）を使う。命令ニーモニック文字列の直接比較はしない。
- `Arch` 実装は `src/core/arch/`（`ArchX86_64.cpp` など）にまとめ、他のモデルは `inc/core/Arch.h` の
  インタフェースだけを見る。arm64 対応は「`ArchArm64` を足して CMake に 1 行」で済むことを目標にする。
- **アーキ別コンパイルの実際**: mach の `arm_thread_state64_t` 等は SDK ヘッダが `__arm64__` で
  ガードしているため、`ArchArm64.cpp` は `-arch arm64` でのみ中身がコンパイルされる（x86_64 ビルドでは
  空の翻訳単位）。よって**どの `Arch` 実装がリンクされるかはビルド（ホスト）アーキで決まる**。
  x86_64 の開発機では x86_64 版を実行でき、arm64 版は毎ビルドでクロスコンパイル確認する
  （CMake の `arch_arm64_check`。コンパイルのみ、実行は arm64 実機で）。
  なお capstone は 1 ライブラリで arm64 も逆アセンブルできるので、**逆アセンブルの検証は x86_64 機でも可能**。

---

## 5. スレッド構成

| スレッド | 役割 |
|---|---|
| メイン | `App::mainLoop`。`EventQueue` から 1 件ずつ取り出し、現在の `Screen` に渡す。描画もここだけが行う（描画の直列化）。 |
| 入力 | STDIN を `read`。Ctrl+C はここで検知して `Target::requestPause()` を呼ぶ。他キーは `EventQueue` へ。 |
| 例外 | `ExceptionServer` が `mach_msg_server` を回す。例外受信時は対象を suspend し、対話が必要な停止ならメインへ通知。`auto`/`step` の続行はこのスレッド内で完結。 |
| 終了監視 | `TargetExitMonitor` の kqueue 待ち。終了を検知したらメインへ通知して自身は終わる。 |
| シグナル待ち | `sigwait` で TERM/HUP/INT/QUIT を受け、メインに「デタッチして終了」を依頼。3 秒応答がなければ自分でデタッチして `_exit`。 |

排他:
- `EventQueue` 内部のミューテックス（キュー）。
- `ImageList` の一覧入れ替え用ミューテックス（例外スレッドの `autoskip` 判定と、メインの再読込が競合するため）。
- `Target` の一時停止要求フラグ（入力スレッドと例外スレッド）。

---

## 6. イベント / データフロー

### キー入力
```
InputThread → KeyMapper → EventQueue → App → Screen::onKey → (必要なら) Command / Target 操作
```

### ブレイク発生
```
対象が ソフト/ハードブレイク → ExceptionServer(例外スレッド) → Target.suspend
  → 対話が必要? ── はい → StopObserver 通知 → EventQueue → App が BreakScreen へ切替 → 再描画
                └ いいえ(auto継続/再開) → 例外スレッド内でステップ設定して return
```

### 描画
`Screen::redraw()` が、必要な `View` に「この矩形に描け」と頼む。
`View` は `Target`/`Symbols`/`Arch` 等のモデルから値を取って `Terminal` に出す。
**モデルは描画関数を持たない**（旧 `Debugger::showDisassemble` 等は廃止）。

---

## 7. 主要な設計判断

### なぜ task_for_pid + 例外ポートか
対象を止めずにアタッチしたい。ptrace は停止を伴い、debugserver は別プロセス。
タスクポートを取り例外ポートを差し替えれば、対象は動き続けたまま例外だけ横取りできる。

### コードページへの書き込み（ソフトブレイク）
コードページは `max_protection` に WRITE が無く、単純な `vm_protect` は `KERN_PROTECTION_FAILURE`。
`VM_PROT_COPY` を付けて CoW のプライベートコピーにしてから書き、元の保護へ戻す。
戻す保護属性は `mach_vm_region` ではなく **`mach_vm_region_recurse`** で取る。
共有キャッシュはサブマップで、`mach_vm_region` だとサブマップ全体の値（実ページと別）が返り、
戻すと実行権限が消えて対象が `EXC_BAD_ACCESS` で落ちるため。
（書き込むバイト列は `Arch::softBreakBytes()`。x86 は 1 byte、arm64 は 4 byte なので
長さも `Arch` から得る。）

### dlopen 検知
アタッチ時に一覧を 1 回読むだけでは後から読み込んだライブラリを引けない。
dyld は一覧更新のたび `infoArrayChangeTimestamp` を進めるので、
コマンド実行前とブレイク描画前にこの 8 バイトを読み、変化していれば一覧を読み直す。
更新中は `infoArray` が一時 NULL になるので、その間は読まない。lldb のように通知関数へ
ブレイクを置いて対象を止める方式は取らない（対象を止めない方針のため）。

### Ctrl+C
- 実行中: タスクを一瞬 suspend し全スレッドを単一ステップ状態に。最初に命令を実行したスレッドで停止。
  他スレッドに遅れて届くトラップは「停止要求の残り」として黙って再開（`pauseTrapThreads_` 相当で判別）。
- `auto` 中: 自動ステップをやめて停止。`autoskip` 用に張ったハードウェアブレイクも消す。
- ブレイク中/未アタッチ: 何もしない。
- 全スレッドがシステムコール待ちだと、どれかが動くまで止まれない → ステータスに「停止待ち」。

### シグナル受信時のデタッチ
シグナルハンドラ内で `detach`（Mach API・ロック・確保）は不可。
起動時に対象シグナルを全スレッドでブロックし、専用スレッドが `sigwait` で受ける。
受けたらメインに「デタッチして終了」を依頼。応答がなければ自分でデタッチして端末を戻し `_exit(128+sig)`。
`SIGKILL` と自身のクラッシュは防げない（対象が道連れになり得る）。

### auto の描画間引き
`auto` は 1 命令ごとにブレイク/再開を UI へ通知すると描画が溜まって固まる。
通知は 0.2 秒に 1 回、停止時は必ず 1 回。

---

## 8. コマンド一覧（`:` 入力）

プロセス: `attach [pid]`（pid 省略で一覧選択）, `detach`, `r`（起動してアタッチ）, `q` / `q!`
ブレイク: `b <式>`（ソフト）, `dr` / `dr n,cond,len,<式>` / `resetdr [n]`（ハードウェア）
表示: `dis <式>`, `x <式>`, `reg`, `threads`, `libs`, `seg` / `seg <式>`, `sym [<式>]`, `bt`, `intel` / `att`, `baseon` / `baseoff`
書換: `setreg`, `setmem1/2/4/8/16`, `setmem`, `memset`, `wf`, `rf`
ステップ: `s`, `n`, `c`, `auto`, `autoskip [<名前>|-]`, `r`(再設定)
設定: `uselib [<名前>|-]`, `help`
ハッシュ: `md5`, `sha1`（CommonCrypto を使用。OpenSSL 非依存）

（旧版の全コマンドを踏襲。実装は `Command` 派生で 1 コマンド 1 クラス。
`intel`/`att` は x86 の構文切替で、arm64 では無効になる arch 依存コマンド。）

---

## 9. 画面とレイアウト

画面: Idle / 実行中 / ブレイク / プロセス選択 / スレッド一覧 / ライブラリ一覧 / シンボル一覧 / リージョン一覧 / メモリダンプ。

ブレイク画面は 4 領域（逆アセンブル・レジスタ・バックトレース・スレッド）を
5 レイアウト（標準 / 右サイド / 下段 / 左右半分 / 逆アセンブルのみ）で配置。
`l`/`L` で切替、`1`/`2`/`3` で各表示 ON/OFF、`0` で全表示。逆アセンブルは常時表示。
各領域は自領域内だけに描画し、はみ出しは `…` で切り詰め。レジスタは幅に応じ複数列。
レジスタの項目数と名前は `Arch` が決める（arm64 では x0〜x30 などになる）。

---

## 10. 式の文法

```
式   := 項 (('+' | '-') 項)*
項   := 単項 (('*' | '/') 単項)*
単項 := ('+' | '-') 単項 | 値
値   := 数値 | レジスタ | 名前 | '(' 式 ')'
数値 : 0x… は 16 進、それ以外 10 進
レジスタ : $ か % + レジスタ名（名前の集合は Arch が決める）
名前 : ライブラリ名!シンボル | ライブラリ名! | シンボル | lib
```
ライブラリ名省略時は 既定ライブラリ(`uselib`) → メイン実行ファイル の順。
C++ は引数を除いた `demo::run` で `demo::run(int)` に一致。

---

## 11. ディレクトリ構成・ビルド・依存方針

```
ta2debug/
  CMakeLists.txt
  DESIGN.md
  README.md
  src/
    app/      App EventQueue InputThread KeyMapper Screen* LineEditor
              CommandRegistry Command* Expression Settings main.cpp
    core/     Target ExceptionServer Breakpoints Stepper ImageList
              Symbols Disassembler TargetExitMonitor mach_excServer.c
      arch/   Arch(基底) ArchX86_64  （将来 ArchArm64 を追加）
    ui/       Terminal Layout PaneView *View StatusLine
    common/   Logger ほか（ロック等は標準 <mutex>/<thread> を使う）
  inc/        src と対になるヘッダ（同じサブディレクトリ構成）
  build/      中間ファイル
```

### 依存方針（大前提 B）

外部ライブラリは**可能な限り持たない**。各外部部品は 1 モジュールの裏に隔離し、差し替え可能にする。

| 部品 | 種別 | 用途 | 隔離先 |
|---|---|---|---|
| capstone | 外部ライブラリ（唯一） | 逆アセンブル | `Disassembler` |
| CommonCrypto | システム（libSystem 同梱） | md5 / sha1 | `md5`/`sha1` コマンド |
| picojson | 同梱ヘッダ 1 つ | 独自シンボル JSON の読み込み | `Symbols` |
| Security.framework | システムフレームワーク | タスクポート取得の権限まわり | `Target` |
| capstone 以外の mach/kqueue 等 | システム（libSystem） | デバッグ機構全般 | 各モデル |

- **OpenSSL を廃止**した（旧版は `md5`/`sha1` に libcrypto を使っていた）。CommonCrypto で同じことができ、
  外部ライブラリを 1 つ減らせる。
- **capstone は残す**。自前で x86_64 と arm64 の逆アセンブラを書くのは現実的でなく、capstone 1 つで両アーキを
  賄えるため、arm64 対応でも依存は増えない。`Disassembler` の裏に隔離してあるので、将来別物に差し替えることも可能。
- 手書きのロック/セマフォは標準 `<mutex>` / `<condition_variable>` / `<thread>` に寄せ、独自の同期部品を減らす。

### ビルド

- CMake + Ninja。`cmake -S . -B build -G Ninja && ninja -C build`。
- 実行ファイル `ta2debug` はリポジトリ直下に出す。
- AddressSanitizer 既定 ON（`-DUSE_ASAN=OFF` で無効）。C++17。
- MIG 生成物 `mach_exc*` は `src/core` と `inc/core` に置く。
- `CMakeLists.txt` はアーキ別ソースをリストで持つ。`ArchX86_64.cpp`/`ArchArm64.cpp` は両方を列挙してよい
  （非ホストのファイルは `#if` で空になる）。ホストが arm64 の Mac なら arm64 版がそのままリンクされる。
- **arm64 のクロスコンパイル確認**: `arch_arm64_check`（`clang++ -arch arm64 -c ArchArm64.cpp`）を
  毎ビルドで走らせ、実機が無くても arm64 コードが SDK に対して壊れていないことを保証する。

---

## 12. 旧版からの主な変更点

1. 神クラス `Debugger` を `Target`/`ExceptionServer`/`Breakpoints`/`Stepper`/
   `ImageList`/`Symbols`/`Disassembler` に分割。
2. **アーキ依存を `Arch` 抽象に隔離**（将来の arm64 対応。§4）。
3. **OpenSSL を廃止し CommonCrypto に置換**、外部依存を削減（§11）。外部ライブラリは capstone のみ。
4. 描画をモデルから分離（`show*` 廃止 → `View` 層へ）。モデルは問い合わせ API のみ。
5. 画面・コマンドの基底を整理し、一覧系画面は `ListView` で共通化。
6. C++17 に更新し、生ポインタと手書きロックを標準機能（`std::mutex`/`std::unique_ptr` 等）へ寄せる。
7. 機能・コマンド・キー操作・バグ修正はすべて踏襲（§1, §7）。
