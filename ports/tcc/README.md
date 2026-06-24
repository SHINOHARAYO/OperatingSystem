# TinyCC Port

This directory contains Neptune-specific port notes. Upstream TinyCC is
vendored unchanged in `third_party/tcc` at the revision recorded in
[`UPSTREAM`](UPSTREAM).

## Current Status

The imported compiler has a native AArch64 backend (`arm64-gen.c`,
`arm64-link.c`, and `arm64-asm.c`). An isolated WSL host build of the pinned
revision succeeds and reports an AArch64 Linux compiler.

Neptune now builds `build/tcc.elf` and packages it as `/bin/tcc.elf`. The
shell command `tcc [options...]` forwards compiler arguments and waits for
the compiler to finish. The compiler runtime includes:

- `main(argc, argv)` through `crt0`
- `open`, `read`, `write`, `close`, and `lseek`
- `malloc`, `calloc`, `realloc`, and core string/memory helpers
- `fcntl.h`, `unistd.h`, stream I/O, conversion, time, and `setjmp`
  compatibility layers
- embedded TinyCC builtin definitions (`tccdefs_.h`), generated from the
  pinned upstream `include/tccdefs.h`

Windows QEMU smoke tests have verified `tcc -v`, `tcc -c t.c`, and the full
`tcc hello.c -o hello.elf` followed by `run hello.elf` path. The latter
creates a statically linked AArch64 ELF executable through Neptune's writable
VFS path.

The app disk now installs a compact target SDK under `/tcc/include`. It
contains Neptune's current `stdio`, `stdlib`, `string`, file, time, and error
headers, plus `stdint.h`, `stddef.h`, `stdbool.h`, `ctype.h`, and TinyCC's
`stdarg.h`. The guest test `tcc -c sdk.c` has compiled a program including
those headers into an AArch64 ELF relocatable object.

## Port Boundary

The current target is an AArch64 ELF command-line compiler that produces
relocatable objects and static Neptune executables. A Neptune wrapper injects
`-static` for executable links because the kernel intentionally has no
dynamic ELF loader. `-c`, `-E`, `-r`, and `-shared` retain their normal TCC
behavior.

In-process `-run`, dynamic loading, PE/Mach-O output, bounds-checking, full
Linux ABI startup, and compiler builtin coverage are not enabled yet.

Keep Neptune adaptations in `ports/tcc` or dedicated wrapper sources. Do not
edit vendored upstream files unless a local patch is documented here.
