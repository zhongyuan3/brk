# 文件系统与 VFS

本文描述 BRK 的虚拟文件系统（VFS）、挂载模型、路径查找、缓存、页缓存、writeback、pipe、select 以及具体文件系统 tmpfs/devtmpfs/procfs/brkfs。块层细节见 [drivers.md](drivers.md)，exec 的地址空间细节见 [memory.md](memory.md) 和 [process.md](process.md)。

相关源码：

- `src/include/brk/fs.h`、`fs_types.h`、`dcache.h`、`path.h`、`mount.h`
- `src/include/brk/fdtable.h`、`fsinfo.h`、`pagecache.h`、`pipe.h`、`select.h`
- `src/fs/*.c`
- `src/fs/tmpfs/`
- `src/fs/procfs/`
- `src/fs/brkfs/`
- `src/fs/devtmpfs.c`
- `src/mm/pagecache.c`

## 1. 分层结构

```text
用户系统调用
   │ open/read/write/stat/mount...
   ▼
VFS 核心
   ├─ fdtable / fsinfo / path lookup / permission
   ├─ inode cache / dentry cache / file objects
   ├─ mount topology
   ├─ exec / ELF loader
   ├─ pipe / select / writeback
   ▼
具体文件系统
   ├─ tmpfs
   ├─ devtmpfs（tmpfs + 设备节点管理）
   ├─ procfs
   └─ brkfs
   ▼
页缓存 / 块设备
   └─ virtio-blk / bio / request queue
```

VFS 不感知具体块设备协议；文件系统通过 `fs_super_block_ops`、`fs_inode_ops`、`fs_file_ops` 接入。

## 2. VFS 对象模型

### 2.1 `fs_driver`

```c
struct fs_driver {
    const char *name;
    unsigned int flags;
    int (*mount)(struct fs_mount_args *, struct fs_mount_result *);
    spinlock_t lock;
    struct list_head super_blocks;
    struct list_head list;
};
```

- 通过 `fs_driver_register()` 加入全局链表；
- `fs_driver_lookup(type_name)` 按名字查找；
- 驱动通常是静态对象，不注销；
- `flags` 中的 `FS_REQUIRES_DEV` 等当前没有被核心强制检查。

### 2.2 `fs_super_block`

```c
struct fs_super_block {
    struct list_head list;
    struct fs_driver *driver;
    struct list_head instance;
    refcnt_t count;
    ...
    struct fs_super_block_ops *ops;
    struct fs_dentry *root;
    void *private_data;
    spinlock_t inodes_lock;
    struct list_head inodes;
    struct list_head dirty_inodes;
    spinlock_t mnt_states_lock;
    struct list_head mnt_states;
    ...
};
```

责任：

- 保存文件系统实例；
- 持有 root dentry；
- 维护 inode 链表和 dirty inode 链表；
- 记录所有 mount state；
- `ops->put_super()` 在最后一个引用释放时调用。

### 2.3 `fs_inode`

关键字段：

- `sb`、`ops`、`fops`
- `refcnt`/`count`
- `lock`：保护 state/alias
- `rwsem`：串行化 I/O、truncate、eviction
- `state`：`I_NEW`、`I_DIRTY*`、`I_FREEING`、`I_WILL_FREE`、`I_BAD`、`I_CLEAR`
- `hash`、`sb_list`、`list`、`dentries`
- `ino`、`mode`、`nlink`、`size`、`rdev`、时间、uid/gid
- `mapping`：可选页缓存
- `private_data`：具体文件系统私有 inode

### 2.4 `fs_dentry`

```c
struct fs_dentry {
    refcnt_t count;
    unsigned long flags;     /* NEGATIVE / MOUNTED / UNHASHED */
    spinlock_t lock;
    struct fs_inode *inode;
    struct list_head alias;
    struct fs_dentry *parent;
    struct list_head child;
    struct list_head children;
    ...
    struct qstr name;
    ...
    struct fs_super_block *sb;
    struct fs_dentry_ops *ops;
    void *private_data;
};
```

- 正 dentry 拥有一个 inode 引用；
- 负 dentry 只表示“名字不存在”；
- `DCACHE_MOUNTED` 表示该 dentry 是挂载点；
- 名字短时放在内联 `short_name[32]`，长名字单独分配。

### 2.5 `fs_file`

