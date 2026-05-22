# Neptune OS

Neptune is an experimental AArch64 microkernel-style OS that boots as a UEFI
application under QEMU. It has isolated EL0 user tasks, capability-based IPC,
lazy virtual memory, zero-copy memory sharing, a user-space service model, an
EL1 VFS fast channel, and a CPU0 O(1) MLFQ scheduler.

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
| `make` | Build the kernel EFI image, user ELFs, and initrd |
| `make image` | Create `build/fat.img` with `BOOTAA64.EFI` and `initrd.bin` |
| `make run` | Boot QEMU in `-nographic` mode |
| `make run_g` | Boot QEMU with a graphical display |
| `make clean` | Remove generated build output |

The default QEMU memory size is `2048M`.

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
| `fs_server` | EL0 | Initrd-backed filesystem service |
| `shell` | EL0 | Interactive command shell |

The kernel keeps authority, scheduling, memory mappings, faults, and IPC
handoff. Filesystem metadata and directory state now live in `fs.elf` user
memory. The boot initrd is mapped read-only into the FS task at
`0xD0000000`; inside it, `fs.elf` mounts `apps.fat` with FatFs and serves file
operations from that real FAT volume.

## Implemented Features

### Boot And Platform

- AArch64 UEFI boot under QEMU.
- FAT image boot with `BOOTAA64.EFI` and `initrd.bin`.
- Exception vectors, MMU, ACPI discovery, GICv2, and ARM generic timer setup.
- 1 kHz timer tick.
- 39-bit lower-half virtual address layout.
- 40-bit physical-address support.
- ASID-aware user address spaces with targeted TLB invalidation.

### Scheduler

- CPU0 per-core scheduler structure.
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
- Global priority boost every 1000 ticks.
- Yield keeps priority; CPU-bound quantum exhaustion demotes.
- Blocking on IPC, VFS, sleep, wait, IRQ, or fault does not demote.

### Memory Management

- Physical page allocator.
- Kernel heap diagnostics.
- Frame objects with refcounts above PMM pages.
- VM objects for anonymous and initrd-backed mappings.
- Per-task VMA tables for ELF, stack, guard, mmap, imports, and file mappings.
- Dynamically growing VMA tables with sorted lookup.
- VMA split/remove/coalesce support.
- Lazy anonymous `mmap`.
- Lazy file-backed mappings.
- Lazy executable loading for initrd-backed spawns.
- Dynamic stack growth up to 64 KiB with a moving guard page.
- `fork` with copy-on-write for writable frame-backed user pages.
- Public `munmap`.
- `vmmap` inspection from the shell.

### Fault And User Safety

- User stack guard page.
- Shared fault resolver for lazy memory, file mappings, and stack growth.
- Usercopy resolves valid lazy pages during syscall copy-in/copy-out.
- Invalid user pointers are rejected.
- User faults kill only the faulting task.

### Orange Cat Capabilities

- Unified per-task capability table.
- Typed slots with object IDs, rights, and flags.
- Endpoint, reply, exec, file, VMA, and frame cap types.
- Reserved `CAP_SELF = 1` and `CAP_NS = 2`.
- Capability checks for IPC, spawn, file access, and memory mapping.
- `capstat` shell command.
- Compatibility boot grants for FS file/exec caps are still installed for
  legacy FS IPC paths.

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
- FatFs is vendored in `third_party/fatfs`.
- The build creates an `apps.fat` FAT volume containing the user ELFs.
- `fs.elf` mounts `apps.fat` in EL0 and owns directory/open-handle state.
- `ls` uses the VFS path.
- `fsls` remains as the legacy FS-over-IPC fallback.
- `run <file>` loads executables through VFS and spawns them from shell memory.

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

Core commands:

