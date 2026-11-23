#include "anim.h"

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <vpx/vp8dx.h>
#include <vpx/vpx_decoder.h>
#include <webp/demux.h>

/* More than any has: past it, shown still. */
#define FRAMES_MAX 4000

/* Averages each block of src's pixels a smaller one covers. Colours count
 * by their alpha: transparent ones mustn't darken an edge. */
static void shrink_into(const Uint8 *src, int w, int h, Uint8 *dst, int nw, int nh)
{
   int x, y, sx, sy, c;

   for (y = 0; y < nh; y++)
   {
      int y0 = y * h / nh, y1 = SDL_max(y0 + 1, (y + 1) * h / nh);
      for (x = 0; x < nw; x++)
      {
         int x0 = x * w / nw, x1 = SDL_max(x0 + 1, (x + 1) * w / nw);
         Uint32 sum[3] = { 0, 0, 0 }, alpha = 0, n = (Uint32)((y1 - y0) * (x1 - x0));
         Uint8 *d = dst + ((size_t)y * nw + x) * 4;

         for (sy = y0; sy < y1; sy++)
            for (sx = x0; sx < x1; sx++)
            {
               const Uint8 *s = src + ((size_t)sy * w + sx) * 4;
               for (c = 0; c < 3; c++)
                  sum[c] += (Uint32)s[c] * s[3];
               alpha += s[3];
            }
         for (c = 0; c < 3; c++)
            d[c] = alpha ? (Uint8)(sum[c] / alpha) : 0;
         d[3] = (Uint8)(alpha / n);
      }
   }
}

static WebPAnimDecoder *open_anim(const void *data, size_t size, WebPAnimInfo *info)
{
   WebPData d = { data, size };
   WebPAnimDecoderOptions options;
   WebPAnimDecoder *dec;

   if (!WebPAnimDecoderOptionsInit(&options))
      return NULL;
   options.color_mode = MODE_RGBA;
   if (!(dec = WebPAnimDecoderNew(&d, &options)))
      return NULL;
   if (!WebPAnimDecoderGetInfo(dec, info) || !info->canvas_width || !info->canvas_height)
   {
      WebPAnimDecoderDelete(dec);
      return NULL;
   }
   return dec;
}

/* Room for count frames of w x h art, shrunk as anim_load says. */
static struct anim *anim_new(int w, int h, int count, int max_w, int max_h, size_t max_bytes)
{
   float f = SDL_min(1.0f, SDL_min((float)max_w / w, (float)max_h / h));
   float bytes = (float)w * f * (float)h * f * 4 * (float)count;
   struct anim *a;
   size_t head = sizeof(*a) + sizeof(int) * (size_t)count;
   int nw, nh;

   /* All its frames, smaller rather than fewer. */
   if (bytes > (float)max_bytes)
      f *= SDL_sqrtf((float)max_bytes / bytes);
   nw = SDL_max(1, (int)(w * f));
   nh = SDL_max(1, (int)(h * f));
   if (!(a = SDL_malloc(head + (size_t)nw * nh * 4 * (size_t)count)))
      return NULL;
   SDL_zerop(a);
   a->w = nw;
   a->h = nh;
   a->ends = (int *)(a + 1);
   a->pixels = (Uint8 *)a + head;
   return a;
}

/* Adds a frame that lasts until end (ms). */
static void anim_add(struct anim *a, const Uint8 *pixels, int w, int h, int end)
{
   shrink_into(pixels, w, h, a->pixels + (size_t)a->w * a->h * 4 * (size_t)a->count, a->w, a->h);
   /* Some have frames of no time at all. */
   a->length = SDL_max(end, a->length + 10);
   a->ends[a->count++] = a->length;
}

/* A single frame isn't an animation. */
static struct anim *anim_done(struct anim *a)
{
   if (a && a->count < 2)
   {
      SDL_free(a);
      return NULL;
   }
   return a;
}

