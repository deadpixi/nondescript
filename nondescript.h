#ifndef NONDESCRIPT_H
#define NONDESCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NDS_VERSION_MAJOR 0
#define NDS_VERSION_MINOR 5
#define NDS_VERSION_STRING "0.5"

#if defined(__GNUC__) || defined(__clang__)
#define NDS_PRINTF(fmtIndex, argIndex) __attribute__((format(printf, fmtIndex, argIndex)))
#else
#define NDS_PRINTF(fmtIndex, argIndex)
#endif

/* Status codes */
typedef enum {
    NDSStatus_OK = 0,
    NDSStatus_Error = -1
} NDSStatus;

/* ID types */
typedef uint32_t NDSTypeID;
typedef uint32_t NDSAtom;
typedef uint32_t NDSChunkID;
typedef size_t NDSMapIterCookie;

#define NDSTypeID_Invalid UINT32_MAX
#define NDSAtom_Invalid UINT32_MAX
#define NDSChunkID_Invalid UINT32_MAX
#define NDSMapIterCookie_Start 0

/* Forward declarations */
typedef struct NDSAllocator NDSAllocator;
typedef struct NDSContext NDSContext;
typedef struct NDSConfigHandle NDSConfigHandle;
typedef struct NDSScript NDSScript;

/* Built-in type IDs */
typedef enum {
    NDSValueType_Nothing,
    NDSValueType_Boolean,
    NDSValueType_Number,
    NDSValueType_String,
    NDSValueType_List,
    NDSValueType_Map,
    NDSValueType_Error,
    NDSValueType_Function,
    NDSValueType_Entry,

    NDSValueType_BuiltinCount
} NDSValueType;


/* Type descriptors */
typedef struct NDSTypeDescriptor NDSTypeDescriptor;
struct NDSTypeDescriptor {
    const char *name;
    const char **propertyNames;
    int propertyCount;
    size_t extraDataSize;


    NDSStatus (*property_get)(NDSContext *ctx, NDSAtom nameAtom);
    NDSStatus (*contains)(NDSContext *ctx);
    NDSStatus (*chunk_get)(NDSContext *ctx, NDSChunkID chunkID);
    NDSStatus (*chunk_get_range)(NDSContext *ctx, NDSChunkID chunkID);
    NDSStatus (*chunk_set)(NDSContext *ctx, NDSChunkID chunkID);
    NDSStatus (*chunk_delete)(NDSContext *ctx, NDSChunkID chunkID);
    NDSStatus (*chunk_get_every)(NDSContext *ctx, NDSChunkID chunkID);
    NDSStatus (*equals)(NDSContext *ctx);
    NDSStatus (*compare)(NDSContext *ctx);
    NDSStatus (*to_string)(NDSContext *ctx);
    void (*finalize)(NDSContext *ctx, void *extraData);
};

/* Configuration */
typedef NDSStatus (*NDSSetupCallback)(NDSConfigHandle *handle, void *userPointer);

typedef struct NDSConfig NDSConfig;
struct NDSConfig {
    NDSAllocator *allocator;
    NDSSetupCallback setup;
    void *userPointer;
    size_t maxStackDepth; /* 0 = default (16384) */
};

#define NDSMaxRegisteredNameLength 255
#define NDSMaxErrorMessageLength 511

NDSAtom
NDSConfigHandle_registerAtom(NDSConfigHandle *h, const char *name);

NDSChunkID
NDSConfigHandle_registerChunkID(NDSConfigHandle *h, const char *name);

NDSTypeID
NDSConfigHandle_registerType(NDSConfigHandle *h, const NDSTypeDescriptor *descriptor);

/* Host function calling */
typedef NDSStatus (*NDSHostFunctionCallback)(NDSContext *ctx, int argCount);

NDSStatus
NDSConfigHandle_registerFunction(NDSConfigHandle *h, const char *name,
                                 NDSHostFunctionCallback callback, int paramCount);

/* Host constant registration */
NDSStatus
NDSConfigHandle_registerConstantNumber(NDSConfigHandle *h, const char *name, double value);

NDSStatus
NDSConfigHandle_registerConstantString(NDSConfigHandle *h, const char *name, const char *s, size_t len);

NDSStatus
NDSConfigHandle_registerConstantBoolean(NDSConfigHandle *h, const char *name, bool value);

/* Host commands */
typedef enum {
    NDSPatternStep_Word,
    NDSPatternStep_Expression,
    NDSPatternStep_Variable,
    NDSPatternStep_Reference,  /* in/out: load variable's value before call, store after */
    NDSPatternStep_End
} NDSPatternStepKind;

typedef struct {
    NDSPatternStepKind kind;
    const char *word;
} NDSPatternStep;

NDSStatus
NDSConfigHandle_registerCommand(NDSConfigHandle *h, const NDSPatternStep *pattern, NDSHostFunctionCallback callback);

