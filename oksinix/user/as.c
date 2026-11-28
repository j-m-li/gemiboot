/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 x86 32-bit AT&T Assembler (/mnt/AS.ELF)
 *
 * Emits standard ELF32 relocatable object files (ET_REL, EM_386) with
 * .text, .rodata, .data, .bss, .rel.*, .symtab, .strtab, and .shstrtab.
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

/* ELF32 constants */
#define ET_REL       1U
#define EM_386       3U
#define SHT_NULL     0U
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
#define STT_NOTYPE  0U
#define STT_SECTION 3U

#define SHN_UNDEF   0U
#define SHN_ABS     0xFFF1U

#define R_386_32     1U
#define R_386_PC32   2U
#define R_386_GOT32  3U
#define R_386_PLT32  4U
#define R_386_GOTOFF 9U
#define R_386_GOTPC  10U

#define REL_MOD_NONE   0
#define REL_MOD_PLT    1
#define REL_MOD_GOTOFF 2
#define REL_MOD_GOT    3
#define REL_MOD_GOTPC  4

/* Section indices inside assembler */
#define SEC_TEXT    0
#define SEC_RODATA  1
#define SEC_DATA    2
#define SEC_BSS     3
#define SEC_COUNT   4

/* Buffer limits (kept well within Ring-3 3.7 MiB static-PIE window) */
#define MAX_SRC_SIZE   262144U
#define MAX_SEC_SIZE   98304U
#define MAX_SYMS       2048
#define MAX_SYM_NAME   48
#define MAX_RELS       2048
#define MAX_NUM_LABELS 512
#define MAX_OUT_SIZE   262144U

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

struct as_sym {
    char  name[MAX_SYM_NAME];
    int   sec;         /* -1 = undef, -2 = abs, 0..3 = SEC_* */
    u32_t value;
    int   is_global;
    int   defined;
    u32_t elf_idx;
};

struct as_rel {
    int   sec;
    u32_t offset;
    int   sym_idx;
    u32_t type;        /* R_386_32 or R_386_PC32 */
};

struct num_label {
    int   num;
    int   sec;
    u32_t offset;
    int   sym_idx;
};

#define OP_NONE 0
#define OP_REG  1
#define OP_IMM  2
#define OP_MEM  3

struct operand {
    int   kind;
    int   reg;         /* 0..7 for OP_REG */
    int   reg_size;    /* 1, 2, or 4 */
    int   base;        /* -1 if none, 0..7 */
    int   index;       /* -1 if none, 0..7 */
    int   scale;       /* 1, 2, 4, 8 */
    s32_t disp;        /* immediate or displacement */
    int   sym_idx;     /* -1 if no symbol reference */
    int   rel_mod;     /* REL_MOD_* */
    int   is_indirect; /* '*' prefix for indirect jmp/call */
};

int main(int argc, char **argv);

static char             src_buf[MAX_SRC_SIZE + 1U];
static u32_t            src_len = 0U;
static u8_t             sec_data[3][MAX_SEC_SIZE];
static u32_t            sec_len[SEC_COUNT];
static int              cur_sec = SEC_TEXT;

static struct as_sym    syms[MAX_SYMS];
static int              num_syms = 0;

static struct as_rel    rels[MAX_RELS];
static int              num_rels = 0;

static struct num_label num_labels[MAX_NUM_LABELS];
static int              num_label_cnt = 0;
static int              cur_pass = 1;
static int              cur_line = 1;
static u32_t            cur_insn_off = 0U;
static int              in_ignored_sec = 0;
static int              as_errors = 0;

static u8_t             out_buf[MAX_OUT_SIZE];
static char             strtab_buf[65536];
static u32_t            strtab_len = 0U;
static struct elf32_sym elf_syms[MAX_SYMS + 8];
static struct elf32_rel sec_rels[3][MAX_RELS];
static u32_t            sec_rel_cnt[3];

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
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static int u_strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(u8_t)*a - (int)(u8_t)*b;
}

static void u_memcpy(void *dst, const void *src, u32_t n)
{
    u8_t *d = (u8_t *)dst;
    const u8_t *s = (const u8_t *)src;
    while (n > 0U) {
        *d++ = *s++;
        n--;
    }
}

static void u_memset(void *dst, int val, u32_t n)
{
    u8_t *d = (u8_t *)dst;
    while (n > 0U) {
        *d++ = (u8_t)val;
        n--;
    }
}

static void as_puts(const char *s)
{
    (void)sys_write(2, s, u_strlen(s));
}

static void as_put_uint(u32_t v)
{
    char buf[16];
    int i = 0;
    if (v == 0U) {
        (void)sys_write(2, "0", 1U);
        return;
    }
    while (v > 0U) {
        buf[i++] = (char)('0' + (v % 10U));
        v /= 10U;
    }
    while (i > 0) {
        i--;
        (void)sys_write(2, &buf[i], 1U);
    }
}

static void as_err(const char *msg)
{
    as_puts("as: line ");
    as_put_uint((u32_t)cur_line);
    as_puts(": ");
    as_puts(msg);
    as_puts("\n");
    as_errors++;
}

static int is_space(char c)
{
    return (c == ' ' || c == '\t' || c == '\r');
}

static int is_digit(char c)
{
    return (c >= '0' && c <= '9');
}

static int is_ident_start(char c)
{
    return ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            c == '_' || c == '.');
}

static int is_ident_char(char c)
{
    return (is_ident_start(c) || is_digit(c) || c == '$');
}

static char to_lower(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return (char)(c + ('a' - 'A'));
    }
    return c;
}

static int u_strcasecmp(const char *a, const char *b)
{
    while (*a != '\0' && to_lower(*a) == to_lower(*b)) {
        a++;
        b++;
    }
    return (int)(u8_t)to_lower(*a) - (int)(u8_t)to_lower(*b);
}

static int find_or_add_sym(const char *name)
{
    int i;
    u32_t len;
    for (i = 0; i < num_syms; i++) {
        if (u_strcmp(syms[i].name, name) == 0) {
            return i;
        }
    }
    if (num_syms >= MAX_SYMS) {
        as_err("symbol table full");
        return -1;
    }
    i = num_syms++;
    u_memset(&syms[i], 0, (u32_t)sizeof(syms[i]));
    len = u_strlen(name);
    if (len >= MAX_SYM_NAME) {
        len = MAX_SYM_NAME - 1U;
    }
    u_memcpy(syms[i].name, name, len);
    syms[i].name[len] = '\0';
    syms[i].sec = -1;
    return i;
}

static void emit_u8(u8_t b)
{
    if (in_ignored_sec) {
        return;
    }
    if (cur_sec == SEC_BSS) {
        sec_len[SEC_BSS]++;
        return;
    }
    if (sec_len[cur_sec] >= MAX_SEC_SIZE) {
        if (cur_pass == 2) {
            as_err("section buffer overflow");
        }
        return;
    }
    if (cur_pass == 2) {
        sec_data[cur_sec][sec_len[cur_sec]] = b;
    }
    sec_len[cur_sec]++;
}

static void emit_u16(u16_t v)
{
    emit_u8((u8_t)(v & 0xFFU));
    emit_u8((u8_t)((v >> 8) & 0xFFU));
}

static void emit_u32(u32_t v)
{
    emit_u8((u8_t)(v & 0xFFU));
    emit_u8((u8_t)((v >> 8) & 0xFFU));
    emit_u8((u8_t)((v >> 16) & 0xFFU));
    emit_u8((u8_t)((v >> 24) & 0xFFU));
}

static void add_reloc(int sym_idx, u32_t type)
{
    if (cur_pass != 2 || cur_sec == SEC_BSS || sym_idx < 0) {
        return;
    }
    if (num_rels >= MAX_RELS) {
        as_err("relocation table full");
        return;
    }
    rels[num_rels].sec     = cur_sec;
    rels[num_rels].offset  = sec_len[cur_sec];
    rels[num_rels].sym_idx = sym_idx;
    rels[num_rels].type    = type;
    num_rels++;
}

static void emit_rel32_sym_mod(int sym_idx, s32_t addend, int rel_mod)
{
    if (sym_idx < 0) {
        emit_u32((u32_t)addend);
        return;
    }
    if (cur_pass == 2) {
        if (rel_mod == REL_MOD_GOTPC) {
            s32_t got_add = addend + (s32_t)(sec_len[cur_sec] - cur_insn_off);
            add_reloc(sym_idx, R_386_GOTPC);
            emit_u32((u32_t)got_add);
            return;
        }
        if (rel_mod == REL_MOD_GOTOFF) {
            add_reloc(sym_idx, R_386_GOTOFF);
            emit_u32((u32_t)addend);
            return;
        }
        if (rel_mod == REL_MOD_GOT) {
            add_reloc(sym_idx, R_386_GOT32);
            emit_u32((u32_t)addend);
            return;
        }
        if (syms[sym_idx].defined && syms[sym_idx].sec == -2) {
            emit_u32((u32_t)((s32_t)syms[sym_idx].value + addend));
            return;
        }
        add_reloc(sym_idx, R_386_32);
    }
    emit_u32((u32_t)addend);
}

static void emit_rel32_sym(int sym_idx, s32_t addend)
{
    emit_rel32_sym_mod(sym_idx, addend, REL_MOD_NONE);
}

