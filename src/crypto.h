#ifndef FUSEBOX_CRYPTO_H
#define FUSEBOX_CRYPTO_H

#include <SDL3/SDL_stdinc.h>

/* Just enough crypto to identify games: AES-128 to rebuild Wii discs
 * from RVZ and to read Switch NCAs, SHA-1 for Wii hash trees. */

struct aes128
{
   Uint32 enc[44];
};

void aes128_init(struct aes128 *aes, const Uint8 key[16]);
void aes128_encrypt_block(const struct aes128 *aes, const Uint8 in[16], Uint8 out[16]);
void aes128_decrypt_block(const struct aes128 *aes, const Uint8 in[16], Uint8 out[16]);
/* In place; size is a multiple of 16. */
void aes128_cbc_encrypt(const struct aes128 *aes, const Uint8 iv[16], Uint8 *data, size_t size);
/* In place; ctr is a big-endian 128-bit counter, advanced per block
 * (Switch NCA sections). size is a multiple of 16. */
void aes128_ctr_crypt(const struct aes128 *aes, Uint8 ctr[16], Uint8 *data, size_t size);
/* Nintendo's XTS (big-endian sector numbers), as used for Switch NCA
 * headers. key is data key then tweak key; size is a multiple of
 * sector_size. */
void aes128_xts_decrypt(const Uint8 key[32], Uint64 first_sector, size_t sector_size,
      Uint8 *data, size_t size);

void sha1(const void *data, size_t size, Uint8 digest[20]);

#endif
