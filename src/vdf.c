#include "vdf.h"

#include <SDL3/SDL.h>

#define VDF_END 8

struct reader
{
   const Uint8 *p, *end;
};

static char *read_cstr(struct reader *r)
{
   const Uint8 *start = r->p;

   while (r->p < r->end && *r->p)
      r->p++;
   if (r->p >= r->end)
      return NULL;
   r->p++;
   return SDL_strndup((const char *)start, (size_t)(r->p - start - 1));
}

static bool read_bytes(struct reader *r, Uint8 *dst, size_t n)
{
   if ((size_t)(r->end - r->p) < n)
      return false;
   SDL_memcpy(dst, r->p, n);
   r->p += n;
   return true;
}

/* A map's items, up to its end byte. */
static bool read_items(struct reader *r, struct vdf *map, int depth)
{
   struct vdf **tail = &map->child;

   if (depth > 32)
      return false;
   while (r->p < r->end)
   {
      Uint8 type = *r->p++;
      struct vdf *item;
      bool ok;

      if (type == VDF_END)
         return true;
      if (!(item = SDL_calloc(1, sizeof(*item))))
         return false;
      *tail = item;
      tail = &item->next;
      item->type = type;
      if (!(item->key = read_cstr(r)))
         return false;
      switch (type)
      {
         case VDF_MAP:
            ok = read_items(r, item, depth + 1);
            break;
         case VDF_STRING:
            ok = (item->str = read_cstr(r)) != NULL;
            break;
         case VDF_INT:
            ok = read_bytes(r, item->raw, 4);
            item->num = (Uint32)item->raw[0] | (Uint32)item->raw[1] << 8
               | (Uint32)item->raw[2] << 16 | (Uint32)item->raw[3] << 24;
            break;
         case VDF_FLOAT:
            ok = read_bytes(r, item->raw, 4);
            break;
         case VDF_UINT64:
            ok = read_bytes(r, item->raw, 8);
            break;
         default:
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION, "VDF: unknown type %d", type);
            ok = false;
            break;
      }
      if (!ok)
         return false;
   }
   return false;
}

struct vdf *vdf_load(const char *path)
{
   struct vdf *root = SDL_calloc(1, sizeof(*root));
   size_t size = 0;
   Uint8 *data;

   if (!root)
      return NULL;
   root->type = VDF_MAP;
   if (!(data = SDL_LoadFile(path, &size)))
   {
      vdf_map(root, "shortcuts");
      return root;
   }
   {
      struct reader r = { data, data + size };
      bool ok = read_items(&r, root, 0);
      SDL_free(data);
      if (!ok)
      {
         SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "VDF: %s isn't readable", path);
         vdf_free(root);
         return NULL;
      }
   }
   vdf_map(root, "shortcuts");
   return root;
}

static void write_items(SDL_IOStream *io, const struct vdf *map)
{
   const struct vdf *item;

   for (item = map->child; item; item = item->next)
   {
      SDL_WriteU8(io, item->type);
      SDL_WriteIO(io, item->key, SDL_strlen(item->key) + 1);
      switch (item->type)
      {
         case VDF_MAP:
            write_items(io, item);
            break;
         case VDF_STRING:
            SDL_WriteIO(io, item->str, SDL_strlen(item->str) + 1);
            break;
         case VDF_INT:
            SDL_WriteU32LE(io, item->num);
            break;
         case VDF_FLOAT:
            SDL_WriteIO(io, item->raw, 4);
            break;
         case VDF_UINT64:
            SDL_WriteIO(io, item->raw, 8);
            break;
      }
   }
   SDL_WriteU8(io, VDF_END);
}

/* Written beside it and moved over it, so a failed write never leaves
 * Steam half a file. The first save keeps the original as .fusebox-backup. */
bool vdf_save(const struct vdf *root, const char *path)
{
   char tmp[1024], backup[1024];
   SDL_IOStream *io;

   SDL_snprintf(tmp, sizeof(tmp), "%s.tmp", path);
   SDL_snprintf(backup, sizeof(backup), "%s.fusebox-backup", path);
   if (!(io = SDL_IOFromFile(tmp, "wb")))
      return false;
   write_items(io, root);
   if (!SDL_CloseIO(io))
   {
      SDL_RemovePath(tmp);
      return false;
   }
   if (SDL_GetPathInfo(path, NULL) && !SDL_GetPathInfo(backup, NULL))
      SDL_CopyFile(path, backup);
   return SDL_RenamePath(tmp, path);
}

void vdf_free(struct vdf *node)
{
   while (node)
   {
      struct vdf *next = node->next;
      vdf_free(node->child);
      SDL_free(node->key);
      SDL_free(node->str);
      SDL_free(node);
      node = next;
   }
}

struct vdf *vdf_find(const struct vdf *map, const char *key)
{
   struct vdf *item;

   for (item = map ? map->child : NULL; item; item = item->next)
      if (SDL_strcasecmp(item->key, key) == 0)
         return item;
   return NULL;
}

const char *vdf_str(const struct vdf *map, const char *key)
{
   struct vdf *item = vdf_find(map, key);
   return item && item->type == VDF_STRING ? item->str : "";
}

Uint32 vdf_int(const struct vdf *map, const char *key)
{
   struct vdf *item = vdf_find(map, key);
   return item && item->type == VDF_INT ? item->num : 0;
}

static struct vdf *add(struct vdf *map, const char *key, Uint8 type)
{
   struct vdf *item = SDL_calloc(1, sizeof(*item)), **tail = &map->child;

   if (!item)
      return NULL;
   item->type = type;
   item->key = SDL_strdup(key);
   while (*tail)
      tail = &(*tail)->next;
   *tail = item;
   return item;
}

struct vdf *vdf_set_str(struct vdf *map, const char *key, const char *value)
{
   struct vdf *item = vdf_find(map, key);

   if (item && item->type != VDF_STRING)
   {
      vdf_remove(map, item);
      item = NULL;
   }
   if (!item && !(item = add(map, key, VDF_STRING)))
      return NULL;
   SDL_free(item->str);
   item->str = SDL_strdup(value ? value : "");
   return item;
}

struct vdf *vdf_set_int(struct vdf *map, const char *key, Uint32 value)
{
   struct vdf *item = vdf_find(map, key);

   if (item && item->type != VDF_INT)
   {
      vdf_remove(map, item);
      item = NULL;
   }
   if (!item && !(item = add(map, key, VDF_INT)))
      return NULL;
   item->num = value;
   return item;
}

struct vdf *vdf_map(struct vdf *map, const char *key)
{
   struct vdf *item = vdf_find(map, key);

   if (item && item->type == VDF_MAP)
      return item;
   if (item)
      vdf_remove(map, item);
   return add(map, key, VDF_MAP);
}

void vdf_remove(struct vdf *map, struct vdf *item)
{
   struct vdf **p;

   for (p = &map->child; *p; p = &(*p)->next)
      if (*p == item)
      {
         *p = item->next;
         item->next = NULL;
         vdf_free(item);
         return;
      }
}