```c
struct fs_file {
    refcnt_t count;
    struct fs_path path;      /* dentry + mount */
    struct fs_inode *inode;   /* 从 path.dentry 派生 */
    struct fs_file_ops *ops;
    spinlock_t lock;
    fmode_t mode;
    bool anon;
    spinlock_t pos_lock;
    loff_t pos;
    void *private_data;
    void *dev_private;
};
```

- 一次 `open()` 对应一个 `fs_file`；
- `anon` 文件没有 path/inode（socket 等）；
- 文件位置 `pos` 由 `pos_lock` 保护；
- `private_data` 给驱动/文件系统使用；
- `dev_private` 由字符设备核心使用。

### 2.6 `fs_path` 与 mount state

```c
struct fs_path {
    struct fs_mount_state *mnt;
    struct fs_dentry *dentry;
};
```

`fs_mount_state`：

- `parent`、`child`、`children`
- `mount_point`
- `root`
- `sb`
- `refcnt`
- `flags`（内部挂载标记）

路径引用规则：`fs_path_get/put()` 同时管理 dentry 和 mount 引用。

### 2.7 fdtable / fsinfo

`fdtable`：

```c
struct file_desc_table {
    struct fs_file *files[OPEN_MAX];
    spinlock_t lock;
    refcnt_t refcnt;
};
```

- `OPEN_MAX = 64`；
- 固定数组，不动态扩展；
- `fdtable_copy()` 复制每个 fd 的文件引用；
- fork 后父子拥有独立 fdtable，但共享打开文件对象。

`fsinfo`：

```c
struct file_system_info {
    struct fs_path cwd, root;
    spinlock_t lock;
    refcnt_t refcnt;
};
```

- 每任务保存 root 和 cwd；
- 没有独立 mount namespace；
- `fsinfo_copy()` 复制路径引用。

## 3. 缓存与生命周期

### 3.1 Inode cache

- 全局 hash：256 桶，key = superblock + inode number；
- `fs_inode_get_locked()`：
  - hash 命中且未处于 `I_FREEING/I_WILL_FREE/I_BAD` 时增加引用；
  - 未命中则分配新 inode，设置 `I_NEW`，发布到 hash 和 `sb->inodes`；
  - 另一个 CPU 可能竞争创建，失败者等待 `I_NEW` 清除；
- `fs_inode_unlock_new()` 清除 `I_NEW` 并唤醒等待者；
- `fs_inode_fail_new()` 设置 `I_BAD`；
- 最后一个 `fs_inode_put()`：
  1. 设置 `I_FREEING`；
  2. 调用 `ops->evict_inode()`；
  3. 销毁页缓存；
  4. 从 hash、`sb->inodes`、dirty 链表移除；
  5. 调用 `free_inode()`/slab 释放。

没有 inode LRU：最后一个引用释放就淘汰。

### 3.2 Dentry cache

- 全局 hash：256 桶，key = parent dentry + name；
- `fs_dentry_lookup()`：
  - hash 命中返回引用；
  - miss 分配新 dentry；
  - 获取父目录 inode 的 `rwsem`；
  - 再次检查 hash；
  - 调用父 inode `lookup()`；
  - 缓存正 dentry、负 dentry 或错误；
- `fs_dentry_splice_alias()` 处理同一 inode 的别名；
- 没有 dentry LRU/回收线程；最后一个引用释放即淘汰。

### 3.3 File cache

- `fs_file_alloc()`：
  - 要求正 dentry；
  - 增加 path 引用；
  - `file->ops = inode->fops`；
  - 调用 `ops->open()`。
- `fs_file_put()` 最后一个引用时调用 `ops->release()` 并释放 path。
- `fs_file_read/write/lseek/ioctl/poll/stat/truncate/fallocate` 是 VFS 的统一包装。

## 4. 挂载模型

### 4.1 全局 mount topology

- `mount_hashtable`：32 桶，key = (parent mount, mountpoint dentry)；
- `mount_lock`：sleeplock，串行化拓扑修改；
- `kernel_mounts`：内部挂载链表，不参与路径查找；
- 没有 per-process mount namespace。

### 4.2 `graft_tree()`

四阶段：

1. **验证**：在 `mount_lock` + `mount_hashtable_lock` 下检查挂载点是否已占用；
2. **转移引用**：新 mount 获取 parent mount 和 mountpoint dentry 引用；
3. **拓扑链接**：
   - 加入 parent 的 children；
   - 先发布 mount hash，再设置 `DCACHE_MOUNTED`；
