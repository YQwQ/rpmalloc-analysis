#include "my_rpmalloc.h"

#include <errno.h>
#include <string.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdatomic.h>	// for atomic

#include <sys/mman.h>	// for madvise
#include <sched.h>
#include <unistd.h>	// for sysconf
#include <pthread.h>	// for pthread_key_create

#include <fcntl.h>	// for open
#include <sys/prctl.h>	// for prctl

// 工具类
typedef struct rpmalloc_config_t {
	size_t page_size;		// 一页大小
	int enable_huge_pages;		// 有大页功能吗
	int disable_decommit;		// 关闭 decommit吗
	const char* page_name;		// 
	const char* huge_page_name;	// 
	int disable_thp;		// 关闭 透明大页吗
} rpmalloc_config_t;

#define NOINLINE __attribute__((noinline))

///////////
///
/// size
///
///////
#define PAGE_HEADER_SIZE 128
#define SPAN_HEADER_SIZE PAGE_HEADER_SIZE

#define SMALL_GRANULARITY 16

#define SMALL_BLOCK_SIZE_LIMIT (4 * 1024)
#define MEDIUM_SMALL_BLOCK_SIZE_LIMIT (32 * 1024)
#define MEDIUM_LARGE_BLOCK_SIZE_LIMIT (256 * 1024)
#define LARGE_BLOCK_SIZE_LIMIT (2 * 1024 * 1024)

#define SMALL_SIZE_CLASS_COUNT 73
#define MEDIUM_SMALL_SIZE_CLASS_COUNT 12
#define MEDIUM_LARGE_SIZE_CLASS_COUNT 12
#define LARGE_SIZE_CLASS_COUNT 12
// 73 + 12 + 12 + 12 = 109
#define SIZE_CLASS_COUNT \
        (SMALL_SIZE_CLASS_COUNT + MEDIUM_SMALL_SIZE_CLASS_COUNT + MEDIUM_LARGE_SIZE_CLASS_COUNT + LARGE_SIZE_CLASS_COUNT)

#define SMALL_PAGE_SIZE_SHIFT 16
#define SMALL_PAGE_SIZE (1 << SMALL_PAGE_SIZE_SHIFT)
#define SMALL_PAGE_MASK (~((uintptr_t)SMALL_PAGE_SIZE - 1))
#define MEDIUM_SMALL_PAGE_SIZE_SHIFT 20
#define MEDIUM_SMALL_PAGE_SIZE (1 << MEDIUM_SMALL_PAGE_SIZE_SHIFT)
#define MEDIUM_SMALL_PAGE_MASK (~((uintptr_t)MEDIUM_SMALL_PAGE_SIZE - 1))
#define MEDIUM_LARGE_PAGE_SIZE_SHIFT 22
#define MEDIUM_LARGE_PAGE_SIZE (1 << MEDIUM_LARGE_PAGE_SIZE_SHIFT)
#define MEDIUM_LARGE_PAGE_MASK (~((uintptr_t)MEDIUM_LARGE_PAGE_SIZE - 1))
#define LARGE_PAGE_SIZE_SHIFT 24
#define LARGE_PAGE_SIZE (1 << LARGE_PAGE_SIZE_SHIFT)
#define LARGE_PAGE_MASK (~((uintptr_t)LARGE_PAGE_SIZE - 1))

#define SPAN_SIZE (256 * 1024 * 1024)
#define SPAN_MASK (~((uintptr_t)(SPAN_SIZE - 1)))

// 全局钥匙
static pthread_key_t pthread_key;

#define EXPECTED(x) __builtin_expect((x), 1)	// CPU 流水线预测成功
#define UNEXPECTED(x) __builtin_expect((x), 0)	// CPU 流水线预测失败

#define pointer_offset(ptr, ofs) (void*)((char*)(ptr) + (ptrdiff_t)(ofs)) // ptr + ofs
#define pointer_diff(first, second) (ptrdiff_t)((const char*)(first) - (const char*)(second)) // 距离

#ifndef ENABLE_ASSERT
#define ENABLE_ASSERT 0	// 先开1，后续关上
#endif

#include<assert.h>
#if ENABLE_ASSERT	// 标准写法
#define rpmalloc_assert(truth, message) \
	do {                                \
		if (!(truth)) {                 \
			assert((truth) && message); \
		}				\
	} while(0)
#else
#define rpmalloc_assert(truth, message)	\
	do{				\
	} while (0)
#endif

// 更快速的memset, 但是有限制
#if __has_builtin(__builtin_memset_inline)
#define memset_const(x, y, s) __builtin_memset_inline(x, y, s)
#else
#define memset_const(x, y, s) memset(x, y, s)
#endif

///////////
/// 低级函数抽象
/////

static inline size_t
rpmalloc_clz(uint64_t x) {
	return (size_t)__builtin_clzll(x);	// 最高位左边有多少个0
}

static inline void
wait_spin(void){
	__asm__ volatile("pause" ::: "memory"); // 汇编 自旋
}



// 内存页 的规格种类
typedef enum page_type_t {
	PAGE_SMALL,		// 64 KB
	PAGE_MEDIUM_SMALL,	// 1  MB
	PAGE_MEDIUM_LARGE,	// 4  MB
	PAGE_LARGE,		// 16 MB
	PAGE_HUGE
} page_type_t;

// 
typedef struct size_class_t {
	// 块大小
	uint32_t block_size;
	// 块数量
	uint32_t block_count;
} size_class_t;

typedef struct block_t block_t;
//
struct block_t {
	block_t* next;
};

typedef struct heap_t heap_t;
typedef struct span_t span_t;
typedef struct page_t page_t;

// span_t 分割出的小块
struct page_t {
	uint32_t size_class;		// 109个规格中的一个
	uint32_t block_size;		// block大小
	uint32_t block_count;		// 能放多少个 block
	uint32_t block_initialized;	// 被划分多少个 block 走了
	uint32_t block_used;		// 被heap拿走的数量
	page_type_t page_type;		// page的规格
	uint32_t is_full : 1;		// 是否已满
	uint32_t is_free : 1;		// 是否完全空闲
	uint32_t is_zero : 1;		// 是否初始化为0
	uint32_t is_decommitted : 1;	// 是否被 decommit
	uint32_t has_aligned_block : 1;	// 是否有对齐块 (忽略)
	uint32_t generic_free : 1;	// huge 或者 有对齐块(忽略) 或者 page是满的
	uint32_t local_free_count;	// 空闲链表中元素的个数
	block_t* local_free;		// 空闲链表
	heap_t* heap;			// 所属 heap
	page_t* next;			// 链表中的 next
	page_t* prev;			// 链表中的 prev
	atomic_ullong thread_free;	// page级快递箱，低32位 block_index, 高32位 list_count
}; // size : 72

// mmap 映射出的一大块
struct span_t {
	page_t page;			// 预留空间给page
	heap_t* heap;			// 所属 heap
	uintptr_t page_address_mask;	// 不同page规格的掩码
	uint32_t page_initialized;	// index, 被获取多少个page了
	uint32_t page_count;		// 能存放多少个 page
	uint32_t page_size;		// 一个page的大小
	page_type_t page_type;		// page 的规格
	uint32_t offset;		// 偏移量: 映射地址 + offset = span_t*
	uint64_t mapped_size;		// 实际映射大小
	span_t* next;
}; // size : 128

