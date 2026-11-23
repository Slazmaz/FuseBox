#include "disc.h"
#include "crypto.h"

#include <SDL3/SDL.h>
#include <zlib.h>
#include <zstd.h>

/* Wii discs are 32 KiB blocks of 1 KiB hashes + 31 KiB data, hashed and
 * encrypted in groups of 64 blocks. */
#define BLOCK_SIZE       0x8000
#define BLOCK_DATA       0x7c00
#define BLOCK_HASHES     0x400
#define GROUP_BLOCKS     64
#define GROUP_SIZE       (BLOCK_SIZE * GROUP_BLOCKS)
#define GROUP_DATA       (BLOCK_DATA * GROUP_BLOCKS)

#define GC_MAGIC  0xc2339f3d
#define WII_MAGIC 0x5d1c9ea3
#define GCZ_MAGIC 0xb10bc001

/* WIA/RVZ compression methods (RVZ adds zstd). */
#define WIA_NONE 0
#define WIA_ZSTD 5

/* Largest hash exception list of one 2 MiB group: one per 20 bytes of
 * every hash block, 22 bytes each, plus the count. */
#define MAX_EXCEPTION_LIST ((BLOCK_HASHES + 19) / 20 * GROUP_BLOCKS * 22 + 2)
#define LFG_K 521
#define LFG_SEED 17

enum format
{
   FORMAT_PLAIN,
   FORMAT_GCZ,
   FORMAT_WIA,
   FORMAT_RVZ
};

struct wia_part
{
   Uint8 key[16];
   struct
   {
      Uint32 first_sector, sectors, group_index, groups;
   } data[2];
};

struct wia_raw
{
   Uint64 offset, size;
   Uint32 group_index, groups;
};

struct wia_group
{
   Uint64 file_offset;
   Uint32 size;
   bool compressed;
   Uint32 packed_size;
};

struct exception
{
   Uint16 offset;
   Uint8 hash[20];
};

struct disc
{
   enum format format;
   SDL_IOStream *io;
   Uint64 size;
   Uint8 header[0x80];

   /* WIA/RVZ */
   Uint32 compression, chunk_size;
   struct wia_part *parts;
   Uint32 part_count;
   struct wia_raw *raws;
   Uint32 raw_count;
   struct wia_group *groups;
   Uint32 group_count;
   /* The last decoded group: its data, and its hash exception lists. */
   Sint64 cached_group;
   Uint8 *chunk;
   Uint8 *except;
   size_t except_size;
   /* Exceptions gathered while reading one 2 MiB Wii group. */
   struct exception *found;
   size_t found_count, found_alloc;
   /* The last rebuilt (encrypted) Wii group. */
   Sint64 wii_part, wii_group;
   Uint8 *wii;
   size_t wii_size;

   /* GCZ */
   Uint64 *block_ptrs;
   Uint32 block_size, block_count;
   Uint64 gcz_data_start, gcz_compressed_size;
   Sint64 cached_block;
   Uint8 *block, *zbuf;
};

static Uint16 be16(const Uint8 *p)
{
   return (Uint16)((p[0] << 8) | p[1]);
}

static Uint32 be32(const Uint8 *p)
{
   return ((Uint32)p[0] << 24) | ((Uint32)p[1] << 16) | ((Uint32)p[2] << 8) | p[3];
}

static Uint64 be64(const Uint8 *p)
{
   return ((Uint64)be32(p) << 32) | be32(p + 4);
}

static Uint32 le32(const Uint8 *p)
{
   return ((Uint32)p[3] << 24) | ((Uint32)p[2] << 16) | ((Uint32)p[1] << 8) | p[0];
}

static Uint64 le64(const Uint8 *p)
{
   return ((Uint64)le32(p + 4) << 32) | le32(p);
}

static bool read_at(struct disc *d, Uint64 offset, void *dst, size_t size)
{
   return SDL_SeekIO(d->io, (Sint64)offset, SDL_IO_SEEK_SET) >= 0
      && SDL_ReadIO(d->io, dst, size) == size;
}

