/* 
   Copyright (C) Andrew Tridgell 1998-2003
   
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   
   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
/* rzip compression - main program */

#include "rzip.h"

static void usage(void)
{
	printf("rzip %d.%d\n", RZIP_MAJOR_VERSION, RZIP_MINOR_VERSION);
	printf("Copright (C) Andrew Tridgell 1998-2003\n\n");
	printf("stdin/stdout patch by Nicolas Rachinsky\n\n");
	printf("usage: rzip [options] <file...>\n");
	printf(" Options:\n");
	printf("     -0            fastest (worst) compression\n");
	printf("     -6            default compression level\n");
	printf("     -9            slowest (best) compression\n");
	printf("     -d            decompress\n");
	printf("     -o filename   specify the output file name\n");
	printf("     -S suffix     specify compressed suffix (default '.rz')\n");
	printf("     -f            force overwrite of any existing files\n");
	printf("     -k            keep existing files\n");
	printf("     -P            show compression progress\n");
	printf("     -L level      set compression level\n");
	printf("     -V            show version\n");
	printf("     -q file       temporary file for input\n");
	printf("     -Q file       temporary file for output\n");
#if 0
	/* damn, this will be quite hard to do */
	printf("     -t          test compressed file integrity\n");
#endif
	printf("\n"); 
	printf("to read from stdin -q is necessary\n"); 
	printf("to write to stdout -Q is necessary\n"); 
	printf("reading from stdin and writing to stdout while compressing results in files\n"); 
	printf("that cannot be decompressed with plain rzip\n"); 
}


static void write_magic(int fd_in, int fd_out)
{
	struct stat st;
	char magic[24];
	uint32_t v;

	memset(magic, 0, sizeof(magic));
	strcpy(magic, "RZIP");
	magic[4] = RZIP_MAJOR_VERSION;
	magic[5] = RZIP_MINOR_VERSION;

	if (fd_in && fstat(fd_in, &st) != 0) {
		fatal("bad magic file descriptor!?\n");
	} else if(!fd_in) {
		st.st_size=0;
	}
	

#if HAVE_LARGE_FILES
	v = htonl(st.st_size & 0xFFFFFFFF);
	memcpy(&magic[6], &v, 4);
	v = htonl(st.st_size >> 32);
	memcpy(&magic[10], &v, 4);
#else
	v = htonl(st.st_size);
	memcpy(&magic[6], &v, 4);
#endif

	if (write(fd_out, magic, sizeof(magic)) != sizeof(magic)) {
		fatal("Failed to write magic header\n");
	}
}

