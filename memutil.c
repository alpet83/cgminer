#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "memutil.h"
#include "miner.h"
#include "malloc.h"


#define MBHDR_SIZE sizeof(mblock_header_t)
#define MB_TAG 0x937AA773
#define MAX_POINTERS 512000

static void *(*old_malloc_hook) (size_t, const void*);
static void *(*old_realloc_hook) (void *p, size_t, const void *x);
static void (*old_free_hook) (void *p, const void *x);



static alloc_point_t g_aps[MAX_ALLOC_PTS];
static void* all_ptrs[MAX_POINTERS] = { NULL };

static int ap_count = 0;
static pthread_mutex_t aps_lock;
static pthread_mutex_t ptrs_lock;
static void *safe_trace[10];

static void *malloc_capture(size_t size, const void *x);
static void *realloc_capture(void *p, size_t size, const void *x);
static void free_capture(void *p, const void *x);


inline void test_ptr (void *ptr, const char *func, const char* name) {
    char msg[32];
    if ( unlikely(!ptr) ) {
        snprintf(msg, 32, "Failed %s %s ", func, name);
        quit(1, msg);
    }
}

inline void *safe_calloc(size_t num, size_t size, const char* name) {
    backtrace(safe_trace, 10);
    void *result = calloc(num, size);
    memset(safe_trace, 0xff, sizeof(safe_trace));
    test_ptr (result, "to calloc", name);
    return result;
}


void stat_memory_usage(int place) {
    static size_t last_alloc = 0;
    static size_t alloc_map[256] = { 0 };

    struct mallinfo mi;
    mi = mallinfo();
    size_t curr_alloc = mi.uordblks;

    if ( last_alloc > 0 && place < 256 ) {
        size_t diff = (curr_alloc - last_alloc);
        alloc_map [place] += diff;
    }


    if (place == 256) {
        int i;
        for (i = 0; i < 256; i ++)
            if ( alloc_map [i] > 0 )
                printf ("alloc_map [%02X] = %d \n", i, alloc_map[i] );
    }
    if (place == 300) {
        printf("Total non-mmapped bytes (arena):       %d\n", mi.arena);
        printf("# of free chunks (ordblks):            %d\n", mi.ordblks);
        printf("# of free fastbin blocks (smblks):     %d\n", mi.smblks);
        printf("# of mapped regions (hblks):           %d\n", mi.hblks);
        printf("Bytes in mapped regions (hblkhd):      %d\n", mi.hblkhd);
        printf("Max. total allocated space (usmblks):  %d\n", mi.usmblks);
        printf("Free bytes held in fastbins (fsmblks): %d\n", mi.fsmblks);
        printf("Total allocated space (uordblks):      %d\n", mi.uordblks);
        printf("Total free space (fordblks):           %d\n", mi.fordblks);
        printf("Topmost releasable block (keepcost):   %d\n", mi.keepcost);
    }
    // */
}

bool compare_trace (void **a, void **b, int size) {
    int n;
    for (n = 0; n < size; n ++)
        if ( a[n] != b [n] ) return false;

    return true;
}

alloc_point_t *find_add_ap(const void* caller, void **trace, int tsize) {
    int np;
    alloc_point_t *result = NULL;

    mutex_lock (&aps_lock);

    for (np = 0; np < ap_count; np ++) {
        if ( g_aps[np].caller == caller ) {
            result = &g_aps[np];
        }

    }
    if (!result) {
        if (ap_count >= MAX_ALLOC_PTS)
            quit(1, "memutil.c: alloc_points overflow");

        result = &g_aps[ap_count ++];
        memset ( result, 0, sizeof (struct alloc_point) );
        memcpy ( result->trace_back, trace, tsize * sizeof(void*) );
        result->trace_size = tsize;
        result->caller = caller;

    }


    mutex_unlock (&aps_lock);

    return result;

}

inline void restore_hooks() {
    __malloc_hook = old_malloc_hook;
    __realloc_hook = old_realloc_hook;
    __free_hook = old_free_hook;
}

inline void install_hooks() {
    __malloc_hook = malloc_capture;
    __realloc_hook = realloc_capture;
    __free_hook = free_capture;
}

int ptr_index(void *p) {
    int i;
    if (NULL == p) return -1;
    for (i = 0; i < MAX_POINTERS; i ++)
        if (all_ptrs [i] == p) return i;
    return -1;
}

