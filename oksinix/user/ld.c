/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 ELF32 i386 Static Linker (/mnt/LD.ELF)
 *
 * Links one or more ELF32 relocatable object files (ET_REL, EM_386) into a
 * runnable position-independent ELF32 executable (ET_DYN). When R_386_32
 * absolute address fixups are present, prepends a 41-byte self-relocating
 * PIC entry stub so the output runs cleanly at any load address on Oksinix
 * (BIOS, UEFI32, UEFI64) and native Linux.
 */

typedef unsigned char  u8_t;
typedef unsigned short u16_t;
typedef unsigned int   u32_t;
typedef int            s32_t;

extern s32_t __oksinix_int80(u32_t nr, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5);

#define SYS_read    3U
#define SYS_write   4U
#define SYS_open    5U
#define SYS_close   6U

#define O_RDONLY    0x0000U
#define O_WRONLY    0x0001U
#define O_CREAT     0x0040U
#define O_TRUNC     0x0200U

#define ET_REL       1U
#define ET_DYN       3U
#define EM_386       3U
#define PT_LOAD      1U

#define SHT_PROGBITS 1U
#define SHT_SYMTAB   2U
#define SHT_STRTAB   3U
#define SHT_NOBITS   8U
#define SHT_REL      9U

#define SHF_WRITE     0x1U
#define SHF_ALLOC     0x2U
#define SHF_EXECINSTR 0x4U

#define STB_LOCAL   0U
#define STB_GLOBAL  1U
#define STB_WEAK    2U

#define SHN_UNDEF   0U
#define SHN_ABS     0xFFF1U
#define SHN_COMMON  0xFFF2U

#define R_386_NONE   0U
#define R_386_32     1U
#define R_386_PC32   2U
#define R_386_GOT32  3U
#define R_386_PLT32  4U
#define R_386_GOTOFF 9U
#define R_386_GOTPC  10U
#define R_386_GOT32X 43U

#define MAX_OBJS       16
#define MAX_OBJ_BYTES  131072U
#define MAX_SECS_PER   24
#define MAX_GSYMS      2048
#define MAX_REL32      2048
#define MAX_OUT_BYTES  262144U
#define MAX_SYM_NAME   64

struct elf32_ehdr {
    u8_t  e_ident[16];
    u16_t e_type;
    u16_t e_machine;
    u32_t e_version;
    u32_t e_entry;
    u32_t e_phoff;
    u32_t e_shoff;
    u32_t e_flags;
    u16_t e_ehsize;
    u16_t e_phentsize;
    u16_t e_phnum;
    u16_t e_shentsize;
    u16_t e_shnum;
    u16_t e_shstrndx;
};

struct elf32_phdr {
    u32_t p_type;
    u32_t p_offset;
    u32_t p_vaddr;
    u32_t p_paddr;
    u32_t p_filesz;
    u32_t p_memsz;
    u32_t p_flags;
    u32_t p_align;
};

struct elf32_shdr {
    u32_t sh_name;
    u32_t sh_type;
    u32_t sh_flags;
    u32_t sh_addr;
    u32_t sh_offset;
    u32_t sh_size;
    u32_t sh_link;
    u32_t sh_info;
    u32_t sh_addralign;
    u32_t sh_entsize;
};

struct elf32_sym {
    u32_t st_name;
    u32_t st_value;
    u32_t st_size;
    u8_t  st_info;
    u8_t  st_other;
    u16_t st_shndx;
};

struct elf32_rel {
    u32_t r_offset;
    u32_t r_info;
};

struct obj_info {
    const char *path;
    u32_t       file_off;   /* offset in obj_pool */
    u32_t       file_size;
    u16_t       shnum;
    u32_t       shoff;
    int         symtab_sec;
    int         strtab_sec;
    /* Mapped vaddr of each input section in final image (0xFFFFFFFF if not allocated) */
    u32_t       sec_vaddr[MAX_SECS_PER];
    int         sec_kind[MAX_SECS_PER]; /* 0=none, 1=text, 2=rodata, 3=data, 4=bss */
};

struct gsym {
    char  name[MAX_SYM_NAME];
    u32_t vaddr;
    int   is_abs;
    int   is_weak;
    int   defined;
};

