#undef TRACEPOINT_PROVIDER
#define TRACEPOINT_PROVIDER mylib

#undef TRACEPOINT_INCLUDE
#define TRACEPOINT_INCLUDE "mylib_tp.h"

#if !defined(_MYLIB_TP_H) || defined(TRACEPOINT_HEADER_MULTI_READ)
#define _MYLIB_TP_H

#include <lttng/tracepoint.h>

TRACEPOINT_EVENT(
    mylib,
    my_traced_function_entry,
    TP_ARGS(
        int, arg1,
        uint64_t, arg2,
        const char*, arg3,
        double, arg4,
        void*, arg5
    ),
    TP_FIELDS(
        ctf_integer(int, arg1, arg1)
        ctf_integer(uint64_t, arg2, arg2)
        ctf_string(arg3, arg3)
        ctf_float(double, arg4, arg4)
        ctf_integer_hex(unsigned long, arg5, (unsigned long)arg5)
    )
)

TRACEPOINT_EVENT(
    mylib,
    my_traced_function_exit,
    TP_ARGS(void),
    TP_FIELDS()
)

#endif /* _MYLIB_TP_H */

#include <lttng/tracepoint-event.h>
