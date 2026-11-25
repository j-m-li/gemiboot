
/*
 * register %ebx, %esi, %edi
 * */

#include "vfs.h"
#include "string.h"
#define BSIZE 512

int lex(int fi);

int nb_char_in = 0;
char *char_in = (void*)0;
int char_in_pos = 0;
int in_file = -1;
int out_file = -1;
char token[BSIZE];
int token_pos = 0;
int token_type = 0;

char *next(void)
{
	int c;
	while (char_in_pos >= nb_char_in) {
		c = char_in[char_in_pos];
		char_in_pos += 1;

		if (token_pos < BSIZE) {
			token[token_pos] = c;
			token_pos += 1;
		}
				
	}
	return token;
}

int lex(int fi)
{
	char *c;
	char buf[BSIZE];
	nb_char_in = vfs_read(fi, buf, BSIZE);
	char_in = buf;
	char_in_pos = 0;
	while (nb_char_in > 0) {
		while (char_in_pos < nb_char_in) {
			c = next();
			if (c) {
				vfs_write(1, token, token_pos);
				token_pos = 0;
			}
		}
		nb_char_in = vfs_read(fi, buf, BSIZE);
	}
	return 0;
}

int bin_cc_main(int argc, char *argv[])
{
	int i;
	int fi;
	char oname[256];
	char *iname;
	if (argc != 2) {
		return -1;
	}
	iname = argv[1];
	i = 0;
	while (iname[i]) {
		oname[i] = iname[i];
		i += 1;
	}
	if (i >= 252) {
		return -1;
	}
	oname[i] = '.';
	i += 1;	
	oname[i] = 'S';
	i += 1;	
	oname[i] = 0;

	fi = vfs_open(argv[1], VFS_O_RDONLY);
	if (fi < 0) {
		return -1;
	}
	out_file = vfs_open(oname, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
	if (out_file < 0) {
		return -1;
	}
	lex(fi);
	vfs_close(fi);
	vfs_close(out_file);
/*	vfs_unlink(oname); */
	return 0;
}

