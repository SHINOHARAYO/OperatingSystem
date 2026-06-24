#include "lib.h"
#include "unistd.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1 && write(1, " ", 1) != 1) return 1;
        size_t length = strlen(argv[i]);
        if (write(1, argv[i], length) != (ssize_t)length) return 1;
    }
    return write(1, "\n", 1) == 1 ? 0 : 1;
}
