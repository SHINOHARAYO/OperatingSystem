# Neptune OS

Neptune is a small AArch64 microkernel-style OS that boots as a UEFI
application under QEMU. It has isolated EL0 user tasks, the Orange Cat unified
kernel capability subsystem, capability-based synchronous IPC, register-only
fast messages, zero-copy memory attachments, a name server, a simple initrd FS
service, a shell, task lifecycle syscalls, per-task VMA tracking, lazy
anonymous memory, dynamic stack growth, ASID-aware address spaces, and basic
memory diagnostics. The scheduler is now a CPU0 O(1) MLFQ using 32 ready
queues, bitmap/CLZ selection, a 1 kHz generic timer tick, and timer-wheel
wakeups.

This is an experimental teaching OS, not a production system.

## Build And Run

The build currently expects a Linux/WSL environment with:

- `clang`
- `lld-link`
- `xxd`
- `dosfstools`
- `mtools`
- `qemu-system-aarch64`
- `qemu-efi-aarch64`

From WSL, in the repository root:

```sh
make
make image
make run
```

Useful targets:

| Target | Purpose |
| --- | --- |
| `make` | Build kernel EFI image, user ELFs, and initrd |
| `make image` | Create `build/fat.img` with `BOOTAA64.EFI` and `initrd.bin` |
| `make run` | Boot QEMU in `-nographic` mode |
| `make run_g` | Boot QEMU with graphical display |
| `make clean` | Remove `build/` and stale generated user ELF headers |

The default QEMU memory size is 2048 MiB. Override it with:

```sh
make run QEMU_MEM=3072M
```

The kernel log level is selected at build time:

| Value | Meaning |
| --- | --- |
| `LOG_LEVEL=0` | Quiet: failures only |
| `LOG_LEVEL=1` | Normal boot milestones and warnings |
| `LOG_LEVEL=2` | Debug logs |
| `LOG_LEVEL=3` | Trace logs |

Example:

```sh
make image LOG_LEVEL=2
```

Some QEMU CPU/backend combinations only expose a 32-bit physical address space.
On `virt`, RAM starts at `0x40000000`, so those combinations can boot up to
about `3072M`. Use a CPU/backend with wider physical addresses before trying
`QEMU_MEM=4096M`.

## Shell Commands

After boot, Neptune starts `uart_server`, `keyboard_server`, `name_server`,
`fs_server`, and `shell`.
The shell prompt is:

```text
Neptune>
```

Available commands:

| Command | Purpose |
| --- | --- |
| `help` | Show shell commands |
| `clear` | Clear the terminal |
| `echo <text>` | Print text |
| `ls` | List initrd files through the user-space FS server |
| `uptime` | Show seconds since boot |
| `mem` | Show RAM and kernel heap usage |
| `vmmap` | Show the shell task's virtual memory areas |
| `capstat` | Show the shell task's Orange Cat capability slots |
| `debug` | Show kernel build/runtime debug information |
| `smoke` | Run bounded core regression checks |
| `lazy` | Check lazy mmap and syscall usercopy into lazy memory |
| `ipcfast` | Test 0/8/64/128-byte register IPC payloads |
| `ipccap` | Transfer an endpoint capability through IPC and use it |
| `ipckill` | Verify IPC queues/reply caps unwind across task kills |
| `memshare` | Share one page with a child task through IPC memory attach |
| `memxfer` | Transfer one page to a child task through IPC memory attach |
| `memcaplife` | Share an exported memory cap after unmapping the old VA |
| `memrevoke` | Lend a page, revoke it, and verify borrower isolation |
| `munmap` | Check user unmap and stale export rejection |
| `forkcow` | Fork the shell and verify copy-on-write isolation |
| `filelazy` | Lazily map an initrd file and read it |
| `vmstress` | Grow and shrink the VMA table under many file mappings |
| `lazyexec` | Spawn an initrd ELF through lazy executable VM objects |
| `taskstress` | Grow the kernel task table with many spin tasks |
| `speed` | Run scheduler, syscall, lazy-fault, memory, and IPC speed checks |
| `ps` | Show scheduler state, including PPID |
| `pong` | Spawn pong, run the IPC demo, wait for exit |
| `fault` | Spawn a task that page-faults, wait for fault status |
| `spawn pong` | Run pong and leave its exit record for `wait` |
| `spawn fault` | Spawn a faulting child and leave its status for `wait` |
| `spawn spin` | Spawn a child that runs until killed |
| `badptr` | Verify invalid syscall pointers are rejected |
| `run <file>` | Ask FS for an executable capability and spawn it |
| `jobs` | Show shell-tracked child jobs with nonblocking status |
| `poll <tid>` | Check child status without reaping it |
| `wait <tid>` | Wait for/reap a child status |
| `kill <tid>` | Kill a user task |

## Process Lifecycle Demo

Run a long-lived child:

```text
spawn spin
ps
jobs
poll 4
kill 4
poll 4
wait 4
jobs
```

Expected behavior:

