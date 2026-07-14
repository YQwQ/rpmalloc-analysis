#pragma once
#define _GNU_SOURCE

#include<stddef.h>

#define RPMALLOC_CACHE_LINE_SIZE 64
#define RPMALLOC_EXPORT __attribute__((visibility("default"))) // 库内共用，库外隐藏
#define RPMALLOC_CACHE_ALIGNED __attribute__((aligned(RPMALLOC_CACHE_LINE_SIZE))) // 对齐

#define RPMALLOC_MAX_ALIGNMENT (256 * 1024)

#ifndef ENABLE_HUGE_PAGES
#define ENABLE_HUGE_PAGES 0	// 默认关闭大页功能
#endif
 
#ifndef DISABLE_THP
#define DISABLE_THP 0		// 默认开启透明大页
#endif


RPMALLOC_EXPORT void
rpfree(void* ptr);

RPMALLOC_EXPORT void*
rpmalloc(size_t size);

RPMALLOC_EXPORT void
rpmalloc_initialize(void);
