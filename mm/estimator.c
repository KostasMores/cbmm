/*
 * Implementation of cost-benefit based memory management.
 */

#include <linux/printk.h>
#include <linux/mm_econ.h>
#include <linux/mm.h>
#include <linux/kobject.h>
#include <linux/init.h>
#include <linux/hashtable.h>
#include <linux/mm_stats.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/task.h>
#include <linux/rwsem.h>

#define HUGE_PAGE_ORDER 9

#define MMAP_FILTER_BUF_SIZE 4096
#define MMAP_FILTER_BUF_DEAD_ZONE 128

///////////////////////////////////////////////////////////////////////////////
// Globals...

// Modes:
// - 0: off (just use default linux behavior)
// - 1: on (cost-benefit estimation)
static int mm_econ_mode = 0;

// Turns on various debugging printks...
int mm_econ_debugging_mode = 0;

// Number of cycles per unit time page allocator zone lock is NOT held.
// In this case, the unit time is 10ms because that is the granularity async
// zero daemon uses.
static u64 mm_econ_contention_ms = 10;

// Set this properly via the sysfs file.
static u64 mm_econ_freq_mhz = 3000;

// The Preloaded Profile, if any.
struct profile_range {
    u64 start;
    u64 end;
    // The benefit depends on what the profile is measuring
    u64 benefit;

    struct rb_node node;
};

// The policy the filter applies to
enum mm_policy {
    PolicyHugePage,
    PolicyEagerPage
};

// The operator to use when deciding if quantity from an mmap matches
// the filter.
enum mmap_comparator {
    CompEquals,
    CompGreaterThan,
    CompLessThan
};

// The different quantities that can be compared in an mmap
enum mmap_quantity {
    QuantSectionOff,
    QuantAddr,
    QuantLen,
    QuantProt,
    QuantFlags,
    QuantFD,
    QuantOff
};

// A comaprison for filtering an mmap with and how to compare the quantity
struct mmap_comparison {
    struct list_head node;
    enum mmap_quantity quant;
    enum mmap_comparator comp;
    u64 val;
};

// A list of quantities of a mmap to use for deciding if that mmap would
// benefit from being huge.
struct mmap_filter {
    struct list_head node;
    enum mm_memory_section section;
    enum mm_policy policy;
    u64 benefit;
    struct list_head comparisons;
};

// A process using mmap filters
struct mmap_filter_proc {
    struct list_head node;
    pid_t pid;
    struct list_head filters;
    struct rb_root hp_ranges_root;
    struct rb_root eager_ranges_root;
};

// List of processes using mmap filters
static LIST_HEAD(filter_procs);
static DECLARE_RWSEM(filter_procs_sem);

// The TLB misses estimator, if any.
static mm_econ_tlb_miss_estimator_fn_t tlb_miss_est_fn = NULL;

// Some stats...

// Number of estimates made.
static u64 mm_econ_num_estimates = 0;
// Number of decisions made.
static u64 mm_econ_num_decisions = 0;
// Number of decisions that are "yes".
static u64 mm_econ_num_decisions_yes = 0;
// Number of huge page promotions in #PFs.
static u64 mm_econ_num_hp_promotions = 0;
// Number of times we decided to run async compaction.
static u64 mm_econ_num_async_compaction = 0;
// Number of times we decided to run async prezeroing.
static u64 mm_econ_num_async_prezeroing = 0;
// Number of allocated bytes for various data structures.
static u64 mm_econ_vmalloc_bytes = 0;

extern inline struct task_struct *extern_get_proc_task(const struct inode *inode);

///////////////////////////////////////////////////////////////////////////////
// Actual implementation
//
// There are two possible estimators:
// 1. kbadgerd (via tlb_miss_est_fn).
// 2. A pre-loaded profile (via preloaded_profile).
//
// In both cases, the required units are misses/huge-page/LTU.

// A wrapper around vmalloc to keep track of allocated memory.
static void *mm_econ_vmalloc(unsigned long size)
{
    void *mem = vmalloc(size);

    if (mem)
        mm_econ_vmalloc_bytes += size;

    return mem;
}

static void mm_econ_vfree(const void *addr, unsigned long size)
{
    mm_econ_vmalloc_bytes -= size;
    vfree(addr);
}

void register_mm_econ_tlb_miss_estimator(
        mm_econ_tlb_miss_estimator_fn_t f)
{
    BUG_ON(!f);
    tlb_miss_est_fn = f;
    pr_warn("mm: registered TLB miss estimator %p\n", f);
}
EXPORT_SYMBOL(register_mm_econ_tlb_miss_estimator);

/*
 * Find the profile of a process by PID, if any.
 *
 * Caller must hold `filter_procs_sem` in either read or write mode.
 */
static struct mmap_filter_proc *
find_filter_proc_by_pid(pid_t pid)
{
    struct mmap_filter_proc *proc;
    list_for_each_entry(proc, &filter_procs, node) {
        if (proc->pid == pid) {
            return proc;
        }
    }

    return NULL;
}

inline bool mm_process_is_using_cbmm(pid_t pid)
{
    return find_filter_proc_by_pid(pid) != NULL;
}

/*
 * Search the profile for the range containing the given address, and return
 * it. Otherwise, return NULL.
 */
static struct profile_range *
profile_search(struct rb_root *ranges_root, u64 addr)
{
    struct rb_node *node = ranges_root->rb_node;

    while (node) {
        struct profile_range *range =
            container_of(node, struct profile_range, node);

        if (range->start <= addr && addr < range->end)
            return range;

        if (addr < range->start)
            node = node->rb_left;
        else
            node = node->rb_right;
    }

    return NULL;
}

/*
 * Search the tree for the first range that satisfies the condition
 * of "there exists some address x in range s.t. x <comp> addr."
 * This is only used for filter comparisons on the section_off quantity
 */
static struct profile_range *
profile_find_first_range(struct rb_root *ranges_root, u64 addr,
        enum mmap_comparator comp)
{
    struct profile_range *result = NULL;
    struct profile_range *range = NULL;
    struct rb_node *node = ranges_root->rb_node;

    // First find any range that satisfies the condition
    while (node) {
        range = container_of(node, struct profile_range, node);

        if (comp == CompLessThan) {
            if (range->start < addr) {
                result = range;
                break;
            } else {
                node = node->rb_left;
            }
        } else if (comp == CompGreaterThan) {
            if (range->end > addr) {
                result = range;
                break;
            } else {
                node = node->rb_right;
            }
        } else if (comp == CompEquals) {
            if (range->start <= addr && addr < range->end) {
                // Since only ranges do not overlap, we just need
                // to find one range that overlaps with addr
                return range;
            } else if (range->start < addr) {
                node = node->rb_right;
            } else {
                node = node->rb_left;
            }
        } else {
            return NULL;
        }
    }

    if (!node)
        return NULL;

    while (node) {
        range = container_of(node, struct profile_range, node);

        if (comp == CompLessThan) {
            if (range->start >= addr)
                break;

            result = range;
            node = rb_next(node);
        } else if (comp == CompGreaterThan) {
            if (range->end <= addr)
                break;

            result = range;
            node = rb_prev(node);
        }
    }

    return result;
}

static inline bool
ranges_overlap(struct profile_range *r1, struct profile_range *r2)
{
    return (((r1->start <= r2->start && r2->start < r1->end)
        || (r2->start <= r1->start && r1->start < r2->end)));
}

