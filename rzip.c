/* 
   Copyright (C) Andrew Tridgell 1998

   Modified to use flat hash, memory limit and variable hash culling
   by Rusty Russell copyright (C) 2003.
   
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
/* rzip compression algorithm */

#include "rzip.h"

#define CHUNK_MULTIPLE 100*1024*1024
#define CKSUM_CHUNK 1024*1024
#define GREAT_MATCH 1024
#define MINIMUM_MATCH 31

/* Hash table works as follows.  We start by throwing tags at every
 * offset into the table.  As it fills, we start eliminating tags
 * which don't have lower bits set to one (ie. first we eliminate all
 * even tags, then all tags divisible by four, etc.).  This ensures
 * that on average, all parts of the file are covered by the hash, if
 * sparsely. */
typedef uint32 tag;

/* All zero means empty.  We might miss the first chunk this way. */
struct hash_entry {
	uint32 offset;
	tag t;
};

/* Levels control hashtable size and bzip2 level. */
static const struct level {
	unsigned bzip_level;
	unsigned mb_used;
	unsigned initial_freq;
	unsigned max_chain_len;
} levels[10] = {
	{ 0, 1, 4, 1 },
	{ 1, 2, 4, 2 },
	{ 3, 4, 4, 2 },
	{ 5, 8, 4, 2 },
	{ 7, 16, 4, 3 },
	{ 9, 32, 4, 4 },
	{ 9, 32, 2, 6 },
	{ 9, 64, 1, 16 }, /* More MB makes sense, but need bigger test files */
	{ 9, 64, 1, 32 },
	{ 9, 64, 1, 128 },
};


struct rzip_state {
	struct rzip_control *control;
	void *ss;
	const struct level *level;
	tag hash_index[256];
	struct hash_entry *hash_table;
	unsigned int hash_bits;
	unsigned int hash_count;
	unsigned int hash_limit;
	tag minimum_tag_mask;
	unsigned int tag_clean_ptr;
	uchar *last_match;
	uint32 cksum;
	uint32 chunk_size;
	int fd_in, fd_out;
	struct {
		uint32 inserts;
		uint32 literals;
		uint32 literal_bytes;
		uint32 matches;
		uint32 match_bytes;
		uint32 tag_hits;
		uint32 tag_misses;
	} stats;
};

static inline void put_u8(void *ss, int stream, uchar b)
{
	if (write_stream(ss, stream, &b, 1) != 0) {
		fatal(NULL);
	}
}

static inline void put_u16(void *ss, int stream, unsigned s)
{
	put_u8(ss, stream, s & 0xFF);
	put_u8(ss, stream, (s>>8) & 0xFF);
}

static inline void put_uint32(void *ss, int stream, unsigned s)
{
	put_u8(ss, stream, s & 0xFF);
	put_u8(ss, stream, (s>>8) & 0xFF);
	put_u8(ss, stream, (s>>16) & 0xFF);
	put_u8(ss, stream, (s>>24) & 0xFF);
}

static int tmp_in_chunk(int fd_in,int chunk)
{
	ssize_t r,w;
	size_t l=0;
	static char buf[64*1024];

	while(chunk-l>0 && (r=read(STDIN_FILENO,buf,MIN(sizeof(buf),chunk-l)))>0) {
		l+=r;
		w=write(fd_in,buf,r);
		if(w<0) 
			fatal("cannot write to temporary file: %s\n",strerror(errno));
		if(w!=r) 
			fatal("partial write?!\n");
	}
	if(r<0) {
		fatal("cannot read from stdin: %s\n",strerror(errno));
	}
	if(lseek(fd_in,0,SEEK_SET)==-1)
		fatal("failed to seek");
	return l;
}

static void put_header(void *ss, uchar head, int len)
{
	put_u8(ss, 0, head);
	put_u16(ss, 0, len);
}


