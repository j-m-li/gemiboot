/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#ifndef GEMIBOOT_ELF_H
#define GEMIBOOT_ELF_H 1

#include "types.h"

#define EI_NIDENT       16

#define ELFMAG0         0x7F
#define ELFMAG1         'E'
#define ELFMAG2         'L'
#define ELFMAG3         'F'

#define ELFCLASS32      1
#define ELFDATA2LSB     1
#define EV_CURRENT      1
#define ET_EXEC         2
#define EM_386          3

#define PT_NULL         0
#define PT_LOAD         1
#define PT_DYNAMIC      2
#define PT_INTERP       3
#define PT_NOTE         4
#define PT_SHLIB        5
#define PT_PHDR         6

#define PF_X            (1 << 0)
#define PF_W            (1 << 1)
#define PF_R            (1 << 2)

#if defined(__GNUC__) || defined(__clang__)
#define PACKED __attribute__((packed))
#else
#define PACKED
#endif

#pragma pack(push, 1)

typedef struct {
    uint8_t   e_ident[EI_NIDENT];
    uint16_t  e_type;
    uint16_t  e_machine;
    uint32_t  e_version;
    uint32_t  e_entry;
    uint32_t  e_phoff;
    uint32_t  e_shoff;
    uint32_t  e_flags;
    uint16_t  e_ehsize;
    uint16_t  e_phentsize;
    uint16_t  e_phnum;
    uint16_t  e_shentsize;
    uint16_t  e_shnum;
    uint16_t  e_shstrndx;
} PACKED Elf32_Ehdr;

typedef struct {
    uint32_t  p_type;
    uint32_t  p_offset;
    uint32_t  p_vaddr;
    uint32_t  p_paddr;
    uint32_t  p_filesz;
    uint32_t  p_memsz;
    uint32_t  p_flags;
    uint32_t  p_align;
} PACKED Elf32_Phdr;

#pragma pack(pop)

int elf32_validate(const void *image, size_t size);
uint32_t elf32_load(const void *image, size_t size);

#endif /* GEMIBOOT_ELF_H */
