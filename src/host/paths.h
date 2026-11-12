#ifndef XBEMU_PATHS_H
#define XBEMU_PATHS_H

/*
 * Guest (Windows-style) and host (POSIX) path conversion.
 *   guest -> host: drop a drive letter ("C:") and turn '\' into '/'.
 *                  "Z:\a\b" becomes "/a/b", so Z: is the host root.
 *   host -> guest: prefix "Z:" and turn '/' into '\'.
 * Returned strings are malloc'd and owned by the caller.
 */
char *guest_to_host_path(const char *guest);
char *host_to_guest_path(const char *host);

#endif
