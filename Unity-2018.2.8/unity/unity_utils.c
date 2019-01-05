#include "unity_utils.h"
#include <stdio.h>
#include <stdlib.h>
#ifdef WIN32
#include <fcntl.h>
#endif
#include <mono/metadata/object.h>
#include <mono/metadata/metadata.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/class-internals.h>
#include <mono/metadata/object-internals.h>
#include <mono/metadata/metadata-internals.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/tokentype.h>
#include <mono/utils/mono-string.h>

#include <glib.h>

#ifdef WIN32
#define UTF8_2_WIDE(src,dst) MultiByteToWideChar( CP_UTF8, 0, src, -1, dst, MAX_PATH )
#endif

#undef exit

void unity_mono_exit( int code )
{
	//fprintf( stderr, "mono: exit called, code %d\n", code );
	exit( code );
}

#ifdef WIN32

HANDLE unity_log_output = 0;

void unity_mono_redirect_output( HANDLE handle )
{
	int fd;
	DWORD written;
//	int fd_copy;
	unity_log_output = handle;	
	fd = _open_osfhandle((intptr_t)handle, (_O_APPEND | _O_TEXT));
	//stdout->_file = fd;
	_dup2(fd,_fileno(stdout));
	//*stdout = *_fdopen(fd, "at");
	
	setvbuf(stdout, NULL, _IONBF, 0);
	
	//fprintf(stdout, "printf from mono\n");
	//WriteFile(handle,"WriteFile from mono",16,&written,NULL);
}

HANDLE unity_mono_get_log_handle()
{
	return unity_log_output;
}

void unity_mono_close_output()
{
	fclose( stdout );
	fclose( stderr );
}

FILE* unity_fopen( const char *name, const char *mode )
{
	wchar_t wideName[MAX_PATH];
	wchar_t wideMode[MAX_PATH];
	UTF8_2_WIDE(name, wideName);
	UTF8_2_WIDE(mode, wideMode);
	return _wfopen( wideName, wideMode );
}

extern LONG CALLBACK seh_vectored_exception_handler(EXCEPTION_POINTERS* ep);
LONG mono_unity_seh_handler(EXCEPTION_POINTERS* ep)
{
#if defined(TARGET_X86) || defined(TARGET_AMD64)
	return seh_vectored_exception_handler(ep);
#else
	g_assert_not_reached();
#endif
}

int (*gUnhandledExceptionHandler)(EXCEPTION_POINTERS*) = NULL;

void mono_unity_set_unhandled_exception_handler(void* handler)
{
	gUnhandledExceptionHandler = handler;
}

#endif //Win32

GString* gEmbeddingHostName = 0;


void mono_unity_write_to_unity_log(MonoString* str)
{
	fprintf(stdout, mono_string_to_utf8(str));
	fflush(stdout);
}


void mono_unity_set_embeddinghostname(const char* name)
{
	gEmbeddingHostName = g_string_new(name);
}



MonoString* mono_unity_get_embeddinghostname()
{
	if (gEmbeddingHostName == 0)
		mono_unity_set_embeddinghostname("mono");
	return mono_string_new_wrapper(gEmbeddingHostName->str);
}

static gboolean socket_security_enabled = FALSE;

gboolean
mono_unity_socket_security_enabled_get ()
{
	return socket_security_enabled;
}

void
mono_unity_socket_security_enabled_set (gboolean enabled)
{
	socket_security_enabled = enabled;
}

void mono_unity_set_vprintf_func (vprintf_func func)
{
	set_vprintf_func (func);
}

gboolean
mono_unity_class_is_interface (MonoClass* klass)
{
	return MONO_CLASS_IS_INTERFACE(klass);
}

gboolean
mono_unity_class_is_abstract (MonoClass* klass)
{
	return (klass->flags & TYPE_ATTRIBUTE_ABSTRACT);
}

void
mono_unity_install_memory_callbacks (MonoMemoryCallbacks* callbacks)
{
	g_mem_set_callbacks (callbacks);
}