static void put_match(struct rzip_state *st, uchar *p, uchar *buf, uint32 offset, int len)
{
	do {
		unsigned ofs;
		int n = len;
		if (n > 0xFFFF) n = 0xFFFF;

		ofs = (uint32)(p - (buf+offset));
		put_header(st->ss, 1, n);
		put_uint32(st->ss, 0, ofs);

		st->stats.matches++;
		st->stats.match_bytes += n;
		len -= n;
		p += n;
		offset += n;
	} while (len);
}

static void put_literal(struct rzip_state *st, uchar *last, uchar *p)
{
	do {
		int len = (int)(p - last);
		if (len > 0xFFFF) len = 0xFFFF;

		st->stats.literals++;
		st->stats.literal_bytes += len;

		put_header(st->ss, 0, len);

		if (len && write_stream(st->ss, 1, last, len) != 0) {
			fatal(NULL);
		}
		last += len;
	} while (p > last);
}

/* Could give false positive on offset 0.  Who cares. */
static int empty_hash(struct rzip_state *st, unsigned int h)
{
	return !st->hash_table[h].offset && !st->hash_table[h].t;
}

static unsigned int primary_hash(struct rzip_state *st, tag t)
{
	return t & ((1 << st->hash_bits) - 1);
}

static inline tag increase_mask(tag tag_mask)
{
	/* Get more precise. */
	return (tag_mask << 1) | 1;
}

static int minimum_bitness(struct rzip_state *st, tag t)
{
	tag better_than_min = increase_mask(st->minimum_tag_mask);
	if ((t & better_than_min) != better_than_min)
		return 1;
	return 0;
}

/* Is a going to be cleaned before b?  ie. does a have fewer low bits
 * set than b? */
static int lesser_bitness(tag a, tag b)
{
	tag mask;

	for (mask = 0; mask != (tag)-1; mask = ((mask<<1)|1)) {
		if ((a & b & mask) != mask)
			break;
	}
	return ((a & mask) < (b & mask));
}

/* If hash bucket is taken, we spill into next bucket(s).  Secondary hashing
   works better in theory, but modern caches make this 20% faster. */
static void insert_hash(struct rzip_state *st, tag t, uint32 offset)
{
	unsigned int h, victim_h = 0, round = 0;
	/* If we need to kill one, this will be it. */
	static int victim_round = 0;

	h = primary_hash(st, t);
	while (!empty_hash(st, h)) {
		/* If this due for cleaning anyway, just replace it:
		   rehashing might move it behind tag_clean_ptr. */
		if (minimum_bitness(st, st->hash_table[h].t)) {
			st->hash_count--;
			break;
		}
		/* If we are better than current occupant, we can't
		   jump over it: it will be cleaned before us, and
		   noone would then find us in the hash table.  Rehash
		   it, then take its place. */
		if (lesser_bitness(st->hash_table[h].t, t)) {
			insert_hash(st, st->hash_table[h].t,
				    st->hash_table[h].offset);
			break;
		}

		/* If we have lots of identical patterns, we end up
		   with lots of the same hash number.  Discard random. */
		if (st->hash_table[h].t == t) {
			if (round == victim_round) {
				victim_h = h;
			}
			if (++round == st->level->max_chain_len) {
				h = victim_h;
				st->hash_count--;
				victim_round++;
				if (victim_round == st->level->max_chain_len)
					victim_round = 0;
				break;
			}
		}

		h++;
		h &= ((1 << st->hash_bits) - 1);
	}

	st->hash_table[h].t = t;
	st->hash_table[h].offset = offset;
}

/* Eliminate one hash entry with minimum number of lower bits set.
   Returns tag requirement for any new entries. */