int main(int argc, char **argv);

static u8_t            obj_pool[MAX_OBJ_BYTES * 2U];
static u32_t           obj_pool_used = 0U;
static struct obj_info objs[MAX_OBJS];
static int             num_objs = 0;

static struct gsym     gsyms[MAX_GSYMS];
static int             num_gsyms = 0;

static u32_t           rel32_offs[MAX_REL32];
static u32_t           num_rel32 = 0U;

static u8_t            out_img[MAX_OUT_BYTES];
static int             ld_errors = 0;

static int sys_open(const char *path, u32_t flags, u32_t mode)
{
    return (int)__oksinix_int80(SYS_open, (u32_t)path, flags, mode, 0U, 0U);
}

static s32_t sys_read(int fd, void *buf, u32_t len)
{
    return __oksinix_int80(SYS_read, (u32_t)fd, (u32_t)buf, len, 0U, 0U);
}

static s32_t sys_write(int fd, const void *buf, u32_t len)
{
    return __oksinix_int80(SYS_write, (u32_t)fd, (u32_t)buf, len, 0U, 0U);
}

static int sys_close(int fd)
{
    return (int)__oksinix_int80(SYS_close, (u32_t)fd, 0U, 0U, 0U, 0U);
}

static u32_t u_strlen(const char *s)
{
    u32_t n = 0U;
    while (s[n] != '\0') n++;
    return n;
}

static int u_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) { a++; b++; }
    return (int)(u8_t)*a - (int)(u8_t)*b;
}

static void u_memcpy(void *dst, const void *src, u32_t n)
{
    u8_t *d = (u8_t *)dst;
    const u8_t *s = (const u8_t *)src;
    while (n > 0U) { *d++ = *s++; n--; }
}

static void u_memset(void *dst, int val, u32_t n)
{
    u8_t *d = (u8_t *)dst;
    while (n > 0U) { *d++ = (u8_t)val; n--; }
}

static void ld_puts(const char *s)
{
    (void)sys_write(2, s, u_strlen(s));
}

static u32_t read_u32_le(const u8_t *p)
{
    return ((u32_t)p[0]) | ((u32_t)p[1] << 8) |
           ((u32_t)p[2] << 16) | ((u32_t)p[3] << 24);
}

static void write_u32_le(u8_t *p, u32_t v)
{
    p[0] = (u8_t)(v & 0xFFU);
    p[1] = (u8_t)((v >> 8) & 0xFFU);
    p[2] = (u8_t)((v >> 16) & 0xFFU);
    p[3] = (u8_t)((v >> 24) & 0xFFU);
}

static u32_t align_up(u32_t val, u32_t al)
{
    if (al <= 1U) return val;
    return (val + al - 1U) & ~(al - 1U);
}

static const struct elf32_shdr *obj_shdr(const struct obj_info *o, u32_t idx)
{
    return (const struct elf32_shdr *)(const void *)&obj_pool[o->file_off + o->shoff + idx * (u32_t)sizeof(struct elf32_shdr)];
}

static int find_gsym(const char *name)
{
    int i;
    for (i = 0; i < num_gsyms; i++) {
        if (u_strcmp(gsyms[i].name, name) == 0) return i;
    }
    return -1;
}

static int add_gsym(const char *name, u32_t vaddr, int is_abs, int is_weak)
{
    int idx = find_gsym(name);
    if (idx >= 0) {
        if (gsyms[idx].defined && !gsyms[idx].is_weak && !is_weak) {
            ld_puts("ld: duplicate symbol: ");
            ld_puts(name);
            ld_puts("\n");
            ld_errors++;
            return idx;
        }
        if (!gsyms[idx].defined || gsyms[idx].is_weak) {
            gsyms[idx].vaddr   = vaddr;
            gsyms[idx].is_abs  = is_abs;
            gsyms[idx].is_weak = is_weak;
            gsyms[idx].defined = 1;
        }
        return idx;
    }
    if (num_gsyms >= MAX_GSYMS) {
        ld_puts("ld: global symbol table full\n");
        ld_errors++;
        return -1;
    }
    idx = num_gsyms++;
    {
        u32_t len = u_strlen(name);
        if (len >= MAX_SYM_NAME) len = MAX_SYM_NAME - 1U;
        u_memcpy(gsyms[idx].name, name, len);
        gsyms[idx].name[len] = '\0';
    }
    gsyms[idx].vaddr   = vaddr;
    gsyms[idx].is_abs  = is_abs;
    gsyms[idx].is_weak = is_weak;
    gsyms[idx].defined = 1;
    return idx;
}

