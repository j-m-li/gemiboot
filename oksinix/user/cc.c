/* SPDX-License-Identifier: CC0-1.0 */
/*
 * Public Domain (CC0 1.0 Universal)
 * Oksinix - Standalone Ring-3 C compiler driver (/mnt/CC.ELF)
 *
 * register %ebx, %esi, %edi
 */

typedef unsigned int u32_t;
typedef int          s32_t;

extern s32_t __oksinix_int80(u32_t nr, u32_t a1, u32_t a2, u32_t a3, u32_t a4, u32_t a5);

#define SYS_read    3U
#define SYS_write   4U
#define SYS_open    5U
#define SYS_close   6U

#define O_RDONLY    0x0000U
#define O_WRONLY    0x0001U
#define O_CREAT     0x0040U
#define O_TRUNC     0x0200U

#define BSIZE 512

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

int   lex(int fi);
char *next(void);
int   main(int argc, char *argv[]);

int nb_char_in = 0;
char *char_in = (char *)0;
int char_in_pos = 0;
int in_file = -1;
int out_file = -1;
char token[BSIZE];
int token_pos = 0;
int token_type = 0;

char *next(void)
{
	int c;
	while (char_in_pos < nb_char_in) {
		c = (unsigned char)char_in[char_in_pos];
		char_in_pos += 1;

		if (token_pos < BSIZE) {
			token[token_pos] = (char)c;
			token_pos += 1;
		}
	}
	return token;
}

int lex(int fi)
{
	char *c;
	char buf[BSIZE];
	nb_char_in = (int)sys_read(fi, buf, BSIZE);
	char_in = buf;
	char_in_pos = 0;
	while (nb_char_in > 0) {
		while (char_in_pos < nb_char_in) {
			c = next();
			if (c != (char *)0 && token_pos > 0) {
				(void)sys_write(1, token, (u32_t)token_pos);
				token_pos = 0;
			}
		}
		nb_char_in = (int)sys_read(fi, buf, BSIZE);
		char_in_pos = 0;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int i;
	int fi;
	char oname[256];
	char *iname;
	if (argc != 2 || argv[1] == (char *)0) {
		(void)sys_write(1, "usage: cc <file.c>\n", 19U);
		return 1;
	}
	iname = argv[1];
	i = 0;
	while (iname[i] != '\0' && i < 252) {
		oname[i] = iname[i];
		i += 1;
	}
	if (iname[i] != '\0') {
		return 1;
	}
	oname[i] = '.';
	i += 1;
	oname[i] = 'S';
	i += 1;
	oname[i] = '\0';

	fi = sys_open(argv[1], O_RDONLY, 0U);
	if (fi < 0) {
		(void)sys_write(1, "cc: cannot open input file\n", 27U);
		return 1;
	}
	out_file = sys_open(oname, O_WRONLY | O_CREAT | O_TRUNC, 0644U);
	if (out_file < 0) {
		(void)sys_close(fi);
		(void)sys_write(1, "cc: cannot create output file\n", 30U);
		return 1;
	}
	lex(fi);
	(void)sys_close(fi);
	(void)sys_close(out_file);
	return 0;
}
