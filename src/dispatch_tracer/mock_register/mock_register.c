/*
 * mock_register.c - libmock_register.so
 *
 * Faithful mock of rocprofiler-register: stores runtime-published API table
 * pointers, and on each registration scans for the "rocprofiler_configure"
 * symbol. If found, dlopens libmock_sdk.so (once) and replays all stored
 * tables through the SDK's published set_api_table callback.
 *
 * The callback receives (name, lib_version, lib_instance, tables, num_tables)
 * matching the real rocprofiler_set_api_table signature.  api_tables[i] is a
 * POINTER TO a table struct whose first field is size_t size.
 *
 * Each library name gets a monotonically increasing instance counter, and
 * the caller receives a mock_register_id_t with {category, instance}.
 *
 * No rocprofiler_configure symbol is defined here -- the scan will only
 * succeed if a tool library has been loaded (LD_PRELOAD) or linked in.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mock_register.h"

#define MAX_REGISTERED_TABLES 16
#define MAX_LIB_NAMES         16

typedef struct {
    char               name[64];
    uint32_t           version;
    void**             api_tables;
    uint64_t           api_table_length;
    mock_register_id_t register_id;
    int                in_use;
} registered_entry_t;

/* Per-name instance counter for assigning register_id.instance. */
typedef struct {
    char     name[64];
    uint32_t category;    /* assigned once, sequentially */
    uint32_t next_instance;
} lib_name_entry_t;

static registered_entry_t g_registry[MAX_REGISTERED_TABLES];
static size_t             g_registry_count = 0;
static pthread_mutex_t    g_registry_lock  = PTHREAD_MUTEX_INITIALIZER;

static lib_name_entry_t   g_lib_names[MAX_LIB_NAMES];
static size_t             g_lib_name_count = 0;

/* Published mutable function pointer the SDK fills in. NULL means
 * no SDK has been loaded yet, so registrations are stored-only. */
mock_register_set_api_table_fn_t mock_register_set_api_table_fn = NULL;

/* Guarded by g_registry_lock. */
static void* g_sdk_handle = NULL;

/* Look up (or create) the per-name entry and return the next
 * register_id for that name.  Caller holds g_registry_lock. */
static mock_register_id_t assign_register_id_locked(const char* name)
{
    /* Search for existing name. */
    for (size_t i = 0; i < g_lib_name_count; ++i) {
        if (strcmp(g_lib_names[i].name, name) == 0) {
            mock_register_id_t id;
            id.category = g_lib_names[i].category;
            id.instance = g_lib_names[i].next_instance++;
            return id;
        }
    }

    /* New name -- allocate a category. */
    mock_register_id_t id;
    if (g_lib_name_count < MAX_LIB_NAMES) {
        lib_name_entry_t* e = &g_lib_names[g_lib_name_count];
        snprintf(e->name, sizeof(e->name), "%s", name);
        e->category      = (uint32_t)g_lib_name_count;
        e->next_instance = 0;

        id.category = e->category;
        id.instance = e->next_instance++;
        g_lib_name_count++;
    } else {
        /* Overflow -- return a sentinel. */
        fprintf(stderr, "[mock_register] lib_name table full (%d names)\n",
                MAX_LIB_NAMES);
        id.category = UINT32_MAX;
        id.instance = UINT32_MAX;
    }
    return id;
}

/* Try to discover a tool by looking up rocprofiler_configure via
 * RTLD_DEFAULT. If present in the process image (e.g. a tool library
 * was dlopen'd with RTLD_GLOBAL, or the mock SDK has already been
 * loaded and re-exported the symbol from a tool), we consider the SDK
 * worth loading. */
static int tool_present(void)
{
    /* Clear any stale error state. */
    (void)dlerror();
    void* sym = dlsym(RTLD_DEFAULT, "rocprofiler_configure");
    /* Ignore dlerror -- a missing symbol returns NULL. */
    return sym != NULL;
}

/* Attempt to dlopen libmock_sdk.so. Holds the registry lock already. */
static void maybe_load_sdk_locked(void)
{
    if (g_sdk_handle != NULL) return;

    /* MOCK_REGISTER_LIB env var lets a test swap the backing library out
     * (e.g. libshim_mock.so for the shim-design validation). When set, it
     * is the only candidate; otherwise fall back to libmock_sdk.so. */
    const char* shim_override = getenv("MOCK_REGISTER_LIB");
    const char* default_candidates[] = {
        "libmock_sdk.so",
        "./libmock_sdk.so",
        NULL
    };
    const char* override_candidates[] = { shim_override, NULL };
    const char** candidates =
        (shim_override && shim_override[0]) ? override_candidates : default_candidates;
    /* Try common resolution paths. RTLD_GLOBAL so the backing library's
     * symbols are visible for subsequent dlsym(RTLD_DEFAULT, ...) lookups. */
    for (size_t i = 0; candidates[i]; ++i) {
        void* h = dlopen(candidates[i], RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            g_sdk_handle = h;
            break;
        }
    }
    if (!g_sdk_handle) {
        /* Couldn't load the SDK -- leave things as stored-only. */
        return;
    }

    /* The SDK's constructor / initializer should have set
     * mock_register_set_api_table_fn via direct assignment. If the SDK
     * was built without doing so, fall back to dlsym for the symbol. */
    if (mock_register_set_api_table_fn == NULL) {
        void* sym = dlsym(g_sdk_handle, "mock_sdk_set_api_table");
        if (sym) {
            mock_register_set_api_table_fn =
                (mock_register_set_api_table_fn_t)sym;
        }
    }
}

