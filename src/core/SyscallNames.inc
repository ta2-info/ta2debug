// 自動生成: SDK <sys/syscall.h> の全 SYS_* を #ifdef ガードで列挙した「番号→名前」表。
// 生成コマンド: grep の結果から {SYS_x,"x"} を出力（手編集しないこと）。arg デコードは kScalls が担当し、
// ここに無い番号の素の名前フォールバックに使う。ビルド時 SDK に追従（存在しない名前は #ifdef で除外）。
// clang-format off
#ifdef SYS_syscall
  {SYS_syscall,"syscall"},
#endif
#ifdef SYS_exit
  {SYS_exit,"exit"},
#endif
#ifdef SYS_fork
  {SYS_fork,"fork"},
#endif
#ifdef SYS_read
  {SYS_read,"read"},
#endif
#ifdef SYS_write
  {SYS_write,"write"},
#endif
#ifdef SYS_open
  {SYS_open,"open"},
#endif
#ifdef SYS_close
  {SYS_close,"close"},
#endif
#ifdef SYS_wait4
  {SYS_wait4,"wait4"},
#endif
#ifdef SYS_link
  {SYS_link,"link"},
#endif
#ifdef SYS_unlink
  {SYS_unlink,"unlink"},
#endif
#ifdef SYS_chdir
  {SYS_chdir,"chdir"},
#endif
#ifdef SYS_fchdir
  {SYS_fchdir,"fchdir"},
#endif
#ifdef SYS_mknod
  {SYS_mknod,"mknod"},
#endif
#ifdef SYS_chmod
  {SYS_chmod,"chmod"},
#endif
#ifdef SYS_chown
  {SYS_chown,"chown"},
#endif
#ifdef SYS_getfsstat
  {SYS_getfsstat,"getfsstat"},
#endif
#ifdef SYS_getpid
  {SYS_getpid,"getpid"},
#endif
#ifdef SYS_setuid
  {SYS_setuid,"setuid"},
#endif
#ifdef SYS_getuid
  {SYS_getuid,"getuid"},
#endif
#ifdef SYS_geteuid
  {SYS_geteuid,"geteuid"},
#endif
#ifdef SYS_ptrace
  {SYS_ptrace,"ptrace"},
#endif
#ifdef SYS_recvmsg
  {SYS_recvmsg,"recvmsg"},
#endif
#ifdef SYS_sendmsg
  {SYS_sendmsg,"sendmsg"},
#endif
#ifdef SYS_recvfrom
  {SYS_recvfrom,"recvfrom"},
#endif
#ifdef SYS_accept
  {SYS_accept,"accept"},
#endif
#ifdef SYS_getpeername
  {SYS_getpeername,"getpeername"},
#endif
#ifdef SYS_getsockname
  {SYS_getsockname,"getsockname"},
#endif
#ifdef SYS_access
  {SYS_access,"access"},
#endif
#ifdef SYS_chflags
  {SYS_chflags,"chflags"},
#endif
#ifdef SYS_fchflags
  {SYS_fchflags,"fchflags"},
#endif
#ifdef SYS_sync
  {SYS_sync,"sync"},
#endif
#ifdef SYS_kill
  {SYS_kill,"kill"},
#endif
#ifdef SYS_getppid
  {SYS_getppid,"getppid"},
#endif
#ifdef SYS_dup
  {SYS_dup,"dup"},
#endif
#ifdef SYS_pipe
  {SYS_pipe,"pipe"},
#endif
#ifdef SYS_getegid
  {SYS_getegid,"getegid"},
#endif
#ifdef SYS_sigaction
  {SYS_sigaction,"sigaction"},
#endif
#ifdef SYS_getgid
  {SYS_getgid,"getgid"},
#endif
#ifdef SYS_sigprocmask
  {SYS_sigprocmask,"sigprocmask"},
#endif
#ifdef SYS_getlogin
  {SYS_getlogin,"getlogin"},
#endif
#ifdef SYS_setlogin
  {SYS_setlogin,"setlogin"},
#endif
#ifdef SYS_acct
  {SYS_acct,"acct"},
#endif
#ifdef SYS_sigpending
  {SYS_sigpending,"sigpending"},
#endif
#ifdef SYS_sigaltstack
  {SYS_sigaltstack,"sigaltstack"},
#endif
#ifdef SYS_ioctl
  {SYS_ioctl,"ioctl"},
#endif
#ifdef SYS_reboot
  {SYS_reboot,"reboot"},
#endif
#ifdef SYS_revoke
  {SYS_revoke,"revoke"},
#endif
#ifdef SYS_symlink
  {SYS_symlink,"symlink"},
#endif
#ifdef SYS_readlink
  {SYS_readlink,"readlink"},
#endif
#ifdef SYS_execve
  {SYS_execve,"execve"},
#endif
#ifdef SYS_umask
  {SYS_umask,"umask"},
#endif
#ifdef SYS_chroot
  {SYS_chroot,"chroot"},
#endif
#ifdef SYS_msync
  {SYS_msync,"msync"},
#endif
#ifdef SYS_vfork
  {SYS_vfork,"vfork"},
#endif
#ifdef SYS_munmap
  {SYS_munmap,"munmap"},
#endif
#ifdef SYS_mprotect
  {SYS_mprotect,"mprotect"},
#endif
#ifdef SYS_madvise
  {SYS_madvise,"madvise"},
#endif
#ifdef SYS_mincore
  {SYS_mincore,"mincore"},
#endif
#ifdef SYS_getgroups
  {SYS_getgroups,"getgroups"},
#endif
#ifdef SYS_setgroups
  {SYS_setgroups,"setgroups"},
#endif
#ifdef SYS_getpgrp
  {SYS_getpgrp,"getpgrp"},
#endif
#ifdef SYS_setpgid
  {SYS_setpgid,"setpgid"},
#endif
#ifdef SYS_setitimer
  {SYS_setitimer,"setitimer"},
