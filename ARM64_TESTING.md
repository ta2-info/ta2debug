# arm64（Apple Silicon）実機テスト手順

`ta2debug` のアーキ依存コード（`src/core/arch/ArchArm64.cpp`）は **x86_64 では未実行**で、
クロスコンパイル確認（`arch_arm64_check`：構文・ヘッダ整合のみ）しか通していない。
**arm64 実機でビルドしたら、このファイルの項目を一度ひととおり流して「実際に動く」ことを確認する。**
確認できた項目は末尾のチェック表に日付を入れていくと、次に何が残っているか分かる。

アーキ非依存の部分（dtrace の syscall 監視、DWARF 行情報、注釈など）は x86_64 で検証済みなので、
ここでは **arm64 固有の実装が絡む箇所**を重点的に挙げる。

---

## 準備

```sh
# 1) ビルド（authoritative は ninja）
cmake -S . -B build -G Ninja && ninja -C build       # => ./ta2debug（arm64 ネイティブ）

# 2) テスト用の自作プログラム（自分のコードのみ対象。DRM 等は不可）
cat > /tmp/t.c <<'EOF'
#include <stdio.h>
#include <unistd.h>
static int add_one(int x){ int y = x + 1; return y; }   // 末端。-O2 でフレームレスになりやすい
static int work(int n){ int s = 0; for (int i=0;i<n;i++) s += add_one(i); return s; }
static long g = 0;                                        // ウォッチポイント対象
int main(void){
  for(;;){ g = work(5); write(1, "tick\n", 5); usleep(300000); }
  return 0;
}
EOF
clang -g -gdwarf-5 -O0 -c /tmp/t.c -o /tmp/t.o && clang -g -o /tmp/t /tmp/t.o && dsymutil /tmp/t
# -O2 版も作ると CFI（フレームレス/末端）確認に使える：
clang -g -O2 -c /tmp/t.c -o /tmp/t2.o && clang -g -o /tmp/t2 /tmp/t2.o && dsymutil /tmp/t2

# 3) 実行は root（SIP 無効前提）。batch は -b、対話 TUI は無印。
TA2=./ta2debug
```

バッチ 1 発で複数項目を流す例：
```sh
sudo -n "$TA2" -b /tmp/t -o "b add_one" -o "c" -o "reg" -o "where" -o "s" -o "where" -o "q"
```

---

## 確認項目（arm64 固有）

### 1. アーキ判定・レジスタ名
`Arch::createForHost()` が arm64 実装を選ぶこと。
```sh
sudo -n "$TA2" -b /tmp/t -o "b main" -o "c" -o "reg" -o "q"
```
- [ ] `reg` が **x0..x30 / pc / sp / cpsr** を出す（`rax` 等 x86 名が出たら誤り）。
- [ ] `reg pc` `reg sp` `reg $lr`（= x30）が引ける。`reg x0` 単体も可。

### 2. バックトレース / CFI アンワインド（最重要）
arm64 は compact unwind のエンコードも、関数入口の戻り先復元も x86 と別物
（`retAddrAtEntryOnStack()==false`、戻り先は **LR(x30)**）。
```sh
sudo -n "$TA2" -b /tmp/t  -o "b add_one" -o "c" -o "where" -o "q"   # 入口で LR 復元
sudo -n "$TA2" -b /tmp/t2 -o "b add_one" -o "c" -o "where" -o "q"   # -O2 フレームレス
```
- [ ] `add_one ← work ← main ← start` の順で**正しい呼び出し階層**が出る。
- [ ] 関数入口（`+0x0`）で止めても戻り先が復元できる（`← [lr] から復元` 等の注記）。
- [ ] `-O2`（フレームポインタ無し／末端関数）でもフレームが崩れない。
- [ ] 日本語の復元注記が実態と合っている（`← fp 連鎖` / `← スタック走査で復元(推定)`）。

### 3. ソフトブレイク（BRK #0）
arm64 は 4 バイトの `BRK #0`、ELR はブレイク命令そのもの（`softBreakPcBackup()==0`）。
```sh
printf 'b add_one\nc\nwhere\nc\nwhere\nc\nwhere\ndetach\n' | sudo -n "$TA2" -b /tmp/t
```
- [ ] 停止 PC が `add_one+0x0`（命令の**先**に 1 ずれない）。
- [ ] `c` のたびに **hit 回数が増える**（ブレイク除去→1 ステップ→再設置の再アーム動作が正しい）。
- [ ] `detach` 後も子プロセスが生き続ける（BRK バイトが元に戻っている）。

### 4. 単一ステップ（MDSCR の SS ビット＝デバッグ状態）
x86 は RFLAGS.TF、arm64 は**デバッグ状態**で 1 命令実行（`singleStepUsesDebugState()==true`）。
```sh
sudo -n "$TA2" -b /tmp/t -o "b work" -o "c" -o "s" -o "s" -o "n" -o "where" -o "q"
```
- [ ] `s`/`si` が 1 命令ずつ進む（PC が命令長ぶん前進）。
- [ ] `n`（next）が `bl add_one` を**関数内に入らず**ステップオーバーする。