/*
 * Remove all ranges overlapping with the new range
 */
static void remove_overlapping_ranges(struct rb_root *ranges_root,
    struct profile_range *new_range)
{
    struct rb_node *node = ranges_root->rb_node;
    struct rb_node *first_overlapping = NULL;
    struct rb_node *next;
    struct profile_range *cur_range;

    // First, find the earliest range that overlaps with the new range, if there is any
    while (node) {
        cur_range = container_of(node, struct profile_range, node);


        if (ranges_overlap(new_range, cur_range)) {
            first_overlapping = node;
            // We've found one node that overlaps, but keep going to see if we
            // can find an earlier one
            node = node->rb_left;
            continue;
        }

        if (new_range->start < cur_range->start)
            node = node->rb_left;
        else
            node = node->rb_right;
    }

    // If no overlapping range exists, we're done
    if (!first_overlapping)
        return;

    // Now we can delete all of the overlapping ranges
    node = first_overlapping;
    next = rb_next(node);
    cur_range = container_of(node, struct profile_range, node);
    while (ranges_overlap(new_range, cur_range)) {
        rb_erase(node, ranges_root);
        mm_econ_vfree(cur_range, sizeof(struct profile_range));

        if (!next)
            break;

        node = next;
        next = rb_next(node);
        cur_range = container_of(node, struct profile_range, node);
    }
}

/*
 * Insert the given range into the profile.
 * If the new range overlaps with any existing ranges, delete the
 * existing ones as must have been unmapped.
 */
static void
profile_range_insert(struct rb_root *ranges_root, struct profile_range *new_range)
{
    struct rb_node **new = &(ranges_root->rb_node), *parent = NULL;

    remove_overlapping_ranges(ranges_root, new_range);

    while (*new) {
        struct profile_range *this =
            container_of(*new, struct profile_range, node);

        parent = *new;
        if (new_range->start < this->start)
            new = &((*new)->rb_left);
        else if (new_range->start > this->start)
            new = &((*new)->rb_right);
        else
            break;
    }

    rb_link_node(&new_range->node, parent, new);
    rb_insert_color(&new_range->node, ranges_root);
}

/*
 * Move the ranges in one rb_tree to another
 */
static void
profile_move(struct rb_root *src, struct rb_root *dst)
{
    struct profile_range *range;
    struct rb_node *node = src->rb_node;

    while (node) {
        range = container_of(node, struct profile_range, node);

        // Remove the entry from the source
        rb_erase(node, src);

        // Add the entry to the destination
        profile_range_insert(dst, range);

        node = src->rb_node;
    }
}

static void
profile_free_all(struct rb_root *ranges_root)
{
    struct rb_node *node = ranges_root->rb_node;

    while(node) {
        struct profile_range *range =
            container_of(node, struct profile_range, node);

        rb_erase(node, ranges_root);
        node = ranges_root->rb_node;

        mm_econ_vfree(range, sizeof(struct profile_range));
    }
}

static void mmap_filters_free_all(struct mmap_filter_proc *proc)
{
    struct mmap_filter *filter;
    struct mmap_comparison *comparison;
    struct list_head *pos, *n;
    struct list_head *cPos, *cN;

    list_for_each_safe(pos, n, &proc->filters) {
        filter = list_entry(pos, struct mmap_filter, node);

        // Free each comparison in this filter
        list_for_each_safe(cPos, cN, &filter->comparisons) {
            comparison = list_entry(cPos, struct mmap_comparison, node);
            list_del(cPos);
            mm_econ_vfree(comparison, sizeof(struct mmap_comparison));
        }

        list_del(pos);
        mm_econ_vfree(filter, sizeof(struct mmap_filter));
    }
}

enum free_huge_page_status {
    fhps_none, // no free huge pages
    fhps_free, // huge pages are available
    fhps_zeroed, // huge pages are available and prezeroed!
};

static enum free_huge_page_status
have_free_huge_pages(void)
{
    int zone_idx;
    struct zone *zone;
    struct page *page;
    struct free_area *area;
    bool is_free = false, is_zeroed = false;
    int order;
    unsigned long flags;

    pg_data_t *pgdat = NODE_DATA(numa_node_id());
    for (zone_idx = ZONE_NORMAL; zone_idx < MAX_NR_ZONES; zone_idx++) {
        zone = &pgdat->node_zones[zone_idx];

        for (order = HUGE_PAGE_ORDER; order < MAX_ORDER; ++order) {
            area = &(zone->free_area[order]);
            is_free = area->nr_free > 0;

            if (is_free) {
                spin_lock_irqsave(&zone->lock, flags);

                page = list_last_entry_or_null(
                        &area->free_list[MIGRATE_MOVABLE], struct page,
                        lru);
                is_zeroed = page && PageZeroed(page);

                spin_unlock_irqrestore(&zone->lock, flags);

                if (mm_econ_debugging_mode == 1) {
                    pr_warn("estimator: found "
                            "free page %p node %d zone %p (%s) "
                            "order %d prezeroed %d list %d",
                            page, zone->zone_pgdat->node_id,
                            zone, zone->name, order,
                            is_zeroed, MIGRATE_MOVABLE);
                }

                goto exit;
            }
        }
    }

exit:
    return is_zeroed ? fhps_zeroed :
        is_free ? fhps_free :
        fhps_none;
}

static u64
compute_hpage_benefit_from_profile(
        const struct mm_action *action)
{
    u64 ret = 0;
    struct mmap_filter_proc *proc;
    struct profile_range *range = NULL;

    down_read(&filter_procs_sem);
    if ((proc = find_filter_proc_by_pid(current->tgid))) // NOTE: assignment
        range = profile_search(&proc->hp_ranges_root, action->address);

    if (range) {
        ret = range->benefit;

        //pr_warn("mm_econ: estimating page benefit: "
        //        "misses=%llu size=%llu per-page=%llu\n",
        //        range->benefit,
        //        (range->end - range->start) >> HPAGE_SHIFT,
        //        ret);
    }
    up_read(&filter_procs_sem);

    return ret;
}

static u64
compute_hpage_benefit(const struct mm_action *action)
{
    if (tlb_miss_est_fn)
        return tlb_miss_est_fn(action);
    else
        return compute_hpage_benefit_from_profile(action);
}

