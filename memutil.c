#include <stdio.h>
#include <stdlib.h>
#include "memutil.h"
#include "miner.h"
// #include <malloc.h>


inline void test_ptr (void *ptr, const char *func, const char* name) {
    char msg[32];
    if ( unlikely(!ptr) ) {
        snprintf(msg, 32, "Failed %s %s ", func, name);
        quit(1, msg);
    }
}

void *safe_calloc(size_t num, size_t size, const char* name) {
    void *result = calloc(num, size);
    test_ptr (result, "to calloc", name);
    return result;
}


void stat_memory_usage(int place) {
    static size_t last_alloc = 0;
    static size_t alloc_map[256] = { 0 };
    size_t curr_alloc;
}
