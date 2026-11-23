#ifndef FUSEBOX_VDF_H
#define FUSEBOX_VDF_H

#include <stdbool.h>
#include <SDL3/SDL_stdinc.h>

/* Steam's binary KeyValues (shortcuts.vdf): read into a tree, changed,
 * written back with everything FuseBox doesn't know kept as it was. */

enum vdf_type
{
   VDF_MAP = 0,
   VDF_STRING = 1,
   VDF_INT = 2,
   VDF_FLOAT = 3,
   VDF_UINT64 = 7
};

struct vdf
{
   Uint8 type;
   char *key;
   char *str;
   /* VDF_INT's value; VDF_FLOAT's and VDF_UINT64's bytes as read. */
   Uint32 num;
   Uint8 raw[8];
   struct vdf *child, *next;
};

/* The file's top level (a map), or NULL. A missing file gives an empty
 * one holding an empty "shortcuts" map. */
struct vdf *vdf_load(const char *path);
bool vdf_save(const struct vdf *root, const char *path);
void vdf_free(struct vdf *node);

/* The map's item named key (Steam's own case varies: "AppName" and
 * "appname"), NULL if missing. */
struct vdf *vdf_find(const struct vdf *map, const char *key);
const char *vdf_str(const struct vdf *map, const char *key);
Uint32 vdf_int(const struct vdf *map, const char *key);
/* Sets (adding at the end if missing) the map's item. */
struct vdf *vdf_set_str(struct vdf *map, const char *key, const char *value);
struct vdf *vdf_set_int(struct vdf *map, const char *key, Uint32 value);
/* The map's map named key, added empty if missing. */
struct vdf *vdf_map(struct vdf *map, const char *key);
/* Takes item out of map and frees it. */
void vdf_remove(struct vdf *map, struct vdf *item);

#endif