static void
compute_eager_page_benefit(const struct mm_action *action, struct mm_cost_delta *cost)
{
    u64 benefit = 0;
    u64 start, end;
    int range_count = 0;
    struct mmap_filter_proc *proc;
    struct profile_range *first_range = NULL;
    struct profile_range *range = NULL;
    struct rb_node *node = NULL;
    struct range *ranges = NULL;

    start = action->address;
    end = action->address + action->len;

    down_read(&filter_procs_sem);
    // First find the first range with an address g.t. the given address
    if ((proc = find_filter_proc_by_pid(current->tgid))) { // NOTE: assignment
        first_range = profile_find_first_range(&proc->eager_ranges_root,
            start, CompGreaterThan);
    }
    if (!first_range)
        goto out;

    node = &first_range->node;

    // Count up all the ranges that have greater benefit than cost so we
    // know how big of an array to allocate later
    while (node) {
        range = container_of(node, struct profile_range, node);

        if (start >= range->end || end <= range->start)
            break;

        if (range->benefit > cost->cost)
            range_count++;

        node = rb_next(node);
    }

    if (range_count == 0)
        goto out;

    // +1 for the ending signal
    cost->extra = 0;
    ranges = vmalloc(sizeof(struct range) * (range_count + 1));
    if (!ranges)
        goto out;

    // Fill in the list of ranges to page in
    range_count = 0;
    node = &first_range->node;
    while (node) {
        range = container_of(node, struct profile_range, node);

        if (start >= range->end || end <= range->start)
            break;

        if (range->benefit > cost->cost) {
            ranges[range_count].start = range->start;
            ranges[range_count].end = range->end;

            if (range->benefit > benefit)
                benefit = range->benefit;

            range_count++;
        }

        node = rb_next(node);
    }
    // Hacky: Just use -1 in the start and end to signal the end of the list
    ranges[range_count].start = ranges[range_count].end = -1;

    // Pass the list of ranges to promote to the decider in the extra field
    cost->extra = (u64)ranges;
out:
    up_read(&filter_procs_sem);

    cost->benefit = benefit;
}

// Estimate cost/benefit of a huge page promotion for the current process.
void
mm_estimate_huge_page_promote_cost_benefit(
       const struct mm_action *action, struct mm_cost_delta *cost)
{
    // Estimated cost.
    //
    // For now, we hard-code a bunch of stuff, and we make a lot of
    // assumptions. We can relax these assumptions later if we need to.

    // TODO: Assume allocation is free if we have free huge pages.
    // TODO: Assume we don't care what node it is on...
    // TODO: Maybe account for opportunity cost as rate/ratio?
    const enum free_huge_page_status fhps = have_free_huge_pages();
    const u64 alloc_cost = fhps > fhps_none ? 0 : (1ul << 32);

    // TODO: Assume constant prep costs (zeroing or copying).
    const u64 prep_cost = fhps > fhps_free ? 0 : 100 * 2000; // ~100us

    // Compute total cost.
    cost->cost = alloc_cost + prep_cost;
    cost->extra = fhps == fhps_zeroed;

    // Estimate benefit.
    cost->benefit = compute_hpage_benefit(action);
}

// Update the given cost/benefit to also account for reclamation of a huge
// page. This assumes that there is already a cost/benefit in `cost`.
void
mm_estimate_huge_page_reclaim_cost(
       const struct mm_action *action, struct mm_cost_delta *cost)
{
    // TODO(markm): for now just assume it is very expensive. We might want to
    // do something more clever later. For example, we can look at the amount
    // of fragmentation or the amount of free memory. If we are heavily
    // fragmented and under memory pressure, then reclaim will be expensive.
    const u64 reclaim_cost = 1000000000; // ~hundreds of ms

    cost->cost += reclaim_cost;
}

// Estimate the cost of running a daemon. In general, this is just the time
// that the daemon runs unless the system is idle -- idle time is considered
// free to consume.
void
mm_estimate_daemon_cost(
       const struct mm_action *action, struct mm_cost_delta *cost)
{
    // FIXME(markm): for now we just use the average system load on all cores
    // because this is easy and cheap. However, we can get something more
    // precise by looking at the number of currently running tasks on only
    // local cores or something like that...
    //
    // nrunning = 0;
    // for_each_cpu_and(cpu, cpumask_of_node(node), cpu_online_mask)
    //   nrunning += cpu_rq(cpu)->nr_running;
    //
    // if (nrunning < ncpus_local)
    //   cost = 0;
    // else
    //   cost = time_to_run;

    const u64 huge_page_zeroing_cost = 1000000;

    __kernel_ulong_t loads[3]; /* 1, 5, and 15 minute load averages */
    int ncpus = num_online_cpus();

    get_avenrun(loads, FIXED_1/200, 0);

    // If we have more cpus than load, running a background daemon is free.
    // Otherwise, the cost is however many cycles the daemon runs, as this is
    // time that is taken away from applications.
    if (ncpus > LOAD_INT(loads[0])) {
        cost->cost = 0;
    } else {
        switch (action->action) {
            case MM_ACTION_RUN_PREZEROING:
                cost->cost = huge_page_zeroing_cost * action->prezero_n;
                break;

            case MM_ACTION_RUN_DEFRAG:
            case MM_ACTION_RUN_PROMOTION:
                // TODO(markm): this should be however long the daemon runs
                // for, which means we need to cap the run time. There are also
                // costs for copying pages and scanning.
                //
                // For now, we just make these really expensive.
                cost->cost = 1ul << 32; // >1s
                break;

            default: // Not a daemon...
                BUG();
                return;
        }
    }
}

// Estimate the benefit of prezeroing memory based on the rate of usage of
// zeroed pages so far.
void mm_estimate_async_prezeroing_benefit(
       const struct mm_action *action, struct mm_cost_delta *cost)
{
    // FIXME(markm): we assume that the cost to zero a 2MB region is about 10^6
    // cycles. This is based on previous measurements we've made.
    const u64 zeroing_per_page_cost = 1000000; // cycles

    // TODO: we want to scale down the benefit to 10ms instead of 1 LTU, I think...

    // The maximum amount of benefit is based on the number of pages we
    // actually zero and actually use. That is, we don't benefit from zeroed
    // pages that are not used, and we do not benefit from unzeroed pages.
    //
    // We will zero no more than `action->prezero_n` pages, and we will use (we
    // estimate) no more than `recent_used` pages, so the benefit is capped at
    // the minimum of these. The `recent_used` is the estimated number of pages
    // used recently.
    const u64 recent_used = mm_estimated_prezeroed_used();

    cost->benefit = min(action->prezero_n, recent_used) * zeroing_per_page_cost;
}

// Estimate the cost of lock contention due to prezeroing.
//
// During the LTU, we can grab the lock at times when it would otherwise be
// idle for free. If we assume that the critical section of the async
// prezeroing is about 150 cycles (to acquire/release and add/remove from
// linked list), then we get the number of times per LTU we can do prezeroing
// for free.
//
// We can then discount action->prezero_n operations by the number of free
// items and expense the rest at the cost of the critical section.
void mm_estimate_async_prezeroing_lock_contention_cost(
       const struct mm_action *action, struct mm_cost_delta *cost)
{
    const u64 critical_section_cost = 150 * 2; // cycles
    const u64 nfree = mm_econ_contention_ms * mm_econ_freq_mhz * 1000
                      / critical_section_cost;

    cost->cost += (action->prezero_n > nfree ? action->prezero_n - nfree  : 0)
                    * critical_section_cost;
}

// Estimate the cost of eagerly allocating a page
void mm_estimate_eager_page_cost_benefit(
        const struct mm_action *action, struct mm_cost_delta *cost)
{
    // Based on our measurements of page fault latency, almost all of the base page
    // faults take less than 10us, so convert that to cycles and use that for the
    // cost.
    // We do not have to consider the cost of faulting in a huge page, since that
    // will be handled by the huge page cost/benefit logic
    cost->cost = mm_econ_freq_mhz * 10;
    // Populates cost->benefit and cost->extra
    compute_eager_page_benefit(action, cost);
}

bool mm_econ_is_on(void)
{
    return mm_econ_mode > 0;
}
EXPORT_SYMBOL(mm_econ_is_on);

