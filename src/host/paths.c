#define _DEFAULT_SOURCE
#include "paths.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

char *guest_to_host_path(const char *guest) {
    if (!guest) return NULL;
    if (isalpha((unsigned char)guest[0]) && guest[1] == ':')
        guest += 2;
    char *out = strdup(guest);
    if (!out) return NULL;
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return out;
}

char *host_to_guest_path(const char *host) {
    if (!host) return NULL;
    size_t n = strlen(host);
    char *out = malloc(n + 3);
    if (!out) return NULL;
    memcpy(out, "Z:", 2);
    memcpy(out + 2, host, n + 1);
    for (char *p = out + 2; *p; p++)
        if (*p == '/') *p = '\\';
    return out;
}
