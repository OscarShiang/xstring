#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef union {
    /* allow strings up to 15 bytes to stay on the stack
     * use the last byte as a null terminator and to store flags
     * much like fbstring:
     * https://github.com/facebook/folly/blob/master/folly/docs/FBString.md
     */
    char data[16];

    struct {
        uint8_t filler[15],
            /* how many free bytes in this stack allocated string
             * same idea as fbstring
             */
            space_left : 4,
            /* if it is on heap, set to 1 */
            is_ptr : 1, flag1 : 1, flag2 : 1, flag3 : 1;
    };

    /* heap allocated */
    struct {
        char *ptr;
        /* supports strings up to 2^54 - 1 bytes */
        size_t size : 54,
            /* capacity is always a power of 2 (unsigned)-1 */
            capacity : 6;
        /* the last 4 bits are important flags */
    };
} xs;

static inline bool xs_is_ptr(const xs *x)
{
    return x->is_ptr;
}
static inline bool xs_is_ref(const xs *x)
{
    return x->flag1;
}
static inline size_t xs_size(const xs *x)
{
    return xs_is_ptr(x) ? x->size : 15 - x->space_left;
}
static inline char *xs_data(const xs *x)
{
    return xs_is_ptr(x) ? (char *) x->ptr : (char *) x->data;
}
static inline size_t xs_capacity(const xs *x)
{
    return xs_is_ptr(x) ? ((size_t) 1 << x->capacity) - 1 : 15;
}

#define xs_literal_empty() \
    (xs) { .space_left = 15 }

static inline int ilog2(uint32_t n)
{
    return 32 - __builtin_clz(n) - 1;
}

xs *xs_new(xs *x, const void *p)
{
    *x = xs_literal_empty();
    size_t len = strlen(p) + 1;
    if (len > 16) {
        x->capacity = ilog2(len) + 1;
        x->size = len - 1;
        x->is_ptr = true;
        x->ptr = malloc((size_t) 1 << x->capacity);
        memcpy(x->ptr, p, len);
    } else {
        memcpy(x->data, p, len);
        x->space_left = 15 - (len - 1);
    }
    return x;
}

/* Memory leaks happen if the string is too long but it is still useful for
 * short strings.
 * "" causes a compile-time error if x is not a string literal or too long.
 */
#define xs_tmp(x)                                          \
    ((void) ((struct {                                     \
         _Static_assert(sizeof(x) <= 16, "it is too big"); \
         int dummy;                                        \
     }){1}),                                               \
     xs_new(&xs_literal_empty(), "" x))

/* grow up to specified size */
xs *xs_grow(xs *x, size_t len)
{
    if (len <= xs_capacity(x))
        return x;
    len = ilog2(len) + 1;
    if (xs_is_ptr(x))
        x->ptr = realloc(x->ptr, (size_t) 1 << len);
    else {
        char buf[16];
        memcpy(buf, x->data, 16);
        x->ptr = malloc((size_t) 1 << len);
        memcpy(x->ptr, buf, 16);
    }
    x->is_ptr = true;
    x->capacity = len;
    return x;
}

static inline xs *xs_newempty(xs *x)
{
    *x = xs_literal_empty();
    return x;
}

static xs *xs_dup(xs *dst)
{
    int len = xs_size(dst);
    char *srcptr = xs_data(dst);

    *dst = *xs_newempty(dst);
    char *dataptr = xs_data(dst);

    memcpy(dataptr, srcptr, len);
    return dst;
}

static inline xs *xs_free(xs *x)
{
    if (xs_is_ptr(x))
        free(xs_data(x));
    return xs_newempty(x);
}

