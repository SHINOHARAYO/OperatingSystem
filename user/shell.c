#include "lib.h"
#include "malloc.h"
#include "fs_proto.h"
#include "ns_proto.h"
#include "display_proto.h"
#include "compositor_proto.h"
#include "keyboard_proto.h"
#include "mouse_proto.h"

#define MAX_CMD 256
#define SHELL_HISTORY 32
#define SHELL_PROMPT "\x1b[92mNeptune>\x1b[0m "
#define INITIAL_JOBS 16
#define PONG_PRIORITY 50

#define PONG_FILE "pong.elf"
#define FAULT_FILE "fault.elf"
#define SPIN_FILE "spin.elf"
#define BADPTR_FILE "badptr.elf"
#define STACKGROW_FILE "stackgrow.elf"
#define MEMSHARE_FILE "memshare.elf"
#define MEMXFER_FILE "memxfer.elf"
#define MEMREVOKE_FILE "memrevoke.elf"
#define IPCFAST_FILE "ipcfast.elf"
#define IPCCAP_FILE "ipccap.elf"
#define IPCKILL_FILE "ipckill.elf"
#define SPEED_FILE "speed.elf"
#define SNAKE_FILE "snake.elf"
#define GFXDEMO_FILE "gfxdemo.elf"
#define GFXBENCH_FILE "gfxbench.elf"
#define LIBCTEST_FILE "libctest.elf"
#define ARGFDTEST_FILE "argfdtest.elf"
#define PIPETEST_FILE "pipetest.elf"
#define TCC_FILE "tcc.elf"
#define PAGE_BYTES 4096ULL

#define IPCKILL_SERVER_HOLD        1
#define IPCKILL_SERVER_DELAY_REPLY 2
#define IPCKILL_CALLER_CALL        3
#define IPCKILL_RECV_WAIT          4
#define IPCCAP_MODE_SERVER         1
#define IPCCAP_MODE_CLIENT         2

#define SHELL_TASK_STATE_SLEEPING 6
#define IPCKILL_PRIORITY 5

typedef struct {
  uint32_t tid;
  char name[32];
  int active;
} shell_job_t;

static shell_job_t *jobs;
static uint32_t job_capacity;
static int keyboard_cap = -1;
static int last_spawn_endpoint_cap = -1;
static int graphical_terminal;
static char pending_key_chars[3];
static uint32_t pending_key_count;
static uint32_t pending_key_index;

static void print_wait_result(wait_info_t info);
static uint32_t spawn_program(const char *name, uint8_t priority);
static uint32_t spawn_program_args(const char *name, uint8_t priority,
                                   char *const argv[], uint32_t argc);
static wait_info_t poll_until_done(uint32_t tid, uint32_t attempts, uint64_t sleep_ms);
static void run_vfstest_demo(void);
static void run_vfsinject_demo(void);
static void run_vfswrite_demo(void);
static void run_vfsinstall_demo(void);
static void list_files_vfs(const char *path);
static void run_recvhex_command(const char *name, uint32_t size);

static int ensure_job_capacity(uint32_t min_capacity) {
  if (job_capacity >= min_capacity) {
    return 0;
  }

  uint32_t new_capacity = job_capacity ? job_capacity * 2 : INITIAL_JOBS;
  while (new_capacity < min_capacity) {
    new_capacity *= 2;
  }

  shell_job_t *new_jobs = (shell_job_t *)malloc(sizeof(shell_job_t) * new_capacity);
  if (!new_jobs) {
    return -1;
  }

  for (uint32_t i = 0; i < new_capacity; i++) {
    if (i < job_capacity && jobs) {
      new_jobs[i] = jobs[i];
    } else {
      new_jobs[i].tid = 0;
      new_jobs[i].name[0] = '\0';
      new_jobs[i].active = 0;
    }
  }

  jobs = new_jobs;
  job_capacity = new_capacity;
  return 0;
}

static int _streq(const char *a, const char *b) {
  while (*a && *b && (*a == *b)) {
    a++;
    b++;
  }
  return *a == '\0' && *b == '\0';
}

static uint32_t _parse_u32(const char *s, int *ok) {
  uint32_t value = 0;
  int seen = 0;

  while (*s == ' ') {
    s++;
  }

  while (*s >= '0' && *s <= '9') {
    value = (value * 10) + (uint32_t)(*s - '0');
    seen = 1;
    s++;
  }

  while (*s == ' ') {
    s++;
  }

  *ok = seen && *s == '\0';
  return value;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static int is_space_char(char c) {
  return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static const char *task_state_name(uint32_t state) {
  switch (state) {
    case 1: return "ready";
    case 2: return "running";
    case 3: return "call";
    case 4: return "recv";
    case 5: return "reply";
    case 6: return "sleep";
    case 7: return "irq";
    case 8: return "wait";
    case 9: return "vfscall";
    case 10: return "vfsrecv";
    default: return "?";
  }
}

static void print_kib_line(const char *label, uint64_t bytes) {
  printf("%s: %lu KiB (%lu bytes)\n", label, bytes / 1024, bytes);
}

static void print_vma_flags(uint64_t flags) {
  printf("%c%c%c",
         (flags & VMA_READ) ? 'r' : '-',
         (flags & VMA_WRITE) ? 'w' : '-',
         (flags & VMA_EXEC) ? 'x' : '-');

  if (flags & VMA_GUARD) printf(" guard");
  if (flags & VMA_STACK) printf(" stack");
  if (flags & VMA_MMAP) printf(" mmap");
  if (flags & VMA_ELF) printf(" elf");
  if (flags & VMA_LAZY) printf(" lazy");
  if (flags & VMA_GROWS_DOWN) printf(" grow");
}

static const char *vma_backing_name(uint32_t backing) {
  switch (backing) {
    case VMA_BACKING_ANON: return "anon";
    case VMA_BACKING_FILE: return "file";
    case VMA_BACKING_DEVICE: return "dev";
    default: return "none";
  }
}

static const char *cap_type_name(uint32_t type) {
  switch (type) {
    case OCAP_ENDPOINT: return "endpoint";
    case OCAP_EXEC: return "exec";
    case OCAP_FRAME: return "frame";
    case OCAP_VMA: return "vma";
    case OCAP_REPLY: return "reply";
    case OCAP_FILE: return "file";
    case OCAP_DMA: return "dma";
    default: return "none";
  }
}

static void print_cap_rights(uint64_t rights) {
  int any = 0;
  if (rights & OCAP_RIGHT_CALL) { printf("call"); any = 1; }
  if (rights & OCAP_RIGHT_REPLY) { printf("%sreply", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_SPAWN) { printf("%sspawn", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_READ) { printf("%sread", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_WRITE) { printf("%swrite", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_MAP) { printf("%smap", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_SHARE) { printf("%sshare", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_TRANSFER) { printf("%stransfer", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_LEND) { printf("%slend", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_REVOKE) { printf("%srevoke", any ? "," : ""); any = 1; }
  if (rights & OCAP_RIGHT_DMA) { printf("%sdma", any ? "," : ""); any = 1; }
  if (!any) {
    printf("-");
  }
}

static void print_capstat(void) {
  cap_info_t first = sys_capstat(0);
  uint32_t capacity = first.capacity;
  if (capacity == 0) {
    printf("(no capability table)\n");
    return;
  }

  printf("SLOT TYPE      OBJECT RIGHTS\n");
  for (uint32_t slot = 1; slot < capacity; slot++) {
    cap_info_t cap = sys_capstat(slot);
    if (!cap.present) {
      continue;
    }

    printf("%u   %s", cap.slot, cap_type_name(cap.type));
    uint32_t name_len = 0;
    const char *name = cap_type_name(cap.type);
    while (name[name_len]) name_len++;
    while (name_len++ < 9) printf(" ");
    printf("%u      ", cap.object_id);
    print_cap_rights(cap.rights);
    if (cap.flags) {
      printf(" flags=0x%lx", cap.flags);
    }
    printf("\n");
  }
}

static const char *log_level_name(uint32_t level) {
  switch (level) {
    case 0: return "quiet";
    case 1: return "info";
    case 2: return "debug";
    case 3: return "trace";
    default: return "custom";
  }
}

static void print_debug_info(void) {
  debug_info_t info = sys_debug_info();
  cap_info_t first_cap = sys_capstat(0);
  memory_info_t mem = sys_mem();

  printf("KERNEL\n");
  printf("  log-level: %u (%s)\n", info.log_level, log_level_name(info.log_level));
  printf("  scheduler: per-core O(1) MLFQ, %u Hz tick, %u core%s online\n",
         info.tick_hz, info.online_cores, info.online_cores == 1 ? "" : "s");
  printf("  task-capacity: %u slots\n", info.task_capacity);
  printf("  max-user-asids: %u\n", info.max_user_asids);

  printf("MEMORY\n");
  printf("  free-ram: %lu KiB\n", mem.free_memory / 1024);
  printf("  heap-used: %lu KiB / %lu KiB\n", mem.heap_used / 1024, mem.heap_size / 1024);
  printf("  user-stack-max: %lu KiB\n", info.user_stack_max_size / 1024);
  printf("  current-vmas: %u / %u\n", info.current_vma_count, info.current_vma_capacity);

  printf("CAPABILITIES\n");
  printf("  current-cap-slots: %u\n", first_cap.capacity);
  printf("  memory-objects: %u / %u\n", info.active_mem_objects, info.max_mem_objects);
  printf("  memory-mappings: %u / %u\n", info.active_mem_mappings, info.max_mem_mappings);
}

static void print_vmmap(void) {
  printf("START              END                FLAGS           PAGES BACKING\n");
  for (uint32_t i = 0; i < 128; i++) {
    vma_info_t vma = sys_vmmap(i);
    if (!vma.present) {
      if (i == 0) {
        printf("(no VMAs)\n");
      }
      return;
    }

    printf("0x%lx  0x%lx  ", vma.start, vma.end);
    print_vma_flags(vma.flags);
    printf("  %lu/%lu %s\n", vma.committed_pages, vma.max_pages,
           vma_backing_name(vma.backing_type));
  }
}

static void write_literal(char *dst, const char *src) {
  while (*src) {
    *dst++ = *src++;
  }
  *dst = '\0';
}

static void cleanup_blocked_child(uint32_t tid) {
  if ((int)tid >= 0) {
    sys_kill(tid);
    print_wait_result(sys_wait(tid));
  }
}

static void run_memshare_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("memshare: mmap failed\n");
    return;
  }

  write_literal(buf, "share:from-shell");
  int cap = sys_mem_export(buf, PAGE_BYTES,
                           MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE);
  if (cap < 0) {
    printf("memshare: export failed\n");
    return;
  }

  uint32_t tid = spawn_program(MEMSHARE_FILE, PONG_PRIORITY);
  int child_cap = last_spawn_endpoint_cap;
  if ((int)tid < 0) {
    printf("memshare: receiver spawn failed\n");
    return;
  }

  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_MEM_WORD_CAP] = (uint64_t)cap,
    [IPC_MEM_WORD_OFFSET] = 0,
    [IPC_MEM_WORD_SIZE] = PAGE_BYTES,
    [IPC_MEM_WORD_DESCRIPTOR] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)child_cap, IPC_FLAG_MEM, 32, payload);
  if (reply.status < 0) {
    printf("memshare: ipc share failed\n");
    cleanup_blocked_child(tid);
    return;
  }

  void *remote = (void *)reply.payload[1];
  wait_info_t status = sys_wait(tid);
  int ok = reply.payload[0] == 1 && buf[0] == 'S' && buf[1] == 'H' && buf[32] == '!';
  printf("memshare: %s local=%c%c remote=0x%lx\n",
         ok ? "ok" : "failed", buf[0], buf[1], (uint64_t)remote);
  print_wait_result(status);
}

static void run_memxfer_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("memxfer: mmap failed\n");
    return;
  }

  write_literal(buf, "xfer:from-shell");
  int cap = sys_mem_export(buf, PAGE_BYTES,
                           MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_TRANSFER);
  if (cap < 0) {
    printf("memxfer: export failed\n");
    return;
  }

  uint32_t tid = spawn_program(MEMXFER_FILE, PONG_PRIORITY);
  int child_cap = last_spawn_endpoint_cap;
  if ((int)tid < 0) {
    printf("memxfer: receiver spawn failed\n");
    return;
  }

  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_MEM_WORD_CAP] = (uint64_t)cap,
    [IPC_MEM_WORD_OFFSET] = 0,
    [IPC_MEM_WORD_SIZE] = PAGE_BYTES,
    [IPC_MEM_WORD_DESCRIPTOR] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_TRANSFER
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)child_cap, IPC_FLAG_MEM, 32, payload);
  if (reply.status < 0) {
    printf("memxfer: ipc transfer failed\n");
    cleanup_blocked_child(tid);
    return;
  }

  void *remote = (void *)reply.payload[1];
  int old_export = sys_mem_export(buf, PAGE_BYTES, MEM_RIGHT_READ);
  wait_info_t status = sys_wait(tid);
  int ok = reply.payload[0] == 1 && old_export < 0;
  printf("memxfer: %s remote=0x%lx old-va-export=%d\n",
         ok ? "ok" : "failed", (uint64_t)remote, old_export);
  print_wait_result(status);
}

