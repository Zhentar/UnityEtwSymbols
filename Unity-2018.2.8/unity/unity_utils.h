#ifndef __UNITY_MONO_UTILS_H
#define __UNITY_MONO_UTILS_H

#include <stdio.h>
#include <mono/metadata/object.h>

/**
 *	Custom exit function, called instead of system exit()
 */
void unity_mono_exit( int code );

/**
 *	Redirects mono output where we want it.
 */
void unity_mono_redirect_output( const char *fout, const char *ferr );

/**
 *	Closes redirected output files.
 */
void unity_mono_close_output(void);

extern MonoString* mono_unity_get_embeddinghostname(void);

void mono_unity_write_to_unity_log(MonoString* str);

#ifdef WIN32
FILE* unity_fopen( const char *name, const char *mode );
#endif

extern gboolean mono_unity_socket_security_enabled_get (void);
extern void mono_unity_socket_security_enabled_set (gboolean enabled);
void mono_unity_set_vprintf_func(vprintf_func func);

void unity_mono_install_memory_callbacks(MonoMemoryCallbacks* callbacks);

gboolean
unity_mono_method_is_inflated (MonoMethod* method);

gboolean
unity_mono_method_is_generic (MonoMethod* method);

void mono_unity_set_data_dir(const char* dir);
char* mono_unity_get_data_dir();
MonoClass* mono_unity_class_get(MonoImage* image, guint32 type_token);


typedef struct mono_unity_unitytls_interface mono_unity_unitytls_interface;
void mono_unity_install_unitytls_interface(mono_unity_unitytls_interface* callbacks);

#endif