/* --- RVZ packing: junk data regenerated from a seed --- */

struct lfg
{
   Uint32 buffer[LFG_K];
   size_t position;
};

static void lfg_forward(struct lfg *l)
{
   int i;
   for (i = 0; i < 32; i++)
      l->buffer[i] ^= l->buffer[i + LFG_K - 32];
   for (i = 32; i < LFG_K; i++)
      l->buffer[i] ^= l->buffer[i - 32];
}

static void lfg_seed(struct lfg *l, const Uint8 *seed)
{
   int i;

   for (i = 0; i < LFG_SEED; i++)
      l->buffer[i] = be32(seed + i * 4);
   for (i = LFG_SEED; i < LFG_K; i++)
      l->buffer[i] = (l->buffer[i - 17] << 23) ^ (l->buffer[i - 16] >> 9) ^ l->buffer[i - 1];
   /* The output shifts the third byte by 18, not 16: done here once. */
   for (i = 0; i < LFG_K; i++)
      l->buffer[i] = (l->buffer[i] & 0xff00ffff) | ((l->buffer[i] >> 2) & 0x00ff0000);
   for (i = 0; i < 4; i++)
      lfg_forward(l);
   l->position = 0;
}

static void lfg_skip(struct lfg *l, size_t bytes)
{
   l->position += bytes;
   while (l->position >= LFG_K * 4)
   {
      lfg_forward(l);
      l->position -= LFG_K * 4;
   }
}

static void lfg_bytes(struct lfg *l, Uint8 *out, size_t count)
{
   while (count--)
   {
      *out++ = (Uint8)(l->buffer[l->position / 4] >> (24 - (l->position % 4) * 8));
      if (++l->position == LFG_K * 4)
      {
         lfg_forward(l);
         l->position = 0;
      }
   }
}

/* Decodes RVZ packed data into out; data_offset (mod 32 KiB) places the
 * junk generator. */
static bool unpack(const Uint8 *in, size_t in_size, Uint8 *out, size_t out_size,
      Uint64 data_offset)
{
   struct lfg *l = NULL;
   size_t pos = 0, done = 0;

   while (done < out_size && pos + 4 <= in_size)
   {
      Uint32 size = be32(in + pos);
      bool junk = size & 0x80000000;
      size_t n;

      pos += 4;
      size &= 0x7fffffff;
      n = SDL_min((size_t)size, out_size - done);
      if (junk)
      {
         if (pos + LFG_SEED * 4 > in_size || (!l && !(l = SDL_malloc(sizeof(*l)))))
            break;
         lfg_seed(l, in + pos);
         lfg_skip(l, (size_t)(data_offset % BLOCK_SIZE));
         lfg_bytes(l, out + done, n);
         pos += LFG_SEED * 4;
      }
      else
      {
         if (pos + n > in_size)
            break;
         SDL_memcpy(out + done, in + pos, n);
         pos += size;
      }
      done += n;
      data_offset += n;
   }
   SDL_free(l);
   return done == out_size;
}

/* --- WIA/RVZ --- */

/* Decompresses a table or group stored with the disc's method. Returns
 * the size written, or -1. */
static Sint64 decompress(Uint32 method, const Uint8 *in, size_t in_size, Uint8 *out, size_t cap)
{
   size_t n;

   if (method == WIA_NONE)
   {
      n = SDL_min(in_size, cap);
      SDL_memcpy(out, in, n);
      return (Sint64)n;
   }
   if (method != WIA_ZSTD)
      return -1;
   n = ZSTD_decompress(out, cap, in, in_size);
   return ZSTD_isError(n) ? -1 : (Sint64)n;
}

static Uint8 *read_table(struct disc *d, Uint64 offset, Uint32 stored_size, size_t size)
{
   Uint8 *in = SDL_malloc(stored_size ? stored_size : 1), *out = SDL_malloc(size ? size : 1);

   if (!in || !out || !read_at(d, offset, in, stored_size)
         || decompress(d->compression, in, stored_size, out, size) != (Sint64)size)
   {
      SDL_free(out);
      out = NULL;
   }
   SDL_free(in);
   return out;
}

