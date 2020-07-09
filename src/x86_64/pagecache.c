/* TODO:
   - per node flush and purge
   - reinstate free list, keep refault counts
   - interface to physical free page list / shootdown epochs

   - would be nice to propagate a priority alone with requests to
     pagecache - which in turn would be passed to page I/O - so that
     page fault fills can go to head of request queue
*/

#include <kernel.h>
#include <page.h>
#include <pagecache.h>
#include <pagecache_internal.h>

//#define PAGECACHE_DEBUG
#if defined(PAGECACHE_DEBUG)
#define pagecache_debug(x, ...) do {rprintf("PGC: " x, ##__VA_ARGS__);} while(0)
#else
#define pagecache_debug(x, ...)
#endif

#ifdef BOOT
#define PAGECACHE_READ_ONLY
#endif

/* TODO: Seems like this ought not to be so large ... but we're
   queueing a ton with the polled ATA driver. There's only one queue globally anyhow. */
#define MAX_PAGE_COMPLETION_VECS 16384

static inline u64 cache_pagesize(pagecache pc)
{
    return U64_FROM_BIT(pc->page_order);
}

static inline int page_state(pagecache_page pp)
{
    return pp->state_offset >> PAGECACHE_PAGESTATE_SHIFT;
}

static inline u64 page_offset(pagecache_page pp)
{
    return pp->state_offset & MASK(PAGECACHE_PAGESTATE_SHIFT);
}

static inline range byte_range_from_page(pagecache pc, pagecache_page pp)
{
    return range_lshift(irangel(page_offset(pp), 1), pc->page_order);
}

static inline void pagelist_enqueue(pagelist pl, pagecache_page pp)
{
    list_insert_before(&pl->l, &pp->l);
    pl->pages++;
}

static inline void pagelist_remove(pagelist pl, pagecache_page pp)
{
    list_delete(&pp->l);
    pl->pages--;
}

static inline void pagelist_move(pagelist dest, pagelist src, pagecache_page pp)
{
    pagelist_remove(src, pp);
    pagelist_enqueue(dest, pp);
}

static inline void pagelist_touch(pagelist pl, pagecache_page pp)
{
    list_delete(&pp->l);
    list_insert_before(&pl->l, &pp->l);
}

static inline void change_page_state_locked(pagecache pc, pagecache_page pp, int state)
{
    int old_state = page_state(pp);
    switch (state) {
#if 0
    /* Temporarily disabling use of free until we have a scheme to
       keep and act on "refault" data */
    case PAGECACHE_PAGESTATE_FREE:
        assert(old_state == PAGECACHE_PAGESTATE_EVICTED);
        pagelist_enqueue(&pc->free, pp);
        break;
#endif
    case PAGECACHE_PAGESTATE_EVICTED:
        if (old_state == PAGECACHE_PAGESTATE_NEW) {
            pagelist_remove(&pc->new, pp);
        } else {
            assert(old_state == PAGECACHE_PAGESTATE_ACTIVE);
            pagelist_remove(&pc->active, pp);
        }
        /* caller must do release following state change to evicted */
        break;
    case PAGECACHE_PAGESTATE_ALLOC:
        assert(old_state == PAGECACHE_PAGESTATE_FREE);
        pagelist_remove(&pc->free, pp);
        break;
    case PAGECACHE_PAGESTATE_READING:
        assert(old_state == PAGECACHE_PAGESTATE_ALLOC);
        break;
    case PAGECACHE_PAGESTATE_WRITING:
        if (old_state == PAGECACHE_PAGESTATE_NEW) {
            pagelist_move(&pc->writing, &pc->new, pp);
        } else if (old_state == PAGECACHE_PAGESTATE_ACTIVE) {
            pagelist_move(&pc->writing, &pc->active, pp);
        } else if (old_state == PAGECACHE_PAGESTATE_WRITING) {
            /* write already pending, move to tail of queue */
            pagelist_touch(&pc->writing, pp);
        } else {
            assert(old_state == PAGECACHE_PAGESTATE_ALLOC);
            pagelist_enqueue(&pc->writing, pp);
        }
        pp->write_count++;
        break;
    case PAGECACHE_PAGESTATE_NEW:
        if (old_state == PAGECACHE_PAGESTATE_ACTIVE) {
            pagelist_move(&pc->new, &pc->active, pp);
        } else if (old_state == PAGECACHE_PAGESTATE_WRITING) {
            pagelist_move(&pc->new, &pc->writing, pp);
        } else {
            assert(old_state == PAGECACHE_PAGESTATE_READING);
            pagelist_enqueue(&pc->new, pp);
        }
        break;
    case PAGECACHE_PAGESTATE_ACTIVE:
        assert(old_state == PAGECACHE_PAGESTATE_NEW);
        pagelist_move(&pc->active, &pc->new, pp);
        break;
    default:
        halt("%s: bad state %d, old %d\n", __func__, state, old_state);
    }

    pp->state_offset = (pp->state_offset & MASK(PAGECACHE_PAGESTATE_SHIFT)) |
        ((u64)state << PAGECACHE_PAGESTATE_SHIFT);
}

