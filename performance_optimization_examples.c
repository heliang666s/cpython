// Python 异步和多线程模块性能优化示例代码

#include "Python.h"
#include "pycore_lock.h"
#include "pycore_llist.h"
#include <stdatomic.h>

// ============================================================================
// 1. 自适应自旋锁优化
// ============================================================================

typedef struct {
    atomic_int lock;
    atomic_int contention_count;
    int max_spin;
} AdaptiveSpinLock;

static inline int get_adaptive_spin_count(AdaptiveSpinLock *lock) {
    int contention = atomic_load(&lock->contention_count);
    
    // 根据竞争程度动态调整自旋次数
    if (contention < 10) {
        return 20;  // 低竞争：少量自旋
    } else if (contention < 100) {
        return 40;  // 中等竞争：适度自旋
    } else {
        return 60;  // 高竞争：更多自旋
    }
}

int adaptive_spinlock_acquire(AdaptiveSpinLock *lock) {
    int expected = 0;
    int spin_count = 0;
    int max_spin = get_adaptive_spin_count(lock);
    
    while (!atomic_compare_exchange_weak(&lock->lock, &expected, 1)) {
        if (spin_count++ < max_spin) {
            // CPU 友好的自旋
            #if defined(__x86_64__) || defined(__i386__)
                __asm__ volatile("pause" ::: "memory");
            #elif defined(__aarch64__)
                __asm__ volatile("yield" ::: "memory");
            #endif
        } else {
            // 自旋失败，增加竞争计数并让出 CPU
            atomic_fetch_add(&lock->contention_count, 1);
            sched_yield();
            spin_count = 0;
        }
        expected = 0;
    }
    
    // 成功获取锁，减少竞争计数
    if (atomic_load(&lock->contention_count) > 0) {
        atomic_fetch_sub(&lock->contention_count, 1);
    }
    
    return 0;
}

// ============================================================================
// 2. 无锁任务队列优化
// ============================================================================

typedef struct TaskNode {
    struct TaskNode *next;
    PyObject *task;
} TaskNode;

typedef struct {
    _Py_atomic_address head;
    _Py_atomic_address tail;
    _Py_atomic_int size;
} LockFreeTaskQueue;

// 无锁入队操作
int lockfree_enqueue(LockFreeTaskQueue *queue, PyObject *task) {
    TaskNode *node = PyMem_Malloc(sizeof(TaskNode));
    if (!node) return -1;
    
    node->task = Py_NewRef(task);
    node->next = NULL;
    
    TaskNode *prev_tail;
    while (1) {
        prev_tail = (TaskNode*)_Py_atomic_load_ptr(&queue->tail);
        TaskNode *next = (TaskNode*)_Py_atomic_load_ptr(&prev_tail->next);
        
        if (prev_tail == (TaskNode*)_Py_atomic_load_ptr(&queue->tail)) {
            if (next == NULL) {
                if (_Py_atomic_compare_exchange_ptr(&prev_tail->next, NULL, node)) {
                    break;
                }
            } else {
                // 帮助其他线程完成操作
                _Py_atomic_compare_exchange_ptr(&queue->tail, prev_tail, next);
            }
        }
    }
    
    _Py_atomic_compare_exchange_ptr(&queue->tail, prev_tail, node);
    _Py_atomic_add_int(&queue->size, 1);
    return 0;
}

// ============================================================================
// 3. 批量任务处理优化
// ============================================================================

#define BATCH_SIZE 16

typedef struct {
    PyObject *tasks[BATCH_SIZE];
    int count;
} TaskBatch;

// 批量收集就绪任务
int collect_ready_tasks_batch(asyncio_state *state, TaskBatch *batch) {
    batch->count = 0;
    
    // 使用单次锁操作收集多个任务
    Py_BEGIN_CRITICAL_SECTION(state->ready_queue);
    
    while (batch->count < BATCH_SIZE && !queue_is_empty(state->ready_queue)) {
        PyObject *task = queue_pop(state->ready_queue);
        if (task) {
            batch->tasks[batch->count++] = task;
        }
    }
    
    Py_END_CRITICAL_SECTION();
    
    return batch->count;
}

// 批量处理任务
int process_task_batch(asyncio_state *state, TaskBatch *batch) {
    // 预分配所需资源
    PyObject **contexts = PyMem_Malloc(batch->count * sizeof(PyObject*));
    if (!contexts) return -1;
    
    // 批量准备上下文
    for (int i = 0; i < batch->count; i++) {
        contexts[i] = get_task_context(batch->tasks[i]);
    }
    
    // 批量执行
    for (int i = 0; i < batch->count; i++) {
        // 执行任务...
        execute_task(batch->tasks[i], contexts[i]);
    }
    
    PyMem_Free(contexts);
    return 0;
}