| Command | Purpose |
| --- | --- |
| `help` | Show shell help |
| `clear` | Clear the terminal |
| `echo <text>` | Print text |
| `uptime` | Show uptime |
| `mem` | Show RAM and heap usage |
| `vmmap` | Show this task's VMA table |
| `capstat` | Show this task's capability slots |
| `debug` | Show kernel/runtime debug info |
| `ps` | Show task state |
| `jobs` | Show shell child jobs |
| `poll <tid>` | Check child status without reaping |
| `wait <tid>` | Wait for and reap child status |
| `kill <tid>` | Kill a user task |

File and VFS commands:

| Command | Purpose |
| --- | --- |
| `ls` | List files through the VFS fast channel |
| `fsls` | List files through legacy FS IPC |
| `vfstest` | Test VFS call/reply handoff |
| `vfsinject` | Test shared page injection |
| `vfsls` | Explicit VFS file listing |
| `vfsopen <file>` | Show VFS metadata for one file |
| `vfsread` | Read one file page through VFS injection |
| `vfsexec` | Resolve an executable through VFS and spawn it |
| `run <file>` | Load and spawn an initrd ELF through VFS |
| `filelazy` | Read a file page through the lazy/VFS path |
| `vmstress` | Stress VMA growth with file mappings |

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

Regression and demo commands:

| Command | Purpose |
| --- | --- |
| `smoke` | Run bounded core regression checks |
| `lazy` | Test lazy mmap and usercopy |
| `lazyexec` | Test lazy executable VM objects |
| `taskstress` | Grow the kernel task table |
| `speed` | Benchmark syscalls, yield, faults, memory, and IPC |
| `pong` | Run the IPC pong demo |
| `fault` | Spawn a deliberate faulting task |
| `badptr` | Verify invalid syscall pointers are rejected |
| `spawn pong` | Spawn pong and leave status for `wait` |
| `spawn fault` | Spawn a faulting child |
| `spawn spin` | Spawn a long-running child |

## Demos

Run a long-lived child:

```text
spawn spin
ps
jobs
kill 4
wait 4
```

Run the VFS path:

```text
ls
vfstest
vfsinject
vfsread
run badptr.elf
wait 7
```

Run core smoke checks:

```text
smoke
```

The current smoke path covers lazy memory, VFS call/injection, register IPC,
invalid pointers, stack growth, and the speed benchmark.

## User Programs

The current initrd contains:

| Program | Source | Purpose |
| --- | --- | --- |
| `shell.elf` | `user/shell.c` | Interactive shell |
| `uart.elf` | `user/uart.c` | UART service |
| `keyboard.elf` | `user/keyboard.c` | Keyboard service |
| `ns.elf` | `user/ns.c` | Name server |
| `fs.elf` | `user/fs.c` | Initrd FS and VFS service |
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

## Known Limitations

- Single-core CPU0 scheduling only; the structures are ready for later SMP.
- User programs are still packaged into `initrd.bin` at build time.
- The current public FS backend is a read-only FAT12 boot volume inside initrd.
- No persistent filesystem or file writes yet.
- No user-space block driver or DMA path yet.
- VFS currently fronts the FatFs-backed boot volume only.
- FS-over-IPC compatibility still exists while the VFS path finishes taking
  over every file operation.
- The initrd page cache and VM object tables are fixed-size v1 tables.
- Memory-cap object and mapping tables are fixed-size v1 tables.
- Copy-on-write currently covers forked, writable, frame-backed user pages.
- Terminal input is intentionally simple and ASCII-oriented.
- `wait` and `poll` only apply to children of the calling task.

## Good Next Steps

- Remove the remaining legacy FS-over-IPC path once VFS covers every command.
- Move executable spawning to a cleaner kernel-owned VFS exec object instead of
  shell-loaded bytes.
- Add a writable user-space filesystem service.
- Add a user-space block driver and connect it to VFS injected buffers.
- Add host-side automated QEMU smoke scripts.
- Extend the per-core scheduler work toward SMP.
- Replace fixed-size v1 object tables with growable or reclaiming allocators.