- `ps` shows the child with shell as PPID.
- `poll 4` first reports the child is still running.
- `kill 4` terminates the child.
- `poll 4` reports the child was killed without reaping it.
- `wait 4` reports the same status and removes the child record.
- `jobs` no longer lists the reaped child.

Run a user fault isolation demo:

```text
spawn fault
jobs
poll 4
wait 4
```

Expected behavior:

- The faulting task writes into the user stack guard page.
- The kernel logs ESR/ELR/FAR.
- The shell survives.
- `poll` and `wait` report the fault status.

Run an ELF through the FS/executable-capability path:

```text
run spin.elf
jobs
kill 4
wait 4
```

## Implemented Features

### Boot And Platform

- Boots as an AArch64 UEFI application under QEMU.
- Loads `initrd.bin` from the FAT image before `ExitBootServices`.
- Initializes exceptions, the MMU, ACPI hardware discovery, GICv2, and the ARM
  generic timer at a 1 kHz scheduler tick.
- Uses a 39-bit lower-half identity map with 40-bit physical-address support.
- Early MMU page-table setup uses scalar volatile stores and explicit barriers,
  so boot correctness does not depend on logging side effects or compiler loop
  vectorization.
- Supports per-user address spaces with allocated ASIDs in `TTBR0_EL1`,
  targeted TLB invalidation, and flush-on-ASID-reuse.

### Memory Management

- Physical page allocator plus basic kernel heap diagnostics.
- Frame objects with refcounts layered above raw PMM pages.
- First-class VM objects for anonymous and initrd-backed mappings.
- Per-task VMA tables for ELF segments, stack, guard, mmap, imported memory,
  and file-backed regions.
- Dynamically growing VMA tables with binary-search lookup and sorted insertion.
- VMA remove/split/coalesce with basic committed-page accounting.
- Lazy anonymous `mmap`; physical pages are committed on first valid access.
- Lazy file-backed initrd mappings through file capabilities and `file_mmap`,
  backed by a shared initrd page cache.
- Lazy executable loading for initrd-backed `spawn_file` and `spawn_exec`.
- Public `munmap` for user mmap/imported mappings.
- `fork` with copy-on-write for writable frame-backed user pages.
- Dynamic user stack growth up to 64 KiB with a moving guard page.
- `vmmap` syscall and shell command for inspecting the current task's VMAs.

### Fault And User Safety

- User stack guard page.
- Fault resolver for lazy VMAs, file-backed VMAs, stack growth, and invalid
  access detection.
- User faults kill only the faulting task.
- Syscall user-pointer validation, including lazy page resolution during
  usercopy.
- Invalid pointer regression coverage through `badptr`.

### Capabilities: Orange Cat

- Unified per-task capability tables with typed slots, object IDs, rights, and
  flags.
- `capstat` syscall and shell command for inspecting current task cap slots.
- Cap types include endpoint, reply, exec, file, VMA, frame, and future objects.
- Reserved `CAP_SELF` and `CAP_NS` endpoint caps are installed for each task.
- Memory, file, exec, endpoint, and reply authority checks use Orange Cat rights
  instead of scheduler-local cap checks.
- Bootstrapped FS authority: the kernel grants the FS service file and exec
  caps for initrd entries at startup.
- User tasks obtain file and executable authority only through FS IPC.

### IPC

- Synchronous capability IPC with `ipc_call`, `ipc_recv`, and one-shot
  `ipc_reply` caps.
- Register-only fast path for messages up to 128 bytes in `x3` through `x18`.
- IPC syscalls use SVC immediates so `x8` can carry payload.
- Slow path supports one zero-copy memory attachment using share, transfer, or
  lend mapping machinery.
- Memory capabilities support zero-copy share, transfer, lend, and revoke.
- Exported memory caps are backed by pinned frame objects, so the cap no
  longer depends on the original exporter VA staying mapped.
- Memory-cap object and mapping management lives in `src/kernel/memcap.c`,
  including export, import, share, transfer, lend, revoke, and munmap cleanup.
- Handoff scheduling switches directly from caller to waiting receiver and from
  replier to blocked caller.
- Kernel IPC code lives in `src/kernel/ipc.c`; `sched.c` now only exposes the
  scheduler boundary needed for task lookup, trap frames, and handoff.

### User Services And Files

- User-space name server for service registration and discovery.
- User-space FS server for initrd listing, file-capability open, and
  executable-capability open over IPC.
- UART and keyboard user services.
- ELF loading for user applications stored in initrd.

### Tasks And Scheduling

- EL0 user task creation and context switching.
- CPU0 per-core scheduler structure prepared for later SMP expansion.
- O(1) MLFQ scheduler with 32 circular ready queues.
- Ready selection uses a 32-bit bitmap where bit 31 is queue 0 and native
  AArch64 `CLZ` finds the highest runnable priority.
- Exponential time slices: 1 ms for queues 0-3, 2 ms for 4-7, 4 ms for 8-15,
  8 ms for 16-23, and 16 ms for 24-31.
