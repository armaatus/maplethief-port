/* Import models of the differential test, C side (#305): the same functions as
 * recomp/difftest/models.py, in the same order (the import id), with the same byte order, so
 * a fault lands on the same address (the first byte outside the window, pc = the stub). The
 * module docstring there has the rules. Writes into __TEXT are not checked here: the Unicorn
 * side drops such a case before the C side runs it. */
#include "maplethief/recomp/recomp.h"

/* Generated next to the lifted code (fns.c): the stub address of each model. */
extern const uint32_t mt_dt_import_stubs[];

static uint8_t rd(mt_cpu *cpu, uint32_t id, uint32_t addr) {
    if (addr >= cpu->mem_size) {
        mt_mem_fault(cpu, addr, 1u, mt_dt_import_stubs[id]);
    }
    return cpu->mem[addr];
}

static void wr(mt_cpu *cpu, uint32_t id, uint32_t addr, uint8_t v) {
    if (addr >= cpu->mem_size) {
        mt_mem_fault(cpu, addr, 1u, mt_dt_import_stubs[id]);
    }
    cpu->mem[addr] = v;
}

static uint32_t quot32(int32_t a, int32_t b) {
    uint32_t ua = a < 0 ? 0u - (uint32_t)a : (uint32_t)a;
    uint32_t ub = b < 0 ? 0u - (uint32_t)b : (uint32_t)b;
    uint32_t q = ua / ub;
    return (a < 0) != (b < 0) ? 0u - q : q;
}

static uint64_t quot64(int64_t a, int64_t b) {
    uint64_t ua = a < 0 ? 0u - (uint64_t)a : (uint64_t)a;
    uint64_t ub = b < 0 ? 0u - (uint64_t)b : (uint64_t)b;
    uint64_t q = ua / ub;
    return (a < 0) != (b < 0) ? 0u - q : q;
}

static void divsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    cpu->r[0] = cpu->r[1] ? quot32((int32_t)cpu->r[0], (int32_t)cpu->r[1]) : 0u;
}

static void modsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    if (cpu->r[1]) {
        cpu->r[0] -= quot32((int32_t)cpu->r[0], (int32_t)cpu->r[1]) * cpu->r[1];
    }
}

static void udivsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    cpu->r[0] = cpu->r[1] ? cpu->r[0] / cpu->r[1] : 0u;
}

static void umodsi3(mt_cpu *cpu, uint32_t id) {
    (void)id;
    if (cpu->r[1]) {
        cpu->r[0] %= cpu->r[1];
    }
}

static void divdi3(mt_cpu *cpu, uint32_t id) {
    uint64_t a = (uint64_t)cpu->r[0] | (uint64_t)cpu->r[1] << 32;
    uint64_t b = (uint64_t)cpu->r[2] | (uint64_t)cpu->r[3] << 32;
    uint64_t q = b ? quot64((int64_t)a, (int64_t)b) : 0u;
    (void)id;
    cpu->r[0] = (uint32_t)q;
    cpu->r[1] = (uint32_t)(q >> 32);
}

static void udivdi3(mt_cpu *cpu, uint32_t id) {
    uint64_t a = (uint64_t)cpu->r[0] | (uint64_t)cpu->r[1] << 32;
    uint64_t b = (uint64_t)cpu->r[2] | (uint64_t)cpu->r[3] << 32;
    uint64_t q = b ? a / b : 0u;
    (void)id;
    cpu->r[0] = (uint32_t)q;
    cpu->r[1] = (uint32_t)(q >> 32);
}

static void memcmp_(mt_cpu *cpu, uint32_t id) {
    uint32_t a = cpu->r[0], b = cpu->r[1], n = cpu->r[2], i;
    for (i = 0; i < n; i++) {
        uint8_t x = rd(cpu, id, a + i), y = rd(cpu, id, b + i);
        if (x != y) {
            cpu->r[0] = (uint32_t)x - (uint32_t)y;
            return;
        }
    }
    cpu->r[0] = 0u;
}

static void memcpy_(mt_cpu *cpu, uint32_t id) {
    uint32_t d = cpu->r[0], s = cpu->r[1], n = cpu->r[2], i;
    for (i = 0; i < n; i++) {
        wr(cpu, id, d + i, rd(cpu, id, s + i));
    }
}

static void memset_(mt_cpu *cpu, uint32_t id) {
    uint32_t d = cpu->r[0], n = cpu->r[2], i;
    for (i = 0; i < n; i++) {
        wr(cpu, id, d + i, (uint8_t)cpu->r[1]);
    }
}

static void strcpy_at(mt_cpu *cpu, uint32_t id, uint32_t d, uint32_t s) {
    uint32_t i;
    for (i = 0;; i++) {
        uint8_t b = rd(cpu, id, s + i);
        wr(cpu, id, d + i, b);
        if (b == 0u) {
            return;
        }
    }
}

static uint32_t strlen_at(mt_cpu *cpu, uint32_t id, uint32_t s) {
    uint32_t i = 0;
    while (rd(cpu, id, s + i) != 0u) {
        i++;
    }
    return i;
}

static void strcat_(mt_cpu *cpu, uint32_t id) {
    uint32_t d = cpu->r[0];
    strcpy_at(cpu, id, d + strlen_at(cpu, id, d), cpu->r[1]);
}

static void strcmp_(mt_cpu *cpu, uint32_t id) {
    uint32_t a = cpu->r[0], b = cpu->r[1], i;
    for (i = 0;; i++) {
        uint8_t x = rd(cpu, id, a + i), y = rd(cpu, id, b + i);
        if (x != y || x == 0u) {
            cpu->r[0] = (uint32_t)x - (uint32_t)y;
            return;
        }
    }
}

static void strcpy_(mt_cpu *cpu, uint32_t id) { strcpy_at(cpu, id, cpu->r[0], cpu->r[1]); }

static void strlen_(mt_cpu *cpu, uint32_t id) { cpu->r[0] = strlen_at(cpu, id, cpu->r[0]); }

/* Unmodeled imports never run: a function that calls one is not selected. */
static void unmodeled(mt_cpu *cpu, uint32_t id) { mt_trap(cpu, MT_TRAP_IMPORT, cpu->r[14], id); }

mt_import_fn mt_import_table[] = {
    divsi3,  modsi3,  udivsi3, umodsi3, divdi3,  udivdi3, memcmp_,
    memcpy_, memset_, strcat_, strcmp_, strcpy_, strlen_, unmodeled,
};
const char *const mt_import_names[] = {
    "___divsi3", "___modsi3", "___udivsi3", "___umodsi3", "___divdi3", "___udivdi3", "_memcmp",
    "_memcpy",   "_memset",   "_strcat",    "_strcmp",    "_strcpy",   "_strlen",    "",
};
