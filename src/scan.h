#ifndef FUSEBOX_SCAN_H
#define FUSEBOX_SCAN_H

/* Finds installed games in other launchers and adds them to the library
 * (installed set): Epic Games Launcher, GOG Galaxy and Battle.net on Windows, Heroic
 * (Epic and GOG) on both. Returns how many it found. Blocking: a few
 * small files and registry keys. */
int scan_launchers(void);

#endif