#endif
#ifdef SYS_swapon
  {SYS_swapon,"swapon"},
#endif
#ifdef SYS_getitimer
  {SYS_getitimer,"getitimer"},
#endif
#ifdef SYS_getdtablesize
  {SYS_getdtablesize,"getdtablesize"},
#endif
#ifdef SYS_dup2
  {SYS_dup2,"dup2"},
#endif
#ifdef SYS_fcntl
  {SYS_fcntl,"fcntl"},
#endif
#ifdef SYS_select
  {SYS_select,"select"},
#endif
#ifdef SYS_fsync
  {SYS_fsync,"fsync"},
#endif
#ifdef SYS_setpriority
  {SYS_setpriority,"setpriority"},
#endif
#ifdef SYS_socket
  {SYS_socket,"socket"},
#endif
#ifdef SYS_connect
  {SYS_connect,"connect"},
#endif
#ifdef SYS_getpriority
  {SYS_getpriority,"getpriority"},
#endif
#ifdef SYS_bind
  {SYS_bind,"bind"},
#endif
#ifdef SYS_setsockopt
  {SYS_setsockopt,"setsockopt"},
#endif
#ifdef SYS_listen
  {SYS_listen,"listen"},
#endif
#ifdef SYS_sigsuspend
  {SYS_sigsuspend,"sigsuspend"},
#endif
#ifdef SYS_gettimeofday
  {SYS_gettimeofday,"gettimeofday"},
#endif
#ifdef SYS_getrusage
  {SYS_getrusage,"getrusage"},
#endif
#ifdef SYS_getsockopt
  {SYS_getsockopt,"getsockopt"},
#endif
#ifdef SYS_readv
  {SYS_readv,"readv"},
#endif
#ifdef SYS_writev
  {SYS_writev,"writev"},
#endif
#ifdef SYS_settimeofday
  {SYS_settimeofday,"settimeofday"},
#endif
#ifdef SYS_fchown
  {SYS_fchown,"fchown"},
#endif
#ifdef SYS_fchmod
  {SYS_fchmod,"fchmod"},
#endif
#ifdef SYS_setreuid
  {SYS_setreuid,"setreuid"},
#endif
#ifdef SYS_setregid
  {SYS_setregid,"setregid"},
#endif
#ifdef SYS_rename
  {SYS_rename,"rename"},
#endif
#ifdef SYS_flock
  {SYS_flock,"flock"},
#endif
#ifdef SYS_mkfifo
  {SYS_mkfifo,"mkfifo"},
#endif
#ifdef SYS_sendto
  {SYS_sendto,"sendto"},
#endif
#ifdef SYS_shutdown
  {SYS_shutdown,"shutdown"},
#endif
#ifdef SYS_socketpair
  {SYS_socketpair,"socketpair"},
#endif
#ifdef SYS_mkdir
  {SYS_mkdir,"mkdir"},
#endif
#ifdef SYS_rmdir
  {SYS_rmdir,"rmdir"},
#endif
#ifdef SYS_utimes
  {SYS_utimes,"utimes"},
#endif
#ifdef SYS_futimes
  {SYS_futimes,"futimes"},
#endif
#ifdef SYS_adjtime
  {SYS_adjtime,"adjtime"},
#endif
#ifdef SYS_gethostuuid
  {SYS_gethostuuid,"gethostuuid"},
#endif
#ifdef SYS_setsid
  {SYS_setsid,"setsid"},
#endif
#ifdef SYS_getpgid
  {SYS_getpgid,"getpgid"},
#endif
#ifdef SYS_setprivexec
  {SYS_setprivexec,"setprivexec"},
#endif
#ifdef SYS_pread
  {SYS_pread,"pread"},
#endif
#ifdef SYS_pwrite
  {SYS_pwrite,"pwrite"},
#endif
#ifdef SYS_nfssvc
  {SYS_nfssvc,"nfssvc"},
#endif
#ifdef SYS_statfs
  {SYS_statfs,"statfs"},
#endif
#ifdef SYS_fstatfs
  {SYS_fstatfs,"fstatfs"},
#endif
#ifdef SYS_unmount
  {SYS_unmount,"unmount"},
#endif
#ifdef SYS_getfh
  {SYS_getfh,"getfh"},
#endif
#ifdef SYS_quotactl
  {SYS_quotactl,"quotactl"},
#endif
#ifdef SYS_mount
  {SYS_mount,"mount"},
#endif
#ifdef SYS_csops
  {SYS_csops,"csops"},
#endif
#ifdef SYS_csops_audittoken
  {SYS_csops_audittoken,"csops_audittoken"},
#endif
#ifdef SYS_waitid
  {SYS_waitid,"waitid"},
#endif
#ifdef SYS_kdebug_typefilter
  {SYS_kdebug_typefilter,"kdebug_typefilter"},
#endif
#ifdef SYS_kdebug_trace_string
  {SYS_kdebug_trace_string,"kdebug_trace_string"},
#endif
#ifdef SYS_kdebug_trace64
  {SYS_kdebug_trace64,"kdebug_trace64"},
#endif
#ifdef SYS_kdebug_trace
  {SYS_kdebug_trace,"kdebug_trace"},
#endif
#ifdef SYS_setgid
  {SYS_setgid,"setgid"},
#endif
#ifdef SYS_setegid
  {SYS_setegid,"setegid"},
#endif
#ifdef SYS_seteuid
  {SYS_seteuid,"seteuid"},
#endif
#ifdef SYS_sigreturn
  {SYS_sigreturn,"sigreturn"},
#endif
#ifdef SYS_thread_selfcounts
  {SYS_thread_selfcounts,"thread_selfcounts"},
#endif
#ifdef SYS_fdatasync
  {SYS_fdatasync,"fdatasync"},
#endif
#ifdef SYS_stat
  {SYS_stat,"stat"},
#endif
#ifdef SYS_fstat
  {SYS_fstat,"fstat"},