// Estimates the change in the given metrics under the given action. Updates
// the given cost struct in place.
//
// Note that this is a pure function! It should not keep state regarding to
// previous queries.
void
mm_estimate_changes(const struct mm_action *action, struct mm_cost_delta *cost)
{
    switch (action->action) {
        case MM_ACTION_NONE:
            cost->cost = 0;
            cost->benefit = 0;
            break;

        case MM_ACTION_PROMOTE_HUGE:
            mm_estimate_huge_page_promote_cost_benefit(action, cost);
            break;

        case MM_ACTION_DEMOTE_HUGE:
            // TODO(markm)
            cost->cost = 0;
            cost->benefit = 0;
            break;

        case MM_ACTION_RUN_DEFRAG:
            mm_estimate_daemon_cost(action, cost);
            cost->benefit = 0; // TODO(markm)
            if (cost->cost < cost->benefit)
                mm_econ_num_async_compaction += 1;
            break;

        case MM_ACTION_RUN_PROMOTION:
            mm_estimate_daemon_cost(action, cost);
            // TODO(markm)
            cost->benefit = 0;
            break;

        case MM_ACTION_RUN_PREZEROING:
            mm_estimate_daemon_cost(action, cost);
            mm_estimate_async_prezeroing_lock_contention_cost(action, cost);
            mm_estimate_async_prezeroing_benefit(action, cost);
            if (cost->cost < cost->benefit)
                mm_econ_num_async_prezeroing += 1;
            break;

        case MM_ACTION_ALLOC_RECLAIM: // Alloc reclaim for thp allocation.
            // Estimate the cost/benefit of the promotion itself.
            mm_estimate_huge_page_promote_cost_benefit(action, cost);
            // Update the cost if we also need to do reclaim.
            mm_estimate_huge_page_reclaim_cost(action, cost);
            break;

        case MM_ACTION_EAGER_PAGING:
            mm_estimate_eager_page_cost_benefit(action, cost);
            break;

        default:
            printk(KERN_WARNING "Unknown mm_action %d\n", action->action);
            break;
    }

    // Record some stats for debugging.
    mm_econ_num_estimates += 1;
    mm_stats_hist_measure(&mm_econ_cost, cost->cost);
    mm_stats_hist_measure(&mm_econ_benefit, cost->benefit);

    if (mm_econ_debugging_mode == 2) {
        pr_warn("estimator: action=%d cost=%llu benefit=%llu",
                action->action, cost->cost, cost->benefit);
    }
}
EXPORT_SYMBOL(mm_estimate_changes);

// Decide whether to take an action with the given cost. Returns true if the
// action associated with `cost` should be TAKEN, and false otherwise.
bool mm_decide(const struct mm_cost_delta *cost)
{
    bool should_do;
    mm_econ_num_decisions += 1;

    if (mm_econ_mode == 0) {
        return true;
    } else if (mm_econ_mode == 1) {
        should_do = cost->benefit > cost->cost;

        if (should_do)
            mm_econ_num_decisions_yes += 1;

        //pr_warn("mm_econ: cost=%llu benefit=%llu\n", cost->cost, cost->benefit); // TODO remove
        return should_do;
    } else {
        BUG();
        return false;
    }
}
EXPORT_SYMBOL(mm_decide);

// Inform the estimator of the promotion of the given huge page.
void mm_register_promotion(u64 addr)
{
    mm_econ_num_hp_promotions += 1;
}

static bool mm_does_quantity_match(struct mmap_comparison *c, u64 val)
{
    if (c->comp == CompEquals) {
        return val == c->val;
    } else if (c->comp == CompGreaterThan) {
        return val > c->val;
    } else if (c->comp == CompLessThan) {
        return val < c->val;
    } else {
        pr_err("Invalid mmap comparatori\n");
        BUG();
    }

    // Should never reach here
    return false;
}

// Split base_range, which is in subranges, at addr based on comp and add
// the new range(s) to subranges.
static bool mm_split_ranges(struct profile_range *base_range, struct rb_root *subranges,
        u64 addr, enum mmap_comparator comp)
{
    struct profile_range *split_range;

    if (comp == CompGreaterThan) {
        if (base_range->start >= addr) {
            return true;
        }

        split_range = mm_econ_vmalloc(sizeof(struct profile_range));
        if (!split_range)
            return false;

        split_range->benefit = 0;
        split_range->start = base_range->start;
        split_range->end = addr;
        base_range->start = addr;

        profile_range_insert(subranges, split_range);
    } else if (comp == CompLessThan) {
        if (base_range->end <= addr) {
            return true;
        }

        split_range = mm_econ_vmalloc(sizeof(struct profile_range));
        if (!split_range)
            return false;

        split_range->benefit = 0;
        split_range->start = addr;
        split_range->end = base_range->end;
        base_range->end = addr;

        profile_range_insert(subranges, split_range);
    } else if (comp == CompEquals) {
        // Do we need to split on the left?
        if (base_range->start < addr) {
            split_range = mm_econ_vmalloc(sizeof(struct profile_range));
            if (!split_range)
                return false;

            split_range->benefit = 0;
            split_range->start = base_range->start;
            split_range->end = addr;
            base_range->start = addr;

            profile_range_insert(subranges, split_range);
        }
        // Do we need to split on the right?
        if (base_range->end > addr + PAGE_SIZE) {
            split_range = mm_econ_vmalloc(sizeof(struct profile_range));
            if (!split_range)
                return false;

            split_range->benefit = 0;
            split_range->start = addr + PAGE_SIZE;
            split_range->end = base_range->end;
            base_range->end = addr + PAGE_SIZE;

            profile_range_insert(subranges, split_range);
        }
    }

    return true;
}

static int mm_copy_profile_range(
        struct rb_root* old_root, struct rb_root* new_root)
{
    struct rb_node *node = NULL;
    struct profile_range *range = NULL;
    struct profile_range *new_range = NULL;

    node = rb_first(old_root);
    while (node) {
        new_range = mm_econ_vmalloc(sizeof(struct profile_range));
        if (!new_range)
            return -1;

        range = container_of(node, struct profile_range, node);
        new_range->start = range->start;
        new_range->end = range->end;
        new_range->benefit = range->benefit;

        profile_range_insert(new_root, new_range);

        node = rb_next(node);
    }

    return 0;
}

