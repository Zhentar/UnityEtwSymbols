#ifndef __MONO_PROFILER_H__
#define __MONO_PROFILER_H__

#include <mono/metadata/object.h>
#include <mono/metadata/appdomain.h>

G_BEGIN_DECLS

typedef enum {
	MONO_PROFILE_NONE = 0,
	MONO_PROFILE_APPDOMAIN_EVENTS = 1 << 0,
	MONO_PROFILE_ASSEMBLY_EVENTS  = 1 << 1,
	MONO_PROFILE_MODULE_EVENTS    = 1 << 2,
	MONO_PROFILE_CLASS_EVENTS     = 1 << 3,
	MONO_PROFILE_JIT_COMPILATION  = 1 << 4,
	MONO_PROFILE_INLINING         = 1 << 5,
	MONO_PROFILE_EXCEPTIONS       = 1 << 6,
	MONO_PROFILE_ALLOCATIONS      = 1 << 7,
	MONO_PROFILE_GC               = 1 << 8,
	MONO_PROFILE_THREADS          = 1 << 9,
	MONO_PROFILE_REMOTING         = 1 << 10,
	MONO_PROFILE_TRANSITIONS      = 1 << 11,
	MONO_PROFILE_ENTER_LEAVE      = 1 << 12,
	MONO_PROFILE_COVERAGE         = 1 << 13,
	MONO_PROFILE_INS_COVERAGE     = 1 << 14,
	MONO_PROFILE_STATISTICAL      = 1 << 15,
	MONO_PROFILE_METHOD_EVENTS    = 1 << 16,
	MONO_PROFILE_MONITOR_EVENTS   = 1 << 17,
	MONO_PROFILE_IOMAP_EVENTS = 1 << 18, /* this should likely be removed, too */
	MONO_PROFILE_GC_MOVES = 1 << 19,
	MONO_PROFILE_FILEIO = 1 << 23
} MonoProfileFlags;

typedef enum {
	MONO_PROFILE_OK,
	MONO_PROFILE_FAILED
} MonoProfileResult;

typedef enum {
	MONO_GC_EVENT_START,
	MONO_GC_EVENT_MARK_START,
	MONO_GC_EVENT_MARK_END,
	MONO_GC_EVENT_RECLAIM_START,
	MONO_GC_EVENT_RECLAIM_END,
	MONO_GC_EVENT_END,
	MONO_GC_EVENT_PRE_STOP_WORLD,
	MONO_GC_EVENT_POST_STOP_WORLD,
	MONO_GC_EVENT_PRE_START_WORLD,
	MONO_GC_EVENT_POST_START_WORLD
} MonoGCEvent;

/* coverage info */
typedef struct {
	MonoMethod *method;
	int iloffset;
	int counter;
	const char *filename;
	int line;
	int col;
} MonoProfileCoverageEntry;

/* executable code buffer info */
typedef enum {
	MONO_PROFILER_CODE_BUFFER_UNKNOWN,
	MONO_PROFILER_CODE_BUFFER_METHOD,
	MONO_PROFILER_CODE_BUFFER_LAST
} MonoProfilerCodeBufferType;

typedef struct _MonoProfiler MonoProfiler;

typedef enum {
	MONO_PROFILER_MONITOR_CONTENTION = 1,
	MONO_PROFILER_MONITOR_DONE = 2,
	MONO_PROFILER_MONITOR_FAIL = 3
} MonoProfilerMonitorEvent;

typedef enum {
	MONO_PROFILER_CALL_CHAIN_NONE = 0,
	MONO_PROFILER_CALL_CHAIN_NATIVE = 1,
	MONO_PROFILER_CALL_CHAIN_GLIBC = 2,
	MONO_PROFILER_CALL_CHAIN_MANAGED = 3,
	MONO_PROFILER_CALL_CHAIN_INVALID = 4
} MonoProfilerCallChainStrategy;

typedef enum {
	MONO_PROFILER_UNLOAD_THREAD_START = 0,
	MONO_PROFILER_UNLOAD_THREAD_FINISH = 1,
	MONO_PROFILER_UNLOAD_THREAD_START_THREADS_SHUTDOWN = 2,
	MONO_PROFILER_UNLOAD_THREAD_FINISH_THREADS_SHUTDOWN = 3,
	MONO_PROFILER_UNLOAD_THREAD_START_THREADPOOL_SHUTDOWN = 4,
	MONO_PROFILER_UNLOAD_THREAD_FINISH_THREADPOOL_SHUTDOWN = 5,
	MONO_PROFILER_UNLOAD_THREAD_START_FINALIZER = 6,
	MONO_PROFILER_UNLOAD_THREAD_FINISH_FINALIZER = 7,
} MonoProfilerDomainUnloadThreadEvent;