#endif
#ifdef SYS_lstat
  {SYS_lstat,"lstat"},
#endif
#ifdef SYS_pathconf
  {SYS_pathconf,"pathconf"},
#endif
#ifdef SYS_fpathconf
  {SYS_fpathconf,"fpathconf"},
#endif
#ifdef SYS_getrlimit
  {SYS_getrlimit,"getrlimit"},
#endif
#ifdef SYS_setrlimit
  {SYS_setrlimit,"setrlimit"},
#endif
#ifdef SYS_getdirentries
  {SYS_getdirentries,"getdirentries"},
#endif
#ifdef SYS_mmap
  {SYS_mmap,"mmap"},
#endif
#ifdef SYS_lseek
  {SYS_lseek,"lseek"},
#endif
#ifdef SYS_truncate
  {SYS_truncate,"truncate"},
#endif
#ifdef SYS_ftruncate
  {SYS_ftruncate,"ftruncate"},
#endif
#ifdef SYS_sysctl
  {SYS_sysctl,"sysctl"},
#endif
#ifdef SYS_mlock
  {SYS_mlock,"mlock"},
#endif
#ifdef SYS_munlock
  {SYS_munlock,"munlock"},
#endif
#ifdef SYS_undelete
  {SYS_undelete,"undelete"},
#endif
#ifdef SYS_open_dprotected_np
  {SYS_open_dprotected_np,"open_dprotected_np"},
#endif
#ifdef SYS_fsgetpath_ext
  {SYS_fsgetpath_ext,"fsgetpath_ext"},
#endif
#ifdef SYS_openat_dprotected_np
  {SYS_openat_dprotected_np,"openat_dprotected_np"},
#endif
#ifdef SYS_getattrlist
  {SYS_getattrlist,"getattrlist"},
#endif
#ifdef SYS_setattrlist
  {SYS_setattrlist,"setattrlist"},
#endif
#ifdef SYS_getdirentriesattr
  {SYS_getdirentriesattr,"getdirentriesattr"},
#endif
#ifdef SYS_exchangedata
  {SYS_exchangedata,"exchangedata"},
#endif
#ifdef SYS_searchfs
  {SYS_searchfs,"searchfs"},
#endif
#ifdef SYS_delete
  {SYS_delete,"delete"},
#endif
#ifdef SYS_copyfile
  {SYS_copyfile,"copyfile"},
#endif
#ifdef SYS_fgetattrlist
  {SYS_fgetattrlist,"fgetattrlist"},
#endif
#ifdef SYS_fsetattrlist
  {SYS_fsetattrlist,"fsetattrlist"},
#endif
#ifdef SYS_poll
  {SYS_poll,"poll"},
#endif
#ifdef SYS_getxattr
  {SYS_getxattr,"getxattr"},
#endif
#ifdef SYS_fgetxattr
  {SYS_fgetxattr,"fgetxattr"},
#endif
#ifdef SYS_setxattr
  {SYS_setxattr,"setxattr"},
#endif
#ifdef SYS_fsetxattr
  {SYS_fsetxattr,"fsetxattr"},
#endif
#ifdef SYS_removexattr
  {SYS_removexattr,"removexattr"},
#endif
#ifdef SYS_fremovexattr
  {SYS_fremovexattr,"fremovexattr"},
#endif
#ifdef SYS_listxattr
  {SYS_listxattr,"listxattr"},
#endif
#ifdef SYS_flistxattr
  {SYS_flistxattr,"flistxattr"},
#endif
#ifdef SYS_fsctl
  {SYS_fsctl,"fsctl"},
#endif
#ifdef SYS_initgroups
  {SYS_initgroups,"initgroups"},
#endif
#ifdef SYS_posix_spawn
  {SYS_posix_spawn,"posix_spawn"},
#endif
#ifdef SYS_ffsctl
  {SYS_ffsctl,"ffsctl"},
#endif
#ifdef SYS_fhopen
  {SYS_fhopen,"fhopen"},
#endif
#ifdef SYS_minherit
  {SYS_minherit,"minherit"},
#endif
#ifdef SYS_semsys
  {SYS_semsys,"semsys"},
#endif
#ifdef SYS_msgsys
  {SYS_msgsys,"msgsys"},
#endif
#ifdef SYS_shmsys
  {SYS_shmsys,"shmsys"},
#endif
#ifdef SYS_semctl
  {SYS_semctl,"semctl"},
#endif
#ifdef SYS_semget
  {SYS_semget,"semget"},
#endif
#ifdef SYS_semop
  {SYS_semop,"semop"},
#endif
#ifdef SYS_msgctl
  {SYS_msgctl,"msgctl"},
#endif
#ifdef SYS_msgget
  {SYS_msgget,"msgget"},
#endif
#ifdef SYS_msgsnd
  {SYS_msgsnd,"msgsnd"},
#endif
#ifdef SYS_msgrcv
  {SYS_msgrcv,"msgrcv"},
#endif
#ifdef SYS_shmat
  {SYS_shmat,"shmat"},
#endif
#ifdef SYS_shmctl
  {SYS_shmctl,"shmctl"},
#endif
#ifdef SYS_shmdt
  {SYS_shmdt,"shmdt"},
#endif
#ifdef SYS_shmget
  {SYS_shmget,"shmget"},
#endif
#ifdef SYS_shm_open
  {SYS_shm_open,"shm_open"},
#endif
#ifdef SYS_shm_unlink
  {SYS_shm_unlink,"shm_unlink"},
#endif
#ifdef SYS_sem_open
  {SYS_sem_open,"sem_open"},
#endif
#ifdef SYS_sem_close
  {SYS_sem_close,"sem_close"},
#endif
#ifdef SYS_sem_unlink
  {SYS_sem_unlink,"sem_unlink"},
#endif
#ifdef SYS_sem_wait
  {SYS_sem_wait,"sem_wait"},
#endif
#ifdef SYS_sem_trywait
  {SYS_sem_trywait,"sem_trywait"},