static int load_object(const char *path)
{
    int fd;
    u32_t start = align_up(obj_pool_used, 4U);
    u32_t sz = 0U;
    struct obj_info *o;
    struct elf32_ehdr eh;
    u16_t i;

    if (num_objs >= MAX_OBJS) {
        ld_puts("ld: too many input objects\n");
        return -1;
    }

    fd = sys_open(path, O_RDONLY, 0U);
    if (fd < 0) {
        ld_puts("ld: cannot open input file: ");
        ld_puts(path);
        ld_puts("\n");
        return -1;
    }

    while (start + sz < (u32_t)sizeof(obj_pool)) {
        s32_t n = sys_read(fd, &obj_pool[start + sz], (u32_t)sizeof(obj_pool) - (start + sz));
        if (n <= 0) break;
        sz += (u32_t)n;
    }
    (void)sys_close(fd);

    if (sz < (u32_t)sizeof(struct elf32_ehdr)) {
        ld_puts("ld: truncated ELF file: ");
        ld_puts(path);
        ld_puts("\n");
        return -1;
    }

    u_memcpy(&eh, &obj_pool[start], (u32_t)sizeof(eh));
    if (eh.e_ident[0] != 0x7FU || eh.e_ident[1] != (u8_t)'E' ||
        eh.e_ident[2] != (u8_t)'L' || eh.e_ident[3] != (u8_t)'F' ||
        eh.e_ident[4] != 1U || eh.e_type != ET_REL || eh.e_machine != EM_386) {
        ld_puts("ld: not an ELF32 i386 relocatable object: ");
        ld_puts(path);
        ld_puts("\n");
        return -1;
    }
    if (eh.e_shnum > MAX_SECS_PER || eh.e_shoff + (u32_t)eh.e_shnum * (u32_t)sizeof(struct elf32_shdr) > sz) {
        ld_puts("ld: invalid section headers in: ");
        ld_puts(path);
        ld_puts("\n");
        return -1;
    }

    o = &objs[num_objs++];
    u_memset(o, 0, (u32_t)sizeof(*o));
    o->path       = path;
    o->file_off   = start;
    o->file_size  = sz;
    o->shnum      = eh.e_shnum;
    o->shoff      = eh.e_shoff;
    o->symtab_sec = -1;
    o->strtab_sec = -1;
    obj_pool_used = start + sz;

    for (i = 0U; i < o->shnum; i++) {
        const struct elf32_shdr *sh = obj_shdr(o, i);
        o->sec_vaddr[i] = 0xFFFFFFFFU;
        o->sec_kind[i]  = 0;
        if (sh->sh_type == SHT_SYMTAB) {
            o->symtab_sec = (int)i;
            o->strtab_sec = (int)sh->sh_link;
        } else if ((sh->sh_flags & SHF_ALLOC) != 0U) {
            if (sh->sh_type == SHT_NOBITS) {
                o->sec_kind[i] = 4; /* .bss */
            } else if ((sh->sh_flags & SHF_EXECINSTR) != 0U) {
                o->sec_kind[i] = 1; /* .text */
            } else if ((sh->sh_flags & SHF_WRITE) != 0U) {
                o->sec_kind[i] = 3; /* .data */
            } else {
                o->sec_kind[i] = 2; /* .rodata */
            }
        }
    }
    return 0;
}

