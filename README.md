# Neptune OS

Neptune is an experimental AArch64 microkernel-style OS that boots as a UEFI
application under QEMU. It has isolated EL0 user tasks, capability-based IPC,
lazy virtual memory, zero-copy memory sharing, a user-space service model, an
EL1 VFS fast channel, and an SMP-shaped per-core O(1) MLFQ scheduler.

This is a teaching and research OS. It is not production software.

## Quick Start

Build from Linux or WSL in the repository root:

```sh
make image
make run
```

The build expects:

- `clang`
- `lld-link`
- `xxd`
- `dosfstools`
- `mtools`
- `qemu-system-aarch64`
- `qemu-efi-aarch64`

Useful targets:

| Target | Purpose |
| --- | --- |
| `make` | Build the kernel EFI image |
| `make image` | Create `build/fat.img`, copy the `/boot` seed into it, create `build/storage.fat` only if missing, and refresh app ELFs in place |
| `make update-storage-apps` | Copy freshly built user ELFs into existing `build/storage.fat` without formatting it |
| `make run` | Rebuild the image targets and boot QEMU in `-nographic` mode |
| `make run_g` | Rebuild the image targets and boot QEMU with a graphical display |
| `make reset-storage` | Replace `build/storage.fat` with a fresh copy of `apps.fat` |
| `make clean` | Remove generated build output but preserve `build/storage.fat` |
| `make distclean` | Remove all generated build output, including `build/storage.fat` |

The default QEMU memory size is `2048M`; the default QEMU CPU count is `4`.

```sh
make run QEMU_MEM=1024M
```

Some QEMU CPU/backend combinations expose only a 32-bit physical address space.
On `virt`, RAM starts at `0x40000000`, so those combinations cannot boot all
memory sizes.

## Logging

The kernel log level is selected at build time:

| Value | Meaning |
| --- | --- |
| `LOG_LEVEL=0` | Quiet: failures only |
| `LOG_LEVEL=1` | Normal boot milestones and warnings |
| `LOG_LEVEL=2` | Debug logs |
| `LOG_LEVEL=3` | Trace logs |

Example:

```sh
make image LOG_LEVEL=1
```

## System Shape

Neptune currently boots this service stack:

| Service | Runs In | Purpose |
| --- | --- | --- |
| `uart_server` | EL0 | Serial output service |
| `keyboard_server` | EL0 | Keyboard input service |
| `name_server` | EL0 | Service registry and endpoint lookup |
| `block_server` | EL0 | Virtio PCI block driver for persistent storage |
| `fs_server` | EL0 | FatFs-backed filesystem and VFS service |
| `init` | EL0 | First user process; owns service startup policy |
| `shell` | EL0 | Interactive command shell launched from `storage.fat` |

The kernel keeps authority, scheduling, memory mappings, faults, and IPC
handoff. Filesystem metadata and directory state now live in `fs.elf` user
memory. The kernel discovers the persistent virtio block device through ACPI
MCFG/PCI ECAM, maps the device BAR into `block.elf`, and exposes
`build/storage.fat` as a persistent sector device over capability IPC. `fs.elf`
mounts the block service with FatFs and serves file operations through the VFS
fast channel.

The EFI stage reads the tiny `/boot` seed from the FAT boot image:
`init.elf`, `ns.elf`, `block.elf`, and `fs.elf`. It builds a small in-memory
boot archive from those files. The kernel creates only `init.elf`; init starts
the storage foundation from that archive, waits for VFS, then reads the
persistent `/etc/boot.txt` from `storage.fat` and launches disk-backed services
such as `/sbin/uart.elf`, `/sbin/keyboard.elf`, and `/bin/shell.elf`.

## Implemented Features

### Boot And Platform

- AArch64 UEFI boot under QEMU.
- FAT image boot with `BOOTAA64.EFI` and a minimal `/boot` seed.
- Exception vectors, MMU, ACPI MADT/MCFG discovery, GICv2, and ARM generic
  timer setup.