static void emit_pc32_sym_mod(int sym_idx, s32_t addend, int rel_mod)
{
    if (sym_idx < 0) {
        emit_u32((u32_t)(addend - 4));
        return;
    }
    if (cur_pass == 2) {
        if (rel_mod == REL_MOD_PLT) {
            add_reloc(sym_idx, R_386_PLT32);
            emit_u32((u32_t)(addend - 4));
            return;
        }
        if (syms[sym_idx].defined && syms[sym_idx].sec == cur_sec && !syms[sym_idx].is_global) {
            s32_t target = (s32_t)syms[sym_idx].value + addend;
            s32_t next_ip = (s32_t)(sec_len[cur_sec] + 4U);
            emit_u32((u32_t)(target - next_ip));
            return;
        }
        add_reloc(sym_idx, R_386_PC32);
    }
    emit_u32((u32_t)(addend - 4));
}

static void skip_ws(const char **pp)
{
    while (is_space(**pp)) {
        (*pp)++;
    }
}

static int parse_reg(const char **pp, int *out_reg, int *out_size)
{
    const char *p = *pp;
    char r[8];
    int len = 0;

    if (*p != '%') {
        return 0;
    }
    p++;
    while (len < 7 && ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || is_digit(*p))) {
        r[len++] = to_lower(*p++);
    }
    r[len] = '\0';

    if (u_strcmp(r, "eax") == 0) { *out_reg = 0; *out_size = 4; }
    else if (u_strcmp(r, "ecx") == 0) { *out_reg = 1; *out_size = 4; }
    else if (u_strcmp(r, "edx") == 0) { *out_reg = 2; *out_size = 4; }
    else if (u_strcmp(r, "ebx") == 0) { *out_reg = 3; *out_size = 4; }
    else if (u_strcmp(r, "esp") == 0) { *out_reg = 4; *out_size = 4; }
    else if (u_strcmp(r, "ebp") == 0) { *out_reg = 5; *out_size = 4; }
    else if (u_strcmp(r, "esi") == 0) { *out_reg = 6; *out_size = 4; }
    else if (u_strcmp(r, "edi") == 0) { *out_reg = 7; *out_size = 4; }
    else if (u_strcmp(r, "ax") == 0)  { *out_reg = 0; *out_size = 2; }
    else if (u_strcmp(r, "cx") == 0)  { *out_reg = 1; *out_size = 2; }
    else if (u_strcmp(r, "dx") == 0)  { *out_reg = 2; *out_size = 2; }
    else if (u_strcmp(r, "bx") == 0)  { *out_reg = 3; *out_size = 2; }
    else if (u_strcmp(r, "sp") == 0)  { *out_reg = 4; *out_size = 2; }
    else if (u_strcmp(r, "bp") == 0)  { *out_reg = 5; *out_size = 2; }
    else if (u_strcmp(r, "si") == 0)  { *out_reg = 6; *out_size = 2; }
    else if (u_strcmp(r, "di") == 0)  { *out_reg = 7; *out_size = 2; }
    else if (u_strcmp(r, "al") == 0)  { *out_reg = 0; *out_size = 1; }
    else if (u_strcmp(r, "cl") == 0)  { *out_reg = 1; *out_size = 1; }
    else if (u_strcmp(r, "dl") == 0)  { *out_reg = 2; *out_size = 1; }
    else if (u_strcmp(r, "bl") == 0)  { *out_reg = 3; *out_size = 1; }
    else if (u_strcmp(r, "ah") == 0)  { *out_reg = 4; *out_size = 1; }
    else if (u_strcmp(r, "ch") == 0)  { *out_reg = 5; *out_size = 1; }
    else if (u_strcmp(r, "dh") == 0)  { *out_reg = 6; *out_size = 1; }
    else if (u_strcmp(r, "bh") == 0)  { *out_reg = 7; *out_size = 1; }
    else {
        return 0;
    }
    *pp = p;
    return 1;
}

static int parse_number(const char **pp, s32_t *out_val)
{
    const char *p = *pp;
    u32_t v = 0U;

    if (p[0] == '\'') {
        p++;
        if (*p == '\\') {
            p++;
            if (*p == 'n') v = 10U;
            else if (*p == 'r') v = 13U;
            else if (*p == 't') v = 9U;
            else if (*p == '0') v = 0U;
            else v = (u8_t)*p;
            if (*p != '\0') p++;
        } else if (*p != '\0') {
            v = (u8_t)*p++;
        }
        if (*p == '\'') p++;
        *out_val = (s32_t)v;
        *pp = p;
        return 1;
    }

    if (!is_digit(*p)) {
        return 0;
    }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            u32_t d;
            if (*p >= '0' && *p <= '9') d = (u32_t)(*p - '0');
            else if (*p >= 'a' && *p <= 'f') d = (u32_t)(*p - 'a' + 10);
            else d = (u32_t)(*p - 'A' + 10);
            v = (v << 4) | d;
            p++;
        }
    } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
        p += 2;
        while (*p == '0' || *p == '1') {
            v = (v << 1) | (u32_t)(*p - '0');
            p++;
        }
    } else {
        while (is_digit(*p)) {
            v = v * 10U + (u32_t)(*p - '0');
            p++;
        }
    }
    *out_val = (s32_t)v;
    *pp = p;
    return 1;
}

static int resolve_num_label(int num, int is_forward)
{
    int i;
    int best = -1;
    u32_t cur_off = sec_len[cur_sec];

    if (cur_pass == 1) {
        return -1;
    }
    if (is_forward) {
        for (i = 0; i < num_label_cnt; i++) {
            if (num_labels[i].num == num && num_labels[i].sec == cur_sec &&
                num_labels[i].offset > cur_off) {
                best = num_labels[i].sym_idx;
                break;
            }
        }
    } else {
        for (i = 0; i < num_label_cnt; i++) {
            if (num_labels[i].num == num && num_labels[i].sec == cur_sec &&
                num_labels[i].offset <= cur_off) {
                best = num_labels[i].sym_idx;
            }
        }
    }
    if (best < 0 && cur_pass == 2) {
        as_err("unresolved numeric local label");
    }
    return best;
}

/* Parse an expression: [+-] term ([+-] term)* with optional single symbol, [...] grouping, and @MOD */
static int parse_expr_mod(const char **pp, s32_t *out_val, int *out_sym, int *out_mod)
{
    const char *p = *pp;
    s32_t total = 0;
    int sym_idx = -1;
    int neg_sym = -1;
    int rel_mod = REL_MOD_NONE;
    int got_any = 0;
    int sign = 1;
    int grp_sign = 1;
    int in_bracket = 0;

    skip_ws(&p);
    for (;;) {
        int eff_sign;
        skip_ws(&p);
        if (*p == ']' && in_bracket) {
            in_bracket = 0;
            grp_sign = 1;
            p++;
            continue;
        }
        if (*p == '+') {
            sign = 1;
            p++;
            skip_ws(&p);
        } else if (*p == '-') {
            sign = -1;
            p++;
            skip_ws(&p);
        } else if (got_any) {
            break;
        }

        if (*p == '[' && !in_bracket) {
            in_bracket = 1;
            grp_sign = sign;
            sign = 1;
            p++;
            skip_ws(&p);
            if (*p == '+') {
                sign = 1;
                p++;
                skip_ws(&p);
            } else if (*p == '-') {
                sign = -1;
                p++;
                skip_ws(&p);
            }
        }

        eff_sign = sign * grp_sign;

        if (is_digit(*p) && (p[1] == 'f' || p[1] == 'b' || p[1] == 'F' || p[1] == 'B') &&
            !is_ident_char(p[2])) {
            int n = *p - '0';
            int fwd = (p[1] == 'f' || p[1] == 'F') ? 1 : 0;
            p += 2;
            sym_idx = resolve_num_label(n, fwd);
            got_any = 1;
        } else if (is_digit(*p) || *p == '\'') {
            s32_t num = 0;
            if (!parse_number(&p, &num)) {
                break;
            }
            total += eff_sign * num;
            got_any = 1;
        } else if (*p == '.' && !is_ident_char(p[1])) {
            p++;
            total += eff_sign * (s32_t)cur_insn_off;
            got_any = 1;
        } else if (is_ident_start(*p)) {
            char name[MAX_SYM_NAME];
            int len = 0;
            int idx;
            while (is_ident_char(*p)) {
                if (len < MAX_SYM_NAME - 1) {
                    name[len++] = *p;
                }
                p++;
            }
            name[len] = '\0';
            if (*p == '@') {
                char mod[16];
                int mlen = 0;
                p++;
                while (is_ident_char(*p)) {
                    if (mlen < 15) mod[mlen++] = *p;
                    p++;
                }
                mod[mlen] = '\0';
                if (u_strcasecmp(mod, "PLT") == 0) rel_mod = REL_MOD_PLT;
                else if (u_strcasecmp(mod, "GOTOFF") == 0) rel_mod = REL_MOD_GOTOFF;
                else if (u_strcasecmp(mod, "GOT") == 0) rel_mod = REL_MOD_GOT;
                else if (u_strcasecmp(mod, "GOTPC") == 0) rel_mod = REL_MOD_GOTPC;
            }
            idx = find_or_add_sym(name);
            if (u_strcmp(name, "_GLOBAL_OFFSET_TABLE_") == 0) {
                rel_mod = REL_MOD_GOTPC;
                sym_idx = idx;
            } else if (idx >= 0 && syms[idx].defined && syms[idx].sec == -2) {
                total += eff_sign * (s32_t)syms[idx].value;
            } else if (eff_sign < 0 && idx >= 0 && syms[idx].defined && syms[idx].sec == cur_sec) {
                /* Fold subtraction of a local label in the current section (e.g. [.-.L_pic_1]) */
                total -= (s32_t)syms[idx].value;
            } else if (eff_sign > 0 && sym_idx < 0) {
                sym_idx = idx;
            } else if (eff_sign < 0 && neg_sym < 0) {
                neg_sym = idx;
            }
            got_any = 1;
        } else {
            break;
        }
        sign = 1;
    }

    /* Fold (symA - symB) when both are defined in the same section */
    if (sym_idx >= 0 && neg_sym >= 0) {
        if (syms[sym_idx].defined && syms[neg_sym].defined &&
            syms[sym_idx].sec == syms[neg_sym].sec) {
            total += (s32_t)syms[sym_idx].value - (s32_t)syms[neg_sym].value;
            sym_idx = -1;
            neg_sym = -1;
        } else if (cur_pass == 1) {
            sym_idx = -1;
            neg_sym = -1;
        } else {
            as_err("cannot subtract symbols from different sections");
        }
    }

    if (!got_any) {
        return 0;
    }
    *out_val = total;
    *out_sym = sym_idx;
    if (out_mod != (int *)0) {
        *out_mod = rel_mod;
    }
    *pp = p;
    return 1;
}

