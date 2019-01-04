#ifndef _MONONET_METADATA_ASSEMBLY_H_ 
#define _MONONET_METADATA_ASSEMBLY_H_

#include <glib.h>

#include <mono/metadata/image.h>

G_BEGIN_DECLS

void          mono_assemblies_init     (void);
void          mono_assemblies_cleanup  (void);
MonoAssembly *mono_assembly_open       (const char *filename,
				       	MonoImageOpenStatus *status);
MonoAssembly *mono_assembly_open_full (const char *filename,
				       	MonoImageOpenStatus *status,
					gboolean refonly);
MonoAssembly* mono_assembly_load       (MonoAssemblyName *aname, 
                                       	const char       *basedir, 
				     	MonoImageOpenStatus *status);
MonoAssembly* mono_assembly_load_full (MonoAssemblyName *aname, 
                                       	const char       *basedir, 
				     	MonoImageOpenStatus *status,
					gboolean refonly);
MonoAssembly* mono_assembly_load_from  (MonoImage *image, const char *fname,
					MonoImageOpenStatus *status);
MonoAssembly* mono_assembly_load_from_full  (MonoImage *image, const char *fname,
					MonoImageOpenStatus *status,
					gboolean refonly);

MonoAssembly* mono_assembly_load_with_partial_name (const char *name, MonoImageOpenStatus *status);

MonoAssembly* mono_assembly_loaded     (MonoAssemblyName *aname);
MonoAssembly* mono_assembly_loaded_full (MonoAssemblyName *aname, gboolean refonly);
void          mono_assembly_get_assemblyref (MonoImage *image, int index, MonoAssemblyName *aname);
void          mono_assembly_load_reference (MonoImage *image, int index);
void          mono_assembly_load_references (MonoImage *image, MonoImageOpenStatus *status);
MonoImage*    mono_assembly_load_module (MonoAssembly *assembly, guint32 idx);
void          mono_assembly_close      (MonoAssembly *assembly);
void          mono_assembly_setrootdir (const char *root_dir);
G_CONST_RETURN gchar *mono_assembly_getrootdir (void);
void	      mono_assembly_foreach    (GFunc func, gpointer user_data);
void          mono_assembly_set_main   (MonoAssembly *assembly);
MonoAssembly *mono_assembly_get_main   (void);
MonoImage    *mono_assembly_get_image  (MonoAssembly *assembly);
gboolean      mono_assembly_fill_assembly_name (MonoImage *image, MonoAssemblyName *aname);
gboolean      mono_assembly_names_equal (MonoAssemblyName *l, MonoAssemblyName *r);
gboolean      mono_assembly_names_equal2 (MonoAssemblyName *l, MonoAssemblyName *r, gboolean ignore_version_and_key);
char*         mono_stringify_assembly_name (MonoAssemblyName *aname);

/* Installs a function which is called each time a new assembly is loaded. */
typedef void  (*MonoAssemblyLoadFunc)         (MonoAssembly *assembly, gpointer user_data);
void          mono_install_assembly_load_hook (MonoAssemblyLoadFunc func, gpointer user_data);

/* 
 * Installs a new function which is used to search the list of loaded 
 * assemblies for a given assembly name.
 */
typedef MonoAssembly *(*MonoAssemblySearchFunc)         (MonoAssemblyName *aname, gpointer user_data);
void          mono_install_assembly_search_hook (MonoAssemblySearchFunc func, gpointer user_data);
void 	      mono_install_assembly_refonly_search_hook (MonoAssemblySearchFunc func, gpointer user_data);

MonoAssembly* mono_assembly_invoke_search_hook (MonoAssemblyName *aname);

/*
 * Installs a new search function which is used as a last resort when loading 
 * an assembly fails. This could invoke AssemblyResolve events.
 */
void          
mono_install_assembly_postload_search_hook (MonoAssemblySearchFunc func, gpointer user_data);

void          
mono_install_assembly_postload_refonly_search_hook (MonoAssemblySearchFunc func, gpointer user_data);


/* Installs a function which is called before a new assembly is loaded
 * The hook are invoked from last hooked to first. If any of them returns
 * a non-null value, that will be the value returned in mono_assembly_load */
typedef MonoAssembly * (*MonoAssemblyPreLoadFunc) (MonoAssemblyName *aname,
						   gchar **assemblies_path,
						   gpointer user_data);

void          mono_install_assembly_preload_hook (MonoAssemblyPreLoadFunc func,
						  gpointer user_data);
void          mono_install_assembly_refonly_preload_hook (MonoAssemblyPreLoadFunc func,
						  gpointer user_data);

void          mono_assembly_invoke_load_hook (MonoAssembly *ass);

typedef struct {
	const char *name;
	const unsigned char *data;
	const unsigned int size;
} MonoBundledAssembly;

void          mono_register_bundled_assemblies (const MonoBundledAssembly **assemblies);
void          mono_register_config_for_assembly (const char* assembly_name, const char* config_xml);
void	      mono_register_machine_config (const char *config_xml);

void          mono_set_rootdir (void);
void          mono_set_dirs (const char *assembly_dir, const char *config_dir);
G_END_DECLS
#endif

