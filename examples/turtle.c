/* turtle.c — Turtle graphics via Nondescript host commands. */
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nondescript.h"

#ifndef M_PI
#define M_PI (acos(-1.0))
#endif

#define UNUSED(x) ((void)(x))

#define COLOR_NAME_MAX 63
typedef struct Turtle Turtle;
struct Turtle{
    double x, y;
    double angle;
    bool penDown;
    char color[COLOR_NAME_MAX + 1];
    double width;
};

typedef struct Canvas Canvas;
struct Canvas{
    char *data;
    size_t length;
    size_t capacity;
};

typedef struct Context Context;
struct Context{
    Turtle turtle;
    Canvas canvas;
};

static Context context = {0};

static bool
Canvas_ensureCapacity(Canvas *self, size_t desiredCapacity)
{
    if (self->capacity >= desiredCapacity)
        return true;

    char *newData = realloc(self->data, desiredCapacity);
    if (!newData)
        return false;

    self->data = newData;
    self->capacity = desiredCapacity;
    return true;
}

static void
Canvas_append(Canvas *self, const char *fmt, ...)
{
    va_list ap;
    va_list ap2;

    va_start(ap, fmt);
    va_copy(ap2, ap);

    /* Determine the length needed. */
    int needed = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (needed < 0){
        fprintf(stderr, "formatting error");
        exit(EXIT_FAILURE);
    }
    if (self->length + needed < self->length){
        fprintf(stderr, "overflow");
        exit(EXIT_FAILURE);
    }
    if (!Canvas_ensureCapacity(self, self->length + needed + 1)){
        fprintf(stderr, "out of memory\n");
        exit(EXIT_FAILURE);
    }

    int n = vsnprintf(self->data + self->length, self->capacity - self->length, fmt, ap2);
    va_end(ap2);
    if (n < 0){
        fprintf(stderr, "formatting error");
        exit(EXIT_FAILURE);
    }

    self->length += n;
}

static NDSStatus
Turtle_moveForward(NDSContext *ctx, int argCount)
{
    UNUSED(argCount); /* can't get here without the correct number of arguments */

    double distance = NDSContext_getSlotNumber(ctx, 0);
    double rad = context.turtle.angle * M_PI / 180.0;
    double nx = context.turtle.x + sin(rad) * distance;
    double ny = context.turtle.y - cos(rad) * distance;

    if (context.turtle.penDown){
        Canvas_append(&context.canvas,
            "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" "
            "stroke=\"%s\" stroke-width=\"%.1f\" stroke-linecap=\"round\"/>\n",
            context.turtle.x, context.turtle.y, nx, ny, context.turtle.color, context.turtle.width);
    }

    context.turtle.x = nx;
    context.turtle.y = ny;

    return NDSStatus_OK;
}

static NDSStatus
Turtle_moveBackward(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    double dist = NDSContext_getSlotNumber(ctx, 0);
    NDSContext_setSlotNumber(ctx, 0, -dist);
    return Turtle_moveForward(ctx, argCount);
}

static NDSStatus
Turtle_turnRight(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    context.turtle.angle += NDSContext_getSlotNumber(ctx, 0);
    return NDSStatus_OK;
}

static NDSStatus
Turtle_turnLeft(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    context.turtle.angle -= NDSContext_getSlotNumber(ctx, 0);
    return NDSStatus_OK;
}

static NDSStatus
Turtle_penUp(NDSContext *ctx, int argCount)
{
    UNUSED(ctx);
    UNUSED(argCount);
    context.turtle.penDown = false;
    return NDSStatus_OK;
}

static NDSStatus
Turtle_penDown(NDSContext *ctx, int argCount)
{
    UNUSED(ctx);
    UNUSED(argCount);
    context.turtle.penDown = true;
    return NDSStatus_OK;
}

static NDSStatus
Turtle_setPenColor(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    const char *c = NDSContext_getSlotString(ctx, 0);
    snprintf(context.turtle.color, sizeof(context.turtle.color), "%s", c);
    return NDSStatus_OK;
}

static NDSStatus
Turtle_setPenWidth(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    context.turtle.width = NDSContext_getSlotNumber(ctx, 0);
    return NDSStatus_OK;
}

static NDSStatus
setup(NDSConfigHandle *h, void *userPointer)
{
    UNUSED(userPointer);

    NDSPatternStep moveForward[] ={
        {NDSPatternStep_Word, "move"},
        {NDSPatternStep_Word, "forward"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "steps"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, moveForward, Turtle_moveForward);

    NDSPatternStep moveBackward[] ={
        {NDSPatternStep_Word, "move"},
        {NDSPatternStep_Word, "backward"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "steps"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, moveBackward, Turtle_moveBackward);

    NDSPatternStep turnRight[] ={
        {NDSPatternStep_Word, "turn"},
        {NDSPatternStep_Word, "right"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "degrees"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, turnRight, Turtle_turnRight);

    NDSPatternStep turnLeft[] ={
        {NDSPatternStep_Word, "turn"},
        {NDSPatternStep_Word, "left"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "degrees"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, turnLeft, Turtle_turnLeft);

    NDSPatternStep penUp[] ={
        {NDSPatternStep_Word, "pen"},
        {NDSPatternStep_Word, "up"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, penUp, Turtle_penUp);

    NDSPatternStep penDown[] ={
        {NDSPatternStep_Word, "pen"},
        {NDSPatternStep_Word, "down"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, penDown, Turtle_penDown);

    NDSPatternStep setPenColor[] ={
        {NDSPatternStep_Word, "change"},
        {NDSPatternStep_Word, "pen"},
        {NDSPatternStep_Word, "color"},
        {NDSPatternStep_Word, "to"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, setPenColor, Turtle_setPenColor);

    NDSPatternStep setPenWidth[] ={
        {NDSPatternStep_Word, "change"},
        {NDSPatternStep_Word, "pen"},
        {NDSPatternStep_Word, "width"},
        {NDSPatternStep_Word, "to"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, setPenWidth, Turtle_setPenWidth);

    return NDSStatus_OK;
}

int
main(int argc, char **argv)
{
    if (argc != 3){
        fprintf(stderr, "usage: %s TURTLESCRIPT OUTPUTFILE\n", argv[0]);
        return EXIT_FAILURE;
    }

    /* Initialize the turtle. */
    context.turtle.x = 300;
    context.turtle.y = 300;
    context.turtle.angle = 0;
    context.turtle.penDown = true;
    context.turtle.width = 1;
    strncpy(context.turtle.color, "black", COLOR_NAME_MAX);

    /* Initialize the NDS config. */
    NDSConfig config = {.setup = setup};
    NDSContext *ctx = NDSContext_new(&config);

    NDSStatus status = NDSContext_evaluateFile(ctx, argv[1]);
    if (status != NDSStatus_OK){
        fprintf(stderr, "error: %s\n", NDSContext_getError(ctx));
        NDSContext_free(ctx);
        free(context.canvas.data);
        return EXIT_FAILURE;
    }

    /* Write SVG */
    FILE *out = fopen(argv[2], "w");
    if (!out){
        fprintf(stderr, "could not open output file %s\n", argv[2]);
        NDSContext_free(ctx);
        free(context.canvas.data);
        return EXIT_FAILURE;
    }
    fprintf(out, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
                 "width=\"600\" height=\"600\" "
                 "style=\"background: white\">\n"
                 "%s"
                 "</svg>\n", context.canvas.data ? context.canvas.data : "");
    fclose(out);

    NDSContext_free(ctx);
    free(context.canvas.data);
    return EXIT_SUCCESS;
}