- 1 kHz timer tick.
- 39-bit lower-half virtual address layout.
- 40-bit physical-address support.
- ASID-aware user address spaces with targeted TLB invalidation.
- Per-ASID CPU residency masks keep ordinary VA+ASID and ASID shootdowns
  limited to cores that have actually executed that address space.
- Cross-core TLB shootdown uses GIC SGIs with per-core acknowledgements for
  VA+ASID, VA-all-ASID, ASID, and full invalidations.
- ACPI/PCI discovery for the EL0 virtio block driver.

### Scheduler

- Per-core scheduler structures with CPU-local current task, ready bitmap,
  ready queues, and timer wheel.
- Kernel subsystems use the scheduler current-task accessor instead of a
  shared exported `current_task` pointer.
- Secondary cores are started with PSCI, get private stacks, initialize their
  GIC CPU interface and local timer, register as online scheduler cores, and
  enter the scheduler through per-core idle TCBs.
- Bootstrap services and IPC/VFS-heavy tasks stay on CPU0; normal priority-0
  user workloads can be placed on or stolen by secondary cores.
- GICv2 SGI send support is present for IPI plumbing.
- Per-core ready queues are protected by scheduler spinlocks.
- Cross-core reschedule requests can be delivered through SGI/IPI.
- ASID allocation and task lookup/growth have first-pass SMP locks.
- Task slots are stable non-moving TCB allocations behind a growable pointer
  table, so scheduler, IPC, and VFS task pointers survive task-capacity growth.
- Public task lookup uses held references with deferred slot recycling, so
  killed tasks are removed immediately but their TCB slot is not reused until
  outstanding kernel references are released.
- IPC endpoint queues and VFS service queues use endpoint-task locks so
  enqueue/dequeue/block transitions are atomic enough for the current SMP
  model.
- Tasks carry a scheduler core id so ready/timer ownership survives task-table
  growth and ready-task migration.
- Idle cores use lock-ordered queue-transfer work stealing for eligible
  `READY` user tasks. A stolen task remains visible on exactly one ready queue.
- Remote kill requests are completed by the target task's owner core, avoiding
  cross-core address-space teardown.
- O(1) MLFQ with 32 circular ready queues.
- Bitmap readiness tracking with native AArch64 `CLZ` selection.
- Queue 0 is highest priority and maps to bitmap bit 31.
- Exponential time slices:
  - queues 0-3: 1 ms
  - queues 4-7: 2 ms
  - queues 8-15: 4 ms
  - queues 16-23: 8 ms
  - queues 24-31: 16 ms
- 1024-bucket timer wheel for sleep and IRQ timeout wakeups.
- Per-core local priority boost every 1000 ticks avoids cross-core ready-queue
  rebuilding while preventing starvation on each CPU.
- Yield keeps priority; CPU-bound quantum exhaustion demotes.
- Blocking on IPC, VFS, sleep, wait, IRQ, or fault does not demote.

### Memory Management

- Physical page allocator.
- Kernel heap diagnostics.
- Frame objects with refcounts above PMM pages.
- VM objects for anonymous and boot-archive-backed mappings.
- Per-task VMA tables for ELF, stack, guard, mmap, imports, and file mappings.
- Dynamically growing VMA tables with sorted lookup.
- VMA split/remove/coalesce support.
- Lazy anonymous `mmap`.
- Lazy file-backed mappings.
- Lazy executable segment loading for kernel boot-archive-backed spawn paths.
- Dynamic stack growth up to 64 KiB with a moving guard page.
- `fork` with copy-on-write for writable frame-backed user pages.
- Public `munmap`.
- Capability-scoped DMA export objects for EL0 drivers that need device-visible
  physical addresses.
- `vmmap` inspection from the shell.

### Fault And User Safety

- User stack guard page.
- Shared fault resolver for lazy memory, file mappings, and stack growth.
- Usercopy resolves valid lazy pages during syscall copy-in/copy-out.
- Invalid user pointers are rejected.
- User faults kill only the faulting task.

### Object Capabilities

- Unified per-task capability table.
- Typed slots with object IDs, rights, and flags.
- Endpoint, reply, exec, file, VMA, frame, and DMA cap types.
- Reserved `CAP_SELF = 1` and `CAP_NS = 2`.
- Capability checks for IPC, spawn, file access, memory mapping, and DMA
  address resolution.
