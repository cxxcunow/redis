/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <errno.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "config.h"

#if (ULONG_MAX == 4294967295UL)
#define MEMTEST_32BIT
#elif (ULONG_MAX == 18446744073709551615ULL)
#define MEMTEST_64BIT
#else
#error "ULONG_MAX value not supported."
#endif

#ifdef MEMTEST_32BIT
#define ULONG_ONEZERO 0xaaaaaaaaUL
#define ULONG_ZEROONE 0x55555555UL
#else
#define ULONG_ONEZERO 0xaaaaaaaaaaaaaaaaUL
#define ULONG_ZEROONE 0x5555555555555555UL
#endif

static struct winsize ws;
size_t progress_printed; /* Printed chars in screen-wide progress bar. */
size_t progress_full; /* How many chars to write to fill the progress bar. */

// 内存检测加载开始，输出开始的一些图线显示
void memtest_progress_start(char *title, int pass) {
    int j;

    /*这里其实包含2个命令，"\xlb[H","xlb[2j",后面的命令是主要的操作
     *"\x1b" 是ESC的16进制ASCII码值，这里也可经表示成八进制的\033,
     *［是一个CSI(Control sequence introducer)，转义序列的作用由最后一个字符决定的，
     *这里J表示删除，默认情况下它删除从当前光标处到行尾的内容，
     *这里的2为参数，它表示删除所有的显示内容。也可以使用printf "\x1b[2J"。*/
    //现定位home最开始的位置，然后实现请屏幕操作
    printf("\x1b[H\x1b[2J");    /* Cursor home, clear screen. */
    /* Fill with dots. */
    // 输出.符号填充屏幕
    for (j = 0; j < ws.ws_col*(ws.ws_row-2); j++) printf(".");
    printf("Please keep the test running several minutes per GB of memory.\n");
    printf("Also check http://www.memtest86.com/ and http://pyropus.ca/software/memtester/");
    // 最后一个参数变了，为K，意义也不一样了变成删除当前行操作
    printf("\x1b[H\x1b[2K");          /* Cursor home, clear current line.  */
    printf("%s [%d]\n", title, pass); /* Print title. */
    progress_printed = 0;
    // 求出填满progress bar所需点的个数
    progress_full = ws.ws_col*(ws.ws_row-3);
    fflush(stdout);
}

void memtest_progress_end(void) {
    printf("\x1b[H\x1b[2J");    /* Cursor home, clear screen. */
}

void memtest_progress_step(size_t curr, size_t size, char c) {
    size_t chars = ((unsigned long long)curr*progress_full)/size, j;

    for (j = 0; j < chars-progress_printed; j++) printf("%c",c);
    progress_printed = chars;
    fflush(stdout);
}

/* Test that addressing is fine. Every location is populated with its own
 * address, and finally verified. This test is very fast but may detect
 * ASAP big issues with the memory subsystem. */
// 此方法是测试内存地址是否有效，此种检测的速度是非常快的，但可能会检测出ASAP的巨大问题
// ASAP网上查了下：（可能为）Automated Statistical Analysis Programme 自动统计分析程序
void memtest_addressing(unsigned long *l, size_t bytes) {
    // 算出地址的长度
    unsigned long words = bytes/sizeof(unsigned long);
    unsigned long j, *p;

    /* Fill */
    p = l;
    for (j = 0; j < words; j++) {
        // 将(unsigned long)p强制类型转换到此时的*p，后面以此来判断，没有转换成功，说明存在内存地址的问题
        *p = (unsigned long)p;
        p++;
        // 用A字符填充部分progress bar
        if ((j & 0xffff) == 0) memtest_progress_step(j,words*2,'A');
    }
    /* Test */
    p = l;
    for (j = 0; j < words; j++) {
        // 比较Address的关键在于
        if (*p != (unsigned long)p) {
            printf("\n*** MEMORY ADDRESSING ERROR: %p contains %lu\n",
                (void*) p, *p);
            exit(1);
        }
        p++;
        if ((j & 0xffff) == 0) memtest_progress_step(j+words,words*2,'A');
    }
}

/* Fill words stepping a single page at every write, so we continue to
 * touch all the pages in the smallest amount of time reducing the
 * effectiveness of caches, and making it hard for the OS to transfer
 * pages on the swap. */
// 在每次写操作的时候，在单页上填满整个字符，这样可以做到最快速的触及所有的页面
// 减少了低效率的缓存使用，但是会让分区在转移页面时会比较困难
// 随机填充内存
void memtest_fill_random(unsigned long *l, size_t bytes) {
    // 一页内存的步数
    unsigned long step = 4096/sizeof(unsigned long);
    // 总内存步数再除以2
    unsigned long words = bytes/sizeof(unsigned long)/2;
    unsigned long iwords = words/step;  /* words per iteration */
    unsigned long off, w, *l1, *l2;

    assert((bytes & 4095) == 0);
    for (off = 0; off < step; off++) {
        l1 = l+off;
        l2 = l1+words;
        for (w = 0; w < iwords; w++) {
            // 下面的rand()达到了随机存储的目的
#ifdef MEMTEST_32BIT
            *l1 = *l2 = ((unsigned long)     (rand()&0xffff)) |
                        (((unsigned long)    (rand()&0xffff)) << 16);
#else
            *l1 = *l2 = ((unsigned long)     (rand()&0xffff)) |
                        (((unsigned long)    (rand()&0xffff)) << 16) |
                        (((unsigned long)    (rand()&0xffff)) << 32) |
                        (((unsigned long)    (rand()&0xffff)) << 48);
#endif
            l1 += step;
            l2 += step;
            if ((w & 0xffff) == 0)
                memtest_progress_step(w+iwords*off,words,'R');
        }
    }
}