static void run_memcaplife_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("memcaplife: mmap failed\n");
    return;
  }

  write_literal(buf, "share:from-shell");
  int cap = sys_mem_export(buf, PAGE_BYTES,
                           MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_SHARE);
  if (cap < 0) {
    printf("memcaplife: export failed\n");
    return;
  }

  int unmapped = sys_munmap(buf, PAGE_BYTES);
  if (unmapped < 0) {
    printf("memcaplife: munmap failed\n");
    return;
  }

  uint32_t tid = spawn_program(MEMSHARE_FILE, PONG_PRIORITY);
  int child_cap = last_spawn_endpoint_cap;
  if ((int)tid < 0 || child_cap < 0) {
    printf("memcaplife: receiver spawn failed\n");
    return;
  }

  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_MEM_WORD_CAP] = (uint64_t)cap,
    [IPC_MEM_WORD_OFFSET] = 0,
    [IPC_MEM_WORD_SIZE] = PAGE_BYTES,
    [IPC_MEM_WORD_DESCRIPTOR] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_SHARE
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)child_cap, IPC_FLAG_MEM, 32, payload);
  wait_info_t status = sys_wait(tid);
  int ok = reply.status == 0 && reply.payload[0] == 1 &&
           status.reason == TASK_TERM_EXITED && status.exit_code == 0;
  printf("memcaplife: %s unmap=%d remote=0x%lx marker=%lu\n",
         ok ? "ok" : "failed", unmapped, reply.payload[1], reply.payload[2]);
  print_wait_result(status);
}

static void run_munmap_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("munmap: mmap failed\n");
    return;
  }

  write_literal(buf, "munmap:live");
  int before = sys_mem_export(buf, PAGE_BYTES, MEM_RIGHT_READ);
  int unmapped = sys_munmap(buf, PAGE_BYTES);
  int after = sys_mem_export(buf, PAGE_BYTES, MEM_RIGHT_READ);

  printf("munmap: %s before=%d unmap=%d after=%d\n",
         (before >= 0 && unmapped == 0 && after < 0) ? "ok" : "failed",
         before, unmapped, after);
}

static void run_memrevoke_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("memrevoke: mmap failed\n");
    return;
  }

  write_literal(buf, "share:from-shell");
  int cap = sys_mem_export(buf, PAGE_BYTES,
                           MEM_RIGHT_READ | MEM_RIGHT_WRITE | MEM_RIGHT_LEND);
  if (cap < 0) {
    printf("memrevoke: export failed\n");
    return;
  }

  uint32_t tid = spawn_program(MEMREVOKE_FILE, PONG_PRIORITY);
  int child_cap = last_spawn_endpoint_cap;
  if ((int)tid < 0) {
    printf("memrevoke: receiver spawn failed\n");
    return;
  }

  uint64_t lend_payload[IPC_INLINE_WORDS] = {
    [IPC_MEM_WORD_CAP] = (uint64_t)cap,
    [IPC_MEM_WORD_OFFSET] = 0,
    [IPC_MEM_WORD_SIZE] = PAGE_BYTES,
    [IPC_MEM_WORD_DESCRIPTOR] = MEM_RIGHT_READ | MEM_RIGHT_WRITE | IPC_MEM_MODE_LEND
  };
  ipc_msg_t lend = sys_ipc_call((uint32_t)child_cap, IPC_FLAG_MEM, 32, lend_payload);
  if (lend.status < 0 || lend.payload[0] != 1) {
    printf("memrevoke: ipc lend failed\n");
    cleanup_blocked_child(tid);
    return;
  }

  void *remote = (void *)lend.payload[1];
  int revoked = sys_mem_revoke((uint32_t)cap);
  uint64_t payload[IPC_INLINE_WORDS] = {(uint64_t)remote};
  ipc_msg_t reply = sys_ipc_call((uint32_t)child_cap, 0, 8, payload);
  (void)reply;
  if (child_cap < 0) {
    printf("memrevoke: receiver send failed\n");
    cleanup_blocked_child(tid);
    return;
  }

  wait_info_t status = sys_wait(tid);
  int ok = revoked == 0 && status.reason == TASK_TERM_FAULTED;
  printf("memrevoke: %s remote=0x%lx revoke=%d\n",
         ok ? "ok" : "failed", (uint64_t)remote, revoked);
  print_wait_result(status);
}

static void run_forkcow_demo(void) {
  char *buf = (char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("forkcow: mmap failed\n");
    return;
  }

  write_literal(buf, "parent");
  int tid = sys_fork();
  if (tid < 0) {
    printf("forkcow: fork failed\n");
    return;
  }

  if (tid == 0) {
    buf[0] = 'c';
    buf[1] = 'h';
    buf[2] = 'i';
    buf[3] = 'l';
    buf[4] = 'd';
    buf[5] = '\0';
    sys_exit(buf[0] == 'c' ? 0 : 1);
  }

  wait_info_t status = sys_wait((uint32_t)tid);
  int ok = status.reason == TASK_TERM_EXITED && status.exit_code == 0 &&
           buf[0] == 'p' && buf[1] == 'a';
  printf("forkcow: %s child=%d parent_buf=%s\n",
         ok ? "ok" : "failed", tid, buf);
  print_wait_result(status);
}

static void run_filelazy_demo(void) {
  vfs_file_info_t info = vfs_open_file("shell.elf");
  unsigned char *file = info.status < 0 ? 0 : (unsigned char *)sys_mmap(PAGE_BYTES);
  if (!file) {
    printf("filelazy: vfs mmap failed\n");
    return;
  }

  int64_t bytes = vfs_read_handle_page(info.handle, 0, file);
  if (bytes < 0) {
    printf("filelazy: vfs read failed\n");
    return;
  }

  int ok = file[0] == 0x7f && file[1] == 'E' && file[2] == 'L' && file[3] == 'F';
  printf("filelazy: %s vfs-bytes=%ld size=%lu magic=%x %c%c%c\n",
         ok ? "ok" : "failed", bytes, info.size,
         (uint32_t)file[0], file[1], file[2], file[3]);
  vfs_close_handle(info.handle);
  sys_munmap(file, PAGE_BYTES);
}

static void run_vmstress_demo(void) {
  vfs_file_info_t info = vfs_open_file("shell.elf");
  if (info.status < 0 || info.size == 0) {
    printf("vmstress: vfs open failed\n");
    return;
  }

  void *maps[80];
  uint32_t mapped = 0;
  uint32_t touched = 0;

  for (uint32_t i = 0; i < 80; i++) {
    unsigned char *file = (unsigned char *)sys_mmap(PAGE_BYTES);
    if (!file) {
      break;
    }
    if (vfs_read_handle_page(info.handle, 0, file) < 0) {
      break;
    }
    maps[mapped++] = file;
    if (file[0] == 0x7f) {
      touched++;
    }
  }

  for (uint32_t i = 0; i < mapped; i++) {
    sys_munmap(maps[i], PAGE_BYTES);
  }

  printf("vmstress: %s mapped=%u touched=%u\n",
         mapped == 80 && touched == 80 ? "ok" : "failed", mapped, touched);
  vfs_close_handle(info.handle);
}

static void run_lazyexec_demo(void) {
  uint32_t tid = spawn_program(BADPTR_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("lazyexec: spawn failed\n");
    return;
  }

  wait_info_t status = sys_wait(tid);
  int ok = status.reason == TASK_TERM_EXITED && status.exit_code == 0;
  printf("lazyexec: %s tid=%u\n", ok ? "ok" : "failed", tid);
  print_wait_result(status);
}

static void run_taskstress_demo(void) {
  enum { TASKSTRESS_ROUNDS = 4, TASKSTRESS_BATCH = 8 };
  uint32_t tids[TASKSTRESS_BATCH];
  uint32_t total_spawned = 0;
  uint32_t total_killed = 0;
  uint32_t timeouts = 0;
  uint32_t start_capacity = sys_task_capacity();

  for (uint32_t round = 0; round < TASKSTRESS_ROUNDS; round++) {
    uint32_t spawned = 0;
    for (uint32_t i = 0; i < TASKSTRESS_BATCH; i++) {
      uint32_t tid = spawn_program(SPIN_FILE, PONG_PRIORITY);
      if ((int)tid < 0) {
        break;
      }
      tids[spawned++] = tid;
      total_spawned++;
      sys_yield();
    }

    for (uint32_t i = 0; i < spawned; i++) {
      if (sys_kill(tids[i]) == 0) {
        wait_info_t info = poll_until_done(tids[i], 200, 5);
        if (info.status == WAIT_STATUS_DONE && info.reason == TASK_TERM_KILLED) {
          info = sys_wait(tids[i]);
          if (info.reason == TASK_TERM_KILLED) {
            total_killed++;
          }
        } else {
          timeouts++;
        }
      } else {
        wait_info_t info = poll_until_done(tids[i], 50, 5);
        if (info.status == WAIT_STATUS_DONE && info.reason == TASK_TERM_KILLED) {
          sys_wait(tids[i]);
          total_killed++;
        }
      }
      sys_yield();
    }
  }

  uint32_t end_capacity = sys_task_capacity();
  int grew = end_capacity > start_capacity;
  printf("taskstress: %s spawned=%u killed=%u timeouts=%u batches=%u capacity=%u->%u %s\n",
         total_spawned == TASKSTRESS_ROUNDS * TASKSTRESS_BATCH &&
             total_killed == total_spawned && timeouts == 0 ? "ok" : "failed",
         total_spawned, total_killed, timeouts, TASKSTRESS_ROUNDS,
         start_capacity, end_capacity,
         grew ? "grown" : "reused");
}

static void print_wait_result(wait_info_t info) {
  if (info.status < 0) {
    int err = -info.status;
    if (err == WAIT_ERR_NO_CHILD) {
      printf("wait failed: no such child\n");
    } else if (err == WAIT_ERR_SELF) {
      printf("wait failed: cannot wait on self\n");
    } else {
      printf("wait failed: invalid request\n");
    }
    return;
  }

  if (info.reason == TASK_TERM_EXITED) {
    printf("TID %u exited with code %d\n", info.tid, info.exit_code);
  } else if (info.reason == TASK_TERM_KILLED) {
    printf("TID %u was killed\n", info.tid);
  } else if (info.reason == TASK_TERM_FAULTED) {
    printf("TID %u faulted: ESR=0x%lx ELR=0x%lx FAR=0x%lx\n",
           info.tid, info.esr, info.elr, info.far);
  } else {
    printf("TID %u ended for unknown reason %u\n", info.tid, info.reason);
  }
}

static void print_poll_result(wait_info_t info) {
  if (info.status == WAIT_STATUS_RUNNING) {
    printf("TID %u is still running\n", info.tid);
    return;
  }

  if (info.status < 0) {
    int err = -info.status;
    if (err == WAIT_ERR_NO_CHILD) {
      printf("poll failed: no such child\n");
    } else if (err == WAIT_ERR_SELF) {
      printf("poll failed: cannot poll self\n");
    } else {
      printf("poll failed: invalid request\n");
    }
    return;
  }

  if (info.reason == TASK_TERM_EXITED) {
    printf("TID %u exited with code %d\n", info.tid, info.exit_code);
  } else if (info.reason == TASK_TERM_KILLED) {
    printf("TID %u was killed\n", info.tid);
  } else if (info.reason == TASK_TERM_FAULTED) {
    printf("TID %u faulted: ESR=0x%lx ELR=0x%lx FAR=0x%lx\n",
           info.tid, info.esr, info.elr, info.far);
  } else {
    printf("TID %u ended for unknown reason %u\n", info.tid, info.reason);
  }
}

static void add_job(uint32_t tid, const char *name) {
  if (ensure_job_capacity(1) < 0) {
    printf("job table allocation failed; status is still available with wait %u\n", tid);
    return;
  }

  for (uint32_t i = 0; i < job_capacity; i++) {
    if (!jobs[i].active) {
      jobs[i].active = 1;
      jobs[i].tid = tid;
      int j = 0;
      while (name[j] && j < (int)sizeof(jobs[i].name) - 1) {
        jobs[i].name[j] = name[j];
        j++;
      }
      jobs[i].name[j] = '\0';
      return;
    }
  }

  uint32_t old_capacity = job_capacity;
  if (ensure_job_capacity(job_capacity + 1) < 0) {
    printf("job table growth failed; status is still available with wait %u\n", tid);
    return;
  }

  jobs[old_capacity].active = 1;
  jobs[old_capacity].tid = tid;
  int j = 0;
  while (name[j] && j < (int)sizeof(jobs[old_capacity].name) - 1) {
    jobs[old_capacity].name[j] = name[j];
    j++;
  }
  jobs[old_capacity].name[j] = '\0';
}

static void remove_job(uint32_t tid) {
  for (uint32_t i = 0; i < job_capacity; i++) {
    if (jobs[i].active && jobs[i].tid == tid) {
      jobs[i].active = 0;
      jobs[i].tid = 0;
      jobs[i].name[0] = '\0';
      return;
    }
  }
}

static void print_job_status(const shell_job_t *job, const wait_info_t *info) {
  if (info->status == WAIT_STATUS_RUNNING) {
    printf("%u   %s   running\n", job->tid, job->name);
  } else if (info->status < 0) {
    printf("%u   %s   gone\n", job->tid, job->name);
  } else if (info->reason == TASK_TERM_EXITED) {
    printf("%u   %s   exited(%d)\n", job->tid, job->name, info->exit_code);
  } else if (info->reason == TASK_TERM_KILLED) {
    printf("%u   %s   killed\n", job->tid, job->name);
  } else if (info->reason == TASK_TERM_FAULTED) {
    printf("%u   %s   faulted FAR=0x%lx\n", job->tid, job->name, info->far);
  } else {
    printf("%u   %s   unknown\n", job->tid, job->name);
  }
}

static void print_jobs(void) {
  int any = 0;

  printf("TID NAME STATUS\n");
  for (uint32_t i = 0; i < job_capacity; i++) {
    if (!jobs[i].active) {
      continue;
    }

    any = 1;
    wait_info_t info = sys_poll(jobs[i].tid);
    print_job_status(&jobs[i], &info);
    if (info.status < 0) {
      remove_job(jobs[i].tid);
    }
  }

  if (!any) {
    printf("(no jobs)\n");
  }
}

