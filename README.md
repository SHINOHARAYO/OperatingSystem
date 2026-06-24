# Neptune OS

Neptune is an experimental AArch64 microkernel-style OS that boots as a UEFI
application under QEMU. It has isolated EL0 user tasks, capability-based IPC,
lazy virtual memory, zero-copy memory sharing, a user-space service model, an
EL1 VFS fast channel, and a four-core per-core O(1) MLFQ scheduler.

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
| `make run_g` | Rebuild and boot QEMU with the firmware `ramfb` graphical display |
| `make reset-storage` | Replace `build/storage.fat` with a fresh copy of `apps.fat` |
| `make clean` | Remove generated build output but preserve `build/storage.fat` |
| `make distclean` | Remove all generated build output, including `build/storage.fat` |

## Raspberry Pi 4 Serial Bring-Up

The Pi 4 target is a separate, serial-only UEFI bring-up profile. It does not
yet provide HDMI graphics, USB input, SD-card storage, or SMP; those services
currently depend on QEMU virtio devices.

Build its deployable EFI tree from WSL:

```sh
make PLATFORM=pi4 pi4-image
```

This creates `build/pi4/pi4-esp`, containing Neptune at
`EFI/BOOT/BOOTAA64.EFI`, its small initrd boot set, and a `config.txt` that
enables the PL011 UART on the GPIO header. Extract a Pi 4 UEFI firmware release
to a directory, then from PowerShell prepare the FAT32 SD-card partition:

```powershell
.\tools\prepare-pi4-boot.ps1 -FirmwareDir C:\path\to\RPi4-UEFI -Destination E:\
```

Use a 3.3 V USB-to-TTL adapter at 115200 8N1: adapter RX to GPIO14/pin 8,
adapter TX to GPIO15/pin 10, and ground to pin 6. Do not connect the adapter's
5 V lead. The initial shell must be used over this serial connection.

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
| `display_server` | EL0 | Firmware framebuffer drawing service |
| `compositor` | EL0 | Capability-shared surfaces and damage composition |
| `terminal` | EL0 | Graphical text console and focused keyboard bridge |
| `init` | EL0 | First user process; owns service startup policy |
| `shell` | EL0 | Command engine attached to graphical terminal or serial fallback |

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
- Per-ASID CPU residency masks track the cores that have executed each address
  space.
- Cross-core TLB invalidation uses AArch64 inner-shareable `TLBI ...IS`
  operations followed by `DSB ISH` and `ISB`, giving architectural broadcast
  completion without software SGI acknowledgement waits.
- ACPI/PCI discovery for the EL0 virtio block driver.

### Graphics

- The EFI stage discovers UEFI Graphics Output Protocol state before
  `ExitBootServices`.
- The EFI stage selects the highest supported GOP mode up to `1920x1080`
  before handing the framebuffer to the display service.
- The kernel maps the framebuffer only into the authorized `display.elf`
  service, together with a read-only display boot-info page.
- `display.elf` owns framebuffer drawing in EL0 and registers itself with the
  name server.
- RGB, BGR, and bitmask framebuffer pixel formats are supported.
- The display service draws a minimal console-style graphical boot log on a
  black background and exposes IPC
  operations for display information, clearing, clipped rectangle fills, and
  registered-buffer blits.
- `compositor.elf` owns a canonical RGB backbuffer and accepts client surfaces
  through lent memory capabilities. Uncovered compositor space remains black.
- Surface creation, ownership checks, movement, damage, and destruction happen
  in EL0. Only damaged rectangles are recomposed and sent to `display.elf`.
- The compositor assigns keyboard focus to the owner of the topmost newly
  created surface. `keyboard.elf` enforces that focus and restores shell input
  when no graphical surface owns it.
- The compositor uses timed IPC receive maintenance cycles to remove surfaces
  whose owner task exited, crashed, or was killed before destroying them.
- The shell `gfx` command queries the service and draws a color test.
- The shell `desktop` command creates three capability-shared compositor
  surfaces and safely destroys/revokes its previous demo surfaces when rerun.
- `/bin/snake.elf` is a standalone graphical application. The shell `snake`
  command launches it, and the game owns keyboard focus while its lent
  compositor surface exists. Its board scales to the active framebuffer
  resolution.
- `terminal.elf` owns a compositor surface containing a text grid, colored
  cursor, a 1,000-line scrollback ring, and basic ANSI color, clear-screen,
  cursor-home, and line-erase handling. Page Up, Page Down, Home, and End
  navigate scrollback while focused.
