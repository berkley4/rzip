#ifndef _RZIP_MD4_H
#define _RZIP_MD4_H
#include <stdint.h>

#define MD4_DIGEST_SIZE		16
#define MD4_HMAC_BLOCK_SIZE	64
#define MD4_BLOCK_WORDS		16
#define MD4_HASH_WORDS		4

typedef unsigned char uchar;

struct md4_ctx {
	uint32_t hash[MD4_HASH_WORDS];
	uint32_t block[MD4_BLOCK_WORDS];
	uint64_t byte_count;
};
void md4_init(struct md4_ctx *mctx);

void md4_update(struct md4_ctx *mctx, const uchar *data, unsigned int len);

void md4_final(struct md4_ctx *mctx, uchar *out);

#endif /* _RZIP_MD4_H */
