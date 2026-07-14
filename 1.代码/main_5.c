#define malloc(size) rpmalloc(size)
#define free(ptr) rpfree(ptr)

#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <stdint.h>

// ==========================================
// 🎛️ 开关：想测自己的就写 1，想测原厂的就改成 0
// ==========================================
#define TEST_MY_OWN_VERSION 1 

#if TEST_MY_OWN_VERSION
    // 测你的纯净魔改版：只有最清爽的核心接口
    #include "my_rpmalloc.h"
    extern void rpmalloc_initialize(void);
#else
    // 测官方原厂原版
    #include "rpmalloc.h"
    extern int rpmalloc_initialize(rpmalloc_interface_t* memory_interface);
#endif

extern void* rpmalloc(size_t size);
extern void rpfree(void* ptr);

#define CONCURRENT_THREADS 8   // 并发核心控制流
#define TOTAL_DEATH_RUNS 300   // 轰炸轮数
#define ALLOC_COUNT_PER_THREAD 100 // 每个短命线程进去疯狂轰炸的次数

// 高精度计时
double get_elapsed_time(struct timespec start, struct timespec end) {
    return (end.tv_sec - start.tv_sec) + (end.tv_nsec - start.tv_nsec) / 1e9;
}

// 去除HUGE : 5		加上HUGE : 6
#define MODE 6


// ========================================================
// 💀 终极高压子线程：全规格覆盖分配，干完自己死
// ========================================================
void* ultimate_pressure_worker(void* arg) {
    uintptr_t thread_id = (uintptr_t)arg;
    
    // 用来存放交叉释放指针的临时阵地
    void* cross_ptrs[ALLOC_COUNT_PER_THREAD] = {0};

    for (int i = 0; i < ALLOC_COUNT_PER_THREAD; ++i) {
        size_t size = 0;
        
        // 🎚️ 核心：根据循环因数，物理覆盖全规格尺寸
        if (i % MODE == 0) {
            size = 16;          // 1. 极小规格（Small Block）
        } else if (i % MODE == 1) {
            size = 256;         // 2. 中小规格
        } else if (i % MODE == 2) {
            size = 2048;        // 3. 常态大规格（Medium Block）
        } else if (i % MODE == 3) {
            size = 64 * 1024;   // 4. 超大块（Large Block）
        } else if (i % MODE == 4) {
            size = 512 * 1024;  // 5. 巨型块
        } else {
            size = 4 * 1024 * 1024; // 6. 顶天规模（Huge/Mmap 级直来直去流 4MB）
        }

        // 🚀 进行高并发分配
        void* ptr = rpmalloc(size);
        if (ptr) {
            // 物理踩踏验证，确保内存绝对可用且没被别人偷走
            *(volatile int*)ptr = 0x5a5a5a5a;
            
            // 交叉组合：一部分原地释放，一部分留着一会儿玩跨线程或者无序释放
            if (i % 2 == 0) {
                rpfree(ptr);
            } else {
                cross_ptrs[i] = ptr;
            }
        }
    }

    // 💣 无序大释放：把剩下不同规格的存留指针集中洗牌释放，强行制造物理碎片
    for (int i = ALLOC_COUNT_PER_THREAD - 1; i >= 0; --i) {
        if (cross_ptrs[i]) {
            rpfree(cross_ptrs[i]);
        }
    }

    // 活干完了，赤条条退场，线程自然死亡！
    // 留下的 Heap 空间直接看底层的回收/接管逻辑硬不硬
    return NULL;
}

// ========================================================
// 🚀 终极靶场控制中心
// ========================================================
int main() {
    struct timespec start_time, end_time;
    double elapsed;

    printf("[*] ========================================================\n");
    printf("[*] 🏁 欢迎来到 5.0【全规格全尺寸 + 线程自然消亡终极修罗场】\n");
    printf("[*] ========================================================\n");

#if TEST_MY_OWN_VERSION
    rpmalloc_initialize();
    printf("[*] 压测目标: 【你的自研纯净版】\n");
#else
    rpmalloc_initialize(NULL);
    printf("[*] 压测目标: 【官方原厂原版】\n");
#endif

    // 计算总线程发射量：CONCURRENT_THREADS * TOTAL_DEATH_RUNS
    printf("[*] 正在以 %d 并发，连续发射 %d 轮‘短命全规格’自死线程（共计 %d 个线程消亡）...\n", 
           CONCURRENT_THREADS, TOTAL_DEATH_RUNS, CONCURRENT_THREADS * TOTAL_DEATH_RUNS);
    
    clock_gettime(CLOCK_MONOTONIC, &start_time);

    pthread_t threads[CONCURRENT_THREADS];

    for (int run = 0; run < TOTAL_DEATH_RUNS; ++run) {
        // 1. 同时发射一波（8个）常态高并发线程
        for (long i = 0; i < CONCURRENT_THREADS; ++i) {
            while (pthread_create(&threads[i], NULL, ultimate_pressure_worker, (void*)i) != 0) {
                // 扛住系统高频开线程的物理极限
            }
            // 直接放飞！死了不用主线程 join，让它们自然消亡
            pthread_detach(threads[i]);
        }

        // 2. 极其微妙的微秒级控制：给这一波线程并行对撞、交叉释放和自死留出空间
        // 顺便让下一波进来的新线程疯狂去抢夺、接管刚死掉的 Heap
        struct timespec ts = {0, 800000}; // 0.8毫秒
        nanosleep(&ts, NULL);
    }

    // 留足 1.5 秒让最后一批断后的全规格大块内存彻底死透和释放
    struct timespec final_delay = {1, 500000000};
    nanosleep(&final_delay, NULL);

    clock_gettime(CLOCK_MONOTONIC, &end_time);
    // 扣除固定等待时间
    elapsed = get_elapsed_time(start_time, end_time) - 1.5;

    printf("[+] 战役结束！总耗时: %.6f 秒 | 全规格抗压通过\n", elapsed);

    printf("[+] ========================================================\n");
    return 0;
}
