#ifndef JANUS_LIB_H
#define JANUS_LIB_H

/* C API for the Janus Engine shared library.
 * Exposes the same container format and defaults as the janus CLI:
 *   - compression runs the adaptive Brotli/LZMA/store trial per segment
 *   - with thread_count == 0 the engine picks up to two worker threads
 *   - log_path may be NULL (no benchmark log is written)
 *
 * Return values: 1 on success, 0 on failure. Negative thread counts fail.
 * Calls are serialized. Error strings are local to the calling thread.
 * Input, output and optional log paths must refer to distinct files.
 * Limits: 16 MiB segments, 64 MiB notation bodies, 8 GiB total output.
 * Output is replaced only after a successful operation.
 */

#ifdef _WIN32
    #ifdef JANUS_LIB_BUILD
        #define JANUS_API __declspec(dllexport)
    #else
        #define JANUS_API __declspec(dllimport)
    #endif
#else
    #define JANUS_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

JANUS_API const char* janus_version_string(void);
JANUS_API const char* janus_last_error(void);

JANUS_API int janus_compress_file(const char* input_path, const char* output_path,
                                  const char* log_path, int thread_count);

JANUS_API int janus_decompress_file(const char* input_path, const char* output_path,
                                    const char* log_path);

#ifdef __cplusplus
}
#endif

#endif /* JANUS_LIB_H */