static int parse_expr(const char **pp, s32_t *out_val, int *out_sym)
{
    return parse_expr_mod(pp, out_val, out_sym, (int *)0);
}

static int parse_operand(const char **pp, struct operand *op)
{
    const char *p = *pp;

    u_memset(op, 0, (u32_t)sizeof(*op));
    op->kind    = OP_NONE;
    op->base    = -1;
    op->index   = -1;
    op->scale   = 1;
    op->sym_idx = -1;

    skip_ws(&p);
    if (*p == '\0' || *p == '\n' || *p == ';' || *p == '#') {
        return 0;
    }

    if (*p == '*') {
        op->is_indirect = 1;
        p++;
        skip_ws(&p);
    }

    if (*p == '$') {
        p++;
        if (!parse_expr_mod(&p, &op->disp, &op->sym_idx, &op->rel_mod)) {
            as_err("invalid immediate operand");
            return 0;
        }
        op->kind = OP_IMM;
        *pp = p;
        return 1;
    }

    if (*p == '%') {
        if (parse_reg(&p, &op->reg, &op->reg_size)) {
            op->kind = OP_REG;
            *pp = p;
            return 1;
        }
        as_err("unknown register");
        return 0;
    }

    /* Memory operand: [disp][(base[,index[,scale]])] */
    if (*p != '(') {
        if (!parse_expr_mod(&p, &op->disp, &op->sym_idx, &op->rel_mod)) {
            return 0;
        }
    }
    skip_ws(&p);
    if (*p == '(') {
        int r, sz;
        p++;
        skip_ws(&p);
        if (*p == '%') {
            if (!parse_reg(&p, &r, &sz) || sz != 4) {
                as_err("expected 32-bit base register");
                return 0;
            }
            op->base = r;
        }
        skip_ws(&p);
        if (*p == ',') {
            p++;
            skip_ws(&p);
            if (*p == '%') {
                if (!parse_reg(&p, &r, &sz) || sz != 4) {
                    as_err("expected 32-bit index register");
                    return 0;
                }
                op->index = r;
            }
            skip_ws(&p);
            if (*p == ',') {
                s32_t sc = 1;
                p++;
                skip_ws(&p);
                if (!parse_number(&p, &sc) || (sc != 1 && sc != 2 && sc != 4 && sc != 8)) {
                    as_err("invalid SIB scale (must be 1, 2, 4, or 8)");
                    return 0;
                }
                op->scale = (int)sc;
            }
        }
        skip_ws(&p);
        if (*p != ')') {
            as_err("missing ')' in memory operand");
            return 0;
        }
        p++;
    }

    op->kind = OP_MEM;
    *pp = p;
    return 1;
}

static void emit_modrm_mem(int reg_field, const struct operand *op)
{
    int base = op->base;
    int idx  = op->index;
    int sc   = op->scale;
    int has_sym = (op->sym_idx >= 0) ? 1 : 0;
    s32_t disp = op->disp;

    reg_field &= 7;

    /* Pure absolute displacement [disp32] */
    if (base < 0 && idx < 0) {
        emit_u8((u8_t)((reg_field << 3) | 5));
        emit_rel32_sym_mod(op->sym_idx, disp, op->rel_mod);
        return;
    }

    /* SIB needed when index is present or base == %esp (4) */
    if (idx >= 0 || base == 4) {
        int ss = 0;
        int sib_idx = (idx >= 0) ? idx : 4;
        int sib_base = (base >= 0) ? base : 5;
        int mod;

        if (sc == 2) ss = 1;
        else if (sc == 4) ss = 2;
        else if (sc == 8) ss = 3;

        if (base < 0) {
            mod = 0; /* [index*scale + disp32] */
        } else if (!has_sym && disp == 0 && base != 5) {
            mod = 0;
        } else if (!has_sym && disp >= -128 && disp <= 127) {
            mod = 1;
        } else {
            mod = 2;
        }

        emit_u8((u8_t)((mod << 6) | (reg_field << 3) | 4));
        emit_u8((u8_t)((ss << 6) | ((sib_idx & 7) << 3) | (sib_base & 7)));

        if (base < 0 || mod == 2) {
            emit_rel32_sym_mod(op->sym_idx, disp, op->rel_mod);
        } else if (mod == 1) {
            emit_u8((u8_t)(disp & 0xFF));
        }
        return;
    }

    /* Simple [base + disp] */
    if (!has_sym && disp == 0 && base != 5) {
        emit_u8((u8_t)((0 << 6) | (reg_field << 3) | (base & 7)));
    } else if (!has_sym && disp >= -128 && disp <= 127) {
        emit_u8((u8_t)((1 << 6) | (reg_field << 3) | (base & 7)));
        emit_u8((u8_t)(disp & 0xFF));
    } else {
        emit_u8((u8_t)((2 << 6) | (reg_field << 3) | (base & 7)));
        emit_rel32_sym_mod(op->sym_idx, disp, op->rel_mod);
    }
}

static void emit_modrm_rm(int reg_field, const struct operand *rm)
{
    if (rm->kind == OP_REG) {
        emit_u8((u8_t)(0xC0U | ((u32_t)(reg_field & 7) << 3) | (u32_t)(rm->reg & 7)));
    } else {
        emit_modrm_mem(reg_field, rm);
    }
}

static void emit_alu(int alu_op, int size, const struct operand *src, const struct operand *dst)
{
    if (size == 2) {
        emit_u8(0x66U);
    }
    if (src->kind == OP_IMM) {
        if (size == 1) {
            emit_u8(0x80U);
            emit_modrm_rm(alu_op, dst);
            emit_u8((u8_t)(src->disp & 0xFF));
        } else if (src->sym_idx < 0 && src->disp >= -128 && src->disp <= 127) {
            emit_u8(0x83U);
            emit_modrm_rm(alu_op, dst);
            emit_u8((u8_t)(src->disp & 0xFF));
        } else {
            emit_u8(0x81U);
            emit_modrm_rm(alu_op, dst);
            if (size == 2) {
                emit_u16((u16_t)(src->disp & 0xFFFF));
            } else {
                emit_rel32_sym_mod(src->sym_idx, src->disp, src->rel_mod);
            }
        }
    } else if (src->kind == OP_REG) {
        u8_t base_op = (u8_t)((alu_op << 3) | ((size == 1) ? 0x00 : 0x01));
        emit_u8(base_op);
        emit_modrm_rm(src->reg, dst);
    } else if (src->kind == OP_MEM && dst->kind == OP_REG) {
        u8_t base_op = (u8_t)((alu_op << 3) | ((size == 1) ? 0x02 : 0x03));
        emit_u8(base_op);
        emit_modrm_mem(dst->reg, src);
    } else {
        as_err("invalid operands for ALU instruction");
    }
}

static void emit_shift(int sh_op, int size, int nops, const struct operand *op1, const struct operand *op2)
{
    const struct operand *dst = (nops == 1) ? op1 : op2;
    if (size == 2) {
        emit_u8(0x66U);
    }
    if (nops == 1) {
        emit_u8((size == 1) ? 0xD0U : 0xD1U);
        emit_modrm_rm(sh_op, dst);
    } else if (op1->kind == OP_REG && op1->reg == 1 && op1->reg_size == 1) {
        emit_u8((size == 1) ? 0xD2U : 0xD3U);
        emit_modrm_rm(sh_op, dst);
    } else if (op1->kind == OP_IMM) {
        if (op1->disp == 1) {
            emit_u8((size == 1) ? 0xD0U : 0xD1U);
            emit_modrm_rm(sh_op, dst);
        } else {
            emit_u8((size == 1) ? 0xC0U : 0xC1U);
            emit_modrm_rm(sh_op, dst);
            emit_u8((u8_t)(op1->disp & 0xFF));
        }
    } else {
        as_err("invalid shift count operand");
    }
}

