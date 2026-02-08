#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define BUNFS_PREFIX     "/$bunfs/root/"
#define BUNFS_PREFIX_LEN 13

static char cache_dir[512];

static void init_cache_dir(void) {
    if (cache_dir[0]) return;
    const char *dir = getenv("BUNFS_CACHE_DIR");
    if (dir) {
        snprintf(cache_dir, sizeof(cache_dir), "%s", dir);
    } else {
        const char *tmpdir = getenv("TMPDIR");
        if (!tmpdir) tmpdir = "/data/data/com.termux/files/usr/tmp";
        snprintf(cache_dir, sizeof(cache_dir), "%s/bunfs-libs", tmpdir);
    }
}

void *dlopen(const char *filename, int flags) {
    static void *(*real_dlopen)(const char *, int) = NULL;
    if (!real_dlopen)
        real_dlopen = dlsym(RTLD_NEXT, "dlopen");

    if (filename && strncmp(filename, BUNFS_PREFIX, BUNFS_PREFIX_LEN) == 0) {
        init_cache_dir();

        const char *name = filename + BUNFS_PREFIX_LEN;
        const char *slash = strrchr(name, '/');
        if (slash) name = slash + 1;

        char buf[512];
        snprintf(buf, sizeof(buf), "%s/%s", cache_dir, name);
        return real_dlopen(buf, flags);
    }

    return real_dlopen(filename, flags);
}
