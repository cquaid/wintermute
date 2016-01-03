#include <sys/types.h>

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "shared/list.h"
#include "shared/util.h"
#include "ptracer/ptracer.h"

#include "match.h"
#include "match_internal.h"
#include "pid_mem.h"
#include "region.h"


/**
 * @file match_search.c
 *
 * Memory searching routines
 *
 * TODO: externalize checks for /proc/pid/mem readability.
 * TODO: create and supply a wintermute context holding a ptracer context.
 * TODO: pretty sure some of the match functions are bullshit.
 */

typedef int(*search_match_fn)(const struct match_object *,
    const struct match_needle *, const struct match_needle *);


struct process_ctx;

typedef int(*process_init_fn)(struct process_ctx *, int fd, pid_t pid);
typedef void(*process_fini_fn)(struct process_ctx *);
typedef int(*process_next_fn)(struct process_ctx *);
typedef int(*process_set_fn)(struct process_ctx *, const struct region *);

struct process_ctx {
    int fd;
    pid_t pid;
    void *data;
    size_t len;
    struct process_ops *ops;
};

struct process_ops {
    process_init_fn init;
    process_fini_fn fini;
    process_next_fn next;
    process_set_fn set;
};


static ssize_t
__read_pid_mem(int fd, pid_t pid, void *buf,
    size_t size, unsigned long addr)
{
    ssize_t err;
    (void)pid;

    err = read_pid_mem_loop_fd(fd, buf, size, (off_t)addr);

    if (err < 0)
        return -1;

    /* Will return smaller size than asked for if end of a range. */
    return err;
}

static ssize_t
__ptrace_peektext(int fd, pid_t pid, void *buf,
    size_t size, unsigned long addr)
{
    int err;
    size_t i;
    size_t rem;
    size_t count;
    unsigned long val;
    (void)fd;

    count = size / sizeof(unsigned long);
    rem = size % sizeof(unsigned long);

    for (i = 0; i < count; ++i) {
        err = ptrace_peektext(pid, addr, &val); 

        if (err != 0)
            return -1;

        *(unsigned long *)buf = val;
        buf += sizeof(unsigned long);
        addr += sizeof(unsigned long);
    }

    if (rem != 0) {
        err = ptrace_peektext(pid, addr, &val);

        if (err != 0)
            return -1;

        memcpy(buf, &val, rem);
    }

    return 0;
}

static inline int
get_match_object(struct match_object *obj, read_fn read_actor,
    int fd, pid_t pid, unsigned long addr)
{
    int neg;
    ssize_t err;

    memset(obj, 0, sizeof(*obj));

    err = read_actor(fd, pid, obj->v.bytes, sizeof(obj->v.bytes), addr);

    if (err < 0)
        return -1;

    /* On 0, assume we got enough data (ptrace case) */
    if (err == 0)
        err = sizeof(obj->v.bytes);

    /* Set address */
    obj->addr = addr;

    neg = (obj->v.i64 < 0LL);

    /* Set integer and floating flags. */

    if (obj->v.u64 <= UINT8_MAX) {
        if (neg)
            obj->flags.i8 = !(obj->v.i64 < INT8_MIN);
        else
            obj->flags.i8 = 1;
    }

    /* < 2 bytes means we can't fit int16 or above. */
    if (err < 2)
        return 0;

    if (obj->v.u64 <= UINT16_MAX) {
        if (neg)
            obj->flags.i16 = !(obj->v.i64 < INT16_MIN);
        else
            obj->flags.i16 = 1;
    }

    /* < 4 bytes means we can't fit int32 or above. */
    if (err < 4)
        return 0;  

    if (obj->v.u64 <= UINT32_MAX) {
        if (neg)
            obj->flags.i32 = !(obj->v.i64 < INT32_MIN);
        else
            obj->flags.i32 = 1;
    }

    /* No clue how to determine if a valid float32. */
    obj->flags.f32 = 1;

    /* < 8 bytes means we can't fit int64 and double. */
    if (err < 8)
        return 0;

    obj->flags.i64 = 1;
    /* No clue how to determine if a valid float64. */
    obj->flags.f64 = 1;

    return 0;
}

