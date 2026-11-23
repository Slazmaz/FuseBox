#ifndef FUSEBOX_HTTP_H
#define FUSEBOX_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL_atomic.h>

/* HTTPS through libcurl, for background threads (names, covers). curl
 * and OpenSSL load on first use, never on the way to the first frame. */

/* GET, or POST when post isn't NULL. header: one extra header line
 * ("Authorization: Bearer ..."), or NULL. user_agent NULL means
 * "FuseBox/0.1 (<platform>)". Setting *abort cuts the transfer short.
 * *body is NUL-terminated, SDL_free'd by the caller. Returns the HTTP
 * status, or -1 when the network failed or the transfer was aborted. */
int http_request(const char *url, const char *post, const char *header,
      const char *user_agent, SDL_AtomicInt *abort, char **body, size_t *size,
      long timeout_s);
/* GET into the file at path, for what's too big to hold in memory. *percent
 * follows the transfer (0-100, -1 until its size is known); may be NULL.
 * Only a stalled transfer times out. Returns the HTTP status; the file is
 * only left there on 200. */
int http_download(const char *url, const char *path, SDL_AtomicInt *abort,
      SDL_AtomicInt *percent);
/* Percent-encoded copy of a URL path or query part; SDL_free it. */
char *http_escape(const char *text);
/* At exit, once no thread uses HTTP any more. */
void http_quit(void);

#endif