/* Exception lists lead a Wii partition group; stored uncompressed they're
 * padded to 4 bytes. Returns their size, or -1. */
static Sint64 exception_lists_size(const Uint8 *p, size_t size, Uint32 lists, bool align)
{
   size_t used = 0;
   Uint32 i;

   for (i = 0; i < lists; i++)
   {
      if (used + 2 > size)
         return -1;
      used += 2 + (size_t)be16(p + used) * 22;
      if (align && i == lists - 1)
         used = (used + 3) & ~(size_t)3;
   }
   return used <= size ? (Sint64)used : -1;
}

/* Decodes group index into d->chunk (size bytes of data). */
static bool load_group(struct disc *d, Uint32 index, size_t size, Uint32 lists, Uint64 data_offset)
{
   const struct wia_group *g;
   Uint8 *in = NULL, *out = NULL;
   const Uint8 *data;
   Sint64 except, n;
   size_t data_size;
   bool ok = false;

   if (d->cached_group == index)
      return true;
   if (index >= d->group_count)
      return false;
   g = &d->groups[index];
   d->cached_group = -1;
   d->except_size = 0;

   /* Size 0: all zeroes and no exceptions. */
   if (g->size == 0)
   {
      SDL_memset(d->chunk, 0, size);
      d->cached_group = index;
      return true;
   }

   in = SDL_malloc(g->size);
   if (!in || !read_at(d, g->file_offset, in, g->size))
      goto out;

   if (g->compressed)
   {
      size_t cap = (size_t)lists * MAX_EXCEPTION_LIST + (g->packed_size ? g->packed_size : size);
      out = SDL_malloc(cap);
      if (!out || (n = decompress(d->compression, in, g->size, out, cap)) < 0)
         goto out;
      data = out;
      data_size = (size_t)n;
   }
   else
   {
      data = in;
      data_size = g->size;
   }

   except = exception_lists_size(data, data_size, lists, !g->compressed);
   if (except < 0 || (size_t)except > (size_t)lists * MAX_EXCEPTION_LIST)
      goto out;
   SDL_memcpy(d->except, data, (size_t)except);
   d->except_size = (size_t)except;
   data += except;
   data_size -= (size_t)except;

   if (g->packed_size)
      ok = unpack(data, SDL_min(data_size, (size_t)g->packed_size), d->chunk, size, data_offset);
   else if (data_size >= size)
   {
      SDL_memcpy(d->chunk, data, size);
      ok = true;
   }
   if (ok)
      d->cached_group = index;

out:
   SDL_free(in);
   SDL_free(out);
   return ok;
}

/* Adds list index of the cached group to d->found, offsets moved by
 * extra (the group's place in its 2 MiB). */
static bool gather_exceptions(struct disc *d, Uint32 list, Uint16 extra)
{
   const Uint8 *p = d->except, *end = d->except + d->except_size;
   Uint16 count, i;

   for (; list > 0; list--)
   {
      if (p + 2 > end)
         return false;
      p += 2 + be16(p) * 22;
   }
   if (p + 2 > end)
      return d->except_size == 0;
   count = be16(p);
   p += 2;
   if (p + (size_t)count * 22 > end)
      return false;

   for (i = 0; i < count; i++, p += 22)
   {
      struct exception *e;
      if (d->found_count == d->found_alloc)
      {
         size_t alloc = d->found_alloc ? d->found_alloc * 2 : 256;
         struct exception *grown = SDL_realloc(d->found, alloc * sizeof(*grown));
         if (!grown)
            return false;
         d->found = grown;
         d->found_alloc = alloc;
      }
      e = &d->found[d->found_count++];
      e->offset = (Uint16)(be16(p) + extra);
      SDL_memcpy(e->hash, p + 2, 20);
   }
   return true;
}

/* Reads [*offset, *offset + *size) where it overlaps one data entry
 * (raw data, or decrypted partition data), as Dolphin's ReadFromGroups. */