static tag clean_one_from_hash(struct rzip_state *st)
{
	tag better_than_min;

again:
	better_than_min = increase_mask(st->minimum_tag_mask);
	if (st->control->verbosity > 1) {
		if (!st->tag_clean_ptr)
			printf("Starting sweep for mask %u\n",
			       st->minimum_tag_mask);
	}

	for (; st->tag_clean_ptr < (1<<st->hash_bits); st->tag_clean_ptr++) {
		if (empty_hash(st, st->tag_clean_ptr))
			continue;
		if ((st->hash_table[st->tag_clean_ptr].t & better_than_min)
		    != better_than_min) {
			st->hash_table[st->tag_clean_ptr].offset = 0;
			st->hash_table[st->tag_clean_ptr].t = 0;
			st->hash_count--;
			return better_than_min;
		}
	}

	/* We hit the end: everthing in hash satisfies the better mask. */
	st->minimum_tag_mask = better_than_min;
	st->tag_clean_ptr = 0;
	goto again;
}

static inline tag next_tag(struct rzip_state *st, uchar *p, tag t)
{
	t ^= st->hash_index[p[-1]];
	t ^= st->hash_index[p[MINIMUM_MATCH-1]];
	return t;
}

static inline tag full_tag(struct rzip_state *st, uchar *p)
{
	tag ret = 0;
	int i;
	for (i=0;i<MINIMUM_MATCH;i++) {
		ret ^= st->hash_index[p[i]];
	}
	return ret;
}

static inline int match_len(struct rzip_state *st, 
			    uchar *p0, uchar *op, uchar *buf, uchar *end, int *rev)
{
	uchar *p = p0;
	int len = 0;

	if (op >= p0) return 0;

	while ((*p == *op) && (p < end)) {
		p++; op++;
	}
	len = p - p0;

	p = p0;
	op -= len;

	end = buf;
	if (end < st->last_match) end = st->last_match;

	while (p > end && op > buf && op[-1] == p[-1]) {
		op--; p--;
	}

	(*rev) = p0 - p;
	len += p0 - p;

	if (len < MINIMUM_MATCH) return 0;

	return len;
}

static int find_best_match(struct rzip_state *st,
			   tag t, uchar *p, uchar *buf, uchar *end, 
			   uint32 *offset, int *reverse, int current_len)
{
	int length = 0;
	int rev;
	unsigned int h, best_h;

	rev = 0;
	(*reverse) = 0;

	/* Could optimize: if lesser goodness, can stop search.  But
	 * chains are usually short anyway. */
	h = primary_hash(st, t);
	while (!empty_hash(st, h)) {
		int mlen;

		if (t == st->hash_table[h].t) {
			mlen = match_len(st, p, buf+st->hash_table[h].offset,
					 buf, end, &rev);

			if (mlen)
				st->stats.tag_hits++;
			else
				st->stats.tag_misses++;

			if (mlen >= length) {
				length = mlen;
				(*offset) = st->hash_table[h].offset - rev;
				(*reverse) = rev;
				best_h = h;
			}
		}

		h++;
		h &= ((1 << st->hash_bits) - 1);
	}

	return length;
}

static void show_distrib(struct rzip_state *st)
{
	int i;
	uint32 total = 0;
	uint32 primary = 0;

	for (i=0;i<(1 << st->hash_bits);i++) {
		if (empty_hash(st, i))
			continue;
		total++;
		if (primary_hash(st, st->hash_table[i].t) == i)
			primary++;
	}

	if (total != st->hash_count)
		printf("WARNING: hash_count says total %u\n", st->hash_count);

	printf("%d total hashes\n", total);
	printf("%d in primary bucket (%-2.3f%%)\n", primary,
	       primary*100.0/total);
}

