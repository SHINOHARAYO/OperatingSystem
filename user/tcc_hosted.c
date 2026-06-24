#include "lib.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <dlfcn.h>

int errno;

static FILE standard_input = {0, 0, 0, EOF, 0};
static FILE standard_output = {1, 0, 0, EOF, 0};
static FILE standard_error = {2, 0, 0, EOF, 0};
FILE *stdin = &standard_input;
FILE *stdout = &standard_output;
FILE *stderr = &standard_error;

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

static unsigned long long parse_unsigned(const char *s, char **end, int base,
                                         int *negative) {
    while (isspace(*s)) s++;
    *negative = 0;
    if (*s == '+' || *s == '-') {
        *negative = *s == '-';
        s++;
    }
    if (base == 0) {
        base = 10;
        if (s[0] == '0') {
            base = 8;
            if (s[1] == 'x' || s[1] == 'X') {
                base = 16;
                s += 2;
            }
        }
    } else if (base == 16 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
    }
    const char *first = s;
    unsigned long long value = 0;
    int digit;
    while ((digit = digit_value(*s)) >= 0 && digit < base) {
        value = value * (unsigned)base + (unsigned)digit;
        s++;
    }
    if (end) *end = (char *)(s == first ? first : s);
    return value;
}

long strtol(const char *s, char **end, int base) {
    int negative;
    unsigned long long value = parse_unsigned(s, end, base, &negative);
    return negative ? -(long)value : (long)value;
}

unsigned long strtoul(const char *s, char **end, int base) {
    int negative;
    unsigned long long value = parse_unsigned(s, end, base, &negative);
    return negative ? (unsigned long)-value : (unsigned long)value;
}

long long strtoll(const char *s, char **end, int base) {
    int negative;
    unsigned long long value = parse_unsigned(s, end, base, &negative);
    return negative ? -(long long)value : (long long)value;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    int negative;
    unsigned long long value = parse_unsigned(s, end, base, &negative);
    return negative ? (unsigned long long)-value : value;
}

double strtod(const char *s, char **end) {
    while (isspace(*s)) s++;
    int negative = 0;
    if (*s == '+' || *s == '-') {
        negative = *s == '-';
        s++;
    }
    double value = 0;
    while (isdigit(*s)) value = value * 10 + (*s++ - '0');
    if (*s == '.') {
        double place = 0.1;
        s++;
        while (isdigit(*s)) {
            value += (*s++ - '0') * place;
            place *= 0.1;
        }
    }
    if (*s == 'e' || *s == 'E') {
        int exponent = (int)strtol(s + 1, (char **)&s, 10);
        while (exponent > 0) { value *= 10; exponent--; }
        while (exponent < 0) { value *= 0.1; exponent++; }
    }
    if (end) *end = (char *)s;
    return negative ? -value : value;
}

float strtof(const char *s, char **end) { return (float)strtod(s, end); }
long double strtold(const char *s, char **end) { return (long double)strtod(s, end); }

char *strcat(char *dst, const char *src) {
    char *result = dst;
    strcpy(dst + strlen(dst), src);
    return result;
}

char *strncat(char *dst, const char *src, size_t n) {
    char *out = dst + strlen(dst);
    size_t i = 0;
    while (i < n && src[i]) out[i] = src[i], i++;
    out[i] = 0;
    return dst;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    for (; *haystack; haystack++) {
        const char *a = haystack;
        const char *b = needle;
        while (*a && *b && *a == *b) a++, b++;
        if (!*b) return (char *)haystack;
    }
    return 0;
}

char *strpbrk(const char *s, const char *accept) {
    for (; *s; s++) if (strchr(accept, *s)) return (char *)s;
    return 0;
}

char *strdup(const char *s) {
    size_t size = strlen(s) + 1;
    char *copy = malloc(size);
    if (copy) memcpy(copy, s, size);
    return copy;
}

int strcasecmp(const char *a, const char *b) {
    while (*a && tolower(*a) == tolower(*b)) a++, b++;
    return tolower(*a) - tolower(*b);
}

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) { char c = *a; *a++ = *b; *b++ = c; }
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *)) {
    char *items = base;
    for (size_t i = 1; i < count; i++) {
        for (size_t j = i; j > 0 && compare(items + (j - 1) * size,
                                               items + j * size) > 0; j--) {
            swap_bytes(items + (j - 1) * size, items + j * size, size);
        }
    }
}

char *getenv(const char *name) { (void)name; return 0; }

void abort(void) { sys_exit(1); }
void exit(int status) { sys_exit(status); }

static int parse_mode(const char *mode) {
    if (!mode || !mode[0]) return -1;
    int flags = mode[0] == 'r' ? O_RDONLY : O_WRONLY | O_CREAT;
    if (mode[0] == 'w') flags |= O_TRUNC;
    if (mode[0] != 'r' && mode[0] != 'w' && mode[0] != 'a') return -1;
    for (const char *p = mode + 1; *p; p++) if (*p == '+') flags = O_RDWR | O_CREAT;
    return flags;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = parse_mode(mode);
    if (flags < 0) return 0;
    int fd = open(path, flags);
    if (fd < 0) { errno = ENOENT; return 0; }
    FILE *stream = calloc(1, sizeof(*stream));
    if (!stream) { close(fd); errno = ENOMEM; return 0; }
    stream->fd = fd;
    stream->ungot = EOF;
    stream->owns_fd = 1;
    if (mode[0] == 'a') lseek(fd, 0, SEEK_END);
    return stream;
}

