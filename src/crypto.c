#include "crypto.h"

#include <SDL3/SDL.h>

static const Uint8 sbox[256] = {
   0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
   0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
   0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
   0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
   0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
   0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
   0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
   0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
   0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
   0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
   0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
   0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
   0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
   0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
   0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
   0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

/* Built once: inverse S-box and the encryption round tables (a whole
 * round per column in four lookups; Wii discs need tens of MB). */
static Uint8 inv_sbox[256];
static Uint32 te[4][256];
static SDL_InitState tables_state;

static Uint8 xtime(Uint8 x)
{
   return (Uint8)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static Uint8 mul(Uint8 a, Uint8 b)
{
   Uint8 r = 0;
   while (b)
   {
      if (b & 1)
         r ^= a;
      a = xtime(a);
      b >>= 1;
   }
   return r;
}

static void build_tables(void)
{
   int i;

   if (!SDL_ShouldInit(&tables_state))
      return;
   for (i = 0; i < 256; i++)
   {
      Uint8 s = sbox[i];
      Uint32 w = ((Uint32)xtime(s) << 24) | ((Uint32)s << 16) | ((Uint32)s << 8)
         | (Uint32)(xtime(s) ^ s);
      inv_sbox[s] = (Uint8)i;
      te[0][i] = w;
      te[1][i] = (w >> 8) | (w << 24);
      te[2][i] = (w >> 16) | (w << 16);
      te[3][i] = (w >> 24) | (w << 8);
   }
   SDL_SetInitialized(&tables_state, true);
}

static Uint32 load_be32(const Uint8 *p)
{
   return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | p[3];
}

static void store_be32(Uint8 *p, Uint32 v)
{
   p[0] = (Uint8)(v >> 24);
   p[1] = (Uint8)(v >> 16);
   p[2] = (Uint8)(v >> 8);
   p[3] = (Uint8)v;
}

void aes128_init(struct aes128 *aes, const Uint8 key[16])
{
   static const Uint8 rcon[10] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36 };
   Uint32 *w = aes->enc;
   int i;

   build_tables();
   for (i = 0; i < 4; i++)
      w[i] = load_be32(key + i * 4);
   for (i = 4; i < 44; i++)
   {
      Uint32 t = w[i - 1];
      if (i % 4 == 0)
         t = (((Uint32)sbox[(t >> 16) & 0xff] << 24) | ((Uint32)sbox[(t >> 8) & 0xff] << 16)
               | ((Uint32)sbox[t & 0xff] << 8) | sbox[t >> 24]) ^ ((Uint32)rcon[i / 4 - 1] << 24);
      w[i] = w[i - 4] ^ t;
   }
}

void aes128_encrypt_block(const struct aes128 *aes, const Uint8 in[16], Uint8 out[16])
{
   const Uint32 *rk = aes->enc;
   Uint32 s0 = load_be32(in) ^ rk[0], s1 = load_be32(in + 4) ^ rk[1];
   Uint32 s2 = load_be32(in + 8) ^ rk[2], s3 = load_be32(in + 12) ^ rk[3];
   Uint32 t0, t1, t2, t3;
   int round;

   for (round = 1; round < 10; round++)
   {
      rk += 4;
      t0 = te[0][s0 >> 24] ^ te[1][(s1 >> 16) & 0xff] ^ te[2][(s2 >> 8) & 0xff] ^ te[3][s3 & 0xff] ^ rk[0];
      t1 = te[0][s1 >> 24] ^ te[1][(s2 >> 16) & 0xff] ^ te[2][(s3 >> 8) & 0xff] ^ te[3][s0 & 0xff] ^ rk[1];
      t2 = te[0][s2 >> 24] ^ te[1][(s3 >> 16) & 0xff] ^ te[2][(s0 >> 8) & 0xff] ^ te[3][s1 & 0xff] ^ rk[2];
      t3 = te[0][s3 >> 24] ^ te[1][(s0 >> 16) & 0xff] ^ te[2][(s1 >> 8) & 0xff] ^ te[3][s2 & 0xff] ^ rk[3];
      s0 = t0;
      s1 = t1;
      s2 = t2;
      s3 = t3;
   }

   /* Last round: no MixColumns. */
   rk += 4;
#define LAST(a, b, c, d) \
   (((Uint32)sbox[(a) >> 24] << 24) | ((Uint32)sbox[((b) >> 16) & 0xff] << 16) \
      | ((Uint32)sbox[((c) >> 8) & 0xff] << 8) | sbox[(d) & 0xff])
   store_be32(out, LAST(s0, s1, s2, s3) ^ rk[0]);
   store_be32(out + 4, LAST(s1, s2, s3, s0) ^ rk[1]);
   store_be32(out + 8, LAST(s2, s3, s0, s1) ^ rk[2]);
   store_be32(out + 12, LAST(s3, s0, s1, s2) ^ rk[3]);
#undef LAST
}

static void add_round_key(Uint8 s[16], const Uint32 *rk)
{
   int i;
   for (i = 0; i < 4; i++)
   {
      s[i * 4] ^= (Uint8)(rk[i] >> 24);
      s[i * 4 + 1] ^= (Uint8)(rk[i] >> 16);
      s[i * 4 + 2] ^= (Uint8)(rk[i] >> 8);
      s[i * 4 + 3] ^= (Uint8)rk[i];
   }
}

static void inv_shift_sub(Uint8 s[16])
{
   Uint8 t[16];
   int c, r;

   /* Row r of column c came from column c - r. */
   for (c = 0; c < 4; c++)
      for (r = 0; r < 4; r++)
         t[((c + r) % 4) * 4 + r] = inv_sbox[s[c * 4 + r]];
   SDL_memcpy(s, t, 16);
}

/* Only used for a few hundred bytes of Switch headers: kept simple. */
void aes128_decrypt_block(const struct aes128 *aes, const Uint8 in[16], Uint8 out[16])
{
   Uint8 s[16];
   int round, c;

   SDL_memcpy(s, in, 16);
   add_round_key(s, aes->enc + 40);
   for (round = 9; round >= 1; round--)
   {
      inv_shift_sub(s);
      add_round_key(s, aes->enc + round * 4);
      for (c = 0; c < 4; c++)
      {
         Uint8 *p = s + c * 4, a = p[0], b = p[1], d = p[2], e = p[3];
         p[0] = mul(a, 14) ^ mul(b, 11) ^ mul(d, 13) ^ mul(e, 9);
         p[1] = mul(a, 9) ^ mul(b, 14) ^ mul(d, 11) ^ mul(e, 13);
         p[2] = mul(a, 13) ^ mul(b, 9) ^ mul(d, 14) ^ mul(e, 11);
         p[3] = mul(a, 11) ^ mul(b, 13) ^ mul(d, 9) ^ mul(e, 14);
      }
   }
   inv_shift_sub(s);
   add_round_key(s, aes->enc);
   SDL_memcpy(out, s, 16);
}

void aes128_cbc_encrypt(const struct aes128 *aes, const Uint8 iv[16], Uint8 *data, size_t size)
{
   const Uint8 *prev = iv;
   size_t i, j;

   for (i = 0; i + 16 <= size; i += 16)
   {
      for (j = 0; j < 16; j++)
         data[i + j] ^= prev[j];
      aes128_encrypt_block(aes, data + i, data + i);
      prev = data + i;
   }
}

void aes128_xts_decrypt(const Uint8 key[32], Uint64 first_sector, size_t sector_size,
      Uint8 *data, size_t size)
{
   struct aes128 data_key, tweak_key;
   size_t offset, i, j;
   Uint64 sector = first_sector;

   aes128_init(&data_key, key);
   aes128_init(&tweak_key, key + 16);
   for (offset = 0; offset + sector_size <= size; offset += sector_size, sector++)
   {
      Uint8 tweak[16] = { 0 };

      for (i = 0; i < 8; i++)
         tweak[15 - i] = (Uint8)(sector >> (i * 8));
      aes128_encrypt_block(&tweak_key, tweak, tweak);

      for (i = 0; i < sector_size; i += 16)
      {
         Uint8 *block = data + offset + i, carry;

         for (j = 0; j < 16; j++)
            block[j] ^= tweak[j];
         aes128_decrypt_block(&data_key, block, block);
         for (j = 0; j < 16; j++)
            block[j] ^= tweak[j];

         /* Next tweak: multiply by x in GF(2^128), little-endian. */
         carry = tweak[15] >> 7;
         for (j = 15; j > 0; j--)
            tweak[j] = (Uint8)((tweak[j] << 1) | (tweak[j - 1] >> 7));
         tweak[0] = (Uint8)((tweak[0] << 1) ^ (carry ? 0x87 : 0));
      }
   }
}

void aes128_ctr_crypt(const struct aes128 *aes, Uint8 ctr[16], Uint8 *data, size_t size)
{
   Uint8 stream[16];
   size_t i, j;

   for (i = 0; i + 16 <= size; i += 16)
   {
      aes128_encrypt_block(aes, ctr, stream);
      for (j = 0; j < 16; j++)
         data[i + j] ^= stream[j];
      for (j = 16; j-- > 0 && ++ctr[j] == 0;)
         ;
   }
}

/* --- SHA-1 --- */

static Uint32 rol(Uint32 x, int n)
{
   return (x << n) | (x >> (32 - n));
}

static void sha1_block(Uint32 h[5], const Uint8 *p)
{
   Uint32 w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f, k, t;
   int i;

   for (i = 0; i < 16; i++)
      w[i] = load_be32(p + i * 4);
   for (i = 16; i < 80; i++)
      w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

   for (i = 0; i < 80; i++)
   {
      if (i < 20)
      {
         f = (b & c) | (~b & d);
         k = 0x5a827999;
      }
      else if (i < 40)
      {
         f = b ^ c ^ d;
         k = 0x6ed9eba1;
      }
      else if (i < 60)
      {
         f = (b & c) | (b & d) | (c & d);
         k = 0x8f1bbcdc;
      }
      else
      {
         f = b ^ c ^ d;
         k = 0xca62c1d6;
      }
      t = rol(a, 5) + f + e + k + w[i];
      e = d;
      d = c;
      c = rol(b, 30);
      b = a;
      a = t;
   }
   h[0] += a;
   h[1] += b;
   h[2] += c;
   h[3] += d;
   h[4] += e;
}

void sha1(const void *data, size_t size, Uint8 digest[20])
{
   Uint32 h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
   const Uint8 *p = data;
   Uint8 last[128] = { 0 };
   size_t rest = size % 64, tail;
   Uint64 bits = (Uint64)size * 8;
   int i;

   for (; size >= 64; size -= 64, p += 64)
      sha1_block(h, p);

   /* Padding: 0x80, zeros, then the length in bits. */
   SDL_memcpy(last, p, rest);
   last[rest] = 0x80;
   tail = rest < 56 ? 64 : 128;
   for (i = 0; i < 8; i++)
      last[tail - 1 - i] = (Uint8)(bits >> (i * 8));
   sha1_block(h, last);
   if (tail == 128)
      sha1_block(h, last + 64);

   for (i = 0; i < 5; i++)
      store_be32(digest + i * 4, h[i]);
}