#ifdef STAGE3
closure_function(1, 0, void, pagecache_service_completions,
                 pagecache, pc)
{
    /* we don't need the pagecache lock here; flag reset is atomic and dequeue is safe */
    assert(bound(pc)->service_enqueued);
    bound(pc)->service_enqueued = false;
    vector v;
    while ((v = dequeue(bound(pc)->completion_vecs)) != INVALID_ADDRESS) {
        status_handler sh;
        status s = vector_pop(v);
        vector_foreach(v, sh) {
            assert(sh);
            apply(sh, s);
        }
        deallocate_vector(v);
    }
}

static void pagecache_page_queue_completions_locked(pagecache pc, pagecache_page pp, status s)
{
    if (pp->completions && vector_length(pp->completions) > 0) {
        vector_push(pp->completions, s);
        assert(enqueue(pc->completion_vecs, pp->completions));
        pp->completions = 0;
        if (!pc->service_enqueued) {
            pc->service_enqueued = true;
            assert(enqueue(runqueue, pc->service_completions));
        }
    }
}
#else
static void pagecache_page_queue_completions_locked(pagecache pc, pagecache_page pp, status s)
{
    if (pp->completions && vector_length(pp->completions) > 0) {
        vector v = pp->completions;
        pp->completions = 0;
        status_handler sh;
        vector_foreach(v, sh) {
            assert(sh);
            apply(sh, s);
        }
        deallocate_vector(v);
    }
}
#endif

closure_function(3, 1, void, pagecache_read_page_complete,
                 pagecache, pc, pagecache_page, pp, sg_list, sg,
                 status, s)
{
    pagecache pc = bound(pc);
    pagecache_page pp = bound(pp);
    pagecache_debug("%s: pc %p, pp %p, status %v\n", __func__, pc, bound(pp), s);
    assert(page_state(pp) == PAGECACHE_PAGESTATE_READING);

    if (!is_ok(s)) {
        /* TODO need policy for capturing/reporting I/O errors... */
        msg_err("error reading page 0x%lx: %v\n", page_offset(pp) << pc->page_order, s);
    }
    spin_lock(&pc->state_lock);
    change_page_state_locked(bound(pc), pp, PAGECACHE_PAGESTATE_NEW);
    pagecache_page_queue_completions_locked(pc, pp, s);
    spin_unlock(&pc->state_lock);
    sg_list_release(bound(sg));
    deallocate_sg_list(bound(sg));
    closure_finish();
}

static void enqueue_page_completion_statelocked(pagecache pc, pagecache_page pp, status_handler sh)
{
    /* completions may have been consumed on service */
    if (!pp->completions)
        pp->completions = allocate_vector(pc->h, 4);
    vector_push(pp->completions, sh);
}

