#ifndef __MEMUTIL_H__
#define __MEMUTIL_H__

#define MAX_ALLOC_PTS 16384

struct alloc_point {
const
    void    *caller;
    void    *trace_back [10];
    int      trace_size;
    unsigned total_alloc;
    unsigned total_freed;
};

typedef struct alloc_point alloc_point_t;

struct mblock_header {
    unsigned tag;
    size_t  size;
    alloc_point_t *ap;
};

typedef struct mblock_header mblock_header_t;



inline void *safe_calloc(size_t num, size_t size, const char* name);
void stat_memory_usage(int place);

void dump_aps(size_t min_diff);
void mem_observer_init();

#endif
