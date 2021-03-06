#include <sys/types.h>
#include <assert.h>
#include <kernel/wait_queue.h>
#include <kernel/thread.h>
#include <kernel/timer.h>
#include <arch/spinlock.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#ifdef CONFIG_MAX_THREADS
#define MAX_THREADS_NUM CONFIG_MAX_THREADS
#else
#define MAX_THREADS_NUM (10)
#endif

static thread_t g_threads[MAX_THREADS_NUM];

spinlock_t thread_lock = SPINLOCK_INITIAL_VALUE;

static struct list_node g_thread_free_list;
static struct list_node thread_list;


static struct list_node run_queue[NUM_PRIORITY];

static uint32_t run_queue_bitmap;

#define IDLE_THREAD_ID (0)
#define MAX_THREAD_ID (MAX_THREADS_NUM - 1)

static thread_t *_current_thread = NULL;

static unsigned int thread_preempt_lock;

extern void x64_thread_context_switch(vaddr_t *sp1, vaddr_t *sp2);

static inline thread_t* thread_id_to_ptr(int thid)
{
    debug_assert(thid > IDLE_THREAD_ID && thid < MAX_THREAD_ID);
    return &g_threads[thid];
}

static inline int thread_ptr_to_id(thread_t *ptr)
{
    debug_assert((unsigned long)ptr >= (unsigned long)&g_threads[0]
                 && (unsigned long)ptr < (unsigned long)&g_threads[MAX_THREAD_ID]);

    return ((unsigned long)ptr - (unsigned long)&g_threads[0])/sizeof(thread_t);
}

void thread_preempt_enable(void)
{
    spinlock_saved_state_t state;

    THREAD_LOCK(state);

    if (thread_preempt_lock > 0)
        thread_preempt_lock--;

    THREAD_UNLOCK(state);
}

void thread_preempt_disable(void)
{
    spinlock_saved_state_t state;

    THREAD_LOCK(state);

    thread_preempt_lock++;

    THREAD_UNLOCK(state);
}

bool thread_preempt_is_enabled(void)
{
    return (bool)thread_preempt_lock == 0;
}

static inline thread_t* get_free_thread_node(void)
{
    spinlock_saved_state_t state;

    if (list_is_empty(&g_thread_free_list))
        return NULL;

    thread_t *thread = NULL;

    THREAD_LOCK(state);

    struct list_node *node = list_remove_head(&g_thread_free_list);
    thread = container_of(node, thread_t, thread_list_node);
    
    THREAD_UNLOCK(state);

    return thread;
}

static inline void free_thread_node(thread_t *thread)
{
    spinlock_saved_state_t state;

    THREAD_LOCK(state);
    list_add_tail(&g_thread_free_list, &thread->thread_list_node);
    THREAD_UNLOCK(state);

}
static inline thread_t* idle_thread()
{
    return &g_threads[IDLE_THREAD_ID];
}

void insert_in_run_queue_head(thread_t *thread)
{
    debug_assert(thread->state == THREAD_READY);
    debug_assert(!list_in_list(&thread->queue_node));
    debug_assert(arch_ints_disabled());
    debug_assert(spinlock_held(&thread_lock));

    list_add_head(&run_queue[thread->priority], &thread->queue_node);
    run_queue_bitmap |= (1<<thread->priority);
}

void insert_in_run_queue_tail(thread_t *thread)
{
    debug_assert(thread->state == THREAD_READY);
    debug_assert(!list_in_list(&thread->queue_node));
    debug_assert(arch_ints_disabled());
    debug_assert(spinlock_held(&thread_lock));

    list_add_tail(&run_queue[thread->priority], &thread->queue_node);
    run_queue_bitmap |= (1<<thread->priority);
}

static void init_thread_strcut(thread_t *thread, const char *name)
{
    memset(thread, 0, sizeof(thread_t));

    debug_assert(strlen(name) < TREAD_NAME_LEN);

    strncpy(thread->name, name, strlen(name));
}

static void initial_thread_func(void)
{
    spin_unlock(&thread_lock);
    arch_enable_ints();

    thread_t *thread = get_current_thread();

    debug_assert(thread->entry != NULL);

    int ret = thread->entry(thread->arg);

    thread_exit(ret);
}