static struct anim *load_webp(const void *data, size_t size, int max_w, int max_h,
      size_t max_bytes)
{
   WebPAnimInfo info;
   WebPAnimDecoder *dec = open_anim(data, size, &info);
   struct anim *a = NULL;
   uint8_t *pixels;
   int end, w, h, count;

   if (!dec)
      return NULL;
   w = (int)info.canvas_width;
   h = (int)info.canvas_height;
   count = (int)info.frame_count;
   if (count > 1 && count <= FRAMES_MAX
         && (a = anim_new(w, h, count, max_w, max_h, max_bytes)))
      while (a->count < count && WebPAnimDecoderHasMoreFrames(dec)
            && WebPAnimDecoderGetNext(dec, &pixels, &end))
         anim_add(a, pixels, w, h, end);
   WebPAnimDecoderDelete(dec);
   return anim_done(a);
}

static Uint32 be32(const Uint8 *p)
{
   return (Uint32)p[0] << 24 | (Uint32)p[1] << 16 | (Uint32)p[2] << 8 | p[3];
}

/* A frame being gathered from an animated PNG's chunks, as a PNG of its
 * own for stb_image. */
struct png_frame
{
   SDL_IOStream *io;
   /* Where it goes on the canvas, and for how long. */
   int w, h, x, y, ms;
   Uint8 dispose, blend;
};

static void write_chunk(SDL_IOStream *io, const char *type, const Uint8 *data, Uint32 size)
{
   SDL_WriteU32BE(io, size);
   SDL_WriteIO(io, type, 4);
   SDL_WriteIO(io, data, size);
   /* stb_image doesn't look at the checksum. */
   SDL_WriteU32BE(io, 0);
}

/* Puts the gathered frame on the canvas and adds it. */
static void finish_frame(struct png_frame *f, struct anim *a, Uint8 *canvas, Uint8 *before,
      int w, int h)
{
   size_t bytes = (size_t)w * h * 4;
   Uint8 *png, *px;
   int fw, fh, n, x, y, c;

   write_chunk(f->io, "IEND", NULL, 0);
   png = SDL_GetPointerProperty(SDL_GetIOProperties(f->io), SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER,
         NULL);
   px = png ? stbi_load_from_memory(png, (int)SDL_TellIO(f->io), &fw, &fh, &n, 4) : NULL;
   SDL_CloseIO(f->io);
   f->io = NULL;
   if (!px)
      return;
   if (fw == f->w && fh == f->h)
   {
      if (f->dispose == 2)
         SDL_memcpy(before, canvas, bytes);
      for (y = 0; y < fh; y++)
         for (x = 0; x < fw; x++)
         {
            const Uint8 *s = px + ((size_t)y * fw + x) * 4;
            Uint8 *d = canvas + ((size_t)(f->y + y) * w + f->x + x) * 4;
            Uint32 under = f->blend ? d[3] * (255u - s[3]) / 255 : 0, alpha = s[3] + under;
            for (c = 0; c < 3; c++)
               d[c] = alpha ? (Uint8)((s[c] * s[3] + d[c] * under) / alpha) : 0;
            d[3] = (Uint8)alpha;
         }
      anim_add(a, canvas, w, h, a->length + f->ms);
      if (f->dispose == 2)
         SDL_memcpy(canvas, before, bytes);
      else if (f->dispose == 1)
         for (y = 0; y < fh; y++)
            SDL_memset(canvas + ((size_t)(f->y + y) * w + f->x) * 4, 0, (size_t)fw * 4);
   }
   SDL_free(px);
}

/* Chunks: IHDR, acTL (how many frames), what they share (palette), then
 * for each frame an fcTL (where, how long) and its data: IDAT for the
 * first, fdAT (a number, then the same) for the others. */
static struct anim *load_apng(const Uint8 *d, size_t size, int max_w, int max_h, size_t max_bytes)
{
   const Uint8 *ihdr = NULL, *end = d + size, *p;
   SDL_IOStream *shared = SDL_IOFromDynamicMem();
   struct png_frame f;
   struct anim *a = NULL;
   Uint8 *canvas = NULL, *before = NULL;
   int w = 0, h = 0;

