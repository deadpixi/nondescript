/* calc.c — Demonstrates calling script functions from C. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nondescript.h"

int
main(int argc, char **argv)
{
    if (argc != 2){
        fprintf(stderr, "usage: %s SCRIPT\n", argv[0]);
        return EXIT_FAILURE;
    }

    NDSContext *ctx = NDSContext_new(NULL);
    NDSStatus status = NDSContext_evaluateFile(ctx, argv[1]);
    if (status != NDSStatus_OK){
        fprintf(stderr, "Error loading %s: %s\n", argv[1], NDSContext_getError(ctx));
        NDSContext_free(ctx);
        return EXIT_FAILURE;
    }

#define CHECK(expr)                                                                                \
    do{                                                                                            \
        if ((expr) != NDSStatus_OK){                                                               \
            fprintf(stderr, "Error: %s\n", NDSContext_getError(ctx));                              \
            NDSContext_free(ctx);                                                                  \
            return EXIT_FAILURE;                                                                   \
        }                                                                                          \
    } while(false)

    CHECK(NDSContext_ensureSlots(ctx, 1));
    NDSContext_setSlotNumber(ctx, 0, 10.0);
    CHECK(NDSContext_callFunction(ctx, "factorial", 1));
    printf("factorial(10) = %.0f\n", NDSContext_getSlotNumber(ctx, 0));

    /* Test recursion limit */
    CHECK(NDSContext_ensureSlots(ctx, 1));
    NDSContext_setSlotNumber(ctx, 0, 1000000.0);
    if (NDSContext_callFunction(ctx, "factorial", 1) != NDSStatus_OK)
        fprintf(stderr, "Error: %s\n", NDSContext_getError(ctx));
    else
        printf("factorial(1000000) = %.0f\n", NDSContext_getSlotNumber(ctx, 0));

    CHECK(NDSContext_ensureSlots(ctx, 1));
    NDSContext_setSlotNumber(ctx, 0, 20.0);
    CHECK(NDSContext_callFunction(ctx, "fibonacci", 1));
    printf("fibonacci(20) = %.0f\n", NDSContext_getSlotNumber(ctx, 0));

    int numbers[] = {1, 2, 7, 12, 17, 42, 97};
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++){
        CHECK(NDSContext_ensureSlots(ctx, 1));
        NDSContext_setSlotNumber(ctx, 0, (double)numbers[i]);
        CHECK(NDSContext_callFunction(ctx, "describe", 1));
        printf("%s\n", NDSContext_getSlotString(ctx, 0));
    }

#undef CHECK

    NDSContext_free(ctx);
    return EXIT_SUCCESS;
}