static void touch_or_fill_page_nodelocked(pagecache_node pn, pagecache_page pp, merge m)
{
    pagecache_volume pv = pn->pv;
    pagecache pc = pv->pc;
    spin_lock(&pc->state_lock);
    pagecache_debug("%s: pn %p, pp %p, m %p, state %d\n", __func__, pn, pp, m, page_state(pp));
    switch (page_state(pp)) {
    case PAGECACHE_PAGESTATE_READING:
        enqueue_page_completion_statelocked(pc, pp, apply_merge(m));
        break;
    case PAGECACHE_PAGESTATE_ALLOC:
        enqueue_page_completion_statelocked(pc, pp, apply_merge(m));
        change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_READING);
        spin_unlock(&pc->state_lock);

        range r = byte_range_from_page(pc, pp);

        /* issue page reads */
        pagecache_debug("   pc %p, pp %p, r %R, reading...\n", pc, pp, r);
        sg_list sg = allocate_sg_list(); // XXX check
        sg_buf sgb = sg_list_tail_add(sg, cache_pagesize(pc));
        sgb->buf = pp->kvirt;
        sgb->size = cache_pagesize(pc);
        sgb->offset = 0;
        sgb->refcount = &pp->refcount;
        refcount_reserve(sgb->refcount);
        apply(pn->fs_read, sg, r,
              closure(pc->h, pagecache_read_page_complete, pc, pp, sg));
        return;
    case PAGECACHE_PAGESTATE_ACTIVE:
        /* move to bottom of active list */
        list_delete(&pp->l);
        list_insert_before(&pc->active.l, &pp->l);
        break;
    case PAGECACHE_PAGESTATE_NEW:
        /* cache hit -> active */
        change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_ACTIVE);
        break;
    case PAGECACHE_PAGESTATE_WRITING:
    case PAGECACHE_PAGESTATE_DIRTY:
        break;
    default:
        halt("%s: invalid state %d\n", __func__, page_state(pp));
    }
    spin_unlock(&pc->state_lock);
}

define_closure_function(2, 0, void, pagecache_page_free,
                        pagecache, pc, pagecache_page, pp)
{
    pagecache_page pp = bound(pp);
    /* remove from existing list depending on state */
    int state = page_state(pp);
    if (state != PAGECACHE_PAGESTATE_EVICTED)
        halt("%s: pc %p, pp %p, invalid state %d\n", __func__, bound(pc), pp, page_state(pp));

    pagecache pc = bound(pc);
    deallocate(pc->contiguous, pp->kvirt, cache_pagesize(pc));
    u64 pre = fetch_and_add(&pc->total_pages, -1);
    assert(pre > 0);
    pagecache_debug("%s: total pages now %ld\n", __func__, pre - 1);
}

static pagecache_page allocate_page_nodelocked(pagecache_node pn, u64 offset)
{
    /* allocate - later we can look at blocks of pages at a time */
    pagecache pc = pn->pv->pc;
    u64 pagesize = U64_FROM_BIT(pc->page_order);
    void *p = allocate(pc->contiguous, pagesize);
    if (p == INVALID_ADDRESS)
        return INVALID_ADDRESS;

    pagecache_page pp = allocate(pc->h, sizeof(struct pagecache_page));
    if (pp == INVALID_ADDRESS)
        goto fail_dealloc_contiguous;

    init_rbnode(&pp->rbnode);
    init_refcount(&pp->refcount, 1, init_closure(&pp->free, pagecache_page_free, pc, pp));
    assert((offset >> PAGECACHE_PAGESTATE_SHIFT) == 0);
    pp->state_offset = ((u64)PAGECACHE_PAGESTATE_ALLOC << PAGECACHE_PAGESTATE_SHIFT) | offset;
    pp->write_count = 0;
    pp->kvirt = p;
    pp->node = pn;
    pp->l.next = pp->l.prev = 0;
    pp->phys = physical_from_virtual(p);
    pp->completions = 0;
    assert(rbtree_insert_node(&pn->pages, &pp->rbnode));
    fetch_and_add(&pc->total_pages, 1); /* decrement happens without cache lock */
    return pp;
  fail_dealloc_contiguous:
    deallocate(pc->contiguous, p, pagesize);
    return INVALID_ADDRESS;
}

#ifndef PAGECACHE_READ_ONLY
static u64 evict_from_list_locked(pagecache pc, struct pagelist *pl, u64 pages)
{
    u64 evicted = 0;
    list_foreach(&pl->l, l) {
        if (evicted >= pages)
            break;

        pagecache_page pp = struct_from_list(l, pagecache_page, l);
        pagecache_debug("%s: list %s, release pp %R, state %d, count %ld\n", __func__,
                        pl == &pc->new ? "new" : "active", byte_range_from_page(pc, pp),
                        page_state(pp), pp->refcount.c);
        change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_EVICTED);
        rbtree_remove_node(&pp->node->pages, &pp->rbnode);
        refcount_release(&pp->refcount); /* eviction, as far as cache is concerned */
        evicted++;
    }
    return evicted;
}

