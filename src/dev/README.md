# Device model and driver layout

This directory is organised around four layers:

```text
core         generic device model, device numbers, devtmpfs
bus          bus matching plus transport implementations
class core   char, block, tty and network infrastructure
driver       concrete hardware or function drivers
```

## Directory map

```text
dev/
├── core/                 device model and services
│   ├── device.c          device/bus/driver, probe, reference counting
│   ├── devno.c           dev_t major/minor allocator
│   └── devtmpfs.c        /dev node creation from struct device
├── bus/
│   ├── platform/         platform bus
│   └── virtio/           virtio bus and virtio-mmio transport
├── of/                   device tree parsing and platform device creation
├── char/                 character-device core
├── tty/                  tty core, line discipline and serial drivers
├── block/                block I/O core, gendisk and block drivers
├── net/                  network device drivers (net core lives in src/net)
├── irqchip/              interrupt controller drivers
└── rtc/                  RTC drivers
```

## Object relationships

```text
VFS
 ├── char devices: inode rdev -> char_dev -> fs_file_ops
 │                  -> tty port -> tty driver -> UART driver
 └── block devices: block_dev -> bio/request_queue -> gendisk
                    -> block driver -> transport

device model:
  OF -> platform devices -> platform bus -> platform drivers
       platform bus -> virtio-mmio transport -> virtio devices
       virtio bus -> virtio function drivers -> block/net/tty classes
```

`devtmpfs` is the only component that turns a registered `struct device`
with `devt` into a `/dev` node.

## Layering rules

1. `core/` must not depend on char, block, tty, net, or concrete drivers.
2. Bus and transport code must not know about VFS classes or `/dev` nodes.
3. Class cores own their dev_t registration and file-operation dispatch.
4. Concrete drivers may depend on core, bus, and their class core, but class
   cores must not depend on concrete drivers.
5. OF code only creates platform devices; it does not know about classes.
6. Device identity lives in `struct device`; class-specific objects such as
   `char_dev`, `block_dev`, and `tty_port` reference it explicitly.
7. New devices that need a `/dev` node must register exactly one `struct
   device` with `devt` and `mode` set.

## Exceptions

- `irqchip` is probed directly from the OF tree during early boot. Interrupt
  controllers must be available before normal platform devices resolve IRQs,
  so this path intentionally bypasses the platform bus.
- `net_device` embeds a `struct device` for parent and lifecycle integration,
  but it has no `devt`, so devtmpfs does not create `/dev` nodes for network
  interfaces.
