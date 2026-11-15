# ta2debug English message catalog (i18n stage i-6)
# Format: <key> = <text>. '#' lines are comments. Escapes: \n \t \\ ; {{ }} are literal braces.
# Placeholder names ({name}) must match the Japanese defaults in base/Messages.def exactly.
# Non-translatable tokens (EXC_BAD_ACCESS, task_for_pid, dtrace_*, command names, register names,
# strerror/mach_error_string text in {detail}) are kept as-is. Hand-editable, like .t2a/.t2b.

# ---- err.target.* ----
err.target.already_attached   = already attached
err.target.task_for_pid       = task_for_pid failed: {detail} (check: running as root, SIP disabled, and that the target is your own binary)
err.target.task_for_pid_launch = task_for_pid failed: {detail}
err.target.posix_spawn        = posix_spawn failed: {detail}
err.target.arch_mismatch      = target architecture differs from host ({target} / host {host})

# ---- err.exc.* ----
err.exc.already_installed      = already installed
err.exc.mach_call              = {call}: {detail}

# ---- err.dtrace.* ----
err.dtrace.busy                = already watching
err.dtrace.open                = dtrace_open failed: {detail}
err.dtrace.compile             = D compile failed: {detail}
err.dtrace.exec                = program_exec failed: {detail}
err.dtrace.go                  = dtrace_go failed: {detail}
err.dtrace.no_probe            = no matching probe (check the syscall/trap name, e.g. write / mach:mach_msg_trap)
err.dtrace.name_required       = a syscall name is required for argument tracing (all is counts only)

# ---- err.session.* ----
err.session.not_attached       = not attached

# ---- err.annot.* / err.bpstore.* ----
err.annot.no_uuid              = cannot save annotations for an image with no UUID
err.annot.open                 = cannot write annotation file: {path}
err.annot.write                = failed to write annotation file
err.annot.rename               = failed to replace annotation file
err.annot.not_found            = no annotation at that address
err.bpstore.no_uuid            = cannot save breakpoints for a program with no UUID
err.bpstore.open               = cannot write save file: {path}
err.bpstore.write              = failed to write save file
err.bpstore.rename             = failed to replace save file

# ---- err.expr.* ----
err.expr.at                    = {error} (at column {pos})
err.expr.empty                 = empty expression
err.expr.trailing              = trailing characters after expression (at column {pos})
err.expr.div_zero              = division by zero
err.expr.no_value              = no value
err.expr.unclosed_paren        = unclosed parenthesis
err.expr.unmatched_paren       = ')' with no matching '('
err.expr.bad_char              = unrecognized character '{ch}'
err.expr.hex_digits            = no hex digits after 0x
err.expr.bad_number            = not a valid number
err.expr.no_reg_name           = missing register name
err.expr.reg_needs_stop        = registers can be read only while stopped
err.expr.unknown_reg           = unknown register '{name}'
err.expr.ambiguous             = symbol '{name}' is ambiguous: {candidates}. Specify an address or 'library!name'
err.expr.no_lib                = library '{lib}' not found
err.expr.sym_not_in_lib        = symbol '{name}' not in {lib}
err.expr.no_name               = missing name
err.expr.no_default_lib        = no default library / main executable known
err.expr.no_images             = '{name}': no images to search (attached?)
err.expr.cancelled             = cancelled
err.expr.multi_lib             = symbol '{name}' exists in multiple libraries ({libs}). Specify 'library!{name}'
err.expr.sym_not_found         = symbol '{name}' not found (searched {where} and all images; you can also try library!{name})

# ---- err.mem.* / err.examine.* ----
err.mem.unreadable             = {addr} is not readable
err.examine.bad_format         = unknown format: {ch} (formats x/d/u/o/t/c/i/s/f/a, sizes b/h/w/g)

# ---- bt.note.* (backtrace recovery notes) ----
bt.note.from_sp                = <- recovered from [sp]
bt.note.from_lr                = <- recovered from lr
bt.note.fp_chain               = <- fp chain
bt.note.stack_scan             = <- recovered by stack scan (estimated)

