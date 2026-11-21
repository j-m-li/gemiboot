
#include "vfs.h"
#include "string.h"
#define BSIZE 512

int nb_char_in = 0;
char *char_in;

int bin_cc_main(int argc, char *argv[])
{
	int i;
	int fi;
	int fo;
	char buf[BSIZE];
	char oname[256];
	char *iname;
	if (argc != 2) {
		return -1;
	}
	iname = argv[1];
	i = 0;
	while (iname[i]) {
		oname[i] = iname[i];
		i = i + 1;
	}
	if (i >= 252) {
		return -1;
	}
	oname[i] = '.';
	i = i + 1;	
	oname[i] = 'S';
	i = i + 1;	
	oname[i] = 0;

	fi = vfs_open(argv[1], VFS_O_RDONLY);
	if (fi < 0) {
		return -1;
	}
	fo = vfs_open(oname, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
	if (fo < 0) {
		return -1;
	}
	nb_char_in = vfs_read(fi, buf, BSIZE);
	while (nb_char_in > 0) {
		vfs_write(1, buf, nb_char_in);
		char_in = buf;
		nb_char_in = vfs_read(fi, buf, BSIZE);
	}
	vfs_close(fi);
	vfs_close(fo);
/*	vfs_unlink(oname); */
	return 0;
}

