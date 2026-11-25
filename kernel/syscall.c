#include <arch/syscall.h>
#include <brk/assert.h>
#include <brk/bits.h>
#include <brk/cpu.h>
#include <brk/lengthof.h>
#include <brk/panic.h>
#include <brk/printk.h>
#include <brk/signal.h>
#include <brk/syscall.h>
#include <brk/task.h>
#include <stdint.h>
#include <uapi/errno.h>

#define SYSCALL_RESTARTABLE BIT(0)
#define SYSCALL_ARG_PTR(n) BIT(n)

struct syscall_desc {
	const char *name;
	u8 nargs;
	u8 user_ptr_mask;
	u8 flags;
	long (*fn)(const struct syscall_ctx *ctx);
};
#define SYSCALL_DESC(sname, argcnt, ptrmask, sflags, hfn) \
	{ .name = (sname),                                \
	  .nargs = (argcnt),                              \
	  .user_ptr_mask = (ptrmask),                     \
	  .flags = (sflags),                              \
	  .fn = (hfn) }

static const struct syscall_desc systable[] = {
	[SYS_READ] = SYSCALL_DESC("read", 3, SYSCALL_ARG_PTR(1),
				  SYSCALL_RESTARTABLE, sys_read),
	[SYS_WRITE] = SYSCALL_DESC("write", 3, SYSCALL_ARG_PTR(1),
				   SYSCALL_RESTARTABLE, sys_write),
	[SYS_READ_IOV] = SYSCALL_DESC("read_iov", 3, SYSCALL_ARG_PTR(1),
				      SYSCALL_RESTARTABLE, sys_read_iov),
	[SYS_WRITE_IOV] = SYSCALL_DESC("write_iov", 3, SYSCALL_ARG_PTR(1),
				       SYSCALL_RESTARTABLE, sys_write_iov),
	[SYS_EXIT] = SYSCALL_DESC("exit", 1, 0, 0, sys_exit),
	[SYS_EXIT_ALL] = SYSCALL_DESC("exit_all", 1, 0, 0, sys_exit_all),
	[SYS_CREATE_THREAD] = SYSCALL_DESC(
		"create_thread", 1, SYSCALL_ARG_PTR(0), 0, sys_create_thread),
	[SYS_FUTEX] = SYSCALL_DESC("futex", 6,
				   SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(3) |
					   SYSCALL_ARG_PTR(4),
				   SYSCALL_RESTARTABLE, sys_futex),
	[SYS_CLOSE] = SYSCALL_DESC("close", 1, 0, 0, sys_close),
	[SYS_SET_SIGNAL] = SYSCALL_DESC("set_signal", 3,
					SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2),
					0, sys_set_signal),
	[SYS_SET_SIGMASK] = SYSCALL_DESC(
		"set_sigmask", 3, SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2), 0,
		sys_set_sigmask),
	[SYS_GET_PENDING] = SYSCALL_DESC("get_pending", 1, SYSCALL_ARG_PTR(0),
					 0, sys_get_pending),
	[SYS_WAIT_SIGNAL] = SYSCALL_DESC(
		"wait_signal", 4,
		SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2),
		SYSCALL_RESTARTABLE, sys_wait_signal),
	[SYS_RESTORE_CONTEXT] =
		SYSCALL_DESC("restore_context", 0, 0, 0, sys_restore_context),
	[SYS_IOCTL] = SYSCALL_DESC("ioctl", 3, 0, 0, sys_ioctl),
	[SYS_TRUNCATE] = SYSCALL_DESC("truncate", 2, SYSCALL_ARG_PTR(0), 0,
				      sys_truncate),
	[SYS_TRUNCATE_FD] =
		SYSCALL_DESC("truncate_fd", 2, 0, 0, sys_truncate_fd),
	[SYS_CHMOD] =
		SYSCALL_DESC("chmod", 4, SYSCALL_ARG_PTR(1), 0, sys_chmod),
	[SYS_CHOWN] =
		SYSCALL_DESC("chown", 5, SYSCALL_ARG_PTR(1), 0, sys_chown),
	[SYS_SET_FILE_TIMES] = SYSCALL_DESC(
		"set_file_times", 4, SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2), 0,
		sys_set_file_times),
	[SYS_OPEN] = SYSCALL_DESC("open", 4, SYSCALL_ARG_PTR(1), 0, sys_open),
	[SYS_DUP] = SYSCALL_DESC("dup", 1, 0, 0, sys_dup),
	[SYS_DUP_TO] = SYSCALL_DESC("dup_to", 3, 0, 0, sys_dup_to),
	[SYS_EXEC] = SYSCALL_DESC("exec", 3,
				  SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(1) |
					  SYSCALL_ARG_PTR(2),
				  0, sys_exec),
	[SYS_GET_PROC] = SYSCALL_DESC("get_proc", 1, SYSCALL_ARG_PTR(0), 0,
				      sys_get_proc),
	[SYS_SET_PGID] = SYSCALL_DESC("set_pgid", 2, 0, 0, sys_set_pgid),
	[SYS_SET_SID] = SYSCALL_DESC("set_sid", 0, 0, 0, sys_set_sid),
	[SYS_GET_PGID] = SYSCALL_DESC("get_pgid", 1, 0, 0, sys_get_pgid),
	[SYS_GET_SID] = SYSCALL_DESC("get_sid", 1, 0, 0, sys_get_sid),
	[SYS_MKDIR] =
		SYSCALL_DESC("mkdir", 3, SYSCALL_ARG_PTR(1), 0, sys_mkdir),
	[SYS_CHDIR] =
		SYSCALL_DESC("chdir", 1, SYSCALL_ARG_PTR(0), 0, sys_chdir),
	[SYS_GET_CWD] =
		SYSCALL_DESC("get_cwd", 2, SYSCALL_ARG_PTR(0), 0, sys_get_cwd),
	[SYS_MKNOD] =
		SYSCALL_DESC("mknod", 4, SYSCALL_ARG_PTR(1), 0, sys_mknod),
	[SYS_LINK] = SYSCALL_DESC("link", 5,
				  SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(3), 0,
				  sys_link),
	[SYS_UNLINK] =
		SYSCALL_DESC("unlink", 3, SYSCALL_ARG_PTR(1), 0, sys_unlink),
	[SYS_PIPE] = SYSCALL_DESC("pipe", 2, SYSCALL_ARG_PTR(0), 0, sys_pipe),
	[SYS_GET_SYSINFO] = SYSCALL_DESC("get_sysinfo", 1, SYSCALL_ARG_PTR(0),
					 0, sys_get_sysinfo),
	[SYS_READ_DIR] = SYSCALL_DESC("read_dir", 4, SYSCALL_ARG_PTR(1), 0,
				      sys_read_dir),
	[SYS_WAIT] = SYSCALL_DESC("wait", 4, SYSCALL_ARG_PTR(2),
				  SYSCALL_RESTARTABLE, sys_wait),
	[SYS_FORK] = SYSCALL_DESC("fork", 0, 0, 0, sys_fork),
	[SYS_GET_TIMES] = SYSCALL_DESC("get_times", 1, SYSCALL_ARG_PTR(0), 0,
				       sys_get_times),
	[SYS_STAT] = SYSCALL_DESC("stat", 4,
				  SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(3), 0,
				  sys_stat),
	[SYS_ACCESS] =
		SYSCALL_DESC("access", 4, SYSCALL_ARG_PTR(1), 0, sys_access),
	[SYS_STAT_FS] = SYSCALL_DESC("stat_fs", 2,
				     SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(1), 0,
				     sys_stat_fs),
	[SYS_STAT_FS_FD] = SYSCALL_DESC("stat_fs_fd", 2, SYSCALL_ARG_PTR(1), 0,
					sys_stat_fs_fd),
	[SYS_PREALLOCATE] =
		SYSCALL_DESC("preallocate", 4, 0, 0, sys_preallocate),
	[SYS_GET_CLOCK] = SYSCALL_DESC("get_clock", 2, SYSCALL_ARG_PTR(1), 0,
				       sys_get_clock),
	[SYS_SET_CLOCK] = SYSCALL_DESC("set_clock", 2, SYSCALL_ARG_PTR(1), 0,
				       sys_set_clock),
	[SYS_SLEEP_CLOCK] = SYSCALL_DESC(
		"sleep_clock", 4, SYSCALL_ARG_PTR(2) | SYSCALL_ARG_PTR(3),
		SYSCALL_RESTARTABLE, sys_sleep_clock),
	[SYS_MMAP] = SYSCALL_DESC("mmap", 6, 0, 0, sys_mmap),
	[SYS_MUNMAP] = SYSCALL_DESC("munmap", 2, 0, 0, sys_munmap),
	[SYS_MSYNC] = SYSCALL_DESC("msync", 3, 0, 0, sys_msync),
	[SYS_YIELD] = SYSCALL_DESC("yield", 0, 0, 0, sys_yield),
	[SYS_SEND_SIGNAL] =
		SYSCALL_DESC("send_signal", 2, 0, 0, sys_send_signal),
	[SYS_CHDIR_FD] = SYSCALL_DESC("chdir_fd", 1, 0, 0, sys_chdir_fd),
	[SYS_MPROTECT] = SYSCALL_DESC("mprotect", 3, 0, 0, sys_mprotect),
	[SYS_RENAME] = SYSCALL_DESC("rename", 4,
				    SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(3), 0,
				    sys_rename),
	[SYS_SYMLINK] = SYSCALL_DESC("symlink", 3,
				     SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(2), 0,
				     sys_symlink),
	[SYS_READLINK] = SYSCALL_DESC("readlink", 4,
				      SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2),
				      0, sys_readlink),
	[SYS_SEEK] = SYSCALL_DESC("seek", 3, 0, 0, sys_seek),
	[SYS_SYNC_FD] = SYSCALL_DESC("sync_fd", 1, 0, 0, sys_sync_fd),
	[SYS_SYNC_DATA_FD] =
		SYSCALL_DESC("sync_data_fd", 1, 0, 0, sys_sync_data_fd),
	[SYS_SYNC] = SYSCALL_DESC("sync", 0, 0, 0, sys_sync),
	[SYS_SET_UID_REAL] =
		SYSCALL_DESC("set_uid_real", 1, 0, 0, sys_set_uid_real),
	[SYS_SET_GID_REAL] =
		SYSCALL_DESC("set_gid_real", 1, 0, 0, sys_set_gid_real),
	[SYS_SET_UID_EFFECTIVE] = SYSCALL_DESC("set_uid_effective", 1, 0, 0,
					       sys_set_uid_effective),
	[SYS_SET_GID_EFFECTIVE] = SYSCALL_DESC("set_gid_effective", 1, 0, 0,
					       sys_set_gid_effective),
	[SYS_SET_UID_SAVED] =
		SYSCALL_DESC("set_uid_saved", 1, 0, 0, sys_set_uid_saved),
	[SYS_SET_GID_SAVED] =
		SYSCALL_DESC("set_gid_saved", 1, 0, 0, sys_set_gid_saved),
	[SYS_SET_UMASK] = SYSCALL_DESC("set_umask", 1, 0, 0, sys_set_umask),
	[SYS_CREATE_SOCKET] =
		SYSCALL_DESC("create_socket", 3, 0, 0, sys_create_socket),
	[SYS_CONNECT] = SYSCALL_DESC("connect", 3, SYSCALL_ARG_PTR(1),
				     SYSCALL_RESTARTABLE, sys_connect),
	[SYS_ACCEPT] = SYSCALL_DESC("accept", 4,
				    SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2),
				    SYSCALL_RESTARTABLE, sys_accept),
	[SYS_SEND_TO] = SYSCALL_DESC("send_to", 6,
				     SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(4),
				     SYSCALL_RESTARTABLE, sys_send_to),
	[SYS_RECV_FROM] = SYSCALL_DESC("recv_from", 6,
				       SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(4) |
					       SYSCALL_ARG_PTR(5),
				       SYSCALL_RESTARTABLE, sys_recv_from),
	[SYS_SHUTDOWN] = SYSCALL_DESC("shutdown", 2, 0, 0, sys_shutdown),
	[SYS_BIND] = SYSCALL_DESC("bind", 3, SYSCALL_ARG_PTR(1), 0, sys_bind),
	[SYS_LISTEN] = SYSCALL_DESC("listen", 2, 0, 0, sys_listen),
	[SYS_GET_SOCK_ADDR] = SYSCALL_DESC(
		"get_sock_addr", 3, SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2), 0,
		sys_get_sock_addr),
	[SYS_GET_PEER_ADDR] = SYSCALL_DESC(
		"get_peer_addr", 3, SYSCALL_ARG_PTR(1) | SYSCALL_ARG_PTR(2), 0,
		sys_get_peer_addr),
	[SYS_SET_SOCK_OPT] = SYSCALL_DESC("set_sock_opt", 5, SYSCALL_ARG_PTR(3),
					  0, sys_set_sock_opt),
	[SYS_GET_SOCK_OPT] = SYSCALL_DESC(
		"get_sock_opt", 5, SYSCALL_ARG_PTR(3) | SYSCALL_ARG_PTR(4), 0,
		sys_get_sock_opt),
	[SYS_POLL] = SYSCALL_DESC("poll", 4,
				  SYSCALL_ARG_PTR(0) | SYSCALL_ARG_PTR(2) |
					  SYSCALL_ARG_PTR(3),
				  SYSCALL_RESTARTABLE, sys_poll),
};