# ---- stop.* (stop reasons) ----
stop.syscall_catch             = syscall catch: {name}
stop.exc.bad_access            = EXC_BAD_ACCESS (invalid memory access) [{kr}] @{addr}
stop.exc.bad_access_code       = EXC_BAD_ACCESS (invalid memory access) [code={code}] @{addr}
stop.exc.bad_instruction       = EXC_BAD_INSTRUCTION (illegal instruction) [code={code}]
stop.exc.arithmetic            = EXC_ARITHMETIC (arithmetic exception) [code={code}]
stop.exc.other                 = exception [code={code}]

# ---- cmd.* ----
cmd.unknown                    = unknown command: {name} (:help for the command list)

# ---- err.job.* ----
err.job.busy                   = busy (press ESC to cancel)
err.job.cancelled              = cancelled
err.job.internal               = internal error: {detail}
err.job.internal_unknown       = internal error (unknown)
err.expr.wrap                  = expression error: {error}

# ---- err.cmd.* ----
err.cmd.need_attach            = attach first
err.cmd.not_stopped            = (not stopped)
err.cmd.not_running            = (not running)

# ---- target.* / attach / launch ----
err.attach.already             = already attached (:detach first)
err.launch.already             = already attached/launched (:detach first)
target.attached                = attached pid {pid}
err.attach.failed              = attach failed: {error}
err.launch.no_path             = specify a file path to launch
target.launched                = launched: {path}  (stops at entry; output goes to {path}.ta2debug.log)
err.launch.failed              = launch failed: {error}
err.restart.not_launched       = only targets started with :run can restart (:attach targets cannot)
target.restarted               = restarted: {path} (stops at entry)
target.restarted_bps           = restarted: {path} (stops at entry; restored {n}/{total} breakpoints)
target.detached                = detached

# ---- auto.* ----
err.auto.needs_stop            = auto works only while stopped (stop at a breakpoint first)
err.auto.bad_arg               = specify call / syscall(sc) / ret / all (default all)
auto.running                   = auto running... (Ctrl+C to interrupt)
auto.interrupted               = auto interrupted
auto.stopped                   = auto -> stopped at {kind} ({n} instructions)
auto.stopped_limit             = auto -> stopped at limit ({n} instructions)

# ---- bp.* (breakpoint results) ----
bp.set                         = #{id} breakpoint set {addr}
err.bp.set_failed              = failed to set breakpoint (memory not writable?)
bp.hw_set                      = #{id} HW breakpoint set {addr}
err.bp.hw_full                 = no HW breakpoint slots free (max {max})
err.watch.bad_arg              = unknown spec: {arg} (size 1/2/4/8, kind r/w/rw)
err.watch.unaligned            = address {addr} is not aligned to {size} bytes
bp.watch_set                   = #{id} watchpoint set {addr} {size} bytes {mode}
err.watch.full                 = no watchpoint slots free (max {max})
bp.removed                     = removed #{id}
err.bp.not_found               = no such breakpoint
err.bp.none_at_addr            = no breakpoint at that address
bp.enabled                     = enabled #{id}
bp.disabled                    = disabled #{id}
err.bp.toggle_failed           = failed to toggle
bp.cleared                     = #{id} breakpoint cleared {addr}
err.bp.clear_failed            = failed to clear
err.bp.rebuild_failed          = change failed (not enough HW slots etc.; reverted)
err.bp.save_needs_attach       = breakpoints can be saved only while running (attached); press w while the target is alive
err.bp.save_no_main            = main executable not found; cannot save
err.bp.save_no_uuid            = main executable has no UUID; cannot save
err.bp.save_failed             = save failed: {error}
bp.saved                       = saved {n} breakpoint(s) -> {path}  (auto-restored on next :run)
bp.saved_skipped               = saved {n} breakpoint(s) (excluded {skipped} outside the image) -> {path}  (auto-restored on next :run)
bp.restored                    = restored {n} saved breakpoint(s)
bp.restore_pending             = {n} of them not yet loaded (auto-restored after dlopen)
bp.restored_new_load           = restored {n} saved breakpoint(s) (newly loaded)

# ---- reg.* / dr / dis ----
err.reg.needs_stop             = registers can be read/modified only while stopped
reg.set                        = set {name} = {value}
err.reg.not_writable           = register not writable: {name}
err.dr.needs_stop              = dr can be read only while stopped
err.dis.needs_stop             = dis works only while stopped (moving the disassembly view)