static void update_magic(off_t l, int fd_out)
{
	char magic[24];
	uint32_t v;

	memset(magic, 0, sizeof(magic));
	strcpy(magic, "RZIP");
	magic[4] = RZIP_MAJOR_VERSION;
	magic[5] = RZIP_MINOR_VERSION;


#if HAVE_LARGE_FILES
	v = htonl(l & 0xFFFFFFFF);
	memcpy(&magic[6], &v, 4);
	v = htonl(l >> 32);
	memcpy(&magic[10], &v, 4);
#else
	v = htonl(l
	memcpy(&magic[6], &v, 4);
#endif

	if(lseek(fd_out,0,SEEK_SET)==-1)
		fatal("Failed to seek\n");

	if (write(fd_out, magic, sizeof(magic)) != sizeof(magic)) {
		fatal("Failed to write magic header\n");
	}
}

static void read_magic(int fd_in, int fd_out, off_t *expected_size)
{
	uint32_t v;
	char magic[24];

	if (read(fd_in, magic, sizeof(magic)) != sizeof(magic)) {
		fatal("Failed to read magic header\n");
	}

	*expected_size = 0;

	if (strncmp(magic, "RZIP", 4) != 0) {
		fatal("Not an rzip file\n");
	}

#if HAVE_LARGE_FILES
	memcpy(&v, &magic[6], 4);
	*expected_size = ntohl(v);
	memcpy(&v, &magic[10], 4);
	*expected_size |= ((off_t)ntohl(v)) << 32;
#else
	memcpy(&v, &magic[6], 4);
	*expected_size = ntohl(v);
#endif

}


/* preserve ownership and permissions where possible */
static void preserve_perms(struct rzip_control *control,
			   int fd_in, int fd_out)
{
	struct stat st;

	if (fstat(fd_in, &st) != 0) {
		fatal("Failed to fstat input file\n");
	}
	if (fchmod(fd_out, (st.st_mode & 0777)) != 0) {
		fatal("Failed to set permissions on %s\n", control->outfile);
	}

	/* chown fail is not fatal */
	fchown(fd_out, st.st_uid, st.st_gid);
}	

	

/*
  decompress one file from the command line
*/
static void decompress_file(struct rzip_control *control)
{
	int fd_in, fd_out = -1, fd_hist = -1;
	off_t expected_size;

	if(control->out_tmp) {
		control->outfile = strdup("-");
	} else {
		if (control->outname) {
			control->outfile = strdup(control->outname);
		} else {
			if (strlen(control->suffix) >= strlen(control->infile) ||
			    strcmp(control->suffix, 
				   control->infile + 
				   strlen(control->infile) - strlen(control->suffix)) != 0) {
				fatal("%s: unknown suffix\n", control->infile);
			}
			
			control->outfile = strdup(control->infile);
			control->outfile[strlen(control->infile) - strlen(control->suffix)] = 0;
		}
	}

	if(control->in_tmp) {
		fd_in = open(control->in_tmp,O_RDWR|O_CREAT|O_EXCL,0600);
		if (fd_in == -1) {
			fatal("Failed to open %s: %s\n", 
			      control->in_tmp, 
			      strerror(errno));
		}
	} else {
		fd_in = open(control->infile,O_RDONLY);
		if (fd_in == -1) {
			fatal("Failed to open %s: %s\n", 
			      control->infile, 
			      strerror(errno));
		}
	}

	if ((control->flags & FLAG_TEST_ONLY) == 0 && ! control->out_tmp) {
		if (control->flags & FLAG_FORCE_REPLACE) {
			fd_out = open(control->outfile,O_WRONLY|O_CREAT|O_TRUNC,0666);
		} else {
			fd_out = open(control->outfile,O_WRONLY|O_CREAT|O_EXCL,0666);
		}
		if (fd_out == -1) {
			fatal("Failed to create %s: %s\n", 
			      control->outfile, strerror(errno));
		}

		if(!control->in_tmp)
			preserve_perms(control, fd_in, fd_out);
		
		fd_hist = open(control->outfile,O_RDONLY);
		if (fd_hist == -1) {
			fatal("Failed to open history file %s\n", 
			      control->outfile);
		}
	} else if(control->out_tmp) {
		fd_out = open(control->out_tmp,O_RDWR|O_CREAT|O_EXCL,0600);
		if (fd_out == -1) {
			fatal("Failed to create %s: %s\n", 
			      control->out_tmp, strerror(errno));
		}

		fd_hist = open(control->out_tmp,O_RDONLY);
		if (fd_hist == -1) {
			fatal("Failed to open history file %s\n", 
			      control->out_tmp);
		}
	}


	
	read_magic(control->in_tmp?STDIN_FILENO:fd_in, fd_out, &expected_size);
	runzip_fd(fd_in, fd_out, fd_hist, expected_size,control->out_tmp?1:0,control->in_tmp?1:0);
	
	if ((control->flags & FLAG_TEST_ONLY) == 0) {
		if (close(fd_hist) != 0 ||
		    close(fd_out) != 0) {
			fatal("Failed to close files\n");
		}
	}

	close(fd_in);


	if(control->out_tmp)
		unlink(control->out_tmp);

	if(control->in_tmp)
		unlink(control->in_tmp);
	else if ((control->flags & (FLAG_KEEP_FILES | FLAG_TEST_ONLY)) == 0) {
		if (unlink(control->infile) != 0) {
			fatal("Failed to unlink %s: %s\n", 
			      control->infile, strerror(errno));
		}
	}

	free(control->outfile);
}

/*
  compress one file from the command line
*/
static void compress_file(struct rzip_control *control)
{
	int fd_in, fd_out;
	off_t l;

	if (strlen(control->suffix) <= strlen(control->infile) &&
	    strcmp(control->suffix, control->infile + strlen(control->infile) - strlen(control->suffix)) == 0) {
		printf("%s: already has %s suffix\n", control->infile, control->suffix);
		return;
	}

	if (control->outname) {
		control->outfile = strdup(control->outname);
	} else {
		control->outfile = malloc(strlen(control->infile) + 
					  strlen(control->suffix) + 1);
		if (!control->outfile) {
			fatal("Failed to allocate outfile name\n");
		}
		strcpy(control->outfile, control->infile);
		strcat(control->outfile, control->suffix);
	}

	if(control->in_tmp) {
		fd_in = open(control->in_tmp,O_RDWR|O_CREAT|O_EXCL,0600);
		if (fd_in == -1) {
			fatal("Failed to open %s: %s\n", control->in_tmp, strerror(errno));
		}

	} else {
		fd_in = open(control->infile,O_RDONLY);
		if (fd_in == -1) {
			fatal("Failed to open %s: %s\n", control->infile, strerror(errno));
		}
	}
	
	if(control->out_tmp) {
		fd_out = open(control->out_tmp,O_RDWR|O_CREAT|O_EXCL,0600);
		if (fd_out == -1) {
			fatal("Failed to open %s: %s\n", control->out_tmp, strerror(errno));
		}
	} else {
		if (control->flags & FLAG_FORCE_REPLACE) {
			fd_out = open(control->outfile,O_WRONLY|O_CREAT|O_TRUNC,0666);
		} else {
			fd_out = open(control->outfile,O_WRONLY|O_CREAT|O_EXCL,0666);
		}
		if (fd_out == -1) {
			fatal("Failed to create %s: %s\n", control->outfile, strerror(errno));
		}
	}

	if(!control->in_tmp && !control->out_tmp)
		preserve_perms(control, fd_in, fd_out);

	if(!control->in_tmp) {
		write_magic(fd_in, fd_out);
	} else {
		write_magic(0, fd_out);
	}

	l = rzip_fd(control, fd_in, fd_out);

	if(control->in_tmp && !control->out_tmp) {
		update_magic(l, fd_out);
	}

	if (close(fd_in) != 0 ||
	    close(fd_out) != 0) {
		fatal("Failed to close files\n");
	}

	if(control->out_tmp)
		unlink (control->out_tmp);

	if(control->in_tmp) {
		unlink (control->in_tmp);
	} else if ((control->flags & FLAG_KEEP_FILES) == 0) {
		if (unlink(control->infile) != 0) {
			fatal("Failed to unlink %s: %s\n", control->infile, strerror(errno));
		}
	}


	free(control->outfile);
}

 int main(int argc, char *argv[])
{
	extern int optind;
	int c, i;
	struct rzip_control control;

	memset(&control, 0, sizeof(control));

	control.compression_level = 6;
	control.flags = 0;
	control.suffix = ".rz";

	if (strstr(argv[0], "runzip")) {
		control.flags |= FLAG_DECOMPRESS;
	}

	while ((c = getopt(argc, argv, "h0123456789dS:tVvkfPo:L:q:Q:")) != -1) {
		if (isdigit(c)) {
			control.compression_level = c - '0';
			continue;
		}
		switch (c) {
		case 'L':
			control.compression_level = atoi(optarg);
			break;
		case 'd':
			control.flags |= FLAG_DECOMPRESS;
			break;
		case 'S':
			control.suffix = optarg;
			break;
		case 'o':
			control.outname = optarg;
			break;
		case 'q':
			control.in_tmp = optarg;
			break;
		case 'Q':
			control.out_tmp = optarg;
			control.flags |= FLAG_KEEP_FILES;
			break;
		case 't':
			fatal("integrity checking currently not implemented\n");
			control.flags |= FLAG_TEST_ONLY;
			break;
		case 'f':
			control.flags |= FLAG_FORCE_REPLACE;
			break;
		case 'k':
			control.flags |= FLAG_KEEP_FILES;
			break;
		case 'v':
			control.verbosity++;
			break;
		case 'P':
			control.flags |= FLAG_SHOW_PROGRESS;
			break;
		case 'V':
			printf("rzip version %d.%d\n", 
			       RZIP_MAJOR_VERSION, RZIP_MINOR_VERSION);
			exit(0);
			break;

		default:
		case 'h':
			usage();
			return -1;
		}
	}

	argc -= optind;
	argv += optind;

	if (control.outname && argc > 1) {
		fatal("Cannot specify output filename with more than 1 file\n");
	}
	
	if (!control.outname && control.in_tmp && ! control.out_tmp) {
		fatal("Must specify output filename when reading from stdin\n");
	}
	
	if (control.outname && control.out_tmp) {
		fatal("Must not specify output filename when writing to stdout\n");
	}
	
	if (control.in_tmp && argc > 0) {
		fatal("Cannot specify -q with input files\n");
	}

	if (control.in_tmp)
		argc=1;

	if (argc < 1) {
		usage();
		exit(1);
	}

	for (i=0;i<argc;i++) {
		if (control.in_tmp)
			control.infile = "-";
		else
			control.infile = argv[i];

		if (control.flags & (FLAG_DECOMPRESS | FLAG_TEST_ONLY)) {
			decompress_file(&control);
		} else {
			compress_file(&control);
		}
	}

	return 0;
}