4. **superblock 记录**：加入 `sb->mnt_states`。

### 4.3 `do_mount()` / `do_umount()`

`do_mount()`：

- 按类型查找 `fs_driver`；
- `fs_path_lookup(dir_name, LOOKUP_MOUNTPOINT)` 找到挂载点；
- 要求正目录 inode；
- 分配 mount state；
- 调用 `driver->mount()`；
- `graft_tree()`；
- 失败清理 root/sb/mount state。

`do_umount()`：

- 要求 mount 仍在 hash 中；
- `refcnt == 2` 才允许卸载（拓扑引用 + 查找引用）；
- 移除 hash、清除 `DCACHE_MOUNTED`、从 parent children 和 `sb->mnt_states` 移除；
- 释放 parent/mountpoint/拓扑引用。

`kernel_mount()`：

- 创建内部挂载；
- `parent = self`，`mount_point = root`；
- 不进入 mount hash，因此普通路径查找看不到。

## 5. 路径查找

### 5.1 `path_init()`

起点：

- 绝对路径：`current->fsinfo->root`；
- `AT_FDCWD`：`current->fsinfo->cwd`；
- dirfd：从 fdtable 取文件，复制其 path。

### 5.2 `path_walk()`

每个组件：

1. 当前 dentry 必须有 inode；
2. 当前 inode 必须是目录；
3. `follow_mount()` 跨越挂载点；
4. 检查当前目录 `MAY_EXEC`；
5. 如果 `LOOKUP_PARENT` 且是最后一个组件，停在父目录；
6. `.` / `..` 特殊处理；
7. 普通名字调用 `dir_lookup_entry()` → `fs_dentry_lookup()`；
8. 符号链接：
   - 非最后组件或设置了 `LOOKUP_FOLLOW` 时展开；
   - 限制 40 次；
   - 绝对目标重置查找起点；
   - 相对目标从当前目录继续；
9. 提交路径并 follow mount。

### 5.3 `..` 与挂载

`fs_path_dot_dot()`：

- 在挂载根且 parent mount 非自身时，跨到 `parent` mount 的 `mount_point`；
- 否则走 `dentry->parent`；
- 到自环根停止。

当前没有 root confinement：如果根被替换，`..` 可能走进隐藏的父挂载。

## 6. 权限、open、exec

### 6.1 权限

- `capable(cap)` 检查 `current->cap_effective`；
- `inode_permission()`：
  - `CAP_DAC_OVERRIDE` 绕过；
  - 否则按 fsuid/fsgid/supplementary groups 检查 owner/group/other 位；
  - exec 仍要求至少一个执行位；
- `CAP_FOWNER` / `CAP_CHOWN` / `CAP_SETGID` 等用于特定操作；
- sticky 目录删除需要 `CAP_FOWNER`、目录 owner 或 victim owner。

### 6.2 `do_openat()`

流程：

1. 计算 lookup flags；
2. 路径查找；
3. 负 dentry + 无 `O_CREAT` → `-ENOENT`；
4. 正 dentry + `O_EXCL` → `-EEXIST`；
5. 负 dentry + `O_CREAT`：
   - 找到父目录；
   - 检查 `MAY_WRITE|MAY_EXEC`；
   - 父目录 `rwsem`；
   - 调用 `ops->create()`；
   - 竞争 `-EEXIST` 时重试；
6. 计算 `fmode`；
7. 检查 `inode_permission()`；
8. 分配 `fs_file`；
9. 写请求时剥离 setuid/setgid；
10. 处理 `O_TRUNC` / `O_APPEND`。

### 6.3 namei 操作

- `mkdir`/`mknod`/`link`/`unlink`/`symlink`/`rmdir`/`rename`
- 大多在父目录 `rwsem` 下重新验证 dentry 状态；
- `rename` 当前只支持同目录目录重命名；
- `RENAME_EXCHANGE` 未实现；
- 没有完整的 `d_move`。

### 6.4 exec

`do_execve()`：

- 收集 argv/envp，检查 `ARG_MAX = 16384`；
- `__do_execve()` 打开可执行文件、检查 `MAY_EXEC`；
- 读取并验证 ELF（仅 `ELFCLASS64` / `ET_EXEC`）；
- 创建新 `uvm_space`，加载 `PT_LOAD`；
- 建立堆和用户栈；
- 写入 argv/envp；
- 切换页表、替换 mm；
- `signal_reset()`；
- 处理 setuid/setgid；
- 设置入口 PC/SP。