/* Count total R_386_32 + R_386_GOT32 relocations across all input objects */
static void count_relocs(u32_t *out_r32, u32_t *out_got)
{
    int oi;
    u32_t c32 = 0U;
    u32_t cgot = 0U;
    for (oi = 0; oi < num_objs; oi++) {
        const struct obj_info *o = &objs[oi];
        u16_t si;
        for (si = 0U; si < o->shnum; si++) {
            const struct elf32_shdr *sh = obj_shdr(o, si);
            if (sh->sh_type == SHT_REL && sh->sh_info < o->shnum && o->sec_kind[sh->sh_info] != 0) {
                u32_t nrel = sh->sh_size / (u32_t)sizeof(struct elf32_rel);
                u32_t ri;
                const struct elf32_rel *rt = (const struct elf32_rel *)(const void *)&obj_pool[o->file_off + sh->sh_offset];
                for (ri = 0U; ri < nrel; ri++) {
                    u32_t rt_type = rt[ri].r_info & 0xFFU;
                    if (rt_type == R_386_32) {
                        c32++;
                    } else if (rt_type == R_386_GOT32 || rt_type == R_386_GOT32X) {
                        cgot++;
                    }
                }
            }
        }
    }
    *out_r32 = c32;
    *out_got = cgot;
}

static u32_t got_base_vaddr = 0U;

static int resolve_sym_addr(const struct obj_info *o, u32_t sym_idx, u32_t *out_addr, int *out_abs)
{
    const struct elf32_shdr *st_sh;
    const struct elf32_shdr *str_sh;
    const struct elf32_sym *syms;
    const char *strs;
    u32_t nsyms;
    const struct elf32_sym *s;
    u8_t bind;

    if (o->symtab_sec < 0 || o->strtab_sec < 0) {
        return -1;
    }
    st_sh  = obj_shdr(o, (u32_t)o->symtab_sec);
    str_sh = obj_shdr(o, (u32_t)o->strtab_sec);
    syms   = (const struct elf32_sym *)(const void *)&obj_pool[o->file_off + st_sh->sh_offset];
    strs   = (const char *)&obj_pool[o->file_off + str_sh->sh_offset];
    nsyms  = st_sh->sh_size / (u32_t)sizeof(struct elf32_sym);

    if (sym_idx >= nsyms) {
        return -1;
    }
    s = &syms[sym_idx];
    bind = (u8_t)(s->st_info >> 4);

    if (s->st_shndx == SHN_ABS) {
        *out_addr = s->st_value;
        *out_abs  = 1;
        return 0;
    }
    if (bind == STB_LOCAL) {
        if (s->st_shndx < o->shnum && o->sec_vaddr[s->st_shndx] != 0xFFFFFFFFU) {
            *out_addr = o->sec_vaddr[s->st_shndx] + s->st_value;
            *out_abs  = 0;
            return 0;
        }
        return -1;
    }

    /* Global / weak / undefined -> look up in merged global symbol table */
    {
        const char *name = &strs[s->st_name];
        int g;
        if (u_strcmp(name, "_GLOBAL_OFFSET_TABLE_") == 0) {
            *out_addr = got_base_vaddr;
            *out_abs  = 0;
            return 0;
        }
        g = find_gsym(name);
        if (g >= 0 && gsyms[g].defined) {
            *out_addr = gsyms[g].vaddr;
            *out_abs  = gsyms[g].is_abs;
            return 0;
        }
        ld_puts("ld: undefined reference to '");
        ld_puts(name);
        ld_puts("'\n");
        ld_errors++;
        return -1;
    }
}

/* Emit 41-byte PIC self-relocation stub at stub_off:
 *   pushal
 *   call +0; pop %ebx; sub $(stub_off+6), %ebx
 *   lea table_off(%ebx), %esi
 *   mov $n_rel32, %ecx
 * .loop:
 *   mov (%esi), %edx
 *   add %ebx, (%ebx,%edx,1)
 *   add $4, %esi
 *   dec %ecx
 *   jnz .loop
 *   popal
 *   jmp real_entry
 */