static bool read_groups(struct disc *d, Uint64 *offset, Uint64 *size, Uint8 **out,
      Uint64 chunk, Uint32 sector, Uint64 data_offset, Uint64 data_size,
      Uint32 group_index, Uint32 groups, Uint32 lists, bool exceptions)
{
   Uint64 skipped, i;

   if (data_offset + data_size <= *offset)
      return true;
   if (*offset < data_offset)
      return false;

   /* The first raw entry starts at 0x80 but holds the whole sector. */
   skipped = data_offset % sector;
   data_offset -= skipped;
   data_size += skipped;

   for (i = (*offset - data_offset) / chunk; i < groups && *size > 0; i++)
   {
      Uint64 group_offset = i * chunk;
      Uint64 in_group = *offset - group_offset - data_offset;
      Uint64 this_chunk = SDL_min(chunk, data_size - group_offset);
      Uint64 n = SDL_min(this_chunk - in_group, *size);

      if (!load_group(d, group_index + (Uint32)i, (size_t)this_chunk, lists, group_offset))
         return false;
      SDL_memcpy(*out, d->chunk + in_group, (size_t)n);

      if (exceptions && !gather_exceptions(d, (Uint32)(in_group / GROUP_DATA),
               (Uint16)(group_offset % GROUP_DATA / BLOCK_DATA * BLOCK_HASHES)))
         return false;

      *offset += n;
      *size -= n;
      *out += n;
   }
   return true;
}

static Uint32 partition_sectors(const struct wia_part *p)
{
   if (p->data[1].sectors)
      return p->data[1].first_sector - p->data[0].first_sector + p->data[1].sectors;
   return p->data[0].sectors;
}

/* Decrypted partition data, gathering hash exceptions on the way. */
static bool read_wii_decrypted(struct disc *d, const struct wia_part *p, Uint64 offset,
      Uint64 size, Uint8 *out)
{
   Uint64 chunk = (Uint64)d->chunk_size * BLOCK_DATA / BLOCK_SIZE;
   Uint32 lists = (Uint32)SDL_max(1, chunk / GROUP_DATA);
   int i;

   for (i = 0; i < 2 && size > 0; i++)
   {
      Uint64 data_offset = (Uint64)(p->data[i].first_sector - p->data[0].first_sector) * BLOCK_DATA;
      if (!read_groups(d, &offset, &size, &out, chunk, BLOCK_DATA, data_offset,
               (Uint64)p->data[i].sectors * BLOCK_DATA, p->data[i].group_index,
               p->data[i].groups, lists, true))
         return false;
   }
   return size == 0;
}

/* Rebuilds 2 MiB group index of partition part as it is on disc: the
 * hash tree (H0 per KiB, H1 per block, H2 per 8 blocks), the stored
 * exceptions, then AES-CBC with the partition's title key. */