// 堆的控制结构体 线程独占的堆
struct heap_t {
	uintptr_t owner_thread;			// 所属线程 ID
	block_t* local_free[SIZE_CLASS_COUNT];	// 空闲的 block
	page_t* page_available[SIZE_CLASS_COUNT];	// 正在使用的 page 链表数组
	page_t* page_free[4];			// 空闲的 page, 只有4种，小 小中 大中 大, large不在
	uint32_t page_free_commit_count[4];	// 4 个不同规格(N-HUGE) 的页没decommit 的 个数
	atomic_uintptr_t thread_free[4];	// heap 级快递箱
	span_t* span_partial[4];		// 正在使用的span 4个规格 no-huge
	span_t* span_used[5];			// 正在使用的 span 链表 (第5个没用到)
	heap_t* next;
	heap_t* prev;
	uint32_t id;				// heap_id
	uint32_t finalize;			// 
	uint32_t offset;			// 原地址 + offset = heap_t* 的现在地址 一般是0
	size_t mapped_size;			// 实际映射大小 一般是一页
}; // size : 1944


////////////
///
/// 全局数据 .data .bss
///
//////

// 备用默认堆(全0)
static RPMALLOC_CACHE_ALIGNED heap_t global_heap_fallback;
// 默认堆的地址
static heap_t* global_heap_default = &global_heap_fallback;
// 可以拿来被线程初始化的 heap 链表(被销毁函数放入进这个链表)
static heap_t* global_heap_queue;
// 正在使用的 heap 链表
static heap_t* global_heap_used;
// 锁 global_heap_queue 和 global_heap_used 共用这个锁
static atomic_uintptr_t global_heap_lock;
// 全局id 计数
static atomic_uint global_heap_id = 1;
// 进程是否初始化
static int global_rpmalloc_initialized;
// 工具类
static rpmalloc_config_t global_config = {0};
// 进行了进程初始化的 线程ID
static uintptr_t global_main_thread_id;

static void heap_lock_acquire(void);
static void heap_lock_release(void);

// Size classes
// SCLASS(n)	: {16 * n, (SMALL页大小 - 128(每页的头 page_t or span_t)) / n * 16}
// MSCLASS(n)	: {块大小, 每页存多少块}
// MLCLASS(n)	: 
// LCLASS(n)	: 
#define SCLASS(n) \
	        { (n * SMALL_GRANULARITY), (SMALL_PAGE_SIZE - PAGE_HEADER_SIZE) / (n * SMALL_GRANULARITY) }
#define MSCLASS(n) \
	        { (n * SMALL_GRANULARITY), (MEDIUM_SMALL_PAGE_SIZE - PAGE_HEADER_SIZE) / (n * SMALL_GRANULARITY) }
#define MLCLASS(n) \
	        { (n * SMALL_GRANULARITY), (MEDIUM_LARGE_PAGE_SIZE - PAGE_HEADER_SIZE) / (n * SMALL_GRANULARITY) }
#define LCLASS(n) \
	        { (n * SMALL_GRANULARITY), (LARGE_PAGE_SIZE - PAGE_HEADER_SIZE) / (n * SMALL_GRANULARITY) }
static const size_class_t global_size_class[SIZE_CLASS_COUNT] = {
	SCLASS(1),	SCLASS(1),      SCLASS(2),      SCLASS(3),      SCLASS(4),      SCLASS(5),      SCLASS(6),
	SCLASS(7),      SCLASS(8),      SCLASS(9),      SCLASS(10),     SCLASS(11),     SCLASS(12),     SCLASS(13),
	SCLASS(14),     SCLASS(15),     SCLASS(16),     SCLASS(17),     SCLASS(18),     SCLASS(19),     SCLASS(20),
	SCLASS(21),     SCLASS(22),     SCLASS(23),     SCLASS(24),     SCLASS(25),     SCLASS(26),     SCLASS(27),
	SCLASS(28),     SCLASS(29),     SCLASS(30),     SCLASS(31),     SCLASS(32),     SCLASS(33),     SCLASS(34),
	SCLASS(35),     SCLASS(36),     SCLASS(37),     SCLASS(38),     SCLASS(39),     SCLASS(40),     SCLASS(41),
	SCLASS(42),     SCLASS(43),     SCLASS(44),     SCLASS(45),     SCLASS(46),     SCLASS(47),     SCLASS(48),
	SCLASS(49),     SCLASS(50),     SCLASS(51),     SCLASS(52),     SCLASS(53),     SCLASS(54),     SCLASS(55),
	SCLASS(56),     SCLASS(57),     SCLASS(58),     SCLASS(59),     SCLASS(60),     SCLASS(61),     SCLASS(62),
	SCLASS(63),     SCLASS(64),     SCLASS(80),     SCLASS(96),     SCLASS(112),    SCLASS(128),    SCLASS(160),
	SCLASS(192),    SCLASS(224),    SCLASS(256),    MSCLASS(320),   MSCLASS(384),   MSCLASS(448),   MSCLASS(512),
	MSCLASS(640),   MSCLASS(768),   MSCLASS(896),   MSCLASS(1024),  MSCLASS(1280),  MSCLASS(1536),  MSCLASS(1792),
	MSCLASS(2048),  MLCLASS(2560),  MLCLASS(3072),  MLCLASS(3584),  MLCLASS(4096),  MLCLASS(5120),  MLCLASS(6144),
	MLCLASS(7168),  MLCLASS(8192),  MLCLASS(10240), MLCLASS(12288), MLCLASS(14336), MLCLASS(16384), LCLASS(20480),
	LCLASS(24576),  LCLASS(28672),  LCLASS(32768),  LCLASS(40960),  LCLASS(49152),  LCLASS(57344),  LCLASS(65536),
	LCLASS(81920),  LCLASS(98304),  LCLASS(114688), LCLASS(131072)}; // 共109个

// heap->page_free可以存在的上限 小:16	中小:8	中大:4	大:2	huge:0
static uint32_t global_page_free_overflow[5] = {16, 8, 4, 2, 0};

// 超过上面的数了 decommit 这里决定保留前面几个 page
static uint32_t global_page_free_retain[5] = {4, 2, 1, 1, 0};


static size_t os_map_granularity;	// 操作系统映射粒度
static size_t os_page_size;		// 一页的大小



#define TLS_MODEL __attribute__((tls_model("initial-exec")))	// 固定在TLS静态存储块

// .tdata
// 线程私有 heap
static _Thread_local heap_t* global_thread_heap TLS_MODEL = &global_heap_fallback; // 初始化成默认的地址 省判断

static inline uintptr_t
get_thread_id(void) {	// fast 一行汇编替换一个系统调用
	void* thp = __builtin_thread_pointer();	// gcc version > 5
	return (uintptr_t)thp;
}

static inline heap_t*
get_thread_heap(void) {	// 获取线程私有的heap
	return global_thread_heap;
}

// 绑定到当前线程
static void
set_thread_heap(heap_t* heap) {
	global_thread_heap = heap;	// 将线程堆 设置为 heap
	if(heap && (heap->id != 0)) {	// 已经初始化了
		rpmalloc_assert(heap->id != 0, "Default heap being used");
		heap->owner_thread = get_thread_id();	// 绑定线程ID (因为可能直接使用死去的线程的heap, 所以需要改)
	}
	pthread_setspecific(pthread_key, heap);	// 将heap 绑定到线程_key
}