// Search mmap_filters for a filter that matches this new memory map
// and add it to the list of ranges.
// pid: The pid of the process who made this mmap
// section: The memory section the memory range belongs to: code, data, heap, or mmap
// mapaddr: The actual address the new mmap is mapped to
// section_off: The offset of the memory range from the start of the section it belongs to
// addr: The hint from the caller for what address the new mmap should be mapped to
// len: The length of the new mmap
// prot: The protection bits for the mmap
// flags: The flags specified in the mmap call
// fd: Descriptor of the file to map
// off: Offset within the file to start the mapping
// Do we need to lock mmap_filters?
// We might need to lock the profile_ranges rb_tree
void mm_add_memory_range(pid_t pid, enum mm_memory_section section, u64 mapaddr, u64 section_off,
        u64 addr, u64 len, u64 prot, u64 flags, u64 fd, u64 off)
{
    struct mmap_filter_proc *proc;
    struct mmap_filter *filter;
    struct mmap_comparison *comp;
    struct profile_range *range = NULL;
    struct list_head *filter_head = NULL;
    // Used to keep track of the subranges of the new memory range that are
    // from splitting a range due to a addr or section_off constraint.
    struct rb_root huge_subranges = RB_ROOT;
    struct rb_root eager_subranges = RB_ROOT;
    struct rb_node *range_node = NULL;
    bool passes_filter;
    u64 val;

    // If this isn't the process we care about, move on
    down_read(&filter_procs_sem);
    proc = find_filter_proc_by_pid(pid);
    up_read(&filter_procs_sem);

    if (!proc)
        return;

    filter_head = &proc->filters;

    // Start with the original range of the new mapping
    range = mm_econ_vmalloc(sizeof(struct profile_range));
    if (!range) {
        pr_warn("mm_add_memory_range: no memory for new range");
        return;
    }
    // Align the range bounds to a page
    range->start = mapaddr & PAGE_MASK;
    range->end = (mapaddr + len + PAGE_SIZE - 1) & PAGE_MASK;
    range->benefit = 0;
    profile_range_insert(&huge_subranges, range);

    if (mm_copy_profile_range(&huge_subranges, &eager_subranges) != 0) {
        pr_warn("mm_add_memory_range: no memory for new range");
        return;
    }

    // Check if this mmap matches any of our filters
    down_read(&filter_procs_sem);
    list_for_each_entry(filter, filter_head, node) {
        // Each filter only applies to either the eager or huge page policy
        // This variable points to the applicable subranges tree
        struct rb_root *subranges = NULL;
        // We need a second rb_tree because we don't want to change the
        // subranges tree unless we are sure a filter matches
        struct rb_root temp_subranges = RB_ROOT;
        // The range in the subranges tree that we are splitting
        struct profile_range *parent_range = NULL;

        if (filter->policy == PolicyHugePage) {
            subranges = &huge_subranges;
        } else if (filter->policy == PolicyEagerPage) {
            subranges = &eager_subranges;
        } else {
            BUG();
        }

        passes_filter = section == filter->section;

        list_for_each_entry(comp, &filter->comparisons, node) {
            if (!passes_filter)
                break;

            // Determine the value to use for this comparison
            if (comp->quant == QuantSectionOff || comp->quant == QuantAddr) {
                // This type of filter comparison is the most complex because
                // it may cause the region to be split one or more times.
                // This happens when the new region overlaps with multiple filters.
                // To handle this case, while we check if the region matches the
                // filter, we also keep track of how we would need to split the
                // regions using temp_subregions. These subregions then replace
                // the larger region if the filter passes the region.

                enum mmap_comparator comparator;
                u64 section_base;
                u64 search_key;
                // Because ranges can be split, we need to handle this more
                // carefully.

                // Find the range to do the comparison on
                // If the comparator is Addr, this is straight forward.
                // Otherwise, this step basically involves converting the section offset
                // given in the filter to a virtual address corresponding to
                // that offset. We need to do this because the memory ranges
                // we are operating on are virtual addresses.
                // We need to account for the mmap section growing down
                if (comp->quant == QuantAddr) {
                    search_key = comp->val;
                    comparator = comp->comp;
                } else if (section == SectionMmap) {
                    section_base = mapaddr + section_off;
                    search_key = section_base - comp->val;

                    if (comp->comp == CompGreaterThan)
                        comparator = CompLessThan;
                    else if (comp->comp == CompLessThan)
                        comparator = CompGreaterThan;
                    else
                        comparator = comp->comp;
                } else {
                    section_base = mapaddr - section_off;
                    search_key = section_base + comp->val;
                    comparator = comp->comp;
                }

                if (!parent_range) {
                    // Find the range to potentially split, and add it to
                    // temp_subranges
                    parent_range = profile_find_first_range(subranges, search_key, comparator);
                    if (!parent_range) {
                        passes_filter = false;
                        break;
                    }

                    // If the found range has already matched with a filter, we
                    // are done
                    if (parent_range->benefit != 0) {
                        passes_filter = false;
                        break;
                    }

                    range = mm_econ_vmalloc(sizeof(struct profile_range));
                    if (!range) {
                        profile_free_all(&temp_subranges);
                        goto err;
                    }
                    range->start = parent_range->start;
                    range->end = parent_range->end;
                    range->benefit = parent_range->benefit;

                    profile_range_insert(&temp_subranges, range);
                } else {
                    // Find the range from the temp_subranges
                    range = profile_find_first_range(&temp_subranges, search_key, comparator);
                    if (!range) {
                        passes_filter = false;
                        break;
                    }
                }

                // Assign the benefit value.
                range->benefit = filter->benefit;

                // Split the range if necessary
                if (!mm_split_ranges(range, &temp_subranges, search_key, comparator)) {
                    profile_free_all(&temp_subranges);
                    goto err;
                }

                continue;
            }
            else if (comp->quant == QuantLen)
                val = len;
            else if (comp->quant == QuantProt)
                val = prot;
            else if (comp->quant == QuantFlags)
                val = flags;
            else if (comp->quant == QuantFD)
                val = fd;
            else
                val = off;

            passes_filter = passes_filter && mm_does_quantity_match(comp, val);
        }

        // If we split a range for this filter, remove the old range
        // from the subranges tree, and add the new ones
        if (passes_filter && parent_range) {
            range_node = &parent_range->node;
            rb_erase(range_node, subranges);
            mm_econ_vfree(parent_range, sizeof(struct profile_range));

            profile_move(&temp_subranges, subranges);
        }
        // If the entire new range matches this filter, set the benefit
        // value for all of the subranges that have not been set yet
        else if(passes_filter) {
            range_node = rb_first(subranges);

            while (range_node) {
                range = container_of(range_node, struct profile_range, node);

                if (range->benefit == 0)
                    range->benefit = filter->benefit;

                range_node = rb_next(range_node);
            }

            // Because the entire new range matched a filter, we no longer
            // have to check the rest of the filters
            break;
        }
    }
    up_read(&filter_procs_sem);

    // Finally, insert all of the new ranges into the proc's tree
    down_write(&filter_procs_sem);
    profile_move(&huge_subranges, &proc->hp_ranges_root);
    profile_move(&eager_subranges, &proc->eager_ranges_root);
    up_write(&filter_procs_sem);
    return;

err:
    pr_warn("mm_add_memory_range: no memory for new range");
    profile_free_all(&huge_subranges);
    profile_free_all(&eager_subranges);
    up_read(&filter_procs_sem);
    //printk("Added range %d %llx %llx %lld %llx\n", section, range->start, range->end, range->benefit, len);
}

