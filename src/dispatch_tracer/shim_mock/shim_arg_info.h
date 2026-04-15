/*
 * shim_arg_info.h — Per-argument metadata and string conversion.
 *
 * Mimics rocprofiler-sdk's rocprofiler_iterate_callback_tracing_kind_operation_args
 * pattern: each argument of a traced API call is described by:
 *   - arg_number (index)
 *   - arg_name   (parameter name from the prototype)
 *   - arg_type   (C type as string)
 *   - arg_value_str (the value converted to string at capture time)
 *
 * The shim populates this at EXIT time (after orig returns, so output
 * params have their final values) and serializes the string
 * representations into the record's args[] payload. The consumer reads
 * strings — it does NOT need to know the struct layout.
 *
 * This is the mechanism that lets tools print:
 *   hipLaunchKernel(function_address=0xdead, numBlocks={256,1,1}, ...)
 * without linking the HIP headers.
 */
#ifndef SHIM_ARG_INFO_H
#define SHIM_ARG_INFO_H

#include <stdint.h>
#include <stdio.h>
#include <inttypes.h>

#define SHIM_MAX_ARGS_PER_OP  8
#define SHIM_ARG_STR_MAX     96

/* One entry per argument. Packed into the record's args[] field as a
 * flat array of shim_arg_entry_t. The consumer reads them sequentially. */
typedef struct {
    char name[32];
    char type[32];
    char value[SHIM_ARG_STR_MAX];
} shim_arg_entry_t;

/* ---- Deep-copy support (MODE_RECORD_FULL) ---- */

#define SHIM_DEEP_COPY_MAX 128

/* Called at EXIT time IN THE TARGET to dereference pointer args and
 * inline pointed-at structs into the record extension area. */
typedef uint32_t (*shim_deep_copy_fn_t)(const void* packed_args,
                                        uint8_t* dst, uint32_t dst_size);

/* Per-op arg descriptor (compile-time metadata). The shim's code
 * generator populates one of these per (table, op). */
typedef struct {
    uint32_t n_args;
    struct {
        const char* name;
        const char* type;
        void (*format)(const void* packed_args, char* dst, uint32_t dst_size);
        /* format_deep reads from the deep-copy payload (inlined struct copy
         * appended after packed args in the record). Used only when
         * MODE_RECORD_FULL is enabled for this op. NULL for scalar args. */
        void (*format_deep)(const void* deep_payload, char* dst, uint32_t dst_size);
    } args[SHIM_MAX_ARGS_PER_OP];
    /* Deep-copy function. Called at EXIT in TARGET to dereference pointer
     * args and inline the pointed-at structs. NULL = all scalar args. */
    shim_deep_copy_fn_t deep_copy;
} shim_op_arg_descriptor_t;

/* ---- String serialization support (iterate_args pattern) ---- */

/* Serialize all args of an op into a flat array of shim_arg_entry_t.
 * Returns the number of bytes written (n_args * sizeof(shim_arg_entry_t)). */
static inline uint32_t shim_serialize_args(
    const shim_op_arg_descriptor_t* desc,
    const void* packed_args,
    uint8_t* out_buf, uint32_t out_buf_size)
{
    if (!desc || !packed_args || !out_buf) return 0;
    uint32_t offset = 0;
    for (uint32_t i = 0; i < desc->n_args; i++) {
        if (offset + sizeof(shim_arg_entry_t) > out_buf_size) break;
        shim_arg_entry_t* entry = (shim_arg_entry_t*)(out_buf + offset);
        snprintf(entry->name, sizeof(entry->name), "%s", desc->args[i].name);
        snprintf(entry->type, sizeof(entry->type), "%s", desc->args[i].type);
        if (desc->args[i].format) {
            desc->args[i].format(packed_args, entry->value, sizeof(entry->value));
        } else {
            snprintf(entry->value, sizeof(entry->value), "?");
        }
        offset += sizeof(shim_arg_entry_t);
    }
    return offset;
}

#endif /* SHIM_ARG_INFO_H */
