#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include "mylib_tracer.skel.h"

#define MAX_STRING_LEN 64
#define MAX_EVENTS 1000000  // Buffer up to 1M events in memory

// Optimized event structures matching BPF side
struct trace_event_entry {
    uint64_t timestamp;
    int32_t arg1;
    uint64_t arg2;
    double arg3;
    uint64_t arg4;
    uint32_t event_type;  // 0=entry
} __attribute__((packed));

struct trace_event_exit {
    uint64_t timestamp;
    uint32_t event_type;  // 1=exit
} __attribute__((packed));

// Union to store any event type
union stored_event {
    struct trace_event_entry entry;
    struct trace_event_exit exit;
    char raw[sizeof(struct trace_event_entry)];  // Max size
};

// Event buffer - store events in memory during tracing
static union stored_event *event_buffer = NULL;
static size_t *event_sizes = NULL;
static volatile sig_atomic_t exiting = 0;
static unsigned long event_count = 0;
static unsigned long events_dropped = 0;

static void sig_handler(int sig) {
    exiting = 1;
}

// Get function offset in library using nm
static long get_function_offset(const char *lib_path, const char *func_name) {
    char cmd[512];
    FILE *fp;
    char line[256];
    long offset = -1;

    snprintf(cmd, sizeof(cmd), "nm -D %s | grep ' T %s$'", lib_path, func_name);
    fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }

    if (fgets(line, sizeof(line), fp)) {
        offset = strtol(line, NULL, 16);
    }

    pclose(fp);
    return offset;
}

// Handle event: Just store in memory buffer (FAST!)
static int handle_event(void *ctx, void *data, size_t data_sz) {
    // Check if buffer is full
    if (event_count >= MAX_EVENTS) {
        events_dropped++;
        return 0;
    }

    // Copy event to buffer
    memcpy(&event_buffer[event_count], data, data_sz);
    event_sizes[event_count] = data_sz;
    event_count++;

    return 0;
}

// Write all buffered events to file (AFTER tracing completes)
static void write_events_to_file(const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "Failed to open output file: %s\n", strerror(errno));
        return;
    }

    printf("Writing %lu events to %s...\n", event_count, filename);

    for (unsigned long i = 0; i < event_count; i++) {
        if (event_sizes[i] == sizeof(struct trace_event_entry)) {
            const struct trace_event_entry *e = &event_buffer[i].entry;
            fprintf(f,
                    "[%lu.%09lu] mylib:my_traced_function_entry: "
                    "{ arg1 = %d, arg2 = %lu, arg3 = %f, arg4 = 0x%lx }\n",
                    e->timestamp / 1000000000,
                    e->timestamp % 1000000000,
                    e->arg1,
                    e->arg2,
                    e->arg3,
                    e->arg4);
        } else if (event_sizes[i] == sizeof(struct trace_event_exit)) {
            const struct trace_event_exit *e = &event_buffer[i].exit;
            fprintf(f,
                    "[%lu.%09lu] mylib:my_traced_function_exit\n",
                    e->timestamp / 1000000000,
                    e->timestamp % 1000000000);
        }
    }

    fclose(f);
    printf("Wrote %lu events (%lu dropped)\n", event_count, events_dropped);
}

// Find library path - try multiple locations
static const char* find_library() {
    static const char* locations[] = {
        "../lib/libmylib.so",                 // CMake build from bin/
        "./lib/libmylib.so",                   // CMake build from build/
        "./build/lib/libmylib.so",             // CMake build from project root
        "../build/lib/libmylib.so",            // CMake build from subdir
        "./build/lib/libmylib.so.1.0",         // CMake versioned lib from root
        "../sample_library/libmylib.so",       // Makefile build from subdir
        "./sample_library/libmylib.so",        // Makefile build from root
        NULL
    };

    for (int i = 0; locations[i] != NULL; i++) {
        if (access(locations[i], F_OK) == 0) {
            return locations[i];
        }
    }
    return NULL;
}

