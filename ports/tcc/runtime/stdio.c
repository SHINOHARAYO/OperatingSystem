#include "lib.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int errno;

static FILE standard_input = {0, 0, 0, EOF, 0};
static FILE standard_output = {1, 0, 0, EOF, 0};
static FILE standard_error = {2, 0, 0, EOF, 0};
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

static int parse_mode(const char *mode) {
    if (!mode || !mode[0]) return -1;
    int flags = mode[0] == 'r' ? O_RDONLY : O_WRONLY | O_CREAT;
    if (mode[0] == 'w') flags |= O_TRUNC;
    if (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') return -1;
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') flags = O_RDWR | O_CREAT;
    }
    return flags;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = parse_mode(mode);
    if (flags < 0) return 0;
    int fd = open(path, flags);
    if (fd < 0) return 0;
    FILE *stream = calloc(1, sizeof(*stream));
    if (!stream) {
        close(fd);
        return 0;
    }
    stream->fd = fd;
    stream->ungot = EOF;
    stream->owns_fd = 1;
    if (mode[0] == 'a') (void)lseek(fd, 0, SEEK_END);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    int result = stream->owns_fd ? close(stream->fd) : 0;
    if (stream->owns_fd) free(stream);
    return result < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (!ptr || !stream || size == 0 || count == 0) return 0;
    size_t bytes = size * count;
    size_t done = 0;
    if (stream->ungot != EOF) {
        ((char *)ptr)[done++] = (char)stream->ungot;
        stream->ungot = EOF;
    }
    size_t remaining = bytes - done;
    ssize_t result = read(stream->fd, (char *)ptr + done, remaining);
    if (result < 0) {
        stream->error = 1;
        return done / size;
    }
    done += (size_t)result;
    if ((size_t)result < remaining) stream->eof = 1;
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    if (!ptr || !stream || size == 0 || count == 0) return 0;
    ssize_t result = write(stream->fd, ptr, size * count);
    if (result < 0) {
        stream->error = 1;
        return 0;
    }
    return (size_t)result / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream || lseek(stream->fd, offset, whence) < 0) return -1;
    stream->eof = 0;
    stream->ungot = EOF;
    return 0;
}

long ftell(FILE *stream) { return stream ? lseek(stream->fd, 0, SEEK_CUR) : -1; }

int fflush(FILE *stream) {
    (void)stream;
    terminal_flush();
    return 0;
}

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->ungot != EOF) {
        int c = stream->ungot;
        stream->ungot = EOF;
        return c;
    }
    char c;
    ssize_t result = read(stream->fd, &c, 1);
    if (result != 1) {
        stream->eof = result == 0;
        stream->error = result < 0;
        return EOF;
    }
    return (unsigned char)c;
}

int fputc(int c, FILE *stream) {
    char byte = (char)c;
    return !stream || write(stream->fd, &byte, 1) != 1 ? EOF : (unsigned char)byte;
}

int fputs(const char *s, FILE *stream) {
    size_t length = strlen(s);
    return fwrite(s, 1, length, stream) == length ? 0 : EOF;
}

int feof(FILE *stream) { return stream && stream->eof; }
int ferror(FILE *stream) { return stream && stream->error; }
int fileno(FILE *stream) { return stream ? stream->fd : -1; }

int sprintf(char *buffer, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vsnprintf(buffer, (size_t)-1, fmt, args);
    va_end(args);
    return result;
}

FILE *fdopen(int fd, const char *mode) {
    if (fd < 0 || parse_mode(mode) < 0) return 0;
    FILE *stream = calloc(1, sizeof(*stream));
    if (!stream) return 0;
    stream->fd = fd;
    stream->ungot = EOF;
    stream->owns_fd = 1;
    return stream;
}

FILE *freopen(const char *path, const char *mode, FILE *stream) {
    if (!stream) return 0;
    int flags = parse_mode(mode);
    int fd = flags < 0 ? -1 : open(path, flags);
    if (fd < 0) return 0;
    if (stream->owns_fd) close(stream->fd);
    stream->fd = fd;
    stream->eof = 0;
    stream->error = 0;
    stream->ungot = EOF;
    stream->owns_fd = 1;
    return stream;
}

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (length < 0 ||
        fwrite(buffer, 1, (size_t)(length < (int)sizeof(buffer) ? length :
                                   (int)sizeof(buffer) - 1), stream) == 0) {
        return -1;
    }
    return length;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vfprintf(stream, fmt, args);
    va_end(args);
    return result;
}

int perror(const char *s) { return fprintf(stderr, "%s\n", s ? s : "error"); }

void abort(void) { sys_exit(1); }
void exit(int status) { sys_exit(status); }
char *getenv(const char *name) { (void)name; return 0; }