static void init_thread_stack(thread_t *thread)
{
    uint64_t stack_top = (uint64_t)thread->stack + thread->stack_size;

    stack_top = stack_top & (~0xfUL); // aligned down to 16 bytes;
    stack_top -= 8;

    struct x64_context_frame *frame = (struct x64_context_frame*)stack_top;

    frame--;
    memset(frame, 0, sizeof(*frame));

    frame->rip = (addr_t)initial_thread_func;
    frame->rflags = 0x3002;

    // FIXME leave FPU

    thread->sp = (addr_t)frame;
}

static thread_t* get_new_thread(void)
{
    thread_t *new;

    uint32_t bitmap = run_queue_bitmap;
    int q_up_bound = sizeof(run_queue_bitmap) * 8;
    int q;
    for (q = 0; q < q_up_bound; q++) {
        if (bitmap & (1 << q))
            break;
    }

    if (q >= q_up_bound) {
        goto idle;
    }

    debug_assert(!list_is_empty(&run_queue[q]));

    list_foreach_entry(&run_queue[q], new, thread_t, queue_node) {
        list_delete(&new->queue_node);
        if (list_is_empty(&run_queue[q])) {
            run_queue_bitmap &= ~(1<<q);
        }

        return new;
    }

idle:
    return idle_thread();
}

thread_t* get_current_thread(void)
{
    debug_assert(_current_thread != NULL);
    return _current_thread;
}

void set_current_thread(thread_t *thread)
{
    _current_thread = thread;
}

static bool thread_is_idle(thread_t *thread)
{
    return !!(thread->flags & THREAD_FLAG_IDLE);
}

void thread_preempt(void)
{
    thread_t *thread = get_current_thread();

    debug_assert(thread->state == THREAD_RUNNING);

    spinlock_saved_state_t state;

    THREAD_LOCK(state);
    thread->state = THREAD_READY;
    if(!(thread_is_idle(thread))) {
        if (thread->remaining_quantum > 0)
            insert_in_run_queue_head(thread);
        else
            insert_in_run_queue_tail(thread);
    }

    thread_resched();
    THREAD_UNLOCK(state);
}

void thread_resched(void)
{
    thread_t *old;
    thread_t *new;

    old = get_current_thread();
    new = get_new_thread();

    debug_assert(new);

    new->state = THREAD_RUNNING;

    if (new == old)
        return;

    if (new->remaining_quantum <= 0)
        new->remaining_quantum = 5;

    set_current_thread(new);

    x64_thread_context_switch(&old->sp, &new->sp);
}

void thread_set_name(const char *name)
{
    spinlock_saved_state_t state;

    thread_t *cur = get_current_thread();

    THREAD_LOCK(state);
    strncpy(cur->name, name, strlen(name));
    THREAD_UNLOCK(state);
}

void thread_set_priority(int priority)
{
    spinlock_saved_state_t state;

    thread_t *cur = get_current_thread();

    THREAD_LOCK(state);

    if (priority <= IDLE_PRIORITY) {
        priority = IDLE_PRIORITY + 1;
    }

    if (priority > HIGHEST_PRIORITY) {
        priority = HIGHEST_PRIORITY;
    }

    cur->priority = priority;
    insert_in_run_queue_head(cur);
    thread_resched();

    THREAD_UNLOCK(state);
}


int thread_create(const char *name, thread_start_routine entry, void *arg, int priority, void *stack, size_t stack_size)
{
    spinlock_saved_state_t state;

    thread_t *thread = get_free_thread_node();
    if (thread == NULL) {
        printf("no free thread node left\r\n");
        return -1;
    }

    init_thread_strcut(thread, name);

    thread->entry = entry;
    thread->arg = arg;
    thread->priority = priority;
    thread->state = THREAD_SUSPENDED;
    thread->blocking_wait_queue = NULL;
    thread->wait_queue_block_ret = NO_ERR;

    thread->retcode = 0;
    wait_queue_init(&thread->exit_wait_queue);

    memset(stack, 0, stack_size);
    thread->stack = stack;
    thread->stack_size = stack_size;

    init_thread_stack(thread);

    THREAD_LOCK(state);
    list_add_tail(&thread_list, &thread->thread_list_node);
    THREAD_UNLOCK(state);

    printf("Created thread %s, prio 0x%x, stack %p\r\n"
           "stack_size 0x%lx, routine %p, sp 0x%lx\n\r",
            thread->name, thread->priority, thread->stack, thread->stack_size,
            thread->entry, thread->sp);
    return thread_ptr_to_id(thread);
}

