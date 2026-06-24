#include "lib.h"
#include "unistd.h"

static int fail(const char *message) {
    printf("pipetest: FAIL %s\n", message);
    return 1;
}

int main(void) {
    int fds[2];
    if (pipe(fds) < 0) return fail("create");

    const char message[] = "capability pipe works";
    if (write(fds[1], message, sizeof(message)) != (ssize_t)sizeof(message))
        return fail("write");
    char buffer[32];
    memset(buffer, 0, sizeof(buffer));
    if (read(fds[0], buffer, sizeof(message)) != (ssize_t)sizeof(message) ||
        strcmp(buffer, message) != 0) return fail("round trip");

    int duplicate = dup(fds[1]);
    if (duplicate < 0 || close(fds[1]) < 0) return fail("dup write end");
    ssize_t duplicate_wrote = write(duplicate, "x", 1);
    ssize_t duplicate_read = read(fds[0], buffer, 1);
    if (duplicate_wrote != 1 || duplicate_read != 1 || buffer[0] != 'x') {
        return fail("duplicated endpoint");
    }
    if (close(duplicate) < 0 || read(fds[0], buffer, 1) != 0)
        return fail("eof");
    if (close(fds[0]) < 0) return fail("close read end");

    if (pipe(fds) < 0 || dup2(fds[1], 1) != 1) return fail("dup2 stdout");
    if (write(1, "redirected", 10) != 10 ||
        read(fds[0], buffer, 10) != 10 || memcmp(buffer, "redirected", 10) != 0)
        return fail("redirected stdout");
    close(1);
    close(fds[0]);
    close(fds[1]);

    if (pipe(fds) < 0) return fail("fork create");
    int child = sys_fork();
    if (child < 0) return fail("fork");
    if (child == 0) {
        close(fds[0]);
        int ok = write(fds[1], "f", 1) == 1 && close(fds[1]) == 0;
        sys_exit(ok ? 0 : 1);
    }
    if (close(fds[1]) < 0 || read(fds[0], buffer, 1) != 1 ||
        buffer[0] != 'f' || read(fds[0], buffer, 1) != 0 ||
        close(fds[0]) < 0) return fail("fork endpoint lifetime");
    wait_info_t child_status = sys_wait((uint32_t)child);
    if (child_status.reason != TASK_TERM_EXITED || child_status.exit_code != 0)
        return fail("fork child status");

    printf("pipetest: ok\n");
    return 0;
}