/* Errors */
void
NDSContext_setError(NDSContext *ctx, const char *message);

void NDS_PRINTF(2, 3) NDSContext_setErrorF(NDSContext *ctx, const char *fmt, ...);

/* User data */
void
NDSContext_setUserPointer(NDSContext *self, void *pointer);

void *
NDSContext_getUserPointer(const NDSContext *self);

/* Context lifecycle */
NDSContext *
NDSContext_new(const NDSConfig *config);

void
NDSContext_free(NDSContext *self);

/* Context memory allocation (uses the context's allocator) */
void *
NDSContext_allocateMemory(NDSContext *self, size_t size);

void
NDSContext_freeMemory(NDSContext *self, void *pointer);

/* Garbage collection */
void
NDSContext_collectGarbage(NDSContext *self);

/* Slot API — indices */
#define NDSContext_VarSlot(n) (-(int)(n) - 1)

NDSStatus
NDSContext_ensureSlots(NDSContext *self, int count);

int
NDSContext_getSlotCount(const NDSContext *self);

double
NDSContext_getSlotNumber(const NDSContext *self, int slot);

bool
NDSContext_getSlotBoolean(const NDSContext *self, int slot);

const char *
NDSContext_getSlotString(const NDSContext *self, int slot);

size_t
NDSContext_getSlotStringLength(const NDSContext *self, int slot);

void *
NDSContext_getSlotPointer(const NDSContext *self, int slot);

int64_t
NDSContext_getSlotInteger(const NDSContext *self, int slot);

NDSTypeID
NDSContext_getSlotType(const NDSContext *self, int slot);

const char *
NDSContext_getTypeName(const NDSContext *self, NDSTypeID typeID);

bool
NDSContext_isSlotNothing(const NDSContext *self, int slot);

bool
NDSContext_isSlotNumber(const NDSContext *self, int slot);

bool
NDSContext_isSlotBoolean(const NDSContext *self, int slot);

bool
NDSContext_isSlotString(const NDSContext *self, int slot);

bool
NDSContext_isSlotList(const NDSContext *self, int slot);

bool
NDSContext_isSlotMap(const NDSContext *self, int slot);

bool
NDSContext_isSlotError(const NDSContext *self, int slot);

bool
NDSContext_isSlotFunction(const NDSContext *self, int slot);

NDSStatus
NDSContext_slotToString(NDSContext *self, int srcSlot, int destSlot);

void
NDSContext_setSlotNumber(NDSContext *self, int slot, double n);

void
NDSContext_setSlotBoolean(NDSContext *self, int slot, bool b);

NDSStatus
NDSContext_setSlotString(NDSContext *self, int slot, const char *s, size_t len);

void
NDSContext_setSlotNothing(NDSContext *self, int slot);

void
NDSContext_setSlotPointer(NDSContext *self, int slot, void *ptr);

NDSStatus
NDSContext_setSlotError(NDSContext *self, int slot, const char *message);

void
NDSContext_copySlot(NDSContext *self, int srcSlot, int destSlot);

void
NDSContext_setSlotTypedNumber(NDSContext *self, int slot, NDSTypeID typeID, double value);

void
NDSContext_setSlotTypedInteger(NDSContext *self, int slot, NDSTypeID typeID, int64_t value);

void
NDSContext_setSlotTypedPointer(NDSContext *self, int slot, NDSTypeID typeID, void *ptr);

typedef enum {
    NDSSlotComparison_Incomparable,
    NDSSlotComparison_LessThan,
    NDSSlotComparison_GreaterThan,
    NDSSlotComparison_Equal,
} NDSSlotComparison;

NDSSlotComparison
NDSContext_compareSlots(NDSContext *self, int slotA, int slotB);

bool
NDSContext_slotsEqual(NDSContext *self, int slotA, int slotB);

void *
NDSContext_setSlotNewObject(NDSContext *self, int slot, NDSTypeID typeID);

void *
NDSContext_getSlotObjectData(const NDSContext *self, int slot);

void
NDSContext_getObjectProperty(NDSContext *self, int objSlot, int propIndex, int destSlot);

void
NDSContext_setObjectProperty(NDSContext *self, int objSlot, int propIndex, int srcSlot);

NDSStatus
NDSContext_setSlotNewList(NDSContext *self, int slot);

size_t
NDSContext_getSlotListCount(const NDSContext *self, int listSlot);

void
NDSContext_getSlotListElement(NDSContext *self, int listSlot, size_t index, int destSlot);

void
NDSContext_setSlotListElement(NDSContext *self, int listSlot, size_t index, int srcSlot);

NDSStatus
NDSContext_insertSlotListElement(NDSContext *self, int listSlot, size_t index, int srcSlot);

NDSStatus
NDSContext_appendSlotListElement(NDSContext *self, int listSlot, int srcSlot);

