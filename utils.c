#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void
oom_error(const char *func)
{
    fprintf(stderr, "fatal error in %s: out of memory\n", func);
    // Prefer abort() over exit(1) for better debuggability.
    abort();
}

char*
xstrdup(const char *s)
{
    char *p = strdup(s);
    if (p == NULL) oom_error(__func__);
    return p;
}

void*
xmalloc(size_t size)
{
    void *p = malloc(size);
    if (p == NULL) oom_error(__func__);
    return p;
}

void*
xcalloc(size_t nmemb, size_t size)
{
    void *p = calloc(nmemb, size);
    if (p == NULL) oom_error(__func__);
    return p;
}

void*
xrealloc(void *ptr, size_t size)
{
    void *p = realloc(ptr, size);
    if (p == NULL) oom_error(__func__);
    return p;
}

void*
xreallocarray(void *ptr, size_t nmemb, size_t size)
{
    size_t alloc_size;
    void *p;
    if (__builtin_mul_overflow(nmemb, size, &alloc_size))
        oom_error(__func__);
    p = realloc(ptr, alloc_size);
    if (p == NULL) oom_error(__func__);
    return p;
}