#endif
#ifdef SYS_sem_post
  {SYS_sem_post,"sem_post"},
#endif
#ifdef SYS_sysctlbyname
  {SYS_sysctlbyname,"sysctlbyname"},
#endif
#ifdef SYS_open_extended
  {SYS_open_extended,"open_extended"},
#endif
#ifdef SYS_umask_extended
  {SYS_umask_extended,"umask_extended"},
#endif
#ifdef SYS_stat_extended
  {SYS_stat_extended,"stat_extended"},
#endif
#ifdef SYS_lstat_extended
  {SYS_lstat_extended,"lstat_extended"},
#endif
#ifdef SYS_fstat_extended
  {SYS_fstat_extended,"fstat_extended"},
#endif
#ifdef SYS_chmod_extended
  {SYS_chmod_extended,"chmod_extended"},
#endif
#ifdef SYS_fchmod_extended
  {SYS_fchmod_extended,"fchmod_extended"},
#endif
#ifdef SYS_access_extended
  {SYS_access_extended,"access_extended"},
#endif
#ifdef SYS_settid
  {SYS_settid,"settid"},
#endif
#ifdef SYS_gettid
  {SYS_gettid,"gettid"},
#endif
#ifdef SYS_setsgroups
  {SYS_setsgroups,"setsgroups"},
#endif
#ifdef SYS_getsgroups
  {SYS_getsgroups,"getsgroups"},
#endif
#ifdef SYS_setwgroups
  {SYS_setwgroups,"setwgroups"},
#endif
#ifdef SYS_getwgroups
  {SYS_getwgroups,"getwgroups"},
#endif
#ifdef SYS_mkfifo_extended
  {SYS_mkfifo_extended,"mkfifo_extended"},
#endif
#ifdef SYS_mkdir_extended
  {SYS_mkdir_extended,"mkdir_extended"},
#endif
#ifdef SYS_identitysvc
  {SYS_identitysvc,"identitysvc"},
#endif
#ifdef SYS_shared_region_check_np
  {SYS_shared_region_check_np,"shared_region_check_np"},
#endif
#ifdef SYS_vm_pressure_monitor
  {SYS_vm_pressure_monitor,"vm_pressure_monitor"},
#endif
#ifdef SYS_psynch_rw_longrdlock
  {SYS_psynch_rw_longrdlock,"psynch_rw_longrdlock"},
#endif
#ifdef SYS_psynch_rw_yieldwrlock
  {SYS_psynch_rw_yieldwrlock,"psynch_rw_yieldwrlock"},
#endif
#ifdef SYS_psynch_rw_downgrade
  {SYS_psynch_rw_downgrade,"psynch_rw_downgrade"},
#endif
#ifdef SYS_psynch_rw_upgrade
  {SYS_psynch_rw_upgrade,"psynch_rw_upgrade"},
#endif
#ifdef SYS_psynch_mutexwait
  {SYS_psynch_mutexwait,"psynch_mutexwait"},
#endif
#ifdef SYS_psynch_mutexdrop
  {SYS_psynch_mutexdrop,"psynch_mutexdrop"},
#endif
#ifdef SYS_psynch_cvbroad
  {SYS_psynch_cvbroad,"psynch_cvbroad"},
#endif
#ifdef SYS_psynch_cvsignal
  {SYS_psynch_cvsignal,"psynch_cvsignal"},
#endif
#ifdef SYS_psynch_cvwait
  {SYS_psynch_cvwait,"psynch_cvwait"},
#endif
#ifdef SYS_psynch_rw_rdlock
  {SYS_psynch_rw_rdlock,"psynch_rw_rdlock"},
#endif
#ifdef SYS_psynch_rw_wrlock
  {SYS_psynch_rw_wrlock,"psynch_rw_wrlock"},
#endif
#ifdef SYS_psynch_rw_unlock
  {SYS_psynch_rw_unlock,"psynch_rw_unlock"},
#endif
#ifdef SYS_psynch_rw_unlock2
  {SYS_psynch_rw_unlock2,"psynch_rw_unlock2"},
#endif
#ifdef SYS_getsid
  {SYS_getsid,"getsid"},
#endif
#ifdef SYS_settid_with_pid
  {SYS_settid_with_pid,"settid_with_pid"},
#endif
#ifdef SYS_psynch_cvclrprepost
  {SYS_psynch_cvclrprepost,"psynch_cvclrprepost"},
#endif
#ifdef SYS_aio_fsync
  {SYS_aio_fsync,"aio_fsync"},
#endif
#ifdef SYS_aio_return
  {SYS_aio_return,"aio_return"},
#endif
#ifdef SYS_aio_suspend
  {SYS_aio_suspend,"aio_suspend"},
#endif
#ifdef SYS_aio_cancel
  {SYS_aio_cancel,"aio_cancel"},
#endif
#ifdef SYS_aio_error
  {SYS_aio_error,"aio_error"},
#endif
#ifdef SYS_aio_read
  {SYS_aio_read,"aio_read"},
#endif
#ifdef SYS_aio_write
  {SYS_aio_write,"aio_write"},
#endif
#ifdef SYS_lio_listio
  {SYS_lio_listio,"lio_listio"},
#endif
#ifdef SYS_iopolicysys
  {SYS_iopolicysys,"iopolicysys"},
#endif
#ifdef SYS_process_policy
  {SYS_process_policy,"process_policy"},
#endif
#ifdef SYS_mlockall
  {SYS_mlockall,"mlockall"},
#endif
#ifdef SYS_munlockall
  {SYS_munlockall,"munlockall"},
#endif
#ifdef SYS_issetugid
  {SYS_issetugid,"issetugid"},
#endif
#ifdef SYS___pthread_kill
  {SYS___pthread_kill,"__pthread_kill"},
#endif
#ifdef SYS___pthread_sigmask
  {SYS___pthread_sigmask,"__pthread_sigmask"},