static heap_t*
heap_allocate(void);

// 获取一个heap 并绑定到当前线程
static heap_t*
get_thread_heap_allocate(void) {
	heap_t* heap = heap_allocate();	// 获取一个 heap
	set_thread_heap(heap);		// 绑定到当前线程
	return heap;
}

static inline uint32_t
get_size_class_tiny(size_t size) { // 除以16 向上对齐 16的倍数
	return (((uint32_t)size + (SMALL_GRANULARITY - 1)) / SMALL_GRANULARITY);
}

// 数学推论请看图解
static inline uint32_t
get_size_class(size_t size) {	// 获取size_class 的一般函数
	uint64_t minblock_count = (size + (SMALL_GRANULARITY - 1)) / SMALL_GRANULARITY;	// 多少个16 组成的
	rpmalloc_assert(minblock_count > 64, "Size class misconfiguration");	// 判断是否溢出
	// 64位下 获取最高位1(0 ~ 63): 63 - 最高位1左边有多少个0(clz) 
	const uint32_t most_significant_bit = (uint32_t)(63 - (int)rpmalloc_clz(minblock_count));
	// 获取最高3位的低2位
	const uint32_t subclass_bits = (minblock_count >> (most_significant_bit - 2)) & 0x03;
	// global_size_class 里的常数是这样的规律	为了防用户 40 -> 41
	const uint32_t class_idx = (uint32_t)((most_significant_bit << 2) + subclass_bits) + 41;
	return class_idx;
}

// 获取 page_type
static inline page_type_t
get_page_type(uint32_t size_class) {
	if (size_class < SMALL_SIZE_CLASS_COUNT) // 73 以下
		return PAGE_SMALL;
	else if (size_class < (SMALL_SIZE_CLASS_COUNT + MEDIUM_SMALL_SIZE_CLASS_COUNT))
		return PAGE_MEDIUM_SMALL; // 73 + 12
	else if (size_class < (SMALL_SIZE_CLASS_COUNT + MEDIUM_SMALL_SIZE_CLASS_COUNT + MEDIUM_LARGE_SIZE_CLASS_COUNT))
		return PAGE_MEDIUM_LARGE; // 73 + 24
	else if (size_class < SIZE_CLASS_COUNT)
		return PAGE_LARGE; // 73 + 36
	return PAGE_HUGE; // 巨大
}

// 向上对齐 page_size
static inline size_t
get_page_aligned_size(size_t size) {
	const size_t page_mask = global_config.page_size - 1;	// page_size 必须是2^n n为正整数
	return (size + page_mask) & ~page_mask;	// 下面但凡有就顶个1上去，之后抛弃下面的
}

////////////
/// OS 工具函数
/////

static void
os_set_page_name(void* address, size_t size) {
	const char* name = global_config.enable_huge_pages ? global_config.huge_page_name : global_config.page_name;
	if((address == MAP_FAILED || !name))	// mmap 失败了，或者没有名字 直接返回
		return;
	prctl(PR_SET_VMA, PR_SET_VMA_ANON_NAME, (uintptr_t)address, size, (uintptr_t)name);
}

// 包装mmap 映射函数
// 获取对齐了 alignment 大小的地址
static void*
os_mmap(size_t size, size_t alignment, size_t* offset, size_t* mapped_size) {
	size_t map_size = size + alignment;	// 最终要映射的大小

	// 私有映射 匿名映射
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
	// 操作系统自己决定地址	读写 且只能读写	获取大页就有 MAP_HUGETLB , flags 是一定要的, 非文件映射 -1
	void*ptr = mmap(0, map_size, PROT_READ | PROT_WRITE,
			(global_config.enable_huge_pages ? MAP_HUGETLB : 0) | flags, -1, 0);
	
#if defined (MADV_HUGEPAGE)
	// 如果直接大页失败了, 下面获取正常的页，并建议它合成大页
	if((ptr == MAP_FAILED || !ptr) && global_config.enable_huge_pages) {
		ptr == mmap(0, map_size, PROT_READ | PROT_WRITE, flags, -1 ,0);	// flags 没加 MAP_HUGETLB 了
		if(ptr && ptr != MAP_FAILED) {	// 如果成功了
			int prm = madvise(ptr, size, MADV_HUGEPAGE);
			(void)prm;
			rpmalloc_assert((prm == 0), "Failed to promote the page to transparent huge page");
		}
	}	
#endif

	os_set_page_name(ptr, map_size);
	if(ptr == MAP_FAILED)
		return 0;	// 失败了直接返回NULL

	// 这里 mmap 必成功
	// 让mmap 的地址对齐 alignment
	if(alignment) {
		size_t padding = ((uintptr_t)ptr & (uintptr_t)(alignment - 1));
		if(padding)
			padding = alignment - padding;	// 需要加 padding 才能对齐 alignment
		rpmalloc_assert(padding <= alignment, "Internal failure in padding");
		rpmalloc_assert(!(padding % 8), "Internal failure in padding");
		ptr = pointer_offset(ptr, padding);	// ptr += padding
		*offset = padding;	// 原地址 = ptr - padding(偏移)
	}
	*mapped_size = map_size;
	return ptr;
}

// 建议操作系统将内存中的一些给他放下内存
static int
os_mdecommit(void* address, size_t size) {
	if(global_config.disable_decommit)
		return 1;
	// 建议操作系统回收掉 address开始 size 大小地方的内存
	if (madvise(address, size, MADV_DONTNEED)) {
		rpmalloc_assert(0, "Failed to decommit virtual memory block");
		return 1;
	}
	return 0;
}

static void
os_munmap(void* address, size_t offset, size_t mapped_size) {
	address = pointer_offset(address, -(int32_t)offset);	// 获得原映射地址
	if (munmap(address, mapped_size))			// 解除映射
		rpmalloc_assert(0, "Failed to unmap virtual memory block");
}

////////////
/// Block 接口
//////

// 获取span 中 block 对应的 page
static inline page_t*
span_get_page_from_block(span_t* span, void* block) {
	// page_address_mask 根据不同 page规格而不同
	return (page_t*)((uintptr_t)block & span->page_address_mask);
}

static inline span_t* page_get_span(page_t* page);
static inline int page_is_thread_heap(page_t* page);

// 获取对应 page_type 的大小
static inline size_t
page_get_size(page_t* page) {
	if(page->page_type == PAGE_SMALL)
		return SMALL_PAGE_SIZE;
	else if(page->page_type == PAGE_MEDIUM_SMALL)
		return MEDIUM_SMALL_PAGE_SIZE;
	else if(page->page_type == PAGE_MEDIUM_LARGE)
		return MEDIUM_LARGE_PAGE_SIZE;
	else if(page->page_type == PAGE_LARGE)
		return LARGE_PAGE_SIZE;
	else	// HUGE
		return page_get_span(page)->page_size;
}

// decommit page
static inline void
page_decommit_memory_pages(page_t* page) {
	void* extra_page = pointer_offset(page, global_config.page_size);
	size_t extra_page_size = page_get_size(page) - global_config.page_size;
	if (os_mdecommit(extra_page, extra_page_size) != 0)	// 保留一系统页大小，保存page 或 span 的信息
		return;
	page->is_decommitted = 1;
}