- `capstat` shell command.

### IPC

- Capability-based synchronous IPC.
- `ipc_call`, `ipc_recv`, and one-shot `ipc_reply`.
- Register-only fast path for payloads up to 128 bytes in `x3` through `x18`.
- Slow path with one zero-copy memory attachment.
- Memory attachment modes: share, transfer, lend, revoke.
- Handoff scheduling from caller to waiting receiver and replier to caller.
- Kill/exit cleanup unwinds IPC queues and reply caps.
- IPC implementation lives in `src/kernel/ipc.c`.

### VFS And Files

- EL1 VFS fast channel in `src/kernel/vfs.c`.
- Per-task fixed VFS route arrays.
- `SYS_VFS_BIND`, `SYS_VFS_CALL`, `SYS_VFS_RECV`, `SYS_VFS_REPLY`, and
  `SYS_VFS_INJECT`.
- Direct handoff from client to FS service for VFS calls.
- Shared page injection maps pages into both client and FS address spaces.
- Injection preserves already-mapped client page contents, so the same path can
  move data FS-to-client or client-to-FS.
- FatFs is vendored in `third_party/fatfs`.
- The build creates an `apps.fat` FAT seed volume with apps under `/bin`,
  services under `/sbin`, and boot policy under `/etc/boot.txt`.
- The build creates `build/storage.fat` from `apps.fat` only when it is missing,
  so filesystem writes survive normal rebuilds.
- Normal image builds refresh app ELFs inside `build/storage.fat` in place, so
  user-created files survive while rebuilt programs become runnable from disk.
- The standalone `initrd.bin` file is no longer used. The EFI stage loads the
  minimal boot seed from `/boot` in the FAT boot image; demos, tests, UART,
  keyboard, and the shell run from `storage.fat`.
- `block.elf` implements a userspace modern virtio-pci block driver and exposes
  512-byte sectors over IPC.
- The kernel parses ACPI MCFG, enumerates PCI ECAM, finds the modern virtio-blk
  PCI function, enables memory/bus-master access, and grants `block.elf` a
  discovered BAR mapping plus a read-only boot-info page.
- `block.elf` uses DMA caps to pin and resolve only explicitly exported buffers
  before programming virtio descriptors.
- `fs.elf` talks to `block.elf` through IPC-backed shared memory and mounts the
  result in EL0 with FatFs.
- `fs.elf` owns directory and open-handle state.
- `fs.elf` supports normalized paths, directory scanning, and `mkdir`.
- `fs.elf` can create or replace files through FatFs writes.
- `fs.elf` exposes root filesystem capacity through VFS `statfs`; the shell
  reports it with `mounts`.
- The shell can inspect, copy, install, and remove files through VFS.
- `recvhex <file> <size>` can ingest a new file over the serial console without
  rebuilding the OS image.
- `ls [path]` uses the VFS path. For example: `ls /`, `ls /bin`, `ls /sbin`.
- `run <file>` asks `fs.elf` to read the executable, creates a kernel-owned
  VFS exec object, and spawns through an `OCAP_EXEC` handle.
- Short executable names fall back to `/bin`, so both `run badptr.elf` and
  `run /bin/badptr.elf` work.
- Copied executables can be launched directly from `storage.fat`.
- VFS exec caps are one-shot spawn tickets; the kernel releases the backing
  exec object after the spawn attempt.

### Task Lifecycle

- EL0 user task creation and context switching.
- Dynamically growing task and termination-record tables.
- Parent-child tracking.
- `ps`, `jobs`, `poll`, `wait`, and `kill`.
- Exit records survive until reaped by the parent.
- Kill/exit cleanup handles scheduler, IPC, VFS, caps, VMAs, and memory caps.

## Shell Commands

After boot:

```text
Neptune>
```

The shell supports a 256-byte command line, end-of-line editing, Backspace,
Delete, Ctrl-U to clear the current line, Ctrl-C to cancel the line, Ctrl-L to
clear the screen, and Up/Down history recall for the last 32 commands.