   SDL_zero(f);
   for (p = d + 8; shared && end - p >= 12; )
   {
      Uint32 len = be32(p);
      const Uint8 *body = p + 8;
      const char *type = (const char *)p + 4;

      if (len > (size_t)(end - p) - 12)
         break;
      p += (size_t)len + 12;
      if (SDL_memcmp(type, "IHDR", 4) == 0 && len == 13)
      {
         ihdr = body;
         w = (int)be32(body);
         h = (int)be32(body + 4);
         if (w < 1 || h < 1 || w > 8192 || h > 8192)
            break;
      }
      else if (SDL_memcmp(type, "acTL", 4) == 0 && len == 8 && ihdr && !a)
      {
         Uint32 count = be32(body);
         if (count < 2 || count > FRAMES_MAX
               || !(a = anim_new(w, h, (int)count, max_w, max_h, max_bytes)))
            break;
         /* Both cleared: transparent. */
         canvas = SDL_calloc((size_t)w * h, 4);
         before = SDL_calloc((size_t)w * h, 4);
         if (!canvas || !before)
            break;
      }
      else if (SDL_memcmp(type, "fcTL", 4) == 0 && len == 26 && a && canvas && before)
      {
         Uint32 fw = be32(body + 4), fh = be32(body + 8), fx = be32(body + 12), fy = be32(body + 16);
         int num = body[20] << 8 | body[21], den = body[22] << 8 | body[23];
         Uint8 head[13];
         const void *extra;
         Sint64 extra_size;

         if (f.io)
            finish_frame(&f, a, canvas, before, w, h);
         /* All the frames there's room for. */
         if ((Uint8 *)(a->ends + a->count) >= a->pixels)
            break;
         if (!fw || !fh || fx > (Uint32)w || fy > (Uint32)h || fw > (Uint32)w - fx
               || fh > (Uint32)h - fy || !(f.io = SDL_IOFromDynamicMem()))
            break;
         f.w = (int)fw;
         f.h = (int)fh;
         f.x = (int)fx;
         f.y = (int)fy;
         f.ms = num * 1000 / (den ? den : 100);
         f.dispose = body[24];
         f.blend = body[25];
         /* Nothing before the first to go back to. */
         if (f.dispose == 2 && !a->count)
            f.dispose = 1;
         SDL_memcpy(head, ihdr, 13);
         SDL_memcpy(head, body + 4, 8);
         SDL_WriteIO(f.io, d, 8);
         write_chunk(f.io, "IHDR", head, 13);
         extra_size = SDL_TellIO(shared);
         extra = SDL_GetPointerProperty(SDL_GetIOProperties(shared),
               SDL_PROP_IOSTREAM_DYNAMIC_MEMORY_POINTER, NULL);
         if (extra && extra_size > 0)
            SDL_WriteIO(f.io, extra, (size_t)extra_size);
      }
      else if (SDL_memcmp(type, "PLTE", 4) == 0 || SDL_memcmp(type, "tRNS", 4) == 0)
         write_chunk(shared, SDL_memcmp(type, "PLTE", 4) == 0 ? "PLTE" : "tRNS", body, len);
      else if (SDL_memcmp(type, "IDAT", 4) == 0 && f.io)
         write_chunk(f.io, "IDAT", body, len);
      else if (SDL_memcmp(type, "fdAT", 4) == 0 && f.io && len >= 4)
         write_chunk(f.io, "IDAT", body + 4, len - 4);
      else if (SDL_memcmp(type, "IEND", 4) == 0)
         break;
   }
   if (f.io && a && canvas && before && (Uint8 *)(a->ends + a->count) < a->pixels)
      finish_frame(&f, a, canvas, before, w, h);
   if (f.io)
      SDL_CloseIO(f.io);
   if (shared)
      SDL_CloseIO(shared);
   SDL_free(canvas);
   SDL_free(before);
   return anim_done(a);
}

/* A video frame's data, and its transparency's (a video of its own). */
struct packet
{
   const Uint8 *data, *alpha;
   size_t size, alpha_size;
   int ms;
};

struct webm
{
   struct packet *packets;
   int count, alloc;
   int cluster_ms;
   bool vp9;
};

/* WebM's numbers start with how long they are: the zeros before the first
 * 1. An id keeps that 1, a size doesn't. */