# ---- mem.* / file.* ----
err.mem.hex_bad                = invalid HEX (even-length hex, e.g. setmem $rsp,9090CC)
mem.written                    = wrote {n} bytes to {addr}
err.mem.write_failed           = write failed (check the address)
err.mem.fill_size              = size must be 1..16MB
mem.filled                     = filled {size} bytes at {addr} with {value}
err.mem.fill_failed            = write failed
err.file.args                  = path required; size 1..256MB
err.mem.read_failed            = cannot read memory
err.file.open                  = cannot open file: {path}
file.dumped                    = dumped {n} bytes from {addr} -> {path}
err.file.empty                 = file is empty or unreadable
file.loaded                    = wrote {n} bytes of {path} to {addr}
err.mem.load_failed            = cannot write to memory

# ---- sym / lib ----
sym.no_image                   = (not in any image)
err.lib.not_found              = library not found: {name}
lib.default_set                = default library: {name} (symbol names in expressions are searched here first)
lib.default_cleared            = default library reset to auto (main executable)

# ---- ann.* ----
err.ann.no_image               = that address is not in any image
err.ann.empty_name             = name is empty (for a memo only, use :memo <expr>,<memo>)
err.ann.save                   = cannot save annotation: {error}
ann.set                        = annotation: {addr} = {name}  -> {path}
ann.set_memo                   = annotation: {addr} = {name}  ; {memo}  -> {path}
err.ann.memo_save              = cannot save memo: {error}
ann.memo_set                   = memo: {addr}  ; {memo}  -> {path}
ann.memo_removed               = memo: removed {addr}  -> {path}
ann.removed                    = removed annotation: {addr}
err.ann.no_uuid_image          = this image has no UUID; cannot save annotations: {image}
err.ann.register               = cannot register symbol: {error}
ann.registered                 = symbol registered: {name}  ({addr} = {image}+{off})  -> {path}  / press m again to edit

# ---- sc.* (syscall watch/stop) ----
err.sc.watch_off_is_stop       = what is running now is the :scstop stop catchpoint (clear it with :scstop off)
err.sc.watch_not_running       = syscall watch is not running
sc.watch_stopped               = stopped syscall watch
err.sc.watch_start             = failed to start syscall watch: {error}
sc.watch_started               = started syscall watch [{filter}] (:scwatch for status / :scwatch off to stop)
sc.trace_started               = started syscall trace [{filter}] (with args and return values; :scwatch to list / :scwatch off to stop)
err.sc.stop_not_set            = no stop catchpoint is set
sc.stop_cleared                = cleared syscall stop catchpoint
err.sc.stop_start              = failed to set stop catchpoint: {error}
sc.stop_set                    = set syscall stop catchpoint [{filter}] (stops on fire; :scstop off to clear)

# ---- disasm ----
disasm.syntax_set              = disassembly syntax: {name} (from the next draw)

# ---- usage.* ----
usage.run                      = usage: run <executable path> [args...]
usage.w                        = usage: w <expr> [size 1/2/4/8] [r|w|rw] (default 8, write)
usage.bd                       = usage: bd <#id | expr>
usage.bt                       = usage: bt <#id | expr> (toggle the breakpoint enabled/disabled)
usage.reg                      = usage: reg <name>=<expr>
usage.x                        = usage: x <expr> [bytes]  /  x/<count><format><size> <expr> (continue with x or Enter)
usage.ann                      = usage: ann <expr>,<name>[,<memo>] (name an address; for a memo only use :memo)
usage.memo                     = usage: memo <expr>,<memo> (attach a memo to an address; empty memo removes it)
usage.unann                    = usage: unann <expr> (remove the annotation at an address)
usage.p                        = usage: p <expr> (p/x hex, p/d decimal)
usage.setmem                   = usage: setmem[1/2/4/8] <expr>,<value>  /  setmem <expr>,<HEX>
usage.memset                   = usage: memset <expr>,<value>,<size>
usage.file_io                  = usage: {cmd} <expr>,<size>,<path>
usage.dis_batch                = usage: dis <expr> [count]

# ---- help.group.* ----
help.group.target              = Attach / launch
help.group.run                 = Run control
help.group.break               = Breakpoints
help.group.inspect             = Inspect
help.group.modify              = Modify
help.group.annot               = Annotations (your own names and memos)
help.group.syscall             = Syscall watch (libdtrace, observe without stopping)
help.group.display             = Display settings
help.group.misc                = Misc

