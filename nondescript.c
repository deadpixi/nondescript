#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <float.h>
#include <inttypes.h>
#include <locale.h>
#include <math.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#include "nondescript.h"

#define UNUSED(x) ((void)(x))

#define INITIAL_ATOM_CAPACITY 32
#define INITIAL_STACK_CAPACITY 256
#define INITIAL_CONSTANT_CAPACITY 64
#define INITIAL_GLOBAL_CAPACITY 16

#define NDS_MAX_GLOBALS   4096
#define NDS_MAX_FUNCTIONS 4096
#define NDS_MAX_ATOMS     65536
#define NDS_MAX_CONSTANTS 65536
#define DEFAULT_MAX_STACK_DEPTH 16384
#define GC_INITIAL_THRESHOLD (1024 * 1024)
#define VM_MAX_INTERPRETER_DEPTH 32
#define TOSTRING_INITIAL_CAPACITY 256
#define NUMERIC_MAX_LEN (3 + DBL_MANT_DIG - DBL_MIN_10_EXP)

#define TokenWord_HostHead (1u << 0)
#define TokenWord_ChunkID (1u << 1)

typedef enum{
    GCColor_White,
    GCColor_Gray,
    GCColor_Black
} GCColor;


typedef struct NDSObject NDSObject;

typedef struct NDSValue NDSValue;
struct NDSValue{
    NDSTypeID type;
    union{
        double number;
        bool boolean;
        NDSObject *object;
        void *opaque;
        int64_t integer;
    } as;
};

typedef struct Instruction Instruction;
typedef struct{
    uint32_t line;
    uint32_t source;
} LineInfo;
typedef struct Function Function;
typedef struct HostCommand HostCommand;
typedef struct CommandTrieNode CommandTrieNode;


/* TODO - NDSObject is too big. Too many fields. We should be able to shrink this down a bit. */
struct NDSObject{
    NDSObject *gcNext;
    NDSObject *grayNext;
    NDSTypeID type; /* TODO - this is duplicative, the type is available in NDSValue */
    GCColor gcColor;
    NDSValue slots[];
};

static inline NDSValue
NDSObject_getProperty(const NDSObject *obj, int index)
{
    return obj->slots[index];
}

static inline void
NDSObject_setProperty(NDSObject *obj, int index, NDSValue value)
{
    obj->slots[index] = value;
}


typedef struct Global Global;
struct Global{
    NDSAtom name;
    NDSValue value;
    bool isConst;
};

typedef struct StringData StringData;
struct StringData{
    size_t length;
    size_t characterCount; /* 0 = not yet computed */
    char *data;
};

typedef struct List List;
struct List{
    NDSValue *items;
    size_t count;
    size_t capacity;
};

#define MAP_EMPTY UINT32_MAX
#define MAP_TOMBSTONE (UINT32_MAX - 1)
#define MAP_INITIAL_BUCKET_COUNT 8
#define MAP_MAX_LOAD_PERCENT 75

typedef struct Map Map;
struct Map{
    NDSValue *keys;
    NDSValue *values;
    uint32_t *buckets;
    size_t count;       /* live entries (excludes deleted) */
    size_t used;        /* next insertion position in dense arrays (>= count) */
    size_t capacity;    /* allocated size of keys/values arrays */
    size_t bucketCount; /* allocated size of buckets array */
};

/* Internal type descriptor — all vtable entries non-NULL after registration */
typedef struct TypeDescriptor TypeDescriptor;
struct TypeDescriptor{
    const char *name;
    NDSAtom nameAtom;
    NDSAtom *propertyAtoms;
    int propertyCount;
    size_t extraDataSize;
    bool isManaged;

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
    void (*trace)(NDSContext *ctx, NDSObject *obj);
    void (*finalize)(NDSContext *ctx, void *extraData);
    size_t (*gc_size)(NDSContext *ctx, NDSObject *obj);

    NDSStatus (*customPropertyGet)(NDSContext *ctx, NDSAtom nameAtom);
};


#define VM_MAX_CALL_DEPTH 256
#define NDS_NO_BLOCK UINT32_MAX

typedef struct VMCallInfo VMCallInfo;
struct VMCallInfo{
    size_t closureBase;      /* for blocks: base of the enclosing function's locals */
    uint32_t block;          /* NDS_NO_BLOCK or funcIdx of attached block */
    size_t callerLocalBase;  /* caller's localBase, passed to blocks via Op_CallGiven */
    size_t handlerIP;        /* function's error handler IP (0 = none) */
    bool handlerActive;      /* true while executing the error handler */
    int handlerErrorSlot;    /* local slot for error variable */
    int maxLocals;           /* total local slots (for handler stack reset) */
};

/* What a beautiful mess this is, good lord. */
#define MAX_IMPORTED_FILES 128
struct NDSContext{
    NDSAllocator *allocator;
    void *userPointer;

    /* Schema — set up during configuration, immutable at runtime */
    TypeDescriptor *types;
    size_t typeCount;
    size_t typeCapacity;

    NDSValue *atoms;
    size_t atomCount;
    size_t atomCapacity;
    uint8_t *wordFlags;
    uint32_t *atomBuckets;
    size_t atomBucketCount;

    NDSValue *constants;
    size_t constantCount;
    size_t constantCapacity;

    Global *globals;
    size_t globalCount;
    size_t globalCapacity;

    Function *functions;
    size_t functionCount;
    size_t functionCapacity;

    HostCommand *commands;
    size_t commandCount;
    size_t commandCapacity;
    CommandTrieNode *commandTrie;

    NDSScriptLoaderFn scriptLoader;
    void *scriptLoaderUserPointer;

    /* Pre-interned constant strings */
    NDSValue stringNothing;
    NDSValue stringTrue;
    NDSValue stringFalse;
    NDSValue stringFunction;
    NDSValue stringError;

    /* VM runtime state */
    NDSValue *stack;
    size_t stackTop;
    size_t stackCapacity;
    size_t maxStackDepth;
    size_t frameBase;
    size_t callDepth;

    /* Garbage collector */
    NDSObject *gcObjects;
    NDSObject *grayList;
    size_t gcBytesAllocated;
    size_t gcNextCollection;

    /* Error state */
    NDSValue currentError;
    char errorMessage[NDSMaxErrorMessageLength + 1];
    char formattedError[NDSMaxErrorMessageLength + 1];
    size_t errorLine;
    uint32_t errorSourceAtom;

    /* Import state */
    size_t importedNames[MAX_IMPORTED_FILES];
    size_t importedNameCount;
    size_t interpreterDepth;

    /* Misc */
    int toStringDepth;
    uint64_t randomState;
    char *decimalPoint; /* C allows for decimal separators to be strings! STRINGS! */
};

struct NDSConfigHandle{
    NDSContext *ctx;
};

struct NDSScript{
    Instruction *code;
    LineInfo *lines;
    size_t codeLength;
    NDSAllocator *allocator;
};

/* Built-in atom indices */
typedef enum{
    NDSBuiltinAtom_Item       = 1,  /* chunk */
    NDSBuiltinAtom_Character,       /* chunk */
    NDSBuiltinAtom_Byte,            /* chunk */
    NDSBuiltinAtom_Key,             /* chunk */
    NDSBuiltinAtom_Value,           /* chunk AND error property */
    NDSBuiltinAtom_Entry,           /* chunk */
    NDSBuiltinAtom_Type,            /* property */
    NDSBuiltinAtom_Length,          /* property */
    NDSBuiltinAtom_Count,           /* property */
    NDSBuiltinAtom_ErrorType,       /* error property */
    NDSBuiltinAtom_Message,         /* error property */
    NDSBuiltinAtom_It,              /* special */
    NDSBuiltinAtom_Size,            /* property (string byte count) */
    NDSBuiltinAtom_Offset,          /* chunk (string/list search) */
    NDSBuiltinAtom_LAST
} NDSBuiltinAtomIndex;

/* Built-in global variable indices */
#define NDSBuiltinGlobal_It 0

static inline TypeDescriptor *
NDSContext_getTypeDesc(NDSContext *self, NDSTypeID type)
{
    assert(type < self->typeCount);
    return &self->types[type];
}

static bool
NDSContext_isManagedType(const NDSContext *self, NDSValue value)
{
    assert(value.type < self->typeCount);
    return self->types[value.type].isManaged;
}

static inline void *
NDSObject_getExtraData(const NDSContext *ctx, NDSObject *obj)
{
    int propertyCount = ctx->types[obj->type].propertyCount;
    return (void *)(obj->slots + propertyCount);
}

/* Internal helpers to extract struct pointers from slots */
static List *
NDSContext_getSlotList(NDSContext *self, int slot)
{
    NDSValue *v = &self->stack[self->frameBase + (size_t)slot];
    return (List *)NDSObject_getExtraData(self, v->as.object);
}

static Map *
NDSContext_getSlotMap(NDSContext *self, int slot)
{
    NDSValue *v = &self->stack[self->frameBase + (size_t)slot];
    return (Map *)NDSObject_getExtraData(self, v->as.object);
}

static StringData *
NDSContext_getSlotStringData(NDSContext *self, int slot)
{
    NDSValue *v = &self->stack[self->frameBase + (size_t)slot];
    return (StringData *)NDSObject_getExtraData(self, v->as.object);
}


static NDSValue *
NDSContext_slotPointer(NDSContext *self, int slot);
static NDSValue
NDSContext_getSlot(const NDSContext *self, int slot);

static bool
Value_equals(NDSContext *ctx, NDSValue a, NDSValue b);
static void
default_trace(NDSContext *ctx, NDSObject *obj);

static uint32_t
Hash_fnv1a(const void *data, size_t length);
static bool
MapEntry_new(NDSContext *ctx, NDSValue key, NDSValue value, NDSValue *dest);
static void
CharClass_init(void);

static inline const char *
NDSContext_atomName(const NDSContext *ctx, NDSAtom atom)
{
    return ((StringData *)NDSObject_getExtraData(ctx, ctx->atoms[atom].as.object))->data;
}

static inline const char *
NDSContext_typeName(const NDSContext *ctx, NDSTypeID type)
{
    return (type < ctx->typeCount && ctx->types[type].name)? ctx->types[type].name : "unknown";
}

static NDSObject *
List_new(NDSContext *ctx, NDSValue *dest);
static bool
List_append(NDSContext *ctx, NDSObject *obj, NDSValue value);

static const NDSValue NDSValue_Nothing = { .type = NDSValueType_Nothing };
static const NDSValue NDSValue_True = { .type = NDSValueType_Boolean, .as.boolean = true };
static const NDSValue NDSValue_False = { .type = NDSValueType_Boolean, .as.boolean = false };

static NDSValue
NDSValue_numberFromDouble(double d)
{
    return (NDSValue){ .type = NDSValueType_Number, .as.number = d };
}

static NDSValue
NDSValue_booleanFromBool(bool b)
{
    return b? NDSValue_True : NDSValue_False;
}

static inline NDSValue
NDSValue_functionFromIndex(size_t idx)
{
    NDSValue v = {0};
    v.type = NDSValueType_Function;
    v.as.integer = (int64_t)idx;
    return v;
}

typedef struct{
    _Alignas(max_align_t) size_t size; /* The one bit of C11 in here... */
} AllocHeader;

static inline AllocHeader *
AllocHeader_fromPointer(void *pointer)
{
    return (AllocHeader *)((char *)pointer - sizeof(AllocHeader));
}

static inline void *
AllocHeader_toPointer(AllocHeader *header)
{
    return (char *)header + sizeof(AllocHeader);
}

static inline size_t
AllocHeader_alignUp(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

void *
NDSAllocator_allocateZeroed(NDSAllocator *self, size_t size)
{
    void *pointer = self->allocate(self, size);
    if (pointer)
        memset(pointer, 0, size);
    return pointer;
}

static bool
Util_safeMulSize(size_t a, size_t b, size_t *result)
{
    if (a > 0 && b > SIZE_MAX / a)
        return false;
    *result = a * b;
    return true;
}

static bool
Util_safeAddSize(size_t a, size_t b, size_t *result)
{
    if (a > SIZE_MAX - b)
        return false;
    *result = a + b;
    return true;
}

static bool
Util_parseDouble(const NDSContext *ctx, const char *src, size_t length, double *out)
{
    const char *dp = ctx->decimalPoint;
    size_t dpLen = strlen(dp);
    char buf[NUMERIC_MAX_LEN + 1] = {0};
    size_t count = 0;

    for (size_t i = 0; i < length; i++){
        if (src[i] == '.'){
            if (count + dpLen >= NUMERIC_MAX_LEN)
                return false;
            memcpy(buf + count, dp, dpLen);
            count += dpLen;
        } else{
            if (count + 1 >= NUMERIC_MAX_LEN)
                return false;
            buf[count++] = src[i];
        }
    }
    buf[count] = '\0';

    if (count == 0){
        *out = 0.0;
        return true;
    }

    char *endptr = NULL;
    errno = 0;
    double d = strtod(buf, &endptr);
    if (endptr == buf || *endptr != '\0' || errno == ERANGE || !isfinite(d))
        return false;
    *out = d;
    return true;
}

/* Portable memmem: find needle in haystack using explicit lengths. 
 * It's insane to me that in this, the Year of Our Lord 2026, memmem isn't
 * standard C.
 */
static const char *
Util_memmem(const char *haystack, size_t haystackLen, const char *needle, size_t needleLen)
{
    if (needleLen == 0)
        return haystack;
    if (needleLen > haystackLen)
        return NULL;
    size_t limit = haystackLen - needleLen;
    for (size_t i = 0; i <= limit; i++){
        if (haystack[i] == needle[0] && memcmp(haystack + i, needle, needleLen) == 0)
            return haystack + i;
    }
    return NULL;
}

/* Double the capacity of a dynamically-sized array. */
static bool
Util_growArray(NDSAllocator *alloc, void **ptr, size_t *capacity, size_t elemSize)
{
    size_t newCap;
    if (!Util_safeMulSize(*capacity, 2, &newCap))
        return false;
    size_t allocSize;
    if (!Util_safeMulSize(newCap, elemSize, &allocSize))
        return false;
    void *newPtr = NDSAllocator_reallocate(alloc, *ptr, allocSize);
    if (!newPtr)
        return false;
    *ptr = newPtr;
    *capacity = newCap;
    return true;
}

static void *
NDSSystemAllocator_allocate(NDSAllocator *self, size_t size)
{
    UNUSED(self);

    AllocHeader *header = malloc(sizeof(AllocHeader) + size);
    if (!header)
        return NULL;
    header->size = size;
    return AllocHeader_toPointer(header);
}

static void *
NDSSystemAllocator_reallocate(NDSAllocator *self, void *pointer, size_t newSize)
{
    UNUSED(self);

    if (!pointer)
        return NDSSystemAllocator_allocate(self, newSize);

    AllocHeader *oldHeader = AllocHeader_fromPointer(pointer);
    AllocHeader *newHeader = realloc(oldHeader, sizeof(AllocHeader) + newSize);
    if (!newHeader)
        return NULL;

    newHeader->size = newSize;
    return AllocHeader_toPointer(newHeader);
}

static void
NDSSystemAllocator_free(NDSAllocator *self, void *pointer)
{
    UNUSED(self);
    if (!pointer)
        return;
    free(AllocHeader_fromPointer(pointer));
}

static NDSAllocator systemAllocator ={
    .allocate = NDSSystemAllocator_allocate,
    .reallocate = NDSSystemAllocator_reallocate,
    .free = NDSSystemAllocator_free,
    .reset = NULL,
    .destroy = NULL,
    .parent = NULL,
};

NDSAllocator *const NDSAllocator_SystemAllocator = &systemAllocator;

/* Slab allocator for memory pooling */
typedef struct Slab Slab;
struct Slab{
    struct Slab *next;
    size_t capacity;
    size_t used;
    char data[];
};

typedef struct NDSPoolAllocator NDSPoolAllocator;
struct NDSPoolAllocator{
    NDSAllocator base;
    size_t slabSize;
    Slab *current;
    Slab *slabs;
};

static Slab *
NDSPoolAllocator_newSlab(NDSPoolAllocator *self, size_t minSize)
{
    size_t capacity = self->slabSize;
    if (capacity < minSize)
        capacity = minSize;

    Slab *slab = NDSAllocator_allocate(self->base.parent, sizeof(Slab) + capacity);
    if (!slab)
        return NULL;

    slab->capacity = capacity;
    slab->used = 0;
    slab->next = self->slabs;
    self->slabs = slab;

    return slab;
}

static void *
NDSPoolAllocator_allocate(NDSAllocator *self, size_t size)
{
    NDSPoolAllocator *pool = (NDSPoolAllocator *)self;
    size_t alignedSize = AllocHeader_alignUp(sizeof(AllocHeader) + size, sizeof(AllocHeader));

    if (pool->current){
        size_t remaining = pool->current->capacity - pool->current->used;
        if (alignedSize <= remaining){
            AllocHeader *header = (AllocHeader *)(pool->current->data + pool->current->used);
            header->size = size;
            pool->current->used += alignedSize;
            return AllocHeader_toPointer(header);
        }
    }

    Slab *slab = NDSPoolAllocator_newSlab(pool, alignedSize);
    if (!slab)
        return NULL;

    pool->current = slab;
    slab->used = alignedSize;

    AllocHeader *header = (AllocHeader *)slab->data;
    header->size = size;
    return AllocHeader_toPointer(header);
}

static void *
NDSPoolAllocator_reallocate(NDSAllocator *self, void *pointer, size_t newSize)
{
    if (!pointer)
        return NDSPoolAllocator_allocate(self, newSize);

    AllocHeader *oldHeader = AllocHeader_fromPointer(pointer);
    size_t oldSize = oldHeader->size;

    void *newPointer = NDSPoolAllocator_allocate(self, newSize);
    if (!newPointer)
        return NULL;
    memcpy(newPointer, pointer, oldSize < newSize? oldSize : newSize);
    return newPointer;
}

static void
NDSPoolAllocator_free(NDSAllocator *self, void *pointer)
{
    UNUSED(self);
    UNUSED(pointer);
}

static void
NDSPoolAllocator_reset(NDSAllocator *self)
{
    NDSPoolAllocator *pool = (NDSPoolAllocator *)self;
    Slab *slab = pool->slabs;
    while (slab){
        Slab *next = slab->next;
        NDSAllocator_free(pool->base.parent, slab);
        slab = next;
    }
    pool->slabs = NULL;
    pool->current = NULL;
}

static void
NDSPoolAllocator_destroy(NDSAllocator *self)
{
    NDSPoolAllocator_reset(self);
    NDSPoolAllocator *pool = (NDSPoolAllocator *)self;
    NDSAllocator_free(pool->base.parent, pool);
}

NDSAllocator *
NDSPoolAllocator_new(NDSAllocator *parent, size_t slabSize)
{
    NDSPoolAllocator *pool = NDSAllocator_allocateZeroed(parent, sizeof(NDSPoolAllocator));
    if (!pool)
        return NULL;

    pool->base.allocate = NDSPoolAllocator_allocate;
    pool->base.reallocate = NDSPoolAllocator_reallocate;
    pool->base.free = NDSPoolAllocator_free;
    pool->base.reset = NDSPoolAllocator_reset;
    pool->base.destroy = NDSPoolAllocator_destroy;
    pool->base.parent = parent;
    pool->slabSize = slabSize;

    return &pool->base;
}

typedef struct NDSLimitAllocator NDSLimitAllocator;
struct NDSLimitAllocator{
    NDSAllocator base;
    size_t maxBytes;
    size_t usedBytes;
};

static void *
NDSLimitAllocator_allocate(NDSAllocator *self, size_t size)
{
    NDSLimitAllocator *limit = (NDSLimitAllocator *)self;
    size_t total = sizeof(AllocHeader) + size;
    if (total < size)
        return NULL;
    if (limit->usedBytes + total > limit->maxBytes)
        return NULL;

    AllocHeader *header = NDSAllocator_allocate(limit->base.parent, total);
    if (!header)
        return NULL;

    header->size = size;
    limit->usedBytes += total;
    return (char *)header + sizeof(AllocHeader);
}

static void *
NDSLimitAllocator_reallocate(NDSAllocator *self, void *pointer, size_t newSize)
{
    NDSLimitAllocator *limit = (NDSLimitAllocator *)self;
    if (!pointer)
        return NDSLimitAllocator_allocate(self, newSize);

    AllocHeader *oldHeader = (AllocHeader *)((char *)pointer - sizeof(AllocHeader));
    size_t oldTotal = sizeof(AllocHeader) + oldHeader->size;
    size_t newTotal = sizeof(AllocHeader) + newSize;
    if (newTotal < newSize)
        return NULL;

    if (limit->usedBytes - oldTotal + newTotal > limit->maxBytes)
        return NULL;

    AllocHeader *newHeader = NDSAllocator_reallocate(limit->base.parent, oldHeader, newTotal);
    if (!newHeader)
        return NULL;

    limit->usedBytes = limit->usedBytes - oldTotal + newTotal;
    newHeader->size = newSize;
    return (char *)newHeader + sizeof(AllocHeader);
}

static void
NDSLimitAllocator_free(NDSAllocator *self, void *pointer)
{
    if (!pointer)
        return;

    NDSLimitAllocator *limit = (NDSLimitAllocator *)self;
    AllocHeader *header = (AllocHeader *)((char *)pointer - sizeof(AllocHeader));
    size_t total = sizeof(AllocHeader) + header->size;
    limit->usedBytes -= total;
    NDSAllocator_free(limit->base.parent, header);
}

static void
NDSLimitAllocator_destroy(NDSAllocator *self)
{
    NDSLimitAllocator *limit = (NDSLimitAllocator *)self;
    NDSAllocator_free(limit->base.parent, limit);
}

NDSAllocator *
NDSLimitAllocator_new(NDSAllocator *parent, size_t maxBytes)
{
    NDSLimitAllocator *limit = NDSAllocator_allocateZeroed(parent, sizeof(NDSLimitAllocator));
    if (!limit)
        return NULL;

    limit->base.allocate = NDSLimitAllocator_allocate;
    limit->base.reallocate = NDSLimitAllocator_reallocate;
    limit->base.free = NDSLimitAllocator_free;
    limit->base.reset = NULL;
    limit->base.destroy = NDSLimitAllocator_destroy;
    limit->base.parent = parent;
    limit->maxBytes = maxBytes;

    return &limit->base;
}

/* GC */

static void
NDSContext_gcSweep(NDSContext *self);

void
NDSContext_collectGarbage(NDSContext *self);

/* Allocate a garbage collected value. Note that dest must be a GC root,
 * or traceable from a GC root, or Bad Things will happen.
 */
static NDSObject *
NDSContext_gcAllocate(NDSContext *self, size_t size, NDSTypeID typeID, NDSValue *dest)
{
    if (self->gcBytesAllocated >= self->gcNextCollection)
        NDSContext_collectGarbage(self);

    /* Try to allocate the object. If allocation fails, try once more
     * after running a collection cycle, vaguely Lua-ish
     */
    NDSObject *obj = NDSAllocator_allocateZeroed(self->allocator, size);
    if (!obj){
        NDSContext_collectGarbage(self);
        obj = NDSAllocator_allocateZeroed(self->allocator, size);
        if (!obj){
            *dest = NDSValue_Nothing;
            return NULL;
        }
    }

    obj->gcNext = self->gcObjects;
    obj->type = typeID;
    obj->gcColor = GCColor_White;
    self->gcObjects = obj;
    self->gcBytesAllocated += size;

    dest->type = typeID;
    dest->as.object = obj;
    return obj;
}

static NDSObject *
NDSContext_newObject(NDSContext *ctx, NDSTypeID type, NDSValue *dest)
{
    TypeDescriptor *td = &ctx->types[type];
    size_t size = sizeof(NDSObject) + (size_t)td->propertyCount * sizeof(NDSValue) + td->extraDataSize;
    return NDSContext_gcAllocate(ctx, size, type, dest);
}

static void
NDSContext_gcPushGray(NDSContext *self, NDSObject *obj)
{
    if (!obj || obj->gcColor != GCColor_White)
        return;
    obj->gcColor = GCColor_Gray;
    obj->grayNext = self->grayList;
    self->grayList = obj;
}

static void
NDSContext_gcMark(NDSContext *self, NDSValue value)
{
    if (NDSContext_isManagedType(self, value) && value.as.object)
        NDSContext_gcPushGray(self, value.as.object);
}

static void
NDSContext_gcMarkRoots(NDSContext *self)
{
    for (size_t i = 0; i < self->stackTop; i++)
        NDSContext_gcMark(self, self->stack[i]);

    for (size_t i = 0; i < self->constantCount; i++)
        NDSContext_gcMark(self, self->constants[i]);

    for (size_t i = 0; i < self->globalCount; i++)
        NDSContext_gcMark(self, self->globals[i].value);

    for (size_t i = 0; i < self->atomCount; i++)
        NDSContext_gcMark(self, self->atoms[i]);

    NDSContext_gcMark(self, self->currentError);
    NDSContext_gcMark(self, self->stringNothing);
    NDSContext_gcMark(self, self->stringTrue);
    NDSContext_gcMark(self, self->stringFalse);
    NDSContext_gcMark(self, self->stringFunction);
    NDSContext_gcMark(self, self->stringError);
}

static void
NDSContext_gcProcessGrays(NDSContext *self)
{
    while (self->grayList){
        NDSObject *obj = self->grayList;
        self->grayList = obj->grayNext;
        obj->grayNext = NULL;
        obj->gcColor = GCColor_Black;

        TypeDescriptor *td = NDSContext_getTypeDesc(self, obj->type);
        td->trace(self, obj);
    }
}

static void
NDSContext_gcSweep(NDSContext *self)
{
    NDSObject **link = &self->gcObjects;
    size_t freedBytes = 0;

    while (*link){
        NDSObject *obj = *link;
        if (obj->gcColor == GCColor_White){
            *link = obj->gcNext;

            TypeDescriptor *td = NDSContext_getTypeDesc(self, obj->type);
            freedBytes += td->gc_size(self, obj);
            td->finalize(self, NDSObject_getExtraData(self, obj));
            NDSAllocator_free(self->allocator, obj);
        } else{
            obj->gcColor = GCColor_White;
            link = &obj->gcNext;
        }
    }

    if (self->gcBytesAllocated > freedBytes)
        self->gcBytesAllocated -= freedBytes;
    else
        self->gcBytesAllocated = 0;

    self->gcNextCollection = self->gcBytesAllocated * 2;
    if (self->gcNextCollection < GC_INITIAL_THRESHOLD)
        self->gcNextCollection = GC_INITIAL_THRESHOLD;
}

void
NDSContext_collectGarbage(NDSContext *self)
{
    NDSContext_gcMarkRoots(self);
    NDSContext_gcProcessGrays(self);
    NDSContext_gcSweep(self);
}

static size_t
String_countChars(StringData *sd)
{
    if (sd->characterCount != 0)
        return sd->characterCount;
    if (sd->length == 0)
        return 0;

    mbstate_t ps = {0};
    size_t count = 0;
    size_t i = 0;
    while (i < sd->length){
        size_t n = mbrlen(sd->data + i, sd->length - i, &ps);
        if (n == (size_t)-1){
            i++;
            count++;
            memset(&ps, 0, sizeof(ps));
        } else if (n == (size_t)-2){
            break;
        } else{
            i += n;
            count++;
        }
    }
    sd->characterCount = count;
    return count;
}

static size_t
String_charOffset(const StringData *sd, size_t charIndex)
{
    mbstate_t ps = {0};
    size_t pos = 0;
    size_t ci = 0;
    while (ci < charIndex && pos < sd->length){
        size_t n = mbrlen(sd->data + pos, sd->length - pos, &ps);
        if (n == (size_t)-1){
            pos++;
            memset(&ps, 0, sizeof(ps));
        } else if (n == (size_t)-2)
            break;
        else
            pos += n;
        ci++;
    }
    return pos;
}

static size_t
String_charLength(const char *data, size_t remaining)
{
    mbstate_t ps;
    memset(&ps, 0, sizeof(ps));
    size_t n = mbrlen(data, remaining, &ps);
    if (n == (size_t)-1 || n == (size_t)-2)
        return 1;
    return n;
}

static bool
NDSContext_newString(NDSContext *self, const char *bytes, size_t length, NDSValue *dest)
{
    /* Direct gcAllocate — strings are created before type registration. */
    NDSObject *obj = NDSContext_gcAllocate(self, sizeof(NDSObject) + sizeof(StringData), NDSValueType_String, dest);
    if (!obj)
        return false;

    char *buf = NDSAllocator_allocate(self->allocator, length + 1);
    if (!buf){
        *dest = NDSValue_Nothing;
        return false;
    }

    memcpy(buf, bytes, length);
    buf[length] = '\0';

    StringData *sd = (StringData *)NDSObject_getExtraData(self, obj);
    sd->length = length;
    sd->data = buf;

    self->gcBytesAllocated += length + 1;
    return true;
}

/* Atoms/interned strings */

#define ATOM_EMPTY UINT32_MAX
#define ATOM_INITIAL_BUCKETS 64
#define ATOM_MAX_LOAD_PERCENT 75

/* TODO - it's stupid to have a Map and not use it here */
static void
Atom_rehash(NDSContext *self)
{
    uint32_t mask = (uint32_t)(self->atomBucketCount - 1);
    for (size_t i = 0; i < self->atomBucketCount; i++)
        self->atomBuckets[i] = ATOM_EMPTY;
    for (size_t i = 1; i < self->atomCount; i++){
        NDSValue atomVal = self->atoms[i];
        if (atomVal.type != NDSValueType_String || !atomVal.as.object)
            continue;
        StringData *sd = (StringData *)NDSObject_getExtraData(self, atomVal.as.object);
        uint32_t h = Hash_fnv1a(sd->data, sd->length) & mask;
        while (self->atomBuckets[h] != ATOM_EMPTY)
            h = (h + 1) & mask;
        self->atomBuckets[h] = (uint32_t)i;
    }
}

static bool
Atom_growBuckets(NDSContext *self)
{
    size_t newCount;
    if (self->atomBucketCount < ATOM_INITIAL_BUCKETS)
        newCount = ATOM_INITIAL_BUCKETS;
    else if (!Util_safeMulSize(self->atomBucketCount, 2, &newCount))
        return false;
    size_t allocSize;
    if (!Util_safeMulSize(newCount, sizeof(uint32_t), &allocSize))
        return false;
    uint32_t *newBuckets = NDSAllocator_allocate(self->allocator, allocSize);
    if (!newBuckets)
        return false;
    NDSAllocator_free(self->allocator, self->atomBuckets);
    self->atomBuckets = newBuckets;
    self->atomBucketCount = newCount;
    Atom_rehash(self);
    return true;
}

static uint32_t
Atom_probe(const NDSContext *self, const char *lower, size_t len, uint32_t hash)
{
    if (!self->atomBuckets)
        return ATOM_EMPTY;
    uint32_t mask = (uint32_t)(self->atomBucketCount - 1);
    uint32_t idx = hash & mask;
    for (;;){
        uint32_t entry = self->atomBuckets[idx];
        if (entry == ATOM_EMPTY)
            return ATOM_EMPTY;
        NDSValue atomVal = self->atoms[entry];
        if (atomVal.type == NDSValueType_String && atomVal.as.object){
            StringData *sd = (StringData *)NDSObject_getExtraData(self, atomVal.as.object);
            if (sd->length == len && memcmp(sd->data, lower, len) == 0)
                return entry;
        }
        idx = (idx + 1) & mask;
    }
}

static size_t
Util_lowercaseName(const char *name, size_t len, char out[NDSMaxRegisteredNameLength + 1])
{
    if (len > NDSMaxRegisteredNameLength)
        len = NDSMaxRegisteredNameLength;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)tolower((unsigned char)name[i]);
    out[len] = '\0';
    return len;
}

static NDSValue *
Atoms_reserveSlot(NDSContext *self)
{
    if (self->atomCount >= NDS_MAX_ATOMS)
        return NULL;
    if (self->atomCount >= self->atomCapacity){
        size_t oldCap = self->atomCapacity;
        size_t flagCap = oldCap;
        /* sizeof(uint8_t) because who knows maybe one day we'll run on PDP-10... */
        if (!Util_growArray(self->allocator, (void **)&self->wordFlags, &flagCap, sizeof(uint8_t)))
            return NULL;
        memset(self->wordFlags + oldCap, 0, (flagCap - oldCap) * sizeof(uint8_t));
        if (!Util_growArray(self->allocator, (void **)&self->atoms, &self->atomCapacity, sizeof(NDSValue)))
            return NULL;
    }
    NDSValue *slot = &self->atoms[self->atomCount];
    *slot = NDSValue_Nothing;
    self->atomCount++;
    return slot;
}

static NDSAtom
NDSContext_internAtom(NDSContext *self, const char *name)
{
    size_t len = strlen(name);

    char lower[NDSMaxRegisteredNameLength + 1];
    len = Util_lowercaseName(name, len, lower);

    uint32_t hash = Hash_fnv1a(lower, len);

    /* Probe for existing atom */
    uint32_t existing = Atom_probe(self, lower, len, hash);
    if (existing != ATOM_EMPTY)
        return (NDSAtom)existing;

    NDSValue *slot = Atoms_reserveSlot(self);
    if (!slot)
        return NDSAtom_Invalid;
    size_t index = self->atomCount - 1;

    if (!NDSContext_newString(self, lower, len, slot)){
        self->atomCount--;
        return NDSAtom_Invalid;
    }

    /* Grow hash table if needed, then insert */
    if (!self->atomBuckets ||
        self->atomCount * 100 > self->atomBucketCount * ATOM_MAX_LOAD_PERCENT){
        if (!Atom_growBuckets(self))
            return NDSAtom_Invalid;
    } else{
        uint32_t mask = (uint32_t)(self->atomBucketCount - 1);
        uint32_t idx = hash & mask;
        while (self->atomBuckets[idx] != ATOM_EMPTY)
            idx = (idx + 1) & mask;
        self->atomBuckets[idx] = (uint32_t)index;
    }

    return (NDSAtom)index;
}

static NDSAtom
NDSContext_findAtom(const NDSContext *self, const char *name, size_t len)
{
    char lower[NDSMaxRegisteredNameLength + 1] = {0};
    len = Util_lowercaseName(name, len, lower);

    uint32_t result = Atom_probe(self, lower, len, Hash_fnv1a(lower, len));
    return result == ATOM_EMPTY? NDSAtom_Invalid : (NDSAtom)result;
}

static NDSValue *
Constants_reserveSlot(NDSContext *self)
{
    if (self->constantCount >= NDS_MAX_CONSTANTS)
        return NULL;
    if (self->constantCount >= self->constantCapacity){
        if (!Util_growArray(self->allocator, (void **)&self->constants, &self->constantCapacity, sizeof(NDSValue)))
            return NULL;
    }
    NDSValue *slot = &self->constants[self->constantCount];
    *slot = NDSValue_Nothing;
    self->constantCount++;
    return slot;
}

static size_t
NDSContext_addStringConstant(NDSContext *self, const char *bytes, size_t length)
{
    for (size_t i = 0; i < self->constantCount; i++){
        NDSValue c = self->constants[i];
        if (c.type != NDSValueType_String || !c.as.object)
            continue;
        StringData *sd = (StringData *)NDSObject_getExtraData(self, c.as.object);
        if (sd->length == length && memcmp(sd->data, bytes, length) == 0)
            return i;
    }
    NDSValue *slot = Constants_reserveSlot(self);
    if (!slot)
        return (size_t)-1;
    if (!NDSContext_newString(self, bytes, length, slot)){
        self->constantCount--;
        return (size_t)-1;
    }
    return self->constantCount - 1;
}

static size_t
NDSContext_addConstant(NDSContext *self, NDSValue value)
{
    for (size_t i = 0; i < self->constantCount; i++){
        NDSValue c = self->constants[i];
        if (c.type != value.type)
            continue;
        switch (value.type){
        case NDSValueType_String:{
            StringData *a = (StringData *)NDSObject_getExtraData(self, c.as.object);
            StringData *b = (StringData *)NDSObject_getExtraData(self, value.as.object);
            if (a->length == b->length && memcmp(a->data, b->data, a->length) == 0)
                return i;
            break;
        }
        case NDSValueType_Number:
            if (memcmp(&c.as.number, &value.as.number, sizeof(double)) == 0)
                return i;
            break;
        case NDSValueType_Function:
            if (c.as.integer == value.as.integer)
                return i;
            break;
        case NDSValueType_Boolean:
            if (c.as.boolean == value.as.boolean)
                return i;
            break;
        case NDSValueType_Nothing:
            return i;
        default:
            break;
        }
    }

    if (self->constantCount >= self->constantCapacity){
        if (!Util_growArray(self->allocator, (void **)&self->constants, &self->constantCapacity, sizeof(NDSValue)))
            return (size_t)-1;
    }

    size_t index = self->constantCount++;
    self->constants[index] = value;
    return index;
}

static NDSValue
NDSContext_getConstant(const NDSContext *self, size_t index)
{
    if (index >= self->constantCount)
        return NDSValue_Nothing;
    return self->constants[index];
}

static size_t
Globals_find(const NDSContext *ctx, NDSAtom name)
{
    for (size_t i = 0; i < ctx->globalCount; i++)
        if (ctx->globals[i].name == name)
            return i;
    return (size_t)-1;
}

static size_t
Globals_ensure(NDSContext *ctx, NDSAtom name)
{
    size_t idx = Globals_find(ctx, name);
    if (idx != (size_t)-1)
        return idx;

    if (ctx->globalCount >= NDS_MAX_GLOBALS)
        return (size_t)-1;
    if (ctx->globalCount >= ctx->globalCapacity){
        size_t oldCap = ctx->globalCapacity;
        if (!Util_growArray(ctx->allocator, (void **)&ctx->globals, &ctx->globalCapacity, sizeof(Global)))
            return (size_t)-1;
        for (size_t i = oldCap; i < ctx->globalCapacity; i++){
            ctx->globals[i].name = NDSAtom_Invalid;
            ctx->globals[i].value = NDSValue_Nothing;
            ctx->globals[i].isConst = false;
        }
    }

    idx = ctx->globalCount++;
    ctx->globals[idx].name = name;
    ctx->globals[idx].value = NDSValue_Nothing;
    ctx->globals[idx].isConst = false;
    return idx;
}

static size_t
Globals_ensureConst(NDSContext *ctx, NDSAtom name)
{
    size_t idx = Globals_ensure(ctx, name);
    if (idx != (size_t)-1)
        ctx->globals[idx].isConst = true;
    return idx;
}


/* Potpourri */

/*
 * Resolve a 1-based index (negative = from end) to a 0-based C index.
 * Returns true on success, false if out of bounds.
 */
static bool
Util_resolveIndex(double rawIndex, size_t count, size_t *outIndex)
{
    if (rawIndex == 0.0 || count == 0)
        return false;
    long long idx = (long long)rawIndex;
    if (idx > 0){
        if ((size_t)idx > count)
            return false;
        *outIndex = (size_t)(idx - 1);
        return true;
    }
    /* Negative: -1 = last */
    long long resolved = (long long)count + idx;
    if (resolved < 0)
        return false;
    *outIndex = (size_t)resolved;
    return true;
}

static uint32_t
Hash_fnv1a(const void *data, size_t length)
{
    const unsigned char *bytes = data;
    uint32_t hash = 2166136261u; /* FNV-1a 32-bit offset basis */
    for (size_t i = 0; i < length; i++){
        hash ^= bytes[i];
        hash *= 16777619u; /* FNV-1a 32-bit prime */
    }
    return hash;
}

static uint32_t
Hash_value(const NDSContext *ctx, NDSValue v)
{
    switch (v.type){
    case NDSValueType_Boolean:
        return v.as.boolean? 1u : 0u;
    case NDSValueType_Number:{
        double d = v.as.number;
        if (d != d)
            return 0;
        if (d == 0.0)
            d = 0.0; /* in case of negative zero */
        return Hash_fnv1a(&d, sizeof(d));
    }
    case NDSValueType_String:
        if (v.as.object){
            StringData *s = (StringData *)NDSObject_getExtraData(ctx, v.as.object);
            return Hash_fnv1a(s->data, s->length);
        }
        return 0;
    default:
        return 0;
    }
}

static bool
Map_isValidKey(NDSValue key)
{
    switch (key.type){
    case NDSValueType_Boolean:
    case NDSValueType_String:
        return true;
    case NDSValueType_Number:
        return key.as.number == key.as.number; /* reject NaN */
    default:
        return false; /* lists, maps, records, user types */
    }
}

static bool
StringBuf_grow(NDSAllocator *alloc, char **buf, size_t *cap, size_t needed)
{
    size_t newCap = *cap;
    while (newCap < needed){
        if (newCap > SIZE_MAX / 2)
            return false;
        newCap *= 2;
    }
    char *newBuf = NDSAllocator_reallocate(alloc, *buf, newCap);
    if (!newBuf)
        return false;
    *buf = newBuf;
    *cap = newCap;
    return true;
}

#define TOSTRING_MAX_DEPTH 16

/* Nothing */

static NDSStatus
Nothing_equals(NDSContext *ctx)
{
    NDSContext_setSlotBoolean(ctx, 0, true);
    return NDSStatus_OK;
}

static NDSStatus
Nothing_toString(NDSContext *ctx)
{
    *NDSContext_slotPointer(ctx, 0) = ctx->stringNothing;
    return NDSStatus_OK;
}

static NDSStatus
Boolean_equals(NDSContext *ctx)
{
    bool a = NDSContext_getSlotBoolean(ctx, 0);
    bool b = NDSContext_getSlotBoolean(ctx, 1);
    NDSContext_setSlotBoolean(ctx, 0, a == b);
    return NDSStatus_OK;
}

static NDSStatus
Boolean_toString(NDSContext *ctx)
{
    bool b = NDSContext_getSlotBoolean(ctx, 0);
    *NDSContext_slotPointer(ctx, 0) = b? ctx->stringTrue : ctx->stringFalse;
    return NDSStatus_OK;
}

static NDSStatus
Number_equals(NDSContext *ctx)
{
    double a = NDSContext_getSlotNumber(ctx, 0);
    double b = NDSContext_getSlotNumber(ctx, 1);
    NDSContext_setSlotBoolean(ctx, 0, a == b);
    return NDSStatus_OK;
}

/* FIXME - compare should use an enum */
static NDSStatus
Number_compare(NDSContext *ctx)
{
    double a = NDSContext_getSlotNumber(ctx, 0);
    double b = NDSContext_getSlotNumber(ctx, 1);
    int cmp = 0;
    if (a < b)
        cmp = -1;
    else if (a > b)
        cmp = 1;
    NDSContext_setSlotNumber(ctx, 0, (double)cmp);
    return NDSStatus_OK;
}

static NDSStatus
Number_toString(NDSContext *ctx)
{
    double d = NDSContext_getSlotNumber(ctx, 0);
    char buf[NUMERIC_MAX_LEN + 1] = {0};

    /* We're avoiding locale dependency here. No I don't like it either.
     * We're also being (almost) needlessly pedantic here, as the decimal point
     * is almost certainly a single character and not a string or multibyte.
     */
    int len = snprintf(buf, sizeof(buf), "%.*g", DBL_DIG, d);
    if (len < 0 || (size_t)len >= sizeof(buf)){
        /* ...can't happen */
        NDSContext_setError(ctx, "numeric formatting error");
        return NDSStatus_Error;
    }

    char *sep = strstr(buf, ctx->decimalPoint);
    if (sep && sep != buf){
        size_t sepLen = strlen(ctx->decimalPoint);
        if (sepLen > 1)
            memmove(sep + 1, sep + sepLen, strlen(sep + sepLen) + 1);
        *sep = '.';
        len -= (int)(sepLen - 1);
    }

    NDSContext_setSlotString(ctx, 0, buf, (size_t)len);
    if (NDSContext_isSlotNothing(ctx, 0)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static void
String_finalize(NDSContext *ctx, void *extraData)
{
    NDSAllocator_free(ctx->allocator, ((StringData *)extraData)->data);
}

static NDSStatus
String_equals(NDSContext *ctx)
{
    NDSValue va = NDSContext_getSlot(ctx, 0);
    NDSValue vb = NDSContext_getSlot(ctx, 1);
    bool eq = false;
    if (!va.as.object && !vb.as.object)
        eq = true;
    else if (!va.as.object || !vb.as.object)
        eq = false;
    else if (va.as.object == vb.as.object)
        eq = true;
    else{
        StringData *sa = (StringData *)NDSObject_getExtraData(ctx, va.as.object);
        StringData *sb = (StringData *)NDSObject_getExtraData(ctx, vb.as.object);
        if (sa->length != sb->length)
            eq = false;
        else
            eq = memcmp(sa->data, sb->data, sa->length) == 0;
    }
    NDSContext_setSlotBoolean(ctx, 0, eq);
    return NDSStatus_OK;
}

static NDSStatus
String_compare(NDSContext *ctx)
{
    NDSValue va = NDSContext_getSlot(ctx, 0);
    NDSValue vb = NDSContext_getSlot(ctx, 1);
    int cmp = 0;
    if (va.as.object == vb.as.object)
        cmp = 0;
    else if (!va.as.object)
        cmp = -1;
    else if (!vb.as.object)
        cmp = 1;
    else{
        StringData *sa = (StringData *)NDSObject_getExtraData(ctx, va.as.object);
        StringData *sb = (StringData *)NDSObject_getExtraData(ctx, vb.as.object);
        size_t minLen = sa->length < sb->length? sa->length : sb->length;
        cmp = memcmp(sa->data, sb->data, minLen);
        if (cmp == 0){
            if (sa->length < sb->length)
                cmp = -1;
            else if (sa->length > sb->length)
                cmp = 1;
        } else{
            cmp = cmp < 0? -1 : 1;
        }
    }
    NDSContext_setSlotNumber(ctx, 0, (double)cmp);
    return NDSStatus_OK;
}

static NDSStatus
String_contains(NDSContext *ctx)
{
    NDSValue haystack_val = NDSContext_getSlot(ctx, 0);
    NDSValue needle_val = NDSContext_getSlot(ctx, 1);
    if (needle_val.type != NDSValueType_String || !haystack_val.as.object ||
        !needle_val.as.object){
        NDSContext_setSlotBoolean(ctx, 0, false);
        return NDSStatus_OK;
    }
    StringData *haystack = (StringData *)NDSObject_getExtraData(ctx, haystack_val.as.object);
    StringData *needle = (StringData *)NDSObject_getExtraData(ctx, needle_val.as.object);
    bool found;
    if (needle->length == 0)
        found = true;
    else if (needle->length > haystack->length)
        found = false;
    else
        found = Util_memmem(haystack->data, haystack->length, needle->data, needle->length) != NULL;
    NDSContext_setSlotBoolean(ctx, 0, found);
    return NDSStatus_OK;
}

static NDSStatus
String_toString(NDSContext *ctx)
{
    UNUSED(ctx);
    return NDSStatus_OK;
}

static NDSStatus
String_propertyGet(NDSContext *ctx, NDSAtom nameAtom)
{
    if (nameAtom == NDSBuiltinAtom_Length){
        StringData *s = NDSContext_getSlotStringData(ctx, 0);
        NDSContext_setSlotNumber(ctx, 0, (double)String_countChars(s));
        return NDSStatus_OK;
    }
    if (nameAtom == NDSBuiltinAtom_Size){
        StringData *s = NDSContext_getSlotStringData(ctx, 0);
        NDSContext_setSlotNumber(ctx, 0, (double)s->length);
        return NDSStatus_OK;
    }
    NDSContext_setErrorF(ctx, "string has no property '%s'", NDSContext_atomName(ctx, nameAtom));
    return NDSStatus_Error;
}

/* FIXME - this is equivalent to getRange, we can factor things out here */
static NDSStatus
String_chunkGet(NDSContext *ctx, NDSChunkID chunkID)
{
    StringData *str = NDSContext_getSlotStringData(ctx, 0);

    if (chunkID == NDSBuiltinAtom_Offset){
        if (NDSContext_getSlotType(ctx, 1) != NDSValueType_String){
            NDSContext_setError(ctx, "offset search key must be a string");
            return NDSStatus_Error;
        }
        StringData *needle = NDSContext_getSlotStringData(ctx, 1);
        if (needle->length == 0 || needle->length > str->length){
            NDSContext_setSlotNumber(ctx, 0, 0.0);
            return NDSStatus_OK;
        }
        const char *found = Util_memmem(str->data, str->length, needle->data, needle->length);
        double pos = found? (double)(found - str->data + 1) : 0.0;
        NDSContext_setSlotNumber(ctx, 0, pos);
        return NDSStatus_OK;
    }

    if (NDSContext_getSlotType(ctx, 1) != NDSValueType_Number){
        NDSContext_setError(ctx, "expected a number");
        return NDSStatus_Error;
    }

    double rawIdx = NDSContext_getSlotNumber(ctx, 1);

    if (chunkID == NDSBuiltinAtom_Character){
        size_t charCount = String_countChars(str);
        size_t idx = 0;
        if (!Util_resolveIndex(rawIdx, charCount, &idx)){
            NDSContext_setError(ctx, "index out of range");
            return NDSStatus_Error;
        }

        if (charCount == str->length){
            NDSContext_setSlotString(ctx, 0, str->data + idx, 1);
            return NDSStatus_OK;
        }

        size_t pos = String_charOffset(str, idx);
        size_t charLen = String_charLength(str->data + pos, str->length - pos);
        if (pos + charLen > str->length)
            charLen = str->length - pos;

        NDSContext_setSlotString(ctx, 0, str->data + pos, charLen);
        return NDSStatus_OK;
    }

    if (chunkID == NDSBuiltinAtom_Byte){
        size_t idx = 0;
        if (!Util_resolveIndex(rawIdx, str->length, &idx)){
            NDSContext_setError(ctx, "index out of range");
            return NDSStatus_Error;
        }
        NDSContext_setSlotNumber(ctx, 0, (double)(unsigned char)str->data[idx]);
        return NDSStatus_OK;
    }

    NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for string", NDSContext_atomName(ctx, chunkID));
    return NDSStatus_Error;
}

static NDSStatus
String_chunkGetRange(NDSContext *ctx, NDSChunkID chunkID)
{
    StringData *str = NDSContext_getSlotStringData(ctx, 0);

    if (NDSContext_getSlotType(ctx, 1) != NDSValueType_Number ||
        NDSContext_getSlotType(ctx, 2) != NDSValueType_Number){
        NDSContext_setError(ctx, "range indices must be numbers");
        return NDSStatus_Error;
    }

    double rawStart = NDSContext_getSlotNumber(ctx, 1);
    double rawEnd = NDSContext_getSlotNumber(ctx, 2);

    if (chunkID == NDSBuiltinAtom_Character){
        size_t charCount = String_countChars(str);
        size_t startIdx, endIdx;
        if (!Util_resolveIndex(rawStart, charCount, &startIdx) ||
            !Util_resolveIndex(rawEnd, charCount, &endIdx)){
            NDSContext_setError(ctx, "index out of range");
            return NDSStatus_Error;
        }
        if (endIdx < startIdx){
            NDSContext_setSlotString(ctx, 0, "", 0);
            return NDSStatus_OK;
        }

        if (charCount == str->length){
            NDSContext_setSlotString(ctx, 0, str->data + startIdx, endIdx - startIdx + 1);
            return NDSStatus_OK;
        }

        /* Multibyte: find byte offsets */
        size_t startByte = String_charOffset(str, startIdx);
        size_t endByte = String_charOffset(str, endIdx);
        size_t endCharLen = String_charLength(str->data + endByte, str->length - endByte);
        NDSContext_setSlotString(ctx, 0, str->data + startByte, endByte - startByte + endCharLen);
        return NDSStatus_OK;
    }

    if (chunkID == NDSBuiltinAtom_Byte){
        size_t startIdx, endIdx;
        if (!Util_resolveIndex(rawStart, str->length, &startIdx) ||
            !Util_resolveIndex(rawEnd, str->length, &endIdx)){
            NDSContext_setError(ctx, "index out of range");
            return NDSStatus_Error;
        }
        if (endIdx < startIdx){
            NDSContext_setSlotString(ctx, 0, "", 0);
            return NDSStatus_OK;
        }
        NDSContext_setSlotString(ctx, 0, str->data + startIdx, endIdx - startIdx + 1);
        return NDSStatus_OK;
    }

    NDSContext_setErrorF(ctx, "'%s' range is not supported for string", NDSContext_atomName(ctx, chunkID));
    return NDSStatus_Error;
}

static NDSStatus
String_chunkGetEvery(NDSContext *ctx, NDSChunkID chunkID)
{
    StringData *str = NDSContext_getSlotStringData(ctx, 0);

    if (NDSContext_ensureSlots(ctx, 3) != NDSStatus_OK)
        return NDSStatus_Error;
    if (NDSContext_setSlotNewList(ctx, 1) != NDSStatus_OK)
        return NDSStatus_Error;
    NDSObject *resultObj = NDSContext_slotPointer(ctx, 1)->as.object;

    if (chunkID == NDSBuiltinAtom_Character){
        mbstate_t ps;
        memset(&ps, 0, sizeof(ps));
        size_t pos = 0;
        while (pos < str->length){
            size_t charLen = mbrlen(str->data + pos, str->length - pos, &ps);
            if (charLen == (size_t)-1){
                charLen = 1;
                memset(&ps, 0, sizeof(ps));
            } else if (charLen == (size_t)-2){
                break;
            }

            if (!NDSContext_newString(ctx, str->data + pos, charLen, NDSContext_slotPointer(ctx, 2))){
                NDSContext_setError(ctx, "out of memory");
                return NDSStatus_Error;
            }
            if (!List_append(ctx, resultObj, NDSContext_getSlot(ctx, 2))){
                NDSContext_setError(ctx, "out of memory");
                return NDSStatus_Error;
            }
            pos += charLen;
        }
    } else if (chunkID == NDSBuiltinAtom_Byte){
        for (size_t i = 0; i < str->length; i++){
            NDSValue bv = NDSValue_numberFromDouble((double)(unsigned char)str->data[i]);
            if (!List_append(ctx, resultObj, bv)){
                NDSContext_setError(ctx, "out of memory");
                return NDSStatus_Error;
            }
        }
    } else{
        NDSContext_setErrorF(ctx, "'every %s' is not supported for string", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    NDSContext_copySlot(ctx, 1, 0);
    return NDSStatus_OK;
}

static NDSObject *
List_new(NDSContext *ctx, NDSValue *dest)
{
    return NDSContext_newObject(ctx, NDSValueType_List, dest);
}

static bool
List_grow(NDSContext *ctx, NDSObject *obj)
{
    List *list = (List *)NDSObject_getExtraData(ctx, obj);
    size_t newCap = 0;
    if (list->capacity < 8)
        newCap = 8;
    else if (!Util_safeMulSize(list->capacity, 2, &newCap))
        return false;
    size_t allocSize;
    if (!Util_safeMulSize(newCap, sizeof(NDSValue), &allocSize))
        return false;
    NDSValue *newItems = NDSAllocator_reallocate(ctx->allocator, list->items, allocSize);
    if (!newItems)
        return false;
    ctx->gcBytesAllocated += (newCap - list->capacity) * sizeof(NDSValue);
    list->items = newItems;
    list->capacity = newCap;
    return true;
}

static NDSValue
List_get(const List *list, size_t index)
{
    if (index >= list->count)
        return NDSValue_Nothing;
    return list->items[index];
}

static bool
List_set(List *list, size_t index, NDSValue value)
{
    if (index >= list->count)
        return false;
    list->items[index] = value;
    return true;
}

static bool
List_insert(NDSContext *ctx, NDSObject *obj, size_t index, NDSValue value)
{
    List *list = (List *)NDSObject_getExtraData(ctx, obj);
    if (index > list->count)
        return false;
    if (list->count >= list->capacity && !List_grow(ctx, obj))
        return false;
    if (index < list->count)
        memmove(&list->items[index + 1], &list->items[index], (list->count - index) * sizeof(NDSValue));

    list->items[index] = value;
    list->count++;
    return true;
}

static bool
List_remove(List *list, size_t index)
{
    if (index >= list->count)
        return false;
    list->count--;
    if (index < list->count)
        memmove(&list->items[index], &list->items[index + 1], (list->count - index) * sizeof(NDSValue));

    return true;
}

static bool
List_append(NDSContext *ctx, NDSObject *obj, NDSValue value)
{
    List *list = (List *)NDSObject_getExtraData(ctx, obj);
    if (list->count >= list->capacity && !List_grow(ctx, obj))
        return false;
    list->items[list->count++] = value;
    return true;
}

static void
List_trace(NDSContext *ctx, NDSObject *obj)
{
    List *list = (List *)NDSObject_getExtraData(ctx, obj);
    for (size_t i = 0; i < list->count; i++)
        NDSContext_gcMark(ctx, list->items[i]);
}

static void
List_finalize(NDSContext *ctx, void *extraData)
{
    List *list = extraData;
    NDSAllocator_free(ctx->allocator, list->items);
}

static NDSStatus
List_equals(NDSContext *ctx)
{
    List *a = NDSContext_getSlotList(ctx, 0);
    List *b = NDSContext_getSlotList(ctx, 1);
    bool eq = true;
    if (a->count != b->count)
        eq = false;
    else{
        for (size_t i = 0; i < a->count; i++){
            if (!Value_equals(ctx, a->items[i], b->items[i])){
                eq = false;
                break;
            }
        }
    }
    NDSContext_setSlotBoolean(ctx, 0, eq);
    return NDSStatus_OK;
}

static NDSStatus
List_contains(NDSContext *ctx)
{
    List *list = NDSContext_getSlotList(ctx, 0);
    NDSValue needle = NDSContext_getSlot(ctx, 1);
    bool found = false;
    for (size_t i = 0; i < list->count; i++){
        if (Value_equals(ctx, list->items[i], needle)){
            found = true;
            break;
        }
    }
    NDSContext_setSlotBoolean(ctx, 0, found);
    return NDSStatus_OK;
}


static NDSStatus
List_toString(NDSContext *ctx)
{
    if (ctx->toStringDepth >= TOSTRING_MAX_DEPTH){
        NDSContext_setSlotString(ctx, 0, "[...]", 5);
        return NDSStatus_OK;
    }
    ctx->toStringDepth++;

    if (NDSContext_ensureSlots(ctx, 2) != NDSStatus_OK){
        ctx->toStringDepth--;
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);
    NDSAllocator *alloc = ctx->allocator;

    size_t cap = TOSTRING_INITIAL_CAPACITY;
    size_t pos = 0;
    char *buf = NDSAllocator_allocate(alloc, cap);
    if (!buf){
        NDSContext_setSlotString(ctx, 0, "[]", 2);
        ctx->toStringDepth--;
        return NDSStatus_OK;
    }

    buf[pos++] = '[';

    for (size_t i = 0; i < list->count; i++){
        if (i > 0){
            if (!StringBuf_grow(alloc, &buf, &cap, pos + 2))
                break;
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        *NDSContext_slotPointer(ctx, 1) = list->items[i];
        if (NDSContext_slotToString(ctx, 1, 1) != NDSStatus_OK)
            break;
        StringData *s = NDSContext_getSlotStringData(ctx, 1);
        if (!StringBuf_grow(alloc, &buf, &cap, pos + s->length + 1))
            break;
        memcpy(buf + pos, s->data, s->length);
        pos += s->length;
    }

    buf[pos++] = ']';
    NDSContext_setSlotString(ctx, 0, buf, pos);
    NDSAllocator_free(alloc, buf);
    ctx->toStringDepth--;
    return NDSStatus_OK;
}

static NDSStatus
List_propertyGet(NDSContext *ctx, NDSAtom nameAtom)
{
    if (nameAtom == NDSBuiltinAtom_Length || nameAtom == NDSBuiltinAtom_Count){
        List *list = NDSContext_getSlotList(ctx, 0);
        NDSContext_setSlotNumber(ctx, 0, (double)list->count);
        return NDSStatus_OK;
    }
    NDSContext_setErrorF(ctx, "list has no property '%s'", NDSContext_atomName(ctx, nameAtom));
    return NDSStatus_Error;
}

static NDSStatus
List_chunkGet(NDSContext *ctx, NDSChunkID chunkID)
{
    List *list = NDSContext_getSlotList(ctx, 0);

    if (chunkID == NDSBuiltinAtom_Offset){
        NDSValue needle = NDSContext_getSlot(ctx, 1);
        for (size_t i = 0; i < list->count; i++){
            if (Value_equals(ctx, list->items[i], needle)){
                NDSContext_setSlotNumber(ctx, 0, (double)(i + 1));
                return NDSStatus_OK;
            }
        }
        NDSContext_setSlotNumber(ctx, 0, 0.0);
        return NDSStatus_OK;
    }

    if (chunkID != NDSBuiltinAtom_Item){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for list", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    if (NDSContext_getSlotType(ctx, 1) != NDSValueType_Number){
        NDSContext_setError(ctx, "expected a number");
        return NDSStatus_Error;
    }
    double rawIdx = NDSContext_getSlotNumber(ctx, 1);

    size_t idx = 0;
    if (!Util_resolveIndex(rawIdx, list->count, &idx)){
        NDSContext_setError(ctx, "index out of range");
        return NDSStatus_Error;
    }

    *NDSContext_slotPointer(ctx, 0) = list->items[idx];
    return NDSStatus_OK;
}

static NDSStatus
List_chunkGetRange(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item){
        NDSContext_setErrorF(ctx, "'%s' range is not supported for list", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }
    if (NDSContext_getSlotType(ctx, 1) != NDSValueType_Number
    ||  NDSContext_getSlotType(ctx, 2) != NDSValueType_Number){
        NDSContext_setError(ctx, "range indices must be numbers");
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);
    double rawStart = NDSContext_getSlotNumber(ctx, 1);
    double rawEnd = NDSContext_getSlotNumber(ctx, 2);

    size_t startIdx = 0;
    size_t endIdx = 0;
    if (!Util_resolveIndex(rawStart, list->count, &startIdx)
    ||  !Util_resolveIndex(rawEnd, list->count, &endIdx)){
        NDSContext_setError(ctx, "index out of range");
        return NDSStatus_Error;
    }

    if (NDSContext_ensureSlots(ctx, 2) != NDSStatus_OK)
        return NDSStatus_Error;
    if (NDSContext_setSlotNewList(ctx, 1) != NDSStatus_OK)
        return NDSStatus_Error;
    NDSObject *resultObj = NDSContext_slotPointer(ctx, 1)->as.object;

    if (endIdx >= startIdx){
        for (size_t i = startIdx; i <= endIdx; i++){
            if (!List_append(ctx, resultObj, list->items[i])){
                NDSContext_setError(ctx, "out of memory");
                return NDSStatus_Error;
            }
        }
    }

    NDSContext_copySlot(ctx, 1, 0);
    return NDSStatus_OK;
}

static NDSStatus
List_chunkGetEvery(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for list", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    List *src = NDSContext_getSlotList(ctx, 0);

    if (NDSContext_ensureSlots(ctx, 2) != NDSStatus_OK)
        return NDSStatus_Error;
    if (NDSContext_setSlotNewList(ctx, 1) != NDSStatus_OK)
        return NDSStatus_Error;
    NDSObject *resultObj = NDSContext_slotPointer(ctx, 1)->as.object;

    for (size_t i = 0; i < src->count; i++){
        if (!List_append(ctx, resultObj, src->items[i])){
            NDSContext_setError(ctx, "out of memory");
            return NDSStatus_Error;
        }
    }

    NDSContext_copySlot(ctx, 1, 0);
    return NDSStatus_OK;
}

static NDSStatus
List_chunkSet(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for list", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_List
    ||  NDSContext_getSlotType(ctx, 1) != NDSValueType_Number){
        NDSContext_setError(ctx, "expected a list and a number");
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);
    size_t idx = 0;
    if (!Util_resolveIndex(NDSContext_getSlotNumber(ctx, 1), list->count, &idx)){
        NDSContext_setError(ctx, "index out of range");
        return NDSStatus_Error;
    }

    list->items[idx] = NDSContext_getSlot(ctx, 2);
    return NDSStatus_OK;
}

static NDSStatus
List_chunkDelete(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for list", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_List
    ||  NDSContext_getSlotType(ctx, 1) != NDSValueType_Number){
        NDSContext_setError(ctx, "expected a list and a number");
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);
    size_t idx = 0;
    if (!Util_resolveIndex(NDSContext_getSlotNumber(ctx, 1), list->count, &idx)){
        NDSContext_setError(ctx, "index out of range");
        return NDSStatus_Error;
    }

    List_remove(list, idx);
    return NDSStatus_OK;
}

static uint32_t
Map_probe(NDSContext *ctx, const Map *map, NDSValue key, uint32_t hash, uint32_t *outDenseIdx)
{
    uint32_t mask = (uint32_t)(map->bucketCount - 1);
    uint32_t idx = hash & mask;
    while (true){
        uint32_t entry = map->buckets[idx];
        if (entry == MAP_EMPTY){
            *outDenseIdx = MAP_EMPTY;
            return idx;
        }
        if (entry != MAP_TOMBSTONE && Value_equals(ctx, map->keys[entry], key)){
            *outDenseIdx = entry;
            return idx;
        }
        idx = (idx + 1) & mask;
    }
}

static bool
Map_growDense(NDSContext *ctx, NDSObject *obj)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);
    size_t newCap;
    if (map->capacity < 8)
        newCap = 8;
    else if (!Util_safeMulSize(map->capacity, 2, &newCap))
        return false;
    size_t allocSize;
    if (!Util_safeMulSize(newCap, sizeof(NDSValue), &allocSize))
        return false;
    NDSValue *newKeys = NDSAllocator_reallocate(ctx->allocator, map->keys, allocSize);
    if (!newKeys)
        return false;
    map->keys = newKeys; /* store immediately so it isn't dangling if values realloc fails */
    NDSValue *newValues = NDSAllocator_reallocate(ctx->allocator, map->values, allocSize);
    if (!newValues)
        return false;
    map->values = newValues;
    ctx->gcBytesAllocated += (newCap - map->capacity) * 2 * sizeof(NDSValue); /* keys + values */
    map->capacity = newCap;
    return true;
}

static void
Map_rehash(const NDSContext *ctx, const Map *map, uint32_t *buckets, size_t bucketCount)
{
    uint32_t mask = (uint32_t)(bucketCount - 1);
    for (size_t i = 0; i < bucketCount; i++)
        buckets[i] = MAP_EMPTY;
    for (size_t i = 0; i < map->used; i++){
        if (map->keys[i].type == NDSValueType_Nothing)
            continue;
        uint32_t h = Hash_value(ctx, map->keys[i]) & mask;
        while (buckets[h] != MAP_EMPTY)
            h = (h + 1) & mask;
        buckets[h] = (uint32_t)i;
    }
}

static void
Map_compact(const NDSContext *ctx, Map *map)
{
    size_t dst = 0;
    for (size_t src = 0; src < map->used; src++){
        if (map->keys[src].type == NDSValueType_Nothing)
            continue;
        if (dst != src){
            map->keys[dst] = map->keys[src];
            map->values[dst] = map->values[src];
        }
        dst++;
    }
    map->used = map->count;
    Map_rehash(ctx, map, map->buckets, map->bucketCount);
}

static bool
Map_growBuckets(NDSContext *ctx, NDSObject *obj)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);

    if (map->used > map->count && map->buckets)
        Map_compact(ctx, map);

    if (map->buckets && (map->used + 1) * 100 <= map->bucketCount * MAP_MAX_LOAD_PERCENT)
        return true;

    size_t newBucketCount = 0;
    if (map->bucketCount < MAP_INITIAL_BUCKET_COUNT)
        newBucketCount = MAP_INITIAL_BUCKET_COUNT;
    else if (!Util_safeMulSize(map->bucketCount, 2, &newBucketCount))
        return false;
    size_t bucketAllocSize = 0;
    if (!Util_safeMulSize(newBucketCount, sizeof(uint32_t), &bucketAllocSize))
        return false;
    uint32_t *newBuckets = NDSAllocator_allocate(ctx->allocator, bucketAllocSize);
    if (!newBuckets)
        return false;
    Map_rehash(ctx, map, newBuckets, newBucketCount);
    ctx->gcBytesAllocated += (newBucketCount - map->bucketCount) * sizeof(uint32_t);
    NDSAllocator_free(ctx->allocator, map->buckets);
    map->buckets = newBuckets;
    map->bucketCount = newBucketCount;
    return true;
}

/* Returns 1 on new insert, 0 on update, -1 on error. */
static int
Map_set(NDSContext *ctx, NDSObject *obj, NDSValue key, NDSValue value)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);

    if (!Map_isValidKey(key))
        return -1;

    if (!map->buckets && !Map_growBuckets(ctx, obj))
        return -1;

    uint32_t hash = Hash_value(ctx, key);
    uint32_t denseIdx;
    Map_probe(ctx, map, key, hash, &denseIdx);

    if (denseIdx != MAP_EMPTY){
        map->values[denseIdx] = value;
        return 0;
    }

    if ((map->used + 1) * 100 > map->bucketCount * MAP_MAX_LOAD_PERCENT){
        if (!Map_growBuckets(ctx, obj))
            return -1;
    }

    if (map->used >= map->capacity && !Map_growDense(ctx, obj))
        return -1;

    uint32_t mask = (uint32_t)(map->bucketCount - 1);
    uint32_t bucketIdx = hash & mask;
    while (map->buckets[bucketIdx] != MAP_EMPTY && map->buckets[bucketIdx] != MAP_TOMBSTONE)
        bucketIdx = (bucketIdx + 1) & mask;

    map->buckets[bucketIdx] = (uint32_t)map->used;
    map->keys[map->used] = key;
    map->values[map->used] = value;
    map->used++;
    map->count++;
    return 1;
}

static bool
Map_get(NDSContext *ctx, NDSObject *obj, NDSValue key, NDSValue *outValue)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);
    if (!map->buckets || map->count == 0 || !Map_isValidKey(key))
        return false;

    uint32_t denseIdx = 0;
    Map_probe(ctx, map, key, Hash_value(ctx, key), &denseIdx);
    if (denseIdx == MAP_EMPTY)
        return false;
    if (outValue)
        *outValue = map->values[denseIdx];
    return true;
}

static bool
Map_remove(NDSContext *ctx, NDSObject *obj, NDSValue key)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);
    if (!map->buckets || map->count == 0 || !Map_isValidKey(key))
        return false;

    uint32_t denseIdx = 0;
    uint32_t bucketIdx = Map_probe(ctx, map, key, Hash_value(ctx, key), &denseIdx);
    if (denseIdx == MAP_EMPTY)
        return false;

    map->buckets[bucketIdx] = MAP_TOMBSTONE;
    map->keys[denseIdx] = NDSValue_Nothing;
    map->values[denseIdx] = NDSValue_Nothing;
    map->count--;

    /* Compact when gaps exceed live entries */
    if (map->used > 2 * map->count)
        Map_compact(ctx, map);

    return true;
}

static void
Map_trace(NDSContext *ctx, NDSObject *obj)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);
    for (size_t i = 0; i < map->used; i++){
        if (map->keys[i].type == NDSValueType_Nothing)
            continue;
        NDSContext_gcMark(ctx, map->keys[i]);
        NDSContext_gcMark(ctx, map->values[i]);
    }
}

static void
Map_finalize(NDSContext *ctx, void *extraData)
{
    Map *map = extraData;
    NDSAllocator_free(ctx->allocator, map->keys);
    NDSAllocator_free(ctx->allocator, map->values);
    NDSAllocator_free(ctx->allocator, map->buckets);
}

static NDSStatus
Map_equals(NDSContext *ctx)
{
    Map *a = NDSContext_getSlotMap(ctx, 0);
    Map *b = NDSContext_getSlotMap(ctx, 1);
    NDSObject *bObj = NDSContext_getSlot(ctx, 1).as.object;
    bool eq = true;
    if (a->count != b->count)
        eq = false;
    else{
        for (size_t i = 0; i < a->used; i++){
            NDSValue val = {0};
            if (a->keys[i].type == NDSValueType_Nothing)
                continue;
            if (!Map_get(ctx, bObj, a->keys[i], &val) || !Value_equals(ctx, a->values[i], val)){
                eq = false;
                break;
            }
        }
    }
    NDSContext_setSlotBoolean(ctx, 0, eq);
    return NDSStatus_OK;
}

static NDSStatus
Map_contains(NDSContext *ctx)
{
    NDSValue containerVal = NDSContext_getSlot(ctx, 0);
    NDSValue key = NDSContext_getSlot(ctx, 1);
    NDSContext_setSlotBoolean(ctx, 0, Map_get(ctx, containerVal.as.object, key, NULL));
    return NDSStatus_OK;
}

static NDSStatus
Map_toString(NDSContext *ctx)
{
    if (ctx->toStringDepth >= TOSTRING_MAX_DEPTH){
        NDSContext_setSlotString(ctx, 0, "{...}", 5);
        return NDSStatus_OK;
    }
    ctx->toStringDepth++;

    if (NDSContext_ensureSlots(ctx, 3) != NDSStatus_OK){
        ctx->toStringDepth--;
        return NDSStatus_Error;
    }

    Map *map = NDSContext_getSlotMap(ctx, 0);
    NDSAllocator *alloc = ctx->allocator;

    size_t cap = TOSTRING_INITIAL_CAPACITY;
    size_t pos = 0;
    char *buf = NDSAllocator_allocate(alloc, cap);
    if (!buf){
        NDSContext_setSlotString(ctx, 0, "{}", 2);
        ctx->toStringDepth--;
        return NDSStatus_OK;
    }

    buf[pos++] = '{';

    bool first = true;
    for (size_t i = 0; i < map->used; i++){
        if (map->keys[i].type == NDSValueType_Nothing)
            continue;
        if (!first){
            if (!StringBuf_grow(alloc, &buf, &cap, pos + 2))
                break;
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        first = false;

        *NDSContext_slotPointer(ctx, 1) = map->keys[i];
        if (NDSContext_slotToString(ctx, 1, 1) != NDSStatus_OK)
            break;
        StringData *s = NDSContext_getSlotStringData(ctx, 1);
        if (!StringBuf_grow(alloc, &buf, &cap, pos + s->length + 4))
            break;
        memcpy(buf + pos, s->data, s->length);
        pos += s->length;

        if (!StringBuf_grow(alloc, &buf, &cap, pos + 2))
            break;
        buf[pos++] = ':';
        buf[pos++] = ' ';

        *NDSContext_slotPointer(ctx, 2) = map->values[i];
        if (NDSContext_slotToString(ctx, 2, 2) != NDSStatus_OK)
            break;
        s = NDSContext_getSlotStringData(ctx, 2);
        if (!StringBuf_grow(alloc, &buf, &cap, pos + s->length + 1))
            break;
        memcpy(buf + pos, s->data, s->length);
        pos += s->length;
    }

    buf[pos++] = '}';
    NDSContext_setSlotString(ctx, 0, buf, pos);
    NDSAllocator_free(alloc, buf);
    ctx->toStringDepth--;
    return NDSStatus_OK;
}

static NDSStatus
Map_propertyGet(NDSContext *ctx, NDSAtom nameAtom)
{
    if (nameAtom == NDSBuiltinAtom_Length || nameAtom == NDSBuiltinAtom_Count){
        Map *map = NDSContext_getSlotMap(ctx, 0);
        NDSContext_setSlotNumber(ctx, 0, (double)map->count);
        return NDSStatus_OK;
    }
    NDSContext_setErrorF(ctx, "map has no property '%s'", NDSContext_atomName(ctx, nameAtom));
    return NDSStatus_Error;
}

static NDSStatus
Map_chunkGet(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item && chunkID != NDSBuiltinAtom_Key){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for map", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    NDSValue containerVal = NDSContext_getSlot(ctx, 0);
    NDSValue key = NDSContext_getSlot(ctx, 1);

    NDSValue result;
    if (!Map_get(ctx, containerVal.as.object, key, &result)){
        NDSContext_setError(ctx, "key not found");
        return NDSStatus_Error;
    }

    *NDSContext_slotPointer(ctx, 0) = result;
    return NDSStatus_OK;
}

static NDSStatus
Map_chunkGetEvery(NDSContext *ctx, NDSChunkID chunkID)
{
    Map *map = NDSContext_getSlotMap(ctx, 0);

    if (NDSContext_ensureSlots(ctx, 3) != NDSStatus_OK)
        return NDSStatus_Error;
    if (NDSContext_setSlotNewList(ctx, 1) != NDSStatus_OK)
        return NDSStatus_Error;
    NDSObject *resultObj = NDSContext_slotPointer(ctx, 1)->as.object;

    for (size_t i = 0; i < map->used; i++){
        if (map->keys[i].type == NDSValueType_Nothing)
            continue;

        if (chunkID == NDSBuiltinAtom_Item || chunkID == NDSBuiltinAtom_Value){
            if (!List_append(ctx, resultObj, map->values[i]))
                goto oom;
        } else if (chunkID == NDSBuiltinAtom_Key){
            if (!List_append(ctx, resultObj, map->keys[i]))
                goto oom;
        } else if (chunkID == NDSBuiltinAtom_Entry){
            if (!MapEntry_new(ctx, map->keys[i], map->values[i], NDSContext_slotPointer(ctx, 2)))
                goto oom;
            if (!List_append(ctx, resultObj, NDSContext_getSlot(ctx, 2)))
                goto oom;
        } else{
            NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for map", NDSContext_atomName(ctx, chunkID));
            return NDSStatus_Error;
        }
    }

    NDSContext_copySlot(ctx, 1, 0);
    return NDSStatus_OK;

oom:
    NDSContext_setError(ctx, "out of memory");
    return NDSStatus_Error;
}

static NDSStatus
Map_chunkSet(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item && chunkID != NDSBuiltinAtom_Key){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for map", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    NDSValue containerVal = NDSContext_getSlot(ctx, 0);
    NDSValue key = NDSContext_getSlot(ctx, 1);
    NDSValue value = NDSContext_getSlot(ctx, 2);

    if (Map_set(ctx, containerVal.as.object, key, value) < 0){
        NDSContext_setError(ctx, "invalid map key");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static NDSStatus
Map_chunkDelete(NDSContext *ctx, NDSChunkID chunkID)
{
    if (chunkID != NDSBuiltinAtom_Item && chunkID != NDSBuiltinAtom_Key &&
        chunkID != NDSBuiltinAtom_Entry){
        NDSContext_setErrorF(ctx, "'%s' is not a valid chunk type for map", NDSContext_atomName(ctx, chunkID));
        return NDSStatus_Error;
    }

    NDSValue containerVal = NDSContext_getSlot(ctx, 0);
    NDSValue key = NDSContext_getSlot(ctx, 1);

    if (!Map_remove(ctx, containerVal.as.object, key)){
        NDSContext_setError(ctx, "key not found");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}


typedef enum{
    ErrorProp_Type = 0,
    ErrorProp_Message = 1,
    ErrorProp_Value = 2
} ErrorProp;

typedef enum{
    MapEntryProp_Key = 0,
    MapEntryProp_Value = 1
} MapEntryProp;

static bool
MapEntry_new(NDSContext *ctx, NDSValue key, NDSValue value, NDSValue *dest)
{
    NDSObject *obj = NDSContext_newObject(ctx, NDSValueType_Entry, dest);
    if (!obj)
        return false;
    NDSObject_setProperty(obj, MapEntryProp_Key, key);
    NDSObject_setProperty(obj, MapEntryProp_Value, value);
    return true;
}

static NDSStatus
Entry_equals(NDSContext *ctx)
{
    NDSContext_ensureSlots(ctx, 4);
    NDSContext_getObjectProperty(ctx, 0, MapEntryProp_Key, 2);
    NDSContext_getObjectProperty(ctx, 1, MapEntryProp_Key, 3);
    if (!NDSContext_slotsEqual(ctx, 2, 3)){
        NDSContext_setSlotBoolean(ctx, 0, false);
        return NDSStatus_OK;
    }
    NDSContext_getObjectProperty(ctx, 0, MapEntryProp_Value, 2);
    NDSContext_getObjectProperty(ctx, 1, MapEntryProp_Value, 3);
    NDSContext_setSlotBoolean(ctx, 0, NDSContext_slotsEqual(ctx, 2, 3));
    return NDSStatus_OK;
}

static NDSStatus
Entry_toString(NDSContext *ctx)
{
    NDSContext_ensureSlots(ctx, 3);
    NDSContext_getObjectProperty(ctx, 0, MapEntryProp_Key, 1);
    NDSContext_slotToString(ctx, 1, 1);
    NDSContext_getObjectProperty(ctx, 0, MapEntryProp_Value, 2);
    NDSContext_slotToString(ctx, 2, 2);

    const char *keyStr = NDSContext_getSlotString(ctx, 1);
    size_t keyLen = NDSContext_getSlotStringLength(ctx, 1);
    const char *valStr = NDSContext_getSlotString(ctx, 2);
    size_t valLen = NDSContext_getSlotStringLength(ctx, 2);

    size_t totalLen = keyLen + 2 + valLen; /* "key: value" */
    char *buf = NDSAllocator_allocate(ctx->allocator, totalLen + 1);
    if (!buf){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    memcpy(buf, keyStr, keyLen);
    buf[keyLen] = ':';
    buf[keyLen + 1] = ' ';
    memcpy(buf + keyLen + 2, valStr, valLen);
    buf[totalLen] = '\0';

    NDSContext_setSlotString(ctx, 0, buf, totalLen);
    NDSAllocator_free(ctx->allocator, buf);
    return NDSStatus_OK;
}

static bool
Error_new(NDSContext *ctx, NDSValue type, NDSValue message, NDSValue value, NDSValue *dest)
{
    NDSObject *obj = NDSContext_newObject(ctx, NDSValueType_Error, dest);
    if (!obj)
        return false;
    NDSObject_setProperty(obj, ErrorProp_Type, type);
    NDSObject_setProperty(obj, ErrorProp_Message, message);
    NDSObject_setProperty(obj, ErrorProp_Value, value);
    return true;
}

static NDSStatus
Error_equals(NDSContext *ctx)
{
    NDSValue a = NDSContext_getSlot(ctx, 0);
    NDSValue b = NDSContext_getSlot(ctx, 1);

    if (a.type != NDSValueType_Error || b.type != NDSValueType_Error || !a.as.object || !b.as.object){
        NDSContext_setSlotBoolean(ctx, 0, false);
        return NDSStatus_OK;
    }

    bool eq = Value_equals(ctx, NDSObject_getProperty(a.as.object, ErrorProp_Type), NDSObject_getProperty(b.as.object, ErrorProp_Type)) &&
              Value_equals(ctx, NDSObject_getProperty(a.as.object, ErrorProp_Message), NDSObject_getProperty(b.as.object, ErrorProp_Message)) &&
              Value_equals(ctx, NDSObject_getProperty(a.as.object, ErrorProp_Value), NDSObject_getProperty(b.as.object, ErrorProp_Value));

    NDSContext_setSlotBoolean(ctx, 0, eq);
    return NDSStatus_OK;
}

static NDSStatus
Error_toString(NDSContext *ctx)
{
    NDSValue v = NDSContext_getSlot(ctx, 0);
    if (v.type != NDSValueType_Error || !v.as.object){
        *NDSContext_slotPointer(ctx, 0) = ctx->stringError;
        return NDSStatus_OK;
    }

    if (NDSContext_ensureSlots(ctx, 3) != NDSStatus_OK)
        return NDSStatus_Error;

    *NDSContext_slotPointer(ctx, 1) = NDSObject_getProperty(v.as.object, ErrorProp_Type);
    NDSStatus typeStatus = NDSContext_slotToString(ctx, 1, 1);

    v = NDSContext_getSlot(ctx, 0);
    *NDSContext_slotPointer(ctx, 2) = NDSObject_getProperty(v.as.object, ErrorProp_Message);
    NDSStatus msgStatus = NDSContext_slotToString(ctx, 2, 2);

    StringData *typeSD = (typeStatus == NDSStatus_OK) ? NDSContext_getSlotStringData(ctx, 1) : NULL;
    StringData *msgSD = (msgStatus == NDSStatus_OK) ? NDSContext_getSlotStringData(ctx, 2) : NULL;

    char buf[NDSMaxErrorMessageLength * 2 + 1];
    int len;
    if (msgSD && msgSD->length > 0)
        len = snprintf(buf, sizeof(buf), "error (%.*s): %.*s", (int)(typeSD? typeSD->length : 0),
                       typeSD? typeSD->data : "", (int)msgSD->length, msgSD->data);
    else
        len = snprintf(buf, sizeof(buf), "error (%.*s)", (int)(typeSD? typeSD->length : 0), typeSD? typeSD->data : "");
    if (len < 0)
        len = 0;
    if ((size_t)len >= sizeof(buf))
        len = (int)(sizeof(buf) - 1);

    NDSContext_setSlotString(ctx, 0, buf, (size_t)len);
    if (NDSContext_isSlotNothing(ctx, 0)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static NDSStatus
Function_toString(NDSContext *ctx)
{
    *NDSContext_slotPointer(ctx, 0) = ctx->stringFunction;
    return NDSStatus_OK;
}


static NDSStatus
default_property_get(NDSContext *ctx, NDSAtom nameAtom)
{
    NDSValue containerVal = NDSContext_getSlot(ctx, 0);
    TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, containerVal.type);

    /* 1. Scan property slots first (so explicit "type" properties take precedence) */
    if (desc->propertyCount > 0 && containerVal.as.object){
        for (int i = 0; i < desc->propertyCount; i++){
            if (desc->propertyAtoms[i] == nameAtom){
                NDSContext_getObjectProperty(ctx, 0, i, 0);
                return NDSStatus_OK;
            }
        }
    }

    /* 2. Built-in "type" atom → return type name string */
    if (nameAtom == NDSBuiltinAtom_Type){
        const char *name = desc->name? desc->name : "unknown";
        NDSContext_setSlotString(ctx, 0, name, strlen(name));
        if (NDSContext_isSlotNothing(ctx, 0))
            return NDSStatus_Error;
        return NDSStatus_OK;
    }

    /* 3. Fall through to customPropertyGet */
    if (desc->customPropertyGet)
        return desc->customPropertyGet(ctx, nameAtom);

    NDSContext_setErrorF(ctx, "'%s' has no property '%s'",
                         NDSContext_typeName(ctx, containerVal.type),
                         NDSContext_atomName(ctx, nameAtom));
    return NDSStatus_Error;
}

static NDSStatus
default_contains(NDSContext *ctx)
{
    NDSContext_setErrorF(ctx, "'contains' not supported for type '%s'", NDSContext_typeName(ctx, NDSContext_getSlot(ctx, 0).type));
    return NDSStatus_Error;
}

#define DEFAULT_CHUNK_CALLBACK(name_, fmt_)                                                         \
    static NDSStatus                                                                               \
    name_(NDSContext *ctx, NDSChunkID chunkID)                                                     \
    {                                                                                              \
        NDSContext_setErrorF(ctx, fmt_,                                                            \
                             NDSContext_atomName(ctx, chunkID),                                    \
                             NDSContext_typeName(ctx, NDSContext_getSlot(ctx, 0).type));            \
        return NDSStatus_Error;                                                                    \
    }
DEFAULT_CHUNK_CALLBACK(default_chunk_get,       "'%s' of '%s' is not supported")
DEFAULT_CHUNK_CALLBACK(default_chunk_get_range,  "'%s' range access not supported for type '%s'")
DEFAULT_CHUNK_CALLBACK(default_chunk_set,        "cannot set '%s' of '%s'")
DEFAULT_CHUNK_CALLBACK(default_chunk_delete,     "cannot delete '%s' of '%s'")
DEFAULT_CHUNK_CALLBACK(default_chunk_get_every,  "'every %s' not supported for type '%s'")
#undef DEFAULT_CHUNK_CALLBACK

static NDSStatus
default_equals(NDSContext *ctx)
{
    NDSValue a = NDSContext_getSlot(ctx, 0);
    NDSValue b = NDSContext_getSlot(ctx, 1);
    NDSContext_setSlotBoolean(ctx, 0, a.as.object == b.as.object);
    return NDSStatus_OK;
}

static NDSStatus
default_compare(NDSContext *ctx)
{
    NDSContext_setErrorF(ctx, "comparison not supported for type '%s'", NDSContext_typeName(ctx, NDSContext_getSlot(ctx, 0).type));
    return NDSStatus_Error;
}

static NDSStatus
default_to_string(NDSContext *ctx)
{
    NDSValue v = NDSContext_getSlot(ctx, 0);
    TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, v.type);
    const char *name = desc->name? desc->name : "unknown";
    NDSContext_setSlotString(ctx, 0, name, strlen(name));
    if (NDSContext_isSlotNothing(ctx, 0)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

static void
default_trace(NDSContext *ctx, NDSObject *obj)
{
    TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, obj->type);
    if (desc->propertyCount == 0)
        return;
    for (int i = 0; i < desc->propertyCount; i++)
        NDSContext_gcMark(ctx, NDSObject_getProperty(obj, i));
}

static void
default_finalize(NDSContext *ctx, void *extraData)
{
    UNUSED(ctx);
    UNUSED(extraData);
}

static size_t
Util_defaultGCSize(NDSContext *ctx, NDSObject *obj);

static TypeDescriptor *
registerTypeInternal(NDSContext *ctx, const NDSTypeDescriptor *pub)
{
    if (ctx->typeCount >= ctx->typeCapacity){
        if (!Util_growArray(ctx->allocator, (void **)&ctx->types, &ctx->typeCapacity, sizeof(TypeDescriptor)))
            return NULL;
    }

    NDSTypeID id = (NDSTypeID)ctx->typeCount++;
    TypeDescriptor *td = &ctx->types[id];
    memset(td, 0, sizeof(*td));

    td->name = pub->name;
    td->nameAtom = pub->name? NDSContext_internAtom(ctx, pub->name) : NDSAtom_Invalid;
    if (pub->name && td->nameAtom == NDSAtom_Invalid)
        return NULL;
    td->propertyCount = pub->propertyCount;
    td->extraDataSize = pub->extraDataSize;
    td->isManaged = (pub->propertyCount > 0 || pub->extraDataSize > 0);

    /* Resolve property names to atoms */
    if (pub->propertyCount > 0 && pub->propertyNames){
        td->propertyAtoms = NDSAllocator_allocate(ctx->allocator, (size_t)pub->propertyCount * sizeof(NDSAtom));
        if (!td->propertyAtoms)
            return NULL;
        for (int i = 0; i < pub->propertyCount; i++){
            td->propertyAtoms[i] = NDSContext_internAtom(ctx, pub->propertyNames[i]);
            if (td->propertyAtoms[i] == NDSAtom_Invalid)
                return NULL;
        }
    }

    /* Wire in vtable: defaults for NULL, user's for non-NULL */
    td->property_get = default_property_get;
    td->customPropertyGet = pub->property_get; /* may be NULL */
    td->contains = pub->contains? pub->contains : default_contains;
    td->chunk_get = pub->chunk_get? pub->chunk_get : default_chunk_get;
    td->chunk_get_range = pub->chunk_get_range? pub->chunk_get_range : default_chunk_get_range;
    td->chunk_set = pub->chunk_set? pub->chunk_set : default_chunk_set;
    td->chunk_delete = pub->chunk_delete? pub->chunk_delete : default_chunk_delete;
    td->chunk_get_every = pub->chunk_get_every? pub->chunk_get_every : default_chunk_get_every;
    td->equals = pub->equals? pub->equals : default_equals;
    td->compare = pub->compare? pub->compare : default_compare;
    td->to_string = pub->to_string? pub->to_string : default_to_string;
    td->trace = default_trace;
    td->finalize = pub->finalize? pub->finalize : default_finalize;
    td->gc_size = Util_defaultGCSize;

    return td;
}

static size_t
Util_defaultGCSize(NDSContext *ctx, NDSObject *obj)
{
    TypeDescriptor *td = &ctx->types[obj->type];
    return sizeof(NDSObject) + (size_t)td->propertyCount * sizeof(NDSValue) + td->extraDataSize;
}

static size_t
String_gcSize(NDSContext *ctx, NDSObject *obj)
{
    StringData *sd = (StringData *)NDSObject_getExtraData(ctx, obj);
    return sizeof(NDSObject) + sizeof(StringData) + sd->length + 1;
}

static size_t
List_gcSize(NDSContext *ctx, NDSObject *obj)
{
    List *list = (List *)NDSObject_getExtraData(ctx, obj);
    return sizeof(NDSObject) + sizeof(List) + list->capacity * sizeof(NDSValue);
}

static size_t
Map_gcSize(NDSContext *ctx, NDSObject *obj)
{
    Map *map = (Map *)NDSObject_getExtraData(ctx, obj);
    return sizeof(NDSObject) + sizeof(Map) + map->capacity * 2 * sizeof(NDSValue) +
           map->bucketCount * sizeof(uint32_t);
}

static const char *errorPropNames[] = {"errorType", "message", "value"};
static const char *mapEntryPropNames[] = {"key", "value"};

static const NDSTypeDescriptor standardTypes[NDSValueType_BuiltinCount] ={
    [NDSValueType_Nothing] ={
        .name = "nothing",
        .equals = Nothing_equals,
        .to_string = Nothing_toString,
    },
    [NDSValueType_Boolean] ={
        .name = "boolean",
        .equals = Boolean_equals,
        .to_string = Boolean_toString,
    },
    [NDSValueType_Number] ={
        .name = "number",
        .equals = Number_equals,
        .compare = Number_compare,
        .to_string = Number_toString,
    },
    [NDSValueType_String] ={
        .name = "string",
        .extraDataSize = sizeof(StringData),
        .property_get = String_propertyGet,
        .contains = String_contains,
        .chunk_get = String_chunkGet,
        .chunk_get_range = String_chunkGetRange,
        .chunk_get_every = String_chunkGetEvery,
        .equals = String_equals,
        .compare = String_compare,
        .to_string = String_toString,
        .finalize = String_finalize,
    },
    [NDSValueType_List] ={
        .name = "list",
        .extraDataSize = sizeof(List),
        .property_get = List_propertyGet,
        .contains = List_contains,
        .chunk_get = List_chunkGet,
        .chunk_get_range = List_chunkGetRange,
        .chunk_set = List_chunkSet,
        .chunk_delete = List_chunkDelete,
        .chunk_get_every = List_chunkGetEvery,
        .equals = List_equals,
        .to_string = List_toString,
        .finalize = List_finalize,
    },
    [NDSValueType_Map] ={
        .name = "map",
        .extraDataSize = sizeof(Map),
        .property_get = Map_propertyGet,
        .contains = Map_contains,
        .chunk_get = Map_chunkGet,
        .chunk_set = Map_chunkSet,
        .chunk_delete = Map_chunkDelete,
        .chunk_get_every = Map_chunkGetEvery,
        .equals = Map_equals,
        .to_string = Map_toString,
        .finalize = Map_finalize,
    },
    [NDSValueType_Entry] ={
        .name = "mapEntry",
        .propertyNames = mapEntryPropNames,
        .propertyCount = 2,
        .equals = Entry_equals,
        .to_string = Entry_toString,
    },
    [NDSValueType_Error] ={
        .name = "error",
        .propertyNames = errorPropNames,
        .propertyCount = 3,
        .equals = Error_equals,
        .to_string = Error_toString,
    },
    [NDSValueType_Function] ={
        .name = "function",
        .to_string = Function_toString,
    },
};

static bool
NDSContext_registerStandardTypes(NDSContext *ctx)
{
    for (int i = 0; i < NDSValueType_BuiltinCount; i++){
        if (!registerTypeInternal(ctx, &standardTypes[i]))
            return false;
    }
    ctx->types[NDSValueType_String].gc_size = String_gcSize;
    ctx->types[NDSValueType_List].trace = List_trace;
    ctx->types[NDSValueType_List].gc_size = List_gcSize;
    ctx->types[NDSValueType_Map].trace = Map_trace;
    ctx->types[NDSValueType_Map].gc_size = Map_gcSize;
    return true;
}

static bool
NDSContext_registerBuiltinAtoms(NDSContext *ctx)
{
    /* If you change the order here, things will break. */
    /* clang-format off */
    static const struct{ const char *name; uint32_t expected; } builtins[] ={
        {"item",           NDSBuiltinAtom_Item},
        {"character",      NDSBuiltinAtom_Character},
        {"byte",           NDSBuiltinAtom_Byte},
        {"key",            NDSBuiltinAtom_Key},
        {"value",          NDSBuiltinAtom_Value},
        {"entry",          NDSBuiltinAtom_Entry},
        {"type",           NDSBuiltinAtom_Type},
        {"length",         NDSBuiltinAtom_Length},
        {"count",          NDSBuiltinAtom_Count},
        {"errortype",      NDSBuiltinAtom_ErrorType},
        {"message",        NDSBuiltinAtom_Message},
        {"it",             NDSBuiltinAtom_It},
        {"size",           NDSBuiltinAtom_Size},
        {"offset",         NDSBuiltinAtom_Offset},
    };
    /* clang-format on */
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++){
        if (NDSContext_internAtom(ctx, builtins[i].name) != builtins[i].expected)
            return false;
    }
    return true;
}

static bool
NDSContext_registerKeywordAtoms(NDSContext *ctx);

typedef enum{
    FuncKind_Script,
    FuncKind_Host
} FuncKind;

typedef NDSHostFunctionCallback HostFuncCallback;

struct Function{
    FuncKind kind;
    NDSAtom name;
    NDSAtom *params;
    int paramCount;
    union{
        struct{
            Instruction *code;
            LineInfo *lines;
            size_t codeLength;
            size_t handlerIP;
            int handlerErrorSlot;
            int maxLocals;
        } script;
        struct{
            HostFuncCallback callback;
        } host;
    } as;
};

#define INITIAL_FUNCTION_CAPACITY 16
#define INITIAL_COMMAND_CAPACITY 8

typedef enum{
    PatternStepKind_Word,
    PatternStepKind_Expression,
    PatternStepKind_Variable
} PatternStepKind;

typedef struct{
    PatternStepKind kind;
    NDSAtom atom;
} InternalPatternStep;

struct HostCommand{
    InternalPatternStep *steps;
    int stepCount;
    int headLength;
    int expressionCount;
    int variableCount;
    size_t functionIndex;
};

struct CommandTrieNode{
    NDSAtom atom;
    size_t commandIndex;
    CommandTrieNode *children;
    CommandTrieNode *next;
};

static NDSStatus
CommandTrie_insert(NDSContext *ctx, size_t commandIndex)
{
    HostCommand *cmd = &ctx->commands[commandIndex];
    CommandTrieNode **list = &ctx->commandTrie;

    for (int i = 0; i < cmd->headLength; i++){
        NDSAtom atom = cmd->steps[i].atom;
        bool isLast = (i == cmd->headLength - 1);

        CommandTrieNode *node = NULL;
        for (CommandTrieNode *c = *list; c; c = c->next){
            if (c->atom == atom){
                node = c;
                break;
            }
        }

        if (node){
            if (!isLast && node->commandIndex != (size_t)-1)
                return NDSStatus_Error;
            if (isLast && node->children)
                return NDSStatus_Error;
            if (isLast){
                if (node->commandIndex != (size_t)-1)
                    return NDSStatus_Error;
                node->commandIndex = commandIndex;
            }
            list = &node->children;
        } else{
            CommandTrieNode *newNode = NDSAllocator_allocateZeroed(ctx->allocator, sizeof(CommandTrieNode));
            if (!newNode)
                return NDSStatus_Error;
            newNode->atom = atom;
            newNode->commandIndex = isLast? commandIndex : (size_t)-1;
            newNode->next = *list;
            *list = newNode;
            list = &newNode->children;
        }
    }
    return NDSStatus_OK;
}

static size_t
FunctionTable_add(NDSContext *ctx, Function func)
{
    if (ctx->functionCount >= NDS_MAX_FUNCTIONS)
        return (size_t)-1;
    if (ctx->functionCount >= ctx->functionCapacity){
        if (!Util_growArray(ctx->allocator, (void **)&ctx->functions, &ctx->functionCapacity, sizeof(Function)))
            return (size_t)-1;
    }
    size_t idx = ctx->functionCount++;
    ctx->functions[idx] = func;
    return idx;
}

static NDSStatus
NDSContext_setupStdlib(NDSContext *context);

static void
PRNG_seed(uint64_t *state, uint64_t seed);

#define ALLOC_TABLE(member, capacityMember, initial, type)                                         \
    do{                                                                                            \
        size_t allocSize;                                                                          \
        if (!Util_safeMulSize((initial), sizeof(type), &allocSize)){                               \
            NDSContext_free(ctx);                                                                  \
            return NULL;                                                                           \
        }                                                                                          \
        ctx->capacityMember = initial;                                                             \
        ctx->member = NDSAllocator_allocateZeroed(alloc, allocSize);                               \
        if (!ctx->member){                                                                         \
            NDSContext_free(ctx);                                                                  \
            return NULL;                                                                           \
        }                                                                                          \
    } while (false)

NDSContext *
NDSContext_new(const NDSConfig *config)
{
    static const NDSConfig defaultConfig = {0};
    if (!config)
        config = &defaultConfig;
    CharClass_init();
    NDSAllocator *alloc = config->allocator? config->allocator : NDSAllocator_SystemAllocator;
    NDSContext *ctx = NDSAllocator_allocateZeroed(alloc, sizeof(NDSContext));
    if (!ctx)
        return NULL;

    /* Allocator — must be set first so NDSContext_free can use it */
    ctx->allocator = alloc;

    /* Type table */
    ALLOC_TABLE(types, typeCapacity, NDSValueType_BuiltinCount + 8, TypeDescriptor);

    /* Other tables */
    ALLOC_TABLE(atoms, atomCapacity, INITIAL_ATOM_CAPACITY, NDSValue);
    ctx->atomCount = 1; /* reserve index 0 */

    ALLOC_TABLE(wordFlags, atomCapacity, INITIAL_ATOM_CAPACITY, uint8_t);
    ALLOC_TABLE(constants, constantCapacity, INITIAL_CONSTANT_CAPACITY, NDSValue);
    ALLOC_TABLE(globals, globalCapacity, INITIAL_GLOBAL_CAPACITY, Global);
    ALLOC_TABLE(stack, stackCapacity, INITIAL_STACK_CAPACITY, NDSValue);
    ALLOC_TABLE(functions, functionCapacity, INITIAL_FUNCTION_CAPACITY, Function);
    ALLOC_TABLE(commands, commandCapacity, INITIAL_COMMAND_CAPACITY, HostCommand);

    /* Stack limit */
    ctx->maxStackDepth = config->maxStackDepth > 0? config->maxStackDepth : DEFAULT_MAX_STACK_DEPTH;

    /* GC */
    ctx->gcNextCollection = GC_INITIAL_THRESHOLD;

    /* Line table */
    ctx->errorSourceAtom = NDSAtom_Invalid;

    /* All init steps below can fail; INIT_CHECK cleans up and returns NULL. */
#define INIT_CHECK(expr)                                                                           \
    do{                                                                                            \
        if (!(expr)){                                                                              \
            NDSContext_free(ctx);                                                                  \
            return NULL;                                                                           \
        }                                                                                          \
    } while (false)

    INIT_CHECK(NDSContext_registerBuiltinAtoms(ctx));
    INIT_CHECK(NDSContext_registerKeywordAtoms(ctx));
    INIT_CHECK(NDSContext_registerStandardTypes(ctx));

    /* Pre-intern constant strings used by toString. */
    INIT_CHECK(NDSContext_newString(ctx, "nothing", sizeof("nothing") - 1, &ctx->stringNothing));
    INIT_CHECK(NDSContext_newString(ctx, "true", sizeof("true") - 1, &ctx->stringTrue));
    INIT_CHECK(NDSContext_newString(ctx, "false", sizeof("false") - 1, &ctx->stringFalse));
    INIT_CHECK(NDSContext_newString(ctx, "(function)", sizeof("(function)") - 1, &ctx->stringFunction));
    INIT_CHECK(NDSContext_newString(ctx, "error", sizeof("error") - 1, &ctx->stringError));

    INIT_CHECK(Globals_ensure(ctx, NDSBuiltinAtom_It) != (size_t)-1);
    INIT_CHECK(NDSContext_setupStdlib(ctx) == NDSStatus_OK);

    /* Set chunk-ID flags */
    NDSConfigHandle handle = { .ctx = ctx };
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "item") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "character") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "byte") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "key") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "value") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "entry") != NDSChunkID_Invalid);
    INIT_CHECK(NDSConfigHandle_registerChunkID(&handle, "offset") != NDSChunkID_Invalid);

    /* Standard constants */
    INIT_CHECK(NDSConfigHandle_registerConstantString(&handle, "NL", "\n", 1) == NDSStatus_OK);
    INIT_CHECK(NDSConfigHandle_registerConstantString(&handle, "TAB", "\t", 1) == NDSStatus_OK);
    INIT_CHECK(NDSConfigHandle_registerConstantString(&handle, "QUOTE", "\"", 1) == NDSStatus_OK);
    INIT_CHECK(NDSConfigHandle_registerConstantNumber(&handle, "PI", acos(-1.0)) == NDSStatus_OK);
    INIT_CHECK(NDSConfigHandle_registerConstantNumber(&handle, "E", exp(1.0)) == NDSStatus_OK);

#undef INIT_CHECK

    PRNG_seed(&ctx->randomState, 1);

    /* Cache locale decimal point for number formatting/parsing */
    /* FIXME - technically speaking this could be a multibyte character...:| */
    struct lconv *lc = localeconv();
    ctx->decimalPoint = (lc && lc->decimal_point[0])? lc->decimal_point : ".";

    /* Setup callback */
    if (config->setup){
        if (config->setup(&handle, config->userPointer) != NDSStatus_OK){
            NDSContext_free(ctx);
            return NULL;
        }
    }

    return ctx;
}

#undef ALLOC_TABLE

static void
CommandTrieNode_freeAll(NDSAllocator *alloc, CommandTrieNode *node)
{
    while (node){
        CommandTrieNode *next = node->next;
        CommandTrieNode_freeAll(alloc, node->children);
        NDSAllocator_free(alloc, node);
        node = next;
    }
}

void
NDSContext_free(NDSContext *self)
{
    NDSAllocator *alloc;
    NDSObject *obj;

    if (!self)
        return;

    alloc = self->allocator;

    /* Free all GC objects */
    obj = self->gcObjects;
    while (obj){
        NDSObject *next = obj->gcNext;

        if (obj->type < self->typeCount && self->types[obj->type].finalize != default_finalize){
            self->types[obj->type].finalize(self, NDSObject_getExtraData(self, obj));
        }

        NDSAllocator_free(alloc, obj);
        obj = next;
    }

    /* Free function table */
    for (size_t i = 0; i < self->functionCount; i++){
        if (self->functions[i].kind == FuncKind_Script){
            NDSAllocator_free(alloc, self->functions[i].as.script.code);
            NDSAllocator_free(alloc, self->functions[i].as.script.lines);
        }
        NDSAllocator_free(alloc, self->functions[i].params);
    }
    NDSAllocator_free(alloc, self->functions);

    /* Free command trie nodes */
    CommandTrieNode_freeAll(alloc, self->commandTrie);

    /* Free host command table */
    for (size_t i = 0; i < self->commandCount; i++)
        NDSAllocator_free(alloc, self->commands[i].steps);
    NDSAllocator_free(alloc, self->commands);

    /* Free type property arrays */
    for (size_t i = 0; i < self->typeCount; i++)
        NDSAllocator_free(alloc, self->types[i].propertyAtoms);

    /* Free dynamic arrays */
    NDSAllocator_free(alloc, self->stack);
    NDSAllocator_free(alloc, self->globals);
    NDSAllocator_free(alloc, self->constants);
    NDSAllocator_free(alloc, self->wordFlags);
    NDSAllocator_free(alloc, self->atomBuckets);
    NDSAllocator_free(alloc, self->atoms);
    NDSAllocator_free(alloc, self->types);
    NDSAllocator_free(alloc, self);
}

void *
NDSContext_allocateMemory(NDSContext *self, size_t size)
{
    return NDSAllocator_allocate(self->allocator, size);
}

void
NDSContext_freeMemory(NDSContext *self, void *pointer)
{
    NDSAllocator_free(self->allocator, pointer);
}

/* User data */

void
NDSContext_setUserPointer(NDSContext *self, void *pointer)
{
    self->userPointer = pointer;
}

void *
NDSContext_getUserPointer(const NDSContext *self)
{
    return self->userPointer;
}

NDSAtom
NDSConfigHandle_registerAtom(NDSConfigHandle *h, const char *name)
{
    return NDSContext_internAtom(h->ctx, name);
}

/* registerChunkID: implemented after lexer (needs TokenType, keyword table) */
static NDSChunkID
NDSConfigHandle_registerChunkID_impl(NDSContext *ctx, const char *name);

NDSChunkID
NDSConfigHandle_registerChunkID(NDSConfigHandle *h, const char *name)
{
    return NDSConfigHandle_registerChunkID_impl(h->ctx, name);
}

NDSTypeID
NDSConfigHandle_registerType(NDSConfigHandle *h, const NDSTypeDescriptor *descriptor)
{
    TypeDescriptor *td = registerTypeInternal(h->ctx, descriptor);
    if (!td)
        return NDSTypeID_Invalid;
    return (NDSTypeID)(td - h->ctx->types);
}

NDSStatus
NDSConfigHandle_registerFunction(NDSConfigHandle *h, const char *name, NDSHostFunctionCallback callback, int paramCount)
{
    NDSContext *ctx = h->ctx;
    NDSAtom atom = NDSContext_internAtom(ctx, name);
    if (atom == NDSAtom_Invalid)
        return NDSStatus_Error;
    Function func = {0};
    func.kind = FuncKind_Host;
    func.name = atom;
    func.paramCount = paramCount;
    func.as.host.callback = callback;
    size_t funcIdx = FunctionTable_add(ctx, func);
    if (funcIdx == (size_t)-1)
        return NDSStatus_Error;
    size_t slot = Globals_ensureConst(ctx, atom);
    if (slot == (size_t)-1)
        return NDSStatus_Error;
    ctx->globals[slot].value = NDSValue_functionFromIndex(funcIdx);
    return NDSStatus_OK;
}

/* Intern a name and ensure a const global slot for it.
   Returns the global index, or (size_t)-1 on failure. */
static size_t
ConfigHandle_ensureConstSlot(NDSConfigHandle *h, const char *name)
{
    NDSAtom atom = NDSContext_internAtom(h->ctx, name);
    if (atom == NDSAtom_Invalid)
        return (size_t)-1;
    return Globals_ensureConst(h->ctx, atom);
}

NDSStatus
NDSConfigHandle_registerConstantNumber(NDSConfigHandle *h, const char *name, double value)
{
    size_t slot = ConfigHandle_ensureConstSlot(h, name);
    if (slot == (size_t)-1)
        return NDSStatus_Error;
    h->ctx->globals[slot].value = NDSValue_numberFromDouble(value);
    return NDSStatus_OK;
}

NDSStatus
NDSConfigHandle_registerConstantString(NDSConfigHandle *h, const char *name, const char *s, size_t len)
{
    size_t slot = ConfigHandle_ensureConstSlot(h, name);
    if (slot == (size_t)-1)
        return NDSStatus_Error;
    if (!NDSContext_newString(h->ctx, s, len, &h->ctx->globals[slot].value))
        return NDSStatus_Error;
    return NDSStatus_OK;
}

NDSStatus
NDSConfigHandle_registerConstantBoolean(NDSConfigHandle *h, const char *name, bool value)
{
    size_t slot = ConfigHandle_ensureConstSlot(h, name);
    if (slot == (size_t)-1)
        return NDSStatus_Error;
    h->ctx->globals[slot].value = NDSValue_booleanFromBool(value);
    return NDSStatus_OK;
}

NDSStatus
NDSConfigHandle_registerCommand(NDSConfigHandle *h, const NDSPatternStep *pattern, NDSHostFunctionCallback callback)
{
    NDSContext *ctx = h->ctx;
    NDSAllocator *alloc = ctx->allocator;

    /* Count steps and validate */
    int stepCount = 0;
    int expressionCount = 0;
    int variableCount = 0;
    for (int i = 0; pattern[i].kind != NDSPatternStep_End; i++){
        if (i >= 256)
            return NDSStatus_Error;
        if (pattern[i].kind == NDSPatternStep_Expression)
            expressionCount++;
        else if (pattern[i].kind == NDSPatternStep_Variable)
            variableCount++;
        else if (pattern[i].kind == NDSPatternStep_Word){
            if (!pattern[i].word || pattern[i].word[0] == '\0')
                return NDSStatus_Error;
        }
        stepCount++;
    }
    if (stepCount == 0)
        return NDSStatus_Error;
    if (pattern[0].kind != NDSPatternStep_Word)
        return NDSStatus_Error;

    /* Build internal steps */
    size_t stepsSize;
    if (!Util_safeMulSize((size_t)stepCount, sizeof(InternalPatternStep), &stepsSize))
        return NDSStatus_Error;
    InternalPatternStep *steps = NDSAllocator_allocateZeroed(alloc, stepsSize);
    if (!steps)
        return NDSStatus_Error;

    int headLength = 0;
    bool inHead = true;
    for (int i = 0; i < stepCount; i++){
        switch (pattern[i].kind){
        case NDSPatternStep_Word:{
            const char *word = pattern[i].word;
            NDSAtom atom = NDSContext_internAtom(ctx, word);
            if (atom == NDSAtom_Invalid){
                NDSAllocator_free(alloc, steps);
                return NDSStatus_Error;
            }
            steps[i].kind = PatternStepKind_Word;
            steps[i].atom = atom;
            /* First word gets HostHead so the statement parser knows to enter
               host command dispatch */
            if (i == 0)
                ctx->wordFlags[atom] |= TokenWord_HostHead;
            if (inHead)
                headLength++;
            break;
        }
        case NDSPatternStep_Expression:
            steps[i].kind = PatternStepKind_Expression;
            inHead = false;
            break;
        case NDSPatternStep_Variable:
            steps[i].kind = PatternStepKind_Variable;
            inHead = false;
            break;
        case NDSPatternStep_End:
            break; /* unreachable */
        }
    }

    /* Register the callback as a host function.
       Use the first head word's atom as the function name. */
    Function func = {0};
    func.kind = FuncKind_Host;
    func.name = steps[0].atom;
    func.paramCount = expressionCount;
    func.as.host.callback = callback;
    size_t funcIdx = FunctionTable_add(ctx, func);
    if (funcIdx == (size_t)-1){
        NDSAllocator_free(alloc, steps);
        return NDSStatus_Error;
    }

    /* Grow command table if needed */
    if (ctx->commandCount >= ctx->commandCapacity){
        if (!Util_growArray(alloc, (void **)&ctx->commands, &ctx->commandCapacity, sizeof(HostCommand))){
            NDSAllocator_free(alloc, steps);
            return NDSStatus_Error;
        }
    }

    /* Store command */
    size_t cmdIndex = ctx->commandCount++;
    HostCommand *cmd = &ctx->commands[cmdIndex];
    cmd->steps = steps;
    cmd->stepCount = stepCount;
    cmd->headLength = headLength;
    cmd->expressionCount = expressionCount;
    cmd->variableCount = variableCount;
    cmd->functionIndex = funcIdx;

    /* Insert into command trie */
    return CommandTrie_insert(ctx, cmdIndex);
}

static const NDSValue *
NDSContext_slotPointerConst(const NDSContext *self, int slot)
{
    if (slot >= 0)
        return &self->stack[self->frameBase + (size_t)slot];
    return &self->stack[self->frameBase - (size_t)(-slot)];
}

static NDSValue *
NDSContext_slotPointer(NDSContext *self, int slot)
{
    return (NDSValue *)NDSContext_slotPointerConst(self, slot);
}

static bool
NDSContext_ensureStack(NDSContext *self, size_t needed)
{
    if (needed <= self->stackCapacity)
        return true;
    if (needed > self->maxStackDepth)
        return false;

    size_t newCap = self->stackCapacity;
    while (newCap < needed){
        if (newCap > SIZE_MAX / 2)
            return false;
        newCap *= 2;
    }
    if (newCap > self->maxStackDepth)
        newCap = self->maxStackDepth;

    size_t allocSize;
    if (!Util_safeMulSize(newCap, sizeof(NDSValue), &allocSize))
        return false;
    NDSValue *newStack = NDSAllocator_reallocate(self->allocator, self->stack, allocSize);
    if (!newStack)
        return false;
    self->stack = newStack;
    self->stackCapacity = newCap;
    return true;
}

NDSStatus
NDSContext_ensureSlots(NDSContext *self, int count)
{
    size_t needed = self->frameBase + (size_t)count;
    if (!NDSContext_ensureStack(self, needed)){
        NDSContext_setError(self, "stack overflow");
        return NDSStatus_Error;
    }

    for (size_t i = self->stackTop; i < needed; i++)
        self->stack[i] = NDSValue_Nothing;

    if (needed > self->stackTop)
        self->stackTop = needed;
    return NDSStatus_OK;
}

int
NDSContext_getSlotCount(const NDSContext *self)
{
    return (int)(self->stackTop - self->frameBase);
}

/* Slot API — reading */

double
NDSContext_getSlotNumber(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->as.number;
}

bool
NDSContext_getSlotBoolean(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->as.boolean;
}

const char *
NDSContext_getSlotString(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    if (v->type == NDSValueType_String && v->as.object){
        StringData *sd = (StringData *)NDSObject_getExtraData(self, v->as.object);
        return sd->data;
    }
    return "";
}

size_t
NDSContext_getSlotStringLength(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    if (v->type == NDSValueType_String && v->as.object){
        StringData *sd = (StringData *)NDSObject_getExtraData(self, v->as.object);
        return sd->length;
    }
    return 0;
}

void *
NDSContext_getSlotPointer(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->as.opaque;
}

int64_t
NDSContext_getSlotInteger(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->as.integer;
}

NDSTypeID
NDSContext_getSlotType(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->type;
}

bool
NDSContext_isSlotNothing(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    return v->type == NDSValueType_Nothing;
}

#define DEFINE_IS_SLOT(suffix, typeID)                                                              \
    bool NDSContext_isSlot##suffix(const NDSContext *self, int slot)                                \
    {                                                                                               \
        return NDSContext_slotPointerConst(self, slot)->type == typeID;                             \
    }
DEFINE_IS_SLOT(Number,   NDSValueType_Number)
DEFINE_IS_SLOT(Boolean,  NDSValueType_Boolean)
DEFINE_IS_SLOT(String,   NDSValueType_String)
DEFINE_IS_SLOT(List,     NDSValueType_List)
DEFINE_IS_SLOT(Map,      NDSValueType_Map)
DEFINE_IS_SLOT(Error,    NDSValueType_Error)
DEFINE_IS_SLOT(Function, NDSValueType_Function)
#undef DEFINE_IS_SLOT

const char *
NDSContext_getTypeName(const NDSContext *self, NDSTypeID typeID)
{
    if (typeID >= self->typeCount)
        return NULL;
    return self->types[typeID].name;
}

/* Invoke a type vtable method with args in a sub-frame. SETUP saves the
   current frame, PUSH pushes each arg, FRAME sets frameBase to the new frame's
   slot 0, TEARDOWN extracts the result (written to slot 0 by the method) and
   restores the caller's frame. */
#define VM_DISPATCH_SETUP(ctx_, nslots_, on_fail_)                                                 \
    size_t savedTop_ = (ctx_)->stackTop;                                                           \
    size_t savedBase_ = (ctx_)->frameBase;                                                         \
    if (!NDSContext_ensureStack((ctx_), (ctx_)->stackTop + (nslots_))){                            \
        on_fail_;                                                                                  \
    }

#define VM_DISPATCH_PUSH(ctx_, val_) (ctx_)->stack[(ctx_)->stackTop++] = (val_)

#define VM_DISPATCH_FRAME(ctx_) (ctx_)->frameBase = savedTop_

#define VM_DISPATCH_TEARDOWN(ctx_, result_var_)                                                    \
    (result_var_) = (ctx_)->stack[savedTop_];                                                      \
    (ctx_)->stackTop = savedTop_;                                                                  \
    (ctx_)->frameBase = savedBase_

NDSStatus
NDSContext_slotToString(NDSContext *self, int srcSlot, int destSlot)
{
    NDSValue v = *NDSContext_slotPointer(self, srcSlot);
    if (v.type == NDSValueType_String){
        *NDSContext_slotPointer(self, destSlot) = v;
        return NDSStatus_OK;
    }

    TypeDescriptor *desc = NDSContext_getTypeDesc(self, v.type);
    VM_DISPATCH_SETUP(self, 1, {
        *NDSContext_slotPointer(self, destSlot) = NDSValue_Nothing;
        NDSContext_setError(self, "toString failed");
        return NDSStatus_Error;
    });
    VM_DISPATCH_PUSH(self, v);
    VM_DISPATCH_FRAME(self);
    NDSStatus status = desc->to_string(self);
    NDSValue result;
    VM_DISPATCH_TEARDOWN(self, result);
    if (status != NDSStatus_OK || result.type != NDSValueType_String){
        *NDSContext_slotPointer(self, destSlot) = NDSValue_Nothing;
        if (self->errorMessage[0] == '\0')
            NDSContext_setError(self, "toString failed");
        return NDSStatus_Error;
    }
    *NDSContext_slotPointer(self, destSlot) = result;
    return NDSStatus_OK;
}

static NDSValue
NDSContext_getSlot(const NDSContext *self, int slot)
{
    return *NDSContext_slotPointerConst(self, slot);
}

/* Slot API — writing */

void
NDSContext_setSlotNumber(NDSContext *self, int slot, double n)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->type = NDSValueType_Number;
    v->as.number = n;
}

void
NDSContext_setSlotBoolean(NDSContext *self, int slot, bool b)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->type = NDSValueType_Boolean;
    v->as.boolean = b;
}

NDSStatus
NDSContext_setSlotString(NDSContext *self, int slot, const char *s, size_t len)
{
    if (!NDSContext_newString(self, s, len, NDSContext_slotPointer(self, slot))){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

void
NDSContext_setSlotNothing(NDSContext *self, int slot)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    *v = NDSValue_Nothing;
}

void
NDSContext_setSlotPointer(NDSContext *self, int slot, void *ptr)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->as.opaque = ptr;
}

NDSStatus
NDSContext_setSlotError(NDSContext *self, int slot, const char *message)
{
    NDSValue *sv = NDSContext_slotPointer(self, slot);
    if (!NDSContext_newString(self, message, strlen(message), sv)){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    /* The msg is rooted via *sv; Error_new overwrites *sv with the error
       object, which then traces the msg via its Message property. */
    NDSValue msgVal = *sv;
    if (!Error_new(self, NDSValue_Nothing, msgVal, NDSValue_Nothing, sv)){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

/* Typed value setters for unmanaged custom types */

void
NDSContext_setSlotTypedNumber(NDSContext *self, int slot, NDSTypeID typeID, double value)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->type = typeID;
    v->as.number = value;
}

void
NDSContext_setSlotTypedInteger(NDSContext *self, int slot, NDSTypeID typeID, int64_t value)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->type = typeID;
    v->as.integer = value;
}

void
NDSContext_setSlotTypedPointer(NDSContext *self, int slot, NDSTypeID typeID, void *ptr)
{
    NDSValue *v = NDSContext_slotPointer(self, slot);
    v->type = typeID;
    v->as.opaque = ptr;
}

void
NDSContext_copySlot(NDSContext *self, int srcSlot, int destSlot)
{
    *NDSContext_slotPointer(self, destSlot) = *NDSContext_slotPointer(self, srcSlot);
}

/* Managed objects */

void *
NDSContext_setSlotNewObject(NDSContext *self, int slot, NDSTypeID typeID)
{
    if (typeID >= self->typeCount){
        NDSContext_setError(self, "invalid type ID");
        return NULL;
    }
    NDSObject *obj = NDSContext_newObject(self, typeID, NDSContext_slotPointer(self, slot));
    if (!obj){
        NDSContext_setError(self, "out of memory");
        return NULL;
    }
    return NDSObject_getExtraData(self, obj);
}

void *
NDSContext_getSlotObjectData(const NDSContext *self, int slot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, slot);
    if (!v->as.object)
        return NULL;
    return NDSObject_getExtraData(self, v->as.object);
}

void
NDSContext_getObjectProperty(NDSContext *self, int objSlot, int propIndex, int destSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, objSlot);
    NDSValue *dest = NDSContext_slotPointer(self, destSlot);
    if (!v->as.object){
        *dest = NDSValue_Nothing;
        return;
    }
    *dest = NDSObject_getProperty(v->as.object, propIndex);
}

void
NDSContext_setObjectProperty(NDSContext *self, int objSlot, int propIndex, int srcSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, objSlot);
    NDSValue *src = NDSContext_slotPointer(self, srcSlot);
    if (!v->as.object)
        return;
    NDSObject_setProperty(v->as.object, propIndex, *src);
}


/* Slot API — lists */

NDSStatus
NDSContext_setSlotNewList(NDSContext *self, int slot)
{
    if (!List_new(self, NDSContext_slotPointer(self, slot))){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

size_t
NDSContext_getSlotListCount(const NDSContext *self, int listSlot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, listSlot);
    if (v->type != NDSValueType_List || !v->as.object)
        return 0;
    return ((List *)NDSObject_getExtraData(self, v->as.object))->count;
}

void
NDSContext_getSlotListElement(NDSContext *self, int listSlot, size_t index, int destSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, listSlot);
    NDSValue *dest = NDSContext_slotPointer(self, destSlot);
    if (v->type != NDSValueType_List || !v->as.object){
        *dest = NDSValue_Nothing;
        return;
    }
    *dest = List_get((List *)NDSObject_getExtraData(self, v->as.object), index);
}

void
NDSContext_setSlotListElement(NDSContext *self, int listSlot, size_t index, int srcSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, listSlot);
    NDSValue *src = NDSContext_slotPointer(self, srcSlot);
    if (v->type != NDSValueType_List || !v->as.object)
        return;
    List_set((List *)NDSObject_getExtraData(self, v->as.object), index, *src);
}

NDSStatus
NDSContext_insertSlotListElement(NDSContext *self, int listSlot, size_t index, int srcSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, listSlot);
    NDSValue *src = NDSContext_slotPointer(self, srcSlot);
    if (v->type != NDSValueType_List || !v->as.object){
        NDSContext_setError(self, "expected a list");
        return NDSStatus_Error;
    }
    if (!List_insert(self, v->as.object, index, *src)){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

NDSStatus
NDSContext_appendSlotListElement(NDSContext *self, int listSlot, int srcSlot)
{
    NDSValue *v = NDSContext_slotPointer(self, listSlot);
    NDSValue *src = NDSContext_slotPointer(self, srcSlot);
    if (v->type != NDSValueType_List || !v->as.object){
        NDSContext_setError(self, "expected a list");
        return NDSStatus_Error;
    }
    if (!List_append(self, v->as.object, *src)){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

void
NDSContext_removeSlotListElement(NDSContext *self, int listSlot, size_t index)
{
    NDSValue *v = NDSContext_slotPointer(self, listSlot);
    if (v->type != NDSValueType_List || !v->as.object)
        return;
    List_remove((List *)NDSObject_getExtraData(self, v->as.object), index);
}

/* Slot API — maps */

NDSStatus
NDSContext_setSlotNewMap(NDSContext *self, int slot)
{
    if (!NDSContext_newObject(self, NDSValueType_Map, NDSContext_slotPointer(self, slot))){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

size_t
NDSContext_getSlotMapCount(const NDSContext *self, int mapSlot)
{
    const NDSValue *v = NDSContext_slotPointerConst(self, mapSlot);
    if (v->type != NDSValueType_Map || !v->as.object)
        return 0;
    return ((Map *)NDSObject_getExtraData(self, v->as.object))->count;
}

bool
NDSContext_getSlotMapValue(NDSContext *self, int mapSlot, int keySlot, int destSlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kv = NDSContext_slotPointer(self, keySlot);
    NDSValue *dest = NDSContext_slotPointer(self, destSlot);
    if (mv->type != NDSValueType_Map || !mv->as.object){
        *dest = NDSValue_Nothing;
        return false;
    }
    NDSValue result = {0};
    if (Map_get(self, mv->as.object, *kv, &result)){
        *dest = result;
        return true;
    }
    *dest = NDSValue_Nothing;
    return false;
}

NDSStatus
NDSContext_setSlotMapValue(NDSContext *self, int mapSlot, int keySlot, int valueSlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kv = NDSContext_slotPointer(self, keySlot);
    NDSValue *vv = NDSContext_slotPointer(self, valueSlot);
    if (mv->type != NDSValueType_Map || !mv->as.object){
        NDSContext_setError(self, "expected a map");
        return NDSStatus_Error;
    }
    if (Map_set(self, mv->as.object, *kv, *vv) < 0){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSStatus_OK;
}

bool
NDSContext_removeSlotMapValue(NDSContext *self, int mapSlot, int keySlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kv = NDSContext_slotPointer(self, keySlot);
    if (mv->type != NDSValueType_Map || !mv->as.object)
        return false;
    return Map_remove(self, mv->as.object, *kv);
}

bool
NDSContext_getSlotMapContainsKey(NDSContext *self, int mapSlot, int keySlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kv = NDSContext_slotPointer(self, keySlot);
    if (mv->type != NDSValueType_Map || !mv->as.object)
        return false;
    return Map_get(self, mv->as.object, *kv, NULL);
}

bool
NDSContext_getSlotMapEntry(NDSContext *self, int mapSlot, size_t index, int keySlot, int valueSlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kd = NDSContext_slotPointer(self, keySlot);
    NDSValue *vd = NDSContext_slotPointer(self, valueSlot);
    if (mv->type != NDSValueType_Map || !mv->as.object){
        *kd = NDSValue_Nothing;
        *vd = NDSValue_Nothing;
        return false;
    }
    Map *map = (Map *)NDSObject_getExtraData(self, mv->as.object);
    if (index >= map->count){
        *kd = NDSValue_Nothing;
        *vd = NDSValue_Nothing;
        return false;
    }
    /* Skip tombstones (gaps left by deletions) in the dense arrays */
    size_t seen = 0;
    for (size_t i = 0; i < map->used; i++){
        if (map->keys[i].type == NDSValueType_Nothing)
            continue;
        if (seen == index){
            *kd = map->keys[i];
            *vd = map->values[i];
            return true;
        }
        seen++;
    }
    *kd = NDSValue_Nothing;
    *vd = NDSValue_Nothing;
    return false;
}

bool
NDSContext_getNextSlotMapEntry(NDSContext *self, int mapSlot, NDSMapIterCookie *cookie, int keySlot, int valueSlot)
{
    NDSValue *mv = NDSContext_slotPointer(self, mapSlot);
    NDSValue *kd = NDSContext_slotPointer(self, keySlot);
    NDSValue *vd = NDSContext_slotPointer(self, valueSlot);
    if (mv->type != NDSValueType_Map || !mv->as.object){
        *kd = NDSValue_Nothing;
        *vd = NDSValue_Nothing;
        return false;
    }
    Map *map = (Map *)NDSObject_getExtraData(self, mv->as.object);
    for (size_t i = *cookie; i < map->used; i++){
        if (map->keys[i].type != NDSValueType_Nothing){
            *kd = map->keys[i];
            *vd = map->values[i];
            *cookie = i + 1;
            return true;
        }
    }
    *kd = NDSValue_Nothing;
    *vd = NDSValue_Nothing;
    return false;
}


typedef enum{
    Token_EOF,
    Token_LexError,
    Token_Newline,
    Token_Number,
    Token_String,
    Token_Word,

    Token_Ampersand,
    Token_Colon,
    Token_Comma,
    Token_LeftBrace,
    Token_LeftBracket,
    Token_LeftParen,
    Token_Minus,
    Token_Plus,
    Token_RightBrace,
    Token_RightBracket,
    Token_RightParen,
    Token_Slash,
    Token_Star,

    Token_Eq,
    Token_Neq,
    Token_Lt,
    Token_Gt,
    Token_LtEq,
    Token_GtEq,

    Token_And,
    Token_Append,
    Token_As,
    Token_Block,
    Token_By,
    Token_Contains,
    Token_Copy,
    Token_Decrement,
    Token_Delete,
    Token_Div,
    Token_Do,
    Token_Each,
    Token_Else,
    Token_End,
    Token_Error,
    Token_Every,
    Token_Exit,
    Token_False,
    Token_First,
    Token_For,
    Token_From,
    Token_Function,
    Token_Given,
    Token_If,
    Token_Import,
    Token_In,
    Token_Increment,
    Token_Is,
    Token_Last,
    Token_Mod,
    Token_My,
    Token_Next,
    Token_Not,
    Token_Nothing,
    Token_Of,
    Token_On,
    Token_Or,
    Token_Raise,
    Token_Repeat,
    Token_Return,
    Token_Set,
    Token_Then,
    Token_To,
    Token_True,
    Token_Where,
    Token_While,
    Token_With
} TokenType;

typedef struct{
    const char *word;
    size_t length;
    TokenType type;
} KeywordEntry;

#define KW(w, t){ (w), sizeof(w) - 1, (t) }

/* clang-format off */
static const KeywordEntry keywordTable[] ={
    KW("and",         Token_And),
    KW("append",      Token_Append),
    KW("as",          Token_As),
    KW("block",       Token_Block),
    KW("by",          Token_By),
    KW("contains",    Token_Contains),
    KW("copy",        Token_Copy),
    KW("decrement",   Token_Decrement),
    KW("delete",      Token_Delete),
    KW("div",         Token_Div),
    KW("do",          Token_Do),
    KW("each",        Token_Each),
    KW("else",        Token_Else),
    KW("end",         Token_End),
    KW("error",       Token_Error),
    KW("every",       Token_Every),
    KW("exit",        Token_Exit),
    KW("false",       Token_False),
    KW("first",       Token_First),
    KW("for",         Token_For),
    KW("from",        Token_From),
    KW("function",    Token_Function),
    KW("given",       Token_Given),
    KW("if",          Token_If),
    KW("import",      Token_Import),
    KW("in",          Token_In),
    KW("increment",   Token_Increment),
    KW("is",          Token_Is),
    KW("last",        Token_Last),
    KW("mod",         Token_Mod),
    KW("my",          Token_My),
    KW("next",        Token_Next),
    KW("not",         Token_Not),
    KW("nothing",     Token_Nothing),
    KW("of",          Token_Of),
    KW("on",          Token_On),
    KW("or",          Token_Or),
    KW("raise",       Token_Raise),
    KW("repeat",      Token_Repeat),
    KW("return",      Token_Return),
    KW("set",         Token_Set),
    KW("then",        Token_Then),
    KW("to",          Token_To),
    KW("true",        Token_True),
    KW("where",       Token_Where),
    KW("while",       Token_While),
    KW("with",        Token_With)
};
/* clang-format on */

#undef KW

#define KEYWORD_COUNT (sizeof(keywordTable) / sizeof(keywordTable[0]))

static bool
NDSContext_registerKeywordAtoms(NDSContext *ctx)
{
    for (size_t i = 0; i < KEYWORD_COUNT; i++){
        if (NDSContext_internAtom(ctx, keywordTable[i].word) == NDSAtom_Invalid)
            return false;
    }
    return true;
}

typedef struct{
    TokenType type;
    uint8_t wordFlags;
    size_t line;
    size_t column;
    union{
        double number;
        struct{
            const char *data;
            size_t length;
        } text;
    } value;
} Token;

/* Locale-independent, encoding-independent lookup table. */
#define CC_ALPHA 1u
#define CC_DIGIT 2u
#define CC_UNDER 4u
static uint8_t CharClass_table[256];

static void
CharClass_init(void)
{
    static bool initialized = false;
    if (initialized)
        return;
    memset(CharClass_table, 0, sizeof(CharClass_table));
    const char *p = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    while (*p)
        CharClass_table[(unsigned char)*p++] |= CC_ALPHA;

    const char *q = "0123456789";
    while (*q)
        CharClass_table[(unsigned char)*q++] |= CC_DIGIT;
    CharClass_table[(unsigned char)'_'] |= CC_UNDER;
    initialized = true;
}

static bool
CharClass_isDigit(int c)
{
    return (CharClass_table[(unsigned char)c] & CC_DIGIT) != 0;
}
static bool
CharClass_isWordStart(int c)
{
    return (CharClass_table[(unsigned char)c] & (CC_ALPHA | CC_UNDER)) != 0;
}
static bool
CharClass_isWord(int c)
{
    return (CharClass_table[(unsigned char)c] & (CC_ALPHA | CC_DIGIT | CC_UNDER)) != 0;
}

static bool
CharClass_isHSpace(int c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

static int
CharClass_toLower(int c)
{
    return tolower((unsigned char)c);
}

#define TOKENIZER_BUF_SIZE 4096
#define TOKENIZER_TEXT_INITIAL 256
#define TOKENIZER_TEXT_MAX (1u << 16) /* 65536 — caps identifier/string token length */
#define TOKENIZER_MAX_ERROR 127
typedef struct Tokenizer Tokenizer;
struct Tokenizer{
    NDSContext *ctx;
    NDSAllocator *allocator;
    NDSReader *reader;

    char buf[TOKENIZER_BUF_SIZE];
    size_t bufLen;
    size_t bufPos;
    bool atEOF;

    char *text;
    size_t textLen;
    size_t textCap;
    bool textError;

    size_t line;
    size_t column;

    char errorBuf[TOKENIZER_MAX_ERROR + 1];
};

typedef struct{
    NDSReader base;
    NDSAllocator *allocator;
    char *data;
    size_t length;
    size_t position;
} StringReader;

static size_t
StringReader_read(NDSReader *self, void *buf, size_t count)
{
    StringReader *sr = (StringReader *)self;
    if (sr->position >= sr->length)
        return NDSReader_EOF;
    size_t remaining = sr->length - sr->position;
    if (count > remaining)
        count = remaining;
    memcpy(buf, sr->data + sr->position, count);
    sr->position += count;
    return count;
}

static void
StringReader_destroy(NDSReader *self)
{
    StringReader *sr = (StringReader *)self;
    NDSAllocator *alloc = sr->allocator;
    NDSAllocator_free(alloc, sr->data);
    NDSAllocator_free(alloc, sr);
}

NDSReader *
NDSContext_newStringReader(NDSContext *self, const char *data, size_t length)
{
    NDSAllocator *alloc = self->allocator;
    StringReader *sr = NDSAllocator_allocateZeroed(alloc, sizeof(StringReader));
    if (!sr)
        return NULL;
    sr->base.read = StringReader_read;
    sr->base.destroy = StringReader_destroy;
    sr->allocator = alloc;
    sr->data = NDSAllocator_allocate(alloc, length);
    if (!sr->data){
        NDSAllocator_free(alloc, sr);
        return NULL;
    }
    memcpy(sr->data, data, length);
    sr->length = length;
    sr->position = 0;
    return &sr->base;
}

typedef struct{
    NDSReader base;
    NDSAllocator *allocator;
    FILE *file;
} FileReader;

static size_t
FileReader_read(NDSReader *self, void *buf, size_t count)
{
    FileReader *fr = (FileReader *)self;
    size_t n = fread(buf, 1, count, fr->file);
    if (n == 0)
        return feof(fr->file)? NDSReader_EOF : NDSReader_Error;
    return n;
}

static void
FileReader_destroy(NDSReader *self)
{
    FileReader *fr = (FileReader *)self;
    NDSAllocator *alloc = fr->allocator;
    if (fr->file)
        fclose(fr->file);
    NDSAllocator_free(alloc, fr);
}

NDSReader *
NDSContext_newFileReader(NDSContext *self, const char *fileName)
{
    FILE *f = fopen(fileName, "rb");
    if (!f)
        return NULL;
    NDSAllocator *alloc = self->allocator;
    FileReader *fr = NDSAllocator_allocateZeroed(alloc, sizeof(FileReader));
    if (!fr){
        fclose(f);
        return NULL;
    }
    fr->base.read = FileReader_read;
    fr->base.destroy = FileReader_destroy;
    fr->allocator = alloc;
    fr->file = f;
    return &fr->base;
}

size_t
NDSReader_read(NDSReader *self, void *buffer, size_t count)
{
    return self->read(self, buffer, count);
}

void
NDSReader_destroy(NDSReader *self)
{
    if (self && self->destroy)
        self->destroy(self);
}

static bool
Tokenizer_init(Tokenizer *self, NDSContext *ctx, NDSAllocator *allocator, NDSReader *reader)
{
    memset(self, 0, sizeof(Tokenizer));
    self->ctx = ctx;
    self->allocator = allocator;
    self->reader = reader;
    self->line = 1;
    self->column = 1;
    self->textCap = TOKENIZER_TEXT_INITIAL;
    self->text = NDSAllocator_allocate(allocator, TOKENIZER_TEXT_INITIAL);
    if (!self->text)
        return false;
    self->text[0] = '\0';
    
    return true;
}

static void
Tokenizer_cleanup(Tokenizer *self)
{
    if (self->text)
        NDSAllocator_free(self->allocator, self->text);
    self->text = NULL;
    self->textLen = 0;
    self->textCap = 0;
}

static void
Tokenizer_refill(Tokenizer *self)
{
    if (self->atEOF)
        return;
    /* Move remaining data to front */
    if (self->bufPos > 0 && self->bufPos < self->bufLen){
        size_t remaining = self->bufLen - self->bufPos;
        memmove(self->buf, self->buf + self->bufPos, remaining);
        self->bufLen = remaining;
        self->bufPos = 0;
    } else if (self->bufPos >= self->bufLen){
        self->bufLen = 0;
        self->bufPos = 0;
    }
    /* Fill rest of buffer */
    size_t space = TOKENIZER_BUF_SIZE - self->bufLen;
    if (space > 0){
        size_t n = NDSReader_read(self->reader, self->buf + self->bufLen, space);
        if (n == NDSReader_EOF || n == NDSReader_Error)
            self->atEOF = true;
        else
            self->bufLen += n;
    }
}

static int
Tokenizer_peek(Tokenizer *self)
{
    if (self->bufPos >= self->bufLen){
        Tokenizer_refill(self);
        if (self->bufPos >= self->bufLen)
            return '\0';
    }
    return (unsigned char)self->buf[self->bufPos];
}

static void
Tokenizer_advance(Tokenizer *self)
{
    if (self->bufPos >= self->bufLen){
        Tokenizer_refill(self);
        if (self->bufPos >= self->bufLen)
            return;
    }
    if (self->buf[self->bufPos] == '\n'){
        self->line++;
        self->column = 1;
    } else
        self->column++;
    self->bufPos++;
}


static void
Tokenizer_textReset(Tokenizer *self)
{
    self->textLen = 0;
}

static void
Tokenizer_textPush(Tokenizer *self, char c)
{
    if (self->textLen + 1 >= TOKENIZER_TEXT_MAX){
        self->textError = true;
        return;
    }
    if (self->textLen + 1 >= self->textCap){
        size_t newCap;
        if (!Util_safeMulSize(self->textCap, 2, &newCap)){
            self->textError = true;
            return;
        }
        if (newCap > TOKENIZER_TEXT_MAX)
            newCap = TOKENIZER_TEXT_MAX;
        char *newBuf = NDSAllocator_reallocate(self->allocator, self->text, newCap);
        if (!newBuf){
            self->textError = true;
            return;
        }
        self->text = newBuf;
        self->textCap = newCap;
    }
    self->text[self->textLen++] = c;
    self->text[self->textLen] = '\0';
}

static void
Tokenizer_skipWhitespace(Tokenizer *self)
{
    for (;;){
        int c = Tokenizer_peek(self);
        if (CharClass_isHSpace(c)){
            Tokenizer_advance(self);
            continue;
        }
        /* Check for -- comment */
        if (c == '-'){
            if (self->bufLen - self->bufPos < 2)
                Tokenizer_refill(self);
            if (self->bufPos + 1 < self->bufLen && self->buf[self->bufPos + 1] == '-'){
                /* Skip to end of line (but not the newline itself) */
                Tokenizer_advance(self);
                Tokenizer_advance(self);
                while (Tokenizer_peek(self) != '\0' && Tokenizer_peek(self) != '\n')
                    Tokenizer_advance(self);
                continue;
            }
        }
        break;
    }
}

static Token Tokenizer_errorToken(Tokenizer *self, const char *message);

static Token
Tokenizer_scanNumber(Tokenizer *self)
{
    Token tok = {0};
    char buf[NUMERIC_MAX_LEN + 1] = {0};
    size_t count = 0;

    tok.type = Token_Number;
    tok.line = self->line;
    tok.column = self->column;
    
    /* Skip leading zeros. */
    while (Tokenizer_peek(self) == '0')
        Tokenizer_advance(self);

    /* Integer part */
    while (CharClass_isDigit(Tokenizer_peek(self)) && count < NUMERIC_MAX_LEN){
        buf[count++] = Tokenizer_peek(self);
        Tokenizer_advance(self);
    }
    if (count == 0)
        buf[count++] = '0';
    if (count > DBL_MAX_10_EXP + 1 || count >= NUMERIC_MAX_LEN)
        goto error;

    /* Fractional part */
    if (Tokenizer_peek(self) == '.'){
        buf[count++] = '.';
        Tokenizer_advance(self);

        while (CharClass_isDigit(Tokenizer_peek(self))){
            if (count >= NUMERIC_MAX_LEN)
                goto error;
            buf[count++] = Tokenizer_peek(self);
            Tokenizer_advance(self);
        }
    }

    /* Exponent */
    if (Tokenizer_peek(self) == 'e' || Tokenizer_peek(self) == 'E'){
        if (count >= NUMERIC_MAX_LEN)
            goto error;
        buf[count++] = 'e';
        Tokenizer_advance(self);

        if (Tokenizer_peek(self) == '+' || Tokenizer_peek(self) == '-'){
            if (count >= NUMERIC_MAX_LEN)
                goto error;
            buf[count++] = Tokenizer_peek(self);
            Tokenizer_advance(self);
        }

        while (CharClass_isDigit(Tokenizer_peek(self))){
            if (count >= NUMERIC_MAX_LEN)
                goto error;
            buf[count++] = Tokenizer_peek(self);
            Tokenizer_advance(self);
        }
    }
    buf[count] = 0;

    if (!Util_parseDouble(self->ctx, buf, count, &tok.value.number))
        goto error;
    return tok;

error:
    return Tokenizer_errorToken(self, "invalid numeric literal");
}

static Token
Tokenizer_scanString(Tokenizer *self)
{
    Token tok = {0};
    tok.type = Token_String;
    tok.line = self->line;
    tok.column = self->column;

    char quote = (char)Tokenizer_peek(self);
    Tokenizer_advance(self); /* skip opening quote */

    Tokenizer_textReset(self);

    for (;;){
        int c = Tokenizer_peek(self);
        if (c == '\0' || c == '\n' || c == '\r')
            return Tokenizer_errorToken(self, "unterminated string literal");
        if (c == (int)quote){
            Tokenizer_advance(self); /* skip closing quote */
            break;
        }
        Tokenizer_textPush(self, (char)c);
        Tokenizer_advance(self);
    }

    tok.value.text.data = self->text;
    tok.value.text.length = self->textLen;
    return tok;
}

static TokenType
Tokenizer_lookupKeyword(const char *lower, size_t length)
{
    size_t lo = 0, hi = KEYWORD_COUNT;
    while (lo < hi){
        size_t mid = lo + (hi - lo) / 2;
        const KeywordEntry *e = &keywordTable[mid];
        size_t minLen = length < e->length? length : e->length;
        int cmp = memcmp(lower, e->word, minLen);
        if (cmp == 0)
            cmp = (length > e->length) - (length < e->length);
        if (cmp < 0)
            hi = mid;
        else if (cmp > 0)
            lo = mid + 1;
        else
            return e->type;
    }
    return Token_Word;
}

static NDSChunkID
NDSConfigHandle_registerChunkID_impl(NDSContext *ctx, const char *name)
{
    size_t len = strlen(name);
    if (len == 0 || len > NDSMaxRegisteredNameLength)
        return NDSChunkID_Invalid;

    char lower[NDSMaxRegisteredNameLength + 1];
    Util_lowercaseName(name, len, lower);

    /* Reject keywords */
    if (Tokenizer_lookupKeyword(lower, len) != Token_Word)
        return NDSChunkID_Invalid;

    NDSAtom atom = NDSContext_internAtom(ctx, lower);
    if (atom == NDSAtom_Invalid)
        return NDSChunkID_Invalid;
    ctx->wordFlags[atom] |= TokenWord_ChunkID;
    return (NDSChunkID)atom;
}

static Token
Tokenizer_scanWord(Tokenizer *self)
{
    Token tok = {0};
    tok.line = self->line;
    tok.column = self->column;

    Tokenizer_textReset(self);

    while (CharClass_isWord(Tokenizer_peek(self))){
        Tokenizer_textPush(self, (char)Tokenizer_peek(self));
        Tokenizer_advance(self);
    }

    /* Build lowercase copy for keyword and atom lookup.
       Words longer than NDSMaxRegisteredNameLength can't be keywords
       or registered names, so skip lookup. */
    char lower[NDSMaxRegisteredNameLength + 1] = {0};
    TokenType kwType = Token_Word;
    if (self->textLen <= NDSMaxRegisteredNameLength){
        for (size_t i = 0; i < self->textLen; i++)
            lower[i] = (char)CharClass_toLower(self->text[i]);
        lower[self->textLen] = '\0';
        kwType = Tokenizer_lookupKeyword(lower, self->textLen);
    }

    if (kwType != Token_Word){
        tok.type = kwType;
        tok.value.text.data = self->text;
        tok.value.text.length = self->textLen;
        /* Look up atom to get wordFlags (for host command pattern words) */
        NDSAtom kwAtom = NDSContext_findAtom(self->ctx, lower, self->textLen);
        if (kwAtom != NDSAtom_Invalid)
            tok.wordFlags = self->ctx->wordFlags[kwAtom];
        return tok;
    }

    /* Regular word — look up classification */
    tok.type = Token_Word;
    tok.wordFlags = 0;
    tok.value.text.data = self->text;
    tok.value.text.length = self->textLen;

    /* Check context word classification table */
    if (self->textLen <= NDSMaxRegisteredNameLength){
        NDSAtom found = NDSContext_findAtom(self->ctx, lower, self->textLen);
        if (found != NDSAtom_Invalid)
            tok.wordFlags = self->ctx->wordFlags[found];
    }

    return tok;
}

static Token
Tokenizer_errorToken(Tokenizer *self, const char *message)
{
    snprintf(self->errorBuf, TOKENIZER_MAX_ERROR, "%s", message);
    Token tok = {0};
    tok.type = Token_LexError;
    tok.line = self->line;
    tok.column = self->column;
    return tok;
}

static Token
Tokenizer_next(Tokenizer *self)
{
    Token tok = {0};

    if (self->textError)
        return Tokenizer_errorToken(self, "token too long or out of memory");

    Tokenizer_skipWhitespace(self);

    tok = (Token){0};
    tok.line = self->line;
    tok.column = self->column;

    int c = Tokenizer_peek(self);

    if (c == '\0'){
        tok.type = Token_EOF;
        return tok;
    }

    if (c == '\n'){
        Tokenizer_advance(self);
        tok.type = Token_Newline;
        return tok;
    }

    if (CharClass_isDigit(c))
        return Tokenizer_scanNumber(self);

    if (c == '"' || c == '\'')
        return Tokenizer_scanString(self);

    if (CharClass_isWordStart(c))
        return Tokenizer_scanWord(self);

    /* Single and double character operators */
    Tokenizer_advance(self);
    Tokenizer_textReset(self);
    Tokenizer_textPush(self, (char)c);

#define SINGLE_OP(ch, type_)       case ch: tok.type = type_; break;
#define DOUBLE_OP(ch, type1, ch2, type2)                                                               \
    case ch:                                                                                           \
        if (Tokenizer_peek(self) == ch2){                                                              \
            Tokenizer_advance(self); Tokenizer_textPush(self, ch2); tok.type = type2;                  \
        } else tok.type = type1;                                                                       \
        break;

    switch (c){
    SINGLE_OP('+', Token_Plus)
    SINGLE_OP('-', Token_Minus)
    SINGLE_OP('*', Token_Star)
    SINGLE_OP('/', Token_Slash)
    SINGLE_OP('&', Token_Ampersand)
    SINGLE_OP(',', Token_Comma)
    SINGLE_OP(':', Token_Colon)
    SINGLE_OP('=', Token_Eq)
    SINGLE_OP('(', Token_LeftParen)
    SINGLE_OP('[', Token_LeftBracket)
    SINGLE_OP('{', Token_LeftBrace)
    SINGLE_OP(')', Token_RightParen)
    SINGLE_OP(']', Token_RightBracket)
    SINGLE_OP('}', Token_RightBrace)
    DOUBLE_OP('<', Token_Lt, '=', Token_LtEq)
    DOUBLE_OP('>', Token_Gt, '=', Token_GtEq)
    case '!':
        if (Tokenizer_peek(self) == '='){
            Tokenizer_advance(self); Tokenizer_textPush(self, '='); tok.type = Token_Neq;
        } else
            return Tokenizer_errorToken(self, "unexpected token");
        break;
    default:
        return Tokenizer_errorToken(self, "unexpected token");
    }

#undef SINGLE_OP
#undef DOUBLE_OP

    tok.value.text.data = self->text;
    tok.value.text.length = self->textLen;
    return tok;
}

typedef enum{
    Op_PushConst,
    Op_PushNothing,
    Op_Add,
    Op_Sub,
    Op_Mul,
    Op_Div,
    Op_IntDiv,
    Op_Mod,
    Op_Negate,
    Op_Concat,
    Op_Not,
    Op_Eq,
    Op_Neq,
    Op_Lt,
    Op_Gt,
    Op_LtEq,
    Op_GtEq,
    Op_Contains,
    Op_Cast,
    Op_Pop,
    Op_Dup,
    Op_Swap,
    Op_Jump,
    Op_JumpIfFalse,
    Op_JumpIfTrue,
    Op_LoadGlobal,
    Op_StoreGlobal,
    Op_LoadLocal,
    Op_StoreLocal,
    Op_ForCheck,
    Op_Call,          /* .a = arg count, .b = block info (NDS_NO_BLOCK or funcIdx); func on stack */
    Op_Return,
    Op_NewList,       /* inst.a = element count */
    Op_NewMap,        /* inst.a = entry count (pops 2*a values: key, value pairs) */
    Op_PropertyGet,   /* inst.a = property name atom; pop container, push result */
    Op_ChunkGet,      /* inst.a = chunkID atom; pop container, pop index, push result */
    Op_ChunkGetRange, /* inst.a = chunkID; pop container, pop end, pop start, push result */
    Op_ChunkGetEvery, /* inst.a = chunkID; pop container, push result list */
    Op_ChunkSet,      /* inst.a = chunkID; pop value, pop index, pop container → chunk_set */
    Op_ChunkDelete,   /* inst.a = chunkID; pop index, pop container → chunk_delete */
    Op_ListAppend,    /* pop value, peek list (stays on stack), append value to list */
    Op_Reserve,       /* .a = count; push N nothings (reserve local slots) */
    Op_MakeError,     /* pop value, message, type → push error */
    Op_Raise,         /* pop error value, set currentError, goto vm_error */
    Op_CallGiven,     /* .a = user arg count; invoke block with outer access */
    Op_BlockGiven,    /* push true if current frame has a block, false otherwise */
    Op_LoadOuter,     /* .a = slot in enclosing frame; push stack[closureBase + a] */
    Op_StoreOuter,    /* .a = slot in enclosing frame; pop value, store to stack[closureBase + a] */
    Op_ShallowCopy    /* pop value; if list/map, push shallow clone; else push as-is */
} OpCode;

struct Instruction{
    OpCode op;
    uint32_t a;
    uint32_t b;
};

typedef struct{
    Instruction *code;
    LineInfo *lines;
    size_t count;
    size_t capacity;
    NDSAllocator *allocator;
} InstructionBuffer;

#define INITIAL_CODE_CAPACITY 64

static void
InstructionBuffer_init(InstructionBuffer *self, NDSAllocator *allocator)
{
    self->code = NDSAllocator_allocate(allocator, INITIAL_CODE_CAPACITY * sizeof(Instruction));
    self->lines = NDSAllocator_allocate(allocator, INITIAL_CODE_CAPACITY * sizeof(LineInfo));
    self->count = 0;
    self->allocator = allocator;
    if (self->code && self->lines){
        self->capacity = INITIAL_CODE_CAPACITY;
    } else{
        NDSAllocator_free(allocator, self->code);
        NDSAllocator_free(allocator, self->lines);
        self->code = NULL;
        self->lines = NULL;
        self->capacity = 0;
    }
}

static size_t
InstructionBuffer_emit(InstructionBuffer *self, Instruction inst)
{
    if (self->count >= self->capacity){
        size_t newCap;
        if (self->capacity < 8)
            newCap = 16;
        else if (!Util_safeMulSize(self->capacity, 2, &newCap))
            return (size_t)-1;
        size_t codeSize, lineSize;
        if (!Util_safeMulSize(newCap, sizeof(Instruction), &codeSize) ||
            !Util_safeMulSize(newCap, sizeof(LineInfo), &lineSize))
            return (size_t)-1;
        Instruction *newCode = NDSAllocator_reallocate(self->allocator, self->code, codeSize);
        if (!newCode)
            return (size_t)-1;
        self->code = newCode;
        LineInfo *newLines = NDSAllocator_reallocate(self->allocator, self->lines, lineSize);
        if (!newLines)
            return (size_t)-1;
        self->lines = newLines;
        self->capacity = newCap;
    }
    size_t index = self->count++;
    self->code[index] = inst;
    self->lines[index] = (LineInfo){ 0, 0 };
    return index;
}

#define COMPILER_MAX_LOOKAHEAD 2
#define COMPILER_MAX_DEPTH 256
#define COMPILER_MAX_LOCALS 256       /* params share this pool */
#define COMPILER_MAX_LOOP_DEPTH 32
#define COMPILER_MAX_LOOP_FIXUPS 256

typedef struct{
    NDSAtom name;
    int scopeDepth;
} Local;

typedef enum{
    LoopKind_While,
    LoopKind_Repeat,
    LoopKind_For
} LoopKind;

typedef struct{
    LoopKind kind;
} LoopFrame;

typedef struct{
    size_t indices[COMPILER_MAX_LOOP_FIXUPS];
    int count;
} FixupList;

#define MAX_IMPORT_DEPTH 16

typedef struct ImportFrame ImportFrame;
struct ImportFrame{
    NDSReader *importReader; /* owned; destroyed on pop or error cleanup */
    NDSReader *outerReader;  /* saved outer reader (NOT owned here) */
    char *buf;               /* pool-allocated saved tokenizer buffer */
    size_t bufLen;
    size_t bufPos;
    bool atEOF;
    size_t line;
    size_t column;
    int nestingDepth;
    Token tokens[COMPILER_MAX_LOOKAHEAD]; /* saved outer compiler lookahead */
    int tokenStart;
    int tokenCount;
    uint32_t sourceAtom;
};

typedef struct{
    NDSContext *ctx;
    NDSAllocator *pool;
    Tokenizer tokenizer;
    Token tokens[COMPILER_MAX_LOOKAHEAD];
    int tokenStart;
    int tokenCount;
    InstructionBuffer code;
    jmp_buf errorJump;
    int depth;
    Local locals[COMPILER_MAX_LOCALS];
    int localCount;
    int maxLocalCount; /* high-water mark for local slots */
    int scopeDepth;
    int scopeBaseStack[COMPILER_MAX_LOCALS]; /* localCount at each pushScope */
    LoopFrame loops[COMPILER_MAX_LOOP_DEPTH];
    int loopCount;
    FixupList exitFixups;
    FixupList nextFixups;
    int errorLocalSlot; /* local slot of error variable in on-error handler, or -1 */
    NDSAtom outerLocals[COMPILER_MAX_LOCALS]; /* enclosing function's locals (for blocks) */
    int outerLocalCount;                      /* 0 when not compiling a block */
    Token previous;                           /* most recently consumed token */
    int nestingDepth;                         /* (), [], {} depth across buffered tokens */
    ImportFrame importFrames[MAX_IMPORT_DEPTH];
    size_t importFrameCount;
    uint32_t sourceAtom; /* atom for current source name */
} Compiler;

/* Parse-depth bookkeeping. PARSE_ENTER at the top, PARSE_LEAVE / PARSE_RETURN to exit. */
#define PARSE_ENTER                                                                                \
    do{                                                                                            \
        if (++self->depth > COMPILER_MAX_DEPTH)                                                    \
            Compiler_error(self, "?FORMULA TOO COMPLEX");                                          \
    } while (false)
#define PARSE_LEAVE                                                                                \
    do{                                                                                            \
        self->depth--;                                                                             \
        return;                                                                                    \
    } while (false)
#define PARSE_RETURN(v)                                                                            \
    do{                                                                                            \
        self->depth--;                                                                             \
        return (v);                                                                                \
    } while (false)

static const char *
Compiler_atomName(const Compiler *self, NDSAtom atom)
{
    return ((StringData *)NDSObject_getExtraData(self->ctx, self->ctx->atoms[atom].as.object))->data;
}

static void NDS_PRINTF(2, 3) Compiler_error(Compiler *self, const char *fmt, ...)
{
    NDSContext *ctx = self->ctx;
    if (self->tokenCount > 0){
        int idx = self->tokenStart % COMPILER_MAX_LOOKAHEAD;
        ctx->errorLine = self->tokens[idx].line;
    }
    ctx->errorSourceAtom = self->sourceAtom;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->errorMessage, sizeof(ctx->errorMessage), fmt, ap);
    va_end(ap);

    longjmp(self->errorJump, 1);
}

static void
Compiler_popReader(Compiler *self);

static void
Compiler_fillToken(Compiler *self)
{
    int savedCount = self->tokenCount;

    while (self->tokenCount < COMPILER_MAX_LOOKAHEAD){
        int idx = (self->tokenStart + self->tokenCount) % COMPILER_MAX_LOOKAHEAD;
        Token tok = Tokenizer_next(&self->tokenizer);

        /* Imported file EOF — pop back to outer reader */
        if (tok.type == Token_EOF && self->importFrameCount > 0){
            if (self->tokenCount == savedCount){
                Compiler_popReader(self);
                break;
            }
            break;
        }

        /* Newlines inside (), [], {} are whitespace. */
        if (tok.type == Token_Newline && self->nestingDepth > 0)
            continue;

        if (tok.type == Token_LeftParen || tok.type == Token_LeftBracket ||
            tok.type == Token_LeftBrace)
            self->nestingDepth++;
        else if ((tok.type == Token_RightParen || tok.type == Token_RightBracket ||
                  tok.type == Token_RightBrace) && self->nestingDepth > 0)
            self->nestingDepth--;

        self->tokens[idx] = tok;

        /* Lex errors are fatal */
        if (tok.type == Token_LexError)
            Compiler_error(self, "syntax error: %s", self->tokenizer.errorBuf);

        /* Copy text data into the pool so it survives tokenizer buffer reuse.
           Number tokens carry their value in the double field, not text. */
        Token *tp = &self->tokens[idx];
        if (tp->type != Token_Number && tp->type != Token_EOF && tp->type != Token_Newline &&
            tp->type != Token_LexError && tp->value.text.data && tp->value.text.length > 0){
            char *copy = NDSAllocator_allocate(self->pool, tp->value.text.length + 1);
            if (copy){
                memcpy(copy, tp->value.text.data, tp->value.text.length);
                copy[tp->value.text.length] = '\0';
                tp->value.text.data = copy;
            } else
                Compiler_error(self, "out of memory");
        }

        self->tokenCount++;
        if (tp->type == Token_EOF || tp->type == Token_Newline)
            break;
    }
}

static Token *
Compiler_current(Compiler *self)
{
    while (self->tokenCount == 0)
        Compiler_fillToken(self);
    return &self->tokens[self->tokenStart];
}

static Token *
Compiler_peek(Compiler *self, int n)
{
    while (self->tokenCount <= n && self->tokenCount < COMPILER_MAX_LOOKAHEAD)
        Compiler_fillToken(self);
    if (n >= self->tokenCount)
        return &self->tokens[(self->tokenStart + self->tokenCount - 1) % COMPILER_MAX_LOOKAHEAD];
    return &self->tokens[(self->tokenStart + n) % COMPILER_MAX_LOOKAHEAD];
}

static void
Compiler_advance(Compiler *self)
{
    if (self->tokenCount == 0)
        Compiler_fillToken(self);
    if (self->tokenCount > 0){
        self->previous = self->tokens[self->tokenStart];
        self->tokenStart = (self->tokenStart + 1) % COMPILER_MAX_LOOKAHEAD;
        self->tokenCount--;
    }
}

static bool
Compiler_check(Compiler *self, TokenType type)
{
    return Compiler_current(self)->type == type;
}

static bool
Compiler_match(Compiler *self, TokenType type)
{
    if (!Compiler_check(self, type))
        return false;
    Compiler_advance(self);
    return true;
}

static void
Compiler_consume(Compiler *self, TokenType type, const char *message)
{
    if (Compiler_current(self)->type == type){
        Compiler_advance(self);
        return;
    }
    Compiler_error(self, "%s", message);
}

static size_t
Compiler_emit(Compiler *self, OpCode op, uint32_t a, uint32_t b)
{
    Instruction inst;
    inst.op = op;
    inst.a = a;
    inst.b = b;
    size_t index = InstructionBuffer_emit(&self->code, inst);
    if (index == (size_t)-1)
        Compiler_error(self, "out of memory");
    if (self->tokenCount > 0){
        int idx = self->tokenStart % COMPILER_MAX_LOOKAHEAD;
        self->code.lines[index].source = self->sourceAtom;
        self->code.lines[index].line = (uint32_t)self->tokens[idx].line;
    }
    return index;
}


static void
Compiler_emitConstant(Compiler *self, NDSValue value)
{
    size_t index = NDSContext_addConstant(self->ctx, value);
    if (index == (size_t)-1)
        Compiler_error(self, "too many constants");
    Compiler_emit(self, Op_PushConst, (uint32_t)index, 0);
}

static void
Compiler_emitStringConstant(Compiler *self, const char *bytes, size_t length)
{
    size_t index = NDSContext_addStringConstant(self->ctx, bytes, length);
    if (index == (size_t)-1)
        Compiler_error(self, "out of memory");
    Compiler_emit(self, Op_PushConst, (uint32_t)index, 0);
}

static size_t
Compiler_emitJump(Compiler *self, OpCode op)
{
    return Compiler_emit(self, op, 0, 0);
}

static void
Compiler_patchJump(Compiler *self, size_t index)
{
    if (index == (size_t)-1)
        return;
    self->code.code[index].a = (uint32_t)self->code.count;
}

static bool
Compiler_init(Compiler *self, NDSContext *ctx, NDSReader *reader, uint32_t sourceAtom)
{
    memset(self, 0, sizeof(Compiler));
    self->ctx = ctx;
    self->errorLocalSlot = -1;
    self->sourceAtom = sourceAtom;
    self->pool = NDSPoolAllocator_new(ctx->allocator, 4096);
    if (!self->pool)
        return false;
    if (!Tokenizer_init(&self->tokenizer, ctx, self->pool, reader)){
        NDSAllocator_destroy(self->pool);
        self->pool = NULL;
        return false;
    }
    InstructionBuffer_init(&self->code, self->pool);
    if (!self->code.code){
        Tokenizer_cleanup(&self->tokenizer);
        NDSAllocator_destroy(self->pool);
        self->pool = NULL;
        return false;
    }
    /* Caller must run Compiler_fillToken after setjmp; it can lex-error.
     * Ask me how I know.
     */
    return true;
}

static void
Compiler_cleanup(Compiler *self)
{
    /* Destroy any open import readers (may be left on error/longjmp) */
    for (size_t i = 0; i < self->importFrameCount; i++)
        NDSReader_destroy(self->importFrames[i].importReader);
    self->importFrameCount = 0;

    /* The pool owns the tokenizer text buffer and instruction buffer.
       Destroying it frees all ephemeral compilation allocations. */
    if (self->pool){
        NDSAllocator_destroy(self->pool);
        self->pool = NULL;
    }
    self->tokenizer.text = NULL;
    self->code.code = NULL;
}

static void
Compiler_pushReader(Compiler *self, NDSReader *importReader)
{
    if (self->importFrameCount >= MAX_IMPORT_DEPTH){
        NDSReader_destroy(importReader);
        Compiler_error(self, "import nesting too deep");
    }

    ImportFrame *frame = &self->importFrames[self->importFrameCount];
    self->importFrameCount++;

    frame->importReader = importReader;

    /* Save outer reader pointer */
    frame->outerReader = self->tokenizer.reader;

    /* Save tokenizer buffer to pool allocation */
    frame->buf = NDSAllocator_allocate(self->pool, self->tokenizer.bufLen);
    if (!frame->buf && self->tokenizer.bufLen > 0)
        Compiler_error(self, "out of memory");
    if (self->tokenizer.bufLen > 0)
        memcpy(frame->buf, self->tokenizer.buf, self->tokenizer.bufLen);
    frame->bufLen = self->tokenizer.bufLen;
    frame->bufPos = self->tokenizer.bufPos;
    frame->atEOF = self->tokenizer.atEOF;
    frame->line = self->tokenizer.line;
    frame->column = self->tokenizer.column;
    frame->nestingDepth = self->nestingDepth;

    /* Save compiler lookahead */
    memcpy(frame->tokens, self->tokens, COMPILER_MAX_LOOKAHEAD * sizeof(Token));
    frame->tokenStart = self->tokenStart;
    frame->tokenCount = self->tokenCount;
    frame->sourceAtom = self->sourceAtom;

    /* Install new reader, reset tokenizer state */
    self->tokenizer.reader = importReader;
    self->tokenizer.bufLen = 0;
    self->tokenizer.bufPos = 0;
    self->tokenizer.atEOF = false;
    self->tokenizer.line = 1;
    self->tokenizer.column = 1;
    self->nestingDepth = 0;

    /* Clear compiler lookahead and prime from imported reader */
    self->tokenStart = 0;
    self->tokenCount = 0;
    Compiler_fillToken(self);
}

static void
Compiler_popReader(Compiler *self)
{
    self->importFrameCount--;
    ImportFrame *frame = &self->importFrames[self->importFrameCount];

    /* Destroy the import reader */
    NDSReader_destroy(frame->importReader);
    frame->importReader = NULL;

    /* Restore tokenizer state */
    self->tokenizer.reader = frame->outerReader;
    if (frame->bufLen > 0)
        memcpy(self->tokenizer.buf, frame->buf, frame->bufLen);
    self->tokenizer.bufLen = frame->bufLen;
    self->tokenizer.bufPos = frame->bufPos;
    self->tokenizer.atEOF = frame->atEOF;
    self->tokenizer.line = frame->line;
    self->tokenizer.column = frame->column;
    self->nestingDepth = frame->nestingDepth;

    /* Restore compiler lookahead */
    memcpy(self->tokens, frame->tokens, COMPILER_MAX_LOOKAHEAD * sizeof(Token));
    self->tokenStart = frame->tokenStart;
    self->tokenCount = frame->tokenCount;
    self->sourceAtom = frame->sourceAtom;
}

static NDSAtom
Compiler_internName(Compiler *self, const char *name, size_t nameLen)
{
    char lower[NDSMaxRegisteredNameLength + 1];
    Util_lowercaseName(name, nameLen, lower);

    NDSAtom atom = NDSContext_internAtom(self->ctx, lower);
    if (atom == NDSAtom_Invalid)
        Compiler_error(self, "out of memory");
    return atom;
}

static void
Compiler_pushScope(Compiler *self)
{
    self->scopeBaseStack[self->scopeDepth] = self->localCount;
    self->scopeDepth++;
}

static void
Compiler_popScope(Compiler *self)
{
    if (self->localCount > self->maxLocalCount)
        self->maxLocalCount = self->localCount;
    self->scopeDepth--;
    self->localCount = self->scopeBaseStack[self->scopeDepth];
}

static int
Compiler_addLocal(Compiler *self, NDSAtom name)
{
    if (self->localCount >= COMPILER_MAX_LOCALS)
        Compiler_error(self, "too many local variables");

    int slot = self->localCount;
    self->locals[slot].name = name;
    self->locals[slot].scopeDepth = self->scopeDepth;
    self->localCount++;
    return slot;
}

static int
Compiler_addAnonymousLocal(Compiler *self)
{
    return Compiler_addLocal(self, NDSAtom_Invalid);
}

static int
Compiler_resolveLocal(Compiler *self, NDSAtom name)
{
    for (int i = self->localCount - 1; i >= 0; i--){
        if (self->locals[i].name == name)
            return i;
    }
    return -1;
}

static int
Compiler_resolveOuter(Compiler *self, NDSAtom name)
{
    for (int i = self->outerLocalCount - 1; i >= 0; i--){
        if (self->outerLocals[i] == name)
            return i;
    }
    return -1;
}

typedef enum{
    VariableLocationKind_NotFound,
    VariableLocationKind_Local,
    VariableLocationKind_Outer,
    VariableLocationKind_Global
} VariableLocationKind;

typedef struct{
    VariableLocationKind kind;
    size_t slot;
} VariableLocation;

static VariableLocation
Compiler_resolveVar(Compiler *self, NDSAtom atom)
{
    int l = Compiler_resolveLocal(self, atom);
    if (l >= 0)
        return (VariableLocation){ VariableLocationKind_Local, (size_t)l };
    int o = Compiler_resolveOuter(self, atom);
    if (o >= 0)
        return (VariableLocation){ VariableLocationKind_Outer, (size_t)o };
    size_t g = Globals_find(self->ctx, atom);
    if (g != (size_t)-1)
        return (VariableLocation){ VariableLocationKind_Global, g };
    return (VariableLocation){ VariableLocationKind_NotFound, 0 };
}

static void
Compiler_emitLoadVarByAtom(Compiler *self, NDSAtom atom)
{
    VariableLocation v = Compiler_resolveVar(self, atom);
    switch (v.kind){
    case VariableLocationKind_Local:  Compiler_emit(self, Op_LoadLocal,  (uint32_t)v.slot, 0); return;
    case VariableLocationKind_Outer:  Compiler_emit(self, Op_LoadOuter,  (uint32_t)v.slot, 0); return;
    case VariableLocationKind_Global: Compiler_emit(self, Op_LoadGlobal, (uint32_t)v.slot, 0); return;
    case VariableLocationKind_NotFound:
        Compiler_error(self, "unknown variable '%s'", Compiler_atomName(self, atom));
    }
}

static void
Compiler_emitLoadVar(Compiler *self, const char *name, size_t nameLen)
{
    Compiler_emitLoadVarByAtom(self, Compiler_internName(self, name, nameLen));
}

/* Store to a variable: locals/outer/globals, or create in innermost scope.
   Value to store must already be on the stack. */
static void
Compiler_emitStoreVarByAtom(Compiler *self, NDSAtom atom)
{
    VariableLocation v = Compiler_resolveVar(self, atom);
    switch (v.kind){
    case VariableLocationKind_Local: Compiler_emit(self, Op_StoreLocal, (uint32_t)v.slot, 0); return;
    case VariableLocationKind_Outer: Compiler_emit(self, Op_StoreOuter, (uint32_t)v.slot, 0); return;
    case VariableLocationKind_Global:
        if (self->ctx->globals[v.slot].isConst)
            Compiler_error(self, "cannot assign to constant '%s'", Compiler_atomName(self, atom));
        Compiler_emit(self, Op_StoreGlobal, (uint32_t)v.slot, 0);
        return;
    case VariableLocationKind_NotFound: break;
    }

    /* Variable doesn't exist — create in innermost scope */
    if (self->scopeDepth > 0){
        int localSlot = Compiler_addLocal(self, atom);
        Compiler_emit(self, Op_StoreLocal, (uint32_t)localSlot, 0);
    } else{
        size_t globalSlot = Globals_ensure(self->ctx, atom);
        if (globalSlot == (size_t)-1)
            Compiler_error(self, "out of memory");
        Compiler_emit(self, Op_StoreGlobal, (uint32_t)globalSlot, 0);
    }
}

static void
Compiler_emitStoreVar(Compiler *self, const char *name, size_t nameLen)
{
    Compiler_emitStoreVarByAtom(self, Compiler_internName(self, name, nameLen));
}

static void
FixupList_add(Compiler *self, FixupList *list, size_t jumpIndex)
{
    if (list->count >= COMPILER_MAX_LOOP_FIXUPS)
        Compiler_error(self, "too many break/next statements");
    list->indices[list->count++] = jumpIndex;
}

static void
FixupList_patch(FixupList *list, Instruction *code, size_t target)
{
    for (int i = 0; i < list->count; i++)
        code[list->indices[i]].a = (uint32_t)target;
    list->count = 0;
}

static void
Compiler_pushLoop(Compiler *self, LoopKind kind)
{
    if (self->loopCount >= COMPILER_MAX_LOOP_DEPTH)
        Compiler_error(self, "too many nested loops");
    self->loops[self->loopCount].kind = kind;
    self->loopCount++;
}

static void
Compiler_popLoop(Compiler *self, size_t continueTarget, size_t exitTarget)
{
    assert(self->loopCount > 0);
    FixupList_patch(&self->nextFixups, self->code.code, continueTarget);
    FixupList_patch(&self->exitFixups, self->code.code, exitTarget);
    self->loopCount--;
}

typedef enum{
    Precedence_None       = 0,
    Precedence_Or         = 1,    /* or                            */
    Precedence_And        = 2,    /* and                           */
    Precedence_Not        = 3,    /* not (prefix only)             */
    Precedence_Comparison = 4,    /* = != < > <= >= contains is    */
    Precedence_Concat     = 5,    /* &                             */
    Precedence_AddSub     = 6,    /* + -                           */
    Precedence_MulDiv     = 7,    /* * / div mod                   */
    Precedence_Unary      = 8,    /* unary - (prefix only)         */
    Precedence_Cast       = 9,    /* as TYPE                       */
    Precedence_OfExpr     = 10    /* prepositional `of` (item ...) */
} Precedence;

typedef enum{
    StmtPhase_None    = 0,  /* not a statement head                 */
    StmtPhase_Initial = 1,  /* `my`                                 */
    StmtPhase_Middle  = 2,  /* set, if, while, host commands, expr  */
    StmtPhase_Final   = 3   /* return, raise, exit, next            */
} StmtPhase;

/* Returns true if the parselet left a value on the stack. */
typedef bool (*StmtParselet)(Compiler *self);
typedef void (*PrefixParselet)(Compiler *self);
typedef void (*InfixParselet)(Compiler *self);

typedef struct {
    StmtParselet   statement;
    StmtPhase      phase;
    PrefixParselet prefix;
    InfixParselet  infix;
    Precedence     infixPrec;
} GrammarRule;

static void Prefix_number(Compiler *self);
static void Prefix_string(Compiler *self);
static void Prefix_true(Compiler *self);
static void Prefix_false(Compiler *self);
static void Prefix_nothing(Compiler *self);
static void Prefix_grouping(Compiler *self);
static void Prefix_listLiteral(Compiler *self);
static void Prefix_mapLiteral(Compiler *self);
static void Prefix_givenCall(Compiler *self);
static void Prefix_blockTest(Compiler *self);
static void Prefix_word(Compiler *self);
static void Prefix_unaryMinus(Compiler *self);
static void Prefix_not(Compiler *self);
static void Prefix_every(Compiler *self);
static void Prefix_firstLast(Compiler *self);

static void Infix_binary(Compiler *self);
static void Infix_comparison(Compiler *self);
static void Infix_and(Compiler *self);
static void Infix_or(Compiler *self);
static void Infix_cast(Compiler *self);

static bool Stmt_my(Compiler *self);
static bool Stmt_set(Compiler *self);
static bool Stmt_copy(Compiler *self);
static bool Stmt_append(Compiler *self);
static bool Stmt_increment(Compiler *self);
static bool Stmt_decrement(Compiler *self);
static bool Stmt_delete(Compiler *self);
static bool Stmt_if(Compiler *self);
static bool Stmt_while(Compiler *self);
static bool Stmt_repeat(Compiler *self);
static bool Stmt_for(Compiler *self);
static bool Stmt_return(Compiler *self);
static bool Stmt_raise(Compiler *self);
static bool Stmt_exit(Compiler *self);
static bool Stmt_next(Compiler *self);
static bool Stmt_wordOrExpr(Compiler *self);

static const GrammarRule rules[] = {
    /* Literals and grouping */
    [Token_Number]      = { .prefix = Prefix_number },
    [Token_String]      = { .prefix = Prefix_string },
    [Token_True]        = { .prefix = Prefix_true },
    [Token_False]       = { .prefix = Prefix_false },
    [Token_Nothing]     = { .prefix = Prefix_nothing },
    [Token_LeftParen]   = { .prefix = Prefix_grouping },
    [Token_LeftBracket] = { .prefix = Prefix_listLiteral },
    [Token_LeftBrace]   = { .prefix = Prefix_mapLiteral },

    /* Identifiers and prepositional heads */
    [Token_Word]        = { .statement = Stmt_wordOrExpr, .phase = StmtPhase_Middle,
                            .prefix = Prefix_word },
    [Token_Every]       = { .prefix = Prefix_every },
    [Token_First]       = { .prefix = Prefix_firstLast },
    [Token_Last]        = { .prefix = Prefix_firstLast },
    [Token_Given]       = { .prefix = Prefix_givenCall },
    [Token_Block]       = { .prefix = Prefix_blockTest },

    /* Arithmetic */
    [Token_Plus]        = { .infix = Infix_binary, .infixPrec = Precedence_AddSub },
    [Token_Minus]       = { .prefix = Prefix_unaryMinus,
                            .infix = Infix_binary, .infixPrec = Precedence_AddSub },
    [Token_Star]        = { .infix = Infix_binary, .infixPrec = Precedence_MulDiv },
    [Token_Slash]       = { .infix = Infix_binary, .infixPrec = Precedence_MulDiv },
    [Token_Mod]         = { .infix = Infix_binary, .infixPrec = Precedence_MulDiv },
    [Token_Div]         = { .infix = Infix_binary, .infixPrec = Precedence_MulDiv },

    /* Concatenation */
    [Token_Ampersand]   = { .infix = Infix_binary, .infixPrec = Precedence_Concat },

    /* Comparison (non-associative; checked inside the parselet) */
    [Token_Eq]          = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_Neq]         = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_Lt]          = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_Gt]          = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_LtEq]        = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_GtEq]        = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_Is]          = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },
    [Token_Contains]    = { .infix = Infix_comparison, .infixPrec = Precedence_Comparison },

    /* Logical */
    [Token_Not]         = { .prefix = Prefix_not },
    [Token_And]         = { .infix = Infix_and, .infixPrec = Precedence_And },
    [Token_Or]          = { .infix = Infix_or,  .infixPrec = Precedence_Or },

    /* Cast */
    [Token_As]          = { .infix = Infix_cast, .infixPrec = Precedence_Cast },

    /* Statement heads */
    [Token_My]          = { .statement = Stmt_my,        .phase = StmtPhase_Initial },
    [Token_Set]         = { .statement = Stmt_set,       .phase = StmtPhase_Middle  },
    [Token_Copy]        = { .statement = Stmt_copy,      .phase = StmtPhase_Middle  },
    [Token_Append]      = { .statement = Stmt_append,    .phase = StmtPhase_Middle  },
    [Token_Increment]   = { .statement = Stmt_increment, .phase = StmtPhase_Middle  },
    [Token_Decrement]   = { .statement = Stmt_decrement, .phase = StmtPhase_Middle  },
    [Token_Delete]      = { .statement = Stmt_delete,    .phase = StmtPhase_Middle  },
    [Token_If]          = { .statement = Stmt_if,        .phase = StmtPhase_Middle  },
    [Token_While]       = { .statement = Stmt_while,     .phase = StmtPhase_Middle  },
    [Token_Repeat]      = { .statement = Stmt_repeat,    .phase = StmtPhase_Middle  },
    [Token_For]         = { .statement = Stmt_for,       .phase = StmtPhase_Middle  },
    [Token_Return]      = { .statement = Stmt_return,    .phase = StmtPhase_Final   },
    [Token_Raise]       = { .statement = Stmt_raise,     .phase = StmtPhase_Final   },
    [Token_Exit]        = { .statement = Stmt_exit,      .phase = StmtPhase_Final   },
    [Token_Next]        = { .statement = Stmt_next,      .phase = StmtPhase_Final   },
};
static const size_t rulesCount = sizeof(rules) / sizeof(rules[0]);

static const GrammarRule *
GrammarRule_get(TokenType type)
{
    static const GrammarRule empty = {0};
    if ((size_t)type >= rulesCount)
        return &empty;
    return &rules[type];
}

static void Compiler_parseMyDeclaration(Compiler *self);
static void Compiler_parseSetStatement(Compiler *self);
static void Compiler_parseCopyStatement(Compiler *self);
static void Compiler_parseAppendStatement(Compiler *self);
static void Compiler_parseIncrementOrDecrement(Compiler *self, OpCode op, const char *keyword);
static void Compiler_parseDeleteStatement(Compiler *self);
static void Compiler_parseIfStatement(Compiler *self);
static void Compiler_parseWhileStatement(Compiler *self);
static void Compiler_parseRepeatStatement(Compiler *self);
static void Compiler_parseForStatement(Compiler *self);
static void Compiler_parseReturnStatement(Compiler *self);
static void Compiler_parseRaiseStatement(Compiler *self);
static void Compiler_parseExitStatement(Compiler *self);
static void Compiler_parseNextStatement(Compiler *self);
static void Compiler_parseHostCommand(Compiler *self);
static void Compiler_consumeOfOrIn(Compiler *self, const char *context);
static bool Token_isHostHead(const Token *tok);

static void Compiler_parseExpression(Compiler *self);
static void Compiler_parsePrecedence(Compiler *self, Precedence minPrec);
static void Compiler_parseBlockContents(Compiler *self);
static void Compiler_parseBlock(Compiler *self);

static bool
Token_canStartPrimary(TokenType type)
{
    return type == Token_Number || type == Token_String || type == Token_LeftParen ||
           type == Token_Word || type == Token_Minus || type == Token_True || type == Token_False ||
           type == Token_Nothing;
}

/* Case-insensitive equality test for a Word's text. */
static bool
Token_textIs(const Token *tok, const char *text)
{
    if (tok->type != Token_Word)
        return false;
    size_t len = strlen(text);
    if (tok->value.text.length != len)
        return false;
    for (size_t i = 0; i < len; i++)
        if (CharClass_toLower((unsigned char)tok->value.text.data[i]) != CharClass_toLower((unsigned char)text[i]))
            return false;
    return true;
}

/* Parses a primary (literals, parens, list/map literals, variable load,
 * function call, given/block forms).
 */
static void
Compiler_parsePrimary(Compiler *self)
{
    PARSE_ENTER;
    Token *tok = Compiler_current(self);
    switch (tok->type){
    case Token_Number:      Prefix_number(self);      PARSE_LEAVE;
    case Token_String:      Prefix_string(self);      PARSE_LEAVE;
    case Token_True:        Prefix_true(self);        PARSE_LEAVE;
    case Token_False:       Prefix_false(self);       PARSE_LEAVE;
    case Token_Nothing:     Prefix_nothing(self);     PARSE_LEAVE;
    case Token_LeftParen:   Prefix_grouping(self);    PARSE_LEAVE;
    case Token_LeftBracket: Prefix_listLiteral(self); PARSE_LEAVE;
    case Token_LeftBrace:   Prefix_mapLiteral(self);  PARSE_LEAVE;
    case Token_Given:       Prefix_givenCall(self);   PARSE_LEAVE;
    case Token_Block:       Prefix_blockTest(self);   PARSE_LEAVE;
    default: break;
    }
    if (tok->type == Token_Word){
        if (Compiler_peek(self, 1)->type == Token_LeftParen){
            Prefix_word(self);
            PARSE_LEAVE;
        }
        Compiler_emitLoadVar(self, tok->value.text.data, tok->value.text.length);
        Compiler_advance(self);
        PARSE_LEAVE;
    }
    Compiler_error(self, "expected expression");
}

static void
Compiler_parseMaybeNegatedPrimary(Compiler *self)
{
    PARSE_ENTER;
    if (Compiler_match(self, Token_Minus)){
        Compiler_parsePrimary(self);
        Compiler_emit(self, Op_Negate, 0, 0);
        PARSE_LEAVE;
    }
    Compiler_parsePrimary(self);
    PARSE_LEAVE;
}

static void
Compiler_consumeOfOrIn(Compiler *self, const char *context)
{
    if (!Compiler_match(self, Token_Of) && !Compiler_match(self, Token_In))
        Compiler_error(self, "expected 'of' or 'in' %s", context);
}

static void
Compiler_emitReturnNothing(Compiler *self)
{
    Compiler_emit(self, Op_PushNothing, 0, 0);
    Compiler_emit(self, Op_Return, 0, 0);
}

static void
Compiler_parseGivenBlock(Compiler *self, size_t callIdx)
{
    PARSE_ENTER;
    /* Consume 'given' '(' paramNames ')' */
    Compiler_consume(self, Token_Given, "expected 'given'");
    Compiler_consume(self, Token_LeftParen, "expected '(' after 'given'");

    NDSAtom blockParams[COMPILER_MAX_LOCALS] = {0};
    int blockParamCount = 0;
    if (!Compiler_check(self, Token_RightParen)){
        do{
            if (!Compiler_check(self, Token_Word))
                Compiler_error(self, "expected parameter name");
            if (blockParamCount >= COMPILER_MAX_LOCALS)
                Compiler_error(self, "too many block parameters");
            Token *paramTok = Compiler_current(self);
            blockParams[blockParamCount] = Compiler_internName(self, paramTok->value.text.data, paramTok->value.text.length);
            blockParamCount++;
            Compiler_advance(self);
        } while (Compiler_match(self, Token_Comma));
    }
    Compiler_consume(self, Token_RightParen, "expected ')' after block parameters");

    /* Save enclosing locals for outer access */
    int enclosingLocalCount = self->localCount;
    NDSAtom enclosingNames[COMPILER_MAX_LOCALS] = {0};
    for (int i = 0; i < enclosingLocalCount; i++)
        enclosingNames[i] = self->locals[i].name;

    /* Save compiler state. self->locals must be saved too — the block body
       compiles anonymous locals into slots 0..N-1 and will otherwise leave
       them overwriting the enclosing function's local entries. */
    InstructionBuffer savedCode = self->code;
    int savedLocalCount = self->localCount;
    Local savedLocals[COMPILER_MAX_LOCALS];
    for (int i = 0; i < savedLocalCount; i++)
        savedLocals[i] = self->locals[i];
    int savedMaxLocalCount = self->maxLocalCount;
    int savedScopeDepth = self->scopeDepth;
    int savedLoopCount = self->loopCount;
    int savedOuterLocalCount = self->outerLocalCount;

    /* Fresh instruction buffer for block body */
    InstructionBuffer_init(&self->code, self->pool);
    if (!self->code.code)
        Compiler_error(self, "out of memory");
    self->localCount = 0;
    self->maxLocalCount = 0;
    self->scopeDepth = 1;
    self->loopCount = 0;

    /* Register ONLY user-declared block params as locals (slots 0..K-1) */
    for (int i = 0; i < blockParamCount; i++)
        Compiler_addLocal(self, blockParams[i]);

    /* Set up outer local access for the block body */
    self->outerLocalCount = enclosingLocalCount;
    for (int i = 0; i < enclosingLocalCount; i++)
        self->outerLocals[i] = enclosingNames[i];

    /* Emit Op_Reserve placeholder (patched after body) */
    size_t reserveIdx = Compiler_emit(self, Op_Reserve, 0, 0);

    /* Parse block body */
    Compiler_parseBlock(self);

    /* Patch Op_Reserve with actual non-param locals */
    if (self->localCount > self->maxLocalCount)
        self->maxLocalCount = self->localCount;
    int extraLocals = self->maxLocalCount - blockParamCount;
    if (extraLocals < 0)
        extraLocals = 0;
    self->code.code[reserveIdx].a = (uint32_t)extraLocals;

    /* Implicit return nothing at end */
    Compiler_emit(self, Op_PushNothing, 0, 0);
    Compiler_emit(self, Op_Return, 0, 0);

    /* Consume 'end' 'given' */
    Compiler_consume(self, Token_End, "expected 'end' after block body");
    Compiler_consume(self, Token_Given, "expected 'given' after 'end'");

    /* Copy bytecode and line info to context storage */
    size_t codeCount = self->code.count;
    Instruction *funcCode = NDSAllocator_allocate(self->ctx->allocator, codeCount * sizeof(Instruction));
    LineInfo *funcLines = NDSAllocator_allocate(self->ctx->allocator, codeCount * sizeof(LineInfo));
    if (!funcCode || !funcLines)
        Compiler_error(self, "out of memory");
    memcpy(funcCode, self->code.code, codeCount * sizeof(Instruction));
    memcpy(funcLines, self->code.lines, codeCount * sizeof(LineInfo));

    Function func = {0};
    func.kind = FuncKind_Script;
    func.name = NDSAtom_Invalid;
    func.params = NULL;
    func.paramCount = blockParamCount;
    func.as.script.code = funcCode;
    func.as.script.lines = funcLines;
    func.as.script.codeLength = codeCount;
    func.as.script.handlerIP = 0;
    func.as.script.handlerErrorSlot = -1;
    func.as.script.maxLocals = self->maxLocalCount;

    size_t funcIndex = FunctionTable_add(self->ctx, func);

    /* Restore compiler state */
    self->code = savedCode;
    self->localCount = savedLocalCount;
    for (int i = 0; i < savedLocalCount; i++)
        self->locals[i] = savedLocals[i];
    self->maxLocalCount = savedMaxLocalCount;
    self->scopeDepth = savedScopeDepth;
    self->loopCount = savedLoopCount;
    self->outerLocalCount = savedOuterLocalCount;

    if (funcIndex == (size_t)-1)
        Compiler_error(self, "out of memory");

    /* Patch the Op_Call instruction's .b to attach the block */
    self->code.code[callIdx].b = (uint32_t)funcIndex;
    PARSE_LEAVE;
}

static void
Compiler_parsePrecedence(Compiler *self, Precedence minPrec)
{
    PARSE_ENTER;
    PrefixParselet prefix = GrammarRule_get(Compiler_current(self)->type)->prefix;
    if (!prefix)
        Compiler_error(self, "expected expression");
    prefix(self);

    while (minPrec <= GrammarRule_get(Compiler_current(self)->type)->infixPrec){
        InfixParselet infix = GrammarRule_get(Compiler_current(self)->type)->infix;
        infix(self);
    }
    PARSE_LEAVE;
}

static void
Compiler_parseExpression(Compiler *self)
{
    PARSE_ENTER;
    Compiler_parsePrecedence(self, Precedence_Or);
    PARSE_LEAVE;
}

/* Prefix parselets: literals and grouping. */

static void
Prefix_number(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(self->previous.value.number));
    PARSE_LEAVE;
}

static void
Prefix_string(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_emitStringConstant(self, self->previous.value.text.data, self->previous.value.text.length);
    PARSE_LEAVE;
}

static void
Prefix_true(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_emitConstant(self, NDSValue_True);
    PARSE_LEAVE;
}

static void
Prefix_false(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_emitConstant(self, NDSValue_False);
    PARSE_LEAVE;
}

static void
Prefix_nothing(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_emit(self, Op_PushNothing, 0, 0);
    PARSE_LEAVE;
}

static void
Prefix_grouping(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume '(' */
    Compiler_parseExpression(self);
    Compiler_consume(self, Token_RightParen, "expected ')' after expression");
    PARSE_LEAVE;
}

static void
Prefix_listLiteral(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume '[' */
    int count = 0;
    if (!Compiler_check(self, Token_RightBracket)){
        do{
            Compiler_parseExpression(self);
            count++;
        } while (Compiler_match(self, Token_Comma));
    }
    Compiler_consume(self, Token_RightBracket, "expected ']' after list literal");
    Compiler_emit(self, Op_NewList, (uint32_t)count, 0);
    PARSE_LEAVE;
}

static void
Prefix_mapLiteral(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume '{' */
    int count = 0;
    if (!Compiler_check(self, Token_RightBrace)){
        do{
            Compiler_parseExpression(self);
            Compiler_consume(self, Token_Colon, "expected ':' after map key");
            Compiler_parseExpression(self);
            count++;
        } while (Compiler_match(self, Token_Comma));
    }
    Compiler_consume(self, Token_RightBrace, "expected '}' after map literal");
    Compiler_emit(self, Op_NewMap, (uint32_t)count, 0);
    PARSE_LEAVE;
}

/* Prefix parselets: word forms (variables, calls, prepositional). */

static void
Prefix_givenCall(Compiler *self)
{
    PARSE_ENTER;
    /* `given(args)` — invoke the current function's attached block. */
    if (Compiler_peek(self, 1)->type != Token_LeftParen){
        Compiler_error(self, "expected '(' after 'given'");
        PARSE_LEAVE;
    }
    Compiler_advance(self); /* consume 'given' */
    Compiler_advance(self); /* consume '(' */

    int argCount = 0;
    if (!Compiler_check(self, Token_RightParen)){
        do{
            Compiler_parseExpression(self);
            argCount++;
        } while (Compiler_match(self, Token_Comma));
    }
    Compiler_consume(self, Token_RightParen, "expected ')' after given arguments");
    Compiler_emit(self, Op_CallGiven, (uint32_t)argCount, 0);
    PARSE_LEAVE;
}

static void
Prefix_blockTest(Compiler *self)
{
    PARSE_ENTER;
    /* `block given` / `block not given` — does this function have a block? */
    Token *next = Compiler_peek(self, 1);
    if (next->type != Token_Given && next->type != Token_Not){
        Compiler_error(self, "expected 'given' or 'not given' after 'block'");
        PARSE_LEAVE;
    }
    Compiler_advance(self); /* consume 'block' */
    bool negate = Compiler_match(self, Token_Not);
    Compiler_consume(self, Token_Given, "expected 'given' after 'block'");
    Compiler_emit(self, Op_BlockGiven, 0, 0);
    if (negate)
        Compiler_emit(self, Op_Not, 0, 0);
    PARSE_LEAVE;
}

static void
Prefix_word(Compiler *self)
{
    PARSE_ENTER;
    Token *tok = Compiler_current(self);
    Token *next = Compiler_peek(self, 1);

    if (tok->wordFlags & TokenWord_ChunkID){
        if (next->type == Token_Of || next->type == Token_In){
            NDSAtom nameAtom = Compiler_internName(self, tok->value.text.data, tok->value.text.length);
            Compiler_advance(self);
            Compiler_advance(self);
            Compiler_parsePrecedence(self, Precedence_OfExpr);
            Compiler_emit(self, Op_PropertyGet, (uint32_t)nameAtom, 0);
            PARSE_LEAVE;
        }
        if (Token_canStartPrimary(next->type)){
            NDSAtom chunkID = Compiler_internName(self, tok->value.text.data, tok->value.text.length);
            Compiler_advance(self);
            Compiler_parseMaybeNegatedPrimary(self);
            if (Compiler_match(self, Token_To)){
                Compiler_parseMaybeNegatedPrimary(self);
                Compiler_consumeOfOrIn(self, "after range");
                Compiler_parsePrecedence(self, Precedence_OfExpr);
                Compiler_emit(self, Op_ChunkGetRange, (uint32_t)chunkID, 0);
            } else{
                Compiler_consumeOfOrIn(self, "after chunk index");
                Compiler_parsePrecedence(self, Precedence_OfExpr);
                Compiler_emit(self, Op_ChunkGet, (uint32_t)chunkID, 0);
            }
            PARSE_LEAVE;
        }
        /* fall through: chunk-flagged word used as a plain variable */
    } else if (next->type == Token_Of || next->type == Token_In){
        NDSAtom nameAtom = Compiler_internName(self, tok->value.text.data, tok->value.text.length);
        Compiler_advance(self);
        Compiler_advance(self);
        Compiler_parsePrecedence(self, Precedence_OfExpr);
        Compiler_emit(self, Op_PropertyGet, (uint32_t)nameAtom, 0);
        PARSE_LEAVE;
    }

    if (next->type == Token_LeftParen){
        const char *name = tok->value.text.data;
        size_t nameLen = tok->value.text.length;
        Compiler_advance(self);
        Compiler_advance(self);

        int argCount = 0;
        if (!Compiler_check(self, Token_RightParen)){
            do{
                Compiler_parseExpression(self);
                argCount++;
            } while (Compiler_match(self, Token_Comma));
        }
        Compiler_consume(self, Token_RightParen, "expected ')' after arguments");

        NDSAtom nameAtom = Compiler_internName(self, name, nameLen);
        VariableLocation v = Compiler_resolveVar(self, nameAtom);
        switch (v.kind){
        case VariableLocationKind_NotFound:
            Compiler_error(self, "unknown function '%s'", Compiler_atomName(self, nameAtom));
            break;
        case VariableLocationKind_Local:
            Compiler_emit(self, Op_LoadLocal, (uint32_t)v.slot, 0);
            break;
        case VariableLocationKind_Outer:
            Compiler_emit(self, Op_LoadOuter, (uint32_t)v.slot, 0);
            break;
        case VariableLocationKind_Global: {
            /* Self-recursive function: const slot may still be Nothing. */
            Global *g = &self->ctx->globals[v.slot];
            if (g->isConst && g->value.type != NDSValueType_Function &&
                              g->value.type != NDSValueType_Nothing)
                Compiler_error(self, "'%s' is not a function", Compiler_atomName(self, nameAtom));
            Compiler_emit(self, Op_LoadGlobal, (uint32_t)v.slot, 0);
            break;
        }
        }

        if (Compiler_check(self, Token_Given) &&
            Compiler_peek(self, 1)->type == Token_LeftParen){
            size_t callIdx = Compiler_emit(self, Op_Call, (uint32_t)argCount, NDS_NO_BLOCK);
            Compiler_parseGivenBlock(self, callIdx);
        } else
            Compiler_emit(self, Op_Call, (uint32_t)argCount, NDS_NO_BLOCK);
        PARSE_LEAVE;
    }

    Compiler_emitLoadVar(self, tok->value.text.data, tok->value.text.length);
    Compiler_advance(self);
    PARSE_LEAVE;
}

static void
Prefix_unaryMinus(Compiler *self)
{
    PARSE_ENTER;
    /* -NUMBER folds into a single negative constant. This works around some interesting grammar glitches. */
    if (Compiler_peek(self, 1)->type == Token_Number){
        Compiler_advance(self); /* consume '-' */
        Compiler_advance(self); /* consume number */
        Compiler_emitConstant(self, NDSValue_numberFromDouble(-self->previous.value.number));
        PARSE_LEAVE;
    }
    Compiler_advance(self); /* consume '-' */
    Compiler_parsePrecedence(self, Precedence_Unary);
    Compiler_emit(self, Op_Negate, 0, 0);
    PARSE_LEAVE;
}

static void
Prefix_not(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume 'not' */
    Compiler_parsePrecedence(self, Precedence_Not);
    Compiler_emit(self, Op_Not, 0, 0);
    PARSE_LEAVE;
}

static void
Prefix_firstLast(Compiler *self)
{
    PARSE_ENTER;
    /* `first CHUNKID of expr` / `last CHUNKID of expr` */
    double idx = Compiler_check(self, Token_First) ? 1.0 : -1.0;
    Token *next = Compiler_peek(self, 1);
    if (next->type != Token_Word || !(next->wordFlags & TokenWord_ChunkID)){
        Compiler_error(self, "expected chunk ID after 'first'/'last'");
        PARSE_LEAVE;
    }
    Compiler_advance(self); /* consume 'first'/'last' */
    Token *chunkTok = Compiler_current(self);
    NDSAtom chunkID = Compiler_internName(self, chunkTok->value.text.data, chunkTok->value.text.length);
    Compiler_advance(self); /* consume chunk ID */
    Compiler_consumeOfOrIn(self, "after chunk ID");
    Compiler_emitConstant(self, NDSValue_numberFromDouble(idx));
    Compiler_parsePrecedence(self, Precedence_OfExpr);
    Compiler_emit(self, Op_ChunkGet, (uint32_t)chunkID, 0);
    PARSE_LEAVE;
}

static void
Prefix_every(Compiler *self)
{
    PARSE_ENTER;
    /* `every CHUNKID of expr [where pred]` */
    Token *next = Compiler_peek(self, 1);
    if (next->type != Token_Word || !(next->wordFlags & TokenWord_ChunkID)){
        Compiler_error(self, "expected chunk ID after 'every'");
        PARSE_LEAVE;
    }
    Compiler_advance(self); /* consume 'every' */
    Token *chunkTok = Compiler_current(self);
    NDSAtom chunkID = Compiler_internName(self, chunkTok->value.text.data, chunkTok->value.text.length);
    Compiler_advance(self); /* consume chunk ID */
    Compiler_consumeOfOrIn(self, "after chunk ID");

    Compiler_parsePrecedence(self, Precedence_OfExpr);
    Compiler_emit(self, Op_ChunkGetEvery, (uint32_t)chunkID, 0);

    if (!Compiler_match(self, Token_Where))
        PARSE_LEAVE;

    /* Inline filter loop. The iterable is on the stack. */
    Compiler_pushScope(self);

    int iterListSlot = Compiler_addAnonymousLocal(self);
    int counterSlot  = Compiler_addAnonymousLocal(self);
    int limitSlot    = Compiler_addAnonymousLocal(self);
    int resultSlot   = Compiler_addAnonymousLocal(self);
    int itSlot       = Compiler_addLocal(self, NDSBuiltinAtom_It);

    Compiler_emit(self, Op_StoreLocal, (uint32_t)iterListSlot, 0);

    Compiler_emit(self, Op_NewList, 0, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)resultSlot, 0);

    Compiler_emitConstant(self, NDSValue_numberFromDouble(0.0));
    Compiler_emit(self, Op_StoreLocal, (uint32_t)counterSlot, 0);

    Compiler_emit(self, Op_LoadLocal, (uint32_t)iterListSlot, 0);
    Compiler_emit(self, Op_PropertyGet, (uint32_t)NDSBuiltinAtom_Count, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)limitSlot, 0);

    size_t conditionStart = self->code.count;
    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)limitSlot, 0);
    Compiler_emit(self, Op_Lt, 0, 0);
    size_t exitJump = Compiler_emitJump(self, Op_JumpIfFalse);

    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, Op_Add, 0, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)iterListSlot, 0);
    Compiler_emit(self, Op_ChunkGet, (uint32_t)NDSBuiltinAtom_Item, 0);

    Compiler_emit(self, Op_StoreLocal, (uint32_t)itSlot, 0);

    Compiler_parseExpression(self);

    size_t skipJump = Compiler_emitJump(self, Op_JumpIfFalse);

    Compiler_emit(self, Op_LoadLocal, (uint32_t)resultSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)itSlot, 0);
    Compiler_emit(self, Op_ListAppend, 0, 0);
    Compiler_emit(self, Op_Pop, 0, 0);

    Compiler_patchJump(self, skipJump);

    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, Op_Add, 0, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)counterSlot, 0);

    Compiler_emit(self, Op_Jump, (uint32_t)conditionStart, 0);

    Compiler_patchJump(self, exitJump);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)resultSlot, 0);

    Compiler_popScope(self);
    PARSE_LEAVE;
}

/* Infix parselets. */

static void
Infix_binary(Compiler *self)
{
    PARSE_ENTER;
    TokenType op = Compiler_current(self)->type;
    Compiler_advance(self);

    OpCode opcode = Op_PushNothing;
    switch (op){
    case Token_Plus:      opcode = Op_Add;    break;
    case Token_Minus:     opcode = Op_Sub;    break;
    case Token_Star:      opcode = Op_Mul;    break;
    case Token_Slash:     opcode = Op_Div;    break;
    case Token_Mod:       opcode = Op_Mod;    break;
    case Token_Div:       opcode = Op_IntDiv; break;
    case Token_Ampersand: opcode = Op_Concat; break;
    default:
        Compiler_error(self, "internal: bad binary op");
        PARSE_LEAVE;
    }
    /* +1 for left associativity. */
    Precedence prec = GrammarRule_get(op)->infixPrec;
    Compiler_parsePrecedence(self, prec + 1);
    Compiler_emit(self, opcode, 0, 0);
    PARSE_LEAVE;
}

/* `is` may begin a multi-word comparison form ("is greater than", etc) */
static OpCode
Compiler_consumeIsTail(Compiler *self)
{
    Token *cur = Compiler_current(self);

    if (cur->type == Token_Not){
        Compiler_advance(self);
        if (Token_textIs(Compiler_current(self), "equal") &&
            Compiler_peek(self, 1)->type == Token_To){
            Compiler_advance(self);
            Compiler_advance(self);
        }
        return Op_Neq;
    }

    if (Token_textIs(cur, "equal") && Compiler_peek(self, 1)->type == Token_To){
        Compiler_advance(self);
        Compiler_advance(self);
        return Op_Eq;
    }

    bool isGreater = Token_textIs(cur, "greater");
    bool isLess    = Token_textIs(cur, "less");
    if ((isGreater || isLess) && Token_textIs(Compiler_peek(self, 1), "than")){
        Compiler_advance(self);
        Compiler_advance(self);
        if (Compiler_check(self, Token_Or) && Token_textIs(Compiler_peek(self, 1), "equal")){
            Compiler_advance(self);
            Compiler_advance(self);
            Compiler_consume(self, Token_To, "expected 'to' after 'or equal'");
            return isGreater ? Op_GtEq : Op_LtEq;
        }
        return isGreater ? Op_Gt : Op_Lt;
    }

    return Op_Eq;
}

static void
Infix_comparison(Compiler *self)
{
    PARSE_ENTER;
    TokenType op = Compiler_current(self)->type;
    Compiler_advance(self);

    OpCode opcode = Op_PushNothing;
    switch (op){
    case Token_Is:                      opcode = Compiler_consumeIsTail(self); break;
    case Token_Eq:                      opcode = Op_Eq;                        break;
    case Token_Neq:                     opcode = Op_Neq;                       break;
    case Token_Lt:                      opcode = Op_Lt;                        break;
    case Token_Gt:                      opcode = Op_Gt;                        break;
    case Token_LtEq:                    opcode = Op_LtEq;                      break;
    case Token_GtEq:                    opcode = Op_GtEq;                      break;
    case Token_Contains:                opcode = Op_Contains;                  break;
    default:
        Compiler_error(self, "internal: bad comparison op");
        PARSE_LEAVE;
    }
    Compiler_parsePrecedence(self, Precedence_Comparison + 1);
    Compiler_emit(self, opcode, 0, 0);

    /* Non-associative. */
    if (GrammarRule_get(Compiler_current(self)->type)->infix == Infix_comparison)
        Compiler_error(self, "comparison operators are non-associative");
    PARSE_LEAVE;
}

static void
Infix_and(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume 'and' */
    Compiler_emit(self, Op_Dup, 0, 0);
    size_t jump = Compiler_emitJump(self, Op_JumpIfFalse);
    Compiler_emit(self, Op_Pop, 0, 0);
    Compiler_parsePrecedence(self, Precedence_And + 1);
    Compiler_patchJump(self, jump);
    PARSE_LEAVE;
}

static void
Infix_or(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume 'or' */
    Compiler_emit(self, Op_Dup, 0, 0);
    size_t jump = Compiler_emitJump(self, Op_JumpIfTrue);
    Compiler_emit(self, Op_Pop, 0, 0);
    Compiler_parsePrecedence(self, Precedence_Or + 1);
    Compiler_patchJump(self, jump);
    PARSE_LEAVE;
}

static void
Infix_cast(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self); /* consume 'as' */
    Token *cur = Compiler_current(self);
    if (cur->type != Token_Word){
        Compiler_error(self, "expected type name after 'as'");
        PARSE_LEAVE;
    }
    NDSAtom atom = Compiler_internName(self, cur->value.text.data, cur->value.text.length);
    NDSTypeID targetType = NDSTypeID_Invalid;
    for (NDSTypeID t = 0; t < self->ctx->typeCount; t++){
        if (self->ctx->types[t].nameAtom == atom){
            targetType = t;
            break;
        }
    }
    if (targetType == NDSTypeID_Invalid)
        Compiler_error(self, "unknown type '%.*s'", (int)cur->value.text.length, cur->value.text.data);
    Compiler_advance(self);
    Compiler_emit(self, Op_Cast, (uint32_t)targetType, 0);
    PARSE_LEAVE;
}

static bool
Stmt_my(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseMyDeclaration(self);
    PARSE_RETURN(false);
}

static bool
Stmt_set(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseSetStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_copy(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseCopyStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_append(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseAppendStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_increment(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseIncrementOrDecrement(self, Op_Add, "increment");
    PARSE_RETURN(false);
}

static bool
Stmt_decrement(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseIncrementOrDecrement(self, Op_Sub, "decrement");
    PARSE_RETURN(false);
}

static bool
Stmt_delete(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseDeleteStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_if(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseIfStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_while(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseWhileStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_repeat(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseRepeatStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_for(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseForStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_return(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseReturnStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_raise(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseRaiseStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_exit(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseExitStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_next(Compiler *self)
{
    PARSE_ENTER;
    Compiler_advance(self);
    Compiler_parseNextStatement(self);
    PARSE_RETURN(false);
}

static bool
Stmt_wordOrExpr(Compiler *self)
{
    PARSE_ENTER;
    if (Token_isHostHead(Compiler_current(self))){
        Compiler_parseHostCommand(self);
        PARSE_RETURN(false);
    }
    Compiler_parseExpression(self);
    PARSE_RETURN(true);
}

static void
Compiler_parseMyDeclaration(Compiler *self)
{
    PARSE_ENTER;
    /* 'my' is only called from parseBlock (never from parseProgram),
       so scopeDepth > 0 is guaranteed — all variables are locals. */

    for (;;){
        if (!Compiler_check(self, Token_Word))
            Compiler_error(self, "expected variable name after 'my'");

        Token *tok = Compiler_current(self);
        Compiler_advance(self);

        NDSAtom atom = Compiler_internName(self, tok->value.text.data, tok->value.text.length);

        /* Check for duplicate in the same scope */
        for (int i = self->localCount - 1; i >= 0; i--){
            if (self->locals[i].scopeDepth < self->scopeDepth)
                break;
            if (self->locals[i].name == atom)
                Compiler_error(self, "duplicate variable '%s'", Compiler_atomName(self, atom));
        }

        /* Allocate slot first (fixed-slot: slot exists in reserved frame space) */
        int slot = Compiler_addLocal(self, atom);

        if (Compiler_match(self, Token_Is)){
            Compiler_parseExpression(self);
            Compiler_emit(self, Op_StoreLocal, (uint32_t)slot, 0);
        }
        /* else: slot already initialized to nothing by Op_Reserve */

        if (!Compiler_match(self, Token_Comma))
            break;
    }
    PARSE_LEAVE;
}

typedef enum{
    AssignStep_Chunk,
    AssignStep_Property
} AssignStepKind;

typedef struct{
    AssignStepKind kind;
    NDSAtom    nameAtom;
    NDSChunkID chunkID;
    int        indexLocal;
    bool       isFirst;
    bool       isLast;
    bool       hasExprIndex;
} AssignStep;

/* Reads one accessor head, consuming the trailing of/in. Returns false
   without consuming anything if the current position isn't an accessor. */
static bool
Compiler_tryReadAccessorHead(Compiler *self, AssignStep *out)
{
    *out = (AssignStep){0};

    if (Compiler_check(self, Token_First) || Compiler_check(self, Token_Last)){
        Token *next = Compiler_peek(self, 1);
        if (next->type != Token_Word || !(next->wordFlags & TokenWord_ChunkID))
            return false;
        bool isLast = Compiler_check(self, Token_Last);
        Compiler_advance(self);
        Token *chunkTok = Compiler_current(self);
        out->kind = AssignStep_Chunk;
        out->chunkID = (NDSChunkID)Compiler_internName(self, chunkTok->value.text.data, chunkTok->value.text.length);
        out->isFirst = !isLast;
        out->isLast  = isLast;
        Compiler_advance(self);
        Compiler_consumeOfOrIn(self, "after chunk ID");
        return true;
    }

    if (!Compiler_check(self, Token_Word))
        return false;

    Token *cur = Compiler_current(self);
    Token *next = Compiler_peek(self, 1);

    if (cur->wordFlags & TokenWord_ChunkID){
        if (next->type == Token_Of || next->type == Token_In){
            out->kind = AssignStep_Property;
            out->nameAtom = Compiler_internName(self, cur->value.text.data, cur->value.text.length);
            Compiler_advance(self);
            Compiler_advance(self);
            return true;
        }
        if (Token_canStartPrimary(next->type)){
            out->kind = AssignStep_Chunk;
            out->chunkID = (NDSChunkID)Compiler_internName(self, cur->value.text.data, cur->value.text.length);
            out->hasExprIndex = true;
            Compiler_advance(self);
            Compiler_parseMaybeNegatedPrimary(self);
            out->indexLocal = Compiler_addAnonymousLocal(self);
            Compiler_emit(self, Op_StoreLocal, (uint32_t)out->indexLocal, 0);
            Compiler_consumeOfOrIn(self, "after chunk index");
            return true;
        }
        return false;
    }

    if (next->type == Token_Of || next->type == Token_In){
        out->kind = AssignStep_Property;
        out->nameAtom = Compiler_internName(self, cur->value.text.data, cur->value.text.length);
        Compiler_advance(self);
        Compiler_advance(self);
        return true;
    }
    return false;
}

static void
Compiler_emitChunkIndex(Compiler *self, const AssignStep *step)
{
    if (step->hasExprIndex)
        Compiler_emit(self, Op_LoadLocal, (uint32_t)step->indexLocal, 0);
    else if (step->isFirst)
        Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    else if (step->isLast)
        Compiler_emitConstant(self, NDSValue_numberFromDouble(-1.0));
}

/* Recursively walks the rest of an assign target, leaving the container
 * on top of the stack.
 */
static void
Compiler_emitAssignNavigation(Compiler *self)
{
    PARSE_ENTER;
    AssignStep step = {0};
    if (Compiler_tryReadAccessorHead(self, &step)){
        Compiler_emitAssignNavigation(self);
        if (step.kind == AssignStep_Chunk){
            Compiler_emitChunkIndex(self, &step);
            Compiler_emit(self, Op_ChunkGet, (uint32_t)step.chunkID, 0);
        } else
            Compiler_emit(self, Op_PropertyGet, (uint32_t)step.nameAtom, 0);
        PARSE_LEAVE;
    }
    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "assign target must end with a variable");
    Token *t = Compiler_current(self);
    Compiler_emitLoadVarByAtom(self, Compiler_internName(self, t->value.text.data, t->value.text.length));
    Compiler_advance(self);
    PARSE_LEAVE;
}

static void
Compiler_parseSetStatement(Compiler *self)
{
    PARSE_ENTER;

    /* `set NAME to EXPR` */
    if (Compiler_check(self, Token_Word) && Compiler_peek(self, 1)->type == Token_To){
        Token *t = Compiler_current(self);
        NDSAtom name = Compiler_internName(self, t->value.text.data, t->value.text.length);
        Compiler_advance(self);
        Compiler_advance(self);
        Compiler_parseExpression(self);
        Compiler_emitStoreVarByAtom(self, name);
        PARSE_LEAVE;
    }

    Compiler_pushScope(self);

    AssignStep outermost = {0};
    if (!Compiler_tryReadAccessorHead(self, &outermost))
        Compiler_error(self, "expected variable or chunk access in 'set' target");
    if (outermost.kind == AssignStep_Property)
        Compiler_error(self, "cannot set a property");

    Compiler_emitAssignNavigation(self);
    Compiler_consume(self, Token_To, "expected 'to' in set statement");
    Compiler_emitChunkIndex(self, &outermost);
    Compiler_parseExpression(self);
    Compiler_emit(self, Op_ChunkSet, (uint32_t)outermost.chunkID, 0);

    Compiler_popScope(self);
    PARSE_LEAVE;
}

static void
Compiler_parseCopyStatement(Compiler *self)
{
    PARSE_ENTER;
    Compiler_parseExpression(self);
    Compiler_emit(self, Op_ShallowCopy, 0, 0);
    Compiler_consume(self, Token_To, "expected 'to' after expression");

    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected variable name after 'to'");
    Token *tok = Compiler_current(self);
    Compiler_emitStoreVar(self, tok->value.text.data, tok->value.text.length);
    Compiler_advance(self);
    PARSE_LEAVE;
}

static void
Compiler_parseAppendStatement(Compiler *self)
{
    PARSE_ENTER;
    /* append EXPR to VAR
     *
     * Stack dance:
     *   parseExpression     → [value]
     *   loadVar(list)       → [value, list]
     *   Op_Swap             → [list, value]
     *   Op_ListAppend       → [list]       (pops value, appends, list stays)
     *   storeGlobal(it)     → []           (list stored as 'it')
     */
    Compiler_parseExpression(self);
    Compiler_consume(self, Token_To, "expected 'to' after expression");

    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected variable name after 'to'");

    Token *tok = Compiler_current(self);
    Compiler_emitLoadVar(self, tok->value.text.data, tok->value.text.length);
    Compiler_emit(self, Op_Swap, 0, 0);
    Compiler_emit(self, Op_ListAppend, 0, 0);
    Compiler_emit(self, Op_StoreGlobal, (uint32_t)NDSBuiltinGlobal_It, 0);
    Compiler_advance(self);
    PARSE_LEAVE;
}

static void
Compiler_parseIncrementOrDecrement(Compiler *self, OpCode op, const char *keyword)
{
    PARSE_ENTER;
    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected variable name after '%s'", keyword);

    Token *tok = Compiler_current(self);
    const char *name = tok->value.text.data;
    size_t nameLen = tok->value.text.length;
    Compiler_advance(self);

    Compiler_emitLoadVar(self, name, nameLen);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, op, 0, 0);
    Compiler_emitStoreVar(self, name, nameLen);
    PARSE_LEAVE;
}

static void
Compiler_parseDeleteStatement(Compiler *self)
{
    PARSE_ENTER;
    Compiler_pushScope(self);

    AssignStep outermost = {0};
    if (!Compiler_tryReadAccessorHead(self, &outermost))
        Compiler_error(self, "cannot delete a variable");
    if (outermost.kind == AssignStep_Property)
        Compiler_error(self, "cannot delete a property");

    Compiler_emitAssignNavigation(self);
    Compiler_emitChunkIndex(self, &outermost);
    Compiler_emit(self, Op_ChunkDelete, (uint32_t)outermost.chunkID, 0);

    Compiler_popScope(self);
    PARSE_LEAVE;
}

static void
Compiler_parseBlockContents(Compiler *self);
static void
Compiler_parseBlock(Compiler *self);
static void
Compiler_parseIfStatement(Compiler *self);
static void
Compiler_parseWhileStatement(Compiler *self);
static void
Compiler_parseRepeatStatement(Compiler *self);
static void
Compiler_parseForStatement(Compiler *self);
static void
Compiler_parseExitStatement(Compiler *self);
static void
Compiler_parseNextStatement(Compiler *self);
static void
Compiler_parseReturnStatement(Compiler *self);
static void
Compiler_parseRaiseStatement(Compiler *self);
static void
Compiler_parseFunctionDefinition(Compiler *self);

static bool
Compiler_checkBlockEnd(Compiler *self)
{
    return Compiler_check(self, Token_End) || Compiler_check(self, Token_Else) ||
           Compiler_check(self, Token_On) || Compiler_check(self, Token_EOF);
}

static void
Compiler_expectBlockNewline(Compiler *self)
{
    if (!Compiler_checkBlockEnd(self) && !Compiler_match(self, Token_Newline))
        Compiler_error(self, "expected newline or end of block");
    while (Compiler_match(self, Token_Newline))
        ;
}

static bool
Token_isHostHead(const Token *tok)
{
    return tok->type == Token_Word && (tok->wordFlags & TokenWord_HostHead);
}

/* Find the atom for a token's text. Works for both Token_Word and keywords. */
static NDSAtom
Compiler_tokenAtom(Compiler *self, const Token *tok)
{
    return Compiler_internName(self, tok->value.text.data, tok->value.text.length);
}

/* Emit code for the tail (non-head) portion of a host command. */
static void
Compiler_emitHostCommandTail(Compiler *self, const HostCommand *cmd)
{
#define MAX_COMMAND_VARIABLES 256
    /* Record variable names for post-call store */
    const char *varNames[MAX_COMMAND_VARIABLES] = {0};
    size_t varNameLens[MAX_COMMAND_VARIABLES] = {0};
    int varIdx = 0;

    /* Reserve variable output slots below the args */
    if (cmd->variableCount > 0)
        Compiler_emit(self, Op_Reserve, (uint32_t)cmd->variableCount, 0);

    /* Walk tail steps (head words already consumed by trie walk) */
    for (int i = cmd->headLength; i < cmd->stepCount; i++){
        InternalPatternStep *step = &cmd->steps[i];
        switch (step->kind){
        case PatternStepKind_Word:{
            Token *tok = Compiler_current(self);
            NDSAtom tokAtom = Compiler_tokenAtom(self, tok);
            if (tokAtom != step->atom)
                Compiler_error(self, "unexpected word in host command");
            Compiler_advance(self);
            break;
        }
        case PatternStepKind_Expression:
            Compiler_parseExpression(self);
            break;
        case PatternStepKind_Variable:{
            Token *tok = Compiler_current(self);
            if (tok->type != Token_Word)
                Compiler_error(self, "expected variable name");
            if (varIdx >= MAX_COMMAND_VARIABLES)
                Compiler_error(self, "too many variables in host command");
            varNames[varIdx] = tok->value.text.data;
            varNameLens[varIdx] = tok->value.text.length;
            varIdx++;
            Compiler_advance(self);
            break;
        }
        }
    }

    /* Emit call: push function value, then Op_Call with arg count */
    Compiler_emitConstant(self, NDSValue_functionFromIndex(cmd->functionIndex));
    Compiler_emit(self, Op_Call, (uint32_t)cmd->expressionCount, NDS_NO_BLOCK);

    /* Store return value to 'it', then discard it */
    Compiler_emit(self, Op_Dup, 0, 0);
    Compiler_emit(self, Op_StoreGlobal, (uint32_t)NDSBuiltinGlobal_It, 0);
    Compiler_emit(self, Op_Pop, 0, 0);

    /* Pop variable slot values and store to named variables.
       Stack after call: [var(M-1)] ... [var(0)]
       Top of stack is var(0), then var(1), etc. */
    for (int i = 0; i < cmd->variableCount; i++)
        Compiler_emitStoreVar(self, varNames[i], varNameLens[i]);
}

/* Walk the command trie to match head words, then parse the tail. */
static void
Compiler_parseHostCommand(Compiler *self)
{
    PARSE_ENTER;
    NDSContext *ctx = self->ctx;
    CommandTrieNode *node = ctx->commandTrie;

    /* Walk the trie consuming head words */
    while (node){
        Token *tok = Compiler_current(self);
        NDSAtom tokAtom = Compiler_tokenAtom(self, tok);

        /* Find matching child */
        CommandTrieNode *child = NULL;
        for (CommandTrieNode *c = node; c; c = c->next){
            if (c->atom == tokAtom){
                child = c;
                break;
            }
        }
        if (!child)
            break;

        Compiler_advance(self);

        if (child->commandIndex != (size_t)-1){
            Compiler_emitHostCommandTail(self, &ctx->commands[child->commandIndex]);
            PARSE_LEAVE;
        }

        node = child->children;
    }

    Compiler_error(self, "unrecognized host command");
}

/* Dispatch a single statement via the grammar table. Tokens with a
   .statement parselet fire it; anything else that can start an
   expression is treated as a bare expression statement. Returns true
   when a value was left on the stack (the caller decides whether to
   pop it — block context pops, top-level may keep the last one). */
static bool
Compiler_dispatchStatement(Compiler *self)
{
    PARSE_ENTER;
    Token *tok = Compiler_current(self);
    const GrammarRule *r = GrammarRule_get(tok->type);

    if (r->statement)
        PARSE_RETURN(r->statement(self));

    if (r->prefix){
        Compiler_parseExpression(self);
        PARSE_RETURN(true);
    }
    Compiler_error(self, "expected statement");
    PARSE_RETURN(false);
}

static void
Compiler_parseBlockContents(Compiler *self)
{
    PARSE_ENTER;
    while (Compiler_match(self, Token_Newline))
        ;

    /* Phase 1: initial statements (`my`). */
    while (!Compiler_checkBlockEnd(self) &&
           GrammarRule_get(Compiler_current(self)->type)->phase == StmtPhase_Initial){
        Compiler_dispatchStatement(self);
        Compiler_expectBlockNewline(self);
    }

    /* Phase 2: middle statements (and bare expressions). */
    while (!Compiler_checkBlockEnd(self)){
        StmtPhase ph = GrammarRule_get(Compiler_current(self)->type)->phase;
        if (ph == StmtPhase_Initial)
            Compiler_error(self, "'my' declarations must appear at the beginning of a block");
        if (ph == StmtPhase_Final)
            break;
        if (Compiler_dispatchStatement(self))
            Compiler_emit(self, Op_Pop, 0, 0);
        Compiler_expectBlockNewline(self);
    }

    /* Phase 3: optional final statement (return, raise, exit, next). */
    if (!Compiler_checkBlockEnd(self)){
        if (GrammarRule_get(Compiler_current(self)->type)->phase != StmtPhase_Final)
            Compiler_error(self, "expected final statement or end of block");
        Compiler_dispatchStatement(self);
        Compiler_expectBlockNewline(self);
        if (!Compiler_checkBlockEnd(self))
            Compiler_error(self, "unreachable code after final statement");
    }
    PARSE_LEAVE;
}

static void
Compiler_parseBlock(Compiler *self)
{
    PARSE_ENTER;
    Compiler_pushScope(self);
    Compiler_parseBlockContents(self);
    Compiler_popScope(self);
    PARSE_LEAVE;
}

static void
Compiler_parseIfStatement(Compiler *self)
{
    PARSE_ENTER;
#define MAX_ELSE_IF_BRANCHES 64
    /* Array of end-jump fixups for chained if/else-if/else */
    size_t endJumps[MAX_ELSE_IF_BRANCHES] = {0};
    int endJumpCount = 0;

    /* Parse condition */
    Compiler_parseExpression(self);

    /* Optional 'then' */
    Compiler_match(self, Token_Then);

    size_t elseJump = Compiler_emitJump(self, Op_JumpIfFalse);

    /* Parse 'then' block */
    Compiler_parseBlock(self);

    for (;;){
        if (Compiler_check(self, Token_Else)){
            /* Jump over else/else-if block */
            if (endJumpCount >= MAX_ELSE_IF_BRANCHES)
                Compiler_error(self, "too many chained else-if branches");
            endJumps[endJumpCount++] = Compiler_emitJump(self, Op_Jump);

            Compiler_patchJump(self, elseJump);
            Compiler_advance(self); /* consume 'else' */

            if (Compiler_match(self, Token_If)){
                /* else if: parse condition */
                Compiler_parseExpression(self);
                Compiler_match(self, Token_Then);
                elseJump = Compiler_emitJump(self, Op_JumpIfFalse);
                Compiler_parseBlock(self);
                continue;
            } else{
                /* bare else */
                Compiler_parseBlock(self);
                break;
            }
        } else{
            /* No else — patch the false jump to here */
            Compiler_patchJump(self, elseJump);
            break;
        }
    }

    /* Consume 'end' 'if' */
    Compiler_consume(self, Token_End, "expected 'end' after if block");
    Compiler_consume(self, Token_If, "expected 'if' after 'end'");

    /* Patch all end jumps */
    for (int i = 0; i < endJumpCount; i++)
        Compiler_patchJump(self, endJumps[i]);
    PARSE_LEAVE;
}

static void
Compiler_parseWhileStatement(Compiler *self)
{
    PARSE_ENTER;
    size_t conditionStart = self->code.count;

    /* Parse condition */
    Compiler_parseExpression(self);
    Compiler_consume(self, Token_Do, "expected 'do' after condition");
    size_t exitJump = Compiler_emitJump(self, Op_JumpIfFalse);

    Compiler_pushLoop(self, LoopKind_While);
    Compiler_parseBlock(self);

    /* Jump back to condition */
    Compiler_emit(self, Op_Jump, (uint32_t)conditionStart, 0);

    size_t afterLoop = self->code.count;
    Compiler_patchJump(self, exitJump);
    Compiler_popLoop(self, conditionStart, afterLoop);

    /* Consume 'end' 'while' */
    Compiler_consume(self, Token_End, "expected 'end' after while block");
    Compiler_consume(self, Token_While, "expected 'while' after 'end'");
    PARSE_LEAVE;
}

static void
Compiler_parseRepeatStatement(Compiler *self)
{
    PARSE_ENTER;
    size_t top = self->code.count;

    Compiler_consume(self, Token_Do, "expected 'do' after 'repeat'");
    Compiler_pushLoop(self, LoopKind_Repeat);
    Compiler_parseBlock(self);

    /* Jump back to top */
    Compiler_emit(self, Op_Jump, (uint32_t)top, 0);

    size_t afterLoop = self->code.count;
    Compiler_popLoop(self, top, afterLoop);

    /* Consume 'end' 'repeat' */
    Compiler_consume(self, Token_End, "expected 'end' after repeat block");
    Compiler_consume(self, Token_Repeat, "expected 'repeat' after 'end'");
    PARSE_LEAVE;
}

static void
Compiler_parseForEachStatement(Compiler *self)
{
    PARSE_ENTER;
    /* 'for' 'each' already consumed. Grammar:
       for each CHUNK_ID IDENTIFIER in <expr> [where <expr>]
           block
       end for
    */
    Compiler_pushScope(self);

    /* Expect chunk ID */
    if (!Compiler_check(self, Token_Word) ||
        !(Compiler_current(self)->wordFlags & TokenWord_ChunkID))
        Compiler_error(self, "expected chunk ID after 'each'");

    Token *chunkTok = Compiler_current(self);
    NDSAtom chunkID = Compiler_internName(self, chunkTok->value.text.data, chunkTok->value.text.length);
    Compiler_advance(self);

    /* Expect iteration variable name */
    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected variable name after chunk ID");

    Token *varTok = Compiler_current(self);
    NDSAtom varAtom = Compiler_internName(self, varTok->value.text.data, varTok->value.text.length);
    Compiler_advance(self);

    /* Consume 'in' */
    if (!Compiler_match(self, Token_In) && !Compiler_match(self, Token_Of))
        Compiler_error(self, "expected 'in' after variable name in for each");

    /* Allocate slots first (fixed-slot) */
    int iterListSlot = Compiler_addAnonymousLocal(self);
    int counterSlot = Compiler_addAnonymousLocal(self);
    int limitSlot = Compiler_addAnonymousLocal(self);
    int varSlot = Compiler_addLocal(self, varAtom);

    /* Evaluate container expression, get every */
    Compiler_parseExpression(self);
    Compiler_emit(self, Op_ChunkGetEvery, (uint32_t)chunkID, 0);

    /* Check for optional 'where' clause */
    bool hasWhere = false;
    if (Compiler_match(self, Token_Where)){
        hasWhere = true;
    }

    /* Store the iteration list */
    Compiler_emit(self, Op_StoreLocal, (uint32_t)iterListSlot, 0);

    /* counter = 0 */
    Compiler_emitConstant(self, NDSValue_numberFromDouble(0.0));
    Compiler_emit(self, Op_StoreLocal, (uint32_t)counterSlot, 0);

    /* limit = count of iterList */
    Compiler_emit(self, Op_LoadLocal, (uint32_t)iterListSlot, 0);
    Compiler_emit(self, Op_PropertyGet, (uint32_t)NDSBuiltinAtom_Count, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)limitSlot, 0);

    /* CONDITION: counter < limit */
    size_t conditionStart = self->code.count;
    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)limitSlot, 0);
    Compiler_emit(self, Op_Lt, 0, 0);
    size_t exitJump = Compiler_emitJump(self, Op_JumpIfFalse);

    /* element = item (counter+1) of iterList */
    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, Op_Add, 0, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)iterListSlot, 0);
    Compiler_emit(self, Op_ChunkGet, (uint32_t)NDSBuiltinAtom_Item, 0);

    /* Store element in iteration variable and global 'it' */
    Compiler_emit(self, Op_Dup, 0, 0);
    Compiler_emit(self, Op_StoreGlobal, (uint32_t)NDSBuiltinGlobal_It, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)varSlot, 0);

    /* Where clause: if present, the where expression was parsed before the block.
       But wait — we consumed 'where' already but haven't parsed its expression.
       The where expression filters: if false, skip to INCREMENT. */
    size_t whereSkipJump = (size_t)-1;
    if (hasWhere){
        Compiler_parseExpression(self);
        whereSkipJump = Compiler_emitJump(self, Op_JumpIfFalse);
    }

    Compiler_consume(self, Token_Do, "expected 'do'");

    Compiler_pushLoop(self, LoopKind_For);
    Compiler_parseBlock(self);

    /* If where clause skipped, jump target is here (INCREMENT) */
    size_t incrementStart = self->code.count;
    if (hasWhere)
        Compiler_patchJump(self, whereSkipJump);

    /* counter++ */
    Compiler_emit(self, Op_LoadLocal, (uint32_t)counterSlot, 0);
    Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, Op_Add, 0, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)counterSlot, 0);

    /* Jump back to condition */
    Compiler_emit(self, Op_Jump, (uint32_t)conditionStart, 0);

    size_t afterLoop = self->code.count;
    Compiler_patchJump(self, exitJump);
    Compiler_popLoop(self, incrementStart, afterLoop);

    /* Consume 'end' 'for' */
    Compiler_consume(self, Token_End, "expected 'end' after for block");
    Compiler_consume(self, Token_For, "expected 'for' after 'end'");

    Compiler_popScope(self);
    PARSE_LEAVE;
}

static void
Compiler_parseForStatement(Compiler *self)
{
    PARSE_ENTER;
    /* Check for 'for each' */
    if (Compiler_match(self, Token_Each)){
        Compiler_parseForEachStatement(self);
        PARSE_LEAVE;
    }

    Compiler_pushScope(self);

    /* Parse iteration variable name */
    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected variable name after 'for'");

    Token *varTok = Compiler_current(self);
    NDSAtom varAtom = Compiler_internName(self, varTok->value.text.data, varTok->value.text.length);
    Compiler_advance(self);

    /* Allocate slots first (fixed-slot) */
    int iSlot = Compiler_addLocal(self, varAtom);
    int limitSlot = Compiler_addAnonymousLocal(self);
    int stepSlot = Compiler_addAnonymousLocal(self);

    /* 'from' <expr> */
    Compiler_consume(self, Token_From, "expected 'from' in for statement");
    Compiler_parseExpression(self);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)iSlot, 0);

    /* 'to' <expr> */
    Compiler_consume(self, Token_To, "expected 'to' in for statement");
    Compiler_parseExpression(self);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)limitSlot, 0);

    /* Optional 'by' <expr> or default 1.0 */
    if (Compiler_match(self, Token_By)){
        Compiler_parseExpression(self);
    } else
        Compiler_emitConstant(self, NDSValue_numberFromDouble(1.0));
    Compiler_emit(self, Op_StoreLocal, (uint32_t)stepSlot, 0);

    Compiler_consume(self, Token_Do, "expected 'do'");

    /* condition: load i, limit, step, ForCheck */
    size_t conditionStart = self->code.count;
    Compiler_emit(self, Op_LoadLocal, (uint32_t)iSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)limitSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)stepSlot, 0);
    Compiler_emit(self, Op_ForCheck, 0, 0);

    size_t exitJump = Compiler_emitJump(self, Op_JumpIfFalse);

    Compiler_pushLoop(self, LoopKind_For);
    Compiler_parseBlock(self);

    /* increment: i = i + step */
    size_t incrementStart = self->code.count;
    Compiler_emit(self, Op_LoadLocal, (uint32_t)iSlot, 0);
    Compiler_emit(self, Op_LoadLocal, (uint32_t)stepSlot, 0);
    Compiler_emit(self, Op_Add, 0, 0);
    Compiler_emit(self, Op_StoreLocal, (uint32_t)iSlot, 0);

    /* Jump back to condition */
    Compiler_emit(self, Op_Jump, (uint32_t)conditionStart, 0);

    size_t afterLoop = self->code.count;
    Compiler_patchJump(self, exitJump);
    Compiler_popLoop(self, incrementStart, afterLoop);

    /* Consume 'end' 'for' */
    Compiler_consume(self, Token_End, "expected 'end' after for block");
    Compiler_consume(self, Token_For, "expected 'for' after 'end'");

    Compiler_popScope(self);
    PARSE_LEAVE;
}

static void
Compiler_checkLoopKind(Compiler *self, const char *keyword)
{
    if (self->loopCount == 0)
        Compiler_error(self, "exit/next outside of a loop");

    LoopKind innermost = self->loops[self->loopCount - 1].kind;
    LoopKind named;

    if (Compiler_match(self, Token_While))
        named = LoopKind_While;
    else if (Compiler_match(self, Token_Repeat))
        named = LoopKind_Repeat;
    else if (Compiler_match(self, Token_For))
        named = LoopKind_For;
    else{
        Compiler_error(self, "expected 'while', 'repeat', or 'for' after '%s'", keyword);
        return; /* unreachable */
    }

    if (named != innermost){
        const char *expectedKind = innermost == LoopKind_While   ? "while"
                                   : innermost == LoopKind_Repeat? "repeat"
                                                                  : "for";
        const char *namedKind = named == LoopKind_While   ? "while"
                                : named == LoopKind_Repeat? "repeat"
                                                           : "for";
        Compiler_error(self, "innermost loop is '%s', not '%s'", expectedKind, namedKind);
    }
}

static void
Compiler_parseExitStatement(Compiler *self)
{
    PARSE_ENTER;
    Compiler_checkLoopKind(self, "exit");
    FixupList_add(self, &self->exitFixups, Compiler_emitJump(self, Op_Jump));
    PARSE_LEAVE;
}

static void
Compiler_parseNextStatement(Compiler *self)
{
    PARSE_ENTER;
    Compiler_checkLoopKind(self, "next");
    FixupList_add(self, &self->nextFixups, Compiler_emitJump(self, Op_Jump));
    PARSE_LEAVE;
}

static void
Compiler_parseReturnStatement(Compiler *self)
{
    PARSE_ENTER;
    if (Compiler_check(self, Token_Newline) || Compiler_check(self, Token_EOF) ||
        Compiler_check(self, Token_End) || Compiler_check(self, Token_Else)){
        Compiler_emit(self, Op_PushNothing, 0, 0);
    } else
        Compiler_parseExpression(self);
    Compiler_emit(self, Op_Return, 0, 0);
    PARSE_LEAVE;
}


static void
Compiler_parseRaiseStatement(Compiler *self)
{
    PARSE_ENTER;
    /* 'raise' already consumed */

    /* Check if bare raise (next token is newline/end/else/on/EOF) */
    if (Compiler_check(self, Token_Newline) || Compiler_check(self, Token_EOF) ||
        Compiler_check(self, Token_End) || Compiler_check(self, Token_Else) ||
        Compiler_check(self, Token_On)){
        /* Bare raise — re-raise current error */
        if (self->errorLocalSlot < 0)
            Compiler_error(self, "raise without arguments is only valid inside an error handler");

        Compiler_emit(self, Op_LoadLocal, (uint32_t)self->errorLocalSlot, 0);
        Compiler_emit(self, Op_Raise, 0, 0);
        PARSE_LEAVE;
    }

    /* Raise with arguments: parse type expression */
    Compiler_parseExpression(self);

    /* Check for 'message' keyword (context-sensitive word, not a reserved keyword) */
    if (Compiler_check(self, Token_Word) &&
        Compiler_tokenAtom(self, Compiler_current(self)) == NDSBuiltinAtom_Message){
        Compiler_advance(self); /* consume 'message' */
        Compiler_parseExpression(self);

        if (Compiler_match(self, Token_With)){
            /* Type + message + value */
            Compiler_parseExpression(self);
        } else
            Compiler_emit(self, Op_PushNothing, 0, 0);
    } else{
        /* Type only — push empty message and nothing value */
        Compiler_emitStringConstant(self, "", 0);
        Compiler_emit(self, Op_PushNothing, 0, 0);
    }
    Compiler_emit(self, Op_MakeError, 0, 0);

    Compiler_emit(self, Op_Raise, 0, 0);
    PARSE_LEAVE;
}

static void
Compiler_parseFunctionDefinition(Compiler *self)
{
    PARSE_ENTER;
    /* 'function' already consumed */

    /* Parse function name */
    if (!Compiler_check(self, Token_Word))
        Compiler_error(self, "expected function name after 'function'");

    Token *nameTok = Compiler_current(self);
    NDSAtom nameAtom = Compiler_internName(self, nameTok->value.text.data, nameTok->value.text.length);
    Compiler_advance(self);

    /* Check for duplicate function definition */
    for (size_t i = 0; i < self->ctx->functionCount; i++){
        if (self->ctx->functions[i].name == nameAtom)
            Compiler_error(self, "duplicate function '%s'", Compiler_atomName(self, nameAtom));
    }
    if (Globals_find(self->ctx, nameAtom) != (size_t)-1)
        Compiler_error(self, "function '%s' conflicts with existing name", Compiler_atomName(self, nameAtom));

    /* Reserve the global slot so the name is resolvable during body
       compilation (enables self-recursion, Pascal-style). The actual
       function value is filled in after the body is compiled. */
    size_t globalSlot = Globals_ensureConst(self->ctx, nameAtom);
    if (globalSlot == (size_t)-1)
        Compiler_error(self, "out of memory");

    /* Parse parameter list */
    Compiler_consume(self, Token_LeftParen, "expected '(' after function name");

    NDSAtom params[COMPILER_MAX_LOCALS] = {0};
    int paramCount = 0;

    if (!Compiler_check(self, Token_RightParen)){
        do{
            if (!Compiler_check(self, Token_Word))
                Compiler_error(self, "expected parameter name");
            if (paramCount >= COMPILER_MAX_LOCALS)
                Compiler_error(self, "too many parameters");

            Token *paramTok = Compiler_current(self);
            params[paramCount] = Compiler_internName(self, paramTok->value.text.data, paramTok->value.text.length);
            paramCount++;
            Compiler_advance(self);
        } while (Compiler_match(self, Token_Comma));
    }

    Compiler_consume(self, Token_RightParen, "expected ')' after parameters");

    /* Save compiler state */
    InstructionBuffer savedCode = self->code;
    int savedLocalCount = self->localCount;
    int savedMaxLocalCount = self->maxLocalCount;
    int savedScopeDepth = self->scopeDepth;
    int savedLoopCount = self->loopCount;
    int savedErrorLocalSlot = self->errorLocalSlot;

    /* Initialize for function body */
    InstructionBuffer_init(&self->code, self->pool);
    if (!self->code.code)
        Compiler_error(self, "out of memory");
    self->localCount = 0;
    self->maxLocalCount = 0;
    self->scopeDepth = 1;
    self->loopCount = 0;
    self->errorLocalSlot = -1;

    /* Register params as locals (slot 0..paramCount-1). */
    for (int i = 0; i < paramCount; i++)
        Compiler_addLocal(self, params[i]);

    /* Emit Op_Reserve placeholder (patched after body+handler) */
    size_t reserveIdx = Compiler_emit(self, Op_Reserve, 0, 0);

    /* Parse function body — no scope wrap so locals persist for handler */
    Compiler_parseBlockContents(self);

    /* Check for optional 'on error' clause */
    size_t handlerIP = 0;
    int handlerErrorSlot = -1;

    if (Compiler_check(self, Token_On)){
        Compiler_emitReturnNothing(self);

        /* Record handler IP */
        handlerIP = self->code.count;

        /* Consume 'on' 'error' */
        Compiler_advance(self); /* consume 'on' */
        Compiler_consume(self, Token_Error, "expected 'error' after 'on'");

        /* Parse error variable name */
        if (!Compiler_check(self, Token_Word))
            Compiler_error(self, "expected error variable name after 'on error'");

        Token *errNameTok = Compiler_current(self);
        NDSAtom errNameAtom = Compiler_internName(self, errNameTok->value.text.data, errNameTok->value.text.length);
        Compiler_advance(self);

        /* Allocate error local at function scope */
        handlerErrorSlot = Compiler_addLocal(self, errNameAtom);
        self->errorLocalSlot = handlerErrorSlot;

        /* Parse handler body (its own block scope) */
        Compiler_parseBlock(self);
    }
    Compiler_emitReturnNothing(self);

    /* Consume 'end' 'function' */
    Compiler_consume(self, Token_End, "expected 'end' after function body");
    Compiler_consume(self, Token_Function, "expected 'function' after 'end'");

    /* Patch Op_Reserve with actual non-param locals */
    if (self->localCount > self->maxLocalCount)
        self->maxLocalCount = self->localCount;
    int extraLocals = self->maxLocalCount - paramCount;
    if (extraLocals < 0)
        extraLocals = 0;
    self->code.code[reserveIdx].a = (uint32_t)extraLocals;

    /* Copy bytecode and line info to context-owned storage */
    size_t codeCount = self->code.count;
    Instruction *funcCode = NDSAllocator_allocate(self->ctx->allocator, codeCount * sizeof(Instruction));
    LineInfo *funcLines = NDSAllocator_allocate(self->ctx->allocator, codeCount * sizeof(LineInfo));
    if (!funcCode || !funcLines)
        Compiler_error(self, "out of memory");
    memcpy(funcCode, self->code.code, codeCount * sizeof(Instruction));
    memcpy(funcLines, self->code.lines, codeCount * sizeof(LineInfo));

    /* Copy param atoms to context-owned storage */
    NDSAtom *paramsCopy = NULL;
    if (paramCount > 0){
        paramsCopy = NDSAllocator_allocate(self->ctx->allocator, (size_t)paramCount * sizeof(NDSAtom));
        if (!paramsCopy)
            Compiler_error(self, "out of memory");
        memcpy(paramsCopy, params, (size_t)paramCount * sizeof(NDSAtom));
    }

    /* Create function entry */
    Function func = {0};
    func.kind = FuncKind_Script;
    func.name = nameAtom;
    func.params = paramsCopy;
    func.paramCount = paramCount;
    func.as.script.code = funcCode;
    func.as.script.lines = funcLines;
    func.as.script.codeLength = codeCount;
    func.as.script.handlerIP = handlerIP;
    func.as.script.handlerErrorSlot = handlerErrorSlot;
    func.as.script.maxLocals = self->maxLocalCount;

    size_t funcIndex = FunctionTable_add(self->ctx, func);
    if (funcIndex == (size_t)-1)
        Compiler_error(self, "out of memory");

    /* Fill in the reserved global slot with the actual function value */
    self->ctx->globals[globalSlot].value = NDSValue_functionFromIndex(funcIndex);

    /* Restore compiler state */
    self->code = savedCode;
    self->localCount = savedLocalCount;
    self->maxLocalCount = savedMaxLocalCount;
    self->scopeDepth = savedScopeDepth;
    self->loopCount = savedLoopCount;
    self->errorLocalSlot = savedErrorLocalSlot;
    PARSE_LEAVE;
}

static void
Compiler_parseImport(Compiler *self)
{
    PARSE_ENTER;
    NDSContext *ctx = self->ctx;

    if (!ctx->scriptLoader)
        Compiler_error(self, "imports are not enabled");

    Token *tok = Compiler_current(self);
    if (tok->type != Token_String)
        Compiler_error(self, "expected string literal after 'import'");

    const char *nameData = tok->value.text.data;
    size_t nameLength = tok->value.text.length;
    Compiler_advance(self);

    /* Deduplicate import name via constant pool index. */
    size_t nameIndex = NDSContext_addStringConstant(ctx, nameData, nameLength);
    if (nameIndex == (size_t)-1)
        Compiler_error(self, "out of memory");

    /* Already imported? Silent skip. */
    for (size_t i = 0; i < ctx->importedNameCount; i++){
        if (ctx->importedNames[i] == nameIndex)
            PARSE_LEAVE;
    }

    /* Record as imported */
    if (ctx->importedNameCount >= MAX_IMPORTED_FILES)
        Compiler_error(self, "too many imported files");
    ctx->importedNames[ctx->importedNameCount++] = nameIndex;

    /* Make a null-terminated copy for the loader callback */
    char *nameCopy = NDSAllocator_allocate(self->pool, nameLength + 1);
    if (!nameCopy)
        Compiler_error(self, "out of memory");
    memcpy(nameCopy, nameData, nameLength);
    nameCopy[nameLength] = '\0';

    /* Call the loader to get a reader */
    char errorMessage[NDSMaxErrorMessageLength + 1] = {0};
    NDSReader *importReader = ctx->scriptLoader(ctx, nameCopy, errorMessage, ctx->scriptLoaderUserPointer);
    if (!importReader){
        if (errorMessage[0] != '\0')
            Compiler_error(self, "%s", errorMessage);
        else
            Compiler_error(self, "failed to load '%s'", nameCopy);
    }

    /* Consume the trailing newline (or accept EOF) before pushing */
    if (!Compiler_check(self, Token_EOF) && !Compiler_match(self, Token_Newline))
        Compiler_error(self, "expected newline after import");

    /* Push reader — imported tokens interpose transparently via fillToken */
    Compiler_pushReader(self, importReader);

    /* Set sourceAtom to the imported file's name */
    self->sourceAtom = NDSContext_internAtom(ctx, nameCopy);
    PARSE_LEAVE;
}

static bool
Compiler_parseProgram(Compiler *self)
{
    PARSE_ENTER;
    bool lastWasExpression = false;

    size_t reservePos = self->code.count;
    Compiler_emit(self, Op_Reserve, 0, 0);

    while (Compiler_match(self, Token_Newline))
        ;

    while (!Compiler_check(self, Token_EOF)){
        /* Pop the previous top-level expression result if more follows.
           Leaving the *last* expression's value on the stack lets the
           host read it (REPL-style use). */
        if (lastWasExpression)
            Compiler_emit(self, Op_Pop, 0, 0);
        lastWasExpression = false;

        Token *tok = Compiler_current(self);
        const GrammarRule *r = GrammarRule_get(tok->type);

        if (tok->type == Token_Import){
            Compiler_advance(self);
            Compiler_parseImport(self);
            while (Compiler_match(self, Token_Newline))
                ;
            continue;
        }
        if (tok->type == Token_Function){
            Compiler_advance(self);
            Compiler_parseFunctionDefinition(self);
        } else if (r->phase == StmtPhase_Initial)
            Compiler_error(self, "'my' is not allowed at the top level");
        else if (tok->type == Token_Exit)
            Compiler_error(self, "'exit' is not allowed at the top level");
        else if (tok->type == Token_Next)
            Compiler_error(self, "'next' is not allowed at the top level");
        else
            lastWasExpression = Compiler_dispatchStatement(self);

        if (!Compiler_check(self, Token_EOF) && !Compiler_match(self, Token_Newline))
            Compiler_error(self, "expected newline or end of input");

        while (Compiler_match(self, Token_Newline))
            ;
    }

    /* Patch the reserve instruction with actual local count */
    if (self->localCount > self->maxLocalCount)
        self->maxLocalCount = self->localCount;
    self->code.code[reservePos].a = (uint32_t)self->maxLocalCount;

    PARSE_RETURN(lastWasExpression);
}

static bool
Value_isTruthy(const NDSContext *ctx, NDSValue v)
{
    switch (v.type){
    case NDSValueType_Nothing:
        return false;
    case NDSValueType_Boolean:
        return v.as.boolean;
    case NDSValueType_Number:
        return v.as.number != 0.0;
    case NDSValueType_String:
        if (!v.as.object)
            return false;
        return ((StringData *)NDSObject_getExtraData(ctx, v.as.object))->length > 0;
    default:
        return true;
    }
}

static bool
Value_equals(NDSContext *ctx, NDSValue a, NDSValue b)
{
    if (a.type != b.type)
        return false;

    /* Pointer identity — short-circuit for same object (also prevents
       infinite recursion on cyclic data structures) */
    if (NDSContext_isManagedType(ctx, a) && a.as.object == b.as.object)
        return true;

    TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, a.type);
    VM_DISPATCH_SETUP(ctx, 2, return false);
    VM_DISPATCH_PUSH(ctx, a);
    VM_DISPATCH_PUSH(ctx, b);
    VM_DISPATCH_FRAME(ctx);
    NDSStatus status = desc->equals(ctx);
    NDSValue result;
    VM_DISPATCH_TEARDOWN(ctx, result);

    if (status != NDSStatus_OK)
        return false;
    return result.as.boolean;
}

static NDSSlotComparison
Value_compare(NDSContext *ctx, NDSValue a, NDSValue b)
{
    if (a.type != b.type)
        return NDSSlotComparison_Incomparable;
    TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, a.type);
    VM_DISPATCH_SETUP(ctx, 2, { return NDSSlotComparison_Incomparable; });
    VM_DISPATCH_PUSH(ctx, a);
    VM_DISPATCH_PUSH(ctx, b);
    VM_DISPATCH_FRAME(ctx);
    NDSStatus status = desc->compare(ctx);
    NDSValue result;
    VM_DISPATCH_TEARDOWN(ctx, result);
    if (status != NDSStatus_OK)
        return NDSSlotComparison_Incomparable;
    int cmp = (int)result.as.number;
    if (cmp < 0)
        return NDSSlotComparison_LessThan;
    if (cmp > 0)
        return NDSSlotComparison_GreaterThan;
    return NDSSlotComparison_Equal;
}

NDSSlotComparison
NDSContext_compareSlots(NDSContext *self, int slotA, int slotB)
{
    return Value_compare(self, NDSContext_getSlot(self, slotA), NDSContext_getSlot(self, slotB));
}

bool
NDSContext_slotsEqual(NDSContext *self, int slotA, int slotB)
{
    return Value_equals(self, NDSContext_getSlot(self, slotA), NDSContext_getSlot(self, slotB));
}

static NDSValue
Value_toNumber(NDSContext *ctx, NDSValue v)
{
    switch (v.type){
    case NDSValueType_Number:
        return v;
    case NDSValueType_Boolean:
        return NDSValue_numberFromDouble(v.as.boolean? 1.0 : 0.0);
    case NDSValueType_String:
        if (v.as.object){
            StringData *sd = (StringData *)NDSObject_getExtraData(ctx, v.as.object);
            double d = 0.0;
            if (Util_parseDouble(ctx, sd->data, sd->length, &d))
                return NDSValue_numberFromDouble(d);
        }
        return NDSValue_Nothing;
    default:
        return NDSValue_Nothing;
    }
}

static NDSValue
Value_toBoolean(const NDSContext *ctx, NDSValue v)
{
    return NDSValue_booleanFromBool(Value_isTruthy(ctx, v));
}

static bool
VM_push(NDSContext *ctx, NDSValue value)
{
    if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1))
        return false;
    ctx->stack[ctx->stackTop++] = value;
    return true;
}

static NDSValue
VM_pop(NDSContext *ctx)
{
    if (ctx->stackTop == 0)
        return NDSValue_Nothing;
    return ctx->stack[--ctx->stackTop];
}

static NDSValue
VM_peek(NDSContext *ctx)
{
    if (ctx->stackTop == 0)
        return NDSValue_Nothing;
    return ctx->stack[ctx->stackTop - 1];
}

static NDSStatus
Builtin_print(NDSContext *ctx, int argCount)
{
    if (NDSContext_ensureSlots(ctx, argCount + 1) != NDSStatus_OK)
        return NDSStatus_Error;
    int tmpSlot = argCount;

    for (int i = 0; i < argCount; i++){
        if (NDSContext_slotToString(ctx, i, tmpSlot) == NDSStatus_OK){
            StringData *s = NDSContext_getSlotStringData(ctx, tmpSlot);
            fwrite(s->data, 1, s->length, stdout);
        }
        if (i < argCount - 1)
            fputc(' ', stdout);
    }

    NDSContext_setSlotNothing(ctx, 0);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_println(NDSContext *ctx, int argCount)
{
    if (Builtin_print(ctx, argCount) != NDSStatus_OK)
        return NDSStatus_Error;
    fputc('\n', stdout);

    return NDSStatus_OK;
}

#define BUILTIN_ONE_ARG_MATH_FUNC(name, func)                                                      \
    static NDSStatus Builtin_##name(NDSContext *context, int argCount)                             \
    {                                                                                              \
        if (argCount < 1){                                                                         \
            NDSContext_setError(context, #name " requires a numeric argument");                    \
            return NDSStatus_Error;                                                                \
        }                                                                                          \
        NDSValue val = NDSContext_getSlot(context, 0);                                             \
        if (val.type != NDSValueType_Number){                                                      \
            NDSContext_setError(context, #name " requires a numeric argument");                    \
            return NDSStatus_Error;                                                                \
        }                                                                                          \
        NDSContext_setSlotNumber(context, 0, func(val.as.number));                                 \
        return NDSStatus_OK;                                                                       \
    }

BUILTIN_ONE_ARG_MATH_FUNC(abs, fabs)
BUILTIN_ONE_ARG_MATH_FUNC(floor, floor)
BUILTIN_ONE_ARG_MATH_FUNC(ceil, ceil)
BUILTIN_ONE_ARG_MATH_FUNC(round, round)
BUILTIN_ONE_ARG_MATH_FUNC(sin, sin)
BUILTIN_ONE_ARG_MATH_FUNC(cos, cos)
BUILTIN_ONE_ARG_MATH_FUNC(sqrt, sqrt)
BUILTIN_ONE_ARG_MATH_FUNC(tan, tan)
BUILTIN_ONE_ARG_MATH_FUNC(atan, atan)
BUILTIN_ONE_ARG_MATH_FUNC(asin, asin)
BUILTIN_ONE_ARG_MATH_FUNC(acos, acos)
BUILTIN_ONE_ARG_MATH_FUNC(log, log)
BUILTIN_ONE_ARG_MATH_FUNC(log10, log10)
BUILTIN_ONE_ARG_MATH_FUNC(exp, exp)

#define BUILTIN_TWO_ARG_MATH_FUNC(name, func)                                                      \
    static NDSStatus Builtin_##name(NDSContext *context, int argCount)                             \
    {                                                                                              \
        if (argCount < 2){                                                                         \
            NDSContext_setError(context, #name " requires two numeric arguments");                 \
            return NDSStatus_Error;                                                                \
        }                                                                                          \
        NDSValue val1 = NDSContext_getSlot(context, 0);                                            \
        if (val1.type != NDSValueType_Number){                                                     \
            NDSContext_setError(context, #name " requires two numeric arguments");                 \
            return NDSStatus_Error;                                                                \
        }                                                                                          \
        NDSValue val2 = NDSContext_getSlot(context, 1);                                            \
        if (val2.type != NDSValueType_Number){                                                     \
            NDSContext_setError(context, #name " requires two numeric arguments");                 \
            return NDSStatus_Error;                                                                \
        }                                                                                          \
        NDSContext_setSlotNumber(context, 0, func(val1.as.number, val2.as.number));                \
        return NDSStatus_OK;                                                                       \
    }

BUILTIN_TWO_ARG_MATH_FUNC(pow, pow)
BUILTIN_TWO_ARG_MATH_FUNC(atan2, atan2)

static NDSStatus
Builtin_clamp(NDSContext *context, int argCount)
{
    if (argCount < 3){
        NDSContext_setError(context, "clamp requires three numeric arguments");
        return NDSStatus_Error;
    }
    NDSValue val = NDSContext_getSlot(context, 0);
    NDSValue lo = NDSContext_getSlot(context, 1);
    NDSValue hi = NDSContext_getSlot(context, 2);
    if (val.type != NDSValueType_Number || lo.type != NDSValueType_Number ||
        hi.type != NDSValueType_Number){
        NDSContext_setError(context, "clamp requires three numeric arguments");
        return NDSStatus_Error;
    }
    double v = val.as.number;
    if (v < lo.as.number)
        v = lo.as.number;
    if (v > hi.as.number)
        v = hi.as.number;
    NDSContext_setSlotNumber(context, 0, v);
    return NDSStatus_OK;
}

/* PCG32 for PRNG */

#define PRNG_MULTIPLIER 6364136223846793005ULL
#define PRNG_INCREMENT 1442695040888963407ULL

static uint32_t
PRNG_next(uint64_t *state)
{
    uint64_t s = *state;
    *state = s * PRNG_MULTIPLIER + PRNG_INCREMENT;
    uint32_t xorshifted = (uint32_t)(((s >> 18u) ^ s) >> 27u);
    uint32_t rot = (uint32_t)(s >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

static void
PRNG_seed(uint64_t *state, uint64_t seed)
{
    *state = 0;
    PRNG_next(state);
    *state += seed;
    PRNG_next(state);
}

#define PRNG_DIVISOR 4294967296.0 /* 2^32 */
static NDSStatus
Builtin_random(NDSContext *context, int argCount)
{
    UNUSED(argCount);
    uint32_t r = PRNG_next(&context->randomState);
    NDSContext_setSlotNumber(context, 0, (double)r / PRNG_DIVISOR);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_setRandomSeed(NDSContext *context, int argCount)
{
    if (argCount < 1){
        NDSContext_setError(context, "random seed must be a numeric value");
        return NDSStatus_Error;
    }
    NDSValue val = NDSContext_getSlot(context, 0);
    if (val.type != NDSValueType_Number){
        NDSContext_setError(context, "random seed must be a numeric value");
        return NDSStatus_Error;
    }
    PRNG_seed(&context->randomState, (uint64_t)val.as.number);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_setRandomSeedFromTime(NDSContext *context, int argCount)
{
    UNUSED(argCount);
    PRNG_seed(&context->randomState, (uint64_t)time(NULL));
    return NDSStatus_OK;
}

static NDSStatus
Builtin_minMax(NDSContext *context, int argCount, NDSSlotComparison op)
{
    if (argCount == 0)
        return NDSStatus_OK; /* returns nothing */

    NDSContext_copySlot(context, 0, 0);
    for (int i = 1; i < argCount; i++){
        NDSSlotComparison cmp = NDSContext_compareSlots(context, i, 0);
        if (cmp == NDSSlotComparison_Incomparable){
            NDSContext_setError(context, "incomparable arguments");
            return NDSStatus_Error;
        }
        if (cmp == op)
            NDSContext_copySlot(context, i, 0);
    }

    return NDSStatus_OK;
}

static NDSStatus
Builtin_min(NDSContext *context, int argCount)
{
    return Builtin_minMax(context, argCount, NDSSlotComparison_LessThan);
}

static NDSStatus
Builtin_max(NDSContext *context, int argCount)
{
    return Builtin_minMax(context, argCount, NDSSlotComparison_GreaterThan);
}

static StringData *
Builtin_requireString(NDSContext *ctx, int slot, const char *name)
{
    NDSValue v = NDSContext_getSlot(ctx, slot);
    if (v.type != NDSValueType_String || !v.as.object){
        NDSContext_setErrorF(ctx, "%s requires a string argument", name);
        return NULL;
    }
    return (StringData *)NDSObject_getExtraData(ctx, v.as.object);
}

static NDSStatus
Builtin_caseConvert(NDSContext *ctx, int argCount, const char *name, int (*fn)(int))
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, name);
    if (!s)
        return NDSStatus_Error;

    char *buf = NDSAllocator_allocate(ctx->allocator, s->length + 1);
    if (!buf){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    for (size_t i = 0; i < s->length; i++)
        buf[i] = (char)fn((unsigned char)s->data[i]);
    buf[s->length] = '\0';
    NDSContext_setSlotString(ctx, 0, buf, s->length);
    NDSAllocator_free(ctx->allocator, buf);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_lowercase(NDSContext *ctx, int argCount)
{
    return Builtin_caseConvert(ctx, argCount, "lowercase", tolower);
}

static NDSStatus
Builtin_uppercase(NDSContext *ctx, int argCount)
{
    return Builtin_caseConvert(ctx, argCount, "uppercase", toupper);
}

static NDSStatus
Builtin_trim(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "trim");
    if (!s)
        return NDSStatus_Error;

    size_t start = 0;
    while (start < s->length && CharClass_isHSpace(s->data[start]))
        start++;
    size_t end = s->length;
    while (end > start && CharClass_isHSpace(s->data[end - 1]))
        end--;
    NDSContext_setSlotString(ctx, 0, s->data + start, end - start);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_startsWith(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "startsWith");
    if (!s)
        return NDSStatus_Error;
    StringData *prefix = Builtin_requireString(ctx, 1, "startsWith");
    if (!prefix)
        return NDSStatus_Error;

    bool result = prefix->length <= s->length && memcmp(s->data, prefix->data, prefix->length) == 0;
    NDSContext_setSlotBoolean(ctx, 0, result);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_endsWith(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "endsWith");
    if (!s)
        return NDSStatus_Error;
    StringData *suffix = Builtin_requireString(ctx, 1, "endsWith");
    if (!suffix)
        return NDSStatus_Error;

    bool result = suffix->length <= s->length &&
                  memcmp(s->data + s->length - suffix->length, suffix->data, suffix->length) == 0;
    NDSContext_setSlotBoolean(ctx, 0, result);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_substring(NDSContext *ctx, int argCount)
{
    StringData *s = Builtin_requireString(ctx, 0, "substring");
    if (!s)
        return NDSStatus_Error;

    if (argCount < 2 || NDSContext_getSlotType(ctx, 1) != NDSValueType_Number){
        NDSContext_setErrorF(ctx, "substring requires a string and a number, got '%s' and '%s'",
                             NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 0)),
                             NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 1)));
        return NDSStatus_Error;
    }

    /* 1-based start, optional length */
    double rawStart = NDSContext_getSlotNumber(ctx, 1);
    size_t start;
    if (!Util_resolveIndex(rawStart, s->length, &start)){
        NDSContext_setSlotString(ctx, 0, "", 0);
        return NDSStatus_OK;
    }

    size_t len = s->length - start;
    if (argCount >= 3 && NDSContext_getSlotType(ctx, 2) == NDSValueType_Number){
        double rawLen = NDSContext_getSlotNumber(ctx, 2);
        if (rawLen < 0)
            rawLen = 0;
        if ((size_t)rawLen < len)
            len = (size_t)rawLen;
    }

    NDSContext_setSlotString(ctx, 0, s->data + start, len);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_replace(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "replace");
    if (!s)
        return NDSStatus_Error;
    StringData *old = Builtin_requireString(ctx, 1, "replace");
    if (!old)
        return NDSStatus_Error;
    StringData *repl = Builtin_requireString(ctx, 2, "replace");
    if (!repl)
        return NDSStatus_Error;

    if (old->length == 0)
        return NDSStatus_OK;

    const char *found = Util_memmem(s->data, s->length, old->data, old->length);
    if (!found)
        return NDSStatus_OK;

    size_t prefix = (size_t)(found - s->data);
    size_t resultLen;
    if (!Util_safeAddSize(s->length - old->length, repl->length, &resultLen)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    char *buf = NDSAllocator_allocate(ctx->allocator, resultLen + 1);
    if (!buf){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }

    memcpy(buf, s->data, prefix);
    memcpy(buf + prefix, repl->data, repl->length);
    memcpy(buf + prefix + repl->length, found + old->length, s->length - prefix - old->length);
    buf[resultLen] = '\0';

    NDSContext_setSlotString(ctx, 0, buf, resultLen);
    NDSAllocator_free(ctx->allocator, buf);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_replaceAll(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "replaceAll");
    if (!s)
        return NDSStatus_Error;
    StringData *old = Builtin_requireString(ctx, 1, "replaceAll");
    if (!old)
        return NDSStatus_Error;
    StringData *repl = Builtin_requireString(ctx, 2, "replaceAll");
    if (!repl)
        return NDSStatus_Error;

    if (old->length == 0)
        return NDSStatus_OK; /* slot 0 already has the original string */

    /* Count occurrences to pre-compute result size */
    size_t count = 0;
    const char *p = s->data;
    size_t remaining = s->length;
    while ((p = Util_memmem(p, remaining, old->data, old->length)) != NULL){
        count++;
        p += old->length;
        remaining = s->length - (size_t)(p - s->data);
    }

    if (count == 0)
        return NDSStatus_OK; /* slot 0 already has the original string */

    /* Compute result length: remove all occurrences, add all replacements.
       count * old->length <= s->length is guaranteed (non-overlapping matches). */
    size_t removed = count * old->length;
    size_t added;
    if (!Util_safeMulSize(count, repl->length, &added)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    size_t resultLen;
    if (!Util_safeAddSize(s->length - removed, added, &resultLen)){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }
    char *buf = NDSAllocator_allocate(ctx->allocator, resultLen + 1);
    if (!buf){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }

    char *dst = buf;
    p = s->data;
    remaining = s->length;
    const char *found;
    while ((found = Util_memmem(p, remaining, old->data, old->length)) != NULL){
        size_t chunk = (size_t)(found - p);
        memcpy(dst, p, chunk);
        dst += chunk;
        memcpy(dst, repl->data, repl->length);
        dst += repl->length;
        p = found + old->length;
        remaining = s->length - (size_t)(p - s->data);
    }
    size_t tail = s->length - (size_t)(p - s->data);
    memcpy(dst, p, tail);
    buf[resultLen] = '\0';

    NDSContext_setSlotString(ctx, 0, buf, resultLen);
    NDSAllocator_free(ctx->allocator, buf);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_split(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    StringData *s = Builtin_requireString(ctx, 0, "split");
    if (!s)
        return NDSStatus_Error;
    StringData *delim = Builtin_requireString(ctx, 1, "split");
    if (!delim)
        return NDSStatus_Error;

    NDSContext_ensureSlots(ctx, argCount + 2);
    int tmpSlot = argCount;
    int strSlot = argCount + 1;

    if (delim->length == 0){
        /* Empty delimiter: return list with the original string */
        NDSContext_copySlot(ctx, 0, tmpSlot);
        NDSContext_setSlotNewList(ctx, 0);
        NDSContext_appendSlotListElement(ctx, 0, tmpSlot);
        return NDSStatus_OK;
    }

    /* Keep the original string rooted while we iterate over its data */
    NDSContext_copySlot(ctx, 0, strSlot);
    NDSContext_setSlotNewList(ctx, 0);

    const char *p = s->data;
    const char *end = s->data + s->length;
    const char *found;
    while ((found = Util_memmem(p, (size_t)(end - p), delim->data, delim->length)) != NULL){
        NDSContext_setSlotString(ctx, tmpSlot, p, (size_t)(found - p));
        NDSContext_appendSlotListElement(ctx, 0, tmpSlot);
        p = found + delim->length;
    }
    /* Remaining tail */
    NDSContext_setSlotString(ctx, tmpSlot, p, (size_t)(end - p));
    NDSContext_appendSlotListElement(ctx, 0, tmpSlot);

    return NDSStatus_OK;
}

static NDSStatus
Builtin_join(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_List){
        NDSContext_setErrorF(ctx, "join requires a list, got '%s'", NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 0)));
        return NDSStatus_Error;
    }
    StringData *delim = Builtin_requireString(ctx, 1, "join");
    if (!delim)
        return NDSStatus_Error;

    NDSAllocator *alloc = ctx->allocator;
    size_t listCount = NDSContext_getSlotListCount(ctx, 0);

    if (listCount == 0){
        NDSContext_setSlotString(ctx, 0, "", 0);
        return NDSStatus_OK;
    }

    /* Build result using StringBuf_grow */
    size_t cap = TOSTRING_INITIAL_CAPACITY;
    size_t pos = 0;
    char *buf = NDSAllocator_allocate(alloc, cap);
    if (!buf){
        NDSContext_setError(ctx, "out of memory");
        return NDSStatus_Error;
    }

    NDSContext_ensureSlots(ctx, argCount + 1);
    int tmpSlot = argCount;

    for (size_t i = 0; i < listCount; i++){
        if (i > 0 && delim->length > 0){
            if (!StringBuf_grow(alloc, &buf, &cap, pos + delim->length))
                break;
            memcpy(buf + pos, delim->data, delim->length);
            pos += delim->length;
        }
        NDSContext_getSlotListElement(ctx, 0, i, tmpSlot);
        if (NDSContext_slotToString(ctx, tmpSlot, tmpSlot) == NDSStatus_OK){
            StringData *sd = NDSContext_getSlotStringData(ctx, tmpSlot);
            if (!StringBuf_grow(alloc, &buf, &cap, pos + sd->length))
                break;
            memcpy(buf + pos, sd->data, sd->length);
            pos += sd->length;
        }
    }

    NDSContext_setSlotString(ctx, 0, buf, pos);
    NDSAllocator_free(alloc, buf);
    return NDSStatus_OK;
}

static NDSStatus
Builtin_char(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_Number){
        NDSContext_setErrorF(ctx, "char requires a number, got '%s'", NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 0)));
        return NDSStatus_Error;
    }
    double d = NDSContext_getSlotNumber(ctx, 0);
    if (d < 0 || d > 255){
        NDSContext_setError(ctx, "char argument must be 0-255");
        return NDSStatus_Error;
    }
    char c = (char)(unsigned char)d;
    NDSContext_setSlotString(ctx, 0, &c, 1);
    return NDSStatus_OK;
}

static void
Util_heapSort(NDSContext *ctx, NDSValue *items, size_t count)
{
    /* Sift down: restore max-heap property for items[start..end) */
#define SIFT_DOWN(start_, end_)                                                                    \
    do{                                                                                            \
        size_t parent_ = (start_);                                                                 \
        for (;;){                                                                                  \
            size_t child_ = parent_ * 2 + 1;                                                       \
            if (child_ >= (end_))                                                                  \
                break;                                                                             \
            if (child_ + 1 < (end_) && Value_compare(ctx, items[child_], items[child_ + 1]) ==     \
                                           NDSSlotComparison_LessThan)                             \
                child_++;                                                                          \
            if (Value_compare(ctx, items[parent_], items[child_]) != NDSSlotComparison_LessThan)   \
                break;                                                                             \
            NDSValue tmp_ = items[parent_];                                                        \
            items[parent_] = items[child_];                                                        \
            items[child_] = tmp_;                                                                  \
            parent_ = child_;                                                                      \
        }                                                                                          \
    } while (false)

    /* Build max-heap */
    for (size_t i = count / 2; i > 0; i--)
        SIFT_DOWN(i - 1, count);

    /* Extract max elements */
    for (size_t end = count; end > 1;){
        end--;
        NDSValue tmp = items[0];
        items[0] = items[end];
        items[end] = tmp;
        SIFT_DOWN(0, end);
    }

#undef SIFT_DOWN
}

static NDSStatus
Builtin_sort(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_List){
        NDSContext_setErrorF(ctx, "sort requires a list, got '%s'", NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 0)));
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);

    if (list->count > 1){
        /* Pre-check: cross-type comparison is always Incomparable, so a
           type check is sufficient to guarantee a total order — provided
           the type actually defines comparison (not default_compare). */
        NDSTypeID firstType = list->items[0].type;
        if (NDSContext_getTypeDesc(ctx, firstType)->compare == default_compare){
            NDSContext_setError(ctx, "sort requires comparable elements");
            return NDSStatus_Error;
        }
        for (size_t i = 1; i < list->count; i++){
            if (list->items[i].type != firstType){
                NDSContext_setError(ctx, "sort requires comparable elements");
                return NDSStatus_Error;
            }
        }
        Util_heapSort(ctx, list->items, list->count);
    }

    return NDSStatus_OK;
}

static NDSStatus
Builtin_reverse(NDSContext *ctx, int argCount)
{
    UNUSED(argCount);
    if (NDSContext_getSlotType(ctx, 0) != NDSValueType_List){
        NDSContext_setErrorF(ctx, "reverse requires a list, got '%s'", NDSContext_typeName(ctx, NDSContext_getSlotType(ctx, 0)));
        return NDSStatus_Error;
    }

    List *list = NDSContext_getSlotList(ctx, 0);

    if (list->count > 1){
        for (size_t i = 0, j = list->count - 1; i < j; i++, j--){
            NDSValue tmp = list->items[i];
            list->items[i] = list->items[j];
            list->items[j] = tmp;
        }
    }

    return NDSStatus_OK;
}

#define REGISTER_BUILTIN_FUNC(name)                                                                \
    do{                                                                                            \
        if (NDSConfigHandle_registerFunction(&handle, #name, Builtin_##name, 0) != NDSStatus_OK)   \
            return NDSStatus_Error;                                                                \
    } while (false)

static NDSStatus
NDSContext_registerStdlibRandom(NDSContext *self)
{
    NDSConfigHandle handle = { self };

    REGISTER_BUILTIN_FUNC(random);

    NDSPatternStep setRandomSeedCommand[] ={
        { NDSPatternStep_Word, "seed"},   { NDSPatternStep_Word, "random"},
        { NDSPatternStep_Word, "number"}, { NDSPatternStep_Word, "generator"},
        { NDSPatternStep_Word, "with"},   { NDSPatternStep_Expression, NULL},
        { NDSPatternStep_End, NULL }
    };
    if (NDSConfigHandle_registerCommand(&handle, setRandomSeedCommand, Builtin_setRandomSeed) != NDSStatus_OK)
        return NDSStatus_Error;

    NDSPatternStep setRandomSeedFromTimeCommand[] ={
        { NDSPatternStep_Word, "seed"},   { NDSPatternStep_Word, "random"},
        { NDSPatternStep_Word, "number"}, { NDSPatternStep_Word, "generator"},
        { NDSPatternStep_Word, "from"},   { NDSPatternStep_Word, "current"},
        { NDSPatternStep_Word, "time"},   { NDSPatternStep_End, NULL }
    };
    if (NDSConfigHandle_registerCommand(&handle, setRandomSeedFromTimeCommand, Builtin_setRandomSeedFromTime) != NDSStatus_OK)
        return NDSStatus_Error;

    return NDSStatus_OK;
}

static NDSStatus
NDSContext_setupStdlib(NDSContext *self)
{
    NDSConfigHandle handle = { self };

    REGISTER_BUILTIN_FUNC(print);
    REGISTER_BUILTIN_FUNC(println);

    REGISTER_BUILTIN_FUNC(abs);
    REGISTER_BUILTIN_FUNC(floor);
    REGISTER_BUILTIN_FUNC(ceil);
    REGISTER_BUILTIN_FUNC(round);
    REGISTER_BUILTIN_FUNC(sin);
    REGISTER_BUILTIN_FUNC(cos);
    REGISTER_BUILTIN_FUNC(sqrt);
    REGISTER_BUILTIN_FUNC(tan);
    REGISTER_BUILTIN_FUNC(atan);
    REGISTER_BUILTIN_FUNC(asin);
    REGISTER_BUILTIN_FUNC(acos);
    REGISTER_BUILTIN_FUNC(log);
    REGISTER_BUILTIN_FUNC(log10);
    REGISTER_BUILTIN_FUNC(exp);
    REGISTER_BUILTIN_FUNC(pow);
    REGISTER_BUILTIN_FUNC(atan2);
    REGISTER_BUILTIN_FUNC(clamp);

    REGISTER_BUILTIN_FUNC(min);
    REGISTER_BUILTIN_FUNC(max);

    REGISTER_BUILTIN_FUNC(char);

    REGISTER_BUILTIN_FUNC(sort);
    REGISTER_BUILTIN_FUNC(reverse);

    REGISTER_BUILTIN_FUNC(lowercase);
    REGISTER_BUILTIN_FUNC(uppercase);
    REGISTER_BUILTIN_FUNC(trim);
    REGISTER_BUILTIN_FUNC(startsWith);
    REGISTER_BUILTIN_FUNC(endsWith);
    REGISTER_BUILTIN_FUNC(substring);
    REGISTER_BUILTIN_FUNC(replace);
    REGISTER_BUILTIN_FUNC(replaceAll);
    REGISTER_BUILTIN_FUNC(split);
    REGISTER_BUILTIN_FUNC(join);
    if (NDSContext_registerStdlibRandom(self) != NDSStatus_OK)
        return NDSStatus_Error;

    return NDSStatus_OK;
}


#define LOCAL(base_, slot_) ctx->stack[(base_) + (size_t)(slot_)]

#define VM_BINARY_ARITH(op_symbol)                                                                 \
    do{                                                                                           \
        NDSValue b = VM_pop(ctx);                                                                  \
        NDSValue a = VM_pop(ctx);                                                                  \
        if (a.type != NDSValueType_Number || b.type != NDSValueType_Number){                      \
            NDSContext_setError(ctx, "arithmetic requires numeric operands");                      \
            VM_push(ctx, NDSValue_Nothing);                                                        \
            goto vm_error;                                                                         \
        }                                                                                          \
        VM_push(ctx, NDSValue_numberFromDouble(a.as.number op_symbol b.as.number));                \
    } while (false)

#define VM_COMPARE(cmp1, cmp2)                                                                     \
    do{                                                                                           \
        NDSValue b = VM_pop(ctx);                                                                  \
        NDSValue a = VM_pop(ctx);                                                                  \
        NDSSlotComparison cmp = Value_compare(ctx, a, b);                                          \
        if (cmp == NDSSlotComparison_Incomparable){                                               \
            NDSContext_setError(ctx, "comparison requires comparable operands");                   \
            VM_push(ctx, NDSValue_Nothing);                                                        \
            goto vm_error;                                                                         \
        }                                                                                          \
        VM_push(ctx, NDSValue_booleanFromBool(cmp == (cmp1) || cmp == (cmp2)));                    \
    } while (false)

void
NDSContext_setError(NDSContext *ctx, const char *message)
{
    if (ctx->errorMessage[0] == '\0')
        snprintf(ctx->errorMessage, sizeof(ctx->errorMessage), "%s", message);
}

void NDS_PRINTF(2, 3) NDSContext_setErrorF(NDSContext *ctx, const char *fmt, ...)
{
    if (ctx->errorMessage[0] != '\0')
        return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ctx->errorMessage, sizeof(ctx->errorMessage), fmt, ap);
    va_end(ap);
}

static bool
VM_newList(NDSContext *ctx, uint32_t count)
{
    if ((size_t)count > ctx->stackTop){
        NDSContext_setError(ctx, "out of memory");
        return false;
    }
    size_t base = ctx->stackTop - count;
    if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1)){
        NDSContext_setError(ctx, "stack overflow");
        ctx->stackTop = base;
        return false;
    }
    NDSValue *dest = &ctx->stack[ctx->stackTop];
    *dest = NDSValue_Nothing;
    ctx->stackTop++;
    NDSObject *listObj = List_new(ctx, dest);
    if (!listObj){
        NDSContext_setError(ctx, "out of memory");
        ctx->stackTop = base;
        return false;
    }
    if (count > 0){
        List *list = (List *)NDSObject_getExtraData(ctx, listObj);
        while (list->capacity < count){
            if (!List_grow(ctx, listObj)){
                NDSContext_setError(ctx, "out of memory");
                ctx->stackTop = base;
                return false;
            }
        }
        for (uint32_t i = 0; i < count; i++)
            list->items[i] = ctx->stack[base + i];
        list->count = count;
    }
    ctx->stack[base] = *dest;
    ctx->stackTop = base + 1;
    return true;
}

static bool
VM_newMap(NDSContext *ctx, uint32_t count)
{
    size_t pairCount = (size_t)count * 2;
    if (pairCount / 2 != count || pairCount > ctx->stackTop){
        NDSContext_setError(ctx, "out of memory");
        return false;
    }
    size_t base = ctx->stackTop - pairCount;
    if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1)){
        NDSContext_setError(ctx, "stack overflow");
        ctx->stackTop = base;
        return false;
    }
    NDSValue *dest = &ctx->stack[ctx->stackTop];
    *dest = NDSValue_Nothing;
    ctx->stackTop++;
    NDSObject *mapObj = NDSContext_newObject(ctx, NDSValueType_Map, dest);
    if (!mapObj){
        NDSContext_setError(ctx, "out of memory");
        ctx->stackTop = base;
        return false;
    }
    for (uint32_t i = 0; i < count; i++){
        NDSValue key = ctx->stack[base + i * 2];
        NDSValue value = ctx->stack[base + i * 2 + 1];
        if (Map_set(ctx, mapObj, key, value) < 0){
            NDSContext_setError(ctx, "out of memory");
            ctx->stackTop = base;
            return false;
        }
    }
    ctx->stack[base] = *dest;
    ctx->stackTop = base + 1;
    return true;
}

/* Stringify and concat the top two stack slots in place. Net effect: -1 slot. */
static bool
VM_concat(NDSContext *ctx)
{
    if (ctx->stackTop < 2)
        return false;

    int aSlot = (int)(ctx->stackTop - ctx->frameBase - 2);
    int bSlot = (int)(ctx->stackTop - ctx->frameBase - 1);

    if (NDSContext_slotToString(ctx, aSlot, aSlot) != NDSStatus_OK)
        return false;
    if (NDSContext_slotToString(ctx, bSlot, bSlot) != NDSStatus_OK)
        return false;

    StringData *sdA = NDSContext_getSlotStringData(ctx, aSlot);
    StringData *sdB = NDSContext_getSlotStringData(ctx, bSlot);

    size_t newLen = 0;
    if (!Util_safeAddSize(sdA->length, sdB->length, &newLen))
        return false;
    char *tmp = NDSAllocator_allocate(ctx->allocator, newLen + 1);
    if (!tmp)
        return false;
    memcpy(tmp, sdA->data, sdA->length);
    memcpy(tmp + sdA->length, sdB->data, sdB->length);
    tmp[newLen] = '\0';
    bool ok = NDSContext_newString(ctx, tmp, newLen, &ctx->stack[ctx->stackTop - 2]);
    NDSAllocator_free(ctx->allocator, tmp);
    if (!ok)
        return false;
    ctx->stackTop--;
    return true;
}

static NDSStatus
VM_callHost(NDSContext *ctx, Function *func, size_t callLocalBase, uint32_t argCount)
{
    size_t savedFrameBase = ctx->frameBase;
    ctx->frameBase = callLocalBase;

    /* Zero-arg functions need slot 0 for the return value */
    if (argCount == 0){
        if (!NDSContext_ensureStack(ctx, callLocalBase + 1)){
            ctx->frameBase = savedFrameBase;
            NDSContext_setError(ctx, "stack overflow");
            return NDSStatus_Error;
        }
        ctx->stack[callLocalBase] = NDSValue_Nothing;
        if (ctx->stackTop <= callLocalBase)
            ctx->stackTop = callLocalBase + 1;
    }

    NDSStatus status = func->as.host.callback(ctx, (int)argCount);

    NDSValue retVal = ctx->stack[callLocalBase]; /* slot 0 = return */
    ctx->stackTop = callLocalBase;
    ctx->frameBase = savedFrameBase;
    VM_push(ctx, retVal);

    return status;
}

static bool
VM_adjustArgs(NDSContext *ctx, Function *func, size_t callBase, uint32_t *argCount)
{
    while ((int)*argCount < func->paramCount){
        if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1))
            return false;
        ctx->stack[ctx->stackTop++] = NDSValue_Nothing;
        (*argCount)++;
    }
    if (func->kind == FuncKind_Script && (int)*argCount > func->paramCount){
        ctx->stackTop = callBase + (size_t)func->paramCount;
        *argCount = (uint32_t)func->paramCount;
    }
    return true;
}

static void
VM_setErrorMessageFromRaise(NDSContext *ctx, NDSValue error)
{
    if (error.type == NDSValueType_Error && error.as.object){
        NDSValue msgProp = NDSObject_getProperty(error.as.object, ErrorProp_Message);
        if (msgProp.type == NDSValueType_String && msgProp.as.object){
            StringData *sd = (StringData *)NDSObject_getExtraData(ctx, msgProp.as.object);
            snprintf(ctx->errorMessage, sizeof(ctx->errorMessage), "%.*s", (int)sd->length, sd->data);
        } else if (ctx->errorMessage[0] == '\0'){
            snprintf(ctx->errorMessage, sizeof(ctx->errorMessage), "error raised");
        }
    } else if (ctx->errorMessage[0] == '\0'){
        snprintf(ctx->errorMessage, sizeof(ctx->errorMessage), "error raised");
    }
}

static NDSStatus
VM_execute(NDSContext *ctx, const Instruction *code, const LineInfo *lines, size_t codeLength,
           size_t localBase, const VMCallInfo *info)
{
    ctx->callDepth++;
    if (ctx->callDepth > VM_MAX_CALL_DEPTH){
        NDSContext_setError(ctx, "stack overflow (too many nested calls)");
        ctx->callDepth--;
        return NDSStatus_Error;
    }
    size_t ip = 0;
    NDSValue error = NDSValue_Nothing; /* used by vm_error handler */
    size_t closureBase = info->closureBase;
    uint32_t block = info->block;
    size_t callerLocalBase = info->callerLocalBase;
    size_t handlerIP = info->handlerIP;
    bool handlerActive = info->handlerActive;
    int handlerErrorSlot = info->handlerErrorSlot;
    int maxLocals = info->maxLocals;

    while (ip < codeLength){
        const Instruction *inst = &code[ip];

        switch (inst->op){
        case Op_PushConst:
            if (!VM_push(ctx, NDSContext_getConstant(ctx, inst->a)))
                goto vm_error;
            break;
        case Op_PushNothing:
            if (!VM_push(ctx, NDSValue_Nothing))
                goto vm_error;
            break;
        case Op_Pop:
            VM_pop(ctx);
            break;
        case Op_Dup:
            if (!VM_push(ctx, VM_peek(ctx)))
                goto vm_error;
            break;
        case Op_Swap: {
            if (ctx->stackTop < 2){
                NDSContext_setError(ctx, "stack underflow");
                goto vm_error;
            }
            NDSValue top = ctx->stack[ctx->stackTop - 1];
            ctx->stack[ctx->stackTop - 1] = ctx->stack[ctx->stackTop - 2];
            ctx->stack[ctx->stackTop - 2] = top;
            break;
        }
        case Op_Add:
            VM_BINARY_ARITH(+);
            break;
        case Op_Sub:
            VM_BINARY_ARITH(-);
            break;
        case Op_Mul: {
            if (ctx->stackTop < 2){
                NDSContext_setError(ctx, "stack underflow");
                goto vm_error;
            }
            NDSValue a = ctx->stack[ctx->stackTop - 2];
            NDSValue b = ctx->stack[ctx->stackTop - 1];
            /* String * Number or Number * String → string repetition */
            if ((a.type == NDSValueType_String && b.type == NDSValueType_Number) ||
                (a.type == NDSValueType_Number && b.type == NDSValueType_String)){
                NDSValue strVal = a.type == NDSValueType_String? a : b;
                double rawCount = a.type == NDSValueType_Number? a.as.number : b.as.number;
                StringData *sd = (StringData *)NDSObject_getExtraData(ctx, strVal.as.object);
                size_t count = rawCount < 0? 0 : (size_t)rawCount;
                NDSValue *dest = &ctx->stack[ctx->stackTop - 2];
                bool ok;
                if (count == 0 || sd->length == 0){
                    ok = NDSContext_newString(ctx, "", 0, dest);
                } else{
                    size_t resultLen = sd->length * count;
                    if (resultLen / count != sd->length){
                        NDSContext_setError(ctx, "string repetition too large");
                        goto vm_error;
                    }
                    char *buf = NDSAllocator_allocate(ctx->allocator, resultLen + 1);
                    if (!buf){
                        NDSContext_setError(ctx, "out of memory");
                        goto vm_error;
                    }
                    for (size_t i = 0; i < count; i++)
                        memcpy(buf + i * sd->length, sd->data, sd->length);
                    buf[resultLen] = '\0';
                    ok = NDSContext_newString(ctx, buf, resultLen, dest);
                    NDSAllocator_free(ctx->allocator, buf);
                }
                if (!ok){
                    NDSContext_setError(ctx, "out of memory");
                    goto vm_error;
                }
                ctx->stackTop--;
            } else if (a.type == NDSValueType_Number && b.type == NDSValueType_Number){
                ctx->stack[ctx->stackTop - 2] = NDSValue_numberFromDouble(a.as.number * b.as.number);
                ctx->stackTop--;
            } else{
                NDSContext_setError(ctx, "arithmetic requires numeric operands");
                goto vm_error;
            }
            break;
        }
        case Op_Div: {
            NDSValue b = VM_pop(ctx);
            NDSValue a = VM_pop(ctx);
            if (a.type != NDSValueType_Number || b.type != NDSValueType_Number){
                NDSContext_setError(ctx, "arithmetic requires numeric operands");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            if (b.as.number == 0.0){
                NDSContext_setError(ctx, "division by zero");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            VM_push(ctx, NDSValue_numberFromDouble(a.as.number / b.as.number));
            break;
        }
        case Op_IntDiv: {
            NDSValue b = VM_pop(ctx);
            NDSValue a = VM_pop(ctx);
            if (a.type != NDSValueType_Number || b.type != NDSValueType_Number){
                NDSContext_setError(ctx, "integer division requires number operands");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            if (b.as.number == 0.0){
                NDSContext_setError(ctx, "division by zero");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            double result = a.as.number / b.as.number;
            result = result >= 0? floor(result) : ceil(result);
            VM_push(ctx, NDSValue_numberFromDouble(result));
            break;
        }
        case Op_Mod: {
            NDSValue b = VM_pop(ctx);
            NDSValue a = VM_pop(ctx);
            if (a.type != NDSValueType_Number || b.type != NDSValueType_Number){
                NDSContext_setError(ctx, "modulo requires number operands");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            if (b.as.number == 0.0){
                NDSContext_setError(ctx, "division by zero");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            VM_push(ctx, NDSValue_numberFromDouble(fmod(a.as.number, b.as.number)));
            break;
        }
        case Op_Negate: {
            NDSValue a = VM_pop(ctx);
            if (a.type != NDSValueType_Number){
                NDSContext_setErrorF(ctx, "cannot negate a value of type '%s'", NDSContext_typeName(ctx, a.type));
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            VM_push(ctx, NDSValue_numberFromDouble(-a.as.number));
            break;
        }
        case Op_Concat: {
            if (!VM_concat(ctx)){
                if (ctx->errorMessage[0] == '\0')
                    NDSContext_setError(ctx, "concatenation failed");
                goto vm_error;
            }
            break;
        }
        case Op_Not: {
            NDSValue a = VM_pop(ctx);
            VM_push(ctx, NDSValue_booleanFromBool(!Value_isTruthy(ctx, a)));
            break;
        }
        case Op_Eq: {
            NDSValue b = VM_pop(ctx);
            NDSValue a = VM_pop(ctx);
            VM_push(ctx, NDSValue_booleanFromBool(Value_equals(ctx, a, b)));
            break;
        }
        case Op_Neq: {
            NDSValue b = VM_pop(ctx);
            NDSValue a = VM_pop(ctx);
            VM_push(ctx, NDSValue_booleanFromBool(!Value_equals(ctx, a, b)));
            break;
        }
        case Op_Lt:
            VM_COMPARE(NDSSlotComparison_LessThan, NDSSlotComparison_LessThan);
            break;
        case Op_Gt:
            VM_COMPARE(NDSSlotComparison_GreaterThan, NDSSlotComparison_GreaterThan);
            break;
        case Op_LtEq:
            VM_COMPARE(NDSSlotComparison_LessThan, NDSSlotComparison_Equal);
            break;
        case Op_GtEq:
            VM_COMPARE(NDSSlotComparison_GreaterThan, NDSSlotComparison_Equal);
            break;
        case Op_Contains: {
            NDSValue needle = VM_pop(ctx);
            NDSValue container = VM_pop(ctx);

            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 2, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_PUSH(ctx, needle);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->contains(ctx);
            NDSValue result;
            VM_DISPATCH_TEARDOWN(ctx, result);
            if (status_ != NDSStatus_OK){
                VM_push(ctx, NDSValue_booleanFromBool(false));
            } else{
                VM_push(ctx, result);
            }
            break;
        }
        case Op_Cast: {
            if (ctx->stackTop == 0){
                NDSContext_setError(ctx, "stack underflow");
                goto vm_error;
            }
            NDSValue a = ctx->stack[ctx->stackTop - 1];
            NDSTypeID target = (NDSTypeID)inst->a;
            bool castOk = true;

            switch (target){
            case NDSValueType_String:
                if (a.type != NDSValueType_String){
                    int slot = (int)(ctx->stackTop - ctx->frameBase - 1);
                    if (NDSContext_slotToString(ctx, slot, slot) != NDSStatus_OK)
                        castOk = false;
                }
                break;
            case NDSValueType_Number: {
                NDSValue result = Value_toNumber(ctx, a);
                ctx->stack[ctx->stackTop - 1] = result;
                if (result.type == NDSValueType_Nothing)
                    castOk = false;
                break;
            }
            case NDSValueType_Boolean:
                ctx->stack[ctx->stackTop - 1] = Value_toBoolean(ctx, a);
                break;
            default:
                castOk = false;
                break;
            }

            if (!castOk){
                NDSContext_setErrorF(ctx, "cannot cast '%s' to '%s'",
                                     NDSContext_typeName(ctx, a.type),
                                     NDSContext_typeName(ctx, target));
                goto vm_error;
            }
            break;
        }
        case Op_Jump:
            ip = inst->a;
            continue;
        case Op_JumpIfFalse: {
            NDSValue a = VM_pop(ctx);
            if (!Value_isTruthy(ctx, a)){
                ip = inst->a;
                continue;
            }
            break;
        }
        case Op_JumpIfTrue: {
            NDSValue a = VM_pop(ctx);
            if (Value_isTruthy(ctx, a)){
                ip = inst->a;
                continue;
            }
            break;
        }
        case Op_LoadGlobal: {
            NDSValue val = NDSValue_Nothing;
            if (inst->a < ctx->globalCount)
                val = ctx->globals[inst->a].value;
            if (!VM_push(ctx, val))
                goto vm_error;
            break;
        }
        case Op_StoreGlobal: {
            NDSValue val = VM_pop(ctx);
            if (inst->a < ctx->globalCount){
                if (ctx->globals[inst->a].isConst){
                    NDSContext_setErrorF(ctx, "cannot assign to constant '%s'", NDSContext_atomName(ctx, ctx->globals[inst->a].name));
                    goto vm_error;
                }
                ctx->globals[inst->a].value = val;
            }
            break;
        }
        case Op_LoadLocal: {
            NDSValue val = LOCAL(localBase, inst->a);
            if (!VM_push(ctx, val))
                goto vm_error;
            break;
        }
        case Op_StoreLocal: {
            NDSValue val = VM_pop(ctx);
            LOCAL(localBase, inst->a) = val;
            break;
        }
        case Op_ForCheck: {
            NDSValue step = VM_pop(ctx);
            NDSValue limit = VM_pop(ctx);
            NDSValue counter = VM_pop(ctx);
            if (counter.type != NDSValueType_Number || limit.type != NDSValueType_Number ||
                step.type != NDSValueType_Number){
                NDSContext_setError(ctx, "for loop requires numeric values");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            if (step.as.number == 0.0){
                NDSContext_setError(ctx, "for loop step cannot be zero");
                VM_push(ctx, NDSValue_Nothing);
                goto vm_error;
            }
            bool inRange = false;
            if (step.as.number > 0)
                inRange = counter.as.number <= limit.as.number;
            else
                inRange = counter.as.number >= limit.as.number;
            VM_push(ctx, NDSValue_booleanFromBool(inRange));
            break;
        }
        case Op_Call: {
            uint32_t argCount = inst->a;
            NDSValue funcVal = VM_pop(ctx);

            if (funcVal.type != NDSValueType_Function){
                NDSContext_setErrorF(ctx, "cannot call a value of type '%s'", NDSContext_typeName(ctx, funcVal.type));
                goto vm_error;
            }

            uint32_t funcIdx = (uint32_t)funcVal.as.integer;
            size_t callLocalBase = ctx->stackTop - argCount;

            if (funcIdx >= ctx->functionCount){
                NDSContext_setError(ctx, "call to undefined function");
                ctx->stackTop = callLocalBase;
                goto vm_error;
            }

            Function *func = &ctx->functions[funcIdx];

            if (!VM_adjustArgs(ctx, func, callLocalBase, &argCount)){
                ctx->stackTop = callLocalBase;
                goto vm_error;
            }

            if (func->kind == FuncKind_Host){
                if (VM_callHost(ctx, func, callLocalBase, argCount) != NDSStatus_OK)
                    goto vm_error;
            } else{
                /* Script function call — recursive */
                VMCallInfo callInfo ={
                    .closureBase = 0,
                    .block = inst->b,
                    .callerLocalBase = localBase,
                    .handlerIP = func->as.script.handlerIP,
                    .handlerActive = false,
                    .handlerErrorSlot = func->as.script.handlerErrorSlot,
                    .maxLocals = func->as.script.maxLocals,
                };
                NDSStatus status = VM_execute(ctx, func->as.script.code, func->as.script.lines,
                                              func->as.script.codeLength, callLocalBase, &callInfo);
                if (status != NDSStatus_OK){
                    ctx->stackTop = callLocalBase;
                    goto vm_error;
                }
            }
            break;
        }
        case Op_Return: {
            NDSValue retVal = VM_pop(ctx);
            ctx->stackTop = localBase;
            VM_push(ctx, retVal);
            ctx->callDepth--;
            return NDSStatus_OK;
        }
        case Op_NewList:
            if (!VM_newList(ctx, inst->a))
                goto vm_error;
            break;
        case Op_NewMap:
            if (!VM_newMap(ctx, inst->a))
                goto vm_error;
            break;
        case Op_PropertyGet: {
            NDSValue container = VM_pop(ctx);
            NDSAtom propAtom = (NDSAtom)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 1, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->property_get(ctx, propAtom);
            NDSValue result;
            VM_DISPATCH_TEARDOWN(ctx, result);
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "property access failed");
                goto vm_error;
            }
            VM_push(ctx, result);
            break;
        }
        case Op_ChunkGet: {
            NDSValue container = VM_pop(ctx);
            NDSValue index = VM_pop(ctx);
            NDSChunkID chunkID = (NDSChunkID)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 2, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_PUSH(ctx, index);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->chunk_get(ctx, chunkID);
            NDSValue result;
            VM_DISPATCH_TEARDOWN(ctx, result);
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "chunk access failed");
                goto vm_error;
            }
            VM_push(ctx, result);
            break;
        }
        case Op_ChunkGetRange: {
            /* stack: [start, end, container] — container on top */
            NDSValue container = VM_pop(ctx);
            NDSValue end = VM_pop(ctx);
            NDSValue start = VM_pop(ctx);
            NDSChunkID chunkID = (NDSChunkID)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 3, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_PUSH(ctx, start);
            VM_DISPATCH_PUSH(ctx, end);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->chunk_get_range(ctx, chunkID);
            NDSValue result;
            VM_DISPATCH_TEARDOWN(ctx, result);
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "range chunk access failed");
                goto vm_error;
            }
            VM_push(ctx, result);
            break;
        }
        case Op_ChunkGetEvery: {
            NDSValue container = VM_pop(ctx);
            NDSChunkID chunkID = (NDSChunkID)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 1, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->chunk_get_every(ctx, chunkID);
            NDSValue result;
            VM_DISPATCH_TEARDOWN(ctx, result);
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "chunk access failed");
                goto vm_error;
            }
            VM_push(ctx, result);
            break;
        }
        case Op_ListAppend: {
            NDSValue value = VM_pop(ctx);
            NDSValue listVal = VM_peek(ctx); /* list stays on stack */
            if (listVal.type != NDSValueType_List){
                NDSContext_setErrorF(ctx, "append requires a list, got '%s'", NDSContext_typeName(ctx, VM_peek(ctx).type));
                goto vm_error;
            }
            if (!List_append(ctx, listVal.as.object, value)){
                NDSContext_setError(ctx, "out of memory");
                goto vm_error;
            }
            break;
        }
        case Op_Reserve: {
            uint32_t count = inst->a;
            if (count > 0){
                if (!NDSContext_ensureStack(ctx, ctx->stackTop + count))
                    goto vm_error;
                memset(&ctx->stack[ctx->stackTop], 0, count * sizeof(NDSValue));
                ctx->stackTop += count;
            }
            break;
        }
        case Op_ChunkSet: {
            /* .a = chunkID; stack: [container, index, value] */
            NDSValue value = VM_pop(ctx);
            NDSValue index = VM_pop(ctx);
            NDSValue container = VM_pop(ctx);
            NDSChunkID chunkID = (NDSChunkID)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 3, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_PUSH(ctx, index);
            VM_DISPATCH_PUSH(ctx, value);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->chunk_set(ctx, chunkID);
            NDSValue discarded;
            VM_DISPATCH_TEARDOWN(ctx, discarded);
            (void)discarded;
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "chunk assignment failed");
                goto vm_error;
            }
            break;
        }
        case Op_ChunkDelete: {
            /* .a = chunkID; stack: [container, index] */
            NDSValue index = VM_pop(ctx);
            NDSValue container = VM_pop(ctx);
            NDSChunkID chunkID = (NDSChunkID)inst->a;
            TypeDescriptor *desc = NDSContext_getTypeDesc(ctx, container.type);
            VM_DISPATCH_SETUP(ctx, 2, goto vm_error);
            VM_DISPATCH_PUSH(ctx, container);
            VM_DISPATCH_PUSH(ctx, index);
            VM_DISPATCH_FRAME(ctx);
            NDSStatus status_ = desc->chunk_delete(ctx, chunkID);
            NDSValue discarded;
            VM_DISPATCH_TEARDOWN(ctx, discarded);
            (void)discarded;
            if (status_ != NDSStatus_OK){
                NDSContext_setError(ctx, "chunk deletion failed");
                goto vm_error;
            }
            break;
        }
        case Op_MakeError: {
            if (ctx->stackTop < 3){
                NDSContext_setError(ctx, "stack underflow");
                goto vm_error;
            }
            NDSValue type = ctx->stack[ctx->stackTop - 3];
            NDSValue message = ctx->stack[ctx->stackTop - 2];
            NDSValue value = ctx->stack[ctx->stackTop - 1];
            if (!Error_new(ctx, type, message, value, &ctx->stack[ctx->stackTop - 3]))
                goto vm_error;
            ctx->stackTop -= 2;
            break;
        }
        case Op_Raise: {
            ctx->currentError = VM_pop(ctx);
            VM_setErrorMessageFromRaise(ctx, ctx->currentError);
            goto vm_error;
        }
        case Op_CallGiven: {
            uint32_t argCount = inst->a;
            uint32_t blockFuncIdx = block;

            if (blockFuncIdx == NDS_NO_BLOCK){
                NDSContext_setError(ctx, "no block provided");
                goto vm_error;
            }

            if (blockFuncIdx >= ctx->functionCount){
                NDSContext_setError(ctx, "invalid block function");
                goto vm_error;
            }

            Function *func = &ctx->functions[blockFuncIdx];
            size_t callLocalBase = ctx->stackTop - argCount;

            if (!VM_adjustArgs(ctx, func, callLocalBase, &argCount)){
                ctx->stackTop = callLocalBase;
                goto vm_error;
            }

            VMCallInfo blockCallInfo ={
                .closureBase = callerLocalBase,
                .block = NDS_NO_BLOCK,
                .callerLocalBase = 0,
                .handlerIP = func->as.script.handlerIP,
                .handlerActive = false,
                .handlerErrorSlot = func->as.script.handlerErrorSlot,
                .maxLocals = func->as.script.maxLocals,
            };
            NDSStatus status = VM_execute(ctx, func->as.script.code, func->as.script.lines,
                                          func->as.script.codeLength, callLocalBase, &blockCallInfo);
            if (status != NDSStatus_OK){
                ctx->stackTop = callLocalBase;
                goto vm_error;
            }
            break;
        }
        case Op_BlockGiven:
            if (!VM_push(ctx, NDSValue_booleanFromBool(block != NDS_NO_BLOCK)))
                goto vm_error;
            break;
        case Op_LoadOuter: {
            NDSValue val = LOCAL(closureBase, inst->a);
            if (!VM_push(ctx, val))
                goto vm_error;
            break;
        }
        case Op_StoreOuter: {
            NDSValue val = VM_pop(ctx);
            LOCAL(closureBase, inst->a) = val;
            break;
        }
        case Op_ShallowCopy: {
            if (ctx->stackTop == 0){
                NDSContext_setError(ctx, "stack underflow");
                goto vm_error;
            }
            NDSValue val = ctx->stack[ctx->stackTop - 1];
            if (val.type == NDSValueType_List && val.as.object){
                if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1)){
                    NDSContext_setError(ctx, "stack overflow");
                    goto vm_error;
                }
                NDSValue *dest = &ctx->stack[ctx->stackTop];
                *dest = NDSValue_Nothing;
                ctx->stackTop++;
                NDSObject *newObj = List_new(ctx, dest);
                if (!newObj){
                    NDSContext_setError(ctx, "out of memory");
                    goto vm_error;
                }
                List *src = (List *)NDSObject_getExtraData(ctx, ctx->stack[ctx->stackTop - 2].as.object);
                size_t count = src->count;
                for (size_t i = 0; i < count; i++){
                    src = (List *)NDSObject_getExtraData(ctx, ctx->stack[ctx->stackTop - 2].as.object);
                    if (!List_append(ctx, newObj, src->items[i])){
                        NDSContext_setError(ctx, "out of memory");
                        goto vm_error;
                    }
                }
                val = *dest;
                ctx->stackTop--;
            } else if (val.type == NDSValueType_Map && val.as.object){
                if (!NDSContext_ensureStack(ctx, ctx->stackTop + 1)){
                    NDSContext_setError(ctx, "stack overflow");
                    goto vm_error;
                }
                NDSValue *dest = &ctx->stack[ctx->stackTop];
                *dest = NDSValue_Nothing;
                ctx->stackTop++;
                NDSObject *newObj = NDSContext_newObject(ctx, NDSValueType_Map, dest);
                if (!newObj){
                    NDSContext_setError(ctx, "out of memory");
                    goto vm_error;
                }
                Map *src = (Map *)NDSObject_getExtraData(ctx, ctx->stack[ctx->stackTop - 2].as.object);
                size_t used = src->used;
                for (size_t i = 0; i < used; i++){
                    src = (Map *)NDSObject_getExtraData(ctx, ctx->stack[ctx->stackTop - 2].as.object);
                    if (src->keys[i].type == NDSValueType_Nothing)
                        continue;
                    if (Map_set(ctx, newObj, src->keys[i], src->values[i]) < 0){
                        NDSContext_setError(ctx, "out of memory");
                        goto vm_error;
                    }
                }
                val = *dest;
                ctx->stackTop--;
            }
            /* Non-container types fall through unchanged. */
            ctx->stack[ctx->stackTop - 1] = val;
            break;
        }
        default:
            NDSContext_setError(ctx, "invalid opcode");
            goto vm_error;
        }

        ip++;
        continue;

    vm_error:
        if (ip < codeLength && lines[ip].line > 0){
            ctx->errorSourceAtom = lines[ip].source;
            ctx->errorLine = lines[ip].line;
        }

        /* Synthesize error value if needed */
        error = ctx->currentError;
        if (error.type == NDSValueType_Nothing &&
            NDSContext_ensureStack(ctx, ctx->stackTop + 2)){
            NDSValue *typeRoot = &ctx->stack[ctx->stackTop];
            NDSValue *msgRoot = &ctx->stack[ctx->stackTop + 1];
            *typeRoot = NDSValue_Nothing;
            *msgRoot = NDSValue_Nothing;
            ctx->stackTop += 2;
            bool typeOk = NDSContext_newString(ctx, "hostError", sizeof("hostError") - 1, typeRoot);
            bool msgOk = NDSContext_newString(ctx, ctx->errorMessage, strlen(ctx->errorMessage), msgRoot);
            if (typeOk && msgOk)
                Error_new(ctx, *typeRoot, *msgRoot, NDSValue_Nothing, &ctx->currentError);
            ctx->stackTop -= 2;
            error = ctx->currentError;
        }

        /* Check this function's handler */
        if (handlerIP != 0 && !handlerActive){
            ctx->stackTop = localBase + (size_t)maxLocals;
            LOCAL(localBase, handlerErrorSlot) = error;
            ctx->currentError = NDSValue_Nothing;
            ctx->errorMessage[0] = '\0';
            ip = handlerIP;
            handlerActive = true;
            continue;
        }

        /* Keep error in currentError for caller's handler.
           Restore errorMessage for top-level reporting. */
        ctx->currentError = error;
        if (error.type == NDSValueType_Error && error.as.object){
            NDSValue msgProp = NDSObject_getProperty(error.as.object, ErrorProp_Message);
            if (msgProp.type == NDSValueType_String && msgProp.as.object){
                StringData *sd = (StringData *)NDSObject_getExtraData(ctx, msgProp.as.object);
                snprintf(ctx->errorMessage, sizeof(ctx->errorMessage), "%.*s", (int)sd->length, sd->data);
            }
        }
        ctx->callDepth--;
        return NDSStatus_Error;
    }

    ctx->callDepth--;
    return NDSStatus_OK;
}

#undef VM_BINARY_ARITH
#undef VM_COMPARE

NDSReader *
NDSDefaultFileReader(NDSContext *ctx, const char *path, char *errorMessage, void *userPointer)
{
    UNUSED(userPointer);
    NDSReader *reader = NDSContext_newFileReader(ctx, path);
    if (!reader){
        snprintf(errorMessage, NDSMaxErrorMessageLength + 1, "could not open file: %s", path);
        return NULL;
    }
    return reader;
}

void
NDSConfigHandle_enableImport(NDSConfigHandle *h, NDSScriptLoaderFn loader, void *userPointer)
{
    NDSContext *ctx = h->ctx;
    ctx->scriptLoader = loader;
    ctx->scriptLoaderUserPointer = userPointer;
}

/* Public API */

static Instruction *
NDSContext_compileReader(NDSContext *self, NDSReader *reader, NDSAtom sourceAtom,
                         size_t *outCodeLength, LineInfo **outLines)
{
    /* Heap-allocate the compiler so it survives longjmp with defined values.
       The volatile pointer ensures the compiler address itself is preserved. */
    Compiler *volatile compiler = NDSAllocator_allocateZeroed(self->allocator, sizeof(Compiler));
    if (!compiler){
        NDSReader_destroy(reader);
        NDSContext_setError(self, "out of memory");
        return NULL;
    }

    if (!Compiler_init(compiler, self, reader, sourceAtom)){
        NDSAllocator_free(self->allocator, (void *)compiler);
        NDSReader_destroy(reader);
        NDSContext_setError(self, "out of memory");
        return NULL;
    }

    if (setjmp(compiler->errorJump) != 0){
        Compiler_cleanup(compiler);
        NDSAllocator_free(self->allocator, (void *)compiler);
        NDSReader_destroy(reader);
        return NULL;
    }

    Compiler_fillToken(compiler);

    bool lastWasExpression = Compiler_parseProgram(compiler);

    if (!lastWasExpression)
        Compiler_emit(compiler, Op_PushNothing, 0, 0);

    Compiler_emit(compiler, Op_Return, 0, 0);

    /* Copy bytecode and line info out of the compiler pool before destroying it */
    size_t codeCount = compiler->code.count;
    Instruction *code = NDSAllocator_allocate(self->allocator, codeCount * sizeof(Instruction));
    LineInfo *lines = NDSAllocator_allocate(self->allocator, codeCount * sizeof(LineInfo));
    if (!code || !lines){
        NDSAllocator_free(self->allocator, code);
        NDSAllocator_free(self->allocator, lines);
        Compiler_cleanup(compiler);
        NDSAllocator_free(self->allocator, (void *)compiler);
        NDSReader_destroy(reader);
        NDSContext_setError(self, "out of memory");
        return NULL;
    }
    memcpy(code, compiler->code.code, codeCount * sizeof(Instruction));
    memcpy(lines, compiler->code.lines, codeCount * sizeof(LineInfo));

    Compiler_cleanup(compiler);
    NDSAllocator_free(self->allocator, (void *)compiler);
    NDSReader_destroy(reader);

    *outCodeLength = codeCount;
    *outLines = lines;
    return code;
}

/*
 * Internal: execute compiled bytecode. On success, result is in slot 0.
 */
static NDSStatus
NDSContext_executeCode(NDSContext *self, const Instruction *code, const LineInfo *lines, size_t codeLength)
{
    size_t savedTop = self->stackTop;
    VMCallInfo topInfo ={
        .closureBase = 0,
        .block = NDS_NO_BLOCK,
        .callerLocalBase = 0,
        .handlerIP = 0,
        .handlerActive = false,
        .handlerErrorSlot = -1,
        .maxLocals = 0,
    };
    NDSStatus result = VM_execute(self, code, lines, codeLength, savedTop, &topInfo);

    /* Ensure every error has a message */
    if (result != NDSStatus_OK && self->errorMessage[0] == '\0')
        snprintf(self->errorMessage, sizeof(self->errorMessage), "runtime error");

    /* Ensure exactly one result on stack relative to savedTop */
    if (self->stackTop <= savedTop){
        if (!NDSContext_ensureStack(self, savedTop + 1))
            return NDSStatus_Error;
        self->stack[savedTop] = NDSValue_Nothing;
        self->stackTop = savedTop + 1;
    }

    /* Move result to slot 0 (frameBase + 0) */
    self->stack[self->frameBase] = self->stack[self->stackTop - 1];
    self->stackTop = savedTop + 1;

    return result;
}

#define CLEAR_ERROR_STATE(ctx)                                                                     \
    do{                                                                                           \
        (ctx)->errorMessage[0] = '\0';                                                             \
        (ctx)->formattedError[0] = '\0';                                                           \
        (ctx)->errorLine = 0;                                                                      \
        (ctx)->errorSourceAtom = NDSAtom_Invalid;                                                  \
    } while (false)

NDSStatus
NDSContext_evaluateReader(NDSContext *self, NDSReader *reader, const char *sourceName)
{
    CLEAR_ERROR_STATE(self);

    self->interpreterDepth++;
    if (self->interpreterDepth > VM_MAX_INTERPRETER_DEPTH){
        NDSContext_setError(self, "interpreter recursion limit exceeded");
        NDSReader_destroy(reader);
        self->interpreterDepth--;
        return NDSStatus_Error;
    }

    if (!sourceName)
        sourceName = "(evaluate)";
    NDSAtom sourceAtom = NDSContext_internAtom(self, sourceName);

    size_t codeLength = 0;
    LineInfo *lines = NULL;
    Instruction *code = NDSContext_compileReader(self, reader, sourceAtom, &codeLength, &lines);
    /* reader is consumed by NDSContext_compileReader */

    if (!code){
        self->interpreterDepth--;
        return NDSStatus_Error;
    }

    NDSStatus result = NDSContext_executeCode(self, code, lines, codeLength);
    NDSAllocator_free(self->allocator, code);
    NDSAllocator_free(self->allocator, lines);

    self->interpreterDepth--;
    return result;
}

NDSStatus
NDSContext_evaluate(NDSContext *self, const char *source, size_t length, const char *sourceName)
{
    CLEAR_ERROR_STATE(self);

    NDSReader *reader = NDSContext_newStringReader(self, source, length);
    if (!reader){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    return NDSContext_evaluateReader(self, reader, sourceName);
}

NDSStatus
NDSContext_evaluateFile(NDSContext *self, const char *fileName)
{
    CLEAR_ERROR_STATE(self);

    NDSReader *reader = NDSContext_newFileReader(self, fileName);
    if (!reader){
        NDSContext_setErrorF(self, "could not open file: %s", fileName);
        return NDSStatus_Error;
    }
    return NDSContext_evaluateReader(self, reader, fileName);
}

NDSScript *
NDSContext_compile(NDSContext *self, const char *source, size_t length, const char *sourceName)
{
    CLEAR_ERROR_STATE(self);

    self->interpreterDepth++;
    if (self->interpreterDepth > VM_MAX_INTERPRETER_DEPTH){
        NDSContext_setError(self, "interpreter recursion limit exceeded");
        self->interpreterDepth--;
        return NULL;
    }

    if (!sourceName)
        sourceName = "(compile)";
    NDSAtom sourceAtom = NDSContext_internAtom(self, sourceName);

    NDSReader *reader = NDSContext_newStringReader(self, source, length);
    if (!reader){
        NDSContext_setError(self, "out of memory");
        self->interpreterDepth--;
        return NULL;
    }

    size_t codeLength = 0;
    LineInfo *lines = NULL;
    Instruction *code = NDSContext_compileReader(self, reader, sourceAtom, &codeLength, &lines);

    self->interpreterDepth--;

    if (!code)
        return NULL;

    NDSScript *script = NDSAllocator_allocate(self->allocator, sizeof(NDSScript));
    if (!script){
        NDSAllocator_free(self->allocator, code);
        NDSAllocator_free(self->allocator, lines);
        NDSContext_setError(self, "out of memory");
        return NULL;
    }
    script->code = code;
    script->lines = lines;
    script->codeLength = codeLength;
    script->allocator = self->allocator;
    return script;
}

NDSStatus
NDSContext_executeScript(NDSContext *self, const NDSScript *script)
{
    CLEAR_ERROR_STATE(self);

    self->interpreterDepth++;
    if (self->interpreterDepth > VM_MAX_INTERPRETER_DEPTH){
        NDSContext_setError(self, "interpreter recursion limit exceeded");
        self->interpreterDepth--;
        return NDSStatus_Error;
    }

    NDSStatus result = NDSContext_executeCode(self, script->code, script->lines, script->codeLength);

    self->interpreterDepth--;
    return result;
}

void
NDSScript_free(NDSScript *script)
{
    if (!script)
        return;
    NDSAllocator_free(script->allocator, script->code);
    NDSAllocator_free(script->allocator, script->lines);
    NDSAllocator_free(script->allocator, script);
}

const char *
NDSContext_getError(NDSContext *self)
{
    if (self->errorMessage[0] == '\0')
        return NULL;
    if (self->errorSourceAtom != NDSAtom_Invalid && self->errorLine > 0){
        snprintf(self->formattedError, sizeof(self->formattedError), "%s:%zu: %s",
                 ((StringData *)NDSObject_getExtraData(self, self->atoms[self->errorSourceAtom].as.object))->data,
                 self->errorLine, self->errorMessage);
        return self->formattedError;
    }
    if (self->errorLine > 0){
        snprintf(self->formattedError, sizeof(self->formattedError), "line %zu: %s", self->errorLine, self->errorMessage);
        return self->formattedError;
    }
    return self->errorMessage;
}

bool
NDSContext_getErrorType(NDSContext *self, int destSlot)
{
    if (self->currentError.type != NDSValueType_Error || !self->currentError.as.object)
        return false;
    *NDSContext_slotPointer(self, destSlot) = NDSObject_getProperty(self->currentError.as.object, ErrorProp_Type);
    return true;
}

bool
NDSContext_getErrorValue(NDSContext *self, int destSlot)
{
    if (self->currentError.type != NDSValueType_Error || !self->currentError.as.object)
        return false;
    *NDSContext_slotPointer(self, destSlot) = NDSObject_getProperty(self->currentError.as.object, ErrorProp_Value);
    return true;
}

bool
NDSContext_getGlobal(NDSContext *self, const char *name, int destSlot)
{
    NDSAtom atom = NDSContext_findAtom(self, name, strlen(name));
    if (atom == NDSAtom_Invalid)
        return false;
    size_t idx = Globals_find(self, atom);
    if (idx == (size_t)-1)
        return false;
    *NDSContext_slotPointer(self, destSlot) = self->globals[idx].value;
    return true;
}

NDSStatus
NDSContext_setGlobal(NDSContext *self, const char *name, int srcSlot)
{
    NDSAtom atom = NDSContext_internAtom(self, name);
    if (atom == NDSAtom_Invalid){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    size_t idx = Globals_ensure(self, atom);
    if (idx == (size_t)-1){
        NDSContext_setError(self, "out of memory");
        return NDSStatus_Error;
    }
    if (self->globals[idx].isConst){
        NDSContext_setErrorF(self, "cannot assign to constant '%s'", name);
        return NDSStatus_Error;
    }
    self->globals[idx].value = *NDSContext_slotPointer(self, srcSlot);
    return NDSStatus_OK;
}

static NDSStatus
NDSContext_callFuncIdx(NDSContext *self, uint32_t funcIdx, int argCount)
{
    if (funcIdx >= self->functionCount){
        NDSContext_setError(self, "invalid function");
        return NDSStatus_Error;
    }

    Function *func = &self->functions[funcIdx];

    /* Arguments are already in slots 0..argCount-1 (frameBase-relative).
       Copy them to the top of stack for the call. */
    size_t callBase = self->stackTop;
    for (int i = 0; i < argCount; i++){
        if (!NDSContext_ensureStack(self, self->stackTop + 1)){
            NDSContext_setError(self, "stack overflow");
            return NDSStatus_Error;
        }
        self->stack[self->stackTop++] = self->stack[self->frameBase + (size_t)i];
    }

    uint32_t ac = (uint32_t)argCount;
    if (!VM_adjustArgs(self, func, callBase, &ac)){
        NDSContext_setError(self, "stack overflow");
        return NDSStatus_Error;
    }
    argCount = (int)ac;

    NDSStatus status;
    if (func->kind == FuncKind_Host){
        status = VM_callHost(self, func, callBase, (uint32_t)argCount);
    } else{
        VMCallInfo info ={
            .closureBase = 0,
            .block = NDS_NO_BLOCK,
            .callerLocalBase = 0,
            .handlerIP = func->as.script.handlerIP,
            .handlerActive = false,
            .handlerErrorSlot = func->as.script.handlerErrorSlot,
            .maxLocals = func->as.script.maxLocals,
        };
        status = VM_execute(self, func->as.script.code, func->as.script.lines,
                            func->as.script.codeLength, callBase, &info);
    }

    if (status != NDSStatus_OK){
        self->stackTop = callBase;
        return NDSStatus_Error;
    }

    /* Move result to slot 0 */
    NDSValue result = self->stack[self->stackTop - 1];
    self->stackTop = callBase;
    self->stack[self->frameBase] = result;
    return NDSStatus_OK;
}

NDSStatus
NDSContext_callFunction(NDSContext *self, const char *name, int argCount)
{
    NDSAtom atom = NDSContext_findAtom(self, name, strlen(name));
    if (atom == NDSAtom_Invalid){
        NDSContext_setError(self, "unknown function");
        return NDSStatus_Error;
    }
    size_t globalIdx = Globals_find(self, atom);
    if (globalIdx == (size_t)-1 || self->globals[globalIdx].value.type != NDSValueType_Function){
        NDSContext_setError(self, "unknown function");
        return NDSStatus_Error;
    }
    return NDSContext_callFuncIdx(self, (uint32_t)self->globals[globalIdx].value.as.integer, argCount);
}

NDSStatus
NDSContext_callSlot(NDSContext *self, int funcSlot, int argCount)
{
    NDSValue fv = NDSContext_getSlot(self, funcSlot);
    if (fv.type != NDSValueType_Function){
        NDSContext_setError(self, "value is not a function");
        return NDSStatus_Error;
    }
    return NDSContext_callFuncIdx(self, (uint32_t)fv.as.integer, argCount);
}
