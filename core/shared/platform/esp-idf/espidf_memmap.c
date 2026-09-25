/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_cache.h"   /* Moybyte #158 */
#endif
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
#include "soc/mmu.h"
#include "rom/cache.h"

/* Moybyte #158: PSRAM at data address D is fetched as instructions at
 * D + (SOC_IROM_LOW - SOC_DROM_LOW) -- the S3 MMU serves both buses from
 * one table. Upstream's (SOC_IROM_LOW - SOC_IROM_HIGH) lands outside every
 * mapped range on IDF 5.5. */
#define MEM_DUAL_BUS_OFFSET (SOC_IROM_LOW - SOC_DROM_LOW)

#define in_ibus_ext(addr) \
    (((uint32)addr >= SOC_IROM_LOW) && ((uint32)addr < SOC_IROM_HIGH))

static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
    if (prot & MMAP_PROT_EXEC) {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#elif CONFIG_IDF_TARGET_ESP32P4
        /* Moybyte #158: PSRAM carries no PMP entry on the P4, so it is RWX; the
         * internal exec heap is the fallback (present only with
         * CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT=n). */
        uint32_t mem_caps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > size + 64
                                ? MALLOC_CAP_SPIRAM : MALLOC_CAP_EXEC;
        os_printf("WAMR exec mmap %u bytes from %s\n", (unsigned)size,
                  mem_caps == MALLOC_CAP_SPIRAM ? "PSRAM" : "internal exec heap");
#else
        uint32_t mem_caps = MALLOC_CAP_EXEC;
#endif

        // Memory allocation with MALLOC_CAP_EXEC will return 4-byte aligned
        // Reserve extra 4 byte to fixup alignment and size for the pointer to
        // the originally allocated address
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        /* Moybyte #158: the instruction-bus mirror is fetch-only on the S3
         * (a store through it is StoreProhibited); clear via the data bus. */
        memset(buf_fixed, 0, size);
        return buf_fixed + MEM_DUAL_BUS_OFFSET;
#else
        memset(buf_fixed, 0, size);
        return buf_fixed;
#endif
    }
    else {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#else
        uint32_t mem_caps = MALLOC_CAP_8BIT;
#endif
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }

        // Memory allocation with MALLOC_CAP_SPIRAM or MALLOC_CAP_8BIT will
        // return 4-byte aligned Reserve extra 4 byte to fixup alignment and
        // size for the pointer to the originally allocated address
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;

        memset(buf_fixed, 0, size);
        return buf_fixed;
    }
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    return os_mremap_slow(old_addr, old_size, new_size);
}

void
os_munmap(void *addr, size_t size)
{
    char *ptr = (char *)addr;

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    if (in_ibus_ext(ptr)) {
        ptr -= MEM_DUAL_BUS_OFFSET;
    }
#endif
    // We don't need special handling of the executable allocations
    // here, free() of esp-idf handles it properly
    return os_free(ptr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    return 0;
}

void
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    IRAM_ATTR
#endif
    os_dcache_flush()
{
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    uint32_t preload;
    extern void Cache_WriteBack_All(void);

    portENTER_CRITICAL(&s_spinlock);

    Cache_WriteBack_All();
    preload = Cache_Disable_ICache();
    Cache_Enable_ICache(preload);

    portEXIT_CRITICAL(&s_spinlock);
#endif
}

void
os_icache_flush(void *start, size_t len)
{
#if CONFIG_IDF_TARGET_ESP32P4
    /* Moybyte #158: the text was written through the data cache; push it out and
     * drop whatever the instruction cache holds for that range. */
    if (start && len) {
        uintptr_t a = (uintptr_t)start & ~(uintptr_t)63;   /* 64 B lines; M2C wants aligned */
        size_t n = (((uintptr_t)start + len + 63) & ~(uintptr_t)63) - a;
        esp_cache_msync((void *)a, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        esp_cache_msync((void *)a, n, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
        __asm__ volatile("fence.i" ::: "memory");
    }
#else
    (void)start;
    (void)len;
#endif
}

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
/* Moybyte #158: one flash partition mapped twice -- ESP_PARTITION_MMAP_DATA for
 * the loader's reads, ESP_PARTITION_MMAP_INST for execution. */
static const char *s_xip_dbus, *s_xip_ibus;
static size_t s_xip_size;

void
os_register_xip_window(const void *ibus, const void *dbus, size_t size)
{
    s_xip_ibus = ibus;
    s_xip_dbus = dbus;
    s_xip_size = size;
}

void *
os_get_ibus_mirror(void *dbus)
{
    const char *p = dbus;
    if (s_xip_size && p >= s_xip_dbus && p < s_xip_dbus + s_xip_size) {
        return (void *)(s_xip_ibus + (p - s_xip_dbus));
    }
    if ((uint32_t)p >= SOC_EXTRAM_DATA_LOW && (uint32_t)p < SOC_EXTRAM_DATA_HIGH) {
        return (void *)(p + MEM_DUAL_BUS_OFFSET);
    }
    return dbus;
}

void *
os_get_dbus_mirror(void *ibus)
{
    const char *p = ibus;
    if (s_xip_size && p >= s_xip_ibus && p < s_xip_ibus + s_xip_size) {
        return (void *)(s_xip_dbus + (p - s_xip_ibus));
    }
    if (in_ibus_ext(ibus)) {
        return (void *)((char *)ibus - MEM_DUAL_BUS_OFFSET);
    }
    else {
        return ibus;
    }
}
#endif
