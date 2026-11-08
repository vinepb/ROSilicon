/* Resolve only the current process's loaded image metadata. No attach or
 * target-memory access; input is a saved address from our own native fixture. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char *end;
    const unsigned long long value = strtoull(argv[1], &end, 0);
    if (!*argv[1] || *end || !value) return 2;
    Dl_info info;
    if (!dladdr((const void *)(uintptr_t)value, &info)) {
        puts("OBS own-symbol=unresolved"); return 1;
    }
    printf("OBS own-address=0x%llx image=%s base=%p symbol=%s symbol-address=%p\n",
           value, info.dli_fname ? info.dli_fname : "unknown", info.dli_fbase,
           info.dli_sname ? info.dli_sname : "unknown", info.dli_saddr);
    return 0;
}
