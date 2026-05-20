#include "lib.h"

void _start(void) {
    while (1) {
        sys_yield();
    }
}
