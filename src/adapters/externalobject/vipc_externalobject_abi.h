#ifndef VECTORIPC_EXTERNALOBJECT_ABI_H
#define VECTORIPC_EXTERNALOBJECT_ABI_H

#include <esabi/esabi.h>

/*
 * VectorIPC's ExternalObject adapter uses ESABI as the sole definition of the
 * host binary interface. This header declares only VectorIPC's direct method;
 * ESABI owns value layout, tags, errors, lifecycle entry points, packing, and
 * calling convention.
 */
ESABI_DIRECT_FUNCTION(vipc);

ESABI_INITIALIZE_FUNCTION;
ESABI_VERSION_FUNCTION;
ESABI_FREE_FUNCTION;
ESABI_TERMINATE_FUNCTION;

#endif
