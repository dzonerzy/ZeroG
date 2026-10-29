/* Every encoding WebCore routes to ICU (TextCodecICU.cpp) must still open with
   the filtered ICU data. Encoding names come from the command line. */
#include <unicode/ucnv.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    int failures = 0;
    for (int i = 1; i < argc; i++) {
        UErrorCode error = U_ZERO_ERROR;
        UConverter *converter = ucnv_open(argv[i], &error);
        if (U_FAILURE(error) || !converter) {
            printf("FAIL  ucnv_open(%s): %s\n", argv[i], u_errorName(error));
            failures++;
        }
        ucnv_close(converter);
    }
    printf("%s  %d/%d ICU converters open\n", failures ? "FAIL" : "ok  ", argc - 1 - failures, argc - 1);
    return failures ? 1 : 0;
}