static int jcc_code(const char *m)
{
    if (u_strcasecmp(m, "jo") == 0) return 0x0;
    if (u_strcasecmp(m, "jno") == 0) return 0x1;
    if (u_strcasecmp(m, "jb") == 0 || u_strcasecmp(m, "jc") == 0 || u_strcasecmp(m, "jnae") == 0) return 0x2;
    if (u_strcasecmp(m, "jae") == 0 || u_strcasecmp(m, "jnb") == 0 || u_strcasecmp(m, "jnc") == 0) return 0x3;
    if (u_strcasecmp(m, "je") == 0 || u_strcasecmp(m, "jz") == 0) return 0x4;
    if (u_strcasecmp(m, "jne") == 0 || u_strcasecmp(m, "jnz") == 0) return 0x5;
    if (u_strcasecmp(m, "jbe") == 0 || u_strcasecmp(m, "jna") == 0) return 0x6;
    if (u_strcasecmp(m, "ja") == 0 || u_strcasecmp(m, "jnbe") == 0) return 0x7;
    if (u_strcasecmp(m, "js") == 0) return 0x8;
    if (u_strcasecmp(m, "jns") == 0) return 0x9;
    if (u_strcasecmp(m, "jl") == 0 || u_strcasecmp(m, "jnge") == 0) return 0xC;
    if (u_strcasecmp(m, "jge") == 0 || u_strcasecmp(m, "jnl") == 0) return 0xD;
    if (u_strcasecmp(m, "jle") == 0 || u_strcasecmp(m, "jng") == 0) return 0xE;
    if (u_strcasecmp(m, "jg") == 0 || u_strcasecmp(m, "jnle") == 0) return 0xF;
    return -1;
}

static int setcc_code(const char *m)
{
    if (m[0] != 's' && m[0] != 'S') return -1;
    if (m[1] != 'e' && m[1] != 'E') return -1;
    if (m[2] != 't' && m[2] != 'T') return -1;
    {
        char tmp[8];
        int i = 0;
        tmp[0] = 'j';
        while (m[3 + i] != '\0' && i < 6) {
            tmp[1 + i] = m[3 + i];
            i++;
        }
        tmp[1 + i] = '\0';
        return jcc_code(tmp);
    }
}

static int infer_size(int explicit_sz, const struct operand *a, const struct operand *b)
{
    if (explicit_sz > 0) return explicit_sz;
    if (a != (const struct operand *)0 && a->kind == OP_REG) return a->reg_size;
    if (b != (const struct operand *)0 && b->kind == OP_REG) return b->reg_size;
    return 4;
}

static void handle_directive(const char *dir, const char **pp)
{
    const char *p = *pp;

    if (u_strcasecmp(dir, ".text") == 0) {
        cur_sec = SEC_TEXT;
        in_ignored_sec = 0;
    } else if (u_strcasecmp(dir, ".data") == 0) {
        cur_sec = SEC_DATA;
        in_ignored_sec = 0;
    } else if (u_strcasecmp(dir, ".rodata") == 0) {
        cur_sec = SEC_RODATA;
        in_ignored_sec = 0;
    } else if (u_strcasecmp(dir, ".bss") == 0) {
        cur_sec = SEC_BSS;
        in_ignored_sec = 0;
    } else if (u_strcasecmp(dir, ".section") == 0) {
        char sname[32];
        int len = 0;
        skip_ws(&p);
        if (*p == '"') p++;
        while (*p != '\0' && *p != '\n' && *p != ',' && *p != '"' && !is_space(*p) && *p != ';') {
            if (len < 31) sname[len++] = *p;
            p++;
        }
        sname[len] = '\0';
        in_ignored_sec = 0;
        if (len >= 5 && sname[0] == '.' && sname[1] == 't' && sname[2] == 'e' && sname[3] == 'x' && sname[4] == 't') {
            cur_sec = SEC_TEXT;
        } else if (len >= 7 && sname[0] == '.' && sname[1] == 'r' && sname[2] == 'o' && sname[3] == 'd' &&
                   sname[4] == 'a' && sname[5] == 't' && sname[6] == 'a') {
            cur_sec = SEC_RODATA;
        } else if (len >= 5 && sname[0] == '.' && sname[1] == 'd' && sname[2] == 'a' && sname[3] == 't' && sname[4] == 'a') {
            cur_sec = SEC_DATA;
        } else if (len >= 4 && sname[0] == '.' && sname[1] == 'b' && sname[2] == 's' && sname[3] == 's') {
            cur_sec = SEC_BSS;
        } else {
            in_ignored_sec = 1;
        }
    } else if (u_strcasecmp(dir, ".global") == 0 || u_strcasecmp(dir, ".globl") == 0) {
        for (;;) {
            char name[MAX_SYM_NAME];
            int len = 0;
            int idx;
            skip_ws(&p);
            while (is_ident_char(*p)) {
                if (len < MAX_SYM_NAME - 1) name[len++] = *p;
                p++;
            }
            name[len] = '\0';
            if (len > 0) {
                idx = find_or_add_sym(name);
                if (idx >= 0) syms[idx].is_global = 1;
            }
            skip_ws(&p);
            if (*p == ',') { p++; continue; }
            break;
        }
    } else if (u_strcasecmp(dir, ".equ") == 0 || u_strcasecmp(dir, ".set") == 0) {
        char name[MAX_SYM_NAME];
        int len = 0;
        s32_t val = 0;
        int rsym = -1;
        skip_ws(&p);
        while (is_ident_char(*p)) {
            if (len < MAX_SYM_NAME - 1) name[len++] = *p;
            p++;
        }
        name[len] = '\0';
        skip_ws(&p);
        if (*p == ',') p++;
        if (len > 0 && parse_expr(&p, &val, &rsym)) {
            int idx = find_or_add_sym(name);
            if (idx >= 0) {
                syms[idx].sec     = -2;
                syms[idx].value   = (u32_t)val;
                syms[idx].defined = 1;
            }
        }
    } else if (u_strcasecmp(dir, ".ascii") == 0 || u_strcasecmp(dir, ".asciz") == 0 ||
               u_strcasecmp(dir, ".string") == 0) {
        int add_nul = (u_strcasecmp(dir, ".ascii") != 0) ? 1 : 0;
        for (;;) {
            skip_ws(&p);
            if (*p != '"') break;
            p++;
            while (*p != '\0' && *p != '"') {
                u8_t ch;
                if (*p == '\\') {
                    p++;
                    if (*p == 'n') { ch = 10U; p++; }
                    else if (*p == 'r') { ch = 13U; p++; }
                    else if (*p == 't') { ch = 9U; p++; }
                    else if (*p == '0') { ch = 0U; p++; }
                    else if (*p == '\\') { ch = '\\'; p++; }
                    else if (*p == '"') { ch = '"'; p++; }
                    else if (*p == 'x') {
                        u32_t hv = 0U;
                        p++;
                        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
                            u32_t d = (*p <= '9') ? (u32_t)(*p - '0') :
                                      (*p <= 'F') ? (u32_t)(*p - 'A' + 10) : (u32_t)(*p - 'a' + 10);
                            hv = (hv << 4) | d;
                            p++;
                        }
                        ch = (u8_t)(hv & 0xFFU);
                    } else {
                        ch = (u8_t)*p++;
                    }
                } else {
                    ch = (u8_t)*p++;
                }
                emit_u8(ch);
            }
            if (*p == '"') p++;
            if (add_nul) emit_u8(0U);
            skip_ws(&p);
            if (*p == ',') { p++; continue; }
            break;
        }
    } else if (u_strcasecmp(dir, ".byte") == 0 || u_strcasecmp(dir, ".word") == 0 ||
               u_strcasecmp(dir, ".short") == 0 || u_strcasecmp(dir, ".2byte") == 0 ||
               u_strcasecmp(dir, ".long") == 0 || u_strcasecmp(dir, ".int") == 0 ||
               u_strcasecmp(dir, ".4byte") == 0) {
        int dsz = 4;
        if (u_strcasecmp(dir, ".byte") == 0) dsz = 1;
        else if (u_strcasecmp(dir, ".word") == 0 || u_strcasecmp(dir, ".short") == 0 ||
                 u_strcasecmp(dir, ".2byte") == 0) dsz = 2;
        for (;;) {
            s32_t val = 0;
            int rsym = -1;
            skip_ws(&p);
            if (!parse_expr(&p, &val, &rsym)) break;
            if (dsz == 1) emit_u8((u8_t)(val & 0xFF));
            else if (dsz == 2) emit_u16((u16_t)(val & 0xFFFF));
            else emit_rel32_sym(rsym, val);
            skip_ws(&p);
            if (*p == ',') { p++; continue; }
            break;
        }
    } else if (u_strcasecmp(dir, ".space") == 0 || u_strcasecmp(dir, ".skip") == 0 ||
               u_strcasecmp(dir, ".zero") == 0) {
        s32_t cnt = 0;
        s32_t fill = 0;
        int rsym = -1;
        skip_ws(&p);
        if (parse_expr(&p, &cnt, &rsym) && cnt > 0) {
            skip_ws(&p);
            if (*p == ',') {
                p++;
                (void)parse_expr(&p, &fill, &rsym);
            }
            while (cnt > 0) {
                emit_u8((u8_t)(fill & 0xFF));
                cnt--;
            }
        }
    } else if (u_strcasecmp(dir, ".align") == 0 || u_strcasecmp(dir, ".balign") == 0 ||
               u_strcasecmp(dir, ".p2align") == 0) {
        s32_t a = 1;
        int rsym = -1;
        u32_t align_val;
        skip_ws(&p);
        if (parse_expr(&p, &a, &rsym) && a > 0) {
            if (u_strcasecmp(dir, ".p2align") == 0) {
                align_val = 1U << (u32_t)a;
            } else {
                align_val = (u32_t)a;
            }
            if (align_val > 1U) {
                u8_t pad = (cur_sec == SEC_TEXT) ? 0x90U : 0x00U;
                while ((sec_len[cur_sec] & (align_val - 1U)) != 0U) {
                    emit_u8(pad);
                }
            }
        }
    } else if (u_strcasecmp(dir, ".comm") == 0 || u_strcasecmp(dir, ".lcomm") == 0) {
        char name[MAX_SYM_NAME];
        int len = 0;
        s32_t sz = 0;
        int rsym = -1;
        skip_ws(&p);
        while (is_ident_char(*p)) {
            if (len < MAX_SYM_NAME - 1) name[len++] = *p;
            p++;
        }
        name[len] = '\0';
        skip_ws(&p);
        if (*p == ',') p++;
        if (len > 0 && parse_expr(&p, &sz, &rsym) && sz > 0) {
            int idx = find_or_add_sym(name);
            if (idx >= 0) {
                u32_t bpos = (sec_len[SEC_BSS] + 3U) & ~3U;
                sec_len[SEC_BSS] = bpos + (u32_t)sz;
                syms[idx].sec     = SEC_BSS;
                syms[idx].value   = bpos;
                syms[idx].defined = 1;
                if (u_strcasecmp(dir, ".comm") == 0) {
                    syms[idx].is_global = 1;
                }
            }
        }
    }
    /* Ignore metadata directives (.type, .size, .file, .ident, .code32, .local, .extern, etc.) */
    *pp = p;
}