void
NDSContext_removeSlotListElement(NDSContext *self, int listSlot, size_t index);

NDSStatus
NDSContext_setSlotNewMap(NDSContext *self, int slot);

size_t
NDSContext_getSlotMapCount(const NDSContext *self, int mapSlot);

bool
NDSContext_getSlotMapValue(NDSContext *self, int mapSlot, int keySlot, int destSlot);

NDSStatus
NDSContext_setSlotMapValue(NDSContext *self, int mapSlot, int keySlot, int valueSlot);

bool
NDSContext_removeSlotMapValue(NDSContext *self, int mapSlot, int keySlot);

bool
NDSContext_getSlotMapContainsKey(NDSContext *self, int mapSlot, int keySlot);

bool
NDSContext_getNextSlotMapEntry(NDSContext *self, int mapSlot, NDSMapIterCookie *cookie, int keySlot, int valueSlot);

/* Allocators */
struct NDSAllocator {
    NDSAllocator *parent;

    void *(*allocate)(NDSAllocator *self, size_t size);
    void *(*reallocate)(NDSAllocator *self, void *pointer, size_t newSize);
    void (*free)(NDSAllocator *self, void *pointer);
    void (*reset)(NDSAllocator *self);
    void (*destroy)(NDSAllocator *self);
};

extern NDSAllocator *const NDSAllocator_SystemAllocator;

NDSAllocator *
NDSPoolAllocator_new(NDSAllocator *parent, size_t slabSize);

NDSAllocator *
NDSLimitAllocator_new(NDSAllocator *parent, size_t maxBytes);

void *
NDSAllocator_allocateZeroed(NDSAllocator *self, size_t size);

static inline void *
NDSAllocator_allocate(NDSAllocator *self, size_t size)
{
    return self->allocate(self, size);
}

static inline void *
NDSAllocator_reallocate(NDSAllocator *self, void *pointer, size_t newSize)
{
    return self->reallocate(self, pointer, newSize);
}

static inline void
NDSAllocator_free(NDSAllocator *self, void *pointer)
{
    self->free(self, pointer);
}

static inline void
NDSAllocator_reset(NDSAllocator *self)
{
    if (self->reset)
        self->reset(self);
}

static inline void
NDSAllocator_destroy(NDSAllocator *self)
{
    if (self && self->destroy)
        self->destroy(self);
}

/* Reader interface */
typedef struct NDSReader NDSReader;
struct NDSReader {
    size_t (*read)(NDSReader *self, void *buf, size_t count);
    void (*destroy)(NDSReader *self);
};

#define NDSReader_EOF ((size_t)-1)
#define NDSReader_Error ((size_t)-2)

NDSReader *
NDSContext_newStringReader(NDSContext *self, const char *data, size_t length);

NDSReader *
NDSContext_newFileReader(NDSContext *self, const char *fileName);

size_t
NDSReader_read(NDSReader *self, void *buffer, size_t count);

void
NDSReader_destroy(NDSReader *self);

/* Imports */
typedef NDSReader *(*NDSScriptLoaderFn)(NDSContext *ctx, const char *name, char *errorMessage, void *userPointer);

void
NDSConfigHandle_enableImport(NDSConfigHandle *h, NDSScriptLoaderFn loader, void *userPointer);

/* Default implementation of reader, reads files given by path. */
NDSReader *
NDSDefaultFileReader(NDSContext *ctx, const char *path, char *errorMessage, void *userPointer);

/* Evaluate a script */
NDSStatus
NDSContext_evaluate(NDSContext *self, const char *source, size_t length, const char *sourceName);

NDSStatus
NDSContext_evaluateReader(NDSContext *self, NDSReader *reader, const char *sourceName);

NDSStatus
NDSContext_evaluateFile(NDSContext *self, const char *fileName);

/* Compile and execute separately. */
NDSScript *
NDSContext_compile(NDSContext *self, const char *source, size_t length, const char *sourceName);

NDSStatus
NDSContext_executeScript(NDSContext *self, const NDSScript *script);

void
NDSScript_free(NDSScript *script);

/* Error reporting */
const char *
NDSContext_getError(NDSContext *self);

bool
NDSContext_getErrorType(NDSContext *self, int destSlot);

bool
NDSContext_getErrorValue(NDSContext *self, int destSlot);

bool
NDSContext_errorAtEOF(NDSContext *self);

/* Globals */
bool
NDSContext_getGlobal(NDSContext *self, const char *name, int destSlot);

NDSStatus
NDSContext_setGlobal(NDSContext *self, const char *name, int srcSlot);

/* Function calls */
NDSStatus
NDSContext_callFunction(NDSContext *self, const char *name, int argCount);

NDSStatus
NDSContext_callSlot(NDSContext *self, int funcSlot, int argCount);

#endif