exec 不关闭 fdtable，也不改变 root/cwd。

## 7. 文件 I/O、页缓存、writeback

### 7.1 统一文件操作

- `fs_file_read/write` 持 `pos_lock` 调用驱动/文件系统；
- `fs_file_lseek` 只在成功时提交 `pos`；
- `fs_file_poll` 默认普通文件可读可写；
- `fs_file_stat` 使用 inode 元数据。

### 7.2 页缓存

见 [memory.md](memory.md) 第 7 节。文件系统通过 `page_cache_ops` 的 `read_page/write_page` 接入。

- brkfs 的普通文件和符号链接使用页缓存；
- 块设备也有自己的页缓存；
- tmpfs 不使用 VFS 页缓存，使用自己的 page chain；
- procfs 使用一次性快照缓冲，不使用页缓存。

### 7.3 writeback

- `writeback_single_inode()`：刷页缓存 → 写 inode 元数据；
- `sync_filesystem()`：快照 dirty inode，逐个回写，最后调用 `sync_fs()`；
- `sync_all_filesystems()`：遍历所有超级块；
- `writeback_thread()`：每 5 秒执行一次；
- `sys_sync` / `sys_syncfs` / `sys_fsync` / `sys_fdatasync`。

### 7.4 共享文件映射的 dirty 语义

BRK 不跟踪硬件 dirty 位，因此：

- 普通用户写入共享映射不会自动标脏；
- `msync()`、`munmap()`、区域释放、部分 truncate/mprotect 路径会标脏；
- 写回依赖显式标记和 writeback。

## 8. Pipe 与 select

### 8.1 Pipe

- `pipefs` 是内部挂载的伪文件系统；
- 每个 pipe 有 4096 字节环形缓冲；
- 读端/写端共享 `anon_pipe`，持有 readers/writers 计数；
- `pipe_read`：
  - 空时等待；
  - writers==0 → EOF；
  - 非阻塞 → `-EAGAIN`；
  - 有信号 → `-EINTR`；
- `pipe_write`：
  - readers==0 → `-EPIPE`（当前不发送 SIGPIPE）；
  - 满时等待；
  - 非阻塞/信号处理类似；
- `pipe_poll` 报告 `POLLIN/POLLOUT/POLLHUP/POLLERR`。

### 8.2 select/poll

`do_poll()`：

- 每轮对每个 fd 调用 `fs_file_poll()`；
- 合并 `POLLERR/POLLHUP/POLLNVAL`；
- 有事件立即返回；
- 否则检查超时/信号，然后睡 10 ms 再重试。

`sys_select()` 把 fd_set 转成 pollfd，再把结果映射回三个 fd_set。

没有 poll wait queue，等待靠定时轮询。

## 9. 具体文件系统

### 9.1 tmpfs

- 纯内存文件系统；
- 超级块维护 inode map 页链；
- inode 数据/目录页通过 `page->tmpfs_next` 链接；
- 目录项 inline 存放在页中，不跨页；
- 支持 create/lookup/mkdir/mknod/symlink/link/unlink/rmdir/truncate；
- `rename` 未实现，返回 `-EOPNOTSUPP`；
- 不使用 VFS 页缓存；
- 已知问题：rmdir 的 nlink 更新不完整、symlink size 未更新。

### 9.2 devtmpfs

- 复用 tmpfs 存储和 inode 操作；
- 维护 `/dev` 下设备节点；
- `devtmpfs_notify(dev, remove)` 在设备注册/注销时排队；
- 启动期 `active=false` 时丢弃通知，由 `devtmpfs_populate()` 统一创建；
- worker 线程从工作链表取任务，调用 `do_mknodat`/`do_unlinkat`；
- 启动流程会 populate 两次：brkfs 挂载前创建块设备节点，挂载 `/dev` 后再重建。

### 9.3 procfs

- 根目录静态条目：`version`、`uptime`、`meminfo`、`filesystems`、`pagecache`、`pagecache_shrink`、`ping`、`tcp`、`interrupts`、`irqtest`；
- `/proc/<pid>/`：`status`、`stat`、`cmdline`；
- inode 编号高位编码类型和索引；
- 打开普通 proc 文件时一次性调用 `show()` 填充一页快照；
- 读操作读快照，写操作只对提供 `write` 回调的条目生效；
- 目录迭代动态列出 PID；
- 没有页缓存，没有创建/删除普通文件。

