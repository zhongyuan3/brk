# 网络协议栈

本文描述 BRK 的 IPv4 网络栈：`sk_buff`、netdevice、以太网、ARP、IP、ICMP、UDP、TCP、socket 层、socket 系统调用和 virtio-net 驱动。设备模型和 IRQ 细节见 [drivers.md](drivers.md) 与 [irq_time.md](irq_time.md)。

相关源码：

- `src/include/brk/net/*.h`
- `src/net/skbuff.c`、`netdevice.c`、`eth.c`、`arp.c`、`ip.c`、`icmp.c`
- `src/net/route.c`、`udp.c`、`tcp.c`、`socket.c`、`sys_socket.c`
- `src/net/loopback.c`、`checksum.c`、`init.c`、`irqtest.c`
- `src/driver/net/virtio_net.c`
- `src/include/uapi/socket.h`、`uapi/syscall.h`

## 1. 分层结构

```text
socket 系统调用
   │ socket/bind/connect/sendto/recvfrom/...
   ▼
socket 抽象（struct socket）
   │ proto_ops
   ├───────────────┐
   ▼               ▼
 UDP             TCP
   │               │
   └───────┬───────┘
           ▼
        IPv4/ICMP
           │
          ARP
           │
        Ethernet
           │
       net_device
           │
       virtio-net
           │
         virtqueue
```

当前只支持 `AF_INET`：

- `SOCK_DGRAM` → UDP
- `SOCK_STREAM` → TCP

## 2. `sk_buff`

### 2.1 线性缓冲区

`struct sk_buff` 是一次性 `kzalloc`：

```text
struct sk_buff + SKB_HEADROOM + payload
head ──► data ──► tail ──► end
          预留头空间    当前数据
```

字段：

- `next`
- `dev`
- `protocol`：主机字节序的 `ETH_P_*`
- `len`
- `head/data/tail/end`
- `cb` union：IP 五元组或 ICMP 字段

特点：

- 没有引用计数；
- 没有 clone；
- 没有分片/非线性数据；
- 所有协议头都通过 `skb_push`/`skb_pull` 在线性缓冲区中操作。

### 2.2 队列

`struct sk_buff_head` 是单链表 FIFO：

- `head`
- `tail`
- `qlen`
- `skb_queue_init/tail/dequeue/purge`

队列辅助函数不加锁，调用者负责同步。

用途：

- ARP pending queue；
- UDP/TCP socket receive queue。

## 3. netdevice

### 3.1 对象

```c
struct net_device {
    char name[IFNAMSIZ];
    unsigned char dev_addr[6];
    unsigned int mtu;
    unsigned int flags;
    unsigned int type;
    int ifindex;
    u32 ip_addr;
    u32 netmask;
    u32 broadcast;
    struct net_device_ops *netdev_ops;
    struct net_device_stats stats;
    spinlock_t tx_lock;
    struct list_head dev_list;
    void *priv;
};
```

操作：

```c
struct net_device_ops {
    int (*ndo_start_xmit)(struct sk_buff *skb, struct net_device *dev);
    int (*ndo_open)(struct net_device *dev);
    int (*ndo_stop)(struct net_device *dev);
};
```

约束：

- `ndo_start_xmit` 消费 skb（成功或失败都由驱动释放）；
- 不能睡眠；
- 可能从任意上下文调用。

### 3.2 注册与发送

`netdev_alloc(name)`：

- 分配 `net_device`；
- 初始化链表和 `tx_lock`。

`netdev_register()`：

- 名字唯一；
- 分配 `ifindex`；
- 加入全局链表；
- 调用 `ndo_open`；
- 设置 `IFF_UP`。

`netdev_start_xmit(skb)`：

- 检查设备存在、`IFF_UP`、`ndo_start_xmit`；
- 更新统计；
- 调用驱动。

### 3.3 接收分发

`struct packet_type`：

```c
struct packet_type {
    u16 type;                  /* ETH_P_* */
    struct net_device *dev;    /* NULL = 任意设备 */
    int (*func)(struct sk_buff *, struct net_device *, struct packet_type *);
    struct list_head list;
};
```

`dev_add_pack()` / `dev_remove_pack()` 注册协议处理器。

`netif_rx(skb)`：

- skb 必须已有 `dev`；
- 扫描 packet_type 链表，匹配 `protocol` 或 `ETH_P_ALL`；
- 调用第一个匹配的 handler；
- 没有匹配则丢弃并 `rx_dropped++`；
- handler 在 `ptype_lock` 外调用。

当前注册：

- `ETH_P_ARP` → `arp_rcv`
- `ETH_P_IP` → `ip_rcv`