# ---- help.cmd.* ----
help.cmd.attach                = omit pid to choose from a process list
help.cmd.run                   = launch a binary and debug it (stops before entry)
help.cmd.restart               = relaunch a :run target with the same args and address (re-arms breakpoints)
help.cmd.detach                = detach (an attached target keeps running; a :run child is terminated)
help.cmd.quit                  = quit
help.cmd.auto                  = auto-continue to just before the next call / syscall / ret (default all; Ctrl+C to interrupt)
help.cmd.continue              = continue
help.cmd.step                  = step into
help.cmd.next                  = step over
help.cmd.wait                  = wait until stopped
help.cmd.b                     = soft breakpoint (INT3/BRK)
help.cmd.bh                    = hardware execute breakpoint (debug registers)
help.cmd.w                     = watchpoint (data watch; default 8-byte write)
help.cmd.bl                    = list (● SW / ◆ HW / [W] watch, [disabled] / (Nhit))
help.cmd.be                    = breakpoint editor overlay: list -> select -> d delete / e enable-disable / h Soft<->Hard / a address / s size / r access
help.cmd.bd                    = delete
help.cmd.bt                    = toggle a breakpoint enabled/disabled
help.cmd.reg                   = register list / one (while stopped). <name>=<expr> to write (partial registers too); xmm[N] / vN for vector/float registers
help.cmd.dr                    = debug registers (x86 dr0-7 / arm64 bvr/bcr/wvr/wcr)
help.cmd.p                     = print the value of an expression (dec+hex / hex / dec)
help.cmd.x                     = memory hex dump / formatted (formats x d u o t c i s f a / sizes b h w g); no args continues
help.cmd.dis                   = move the disassembly view to that address (default current PC; then ↑↓/Ctrl+F). In batch, text output
help.cmd.sym                   = symbol at an address
help.cmd.img                   = loaded images (libraries) list
help.cmd.seg                   = segment list
help.cmd.syms                  = symbol list for a library (default the default library; filter is substring)
help.cmd.uselib                = show / set / clear the default library searched first when an expression omits the library name
help.cmd.where                 = backtrace
help.cmd.setmem                = write bytes (HEX) (e.g. $rsp,9090CC)
help.cmd.setmem_n              = write an N-byte value
help.cmd.memset                = fill with a 1-byte value
help.cmd.wf                    = dump memory to a file
help.cmd.rf                    = load a file into memory
help.cmd.ann                   = name an address (then usable via :b <name> / in expressions; also shown in output)
help.cmd.memo                  = attach a memo to an address (empty memo removes it)
help.cmd.unann                 = remove an annotation
help.cmd.anns                  = annotation list (default the main executable); also shows the save file path
help.cmd.scwatch               = observe syscalls / Mach traps without stopping (<name> traces args and return values; all / mach for counts; list [word] for names; no arg for status; off to stop)
help.cmd.scstop                = stop the target when that syscall/trap fires (like a breakpoint; c runs to the next fire; list [word] / no arg for status / off to clear; exclusive with :scwatch)
help.cmd.intel                 = switch disassembly to Intel syntax (x86)
help.cmd.atandt                = switch disassembly to AT&T syntax (default) (x86)
help.cmd.baseoff               = show disassembly addresses relative to the image base
help.cmd.baseon                = show disassembly addresses as real addresses (default)
help.cmd.mouse                 = mouse on/off (toggle if omitted). ON = click/drag select-copy, OFF = back to the terminal's own selection
help.cmd.help                  = this help

