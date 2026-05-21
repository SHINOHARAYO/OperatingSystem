#include "lib.h"
#include "malloc.h"
#include "fs_proto.h"
#include "ns_proto.h"

#define MAX_CMD 128
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
#define PAGE_BYTES 4096ULL

#define IPCKILL_SERVER_HOLD        1
#define IPCKILL_SERVER_DELAY_REPLY 2
#define IPCKILL_CALLER_CALL        3
#define IPCKILL_RECV_WAIT          4

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
static int fs_cap = -1;
static int last_spawn_endpoint_cap = -1;

static void print_wait_result(wait_info_t info);
static uint32_t spawn_program(const char *name, uint8_t priority);
static int open_file_cap(const char *name);
static wait_info_t poll_until_done(uint32_t tid, uint32_t attempts, uint64_t sleep_ms);

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

static int _starts_with(const char *buf, const char *prefix) {
  while (*prefix) {
    if (*buf != *prefix)
      return 0;
    buf++;
    prefix++;
  }
  return 1;
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
  printf("  scheduler: CPU0 O(1) MLFQ, %u Hz tick\n", info.tick_hz);
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

static void run_lazy_memory_check(void) {
  char *name = (char *)malloc(64);
  if (!name) {
    printf("lazy: malloc failed\n");
    return;
  }

  int file_cap = open_file_cap("shell.elf");
  int64_t size = file_cap < 0 ? -1 : sys_file_stat((uint32_t)file_cap, name, 64);
  if (size < 0) {
    printf("lazy: copy_to_user into lazy page failed\n");
    return;
  }

  char *buf = (char *)malloc(8192);
  if (!buf) {
    printf("lazy: second malloc failed\n");
    return;
  }

  buf[0] = 'N';
  buf[4096] = 'P';
  buf[8191] = '!';
  printf("lazy: %s size=%ld touched=%c%c%c\n",
         name, size, buf[0], buf[4096], buf[8191]);
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
  int file_cap = open_file_cap("shell.elf");
  unsigned char *file = file_cap < 0 ? 0 : (unsigned char *)sys_file_mmap((uint32_t)file_cap);
  if (!file) {
    printf("filelazy: mmap failed\n");
    return;
  }

  int ok = file[0] == 0x7f && file[1] == 'E' && file[2] == 'L' && file[3] == 'F';
  printf("filelazy: %s magic=%x %c%c%c\n",
         ok ? "ok" : "failed", (uint32_t)file[0], file[1], file[2], file[3]);
}

static void run_vmstress_demo(void) {
  char name[32];
  int file_cap = open_file_cap("shell.elf");
  int64_t file_size = file_cap < 0 ? -1 : sys_file_stat((uint32_t)file_cap, name, sizeof(name));
  if (file_size <= 0) {
    printf("vmstress: file stat failed\n");
    return;
  }

  uint64_t map_size = ((uint64_t)file_size + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
  void *maps[80];
  uint32_t mapped = 0;
  uint32_t touched = 0;

  for (uint32_t i = 0; i < 80; i++) {
    unsigned char *file = (unsigned char *)sys_file_mmap((uint32_t)file_cap);
    if (!file) {
      break;
    }
    maps[mapped++] = file;
    if (file[0] == 0x7f) {
      touched++;
    }
  }

  for (uint32_t i = 0; i < mapped; i++) {
    sys_munmap(maps[i], map_size);
  }

  printf("vmstress: %s mapped=%u touched=%u\n",
         mapped == 80 && touched == 80 ? "ok" : "failed", mapped, touched);
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
  uint32_t tids[32];
  uint32_t spawned = 0;

  for (uint32_t i = 0; i < 32; i++) {
    uint32_t tid = spawn_program(SPIN_FILE, PONG_PRIORITY);
    if ((int)tid < 0) {
      break;
    }
    tids[spawned++] = tid;
  }

  uint32_t killed = 0;
  for (uint32_t i = 0; i < spawned; i++) {
    if (sys_kill(tids[i]) == 0) {
      wait_info_t info = sys_wait(tids[i]);
      if (info.reason == TASK_TERM_KILLED) {
        killed++;
      }
    }
  }

  printf("taskstress: %s spawned=%u killed=%u capacity=%u\n",
         spawned == 32 && killed == spawned ? "ok" : "failed",
         spawned, killed, sys_task_capacity());
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
  uint32_t pong_tid = spawn_program(PONG_FILE, PONG_PRIORITY);
  int pong_cap = last_spawn_endpoint_cap;
  if ((int)pong_tid < 0 || pong_cap < 0) {
    printf("ipccap: pong spawn failed\n");
    return;
  }

  uint32_t cap_tid = spawn_program(IPCCAP_FILE, PONG_PRIORITY);
  int cap_endpoint = last_spawn_endpoint_cap;
  if ((int)cap_tid < 0 || cap_endpoint < 0) {
    printf("ipccap: receiver spawn failed\n");
    cleanup_blocked_child(pong_tid);
    return;
  }

  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_CAP_WORD_CAP] = (uint64_t)pong_cap,
    [IPC_CAP_WORD_OP] = 0
  };
  ipc_msg_t reply = sys_ipc_call((uint32_t)cap_endpoint, IPC_FLAG_CAP, 16, payload);
  wait_info_t cap_status = sys_wait(cap_tid);
  int ok = reply.status == 0 && reply.payload[0] == 1 &&
           cap_status.reason == TASK_TERM_EXITED && cap_status.exit_code == 0;

  int kill_result = sys_kill(pong_tid);
  wait_info_t pong_poll = poll_until_done(pong_tid, 20, 50);
  printf("ipccap: %s reply=%lu\n", ok ? "ok" : "failed", reply.payload[1]);
  print_wait_result(cap_status);
  if (pong_poll.status == WAIT_STATUS_DONE) {
    print_wait_result(sys_wait(pong_tid));
  } else {
    printf("ipccap: pong cleanup pending kill=%d\n", kill_result);
  }
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
  run_lazy_memory_check();
  run_ipcfast_demo();
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

static int get_fs_cap(void) {
  if (fs_cap < 0) {
    fs_cap = ns_resolve("fs");
  }
  return fs_cap;
}

static void pack_fs_name(uint64_t out[3], const char *name) {
  char *dst = (char *)out;
  out[0] = 0;
  out[1] = 0;
  out[2] = 0;
  for (int i = 0; i < 23 && name[i]; i++) {
    dst[i] = name[i];
  }
}

static int open_file_cap(const char *name) {
  int cap = get_fs_cap();
  if (cap < 0) {
    return -1;
  }

  uint64_t packed[3];
  pack_fs_name(packed, name);
  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_WORD_OP] = FS_REQ_OPEN_FILE,
    [IPC_WORD_ARG0] = packed[0],
    [IPC_WORD_ARG1] = packed[1],
    [IPC_WORD_ARG2] = packed[2]
  };
  ipc_msg_t msg = sys_ipc_call((uint32_t)cap, 0, 32, payload);
  return msg.status < 0 ? -1 : (int)msg.payload[0];
}

