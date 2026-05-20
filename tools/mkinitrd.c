#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define INITRD_MAGIC 0x4452494E
#define INITRD_VERSION 1
#define INITRD_NAME_LEN 56

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t file_count;
    uint32_t header_size;
} initrd_header_t;

typedef struct {
    char name[INITRD_NAME_LEN];
    uint64_t offset;
    uint64_t size;
} initrd_entry_t;

static const char *base_name(const char *path) {
    const char *best = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            best = p + 1;
        }
    }
    return best;
}

static uint64_t file_size(FILE *file) {
    if (fseek(file, 0, SEEK_END) != 0) {
        return 0;
    }
    long size = ftell(file);
    if (size < 0) {
        return 0;
    }
    rewind(file);
    return (uint64_t)size;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <output> <file> [file...]\n", argv[0]);
        return 1;
    }

    uint32_t file_count = (uint32_t)(argc - 2);
    uint32_t header_size = sizeof(initrd_header_t) +
                           (file_count * sizeof(initrd_entry_t));
    initrd_entry_t *entries = calloc(file_count, sizeof(initrd_entry_t));
    if (!entries) {
        perror("calloc");
        return 1;
    }

    uint64_t offset = header_size;
    for (uint32_t i = 0; i < file_count; i++) {
        const char *path = argv[i + 2];
        const char *name = base_name(path);
        if (strlen(name) >= INITRD_NAME_LEN) {
            fprintf(stderr, "file name too long: %s\n", name);
            free(entries);
            return 1;
        }

        FILE *file = fopen(path, "rb");
        if (!file) {
            perror(path);
            free(entries);
            return 1;
        }

        uint64_t size = file_size(file);
        fclose(file);

        strcpy(entries[i].name, name);
        entries[i].offset = offset;
        entries[i].size = size;
        offset += size;
    }

    FILE *out = fopen(argv[1], "wb");
    if (!out) {
        perror(argv[1]);
        free(entries);
        return 1;
    }

    initrd_header_t header = {
        .magic = INITRD_MAGIC,
        .version = INITRD_VERSION,
        .file_count = file_count,
        .header_size = header_size,
    };

    if (fwrite(&header, sizeof(header), 1, out) != 1 ||
        fwrite(entries, sizeof(initrd_entry_t), file_count, out) != file_count) {
        perror("write header");
        fclose(out);
        free(entries);
        return 1;
    }

    for (uint32_t i = 0; i < file_count; i++) {
        FILE *in = fopen(argv[i + 2], "rb");
        if (!in) {
            perror(argv[i + 2]);
            fclose(out);
            free(entries);
            return 1;
        }

        char buffer[4096];
        size_t n;
        while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
            if (fwrite(buffer, 1, n, out) != n) {
                perror("write data");
                fclose(in);
                fclose(out);
                free(entries);
                return 1;
            }
        }
        fclose(in);
    }

    fclose(out);
    free(entries);
    return 0;
}