static_assert(SYS_COUNT == lengthof(systable),
	      "syscall table size does not match SYS_COUNT");

void syscall(void)
{
	struct thread *thread = current_thread();
	struct trap_frame *regs = thread->frame;
	struct syscall_ctx ctx = {
		.regs = regs,
	};
	const struct syscall_desc *desc;
	long ret;

	ctx.nr = arch_syscall_nr(regs);
	for (unsigned int i = 0; i < SYSCALL_MAX_ARGS; i++)
		ctx.args[i] = arch_syscall_arg(regs, i);

	if (ctx.nr >= SYS_COUNT || !(desc = &systable[ctx.nr])->fn) {
		ret = -ENOSYS;
		goto out;
	}

	ret = desc->fn(&ctx);
	if (ret != -ERESTART)
		goto out;

	/*
	 * ERESTART is internal.  A pending signal must interrupt the
	 * syscall and run its handler; otherwise rewind the user PC so
	 * the ecall is executed again after returning to user mode.
	 */
	if (!(desc->flags & SYSCALL_RESTARTABLE) || signal_pending(thread) ||
	    process_group_exiting(thread->process)) {
		ret = -EINTR;
	} else {
		arch_syscall_restart(regs);
		ret = 0;
	}

out:
	arch_syscall_set_ret(regs, ret);
}

u64 syscall_arg_raw(const struct syscall_ctx *ctx, unsigned int argno)
{
	if (argno >= SYSCALL_MAX_ARGS)
		panic("%s(): illegal argument number: %u\n", __func__, argno);
	return ctx->args[argno];
}

void *syscall_arg_ptr(const struct syscall_ctx *ctx, unsigned int argno)
{
	return (void *)(uintptr_t)syscall_arg_raw(ctx, argno);
}

int syscall_arg_int(const struct syscall_ctx *ctx, unsigned int argno)
{
	return (int)syscall_arg_raw(ctx, argno);
}

long syscall_arg_long(const struct syscall_ctx *ctx, unsigned int argno)
{
	return (long)syscall_arg_raw(ctx, argno);
}

size_t syscall_arg_size(const struct syscall_ctx *ctx, unsigned int argno)
{
	return (size_t)syscall_arg_raw(ctx, argno);
}

user_addr_t syscall_arg_user(const struct syscall_ctx *ctx, unsigned int argno)
{
	return (user_addr_t)syscall_arg_raw(ctx, argno);
}