Use `help` for grouped command help, or `help <command>` for one command.

Core commands:

| Command | Purpose |
| --- | --- |
| `help [command]` | Show grouped help or details for one command |
| `clear` | Clear the terminal |
| `echo <text>` | Print text |
| `uptime` | Show uptime |

File and VFS commands:

| Command | Purpose |
| --- | --- |
| `ls [path]` | List files through the VFS fast channel |
| `cat <file>` | Print a file through VFS |
| `cp <src> <dst>` | Copy a file through VFS |
| `install <src> <dst>` | Copy an executable into the writable filesystem |
| `recvhex <file> <size>` | Receive hex bytes from the console into a file |
| `rm <file>` | Remove a file through VFS |
| `mkdir <path>` | Create a directory through VFS |
| `mounts` | Show mounted filesystems |
| `vfstest` | Test VFS call/reply handoff |
| `vfsinject` | Test shared page injection |
| `vfsls [path]` | Explicit VFS file listing |
| `vfsopen <file>` | Show VFS metadata for one file |
| `vfsread` | Read one file page through VFS injection |
| `vfswrite` | Create and verify `notes.txt` through writable FatFs |
| `vfsinstall` | Copy an ELF, run it from the writable FS, then remove it |
| `vfsexec` | Resolve an executable through VFS, create an exec cap, and spawn it |
| `run <file>` | Spawn an ELF through a kernel-owned VFS exec object |

Process commands:

| Command | Purpose |
| --- | --- |
| `ps` | Show task scheduler state |
| `jobs` | Show shell child jobs |
| `spawn <pong|fault|spin>` | Spawn a demo child and leave status for `wait` |
| `kill <tid>` | Kill a user task |
| `wait <tid>` | Wait for and reap child status |
| `poll <tid>` | Check child status without reaping |

Diagnostic commands:

| Command | Purpose |
| --- | --- |
| `mem` | Show RAM and heap usage |
| `vmmap` | Show this task's VMA table |
| `capstat` | Show this task's capability slots |
| `debug` | Show kernel/runtime debug info |

IPC and memory commands:

| Command | Purpose |
| --- | --- |
| `ipcfast` | Test 0/8/64/128-byte register IPC payloads |
| `ipccap` | Transfer an endpoint cap through IPC |
| `ipckill` | Verify IPC cleanup on task kill |
| `memshare` | Share one page with another task |
| `memxfer` | Transfer one page to another task |
| `memcaplife` | Use a memory cap after unmapping the old VA |
| `memrevoke` | Lend and revoke a page |
| `munmap` | Test unmap and stale export rejection |
| `forkcow` | Verify copy-on-write after fork |

Test and demo commands:

| Command | Purpose |
| --- | --- |
| `demo [quick|vfs|ipc|mem|proc|all]` | Run curated bounded demo suites |
| `smoke` | Run bounded core regression checks |
| `vfstest` | Test VFS call/reply handoff |
| `vfsinject` | Test shared page injection |
| `filelazy` | Read a file page through the VFS page path |
| `vmstress` | Stress VMA growth with file mappings |
| `lazyexec` | Spawn `badptr.elf` through the VFS executable path |
| `taskstress` | Stress repeated task spawn, kill, and reap |
| `speed` | Benchmark syscalls, yield, faults, memory, and IPC |
| `pong` | Run the IPC pong demo |
| `fault` | Spawn a deliberate faulting task |
| `badptr` | Verify invalid syscall pointers are rejected |

## Demos

Use the curated demo runner for normal checks:

```text
demo
demo quick
demo vfs
demo ipc
demo mem
demo proc
```

`demo all` runs every suite. It is intentionally noisy and slower, so use it
when you want broad confidence rather than a quick smoke.

Heavier stress probes such as `ipckill` stay as individual commands so the
curated suites return cleanly to the shell.

Process lifecycle demo:

```text
spawn spin
ps
jobs
kill 4
wait 4
```

VFS path demo, expanded:

```text
ls
vfstest
vfsinject
vfsread
vfswrite
vfsinstall
run badptr.elf
wait 7
```

Run core smoke checks:

```text
smoke
```

