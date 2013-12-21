/* 
   Copyright (C) Andrew Tridgell 1998
   
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

#define RZIP_MAJOR_VERSION 2
#define RZIP_MINOR_VERSION 1

#define NUM_STREAMS 2

#define _GNU_SOURCE

#include "config.h"

#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>

#ifdef HAVE_STRING_H
#include <string.h>
#endif

#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

#include <fcntl.h>
#include <sys/stat.h>

#ifdef HAVE_CTYPE_H
#include <ctype.h>
#endif
#include <errno.h>
#include <sys/mman.h>

#ifndef uchar
#define uchar unsigned char
#endif

#ifndef int32
#if (SIZEOF_INT == 4)
#define int32 int
#elif (SIZEOF_LONG == 4)
#define int32 long
#elif (SIZEOF_SHORT == 4)
#define int32 short
#endif
#endif

#ifndef int16
#if (SIZEOF_INT == 2)
#define int16 int
#elif (SIZEOF_SHORT == 2)
#define int16 short
#endif
#endif

#ifndef uint32
#define uint32 unsigned int32
#endif

#ifndef uint16
#define uint16 unsigned int16
#endif

#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif

#if !HAVE_STRERROR
extern char *sys_errlist[];
#define strerror(i) sys_errlist[i]
#endif

#ifndef HAVE_ERRNO_DECL
extern int errno;
#endif


#define FLAG_SHOW_PROGRESS 2
#define FLAG_KEEP_FILES 4
#define FLAG_TEST_ONLY 8
#define FLAG_FORCE_REPLACE 16
#define FLAG_DECOMPRESS 32


struct rzip_control {
	const char *infile, *outname;
	const char *in_tmp, *out_tmp;
	char *outfile;
	const char *suffix;
	unsigned compression_level;
	unsigned flags;
	unsigned verbosity;
};

void fatal(const char *format, ...);
void err_msg(const char *format, ...);
off_t runzip_fd(int fd_in, int fd_out, int fd_hist, off_t expected_size, int out_is_pipe, int in_is_pipe);
off_t rzip_fd(struct rzip_control *control, int fd_in, int fd_out);
void *open_stream_out(int f, int n, int bzip_level, int piped);
void *open_stream_in(int f, int n, int piped, int *eof);
int write_stream(void *ss, int stream, uchar *p, int len);
int read_stream(void *ss, int stream, uchar *p, int len);
int close_stream_out(void *ss);
int close_stream_in(void *ss);
void *Realloc(void *p, int size);
uint32 crc32_buffer(const uchar *buf, int n, uint32 crc);