void thread_yield(void)
{
    spinlock_saved_state_t state;
    thread_t *old = get_current_thread();

    debug_assert(old->state == THREAD_RUNNING);

    THREAD_LOCK(state);

    old->state= THREAD_READY;
    old->remaining_quantum = 0;


    if (!thread_is_idle(old)) {
        //printf("thread_yield, cur thread %s, flags 0x%x\r\n", old->name, old->flags);
        insert_in_run_queue_tail(old);
    }

    thread_resched();

    THREAD_UNLOCK(state);
}


int thread_resume(int thid)
{
    spinlock_saved_state_t state;
    thread_t *thread = NULL;
    
    thread = thread_id_to_ptr(thid);

    debug_assert(thread->state != THREAD_DEATH);
    bool resched = false;
    bool ints_disabled = arch_ints_disabled();

    THREAD_LOCK(state);
    if (thread->state == THREAD_SUSPENDED) {
        thread->state = THREAD_READY;
        insert_in_run_queue_head(thread);
        if (!ints_disabled)
            resched = true;
    }

    THREAD_UNLOCK(state);
    if (resched)
        thread_yield();

    return NO_ERR;
}


int thread_join(int thid, int *retcode, time_t timeout)
{
    spinlock_saved_state_t state;
    thread_t *thread = NULL;
    
    thread = thread_id_to_ptr(thid);

    THREAD_LOCK(state);

    if (thread->flags & THREAD_FLAG_DETACHED) {
        THREAD_UNLOCK(state);
        return -ERR_THREAD_DETACHED;
    }

    if (thread->state != THREAD_DEATH) {
        int err = wait_queue_block(&thread->exit_wait_queue, timeout);
        if (err < 0) {
            THREAD_UNLOCK(state);
            return err;
        }
    }

    debug_assert(thread->state == THREAD_DEATH);
    debug_assert(thread->blocking_wait_queue == NULL);
    debug_assert(!list_in_list(&thread->queue_node));

    if (retcode)
        *retcode = thread->retcode;

    list_delete(&thread->thread_list_node);
    free_thread_node(thread);

    THREAD_UNLOCK(state);

    return NO_ERR;
}

int thread_detach(int thid)
{
    spinlock_saved_state_t state;
    thread_t *thread = NULL;
    
    thread = thread_id_to_ptr(thid);
    THREAD_LOCK(state);

    wait_queue_wake_all(&thread->exit_wait_queue, false, ERR_THREAD_DETACHED);

    if (thread->state == THREAD_DEATH) {
        thread->flags &= ~THREAD_FLAG_DETACHED;
        THREAD_UNLOCK(state);
        return thread_join(thread, NULL, 0);
    } else {
        thread->flags |= THREAD_FLAG_DETACHED;
        THREAD_UNLOCK(state);
    }

    return NO_ERR;
}

int thread_detach_and_resume(int thid)
{
    int err;
    thread_t *thread = NULL;
    
    thread = thread_id_to_ptr(thid);

    err = thread_detach(thread);
    if (err < 0)
        return err;

    thread_resume(thread);
    return NO_ERR;
}


void thread_exit(int retcode)
{
    spinlock_saved_state_t state;
    thread_t *thread = get_current_thread();

    debug_assert(thread->state == THREAD_RUNNING);
    debug_assert(!thread_is_idle(thread));

    THREAD_LOCK(state);

    thread->state = THREAD_DEATH;
    thread->retcode = retcode;

    if (thread->flags & THREAD_FLAG_DETACHED) {
        list_delete(&thread->thread_list_node);
        free_thread_node(thread);
    } else {
        wait_queue_wake_all(&thread->exit_wait_queue, false, 0);
    }

    thread_resched();

    platform_panic("fell through thread_exit\n");
}

static enum handler_return thread_sleep_handler(timer_t *timer, time_t now, void *arg)
{
    thread_t *thread = (thread_t *)arg;
    spinlock_saved_state_t state;

