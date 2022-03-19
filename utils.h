#ifndef UTILS_H_
#define UTILS_H_

char* xstrdup(const char *s);
void* xmalloc(size_t size);
void* xcalloc(size_t nmemb, size_t size);
void* xrealloc(void *ptr, size_t size);
void* xreallocarray(void *ptr, size_t nmemb, size_t size);

#endif