## 4. Ethernet

`struct ethhdr`：

```c
u8 dst[6];
u8 src[6];
u16 h_proto;
```

- `eth_type_trans()` 读取协议、pull 14 字节头；
- `eth_header()` push 头，源 MAC 来自 `dev->dev_addr`，目标缺省为广播；
- `eth_broadcast_addr` = `ff:ff:ff:ff:ff:ff`。

## 5. 地址与校验和

地址约定：

- `NET_IP4(a,b,c,d)` 创建的值与四个网络字节序字节完全相同；
- IPv4 地址字段不做字节交换；
- 端口、长度、协议继续使用 `htons/ntohs`。

默认配置：

- IP `10.0.2.15`
- mask `255.255.255.0`
- gateway `10.0.2.2`

校验和：

- `inet_csum_add` 累加大端 16 位字；
- `inet_csum_fold` 折叠并取反；
- 有效头部带校验和时 `inet_csum(...) == 0`。

## 6. 路由

`route.[ch]`：

- 固定 8 项路由表；
- `route_add(dst, mask, gw, dev)`；
- `ip_route_output(daddr)` 选择最长前缀匹配；
- 没有路由删除/替换 API；
- 无匹配返回 `-ENETUNREACH`。

初始化：

- loopback 注册 `127.0.0.0/8`；
- virtio-net 注册 `10.0.2.0/24` 和默认路由 `0.0.0.0/0 via 10.0.2.2`。

## 7. ARP

### 7.1 表

- 16 项；
- 每项 `ip/mac/state/last_request_ns/last_used/pending queue`；
- 状态：`ARP_INCOMPLETE`、`ARP_REACHABLE`；
- 无 aging timeout，只有 LRU 淘汰。

### 7.2 流程

`arp_output(skb, dev, next_hop)`：

1. 查表；
2. 已解析：填以太网头并发送；
3. 未解析：
   - 把 skb 挂到 pending queue（最多 4 个，超出丢最旧）；
   - 如果之前没请求或超过 1 秒，发送 ARP request；
   - 返回 `-EAGAIN`。

`arp_rcv()`：

- 学习发送者 IP/MAC；
- 如果目标 IP 是本机，发送 reply；
- 释放 skb。

`arp_update()`：

- 更新/创建表项；
- 标记 `ARP_REACHABLE`；
- 刷新 pending queue。

## 8. IPv4

`ip_rcv()`：

- 校验长度、版本、IHL、总长度、校验和；
- 丢弃所有分片（不重组）；
- 保存源/目的地址到 `skb->cb`；
- 裁剪到 `tot_len`，pull IP 头；
- 分发到 ICMP/UDP/TCP；
- 不检查目的地址，不做转发。

`ip_output(skb, saddr, daddr, protocol, ttl)`：

1. 路由；
2. 源地址为 0 时使用设备地址；
3. push 20 字节 IP 头；
4. 全局 `ip_id` 递增（未加锁）；
5. 设置 DF、TTL、协议、地址、校验和；
6. 设置 `skb->dev`、`protocol = ETH_P_IP`；
7. loopback 直接 `netif_rx()`，否则 `arp_output()`。

## 9. ICMP

支持：

- echo request/reply；
- destination unreachable 类型识别；
- `net_ping()` 用于 procfs/IRQ 自测。

`icmp_rcv()`：

- 校验长度和校验和；
- echo request → 改为 reply，交换地址，重新计算校验和，`ip_output()`；
- echo reply → 打印日志。

## 10. UDP

### 10.1 Socket

```c
struct sock {
    spinlock_t lock;
    int family, type, protocol;
    u32 saddr, daddr;
    u16 sport, dport;
    bool bound, connected, shutdown, eof, err;
    struct sk_buff_head rcv_queue;
    struct list_head list;
};
```

UDP 全局：

- `udp_socks` 链表；
- `udp_lock`；
- 轮转的临时端口分配器（49152..65535）；
- 每 socket 接收队列最多 64 个 skb。

### 10.2 发送

`udp_sendmsg()`：

1. 目标来自 `addr` 或 connected 元组；
2. 未绑定时自动分配端口；
3. 路由；
4. 检查 `len <= mtu - IP头 - UDP头`，不支持 IP 分片；
5. 源地址 = bound 地址或设备地址；
6. 构造 UDP 头；
7. 计算伪头部校验和；
8. `ip_output()`；
9. 返回用户长度。

### 10.3 接收

`udp_rcv()`：