static int run_pong_exchange(uint32_t pong_tid) {
  (void)pong_tid;
  if (last_spawn_endpoint_cap < 0) {
    return -1;
  }
  for (int i = 0; i < 5; i++) {
    uint64_t payload[IPC_INLINE_WORDS] = {(uint64_t)i, 0};
    ipc_msg_t reply = sys_ipc_call((uint32_t)last_spawn_endpoint_cap, 0, 16, payload);
    if (reply.status < 0) {
      return -1;
    }
  }
  return 0;
}

static uint64_t ipcfast_word(uint32_t round, uint32_t word) {
  return 0xF000000000000000ULL | ((uint64_t)round << 32) | word;
}

static void run_ipcfast_demo(void) {
  uint32_t tid = spawn_program(IPCFAST_FILE, PONG_PRIORITY);
  int child_cap = last_spawn_endpoint_cap;
  if ((int)tid < 0 || child_cap < 0) {
    printf("ipcfast: spawn failed\n");
    return;
  }

  uint64_t lengths[4] = {0, 8, 64, 128};
  int ok = 1;
  for (uint32_t round = 0; round < 4; round++) {
    uint64_t payload[IPC_INLINE_WORDS] = {0};
    uint32_t words = (uint32_t)((lengths[round] + 7) / 8);
    for (uint32_t i = 0; i < words; i++) {
      payload[i] = ipcfast_word(round, i);
    }

    ipc_msg_t reply = sys_ipc_call((uint32_t)child_cap, 0, lengths[round], payload);
    uint64_t expected_last = words ? ipcfast_word(round, words - 1) : 0;
    if (reply.status < 0 || reply.payload[0] != 1 ||
        reply.payload[1] != lengths[round] || reply.payload[2] != expected_last) {
      ok = 0;
    }
  }

  wait_info_t status = sys_wait(tid);
  printf("ipcfast: %s 0/8/64/128 bytes\n",
         (ok && status.reason == TASK_TERM_EXITED && status.exit_code == 0) ? "ok" : "failed");
  print_wait_result(status);
}

static void run_ipccap_demo(void) {
  uint32_t server_tid = spawn_program(IPCCAP_FILE, PONG_PRIORITY);
  int server_cap = last_spawn_endpoint_cap;
  if ((int)server_tid < 0 || server_cap < 0) {
    printf("ipccap: server spawn failed\n");
    return;
  }

  uint64_t server_setup[IPC_INLINE_WORDS] = {
    [IPC_CAP_WORD_OP] = IPCCAP_MODE_SERVER
  };
  ipc_msg_t server_ready = sys_ipc_call((uint32_t)server_cap, 0, 8, server_setup);
  if (server_ready.status < 0 || server_ready.payload[0] != 1) {
    printf("ipccap: server setup failed\n");
    cleanup_blocked_child(server_tid);
    return;
  }

  uint32_t client_tid = spawn_program(IPCCAP_FILE, PONG_PRIORITY);
  int client_cap = last_spawn_endpoint_cap;
  if ((int)client_tid < 0 || client_cap < 0) {
    printf("ipccap: client spawn failed\n");
    cleanup_blocked_child(server_tid);
    return;
  }

  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_CAP_WORD_CAP] = (uint64_t)server_cap,
    [IPC_CAP_WORD_OP] = IPCCAP_MODE_CLIENT
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)client_cap, IPC_FLAG_CAP, 16, payload);
  wait_info_t client_status = sys_wait(client_tid);
  wait_info_t server_status = sys_wait(server_tid);
  int ok = reply.status == 0 && reply.payload[0] == 1 &&
           client_status.reason == TASK_TERM_EXITED && client_status.exit_code == 0 &&
           server_status.reason == TASK_TERM_EXITED && server_status.exit_code == 0;
  printf("ipccap: %s reply=%lu\n", ok ? "ok" : "failed", reply.payload[1]);
  print_wait_result(client_status);
  print_wait_result(server_status);
}

static int setup_ipckill_task(uint32_t tid, int endpoint_cap, uint64_t mode) {
  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_WORD_OP] = mode
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)endpoint_cap, 0, 8, payload);
  return reply.status == 0 && reply.payload[0] == 1 && (int)tid >= 0 ? 0 : -1;
}

static int start_ipckill_caller(uint32_t caller_tid, int caller_cap, int server_cap) {
  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_CAP_WORD_CAP] = (uint64_t)server_cap,
    [IPC_CAP_WORD_OP] = IPCKILL_CALLER_CALL
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)caller_cap, IPC_FLAG_CAP, 16, payload);
  return reply.status == 0 && reply.payload[0] == 1 && (int)caller_tid >= 0 ? 0 : -1;
}

static task_info_t find_task(uint32_t tid) {
  task_info_t empty = {0};
  uint32_t capacity = sys_task_capacity();
  for (uint32_t i = 0; i < capacity; i++) {
    task_info_t task = sys_ps(i);
    if (task.present && task.tid == tid) {
      return task;
    }
  }
  return empty;
}

static int wait_for_task_state(uint32_t tid, uint32_t state, uint32_t attempts, uint64_t sleep_ms) {
  for (uint32_t i = 0; i < attempts; i++) {
    task_info_t task = find_task(tid);
    if (task.present && task.state == state) {
      return 0;
    }
    sys_sleep(sleep_ms);
  }
  return -1;
}

static wait_info_t poll_until_done(uint32_t tid, uint32_t attempts, uint64_t sleep_ms) {
  wait_info_t info = {0};
  for (uint32_t i = 0; i < attempts; i++) {
    info = sys_poll(tid);
    if (info.status == WAIT_STATUS_DONE) {
      return info;
    }
    sys_sleep(sleep_ms);
  }
  return info;
}

static void run_ipckill_demo(void) {
  int ok_server_kill = 0;
  int ok_caller_kill = 0;
  int ok_recv_kill = 0;

  uint32_t server_tid = spawn_program(IPCKILL_FILE, IPCKILL_PRIORITY);
  int server_cap = last_spawn_endpoint_cap;
  if ((int)server_tid >= 0 && setup_ipckill_task(server_tid, server_cap, IPCKILL_SERVER_HOLD) == 0) {
    uint32_t caller_tid = spawn_program(IPCKILL_FILE, IPCKILL_PRIORITY);
    int caller_cap = last_spawn_endpoint_cap;
    if ((int)caller_tid >= 0 && start_ipckill_caller(caller_tid, caller_cap, server_cap) == 0) {
      sys_sleep(100);
      int killed = sys_kill(server_tid);
      wait_info_t server_status = sys_wait(server_tid);
      wait_info_t caller_status = sys_wait(caller_tid);
      ok_server_kill = killed == 0 &&
                       server_status.reason == TASK_TERM_KILLED &&
                       caller_status.reason == TASK_TERM_EXITED &&
                       caller_status.exit_code == 0;
    } else if ((int)caller_tid >= 0) {
      cleanup_blocked_child(caller_tid);
      cleanup_blocked_child(server_tid);
    }
  } else if ((int)server_tid >= 0) {
    cleanup_blocked_child(server_tid);
  }

  server_tid = spawn_program(IPCKILL_FILE, IPCKILL_PRIORITY);
  server_cap = last_spawn_endpoint_cap;
  if ((int)server_tid >= 0 && setup_ipckill_task(server_tid, server_cap, IPCKILL_SERVER_DELAY_REPLY) == 0) {
    uint32_t caller_tid = spawn_program(IPCKILL_FILE, IPCKILL_PRIORITY);
    int caller_cap = last_spawn_endpoint_cap;
    if ((int)caller_tid >= 0 && start_ipckill_caller(caller_tid, caller_cap, server_cap) == 0) {
      int server_holds_reply = wait_for_task_state(server_tid, SHELL_TASK_STATE_SLEEPING, 30, 100) == 0;
      int killed = sys_kill(caller_tid);
      wait_info_t caller_status = sys_wait(caller_tid);
      wait_info_t server_status = poll_until_done(server_tid, 50, 100);
      ok_caller_kill = server_holds_reply &&
                       killed == 0 &&
                       caller_status.reason == TASK_TERM_KILLED &&
                       server_status.reason == TASK_TERM_EXITED &&
                       server_status.exit_code == 0;
      if (server_status.status == WAIT_STATUS_RUNNING) {
        sys_kill(server_tid);
        (void)sys_wait(server_tid);
      } else if (server_status.status == WAIT_STATUS_DONE) {
        (void)sys_wait(server_tid);
      }
    } else if ((int)caller_tid >= 0) {
      cleanup_blocked_child(caller_tid);
      cleanup_blocked_child(server_tid);
    }
  } else if ((int)server_tid >= 0) {
    cleanup_blocked_child(server_tid);
  }

  uint32_t waiter_tid = spawn_program(IPCKILL_FILE, IPCKILL_PRIORITY);
  int waiter_cap = last_spawn_endpoint_cap;
  if ((int)waiter_tid >= 0 && setup_ipckill_task(waiter_tid, waiter_cap, IPCKILL_RECV_WAIT) == 0) {
    sys_sleep(100);
    int killed = sys_kill(waiter_tid);
    wait_info_t waiter_status = sys_wait(waiter_tid);
    ok_recv_kill = killed == 0 && waiter_status.reason == TASK_TERM_KILLED;
  } else if ((int)waiter_tid >= 0) {
    cleanup_blocked_child(waiter_tid);
  }

  printf("ipckill: %s server=%d caller=%d recv=%d\n",
         (ok_server_kill && ok_caller_kill && ok_recv_kill) ? "ok" : "failed",
         ok_server_kill, ok_caller_kill, ok_recv_kill);
}

static int run_and_expect_exit(const char *name) {
  uint32_t tid = spawn_program(name, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("smoke: %s spawn failed\n", name);
    return -1;
  }

  wait_info_t status = sys_wait(tid);
  int ok = status.reason == TASK_TERM_EXITED && status.exit_code == 0;
  printf("smoke: %s %s\n", name, ok ? "ok" : "failed");
  if (!ok) {
    print_wait_result(status);
    return -1;
  }
  return 0;
}

static void run_smoke_demo(void) {
  int failures = 0;

  printf("smoke: starting\n");
  run_vfstest_demo();
  run_vfsinject_demo();
  run_vfswrite_demo();
  run_vfsinstall_demo();
  run_ipcfast_demo();
  run_ipccap_demo();
  if (run_and_expect_exit(BADPTR_FILE) < 0) failures++;
  if (run_and_expect_exit(STACKGROW_FILE) < 0) failures++;
  if (run_and_expect_exit(SPEED_FILE) < 0) failures++;

  printf("smoke: %s failures=%d\n", failures == 0 ? "done" : "failed", failures);
}

static int get_keyboard_cap(void) {
  while (keyboard_cap < 0) {
    keyboard_cap = ns_resolve("keyboard");
    if (keyboard_cap < 0) {
      sys_sleep(10);
    }
  }
  return keyboard_cap;
}

static char read_console_char(void) {
  if (graphical_terminal) {
    char c = terminal_read_char();
    if (c) {
      return c;
    }
    graphical_terminal = 0;
  }
  if (pending_key_index < pending_key_count) {
    return pending_key_chars[pending_key_index++];
  }
  pending_key_count = 0;
  pending_key_index = 0;

  while (1) {
    uint64_t request[IPC_INLINE_WORDS] = {KEYBOARD_REQ_READ};
    ipc_msg_t reply =
        sys_ipc_call((uint32_t)get_keyboard_cap(), 0, 8, request);
    if (reply.status < 0) {
      sys_sleep(10);
      continue;
    }
    uint64_t event = reply.payload[0];
    if (KEYBOARD_EVENT_TYPE(event) != KEYBOARD_EVENT_KEY ||
        (KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_PRESS &&
         KEYBOARD_EVENT_VALUE(event) != KEYBOARD_KEY_REPEAT)) {
      continue;
    }
    char c = KEYBOARD_EVENT_CHAR(event);
    if (c) {
      return c;
    }
    uint32_t code = (uint32_t)KEYBOARD_EVENT_CODE(event);
    char suffix = 0;
    if (code == 103) suffix = 'A';
    else if (code == 108) suffix = 'B';
    else if (code == 106) suffix = 'C';
    else if (code == 105) suffix = 'D';
    else if (code == 102) suffix = 'H';
    else if (code == 107) suffix = 'F';
    else if (code == 111) {
      pending_key_chars[0] = '[';
      pending_key_chars[1] = '3';
      pending_key_chars[2] = '~';
      pending_key_count = 3;
      pending_key_index = 0;
      return 27;
    }
    if (suffix) {
      pending_key_chars[0] = '[';
      pending_key_chars[1] = suffix;
      pending_key_count = 2;
      pending_key_index = 0;
      return 27;
    }
  }
}


static uint32_t spawn_program(const char *name, uint8_t priority) {
  return spawn_program_args(name, priority, 0, 0);
}

static uint32_t spawn_program_args(const char *name, uint8_t priority,
                                   char *const argv[], uint32_t argc) {
  last_spawn_endpoint_cap = -1;
  spawn_result_t spawned = vfs_spawn_program_args(name, priority, argv, argc);
  if (spawned.tid != (uint32_t)-1) {
    last_spawn_endpoint_cap = spawned.endpoint_cap;
  }
  return spawned.tid;
}


static void run_vfstest_demo(void) {
  if (ensure_vfs_bound() < 0) {
    printf("vfstest: vfs server unavailable\n");
    return;
  }

  vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_PING, 41, 7);
  int ok = reply.status == 0 && reply.value0 == 42 && reply.value1 == 7;
  printf("vfstest: %s value=%lu echo=%lu client=%lu\n",
         ok ? "ok" : "failed", reply.value0, reply.value1, reply.value2);
}