#endif
#ifdef SYS___sigwait
  {SYS___sigwait,"__sigwait"},
#endif
#ifdef SYS___disable_threadsignal
  {SYS___disable_threadsignal,"__disable_threadsignal"},
#endif
#ifdef SYS___pthread_markcancel
  {SYS___pthread_markcancel,"__pthread_markcancel"},
#endif
#ifdef SYS___pthread_canceled
  {SYS___pthread_canceled,"__pthread_canceled"},
#endif
#ifdef SYS___semwait_signal
  {SYS___semwait_signal,"__semwait_signal"},
#endif
#ifdef SYS_proc_info
  {SYS_proc_info,"proc_info"},
#endif
#ifdef SYS_sendfile
  {SYS_sendfile,"sendfile"},
#endif
#ifdef SYS_stat64
  {SYS_stat64,"stat64"},
#endif
#ifdef SYS_fstat64
  {SYS_fstat64,"fstat64"},
#endif
#ifdef SYS_lstat64
  {SYS_lstat64,"lstat64"},
#endif
#ifdef SYS_stat64_extended
  {SYS_stat64_extended,"stat64_extended"},
#endif
#ifdef SYS_lstat64_extended
  {SYS_lstat64_extended,"lstat64_extended"},
#endif
#ifdef SYS_fstat64_extended
  {SYS_fstat64_extended,"fstat64_extended"},
#endif
#ifdef SYS_getdirentries64
  {SYS_getdirentries64,"getdirentries64"},
#endif
#ifdef SYS_statfs64
  {SYS_statfs64,"statfs64"},
#endif
#ifdef SYS_fstatfs64
  {SYS_fstatfs64,"fstatfs64"},
#endif
#ifdef SYS_getfsstat64
  {SYS_getfsstat64,"getfsstat64"},
#endif
#ifdef SYS___pthread_chdir
  {SYS___pthread_chdir,"__pthread_chdir"},
#endif
#ifdef SYS___pthread_fchdir
  {SYS___pthread_fchdir,"__pthread_fchdir"},
#endif
#ifdef SYS_audit
  {SYS_audit,"audit"},
#endif
#ifdef SYS_auditon
  {SYS_auditon,"auditon"},
#endif
#ifdef SYS_getauid
  {SYS_getauid,"getauid"},
#endif
#ifdef SYS_setauid
  {SYS_setauid,"setauid"},
#endif
#ifdef SYS_getaudit_addr
  {SYS_getaudit_addr,"getaudit_addr"},
#endif
#ifdef SYS_setaudit_addr
  {SYS_setaudit_addr,"setaudit_addr"},
#endif
#ifdef SYS_auditctl
  {SYS_auditctl,"auditctl"},
#endif
#ifdef SYS_bsdthread_create
  {SYS_bsdthread_create,"bsdthread_create"},
#endif
#ifdef SYS_bsdthread_terminate
  {SYS_bsdthread_terminate,"bsdthread_terminate"},
#endif
#ifdef SYS_kqueue
  {SYS_kqueue,"kqueue"},
#endif
#ifdef SYS_kevent
  {SYS_kevent,"kevent"},
#endif
#ifdef SYS_lchown
  {SYS_lchown,"lchown"},
#endif
#ifdef SYS_bsdthread_register
  {SYS_bsdthread_register,"bsdthread_register"},
#endif
#ifdef SYS_workq_open
  {SYS_workq_open,"workq_open"},
#endif
#ifdef SYS_workq_kernreturn
  {SYS_workq_kernreturn,"workq_kernreturn"},
#endif
#ifdef SYS_kevent64
  {SYS_kevent64,"kevent64"},
#endif
#ifdef SYS_thread_selfid
  {SYS_thread_selfid,"thread_selfid"},
#endif
#ifdef SYS_ledger
  {SYS_ledger,"ledger"},
#endif
#ifdef SYS_kevent_qos
  {SYS_kevent_qos,"kevent_qos"},
#endif
#ifdef SYS_kevent_id
  {SYS_kevent_id,"kevent_id"},
#endif
#ifdef SYS___mac_execve
  {SYS___mac_execve,"__mac_execve"},
#endif
#ifdef SYS___mac_syscall
  {SYS___mac_syscall,"__mac_syscall"},
#endif
#ifdef SYS___mac_get_file
  {SYS___mac_get_file,"__mac_get_file"},
#endif
#ifdef SYS___mac_set_file
  {SYS___mac_set_file,"__mac_set_file"},
#endif
#ifdef SYS___mac_get_link
  {SYS___mac_get_link,"__mac_get_link"},
#endif
#ifdef SYS___mac_set_link
  {SYS___mac_set_link,"__mac_set_link"},
#endif
#ifdef SYS___mac_get_proc
  {SYS___mac_get_proc,"__mac_get_proc"},
#endif
#ifdef SYS___mac_set_proc
  {SYS___mac_set_proc,"__mac_set_proc"},
#endif
#ifdef SYS___mac_get_fd
  {SYS___mac_get_fd,"__mac_get_fd"},
#endif
#ifdef SYS___mac_set_fd
  {SYS___mac_set_fd,"__mac_set_fd"},
#endif
#ifdef SYS___mac_get_pid
  {SYS___mac_get_pid,"__mac_get_pid"},
#endif
#ifdef SYS_pselect
  {SYS_pselect,"pselect"},
#endif
#ifdef SYS_pselect_nocancel
  {SYS_pselect_nocancel,"pselect_nocancel"},
#endif
#ifdef SYS_read_nocancel
  {SYS_read_nocancel,"read_nocancel"},
#endif
#ifdef SYS_write_nocancel
  {SYS_write_nocancel,"write_nocancel"},
#endif
#ifdef SYS_open_nocancel
  {SYS_open_nocancel,"open_nocancel"},
#endif
#ifdef SYS_close_nocancel
  {SYS_close_nocancel,"close_nocancel"},