static bool build_wii_group(struct disc *d, Uint32 part, Uint64 index)
{
   const struct wia_part *p = &d->parts[part];
   Uint64 total = (Uint64)partition_sectors(p) * BLOCK_DATA;
   Uint64 offset = index * GROUP_DATA, size;
   Uint8 *data, *hashes, h1[8][GROUP_BLOCKS / 8 * 20], h2[8 * 20];
   struct aes128 aes;
   size_t blocks, b, i;
   bool ok = false;

   if (d->wii_part == part && d->wii_group == (Sint64)index)
      return true;
   if (offset >= total)
      return false;
   size = SDL_min((Uint64)GROUP_DATA, total - offset);
   blocks = (size_t)((size + BLOCK_DATA - 1) / BLOCK_DATA);

   data = SDL_calloc(1, GROUP_DATA);
   hashes = SDL_calloc(GROUP_BLOCKS, BLOCK_HASHES);
   if (!data || !hashes)
      goto out;
   d->found_count = 0;
   if (!read_wii_decrypted(d, p, offset, size, data))
      goto out;

   /* Missing data at the partition's end hashes as zeroes. */
   for (b = 0; b < GROUP_BLOCKS; b++)
      for (i = 0; i < 31; i++)
         sha1(data + b * BLOCK_DATA + i * 0x400, 0x400, hashes + b * BLOCK_HASHES + i * 20);
   for (b = 0; b < GROUP_BLOCKS; b++)
      sha1(hashes + b * BLOCK_HASHES, 31 * 20, h1[b / 8] + (b % 8) * 20);
   for (i = 0; i < 8; i++)
      sha1(h1[i], sizeof(h1[0]), h2 + i * 20);
   for (b = 0; b < GROUP_BLOCKS; b++)
   {
      SDL_memcpy(hashes + b * BLOCK_HASHES + 0x280, h1[b / 8], sizeof(h1[0]));
      SDL_memcpy(hashes + b * BLOCK_HASHES + 0x340, h2, sizeof(h2));
   }

   for (i = 0; i < d->found_count; i++)
   {
      size_t at = d->found[i].offset;
      if (at / BLOCK_HASHES >= GROUP_BLOCKS || at % BLOCK_HASHES + 20 > BLOCK_HASHES)
         goto out;
      SDL_memcpy(hashes + at, d->found[i].hash, 20);
   }

   if (!d->wii && !(d->wii = SDL_malloc(GROUP_SIZE)))
      goto out;
   aes128_init(&aes, p->key);
   for (b = 0; b < blocks; b++)
   {
      static const Uint8 zero_iv[16];
      Uint8 *block = d->wii + b * BLOCK_SIZE;

      SDL_memcpy(block, hashes + b * BLOCK_HASHES, BLOCK_HASHES);
      aes128_cbc_encrypt(&aes, zero_iv, block, BLOCK_HASHES);
      SDL_memcpy(block + BLOCK_HASHES, data + b * BLOCK_DATA, BLOCK_DATA);
      aes128_cbc_encrypt(&aes, block + 0x3d0, block + BLOCK_HASHES, BLOCK_DATA);
   }
   d->wii_part = part;
   d->wii_group = (Sint64)index;
   d->wii_size = blocks * BLOCK_SIZE;
   ok = true;

out:
   if (!ok)
      d->wii_group = -1;
   SDL_free(data);
   SDL_free(hashes);
   return ok;
}

static bool read_wia(struct disc *d, Uint64 offset, Uint8 *out, Uint64 size)
{
   if (offset < sizeof(d->header))
   {
      Uint64 n = SDL_min(sizeof(d->header) - offset, size);
      SDL_memcpy(out, d->header + offset, (size_t)n);
      offset += n;
      size -= n;
      out += n;
   }

   while (size > 0)
   {
      Uint64 best_end = ~(Uint64)0;
      Sint64 raw = -1, part = -1;
      Uint32 i;
      int j;

      /* The data entry that ends first after offset holds it. */
      for (i = 0; i < d->raw_count; i++)
      {
         Uint64 end = d->raws[i].offset + d->raws[i].size;
         if (d->raws[i].size && end > offset && end < best_end)
         {
            best_end = end;
            raw = i;
            part = -1;
         }
      }
      for (i = 0; i < d->part_count; i++)
         for (j = 0; j < 2; j++)
         {
            Uint64 end = ((Uint64)d->parts[i].data[j].first_sector + d->parts[i].data[j].sectors)
               * BLOCK_SIZE;
            if (d->parts[i].data[j].sectors && end > offset && end < best_end)
            {
               best_end = end;
               part = i;
               raw = -1;
            }
         }

      if (raw >= 0)
      {
         const struct wia_raw *r = &d->raws[raw];
         if (!read_groups(d, &offset, &size, &out, d->chunk_size, BLOCK_SIZE, r->offset,
                  r->size, r->group_index, r->groups, 0, false))
            return false;
      }
      else if (part >= 0)
      {
         Uint64 start = (Uint64)d->parts[part].data[0].first_sector * BLOCK_SIZE;
         Uint64 n = SDL_min(best_end - offset, size);

         if (offset < start)
            return false;
         while (n > 0)
         {
            Uint64 index = (offset - start) / GROUP_SIZE;
            Uint64 in_group = (offset - start) % GROUP_SIZE, m;

            if (!build_wii_group(d, (Uint32)part, index) || in_group >= d->wii_size)
               return false;
            m = SDL_min(d->wii_size - in_group, n);
            SDL_memcpy(out, d->wii + in_group, (size_t)m);
            offset += m;
            size -= m;
            out += m;
            n -= m;
         }
      }
      else
         return false;
   }
   return true;
}