static void hash_search(struct rzip_state *st, uchar *buf, 
			double pct_base, double pct_multiple)
{
	uchar *p, *end;
	tag t = 0;
	uint32 cksum_limit = 0;
	int pct, lastpct=0;
	struct {
		uchar *p;
		uint32 ofs;
		int len;
	} current;
	tag tag_mask = (1 << st->level->initial_freq)-1;

	if (st->hash_table) {
		memset(st->hash_table, 0,
		       sizeof(st->hash_table[0]) * (1<<st->hash_bits));
	} else {
		uint32 hashsize = st->level->mb_used
			* (1024*1024 / sizeof(st->hash_table[0]));
		for (st->hash_bits = 0;
		     (1<<st->hash_bits) < hashsize;
		     st->hash_bits++);

		if (st->control->verbosity > 1)
			printf("hashsize = %u.  bits = %u. %uMB\n",
			       hashsize, st->hash_bits, st->level->mb_used);

		/* 66% full at max. */
		st->hash_limit = (1<<st->hash_bits)/3 * 2;
		st->hash_table = calloc(sizeof(st->hash_table[0]),
					(1<<st->hash_bits));
	}

	if (!st->hash_table) {
		fatal("Failed to allocate hash table in hash_search\n");
	}

	st->minimum_tag_mask = tag_mask;
	st->tag_clean_ptr = 0;
	st->cksum = 0;
	st->hash_count = 0;

	p = buf;
	end = buf + st->chunk_size - MINIMUM_MATCH;
	st->last_match = p;
	current.len = 0;
	current.p = p;
	current.ofs = 0;

	t = full_tag(st, p);

	while (p < end) {
		uint32 offset;
		int mlen, reverse;

		p++;
		t = next_tag(st, p, t);

		/* Don't look for a match if there are no tags with
		   this number of bits in the hash table. */
		if ((t & st->minimum_tag_mask) != st->minimum_tag_mask)
			continue;

		mlen = find_best_match(st, t, p, buf, end, 
				       &offset, &reverse, current.len);

		/* Only insert occasionally into hash. */
		if ((t & tag_mask) == tag_mask) {
			st->stats.inserts++;
			st->hash_count++;
			insert_hash(st, t, (uint32)(p - buf));
			if (st->hash_count > st->hash_limit)
				tag_mask = clean_one_from_hash(st);
		}

		if (mlen > current.len) {
			current.p = p - reverse;
			current.len = mlen;
			current.ofs = offset;
		}

		if ((current.len >= GREAT_MATCH || p>=current.p+MINIMUM_MATCH)
		    && current.len >= MINIMUM_MATCH) {
			if (st->last_match < current.p)
				put_literal(st, st->last_match, current.p);
			put_match(st, current.p, buf, current.ofs, current.len);
			st->last_match = current.p + current.len;
			current.p = p = st->last_match;
			current.len = 0;
			t = full_tag(st, p);
		}

		if ((st->control->flags & FLAG_SHOW_PROGRESS) && (p-buf) % 100 == 0) {
			pct = pct_base + (pct_multiple * (100.0*(p-buf))/st->chunk_size);
			if (pct != lastpct) {
				struct stat s1, s2;
				fstat(st->fd_in, &s1);
				fstat(st->fd_out, &s2);
				printf("%s %2d%%\r", 
				       st->control->infile, pct);
				fflush(stdout);
				lastpct = pct;
			}
		}

		if ((p-buf) > cksum_limit) {
			int n = st->chunk_size - (p-buf);
			st->cksum = crc32_buffer(buf+cksum_limit, n, st->cksum);
			cksum_limit += n;
		}
	}


	if (st->control->verbosity > 1) {
		show_distrib(st);
	}

	if (st->last_match < buf + st->chunk_size) {
		put_literal(st, st->last_match,buf + st->chunk_size);
	}

	if (st->chunk_size > cksum_limit) {
		int n = st->chunk_size - cksum_limit;
		st->cksum = crc32_buffer(buf+cksum_limit, n, st->cksum);
		cksum_limit += n;
	}

	put_literal(st, NULL,0);
	put_uint32(st->ss, 0, st->cksum);
}


static void init_hash_indexes(struct rzip_state *st)
{
	int i;
	for (i=0;i<256;i++) {
		st->hash_index[i] = ((random()<<16) ^ random());
	}
}

