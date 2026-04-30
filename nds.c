#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nondescript.h"

#define REPL_MAX_BUFFER (64 * 1024)
#define REPL_MAX_LINE   4096

static NDSStatus
setup(NDSConfigHandle *h, void *userPointer)
{
    (void)userPointer;
    NDSConfigHandle_enableImport(h, NDSDefaultFileReader, NULL);
    return NDSStatus_OK;
}

static int
runFile(const char *path)
{
    NDSConfig config = {.setup = setup};
    NDSContext *ctx = NDSContext_new(&config);
    if (!ctx){
        fprintf(stderr, "nds: failed to create context\n");
        return -1;
    }

    NDSStatus status = NDSContext_evaluateFile(ctx, path);
    if (status != NDSStatus_OK){
        const char *err = NDSContext_getError(ctx);
        fprintf(stderr, "nds: %s\n", err? err : "unknown error");
    }

    NDSContext_free(ctx);
    return status == NDSStatus_OK? 0 : 1;
}

static int
runRepl(void)
{
    NDSConfig config = {.setup = setup};
    NDSContext *ctx = NDSContext_new(&config);
    if (!ctx){
        fprintf(stderr, "nds: failed to create context\n");
        return -1;
    }

    char buffer[REPL_MAX_BUFFER] = {0};
    size_t bufLen = 0;

    for (;;){
        fputs(bufLen == 0? ">>> " : "... ", stdout);
        fflush(stdout);

        char line[REPL_MAX_LINE] = {0};
        if (!fgets(line, sizeof(line), stdin)){
            fputc('\n', stdout);
            break;
        }

        size_t lineLen = strlen(line);
        if (bufLen + lineLen >= sizeof(buffer)){
            fprintf(stderr, "nds: input too large\n");
            bufLen = 0;
            continue;
        }
        memcpy(buffer + bufLen, line, lineLen);
        bufLen += lineLen;

        NDSStatus status = NDSContext_evaluate(ctx, buffer, bufLen, "(repl)");
        if (status == NDSStatus_OK){
            bufLen = 0;
            continue;
        }
        if (NDSContext_errorAtEOF(ctx))
            continue;

        const char *err = NDSContext_getError(ctx);
        fprintf(stderr, "%s\n", err? err : "unknown error");
        bufLen = 0;
    }

    NDSContext_free(ctx);
    return 0;
}

int
main(int argc, char **argv)
{
    if (argc > 2){
        fprintf(stderr, "usage: %s [file]\n", argv[0]);
        return 2;
    }
    if (argc == 2)
        return runFile(argv[1]);
    return runRepl();
}
