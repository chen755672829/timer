#include <stdio.h>
#include "spinlock.h"
#include "timewheel.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#include <sys/time.h>
#include <mach/task.h>
#include <mach/mach.h>
#else
#include <time.h>
#endif

typedef struct link_list {
    timer_node_t head;
    timer_node_t *tail;
}link_list_t;

typedef struct timer {
    link_list_t near[TIME_NEAR];   // 时间轮的最外层级
    link_list_t t[4][TIME_LEVEL];  // 时间轮四层级
    struct spinlock lock;          // 自旋锁
    uint32_t time;                 // 时间走针，+1：代表时间过了10ms。
    uint32_t starttime;            // 开始时间
    uint64_t current;              // 记录经过多次时间，单位为10ms。
    uint64_t current_point;        // 当前具体时间点，单位为10ms。
}s_timer_t;

static s_timer_t * TI = NULL;

/***
 * 取出一串定时任务
 * 取出定时任务后，然后又把链表进行初始化，方便下一次插入定时任务
 * 与nginx使用的链表有异曲同工之妙
***/
timer_node_t *
link_clear(link_list_t *list) {
    timer_node_t * ret = list->head.next;
    list->head.next = 0;
    list->tail = &(list->head);
    return ret;
}


/***
 * 把定时任务插入当前链表
 * 这里也是使用的tail指针，进行插入的。
***/
void
link(link_list_t *list, timer_node_t *node) {
    list->tail->next = node;
    list->tail = node;
    node->next = 0;
}