static void
heap_page_free_decommit(heap_t* heap, uint32_t page_type, uint32_t page_retain_count) {
	page_t* page = heap->page_free[page_type];	// 获取对应 page_type 的free链表
	while(page && page_retain_count) {		// 跳过前 page_retain_count 个元素 (局部性)
		page = page->next;
		--page_retain_count;
	}
	while(page && (page->is_decommitted == 0)) {		// 将后续的page 都 decommit
		page_decommit_memory_pages(page);		// decommit 此page
		--heap->page_free_commit_count[page_type];	// --commit_count
		page = page->next; 				// 跳到链表的下一个元素
	}
}

// 将完全空闲的page 加入 heap->page_free 链表
static void
page_available_to_free(page_t* page) {
	rpmalloc_assert(page->is_full == 0, "Page full flag internal failure");			// 没满
	rpmalloc_assert(page->is_decommitted == 0, "Page decommitted flag internal failure");	// 没被 decommit
	heap_t* heap = page->heap;	// 获取对应的 heap
	if(heap->page_available[page->size_class] == page) {		// 若heap 正在使用这个 page
		heap->page_available[page->size_class] = page->next;	// 将这个拿出链表，用其他的 page
	}
	else {	// 若heap 不是正在使用这个 page，也就不是 head
		page->prev->next = page->next;		// 将其拿出这个链表
		if (page->next)				// page->next 存在就把它更新一下
			page->next->prev = page->prev;
	}
	page->is_free = 1;	// 完全空闲
	page->is_zero = 0;	// 未置零
	page->next = heap->page_free[page->page_type];	// 加入 heap->page_free 链表，获取原 head
	heap->page_free[page->page_type] = page;	// 头插 现head = page

	// ++count 如果对应规格的正在内存上的page数量超过了一定的数量，decommit
	if(++heap->page_free_commit_count[page->page_type] >= global_page_free_overflow[page->page_type])
		heap_page_free_decommit(heap, page->page_type, global_page_free_retain[page->page_type]);
}

// 复活
static void
page_full_to_available(page_t* page) {
	rpmalloc_assert(page->is_full == 1, "Page full flag internal failure");
	rpmalloc_assert(page->is_decommitted == 0, "Page decommitted flag internal failure");	// full
	heap_t* heap = page->heap;
	// 头插 插入 heap->page_available 链表中
	page->next = heap->page_available[page->size_class];
	if(page->next)
		page->next->prev = page;
	heap->page_available[page->size_class] = page;
	// 设置状态
	page->is_full = 0;
	page->generic_free = 0;	
}

// 自产自销free 并且复活此page
static inline void
page_put_local_free_block(page_t* page, block_t* block) {
	block->next = page->local_free;		// 头插 插入page->local_free 链表
	page->local_free = block;
	++page->local_free_count;		// free_count++
	--page->block_used;			// used--
	if(EXPECTED(page->is_full != 0)) {	// 防御性 之前是满的
		page_full_to_available(page);	// free 之后 复活
	}
}

// 根据index 获取 page中的 block
static inline block_t* page_block(page_t* page, uint32_t block_index);

// 返回高32为的 list_count 并将block 设置为当前链表中的首个元素
static inline uint32_t page_block_from_thread_free_list(page_t* page, uint64_t token, block_t** block);

// 跳过存放数据的头部 128 B
static inline block_t*
page_block_start(page_t* page) {
	return pointer_offset(page, PAGE_HEADER_SIZE);
}

// 获取block 在 page 中的 index : 第几块
static inline uint32_t
page_block_index(page_t* page, block_t* block) {
	block_t* block_first = page_block_start(page);
	return (uint32_t)pointer_diff(block, block_first) / page->block_size;	// block 距离起始地点的距离 / size
}

// 将list_count 和 block_index 打包成 uint64_t
static inline uint64_t
page_block_to_thread_free_list(page_t* page, uint32_t block_index, uint32_t list_count) {
	(void)sizeof(page);
	return ((uint64_t)list_count << 32ULL) | (uint64_t)block_index;
}

static NOINLINE void
page_put_thread_free_block(page_t* page, block_t* block) {
	// 如果不加这行 而在下面2个分支的 load 加上 acquire 的话
	// 流水线预测失败可能 处理 2次 失效队列
	atomic_thread_fence(memory_order_acquire);	// 处理失效队列	
	if(page->is_full) {	// 是满的 且非自产自销
		heap_t* heap = page->heap;
		// 获得 heap->thread_free 快递箱链表	relaxed 无所谓，反正下面CAS失败会更新
		uintptr_t prev_head = atomic_load_explicit(&heap->thread_free[page->page_type], memory_order_relaxed);
		block->next = (void*)prev_head;	// block->next = head 头插

		// 将此 block 插入 heap 的快递箱单向链表
		while (!atomic_compare_exchange_weak_explicit(&heap->thread_free[page->page_type], &prev_head,
			(uintptr_t)block, memory_order_release, memory_order_relaxed)) {
			block->next = (void*)prev_head;	// 失败更新
			wait_spin();			// 失败自旋
		}
	}
	else {	// 不是满的 且非自产自销
		// 获取 page->thread_free ,低32位存了 index 高32位存了 list_count
		unsigned long long prev_thread_free = atomic_load_explicit(&page->thread_free, memory_order_relaxed);
		// 获取index
		uint32_t block_index = page_block_index(page, block);
		// page + HEAD + size * index == block 是正常的
		rpmalloc_assert(page_block(page, block_index) == block, "Block pointer is not aligned to start of block");
		// 将 block->next 设置为当前 page->thread_free 链表的 head 并获取 list_count
		uint32_t list_size = page_block_from_thread_free_list(page, prev_thread_free, &block->next) + 1;
		// 打包
		uint64_t thread_free = page_block_to_thread_free_list(page, block_index, list_size);
		// 将其头插进入page的快递箱链表
		while (!atomic_compare_exchange_weak_explicit(&page->thread_free, &prev_thread_free, thread_free,
					memory_order_release, memory_order_relaxed)) {
			// 失败了则更新 block->next list_size 并重新打包
			list_size = page_block_from_thread_free_list(page, prev_thread_free, &block->next) + 1;
			thread_free = page_block_to_thread_free_list(page, block_index, list_size);
			wait_spin();
		}
	}
}

static NOINLINE void
span_deallocate_block(span_t* span, page_t* page, void* block) {
	// 如果是大页
	if(UNEXPECTED(page->page_type == PAGE_HUGE)) {
		os_munmap(span, span->offset, span->mapped_size);	// push不进去 直接 unmap解除映射
		return;
	}

	// 满的 或者非自产自销
	int is_thread_local = page_is_thread_heap(page);
	if(EXPECTED(is_thread_local != 0)) { // 满的 是自产自销
		page_put_local_free_block(page, block);		// 复活
	}
	else {	// 非自产自销
		page_put_thread_free_block(page, block);	// 放入对应快递箱里去(heap or page)
	}
}