#endif
#ifdef SYS_wait4_nocancel
  {SYS_wait4_nocancel,"wait4_nocancel"},
#endif
#ifdef SYS_recvmsg_nocancel
  {SYS_recvmsg_nocancel,"recvmsg_nocancel"},
#endif
#ifdef SYS_sendmsg_nocancel
  {SYS_sendmsg_nocancel,"sendmsg_nocancel"},
#endif
#ifdef SYS_recvfrom_nocancel
  {SYS_recvfrom_nocancel,"recvfrom_nocancel"},
#endif
#ifdef SYS_accept_nocancel
  {SYS_accept_nocancel,"accept_nocancel"},
#endif
#ifdef SYS_msync_nocancel
  {SYS_msync_nocancel,"msync_nocancel"},
#endif
#ifdef SYS_fcntl_nocancel
  {SYS_fcntl_nocancel,"fcntl_nocancel"},
#endif
#ifdef SYS_select_nocancel
  {SYS_select_nocancel,"select_nocancel"},
#endif
#ifdef SYS_fsync_nocancel
  {SYS_fsync_nocancel,"fsync_nocancel"},
#endif
#ifdef SYS_connect_nocancel
  {SYS_connect_nocancel,"connect_nocancel"},
#endif
#ifdef SYS_sigsuspend_nocancel
  {SYS_sigsuspend_nocancel,"sigsuspend_nocancel"},
#endif
#ifdef SYS_readv_nocancel
  {SYS_readv_nocancel,"readv_nocancel"},
#endif
#ifdef SYS_writev_nocancel
  {SYS_writev_nocancel,"writev_nocancel"},
#endif
#ifdef SYS_sendto_nocancel
  {SYS_sendto_nocancel,"sendto_nocancel"},
#endif
#ifdef SYS_pread_nocancel
  {SYS_pread_nocancel,"pread_nocancel"},
#endif
#ifdef SYS_pwrite_nocancel
  {SYS_pwrite_nocancel,"pwrite_nocancel"},
#endif
#ifdef SYS_waitid_nocancel
  {SYS_waitid_nocancel,"waitid_nocancel"},
#endif
#ifdef SYS_poll_nocancel
  {SYS_poll_nocancel,"poll_nocancel"},
#endif
#ifdef SYS_msgsnd_nocancel
  {SYS_msgsnd_nocancel,"msgsnd_nocancel"},
#endif
#ifdef SYS_msgrcv_nocancel
  {SYS_msgrcv_nocancel,"msgrcv_nocancel"},
#endif
#ifdef SYS_sem_wait_nocancel
  {SYS_sem_wait_nocancel,"sem_wait_nocancel"},
#endif
#ifdef SYS_aio_suspend_nocancel
  {SYS_aio_suspend_nocancel,"aio_suspend_nocancel"},
#endif
#ifdef SYS___sigwait_nocancel
  {SYS___sigwait_nocancel,"__sigwait_nocancel"},
#endif
#ifdef SYS___semwait_signal_nocancel
  {SYS___semwait_signal_nocancel,"__semwait_signal_nocancel"},
#endif
#ifdef SYS___mac_mount
  {SYS___mac_mount,"__mac_mount"},
#endif
#ifdef SYS___mac_get_mount
  {SYS___mac_get_mount,"__mac_get_mount"},
#endif
#ifdef SYS___mac_getfsstat
  {SYS___mac_getfsstat,"__mac_getfsstat"},
#endif
#ifdef SYS_fsgetpath
  {SYS_fsgetpath,"fsgetpath"},
#endif
#ifdef SYS_audit_session_self
  {SYS_audit_session_self,"audit_session_self"},
#endif
#ifdef SYS_audit_session_join
  {SYS_audit_session_join,"audit_session_join"},
#endif
#ifdef SYS_fileport_makeport
  {SYS_fileport_makeport,"fileport_makeport"},
#endif
#ifdef SYS_fileport_makefd
  {SYS_fileport_makefd,"fileport_makefd"},
#endif
#ifdef SYS_audit_session_port
  {SYS_audit_session_port,"audit_session_port"},
#endif
#ifdef SYS_pid_suspend
  {SYS_pid_suspend,"pid_suspend"},
#endif
#ifdef SYS_pid_resume
  {SYS_pid_resume,"pid_resume"},
#endif
#ifdef SYS_pid_hibernate
  {SYS_pid_hibernate,"pid_hibernate"},
#endif
#ifdef SYS_pid_shutdown_sockets
  {SYS_pid_shutdown_sockets,"pid_shutdown_sockets"},
#endif
#ifdef SYS_kas_info
  {SYS_kas_info,"kas_info"},
#endif
#ifdef SYS_memorystatus_control
  {SYS_memorystatus_control,"memorystatus_control"},
#endif
#ifdef SYS_guarded_open_np
  {SYS_guarded_open_np,"guarded_open_np"},
#endif
#ifdef SYS_guarded_close_np
  {SYS_guarded_close_np,"guarded_close_np"},
#endif
#ifdef SYS_guarded_kqueue_np
  {SYS_guarded_kqueue_np,"guarded_kqueue_np"},
#endif
#ifdef SYS_change_fdguard_np
  {SYS_change_fdguard_np,"change_fdguard_np"},
#endif
#ifdef SYS_usrctl
  {SYS_usrctl,"usrctl"},
#endif
#ifdef SYS_proc_rlimit_control
  {SYS_proc_rlimit_control,"proc_rlimit_control"},
#endif
#ifdef SYS_connectx
  {SYS_connectx,"connectx"},
#endif
#ifdef SYS_disconnectx
  {SYS_disconnectx,"disconnectx"},
#endif
#ifdef SYS_peeloff
  {SYS_peeloff,"peeloff"},
#endif
#ifdef SYS_socket_delegate
  {SYS_socket_delegate,"socket_delegate"},
#endif
#ifdef SYS_telemetry
  {SYS_telemetry,"telemetry"},