static void balance_page_lists_locked(pagecache pc)
{
    /* balance active and new lists */
    s64 dp = ((s64)pc->active.pages - (s64)pc->new.pages) / 2;
    pagecache_debug("%s: active %ld, new %ld, dp %ld\n", __func__, pc->active.pages, pc->new.pages, dp);
    list_foreach(&pc->active.l, l) {
        if (dp <= 0)
            break;
        pagecache_page pp = struct_from_list(l, pagecache_page, l);
        /* We don't presently have a notion of "time" in the cache, so
           just cull unreferenced buffers in LRU fashion until active
           pages are equivalent to new...loosely inspired by linux
           approach. */
        if (pp->refcount.c == 1) {
            pagecache_debug("   pp %R -> new\n", byte_range_from_page(pc, pp));
            change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_NEW);
            dp--;
        }
    }
}

static pagecache_page page_lookup_nodelocked(pagecache_node pn, u64 n)
{
    struct pagecache_page k;
    k.state_offset = n;
    return (pagecache_page)rbtree_lookup(&pn->pages, &k.rbnode);
}

static void touch_page_by_num_nodelocked(pagecache_node pn, u64 n, merge m)
{
    pagecache_page pp = page_lookup_nodelocked(pn, n);
    if (pp == INVALID_ADDRESS) {
        pp = allocate_page_nodelocked(pn, n);
        if (pp == INVALID_ADDRESS) {
            apply(apply_merge(m), timm("result", "failed to allocate pagecache_page"));
            return;
        }
    }
    touch_or_fill_page_nodelocked(pn, pp, m);
}

closure_function(5, 1, void, pagecache_write_sg_finish,
                 pagecache_node, pn, range, q, sg_list, sg, status_handler, completion, boolean, complete,
                 status, s)
{
    pagecache_node pn = bound(pn);
    pagecache pc = pn->pv->pc;
    range q = bound(q);
    int page_order = pc->page_order;
    int block_order = pn->pv->block_order;
    u64 pi = q.start >> page_order;
    u64 end = (q.end + MASK(pc->page_order)) >> page_order;
    sg_list sg = bound(sg);

    pagecache_debug("%s: pn %p, q %R, sg %p, complete %d, status %v\n", __func__, pn, q,
                    sg, bound(complete), s);

    spin_lock(&pn->pages_lock);
    pagecache_page pp = page_lookup_nodelocked(pn, pi);
    if (bound(complete)) {
        /* TODO: We handle storage errors after the syscall write
           completion has been applied. This means that storage
           allocation and I/O errors aren't being propagated back to
           the syscalls that caused them and are therefore imprecise.
           For now, we take note of any write error and stash it in
           the volume to be returned on a subsequent call.

           As of now, we do not automatically clear a pending error
           condition after reporting. Some logic will need to be added
           to clear specific conditions and allow the application to
           recover from an error (e.g. test for and clear a pending
           FS_STATUS_NOSPACE after an extent has been deleted).

           This is clearly a stop-gap, meant to prevent endless,
           runaway writes on common conditions like storage
           exhaustion. */

        if (!is_ok(s)) {
            pagecache_debug("%s: write_error now %v\n", __func__, s);
            pn->pv->write_error = s;
        }

        do {
            assert(pp != INVALID_ADDRESS && page_offset(pp) == pi);
            spin_lock(&pc->state_lock);
            assert(pp->write_count > 0);
            if (pp->write_count-- == 1) {
                change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_NEW);
                pagecache_page_queue_completions_locked(pc, pp, s);
            }
            spin_unlock(&pc->state_lock);
            pi++;
            pp = (pagecache_page)rbnode_get_next((rbnode)pp);
        } while (pi < end);
        spin_unlock(&pn->pages_lock);
        closure_finish();
        return;
    }

    /* apply writes, allocating pages as needed */
    u64 offset = q.start & MASK(page_order);
    u64 block_offset = q.start & MASK(block_order);
    range r = irange(q.start & ~MASK(block_order), q.end);
    sg_list write_sg;
    if (sg) {
        write_sg = allocate_sg_list();
        if (write_sg == INVALID_ADDRESS) {
            spin_unlock(&pn->pages_lock);
            apply(bound(completion), timm("result", "failed to allocate write sg"));
            closure_finish();
            return;
        }
    } else {
        write_sg = 0;
    }
    do {
        if (pp == INVALID_ADDRESS || page_offset(pp) > pi) {
            assert(offset == 0 && block_offset == 0); /* should never alloc for unaligned head */
            pp = allocate_page_nodelocked(pn, pi);
            if (pp == INVALID_ADDRESS) {
                spin_unlock(&pn->pages_lock);
                apply(bound(completion), timm("result", "failed to allocate pagecache_page"));
                if (write_sg) {
                    sg_list_release(write_sg);
                    deallocate_sg_list(write_sg);
                }
                closure_finish();
                return;
            }

            /* When writing a new page at the end of a node whose length is not block-aligned, zero
               the remaining portion of the last block. The filesystem will depend on this to properly
               implement file holes. */
            range i = range_intersection(byte_range_from_page(pc, pp), q);
            u64 tail_offset = i.end & MASK(block_order);
            if (tail_offset) {
                u64 page_offset = i.end & MASK(page_order);
                u64 len = U64_FROM_BIT(block_order) - tail_offset;
                pagecache_debug("   zero unaligned end, i %R, page offset 0x%lx, len 0x%lx\n",
                                i, page_offset, len);
                assert(i.end == pn->length);
                zero(pp->kvirt + page_offset, len);
            }
        }
        u64 copy_len = MIN(q.end - (pi << page_order), cache_pagesize(pc)) - offset;
        u64 req_len = pad(copy_len + block_offset, U64_FROM_BIT(block_order));
        if (write_sg) {
            sg_buf sgb = sg_list_tail_add(write_sg, req_len);
            sgb->buf = pp->kvirt;
            sgb->offset = offset - block_offset;
            sgb->size = sgb->offset + req_len;
            sgb->refcount = &pp->refcount;
            refcount_reserve(sgb->refcount);
            u64 res = sg_copy_to_buf(pp->kvirt + offset, sg, copy_len);
            assert(res == copy_len);
        } else {
            zero(pp->kvirt + offset, copy_len);
        }
        spin_lock(&pc->state_lock);
        change_page_state_locked(pc, pp, PAGECACHE_PAGESTATE_WRITING);
        spin_unlock(&pc->state_lock);
        offset = 0;
        block_offset = 0;
        pi++;
        pp = (pagecache_page)rbnode_get_next((rbnode)pp);
    } while (pi < end);
    spin_unlock(&pn->pages_lock);

    /* issue write */
    bound(complete) = true;
    pagecache_debug("   calling fs_write, range %R, sg %p\n", r, write_sg);
    apply(pn->fs_write, write_sg, r, (status_handler)closure_self());
    apply(bound(completion), STATUS_OK);
}