- 校验长度和 checksum（0 表示未提供）；
- 在 `udp_lock` 下匹配本地端口/地址/connected peer；
- 在 `sk->lock` 下入接收队列（上限 64）；
- 唤醒 `sk` 上的睡眠者。

`udp_recvmsg()`：

- 队列空时睡眠，除非 shutdown/非阻塞；
- 出队一个 skb；
- 填充源地址；
- 复制 min(用户长度, skb 长度)；
- 释放 skb。

## 11. TCP

### 11.1 状态机

支持状态：

```text
CLOSE
LISTEN
SYN_SENT
SYN_RECV
ESTABLISHED
FIN_WAIT_1
FIN_WAIT_2
CLOSE_WAIT
LAST_ACK
TIME_WAIT
```

常量：

- `TCP_MSS = 1460`
- `TCP_DEFAULT_MSS = 536`
- `TCP_RCV_WND = 4096`
- `TCP_RCV_QUEUE_MAX = 64`
- `TCP_MAX_RETRIES = 5`
- RTO 初始 1 秒，指数退避到 8 秒
- TIME_WAIT 2 秒
- FIN_WAIT_2 10 秒
- SYN_RECV 10 秒
- 定时器每 100 ms 执行一次

### 11.2 连接

- `tcp_bind`/自动绑定与 UDP 类似的端口冲突规则；
- `tcp_connect`：
  - 保存 peer；
  - 初始化 ISS、`snd_nxt`；
  - 发送 SYN；
  - 在 `tcp_lock` 下睡眠直到离开 SYN_SENT；
  - 只有 ESTABLISHED 返回 0，否则 `-ECONNREFUSED`；
- `tcp_listen`：自动绑定，设置 LISTEN 和 backlog；
- `tcp_accept`：在 `tcp_lock` 下等待 accept queue，出队子 socket。

### 11.3 发送

`tcp_sendmsg()`：

- 按 MSS 分块；
- 只允许 ESTABLISHED/CLOSE_WAIT；
- 一次只允许一个 outstanding TX；
- 设置 `snd_nxt`、TX_DATA、seq、RTO；
- 发送后等待 ACK/错误；
- 失败时回滚 `snd_nxt`。

### 11.4 接收

`tcp_rcv()`：

1. 校验头、数据偏移、checksum；
2. 解析 seq/ack/flags/window；
3. 在 `tcp_lock` 下：
   - 查找 socket；
   - 对 LISTEN 的裸 SYN 创建子 socket（SYN_RECV）；
   - 处理 RST；
   - SYN_SENT 接受 SYN|ACK；
   - SYN_RECV 处理重复 SYN 和 ACK；
   - ACK 推进 `snd_una`；
   - 只接受 in-order payload；乱序/重复只 ACK 不入队；
   - FIN 处理状态转换和 EOF；
4. 解锁后发送 ACK/重传/唤醒。

### 11.5 关闭与重传

- `tcp_shutdown` 发送 FIN；
- `tcp_release` 对 ESTABLISHED/CLOSE_WAIT 发送 FIN 并标记 orphan；
- orphan 由 TCP timer 最终释放；
- `tcp_timer_tick`：
  - 每 100 ms；
  - 清理超时 SYN_RECV；
  - 清理 TIME_WAIT/FIN_WAIT_2/CLOSE orphan；
  - 对超时 TX 最多重传 5 次，RTO 指数退避；
  - 重传在解锁后发送。

### 11.6 TCP 限制

- 只有一个 outstanding TX；
- 不处理乱序、窗口缩放、SACK、时间戳；
- 不支持 IP 分片，依赖 MSS；
- `tcp_lock` 同时作为睡眠条件锁，阻塞操作会阻塞整个 TCP 层；
- 部分接收路径在解锁后使用 socket 指针，存在与 orphan 释放竞争的窗口。

## 12. Socket 层

### 12.1 对象

```c
struct socket {
    int type;
    bool nonblock;
    struct sock *sk;
    struct proto_ops *ops;
};
```

`struct proto_ops`：

```c
release, bind, connect, listen, accept,
sendmsg, recvmsg, shutdown, getname, poll
```

`fs_file` 的 `private_data` 指向 socket；socket 文件操作：

- `read` → `recvmsg`
- `write` → `sendmsg`
- `release` → protocol release
- `llseek` → `-ESPIPE`
- `ioctl` → `-ENOTTY`
- `poll` → protocol poll

### 12.2 系统调用

支持：

- `socket`
- `bind`
- `connect`
- `listen`
- `accept` / `accept4`
- `sendto`
- `recvfrom`
- `shutdown`
- `getsockname`
- `getpeername`
- `setsockopt`（有限）
- `getsockopt`（总是 `-ENOPROTOOPT`）

