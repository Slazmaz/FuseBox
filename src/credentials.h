#ifndef FUSEBOX_CREDENTIALS_H
#define FUSEBOX_CREDENTIALS_H

#include <stdbool.h>
#include <stddef.h>

/* API keys (steamgriddb_api_key) from <config>/credentials.cfg, as
 * key = "value": a file of its own, kept out of library.json, which other
 * apps read. Never log what comes back. */

/* Reads the file each call. */
bool credentials_get(const char *config_dir, const char *key, char *dst, size_t size);
/* Sets key in dir's credentials.cfg (made if missing), the rest kept; an
 * empty value takes it out. */
bool credentials_set(const char *dir, const char *key, const char *value);

#endif
