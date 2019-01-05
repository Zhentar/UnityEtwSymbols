#include "mono/metadata/profiler.h"
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/class-internals.h>
#include <glib.h>

#include <Windows.h>
#include <evntprov.h>


const GUID PROVIDER_JSCRIPT9 = { 0x57277741, 0x3638, 0x4a4b, {0xbd, 0xba, 0x0a, 0xc6, 0xe4, 0x5d, 0xa5, 0x6c} };
const EVENT_DESCRIPTOR SourceLoad = { 0x29, 0x0, 0x0, 0x4, 0xc, 0x2, 0x1 };
const EVENT_DESCRIPTOR MethodLoad = { 0x9, 0x0, 0x0, 0x4, 0xa, 0x1, 0x1 };

typedef struct { void* dummy;} EtwProfiler;
static EtwProfiler etw_profiler;

static REGHANDLE etw_registration_handle = NULL;

void
on_method_jitted(MonoProfiler *prof, MonoMethod   *method, MonoJitInfo* jinfo, int result)
{
	gunichar2 *name;
	char* name_u8;
	guint64 sourceId = 0;
	guint64 code_size = jinfo->code_size;
	void* scriptContextId = NULL;
	guint32 flags = 0;
	guint64 map = 0;
	guint64 assembly = method->klass->image->assembly; //TODO
	guint32 line_col = 0;

	name_u8 = mono_method_full_name(method, FALSE);
	name = u8to16(name_u8);

	EVENT_DATA_DESCRIPTOR EventData[10];

	EventDataDescCreate(&EventData[0], &scriptContextId, sizeof(PVOID));
	EventDataDescCreate(&EventData[1], &jinfo->code_start, sizeof(PVOID));
	EventDataDescCreate(&EventData[2], &code_size, sizeof(unsigned __int64));
	EventDataDescCreate(&EventData[3], &method, sizeof(const unsigned int)); //MethodID
	EventDataDescCreate(&EventData[4], &flags, sizeof(const unsigned short));
	EventDataDescCreate(&EventData[5], &map, sizeof(const unsigned short));
	EventDataDescCreate(&EventData[6], &assembly, sizeof(unsigned __int64));
	EventDataDescCreate(&EventData[7], &line_col, sizeof(const unsigned int));
	EventDataDescCreate(&EventData[8], &line_col, sizeof(const unsigned int));
	EventDataDescCreate(&EventData[9], name, sizeof(gunichar2) * (wcslen(name) + 1)); //Name
	//EventDataDescCreate(&EventData[9], name_buffer, sizeof(wchar_t) * (name_len));

	DWORD status = EventWrite(
		etw_registration_handle,              // From EventRegister
		&MethodLoad,                  // EVENT_DESCRIPTOR generated from the manifest
		(ULONG)10,					// Size of the array of EVENT_DATA_DESCRIPTORs
		EventData                  // Array of descriptors that contain the event data
	);
	g_free(name);
	g_free(name_u8);
}

void
on_load_assembly(MonoProfiler *prof, MonoAssembly *assembly, int result)
{
	gunichar2 *name;
	void* scriptContextId = NULL;
	guint32 flags = 0;
	guint64 assembly64 = assembly;

	name = u8to16(assembly->aname.name);
	EVENT_DATA_DESCRIPTOR descriptors[4];
	EventDataDescCreate(&descriptors[0], &assembly64, sizeof(guint64)); //SourceID
	EventDataDescCreate(&descriptors[1], &scriptContextId, sizeof(void*)); //ScriptContextID
	EventDataDescCreate(&descriptors[2], &flags, sizeof(guint32)); //Flags
	EventDataDescCreate(&descriptors[3], name, sizeof(gunichar2) * (wcslen(name) + 1)); //Name

	DWORD status = EventWrite(
		etw_registration_handle,              // From EventRegister
		&SourceLoad,                  // EVENT_DESCRIPTOR generated from the manifest
		(ULONG)4,  // Size of the array of EVENT_DATA_DESCRIPTORs
		descriptors                  // Array of descriptors that contain the event data
	);

	g_free(name);
}

void
init_etw_symbol_profiler()
{
	mono_profiler_install((MonoProfiler*)&etw_profiler, NULL);
	mono_profiler_set_events(MONO_PROFILE_ASSEMBLY_EVENTS | MONO_PROFILE_JIT_COMPILATION );
	mono_profiler_install_assembly(NULL, on_load_assembly, NULL, NULL);
	mono_profiler_install_jit_end(on_method_jitted);

	DWORD status = EventRegister(
		&PROVIDER_JSCRIPT9,      // GUID that identifies the provider
		NULL,               // Callback not used
		NULL,               // Context not used
		&etw_registration_handle // Used when calling EventWrite and EventUnregister
	);
}