static void emit_reloc_stub(u32_t stub_off, u32_t table_off, u32_t n_rel, u32_t real_entry)
{
    u8_t *p = &out_img[stub_off];
    u32_t next_ip = stub_off + 6U; /* address right after call +0 (1+5=6) */
    s32_t jmp_rel;

    p[0] = 0x60U;                               /* pushal */
    p[1] = 0xE8U; write_u32_le(&p[2], 0U);      /* call +0 */
    p[6] = 0x5BU;                               /* popl %ebx */
    p[7] = 0x81U; p[8] = 0xEBU;                 /* subl $next_ip, %ebx */
    write_u32_le(&p[9], next_ip);
    p[13] = 0x8DU; p[14] = 0xB3U;               /* leal table_off(%ebx), %esi */
    write_u32_le(&p[15], table_off);
    p[19] = 0xB9U;                              /* movl $n_rel, %ecx */
    write_u32_le(&p[20], n_rel);
    p[24] = 0x8BU; p[25] = 0x16U;               /* movl (%esi), %edx */
    p[26] = 0x01U; p[27] = 0x1CU; p[28] = 0x13U;/* addl %ebx, (%ebx,%edx,1) */
    p[29] = 0x83U; p[30] = 0xC6U; p[31] = 0x04U;/* addl $4, %esi */
    p[32] = 0x49U;                              /* decl %ecx */
    p[33] = 0x75U; p[34] = 0xF5U;               /* jnz -11 (back to p[24]) */
    p[35] = 0x61U;                              /* popal */
    p[36] = 0xE9U;                              /* jmp rel32 */
    jmp_rel = (s32_t)real_entry - (s32_t)(stub_off + 41U);
    write_u32_le(&p[37], (u32_t)jmp_rel);
}

/* Emit 25-byte automatic _start -> main(argc, argv) -> SYS_exit_group trampoline
 * when linking C object files directly without a separate crt0.o:
 *   movl 0(%esp), %eax       ; argc
 *   leal 4(%esp), %ecx       ; argv
 *   xorl %ebp, %ebp
 *   pushl %ecx
 *   pushl %eax
 *   call main
 *   movl %eax, %ebx
 *   movl $252, %eax          ; SYS_exit_group
 *   int $0x80
 */
static void emit_main_trampoline(u32_t tramp_off, u32_t main_vaddr)
{
    u8_t *p = &out_img[tramp_off];
    s32_t call_rel = (s32_t)main_vaddr - (s32_t)(tramp_off + 16U);

    p[0]  = 0x8BU; p[1]  = 0x04U; p[2]  = 0x24U;
    p[3]  = 0x8DU; p[4]  = 0x4CU; p[5]  = 0x24U; p[6]  = 0x04U;
    p[7]  = 0x31U; p[8]  = 0xEDU;
    p[9]  = 0x51U;
    p[10] = 0x50U;
    p[11] = 0xE8U; write_u32_le(&p[12], (u32_t)call_rel);
    p[16] = 0x89U; p[17] = 0xC3U;
    p[18] = 0xB8U; write_u32_le(&p[19], 252U);
    p[23] = 0xCDU; p[24] = 0x80U;
}

