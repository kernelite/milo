#include <stdio.h>

int main(int argc, char** argv) {
    printf("  [EL0 musl] Hello from musl-libc! argc=%d argv[0]=%s\n", argc,
           (argc > 0 && argv[0] != NULL) ? argv[0] : "(null)");
    puts("  [EL0 musl] Buffered stdio writev flushed cleanly.");
    return 0;
}