### 5. ハードウェア実行ブレイク（デバッグレジスタ）
`hwBreakHit()` は arm64 では debugState から判らず -1 を返す → PC 一致で判定する経路。
```sh
sudo -n "$TA2" -b /tmp/t -o "bh add_one" -o "c" -o "where" -o "c" -o "where" -o "q"
```
- [ ] `bh` で `add_one` に止まり、`c` で再発火する。
- [ ] ソフトブレイクと混在させても取り違えない。

### 6. ハードウェアウォッチポイント（データブレイク）
arm64 は実行ブレイクと**別バンク**（`hwExecWatchSharePool()==false`）で、
**アクセス命令の手前で止まり再実行**する（`hwWatchStopsBeforeAccess()==true`）ため、
継続時に 1 命令だけ外して跨ぐ処理が要る。ここが x86 と最も挙動が違う。
```sh
# g のアドレスを sym で引いてから張る（例）
sudo -n "$TA2" -b /tmp/t -o "b main" -o "c" -o "w g 8 w" -o "c" -o "reg pc" -o "where" -o "c" -o "q"
```
- [ ] `g` への書き込みで停止する（`w ... w`）。`r`/`rw` も試す。
- [ ] 停止後 `c` でハングやループに陥らず、**次の書き込みまで進む**（跨ぎ処理が効いている）。
- [ ] `size` 1/2/4/8 と境界整列で正しく張れる。

### 7. ベクタ / デバッグレジスタの表示
```sh
sudo -n "$TA2" -b /tmp/t -o "b main" -o "c" -o "reg v0" -o "dr" -o "reg x0=0x1234" -o "reg x0" -o "q"
```
- [ ] `reg v0`（128bit）/ `reg vN` が出る。
- [ ] `dr` が **bvr/bcr/wvr/wcr** を出す（x86 の dr0-7 名でない）。
- [ ] `reg x0=<値>` の書き込みが読み戻せる。

### 8. syscall デコード + dtrace 監視
arm64 は `svc`、番号は **x16**、引数は **x0..x5**。dtrace 自体はアーキ非依存だが実機で確認。
```sh
sudo -n "$TA2" -b /tmp/t -o "b main" -o "c" -o "auto sc" -o "q"               # → syscall: write(...) デコード
printf 'scwatch write\nc\nq\n'      | sudo -n "$TA2" -b /tmp/t                 # observe（引数・戻り値）
printf 'scstop write\nc\nwhere\nq\n'| sudo -n "$TA2" -b /tmp/t                 # stop キャッチポイント
sudo -n "$TA2" -b /tmp/t -o "scwatch list write" -o "q"                        # 名前一覧
```
- [ ] `→ syscall:` が `write(1, 0x.., 5)` のように引数まで解く。
- [ ] `scwatch`/`scstop`/`scwatch list` が arm64 でも動く（Mach トラップ名もデコードされる）。

### 9. 逆アセンブル / 命令分類
```sh
sudo -n "$TA2" -b /tmp/t -o "b work" -o "c" -o "dis" -o "x/8i $pc" -o "q"
```
- [ ] arm64 ニーモニック（`stp`/`ldp`/`bl`/`ret`/`svc` 等）が出る。
- [ ] `n`/`auto call`/`auto ret` が `bl`/`blr`/`ret` を正しく境界と見なす（項目 4 と合わせて）。
- [ ] （`intel`/`atandt` は x86 専用。arm64 では無効で構わない。）

### 10. アドレス安定性・ソース行
```sh
sudo -n "$TA2" -b /tmp/t -o "b add_one" -o "c" -o "where" -o "q"   # 2 回実行して PC が同一か
```
- [ ] `run`（launch）した対象は ASLR 無効でアドレスが再実行間で不変。
- [ ] dSYM があれば `src: t.c:<行>  <ソース>` が出る（`-gdwarf-5` でも。アーキ非依存だが実機確認）。

---

## 進捗チェック表

| 項目 | 確認日 | 備考 |
|---|---|---|
| 1 レジスタ名 | | |
| 2 バックトレース/CFI | | ← 最重要。-O2 と入口の両方 |
| 3 ソフトブレイク(BRK) | | hit 増加 + detach 生存 |
| 4 単一ステップ(SS) | | n のステップオーバー |
| 5 HW 実行ブレイク | | |
| 6 ウォッチポイント | | 跨ぎ処理（arm64 固有） |
| 7 ベクタ/デバッグレジスタ | | |
| 8 syscall/dtrace | | |
| 9 逆アセンブル/分類 | | |
| 10 アドレス安定/ソース行 | | |

> メモ: ブレイク/ステッパ系（3〜6）を変更したら、**SW と HW の両方**を必ず live テストし、
> 継続のたびに hit 数が増える（再アーム）ことと、detach 後に子が生き残ることを確認する。
