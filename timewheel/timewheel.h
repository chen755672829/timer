#ifndef _MARK_TIMEWHEEL_
#define _MARK_TIMEWHEEL_

#include <stdint.h>

#define TIME_NEAR_SHIFT 8
#define TIME_NEAR (1 << TIME_NEAR_SHIFT)
#define TIME_LEVEL_SHIFT 6
#define TIME_LEVEL (1 << TIME_LEVEL_SHIFT)
#define TIME_NEAR_MASK (TIME_NEAR-1)
#define TIME_LEVEL_MASK (TIME_LEVEL-1)

/***
 *  时间轮定时器，是否开启删除节点操作
 *  这里不建议开启删除操作：
 *  (1) 首先因为skynet中就没有删除这个操作。
 *  (2) 因为删除节点操作，可能会出现错误，timer_node节点可能已经被实现，
 *      也可以避免这种情况，当释放节点空间时，获取自旋锁，当删除节点时，获取自旋锁
 *  (3) 就算使用自旋锁避免了，但是也有可能出现问题，因为当删除时，获取自旋锁，可能这时候时间任务已经加入到任务就绪队列中，等待执行了，
        就算，你获取到自旋锁然后，把cancel 赋予1，也无事于补了
    综合三个点，就是说明时间轮定时器，不要有删除操作，会出问题的。
***/ 
#define DELETE_TIMENODE_SWITCH  0

typedef struct timer_node timer_node_t;
typedef void (*handler_pt) (struct timer_node *node);

typedef struct timer_event {
    handler_pt callback;
    int id;     //此时携带参数
} timer_event_t;

struct timer_node {
    struct timer_node *next;
    uint32_t expire;    // 定时器中的time指针，走到expire值时，会执行当前任务。
#if DELETE_TIMENODE_SWITCH
    uint8_t cancel;
#endif
    timer_event_t event[0];
};

timer_node_t* add_timer(int time, handler_pt func, int threadid);

void expire_timer(void);

#if DELETE_TIMENODE_SWITCH
void del_timer(timer_node_t* node);
#endif

void init_timer(void);

void clear_timer();

#endif
