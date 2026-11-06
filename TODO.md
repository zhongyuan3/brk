# TODO — 移除/替代非 POSIX 系统调用

> 目标：按「能删的直接删；可用 POSIX 标准接口替代的，实现后者再删；无 POSIX 替代的也删，连带清理用户库涉及的接口/命令」的标准，清理 Linux 专属系统调用。
>
> ABI 单源：`brk-user` 通过 `KERNEL_ROOT ?= ../brk` 直接引用 `src/include/uapi`。新 syscall 编号先占位表尾，**最后统一 renumber 0..N**。

---

## Phase 0 — 基线

- [ ] 两个仓库 `git status` 干净；各开工作分支，内核/用户库成对提交
- [ ] `make run` 起机，`brktest` 全绿，记录通过项数；`brk-user` 全量编译通过
- [ ] 标记待迁移的 `brktest` 用例：
  - 权限组：capget/capset 用例（约 1015–1040、1485 行）
  - 属组：`setgroups` 用例（约 857、872 行）
  - fs 用量：`statfs("/")` 检查（约 401 行）
  - fork/wait、目录遍历相关用例清单

## Phase 1 — 纯删除（无 POSIX 替代、用户态零使用）

**内核侧统一改法**：每项：`uapi/syscall.h` 删常量 → `brk/syscall.h` 删原型 → 删实现 → `kern/syscall.c` 删表项。

- [ ] `SYS_gettid` — `kern/sysproc.c:78`
- [ ] `SYS_syncfs` — `fs/sysfile.c:1395`
- [ ] `SYS_renameat2` — `fs/sysfile.c:537`
- [ ] `SYS_futimesat` — `fs/sysfile.c:1309`
- [ ] `SYS_setfsuid` / `SYS_setfsgid` — `kern/sysproc.c:326/339`（内部 `fsuid` 字段保留）
- [ ] `SYS_mremap` — `mm/mmap.c:1444 sys_mremap` + `mm/mmap.c:717 uvm_mremap`（约 150 行）+ uapi/mman.h 内 `MREMAP_*` 常量
- [ ] `SYS_clone` — `kern/sysproc.c:26`（现为 `ENOSYS` 存根）
- [ ] `SYS_getdents`（旧 32 位版）— `fs/sysfile.c:768`

用户侧：无引用，无需改动。
验证：两仓库全量编译通过。

## Phase 2 — faccessat2 合并进 POSIX 形状的 faccessat

> POSIX.1-2024 给 `faccessat()` 增加 `flags`（AT_EACCESS / AT_SYMLINK_NOFOLLOW）；Linux 的 `faccessat2` 是同一功能、另一个名字。

- [ ] `fs/sysfile.c`：`sys_faccessat` 升级为 4 参 `faccessat(dirfd, path, amode, flags)`，合并 `sys_faccessat2`（:269）的 flags 逻辑
- [ ] 删 `sys_faccessat2` / 表项 / 原型 / 常量
- [ ] `brk-user` `libbrkc/unistd/access.c`：`SYS_faccessat2` → `SYS_faccessat`（签名不变，POSIX 形状即 4 参）
- [ ] 验证：brktest 权限用例 + cp/rm/test 冒烟

## Phase 3 — fallocate → posix_fallocate

> POSIX.1-2024 把预分配标准化为 `posix_fallocate()`，无 `mode` 参数。

- [ ] uapi 新增 `SYS_posix_fallocate(fd, offset, len)`（POSIX 无 mode 参数）
- [ ] 内核：复用 `fs/sysfile.c:347 sys_fallocate` 核心逻辑，mode 固定 0
- [ ] `libbrkc/stat/fallocate.c` → `posix_fallocate.c`，函数改 `posix_fallocate`；更新头文件声明
- [ ] `cmd/fallocate.c` 改调 `posix_fallocate`，命令名保留 `fallocate`
- [ ] 删 `SYS_fallocate`、`sys_fallocate`、uapi `FALLOC_FL_*`
- [ ] 验证：fallocate 命令 + brktest 相关用例

## Phase 4 — getdents / getdents64 → posix_getdents（大项）

> 关键点：用户库**不走 readdir 封装**，cp/rm/mv/chmod/chown/chgrp/ps/init/sh_edit 等 10+ 命令**直接手写 `getdents64` + `struct dirent64` 遍历**（dbuf + d_reclen 步进）。libc 与命令两侧都要改。

- [ ] uapi/dirent.h 按 Issue 8 新增：
  ```c
  typedef __kernel_ulong_t reclen_t;
  struct posix_dent { ino_t d_ino; reclen_t d_reclen; unsigned char d_type; char d_name[]; };
  ```
  保留 `DT_*`；`struct dirent` 保留为 `readdir()` 返回结构