static void run_vfsinject_demo(void) {
  if (ensure_vfs_bound() < 0) {
    printf("vfsinject: vfs server unavailable\n");
    return;
  }

  char *name = (char *)sys_mmap(PAGE_BYTES);
  if (!name) {
    printf("vfsinject: mmap failed\n");
    return;
  }

  for (uint32_t i = 0; i < PAGE_BYTES; i++) {
    name[i] = 0;
  }
  vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_LIST, 0, (uint64_t)name);
  int ok = reply.status == 0 && reply.value0 != FS_RESP_END && name[0] != '\0';
  printf("vfsinject: %s size=%lu name=%s client=0x%lx fs=0x%lx\n",
         ok ? "ok" : "failed", reply.value0, name, reply.value1, reply.value2);
  sys_munmap(name, PAGE_BYTES);
}

static void list_files_vfs(const char *path) {
  if (ensure_vfs_bound() < 0) {
    printf("vfsls: vfs server unavailable\n");
    return;
  }

  printf("SIZE      NAME\n");
  for (uint32_t index = 0; index < 256; index++) {
    char *name = (char *)sys_mmap(PAGE_BYTES);
    if (!name) {
      printf("vfsls: mmap failed\n");
      return;
    }

    for (uint32_t i = 0; i < PAGE_BYTES; i++) {
      name[i] = 0;
    }
    if (path && path[0] && !(path[0] == '/' && path[1] == '\0')) {
      write_literal(name, path);
    }
    vfs_reply_t reply = vfs_call_retry(VFS_FS_REQ_LIST, index, (uint64_t)name);
    if (reply.status < 0) {
      sys_munmap(name, PAGE_BYTES);
      printf("vfsls: call failed\n");
      return;
    }
    if (reply.value0 == FS_RESP_END) {
      sys_munmap(name, PAGE_BYTES);
      return;
    }

    printf("%lu  %s\n", reply.value0, name);
    sys_munmap(name, PAGE_BYTES);
  }
}


static void run_vfsread_demo(void) {
  unsigned char *buf = (unsigned char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    printf("vfsread: mmap failed\n");
    return;
  }

  vfs_file_info_t info = vfs_open_file("/bin/shell.elf");
  int64_t got = info.status < 0 ? -1 : vfs_read_handle_page(info.handle, 0, buf);
  int ok = got >= 4 &&
           buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F';
  printf("vfsread: %s bytes=%ld total=%lu magic=%x %c%c%c\n",
         ok ? "ok" : "failed", got, info.size,
         (uint32_t)buf[0], buf[1], buf[2], buf[3]);
  if (info.status == 0) {
    vfs_close_handle(info.handle);
  }
  sys_munmap(buf, PAGE_BYTES);
}

static void run_vfswrite_demo(void) {
  fs_write_page_t *request = (fs_write_page_t *)sys_mmap(PAGE_BYTES);
  char *readback = (char *)sys_mmap(PAGE_BYTES);
  if (!request || !readback) {
    printf("vfswrite: mmap failed\n");
    if (request) sys_munmap(request, PAGE_BYTES);
    if (readback) sys_munmap(readback, PAGE_BYTES);
    return;
  }

  for (uint32_t i = 0; i < PAGE_BYTES; i++) {
    ((char *)request)[i] = 0;
    readback[i] = 0;
  }

  const char *name = "notes.txt";
  const char *text = "Neptune writable FatFs path is alive.";
  write_literal(request->name, name);
  write_literal((char *)request->data, text);
  request->offset = 0;
  request->size = 0;
  while (text[request->size]) {
    request->size++;
  }
  request->flags = VFS_WRITE_FLAG_TRUNCATE;

  int64_t wrote = vfs_write_page(request);
  vfs_file_info_t info = vfs_open_file(name);
  int64_t read = -1;
  if (info.status == 0) {
    read = vfs_read_handle_page(info.handle, 0, readback);
    vfs_close_handle(info.handle);
  }

  int ok = wrote == (int64_t)request->size &&
           read >= wrote &&
           streq(readback, text);
  printf("vfswrite: %s wrote=%ld read=%ld file=%s size=%lu\n",
         ok ? "ok" : "failed", wrote, read, name,
         info.status == 0 ? info.size : 0);

  sys_munmap(request, PAGE_BYTES);
  sys_munmap(readback, PAGE_BYTES);
}

static void run_vfsexec_demo(void) {
  vfs_file_info_t info = vfs_open_file(BADPTR_FILE);
  if (info.status < 0) {
    printf("vfsexec: could not resolve %s through VFS\n", BADPTR_FILE);
    return;
  }
  uint32_t index = info.index;
  uint64_t size = info.size;
  vfs_close_handle(info.handle);

  uint32_t tid = spawn_program(BADPTR_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("vfsexec: spawn failed\n");
    return;
  }

  wait_info_t status = sys_wait(tid);
  int ok = status.reason == TASK_TERM_EXITED && status.exit_code == 0;
  printf("vfsexec: %s index=%lu size=%lu tid=%u\n",
         ok ? "ok" : "failed", (uint64_t)index, size, tid);
  print_wait_result(status);
}

static void run_vfsopen_command(const char *name) {
  vfs_file_info_t info = vfs_open_file(name);
  if (info.status < 0) {
    printf("vfsopen: '%s' not found\n", name);
    return;
  }

  printf("vfsopen: %s index=%u size=%lu\n", name, info.index, info.size);
  vfs_close_handle(info.handle);
}

static void run_cat_command(const char *name) {
  vfs_file_info_t info = vfs_open_file(name);
  if (info.status < 0) {
    printf("cat: '%s' not found\n", name);
    return;
  }

  unsigned char *buf = (unsigned char *)sys_mmap(PAGE_BYTES);
  if (!buf) {
    vfs_close_handle(info.handle);
    printf("cat: mmap failed\n");
    return;
  }

  uint32_t page = 0;
  uint64_t shown = 0;
  while (shown < info.size) {
    int64_t got = vfs_read_handle_page(info.handle, page, buf);
    if (got < 0) {
      printf("\ncat: read failed\n");
      break;
    }
    if (got == 0) {
      break;
    }

    char out[129];
    uint32_t out_len = 0;
    for (uint64_t i = 0; i < (uint64_t)got && shown < info.size; i++, shown++) {
      unsigned char c = buf[i];
      if (c == '\n' || c == '\r' || c == '\t' || (c >= 32 && c <= 126)) {
        out[out_len++] = (char)c;
      } else {
        out[out_len++] = '.';
      }
      if (out_len == 128) {
        out[out_len] = '\0';
        puts(out);
        out_len = 0;
      }
    }
    if (out_len) {
      out[out_len] = '\0';
      puts(out);
    }
    page++;
  }

  if (shown == 0 || (shown && buf[(shown - 1) & (PAGE_BYTES - 1)] != '\n')) {
    printf("\n");
  }
  sys_munmap(buf, PAGE_BYTES);
  vfs_close_handle(info.handle);
}

static void run_rm_command(const char *name) {
  if (vfs_delete_file(name) < 0) {
    printf("rm: could not remove '%s'\n", name);
    return;
  }
  printf("removed %s\n", name);
}

static void run_copy_command(const char *src, const char *dst, const char *verb) {
  int64_t copied = vfs_copy_file(src, dst);
  if (copied < 0) {
    printf("%s: failed %s -> %s\n", verb, src, dst);
    return;
  }
  printf("%s: %s -> %s (%ld bytes)\n", verb, src, dst, copied);
}

static void run_recvhex_command(const char *name, uint32_t size) {
  if (!name || name[0] == '\0' || size == 0) {
    printf("usage: recvhex <file> <size>\n");
    return;
  }

  fs_write_page_t *request = (fs_write_page_t *)sys_mmap(PAGE_BYTES);
  if (!request) {
    printf("recvhex: mmap failed\n");
    return;
  }

  printf("recvhex: paste %u bytes as hex for %s\n", size, name);

  uint32_t written_total = 0;
  uint32_t chunk_used = 0;
  int high_nibble = -1;
  int failed = 0;

  for (uint32_t i = 0; i < PAGE_BYTES; i++) {
    ((char *)request)[i] = 0;
  }
  write_literal(request->name, name);
  request->offset = 0;
  request->flags = VFS_WRITE_FLAG_TRUNCATE;

  while (written_total < size && !failed) {
    char c = read_console_char();
    if (is_space_char(c)) {
      continue;
    }

    int value = hex_value(c);
    if (value < 0) {
      printf("\nrecvhex: invalid hex character\n");
      failed = 1;
      break;
    }

    if (high_nibble < 0) {
      high_nibble = value;
      continue;
    }

    request->data[chunk_used++] = (uint8_t)((high_nibble << 4) | value);
    high_nibble = -1;

    if (chunk_used == FS_WRITE_DATA_MAX || written_total + chunk_used == size) {
      request->size = chunk_used;
      int64_t wrote = vfs_write_page(request);
      if (wrote != (int64_t)chunk_used) {
        printf("\nrecvhex: write failed at offset %u\n", written_total);
        failed = 1;
        break;
      }

      written_total += chunk_used;
      request->offset = written_total;
      request->flags = 0;
      chunk_used = 0;
      for (uint32_t i = 0; i < FS_WRITE_DATA_MAX; i++) {
        request->data[i] = 0;
      }
      printf(".");
    }
  }

  if (!failed && high_nibble >= 0) {
    printf("\nrecvhex: odd number of hex digits\n");
    failed = 1;
  }

  sys_munmap(request, PAGE_BYTES);
  if (failed) {
    (void)vfs_delete_file(name);
    return;
  }

  printf("\nrecvhex: wrote %u bytes to %s\n", written_total, name);
}

static void run_vfsinstall_demo(void) {
  const char *dst = "badcopy.elf";
  (void)vfs_delete_file(dst);
  int64_t copied = vfs_copy_file(BADPTR_FILE, dst);
  if (copied < 0) {
    printf("vfsinstall: copy failed\n");
    return;
  }

  uint32_t tid = spawn_program(dst, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("vfsinstall: run failed after copy\n");
    return;
  }

  wait_info_t status = sys_wait(tid);
  int ok = status.reason == TASK_TERM_EXITED && status.exit_code == 0;
  printf("vfsinstall: %s copied=%ld file=%s tid=%u\n",
         ok ? "ok" : "failed", copied, dst, tid);
  print_wait_result(status);
  if (vfs_delete_file(dst) < 0) {
    printf("vfsinstall: cleanup rm failed\n");
  }
}

typedef struct shell_command shell_command_t;
typedef void (*shell_handler_t)(int argc, char **argv, char *line);

struct shell_command {
  const char *name;
  const char *usage;
  const char *group;
  const char *description;
  shell_handler_t handler;
};

static void shell_copy(char *dst, const char *src, uint32_t limit) {
  uint32_t i = 0;
  if (limit == 0) {
    return;
  }
  while (i + 1 < limit && src[i]) {
    dst[i] = src[i];
    i++;
  }
  dst[i] = '\0';
}

static int parse_args(char *line, char **argv, int max_args) {
  int argc = 0;
  char *p = line;

  while (*p && argc < max_args) {
    while (*p == ' ' || *p == '\t') {
      p++;
    }
    if (*p == '\0') {
      break;
    }
    argv[argc++] = p;
    while (*p && *p != ' ' && *p != '\t') {
      p++;
    }
    if (*p) {
      *p++ = '\0';
    }
  }

  return argc;
}

static void print_usage(const shell_command_t *cmd) {
  printf("usage: %s\n", cmd->usage);
}

static void cmd_help(int argc, char **argv, char *line);
static void cmd_clear(int argc, char **argv, char *line);
static void cmd_echo(int argc, char **argv, char *line);
static void cmd_uptime(int argc, char **argv, char *line);
static void cmd_ls(int argc, char **argv, char *line);
static void cmd_cat(int argc, char **argv, char *line);
static void cmd_cp(int argc, char **argv, char *line);
static void cmd_install(int argc, char **argv, char *line);
static void cmd_recvhex(int argc, char **argv, char *line);
static void cmd_rm(int argc, char **argv, char *line);
static void cmd_mkdir(int argc, char **argv, char *line);
static void cmd_mounts(int argc, char **argv, char *line);
static void cmd_run(int argc, char **argv, char *line);
static void cmd_runbg(int argc, char **argv, char *line);
static void cmd_spawn(int argc, char **argv, char *line);
static void cmd_ps(int argc, char **argv, char *line);
static void cmd_jobs(int argc, char **argv, char *line);
static void cmd_kill(int argc, char **argv, char *line);
static void cmd_wait(int argc, char **argv, char *line);
static void cmd_poll(int argc, char **argv, char *line);
static void cmd_mem(int argc, char **argv, char *line);
static void cmd_vmmap(int argc, char **argv, char *line);
static void cmd_capstat(int argc, char **argv, char *line);
static void cmd_debug(int argc, char **argv, char *line);
static void cmd_ipcstat(int argc, char **argv, char *line);
static void cmd_termtest(int argc, char **argv, char *line);
static void cmd_gfx(int argc, char **argv, char *line);
static void cmd_gfxstat(int argc, char **argv, char *line);
static void cmd_mouse(int argc, char **argv, char *line);
static void cmd_desktop(int argc, char **argv, char *line);
static void cmd_gfxdemo(int argc, char **argv, char *line);
static void cmd_gfxbench(int argc, char **argv, char *line);
static void cmd_snake(int argc, char **argv, char *line);
static void cmd_libctest(int argc, char **argv, char *line);
static void cmd_argfdtest(int argc, char **argv, char *line);
static void cmd_pipetest(int argc, char **argv, char *line);
static void cmd_tcc(int argc, char **argv, char *line);
static void cmd_pong(int argc, char **argv, char *line);
static void cmd_fault(int argc, char **argv, char *line);
static void cmd_badptr(int argc, char **argv, char *line);
static void cmd_smoke(int argc, char **argv, char *line);
static void cmd_vfstest(int argc, char **argv, char *line);
static void cmd_vfsinject(int argc, char **argv, char *line);
static void cmd_vfsls(int argc, char **argv, char *line);
static void cmd_vfsopen(int argc, char **argv, char *line);
static void cmd_vfsread(int argc, char **argv, char *line);
static void cmd_vfswrite(int argc, char **argv, char *line);
static void cmd_vfsinstall(int argc, char **argv, char *line);
static void cmd_vfsexec(int argc, char **argv, char *line);
static void cmd_ipcfast(int argc, char **argv, char *line);
static void cmd_ipccap(int argc, char **argv, char *line);
static void cmd_ipckill(int argc, char **argv, char *line);
static void cmd_memshare(int argc, char **argv, char *line);
static void cmd_memxfer(int argc, char **argv, char *line);
static void cmd_memcaplife(int argc, char **argv, char *line);
static void cmd_memrevoke(int argc, char **argv, char *line);
static void cmd_munmap(int argc, char **argv, char *line);
static void cmd_forkcow(int argc, char **argv, char *line);
static void cmd_filelazy(int argc, char **argv, char *line);
static void cmd_vmstress(int argc, char **argv, char *line);
static void cmd_lazyexec(int argc, char **argv, char *line);
static void cmd_taskstress(int argc, char **argv, char *line);
static void cmd_speed(int argc, char **argv, char *line);
static void cmd_demo(int argc, char **argv, char *line);