- User-space output is buffered before being enqueued to `terminal.elf`; the
  terminal replies to writers before compositor presentation and continues
  serving output while the shell waits for keyboard input.
- The shell binds reserved inherited `stdin`, `stdout`, and `stderr` endpoint
  capability slots. Spawned and forked programs therefore write to the same
  graphical terminal automatically, while output remains mirrored to
  `uart.elf` for serial fallback and diagnostics.
- `keyboard.elf` buffers UART and virtio-keyboard press, release, repeat, and
  modifier events. Focused clients use blocking input IPC; the driver uses a
  short scheduler sleep while waiting so clients do not repeatedly poll and
  focus-control IPC cannot deadlock.
- `make run_g` boots QEMU with firmware `ramfb`; `make run` remains serial-only.

### Scheduler

- Per-core scheduler structures with CPU-local current task, ready bitmap,
  ready queues, and timer wheel.
- Kernel subsystems use the scheduler current-task accessor instead of a
  shared exported `current_task` pointer.
- Secondary cores are started with PSCI, get private stacks, initialize their
  GIC CPU interface and local timer, register as online scheduler cores, and
  enter the scheduler through per-core idle TCBs.
- Bootstrap services stay on CPU0; ordinary user tasks, including IPC, VFS,
  and memory-capability clients, can be placed on or stolen by secondary cores.
- GICv2 SGI send support is present for IPI plumbing.
- Per-core ready queues and timer wheels are protected by IRQ-save scheduler
  spinlocks, preventing timer/reschedule IRQ self-deadlocks.
- Cross-core reschedule requests can be delivered through SGI/IPI.
- ASID allocation and task lookup/growth have first-pass SMP locks.
- Task slots are stable non-moving TCB allocations behind a growable pointer
  table, so scheduler, IPC, and VFS task pointers survive task-capacity growth.
- Public task lookup uses held references with deferred slot recycling, so
  killed tasks are removed immediately but their TCB slot is not reused until
  outstanding kernel references are released.
- IPC endpoint queues and VFS service queues use IRQ-save endpoint-task locks
  for atomic enqueue/dequeue/block transitions.
- Tasks carry a scheduler core id so ready/timer ownership survives task-table
  growth and ready-task migration.
- Idle cores use lock-ordered queue-transfer work stealing for eligible
  `READY` user tasks. A stolen task remains visible on exactly one ready queue.
- Remote kill requests are completed by the target task's owner core, avoiding
  cross-core address-space teardown.
- Child termination records and wait transitions are serialized, preventing
  lost wakeups when a child exits on another core.
- Four simultaneous normal EL0 workloads execute on CPUs 0 through 3 under
  QEMU, and remote kill/wait lifecycle cleanup works across those cores.
- O(1) MLFQ with 32 circular ready queues.
- Bitmap readiness tracking with native AArch64 `CLZ` selection.
- Queue 0 is highest priority and maps to bitmap bit 31.
- Exponential time slices:
  - queues 0-3: 1 ms
  - queues 4-7: 2 ms
  - queues 8-15: 4 ms
  - queues 16-23: 8 ms
  - queues 24-31: 16 ms
- 1024-bucket owner-core timer wheels with locked insert, remove, timeout, and
  IRQ-wakeup paths.
- Per-core local priority boost every 1000 ticks avoids cross-core ready-queue
  rebuilding while preventing starvation on each CPU.
- Yield keeps priority; CPU-bound quantum exhaustion demotes.
- Blocking on IPC, VFS, sleep, wait, IRQ, or fault does not demote.

### Memory Management

- Physical page allocator.
- Kernel heap diagnostics.
- Frame objects with refcounts above PMM pages.
- PMM allocation, frame refcounts, page-table mutation, VM objects, and the
  page cache are serialized for concurrent use by user tasks on different
  cores.
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
- Per-task capability table growth, lookup, install, copy, clear, and revoke
  operations are serialized by Orange Cat.
- Typed slots with object IDs, rights, and flags.
- Endpoint, reply, exec, file, VMA, frame, and DMA cap types.
- Reserved `CAP_SELF = 1`, `CAP_NS = 2`, and inherited endpoint slots
  `CAP_STDIN = 3`, `CAP_STDOUT = 4`, and `CAP_STDERR = 5`.
- Capability checks for IPC, spawn, file access, memory mapping, and DMA
  address resolution.
- `capstat` shell command.

### IPC