/* Replay every stored registration through the SDK callback.
 * Caller holds the registry lock. */
static void replay_all_locked(void)
{
    if (mock_register_set_api_table_fn == NULL) return;
    for (size_t i = 0; i < MAX_REGISTERED_TABLES; ++i) {
        if (!g_registry[i].in_use) continue;
        mock_register_set_api_table_fn(g_registry[i].name,
                                       g_registry[i].version,
                                       g_registry[i].register_id.instance,
                                       g_registry[i].api_tables,
                                       g_registry[i].api_table_length);
    }
}

/* ---------------------------------------------------------------- */
/* Early-load constructor: when MOCK_REGISTER_LIB is set, dlopen the  */
/* override library BEFORE any runtime calls register_library. This   */
/* mirrors the alternative shim model where rocprofiler-   */
/* register unconditionally loads the shim at startup. Without this,  */
/* tool_present() would return NULL and the conditional dlopen in     */
/* maybe_load_sdk_locked would never trigger for a tool that only     */
/* exports its own rocprofiler_configure (like libshim_mock.so).      */
/*                                                                    */
/* We deliberately do NOT hold g_registry_lock here to avoid a        */
/* deadlock: the dlopen'd library's constructor is free to register   */
/* its own set_api_table callback via the published global; if it     */
/* also tried mock_register_invoke_all_registrations(), that is a     */
/* no-op because no runtimes have registered yet.                     */
/* ---------------------------------------------------------------- */

__attribute__((constructor))
static void mock_register_early_load(void)
{
    const char* lib = getenv("MOCK_REGISTER_LIB");
    if (!lib || !lib[0]) return;

    void* h = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        fprintf(stderr, "[mock_register] MOCK_REGISTER_LIB dlopen(%s) failed: %s\n",
                lib, dlerror());
        return;
    }
    /* Publish the handle + the set_api_table fn (if not already set by the
     * lib's own constructor) so subsequent registrations go through it. */
    pthread_mutex_lock(&g_registry_lock);
    if (g_sdk_handle == NULL) g_sdk_handle = h;
    if (mock_register_set_api_table_fn == NULL) {
        void* sym = dlsym(h, "mock_sdk_set_api_table");
        if (sym) mock_register_set_api_table_fn =
                     (mock_register_set_api_table_fn_t)sym;
    }
    pthread_mutex_unlock(&g_registry_lock);
}

int mock_register_library_api_table(const char*         lib_name,
                                    uint32_t            lib_version,
                                    void**              api_tables,
                                    uint64_t            api_table_length,
                                    mock_register_id_t* register_id)
{
    if (!lib_name || !api_tables || api_table_length == 0 || !register_id)
        return MOCK_REGISTER_ERROR;

    pthread_mutex_lock(&g_registry_lock);

    /* Find a free slot. Dedup by name is intentionally NOT done -- the
     * rocprofiler-register behavior allows multiple registrations. */
    int slot = -1;
    for (size_t i = 0; i < MAX_REGISTERED_TABLES; ++i) {
        if (!g_registry[i].in_use) { slot = (int)i; break; }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_registry_lock);
        fprintf(stderr, "[mock_register] capacity exceeded (%d tables)\n",
                MAX_REGISTERED_TABLES);
        return MOCK_REGISTER_ERROR;
    }

    /* Assign a unique register_id for this library instance. */
    mock_register_id_t id = assign_register_id_locked(lib_name);
    *register_id = id;

    registered_entry_t* e = &g_registry[slot];
    snprintf(e->name, sizeof(e->name), "%s", lib_name);
    e->version          = lib_version;
    e->api_tables       = api_tables;
    e->api_table_length = api_table_length;
    e->register_id      = id;
    e->in_use           = 1;
    if ((size_t)slot + 1 > g_registry_count) g_registry_count = slot + 1;

    /* Is a tool present in the image? If so, make sure the SDK is loaded
     * and hand off this (and any previously-deferred) registration.
     * In shim-mode the set_api_table callback has already been published
     * by the early-load constructor, so we can skip the tool_present probe
     * and the dlopen entirely -- just replay this table through the shim. */
    int rc;
    if (mock_register_set_api_table_fn != NULL) {
        mock_register_set_api_table_fn(e->name, e->version,
                                       e->register_id.instance,
                                       e->api_tables, e->api_table_length);
        rc = MOCK_REGISTER_OK;
    } else if (tool_present()) {
        maybe_load_sdk_locked();
        if (mock_register_set_api_table_fn != NULL) {
            mock_register_set_api_table_fn(e->name, e->version,
                                           e->register_id.instance,
                                           e->api_tables, e->api_table_length);
            rc = MOCK_REGISTER_OK;
        } else {
            rc = MOCK_REGISTER_NO_TOOL;
        }
    } else {
        rc = MOCK_REGISTER_NO_TOOL;
    }

    pthread_mutex_unlock(&g_registry_lock);
    return rc;
}

int mock_register_invoke_all_registrations(void)
{
    pthread_mutex_lock(&g_registry_lock);
    if (mock_register_set_api_table_fn == NULL) {
        /* The SDK may have set the callback but not yet been given a
         * chance to process pending tables. Try to load it now. */
        if (tool_present()) maybe_load_sdk_locked();
    }
    int handed_off = 0;
    if (mock_register_set_api_table_fn != NULL) {
        replay_all_locked();
        handed_off = 1;
    }
    pthread_mutex_unlock(&g_registry_lock);
    return handed_off ? MOCK_REGISTER_OK : MOCK_REGISTER_NO_TOOL;
}