static const shell_command_t commands[] = {
  {"help", "help [command]", "core", "Show grouped help or details for one command", cmd_help},
  {"clear", "clear", "core", "Clear the terminal", cmd_clear},
  {"echo", "echo <text>", "core", "Print text", cmd_echo},
  {"uptime", "uptime", "core", "Show uptime", cmd_uptime},

  {"ls", "ls [path]", "files", "List files through the VFS fast channel", cmd_ls},
  {"cat", "cat <file>", "files", "Print a file through VFS", cmd_cat},
  {"cp", "cp <src> <dst>", "files", "Copy a file through VFS", cmd_cp},
  {"install", "install <src> <dst>", "files", "Copy an executable into the writable filesystem", cmd_install},
  {"recvhex", "recvhex <file> <size>", "files", "Receive hex bytes from the console into a file", cmd_recvhex},
  {"rm", "rm <file>", "files", "Remove a file through VFS", cmd_rm},
  {"mkdir", "mkdir <path>", "files", "Create a directory through VFS", cmd_mkdir},
  {"mounts", "mounts", "files", "Show mounted filesystems", cmd_mounts},
  {"vfsls", "vfsls [path]", "files", "List files through the VFS fast channel", cmd_vfsls},
  {"vfsopen", "vfsopen <file>", "files", "Show VFS metadata for one file", cmd_vfsopen},
  {"vfsread", "vfsread", "files", "Read one file page through VFS injection", cmd_vfsread},
  {"vfswrite", "vfswrite", "files", "Create and verify notes.txt through writable FatFs", cmd_vfswrite},
  {"vfsinstall", "vfsinstall", "files", "Copy and run an ELF from the writable FS", cmd_vfsinstall},
  {"vfsexec", "vfsexec", "files", "Resolve an executable through VFS, create an exec cap, and spawn it", cmd_vfsexec},
  {"run", "run <file> [args...]", "files", "Run an ELF in the foreground with argv", cmd_run},
  {"runbg", "runbg <file> [args...]", "files", "Spawn an ELF in the background with argv", cmd_runbg},

  {"ps", "ps", "process", "Show task scheduler state", cmd_ps},
  {"jobs", "jobs", "process", "Show shell child jobs", cmd_jobs},
  {"spawn", "spawn <pong|fault|spin>", "process", "Spawn a demo child and leave status for wait", cmd_spawn},
  {"kill", "kill <tid>", "process", "Kill a user task", cmd_kill},
  {"wait", "wait <tid>", "process", "Wait for and reap child status", cmd_wait},
  {"poll", "poll <tid>", "process", "Check child status without reaping", cmd_poll},

  {"mem", "mem", "diagnostics", "Show RAM and heap usage", cmd_mem},
  {"vmmap", "vmmap", "diagnostics", "Show this task's VMA table", cmd_vmmap},
  {"capstat", "capstat", "diagnostics", "Show this task's capability slots", cmd_capstat},
  {"debug", "debug", "diagnostics", "Show kernel/runtime debug info", cmd_debug},
  {"ipcstat", "ipcstat", "diagnostics", "Show IPC fast-path profile counters", cmd_ipcstat},
  {"termtest", "termtest", "diagnostics", "Render terminal font, color, wrap, cursor, and scrollback test page", cmd_termtest},
  {"gfx", "gfx", "diagnostics", "Query and redraw the graphical display", cmd_gfx},
  {"gfxstat", "gfxstat [reset]", "diagnostics", "Show or reset compositor dirty-rect counters", cmd_gfxstat},
  {"mouse", "mouse", "diagnostics", "Show mouse driver event counters", cmd_mouse},
  {"desktop", "desktop", "diagnostics", "Draw capability-shared compositor surfaces", cmd_desktop},
  {"gfxdemo", "gfxdemo", "diagnostics", "Run a bounded animated compositor stress demo", cmd_gfxdemo},
  {"gfxbench", "gfxbench", "diagnostics", "Run bounded compositor/GPU benchmark and print counters", cmd_gfxbench},
  {"snake", "snake", "diagnostics", "Play graphical Snake with WASD", cmd_snake},
  {"libctest", "libctest", "tests", "Run Neptune libc regression checks", cmd_libctest},
  {"argfdtest", "argfdtest", "tests", "Verify argv and libc file descriptors", cmd_argfdtest},
  {"pipetest", "pipetest", "tests", "Verify kernel pipes, endpoint duplication, and EOF", cmd_pipetest},
  {"tcc", "tcc [options...]", "development", "Run the native Tiny C Compiler", cmd_tcc},

  {"demo", "demo [quick|vfs|ipc|mem|proc|all]", "tests", "Run curated bounded demo suites", cmd_demo},
  {"smoke", "smoke", "tests", "Run bounded core regression checks", cmd_smoke},
  {"vfstest", "vfstest", "tests", "Test VFS call/reply handoff", cmd_vfstest},
  {"vfsinject", "vfsinject", "tests", "Test shared page injection", cmd_vfsinject},

  {"ipcfast", "ipcfast", "ipc/memory", "Test 0/8/64/128-byte register IPC payloads", cmd_ipcfast},
  {"ipccap", "ipccap", "ipc/memory", "Transfer an endpoint cap through IPC", cmd_ipccap},
  {"ipckill", "ipckill", "ipc/memory", "Verify IPC cleanup on task kill", cmd_ipckill},
  {"memshare", "memshare", "ipc/memory", "Share one page with another task", cmd_memshare},
  {"memxfer", "memxfer", "ipc/memory", "Transfer one page to another task", cmd_memxfer},
  {"memcaplife", "memcaplife", "ipc/memory", "Use a memory cap after unmapping the old VA", cmd_memcaplife},
  {"memrevoke", "memrevoke", "ipc/memory", "Lend and revoke a page", cmd_memrevoke},
  {"munmap", "munmap", "ipc/memory", "Test unmap and stale export rejection", cmd_munmap},
  {"forkcow", "forkcow", "ipc/memory", "Verify copy-on-write after fork", cmd_forkcow},

  {"filelazy", "filelazy", "tests", "Read a file page through the VFS page path", cmd_filelazy},
  {"vmstress", "vmstress", "tests", "Stress VMA growth with file mappings", cmd_vmstress},
  {"lazyexec", "lazyexec", "tests", "Spawn badptr.elf through the VFS executable path", cmd_lazyexec},
  {"taskstress", "taskstress", "tests", "Stress repeated task spawn, kill, and reap", cmd_taskstress},
  {"speed", "speed", "tests", "Benchmark syscalls, yield, faults, memory, and IPC", cmd_speed},
  {"pong", "pong", "tests", "Run the IPC pong demo", cmd_pong},
  {"fault", "fault", "tests", "Spawn a deliberate faulting task", cmd_fault},
  {"badptr", "badptr", "tests", "Verify invalid syscall pointers are rejected", cmd_badptr},
};

static const uint32_t command_count = sizeof(commands) / sizeof(commands[0]);

static const shell_command_t *find_command(const char *name) {
  for (uint32_t i = 0; i < command_count; i++) {
    if (_streq(commands[i].name, name)) {
      return &commands[i];
    }
  }
  return 0;
}

static int command_accepts_args(const shell_command_t *cmd) {
  const char *p = cmd->usage;
  while (*p) {
    if (*p == ' ') {
      return 1;
    }
    p++;
  }
  return 0;
}

static void print_grouped_help(void) {
  static const char *groups[] = {
    "core", "files", "process", "diagnostics", "ipc/memory", "tests"
  };
  static const char spaces[] = "                        ";

  printf("Available commands:\n");
  for (uint32_t g = 0; g < sizeof(groups) / sizeof(groups[0]); g++) {
    printf("\n%s:\n", groups[g]);
    for (uint32_t i = 0; i < command_count; i++) {
      if (_streq(commands[i].group, groups[g])) {
        printf("  %s", commands[i].usage);
        uint32_t len = 0;
        while (commands[i].usage[len]) len++;
        puts(spaces + (len < 24 ? len : 24));
        printf("%s\n", commands[i].description);
      }
    }
  }
}

static void cmd_help(int argc, char **argv, char *line) {
  (void)line;
  if (argc == 1) {
    print_grouped_help();
    return;
  }
  if (argc != 2) {
    print_usage(find_command("help"));
    return;
  }

  const shell_command_t *cmd = find_command(argv[1]);
  if (!cmd) {
    printf("No help for '%s'\n", argv[1]);
    return;
  }

  printf("%s\n", cmd->usage);
  printf("  group: %s\n", cmd->group);
  printf("  %s\n", cmd->description);
}

static void cmd_clear(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  printf("\x1b[2J\x1b[H");
}

static void cmd_echo(int argc, char **argv, char *line) {
  (void)argv;
  if (argc < 2) {
    printf("\n");
    return;
  }
  char *text = line + 4;
  while (*text == ' ' || *text == '\t') {
    text++;
  }
  printf("%s\n", text);
}

static void cmd_uptime(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint64_t secs = sys_uptime();
  uint64_t h = secs / 3600;
  uint64_t m = (secs % 3600) / 60;
  uint64_t s = secs % 60;
  printf("Uptime: %luh %lum %lus\n", h, m, s);
}

static void cmd_ls(int argc, char **argv, char *line) {
  (void)line;
  if (argc > 2) {
    print_usage(find_command("ls"));
    return;
  }
  list_files_vfs(argc == 2 ? argv[1] : "");
}

static void cmd_cat(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("cat"));
    return;
  }
  run_cat_command(argv[1]);
}

static void cmd_cp(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 3) {
    print_usage(find_command("cp"));
    return;
  }
  run_copy_command(argv[1], argv[2], "cp");
}

static void cmd_install(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 3) {
    print_usage(find_command("install"));
    return;
  }
  run_copy_command(argv[1], argv[2], "install");
}

static void cmd_recvhex(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 3) {
    print_usage(find_command("recvhex"));
    return;
  }
  int ok = 0;
  uint32_t size = _parse_u32(argv[2], &ok);
  if (!ok || size == 0) {
    print_usage(find_command("recvhex"));
    return;
  }
  run_recvhex_command(argv[1], size);
}

static void cmd_rm(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("rm"));
    return;
  }
  run_rm_command(argv[1]);
}

static void cmd_mkdir(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("mkdir"));
    return;
  }
  if (vfs_mkdir(argv[1]) < 0) {
    printf("mkdir: could not create '%s'\n", argv[1]);
  }
}

static void cmd_mounts(int argc, char **argv, char *line) {
  (void)argv;
  (void)line;
  if (argc != 1) {
    print_usage(find_command("mounts"));
    return;
  }

  vfs_statfs_t stat = vfs_statfs();
  if (stat.status < 0) {
    printf("mounts: VFS root unavailable\n");
    return;
  }

  printf("MOUNT TYPE  SOURCE       TOTAL KiB FREE KiB CLUSTER\n");
  printf("/     fatfs virtio-blk0  %lu %lu %lu\n",
         stat.total_kib, stat.free_kib, stat.cluster_bytes);
}

static void cmd_run(int argc, char **argv, char *line) {
  (void)line;
  if (argc < 2) {
    print_usage(find_command("run"));
    return;
  }
  uint32_t tid = spawn_program_args(argv[1], PONG_PRIORITY,
                                    &argv[1], (uint32_t)(argc - 1));
  if ((int)tid < 0) {
    printf("Error: could not run '%s'\n", argv[1]);
  } else {
    wait_info_t result = sys_wait(tid);
    if (result.reason != TASK_TERM_EXITED || result.exit_code != 0) {
      print_wait_result(result);
    }
  }
}

static void cmd_runbg(int argc, char **argv, char *line) {
  (void)line;
  if (argc < 2) {
    print_usage(find_command("runbg"));
    return;
  }
  uint32_t tid = spawn_program_args(argv[1], PONG_PRIORITY,
                                    &argv[1], (uint32_t)(argc - 1));
  if ((int)tid < 0) {
    printf("Error: could not run '%s'\n", argv[1]);
  } else {
    add_job(tid, argv[1]);
    printf("Started %s as TID %u\n", argv[1], tid);
  }
}