#endif
#ifdef SYS_proc_uuid_policy
  {SYS_proc_uuid_policy,"proc_uuid_policy"},
#endif
#ifdef SYS_memorystatus_get_level
  {SYS_memorystatus_get_level,"memorystatus_get_level"},
#endif
#ifdef SYS_system_override
  {SYS_system_override,"system_override"},
#endif
#ifdef SYS_vfs_purge
  {SYS_vfs_purge,"vfs_purge"},
#endif
#ifdef SYS_sfi_ctl
  {SYS_sfi_ctl,"sfi_ctl"},
#endif
#ifdef SYS_sfi_pidctl
  {SYS_sfi_pidctl,"sfi_pidctl"},
#endif
#ifdef SYS_coalition
  {SYS_coalition,"coalition"},
#endif
#ifdef SYS_coalition_info
  {SYS_coalition_info,"coalition_info"},
#endif
#ifdef SYS_necp_match_policy
  {SYS_necp_match_policy,"necp_match_policy"},
#endif
#ifdef SYS_getattrlistbulk
  {SYS_getattrlistbulk,"getattrlistbulk"},
#endif
#ifdef SYS_clonefileat
  {SYS_clonefileat,"clonefileat"},
#endif
#ifdef SYS_openat
  {SYS_openat,"openat"},
#endif
#ifdef SYS_openat_nocancel
  {SYS_openat_nocancel,"openat_nocancel"},
#endif
#ifdef SYS_renameat
  {SYS_renameat,"renameat"},
#endif
#ifdef SYS_faccessat
  {SYS_faccessat,"faccessat"},
#endif
#ifdef SYS_fchmodat
  {SYS_fchmodat,"fchmodat"},
#endif
#ifdef SYS_fchownat
  {SYS_fchownat,"fchownat"},
#endif
#ifdef SYS_fstatat
  {SYS_fstatat,"fstatat"},
#endif
#ifdef SYS_fstatat64
  {SYS_fstatat64,"fstatat64"},
#endif
#ifdef SYS_linkat
  {SYS_linkat,"linkat"},
#endif
#ifdef SYS_unlinkat
  {SYS_unlinkat,"unlinkat"},
#endif
#ifdef SYS_readlinkat
  {SYS_readlinkat,"readlinkat"},
#endif
#ifdef SYS_symlinkat
  {SYS_symlinkat,"symlinkat"},
#endif
#ifdef SYS_mkdirat
  {SYS_mkdirat,"mkdirat"},
#endif
#ifdef SYS_getattrlistat
  {SYS_getattrlistat,"getattrlistat"},
#endif
#ifdef SYS_proc_trace_log
  {SYS_proc_trace_log,"proc_trace_log"},
#endif
#ifdef SYS_bsdthread_ctl
  {SYS_bsdthread_ctl,"bsdthread_ctl"},
#endif
#ifdef SYS_openbyid_np
  {SYS_openbyid_np,"openbyid_np"},
#endif
#ifdef SYS_recvmsg_x
  {SYS_recvmsg_x,"recvmsg_x"},
#endif
#ifdef SYS_sendmsg_x
  {SYS_sendmsg_x,"sendmsg_x"},
#endif
#ifdef SYS_thread_selfusage
  {SYS_thread_selfusage,"thread_selfusage"},
#endif
#ifdef SYS_csrctl
  {SYS_csrctl,"csrctl"},
#endif
#ifdef SYS_guarded_open_dprotected_np
  {SYS_guarded_open_dprotected_np,"guarded_open_dprotected_np"},
#endif
#ifdef SYS_guarded_write_np
  {SYS_guarded_write_np,"guarded_write_np"},
#endif
#ifdef SYS_guarded_pwrite_np
  {SYS_guarded_pwrite_np,"guarded_pwrite_np"},
#endif
#ifdef SYS_guarded_writev_np
  {SYS_guarded_writev_np,"guarded_writev_np"},
#endif
#ifdef SYS_renameatx_np
  {SYS_renameatx_np,"renameatx_np"},
#endif
#ifdef SYS_mremap_encrypted
  {SYS_mremap_encrypted,"mremap_encrypted"},
#endif
#ifdef SYS_netagent_trigger
  {SYS_netagent_trigger,"netagent_trigger"},
#endif
#ifdef SYS_stack_snapshot_with_config
  {SYS_stack_snapshot_with_config,"stack_snapshot_with_config"},
#endif
#ifdef SYS_microstackshot
  {SYS_microstackshot,"microstackshot"},
#endif
#ifdef SYS_grab_pgo_data
  {SYS_grab_pgo_data,"grab_pgo_data"},
#endif
#ifdef SYS_persona
  {SYS_persona,"persona"},
#endif
#ifdef SYS_mach_eventlink_signal
  {SYS_mach_eventlink_signal,"mach_eventlink_signal"},
#endif
#ifdef SYS_mach_eventlink_wait_until
  {SYS_mach_eventlink_wait_until,"mach_eventlink_wait_until"},
#endif
#ifdef SYS_mach_eventlink_signal_wait_until
  {SYS_mach_eventlink_signal_wait_until,"mach_eventlink_signal_wait_until"},
#endif
#ifdef SYS_work_interval_ctl
  {SYS_work_interval_ctl,"work_interval_ctl"},
#endif
#ifdef SYS_getentropy
  {SYS_getentropy,"getentropy"},
#endif
#ifdef SYS_necp_open
  {SYS_necp_open,"necp_open"},
#endif
#ifdef SYS_necp_client_action
  {SYS_necp_client_action,"necp_client_action"},
#endif
#ifdef SYS___nexus_open
  {SYS___nexus_open,"__nexus_open"},
#endif
#ifdef SYS___nexus_register
  {SYS___nexus_register,"__nexus_register"},
#endif
#ifdef SYS___nexus_deregister
  {SYS___nexus_deregister,"__nexus_deregister"},