/* Like memtest_fill_random() but uses the two specified values to fill
 * memory, in an alternated way (v1|v2|v1|v2|...) */
void memtest_fill_value(unsigned long *l, size_t bytes, unsigned long v1,
                        unsigned long v2, char sym)
{
    unsigned long step = 4096/sizeof(unsigned long);
    unsigned long words = bytes/sizeof(unsigned long)/2;
    unsigned long iwords = words/step;  /* words per iteration */
    unsigned long off, w, *l1, *l2, v;

    assert((bytes & 4095) == 0);
    for (off = 0; off < step; off++) {
        l1 = l+off;
        l2 = l1+words;
        v = (off & 1) ? v2 : v1;
        for (w = 0; w < iwords; w++) {
#ifdef MEMTEST_32BIT
            *l1 = *l2 = ((unsigned long)     v) |
                        (((unsigned long)    v) << 16);
#else
            *l1 = *l2 = ((unsigned long)     v) |
                        (((unsigned long)    v) << 16) |
                        (((unsigned long)    v) << 32) |
                        (((unsigned long)    v) << 48);
#endif
            l1 += step;
            l2 += step;
            if ((w & 0xffff) == 0)
                memtest_progress_step(w+iwords*off,words,sym);
        }
    }
}

void memtest_compare(unsigned long *l, size_t bytes) {
    unsigned long words = bytes/sizeof(unsigned long)/2;
    unsigned long w, *l1, *l2;

    assert((bytes & 4095) == 0);
    l1 = l;
    l2 = l1+words;
    for (w = 0; w < words; w++) {
        if (*l1 != *l2) {
            printf("\n*** MEMORY ERROR DETECTED: %p != %p (%lu vs %lu)\n",
                (void*)l1, (void*)l2, *l1, *l2);
            exit(1);
        }
        l1 ++;
        l2 ++;
        if ((w & 0xffff) == 0) memtest_progress_step(w,words,'=');
    }
}

void memtest_compare_times(unsigned long *m, size_t bytes, int pass, int times) {
    int j;

    for (j = 0; j < times; j++) {
        memtest_progress_start("Compare",pass);
        memtest_compare(m,bytes);
        memtest_progress_end();
    }
}

// 整个内存检测类操作的测试方法，passes为目标的循环数
void memtest_test(size_t megabytes, int passes) {
    size_t bytes = megabytes*1024*1024;
    unsigned long *m = malloc(bytes);
    int pass = 0;

    if (m == NULL) {
        fprintf(stderr,"Unable to allocate %zu megabytes: %s",
            megabytes, strerror(errno));
        exit(1);
    }

    // 必须经过passes论循环测试
    while (pass != passes) {
        pass++;

        // 地址检测
        memtest_progress_start("Addressing test",pass);
        memtest_addressing(m,bytes);
        memtest_progress_end();

        // 随机填充检测
        memtest_progress_start("Random fill",pass);
        memtest_fill_random(m,bytes);
        memtest_progress_end();
        // 填充后比较四次
        memtest_compare_times(m,bytes,pass,4);

        // 给定数值填充，这里称为Solid fill固态填充
        memtest_progress_start("Solid fill",pass);
        memtest_fill_value(m,bytes,0,(unsigned long)-1,'S');
        memtest_progress_end();
        // 填充后比较四次
        memtest_compare_times(m,bytes,pass,4);

        // 也是属于给定数值填充，这里叫Checkerboard fill键盘填充
        memtest_progress_start("Checkerboard fill",pass);
        memtest_fill_value(m,bytes,ULONG_ONEZERO,ULONG_ZEROONE,'C');
        memtest_progress_end();
        // 填充后比较四次
        memtest_compare_times(m,bytes,pass,4);
    }
}

void memtest_non_destructive_invert(void *addr, size_t size) {
    volatile unsigned long *p = addr;
    size_t words = size / sizeof(unsigned long);
    size_t j;

    /* Invert */
    for (j = 0; j < words; j++)
        p[j] = ~p[j];
}

void memtest_non_destructive_swap(void *addr, size_t size) {
    volatile unsigned long *p = addr;
    size_t words = size / sizeof(unsigned long);
    size_t j;

    /* Swap */
    for (j = 0; j < words; j += 2) {
        unsigned long a, b;

        a = p[j];
        b = p[j+1];
        p[j] = b;
        p[j+1] = a;
    }
}

// 开发给整个系统使用的内存检测方法
void memtest(size_t megabytes, int passes) {
    // ioctl()函数控制I/O设备 ，提供了一种获得设备信息和向设备发送控制参数的手段。用于向设备发控制和配置命令 ，
    // 有些命令需要控制参数，这些数据是不能用read / write 读写的,称为Out-of-band数据。
    // 也就是说,read / write 读写的数据是in-band数据,是I/O操作的主体，而ioctl 命令传送的是控制信息,其中的数据是辅助的数据。
    if (ioctl(1, TIOCGWINSZ, &ws) == -1) {
        ws.ws_col = 80;
        ws.ws_row = 20;
    }
    memtest_test(megabytes,passes);
    printf("\nYour memory passed this test.\n");
    printf("Please if you are still in doubt use the following two tools:\n");
    printf("1) memtest86: http://www.memtest86.com/\n");
    printf("2) memtester: http://pyropus.ca/software/memtester/\n");
    exit(0);
}
