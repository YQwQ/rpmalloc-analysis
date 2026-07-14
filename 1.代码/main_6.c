#define malloc(size) rpmalloc(size)
#define free(ptr) rpfree(ptr)

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>
#include <stdbool.h>

// ==========================================
// 🎛️ 开关：想测自己的就写 1，想测原厂的就改成 0
// ==========================================
#define TEST_MY_OWN_VERSION 1

#if TEST_MY_OWN_VERSION
    #include "my_rpmalloc.h"
    extern void rpmalloc_initialize(void);
#else
    #include "rpmalloc.h"
    extern int rpmalloc_initialize(rpmalloc_interface_t* memory_interface);
#endif

extern void* rpmalloc(size_t size);
extern void rpfree(void* ptr);

#define CONCURRENT_PAIRS 4     // 4对（共8个）主控核心链路
#define TOTAL_BURST_RUNS 200   // 轰炸轮数
#define BURST_COUNT 100        // 每轮中转站的指针上限

// 全局无锁中转站（极其简易的无锁环形缓冲区，专门用来传递指针发生跨线程冲突）
void* g_pointer_bridge[BURST_COUNT];
volatile int g_produce_idx = 0;
volatile int g_consume_idx = 0;

// 高精度计时
double get_elapsed_time(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

// ========================================================
// 🏭 生产线程（负责申请、写入、放飞指针，然后自然死亡）
// ========================================================
void* cross_thread_producer(void* arg) {
    uintptr_t id = (uintptr_t)arg;
    
    for (int i = 0; i < BURST_COUNT / CONCURRENT_PAIRS; ++i) {
        size_t size = 0;
        // 物理覆盖全规格尺寸（去除 4MB HUGE 影响）
        int spec = i % 5;
        if (spec == 0)      size = 16;
        else if (spec == 1) size = 256;
        else if (spec == 2) size = 2048;
        else if (spec == 3) size = 64 * 1024;
        else                size = 512 * 1024;

        void* ptr = rpmalloc(size);
        if (ptr) {
            *(volatile int*)ptr = 0x66666666; // 写入魔数标记
            
            // 挤入无锁中转站发送给消费者
            int current_p;
            do {
                current_p = g_produce_idx;
            } while (!__sync_bool_compare_and_swap(&g_produce_idx, current_p, (current_p + 1) % BURST_COUNT));
            
            // 等待消费者把这里消费掉
            while (g_pointer_bridge[current_p] != NULL) {
                __sync_synchronize();
            }
            g_pointer_bridge[current_p] = ptr;
        }
    }
    return NULL;
}

// ========================================================
// 🛒 消费线程（从别处捞指针，帮已经死了/在死路上的兄弟跨线程收尸）
// ========================================================
void* cross_thread_consumer(void* arg) {
    for (int i = 0; i < BURST_COUNT / CONCURRENT_PAIRS; ++i) {
        int current_c;
        do {
            current_c = g_consume_idx;
        } while (!__sync_bool_compare_and_swap(&g_consume_idx, current_c, (current_c + 1) % BURST_COUNT));

        // 等待生产者塞入指针
        while (g_pointer_bridge[current_c] == NULL) {
            __sync_synchronize();
        }

        void* ptr = g_pointer_bridge[current_c];
        
        // 跨线程数据校验，确保内存原子可见性没毛病
        if (*(volatile int*)ptr != 0x66666666) {
            printf("[!] 警告：跨线程内存数据发生物理践踏或未对齐错误！\n");
        }

        // 🔥 核心物理测试点：跨线程释放（Cross-Thread Free）
        // 当前线程释放属于 producer 那个线程的 Heap 块！
        rpfree(ptr);
        
        // 清空站台位
        g_pointer_bridge[current_c] = NULL;
    }
    return NULL;
}

// ========================================================
// 🚀 终极靶场控制中心
// ========================================================
int main() {
    struct timespec start_time, end_time;
    double elapsed;

    printf("[*] ========================================================\n");
    printf("[*] 🏁 欢迎来到 6.0【终极跨线程释放 + 全规格 + 线程自死靶场】\n");
    printf("[*] ========================================================\n");

#if TEST_MY_OWN_VERSION
    rpmalloc_initialize();
    printf("[*] 压测目标: 【你的自研纯净版】\n");
#else
    rpmalloc_initialize(NULL);
    printf("[*] 压测目标: 【官方原厂原版】\n");
#endif

    printf("[*] 正在并行发射 %d 轮‘跨线程交替对撞’自死线程（共计 %d 个线程消亡）...\n", 
           TOTAL_BURST_RUNS, CONCURRENT_PAIRS * 2 * TOTAL_BURST_RUNS);
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    pthread_t prods[CONCURRENT_PAIRS];
    pthread_t cons[CONCURRENT_PAIRS];

    for (int run = 0; run < TOTAL_BURST_RUNS; ++run) {
        // 同时派生并发的生产者和消费者
        for (long i = 0; i < CONCURRENT_PAIRS; ++i) {
            pthread_create(&prods[i], NULL, cross_thread_producer, (void*)i);
            pthread_create(&cons[i], NULL, cross_thread_consumer, (void*)i);
            
            // 脱离父子关系，放飞！让他们自然死、自己销毁句柄
            pthread_detach(prods[i]);
            pthread_detach(cons[i]);
        }

        // 控制突发（Burst）流速度，给无锁队列对撞、跨线程链表挂载留出硬件物理对撞时间
        struct timespec ts = {0, 1500000}; // 1.5毫秒碎步
        nanosleep(&ts, NULL);
    }

    // 断后等待时间，留足 1.5 秒让最后一批远程跨线程释放彻底收尸干净
    struct timespec final_delay = {1, 500000000};
    nanosleep(&final_delay, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    elapsed = get_elapsed_time(start_time, end_time) - 1.5;

    printf("[+] 战役结束！总耗时: %.6f 秒 | 跨线程无锁收尸流通过\n", elapsed);

    printf("[+] ========================================================\n");
    return 0;
}
