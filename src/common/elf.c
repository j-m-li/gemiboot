/*
 * GEMIBOOT - x86 Boot Loader for GEMIOS
 * Dedicated to the Public Domain (CC0)
 */

#include "elf.h"
#include "types.h"

int elf32_validate(const void *image, size_t size) {
    const Elf32_Ehdr *ehdr;

    if (!image || size < sizeof(Elf32_Ehdr)) {
        return -1;
    }

    ehdr = (const Elf32_Ehdr*)image;

    /* Verify ELF Magic */
    if (ehdr->e_ident[0] != ELFMAG0 ||
        ehdr->e_ident[1] != ELFMAG1 ||
        ehdr->e_ident[2] != ELFMAG2 ||
        ehdr->e_ident[3] != ELFMAG3) {
        return -1;
    }

    /* Verify 32-bit Little Endian Intel 386 executable */
    if (ehdr->e_ident[4] != ELFCLASS32 ||
        ehdr->e_ident[5] != ELFDATA2LSB ||
        ehdr->e_type != ET_EXEC ||
        ehdr->e_machine != EM_386) {
        return -1;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return -1;
    }

    return 0;
}

uint32_t elf32_load(const void *image, size_t size) {
    const Elf32_Ehdr *ehdr;
    const uint8_t *raw_bytes;
    uint16_t i;

    if (elf32_validate(image, size) != 0) {
        return 0;
    }

    ehdr = (const Elf32_Ehdr*)image;
    raw_bytes = (const uint8_t*)image;

    for (i = 0; i < ehdr->e_phnum; i++) {
        const Elf32_Phdr *phdr;
        uint32_t phdr_offset;

        phdr_offset = ehdr->e_phoff + (uint32_t)(i * ehdr->e_phentsize);
        if (phdr_offset + sizeof(Elf32_Phdr) > size) {
            return 0;
        }

        phdr = (const Elf32_Phdr*)(raw_bytes + phdr_offset);

        if (phdr->p_type == PT_LOAD) {
            void *dest;
            const void *src;

            dest = (void*)(uintptr_t)phdr->p_paddr;
            src = (const void*)(raw_bytes + phdr->p_offset);

            if (phdr->p_filesz > 0) {
                if (phdr->p_offset + phdr->p_filesz > size) {
                    return 0;
                }
                memcpy(dest, src, phdr->p_filesz);
            }

            /* Clear BSS (p_memsz > p_filesz) */
            if (phdr->p_memsz > phdr->p_filesz) {
                void *bss_start;
                size_t bss_size;

                bss_start = (void*)((uintptr_t)phdr->p_paddr + phdr->p_filesz);
                bss_size = (size_t)(phdr->p_memsz - phdr->p_filesz);
                memset(bss_start, 0, bss_size);
            }
        }
    }

    return ehdr->e_entry;
}