static inline void
block_deallocate(block_t* block) {
	span_t* span = (span_t*)((uintptr_t)block & SPAN_MASK);	// 通过与block的地址位运算 获取 span
	page_t* page = span_get_page_from_block(span, block);	// 获取 block 对应的 page
	const int is_thread_local = page_is_thread_heap(page);	// 是否是当前线程 自产自销

	if(EXPECTED(is_thread_local != 0)) {		// 自产自销
		if(EXPECTED(page->generic_free == 0)) {	// 非huge 非满 且没有对齐块	fast
			block->next = page->local_free;	// 插入 page->local_free 链表
			page->local_free = block;	// 头插
			++page->local_free_count;	// free_count++		used--
			if(UNEXPECTED(--page->block_used == 0))	// 如果上面释放后 page 为空了
				page_available_to_free(page);	// page 加入 heap->page_free链表 
		}
		else {	// huge or full
			span_deallocate_block(span, page, block);
		}
	}
	else {	// 非自产自销
		span_deallocate_block(span, page, block);	// 分流，慢速通道
	}
}

////////////
/// 初始化和析构 销毁函数
/////

static inline void
heap_release(heap_t* heap) {
	heap_lock_acquire();
	// 将 heap 从 global_heap_used 链表中拿出来
	if (heap->prev)
		heap->prev->next = heap->next;
	if (heap->next)
		heap->next->prev = heap->prev;
	// 如果 heap 是 head 更新 head
	if (global_heap_used == heap)
		global_heap_used = heap->next;
	// 头插 插入 queue 这个链表
	heap->next = global_heap_queue;
	global_heap_queue = heap;
	heap_lock_release();
}

static inline void
rpmalloc_thread_finalize(void) {
	heap_t* heap = get_thread_heap();
	heap_release(heap);			// 将heap 从 used 链表 拿到 queue 链表里去
	//set_thread_heap(global_heap_default);	// 官方是extern的 thread_destructor 所以需要防一手
}

// 析构/销毁函数
static inline void
rpmalloc_thread_destructor(void* value) {		// 操作系统传过来的 heap_t* 参数
	if(get_thread_id() == global_main_thread_id)	// 如果是进行了进程初始化的 什么都不动
		return;					// 此时 进程将死亡，操作系统回收整个进程的资源
	if(value)
		rpmalloc_thread_finalize();
}

// 线程级初始化
static inline void
rpmalloc_thread_initialize(void) {
	if(get_thread_heap() == global_heap_default)
		get_thread_heap_allocate();
}

// 进程级初始化
extern void
rpmalloc_initialize(void) {	// 必须首先在 main函数里调用
	if (global_rpmalloc_initialized) {
		rpmalloc_thread_initialize();
		return;
	}

	global_rpmalloc_initialized = 1;

	os_map_granularity = (size_t)sysconf(_SC_PAGESIZE);	// 操作系统映射粒度:4096
	os_page_size = os_map_granularity;

#if ENABLE_HUGE_PAGES	// 希望开启大页
	global_config.enable_huge_pages = 1;
#endif
#if DISABLE_THP		// 关闭透明大页
	global_config.disable_thp = 1;
#endif

	if(global_config.enable_huge_pages) {	// 如果开启大页功能
		size_t huge_page_size = 0;
		int file = open("/proc/meminfo", O_RDONLY);	// 去此内核文件读取大页的大小
		if(file >= 0) {
			char buf[1024];	// 一般足够用了，不够用改为4096 并且while 循环
			ssize_t byte_read = read(file, buf, sizeof(buf) - 1);
			if(byte_read > 0) {
				buf[byte_read] = '\0';	// 补充一个‘\0’, 如果上面数组够用则这行没用
				
				char* pos = strstr(buf, "Hugepagesize:");
				if(pos)	// 跳过前13个 也就是 Hugepagesize: 获取大页大小 单位是KB
					huge_page_size = (size_t)strtol(pos + 13, NULL, 10) * 1024;
			}
			close(file);
		}
		if(huge_page_size) {	// 如果获得了大页大小
			os_page_size = huge_page_size;		// 更新
			os_map_granularity = huge_page_size;
		}
		else {
			global_config.enable_huge_pages = 0;			// 关闭透明大页
		}
	}

	global_config.page_size = os_page_size;
	if(global_config.enable_huge_pages || global_config.page_size > (256 * 1024))	// 一页256KB 或者开启大页
		global_config.disable_decommit = 1;	// 如果数据太大了，禁止decommit
	if(global_config.disable_thp)
		(void)prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);	// 拒绝透明大页

	global_config.huge_page_name = "rpmalloc_HUGE";
	global_config.page_name = "rpmalloc";

	pthread_key_create(&pthread_key, rpmalloc_thread_destructor);	// 获得全局钥匙，并绑定一个析构函数
	global_main_thread_id = get_thread_id();	// 设置进程级初始化的线程ID

	rpmalloc_thread_initialize();
}
//////////////
///////
/// 获取 xxx_t
///////
//////////

////////////    
/// Page 接口
/////   

// 根据page 获得它所在的span
static inline span_t*
page_get_span(page_t* page) {
	return (span_t*)((uintptr_t)page & SPAN_MASK);  
}               

// page所在的heap是不是 自己的线程持有的
static inline int
page_is_thread_heap(page_t* page) {
	return (page->heap->owner_thread == get_thread_id());
}

// 获取page->local_free链表的 head 并更新数据
static block_t*
page_get_local_free_block(page_t* page) {
	block_t* block = page->local_free;	// 获取 page->local_free链表的 head
	page->local_free = block->next;		// 将head pop 出来, 更新新 head
	--page->local_free_count;		// count--
	++page->block_used;			// used++	这里的必定会给heap
	return block;
}

// page + HEADER_SIZE + (index * block_size)
static inline block_t*
page_block(page_t* page, uint32_t block_index) {
	return pointer_offset(page, PAGE_HEADER_SIZE + (page->block_size * block_index));
}

// 如果list中存在元素, block 更新为这个链表中的首个元素 并且返回 list_count
static inline uint32_t  
page_block_from_thread_free_list(page_t* page, uint64_t token, block_t** block) {
	uint32_t block_index = (uint32_t)(token & 0xFFFFFFFFULL);		// 获取低32位 表示首个元素index
	uint32_t list_count = (uint32_t)((token >> 32ULL) & 0xFFFFFFFFULL);	// 获取高32位 表示list_count
	*block = list_count ? page_block(page, block_index) : 0;		// 如果有则获得这个 block
	return list_count;	// 返回 list_count
}

// 获取一个block, 如果block足够小 则串起一个系统页的来
// 不一定能够整除，可以存在最后一个block的数据在两个系统页中都有
// 同样，如果是最后一个页发生上述情况，因为没有下一个页了，所以会发生错误 (最后一个block已经串好了) 所以需要 count 判断
static NOINLINE void*
page_initialize_blocks(page_t* page) {
	// 不能满
	rpmalloc_assert(page->block_initialized < page->block_count, "Block initialization internal failure");
	block_t* block = page_block(page, page->block_initialized);	// 获得首个block地址
	++page->block_initialized;	// 被用了多少块了 ++
	++page->block_used;		// used_count ++ 这个块必定会被拿走的
	
	// 如果 page 的规格是最小的 且 block_size < 系统页大小的一半
	if ((page->page_type == PAGE_SMALL) && (page->block_size < (global_config.page_size >> 1))) {
		// start = block & ~(page_size - 1) : block 所在的系统页的地址
		void* memory_page_start = (void*)((uintptr_t)block & ~(uintptr_t)(global_config.page_size - 1));
		// next : block所在系统页的下一个页
		void* memory_page_next = pointer_offset(memory_page_start, global_config.page_size);
		block_t* free_block = pointer_offset(block, page->block_size);	// block 的下一个块
		block_t* first_block = free_block;	// 串的起始block
		block_t* last_block = free_block;	// 串的最后block
		uint32_t list_count = 0;		// 串的个数
		uint32_t max_list_count = page->block_count - page->block_initialized;	// page还剩下的块数
		// 将这一系统页的block串起来
		while (((void*)free_block < memory_page_next) && (list_count < max_list_count)) {
			last_block = free_block;	// 更新last
			free_block->next = pointer_offset(free_block, page->block_size);	// 串
			free_block = free_block->next;	// free_block 往后走
			++list_count;			// 串的count++
		}
		if (list_count) {				// 如果有串
			last_block->next = 0;
			page->local_free = first_block;		// 将串放入page->local_free链表里去(进入此函数时, 链表空)
			page->block_initialized += list_count;	// 用了这么多块
			page->local_free_count = list_count;	// free_count += list_count 插入了这么多free_node
		}
	}
	return block;
}