/* compress a chunk of an open file. Assumes that the file is able to
   be mmap'd and is seekable */
static void rzip_chunk(struct rzip_state *st, int fd_in, int fd_out, off_t offset, 
		       double pct_base, double pct_multiple, int outpiped)
{
	uchar *buf;

	buf = (uchar *)mmap(NULL,st->chunk_size,PROT_READ,MAP_SHARED,fd_in,offset);
	if (buf == (uchar *)-1) {
		fatal("Failed to map buffer in rzip_fd\n");
	}

	st->ss = open_stream_out(fd_out, NUM_STREAMS, st->level->bzip_level, outpiped);
	if (!st->ss) {
		fatal("Failed to open streams in rzip_fd\n");
	}
	hash_search(st, buf, pct_base, pct_multiple);
	if (close_stream_out(st->ss) != 0) {
		fatal("Failed to flush/close streams in rzip_fd\n");
	}
	munmap(buf, st->chunk_size);
}


/* compress a whole file chunks at a time */
off_t rzip_fd(struct rzip_control *control, int fd_in, int fd_out)
{
	struct stat s, s2;
	int progress= control->flags & FLAG_SHOW_PROGRESS;
	off_t len, total_len=0;
	struct rzip_state *st;
	int outpiped=control->out_tmp?1:0;

	st = calloc(sizeof(*st), 1);
	if (!st) {
		fatal("Failed to allocate control state in rzip_fd\n");
	}

	st->level = &levels[MIN(9, control->compression_level)];
	st->control = control;
	st->fd_in = fd_in;
	st->fd_out = fd_out;

	init_hash_indexes(st);

	if(!control->in_tmp) {
		if (fstat(fd_in, &s)) {
			fatal("Failed to stat fd_in in rzip_fd - %s\n", strerror(errno));
		}
		len = s.st_size;
	} else {
		len = 1;
		control->flags &= ~FLAG_SHOW_PROGRESS;
	}

	if(!control->out_tmp) {
		control->flags &= ~FLAG_SHOW_PROGRESS;
	}

	while (len) {
		int chunk;
		double pct_base, pct_multiple;

		if (control->compression_level == 0) {
			chunk = CHUNK_MULTIPLE;
		} else {
			chunk = control->compression_level * CHUNK_MULTIPLE;
		}

		if(control->in_tmp) {
			len=chunk;
			chunk=tmp_in_chunk(fd_in,chunk);
			if(chunk<len)
				len=0;

			st->chunk_size = chunk;

			rzip_chunk(st, fd_in, fd_out, 0, pct_base, pct_multiple, outpiped);
		} else {
			if (chunk > len) chunk = len;

			pct_base = (100.0 * (s.st_size - len)) / s.st_size;
			pct_multiple = ((double)chunk) / s.st_size;

			st->chunk_size = chunk;

			rzip_chunk(st, fd_in, fd_out, s.st_size - len, pct_base, pct_multiple, outpiped);
			len -= chunk;
		}

		total_len+=chunk;
	}


	if (st->control->verbosity > 1) {
		printf("matches=%d match_bytes=%d\n", 
		       st->stats.matches, st->stats.match_bytes);
		printf("literals=%d literal_bytes=%d\n", 
		       st->stats.literals, st->stats.literal_bytes);
		printf("true_tag_positives=%d false_tag_positives=%d\n", 
		       st->stats.tag_hits, st->stats.tag_misses);
		printf("inserts=%d match %.3f\n", 
		       st->stats.inserts,
		       (1.0 + st->stats.match_bytes) / st->stats.literal_bytes);
	}

	if(!control->out_tmp) {
		fstat(fd_out, &s2);

		if (progress ||
		    st->control->verbosity > 0) {
			printf("%s - compression ratio %.3f\n", 
			       st->control->infile, 1.0 * total_len / s2.st_size);
		}
	}

	if (st->hash_table) {
		free(st->hash_table);
	}
	free(st);

	return total_len;
}
