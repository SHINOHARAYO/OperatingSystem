#include "lib.h"

int tcc_upstream_main(int argc, char **argv);

static int does_not_link_executable(const char *argument) {
    return strcmp(argument, "-c") == 0 || strcmp(argument, "-E") == 0 ||
           strcmp(argument, "-r") == 0 || strcmp(argument, "-shared") == 0;
}

int main(int argc, char **argv) {
    int add_static = 1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-static") == 0 ||
            does_not_link_executable(argv[i])) {
            add_static = 0;
            break;
        }
    }

    if (!add_static) {
        return tcc_upstream_main(argc, argv);
    }

    char *static_argv[argc + 2];
    static_argv[0] = argv[0];
    static_argv[1] = "-static";
    for (int i = 1; i < argc; i++) {
        static_argv[i + 1] = argv[i];
    }
    static_argv[argc + 1] = 0;
    return tcc_upstream_main(argc + 1, static_argv);
}