int main(int argc, char **argv) {
    struct mylib_tracer_bpf *skel;
    struct ring_buffer *rb = NULL;
    int err;
    const char *lib_path;
    const char *func_name = "my_traced_function";
    long func_offset;
    struct bpf_link *link_entry = NULL;
    struct bpf_link *link_exit = NULL;

    const char *output_file = NULL;

    // Check if we should write trace to file (via environment variable or command line)
    // By default, we only collect statistics (no file output) for minimal overhead
    const char *write_trace_env = getenv("EBPF_TRACE_WRITE_FILE");
    int should_write_file = (write_trace_env != NULL && strcmp(write_trace_env, "1") == 0);

    if (argc == 2) {
        output_file = argv[1];
        should_write_file = 1;  // If file specified on command line, always write
    } else if (argc > 2) {
        fprintf(stderr, "Usage: %s [output_file]\n", argv[0]);
        fprintf(stderr, "\n");
        fprintf(stderr, "By default, traces events in memory only (no file output).\n");
        fprintf(stderr, "To write trace to file:\n");
        fprintf(stderr, "  1. Specify output_file on command line, OR\n");
        fprintf(stderr, "  2. Set EBPF_TRACE_WRITE_FILE=1 environment variable\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s /tmp/trace.txt          # Write to file (command line)\n", argv[0]);
        fprintf(stderr, "  EBPF_TRACE_WRITE_FILE=1 %s /tmp/trace.txt  # Write to file (env var)\n", argv[0]);
        fprintf(stderr, "  %s                         # No file output (benchmark mode)\n", argv[0]);
        return 1;
    }

    // If env var is set but no file specified, use default location
    if (should_write_file && !output_file) {
        output_file = "/tmp/ebpf_trace.txt";
    }

    // Find the library
    lib_path = find_library();
    if (!lib_path) {
        fprintf(stderr, "Failed to find libmylib.so in any expected location\n");
        fprintf(stderr, "Tried:\n");
        fprintf(stderr, "  ../lib/libmylib.so\n");
        fprintf(stderr, "  ./lib/libmylib.so\n");
        fprintf(stderr, "  ../build/lib/libmylib.so\n");
        fprintf(stderr, "  ../sample_library/libmylib.so\n");
        return 1;
    }

    printf("Using library: %s\n", lib_path);

    // Allocate event buffer (do this BEFORE tracing starts)
    event_buffer = calloc(MAX_EVENTS, sizeof(union stored_event));
    event_sizes = calloc(MAX_EVENTS, sizeof(size_t));
    if (!event_buffer || !event_sizes) {
        fprintf(stderr, "Failed to allocate event buffer\n");
        return 1;
    }
    printf("Allocated buffer for %d events (%zu MB)\n", MAX_EVENTS,
           (MAX_EVENTS * sizeof(union stored_event)) / (1024*1024));

    // Set up signal handler
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Set up libbpf errors and debug info callback
    libbpf_set_print(NULL);

    // Open BPF application
    skel = mylib_tracer_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        err = -1;
        goto cleanup;
    }

    // Load & verify BPF programs
    err = mylib_tracer_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load and verify BPF skeleton\n");
        goto cleanup;
    }

    // Get function offset
    func_offset = get_function_offset(lib_path, func_name);
    if (func_offset < 0) {
        fprintf(stderr, "Failed to find function offset for %s\n", func_name);
        err = -1;
        goto cleanup;
    }

    printf("Found %s at offset 0x%lx\n", func_name, func_offset);

    // Attach entry probe
    link_entry = bpf_program__attach_uprobe(skel->progs.my_traced_function_entry,
                                            false /* not uretprobe */,
                                            -1 /* any process */,
                                            lib_path,
                                            func_offset);
    if (!link_entry) {
        err = -errno;
        fprintf(stderr, "Failed to attach entry uprobe: %s\n", strerror(-err));
        goto cleanup;
    }

    // Attach exit probe
    link_exit = bpf_program__attach_uprobe(skel->progs.my_traced_function_exit,
                                           true /* uretprobe */,
                                           -1 /* any process */,
                                           lib_path,
                                           func_offset);
    if (!link_exit) {
        err = -errno;
        fprintf(stderr, "Failed to attach exit uprobe: %s\n", strerror(-err));
        goto cleanup;
    }

    printf("Successfully attached uprobes to %s\n", func_name);
    printf("Tracing... Press Ctrl-C to stop.\n");

    // Set up ring buffer polling
    rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event, NULL, NULL);
    if (!rb) {
        err = -1;
        fprintf(stderr, "Failed to create ring buffer\n");
        goto cleanup;
    }

    // Process events
    while (!exiting) {
        err = ring_buffer__poll(rb, 100 /* timeout, ms */);
        if (err == -EINTR) {
            err = 0;
            break;
        }
        if (err < 0) {
            fprintf(stderr, "Error polling ring buffer: %d\n", err);
            break;
        }
    }

    printf("\nTracing stopped. Captured %lu events.\n", event_count);

    // Write all buffered events to file (AFTER tracing completes) - only if requested
    if (should_write_file && event_count > 0 && output_file) {
        write_events_to_file(output_file);
    } else if (!should_write_file) {
        printf("File output disabled. Events captured in memory only.\n");
        printf("Set EBPF_TRACE_WRITE_FILE=1 or specify output file to write trace.\n");
    }

cleanup:
    if (rb)
        ring_buffer__free(rb);
    if (link_entry)
        bpf_link__destroy(link_entry);
    if (link_exit)
        bpf_link__destroy(link_exit);
    if (skel)
        mylib_tracer_bpf__destroy(skel);

    // Free event buffers
    if (event_buffer)
        free(event_buffer);
    if (event_sizes)
        free(event_sizes);

    return err < 0 ? -err : 0;
}