- 1 kHz ARM generic timer tick; `sleep(ms)` now uses millisecond ticks.
- 1024-bucket timer wheel for sleep and IRQ timeout wakeups.
- Global priority boost every 1000 ticks to prevent starvation.
- Yield keeps the current priority and moves the task to the tail of its
  queue; CPU-bound quantum expiry demotes by one queue.
- Blocking on IPC, sleep, wait, IRQ, or fault removes the task from scheduler
  ownership without demoting it.
- Dynamically growing task and termination-record tables.
- Parent-child task tracking with child termination records.
- `wait` and `poll` support for child status.
- Kill/exit cleanup unwinds IPC waiters, reply caps, memory objects, and shared
  mappings.

### Shell And Syscalls

- Interactive shell with dynamically growing job table.
- Shell-side job tracking for spawned children.
- `debug` reports build log level, scheduler tick rate, task capacity, ASID
  limit, stack growth limit, current VMA table use, cap slots, and memory-cap
  table use.
- `smoke` runs bounded core checks for lazy memory, register IPC, invalid
  pointers, stack growth, and the speed benchmark.
- Implemented syscalls include sleep, spawn, spawn-file compatibility,
  spawn-exec, ipc-call, ipc-recv, ipc-reply, exit, kill, wait, poll, ps, mem,
  mmap, munmap, mem-export, mem-share, mem-transfer, mem-lend, mem-revoke,
  uptime, fork, file-stat, file-mmap, capstat, and debug-info.

## User Programs

The current embedded user programs are:

| Program | Source | Purpose |
| --- | --- | --- |
| `shell` | `user/shell.c` | Interactive command shell |
| `uart_server` | `user/uart.c` | Serial output service |
| `keyboard_server` | `user/keyboard.c` | Keyboard input service |
| `name_server` | `user/ns.c` | Service registration and capability minting |
| `fs_server` | `user/fs.c` | Initrd listing service over IPC |
| `pong` | `user/pong.c` | IPC demo child |
| `fault` | `user/fault.c` | Deliberate guard-page fault test |
| `spin` | `user/spin.c` | Long-lived child for ps/kill/wait/jobs |
| `badptr` | `user/badptr.c` | Invalid syscall pointer regression test |
| `stackgrow` | `user/stackgrow.c` | Dynamic stack growth regression test |
| `memshare` | `user/memshare.c` | Shared-page receiver demo |
| `memxfer` | `user/memxfer.c` | Transferred-page receiver demo |
| `memrevoke` | `user/memrevoke.c` | Lend/revoke borrower isolation demo |
| `ipcfast` | `user/ipcfast.c` | Register-only IPC boundary test |
| `ipccap` | `user/ipccap.c` | Endpoint-cap transfer regression test |
| `ipckill` | `user/ipckill.c` | IPC kill/unwind regression helper |
| `speed` | `user/speed.c` | OS speed benchmark for CPU loop, syscalls, yield, lazy faults, memory writes, and IPC |
| `speedipc` | `user/speedipc.c` | Helper endpoint used by the speed IPC benchmark |

The build packs these ELFs into `build/initrd.bin`. UEFI loads that file from
the FAT image before `ExitBootServices`. The FS server validates filenames and
transfers boot-granted file or executable capabilities to clients.

## Known Limitations

- The FS server currently exposes initrd only; there is no general VFS yet.
- Scheduler is CPU0-only; the data shape is per-core, but SMP dispatch is not
  implemented yet.
- User programs are still packaged at build time into `initrd.bin`.
- No persistent filesystem or file writes yet.
- The bootstrap identity map covers the 39-bit lower VA space. This supports
  large QEMU RAM sizes in principle, but current smoke testing covers 4 GiB.
- Raw TID IPC and old cap-send/cap-recv syscall behavior are disabled; TIDs
  remain for debug and lifecycle calls such as `ps`, `kill`, `wait`, and
  `poll`.
- `wait`/`poll` only apply to children of the calling task.
- Kernel heap is still simple and mostly reusable only through task slot reuse.
- Bootstrap services are still eagerly loaded; initrd-backed user spawns are
  lazy-loaded through VM objects.
- The initrd page cache is fixed-size and has no eviction policy yet.
- VM objects use a fixed-size v1 table.
- Memory caps currently export page-aligned, committed, non-guard, non-stack
  user VMAs into pinned frame-list objects.
- Memory-cap object and mapping tables are fixed-size v1 tables.
- Copy-on-write currently covers forked, writable, frame-backed user pages.
- Terminal input is intentionally simple and ASCII-oriented.
- `smoke` is intentionally bounded and does not include every destructive or
  long-running regression; run `ipccap`, `ipckill`, `memshare`, `memxfer`, and
  `memrevoke` directly for those specific paths.

## Good Next Steps

- Generalize memory-cap backing from pinned frame-list objects into reusable VM
  object/page-cache backing.
- Add a VFS layer over initrd entries.
- Add real file-backed user program loading from FAT or another filesystem.
- Add a nonblocking keyboard/event API for richer shell interaction.
- Add job names and child creation metadata to kernel `ps`.
- Add host-side QEMU smoke scripts that drive `debug`, `smoke`, and the
  heavier dedicated IPC/memory-cap commands with timeouts.