void mm_copy_profile(pid_t old_pid, pid_t new_pid)
{
    struct mmap_filter_proc *proc = NULL;
    struct mmap_filter_proc *new_proc = NULL;
    struct mmap_filter *filter = NULL;
    struct mmap_filter *new_filter = NULL;
    struct mmap_comparison *comparison = NULL;
    struct mmap_comparison *new_comparison = NULL;

    down_read(&filter_procs_sem);

    // First, find out if a profile for old_pid exists
    proc = find_filter_proc_by_pid(old_pid);

    if (!proc) {
        up_read(&filter_procs_sem);
        return;
    }

    new_proc = mm_econ_vmalloc(sizeof(struct mmap_filter_proc));
    if (!new_proc)
        goto err;
    new_proc->pid = new_pid;
    INIT_LIST_HEAD(&new_proc->filters);
    new_proc->hp_ranges_root = RB_ROOT;
    new_proc->eager_ranges_root = RB_ROOT;

    // First, copy the filters
    list_for_each_entry(filter, &proc->filters, node) {
        new_filter = mm_econ_vmalloc(sizeof(struct mmap_filter));
        if (!new_filter)
            goto err;

        new_filter->section = filter->section;
        new_filter->benefit = filter->benefit;
        new_filter->policy = filter->policy;
        INIT_LIST_HEAD(&new_filter->comparisons);

        list_add_tail(&new_filter->node, &new_proc->filters);

        list_for_each_entry(comparison, &filter->comparisons, node) {
            new_comparison = mm_econ_vmalloc(sizeof(struct mmap_comparison));
            if (!new_comparison)
                goto err;

            new_comparison->quant = comparison->quant;
            new_comparison->comp = comparison->comp;
            new_comparison->val = comparison->val;

            list_add_tail(&new_comparison->node, &new_filter->comparisons);
        }
    }

    // Now, copy the ranges
    if (mm_copy_profile_range(&proc->hp_ranges_root, &new_proc->hp_ranges_root) != 0)
        goto err;
    if (mm_copy_profile_range(&proc->eager_ranges_root, &new_proc->eager_ranges_root) != 0)
        goto err;

    up_read(&filter_procs_sem);

    // Now, add the new proc to the list of procs
    down_write(&filter_procs_sem);
    list_add_tail(&new_proc->node, &filter_procs);
    up_write(&filter_procs_sem);

    return;
err:
    up_read(&filter_procs_sem);

    pr_warn("mm_econ: Unable to copy profile from %d to %d", old_pid, new_pid);
    if (new_proc) {
        profile_free_all(&new_proc->hp_ranges_root);
        profile_free_all(&new_proc->eager_ranges_root);
        mmap_filters_free_all(new_proc);
        mm_econ_vfree(new_proc, sizeof(struct mmap_filter_proc));
    }
}

void mm_profile_check_exiting_proc(pid_t pid)
{
    struct mmap_filter_proc *proc;

    down_read(&filter_procs_sem);
    proc = find_filter_proc_by_pid(pid);
    up_read(&filter_procs_sem);

    if (proc) {
        down_write(&filter_procs_sem);
        // If the process exits, we should also clear its profile
        profile_free_all(&proc->hp_ranges_root);
        profile_free_all(&proc->eager_ranges_root);
        mmap_filters_free_all(proc);

        // Remove the node from the list
        list_del(&proc->node);
        mm_econ_vfree(proc, sizeof(struct mmap_filter_proc));
        up_write(&filter_procs_sem);
    }
}

///////////////////////////////////////////////////////////////////////////////
// sysfs files

static ssize_t enabled_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", mm_econ_mode);
}

static ssize_t enabled_store(struct kobject *kobj,
        struct kobj_attribute *attr,
        const char *buf, size_t count)
{
    int mode;
    int ret;

    ret = kstrtoint(buf, 0, &mode);

    if (ret != 0) {
        mm_econ_mode = 0;
        return ret;
    }
    else if (mode >= 0 && mode <= 1) {
        mm_econ_mode = mode;
        return count;
    }
    else {
        mm_econ_mode = 0;
        return -EINVAL;
    }
}
static struct kobj_attribute enabled_attr =
__ATTR(enabled, 0644, enabled_show, enabled_store);

static ssize_t debugging_mode_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%d\n", mm_econ_debugging_mode);
}

static ssize_t debugging_mode_store(struct kobject *kobj,
        struct kobj_attribute *attr,
        const char *buf, size_t count)
{
    int mode;
    int ret;

    ret = kstrtoint(buf, 0, &mode);

    if (ret != 0) {
        mm_econ_debugging_mode = 0;
        return ret;
    }
    else {
        mm_econ_debugging_mode = mode;
        return count;
    }
}
static struct kobj_attribute debugging_mode_attr =
__ATTR(debugging_mode, 0644, debugging_mode_show, debugging_mode_store);

static ssize_t contention_cycles_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", mm_econ_contention_ms);
}

static ssize_t contention_cycles_store(struct kobject *kobj,
        struct kobj_attribute *attr,
        const char *buf, size_t count)
{
    u64 ms;
    int ret;

    ret = kstrtou64(buf, 0, &ms);

    if (ret != 0) {
        return ret;
    }
    else {
        mm_econ_contention_ms = ms;
        return count;
    }
}
static struct kobj_attribute contention_cycles_attr =
__ATTR(contention_cyles, 0644, contention_cycles_show, contention_cycles_store);

static ssize_t freq_mhz_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf, "%llu\n", mm_econ_freq_mhz);
}

static ssize_t freq_mhz_store(struct kobject *kobj,
        struct kobj_attribute *attr,
        const char *buf, size_t count)
{
    u64 mhz;
    int ret;

    ret = kstrtou64(buf, 0, &mhz);

    if (ret != 0) {
        return ret;
    }
    else {
        mm_econ_freq_mhz = mhz;
        return count;
    }
}
static struct kobj_attribute freq_mhz_attr =
__ATTR(freq_mhz, 0644, freq_mhz_show, freq_mhz_store);

static ssize_t stats_show(struct kobject *kobj,
        struct kobj_attribute *attr, char *buf)
{
    return sprintf(buf,
            "estimated=%lld\ndecided=%lld\n"
            "yes=%lld\npromoted=%lld\n"
            "compactions=%lld\nprezerotry=%lld\n"
            "vmallocbytes=%lld\n",
            mm_econ_num_estimates,
            mm_econ_num_decisions,
            mm_econ_num_decisions_yes,
            mm_econ_num_hp_promotions,
            mm_econ_num_async_compaction,
            mm_econ_num_async_prezeroing,
            mm_econ_vmalloc_bytes);
}

static ssize_t stats_store(struct kobject *kobj,
        struct kobj_attribute *attr,
        const char *buf, size_t count)
{
    return -EINVAL;
}
static struct kobj_attribute stats_attr =
__ATTR(stats, 0444, stats_show, stats_store);

static struct attribute *mm_econ_attr[] = {
    &enabled_attr.attr,
    &contention_cycles_attr.attr,
    &stats_attr.attr,
    &debugging_mode_attr.attr,
    &freq_mhz_attr.attr,
    NULL,
};

static const struct attribute_group mm_econ_attr_group = {
    .attrs = mm_econ_attr,
};

///////////////////////////////////////////////////////////////////////////////
// procfs files

static void mm_memory_section_get_str(char *buf, enum mm_memory_section section)
{
    if (section == SectionCode) {
        strcpy(buf, "code");
    } else if (section == SectionData) {
        strcpy(buf, "data");
    } else if (section == SectionHeap) {
        strcpy(buf, "heap");
    } else if (section == SectionMmap) {
        strcpy(buf, "mmap");
    } else {
        printk(KERN_WARNING "Invalid memory section");
        BUG();
    }
}

static void mm_policy_get_str(char *buf, enum mm_policy policy)
{
    if (policy == PolicyHugePage) {
        strcpy(buf, "huge");
    } else if (policy == PolicyEagerPage) {
        strcpy(buf, "eager");
    } else {
        pr_warn("Invalid mm policy");
        BUG();
    }
}

