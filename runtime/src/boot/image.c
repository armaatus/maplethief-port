/* What the boot reads from the Mach-O besides its segments (maplethief/boot.h, ADR 0027):
 * the sections, the LC_UNIXTHREAD entry, the size of the rebase opcodes, and whether the
 * entry is crt1's start stub. The file is untrusted, so every offset and size is checked
 * before it is read. */
#include <stdio.h>
#include <string.h>

#include "maplethief/boot.h"

#define MH_MAGIC 0xFEEDFACEu
#define CPU_TYPE_ARM 12u
#define LC_SEGMENT 0x1u
#define LC_UNIXTHREAD 0x5u
#define LC_DYLD_INFO 0x22u
#define LC_DYLD_INFO_ONLY 0x80000022u
#define ARM_THREAD_STATE 1u
#define ARM_THREAD_STATE_COUNT 17u /* r0-r12, sp, lr, pc, cpsr */
#define HEADER_SIZE 28u
#define SEGMENT_SIZE 56u
#define SECTION_SIZE 68u

/* crt1.o's start for iOS armv7 (ARM): the kernel left argc at [sp]; compute argv, envp and
 * apple, align sp to 8, then `blx main` and `b exit`. The game's stub at 0x27E0 is exactly
 * this; the tenth word is the blx. */
static const uint32_t k_crt1_start[9] = {
    0xE59D0000u, /* ldr  r0, [sp]             argc */
    0xE28D1004u, /* add  r1, sp, #4           argv */
    0xE2804001u, /* add  r4, r0, #1 */
    0xE0812104u, /* add  r2, r1, r4, lsl #2   envp */
    0xE3CDD007u, /* bic  sp, sp, #7 */
    0xE1A03002u, /* mov  r3, r2 */
    0xE4934004u, /* ldr  r4, [r3], #4         skip envp ... */
    0xE3540000u, /* cmp  r4, #0 */
    0x1AFFFFFCu, /* bne  .-8                  ... to apple */
};

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int fail(const char *why, uint32_t value) {
    fprintf(stderr, "Mach-O image: %s (0x%X)\n", why, (unsigned)value);
    return -1;
}

static void copy_name(char out[17], const uint8_t *in) {
    memcpy(out, in, 16);
    out[16] = '\0';
}

/* File offset of guest address `addr` for `len` bytes inside one segment's file data, or -1. */
static long file_offset(const uint8_t *file, size_t size, uint32_t addr, uint32_t len) {
    uint32_t ncmds = rd32(file + 16), off = HEADER_SIZE, i;
    for (i = 0; i < ncmds; i++) {
        uint32_t cmd = rd32(file + off), cmdsize = rd32(file + off + 4);
        if (cmd == LC_SEGMENT) {
            uint32_t vmaddr = rd32(file + off + 24), fileoff = rd32(file + off + 32);
            uint32_t filesize = rd32(file + off + 36);
            if (addr >= vmaddr && (uint64_t)addr - vmaddr + len <= filesize &&
                (uint64_t)fileoff + filesize <= size) {
                return (long)(fileoff + (addr - vmaddr));
            }
        }
        off += cmdsize;
    }
    return -1;
}

/* main's function pointer if `entry` is crt1's start stub, else 0. */
static uint32_t crt1_main(const uint8_t *file, size_t size, uint32_t entry, uint32_t cpsr) {
    long at;
    uint32_t i, blx, imm;
    if ((entry & 3u) != 0u || (cpsr & MT_CPSR_T) != 0u) {
        return 0u; /* the stub is ARM code */
    }
    at = file_offset(file, size, entry, 40u);
    if (at < 0) {
        return 0u;
    }
    for (i = 0; i < 9u; i++) {
        if (rd32(file + at + 4 * i) != k_crt1_start[i]) {
            return 0u;
        }
    }
    blx = rd32(file + at + 36);
    if ((blx & 0xFE000000u) != 0xFA000000u) { /* blx <label>: cond 1111, 101H */
        return 0u;
    }
    /* target = pc (the blx + 8) + sign-extended imm24 * 4 + H * 2, in Thumb state */
    imm = (blx & 0x00FFFFFFu) << 2;
    if ((imm & 0x02000000u) != 0u) {
        imm |= 0xFC000000u;
    }
    return (entry + 36u + 8u + imm + ((blx >> 23) & 2u)) | 1u;
}

