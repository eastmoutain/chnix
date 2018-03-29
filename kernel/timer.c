#include <debug.h>
#include <assert.h>
#include <stdio.h>
#include <kernel/timer.h>
#include <arch/spinlock.h>
#include <list.h>

spinlock_t timer_lock;

struct list_node timer_queue;

static enum handler_return timer_tick(void *arg, time_t now);

void timer_initialize(timer_t *timer)
{
    *timer = (timer_t)TIMER_INITIAL_VALUE(*timer);
}

static void insert_timer_in_queue(timer_t *timer)
{
    timer_t *entry;

    debug_assert(arch_ints_disabled());

    list_foreach_entry(&timer_queue, entry, timer_t, node) {
        if (TIME_GT(entry->sched_time, timer->sched_time)) {
            list_add_before(&entry->node, &timer->node);
            return;
        }
    }

    list_add_tail(&timer_queue, &timer->node);
}

static void timer_set(timer_t *timer, time_t delay, time_t period, timer_callback callback, void *arg)
{

}

// FIXME not impleted
static enum handler_return timer_tick(void *arg, time_t now)
{
    return INT_NO_RESCHEDULE;
}


void timer_init(void)
{
    timer_lock = SPINLOCK_INITIAL_VALUE;

    list_init(&timer_queue);

    platform_set_periodic_timer(timer_tick, NULL, 10); // 10ms
}
