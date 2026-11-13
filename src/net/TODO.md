# Network Stack TODO

This file tracks the work still needed before the BRK network stack can be
considered generally usable outside the QEMU lab environment.

## Current status

The stack currently supports:

- Ethernet and virtio-net
- ARP with cache expiry and retry
- IPv4 send/receive, output fragmentation, and basic reassembly
- ICMP echo, ICMP error generation, and raw ICMP sockets
- UDP and TCP sockets
- Basic TCP handshake, data transfer, retransmission, shutdown, and TIME_WAIT
- Non-blocking socket operations and common `SO_*` / `MSG_*` flags
- DHCP client, `ifconfig`, `route`, and `ping` userspace tools
- RX queue with a deferred `net-rx` worker thread
- Interface enumeration, link up/down, MTU, and RX/TX statistics
- Loopback and basic HTTP/TCP/UDP traffic

This is sufficient for QEMU experiments, demos, and basic local networking,
but it is not yet a production-grade or general-purpose TCP/IP stack.

## P0: Required for general use

### 1. TCP robustness and throughput

- Implement out-of-order segment queueing and reassembly.
- Support more than one in-flight data segment.
- Add a proper send buffer and receive buffer.
- Implement congestion control:
  - slow start
  - congestion avoidance
  - fast retransmit
  - fast recovery
- Implement receive-window scaling.
- Implement SACK and duplicate-ACK processing.
- Add zero-window probing.
- Add keepalive support.
- Add Nagle and delayed-ACK behavior.
- Handle PMTU/MSS discovery correctly.
- Enforce listen backlog limits for half-open and established connections.
- Harden TIME_WAIT and connection teardown under load.
- Add a socket error queue or equivalent mechanism for asynchronous TCP errors.

### 2. RX scalability

- Replace the single `net-rx` worker with per-CPU NAPI-style polling.
- Add per-queue RX budgets and fair draining.
- Add multi-queue virtio-net support.
- Bind RX/TX queues to CPUs where possible.
- Add GRO and optional checksum offload.
- Add queue-level drop and error accounting.
- Prevent a single RX thread from becoming a throughput bottleneck.

### 3. DHCP lifecycle

- Implement lease renewal (`RENEWING` / `REBINDING`).
- Handle DHCP NAK and lease expiry.
- Support multiple interfaces.
- Reapply DNS and default-route configuration after renewal.
- Provide a fallback configuration path when no DHCP server is available.
- Add tests for long-running leases and server restarts.

### 4. Socket API completeness

- Enforce `SO_RCVBUF` and `SO_SNDBUF` limits instead of only storing them.
- Add `SO_LINGER`.
- Add `SO_RCVTIMEO` and `SO_SNDTIMEO`.
- Support `fcntl(O_NONBLOCK)` semantics consistently.
- Support scatter/gather `sendmsg` / `recvmsg`.
- Complete `getsockopt` / `setsockopt` coverage for common options.
- Add more complete `MSG_*` behavior where applicable.
- Define and document raw-socket security and permission rules.

### 5. IPv4 and ICMP completeness

- Validate and harden fragment reassembly:
  - overlapping fragments
  - duplicate fragments
  - timeout cleanup
  - queue exhaustion
  - maximum datagram sizes
- Add PMTU discovery and ICMP fragmentation-needed handling.
- Deliver ICMP errors to affected TCP/UDP sockets.
- Handle broadcast and multicast addresses consistently.
- Add IPv4 header option handling where required.
- Add tests for fragmented UDP and TCP under loss/reordering.

### 6. Routing and interface management

- Add route listing and route lookup diagnostics.
- Add route metrics and multiple route priorities.
- Add route replace, flush, and delete-by-interface operations.
- Add source-address selection for multi-interface hosts.
- React correctly to address, netmask, MTU, and link-state changes.
- Add a stable netdevice notification mechanism for address/link changes.
- Add `/proc/net/dev`, `/proc/net/route`, and `/proc/net/tcp`-style views
  or an equivalent diagnostics interface.

## P1: Robustness and operational quality

### 7. ARP

- Make retry counts and timeouts configurable.
- Add duplicate-address detection.
- Add gratuitous ARP.
- Add per-interface sysctl-style tuning.
- Improve pending-queue behavior under high connection churn.
- Add ARP statistics.

### 8. TCP and UDP error handling

- Surface ICMP port-unreachable errors to connected UDP sockets.
- Add proper TCP reset behavior for all invalid state transitions.
- Add MSS/PMTU feedback from ICMP errors.
- Add urgent-data handling or explicitly document it as unsupported.
- Add socket-level error statistics.

### 9. Network device model

- Add link-carrier state separate from administrative up/down.
- Add hardware address change support.
- Add promiscuous and all-multicast modes.
- Add per-queue statistics.
- Add netdevice notifier callbacks for address and link changes.
- Add MTU propagation to the driver where required.
- Document the `SIOCGIFCONF` / `SIOCGIFSTATS` contract and add regression tests.

### 10. Observability

- Add packet drop reasons.
- Add per-queue and per-protocol counters.
- Add a route table dump.
- Add a socket state dump covering TCP, UDP, and raw sockets.
- Add tracing points or debug counters for RX/TX paths.

## P2: Optional or future work

- IPv6 and ICMPv6.
- Multicast and IGMP.
- VLAN support.
- Multiple addresses per interface.
- DHCPv6.
- Firewall/netfilter support.
- Traffic shaping and QoS.
- Packet capture hooks.
- TLS belongs in userspace and is not a kernel networking item.

## Acceptance criteria for "generally usable"

The stack should pass all of the following before being considered generally
usable:

- A multi-hour run with an active DHCP lease and renewal completes without
  losing connectivity.
- A large TCP transfer completes correctly under packet loss and reordering.
- Multiple concurrent TCP connections, server restarts, and TIME_WAIT reuse
  work without leaks or port exhaustion.
- Large UDP datagrams survive fragmentation and reassembly without leaks.
- RX traffic at a sustained rate does not stall a single worker thread.
- Fragment and socket queues remain bounded under stress.
- `brktest net` and `brktest stress_net` pass, plus new loss/reorder/large-packet
  tests.
- No kernel panics, unbounded memory growth, or deadlocks under network stress.
- Interface, route, ARP, and socket diagnostics are available for debugging.

## Known limitations

- RX processing uses one worker thread and is not per-CPU NAPI.
- `SO_RCVBUF` and `SO_SNDBUF` are stored but do not yet control queue limits.
- virtio-net uses a single queue and no offloads.
- DHCP performs initial configuration only; lease renewal is not implemented.
- The IPv4 fragment table has a fixed small size and a fixed timeout.
- Raw sockets currently support ICMP only.
- IPv6, multicast, firewall, and QoS are not implemented.