xs *xs_concat(xs *string, const xs *prefix, const xs *suffix)
{
    size_t pres = xs_size(prefix), sufs = xs_size(suffix),
           size = xs_size(string), capacity = xs_capacity(string);

    char *pre = xs_data(prefix), *suf = xs_data(suffix),
         *data = xs_data(string);

    if (xs_is_ref(string))
        *string = *xs_dup(string);

    if (size + pres + sufs <= capacity) {
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);
        string->space_left = 15 - (size + pres + sufs);
    } else {
        xs_grow(string, size + pres + sufs);
        data = xs_data(string);
        memmove(data + pres, data, size);
        memcpy(data, pre, pres);
        memcpy(data + pres + size, suf, sufs + 1);
        string->size = size + pres + sufs;
    }
    return string;
}

xs *xs_trim(xs *x, const char *trimset)
{
    if (!trimset[0])
        return x;

    if (xs_is_ref(x))
        *x = *xs_dup(x);

    char *dataptr = xs_data(x), *orig = dataptr;

    /* similar to strspn/strpbrk but it operates on binary data */
    uint8_t mask[32] = {0};

#define check_bit(byte) (mask[(uint8_t) byte / 8] & 1 << (uint8_t) byte % 8)
#define set_bit(byte) (mask[(uint8_t) byte / 8] |= 1 << (uint8_t) byte % 8)

    size_t i, slen = xs_size(x), trimlen = strlen(trimset);

    for (i = 0; i < trimlen; i++)
        set_bit(trimset[i]);
    for (i = 0; i < slen; i++)
        if (!check_bit(dataptr[i]))
            break;
    for (; slen > 0; slen--)
        if (!check_bit(dataptr[slen - 1]))
            break;
    dataptr += i;
    slen -= i;

    /* reserved space as a buffer on the heap.
     * Do not reallocate immediately. Instead, reuse it as possible.
     * Do not shrink to in place if < 16 bytes.
     */
    memmove(orig, dataptr, slen);
    /* do not dirty memory unless it is needed */
    if (orig[slen])
        orig[slen] = 0;

    if (xs_is_ptr(x))
        x->size = slen;
    else
        x->space_left = 15 - slen;
    return x;
}

char *xs_tok(xs *str, const char *sep, char **last)
{
    char *dataptr;
    if (str) {
        if (xs_is_ref(str))
            *str = *xs_dup(str);
        dataptr = xs_data(str);
    } else 
	dataptr = *last;
    if (!*dataptr)
        return NULL;

    uint8_t mask[32] = {0};
    size_t i, len = strlen(dataptr), seplen = strlen(sep);
    for (i = 0; i < seplen; i++)
        set_bit(sep[i]);
    for (*last = dataptr; **last; ++(*last)) {
	if (check_bit(**last)) {
	    **last = '\0';
	    ++(*last);
	    break;
	}
    }
    return dataptr;
#undef check_bit
#undef set_bit
}

xs *xs_copy(xs *dst, xs *src)
{
    if (xs_is_ptr(dst))
        *dst = *xs_free(dst);
    if (xs_size(src) <= 16) {
        memcpy(dst, src, xs_size(src));
        dst->is_ptr = 0;
        dst->space_left = src->space_left;
    } else {
        dst->ptr = src->ptr;
        dst->is_ptr = 1;
        dst->size = xs_size(src);
        dst->flag1 = 1;  // indicate that the string is just a reference
    }
    return dst;
}

#include <stdio.h>
#define autofree __attribute__((cleanup(xs_free)))

int main()
{
    autofree xs string = *xs_tmp("the-is-a-test");
    /*
    xs_trim(&string, "\n ");
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));

    xs prefix = *xs_tmp("((("), suffix = *xs_tmp("))) hello word");
    xs_concat(&string, &prefix, &suffix);
    printf("[%s] : %2zu\n", xs_data(&string), xs_size(&string));
    */
    char *sep = " -", *last = NULL;
    char *delim = xs_tok(&string, sep, &last);
    while (delim != NULL) {
        printf("%s\n", delim);
        delim = xs_tok(NULL, sep, &last);
    }

    return 0;
}