closure_function(1, 3, void, pagecache_write_sg,
                 pagecache_node, pn,
                 sg_list, sg, range, q, status_handler, completion)
{
    pagecache_node pn = bound(pn);
    pagecache_volume pv = pn->pv;
    pagecache pc = pv->pc;
    pagecache_debug("%s: node %p, q %R, sg %p, completion %F\n", __func__, pn, q, sg, completion);

    if (!is_ok(pv->write_error)) {
        /* From a previous (asynchronous) write failure - see comment
           in pagecache_write_sg_finish above */
        pagecache_debug("   pending write error %v\n", __func__, pv->write_error);
        apply(completion, pv->write_error);
        return;
    }

    if (range_span(q) == 0) {
        apply(completion, STATUS_OK);
        return;
    }

    /* extend node length if writing past current end */
    if (q.end > pn->length)
        pn->length = q.end;

    /* prepare pages for writing */
    merge m = allocate_merge(pc->h, closure(pc->h, pagecache_write_sg_finish, pn, q, sg, completion, false));
    status_handler sh = apply_merge(m);

    /* initiate reads for rmw start and/or end */
    u64 start_offset = q.start & MASK(pc->page_order);
    u64 end_offset = q.end & MASK(pc->page_order);
    range r = range_rshift(q, pc->page_order);
    spin_lock(&pn->pages_lock);
    if (start_offset != 0) {
        touch_page_by_num_nodelocked(pn, q.start >> pc->page_order, m);
        r.start++;
    }
    if (end_offset != 0 && (q.end < pn->length) && /* tail rmw */
        !((q.start & ~MASK(pc->page_order)) ==
          (q.end & ~MASK(pc->page_order)) && start_offset != 0) /* no double fill */) {
        touch_page_by_num_nodelocked(pn, q.end >> pc->page_order, m);
    }

    /* scan whole pages, blocking for any pending reads */
    pagecache_page pp = page_lookup_nodelocked(pn, r.start);
    while (pp != INVALID_ADDRESS && page_offset(pp) < r.end) {
        spin_lock(&pc->state_lock);
        if (page_state(pp) == PAGECACHE_PAGESTATE_READING)
            enqueue_page_completion_statelocked(pc, pp, apply_merge(m));
        spin_unlock(&pc->state_lock);
        pp = (pagecache_page)rbnode_get_next((rbnode)pp);
    }
    spin_unlock(&pn->pages_lock);
    apply(sh, STATUS_OK);
}

