#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../nondescript.h"

#define UNUSED(x) ((void)(x))


/* A host type to be used in tests. */
enum{
    PointProp_X = 0,
    PointProp_Y = 1
};

static NDSTypeID Point_typeID;
static NDSChunkID Point_coordinateChunkID;

static NDSStatus
Point_chunkGet(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != Point_coordinateChunkID){
        NDSContext_setError(ctx, "unsupported chunk for point");
        return NDSStatus_Error;
    }
    int idx = (int)NDSContext_getSlotNumber(ctx, 1);
    if (idx == 1)
        NDSContext_getObjectProperty(ctx, 0, PointProp_X, 0);
    else if (idx == 2)
        NDSContext_getObjectProperty(ctx, 0, PointProp_Y, 0);
    else{
        NDSContext_setError(ctx, "coordinate index must be 1 or 2");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static NDSStatus
Point_equals(NDSContext *ctx)
{
    NDSContext_ensureSlots(ctx, 4);
    NDSContext_getObjectProperty(ctx, 0, PointProp_X, 2);
    NDSContext_getObjectProperty(ctx, 1, PointProp_X, 3);
    if (!NDSContext_slotsEqual(ctx, 2, 3)){
        NDSContext_setSlotBoolean(ctx, 0, false);
        return NDSStatus_OK;
    }
    NDSContext_getObjectProperty(ctx, 0, PointProp_Y, 2);
    NDSContext_getObjectProperty(ctx, 1, PointProp_Y, 3);
    NDSContext_setSlotBoolean(ctx, 0, NDSContext_slotsEqual(ctx, 2, 3));
    return NDSStatus_OK;
}

static NDSStatus
Point_toString(NDSContext *ctx)
{
    NDSContext_ensureSlots(ctx, 2);
    NDSContext_getObjectProperty(ctx, 0, PointProp_X, 1);
    double x = NDSContext_getSlotNumber(ctx, 1);
    NDSContext_getObjectProperty(ctx, 0, PointProp_Y, 1);
    double y = NDSContext_getSlotNumber(ctx, 1);
    char buf[128] = {0};
    int n = snprintf(buf, sizeof(buf), "(%.14g, %.14g)", x, y);
    return NDSContext_setSlotString(ctx, 0, buf, (size_t)n);
}

static NDSStatus
Function_makePoint(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    double x = NDSContext_getSlotNumber(ctx, 0);
    double y = NDSContext_getSlotNumber(ctx, 1);
    NDSContext_ensureSlots(ctx, 3);
    NDSContext_setSlotNewObject(ctx, 2, Point_typeID);
    NDSContext_setSlotNumber(ctx, 0, x);
    NDSContext_setObjectProperty(ctx, 2, PointProp_X, 0);
    NDSContext_setSlotNumber(ctx, 0, y);
    NDSContext_setObjectProperty(ctx, 2, PointProp_Y, 0);
    NDSContext_copySlot(ctx, 2, 0);
    return NDSStatus_OK;
}

static int assertCount = 0;
static int assertFails = 0;
static const char *currentFile = NULL;

static NDSStatus
Command_assert(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    assertCount++;
    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_Boolean){
        assertFails++;
        fprintf(stderr, "  ASSERT requires a boolean in %s\n", currentFile);
        NDSContext_setError(ctx, "assert requires a boolean expression");
        return NDSStatus_Error;
    }
    if (!NDSContext_getSlotBoolean(ctx, 0)){
        assertFails++;
        fprintf(stderr, "  ASSERT FAILED in %s\n", currentFile);
        NDSContext_setError(ctx, "assertion failed");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static NDSStatus
Command_forceGarbageCollection(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    NDSContext_collectGarbage(ctx);
    return NDSStatus_OK;
}

static NDSStatus
Function_hostAdd(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    double a = NDSContext_getSlotNumber(ctx, 0);
    double b = NDSContext_getSlotNumber(ctx, 1);
    NDSContext_setSlotNumber(ctx, 0, a + b);
    return NDSStatus_OK;
}

static NDSStatus
Function_callBack(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    const char *funcName = NDSContext_getSlotString(ctx, 0);
    NDSContext_copySlot(ctx, 1, 0);
    NDSContext_ensureSlots(ctx, 1);
    return NDSContext_callFunction(ctx, funcName, 1);
}

/* callBackValue(func, arg) — same as callBack but takes the function value directly */
static NDSStatus
Function_callBackvalue(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    NDSContext_ensureSlots(ctx, 3);
    NDSContext_copySlot(ctx, 0, 2);
    NDSContext_copySlot(ctx, 1, 0);
    return NDSContext_callSlot(ctx, 2, 1);
}

/* swap EXPR and EXPR into VAR and VAR
 * Two expression slots, two variable slots — tests both slot types
 * tests autovivify of slots.
 */
static NDSStatus
Command_swap(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    NDSContext_copySlot(ctx, 1, NDSContext_VarSlot(0));
    NDSContext_copySlot(ctx, 0, NDSContext_VarSlot(1));
    return NDSStatus_OK;
}

/* unpack EXPR into VAR and VAR and VAR
 * One expression (a list), three variable slots. 
 * Should autovivify slots.
 */
static NDSStatus
Command_unpack(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    if (!NDSContext_isSlotList(ctx, 0)){
        NDSContext_setError(ctx, "unpack requires a list");
        return NDSStatus_Error;
    }
    size_t count = NDSContext_getSlotListCount(ctx, 0);
    NDSContext_ensureSlots(ctx, 2);
    for (size_t i = 0; i < 3 && i < count; i++){
        NDSContext_getSlotListElement(ctx, 0, i, 1);
        NDSContext_copySlot(ctx, 1, NDSContext_VarSlot((int)i));
    }
    for (size_t i = count; i < 3; i++)
        NDSContext_setSlotNothing(ctx, NDSContext_VarSlot((int)i));
    return NDSStatus_OK;
}

static NDSStatus
setup(NDSConfigHandle *h, void *userPointer)
{
    UNUSED(userPointer);

    NDSPatternStep assertCmd[] ={
        {NDSPatternStep_Word, "assert"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, assertCmd, Command_assert);

    NDSPatternStep forceGCCmd[] ={
        {NDSPatternStep_Word, "force"},
        {NDSPatternStep_Word, "garbage"},
        {NDSPatternStep_Word, "collection"},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, forceGCCmd, Command_forceGarbageCollection);

    NDSPatternStep swapCmd[] ={
        {NDSPatternStep_Word, "swap"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "with"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "into"},
        {NDSPatternStep_Variable, NULL},
        {NDSPatternStep_Word, "and"},
        {NDSPatternStep_Variable, NULL},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, swapCmd, Command_swap);

    NDSPatternStep unpackCmd[] ={
        {NDSPatternStep_Word, "unpack"},
        {NDSPatternStep_Expression, NULL},
        {NDSPatternStep_Word, "into"},
        {NDSPatternStep_Variable, NULL},
        {NDSPatternStep_Word, "and"},
        {NDSPatternStep_Variable, NULL},
        {NDSPatternStep_Word, "and"},
        {NDSPatternStep_Variable, NULL},
        {NDSPatternStep_End, NULL}
    };
    NDSConfigHandle_registerCommand(h, unpackCmd, Command_unpack);

    NDSConfigHandle_registerFunction(h, "hostAdd", Function_hostAdd, 2);
    NDSConfigHandle_registerFunction(h, "callBack", Function_callBack, 2);
    NDSConfigHandle_registerFunction(h, "callBackValue", Function_callBackvalue, 2);
    NDSConfigHandle_registerFunction(h, "point", Function_makePoint, 2);

    Point_coordinateChunkID = NDSConfigHandle_registerChunkID(h, "coordinate");
    static const char *pointPropNames[] = {"x", "y"};
    static NDSTypeDescriptor pointDesc ={
        .name = "point",
        .propertyNames = pointPropNames,
        .propertyCount = 2,
        .chunk_get = Point_chunkGet,
        .equals = Point_equals,
        .to_string = Point_toString,
    };
    Point_typeID = NDSConfigHandle_registerType(h, &pointDesc);

    NDSConfigHandle_enableImport(h, NDSDefaultFileReader, NULL);

    return NDSStatus_OK;
}

int
main(int argc, char **argv)
{
    if (argc < 2){
        fprintf(stderr, "usage: %s test1.nds [test2.nds ...]\n", argv[0]);
        return EXIT_FAILURE;
    }

    int totalTests = 0;
    int totalPassed = 0;
    int totalFailed = 0;

    for (int i = 1; i < argc; i++){
        const char *file = argv[i];

        totalTests++;
        assertCount = 0;
        assertFails = 0;
        currentFile = file;

        NDSConfig config = {.setup = setup};
        NDSContext *ctx = NDSContext_new(&config);
        if (!ctx){
            fprintf(stderr, "FAIL %s — could not create context\n", file);
            totalFailed++;
            continue;
        }

        NDSStatus status = NDSContext_evaluateFile(ctx, file);
        if (status != NDSStatus_OK){
            fprintf(stderr, "FAIL %s — %s\n", file, NDSContext_getError(ctx));
            totalFailed++;
        } else if (assertFails > 0){
            fprintf(stderr, "FAIL %s — %d/%d assertions failed\n", file, assertFails, assertCount);
            totalFailed++;
        } else{
            printf("PASS %s (%d assertions)\n", file, assertCount);
            totalPassed++;
        }

        NDSContext_free(ctx);
    }

    printf("\n%d/%d tests passed", totalPassed, totalTests);
    if (totalFailed > 0)
        printf(", %d failed", totalFailed);
    printf("\n");

    return totalFailed > 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