static void assemble_instruction(const char *mnem, const char **pp)
{
    struct operand op1, op2;
    int nops = 0;
    int cc;
    const char *p = *pp;

    cur_insn_off = sec_len[cur_sec];

    /* Optional 'rep' prefix before string instruction */
    if (u_strcasecmp(mnem, "rep") == 0 || u_strcasecmp(mnem, "repe") == 0 || u_strcasecmp(mnem, "repz") == 0) {
        emit_u8(0xF3U);
        skip_ws(&p);
        if (is_ident_start(*p)) {
            char sub[32];
            int len = 0;
            while (is_ident_char(*p) && len < 31) sub[len++] = *p++;
            sub[len] = '\0';
            assemble_instruction(sub, &p);
        }
        *pp = p;
        return;
    }

    if (parse_operand(&p, &op1)) {
        nops = 1;
        skip_ws(&p);
        if (*p == ',') {
            p++;
            if (parse_operand(&p, &op2)) {
                nops = 2;
            }
        }
    }
    *pp = p;

    /* Zero-operand instructions */
    if (u_strcasecmp(mnem, "nop") == 0) { emit_u8(0x90U); return; }
    if (u_strcasecmp(mnem, "ret") == 0 || u_strcasecmp(mnem, "retl") == 0) {
        if (nops == 1 && op1.kind == OP_IMM) {
            emit_u8(0xC2U);
            emit_u16((u16_t)(op1.disp & 0xFFFF));
        } else {
            emit_u8(0xC3U);
        }
        return;
    }
    if (u_strcasecmp(mnem, "leave") == 0) { emit_u8(0xC9U); return; }
    if (u_strcasecmp(mnem, "hlt") == 0) { emit_u8(0xF4U); return; }
    if (u_strcasecmp(mnem, "clc") == 0) { emit_u8(0xF8U); return; }
    if (u_strcasecmp(mnem, "stc") == 0) { emit_u8(0xF9U); return; }
    if (u_strcasecmp(mnem, "cld") == 0) { emit_u8(0xFCU); return; }
    if (u_strcasecmp(mnem, "std") == 0) { emit_u8(0xFDU); return; }
    if (u_strcasecmp(mnem, "cdq") == 0 || u_strcasecmp(mnem, "cltd") == 0) { emit_u8(0x99U); return; }
    if (u_strcasecmp(mnem, "cwde") == 0 || u_strcasecmp(mnem, "cwtl") == 0) { emit_u8(0x98U); return; }
    if (u_strcasecmp(mnem, "ud2") == 0) { emit_u8(0x0FU); emit_u8(0x0BU); return; }
    if (u_strcasecmp(mnem, "movsb") == 0) { emit_u8(0xA4U); return; }
    if (u_strcasecmp(mnem, "movsl") == 0 || u_strcasecmp(mnem, "movsd") == 0) { emit_u8(0xA5U); return; }
    if (u_strcasecmp(mnem, "stosb") == 0) { emit_u8(0xAAU); return; }
    if (u_strcasecmp(mnem, "stosl") == 0) { emit_u8(0xABU); return; }
    if (u_strcasecmp(mnem, "lodsb") == 0) { emit_u8(0xACU); return; }
    if (u_strcasecmp(mnem, "lodsl") == 0) { emit_u8(0xADU); return; }

    /* int $imm8 */
    if (u_strcasecmp(mnem, "int") == 0 && nops == 1 && op1.kind == OP_IMM) {
        emit_u8(0xCDU);
        emit_u8((u8_t)(op1.disp & 0xFF));
        return;
    }

    /* push / pop */
    if (u_strcasecmp(mnem, "push") == 0 || u_strcasecmp(mnem, "pushl") == 0) {
        if (nops != 1) { as_err("push expects 1 operand"); return; }
        if (op1.kind == OP_REG && op1.reg_size == 4) {
            emit_u8((u8_t)(0x50U + (u32_t)op1.reg));
        } else if (op1.kind == OP_IMM) {
            if (op1.sym_idx < 0 && op1.disp >= -128 && op1.disp <= 127) {
                emit_u8(0x6AU);
                emit_u8((u8_t)(op1.disp & 0xFF));
            } else {
                emit_u8(0x68U);
                emit_rel32_sym(op1.sym_idx, op1.disp);
            }
        } else if (op1.kind == OP_MEM) {
            emit_u8(0xFFU);
            emit_modrm_mem(6, &op1);
        } else {
            as_err("invalid operand for push");
        }
        return;
    }
    if (u_strcasecmp(mnem, "pop") == 0 || u_strcasecmp(mnem, "popl") == 0) {
        if (nops != 1) { as_err("pop expects 1 operand"); return; }
        if (op1.kind == OP_REG && op1.reg_size == 4) {
            emit_u8((u8_t)(0x58U + (u32_t)op1.reg));
        } else if (op1.kind == OP_MEM) {
            emit_u8(0x8FU);
            emit_modrm_mem(0, &op1);
        } else {
            as_err("invalid operand for pop");
        }
        return;
    }

    /* jmp / call / jcc */
    if (u_strcasecmp(mnem, "jmp") == 0 || u_strcasecmp(mnem, "call") == 0) {
        int is_call = (u_strcasecmp(mnem, "call") == 0) ? 1 : 0;
        if (nops != 1) { as_err("jmp/call expects 1 operand"); return; }
        if (op1.is_indirect || op1.kind == OP_REG) {
            emit_u8(0xFFU);
            emit_modrm_rm(is_call ? 2 : 4, &op1);
        } else if (op1.kind == OP_MEM) {
            emit_u8(is_call ? 0xE8U : 0xE9U);
            emit_pc32_sym_mod(op1.sym_idx, op1.disp, op1.rel_mod);
        } else {
            as_err("invalid target for jmp/call");
        }
        return;
    }

    cc = jcc_code(mnem);
    if (cc >= 0) {
        if (nops != 1 || op1.kind != OP_MEM) { as_err("jcc expects a label"); return; }
        emit_u8(0x0FU);
        emit_u8((u8_t)(0x80U + (u32_t)cc));
        emit_pc32_sym_mod(op1.sym_idx, op1.disp, op1.rel_mod);
        return;
    }

    cc = setcc_code(mnem);
    if (cc >= 0) {
        if (nops != 1) { as_err("setcc expects 1 operand"); return; }
        emit_u8(0x0FU);
        emit_u8((u8_t)(0x90U + (u32_t)cc));
        emit_modrm_rm(0, &op1);
        return;
    }

    /* mov / movl / movw / movb */
    if (u_strcasecmp(mnem, "mov") == 0 || u_strcasecmp(mnem, "movl") == 0 ||
        u_strcasecmp(mnem, "movw") == 0 || u_strcasecmp(mnem, "movb") == 0) {
        int esz = (u_strcasecmp(mnem, "movb") == 0) ? 1 :
                  (u_strcasecmp(mnem, "movw") == 0) ? 2 :
                  (u_strcasecmp(mnem, "movl") == 0) ? 4 : 0;
        int sz = infer_size(esz, &op1, &op2);
        if (nops != 2) { as_err("mov expects 2 operands"); return; }
        if (sz == 2) emit_u8(0x66U);
        if (op1.kind == OP_IMM && op2.kind == OP_REG) {
            emit_u8((u8_t)(((sz == 1) ? 0xB0U : 0xB8U) + (u32_t)op2.reg));
            if (sz == 1) emit_u8((u8_t)(op1.disp & 0xFF));
            else if (sz == 2) emit_u16((u16_t)(op1.disp & 0xFFFF));
            else emit_rel32_sym(op1.sym_idx, op1.disp);
        } else if (op1.kind == OP_IMM && op2.kind == OP_MEM) {
            emit_u8((sz == 1) ? 0xC6U : 0xC7U);
            emit_modrm_mem(0, &op2);
            if (sz == 1) emit_u8((u8_t)(op1.disp & 0xFF));
            else if (sz == 2) emit_u16((u16_t)(op1.disp & 0xFFFF));
            else emit_rel32_sym(op1.sym_idx, op1.disp);
        } else if (op1.kind == OP_REG) {
            emit_u8((sz == 1) ? 0x88U : 0x89U);
            emit_modrm_rm(op1.reg, &op2);
        } else if (op1.kind == OP_MEM && op2.kind == OP_REG) {
            emit_u8((sz == 1) ? 0x8AU : 0x8BU);
            emit_modrm_mem(op2.reg, &op1);
        } else {
            as_err("invalid operands for mov");
        }
        return;
    }

    /* movzbl / movzwl / movsbl / movswl */
    if (u_strcasecmp(mnem, "movzbl") == 0 || u_strcasecmp(mnem, "movzwl") == 0 ||
        u_strcasecmp(mnem, "movsbl") == 0 || u_strcasecmp(mnem, "movswl") == 0) {
        u8_t op2b = (u_strcasecmp(mnem, "movzbl") == 0) ? 0xB6U :
                    (u_strcasecmp(mnem, "movzwl") == 0) ? 0xB7U :
                    (u_strcasecmp(mnem, "movsbl") == 0) ? 0xBEU : 0xBFU;
        if (nops != 2 || op2.kind != OP_REG) { as_err("movzx/movsx expects reg destination"); return; }
        emit_u8(0x0FU);
        emit_u8(op2b);
        emit_modrm_rm(op2.reg, &op1);
        return;
    }

    /* lea / leal */
    if (u_strcasecmp(mnem, "lea") == 0 || u_strcasecmp(mnem, "leal") == 0) {
        if (nops != 2 || op1.kind != OP_MEM || op2.kind != OP_REG) {
            as_err("lea expects mem, reg");
            return;
        }
        emit_u8(0x8DU);
        emit_modrm_mem(op2.reg, &op1);
        return;
    }

    /* xchg */
    if (u_strcasecmp(mnem, "xchg") == 0 || u_strcasecmp(mnem, "xchgl") == 0) {
        if (nops != 2) { as_err("xchg expects 2 operands"); return; }
        if (op1.kind == OP_REG) {
            emit_u8(0x87U);
            emit_modrm_rm(op1.reg, &op2);
        } else if (op2.kind == OP_REG) {
            emit_u8(0x87U);
            emit_modrm_rm(op2.reg, &op1);
        } else {
            as_err("invalid operands for xchg");
        }
        return;
    }

    /* test / testl / testb */
    if (u_strcasecmp(mnem, "test") == 0 || u_strcasecmp(mnem, "testl") == 0 ||
        u_strcasecmp(mnem, "testw") == 0 || u_strcasecmp(mnem, "testb") == 0) {
        int esz = (u_strcasecmp(mnem, "testb") == 0) ? 1 :
                  (u_strcasecmp(mnem, "testw") == 0) ? 2 :
                  (u_strcasecmp(mnem, "testl") == 0) ? 4 : 0;
        int sz = infer_size(esz, &op1, &op2);
        if (nops != 2) { as_err("test expects 2 operands"); return; }
        if (sz == 2) emit_u8(0x66U);
        if (op1.kind == OP_IMM) {
            emit_u8((sz == 1) ? 0xF6U : 0xF7U);
            emit_modrm_rm(0, &op2);
            if (sz == 1) emit_u8((u8_t)(op1.disp & 0xFF));
            else if (sz == 2) emit_u16((u16_t)(op1.disp & 0xFFFF));
            else emit_rel32_sym(op1.sym_idx, op1.disp);
        } else if (op1.kind == OP_REG) {
            emit_u8((sz == 1) ? 0x84U : 0x85U);
            emit_modrm_rm(op1.reg, &op2);
        } else if (op2.kind == OP_REG) {
            emit_u8((sz == 1) ? 0x84U : 0x85U);
            emit_modrm_rm(op2.reg, &op1);
        } else {
            as_err("invalid operands for test");
        }
        return;
    }

    /* 2-operand ALU: add, or, adc, sbb, and, sub, xor, cmp */
    {
        static const char *const alu_names[8] = {
            "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp"
        };
        int a;
        for (a = 0; a < 8; a++) {
            u32_t blen = u_strlen(alu_names[a]);
            u32_t mlen = u_strlen(mnem);
            int esz = -1;
            if (mlen == blen && u_strcasecmp(mnem, alu_names[a]) == 0) {
                esz = 0;
            } else if (mlen == blen + 1U) {
                char suf = to_lower(mnem[blen]);
                if (suf == 'b' || suf == 'w' || suf == 'l') {
                    char tmp[8];
                    u_memcpy(tmp, mnem, blen);
                    tmp[blen] = '\0';
                    if (u_strcasecmp(tmp, alu_names[a]) == 0) {
                        esz = (suf == 'b') ? 1 : (suf == 'w') ? 2 : 4;
                    }
                }
            }
            if (esz >= 0) {
                int sz = infer_size(esz, &op1, &op2);
                if (nops != 2) { as_err("ALU instruction expects 2 operands"); return; }
                emit_alu(a, sz, &op1, &op2);
                return;
            }
        }
    }

    /* Unary inc / dec / neg / not / mul / imul / div / idiv */
    if (u_strcasecmp(mnem, "inc") == 0 || u_strcasecmp(mnem, "incl") == 0 || u_strcasecmp(mnem, "incb") == 0 ||
        u_strcasecmp(mnem, "dec") == 0 || u_strcasecmp(mnem, "decl") == 0 || u_strcasecmp(mnem, "decb") == 0) {
        int is_dec = (to_lower(mnem[0]) == 'd') ? 1 : 0;
        int sz = infer_size((to_lower(mnem[3]) == 'b') ? 1 : (to_lower(mnem[3]) == 'l') ? 4 : 0, &op1, (const struct operand *)0);
        if (nops != 1) { as_err("inc/dec expects 1 operand"); return; }
        if (sz == 4 && op1.kind == OP_REG) {
            emit_u8((u8_t)((is_dec ? 0x48U : 0x40U) + (u32_t)op1.reg));
        } else {
            emit_u8((sz == 1) ? 0xFEU : 0xFFU);
            emit_modrm_rm(is_dec ? 1 : 0, &op1);
        }
        return;
    }

    if (u_strcasecmp(mnem, "not") == 0 || u_strcasecmp(mnem, "notl") == 0 || u_strcasecmp(mnem, "notb") == 0 ||
        u_strcasecmp(mnem, "neg") == 0 || u_strcasecmp(mnem, "negl") == 0 || u_strcasecmp(mnem, "negb") == 0 ||
        u_strcasecmp(mnem, "mul") == 0 || u_strcasecmp(mnem, "mull") == 0 ||
        u_strcasecmp(mnem, "div") == 0 || u_strcasecmp(mnem, "divl") == 0 ||
        u_strcasecmp(mnem, "idiv") == 0 || u_strcasecmp(mnem, "idivl") == 0) {
        int ext = (to_lower(mnem[0]) == 'n' && to_lower(mnem[1]) == 'o') ? 2 :
                  (to_lower(mnem[0]) == 'n') ? 3 :
                  (to_lower(mnem[0]) == 'm') ? 4 :
                  (to_lower(mnem[0]) == 'd') ? 6 : 7;
        u32_t mlen = u_strlen(mnem);
        int sz = infer_size((to_lower(mnem[mlen - 1U]) == 'b') ? 1 : 4, &op1, (const struct operand *)0);
        if (nops != 1) { as_err("unary ALU expects 1 operand"); return; }
        emit_u8((sz == 1) ? 0xF6U : 0xF7U);
        emit_modrm_rm(ext, &op1);
        return;
    }

    if (u_strcasecmp(mnem, "imul") == 0 || u_strcasecmp(mnem, "imull") == 0) {
        if (nops == 1) {
            emit_u8(0xF7U);
            emit_modrm_rm(5, &op1);
        } else if (nops == 2 && op2.kind == OP_REG) {
            if (op1.kind == OP_IMM) {
                emit_u8(0x69U);
                emit_modrm_rm(op2.reg, &op2);
                emit_rel32_sym(op1.sym_idx, op1.disp);
            } else {
                emit_u8(0x0FU);
                emit_u8(0xAFU);
                emit_modrm_rm(op2.reg, &op1);
            }
        } else {
            as_err("invalid operands for imul");
        }
        return;
    }

    /* Shifts & rotates */
    if (u_strcasecmp(mnem, "rol") == 0 || u_strcasecmp(mnem, "roll") == 0 || u_strcasecmp(mnem, "rolb") == 0) {
        emit_shift(0, (to_lower(mnem[3]) == 'b') ? 1 : 4, nops, &op1, &op2); return;
    }
    if (u_strcasecmp(mnem, "ror") == 0 || u_strcasecmp(mnem, "rorl") == 0 || u_strcasecmp(mnem, "rorb") == 0) {
        emit_shift(1, (to_lower(mnem[3]) == 'b') ? 1 : 4, nops, &op1, &op2); return;
    }
    if (u_strcasecmp(mnem, "shl") == 0 || u_strcasecmp(mnem, "shll") == 0 || u_strcasecmp(mnem, "shlb") == 0 ||
        u_strcasecmp(mnem, "sal") == 0 || u_strcasecmp(mnem, "sall") == 0 || u_strcasecmp(mnem, "salb") == 0) {
        emit_shift(4, (to_lower(mnem[3]) == 'b') ? 1 : 4, nops, &op1, &op2); return;
    }
    if (u_strcasecmp(mnem, "shr") == 0 || u_strcasecmp(mnem, "shrl") == 0 || u_strcasecmp(mnem, "shrb") == 0) {
        emit_shift(5, (to_lower(mnem[3]) == 'b') ? 1 : 4, nops, &op1, &op2); return;
    }
    if (u_strcasecmp(mnem, "sar") == 0 || u_strcasecmp(mnem, "sarl") == 0 || u_strcasecmp(mnem, "sarb") == 0) {
        emit_shift(7, (to_lower(mnem[3]) == 'b') ? 1 : 4, nops, &op1, &op2); return;
    }

    as_puts("as: line ");
    as_put_uint((u32_t)cur_line);
    as_puts(": unknown instruction '");
    as_puts(mnem);
    as_puts("'\n");
    as_errors++;
}

