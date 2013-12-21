/*  Example piece of code to demonstrate how to find a match in a
    stream, without buffering, for use in rzip on *really* huge files.
    
    Copyright (C) 2003  Rusty Russell, IBM Corporation

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
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#include <stdio.h>
#include <stdbool.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include "md4.h"

struct hash_entry {
	uint64_t offset;
	uchar bitness;
	uchar md4[MD4_DIGEST_SIZE];
};

typedef uint32_t tag;

#define HASH_BITS 10
#define MINIMUM_BITNESS 1
#define MAXIMUM_BITNESS 32 /* need to increase tag size to increase this */
#define WINDOW_LENGTH 31

/* Simple optimization for read() */
#define BUFFER_SIZE 4096

static struct hash_entry hash[1 << HASH_BITS];
static const unsigned int hash_limit = (1 << HASH_BITS) * 2 / 3;
static unsigned int hash_min_bitness = MINIMUM_BITNESS;
static unsigned int hash_count;
static unsigned int hash_clean_ptr;
static tag hash_index[256];
static int verbosity = 2;

static void barf_perror(const char *fmt, ...)
{
	char *str = strerror(errno);
	va_list arglist;

	fprintf(stderr, "FATAL: ");

	va_start(arglist, fmt);
	vfprintf(stderr, fmt, arglist);
	va_end(arglist);

	fprintf(stderr, ": %s\n", str);
	exit(1);
}

static int bitness(tag t)
{
	return ffs(t) ?: 32;
}

static int empty_hash(unsigned int h)
{
	return !hash[h].bitness;
}

static uint32_t primary_hash(const struct hash_entry *h)
{
	return h->md4[1] & ((1 << HASH_BITS) - 1);
}

static inline int hash_equals(const struct hash_entry *a,
			      const struct hash_entry *b)
{
	return memcmp(a->md4, b->md4, sizeof(a->md4)) == 0;
}

/* Eliminate one hash entry of minimum bitness. */
static void clean_one_from_hash(void)
{
again:
	if (verbosity > 1) {
		if (!hash_clean_ptr)
			printf("Starting sweep for mask %u\n",
			       hash_min_bitness);
	}

	for (; hash_clean_ptr < (1<<HASH_BITS); hash_clean_ptr++) {
		if (empty_hash(hash_clean_ptr))
			continue;
		if (hash[hash_clean_ptr].bitness <= hash_min_bitness) {
			hash[hash_clean_ptr].bitness = 0;
			hash_count--;
			return;
		}
	}

	/* We hit the end: everything in hash satisfies the better mask. */
	hash_min_bitness++;
	hash_clean_ptr = 0;
	goto again;
}

/* If we find a duplicate, return that instead of inserting. */
static struct hash_entry *insert_hash(const struct hash_entry *new)
{
	unsigned int h;

	/* If hash bucket is taken, we spill into next bucket(s).
	   Secondary hashing works better in theory, but modern caches
	   make this 20% faster. */

	h = primary_hash(new);
	while (!empty_hash(h)) {
		if (hash_equals(&hash[h], new))
			return &hash[h];

		/* If this due for cleaning anyway, just replace it:
		   rehashing might move it behind tag_clean_ptr. */
		if (hash[h].bitness == hash_min_bitness) {
			hash_count--;
			break;
		}
		/* If we are better than current occupant, we can't
		   jump over it: it will be cleaned before us, and
		   noone would then find us in the hash table.  Rehash
		   it, and take its place. */
		if (hash[h].bitness < new->bitness) {
			struct hash_entry old = hash[h];
			hash[h] = *new;
			insert_hash(&old);
			return NULL;
		}

		h++;
		h &= ((1 << HASH_BITS) - 1);
	}

	hash[h] = *new;
	if (++hash_count > hash_limit)
		clean_one_from_hash();
	return NULL;
}

static inline tag next_tag(uchar old, uchar new, tag t)
{
	t ^= hash_index[old];
	t ^= hash_index[new];
	return t;
}

static inline tag full_tag(uchar *p, unsigned int len)
{
	tag ret = 0;
	int i;
	for (i=0;i<len;i++) {
		ret ^= hash_index[p[i]];
	}
	return ret;
}

static void found_match(int fd_in, off_t len, off_t off, off_t old_off)
{
	char olddata[len], newdata[len];
	off_t current_off;

	/* Ignore overlapping matches. */
	if (old_off + len <= off) {
		printf("match: %llu %llu %llu\n",
		       (long long)old_off,
		       (long long)off,
		       (long long)len);
	}

	current_off = lseek(fd_in, 0, SEEK_CUR);
	lseek(fd_in, old_off, SEEK_SET);
	read(fd_in, olddata, len);
	lseek(fd_in, off, SEEK_SET);
	read(fd_in, newdata, len);
	lseek(fd_in, current_off, SEEK_SET);

	assert(memcmp(olddata, newdata, len) == 0);
}