- Capability-based synchronous IPC.
- `ipc_call`, `ipc_recv`, one-shot `ipc_reply`, and atomic `ipc_reply_recv`.
- Register-only fast path for payloads up to 128 bytes in `x3` through `x18`.
- `ipc_reply_recv` parks a server on its receive endpoint before handing control
  back to the caller, allowing repeated same-core request/reply traffic to avoid
  ready-queue insertion and dynamic reply-cap allocation.
- Direct rendezvous uses a generation-tagged one-shot reply token held entirely
  in kernel task state. Ordinary `ipc_recv` keeps the capability-backed reply
  path, so services opt into the tighter fast-path lifecycle explicitly.
- Slow path with one zero-copy memory attachment.
- Memory attachment modes: share, transfer, lend, revoke.
- Handoff scheduling from caller to waiting receiver and replier to caller.
- Kill/exit cleanup unwinds IPC queues and reply caps.
- Per-core IPC profiling records call-path selection, capability lookup counter
  ticks, and round-trip time; inspect aggregate ticks and nanoseconds with
  `ipcstat`.
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
clear the screen, and Up/Down history recall for the last 32 commands. The
graphical terminal keeps 1,000 lines of scrollback navigable with Page Up,
Page Down, Home, and End.

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
| `gfx` | Query and exercise the EL0 display service |
| `desktop` | Draw capability-shared compositor surfaces |
| `snake` | Play graphical Snake using WASD |

IPC and memory commands:

| Command | Purpose |
| --- | --- |
| `ipcfast` | Test 0/8/64/128-byte register IPC payloads |
| `ipccap` | Transfer an endpoint cap through IPC |
| `ipckill` | Verify IPC cleanup on task kill |
| `ipcstat` | Show IPC fast-path hit rate and cycle counters |
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
| `snake.elf` | `user/snake.c` | Standalone graphical Snake game |

The `/sbin` service set contains `init.elf`, `uart.elf`, `keyboard.elf`,
`display.elf`, `compositor.elf`, `terminal.elf`, `ns.elf`, `block.elf`, and
`fs.elf`.

## Known Limitations

- Bootstrap and hardware-facing service tasks remain pinned to CPU0. Ordinary
  IPC/VFS clients migrate, but migrating the services themselves still needs a
  complete device-state and service-lifecycle audit.
- Work stealing currently moves only eligible `READY` tasks. Running, blocked,
  sleeping, timer-owned, and service tasks do not migrate yet.
- Direct register IPC currently requires a same-core caller/server rendezvous
  and an `ipc_reply_recv` server loop. Cross-core calls and ordinary receive
  loops use the queue-based capability reply path.
- Memory-cap mapping/object lifecycle, VMA metadata mutation, and some
  device-state tables still need stronger synchronization before arbitrary
  service tasks can migrate.
- Page-table mutation and page-cache fills currently use global locks. They are
  correct for current SMP workloads but may become contention points.
- The `/boot` seed still contains `init.elf`, `ns.elf`, `block.elf`, and
  `fs.elf` because those are needed before `storage.fat` can be mounted; the
  normal service copies, shell, apps, and boot policy live in `storage.fat`.
- The block driver requires a modern virtio-blk PCI device for `storage.fat`;
  there is no RAM-disk fallback yet.
- Graphics currently use a firmware `ramfb` linear framebuffer. There is no
  virtio-gpu modesetting, acceleration, or pointer input yet.
- The compositor uses fixed-size surface and display-buffer tables. Clients
  cannot resize surfaces or request window chrome.
- The graphical terminal uses a compact built-in ASCII bitmap font and one
  shared console session. It does not yet support selectable text, Unicode,
  multiple sessions, or a general PTY byte-stream object.
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
- Add pointer routing, stacking controls, resize events, and window chrome,
  then replace firmware `ramfb` with a user-space virtio-gpu driver.
- Generalize the inherited standard-stream endpoints into reusable PTY session
  objects so multiple graphical terminals and shells can coexist.
- Establish a `no_std` Rust userspace target, then evaluate Slint's software
  renderer as an optional high-level application toolkit while keeping display,
  compositor, terminal, and input protocols toolkit-independent.
- Add host-side automated QEMU smoke scripts with SMP assertions.
- Extend migration to blocked/timer-owned tasks with explicit ownership
  transfer, then evaluate service-task migration.
- Add per-address-space VMA locking and finish memory-cap and device-state
  lifecycle synchronization.
- Replace the global page-table mutation lock with per-address-space locks.
- Replace fixed-size v1 object tables with growable or reclaiming allocators.
