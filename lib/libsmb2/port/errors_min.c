/* Upstream's src/errors.c without its 25 KB table of NT status names.
 *
 * nterror_to_str() only feeds libsmb2's own error messages, and Files works from the errno and
 * the NT code (src/smb_backend.cpp), so the screen doesn't change. library.json leaves errors.c
 * out of the build and compiles this file in its place. It includes upstream's file unchanged,
 * with that nterror_to_str() renamed so that the linker drops it and its strings, and keeps
 * nterror_to_errno() as it is. The messages show the status code where the name was.
 */

#define nterror_to_str libsmb2_nterror_to_str_unused
#include "../src/errors.c"
#undef nterror_to_str

#include <stdio.h>

const char *nterror_to_str(uint32_t status) {
        /* One copy per task: libsmb2 runs on whichever task calls it. */
        static __thread char text[11];
        snprintf(text, sizeof(text), "0x%08lx", (unsigned long)status);
        return text;
}