static void run_pass(int pass)
{
    const char *p = src_buf;
    int i;

    cur_pass = pass;
    cur_line = 1;
    cur_sec  = SEC_TEXT;
    cur_insn_off = 0U;
    in_ignored_sec = 0;
    for (i = 0; i < SEC_COUNT; i++) {
        sec_len[i] = 0U;
    }

    while (*p != '\0') {
        skip_ws(&p);

        /* Handle C-style block comments */
        if (p[0] == '/' && p[1] == '*') {
            p += 2;
            while (*p != '\0' && !(p[0] == '*' && p[1] == '/')) {
                if (*p == '\n') cur_line++;
                p++;
            }
            if (p[0] == '*' && p[1] == '/') p += 2;
            continue;
        }

        /* Handle line comments (# or //) */
        if (*p == '#' || (p[0] == '/' && p[1] == '/')) {
            while (*p != '\0' && *p != '\n') p++;
            continue;
        }

        if (*p == '\n') {
            cur_line++;
            p++;
            continue;
        }
        if (*p == ';') {
            p++;
            continue;
        }

        /* Numeric local label definition: [0-9]: */
        if (is_digit(*p) && p[1] == ':') {
            int n = *p - '0';
            p += 2;
            if (pass == 1 && num_label_cnt < MAX_NUM_LABELS) {
                char lname[16];
                int idx;
                lname[0] = '.';
                lname[1] = 'L';
                lname[2] = 'n';
                lname[3] = (char)('0' + (num_label_cnt / 100) % 10);
                lname[4] = (char)('0' + (num_label_cnt / 10) % 10);
                lname[5] = (char)('0' + num_label_cnt % 10);
                lname[6] = '\0';
                idx = find_or_add_sym(lname);
                if (idx >= 0) {
                    syms[idx].sec     = cur_sec;
                    syms[idx].value   = sec_len[cur_sec];
                    syms[idx].defined = 1;
                    num_labels[num_label_cnt].num     = n;
                    num_labels[num_label_cnt].sec     = cur_sec;
                    num_labels[num_label_cnt].offset  = sec_len[cur_sec];
                    num_labels[num_label_cnt].sym_idx = idx;
                    num_label_cnt++;
                }
            }
            continue;
        }

        /* Identifier (label, symbol = expr, directive, or mnemonic) */
        if (is_ident_start(*p)) {
            char tok[MAX_SYM_NAME];
            int len = 0;
            const char *after_tok;

            while (is_ident_char(*p)) {
                if (len < MAX_SYM_NAME - 1) tok[len++] = *p;
                p++;
            }
            tok[len] = '\0';
            after_tok = p;
            skip_ws(&after_tok);

            if (*after_tok == ':') {
                int idx = find_or_add_sym(tok);
                if (idx >= 0) {
                    syms[idx].sec     = cur_sec;
                    syms[idx].value   = sec_len[cur_sec];
                    syms[idx].defined = 1;
                }
                p = after_tok + 1;
                continue;
            }
            if (*after_tok == '=') {
                s32_t val = 0;
                int rsym = -1;
                p = after_tok + 1;
                if (parse_expr(&p, &val, &rsym)) {
                    int idx = find_or_add_sym(tok);
                    if (idx >= 0) {
                        syms[idx].sec     = -2;
                        syms[idx].value   = (u32_t)val;
                        syms[idx].defined = 1;
                    }
                }
            } else if (tok[0] == '.') {
                handle_directive(tok, &p);
            } else {
                assemble_instruction(tok, &p);
            }
        }

        /* Skip to end of statement */
        while (*p != '\0' && *p != '\n' && *p != ';') {
            if (p[0] == '/' && p[1] == '*') break;
            p++;
        }
    }
}