- [ ] uapi 新增 `SYS_posix_getdents(fd, buf, nbyte, flags)`
- [ ] 内核 `fs/sysfile.c`：`getdents64_context`/`getdents64_filldir`（:827–900）改填 `struct posix_dent`（d_reclen 含末尾 padding 按 alignof 重算），新增 `sys_posix_getdents`；offset/lseek 语义沿用现有 dir iterator
- [ ] libc：`unistd/getdents64.c` 删；dirent/stdio `opendir/readdir` 改在 `posix_dent` 上实现
- [ ] 顺手抽公共 helper（如 `dir_foreach(dirfd, cb)`）以降低重复
- [ ] 手写遍历的命令全部切换 `struct dirent64 *` → `struct posix_dent *`：
  - `init.c:46`、`ps.c:128`、`chown.c:109`、`chgrp.c`、`chmod.c`、`cp.c:130`、`mv.c:126/206`、`rm.c:88`、`sh_edit.c:354`
- [ ] 删 `SYS_getdents64`、`SYS_getdents`、uapi `struct dirent64`
- [ ] `brk-user` `libbrkc/misc/abi_check.c:48/49` dirent64 断言改 `posix_dent`（附 `sizeof(ino_t)==8`）
- [ ] 验证：ls、cp -r、rm -r、mv、ps + brktest 全部目录用例

## Phase 5 — statfs / fstatfs → statvfs / fstatvfs

- [ ] uapi 新增 `uapi/statvfs.h`，新增 `SYS_statvfs(path, struct statvfs*)` / `SYS_fstatvfs(fd, ...)`
  - 字段：`f_bsize`、`f_frsize`、`f_blocks`、`f_bfree`、`f_bavail`、`f_files`、`f_ffree`、`f_favail`、`f_fsid`、`f_flag`（ST_NOSUID/ST_RDONLY）、`f_namemax`
- [ ] 内核 `fs/sysfile.c:313 sys_statfs`/`sys_fstatfs` 改写为 `sys_statvfs/sys_fstatvfs`（同统计源，字段 `f_bsize→f_frsize` 语义对照；f_namemax 从超级块或常数）
- [ ] `libbrkc/stat/statfs.c` → `statvfs.c`；`sys/statfs.h` → 删；新增 `sys/statvfs.h`（检查 `pwd`/brklib 上 statfs.h include）
- [ ] uapi `uapi/statfs.h` 删除（内核内部结构保留）；`uapi/statfs.h` include 处改造
- [ ] `cmd/df.c` 字段全改 statvfs 命名；`cmd/brktest.c:401` 同步
- [ ] 验证：`df`、brktest fs 用量用例

## Phase 6 — wait4 → waitid（大项）

- [ ] uapi/signal.h **新增 `siginfo_t`**（POSIX 最小字段集）：`si_signo`、`si_errno`、`si_code`（CLD_EXITED/CLD_KILLED/CLD_STOPPED/CLD_TRAPPED）、`si_pid`、`si_uid`、`si_status`、`si_value(sigval)`
- [ ] uapi 核对/补齐 `P_ALL/P_PID/P_PGID`、`WNOHANG/WNOWAIT/WEXITED/WSTOPPED/WCONTINUED` 常量
- [ ] uapi 新增 `SYS_waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options)`——POSIX 4 参形状（Linux 第 5 参 rusage 不采用）
- [ ] 内核实现（`kern/sysproc.c`，基于 `task_wait(pid, wstatus, opts, rus)`）：
  - `P_PID/P_ALL` 映射；`P_PGID` 不支持时报 EINVAL（docs 记录）
  - `WNOHANG` → 返回 0 且 `infop->si_signo = 0`
  - `WNOWAIT` 不摘除子进程
  - `WSTOPPED/WCONTINUED`：当前无事件则返回 EINVAL（job control 未实现，POSIX 允许）
- [ ] libc：`libbrkc/wait/wait.c` / `waitpid.c` 重写在 `waitid` 之上；`sys/wait.h` 删 `wait4` 原型（可补 `waitid` 声明）
- [ ] 删 `SYS_wait4`、`sys_wait4`（`struct rusage` uapi 类型保留——另有 `getrusage` POSIX 用途待 backlog）
- [ ] 验证：sh 管道/退出码、xargs、init 的 `wait()`、brktest 全部 fork/wait 用例

## Phase 7 — mount / umount2 删除（无 POSIX 替代、无用户依赖）

- [ ] 内核：删 `fs/sysfile.c:648 sys_mount`、`:661 sys_umount2` + 常量 + 原型 + 表项；`fs/init.c` 的内部 `do_mount("/proc")` 路径不动
- [ ] 用户库：删 `libbrkc/misc/mount.c`、`src/include/sys/mount.h`（确认无 cmd 引用）
- [ ] 验证：起机后 `/proc` 正常、`df`/`ls /proc` 可见

