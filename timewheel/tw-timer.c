#include <stdio.h>
#include <unistd.h>

#include <pthread.h>
#include <time.h>
#include <stdlib.h>
#include "timewheel.h"

struct context {
    volatile int quit;
    int thread;
};

struct thread_param {
    struct context *ctx;
    int id;
};

static struct context ctx = {0};

/* 每隔100*10ms =1s 调用一次do_timer函数*/
void do_timer(timer_node_t *node) {
    //printf("do_timer expired:%d - thread-id:%d\n", node->expire, node->event[0].id);
    add_timer(100, do_timer, node->event[0].id);
}

//若以后写项目中想一直探测此任务的话，就像这样使用，每隔100*10ms =1s 调用一次do_clock函数。
void do_clock(timer_node_t *node) {
    static int time;
    time ++;
    printf("---time = %d ---\n", time);
    add_timer(100, do_clock, node->event[0].id);
}

void* thread_worker(void *p) {
    struct thread_param *tp = p;
    int id = tp->id;
    struct context *ctx = tp->ctx;
    int expire = rand() % 200; 
    add_timer(expire, do_timer, id);
    while (!ctx->quit) {
        usleep(1000);
    }
    printf("thread_worker:%d exit!\n", id);
    return NULL;
}

void do_quit(timer_node_t * node) {
    ctx.quit = 1;
}

int main() {
    srand(time(NULL));
    ctx.thread = 2;
    pthread_t pid[ctx.thread];

    init_timer();
    // 添加一个6000*10ms = 60s 后到期的一个定时任务
    add_timer(6000, do_quit, 100);
    add_timer(0, do_clock, 100);
    struct thread_param task_thread_p[ctx.thread];
    int i;
    for (i = 0; i < ctx.thread; i++) {
        task_thread_p[i].id = i;
        task_thread_p[i].ctx = &ctx;
        if (pthread_create(&pid[i], NULL, thread_worker, &task_thread_p[i])) {
            fprintf(stderr, "create thread failed\n");
            exit(1);
        }
    }

    // 一般会另外起一个定时器线程，进行判断时间轮定时器中的任务是否到期。
    // 定时任务到期后，一般会加入到 消息队列中去 或由 线程池进行执行。
    while (!ctx.quit) {
        expire_timer();
        usleep(2500); // 每隔2.5ms进行检测一下
    }
    clear_timer();
    for (i = 0; i < ctx.thread; i++) {
        pthread_join(pid[i], NULL);
    }
    printf("all thread is closed\n");
    return 0;
}

// 本文件目的为测试时间轮定时器。
// gcc tw-timer.c timewheel.c -o tw  -lpthread 