static uint32_t spawn_program(const char *name, uint8_t priority) {
  last_spawn_endpoint_cap = -1;
  int cap = get_fs_cap();
  if (cap < 0) {
    return (uint32_t)-1;
  }

  uint64_t packed[3];
  pack_fs_name(packed, name);
  uint64_t payload[IPC_INLINE_WORDS] = {
    [IPC_WORD_OP] = FS_REQ_OPEN_EXEC,
    [IPC_WORD_ARG0] = packed[0],
    [IPC_WORD_ARG1] = packed[1],
    [IPC_WORD_ARG2] = packed[2]
  };
  ipc_msg_t msg = sys_ipc_call((uint32_t)cap, 0, 32, payload);
  int exec_cap = (int)msg.payload[0];
  if (exec_cap < 0) {
    return (uint32_t)-1;
  }

  spawn_result_t spawned = sys_spawn_exec2((uint32_t)exec_cap, priority);
  last_spawn_endpoint_cap = spawned.endpoint_cap;
  return spawned.tid;
}

static void unpack_fs_name(char *name, const ipc_msg_t *msg) {
  const char *src = (const char *)&msg->payload[1];
  int i = 0;
  for (; i < 23 && src[i]; i++) {
    name[i] = src[i];
  }
  name[i] = '\0';
}