static void cmd_spawn(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("spawn"));
    return;
  }

  if (_streq(argv[1], "pong")) {
    printf("Spawning pong...\n");
    uint32_t pong_tid = spawn_program(PONG_FILE, PONG_PRIORITY);
    if ((int)pong_tid < 0) {
      printf("Error: Could not spawn pong (task limit reached?)\n");
    } else {
      printf("Pong is TID %d. Exchanging 5 messages...\n", (int)pong_tid);
      if (run_pong_exchange(pong_tid) < 0) {
        printf("Error: pong IPC failed\n");
      } else {
        add_job(pong_tid, "pong");
        printf("Pong finished. Run 'wait %u' to reap it.\n", pong_tid);
      }
    }
  } else if (_streq(argv[1], "fault")) {
    printf("Spawning fault test...\n");
    uint32_t fault_tid = spawn_program(FAULT_FILE, PONG_PRIORITY);
    if ((int)fault_tid < 0) {
      printf("Error: Could not spawn fault test (task limit reached?)\n");
    } else {
      add_job(fault_tid, "fault");
      printf("Fault test is TID %d. Run 'wait %u' to reap it.\n",
             (int)fault_tid, fault_tid);
    }
  } else if (_streq(argv[1], "spin")) {
    uint32_t spin_tid = spawn_program(SPIN_FILE, 0);
    if ((int)spin_tid < 0) {
      printf("Error: Could not spawn spin task (task limit reached?)\n");
    } else {
      add_job(spin_tid, "spin");
      printf("Spin task is TID %d. Run 'ps', 'kill %u', then 'wait %u'.\n",
             (int)spin_tid, spin_tid, spin_tid);
    }
  } else {
    print_usage(find_command("spawn"));
  }
}

static void cmd_ps(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  printf("TID PPID CPU STATE   PRI BASE TICKS WAIT TARGET IRQ\n");
  uint32_t capacity = sys_task_capacity();
  for (uint32_t i = 0; i < capacity; i++) {
    task_info_t task = sys_ps(i);
    if (!task.present) {
      continue;
    }
    printf("%u   %u    %u   %s   %u   %u    %u     %u    %u      %u\n",
           task.tid, task.parent_tid, task.core_id, task_state_name(task.state),
           (uint32_t)task.priority, (uint32_t)task.base_priority,
           task.ticks_remaining, task.wait_time,
           task.ipc_target_tid, task.awaiting_irq);
  }
}

static void cmd_jobs(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  print_jobs();
}

static void cmd_kill(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("kill"));
    return;
  }
  int ok = 0;
  uint32_t tid = _parse_u32(argv[1], &ok);
  if (!ok) {
    print_usage(find_command("kill"));
  } else if (sys_kill(tid) < 0) {
    printf("Error: could not kill TID %u\n", tid);
  } else {
    printf("Killed TID %u\n", tid);
  }
}

static void cmd_wait(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("wait"));
    return;
  }
  int ok = 0;
  uint32_t tid = _parse_u32(argv[1], &ok);
  if (!ok) {
    print_usage(find_command("wait"));
    return;
  }
  wait_info_t info = sys_wait(tid);
  print_wait_result(info);
  if (info.status == WAIT_STATUS_DONE) {
    remove_job(tid);
  }
}

static void cmd_poll(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("poll"));
    return;
  }
  int ok = 0;
  uint32_t tid = _parse_u32(argv[1], &ok);
  if (!ok) {
    print_usage(find_command("poll"));
  } else {
    print_poll_result(sys_poll(tid));
  }
}

static void cmd_mem(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  memory_info_t mem = sys_mem();
  print_kib_line("Total RAM", mem.total_memory);
  print_kib_line("Free RAM", mem.free_memory);
  print_kib_line("Kernel heap size", mem.heap_size);
  print_kib_line("Kernel heap used", mem.heap_used);
  print_kib_line("Kernel heap mapped", mem.heap_mapped);
}

static void cmd_vmmap(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  print_vmmap();
}

static void cmd_capstat(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  print_capstat();
}

static void cmd_debug(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  print_debug_info();
}

static void cmd_ipcstat(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  ipc_profile_t p = sys_ipc_profile();
  uint64_t completed = p.replies ? p.replies : 1;
  uint64_t direct = p.direct_calls ? p.direct_calls : 1;
  uint64_t lookup_ticks = p.calls ? p.cap_lookup_cycles / p.calls : 0;
  uint64_t roundtrip_ticks = p.total_roundtrip_cycles / completed;
  uint64_t direct_ticks = p.direct_roundtrip_cycles / direct;
  uint64_t hz = p.counter_hz ? p.counter_hz : 1;
  printf("ipc: calls=%lu direct=%lu slow=%lu replies=%lu\n",
         p.calls, p.direct_calls, p.slow_calls, p.replies);
  printf("ipc: avg-cap-lookup=%lu ticks %lu ns\n",
         lookup_ticks, lookup_ticks * 1000000000ULL / hz);
  if (p.total_roundtrip_cycles != 0) {
    printf("ipc: avg-roundtrip=%lu ticks %lu ns\n",
           roundtrip_ticks, roundtrip_ticks * 1000000000ULL / hz);
    printf("ipc: avg-direct-roundtrip=%lu ticks %lu ns\n",
           direct_ticks, direct_ticks * 1000000000ULL / hz);
  } else {
    printf("ipc: cycle timing is disabled in the hot path\n");
  }
}

static void print_color_sample(uint32_t color, const char *name) {
  printf("\x1b[%um%s\x1b[0m ", 30 + color, name);
}

static void print_bright_color_sample(uint32_t color, const char *name) {
  printf("\x1b[%um%s\x1b[0m ", 90 + color, name);
}

static void cmd_termtest(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  printf("\x1b[2J\x1b[H");
  printf("Neptune terminal visual test\n");
  printf("============================\n\n");

  printf("Font samples:\n");
  printf("  ABCDEFGHIJKLMNOPQRSTUVWXYZ\n");
  printf("  abcdefghijklmnopqrstuvwxyz\n");
  printf("  0123456789\n");
  printf("  !\"#$%%&'()*+,-./:;<=>?@[\\]^_`{|}~\n\n");

  printf("ANSI colors:\n");
  print_color_sample(0, "black");
  print_color_sample(1, "red");
  print_color_sample(2, "green");
  print_color_sample(3, "yellow");
  print_color_sample(4, "blue");
  print_color_sample(5, "magenta");
  print_color_sample(6, "cyan");
  print_color_sample(7, "white");
  printf("\n");
  print_bright_color_sample(0, "gray");
  print_bright_color_sample(1, "bright-red");
  print_bright_color_sample(2, "bright-green");
  print_bright_color_sample(3, "bright-yellow");
  print_bright_color_sample(4, "bright-blue");
  print_bright_color_sample(5, "bright-magenta");
  print_bright_color_sample(6, "bright-cyan");
  print_bright_color_sample(7, "bright-white");
  printf("\n\n");

  printf("Cursor movement: start");
  printf("\x1b[5D#####");
  printf(" ok\n");

  printf("Wrapping:\n");
  printf("  |----10----|----20----|----30----|----40----|----50----|----60----|----70----|----80----|----90----|---100---|---110---|---120---|---130---|---140---|---150---|---160---|---170---|---180---|\n");
  printf("  The quick brown fox jumps over the lazy dog. 0123456789. This long line should wrap cleanly at the terminal edge without smearing stale pixels.\n\n");

  printf("Scrollback fill follows. Use PageUp/PageDown, Ctrl+Home, Ctrl+End.\n");
  for (uint32_t i = 0; i < 48; i++) {
    printf("  scrollback line %02u  AaBbCc 12345 !?[]{} <> /\\ _-| color=", i);
    print_bright_color_sample(i % 8, "sample");
    printf("\n");
  }
  printf("\ntermtest: done\n");
  terminal_flush();
}

static const char *display_backend_name(uint64_t backend) {
  switch (backend) {
    case DISPLAY_BACKEND_RAMFB: return "ramfb";
    case DISPLAY_BACKEND_VIRTIO_GPU: return "virtio-gpu";
    default: return "none";
  }
}

static void print_display_features(uint64_t features) {
  int any = 0;
  if (features & DISPLAY_FEATURE_FILL_RECT) {
    printf("fill"); any = 1;
  }
  if (features & DISPLAY_FEATURE_REGISTER_BUFFER) {
    printf("%sregister-buffer", any ? "," : ""); any = 1;
  }
  if (features & DISPLAY_FEATURE_BLIT_BUFFER) {
    printf("%sblit-buffer", any ? "," : ""); any = 1;
  }
  if (features & DISPLAY_FEATURE_CPU_RGB888) {
    printf("%scpu-rgb888", any ? "," : ""); any = 1;
  }
  if (features & DISPLAY_FEATURE_SYNC_PRESENT) {
    printf("%ssync-present", any ? "," : ""); any = 1;
  }
  if (features & DISPLAY_FEATURE_PRESENT_QUEUE) {
    printf("%spresent-queue", any ? "," : ""); any = 1;
  }
  if (!any) {
    printf("none");
  }
}

static void cmd_gfx(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  int display_cap = ns_resolve("display");
  if (display_cap < 0) {
    printf("gfx: display service unavailable\n");
    return;
  }

  uint64_t request[IPC_INLINE_WORDS] = {DISPLAY_REQ_INFO};
  ipc_msg_t info = sys_ipc_call((uint32_t)display_cap, 0, 8, request);
  if (info.status < 0) {
    printf("gfx: no graphical framebuffer available\n");
    uint64_t stats_request[IPC_INLINE_WORDS] = {DISPLAY_REQ_STATS};
    ipc_msg_t stats = sys_ipc_call((uint32_t)display_cap, 0, 8, stats_request);
    if (stats.status == 0) {
      printf("gfx: backend=%s gpu-init-error=%lu display-errors=%lu\n",
             display_backend_name(stats.payload[4]), stats.payload[5],
             stats.payload[3]);
    }
    return;
  }

  uint32_t width = (uint32_t)(info.payload[0] >> 32);
  uint32_t height = (uint32_t)info.payload[0];
  uint32_t stride = (uint32_t)(info.payload[1] >> 32);
  uint32_t format = (uint32_t)info.payload[1];
  printf("gfx: %ux%u stride=%u format=%u bytes=%lu\n",
         width, height, stride, format, info.payload[2]);

  uint64_t caps_request[IPC_INLINE_WORDS] = {DISPLAY_REQ_CAPS};
  ipc_msg_t caps = sys_ipc_call((uint32_t)display_cap, 0, 8, caps_request);
  if (caps.status == 0) {
    printf("gfx: backend=%s max-buffers=%lu features=",
           display_backend_name(caps.payload[0]), caps.payload[2]);
    print_display_features(caps.payload[1]);
    printf("\n");
  }

  uint64_t stats_request[IPC_INLINE_WORDS] = {DISPLAY_REQ_STATS};
  ipc_msg_t stats = sys_ipc_call((uint32_t)display_cap, 0, 8, stats_request);
  if (stats.status == 0) {
    printf("gfx: display-stats fills=%lu registers=%lu blits=%lu errors=%lu gpu-init-error=%lu\n",
           stats.payload[0], stats.payload[1], stats.payload[2],
           stats.payload[3], stats.payload[5]);
    printf("gfx: display present-queue=%lu\n", stats.payload[14]);
    if (stats.payload[4] == DISPLAY_BACKEND_VIRTIO_GPU) {
      printf("gfx: gpu commands=%lu transfers=%lu flushes=%lu bytes=%lu\n",
             stats.payload[6], stats.payload[7], stats.payload[8],
             stats.payload[9]);
      printf("gfx: gpu queued=%lu merged=%lu batches=%lu max-dirty=%lu\n",
             stats.payload[10], stats.payload[11], stats.payload[12],
             stats.payload[13]);
    }
  }

  request[0] = DISPLAY_REQ_CLEAR;
  request[1] = 0x071A2B;
  sys_ipc_call((uint32_t)display_cap, 0, 16, request);

  static const uint32_t colors[5] = {
    0x12B8A6, 0xF4C95D, 0xEF6F6C, 0x5DADE2, 0xF4F7FA
  };
  uint32_t margin = width / 12;
  uint32_t gap = width / 80;
  uint32_t bar_width = (width - margin * 2 - gap * 4) / 5;
  uint32_t bar_height = height / 3;
  uint32_t y = (height - bar_height) / 2;
  for (uint32_t i = 0; i < 5; i++) {
    request[0] = DISPLAY_REQ_FILL_RECT;
    request[1] = display_pack_pair(margin + i * (bar_width + gap), y);
    request[2] = display_pack_pair(bar_width, bar_height);
    request[3] = colors[i];
    sys_ipc_call((uint32_t)display_cap, 0, 32, request);
  }
}

static void cmd_gfxstat(int argc, char **argv, char *line) {
  (void)line;
  int compositor_cap = ns_resolve("compositor");
  if (compositor_cap < 0) {
    printf("gfxstat: compositor unavailable\n");
    return;
  }

  uint64_t request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_STATS};
  if (argc > 1 && _streq(argv[1], "reset")) {
    request[0] = COMPOSITOR_REQ_RESET_STATS;
    ipc_msg_t reset = sys_ipc_call((uint32_t)compositor_cap, 0, 8, request);
    printf(reset.status < 0 ? "gfxstat: reset failed\n" :
                              "gfxstat: reset compositor counters\n");
    return;
  }

  ipc_msg_t stats = sys_ipc_call((uint32_t)compositor_cap, 0, 8, request);
  if (stats.status < 0) {
    printf("gfxstat: query failed\n");
    return;
  }
  printf("gfxstat: damage=%lu queued=%lu merged=%lu collapses=%lu\n",
         stats.payload[0], stats.payload[1], stats.payload[2],
         stats.payload[3]);
  printf("gfxstat: flushes=%lu presented=%lu max-dirty=%lu full=%lu\n",
         stats.payload[4], stats.payload[5], stats.payload[6],
         stats.payload[7]);
  printf("gfxstat: present-queue-drops=%lu\n", stats.payload[8]);
}