static bool open_wia(struct disc *d, const Uint8 *head1)
{
   Uint8 head2[0xdc] = { 0 }, *table = NULL;
   Uint32 head2_size = be32(head1 + 0x0c), part_size, i;
   Uint64 raw_offset, group_offset;
   Uint32 raw_stored, group_stored;
   size_t group_entry = d->format == FORMAT_RVZ ? 12 : 8;

   d->size = be64(head1 + 0x24);
   if (head2_size < 0xd4 || !read_at(d, 0x48, head2, SDL_min(head2_size, (Uint32)sizeof(head2))))
      return false;

   d->compression = be32(head2 + 0x04);
   d->chunk_size  = be32(head2 + 0x0c);
   SDL_memcpy(d->header, head2 + 0x10, sizeof(d->header));
   if (d->chunk_size < BLOCK_SIZE)
      return false;

   /* Past here only the header is needed for a title: failures leave a
    * disc that can't read further than it. */
   d->part_count = be32(head2 + 0x90);
   part_size     = be32(head2 + 0x94);
   d->raw_count  = be32(head2 + 0xb4);
   raw_offset    = be64(head2 + 0xb8);
   raw_stored    = be32(head2 + 0xc0);
   d->group_count = be32(head2 + 0xc4);
   group_offset  = be64(head2 + 0xc8);
   group_stored  = be32(head2 + 0xd0);
   if (d->part_count > 16 || part_size > 0x100 || d->raw_count > 0x10000
         || d->group_count > 0x1000000)
      goto fail;

   d->parts = SDL_calloc(d->part_count + 1, sizeof(*d->parts));
   table = SDL_calloc(d->part_count + 1, part_size + 1);
   if (!d->parts || !table || !read_at(d, be64(head2 + 0x98), table, (size_t)d->part_count * part_size))
      goto fail;
   for (i = 0; i < d->part_count; i++)
   {
      Uint8 entry[0x30] = { 0 };
      int j;
      SDL_memcpy(entry, table + i * part_size, SDL_min(part_size, (Uint32)sizeof(entry)));
      SDL_memcpy(d->parts[i].key, entry, 16);
      for (j = 0; j < 2; j++)
      {
         d->parts[i].data[j].first_sector = be32(entry + 0x10 + j * 0x10);
         d->parts[i].data[j].sectors      = be32(entry + 0x14 + j * 0x10);
         d->parts[i].data[j].group_index  = be32(entry + 0x18 + j * 0x10);
         d->parts[i].data[j].groups       = be32(entry + 0x1c + j * 0x10);
      }
   }
   SDL_free(table);

   table = read_table(d, raw_offset, raw_stored, (size_t)d->raw_count * 0x18);
   d->raws = SDL_calloc(d->raw_count + 1, sizeof(*d->raws));
   if (!table || !d->raws)
      goto fail;
   for (i = 0; i < d->raw_count; i++)
   {
      const Uint8 *e = table + i * 0x18;
      d->raws[i].offset      = be64(e);
      d->raws[i].size        = be64(e + 8);
      d->raws[i].group_index = be32(e + 16);
      d->raws[i].groups      = be32(e + 20);
   }
   SDL_free(table);

   table = read_table(d, group_offset, group_stored, d->group_count * group_entry);
   d->groups = SDL_calloc(d->group_count + 1, sizeof(*d->groups));
   if (!table || !d->groups)
      goto fail;
   for (i = 0; i < d->group_count; i++)
   {
      const Uint8 *e = table + i * group_entry;
      Uint32 size = be32(e + 4);
      struct wia_group *g = &d->groups[i];

      g->file_offset = (Uint64)be32(e) << 2;
      if (d->format == FORMAT_RVZ)
      {
         /* The top bit says whether this group uses the compression. */
         g->compressed = (size & 0x80000000) && d->compression != WIA_NONE;
         g->size = size & 0x7fffffff;
         g->packed_size = be32(e + 8);
      }
      else
      {
         g->compressed = d->compression != WIA_NONE;
         g->size = size;
      }
   }
   SDL_free(table);
   table = NULL;

   d->chunk = SDL_malloc(d->chunk_size);
   d->except = SDL_malloc((size_t)SDL_max(1, d->chunk_size / GROUP_SIZE) * MAX_EXCEPTION_LIST);
   if (!d->chunk || !d->except)
      goto fail;
   return true;

fail:
   SDL_free(table);
   d->group_count = d->raw_count = d->part_count = 0;
   return true;
}