/*
 * Functions that the runtime will call on the profiler.
 */

typedef void (*MonoProfileFunc) (MonoProfiler *prof);

typedef void (*MonoProfileFinalizerThreadFunc)     (MonoProfiler *prof);
typedef void (*MonoProfileAppDomainFunc) (MonoProfiler *prof, MonoDomain   *domain);
typedef void (*MonoProfileAppDomainUnloadFunc) (MonoProfiler *prof);
typedef void (*MonoProfileAppDomainUnloadThreadFunc) (MonoProfiler *prof, MonoProfilerDomainUnloadThreadEvent event);
typedef void (*MonoProfileMethodFunc)   (MonoProfiler *prof, MonoMethod   *method);
typedef void (*MonoProfileClassFunc)    (MonoProfiler *prof, MonoClass    *klass);
typedef void (*MonoProfileModuleFunc)   (MonoProfiler *prof, MonoImage    *module);
typedef void (*MonoProfileAssemblyFunc) (MonoProfiler *prof, MonoAssembly *assembly);
typedef void (*MonoProfileMonitorFunc)  (MonoProfiler *prof, MonoObject *obj, MonoProfilerMonitorEvent event);

typedef void (*MonoProfileExceptionFunc) (MonoProfiler *prof, MonoObject *object);
typedef void (*MonoProfileExceptionClauseFunc) (MonoProfiler *prof, MonoMethod *method, int clause_type, int clause_num);

typedef void (*MonoProfileAppDomainResult)(MonoProfiler *prof, MonoDomain   *domain,   int result);
typedef void (*MonoProfileMethodResult)   (MonoProfiler *prof, MonoMethod   *method,   int result);
typedef void (*MonoProfileJitResult)      (MonoProfiler *prof, MonoMethod   *method,   MonoJitInfo* jinfo,   int result);
typedef void (*MonoProfileClassResult)    (MonoProfiler *prof, MonoClass    *klass,    int result);
typedef void (*MonoProfileModuleResult)   (MonoProfiler *prof, MonoImage    *module,   int result);
typedef void (*MonoProfileAssemblyResult) (MonoProfiler *prof, MonoAssembly *assembly, int result);

typedef void (*MonoProfileMethodInline)   (MonoProfiler *prof, MonoMethod   *parent, MonoMethod *child, int *ok);

typedef void (*MonoProfileThreadFunc)     (MonoProfiler *prof, gsize tid);
typedef void (*MonoProfileAllocFunc)      (MonoProfiler *prof, MonoObject *obj, MonoClass *klass);
typedef void (*MonoProfileFileIOFunc)     (MonoProfiler *prof, int kind, int count);
typedef void (*MonoProfileStatFunc)       (MonoProfiler *prof, guchar *ip, void *context);
typedef void (*MonoProfileStatCallChainFunc) (MonoProfiler *prof, int call_chain_depth, guchar **ip, void *context);
typedef void (*MonoProfileGCFunc)         (MonoProfiler *prof, MonoGCEvent event, int generation);
typedef void (*MonoProfileGCMoveFunc) (MonoProfiler *prof, void **objects, int num);
typedef void (*MonoProfileGCResizeFunc)   (MonoProfiler *prof, gint64 new_size);

typedef void (*MonoProfileIomapFunc) (MonoProfiler *prof, const char *report, const char *pathname, const char *new_pathname);

typedef gboolean (*MonoProfileCoverageFilterFunc)   (MonoProfiler *prof, MonoMethod *method);

typedef void (*MonoProfileCoverageFunc)   (MonoProfiler *prof, const MonoProfileCoverageEntry *entry);


typedef void (*MonoProfilerCodeChunkNew) (MonoProfiler *prof, gpointer chunk, int size);
typedef void (*MonoProfilerCodeChunkDestroy) (MonoProfiler *prof, gpointer chunk);
typedef void (*MonoProfilerCodeBufferNew) (MonoProfiler *prof, gpointer buffer, int size, MonoProfilerCodeBufferType type, void *data);