static u32_t add_str(const char *s)
{
    u32_t off = strtab_len;
    u32_t len = u_strlen(s) + 1U;
    if (strtab_len + len <= (u32_t)sizeof(strtab_buf)) {
        u_memcpy(&strtab_buf[strtab_len], s, len);
        strtab_len += len;
    }
    return off;
}

static u32_t align4(u32_t v)
{
    return (v + 3U) & ~3U;
}

static int write_elf32_rel(const char *out_path)
{
    /* Fixed section header layout:
     * 0: NULL
     * 1: .text
     * 2: .rodata
     * 3: .data
     * 4: .bss
     * 5: .rel.text
     * 6: .rel.rodata
     * 7: .rel.data
     * 8: .symtab
     * 9: .strtab
     * 10: .shstrtab
     */
    static const char shstrtab_data[] =
        "\0.text\0.rodata\0.data\0.bss\0.rel.text\0.rel.rodata\0.rel.data\0.symtab\0.strtab\0.shstrtab\0";
    static const u32_t sh_name_offs[11] = {
        0U, 1U, 7U, 15U, 21U, 26U, 36U, 48U, 58U, 66U, 74U
    };
    struct elf32_ehdr eh;
    struct elf32_shdr sh[11];
    u32_t total_elf_syms = 0U;
    u32_t first_global = 0U;
    u32_t off;
    int i, s;
    int fd;

    strtab_len = 0U;
    strtab_buf[strtab_len++] = '\0';

    /* Build ELF symbol table: local symbols first, then globals */
    u_memset(elf_syms, 0, (u32_t)sizeof(elf_syms));
    total_elf_syms = 1U; /* 0 = STN_UNDEF */

    /* Section symbols 1..4 for .text, .rodata, .data, .bss */
    for (s = 0; s < 4; s++) {
        elf_syms[total_elf_syms].st_name  = 0U;
        elf_syms[total_elf_syms].st_value = 0U;
        elf_syms[total_elf_syms].st_size  = 0U;
        elf_syms[total_elf_syms].st_info  = (u8_t)((STB_LOCAL << 4) | STT_SECTION);
        elf_syms[total_elf_syms].st_other = 0U;
        elf_syms[total_elf_syms].st_shndx = (u16_t)(s + 1);
        total_elf_syms++;
    }

    /* Local defined symbols */
    for (i = 0; i < num_syms; i++) {
        if (!syms[i].is_global && syms[i].defined) {
            syms[i].elf_idx = total_elf_syms;
            elf_syms[total_elf_syms].st_name  = add_str(syms[i].name);
            elf_syms[total_elf_syms].st_value = syms[i].value;
            elf_syms[total_elf_syms].st_size  = 0U;
            elf_syms[total_elf_syms].st_info  = (u8_t)((STB_LOCAL << 4) | STT_NOTYPE);
            elf_syms[total_elf_syms].st_other = 0U;
            elf_syms[total_elf_syms].st_shndx = (syms[i].sec == -2) ? (u16_t)SHN_ABS : (u16_t)(syms[i].sec + 1);
            total_elf_syms++;
        }
    }
    first_global = total_elf_syms;

    /* Global and undefined symbols */
    for (i = 0; i < num_syms; i++) {
        if (syms[i].is_global || !syms[i].defined) {
            syms[i].elf_idx = total_elf_syms;
            elf_syms[total_elf_syms].st_name  = add_str(syms[i].name);
            elf_syms[total_elf_syms].st_value = syms[i].defined ? syms[i].value : 0U;
            elf_syms[total_elf_syms].st_size  = 0U;
            elf_syms[total_elf_syms].st_info  = (u8_t)((STB_GLOBAL << 4) | STT_NOTYPE);
            elf_syms[total_elf_syms].st_other = 0U;
            if (!syms[i].defined) {
                elf_syms[total_elf_syms].st_shndx = SHN_UNDEF;
            } else if (syms[i].sec == -2) {
                elf_syms[total_elf_syms].st_shndx = SHN_ABS;
            } else {
                elf_syms[total_elf_syms].st_shndx = (u16_t)(syms[i].sec + 1);
            }
            total_elf_syms++;
        }
    }

    /* Group relocations by section */
    sec_rel_cnt[0] = 0U;
    sec_rel_cnt[1] = 0U;
    sec_rel_cnt[2] = 0U;
    for (i = 0; i < num_rels; i++) {
        int rsec = rels[i].sec;
        u32_t sym_elf = syms[rels[i].sym_idx].elf_idx;
        u32_t idx = sec_rel_cnt[rsec]++;
        sec_rels[rsec][idx].r_offset = rels[i].offset;
        sec_rels[rsec][idx].r_info   = (sym_elf << 8) | (rels[i].type & 0xFFU);
    }

    /* Lay out output image */
    u_memset(out_buf, 0, (u32_t)sizeof(out_buf));
    u_memset(sh, 0, (u32_t)sizeof(sh));
    off = align4((u32_t)sizeof(struct elf32_ehdr));

    /* 1: .text */
    sh[1].sh_name      = sh_name_offs[1];
    sh[1].sh_type      = SHT_PROGBITS;
    sh[1].sh_flags     = SHF_ALLOC | SHF_EXECINSTR;
    sh[1].sh_offset    = off;
    sh[1].sh_size      = sec_len[SEC_TEXT];
    sh[1].sh_addralign = 4U;
    if (sec_len[SEC_TEXT] > 0U) {
        u_memcpy(&out_buf[off], sec_data[SEC_TEXT], sec_len[SEC_TEXT]);
        off = align4(off + sec_len[SEC_TEXT]);
    }

    /* 2: .rodata */
    sh[2].sh_name      = sh_name_offs[2];
    sh[2].sh_type      = SHT_PROGBITS;
    sh[2].sh_flags     = SHF_ALLOC;
    sh[2].sh_offset    = off;
    sh[2].sh_size      = sec_len[SEC_RODATA];
    sh[2].sh_addralign = 4U;
    if (sec_len[SEC_RODATA] > 0U) {
        u_memcpy(&out_buf[off], sec_data[SEC_RODATA], sec_len[SEC_RODATA]);
        off = align4(off + sec_len[SEC_RODATA]);
    }

    /* 3: .data */
    sh[3].sh_name      = sh_name_offs[3];
    sh[3].sh_type      = SHT_PROGBITS;
    sh[3].sh_flags     = SHF_ALLOC | SHF_WRITE;
    sh[3].sh_offset    = off;
    sh[3].sh_size      = sec_len[SEC_DATA];
    sh[3].sh_addralign = 4U;
    if (sec_len[SEC_DATA] > 0U) {
        u_memcpy(&out_buf[off], sec_data[SEC_DATA], sec_len[SEC_DATA]);
        off = align4(off + sec_len[SEC_DATA]);
    }

    /* 4: .bss */
    sh[4].sh_name      = sh_name_offs[4];
    sh[4].sh_type      = SHT_NOBITS;
    sh[4].sh_flags     = SHF_ALLOC | SHF_WRITE;
    sh[4].sh_offset    = off;
    sh[4].sh_size      = sec_len[SEC_BSS];
    sh[4].sh_addralign = 4U;

    /* 5..7: .rel.text, .rel.rodata, .rel.data */
    for (s = 0; s < 3; s++) {
        u32_t rbytes = sec_rel_cnt[s] * (u32_t)sizeof(struct elf32_rel);
        sh[5 + s].sh_name      = sh_name_offs[5 + s];
        sh[5 + s].sh_type      = SHT_REL;
        sh[5 + s].sh_offset    = off;
        sh[5 + s].sh_size      = rbytes;
        sh[5 + s].sh_link      = 8U; /* .symtab */
        sh[5 + s].sh_info      = (u32_t)(1 + s);
        sh[5 + s].sh_addralign = 4U;
        sh[5 + s].sh_entsize   = (u32_t)sizeof(struct elf32_rel);
        if (rbytes > 0U) {
            u_memcpy(&out_buf[off], sec_rels[s], rbytes);
            off = align4(off + rbytes);
        }
    }

    /* 8: .symtab */
    {
        u32_t sbytes = total_elf_syms * (u32_t)sizeof(struct elf32_sym);
        sh[8].sh_name      = sh_name_offs[8];
        sh[8].sh_type      = SHT_SYMTAB;
        sh[8].sh_offset    = off;
        sh[8].sh_size      = sbytes;
        sh[8].sh_link      = 9U; /* .strtab */
        sh[8].sh_info      = first_global;
        sh[8].sh_addralign = 4U;
        sh[8].sh_entsize   = (u32_t)sizeof(struct elf32_sym);
        u_memcpy(&out_buf[off], elf_syms, sbytes);
        off = align4(off + sbytes);
    }

    /* 9: .strtab */
    sh[9].sh_name      = sh_name_offs[9];
    sh[9].sh_type      = SHT_STRTAB;
    sh[9].sh_offset    = off;
    sh[9].sh_size      = strtab_len;
    sh[9].sh_addralign = 1U;
    u_memcpy(&out_buf[off], strtab_buf, strtab_len);
    off = align4(off + strtab_len);

    /* 10: .shstrtab */
    sh[10].sh_name      = sh_name_offs[10];
    sh[10].sh_type      = SHT_STRTAB;
    sh[10].sh_offset    = off;
    sh[10].sh_size      = (u32_t)sizeof(shstrtab_data);
    sh[10].sh_addralign = 1U;
    u_memcpy(&out_buf[off], shstrtab_data, (u32_t)sizeof(shstrtab_data));
    off = align4(off + (u32_t)sizeof(shstrtab_data));

    if (off + (u32_t)sizeof(sh) > MAX_OUT_SIZE) {
        as_puts("as: output ELF too large\n");
        return -1;
    }

    /* Section headers */
    u_memcpy(&out_buf[off], sh, (u32_t)sizeof(sh));

    /* ELF32 header */
    u_memset(&eh, 0, (u32_t)sizeof(eh));
    eh.e_ident[0]  = 0x7FU;
    eh.e_ident[1]  = (u8_t)'E';
    eh.e_ident[2]  = (u8_t)'L';
    eh.e_ident[3]  = (u8_t)'F';
    eh.e_ident[4]  = 1U; /* ELFCLASS32 */
    eh.e_ident[5]  = 1U; /* ELFDATA2LSB */
    eh.e_ident[6]  = 1U; /* EV_CURRENT */
    eh.e_type      = ET_REL;
    eh.e_machine   = EM_386;
    eh.e_version   = 1U;
    eh.e_shoff     = off;
    eh.e_ehsize    = (u16_t)sizeof(struct elf32_ehdr);
    eh.e_shentsize = (u16_t)sizeof(struct elf32_shdr);
    eh.e_shnum     = 11U;
    eh.e_shstrndx  = 10U;
    u_memcpy(out_buf, &eh, (u32_t)sizeof(eh));

    off += (u32_t)sizeof(sh);

    fd = sys_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644U);
    if (fd < 0) {
        as_puts("as: cannot create output file: ");
        as_puts(out_path);
        as_puts("\n");
        return -1;
    }
    if (sys_write(fd, out_buf, off) != (s32_t)off) {
        (void)sys_close(fd);
        as_puts("as: write failed\n");
        return -1;
    }
    (void)sys_close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    const char *in_path = (const char *)0;
    const char *out_path = (const char *)0;
    char default_out[256];
    int i;
    int fd;
    s32_t n;

    for (i = 1; i < argc; i++) {
        if (u_strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (u_strcmp(argv[i], "--32") == 0) {
            /* i386 is already the default */
        } else if (argv[i][0] != '-') {
            in_path = argv[i];
        }
    }

    if (in_path == (const char *)0) {
        as_puts("usage: as [-o out.o] <input.s>\n");
        return 1;
    }

    if (out_path == (const char *)0) {
        u32_t ilen = u_strlen(in_path);
        if (ilen >= 252U) {
            ilen = 252U;
        }
        u_memcpy(default_out, in_path, ilen);
        if (ilen >= 2U && default_out[ilen - 2U] == '.' &&
            (default_out[ilen - 1U] == 's' || default_out[ilen - 1U] == 'S')) {
            default_out[ilen - 1U] = 'o';
            default_out[ilen] = '\0';
        } else {
            default_out[ilen] = '.';
            default_out[ilen + 1U] = 'o';
            default_out[ilen + 2U] = '\0';
        }
        out_path = default_out;
    }

    fd = sys_open(in_path, O_RDONLY, 0U);
    if (fd < 0) {
        as_puts("as: cannot open input file: ");
        as_puts(in_path);
        as_puts("\n");
        return 1;
    }
    src_len = 0U;
    while (src_len < MAX_SRC_SIZE) {
        n = sys_read(fd, &src_buf[src_len], MAX_SRC_SIZE - src_len);
        if (n <= 0) break;
        src_len += (u32_t)n;
    }
    if (src_len >= MAX_SRC_SIZE) {
        char extra;
        if (sys_read(fd, &extra, 1U) > 0) {
            (void)sys_close(fd);
            as_puts("as: input file too large\n");
            return 1;
        }
    }
    (void)sys_close(fd);
    src_buf[src_len] = '\0';

    run_pass(1);
    run_pass(2);

    if (as_errors > 0) {
        return 1;
    }

    if (write_elf32_rel(out_path) < 0) {
        return 1;
    }
    return 0;
}