### 9.4 brkfs

BRK 自有磁盘文件系统，目标是小型、可检查、支持真实块设备。

#### 磁盘格式

- superblock 位于字节偏移 1024，大小 1024；
- 备份 superblock 在卷最后一块；
- 魔数 `0x6b7262`，版本 2；
- 块大小 1024..4096，inode 128..256；
- 支持 direct + 1/2/3 级间接块；
- CRC32C 校验 superblock、inode、目录块；
- 状态：CLEAN/DIRTY/ERROR；
- orphan 链表用于崩溃后清理未完成删除。

#### 块映射

`i_block[10]`：

```text
0..6   direct
7      single indirect
8      double indirect
9      triple indirect
```

`brkfs_inode_getblk()` 负责：

- 查询/分配数据块；
- 按 `CREATE` 标志决定是否分配；
- 维护间接块层级；
- 更新 feature bits（double/triple/dir indirect）；
- 分配/释放数据位图和空闲计数。

#### Inode 与目录

- inode table 按 `(ino-1)/inodes_per_block` 定位；
- 读取时验证 on-disk ino 和 CRC；
- 目录项 inline，支持 split/merge；
- 目录块尾部 4 字节保存 CRC；
- 支持 create/link/unlink/symlink/mkdir/rmdir/rename/mknod；
- 删除 nlink 到 0 时加入 orphan，真正释放由 evict/cleanup 完成。

#### 文件 I/O

- 普通文件/符号链接使用页缓存；
- `read_page` 按文件系统块读取，洞填零；
- `write_page` 按 `inode->size` 写入，必要时分配块；
- `fsync` 刷页缓存、写 inode、刷块设备页缓存、`blkdev_flush`。

#### 崩溃一致性

- 没有 journal；
- 依赖 CRC、CLEAN/DIRTY、orphan cleanup；
- mount 时状态 ERROR 拒绝挂载，DIRTY 只警告并尝试 orphan cleanup。

## 10. 初始化顺序

```text
core_initcalls:
  fs_dentry_cache_init
  fs_file_cache_init
  fs_inode_cache_init

subsys_initcalls:
  设备驱动注册/探测（块设备、TTY、网络）

CPU0 静态 init_task:
  fs_init()
    ├─ register_builtin_filesystems()
    ├─ gendisk_scan_partitions()
    ├─ kernel_mount(tmpfs)
    ├─ 创建 /dev
    ├─ devtmpfs_populate()
    ├─ mount brkfs on /
    ├─ 更新 root/cwd
    ├─ mount /dev、/proc
    ├─ devtmpfs_start()
    └─ 打开 /dev/tty0，设置 fd 0/1/2
```

用户 init exec 成功后启动：

- console log 线程
- klogd
- writeback 线程

## 11. 锁与生命周期规则

- `filesystems_lock`：fs_driver 链表；
- `inode_hash_lock`：inode hash；
- `dentry_hash_lock`：dentry hash；
- `inode->lock`：inode state/alias；
- `inode->rwsem`：I/O/truncate/evict；
- `sb->inodes_lock`：sb inode 链表；
- `sb->mnt_states_lock`：mount state 链表；
- `mount_hashtable_lock`：mount hash；
- `mount_lock`：拓扑修改；
- `fdtable->lock`：fd 数组；
- `fsinfo->lock`：root/cwd；
- `mapping->lock` → `cached_page->lock`：页缓存；
- `inode->rwsem` → `mapping->lock` 的典型顺序出现在 truncate/writeback。

对象规则：

- `fs_file` 持有 path（dentry + mount）引用；
- 正 dentry 持有 inode 引用；
- mount state 持有 root dentry 和 sb 引用；
- superblock 的 `root` 持有一个额外 dentry 引用；
- 内部挂载由 `kernel_mounts` 和 `MOUNT_STATE_INTERNAL` 标识。

## 12. 当前限制

- 没有 per-task mount namespace；
- 没有 `chroot`/`pivot_root`/`unshare`；
- 没有 dcache/inode LRU 或后台回收；
- rename 功能有限，目录 rename 只允许同目录；
- 没有 journal，brkfs 崩溃一致性有限；
- 共享文件映射不自动标脏；
- pipe 不发送 `SIGPIPE`；
- select/poll 是 10 ms 轮询；
- `O_CLOEXEC` 没有实际 close-on-exec 语义；
- `fs_driver` flags 多数未强制。