int ptr_save(void *p) {
    int i;
    mutex_lock (&ptrs_lock);
    for (i = 0; i < MAX_POINTERS; i ++)
        if (NULL == all_ptrs [i]) {
            all_ptrs [i] = p;
            return i;
        }
    mutex_unlock (&ptrs_lock);
    return -1;
}

void dump_aps(size_t min_diff) {
    restore_hooks();
    mutex_lock(&aps_lock);


    int n;
    for (n = 0; n < ap_count; n ++) {
        alloc_point_t *ap = &g_aps [n];
        size_t diff = ( ap->total_alloc - ap->total_freed );
        if ( diff > min_diff ) {
            // char msg [256];
            int i;

            printf ("alloc-diff = %7d at %08p :\n\r", diff, ap->caller);
            char **ss = (char**) backtrace_symbols( ap->trace_back, ap->trace_size );
            if (ss)
                for (i = 0; i < ap->trace_size; i ++)
                    printf("\t %s \n\r", ss[i]);
        }

    }
    mutex_unlock(&aps_lock);
    install_hooks();
}

inline void *reg_alloc(void *p, size_t size, const void *caller) {

    mblock_header_t *bh = (mblock_header_t *)p;

    p += MBHDR_SIZE;

    ptr_save(p);

    void *trace[10] = { NULL };
    int tsize = backtrace(trace, 10);
    bh->ap = find_add_ap(caller, trace, tsize);
    bh->ap->total_alloc += size;
    bh->tag = MB_TAG;
    bh->size = size;

    return p;
}

void mm_log(char *msg) {
    mutex_lock(&aps_lock);
    char filename[] = "cgminer_mm.log";
    FILE *f = fopen(filename, "a");
    fprintf(f, "%s\n", msg);
    fclose(f);
    mutex_unlock(&aps_lock);
}

inline void mm_log_op(char *op, void *ptr, size_t size, const void *caller) {
    char msg[256];

    snprintf(msg, 256, "%s;%08p;%d;%08p;%p-%p-%p-%p", op, ptr, size, caller, safe_trace[0], safe_trace[1], safe_trace[2], safe_trace[3]);
    mm_log(msg);
}


static void *malloc_capture(size_t size, const void *caller) {
    restore_hooks();
    void *result;

#ifdef HARD_MM
    result = malloc(size + MBHDR_SIZE);

    if (result)
        result = reg_alloc (result, size, caller);
#else

    result = malloc(size);
    mm_log_op("malloc", result, size, caller);
#endif


    install_hooks();

    return result;
}

static void *realloc_capture(void *p, size_t size, const void *caller) {
    restore_hooks();
    void *result = NULL;
#ifdef HARD_MM

    if (p) {
        mutex_lock(&ptrs_lock);
        int idx = ptr_index(p);

        // must work with only owned pointers
        if ( idx >= 0 ) {
            all_ptrs[idx] = NULL; // bye-bye!
            mblock_header_t *bh = (mblock_header_t *)(p - MBHDR_SIZE);
            if (MB_TAG == bh->tag && bh->size && bh->ap)
            {
                bh->ap->total_freed -= bh->size;
                p = bh;
            }
        }
        mutex_unlock(&ptrs_lock);
    }

    void *result = NULL;

    if (0 == size)
        free(p);
    else
        result = realloc(p, size + MBHDR_SIZE);

    if (result)
        result = reg_alloc(result, size, caller);
#else

    if (p)
        mm_log_op("realloc-", p, malloc_usable_size(p), caller);

    result = realloc (p, size);

    if (result)
        mm_log_op("realloc+", result, size, caller);
#endif

    install_hooks();

    return result;
}

static void free_capture(void *p, const void *caller) {

    if (!p) return;
#ifdef HARD_MM
    mutex_lock(&ptrs_lock);
    int idx = ptr_index(p);
    mutex_unlock(&ptrs_lock);

    // must work with only owned pointers
    if ( idx >= 0 ) {
        restore_hooks();
        mblock_header_t *bh = (mblock_header_t *)(p - MBHDR_SIZE);
        if (MB_TAG == bh->tag &&  bh->size && bh->ap) {
            bh->ap->total_freed -= bh->size;
            p = bh;
        }
        free (p);
        install_hooks();
    }
#else
    restore_hooks();

    mm_log_op("free", p, malloc_usable_size(p), caller);
    free (p);
    install_hooks();
#endif

}


void mem_observer_init() {
    mutex_init (&aps_lock);
    mutex_init (&ptrs_lock);
    memset(g_aps, 0, sizeof(g_aps));
    memset(all_ptrs, 0, sizeof(all_ptrs));

    install_hooks();
}