// classes_ref is a preallocated array of *length_ref MonoClass*
// returned classes are stored in classes_ref, number of stored classes is stored in length_ref
// return value is number of classes found (which may be greater than number of classes stored)
unsigned mono_unity_get_all_classes_with_name_case (MonoImage *image, const char *name, MonoClass **classes_ref, unsigned *length_ref)
{
	MonoClass *klass;
	MonoTableInfo *tdef = &image->tables [MONO_TABLE_TYPEDEF];
	int i, count;
	guint32 attrs, visibility;
	unsigned length = 0;

	/* (yoinked from icall.c) we start the count from 1 because we skip the special type <Module> */
	for (i = 1; i < tdef->rows; ++i)
	{
		klass = mono_class_get (image, (i + 1) | MONO_TOKEN_TYPE_DEF);
		if (klass && klass->name && 0 == mono_utf8_strcasecmp (klass->name, name))
		{
			if (length < *length_ref)
				classes_ref[length] = klass;
			++length;
		}
	}

	if (length < *length_ref)
		*length_ref = length;
	return length;
}

gboolean
unity_mono_method_is_inflated (MonoMethod* method)
{
	return method->is_inflated;
}

gboolean
unity_mono_method_is_generic (MonoMethod* method)
{
	return method->is_generic;
}

MonoMethod*
unity_mono_reflection_method_get_method(MonoReflectionMethod* mrf)
{
	if(!mrf)
		return NULL;

	return mrf->method;
}

// layer to proxy differences between old and new Mono
void
mono_unity_runtime_set_main_args (int argc, const char* argv[])
{
	mono_set_commandline_arguments (argc, argv, NULL);
}

MonoString*
mono_unity_string_empty_wrapper ()
{
	return mono_string_new (mono_domain_get (), "");
}

MonoArray*
mono_unity_array_new_2d (MonoDomain *domain, MonoClass *eklass, size_t size0, size_t size1)
{
	mono_array_size_t sizes[] = { (mono_array_size_t)size0, (mono_array_size_t)size1 };
	MonoClass* ac = mono_array_class_get (eklass, 2);

	return mono_array_new_full (domain, ac, sizes, NULL);
}

MonoArray*
mono_unity_array_new_3d (MonoDomain *domain, MonoClass *eklass, size_t size0, size_t size1, size_t size2)
{
	mono_array_size_t sizes[] = { (mono_array_size_t)size0, (mono_array_size_t)size1, (mono_array_size_t)size2 };
	MonoClass* ac = mono_array_class_get (eklass, 3);

	return mono_array_new_full (domain, ac, sizes, NULL);
}

void
mono_unity_domain_set_config (MonoDomain *domain, const char *base_dir, const char *config_file_name)
{
	// nothing on old Mono
}

void
mono_unity_g_free (void* ptr)
{
	g_free (ptr);
}

MonoException*
mono_unity_loader_get_last_error_and_error_prepare_exception ()
{
	// We need to call these two methods to clear the thread local
	// loader error status in mono. If not we'll randomly process the error
	// the next time it's checked.
	void* last_error = mono_loader_get_last_error ();
	if (last_error == NULL)
		return NULL;

	return mono_loader_error_prepare_exception (last_error);
}

MonoClass*
mono_unity_class_get_generic_type_definition (MonoClass* klass)
{
	return klass->generic_class ? mono_class_get_generic_type_definition (klass) : NULL;
}

MonoClass*
mono_unity_class_get_generic_parameter_at (MonoClass* klass, guint32 index)
{
	if (!klass->generic_container || index >= klass->generic_container->type_argc)
		return NULL;

	return mono_class_from_generic_parameter (mono_generic_container_get_param (klass->generic_container, index), klass->image, FALSE);
}

guint32
mono_unity_class_get_generic_parameter_count (MonoClass* klass)
{
	if (!klass->generic_container)
		return 0;

	return klass->generic_container->type_argc;
}

static char* data_dir = NULL;
void
mono_unity_set_data_dir(const char* dir)
{
	if (data_dir)
		g_free(data_dir);

	data_dir = g_new(char*, strlen(dir) + 1);
	strcpy(data_dir, dir);
}
 
char*
mono_unity_get_data_dir()
{
	return data_dir;
}

MonoClass* mono_unity_class_get(MonoImage* image, guint32 type_token)
{
	return mono_class_get(image, type_token);
}

// unitytls is only available in new mono (mbe). This dummy makes sure that the editor does not need to distinguish between those versions.
void
mono_unity_install_unitytls_interface(mono_unity_unitytls_interface* callbacks)
{
}