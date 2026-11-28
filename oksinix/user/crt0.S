/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 Linux i386 static-PIE ELF32 CRT0 entry point
 *
 * Performs standard Linux static-PIE self-relocation (R_386_RELATIVE via
 * auxv AT_PHDR / PT_PHDR / PT_DYNAMIC / DT_REL) before invoking main(argc, argv).
 */

.section .text
.code32
.global _start
.type _start, @function
_start:
    /* Preserve initial System V i386 ELF stack pointer in %ebp */
    movl %esp, %ebp

    /* Advance %esi past argc, argv[0..argc], and envp[0..envc] to auxv[] */
    movl 0(%ebp), %eax            /* argc */
    leal 8(%ebp,%eax,4), %esi     /* &envp[0] */
.Lskip_env:
    movl (%esi), %eax
    addl $4, %esi
    testl %eax, %eax
    jnz .Lskip_env

    /* Read AT_PHDR (3), AT_PHENT (4), AT_PHNUM (5) from auxv[] */
    xorl %ebx, %ebx               /* phdr = 0 */
    xorl %ecx, %ecx               /* phent = 0 */
    xorl %edx, %edx               /* phnum = 0 */
.Laux_loop:
    movl 0(%esi), %eax            /* a_type */
    testl %eax, %eax              /* AT_NULL == 0 */
    jz .Laux_done
    movl 4(%esi), %edi            /* a_val */
    addl $8, %esi
    cmpl $3, %eax                 /* AT_PHDR */
    jne 1f
    movl %edi, %ebx
    jmp .Laux_loop
1:  cmpl $4, %eax                 /* AT_PHENT */
    jne 2f
    movl %edi, %ecx
    jmp .Laux_loop
2:  cmpl $5, %eax                 /* AT_PHNUM */
    jne .Laux_loop
    movl %edi, %edx
    jmp .Laux_loop
.Laux_done:
    testl %ebx, %ebx
    jz .Lreloc_done
    testl %ecx, %ecx
    jz .Lreloc_done
    testl %edx, %edx
    jz .Lreloc_done

    /* Scan program headers for PT_PHDR (6) -> load_bias, and PT_DYNAMIC (2) -> dyn_vaddr */
    xorl %esi, %esi               /* load_bias = 0 */
    xorl %edi, %edi               /* dyn_vaddr = 0 (unbiased) */
    pushl %edx                    /* save phnum */
    pushl %ebx                    /* save AT_PHDR */
.Lphdr_loop:
    movl 0(%ebx), %eax            /* p_type */
    cmpl $6, %eax                 /* PT_PHDR */
    jne 1f
    movl 0(%esp), %esi            /* AT_PHDR */
    subl 8(%ebx), %esi            /* load_bias = AT_PHDR - phdr->p_vaddr */
    jmp 2f
1:  cmpl $2, %eax                 /* PT_DYNAMIC */
    jne 2f
    movl 8(%ebx), %edi            /* phdr->p_vaddr */
2:  addl %ecx, %ebx
    decl %edx
    jnz .Lphdr_loop
    addl $8, %esp

    testl %esi, %esi
    jz .Lreloc_done
    testl %edi, %edi
    jz .Lreloc_done

    /* %edi = runtime _DYNAMIC address */
    addl %esi, %edi

    /* Scan Elf32_Dyn for DT_REL (17) and DT_RELSZ (18) */
    xorl %ebx, %ebx               /* rel_va = 0 */
    xorl %ecx, %ecx               /* rel_sz = 0 */
.Ldyn_loop:
    movl 0(%edi), %eax            /* d_tag */
    testl %eax, %eax              /* DT_NULL == 0 */
    jz .Ldyn_done
    cmpl $17, %eax                /* DT_REL */
    jne 1f
    movl 4(%edi), %ebx
    jmp 2f
1:  cmpl $18, %eax                /* DT_RELSZ */
    jne 2f
    movl 4(%edi), %ecx
2:  addl $8, %edi
    jmp .Ldyn_loop
.Ldyn_done:
    testl %ebx, %ebx
    jz .Lreloc_done
    testl %ecx, %ecx
    jz .Lreloc_done

    cmpl %esi, %ebx
    jae 1f
    addl %esi, %ebx
1:
    /* Apply R_386_RELATIVE (type 8) relocations: *(load_bias + r_offset) += load_bias */
.Lrel_loop:
    cmpl $8, %ecx
    jb .Lreloc_done
    movl 4(%ebx), %eax            /* r_info */
    andl $0xFF, %eax              /* ELF32_R_TYPE(r_info) */
    cmpl $8, %eax                 /* R_386_RELATIVE */
    jne 2f
    movl 0(%ebx), %edx            /* r_offset */
    addl %esi, %edx               /* load_bias + r_offset */
    addl %esi, (%edx)             /* *where += load_bias */
2:  addl $8, %ebx
    subl $8, %ecx
    jmp .Lrel_loop

.Lreloc_done:
    /* Restore stack and invoke main(argc, argv) */
    movl %ebp, %esp
    movl 0(%esp), %eax
    leal 4(%esp), %ecx
    xorl %ebp, %ebp
    pushl %ecx
    pushl %eax
    call main

    /* Exit via Linux i386 SYS_exit_group (252) with main() return code */
    movl %eax, %ebx
    movl $252, %eax
    int $0x80
1:  jmp 1b

.size _start, . - _start
.section .note.GNU-stack,"",@progbits