The current quick smoke path covers lazy memory, VFS call/injection, VFS
writeback, register IPC, invalid pointers, stack growth, and the speed
benchmark.

## User Programs

The FAT boot image contains only the `/boot` seed needed to reach
`storage.fat`:

| Program | Source | Purpose |
| --- | --- | --- |
| `init.elf` | `user/init.c` | First user process; starts services from policy |
| `ns.elf` | `user/ns.c` | Name server |
| `block.elf` | `user/block.c` | Virtio-pci block service |
| `fs.elf` | `user/fs.c` | FatFs and VFS service |

The persistent `storage.fat` layout is:

| Path | Contents |
| --- | --- |
| `/bin` | Shell, demos, tests, and benchmarks |
| `/sbin` | Init and service binaries |
| `/etc/boot.txt` | Persistent boot manifest for disk-launched services |

The `/bin` app set contains:

| Program | Source | Purpose |
| --- | --- | --- |
| `shell.elf` | `user/shell.c` | Interactive shell |
| `pong.elf` | `user/pong.c` | IPC demo |
| `fault.elf` | `user/fault.c` | Deliberate fault test |
| `spin.elf` | `user/spin.c` | Long-running task |
| `badptr.elf` | `user/badptr.c` | Invalid pointer test |
| `stackgrow.elf` | `user/stackgrow.c` | Stack growth test |
| `memshare.elf` | `user/memshare.c` | Memory share receiver |
| `memxfer.elf` | `user/memxfer.c` | Memory transfer receiver |
| `memrevoke.elf` | `user/memrevoke.c` | Lend/revoke receiver |
| `ipcfast.elf` | `user/ipcfast.c` | Register IPC test |
| `ipccap.elf` | `user/ipccap.c` | Capability transfer test |
| `ipckill.elf` | `user/ipckill.c` | IPC kill cleanup test |
| `speed.elf` | `user/speed.c` | Performance benchmark |
| `speedipc.elf` | `user/speedipc.c` | IPC benchmark helper |

The `/sbin` service set contains `init.elf`, `uart.elf`, `keyboard.elf`,
`ns.elf`, `block.elf`, and `fs.elf`.

## Known Limitations

- Secondary cores run and steal normal EL0 workloads, but IPC/VFS-heavy tasks
  remain pinned to CPU0 until shared service handoff paths are fully
  SMP-hardened.
- Work stealing currently moves only eligible `READY` tasks. Running, blocked,
  sleeping, timer-owned, and service tasks do not migrate yet.
- TLB shootdowns use one request mailbox per target core. ASID residency keeps
  contention low, but fully concurrent cross-core mapping mutation still needs
  a serialized or generation-queued shootdown protocol.
- Timer-wheel ownership, IPC/VFS queues, and remaining shared object tables
  still need a complete IRQ-safe locking audit before arbitrary tasks can
  migrate between cores.
- The `/boot` seed still contains `init.elf`, `ns.elf`, `block.elf`, and
  `fs.elf` because those are needed before `storage.fat` can be mounted; the
  normal service copies, shell, apps, and boot policy live in `storage.fat`.
- The block driver requires a modern virtio-blk PCI device for `storage.fat`;
  there is no RAM-disk fallback yet.
- DMA caps pin frame-backed user pages, but there is still no IOMMU or device
  isolation layer.
- VFS currently fronts one FatFs-backed volume only.
- The boot-archive page cache and VM object tables are fixed-size v1 tables.
- Memory-cap object and mapping tables are fixed-size v1 tables.
- Copy-on-write currently covers forked, writable, frame-backed user pages.
- Terminal input is intentionally simple and ASCII-oriented.
- `wait` and `poll` only apply to children of the calling task.

## Good Next Steps

- Add IOMMU-style device domains and per-device DMA authority.
- Add host-side automated QEMU smoke scripts with SMP assertions.
- Extend migration to blocked/timer-owned tasks with explicit ownership
  transfer, then evaluate service-task migration.
- Add IRQ-safe locks and a concurrent TLB shootdown request protocol.
- Replace fixed-size v1 object tables with growable or reclaiming allocators.