/* evict pages from new and active lists, then rebalance */
static u64 evict_pages_locked(pagecache pc, u64 pages)
{
    u64 evicted = evict_from_list_locked(pc, &pc->new, pages);
    if (evicted < pages) {
        /* To fill the requested pages evictions, we are more
           aggressive here, evicting even in-use pages (rc > 1) in the
           active list. */
        evicted += evict_from_list_locked(pc, &pc->active, pages - evicted);
    }
    balance_page_lists_locked(pc);
    return evicted;
}

u64 pagecache_drain(pagecache pc, u64 drain_bytes)
{
    u64 pages = pad(drain_bytes, cache_pagesize(pc)) >> pc->page_order;

    /* We could avoid taking both locks here if we keep drained page
       objects around (which incidentally could be useful to keep
       refault data). */

    // XXX TODO This is a race issue on SMP now ... the locking scheme here needs to be rehashed
//    spin_lock(&pc->pages_lock);
    spin_lock(&pc->state_lock);
    u64 evicted = evict_pages_locked(pc, pages);
    spin_unlock(&pc->state_lock);
//    spin_unlock(&pc->pages_lock);
    return evicted << pc->page_order;
}

void pagecache_sync_volume(pagecache_volume pv, status_handler complete)
{
    pagecache_debug("%s: broken, redo!\n", __func__);
    pagecache pc = pv->pc;
    assert(complete);

    pagecache_page pp = 0;
    /* If writes are pending, tack completion onto the mostly recently written page. */
    spin_lock(&pc->state_lock);
    if (!list_empty(&pc->writing.l)) {
        list l = pc->writing.l.prev;
        pp = struct_from_list(l, pagecache_page, l);
        enqueue_page_completion_statelocked(pc, pp, complete);
        spin_unlock(&pc->state_lock);
        return;
    }
    spin_unlock(&pc->state_lock);
    apply(complete, STATUS_OK);
}
#endif /* !PAGECACHE_READ_ONLY */

closure_function(1, 3, void, pagecache_read_sg,
                 pagecache_node, pn,
                 sg_list, sg, range, q, status_handler, completion)
{
    pagecache_node pn = bound(pn);
    pagecache pc = pn->pv->pc;
    pagecache_debug("%s: node %p, q %R, sg %p, completion %F\n", __func__, pn, q, sg, completion);

    merge m = allocate_merge(pc->h, completion);
    status_handler sh = apply_merge(m);
    struct pagecache_page k;
    if (q.end > pn->length)
        q.end = pn->length;
    k.state_offset = q.start >> pc->page_order;
    u64 end = (q.end + MASK(pc->page_order)) >> pc->page_order;
    spin_lock(&pn->pages_lock);
    pagecache_page pp = (pagecache_page)rbtree_lookup(&pn->pages, &k.rbnode);
    for (u64 pi = k.state_offset; pi < end; pi++) {
        if (pp == INVALID_ADDRESS || page_offset(pp) > pi) {
            pp = allocate_page_nodelocked(pn, pi);
            if (pp == INVALID_ADDRESS) {
                spin_unlock(&pn->pages_lock);
                apply(apply_merge(m), timm("result", "failed to allocate pagecache_page"));
                return;
            }
        }

        range r = byte_range_from_page(pc, pp);
        range i = range_intersection(q, r);
        u64 length = range_span(i);
        sg_buf sgb = sg_list_tail_add(sg, length);
        sgb->buf = pp->kvirt + (i.start - r.start);
        sgb->size = length;
        sgb->offset = 0;
        sgb->refcount = &pp->refcount;
        refcount_reserve(&pp->refcount);

        touch_or_fill_page_nodelocked(pn, pp, m);
        pp = (pagecache_page)rbnode_get_next((rbnode)pp);
    }
    spin_unlock(&pn->pages_lock);

    /* finished issuing requests */
    apply(sh, STATUS_OK);
}

closure_function(1, 1, boolean, pagecache_page_print_key,
                 pagecache, pc,
                 rbnode, n)
{
    rprintf(" 0x%lx", page_offset((pagecache_page)n) << cache_pagesize(bound(pc)));
    return true;
}