// 将page->free 给 heap_free (block)
static void
page_push_local_free_to_heap(page_t* page) {
	page->heap->local_free[page->size_class] = page->local_free;	// 直接把指针丢过去
	page->block_used += page->local_free_count;			// 被拿走了的 就是 used
	page->local_free = 0;		// 链表清空
	page->local_free_count = 0;	// 个数归零
}

// 收取快递箱
static NOINLINE void
page_adopt_thread_free_block_list(page_t* page) {
	if (UNEXPECTED(page->local_free != 0))	// 防御性
		return;
	// 最低限度的获取 thread_free	反正下面 CAS会获得最新的 page->thread_free
	unsigned long long thread_free = atomic_load_explicit(&page->thread_free, memory_order_relaxed);
	if(thread_free != 0) {	// CAS 强指令 acquire 保证后面的不会到此指令前面去
		// 获取并将其置零 说明拿走了这个快递箱
		while (!atomic_compare_exchange_weak_explicit(&page->thread_free, &thread_free, 0,
			memory_order_acquire, memory_order_relaxed))
			wait_spin();	// CAS 失败自旋且会更新thread_free
		// 更新 local_free_count 和 链表
		page->local_free_count = page_block_from_thread_free_list(page, thread_free, &page->local_free);
		// local_free_count == 0 正常来说必定成功
		rpmalloc_assert(page->local_free_count <= page->block_used, "Page thread free list count internal failure");
		page->block_used -= page->local_free_count;	// 从heap 还回来了这么多 used -= 还的数量
	}
}

// 将page 从 available 中拿到 full 中
static void
page_available_to_full(page_t* page) {
	heap_t* heap = page->heap;
	// 如果正在使用这个 page
	if (EXPECTED(heap->page_available[page->size_class] == page)) {
		heap->page_available[page->size_class] = page->next;	// 将其拿出来，现在head 是 page->next
	}
	else {	// ### ？大概率防御性设计	我不认为会走这里
		printf("page_available_to_full_else\n");
		page->prev->next = page->next;
		if (page->next)
			page->next->prev = page->prev;
	}
	page->is_full = 1;	// 满
	page->is_zero = 0;	// 非零
	page->generic_free = 1;	// full 会设置这个标志
}

// 从page 里获取一 block
static inline void*
page_allocate_block(page_t* page, unsigned int zero) { // 这个函数是page有空闲才能进入的
	unsigned int is_zero = 0;
	// 如果page->local_free 链表存在元素，获取 head 元素，否则为 NULL
	block_t* block = (page->local_free != 0) ? page_get_local_free_block(page) : 0;
	if(UNEXPECTED(block == 0)) {	// 如果不存在元素
		if(atomic_load_explicit(&page->thread_free, memory_order_acquire) != 0) {	// 有快递(局部性)
			page_adopt_thread_free_block_list(page);	// 将快递放入page->local_free 里
			block = (page->local_free != 0) ? page_get_local_free_block(page) : 0;	// 然后再重复
		}
		if(block == 0) {	// 如果还未获得
			block = page_initialize_blocks(page);	// 从page中获取一个新 block
			is_zero = page->is_zero;
		}
	}

	// 用的数量小于总量	如果free获得了别的线程的block 会导致此错误
	rpmalloc_assert(page->block_used <= page->block_count, "Page block use counter out of sync");
	
	// 如果 page的 free链表存在 且 heap的 free链表不存在元素(后者防御性判断)
	if(page->local_free && !page->heap->local_free[page->size_class])
		page_push_local_free_to_heap(page);	// 将page的 free链表中的元素全部加入到 heap的 free链表里

	if(page->block_used == page->block_count) {		// used == 总数 满了
		page_adopt_thread_free_block_list(page);	// 尝试收取快递
		if(page->block_used == page->block_count) {	// 如果没收到快递 则还是用完了的状态
			// 此时还没设置 is_full, is_full == 0
			rpmalloc_assert(!page->is_full, "Page block use counter out of sync with full flag");
			page_available_to_full(page);	// 将其从使用中状态变成满的状态
		}
	}

	// 需不需要置零
	if(zero){
		if(!is_zero)
			memset(block, 0, page->block_size);
		else
			*(uintptr_t*)block = 0;	// 这里可能是保存了下一个block的地址，置0 且后面都为0
	}
	return block;
}

/////////
/// heap 接口
/////

// 让线程获得heap锁
static inline void
heap_lock_acquire(void) {
	uintptr_t lock = 0;
	uintptr_t this_lock = get_thread_id();	// 获得锁的方式 是将锁改为该线程ID
	while(!atomic_compare_exchange_strong_explicit(&global_heap_lock, &lock, this_lock,
				memory_order_acquire, memory_order_relaxed)) {
		lock = 0;	// CAS 失败会将 lock 改为最新的 也就是获得了锁的线程ID，必须重置为 0
		wait_spin();	// 自旋
	}
}

// 让线程释放heap锁
static inline void
heap_lock_release(void) {
	rpmalloc_assert((uintptr_t)atomic_load_explicit(&global_heap_lock, memory_order_relaxed) == get_thread_id(),
			"Bad heap lock");	// 锁不被我持有，assert直接死亡，所以 relaxed
	atomic_store_explicit(&global_heap_lock, 0, memory_order_release); // 释放锁, 改为0
}

// 对heap 初始化
static inline heap_t*
heap_initialize(void* block) {
	heap_t* heap = block;
	memset_const(heap, 0, sizeof(heap_t));	// 将heap_t 初始化为 0	预防极端情况：线程一瞬间出生非常多
	// LOCK汇编 获得一个递增的全局id，赋值给heap->id	此数据前后都不依赖 所wait_spin();	// 自旋 CAS
	heap->id = 1 + atomic_fetch_add_explicit(&global_heap_id, 1, memory_order_relaxed);
	return heap;
}

// 申请一个新heap
static heap_t*
heap_allocate_new(void) {
	size_t heap_size = get_page_aligned_size(sizeof(heap_t));	// 对齐一个系统页
	size_t offset = 0;
	size_t mapped_size = 0;
	// mmap 映射一个系统页给 heap 使用
	block_t* block = os_mmap(heap_size, 0, &offset, &mapped_size);	// 防御性编程, 不用heap_t
	heap_t* heap = heap_initialize((void*)block);	// 对heap 初始化
	heap->offset = (uint32_t)offset;		// 更新heap
	heap->mapped_size = mapped_size;		// 实际映射大小
	return heap;
}