static bool ebml_number(const Uint8 **p, const Uint8 *end, bool id, Uint64 *v)
{
   int len = 1, i;

   if (*p >= end || !**p)
      return false;
   while (!(**p & (0x80 >> (len - 1))))
      len++;
   if (end - *p < len)
      return false;
   *v = id ? **p : **p & (0xFF >> len);
   for (i = 1; i < len; i++)
      *v = *v << 8 | (*p)[i];
   /* All ones: to the end of what it's in. */
   if (!id && *v == ((Uint64)1 << (7 * len)) - 1)
      *v = SDL_MAX_UINT64;
   *p += len;
   return true;
}

/* A block: its track, its time in the cluster, flags, then the frame. */
static void add_packet(struct webm *m, const Uint8 *p, const Uint8 *end)
{
   Uint64 track;
   struct packet *k;

   /* Several frames in one (lacing) is for sound. */
   if (!ebml_number(&p, end, false, &track) || end - p < 4 || (p[2] & 0x06))
      return;
   if (m->count >= FRAMES_MAX)
      return;
   if (m->count == m->alloc)
   {
      int alloc = m->alloc ? m->alloc * 2 : 64;
      struct packet *grown = SDL_realloc(m->packets, sizeof(*grown) * (size_t)alloc);
      if (!grown)
         return;
      m->packets = grown;
      m->alloc = alloc;
   }
   k = &m->packets[m->count++];
   SDL_zerop(k);
   k->ms = m->cluster_ms + (Sint16)(p[0] << 8 | p[1]);
   k->data = p + 3;
   k->size = (size_t)(end - k->data);
}

static void read_webm(struct webm *m, const Uint8 *p, const Uint8 *end, int depth)
{
   Uint64 id, size, v;
   int i;

   while (ebml_number(&p, end, true, &id) && ebml_number(&p, end, false, &size))
   {
      const Uint8 *stop = size > (Uint64)(end - p) ? end : p + size;

      switch (id)
      {
         /* Segment, cluster, block group, tracks, track; block additions. */
         case 0x18538067: case 0x1F43B675: case 0xA0: case 0x1654AE6B: case 0xAE:
         case 0x75A1: case 0xA6:
            if (depth < 8)
               read_webm(m, p, stop, depth + 1);
            break;
         case 0xE7: /* The cluster's time. */
            for (v = 0, i = 0; p + i < stop; i++)
               v = v << 8 | p[i];
            m->cluster_ms = (int)v;
            break;
         case 0x86: /* The codec. */
            m->vp9 = stop - p >= 5 && SDL_memcmp(p, "V_VP9", 5) == 0;
            break;
         case 0xA1: case 0xA3: /* A block, in a group or alone. */
            add_packet(m, p, stop);
            break;
         case 0xA5: /* The transparency, after its block. */
            if (m->count)
            {
               m->packets[m->count - 1].alpha = p;
               m->packets[m->count - 1].alpha_size = (size_t)(stop - p);
            }
            break;
         default:
            break;
      }
      p = stop;
   }
}