closure_function(0, 2, int, pagecache_page_compare,
                 rbnode, a, rbnode, b)
{
    u64 oa = page_offset((pagecache_page)a);
    u64 ob = page_offset((pagecache_page)b);
    return oa == ob ? 0 : (oa < ob ? -1 : 1);
}

void pagecache_set_node_length(pagecache_node pn, u64 length)
{
    pn->length = length;
}

void pagecache_deallocate_node(pagecache_node pn)
{
    /* TODO: We probably need to add a refcount to the node with a
       reference for every page in the cache. This would need to:

       - prevent issuing of new operations
       - flush for node
       - drain all pages of this node from the cache
       - finally delete after the last refcount release

       For now, we're leaking nodes for files that get deleted and log
       extensions that get retired.
    */
}

sg_io pagecache_node_get_reader(pagecache_node pn)
{
    return pn->cache_read;
}

sg_io pagecache_node_get_writer(pagecache_node pn)
{
    return pn->cache_write;
}

pagecache_node pagecache_allocate_node(pagecache_volume pv, sg_io fs_read, sg_io fs_write)
{
    heap h = pv->pc->h;
    pagecache_node pn = allocate(h, sizeof(struct pagecache_node));
    if (pn == INVALID_ADDRESS)
        return pn;
    list_insert_before(&pv->nodes, &pn->l);
    pn->pv = pv;
    spin_lock_init(&pn->pages_lock);
    init_rbtree(&pn->pages, closure(h, pagecache_page_compare),
                closure(h, pagecache_page_print_key, pv->pc));
    pn->length = 0;
    pn->cache_read = closure(h, pagecache_read_sg, pn);
#ifndef PAGECACHE_READ_ONLY
    pn->cache_write = closure(h, pagecache_write_sg, pn);
#else
    pn->cache_write = 0;
#endif
    pn->fs_read = fs_read;
    pn->fs_write = fs_write;
    return pn;
}

// XXX TODO
void pagecache_sync_node(pagecache_node pn, status_handler sh)
{
    /* nop */
    apply(sh, STATUS_OK);
}

void *pagecache_get_zero_page(pagecache pc)
{
    return pc->zero_page;
}

int pagecache_get_page_order(pagecache pc)
{
    return pc->page_order;
}

pagecache_volume pagecache_allocate_volume(pagecache pc, u64 length, int block_order)
{
    pagecache_volume pv = allocate(pc->h, sizeof(struct pagecache_volume));
    if (pv == INVALID_ADDRESS)
        return pv;
    pv->pc = pc;
    list_insert_before(&pc->volumes, &pv->l);
    list_init(&pv->nodes);
    pv->length = length;
    pv->block_order = block_order;
    pv->write_error = STATUS_OK;
    return pv;
}

void pagecache_dealloc_volume(pagecache_volume pv)
{
    list_delete(&pv->l);
    deallocate(pv->pc->h, pv, sizeof(*pv));
}

static inline void page_list_init(struct pagelist *pl)
{
    list_init(&pl->l);
    pl->pages = 0;
}

pagecache allocate_pagecache(heap general, heap contiguous, u64 pagesize)
{
    pagecache pc = allocate(general, sizeof(struct pagecache));
    if (pc == INVALID_ADDRESS)
        return pc;

    pc->total_pages = 0;
    pc->page_order = find_order(pagesize);
    assert(pagesize == U64_FROM_BIT(pc->page_order));
    pc->h = general;
    pc->contiguous = contiguous;
    pc->zero_page = allocate_zero(contiguous, pagesize);
    if (pc->zero_page == INVALID_ADDRESS) {
        msg_err("failed to allocate zero page\n");
        deallocate(general, pc, sizeof(struct pagecache));
        return INVALID_ADDRESS;
    }

    spin_lock_init(&pc->state_lock);
    page_list_init(&pc->free);
    page_list_init(&pc->new);
    page_list_init(&pc->active);
    page_list_init(&pc->writing);
    page_list_init(&pc->dirty);
    list_init(&pc->volumes);

#ifdef STAGE3
    pc->completion_vecs = allocate_queue(general, MAX_PAGE_COMPLETION_VECS);
    assert(pc->completion_vecs != INVALID_ADDRESS);
    pc->service_completions = closure(general, pagecache_service_completions, pc);
    pc->service_enqueued = false;
#endif
    return pc;
}