#endif
#ifdef SYS___nexus_create
  {SYS___nexus_create,"__nexus_create"},
#endif
#ifdef SYS___nexus_destroy
  {SYS___nexus_destroy,"__nexus_destroy"},
#endif
#ifdef SYS___nexus_get_opt
  {SYS___nexus_get_opt,"__nexus_get_opt"},
#endif
#ifdef SYS___nexus_set_opt
  {SYS___nexus_set_opt,"__nexus_set_opt"},
#endif
#ifdef SYS___channel_open
  {SYS___channel_open,"__channel_open"},
#endif
#ifdef SYS___channel_get_info
  {SYS___channel_get_info,"__channel_get_info"},
#endif
#ifdef SYS___channel_sync
  {SYS___channel_sync,"__channel_sync"},
#endif
#ifdef SYS___channel_get_opt
  {SYS___channel_get_opt,"__channel_get_opt"},
#endif
#ifdef SYS___channel_set_opt
  {SYS___channel_set_opt,"__channel_set_opt"},
#endif
#ifdef SYS_ulock_wait
  {SYS_ulock_wait,"ulock_wait"},
#endif
#ifdef SYS_ulock_wake
  {SYS_ulock_wake,"ulock_wake"},
#endif
#ifdef SYS_fclonefileat
  {SYS_fclonefileat,"fclonefileat"},
#endif
#ifdef SYS_fs_snapshot
  {SYS_fs_snapshot,"fs_snapshot"},
#endif
#ifdef SYS_register_uexc_handler
  {SYS_register_uexc_handler,"register_uexc_handler"},
#endif
#ifdef SYS_terminate_with_payload
  {SYS_terminate_with_payload,"terminate_with_payload"},
#endif
#ifdef SYS_abort_with_payload
  {SYS_abort_with_payload,"abort_with_payload"},
#endif
#ifdef SYS_necp_session_open
  {SYS_necp_session_open,"necp_session_open"},
#endif
#ifdef SYS_necp_session_action
  {SYS_necp_session_action,"necp_session_action"},
#endif
#ifdef SYS_setattrlistat
  {SYS_setattrlistat,"setattrlistat"},
#endif
#ifdef SYS_net_qos_guideline
  {SYS_net_qos_guideline,"net_qos_guideline"},
#endif
#ifdef SYS_fmount
  {SYS_fmount,"fmount"},
#endif
#ifdef SYS_ntp_adjtime
  {SYS_ntp_adjtime,"ntp_adjtime"},
#endif
#ifdef SYS_ntp_gettime
  {SYS_ntp_gettime,"ntp_gettime"},
#endif
#ifdef SYS_os_fault_with_payload
  {SYS_os_fault_with_payload,"os_fault_with_payload"},
#endif
#ifdef SYS_kqueue_workloop_ctl
  {SYS_kqueue_workloop_ctl,"kqueue_workloop_ctl"},
#endif
#ifdef SYS___mach_bridge_remote_time
  {SYS___mach_bridge_remote_time,"__mach_bridge_remote_time"},
#endif
#ifdef SYS_coalition_ledger
  {SYS_coalition_ledger,"coalition_ledger"},
#endif
#ifdef SYS_log_data
  {SYS_log_data,"log_data"},
#endif
#ifdef SYS_memorystatus_available_memory
  {SYS_memorystatus_available_memory,"memorystatus_available_memory"},
#endif
#ifdef SYS_objc_bp_assist_cfg_np
  {SYS_objc_bp_assist_cfg_np,"objc_bp_assist_cfg_np"},
#endif
#ifdef SYS_shared_region_map_and_slide_2_np
  {SYS_shared_region_map_and_slide_2_np,"shared_region_map_and_slide_2_np"},
#endif
#ifdef SYS_pivot_root
  {SYS_pivot_root,"pivot_root"},
#endif
#ifdef SYS_task_inspect_for_pid
  {SYS_task_inspect_for_pid,"task_inspect_for_pid"},
#endif
#ifdef SYS_task_read_for_pid
  {SYS_task_read_for_pid,"task_read_for_pid"},
#endif
#ifdef SYS_preadv
  {SYS_preadv,"preadv"},
#endif
#ifdef SYS_pwritev
  {SYS_pwritev,"pwritev"},
#endif
#ifdef SYS_preadv_nocancel
  {SYS_preadv_nocancel,"preadv_nocancel"},
#endif
#ifdef SYS_pwritev_nocancel
  {SYS_pwritev_nocancel,"pwritev_nocancel"},
#endif
#ifdef SYS_ulock_wait2
  {SYS_ulock_wait2,"ulock_wait2"},
#endif
#ifdef SYS_proc_info_extended_id
  {SYS_proc_info_extended_id,"proc_info_extended_id"},
#endif
#ifdef SYS_tracker_action
  {SYS_tracker_action,"tracker_action"},
#endif
#ifdef SYS_debug_syscall_reject
  {SYS_debug_syscall_reject,"debug_syscall_reject"},
#endif
#ifdef SYS_debug_syscall_reject_config
  {SYS_debug_syscall_reject_config,"debug_syscall_reject_config"},
#endif
#ifdef SYS_graftdmg
  {SYS_graftdmg,"graftdmg"},
#endif
#ifdef SYS_map_with_linking_np
  {SYS_map_with_linking_np,"map_with_linking_np"},
#endif
#ifdef SYS_freadlink
  {SYS_freadlink,"freadlink"},
#endif
#ifdef SYS_record_system_event
  {SYS_record_system_event,"record_system_event"},
#endif
#ifdef SYS_mkfifoat
  {SYS_mkfifoat,"mkfifoat"},
#endif
#ifdef SYS_mknodat
  {SYS_mknodat,"mknodat"},
#endif
#ifdef SYS_ungraftdmg
  {SYS_ungraftdmg,"ungraftdmg"},
#endif
#ifdef SYS_invalid
  {SYS_invalid,"invalid"},
#endif
// clang-format on