static void strong_sums(unsigned char *data,
			struct md4_ctx md4[],
			unsigned int len)
{
	unsigned int i;

	for (i = hash_min_bitness; i <= MAXIMUM_BITNESS; i++)
		md4_update(&md4[i], data, len);
}

bool debug;

static off_t stream_search(int fd_in)
{
	unsigned int i, doff, dlen, laststrong;
	off_t off, lastlen;
	unsigned char data[BUFFER_SIZE];
	struct md4_ctx md4[MAXIMUM_BITNESS+1];
	off_t start[MAXIMUM_BITNESS+1];
	tag t;
	int r;

	/* Refill window */
	r = read(fd_in, data, BUFFER_SIZE);
	if (r < 0)
		barf_perror("read of input failed: %s");

	if (r <= WINDOW_LENGTH)
		return t;

	t = full_tag(data, WINDOW_LENGTH);

	/* Initialize the strong checksums */
	for (i = hash_min_bitness; i <= MAXIMUM_BITNESS; i++) {
		md4_init(&md4[i]);
		start[i] = 0;
	}
	dlen = r - WINDOW_LENGTH;
	doff = WINDOW_LENGTH;
	off = WINDOW_LENGTH;
	laststrong = 0;

	while (dlen > 0) {
		assert(doff >= WINDOW_LENGTH);
		/* Make sure we always have some buffer. */
		if (dlen == 1) {
			/* Keep WINDOW_LENGTH behind at all times. */
			unsigned int newstart, keep;

			newstart = doff - WINDOW_LENGTH;
			keep =  WINDOW_LENGTH + dlen;
			if (laststrong < newstart) {
				strong_sums(data+laststrong, md4,
					    newstart-laststrong);
				laststrong = 0;
			} else
				laststrong -= newstart;

			memmove(data, data+newstart, keep);
			doff = WINDOW_LENGTH;
			r = read(fd_in, data+keep, BUFFER_SIZE-keep);
			if (r < 0)
				barf_perror("read of input failed: %s");
			dlen += r;
		}
		t = next_tag(data[doff-WINDOW_LENGTH], data[doff], t);

		if (bitness(t) > hash_min_bitness) {
			/* Update strong sums. */
			strong_sums(data+laststrong, md4, doff-laststrong);
			if (debug) 
				printf("Adding: %lu - %lu %.*s\n",
				       laststrong, doff,
				       doff-laststrong,
				       data+laststrong);
				       
			laststrong = doff;
			
			/* End each block of sufficient bitness. */
			lastlen = 0;
			for (i = bitness(t); i >= hash_min_bitness; i--) {
				struct hash_entry he, *old;

				md4_final(&md4[i], (unsigned char *)he.md4);
				he.bitness = i;
				he.offset = start[i];

				/* Don't put in the same hash twice. */
				if (off - start[i] != lastlen) {
					if (verbosity > 1)
						printf("%i: Putting %lu@%lu"
						       " in hash\n",
						       i, off - start[i],
						       start[i]);
					old = insert_hash(&he);
					lastlen = off - start[i];
					if (old) {
						found_match(fd_in,
							    lastlen,
							    start[i],
							    old->offset);
					}
				}

				/* Start new strong hash, from *start* of
				   window (ie. overlaps last one). */
				md4_init(&md4[i]);
				md4_update(&md4[i],
					   data + doff - WINDOW_LENGTH,
					   WINDOW_LENGTH);
				start[i] = off - WINDOW_LENGTH;
				if (start[i] == 28039)
					debug = true;

				if (debug)
					printf("Starting %lu: %.*s\n",
					       start[i], 
					       WINDOW_LENGTH,
					       data + doff - WINDOW_LENGTH);
			}
		}
		doff++;
		dlen--;
		off++;
	}
	return off;
}

static void init_hash_indexes(void)
{
	int i;
	for (i=0;i<256;i++) {
		hash_index[i] = ((random()<<16) ^ random());
	}
}

int main(int argc, char *argv[])
{
	unsigned int i;

	init_hash_indexes();

	for (i = 1; i < argc; i++) {
		off_t len;
		int fd = open(argv[i], O_RDONLY);
		if (fd < 0)
			barf_perror("Opening %s", argv[i]);
		
		len = stream_search(fd);
		close(fd);
		printf("Length of %s: %llu\n", argv[i], (long long)len);
		
	}
	return 0;
}
