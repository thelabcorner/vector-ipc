#ifndef VECTORIPC_EXTERNALOBJECT_ABI_H
#define VECTORIPC_EXTERNALOBJECT_ABI_H

#include <stdint.h>

/*
 * Minimal clean declaration of the documented ExtendScript ExternalObject
 * direct-interface ABI. Keep 8-byte packing: the host and DLL must agree on
 * TaggedData layout.
 */
#pragma pack(push, 8)
typedef struct vipc_es_tagged_data {
    union {
        int32_t intval;
        double fltval;
        char *string;
        int32_t *hObject;
    } data;
    int32_t type;
    int32_t filler;
} vipc_es_tagged_data;
#pragma pack(pop)

enum {
    VIPC_ES_TYPE_UNDEFINED = 0,
    VIPC_ES_TYPE_BOOL = 2,
    VIPC_ES_TYPE_DOUBLE = 3,
    VIPC_ES_TYPE_STRING = 4,
    VIPC_ES_TYPE_INTEGER = 123,
    VIPC_ES_TYPE_UINTEGER = 124,
    VIPC_ES_TYPE_SCRIPT = 125
};

enum {
    VIPC_ES_ERR_OK = 0
};

#if defined(_WIN32)
#define VIPC_ES_EXPORT __declspec(dllexport)
#else
#define VIPC_ES_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

VIPC_ES_EXPORT long vipc(
    vipc_es_tagged_data *argv,
    long argc,
    vipc_es_tagged_data *retval);

VIPC_ES_EXPORT char *ESInitialize(
    const vipc_es_tagged_data **argv,
    long argc);

VIPC_ES_EXPORT long ESGetVersion(void);
VIPC_ES_EXPORT void ESFreeMem(void *pointer);
VIPC_ES_EXPORT void ESTerminate(void);

#ifdef __cplusplus
}
#endif

#endif