static void print_mouse_stats(const char *prefix, ipc_msg_t stats) {
  if (stats.status < 0) {
    printf("%s: query failed\n", prefix);
    return;
  }
  uint32_t x = (uint32_t)(stats.payload[6] >> 32);
  uint32_t y = (uint32_t)stats.payload[6];
  printf("%s: found=%lu mode=%s reads=%lu raw=%lu queued=%lu dropped=%lu pos=%u,%u buttons=%lu\n",
         prefix, stats.payload[0],
         stats.payload[1] ? "absolute" : "relative",
         stats.payload[2], stats.payload[3], stats.payload[4],
         stats.payload[5], x, y, stats.payload[7]);
}

static void cmd_mouse(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  int mouse_cap = ns_resolve("mouse");
  if (mouse_cap < 0) {
    printf("mouse: service unavailable\n");
    return;
  }

  uint64_t request[IPC_INLINE_WORDS] = {MOUSE_REQ_INFO};
  ipc_msg_t info = sys_ipc_call((uint32_t)mouse_cap, 0, 8, request);
  if (info.status < 0) {
    printf("mouse: info failed\n");
    return;
  }
  printf("mouse: max=%u,%u ready=%lu mode=%s\n",
         (uint32_t)(info.payload[0] >> 32), (uint32_t)info.payload[0],
         info.payload[1], info.payload[2] ? "absolute" : "relative");

  request[0] = MOUSE_REQ_STATS;
  ipc_msg_t before = sys_ipc_call((uint32_t)mouse_cap, 0, 8, request);
  print_mouse_stats("mouse-before", before);
  printf("mouse: move/click now, sampling for 2 seconds...\n");
  for (uint32_t i = 0; i < 20; i++) {
    uint64_t read_request[IPC_INLINE_WORDS] = {MOUSE_REQ_READ};
    (void)sys_ipc_call((uint32_t)mouse_cap, 0, 8, read_request);
    sys_sleep(100);
  }
  ipc_msg_t after = sys_ipc_call((uint32_t)mouse_cap, 0, 8, request);
  print_mouse_stats("mouse-after", after);
}

typedef struct {
  uint32_t *pixels;
  uint64_t size;
  int mem_cap;
  uint32_t surface_id;
} desktop_surface_t;

static desktop_surface_t desktop_surfaces[3];