// ============================================================================
// 4. 分级对象池优化
// ============================================================================

typedef struct {
    PyObject *small_pool[128];   // 小对象池
    PyObject *medium_pool[64];   // 中等对象池
    PyObject *large_pool[32];    // 大对象池
    int small_count;
    int medium_count;
    int large_count;
    PyMutex lock;
} TieredObjectPool;

PyObject* tiered_pool_get(TieredObjectPool *pool, size_t size) {
    PyObject *obj = NULL;
    
    PyMutex_Lock(&pool->lock);
    
    if (size <= 256 && pool->small_count > 0) {
        obj = pool->small_pool[--pool->small_count];
    } else if (size <= 1024 && pool->medium_count > 0) {
        obj = pool->medium_pool[--pool->medium_count];
    } else if (size <= 4096 && pool->large_count > 0) {
        obj = pool->large_pool[--pool->large_count];
    }
    
    PyMutex_Unlock(&pool->lock);
    
    if (!obj) {
        // 分配新对象
        obj = allocate_new_object(size);
    }
    
    return obj;
}

// ============================================================================
// 5. 动态公平性调整
// ============================================================================

typedef struct {
    PyMutex base_mutex;
    atomic_int wait_time_avg;  // 平均等待时间（微秒）
    atomic_int fairness_timeout_us;  // 动态公平性超时
} AdaptiveFairMutex;

void update_fairness_timeout(AdaptiveFairMutex *mutex, int64_t wait_time_us) {
    // 使用指数移动平均更新平均等待时间
    int old_avg = atomic_load(&mutex->wait_time_avg);
    int new_avg = (old_avg * 7 + wait_time_us) / 8;
    atomic_store(&mutex->wait_time_avg, new_avg);
    
    // 根据平均等待时间调整公平性超时
    if (new_avg < 100) {
        // 低延迟场景：更短的公平性超时
        atomic_store(&mutex->fairness_timeout_us, 500);
    } else if (new_avg < 1000) {
        // 中等延迟场景
        atomic_store(&mutex->fairness_timeout_us, 1000);
    } else {
        // 高延迟场景：更长的公平性超时
        atomic_store(&mutex->fairness_timeout_us, 2000);
    }
}

// ============================================================================
// 6. 工作窃取队列
// ============================================================================

typedef struct {
    PyObject **tasks;
    atomic_int top;     // 本地线程推入/弹出
    atomic_int bottom;  // 其他线程窃取
    size_t capacity;
} WorkStealingQueue;

// 本地线程推入任务
int ws_push(WorkStealingQueue *q, PyObject *task) {
    int b = atomic_load(&q->bottom);
    int t = atomic_load_acquire(&q->top);
    
    if (b - t >= q->capacity) {
        // 队列满，需要扩容
        return -1;
    }
    
    q->tasks[b % q->capacity] = task;
    atomic_store_release(&q->bottom, b + 1);
    return 0;
}

// 其他线程窃取任务
PyObject* ws_steal(WorkStealingQueue *q) {
    int t = atomic_load_acquire(&q->top);
    int b = atomic_load_acquire(&q->bottom);
    
    if (t >= b) {
        return NULL;  // 队列空
    }
    
    PyObject *task = q->tasks[t % q->capacity];
    
    if (!atomic_compare_exchange_strong(&q->top, &t, t + 1)) {
        return NULL;  // 窃取失败
    }
    
    return task;
}

// ============================================================================
// 7. 优化的 GIL 释放策略
// ============================================================================

#define AGGRESSIVE_GIL_RELEASE_THRESHOLD_NS 10000  // 10 微秒

typedef struct {
    int64_t start_time;
    int64_t expected_duration;
    bool should_release_gil;
} GILReleaseContext;

void gil_release_init(GILReleaseContext *ctx, int64_t expected_ns) {
    ctx->start_time = get_monotonic_time_ns();
    ctx->expected_duration = expected_ns;
    ctx->should_release_gil = (expected_ns > AGGRESSIVE_GIL_RELEASE_THRESHOLD_NS);
}

#define GIL_RELEASE_START(ctx) \
    do { \
        if ((ctx)->should_release_gil) { \
            Py_BEGIN_ALLOW_THREADS
            
#define GIL_RELEASE_END(ctx) \
            Py_END_ALLOW_THREADS \
        } \
    } while(0)

// 使用示例
void optimized_io_operation(void *data, size_t size) {
    GILReleaseContext ctx;
    // 估计操作时间基于数据大小
    int64_t expected_ns = size * 100;  // 假设每字节 100ns
    
    gil_release_init(&ctx, expected_ns);
    
    GIL_RELEASE_START(&ctx);
    // 执行 I/O 操作
    perform_io(data, size);
    GIL_RELEASE_END(&ctx);
}