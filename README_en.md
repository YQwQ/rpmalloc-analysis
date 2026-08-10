# 🚀 Rpmalloc Source Code Analysis & Core Mechanism Reconstruction (Streamlined Educational Edition)

> **Minimalist, Hardcore, Zero Performance Loss.**
> This project extracts the core architecture from the original `rpmalloc` codebase, preserving its high performance while providing a clean, clear reference for studying multi-threaded concurrent memory allocators.

---

## ⚡ Core Features (Why this repo?)

Many developers are intimidated by the thousands of lines in `rpmalloc`'s original codebase. This project lowers the bar for reading source code:

* **Core Code Extraction**: Stripped out non-essential engineering bloat, keeping only 3 primary public APIs: **`rpmalloc`**, **`rpfree`**, and **initialization**. Code size reduced from 3,000+ lines to ~1,250 lines.
* **Pure Mechanism Preserved**: Fully retains the core memory pool structure, Span/Page/Block physical topology, per-thread isolation, and spinless lock-free mailbox (lockless queue) interaction.
* **Lightweight Simplification**: Removed the `Huge` allocation caching mechanism to prevent learners from getting lost in edge-case details. Target environment: Ubuntu 24.04.4 LTS + GCC 13.3.0 (x86-64 hardware level, non-essential cross-platform abstraction removed).
* **Zero Performance Loss**: The underlying physical memory pool layout, bitwise alignment algorithms, and lock-free CAS paths are 100% faithful to the high-performance original. Multi-threaded benchmark results show **virtually identical throughput compared to the original version (excluding Huge allocations)**.

---

## 📂 Repository Layout

* 📁 **1. 代码 (Code)**: Clean, refactored core source code with deep physical-level Chinese annotations.
* 📁 **2. 图解 (Diagrams)**:
  * `01.图解_基础篇.png`: Multi-thread isolation, Heap/Span/Page physical hierarchy, and basic design philosophy.
  * `02.图解_进阶篇.png`: Span/Page allocation mechanisms, thread termination cleanup, multi-core concurrency theory, and atomic primitives explained.
* 📁 **3. 学习过程 (Study Notes)**: Development notes, derivation drafts, and bitwise logic breakdowns.
* 📊 **简单测试_性能对比.png**: Multi-threaded pressure benchmark comparing throughput between the refactored edition, original `rpmalloc`, and default allocators.

---

## 🛠️ Quick Start (Building & Testing)

Test source files are provided in the `1.代码/` directory. You can compile and benchmark using **GCC** to verify the high-concurrency throughput.

### 1. Compile and run multi-threaded & thread-termination test (`main_5.c`)
```bash
gcc my_rpmalloc.c main_5.c -o test_main_5_mine -lpthread -O3
gcc rpmalloc.c main_5.c -o test_main_5_orig -lpthread -O3

2. Compile and run high-concurrency cross-thread free test (main_6.c)
gcc my_rpmalloc.c main_6.c -o test_main_6_mine -lpthread -O3
gcc rpmalloc.c main_6.c -o test_main_6_orig -lpthread -O3

(Note: Line 13 in main_5.c toggles between Refactored/Original versions; Line 38 toggles option 5 [without Huge] and 6 [with Huge])
⚖️ License & Terms of Use

This project uses a Dual-License approach (see the LICENSE file in the root directory):

    Source Code: Strictly follows the original author's MIT License.

    Diagrams & Documentation: Protected under CC BY-NC-SA 4.0 (Attribution-NonCommercial-ShareAlike 4.0 International).