// 获取一个heap
static heap_t*
heap_allocate(void) {
	heap_t* heap = 0;
	// 先看全局 heap链表有没有
	heap_lock_acquire();
	heap = global_heap_queue;			// 获得链表首元素	线程死亡会把heap加入此链表
	global_heap_queue = heap ? heap->next : 0;	// 获得了就更新 链表首元素
	heap_lock_release();

	if(!heap)
		heap = heap_allocate_new();	// 如果链表没有元素,获取一个新的heap
	if(heap) {	// 继续对heap 初始化
		uintptr_t current_thread_id = get_thread_id();	// 获取当前线程ID
		heap_lock_acquire();
		heap->next = global_heap_used;			// 加入使用链表, 头插
		heap->prev = 0;					// 头插 prev = 0
		if (global_heap_used)
			global_heap_used->prev = heap;		// 如果原来存在元素，设置之前头.prev = curr
		global_heap_used = heap;			// 更新头: curr
		heap_lock_release();
		heap->owner_thread = current_thread_id;		// 设置 线程ID
	}
	return heap;
}

/////////
/// span 接口
/////

// 从 span 中获取一个page
static inline page_t*
span_allocate_page(span_t* span) {
	rpmalloc_assert(span->page_initialized < span->page_count, "Page initialization internal failure");
	heap_t* heap = span->heap;		// 获取span的对应控制堆
	page_t* page = pointer_offset(span, span->page_size * span->page_initialized);	// 去到空闲的地方 index * size

	++span->page_initialized;		// 到下一块去	mmap 映射出来一开始都是 0
	page->page_type = span->page_type;	// page 大小类型
	page->is_zero = 1;			// mmap 拿到的就是 全0的，说明已经不需要memset 0 了
	page->heap = heap;			// page 的heap
	rpmalloc_assert(page_is_thread_heap(page), "Page owner thread mismatch");	// 判断page 是不是自己线程的

	if (span->page_initialized == span->page_count) {	// 获取page后, 如果用完了这个 span 加入used链表
		rpmalloc_assert(span == heap->span_partial[span->page_type], "Span partial tracking out of sync");
		heap->span_partial[span->page_type] = 0;	// 空间用完了，置0，下次只能再次 mmap了
		span->next = heap->span_used[span->page_type];	// 头插，这是用完了span空间的队列
		heap->span_used[span->page_type] = span;	// 头插
	}
	return page;
}

// 将系统页数据开始的地方置零
static inline int
page_commit_memory_pages(page_t* page) {
	void* first_page = pointer_offset(page, PAGE_HEADER_SIZE);		// 找到起始地方
	memset(first_page, 0, global_config.page_size - PAGE_HEADER_SIZE);	// 置零
	page->is_zero = 1;
}

// 对page继续进行初始化
static inline void
heap_make_free_page_available(heap_t* heap, uint32_t size_class, page_t* page) {
	page->size_class = size_class;	// 109 个规格中的哪一个
	page->block_size = global_size_class[size_class].block_size;	// page 存放的 block大小
	page->block_count = global_size_class[size_class].block_count;	// page 能存放多少个 block
	page->block_used = 0;		// 被heap 拿走的数量
	page->block_initialized = 0;	// 被分割了的数量
	page->local_free = 0;		// 本地空闲 block 链表
	page->local_free_count = 0;	// 空闲个数
	page->is_full = 0;		// 满
	page->is_free = 0;		// 空
	page->has_aligned_block = 0;	// 没有对齐块
	page->generic_free = 0;		// 一个标记 为1时 free 走慢速通道
	page->heap = heap;		// 绑定到 heap
	page_t* head = heap->page_available[size_class];	// 插入heap的page正在使用的链表
	page->next = head;		// 头插
	page->prev = 0;
	// 把快递箱置空 release 应该只是为了刷出缓冲区
	atomic_store_explicit(&page->thread_free, 0, memory_order_release);
	if (head)
		head->prev = page;	// 如果原head 存在 设置 prev
	heap->page_available[size_class] = page;		// 更新头部
	if(page->is_decommitted != 0)	// 若获得的这个是被 decommit 了的(decommit 会保留前1个系统页)
		page_commit_memory_pages(page);	// 将这个系统页的数据开始的地方全置零
}

static inline span_t*
heap_get_span(heap_t* heap, page_type_t page_type) {
	// 首先去 heap 正在使用的 span 里获取, 如果有则快速获得
	if (EXPECTED(heap->span_partial[page_type] != 0))
		return heap->span_partial[page_type];

	// 没获得，新 mmap 一块span 并对其初始化
	size_t offset = 0;
	size_t mapped_size = 0;
	span_t* span = os_mmap(SPAN_SIZE, SPAN_SIZE, &offset, &mapped_size);	// 映射对齐SPAN_SIZE
	if(EXPECTED(span != 0)) {
		uint32_t page_count = 0;		// span 里能存放的 page的个数
		uint32_t page_size = 0;			// page 大小
		uintptr_t page_address_mask = 0;	// 对应page规格的 掩码
		if (page_type == PAGE_SMALL) {	// 小 页
			page_count = SPAN_SIZE / SMALL_PAGE_SIZE;	// 2^12
			page_size = SMALL_PAGE_SIZE;			// 2^16
			page_address_mask = SMALL_PAGE_MASK;
		} else if (page_type == PAGE_MEDIUM_SMALL) { // 中小 页
			page_count = SPAN_SIZE / MEDIUM_SMALL_PAGE_SIZE;// 2^8
			page_size = MEDIUM_SMALL_PAGE_SIZE;		// 2^20
			page_address_mask = MEDIUM_SMALL_PAGE_MASK;
		} else if (page_type == PAGE_MEDIUM_LARGE) { // 中 页
			page_count = SPAN_SIZE / MEDIUM_LARGE_PAGE_SIZE;// 2^6
			page_size = MEDIUM_LARGE_PAGE_SIZE;		// 2^22
			page_address_mask = MEDIUM_LARGE_PAGE_MASK;
		} else { // 其他 large 吧, 不是huge，huge有另外的
			page_count = SPAN_SIZE / LARGE_PAGE_SIZE;	// 2^4 个
			page_size = LARGE_PAGE_SIZE;			// 2^24
			page_address_mask = LARGE_PAGE_MASK;
		}
		span->heap = heap;				// span 所属的heap
		span->page_type = page_type;			// span 的所属类型 之后就存这个类型的 page了
		span->page_count = page_count;			// 有多少个这个类型的 page
		span->page_size = page_size;			// 所属类型page的大小
		span->page_address_mask = page_address_mask;	// 掩码
		span->offset = (uint32_t)offset;		// offset: 现在的地址 - offset = 映射的起始地址
		span->mapped_size = mapped_size;		// 最终映射的大小
		heap->span_partial[page_type] = span;		// span 放入heap中
	}
	return span;
}

static page_t*
heap_get_page(heap_t* heap, uint32_t size_class);