/* --- GCZ (Dolphin's older format: zlib per block) --- */

static bool open_gcz(struct disc *d, const Uint8 *head)
{
   Uint8 *ptrs;
   Uint32 i;

   d->gcz_compressed_size = le64(head + 8);
   d->size        = le64(head + 16);
   d->block_size  = le32(head + 24);
   d->block_count = le32(head + 28);
   d->gcz_data_start = 32 + (Uint64)d->block_count * 12;
   if (!d->block_size || d->block_size > 0x1000000 || d->block_count > 0x1000000)
      return false;

   ptrs = SDL_malloc((size_t)d->block_count * 8 + 1);
   d->block_ptrs = SDL_malloc((size_t)d->block_count * sizeof(Uint64) + 1);
   d->block = SDL_malloc(d->block_size);
   d->zbuf = SDL_malloc(d->block_size + 64);
   if (!ptrs || !d->block_ptrs || !d->block || !d->zbuf
         || !read_at(d, 32, ptrs, (size_t)d->block_count * 8))
   {
      SDL_free(ptrs);
      return false;
   }
   for (i = 0; i < d->block_count; i++)
      d->block_ptrs[i] = le64(ptrs + i * 8);
   SDL_free(ptrs);
   d->cached_block = -1;
   return true;
}

static bool load_gcz_block(struct disc *d, Uint32 index)
{
   const Uint64 stored_flag = (Uint64)1 << 63;
   Uint64 start, end;
   uLongf size = d->block_size;

   if (d->cached_block == index)
      return true;
   if (index >= d->block_count)
      return false;
   start = d->block_ptrs[index] & ~stored_flag;
   end = index + 1 < d->block_count ? d->block_ptrs[index + 1] & ~stored_flag
      : d->gcz_compressed_size;
   if (end < start || end - start > d->block_size + 64)
      return false;

   d->cached_block = -1;
   if (d->block_ptrs[index] & stored_flag)
   {
      if (!read_at(d, d->gcz_data_start + start, d->block, (size_t)(end - start)))
         return false;
   }
   else if (!read_at(d, d->gcz_data_start + start, d->zbuf, (size_t)(end - start))
         || uncompress(d->block, &size, d->zbuf, (uLong)(end - start)) != Z_OK)
      return false;
   d->cached_block = index;
   return true;
}

static bool read_gcz(struct disc *d, Uint64 offset, Uint8 *out, Uint64 size)
{
   while (size > 0)
   {
      Uint64 in_block = offset % d->block_size;
      Uint64 n = SDL_min(d->block_size - in_block, size);

      if (!load_gcz_block(d, (Uint32)(offset / d->block_size)))
         return false;
      SDL_memcpy(out, d->block + in_block, (size_t)n);
      offset += n;
      size -= n;
      out += n;
   }
   return true;
}

/* --- Public --- */

struct disc *disc_open(const char *path)
{
   struct disc *d = SDL_calloc(1, sizeof(*d));
   Uint8 head[0x48];
   bool ok = false;