# ---- ui.* (shared frontend text) ----
ui.not_stopped                 = (not stopped)
ui.not_running                 = (not running)
ui.target_exited               = the target process exited
ui.unknown_register            = unknown register: {name}
ui.cmd_unsupported             = (command not supported in this frontend)
ui.defaultlib                  = default library: {name}
ui.defaultlib.auto             = default library: (auto = main executable)
ui.defaultlib.auto_short       = default library: (auto)
ui.crash_hint                  = ⚠ {reason}  — press c to let the target terminate normally (pass the signal to its own handler)
ui.auto.limit                  = auto -> stopped at limit ({n} instructions)
ui.auto.interrupted            = auto -> interrupted ({n} instructions)
ui.auto.kind                   = auto -> stopped at {kind} ({n} instructions)
ui.note_sep                    =  /
ui.regs.title                  = Registers
ui.enabled                     = enabled
ui.disabled                    = disabled
ui.bp.count                    = {n} breakpoint(s)
ui.bp.none                     = (no breakpoints)
ui.img.count                   = {n} image(s)
ui.image_not_found             = image not found
ui.image_unknown               = target image unknown
ui.seg.title                   = segments {image} (slide {slide})
ui.seg.none                    = (no segment info)
ui.sym.count                   = {n} symbol(s) in {image}
ui.sym.truncated               = (head only)
ui.sym.none                    = (none)
ui.ann.count                   = {n} annotation(s) in {image}
ui.ann.no_uuid_suffix          = (no UUID = cannot save)
ui.ann.no_name                 = (no name)
ui.ann.none                    = (no annotations)
ui.examine.none                = (nothing to show)
ui.help.title                  = ta2debug command list
ui.title.breakpoints           = Breakpoints
ui.title.images                = Images
ui.title.segments              = Segments
ui.title.symbols               = Symbols
ui.title.annotations           = Annotations
ui.title.help                  = Help
ui.title.syscall               = syscall
ui.title.syscall_names         = syscall names
ui.running_hint                = (running; stops when a breakpoint is hit)
ui.idle_hint                   = :run <path> / :attach <pid> to start   :help   :q to quit
ui.pager_hint                  = (↑↓ Space/b page  g/G ends  ESC/q close)
ui.vec.title                   = Vector registers
ui.vec.unavailable             = cannot read vector registers
ui.vec.arch_unsupported        = this architecture has no vector registers
ui.vec.unknown                 = unknown vector register: {name}
ui.dr.title                    = Debug registers
ui.dr.unavailable              = cannot read debug registers
ui.dr.arch_unsupported         = this architecture does not show debug registers
ui.baseoff.on                  = disassembly address display: relative to image base
ui.baseoff.off                 = disassembly address display: real address
ui.sym.none_at                 = no symbol at that address
ui.sc.idle                     = syscall watch: stopped (:scwatch <name|all> to start)
ui.sc.watch_but_stop           = syscall watch is not running (what is running is the :scstop catchpoint; see :scstop)
ui.sc.watch_running            = syscall watch [running {filter}]  total {n} (:scwatch off to stop)
ui.sc.watch_stopped            = syscall watch [stopped {filter}]  total {n}
ui.sc.trace_running            = syscall trace [running {filter}]  total {n} (:scwatch off to stop)
ui.sc.trace_stopped            = syscall trace [stopped {filter}]  total {n}
ui.sc.none_yet                 = (no fires yet)
ui.sc.stop_idle                = syscall stop catch: not set (:scstop <name> to set)
ui.sc.stop_running             = syscall stop catch [running {filter}]  fired {n} (c for the next fire)
ui.sc.stop_stopped             = syscall stop catch [stopped {filter}]  fired {n}
ui.sc.names_bsd                = BSD syscalls ({n})
ui.sc.names_mach               = Mach traps ({n})
ui.sc.names_none               = (none)
ui.layout.changed              = layout {n}
ui.search.cancelled            = search cancelled
ui.search.progress             = searching symbols... {done}/{total} (ESC to cancel)
ui.mouse.on                    = mouse: ON (click to move cursor; drag-select is the terminal's)
ui.mouse.off                   = mouse: OFF (back to the terminal's selection)
ui.copied                      = copied {n} bytes
ui.examine.no_prev             = no x yet (start with x <expr>)
ui.picker.title                = Select a process to attach (your own programs only)
ui.picker.filter               = Filter:
ui.picker.hint                 = ↑↓/C-p,C-n row   C-f/C-b page   ←→ scroll args   Enter attach   type to filter   ESC cancel
ui.picker.empty                = (no matching process)
ui.picker.not_attachable       = (not attachable)
ui.bpedit.title                = Breakpoint editor
ui.bpedit.hint                 = ↑↓ select   Space enable/disable   h SW<->HW   d/Del delete   ESC/q close
ui.bpedit.empty                = (no breakpoints; set one with :b <expr>)
ui.bpedit.kind_toggled         = changed #{id} to {kind}
ui.syms.title                  = symbols in {image}  {hits}/{total}  [{sort}]
ui.syms.sort_addr              = by address
ui.syms.sort_name              = by name
ui.syms.filter                 = Filter:
ui.syms.hint                   = ↑↓ select   Enter set/clear breakpoint   Tab sort   type to filter   ESC close
ui.syms.empty                  = (no matching symbol)