## Phase 8 — capget / capset 删除（无 POSIX 替代；内核能力机制原样保留）

- [ ] 内核：删 `kern/sysproc.c:400/430` + 常量 + 原型 + 表项；`capable()`/`CAP_*` 内核机制不动
- [ ] 用户库：删 `libbrkc/misc/capability.c`；`sys/capability.h` 与 uapi/capability.h 保留（内核仍用）
- [ ] 删命令 `cmd/cap`
- [ ] `cmd/brktest.c`：删 capget/capset 用例（约 1015–1040、1485）
- [ ] **backlog（本次不做）**：能力值暴露到 procfs（如 `/proc/<pid>/cap_*`）以恢复只读观察
- [ ] 验证：其余权限用例仍绿

## Phase 9 — setgroups 删除（无 POSIX 替代；连带 su / login 属组切换功能）

- [ ] 内核：删 `SYS_setgroups` + `sysproc` 实现 + 原型 + 表项；`getgroups` 保留（POSIX）
- [ ] 用户库：`libbrkc/unistd/setid.c` 删 `setgroups` + 相关头声明
- [ ] `cmd/su.c:23/25`、`cmd/login.c:21/23`：删补充组赋值逻辑，保留 uid/gid 切换
- [ ] `cmd/brktest.c:857/872`：删用例
- [ ] **backlog（本次不做）**：exec 时从 /etc/group 重建属组
- [ ] 验证：su/login 冒烟（切 uid/gid，属组为空）

## Phase 10 — rt_* → POSIX 名

- [ ] 内核 `uapi/syscall.h` 常量改名：
  - `SYS_rt_sigaction → SYS_sigaction`
  - `SYS_rt_sigprocmask → SYS_sigprocmask`
  - `SYS_rt_sigreturn → SYS_sigreturn`
- [ ] `kern/signal.c:516/529/542` 三函数改名；`include/brk/syscall.h`、`kern/syscall.c` 同步
- [ ] `brk-user` `libbrkc/signal/sigaction.c:60/71/76` 三处常量改名；trampoline `__brk_signal_dispatch` + `sigreturn()` 不动
- [ ] 未实现项一致性改名（零风险）：`rt_sigpending→sigpending`、`rt_sigtimedwait→sigtimedwait`、`rt_sigqueueinfo→sigqueue`、`rt_sigsuspend→sigsuspend`；`newfstatat→fstatat`、`pread64/pwrite64→pread/pwrite`、`pselect6→pselect`、`fadvise64→posix_fadvise`（`mq_getsetattr` 保留；`semtimedop` 不动）
- [ ] **可选建议**：删 `sys_sigaction` / `sigprocmask` 第 4 参 `sigsetsize`（内核只校验不使用；纯 Linux 遗产）， libc 同步去掉实参
- [ ] 验证：brktest 信号用例（handler/mask）、shell 交互信号、ps

## Phase 11 — 收尾

- [ ] **重新编号**：`uapi/syscall.h` 重排 0..N，无空洞；`posix_getdents/statvfs/fstatvfs/posix_fallocate/waitid` 归入正式位置
- [ ] `include/brk/syscall.h` 与新表核对：无孤儿原型，无“有编号缺实现”声明
- [ ] 跑 `scripts/format_src.py` + `scripts/fix_header_guard.py`（Phase 4/5 有头文件增删）
- [ ] 全量：内核 `make -j`、`make run`；`brk-user` 全量 + rootfs 重建 + `brktest` 全量回归
- [ ] 用户仓库 grep：无已删除编号 / 已删除接口调用残留
- [ ] 更新两仓库 README / docs 中的 ABI 与接口清单
- [ ] 提交切分：
  1. Phase 1–2（低风险删除 + faccessat 合并）
  2. Phase 3（posix_fallocate）
  3. Phase 4（posix_getdents）
  4. Phase 5（statvfs）
  5. Phase 6（waitid）
  6. Phase 7–9（mount/cap/groups 连带删除）
  7. Phase 10+11（更名 + 重排）

---

## 不可替代 / 须知

- POSIX **没有设置补充组的接口**：`su`/`login` 属组切换功能随 `setgroups` 移除而消失（属组为空）。
- **没有 capabilities 的 POSIX 用户态接口**：`capget`/`capset` 删除后，`cap(1)` 失效且不能调能力；内核内部的 DAC/KILL/SETUID/CHOWN 检查不受影响。
- `posix_getdents` 与 `waitid` 是**必须新建的后端**，做不完整就走不到「删除」这步——按上面的裁剪规则（P_PGID、WSTOPPED/WCONTINUED 可先不支持并记录）逐项实施即可，都是 POSIX 允许的降级。
- `brk-user` 的 Makefile 经 `uapi` 单源同步编号，无需做“两份表”维护，但是**所有编译产物和 rootfs 都要重生成**。