int fclose(FILE *stream) {
    if (!stream) return EOF;
    int result = stream->owns_fd ? close(stream->fd) : 0;
    if (stream->owns_fd) free(stream);
    return result < 0 ? EOF : 0;
}

size_t fread(void *ptr, size_t size, size_t count, FILE *stream) {
    if (!size || !count || !stream) return 0;
    size_t bytes = size * count;
    size_t done = 0;
    if (stream->ungot != EOF && bytes) {
        ((char *)ptr)[done++] = (char)stream->ungot;
        stream->ungot = EOF;
    }
    size_t remaining = bytes - done;
    ssize_t got = read(stream->fd, (char *)ptr + done, remaining);
    if (got < 0) { stream->error = 1; return done / size; }
    done += (size_t)got;
    if ((size_t)got < remaining) stream->eof = 1;
    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t count, FILE *stream) {
    if (!size || !count || !stream) return 0;
    size_t bytes = size * count;
    ssize_t wrote = write(stream->fd, ptr, bytes);
    if (wrote < 0) { stream->error = 1; return 0; }
    return (size_t)wrote / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream || lseek(stream->fd, offset, whence) < 0) return -1;
    stream->eof = 0;
    stream->ungot = EOF;
    return 0;
}

long ftell(FILE *stream) { return stream ? lseek(stream->fd, 0, SEEK_CUR) : -1; }
int fflush(FILE *stream) { (void)stream; terminal_flush(); return 0; }

int fgetc(FILE *stream) {
    if (!stream) return EOF;
    if (stream->ungot != EOF) { int c = stream->ungot; stream->ungot = EOF; return c; }
    char c;
    ssize_t result = read(stream->fd, &c, 1);
    if (result != 1) { stream->eof = result == 0; stream->error = result < 0; return EOF; }
    return (unsigned char)c;
}

int fputc(int c, FILE *stream) {
    char byte = (char)c;
    return !stream || write(stream->fd, &byte, 1) != 1 ? EOF : (unsigned char)byte;
}

int fputs(const char *s, FILE *stream) { return fwrite(s, 1, strlen(s), stream) == strlen(s) ? 0 : EOF; }
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
    stream->eof = stream->error = 0;
    stream->ungot = EOF;
    stream->owns_fd = 1;
    return stream;
}

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), fmt, args);
    if (length < 0 || fwrite(buffer, 1, (size_t)(length < (int)sizeof(buffer) ? length : (int)sizeof(buffer) - 1), stream) == 0) return -1;
    return length;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = vfprintf(stream, fmt, args);
    va_end(args);
    return result;
}

int perror(const char *s) { return fprintf(stderr, "%s: %s\n", s, strerror(errno)); }

char *strerror(int error) {
    if (error == ENOENT) return "no such file";
    if (error == ENOMEM) return "out of memory";
    if (error == EINVAL) return "invalid argument";
    return "I/O error";
}

time_t time(time_t *result) {
    time_t value = (time_t)(sys_uptime_ms() / 1000);
    if (result) *result = value;
    return value;
}

struct tm *localtime(const time_t *value) {
    static struct tm result;
    time_t seconds = value ? *value : time(0);
    result.tm_sec = seconds % 60;
    result.tm_min = (seconds / 60) % 60;
    result.tm_hour = (seconds / 3600) % 24;
    result.tm_mday = 1 + seconds / 86400;
    result.tm_mon = 0;
    result.tm_year = 70;
    return &result;
}

int gettimeofday(struct timeval *tv, void *timezone) {
    (void)timezone;
    if (!tv) return -1;
    uint64_t ms = sys_uptime_ms();
    tv->tv_sec = (time_t)(ms / 1000);
    tv->tv_usec = (long)((ms % 1000) * 1000);
    return 0;
}

void *dlopen(const char *path, int mode) { (void)path; (void)mode; return 0; }
void *dlsym(void *handle, const char *name) { (void)handle; (void)name; return 0; }
int dlclose(void *handle) { (void)handle; return 0; }
char *dlerror(void) { return "dynamic loading is unavailable"; }

int unlink(const char *path) { return vfs_delete_file(path) < 0 ? -1 : 0; }
int remove(const char *path) { return unlink(path); }

char *getcwd(char *buffer, size_t size) {
    if (!buffer || size < 2) return 0;
    buffer[0] = '/';
    buffer[1] = 0;
    return buffer;
}

char *realpath(const char *path, char *resolved) {
    if (!path) return 0;
    if (!resolved) return strdup(path);
    strcpy(resolved, path);
    return resolved;
}

char **environ;