static int objects_define_sym(const char *target)
{
    int oi;
    for (oi = 0; oi < num_objs; oi++) {
        const struct obj_info *o = &objs[oi];
        const struct elf32_shdr *st_sh;
        const struct elf32_shdr *str_sh;
        const struct elf32_sym *syms;
        const char *strs;
        u32_t nsyms, si;
        if (o->symtab_sec < 0 || o->strtab_sec < 0) continue;
        st_sh  = obj_shdr(o, (u32_t)o->symtab_sec);
        str_sh = obj_shdr(o, (u32_t)o->strtab_sec);
        syms   = (const struct elf32_sym *)(const void *)&obj_pool[o->file_off + st_sh->sh_offset];
        strs   = (const char *)&obj_pool[o->file_off + str_sh->sh_offset];
        nsyms  = st_sh->sh_size / (u32_t)sizeof(struct elf32_sym);
        for (si = 1U; si < nsyms; si++) {
            u8_t bind = (u8_t)(syms[si].st_info >> 4);
            if ((bind == STB_GLOBAL || bind == STB_WEAK) && syms[si].st_shndx != SHN_UNDEF) {
                if (u_strcmp(&strs[syms[si].st_name], target) == 0) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *out_path = "a.out";
    const char *entry_name = "_start";
    int i, oi, kind;
    u32_t need_stub;
    u32_t use_main_tramp = 0U;
    u32_t main_tramp_off = 0U;
    u32_t max_r32 = 0U;
    u32_t max_got = 0U;
    u32_t got_cur = 0U;
    u32_t stub_off;
    u32_t cursor;
    u32_t filesz;
    u32_t memsz;
    u32_t rel_table_off = 0U;
    u32_t entry_vaddr;
    int entry_sym;
    struct elf32_ehdr eh;
    struct elf32_phdr ph;
    int fd;

    for (i = 1; i < argc; i++) {
        if (u_strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (u_strcmp(argv[i], "-e") == 0 && i + 1 < argc) {
            entry_name = argv[++i];
        } else if (u_strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            i++; /* accept -m elf_i386 */
        } else if (argv[i][0] == '-') {
            /* accept standard flags like -static, -pie, -nostdlib */
        } else {
            if (load_object(argv[i]) < 0) {
                return 1;
            }
        }
    }

    if (num_objs == 0) {
        ld_puts("usage: ld [-o out] [-e entry] <file1.o> [file2.o ...]\n");
        return 1;
    }

    u_memset(out_img, 0, (u32_t)sizeof(out_img));

    /* Reserve ELF header (52 B) + 1 Program Header (32 B) = 84 B, aligned to 16 B */
    stub_off = align_up((u32_t)sizeof(struct elf32_ehdr) + (u32_t)sizeof(struct elf32_phdr), 16U);
    count_relocs(&max_r32, &max_got);
    need_stub = (max_r32 + max_got > 0U) ? 1U : 0U;
    cursor = need_stub ? align_up(stub_off + 41U, 16U) : stub_off;

    /* If default entry "_start" is not defined but "main" is, synthesize a 25-byte CRT0 trampoline */
    if (u_strcmp(entry_name, "_start") == 0 &&
        !objects_define_sym("_start") && objects_define_sym("main")) {
        use_main_tramp = 1U;
        main_tramp_off = cursor;
        cursor = align_up(main_tramp_off + 25U, 16U);
    }

    /* 1. Assign virtual addresses & copy PROGBITS sections in order: .text, .rodata, .data */
    for (kind = 1; kind <= 3; kind++) {
        for (oi = 0; oi < num_objs; oi++) {
            struct obj_info *o = &objs[oi];
            u16_t si;
            for (si = 0U; si < o->shnum; si++) {
                if (o->sec_kind[si] == kind) {
                    const struct elf32_shdr *sh = obj_shdr(o, si);
                    u32_t al = (sh->sh_addralign > 0U) ? sh->sh_addralign : 4U;
                    if (al > 4096U) al = 4096U;
                    cursor = align_up(cursor, al);
                    if (cursor + sh->sh_size > MAX_OUT_BYTES) {
                        ld_puts("ld: output image too large\n");
                        return 1;
                    }
                    o->sec_vaddr[si] = cursor;
                    if (sh->sh_size > 0U) {
                        u_memcpy(&out_img[cursor], &obj_pool[o->file_off + sh->sh_offset], sh->sh_size);
                        cursor += sh->sh_size;
                    }
                }
            }
        }
    }

    /* Reserve synthetic .got table (for R_386_GOTPC/GOTOFF/GOT32) right after .data */
    cursor = align_up(cursor, 4U);
    got_base_vaddr = cursor;
    got_cur = cursor;
    cursor += max_got * 4U;
    if (cursor > MAX_OUT_BYTES) {
        ld_puts("ld: output image too large\n");
        return 1;
    }

    /* Reserve space for rel32 patch table right after .got (before .bss) */
    if (need_stub) {
        cursor = align_up(cursor, 4U);
        rel_table_off = cursor;
        cursor += (max_r32 + max_got) * 4U;
        if (cursor > MAX_OUT_BYTES) {
            ld_puts("ld: output image too large\n");
            return 1;
        }
    }
    filesz = cursor;

    /* 2. Assign .bss sections (NOBITS, zero-filled by loader) */
    for (oi = 0; oi < num_objs; oi++) {
        struct obj_info *o = &objs[oi];
        u16_t si;
        for (si = 0U; si < o->shnum; si++) {
            if (o->sec_kind[si] == 4) {
                const struct elf32_shdr *sh = obj_shdr(o, si);
                u32_t al = (sh->sh_addralign > 0U) ? sh->sh_addralign : 4U;
                if (al > 4096U) al = 4096U;
                cursor = align_up(cursor, al);
                o->sec_vaddr[si] = cursor;
                cursor += sh->sh_size;
            }
        }
    }

    /* 3. Collect all defined global/weak/common symbols */
    for (oi = 0; oi < num_objs; oi++) {
        const struct obj_info *o = &objs[oi];
        const struct elf32_shdr *st_sh;
        const struct elf32_shdr *str_sh;
        const struct elf32_sym *syms;
        const char *strs;
        u32_t nsyms, si;

        if (o->symtab_sec < 0 || o->strtab_sec < 0) continue;
        st_sh  = obj_shdr(o, (u32_t)o->symtab_sec);
        str_sh = obj_shdr(o, (u32_t)o->strtab_sec);
        syms   = (const struct elf32_sym *)(const void *)&obj_pool[o->file_off + st_sh->sh_offset];
        strs   = (const char *)&obj_pool[o->file_off + str_sh->sh_offset];
        nsyms  = st_sh->sh_size / (u32_t)sizeof(struct elf32_sym);

        for (si = 1U; si < nsyms; si++) {
            const struct elf32_sym *s = &syms[si];
            u8_t bind = (u8_t)(s->st_info >> 4);
            const char *name = &strs[s->st_name];
            if (bind != STB_GLOBAL && bind != STB_WEAK) continue;
            if (s->st_shndx == SHN_UNDEF) continue;
            if (s->st_shndx == SHN_ABS) {
                (void)add_gsym(name, s->st_value, 1, (bind == STB_WEAK) ? 1 : 0);
            } else if (s->st_shndx == SHN_COMMON) {
                u32_t al = (s->st_value > 0U) ? s->st_value : 4U;
                if (find_gsym(name) < 0) {
                    cursor = align_up(cursor, al);
                    (void)add_gsym(name, cursor, 0, 1);
                    cursor += s->st_size;
                }
            } else if (s->st_shndx < o->shnum && o->sec_vaddr[s->st_shndx] != 0xFFFFFFFFU) {
                (void)add_gsym(name, o->sec_vaddr[s->st_shndx] + s->st_value, 0,
                               (bind == STB_WEAK) ? 1 : 0);
            }
        }
    }
    memsz = cursor;

    /* 4. Apply relocations across all input objects */
    num_rel32 = 0U;
    for (oi = 0; oi < num_objs; oi++) {
        const struct obj_info *o = &objs[oi];
        u16_t si;
        for (si = 0U; si < o->shnum; si++) {
            const struct elf32_shdr *sh = obj_shdr(o, si);
            u32_t tsec;
            u32_t nrel, ri;
            const struct elf32_rel *rt;

            if (sh->sh_type != SHT_REL) continue;
            tsec = sh->sh_info;
            if (tsec >= o->shnum || o->sec_vaddr[tsec] == 0xFFFFFFFFU || o->sec_kind[tsec] == 4) continue;

            nrel = sh->sh_size / (u32_t)sizeof(struct elf32_rel);
            rt = (const struct elf32_rel *)(const void *)&obj_pool[o->file_off + sh->sh_offset];

            for (ri = 0U; ri < nrel; ri++) {
                u32_t r_type = rt[ri].r_info & 0xFFU;
                u32_t r_sym  = rt[ri].r_info >> 8;
                u32_t patch_va = o->sec_vaddr[tsec] + rt[ri].r_offset;
                u32_t sym_va = 0U;
                int   sym_abs = 0;
                u32_t addend;

                if (r_type == R_386_NONE) continue;
                if (patch_va + 4U > filesz) {
                    ld_puts("ld: relocation offset out of bounds\n");
                    ld_errors++;
                    continue;
                }
                if (resolve_sym_addr(o, r_sym, &sym_va, &sym_abs) < 0) {
                    continue;
                }
                addend = read_u32_le(&out_img[patch_va]);

                if (r_type == R_386_32) {
                    write_u32_le(&out_img[patch_va], sym_va + addend);
                    if (!sym_abs) {
                        if (num_rel32 < MAX_REL32) {
                            rel32_offs[num_rel32++] = patch_va;
                        } else {
                            ld_puts("ld: too many R_386_32 relocations\n");
                            ld_errors++;
                        }
                    }
                } else if (r_type == R_386_PC32 || r_type == R_386_PLT32) {
                    write_u32_le(&out_img[patch_va], sym_va + addend - patch_va);
                } else if (r_type == R_386_GOTPC) {
                    write_u32_le(&out_img[patch_va], got_base_vaddr + addend - patch_va);
                } else if (r_type == R_386_GOTOFF) {
                    write_u32_le(&out_img[patch_va], sym_va + addend - got_base_vaddr);
                } else if (r_type == R_386_GOT32 || r_type == R_386_GOT32X) {
                    u32_t slot_va = got_cur;
                    got_cur += 4U;
                    write_u32_le(&out_img[slot_va], sym_va);
                    if (!sym_abs) {
                        if (num_rel32 < MAX_REL32) {
                            rel32_offs[num_rel32++] = slot_va;
                        } else {
                            ld_puts("ld: too many GOT relocations\n");
                            ld_errors++;
                        }
                    }
                    write_u32_le(&out_img[patch_va], slot_va + addend - got_base_vaddr);
                } else {
                    ld_puts("ld: unsupported relocation type\n");
                    ld_errors++;
                }
            }
        }
    }

    /* 5. Locate entry point */
    entry_sym = find_gsym(entry_name);
    if (entry_sym < 0 && u_strcmp(entry_name, "_start") == 0) {
        entry_sym = find_gsym("main");
    }
    if (entry_sym < 0 || !gsyms[entry_sym].defined) {
        ld_puts("ld: entry symbol not found: ");
        ld_puts(entry_name);
        ld_puts("\n");
        ld_errors++;
    }

    if (ld_errors > 0) {
        return 1;
    }

    entry_vaddr = gsyms[entry_sym].vaddr;
    if (use_main_tramp) {
        emit_main_trampoline(main_tramp_off, entry_vaddr);
        entry_vaddr = main_tramp_off;
    }
    if (need_stub && num_rel32 > 0U) {
        u32_t r;
        for (r = 0U; r < num_rel32; r++) {
            write_u32_le(&out_img[rel_table_off + r * 4U], rel32_offs[r]);
        }
        emit_reloc_stub(stub_off, rel_table_off, num_rel32, entry_vaddr);
        entry_vaddr = stub_off;
    }

    /* 6. Write ELF32 header & single PT_LOAD segment */
    u_memset(&eh, 0, (u32_t)sizeof(eh));
    eh.e_ident[0]  = 0x7FU;
    eh.e_ident[1]  = (u8_t)'E';
    eh.e_ident[2]  = (u8_t)'L';
    eh.e_ident[3]  = (u8_t)'F';
    eh.e_ident[4]  = 1U; /* ELFCLASS32 */
    eh.e_ident[5]  = 1U; /* ELFDATA2LSB */
    eh.e_ident[6]  = 1U; /* EV_CURRENT */
    eh.e_type      = ET_DYN;
    eh.e_machine   = EM_386;
    eh.e_version   = 1U;
    eh.e_entry     = entry_vaddr;
    eh.e_phoff     = (u32_t)sizeof(struct elf32_ehdr);
    eh.e_ehsize    = (u16_t)sizeof(struct elf32_ehdr);
    eh.e_phentsize = (u16_t)sizeof(struct elf32_phdr);
    eh.e_phnum     = 1U;

    u_memset(&ph, 0, (u32_t)sizeof(ph));
    ph.p_type   = PT_LOAD;
    ph.p_offset = 0U;
    ph.p_vaddr  = 0U;
    ph.p_paddr  = 0U;
    ph.p_filesz = filesz;
    ph.p_memsz  = memsz;
    ph.p_flags  = 7U; /* PF_R | PF_W | PF_X */
    ph.p_align  = 4096U;

    u_memcpy(&out_img[0], &eh, (u32_t)sizeof(eh));
    u_memcpy(&out_img[sizeof(struct elf32_ehdr)], &ph, (u32_t)sizeof(ph));

    fd = sys_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0755U);
    if (fd < 0) {
        ld_puts("ld: cannot create output file: ");
        ld_puts(out_path);
        ld_puts("\n");
        return 1;
    }
    if (sys_write(fd, out_img, filesz) != (s32_t)filesz) {
        (void)sys_close(fd);
        ld_puts("ld: write failed\n");
        return 1;
    }
    (void)sys_close(fd);
    return 0;
}
