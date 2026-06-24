#ifndef NEPTUNE_DLFCN_H
#define NEPTUNE_DLFCN_H

#define RTLD_LAZY 1
void *dlopen(const char *path, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);

#endif