int mt_image_info_read(const uint8_t *file, size_t size, mt_image_info *out) {
    uint32_t ncmds, sizeofcmds, off, i, have_entry = 0;
    memset(out, 0, sizeof *out);
    if (file == NULL || size < HEADER_SIZE) {
        return fail("file too small for a Mach header", (uint32_t)size);
    }
    if (rd32(file) != MH_MAGIC || rd32(file + 4) != CPU_TYPE_ARM) {
        return fail("not a thin 32-bit ARM Mach-O", rd32(file));
    }
    ncmds = rd32(file + 16);
    sizeofcmds = rd32(file + 20);
    if ((uint64_t)HEADER_SIZE + sizeofcmds > size) {
        return fail("load commands run past the end of the file", sizeofcmds);
    }
    off = HEADER_SIZE;
    for (i = 0; i < ncmds; i++) {
        uint32_t cmd, cmdsize;
        if ((uint64_t)off + 8u > HEADER_SIZE + (uint64_t)sizeofcmds) {
            return fail("load command header past sizeofcmds", i);
        }
        cmd = rd32(file + off);
        cmdsize = rd32(file + off + 4);
        if (cmdsize < 8u || (cmdsize & 3u) != 0u ||
            (uint64_t)off + cmdsize > HEADER_SIZE + (uint64_t)sizeofcmds) {
            return fail("bad load command size", cmdsize);
        }
        if (cmd == LC_SEGMENT) {
            uint32_t nsects, s;
            if (cmdsize < SEGMENT_SIZE) {
                return fail("LC_SEGMENT too small", cmdsize);
            }
            nsects = rd32(file + off + 48);
            if ((uint64_t)SEGMENT_SIZE + (uint64_t)nsects * SECTION_SIZE > cmdsize) {
                return fail("LC_SEGMENT sections run past the command", nsects);
            }
            if (rd32(file + off + 32) == 0u && rd32(file + off + 36) != 0u) {
                out->mach_header = rd32(file + off + 24); /* the segment that maps the header */
            }
            for (s = 0; s < nsects; s++) {
                const uint8_t *sec = file + off + SEGMENT_SIZE + s * SECTION_SIZE;
                mt_section *o;
                if (out->section_count == MT_MAX_SECTIONS) {
                    return fail("too many sections", out->section_count);
                }
                o = &out->sections[out->section_count++];
                copy_name(o->sectname, sec);
                copy_name(o->segname, sec + 16);
                o->addr = rd32(sec + 32);
                o->size = rd32(sec + 36);
                o->flags = rd32(sec + 56);
                if ((uint64_t)o->addr + o->size > 0x100000000ull) {
                    return fail("section wraps around the address space", o->addr);
                }
            }
        } else if (cmd == LC_UNIXTHREAD) {
            uint32_t at = off + 8u, end = off + cmdsize;
            while ((uint64_t)at + 8u <= end) {
                uint32_t flavor = rd32(file + at), count = rd32(file + at + 4);
                if ((uint64_t)at + 8u + (uint64_t)count * 4u > end) {
                    return fail("LC_UNIXTHREAD state runs past the command", count);
                }
                if (flavor == ARM_THREAD_STATE && count == ARM_THREAD_STATE_COUNT) {
                    out->entry = rd32(file + at + 8 + 15 * 4);
                    out->entry_cpsr = rd32(file + at + 8 + 16 * 4);
                    have_entry = 1;
                }
                at += 8u + count * 4u;
            }
        } else if (cmd == LC_DYLD_INFO || cmd == LC_DYLD_INFO_ONLY) {
            if (cmdsize < 48u) {
                return fail("LC_DYLD_INFO too small", cmdsize);
            }
            out->rebase_size = rd32(file + off + 12);
        }
        off += cmdsize;
    }
    if (!have_entry) {
        return fail("no LC_UNIXTHREAD with an ARM thread state", 0u);
    }
    out->main = crt1_main(file, size, out->entry, out->entry_cpsr);
    return 0;
}

const mt_section *mt_image_section(const mt_image_info *info, const char *segname,
                                   const char *sectname) {
    uint32_t i;
    for (i = 0; i < info->section_count; i++) {
        const mt_section *s = &info->sections[i];
        if (strcmp(s->segname, segname) == 0 && strcmp(s->sectname, sectname) == 0) {
            return s;
        }
    }
    return NULL;
}