    timer = timer;
    now = now;
    debug_assert(thread->state == THREAD_SLEEPING);

    THREAD_LOCK(state);

    thread->state = THREAD_READY;
    insert_in_run_queue_head(thread);

    THREAD_UNLOCK(state);

    return INT_RESCHEDULE;
}

void thread_sleep(time_t delay)
{
    timer_t timer;
    spinlock_saved_state_t state;

    thread_t *thread = get_current_thread();

    debug_assert(thread->state == THREAD_RUNNING);
    debug_assert(!thread_is_idle(thread));

    timer_initialize(&timer);

    THREAD_LOCK(state);

    timer_set_oneshot(&timer, delay, thread_sleep_handler, (void*)thread);
    thread->state = THREAD_SLEEPING;
    thread_resched();

    THREAD_UNLOCK(state);
}

void thread_block(void)
{
    thread_t *thread = get_current_thread();

    debug_assert(thread->state == THREAD_BLOCKED);
    debug_assert(spinlock_held(&thread_lock));
    debug_assert(!thread_is_idle(thread));

    thread_resched();
}

void thread_unblock(int thid, bool resched)
{
    thread_t *thread = NULL;
    
    thread = thread_id_to_ptr(thid);
    debug_assert(thread->state == THREAD_BLOCKED);
    debug_assert(spinlock_held(&thread_lock));
    debug_assert(!thread_is_idle(thread));

    thread->state = THREAD_READY;
    insert_in_run_queue_head(thread);

    if (resched) {
        thread_resched();
    }
}

enum handler_return thread_timer_tick(void)
{
    thread_t *thread = get_current_thread();

    thread->remaining_quantum--;
    //printf("thread %s quantum 0x%x\r\n", thread->name, thread->remaining_quantum);
    if (thread->remaining_quantum <= 0 && thread_preempt_is_enabled()) {
        //printf("thread %s will be shecduled out\r\n", thread->name);
        return INT_RESCHEDULE;
    } else {
        return INT_NO_RESCHEDULE;
    }
}

int thread_unblock_from_wait_queue(thread_t *thread, int error)
{
    debug_assert(arch_ints_disabled());
    debug_assert(spinlock_held(&thread_lock));

    if (thread->state != THREAD_BLOCKED)
        return -ERR_NOT_BLOCKED;

    debug_assert(thread->blocking_wait_queue != NULL);
    debug_assert(list_in_list(&thread->queue_node));

    list_delete(&thread->queue_node);
    thread->blocking_wait_queue->count--;
    thread->blocking_wait_queue = NULL;
    thread->state = THREAD_READY;
    thread->wait_queue_block_ret = error;
    insert_in_run_queue_head(thread);

    return NO_ERR;
}

void thread_init_early(void)
{
    int i;

    for (i = 0; i < NUM_PRIORITY; i++) {
        list_initialize(&run_queue[i]);
    }

    run_queue_bitmap = 0;

    list_initialize(&thread_list);
    list_initialize(&g_thread_free_list);

    memset(g_threads, 0, sizeof(g_threads));
    for (i = IDLE_THREAD_ID + 1; i < MAX_THREAD_ID; i++) {
        g_threads[i].thread_id = i;
        list_add_tail(&g_thread_free_list, &g_threads[i].thread_list_node);
    }
    g_threads[0].thread_id = 0;
    
    thread_t *idle = idle_thread();
    init_thread_strcut(idle, "boot thread");

    idle->priority = HIGHEST_PRIORITY;
    idle->state = THREAD_RUNNING;
    idle->flags = THREAD_FLAG_DETACHED;
    wait_queue_init(&idle->exit_wait_queue);
    list_add_head(&thread_list, &idle->thread_list_node);
    set_current_thread(idle);
}

static void idle_thread_routine(void)
{
    for (; ;) {
        asm("nop");
    }
}
void thread_become_idle(void)
{
    debug_assert(arch_ints_disabled());

    thread_t *thread = get_current_thread();
    thread_set_name("idle");
    thread->priority = IDLE_PRIORITY;
    thread->flags |= THREAD_FLAG_IDLE;

    arch_enable_ints();
    thread_yield();

    printf("main rotine becomes idle thread\r\n");
    idle_thread_routine();
}