static char mmap_comparator_get_char(enum mmap_comparator comp)
{
    if (comp == CompEquals) {
        return '=';
    } else if (comp == CompGreaterThan) {
        return '>';
    } else if (comp == CompLessThan) {
        return '<';
    } else {
        printk(KERN_WARNING "Invalid mmap comparator");
        BUG();
    }
}

static void mmap_quantity_get_str(char *buf, enum mmap_quantity quant)
{
    if (quant == QuantSectionOff) {
        strcpy(buf, "section_off");
    } else if (quant == QuantAddr) {
        strcpy(buf, "addr");
    } else if (quant == QuantLen) {
        strcpy(buf, "len");
    } else if (quant == QuantProt) {
        strcpy(buf, "prot");
    } else if (quant == QuantFlags) {
        strcpy(buf, "flags");
    } else if (quant == QuantFD) {
        strcpy(buf, "fd");
    } else if (quant == QuantOff) {
        strcpy(buf, "off");
    } else {
        pr_warn("Invalid mmap quantity");
        BUG();
    }
}

static ssize_t mmap_filters_read(struct file *file,
        char __user *buf, size_t count, loff_t *ppos)
{
    struct task_struct *task = extern_get_proc_task(file_inode(file));
    char *buffer;
    ssize_t len = 0;
    ssize_t ret = 0;
    struct mmap_filter *filter;
    struct mmap_comparison *comparison;
    struct mmap_filter_proc *proc;
    struct list_head *filter_head = NULL;

    if (!task)
        return -ESRCH;

    buffer = mm_econ_vmalloc(MMAP_FILTER_BUF_SIZE);
    if (!buffer) {
        put_task_struct(task);
        return -ENOMEM;
    }

    // First, print the CSV Header for easier reading
    len = sprintf(buffer, "POLICY,SECTION,MISSES,CONSTRAINTS...\n");

    // Find the filters that correspond to this process if there are any
    down_read(&filter_procs_sem);
    proc = find_filter_proc_by_pid(task->tgid);
    if (!proc)
        goto out;

    filter_head = &proc->filters;

    // Print out all of the filters
    list_for_each_entry(filter, filter_head, node) {
        char policy[8];
        char section[8];
        char quantity[16];
        u64 benefit = filter->benefit;
        char comparator;
        u64 val;

        mm_policy_get_str(policy, filter->policy);
        mm_memory_section_get_str(section, filter->section);

        // Make sure we don't overflow the buffer
        if (len > MMAP_FILTER_BUF_SIZE - MMAP_FILTER_BUF_DEAD_ZONE)
            goto out;

        // Print the per filter information
        len += sprintf(&buffer[len], "%s,%s,0x%llx", policy, section, benefit);

        list_for_each_entry(comparison, &filter->comparisons, node) {
            mmap_quantity_get_str(quantity, comparison->quant);
            comparator = mmap_comparator_get_char(comparison->comp);
            val = comparison->val;

            // Make sure we don't overflow the buffer
            if (len > MMAP_FILTER_BUF_SIZE - MMAP_FILTER_BUF_DEAD_ZONE)
                goto out;

            // Print the per comparison information
            len += sprintf(&buffer[len], ",%s,%c,0x%llx", quantity,
                comparator, val);
        }

        // Remember to end with a newline
        len += sprintf(&buffer[len], "\n");
    }

out:
    up_read(&filter_procs_sem);

    ret = simple_read_from_buffer(buf, count, ppos, buffer, len);

    // Remember to free the buffer
    mm_econ_vfree(buffer, MMAP_FILTER_BUF_SIZE);

    put_task_struct(task);

    return ret;
}

static int get_memory_section(char *buf, enum mm_memory_section *section)
{
    int ret = 0;

    if (strcmp(buf, "code") == 0) {
        *section = SectionCode;
    } else if (strcmp(buf, "data") == 0) {
        *section = SectionData;
    } else if (strcmp(buf, "heap") == 0) {
        *section = SectionHeap;
    } else if (strcmp(buf, "mmap") == 0) {
        *section = SectionMmap;
    } else {
        ret = -1;
    }

    return ret;
}

static int get_mm_policy(char *buf, enum mm_policy *policy)
{
    int ret = 0;

    if (strcmp(buf, "huge") == 0) {
        *policy = PolicyHugePage;
    } else if (strcmp(buf, "eager") == 0) {
        *policy = PolicyEagerPage;
    } else {
        ret = -1;
    }

    return ret;
}

static int get_mmap_quantity(char *buf, enum mmap_quantity *quant)
{
    int ret = 0;

    if (strcmp(buf, "section_off") == 0) {
        *quant = QuantSectionOff;
    } else if (strcmp(buf, "addr") == 0) {
        *quant = QuantAddr;
    } else if (strcmp(buf, "len") == 0) {
        *quant = QuantLen;
    } else if (strcmp(buf, "prot") == 0) {
        *quant = QuantProt;
    } else if (strcmp(buf, "flags") == 0) {
        *quant = QuantFlags;
    } else if (strcmp(buf, "fd") == 0) {
        *quant = QuantFD;
    } else if (strcmp(buf, "off") == 0) {
        *quant = QuantOff;
    } else {
        ret = -1;
    }

    return ret;
}

static int get_mmap_comparator(char *buf, enum mmap_comparator *comp)
{
    int ret = 0;

    if (strcmp(buf, "=") == 0) {
        *comp = CompEquals;
    } else if (strcmp(buf, ">") == 0) {
        *comp = CompGreaterThan;
    } else if (strcmp(buf, "<") == 0) {
        *comp = CompLessThan;
    } else {
        ret = -1;
    }

    return ret;
}

static int mmap_filter_read_comparison(char **tok, struct mmap_comparison *c)
{
    int ret = 0;
    u64 value = 0;
    char *value_buf;

    // Get the quantity
    value_buf = strsep(tok, ",");
    if (!value_buf) {
        return -1;
    }

    ret = get_mmap_quantity(value_buf, &c->quant);
    if (ret != 0) {
        return -1;
    }

    // Get the comparator
    value_buf = strsep(tok, ",");
    if (!value_buf) {
        return -1;
    }

    ret = get_mmap_comparator(value_buf, &c->comp);
    if (ret != 0) {
        return -1;
    }

    // Get the value
    value_buf = strsep(tok, ",");
    if (!value_buf) {
        return -1;
    }

    ret = kstrtoull(value_buf, 0, &value);
    if (ret != 0) {
        return -1;
    }

    c->val = value;

    return 0;
}