static uint64_t page_round(uint64_t size) {
  return (size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
}

static void desktop_fill(uint32_t *pixels, uint32_t stride,
                         uint32_t x, uint32_t y, uint32_t width, uint32_t height,
                         uint32_t color) {
  for (uint32_t row = 0; row < height; row++) {
    for (uint32_t col = 0; col < width; col++) {
      pixels[(uint64_t)(y + row) * stride + x + col] = color;
    }
  }
}

static void desktop_destroy_old(int compositor_cap) {
  for (uint32_t i = 0; i < 3; i++) {
    desktop_surface_t *surface = &desktop_surfaces[i];
    if (!surface->surface_id) {
      continue;
    }
    uint64_t payload[IPC_INLINE_WORDS] = {
      COMPOSITOR_REQ_DESTROY_SURFACE, surface->surface_id
    };
    (void)sys_ipc_call((uint32_t)compositor_cap, 0, 16, payload);
    (void)sys_mem_revoke((uint32_t)surface->mem_cap);
    (void)sys_munmap(surface->pixels, surface->size);
    surface->surface_id = 0;
    surface->pixels = 0;
    surface->size = 0;
    surface->mem_cap = -1;
  }
}

static int desktop_create_surface(int compositor_cap, desktop_surface_t *surface,
                                  uint32_t width, uint32_t height,
                                  uint32_t x, uint32_t y,
                                  uint32_t body, uint32_t accent) {
  surface->size = page_round((uint64_t)width * height * sizeof(uint32_t));
  surface->pixels = (uint32_t *)sys_mmap(surface->size);
  if (!surface->pixels) {
    return -1;
  }
  desktop_fill(surface->pixels, width, 0, 0, width, height, body);
  desktop_fill(surface->pixels, width, 0, 0, width, height / 8, accent);
  desktop_fill(surface->pixels, width, width / 14, height / 4,
               width * 6 / 7, 3, accent);
  desktop_fill(surface->pixels, width, width / 14, height / 2,
               width * 4 / 7, 3, accent);
  desktop_fill(surface->pixels, width, width / 14, height * 3 / 4,
               width * 5 / 7, 3, accent);

  surface->mem_cap = sys_mem_export(surface->pixels, surface->size,
                                    MEM_RIGHT_READ | MEM_RIGHT_WRITE |
                                    MEM_RIGHT_LEND);
  if (surface->mem_cap < 0) {
    sys_munmap(surface->pixels, surface->size);
    return -1;
  }
  uint64_t payload[IPC_INLINE_WORDS] = {
    [0] = (uint64_t)surface->mem_cap,
    [1] = 0,
    [2] = surface->size,
    [3] = MEM_RIGHT_READ | IPC_MEM_MODE_LEND,
    [4] = COMPOSITOR_REQ_CREATE_SURFACE,
    [5] = display_pack_pair(width, height),
    [6] = display_pack_pair(x, y),
    [7] = width
  };
  ipc_msg_t created =
      sys_ipc_call((uint32_t)compositor_cap, IPC_FLAG_MEM, 64, payload);
  if (created.status < 0 || created.payload[0] == 0) {
    sys_mem_revoke((uint32_t)surface->mem_cap);
    sys_munmap(surface->pixels, surface->size);
    return -1;
  }
  surface->surface_id = (uint32_t)created.payload[0];
  return 0;
}

static void cmd_desktop(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  int compositor_cap = ns_resolve("compositor");
  if (compositor_cap < 0) {
    printf("desktop: compositor unavailable\n");
    return;
  }
  uint64_t request[IPC_INLINE_WORDS] = {COMPOSITOR_REQ_INFO};
  ipc_msg_t info = sys_ipc_call((uint32_t)compositor_cap, 0, 8, request);
  if (info.status < 0) {
    printf("desktop: graphical display unavailable\n");
    return;
  }
  uint32_t screen_width = (uint32_t)(info.payload[0] >> 32);
  uint32_t screen_height = (uint32_t)info.payload[0];
  desktop_destroy_old(compositor_cap);

  uint32_t width = screen_width / 3;
  uint32_t height = screen_height / 3;
  int ok = 0;
  ok |= desktop_create_surface(compositor_cap, &desktop_surfaces[0],
                               width, height, screen_width / 12,
                               screen_height / 8, 0x153349, 0x12B8A6);
  ok |= desktop_create_surface(compositor_cap, &desktop_surfaces[1],
                               width, height, screen_width / 3,
                               screen_height / 3, 0x2B2945, 0xF4C95D);
  ok |= desktop_create_surface(compositor_cap, &desktop_surfaces[2],
                               width, height, screen_width * 7 / 12,
                               screen_height / 5, 0x3D2636, 0xEF6F6C);
  printf(ok == 0 ? "desktop: composed three shared surfaces\n" :
                   "desktop: one or more surfaces failed\n");
}

static void cmd_gfxdemo(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t tid = spawn_program(GFXDEMO_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("gfxdemo: could not spawn %s\n", GFXDEMO_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_gfxbench(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t tid = spawn_program(GFXBENCH_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("gfxbench: could not spawn %s\n", GFXBENCH_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_snake(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t tid = spawn_program(SNAKE_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("snake: could not spawn %s\n", SNAKE_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_libctest(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t tid = spawn_program(LIBCTEST_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("libctest: could not spawn %s\n", LIBCTEST_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_argfdtest(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  char *child_argv[] = {ARGFDTEST_FILE, "alpha", "beta"};
  uint32_t tid = spawn_program_args(ARGFDTEST_FILE, PONG_PRIORITY,
                                    child_argv, 3);
  if ((int)tid < 0) {
    printf("argfdtest: could not spawn %s\n", ARGFDTEST_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_pipetest(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t tid = spawn_program(PIPETEST_FILE, PONG_PRIORITY);
  if ((int)tid < 0) {
    printf("pipetest: could not spawn %s\n", PIPETEST_FILE);
    return;
  }
  print_wait_result(sys_wait(tid));
}

static void cmd_tcc(int argc, char **argv, char *line) {
  (void)line;
  if (argc > 8) {
    printf("tcc: at most 7 arguments are supported\n");
    return;
  }
  char *child_argv[8];
  child_argv[0] = TCC_FILE;
  for (int i = 1; i < argc; i++) {
    child_argv[i] = argv[i];
  }
  uint32_t tid = spawn_program_args(TCC_FILE, PONG_PRIORITY,
                                    child_argv, (uint32_t)argc);
  if ((int)tid < 0) {
    printf("tcc: could not spawn %s\n", TCC_FILE);
    return;
  }
  wait_info_t result = sys_wait(tid);
  if (result.reason != TASK_TERM_EXITED || result.exit_code != 0) {
    print_wait_result(result);
  }
}

static void cmd_pong(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  printf("Spawning pong...\n");
  uint32_t pong_tid = spawn_program(PONG_FILE, PONG_PRIORITY);
  if ((int)pong_tid < 0) {
    printf("Error: Could not spawn pong (task limit reached?)\n");
  } else {
    printf("Pong is TID %d. Exchanging 5 messages...\n", (int)pong_tid);
    if (run_pong_exchange(pong_tid) < 0) {
      printf("Error: pong IPC failed\n");
    } else {
      print_wait_result(sys_wait(pong_tid));
    }
  }
}

static void cmd_fault(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  printf("Spawning fault test...\n");
  uint32_t fault_tid = spawn_program(FAULT_FILE, PONG_PRIORITY);
  if ((int)fault_tid < 0) {
    printf("Error: Could not spawn fault test (task limit reached?)\n");
  } else {
    printf("Fault test is TID %d. It should be killed by the kernel.\n",
           (int)fault_tid);
    print_wait_result(sys_wait(fault_tid));
  }
}

static void cmd_badptr(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  uint32_t badptr_tid = spawn_program(BADPTR_FILE, PONG_PRIORITY);
  if ((int)badptr_tid < 0) {
    printf("Error: Could not spawn badptr test\n");
  } else {
    printf("Bad pointer test is TID %d.\n", (int)badptr_tid);
    print_wait_result(sys_wait(badptr_tid));
  }
}

static void cmd_smoke(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_smoke_demo();
}

static void cmd_vfstest(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfstest_demo();
}

static void cmd_vfsinject(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfsinject_demo();
}

static void cmd_vfsls(int argc, char **argv, char *line) {
  (void)line;
  if (argc > 2) {
    print_usage(find_command("vfsls"));
    return;
  }
  list_files_vfs(argc == 2 ? argv[1] : "");
}

static void cmd_vfsopen(int argc, char **argv, char *line) {
  (void)line;
  if (argc != 2) {
    print_usage(find_command("vfsopen"));
    return;
  }
  run_vfsopen_command(argv[1]);
}

static void cmd_vfsread(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfsread_demo();
}

static void cmd_vfswrite(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfswrite_demo();
}

static void cmd_vfsinstall(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfsinstall_demo();
}

static void cmd_vfsexec(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vfsexec_demo();
}

static void cmd_ipcfast(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_ipcfast_demo();
}

static void cmd_ipccap(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_ipccap_demo();
}

static void cmd_ipckill(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_ipckill_demo();
}

static void cmd_memshare(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_memshare_demo();
}

static void cmd_memxfer(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_memxfer_demo();
}

static void cmd_memcaplife(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_memcaplife_demo();
}

static void cmd_memrevoke(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_memrevoke_demo();
}

static void cmd_munmap(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_munmap_demo();
}

static void cmd_forkcow(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_forkcow_demo();
}

static void cmd_filelazy(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_filelazy_demo();
}

static void cmd_vmstress(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_vmstress_demo();
}

static void cmd_lazyexec(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_lazyexec_demo();
}

static void cmd_taskstress(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
  run_taskstress_demo();
}

static void cmd_speed(int argc, char **argv, char *line) {
  (void)argc; (void)argv; (void)line;
#if NEPTUNE_PLATFORM_PI4
  spawn_result_t speed = sys_spawn_file2(SPEED_FILE, PONG_PRIORITY);
  if ((int)speed.tid < 0) {
    printf("Error: Could not spawn speed benchmark\n");
  } else {
    print_wait_result(sys_wait(speed.tid));
  }
#else
  uint32_t speed_tid = spawn_program(SPEED_FILE, PONG_PRIORITY);
  if ((int)speed_tid < 0) {
    printf("Error: Could not spawn speed benchmark\n");
  } else {
    print_wait_result(sys_wait(speed_tid));
  }
#endif
}

static void print_demo_suites(void) {
  printf("Demo suites:\n");
  printf("  demo quick    Core smoke path\n");
  printf("  demo vfs      VFS call, injection, read, write, install, exec\n");
  printf("  demo ipc      Register IPC and cap transfer\n");
  printf("  demo mem      Share, transfer, cap lifetime, revoke, unmap, COW\n");
  printf("  demo proc     Fault isolation, stack growth, task lifecycle stress\n");
  printf("  demo all      Run every suite above\n");
}

static void run_demo_quick(void) {
  run_smoke_demo();
}

static void run_demo_vfs(void) {
  printf("demo vfs: starting\n");
  list_files_vfs("");
  run_vfstest_demo();
  run_vfsinject_demo();
  run_vfsread_demo();
  run_vfswrite_demo();
  run_vfsinstall_demo();
  run_vfsexec_demo();
  printf("demo vfs: done\n");
}

static void run_demo_ipc(void) {
  printf("demo ipc: starting\n");
  run_ipcfast_demo();
  run_ipccap_demo();
  printf("demo ipc: done\n");
}

static void run_demo_mem(void) {
  printf("demo mem: starting\n");
  run_memshare_demo();
  run_memxfer_demo();
  run_memcaplife_demo();
  run_memrevoke_demo();
  run_munmap_demo();
  run_forkcow_demo();
  printf("demo mem: done\n");
}

static void run_demo_proc(void) {
  printf("demo proc: starting\n");
  (void)run_and_expect_exit(BADPTR_FILE);
  (void)run_and_expect_exit(STACKGROW_FILE);
  run_taskstress_demo();
  printf("demo proc: done\n");
}

static void cmd_demo(int argc, char **argv, char *line) {
  (void)line;
  if (argc == 1) {
    print_demo_suites();
    return;
  }
  if (argc != 2) {
    print_usage(find_command("demo"));
    return;
  }

  if (_streq(argv[1], "quick")) {
    run_demo_quick();
  } else if (_streq(argv[1], "vfs")) {
    run_demo_vfs();
  } else if (_streq(argv[1], "ipc")) {
    run_demo_ipc();
  } else if (_streq(argv[1], "mem")) {
    run_demo_mem();
  } else if (_streq(argv[1], "proc")) {
    run_demo_proc();
  } else if (_streq(argv[1], "all")) {
    run_demo_quick();
    run_demo_vfs();
    run_demo_ipc();
    run_demo_mem();
    run_demo_proc();
  } else {
    print_demo_suites();
  }
}

#define PIPELINE_MAX_STAGES 6

static char *trim_stage(char *text) {
  while (*text == ' ' || *text == '\t') text++;
  char *end = text;
  while (*end) end++;
  while (end > text && (end[-1] == ' ' || end[-1] == '\t')) end--;
  *end = '\0';
  return text;
}

static spawn_result_t spawn_pipeline_stage(char *text, uint32_t stdin_cap,
                                           uint32_t stdout_cap) {
  spawn_result_t fail = {.tid = (uint32_t)-1, .endpoint_cap = -1};
  char *argv[8];
  int argc = parse_args(text, argv, 8);
  if (argc == 0) return fail;

  char filename[64];
  char **child_argv = argv;
  uint32_t child_argc = (uint32_t)argc;
  if (_streq(argv[0], "run")) {
    if (argc < 2) return fail;
    child_argv = &argv[1];
    child_argc--;
    shell_copy(filename, argv[1], sizeof(filename));
  } else {
    int has_suffix = 0;
    for (const char *p = argv[0]; *p; p++) {
      if (*p == '.') has_suffix = 1;
    }
    if (has_suffix) shell_copy(filename, argv[0], sizeof(filename));
    else snprintf(filename, sizeof(filename), "%s.elf", argv[0]);
  }
  return vfs_spawn_program_stdio(filename, PONG_PRIORITY, child_argv,
                                 child_argc, stdin_cap, stdout_cap, 0);
}

static void dispatch_pipeline(char *line) {
  char *stages[PIPELINE_MAX_STAGES];
  char input_stage[96];
  char output_stage[96];
  uint32_t stage_count = 1;
  stages[0] = line;
  for (char *p = line; *p; p++) {
    if (*p != '|') continue;
    if (stage_count == PIPELINE_MAX_STAGES) {
      printf("pipeline: at most %u stages are supported\n", PIPELINE_MAX_STAGES);
      return;
    }
    *p = '\0';
    stages[stage_count++] = p + 1;
  }
  for (uint32_t i = 0; i < stage_count; i++) {
    stages[i] = trim_stage(stages[i]);
    if (!stages[i][0]) {
      printf("pipeline: empty stage\n");
      return;
    }
  }


  char *input_path = 0;
  for (char *p = stages[0]; *p; p++) {
    if (*p == '<') {
      *p = '\0';
      input_path = trim_stage(p + 1);
      stages[0] = trim_stage(stages[0]);
      break;
    }
  }
  char *output_path = 0;
  for (char *p = stages[stage_count - 1]; *p; p++) {
    if (*p == '>') {
      *p = '\0';
      output_path = trim_stage(p + 1);
      stages[stage_count - 1] = trim_stage(stages[stage_count - 1]);
      break;
    }
  }
  if ((input_path && (!input_path[0] || !stages[0][0])) ||
      (output_path && (!output_path[0] || !stages[stage_count - 1][0]))) {
    printf("redirection: command and filename are required\n");
    return;
  }
  if (input_path) {
    if (stage_count == PIPELINE_MAX_STAGES) {
      printf("pipeline: too many stages after input redirection\n");
      return;
    }
    for (uint32_t i = stage_count; i > 0; i--) stages[i] = stages[i - 1];
    snprintf(input_stage, sizeof(input_stage), "cat %s", input_path);
    stages[0] = input_stage;
    stage_count++;
  }
  if (output_path) {
    if (stage_count == PIPELINE_MAX_STAGES) {
      printf("pipeline: too many stages after output redirection\n");
      return;
    }
    snprintf(output_stage, sizeof(output_stage), "writefile %s", output_path);
    stages[stage_count++] = output_stage;
  }

  pipe_cap_pair_t links[PIPELINE_MAX_STAGES - 1];
  uint32_t created = 0;
  for (; created + 1 < stage_count; created++) {
    links[created] = sys_pipe_create();
    if (links[created].read_cap < 0 || links[created].write_cap < 0) {
      printf("pipeline: cannot create pipe\n");
      break;
    }
  }
  if (created + 1 != stage_count) {
    for (uint32_t i = 0; i < created; i++) {
      sys_pipe_close((uint32_t)links[i].read_cap);
      sys_pipe_close((uint32_t)links[i].write_cap);
    }
    return;
  }

  uint32_t tids[PIPELINE_MAX_STAGES];
  uint32_t spawned = 0;
  for (; spawned < stage_count; spawned++) {
    uint32_t input = spawned == 0 ? 0 : (uint32_t)links[spawned - 1].read_cap;
    uint32_t output = spawned + 1 == stage_count
                          ? 0 : (uint32_t)links[spawned].write_cap;
    spawn_result_t result = spawn_pipeline_stage(stages[spawned], input, output);
    if (result.tid == (uint32_t)-1) {
      printf("pipeline: cannot start stage %u\n", spawned + 1);
      break;
    }
    tids[spawned] = result.tid;
  }

  for (uint32_t i = 0; i + 1 < stage_count; i++) {
    sys_pipe_close((uint32_t)links[i].read_cap);
    sys_pipe_close((uint32_t)links[i].write_cap);
  }
  for (uint32_t i = 0; i < spawned; i++) {
    wait_info_t status = sys_wait(tids[i]);
    if (status.status != WAIT_STATUS_DONE || status.reason != TASK_TERM_EXITED ||
        status.exit_code != 0) {
      print_wait_result(status);
    }
  }
}

static void dispatch_command(char *line) {
  for (char *p = line; *p; p++) {
    if (*p == '|' || *p == '<' || *p == '>') {
      dispatch_pipeline(line);
      return;
    }
  }
  char original[MAX_CMD];
  char *argv[8];
  shell_copy(original, line, sizeof(original));
  int argc = parse_args(line, argv, 8);
  if (argc == 0) {
    return;
  }

  const shell_command_t *cmd = find_command(argv[0]);
  if (!cmd) {
    printf("Unknown command: '%s'\n", argv[0]);
    printf("Type 'help' for grouped commands, or 'help <command>'.\n");
    return;
  }

  if (!command_accepts_args(cmd) && argc != 1) {
    print_usage(cmd);
    return;
  }

  cmd->handler(argc, argv, original);
}

static char history[SHELL_HISTORY][MAX_CMD];
static int history_start;
static int history_len;

static const char *history_get(int index) {
  return history[(history_start + index) % SHELL_HISTORY];
}

static void history_add(const char *cmd) {
  if (cmd[0] == '\0') {
    return;
  }
  if (history_len > 0 && _streq(history_get(history_len - 1), cmd)) {
    return;
  }

  int slot;
  if (history_len < SHELL_HISTORY) {
    slot = (history_start + history_len) % SHELL_HISTORY;
    history_len++;
  } else {
    slot = history_start;
    history_start = (history_start + 1) % SHELL_HISTORY;
  }
  shell_copy(history[slot], cmd, MAX_CMD);
}

static void redraw_input(const char *cmd, int len, int cursor) {
  printf("\r\x1b[2K%s%s", SHELL_PROMPT, cmd);
  if (cursor < len) {
    printf("\x1b[%uD", (uint32_t)(len - cursor));
  }
  terminal_commit();
}

static void set_input_from_history(char *cmd, int *len, int *cursor,
                                   const char *history_line) {
  shell_copy(cmd, history_line, MAX_CMD);
  *len = 0;
  while (cmd[*len]) (*len)++;
  *cursor = *len;
  redraw_input(cmd, *len, *cursor);
}

static void delete_input_range(char *cmd, int *len, int cursor, int count) {
  if (count <= 0 || cursor >= *len) {
    return;
  }
  if (count > *len - cursor) {
    count = *len - cursor;
  }
  for (int i = cursor; i + count <= *len; i++) {
    cmd[i] = cmd[i + count];
  }
  *len -= count;
}

static void insert_input_char(char *cmd, int *len, int *cursor, char c) {
  if (*len >= MAX_CMD - 1) {
    return;
  }
  for (int i = *len; i >= *cursor; i--) {
    cmd[i + 1] = cmd[i];
  }
  cmd[*cursor] = c;
  (*len)++;
  (*cursor)++;
}

static void print_prompt(void) {
  printf("%s", SHELL_PROMPT);
  terminal_commit();
}

void _start(void) {
  char cmd[MAX_CMD];
  int len = 0;
  int cursor = 0;
  int history_cursor = -1;

  while (ns_register("shell") < 0) {
    sys_sleep(10);
  }
  graphical_terminal = terminal_enable() == 0;

  printf("  _   _            _                     __   ___  \n");
  printf(" | \\ | | ___ _ __ | |_ _   _ _ __   ___ / _ \\/ ___| \n");
  printf(" |  \\| |/ _ \\ '_ \\| __| | | | '_ \\ / _ \\ | | \\___ \\ \n");
  printf(" | |\\  |  __/ |_) | |_| |_| | | | |  __/ |_| |___) |\n");
  printf(" |_| \\_|\\___| .__/ \\__|\\__,_|_| |_|\\___|\\___/|____/ \n");
  printf("            |_|    AArch64 Microkernel Shell v0.2\n\n");
  printf("Type 'help' for available commands.\n");

  cmd[0] = '\0';
  print_prompt();

  while (1) {
    char c = read_console_char();

    if (c == '\r' || c == '\n') {
      printf("\n");
      terminal_flush();
      cmd[len] = '\0';
      history_add(cmd);
      dispatch_command(cmd);
      len = 0;
      cursor = 0;
      cmd[0] = '\0';
      history_cursor = -1;
      print_prompt();
    } else if (c == 3) {
      printf("^C\n");
      len = 0;
      cursor = 0;
      cmd[0] = '\0';
      history_cursor = -1;
      print_prompt();
    } else if (c == 12) {
      printf("\x1b[2J\x1b[H");
      redraw_input(cmd, len, cursor);
    } else if (c == 21) {
      len = 0;
      cursor = 0;
      cmd[0] = '\0';
      history_cursor = -1;
      redraw_input(cmd, len, cursor);
    } else if (c == 27) {
      char c1 = read_console_char();
      if (c1 == '[') {
        char c2 = read_console_char();
        int value = 0;
        int saw_value = 0;
        while (c2 >= '0' && c2 <= '9') {
          saw_value = 1;
          value = value * 10 + (c2 - '0');
          c2 = read_console_char();
        }
        if (c2 == 'A' && history_len > 0) {
          if (history_cursor < 0) {
            history_cursor = history_len - 1;
          } else if (history_cursor > 0) {
            history_cursor--;
          }
          set_input_from_history(cmd, &len, &cursor,
                                 history_get(history_cursor));
        } else if (c2 == 'B' && history_len > 0) {
          if (history_cursor >= 0 && history_cursor < history_len - 1) {
            history_cursor++;
            set_input_from_history(cmd, &len, &cursor,
                                   history_get(history_cursor));
          } else {
            history_cursor = -1;
            len = 0;
            cursor = 0;
            cmd[0] = '\0';
            redraw_input(cmd, len, cursor);
          }
        } else if (c2 == 'C') {
          if (cursor < len) {
            cursor++;
            printf("\x1b[1C");
            terminal_commit();
          }
        } else if (c2 == 'D') {
          if (cursor > 0) {
            cursor--;
            printf("\x1b[1D");
            terminal_commit();
          }
        } else if (c2 == 'H') {
          cursor = 0;
          redraw_input(cmd, len, cursor);
        } else if (c2 == 'F') {
          cursor = len;
          redraw_input(cmd, len, cursor);
        } else if (c2 == '~') {
          if (saw_value && value == 3) {
            delete_input_range(cmd, &len, cursor, 1);
            history_cursor = -1;
            redraw_input(cmd, len, cursor);
          } else if (saw_value && (value == 1 || value == 7)) {
            cursor = 0;
            redraw_input(cmd, len, cursor);
          } else if (saw_value && (value == 4 || value == 8)) {
            cursor = len;
            redraw_input(cmd, len, cursor);
          }
        }
      }
    } else if (c == 127 || c == '\b') {
      if (cursor > 0) {
        int erase_tail = cursor == len;
        cursor--;
        delete_input_range(cmd, &len, cursor, 1);
        history_cursor = -1;
        if (erase_tail) {
          printf("\b \b");
          terminal_commit();
        } else {
          redraw_input(cmd, len, cursor);
        }
      }
    } else if (c >= 32 && c < 127 && len < MAX_CMD - 1) {
      int append = cursor == len;
      insert_input_char(cmd, &len, &cursor, c);
      history_cursor = -1;
      if (append) {
        printf("%c", c);
        terminal_commit();
      } else {
        redraw_input(cmd, len, cursor);
      }
    }
  }
}