/*
 * Function the profiler may call.
 */

MonoProfiler* mono_profiler_create();

void mono_profiler_install       (MonoProfiler *prof, MonoProfileFunc shutdown_callback);
void mono_profiler_set_events    (MonoProfileFlags events);
void mono_profiler_set_profiler_events (MonoProfiler *profiler, MonoProfileFlags events);


MonoProfileFlags mono_profiler_get_events (void);

void mono_profiler_install_finalizer_thread(MonoProfileFinalizerThreadFunc start);

void mono_profiler_install_appdomain_unload_thread(MonoProfileAppDomainUnloadThreadFunc func);
void mono_profiler_install_appdomain_start_finish_unload (MonoProfileAppDomainFunc start_unload, MonoProfileAppDomainUnloadFunc finish_unload);

void mono_profiler_install_appdomain   (MonoProfileAppDomainFunc start_load, MonoProfileAppDomainResult end_load,
                                        MonoProfileAppDomainFunc start_unload, MonoProfileAppDomainFunc end_unload);
void mono_profiler_install_assembly    (MonoProfileAssemblyFunc start_load, MonoProfileAssemblyResult end_load,
                                        MonoProfileAssemblyFunc start_unload, MonoProfileAssemblyFunc end_unload);
void mono_profiler_install_module      (MonoProfileModuleFunc start_load, MonoProfileModuleResult end_load,
                                        MonoProfileModuleFunc start_unload, MonoProfileModuleFunc end_unload);
void mono_profiler_install_class       (MonoProfileClassFunc start_load, MonoProfileClassResult end_load,
                                        MonoProfileClassFunc start_unload, MonoProfileClassFunc end_unload);

void mono_profiler_install_jit_compile (MonoProfileMethodFunc start, MonoProfileMethodResult end);
void mono_profiler_install_jit_end (MonoProfileJitResult end);
void mono_profiler_install_method_free (MonoProfileMethodFunc callback);
void mono_profiler_install_method_invoke (MonoProfileMethodFunc start, MonoProfileMethodFunc end);
void mono_profiler_install_enter_leave (MonoProfileMethodFunc enter, MonoProfileMethodFunc fleave);
void mono_profiler_install_thread      (MonoProfileThreadFunc start, MonoProfileThreadFunc end);
void mono_profiler_install_thread_fast_attach_detach (MonoProfileThreadFunc fast_attach, MonoProfileThreadFunc fast_detach);
void mono_profiler_install_transition  (MonoProfileMethodResult callback);
void mono_profiler_install_allocation  (MonoProfileAllocFunc callback);
void mono_profiler_install_fileio      (MonoProfileFileIOFunc callback);
void mono_profiler_install_monitor     (MonoProfileMonitorFunc callback);
void mono_profiler_install_statistical (MonoProfileStatFunc callback);
void mono_profiler_install_statistical_call_chain (MonoProfileStatCallChainFunc callback, int call_chain_depth, MonoProfilerCallChainStrategy call_chain_strategy);
void mono_profiler_install_exception   (MonoProfileExceptionFunc throw_callback, MonoProfileMethodFunc exc_method_leave, MonoProfileExceptionClauseFunc clause_callback);
void mono_profiler_install_coverage_filter (MonoProfileCoverageFilterFunc callback);
void mono_profiler_coverage_get  (MonoProfiler *prof, MonoMethod *method, MonoProfileCoverageFunc func);
void mono_profiler_install_gc    (MonoProfileGCFunc callback, MonoProfileGCResizeFunc heap_resize_callback);
void mono_profiler_install_gc_moves (MonoProfileGCMoveFunc callback);
void mono_profiler_install_runtime_initialized (MonoProfileFunc runtime_initialized_callback);

void mono_profiler_install_iomap (MonoProfileIomapFunc callback);

void mono_profiler_install_code_chunk_new (MonoProfilerCodeChunkNew callback);
void mono_profiler_install_code_chunk_destroy (MonoProfilerCodeChunkDestroy callback);
void mono_profiler_install_code_buffer_new (MonoProfilerCodeBufferNew callback);

void mono_profiler_load             (const char *desc);

G_END_DECLS

#endif /* __MONO_PROFILER_H__ */