#define new_chunk(siz) \
    malloc(sizeof(struct match_chunk_header) \
        + (siz - 1) * sizeof(struct match_chunk_object))


static int
process_region(struct process_ctx *ctx,
    struct match_list *list, const struct region *region,
    int options, search_match_fn match,
    struct match_chunk_header **pcurrent_chunk)
{
    struct match_chunk_header *current_chunk = *pcurrent_chunk;

    if (current_chunk == NULL) {
        current_chunk = new_chunk(MATCH_CHUNK_SIZE_HUGE);

        if (current_chunk == NULL)
            return -1;
    }
}


static int
__process_pid_mem_init(struct process_ctx *ctx, int fd, pid_t pid)
{
    ctx->fd = fd;
    ctx->pid = pid;

    ctx->data = NULL;
    ctx->len = 0;

    return 0;
}

static void
__process_pid_mem_fini(struct process_ctx *ctx)
{
}

static int
__process_pid_mem_next(struct process_ctx *ctx)
{
    return 0;
}

static int
__process_pid_mem_set(struct process_ctx *ctx, const struct region *region)
{
    return 0;
}

static const process_ops __process_ops_pid_mem = {
    .init = __process_pid_mem_init,
    .fini = __process_pid_mem_fini,
    .next = __process_pid_mem_next,
    .set = __process_pid_mem_set
};

static int
__search(pid_t pid, struct match_list *list,
    const struct match_needle *needle_1,
    const struct match_needle *needle_2,
    const struct region_list *regions,
    int options, search_match_fn match)
{
    int fd;
    int err;
    int ret;
    int oerrno;

    struct process_ctx ctx;

    struct match_chunk_header *current_chunk = NULL;


    /* Determine which memory reading method to use. */
    err = can_read_pid_mem(pid);

    if (err == 0) {
        fd = open_pid_mem(pid, PID_MEM_FLAGS_READ);

        /* Even if we have access, if we can't open the file,
         * try to use ptrace instead. */
        if (fd < 0)
            ctx.ops = __process_ops_ptrace;
        else
            ctx.ops = __process_ops_pid_mem;
    }
    else {
        ctx.ops = __process_ops_ptrace;
        fd = -1;
    }

    /* Initialize the processing context. */
    err = ctx.ops->init(&ctx, fd, pid);

    if (err != 0) {
        ret = -1;
        goto out;
    }

    list_for_each(entry, &(regions->head)) {
        struct region *region;

        region = region_entry(entry);

        err = process_region(&ctx, list,
                region, options, match,
                &current_chunk);

        if (err != 0) {
            ret = -1;
            goto out;
        }
    }

out:

    if (ret != 0)
        oerrno = errno;

    /* Finalize the processing context */
    ctx.ops.fini(&ctx);

    /* close the pid_mem fd if open */
    if (fd != -1)
        (void)close_pid_mem(fd);

    if (ret != 0)
        errno = oerrno;

    return ret;
}


static int
__search_eq(const struct match_object *value,
    const struct match_needle *needle, const struct match_needle *unused)
{
    (void)unused;

    if (needle->obj.flags.i8) {
        if ((needle->obj.v.u8 == value->v.u8)
            return 1;
    }

    if (needle->obj.flags.i16) {
        if (needle->obj.v.u16 == value->v.u16)
            return 1;
    }

    if (needle->obj.flags.i32 || needle->obj.flags.f32) {
        if (needle->obj.v.u32 == value->v.u32)
            return 1;
    }
 
    if (needle->obj.flags.i64 || needle->obj.flags.f64) {
        if (needle->obj.v.u64 == value->v.u64)
            return 1;
    }

   return 0;
}

/**
 * Find matches equal to a value in the match list.
 *
 * @param[in] pid - process id these matches are for
 * @param list - list to match against
 * @param[in] needle - value to find
 *
 * @return 0 on success
 * @return < 0 on failure with error returned in errno
 */
int
match_eq(pid_t pid, struct match_list *list,
    const struct match_needle *needle,
    const struct region_list *region,
    int options)
{
    return __search(pid, list, needle, NULL, regions, options, __search_eq);
}


/* vim: set et ts=4 sts=4 sw=4 syntax=c : */