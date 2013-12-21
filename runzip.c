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
/* rzip decompression algorithm */

#include "rzip.h"

static inline uchar read_u8(void *ss, int stream)
{
	uchar b;
	if (read_stream(ss, stream, &b, 1) != 1) {
		fatal("Stream read failed\n");
	}
	return b;
}

static inline unsigned read_u16(void *ss, int stream)
{
	unsigned ret;
	ret = read_u8(ss, stream);
	ret |= read_u8(ss, stream)<<8;
	return ret;
}

static inline unsigned read_u32(void *ss, int stream)
{
	unsigned ret;
	ret = read_u8(ss, stream);
	ret |= read_u8(ss, stream)<<8;
	ret |= read_u8(ss, stream)<<16;
	ret |= read_u8(ss, stream)<<24;
	return ret;
}

static inline unsigned read_u24(void *ss, int stream)
{
	unsigned ret;
	ret = read_u8(ss, stream);
	ret |= read_u8(ss, stream)<<8;
	ret |= read_u8(ss, stream)<<16;
	return ret;
}


static int read_header(void *ss, uchar *head)
{
	*head = read_u8(ss, 0);
	return read_u16(ss, 0);
}


static int unzip_literal(void *ss, int len, int fd_out, uint32 *cksum)
{
	uchar *buf;

	buf = malloc(len);
	if (!buf) {
		fatal("Failed to allocate literal buffer of size %d\n", len);
	}

	read_stream(ss, 1, buf, len);
	if (write(fd_out, buf, len) != len) {
		fatal("Failed to write literal buffer of size %d\n", len);
	}

	*cksum = crc32_buffer(buf, len, *cksum);

	free(buf);
	return len;
}

static int unzip_match(void *ss, int len, int fd_out, int fd_hist, uint32 *cksum)
{
	unsigned offset;
	int n, total=0;
	off_t cur_pos = lseek(fd_out, 0, SEEK_CUR);
	offset = read_u32(ss, 0);

	if (lseek(fd_hist, cur_pos-offset, SEEK_SET) == (off_t)-1) {
		fatal("Seek failed by %d from %d on history file in unzip_match - %s\n", 
		      offset, cur_pos, strerror(errno));
	}

	while (len) {
		uchar *buf;
		n = MIN(len, offset);

		buf = malloc(n);
		if (!buf) {
			fatal("Failed to allocate %d bytes in unzip_match\n", n);
		}

		if (read(fd_hist, buf, n) != n) {
			fatal("Failed to read %d bytes in unzip_match\n", n);
		}

		if (write(fd_out, buf, n) != n) {
			fatal("Failed to write %d bytes in unzip_match\n", n);
		}

		*cksum = crc32_buffer(buf, n, *cksum);

		len -= n;
		free(buf);
		total += n;
	}

	return total;
}


/* decompress a section of an open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
static int runzip_chunk(int fd_in, int fd_out, int fd_hist)
{
	uchar head;
	int len;
	struct stat st;
	void *ss;
	off_t ofs;
	int total = 0;
	uint32 good_cksum, cksum = 0;
	
	ofs = lseek(fd_in, 0, SEEK_CUR);
	if (ofs == (off_t)-1) {
		fatal("Failed to seek input file in runzip_fd\n");
	}

	if (fstat(fd_in, &st) != 0 || st.st_size-ofs == 0) {
		return 0;
	}

	ss = open_stream_in(fd_in, NUM_STREAMS);
	if (!ss) {
		fatal(NULL);
	}

	while ((len = read_header(ss, &head)) || head) {
		switch (head) {
		case 0:
			total += unzip_literal(ss, len, fd_out, &cksum);
			break;

		default:
			total += unzip_match(ss, len, fd_out, fd_hist, &cksum);
			break;
		}
	}

	good_cksum = read_u32(ss, 0);
	if (good_cksum != cksum) {
		fatal("Bad checksum 0x%08x - expected 0x%08x\n", cksum, good_cksum);
	}

	if (close_stream_in(ss) != 0) {
		fatal("Failed to close stream!\n");
	}

	return total;
}

/* decompress a open file. Call fatal() on error
   return the number of bytes that have been retrieved
 */
off_t runzip_fd(int fd_in, int fd_out, int fd_hist, off_t expected_size)
{
	off_t total = 0;
	while (total < expected_size) {
		total += runzip_chunk(fd_in, fd_out, fd_hist);
	}
	return total;
}