/***
 * brief: 把定时任务插入到具体层对应的格子的链表中
 * @param1 [in] s_timer_t *T; 定时器
 * @param2 [in] timer_node_t *node  定时任务节点。
***/
#if 0
/***
 *  第1种表示方法 
 *  time 是 当定时器 T->time走针等于 time时，这个定时任务才会执行。
 *  current_time为当前定时器 T->time走针，走到的值，也是走到具体格子的位置。
 *  mses：为T->time走针，还需要走多少，才能执行这个定时任务。也就是还剩下多长时间，才能执行这个定时任务任务。
 *  若msec 小于 2^8，将把任务插入到最外层中，具体插入到那个格子中，time &（2^8-1) 的位置。请查看我的解释文档
***/
void
add_node(s_timer_t *T, timer_node_t *node) {
    uint32_t time=node->expire;
    uint32_t current_time=T->time;
    uint32_t msec = time - current_time;
    if (msec < TIME_NEAR) { //[0, 0x100)
        link(&T->near[time&TIME_NEAR_MASK],node);
    } else if (msec < (1 << (TIME_NEAR_SHIFT+TIME_LEVEL_SHIFT))) {//[0x100, 0x4000)
        link(&T->t[0][((time>>TIME_NEAR_SHIFT) & TIME_LEVEL_MASK)], node);
    } else if (msec < (1 << (TIME_NEAR_SHIFT+2*TIME_LEVEL_SHIFT))) {//[0x4000, 0x100000)
        link(&T->t[1][((time>>(TIME_NEAR_SHIFT + TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);	
    } else if (msec < (1 << (TIME_NEAR_SHIFT+3*TIME_LEVEL_SHIFT))) {//[0x100000, 0x4000000)
        link(&T->t[2][((time>>(TIME_NEAR_SHIFT + 2*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);
    } else {//[0x4000000, 0xffffffff]
        link(&T->t[3][((time>>(TIME_NEAR_SHIFT + 3*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);
    }
}
#else
/*** 
 *  第2种表示方法 
 * time|TIME_NEAR_MASK  = time | (11111111) 得到值，后8位一定为1。
 * time|TIME_NEAR_MASK 与 current_time|TIME_NEAR_MASK 比较是否相等，比较就是前24位，是否相等了。若相等，就要插入到最外层；前24位若相等，那么 就说明 time-current_time的值一定小于  2^8，与第一种方法 异曲同工
***/
static void
add_node(struct timer *T,struct timer_node *node) {
    uint32_t time=node->expire;
    uint32_t current_time=T->time;

    // 最外层，这是 或(|) 不是 与(&)
    if ((time|TIME_NEAR_MASK) == (current_time|TIME_NEAR_MASK)) {
        link(&T->near[time&TIME_NEAR_MASK],node);
    } else {
        int i;
        uint32_t mask = TIME_NEAR << TIME_LEVEL_SHIFT;
        // 从第一层开始遍历
        for (i = 0; i < 3; i++) {
            if ((time|(mask-1)) == (current_time|(mask-1))) {
                break;
            }
            mask <<= TIME_LEVEL_SHIFT;
        }
        link(&T->t[i][((time>>(TIME_NEAR_SHIFT + i*TIME_LEVEL_SHIFT)) & TIME_LEVEL_MASK)],node);
    }
}
#endif


/***
 * brief: 添加一个定时任务
 * @param1 [in] int time; 定时器任务的超时时间，单位为10ms
 * @param2 [in] func  定时器具体需要执行的事件任务函数
 * @param3 [in] threadid  回调函数具体参数。
 * return timer_node_t* 定时任务节点指针。
***/
timer_node_t*
add_timer(int time, handler_pt func, int threadid) {
    timer_node_t *node = (timer_node_t *)malloc(sizeof(timer_node_t) + sizeof(timer_event_t));
    node->event[0].callback = func;
    node->event[0].id = threadid;

    spinlock_lock(&TI->lock);
    // TI->time 指针走到等于 expire值时，会执行node中事件任务
    node->expire = time + TI->time;

    if (time <= 0) {
        spinlock_unlock(&TI->lock);
        node->event[0].callback(node);    // 这里也是直接加入到就绪队列中
        free(node);
        return NULL;
    }
    add_node(TI, node);
    spinlock_unlock(&TI->lock);
    return node;
}


/***
 * brief:重映射时，调用这个函数，取出当前格子中的所有任务，
 *  并把当前格子进行初始化，然后再把任务一个一个的插入（重映射）到时间轮定时器中
 * @param1 [in] s_timer_t *T 为时间轮定时器结构
 * @param2 [in] level 要重映射的任务在那一层。
 * @param3 [in] idx  要重映射的任务为这一层的哪一个格子。
***/
void
move_list(s_timer_t *T, int level, int idx) {
    timer_node_t *current = link_clear(&T->t[level][idx]);
    while (current) {
        timer_node_t *temp = current->next;
        add_node(T, current);
        current=temp;
    }
}


/***
 * timer_shift重映射。这个函数中需要注意点：
 * (1) 每一次调用timer_shift函数，定时器时间走针T->time 会加+1，看在expire_timer函数中调用timer_update的时机。
 * (2) 当时间走针 T->time%256==0 才会进行第一次检测重映射，只有时间走完一圈最外层，才有进行重映射的必要。
 * 重映射就是把 第一层格子中个任务映射到最外层。把第二层格子中的任务映射到第一层或最外层 ....... 请看文档链接的 3.1和3.3小结
 * (3) ct == 0 时，执行move_list(T, 3, 0); 请看文档链接中的 4.2.2.2小节的讲解。
***/
void
timer_shift(s_timer_t *T) {
    int mask = TIME_NEAR;
    uint32_t ct = ++T->time;
    // ct == 0 表示 T->time = 2^32次方，请看4.2.2.2小节讲解
    if (ct == 0) {
        move_list(T, 3, 0);
    } else {
        // 获取前24位
        uint32_t time = ct >> TIME_NEAR_SHIFT;  // time 为去除第一轮外的时间, 剩下的前24位
        int i=0;
        // ct % 256 == 0
        // ct 是当前时间轮走到哪了。 ct & (mask-1) == 0 开始只有当 ct 走完一轮 256后，ct对256取余等于0  
        while ((ct & (mask-1)) == 0) {
            int idx = time & TIME_LEVEL_MASK;
            if (idx != 0) {
                move_list(T, i, idx);
                break;
            }
            mask <<= TIME_LEVEL_SHIFT;
            time >>= TIME_LEVEL_SHIFT;
            ++i;
        }
    }
}


/***
 * 遍历这一串到期任务，并执行这些任务或把任务加入到就绪队列中去
***/
void
dispatch_list(timer_node_t *current) {
    do {
        timer_node_t * temp = current;
        current=current->next;
#if DELETE_TIMENODE_SWITCH
        if (temp->cancel == 0)
#endif
        temp->event[0].callback(temp);  // 这里可以把事件加入到 一个就绪队列中，让线程池进行处理。
        free(temp);
    } while (current);
}

/***
 * 检测最外层任务当前的格子中是否存在任务，若这个格子中有任务，就任务说明到期了
***/
void
timer_execute(s_timer_t *T) {
    int idx = T->time & TIME_NEAR_MASK;
    while (T->near[idx].head.next) {
        timer_node_t *current = link_clear(&T->near[idx]);
        spinlock_unlock(&T->lock);
        dispatch_list(current);
        spinlock_lock(&T->lock);
    }
}

void 
timer_update(s_timer_t *T) {
    spinlock_lock(&T->lock);
    timer_execute(T);
    // 检测是否需要，重新映射
    timer_shift(T);
    // 检测第一层级是否过期
    timer_execute(T);
    spinlock_unlock(&T->lock);
}


#if DELETE_TIMENODE_SWITCH
void
del_timer(timer_node_t *node) {
    node->cancel = 1;
}
#endif


/***
 * 创建时间轮定时器。
***/
s_timer_t *
timer_create_timer() {
    s_timer_t *r=(s_timer_t *)malloc(sizeof(s_timer_t));
    memset(r,0,sizeof(*r));
    int i,j;
    for (i=0;i<TIME_NEAR;i++) {
        link_clear(&r->near[i]);
    }
    for (i=0; i<4; i++) {
        for (j=0;j<TIME_LEVEL;j++) {
            link_clear(&r->t[i][j]);
        }
    }
    spinlock_init(&r->lock);
    r->current = 0;
    return r;
}


/***
 * 获取当前时间，这里当前时间值，是用10ms进行表示的。这个可以根据自己的需求进行调。
***/
uint64_t
gettime() {
    uint64_t t;
#if !defined(__APPLE__) || defined(AVAILABLE_MAC_OS_X_VERSION_10_12_AND_LATER)
    //printf("%s  clock_gettime(CLOCK_MONOTONIC, &ti)\n", __func__);
    struct timespec ti;
    clock_gettime(CLOCK_MONOTONIC, &ti);
#if 1
    // 定时器精确到10ms 10毫秒
    t = (uint64_t)ti.tv_sec * 100;
    t += ti.tv_nsec / 10000000;
#else
    // 定时器精确到 1ms
    t = (uint64_t)ti.tv_sec * 1000;
    t += ti.tv_nsec / 1000000;
#endif
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    // 单位10ms
    t = (uint64_t)tv.tv_sec * 100;
    t += tv.tv_usec / 10000;
#endif
	return t;
}


/***
 * brief: 探测任务到期
 * 其中需要注意的是
 * (1) gettime 这个函数获取的时间值，单位为10ms,赋值于cp，也就是只有时间经过了10ms, cp的值才会变化。
 * (2) 调用expire_timer，由于需要精确到10ms，这里是每个时间精度的1/4时间就要检测一次，也就每过2.5ms就调用一次expire_timer
 * (3) diff 表示时间已经经过了多少个10ms了
***/
void
expire_timer(void) {
    uint64_t cp = gettime();
    if(cp < TI->current_point) {
        fprintf(stderr, "%s current time less record time cp == %llu TI== %llu time error\n",
                __func__, cp, TI->current_point);
        TI->current_point = cp;
    } else if (cp != TI->current_point) {
        uint32_t diff = (uint32_t)(cp - TI->current_point);
        TI->current_point = cp;
        int i;
        for (i=0; i<diff; i++) {
            timer_update(TI);
        }
    }
}


/***
 * 创建和初始化定时器
***/
void 
init_timer(void) {
    TI = timer_create_timer();
    TI->current_point = gettime();
}


/***
 * 清理定时器资源。
***/
void
clear_timer() {
    int i,j;
    for (i=0;i<TIME_NEAR;i++) {
        link_list_t * list = &TI->near[i];
        timer_node_t* current = list->head.next;
        while(current) {
            timer_node_t * temp = current;
            current = current->next;
            free(temp);
        }
        link_clear(&TI->near[i]);
    }
    for (i=0;i<4;i++) {
        for (j=0;j<TIME_LEVEL;j++) {
            link_list_t * list = &TI->t[i][j];
            timer_node_t* current = list->head.next;
            while (current) {
                timer_node_t * temp = current;
                current = current->next;
                free(temp);
            }
            link_clear(&TI->t[i][j]);
        }
    }
}