   if (!d)
      return NULL;
   d->cached_group = d->wii_group = d->wii_part = -1;
   d->io = SDL_IOFromFile(path, "rb");
   if (!d->io || !read_at(d, 0, head, sizeof(head)))
      goto out;

   if (SDL_memcmp(head, "RVZ\x01", 4) == 0 || SDL_memcmp(head, "WIA\x01", 4) == 0)
   {
      d->format = head[0] == 'R' ? FORMAT_RVZ : FORMAT_WIA;
      ok = open_wia(d, head);
   }
   else if (le32(head) == GCZ_MAGIC)
   {
      d->format = FORMAT_GCZ;
      ok = open_gcz(d, head) && read_gcz(d, 0, d->header, sizeof(d->header));
   }
   else
   {
      d->format = FORMAT_PLAIN;
      d->size = (Uint64)SDL_GetIOSize(d->io);
      ok = read_at(d, 0, d->header, sizeof(d->header));
   }

   ok = ok && disc_kind(d) != DISC_UNKNOWN;
out:
   if (!ok)
   {
      disc_close(d);
      return NULL;
   }
   return d;
}

enum disc_kind disc_peek(const char *path)
{
   SDL_IOStream *io = SDL_IOFromFile(path, "rb");
   Uint8 head[0x60];
   enum disc_kind kind = DISC_UNKNOWN;
   Uint32 type;

   if (!io)
      return DISC_UNKNOWN;
   if (SDL_ReadIO(io, head, sizeof(head)) == sizeof(head))
   {
      /* WIA/RVZ: the second header's disc type, after the first's 0x48
       * bytes; GCZ: its sub type; CISO: the disc's own header after its
       * 0x8000-byte block map; else (plain) the disc's header itself. */
      if (SDL_memcmp(head, "RVZ\x01", 4) == 0 || SDL_memcmp(head, "WIA\x01", 4) == 0)
      {
         type = be32(head + 0x48);
         kind = type == 2 ? DISC_WII : type == 1 ? DISC_GAMECUBE : DISC_UNKNOWN;
      }
      else if (le32(head) == GCZ_MAGIC)
         kind = le32(head + 4) == 1 ? DISC_WII : DISC_GAMECUBE;
      else
      {
         if (SDL_memcmp(head, "CISO", 4) == 0
               && (SDL_SeekIO(io, 0x8000, SDL_IO_SEEK_SET) < 0
                  || SDL_ReadIO(io, head, 0x20) != 0x20))
            SDL_memset(head, 0, sizeof(head));
         if (be32(head + 0x18) == WII_MAGIC)
            kind = DISC_WII;
         else if (be32(head + 0x1c) == GC_MAGIC)
            kind = DISC_GAMECUBE;
      }
   }
   SDL_CloseIO(io);
   return kind;
}

void disc_close(struct disc *d)
{
   if (!d)
      return;
   if (d->io)
      SDL_CloseIO(d->io);
   SDL_free(d->parts);
   SDL_free(d->raws);
   SDL_free(d->groups);
   SDL_free(d->chunk);
   SDL_free(d->except);
   SDL_free(d->found);
   SDL_free(d->wii);
   SDL_free(d->block_ptrs);
   SDL_free(d->block);
   SDL_free(d->zbuf);
   SDL_free(d);
}

enum disc_kind disc_kind(const struct disc *d)
{
   if (be32(d->header + 0x18) == WII_MAGIC)
      return DISC_WII;
   if (be32(d->header + 0x1c) == GC_MAGIC)
      return DISC_GAMECUBE;
   return DISC_UNKNOWN;
}

Uint64 disc_size(const struct disc *d)
{
   return d->size;
}

bool disc_read(struct disc *d, Uint64 offset, void *dst, size_t size)
{
   if (offset > d->size || size > d->size - offset)
      return false;
   switch (d->format)
   {
      case FORMAT_PLAIN:
         return read_at(d, offset, dst, size);
      case FORMAT_GCZ:
         return read_gcz(d, offset, dst, size);
      default:
         return read_wia(d, offset, dst, size);
   }
}