static void list_files(void) {
  printf("SIZE      NAME\n");

  for (uint32_t index = 0; index < 128; index++) {
    int cap = get_fs_cap();
    uint64_t payload[IPC_INLINE_WORDS] = {
      [IPC_WORD_OP] = FS_REQ_LIST,
      [IPC_WORD_ARG0] = index
    };
    ipc_msg_t msg = cap < 0 ? (ipc_msg_t){.status = -1} :
                    sys_ipc_call((uint32_t)cap, 0, 16, payload);
    if (msg.status < 0) {
      printf("fs: server unavailable\n");
      return;
    }
    if (msg.payload[0] == FS_RESP_END) {
      return;
    }

    char name[24];
    unpack_fs_name(name, &msg);
    printf("%lu  %s\n", msg.payload[0], name);
  }
}

static void print_prompt(void) { printf("\nNeptune> "); }

void _start(void) {
  char cmd[MAX_CMD];
  int len = 0;

  while (ns_register("shell") < 0) {
    sys_sleep(10);
  }

  printf("  _   _            _                     __   ___  \n");
  printf(" | \\ | | ___ _ __ | |_ _   _ _ __   ___ / _ \\/ ___| \n");
  printf(" |  \\| |/ _ \\ '_ \\| __| | | | '_ \\ / _ \\ | | \\___ \\ \n");
  printf(" | |\\  |  __/ |_) | |_| |_| | | | |  __/ |_| |___) |\n");
  printf(" |_| \\_|\\___| .__/ \\__|\\__,_|_| |_|\\___|\\___/|____/ \n");
  printf("            |_|    AArch64 Microkernel Shell v0.1\n\n");
  printf("Type 'help' for available commands.\n");

  print_prompt();

  while (1) {
    (void)get_keyboard_cap();
    ipc_msg_t key = sys_ipc_recv(CAP_SELF);
    char c = (char)key.payload[0];
    sys_ipc_reply(key.reply_cap, 0, 0, 0, 0);

    if (c == '\r' || c == '\n') {
      printf("\n");
      cmd[len] = '\0';

      if (len == 0) {

      } else if (_streq(cmd, "help")) {
        printf("Available commands:\n");
        printf("  help          Show this help message\n");
        printf("  pong          Spawn pong (5-message IPC demo, then exits)\n");
        printf("  fault         Spawn a task that page-faults and gets killed\n");
        printf("  spawn pong    Run pong, leave status for wait <tid>\n");
        printf("  spawn fault   Spawn fault test, leave status for wait <tid>\n");
        printf("  spawn spin    Spawn a child that runs until killed\n");
        printf("  badptr        Verify bad syscall pointers are rejected\n");
        printf("  run <file>    Ask FS for an executable cap and spawn it\n");
        printf("  clear         Clear the screen\n");
        printf("  echo <text>   Print text back to the terminal\n");
        printf("  ls            List files from the FS server\n");
        printf("  uptime        Show seconds elapsed since boot\n");
        printf("  mem           Show memory usage\n");
        printf("  vmmap         Show this task's VMAs\n");
        printf("  capstat       Show this task's capability slots\n");
        printf("  debug         Show kernel build/runtime debug info\n");
        printf("  smoke         Run core regression smoke checks\n");
        printf("  lazy          Check lazy mmap and syscall usercopy\n");
        printf("  ipcfast       Test 0/8/64/128-byte register IPC\n");
        printf("  ipccap        Transfer an endpoint cap through IPC\n");
        printf("  ipckill       Test IPC cleanup across task kills\n");
        printf("  speed         Run userspace OS speed benchmarks\n");
        printf("  memshare      Share one page with a child task\n");
        printf("  memxfer       Transfer one page to a child task\n");
        printf("  memcaplife    Share a cap after unmapping old VA\n");
        printf("  memrevoke     Lend then revoke a child mapping\n");
        printf("  munmap        Unmap one lazy mmap page\n");
        printf("  forkcow       Fork shell and verify copy-on-write\n");
        printf("  filelazy      Lazily map an initrd file and read it\n");
        printf("  vmstress      Grow and shrink the VMA table\n");
        printf("  lazyexec      Spawn an initrd ELF through lazy VM objects\n");
        printf("  taskstress    Grow the kernel task table with spin tasks\n");
        printf("  ps            Show task scheduler state\n");
        printf("  jobs          Show shell child jobs\n");
        printf("  kill <tid>    Kill a user task\n");
        printf("  wait <tid>    Wait for a child task status\n");
        printf("  poll <tid>    Check child status without reaping\n");

      } else if (_streq(cmd, "pong")) {
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

      } else if (_streq(cmd, "fault")) {
        printf("Spawning fault test...\n");
        uint32_t fault_tid = spawn_program(FAULT_FILE, PONG_PRIORITY);
        if ((int)fault_tid < 0) {
          printf("Error: Could not spawn fault test (task limit reached?)\n");
        } else {
          printf("Fault test is TID %d. It should be killed by the kernel.\n",
                 (int)fault_tid);
          print_wait_result(sys_wait(fault_tid));
        }

      } else if (_streq(cmd, "spawn pong")) {
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

      } else if (_streq(cmd, "spawn fault")) {
        printf("Spawning fault test...\n");
        uint32_t fault_tid = spawn_program(FAULT_FILE, PONG_PRIORITY);
        if ((int)fault_tid < 0) {
          printf("Error: Could not spawn fault test (task limit reached?)\n");
        } else {
          add_job(fault_tid, "fault");
          printf("Fault test is TID %d. Run 'wait %u' to reap it.\n",
                 (int)fault_tid, fault_tid);
        }

      } else if (_streq(cmd, "spawn spin")) {
        uint32_t spin_tid = spawn_program(SPIN_FILE, PONG_PRIORITY);
        if ((int)spin_tid < 0) {
          printf("Error: Could not spawn spin task (task limit reached?)\n");
        } else {
          add_job(spin_tid, "spin");
          printf("Spin task is TID %d. Run 'ps', 'kill %u', then 'wait %u'.\n",
                 (int)spin_tid, spin_tid, spin_tid);
        }

      } else if (_streq(cmd, "badptr")) {
        uint32_t badptr_tid = spawn_program(BADPTR_FILE, PONG_PRIORITY);
        if ((int)badptr_tid < 0) {
          printf("Error: Could not spawn badptr test\n");
        } else {
          printf("Bad pointer test is TID %d.\n", (int)badptr_tid);
          print_wait_result(sys_wait(badptr_tid));
        }

      } else if (_starts_with(cmd, "run ")) {
        const char *name = cmd + 4;
        uint32_t tid = spawn_program(name, PONG_PRIORITY);
        if ((int)tid < 0) {
          printf("Error: could not run '%s'\n", name);
        } else {
          add_job(tid, name);
          printf("Started %s as TID %u\n", name, tid);
        }

      } else if (_streq(cmd, "clear")) {
        printf("\x1b[2J\x1b[H");

      } else if (_streq(cmd, "ls")) {
        list_files();

      } else if (_starts_with(cmd, "echo ")) {
        printf("%s\n", cmd + 5);

      } else if (_streq(cmd, "uptime")) {
        uint64_t secs = sys_uptime();
        uint64_t h = secs / 3600;
        uint64_t m = (secs % 3600) / 60;
        uint64_t s = secs % 60;
        printf("Uptime: %luh %lum %lus\n", h, m, s);

      } else if (_streq(cmd, "mem")) {
        memory_info_t mem = sys_mem();
        print_kib_line("Total RAM", mem.total_memory);
        print_kib_line("Free RAM", mem.free_memory);
        print_kib_line("Kernel heap size", mem.heap_size);
        print_kib_line("Kernel heap used", mem.heap_used);
        print_kib_line("Kernel heap mapped", mem.heap_mapped);

      } else if (_streq(cmd, "vmmap")) {
        print_vmmap();

      } else if (_streq(cmd, "capstat")) {
        print_capstat();

      } else if (_streq(cmd, "debug")) {
        print_debug_info();

      } else if (_streq(cmd, "smoke")) {
        run_smoke_demo();

      } else if (_streq(cmd, "lazy")) {
        run_lazy_memory_check();

      } else if (_streq(cmd, "ipcfast")) {
        run_ipcfast_demo();

      } else if (_streq(cmd, "ipccap")) {
        run_ipccap_demo();

      } else if (_streq(cmd, "ipckill")) {
        run_ipckill_demo();

      } else if (_streq(cmd, "speed")) {
        uint32_t speed_tid = spawn_program(SPEED_FILE, PONG_PRIORITY);
        if ((int)speed_tid < 0) {
          printf("Error: Could not spawn speed benchmark\n");
        } else {
          print_wait_result(sys_wait(speed_tid));
        }

      } else if (_streq(cmd, "memshare")) {
        run_memshare_demo();

      } else if (_streq(cmd, "memxfer")) {
        run_memxfer_demo();

      } else if (_streq(cmd, "memcaplife")) {
        run_memcaplife_demo();

      } else if (_streq(cmd, "memrevoke")) {
        run_memrevoke_demo();

      } else if (_streq(cmd, "munmap")) {
        run_munmap_demo();

      } else if (_streq(cmd, "forkcow")) {
        run_forkcow_demo();

      } else if (_streq(cmd, "filelazy")) {
        run_filelazy_demo();

      } else if (_streq(cmd, "vmstress")) {
        run_vmstress_demo();

      } else if (_streq(cmd, "lazyexec")) {
        run_lazyexec_demo();

      } else if (_streq(cmd, "taskstress")) {
        run_taskstress_demo();

      } else if (_streq(cmd, "ps")) {
        printf("TID PPID STATE   PRI BASE TICKS WAIT TARGET IRQ\n");
        uint32_t capacity = sys_task_capacity();
        for (uint32_t i = 0; i < capacity; i++) {
          task_info_t task = sys_ps(i);
          if (!task.present) {
            continue;
          }
          printf("%u   %u    %s   %u   %u    %u     %u    %u      %u\n",
                 task.tid, task.parent_tid, task_state_name(task.state),
                 (uint32_t)task.priority, (uint32_t)task.base_priority,
                 task.ticks_remaining, task.wait_time,
                 task.ipc_target_tid, task.awaiting_irq);
        }

      } else if (_streq(cmd, "jobs")) {
        print_jobs();

      } else if (_starts_with(cmd, "kill ")) {
        int ok = 0;
        uint32_t tid = _parse_u32(cmd + 5, &ok);
        if (!ok) {
          printf("Usage: kill <tid>\n");
        } else if (sys_kill(tid) < 0) {
          printf("Error: could not kill TID %u\n", tid);
        } else {
          printf("Killed TID %u\n", tid);
        }

      } else if (_starts_with(cmd, "wait ")) {
        int ok = 0;
        uint32_t tid = _parse_u32(cmd + 5, &ok);
        if (!ok) {
          printf("Usage: wait <tid>\n");
        } else {
          wait_info_t info = sys_wait(tid);
          print_wait_result(info);
          if (info.status == WAIT_STATUS_DONE) {
            remove_job(tid);
          }
        }

      } else if (_starts_with(cmd, "poll ")) {
        int ok = 0;
        uint32_t tid = _parse_u32(cmd + 5, &ok);
        if (!ok) {
          printf("Usage: poll <tid>\n");
        } else {
          print_poll_result(sys_poll(tid));
        }

      } else {
        printf("Unknown command: '%s'\n", cmd);
        printf("Type 'help' for a list of commands.\n");
      }

      len = 0;
      print_prompt();

    } else if (c == 127 || c == '\b') {
      if (len > 0) {
        len--;
        printf("\b \b");
      }
    } else if (c >= 32 && c < 127 && len < MAX_CMD - 1) {
      cmd[len++] = c;
      printf("%c", c);
    }
  }
}
