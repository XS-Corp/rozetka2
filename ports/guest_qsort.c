/* The comparator remains x86 guest code, called by the guest algorithm. */
#include <stdint.h>
#include <stdlib.h>

static void swap(unsigned char *a, unsigned char *b, size_t width)
{
    for (size_t i = 0; i < width; i++) { unsigned char value = a[i]; a[i] = b[i]; b[i] = value; }
}
static void sift(unsigned char *base, size_t root, size_t last, size_t width,
                 int (*compare)(const void *, const void *))
{
    if (last == 0) return;
    while (root <= (last - 1) / 2) {
        size_t child = root * 2 + 1;
        if (child < last && compare(base + child * width, base + (child + 1) * width) < 0) child++;
        if (compare(base + root * width, base + child * width) >= 0) return;
        swap(base + root * width, base + child * width, width);
        root = child;
    }
}
void qsort(void *pointer, size_t count, size_t width, int (*compare)(const void *, const void *))
{
    if (count < 2 || width == 0) return;
    if (count > SIZE_MAX / width || compare == NULL) abort();
    unsigned char *base = pointer;
    for (size_t root = count / 2; root != 0;) sift(base, --root, count - 1, width, compare);
    for (size_t last = count - 1; last != 0; last--) {
        swap(base, base + last * width, width);
        sift(base, 0, last - 1, width, compare);
    }
}