static const vpx_image_t *decode_frame(vpx_codec_ctx_t *ctx, const Uint8 *data, size_t size)
{
   vpx_codec_iter_t iter = NULL;

   if (vpx_codec_decode(ctx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK)
      return NULL;
   /* None for a frame only others build on. */
   return vpx_codec_get_frame(ctx, &iter);
}

static Uint8 clamp8(int v)
{
   return (Uint8)SDL_clamp(v, 0, 255);
}

static struct anim *load_webm(const Uint8 *d, size_t size, int max_w, int max_h, size_t max_bytes)
{
   vpx_codec_ctx_t colour, alpha;
   vpx_codec_iface_t *codec;
   struct webm m;
   struct anim *a = NULL;
   Uint8 *rgba = NULL;
   int i, w = 0, h = 0;

   SDL_zero(m);
   read_webm(&m, d, d + size, 0);
   codec = m.vp9 ? vpx_codec_vp9_dx() : vpx_codec_vp8_dx();
   if (m.count < 2 || vpx_codec_dec_init(&colour, codec, NULL, 0) != VPX_CODEC_OK)
   {
      SDL_free(m.packets);
      return NULL;
   }
   if (vpx_codec_dec_init(&alpha, codec, NULL, 0) != VPX_CODEC_OK)
   {
      vpx_codec_destroy(&colour);
      SDL_free(m.packets);
      return NULL;
   }
   for (i = 0; i < m.count; i++)
   {
      const struct packet *k = &m.packets[i];
      const vpx_image_t *c = decode_frame(&colour, k->data, k->size), *t = NULL;
      int x, y, end;

      if (k->alpha)
         t = decode_frame(&alpha, k->alpha, k->alpha_size);
      if (!c || c->fmt != VPX_IMG_FMT_I420)
         continue;
      if (!a)
      {
         w = (int)c->d_w;
         h = (int)c->d_h;
         if (!(a = anim_new(w, h, m.count, max_w, max_h, max_bytes))
               || !(rgba = SDL_malloc((size_t)w * h * 4)))
            break;
      }
      if ((int)c->d_w != w || (int)c->d_h != h)
         continue;
      if (t && ((int)t->d_w != w || (int)t->d_h != h))
         t = NULL;
      for (y = 0; y < h; y++)
      {
         const Uint8 *py = c->planes[0] + (ptrdiff_t)y * c->stride[0];
         const Uint8 *pu = c->planes[1] + (ptrdiff_t)(y / 2) * c->stride[1];
         const Uint8 *pv = c->planes[2] + (ptrdiff_t)(y / 2) * c->stride[2];
         Uint8 *o = rgba + (size_t)y * w * 4;
         for (x = 0; x < w; x++, o += 4)
         {
            /* BT.601, 16-235. */
            int l = (py[x] - 16) * 298, u = pu[x / 2] - 128, v = pv[x / 2] - 128;
            o[0] = clamp8((l + 409 * v + 128) >> 8);
            o[1] = clamp8((l - 100 * u - 208 * v + 128) >> 8);
            o[2] = clamp8((l + 516 * u + 128) >> 8);
            o[3] = t ? t->planes[0][(ptrdiff_t)y * t->stride[0] + x] : 255;
         }
      }
      /* Until the next one; the last as long as the one before it. */
      end = i + 1 < m.count ? m.packets[i + 1].ms : 2 * k->ms - m.packets[i - 1].ms;
      anim_add(a, rgba, w, h, end - m.packets[0].ms);
   }
   vpx_codec_destroy(&colour);
   vpx_codec_destroy(&alpha);
   SDL_free(m.packets);
   SDL_free(rgba);
   return anim_done(a);
}

struct anim *anim_load(const void *data, size_t size, int max_w, int max_h, size_t max_bytes)
{
   if (size > 4 && SDL_memcmp(data, "\x1A\x45\xDF\xA3", 4) == 0)
      return load_webm(data, size, max_w, max_h, max_bytes);
   if (size > 8 && SDL_memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0)
      return load_apng(data, size, max_w, max_h, max_bytes);
   return load_webp(data, size, max_w, max_h, max_bytes);
}

int anim_frame(const struct anim *a, Uint64 ms, int *next)
{
   int at = (int)(ms % (Uint64)a->length), i = 0;

   while (i + 1 < a->count && at >= a->ends[i])
      i++;
   *next = a->ends[i] - at;
   return i;
}

Uint8 *anim_first(const void *data, size_t size, int *w, int *h)
{
   WebPAnimInfo info;
   WebPAnimDecoder *dec = open_anim(data, size, &info);
   uint8_t *pixels;
   Uint8 *copy = NULL;
   int end;

   if (!dec)
      return NULL;
   if (WebPAnimDecoderGetNext(dec, &pixels, &end))
   {
      size_t bytes = (size_t)info.canvas_width * info.canvas_height * 4;
      if ((copy = SDL_malloc(bytes)))
         SDL_memcpy(copy, pixels, bytes);
      *w = (int)info.canvas_width;
      *h = (int)info.canvas_height;
   }
   WebPAnimDecoderDelete(dec);
   return copy;
}