参数校验：

- `AF_INET`
- `addrlen >= sizeof(sockaddr_in)`
- `SOCK_NONBLOCK` 存入 socket
- `SOCK_CLOEXEC` 当前没有实际 close-on-exec 语义

未实现：

- `socketpair`
- 完整 `setsockopt`
- IPv6
- `AF_UNIX`
- `MSG_*` flags 语义

## 13. Loopback

- `loopback_init()` 是 core initcall；
- 设备名 `lo`；
- MTU 65535；
- `IFF_UP | IFF_LOOPBACK`；
- 地址 `127.0.0.1/8`；
- `loopback_xmit` 直接 `netif_rx(skb)`，不加/不去以太网头；
- IP 输出对 loopback 直接发送，绕过 ARP。

## 14. virtio-net

### 14.1 队列

- RX queue 0；
- TX queue 1；
- 64 个描述符；
- 每缓冲区 2048 字节；
- 最大帧 1514；
- 10 字节 virtio-net header；
- 不支持 mergeable RX buffers。

### 14.2 初始化

1. reset；
2. ACK/DRIVER；
3. 协商 features，关闭 checksum/TSO/UFO/GSO/MRG_RXBUF/control VQ/MQ；
4. 建立 RX/TX virtqueue；
5. 读取 MAC/MTU；
6. 分配 TX buffer；
7. 提交所有 RX buffer；
8. `request_irq(IRQV_SHARED)`；
9. DRIVER_OK；
10. 创建 `ethN` net_device；
11. 注册设备，添加默认路由；
12. 设置 RX callback，notify RX queue。

### 14.3 TX

`vnet_ndo_start_xmit()`：

- 调用 `virtio_net_transmit()`；
- 无论成功失败都释放 skb。

`virtio_net_transmit()`：

- 获取 vnet lock；
- 分配 TX descriptor；
- 写 10 字节 virtio header + 以太网帧；
- 提交并 notify；
- 不睡眠。

### 14.4 RX

IRQ handler：

- 获取 vnet lock；
- ack MMIO 中断；
- 处理 used RX/TX ring；
- 收集最多 64 个包；
- 重新提交 RX 描述符；
- 解锁后调用 RX callback。

RX callback `vnet_rx_to_stack()`：

- 分配 skb；
- 复制帧；
- 设置 `skb->dev`；
- `eth_type_trans()`；
- `netif_rx()`。

当前 RX 路径直接在硬中断上下文完成 Ethernet → IP → UDP/TCP 处理和任务唤醒，没有 NAPI/softirq。

## 15. 初始化顺序

```text
core_initcall(net_init):
  arp_init()
  loopback_init()
  dev_add_pack(ETH_P_ARP, arp_rcv)
  dev_add_pack(ETH_P_IP, ip_rcv)

subsys_initcall(virtio_net_init):
  dt_init_scan_virtio_dev()
  driver_register(virtio_net_driver)
    └─ probe → 创建 ethN、路由、IRQ、RX queue

user_init_entry:
  bring_up_secondaries()
```

网络栈在设备驱动 probe 前已经注册协议处理器，因此 `lo` 可用；`ethN` 在 virtio-net probe 后出现。

## 16. 锁与不变量

- `netdev_lock`：netdevice 全局链表；
- `ptype_lock`：packet_type 链表，handler 在锁外调用；
- `route_lock`：路由表；
- `arp_lock`：ARP 表/pending queue，发送在锁外；
- `udp_lock` → `sk->lock`：UDP 接收路径；
- `tcp_lock` → `sk->lock`：TCP 接收路径；
- `tcp_lock` 同时作为阻塞 connect/send/accept 的条件锁；
- `vnet->lock`：virtio-net RX/TX 队列；
- skb 队列本身不内置锁，调用者负责。

skb 所有者规则：

- `netif_rx()` 消费 skb；
- `netdev_start_xmit()` 消费 skb；
- 协议 handler 负责释放；
- 失败路径必须显式释放。

## 17. 当前限制

- 只支持 IPv4，无 IPv6；
- IP 分片不支持，接收直接丢弃；
- ARP 没有超时/重试机制；
- 路由表固定 8 项，无删除；
- 网络统计和 `ip_id` 非原子；
- TCP 状态机简化，单 outstanding TX；
- 接收路径在硬中断上下文；
- 没有 NAPI、softirq、workqueue；
- 没有 netfilter、socket 选项完整语义、SIGPIPE；
- 没有完整 `SOCK_CLOEXEC`；
- `socketpair` 未实现。