static ssize_t mmap_filters_write(struct file *file,
        const char __user *buf, size_t count,
        loff_t *ppos)
{
    struct task_struct *task = NULL;
    char *buf_from_user = NULL;
    char *outerTok = NULL;
    char *tok = NULL;
    struct mmap_filter *filter = NULL;
    struct mmap_comparison *comparison = NULL;
    struct mmap_filter_proc *proc = NULL;
    bool alloc_new_proc;
    ssize_t error = 0;
    size_t bytes_read = 0;
    size_t filter_len = 0;
    int ret;
    u64 value;
    char * value_buf;

    // Copy the input from userspace
    buf_from_user = mm_econ_vmalloc(count + 1);
    if (!buf_from_user)
        return -ENOMEM;
    if (copy_from_user(buf_from_user, buf, count)) {
        error = -EFAULT;
        goto err;
    }
    buf_from_user[count] = 0;
    outerTok = buf_from_user;

    task = extern_get_proc_task(file_inode(file));
    if (!task) {
        error = -ESRCH;
        goto err;
    }

    down_write(&filter_procs_sem);
    // Allocate the proc structure if necessary
    if ((proc = find_filter_proc_by_pid(task->tgid))) { // NOTE: assignment
        alloc_new_proc = false;
    } else {
        alloc_new_proc = true;
        proc = mm_econ_vmalloc(sizeof(struct mmap_filter_proc));
        if (!proc) {
            up_write(&filter_procs_sem);
            error = -ENOMEM;
            goto err;
        }

        // Initialize the new proc
        proc->pid = task->tgid;
        INIT_LIST_HEAD(&proc->filters);
        proc->hp_ranges_root = RB_ROOT;
    }
    up_write(&filter_procs_sem);

    // Read in the filters
    tok = strsep(&outerTok, "\n");
    while (outerTok) {
        bool invalid_filter = false;

        if (tok[0] == '\0') {
            break;
        }

        // Include the \n that was removed by strsep
        filter_len = strlen(tok) + 1;

        filter = mm_econ_vmalloc(sizeof(struct mmap_filter));
        if (!filter) {
            error = -ENOMEM;
            goto err;
        }

        // Get the policy the filter applies to
        value_buf = strsep(&tok, ",");
        if (!value_buf) {
            break;
        }
        ret = get_mm_policy(value_buf, &filter->policy);
        if (ret != 0) {
            break;
        }

        // Get the section of the memory map
        value_buf = strsep(&tok, ",");
        if (!value_buf) {
            break;
        }
        ret = get_memory_section(value_buf, &filter->section);
        if (ret != 0) {
            break;
        }

        // Get the benefit for the filter
        value_buf = strsep(&tok, ",");
        if (!value_buf) {
            break;
        }

        ret = kstrtoull(value_buf, 0, &value);
        if (ret != 0) {
            break;
        }

        filter->benefit = value;

        // Read in the comparisons of the filter
        INIT_LIST_HEAD(&filter->comparisons);
        while (tok) {
            if (tok[0] == '\0')
                break;

            comparison = mm_econ_vmalloc(sizeof(struct mmap_comparison));
            if (!comparison) {
                error = -ENOMEM;
                goto err;
            }

            ret = mmap_filter_read_comparison(&tok, comparison);
            if (ret != 0) {
                invalid_filter = true;
                mm_econ_vfree(comparison, sizeof(struct mmap_comparison));
                break;
            }

            // Add the comparison to the list of comparisons
            list_add_tail(&comparison->node, &filter->comparisons);
        }

        if (invalid_filter)
            break;

        // Add the new filter to the list
        down_write(&filter_procs_sem);
        list_add_tail(&filter->node, &proc->filters);
        up_write(&filter_procs_sem);

        // Get the next filter
        tok = strsep(&outerTok, "\n");

        bytes_read += filter_len;
    }

    // The write system call might not write the entire filter file in one go.
    // We most handle the case where the file is seperated in the middle of a
    // filter, making it look invalid.
    // If that happens, we simply say we read up until the last full filter.
    // However, if we read no good filters before the first invalid filter,
    // just assume the filter is bad.
    if (bytes_read == 0) {
        error = -EINVAL;
        goto err;
    }

    // Link the new proc if we need to
    if (alloc_new_proc) {
        down_write(&filter_procs_sem);
        list_add_tail(&proc->node, &filter_procs);
        up_write(&filter_procs_sem);
    }

    mm_econ_vfree(buf_from_user, count + 1);
    put_task_struct(task);

    return bytes_read;

err:
    if (filter)
        mm_econ_vfree(filter, sizeof(struct mmap_filter));
    if (proc) {
        down_write(&filter_procs_sem);
        mmap_filters_free_all(proc);
        up_write(&filter_procs_sem);
        if (alloc_new_proc)
            mm_econ_vfree(proc, sizeof(struct mmap_filter_proc));
    }
    if (task)
        put_task_struct(task);
    if (buf_from_user)
        mm_econ_vfree(buf_from_user, count + 1);
    return error;
}

const struct file_operations proc_mmap_filters_operations = {
    .read = mmap_filters_read,
    .write = mmap_filters_write,
    .llseek = default_llseek,
};

static ssize_t print_range_tree(char *buffer, ssize_t buf_size, struct rb_node *node)
{
    ssize_t len = 0;

    while (node) {
        struct profile_range *range =
            container_of(node, struct profile_range, node);

        // Make sure we don't overflow the buffer
        if (len > buf_size - MMAP_FILTER_BUF_DEAD_ZONE)
            return len;

        len += sprintf(
            &buffer[len],
            "[0x%llx, 0x%llx) (%llu bytes) benefit=0x%llx\n",
            range->start,
            range->end,
            range->end - range->start,
            range->benefit
        );

        node = rb_next(node);
    }

    return len;
}

static ssize_t print_profile(struct file *file,
        char __user *buf, size_t count, loff_t *ppos)
{
    struct task_struct *task = extern_get_proc_task(file_inode(file));
    char *buffer;
    ssize_t len = 0;
    ssize_t ret = 0;
    struct mmap_filter_proc *proc;
    struct rb_node *node = NULL;
    bool found = false;

    if (!task)
        return -ESRCH;

    // Find the data for the process this relates to
    down_read(&filter_procs_sem);
    list_for_each_entry(proc, &filter_procs, node) {
        if (proc->pid == task->tgid) {
            found = true;
            break;
        }
    }
    up_read(&filter_procs_sem);
    if (!found) {
        put_task_struct(task);
        return 0;
    }

    buffer = mm_econ_vmalloc(MMAP_FILTER_BUF_SIZE);
    if (!buffer) {
        put_task_struct(task);
        return -ENOMEM;
    }

    down_read(&filter_procs_sem);
    len += sprintf(buffer, "Huge Page Ranges:\n");
    node = rb_first(&proc->hp_ranges_root);
    len += print_range_tree(&buffer[len], MMAP_FILTER_BUF_SIZE - len, node);

    len += sprintf(&buffer[len], "Eager Page Ranges:\n");
    node = rb_first(&proc->eager_ranges_root);
    len += print_range_tree(&buffer[len], MMAP_FILTER_BUF_SIZE - len, node);

    up_read(&filter_procs_sem);

    ret = simple_read_from_buffer(buf, count, ppos, buffer, len);

    mm_econ_vfree(buffer, MMAP_FILTER_BUF_SIZE);

    put_task_struct(task);

    return ret;
}

const struct file_operations proc_mem_ranges_operations = {
    .read = print_profile,
    .llseek = default_llseek,
};
///////////////////////////////////////////////////////////////////////////////
// Init

static int __init mm_econ_init(void)
{
    struct kobject *mm_econ_kobj;
    int err;

    mm_econ_kobj = kobject_create_and_add("mm_econ", mm_kobj);
    if (unlikely(!mm_econ_kobj)) {
        pr_err("failed to create mm_econ kobject\n");
        return -ENOMEM;
    }

    err = sysfs_create_group(mm_econ_kobj, &mm_econ_attr_group);
    if (err) {
        pr_err("failed to register mm_econ group\n");
        kobject_put(mm_econ_kobj);
        return err;
    }

    return 0;
}
subsys_initcall(mm_econ_init);