static page_t*
heap_get_page_generic(heap_t* heap, uint32_t size_class) {
	page_type_t page_type = get_page_type(size_class);	// 获取size_class 对应page 类型
	uintptr_t block_mt = atomic_load_explicit(&heap->thread_free[page_type], memory_order_relaxed);	// 获取快递箱 
	if (UNEXPECTED(block_mt != 0)) {
		while (!atomic_compare_exchange_weak_explicit(&heap->thread_free[page_type], &block_mt, 0,
			memory_order_acquire,memory_order_relaxed))
		{			// 获取这串，并改为0
			wait_spin();	// CAS失败 自旋	block_mt更新为最新值
		}
		block_t* block = (void*)block_mt;
		while (block) {	// 如果快递箱有东西，全部拿到手之后 再次free(自产自销)
			block_t* next_block = block->next;
			block_deallocate(block);	// free
			block = next_block;
		}
		return heap_get_page(heap, size_class);	// retry
	}

	// 查看是否有空闲的页
	page_t* page = heap->page_free[page_type];
	if(EXPECTED(page != 0)) {
		heap->page_free[page_type] = page->next;
		if(page->is_decommitted == 0) {		// 从free链表拿到了page --free_count
			rpmalloc_assert(heap->page_free_commit_count[page_type] > 0, "Free committed page count out of sync");
			--heap->page_free_commit_count[page_type];	// 计数减1
		}
		heap_make_free_page_available(heap, size_class, page);	// 对该 page 初始化
		return page;
	}
	// 没拿到 count == 0
	rpmalloc_assert(heap->page_free_commit_count[page_type] == 0, "Free committed page count out of sync");

	if(heap->id == 0) {	// 如果线程已经初始化过了，heap->id != 0
		rpmalloc_initialize();	// 初始化
		return heap_get_page(get_thread_heap(), size_class);	// retry
	}

	// 啥也没有，获取一个span
	span_t* span = heap_get_span(heap, page_type);
	if(EXPECTED(span != 0)) {
		page = span_allocate_page(span);	// 从 span中获取一个page
		heap_make_free_page_available(page->heap, size_class ,page);	// 对该 page 初始化
	}
	return page;
}

// 获取 page
static page_t*
heap_get_page(heap_t* heap, uint32_t size_class) {
	page_t* page = heap->page_available[size_class];	// 去桶的对应大小的链表里去拿page
	if(EXPECTED(page != 0))
		return page;	// 拿到了则返回
	return heap_get_page_generic(heap, size_class);		// 没拿到则申请
}

// fast
static inline void*
heap_pop_local_free(heap_t* heap, uint32_t size_class) {
	block_t** free_list = heap->local_free + size_class;	// 获取桶中对应链表的地址
	block_t* block = *free_list;				// 获取链表首个元素
	if(EXPECTED(block != 0))
		*free_list = block->next;			// 若链表不为空, 讲链表首个元素更新
	return block;
}

static NOINLINE void*	// 获取一个64规格以内的 block
heap_allocate_block_small_to_large(heap_t* heap, uint32_t size_class, unsigned int zero) {
	page_t* page = heap_get_page(heap, size_class);	// 获取 page
	if(EXPECTED(page != 0))
		return page_allocate_block(page, zero);	// page 切割一个 block
	return 0;
}

// huge
static NOINLINE void*
heap_allocate_block_huge(heap_t* heap, size_t size, unsigned int zero) {
	// 若未初始化
	if(heap->id == 0) {
		rpmalloc_initialize();		// 进行初始化
		heap = get_thread_heap();	// 更新 heap
	}
	size_t alloc_size = get_page_aligned_size(size + SPAN_HEADER_SIZE);	// 必要的:SPAN头 + 用户数据
	size_t offset = 0;	// mmap 映射地址 + offset = 现在的地址
	size_t mapped_size = 0;	// 对齐大小
	void* block = 0;
	
	// 直接映射，并给一系列参数初始化 (其中一些没用)
	block = os_mmap(alloc_size, SPAN_SIZE, &offset, &mapped_size);
	if(EXPECTED(block != 0)) {		// 获得了 
		span_t* span = block;		// 将 映射来的空间作为 span_t 使用，存放数据
		span->heap = heap;		// 更新 heap
		span->page_type = PAGE_HUGE;	// 巨大
		span->page_address_mask = LARGE_PAGE_MASK;	// 随便给一个
		span->page_size = (uint32_t)global_config.page_size;			// 用这个存系统页大小
		span->page_count = (uint32_t)(alloc_size / global_config.page_size);	// 空间相当于多少个系统页
		span->offset = (uint32_t)offset;	// 更新 offset
		span->mapped_size = mapped_size;	// 更新 实际映射大小
		span->page.heap = heap;			// span->page 也更新 heap
		span->page.is_full = 1;
		span->page.generic_free = 1;		// HUGE
		span->page.page_type = PAGE_HUGE;	// page_type: HUGE
		void* ptr = pointer_offset(block, SPAN_HEADER_SIZE);	// 用户数据起始地址: block + span_t

		if(zero)
			memset(ptr, 0, size);	// 需要置零 置零
		return ptr;
	}
	return 0;	// 没获得 return NULL;
}

// 分流函数 慢速通道	处理 > 1KB 的所有情况
static NOINLINE void*
heap_allocate_block_generic(heap_t* heap, size_t size, unsigned int zero) {
	uint32_t size_class = get_size_class(size);	// 64 -> 80, 所以必须用另个函数 不能 tiny
	if(EXPECTED(size_class < SIZE_CLASS_COUNT)) {
		block_t* block = heap_pop_local_free(heap, size_class);
		if(EXPECTED(block != 0)) {
			if(zero)
				memset(block, 0, global_size_class[size_class].block_size);
			return block;
		}
		return heap_allocate_block_small_to_large(heap, size_class, zero);	// small_to_large
	}
	return heap_allocate_block_huge(heap, size, zero);	// HUGE
}

static inline void*
heap_allocate_block(heap_t* heap, size_t size, unsigned int zero) {
	if(size <= (SMALL_GRANULARITY * 64)) {	// 如果是前64个规格
		uint32_t size_class = get_size_class_tiny(size);	// 获得实际malloc 的大小的规格
		block_t* block = heap_pop_local_free(heap, size_class);	// 先从heap的空闲链表直接获取
		if(EXPECTED(block != 0)) {
			if(zero)	// zero判断需不需要置0
				memset(block, 0, global_size_class[size_class].block_size);
			return block;
		}
		return heap_allocate_block_small_to_large(heap, size_class, zero);	// 分配新块
	}
	return heap_allocate_block_generic(heap, size, zero);	// 大于64规格的 分流
}

////////////
/// malloc 入口
///////
extern inline void*
rpmalloc(size_t size) {
	heap_t* heap = get_thread_heap();		// 获取线程的heap
	return heap_allocate_block(heap, size, 0);
}

extern inline void
rpfree(void* ptr) {
	if(UNEXPECTED(ptr == 0))
		return;	// 传NULL 直接返回
	block_deallocate(ptr);
}





/*
int main(void){
	rpmalloc_initialize();	// 首先初始化，避免多次进程初始化出现
	printf("sizeof(heap_t) = %d\n", sizeof(heap_t));
	printf("sizeof(page_t) = %d\n", sizeof(page_t));
	printf("sizeof(span_t) = %d\n", sizeof(span_t));

	int* p = (int*)rpmalloc(32);
	*p = 4396;
	printf("%uld : %d\n", p, *p);
	return 0;
} */

