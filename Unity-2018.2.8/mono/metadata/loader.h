#ifndef _MONO_METADATA_LOADER_H_
#define _MONO_METADATA_LOADER_H_ 1

#include <mono/metadata/metadata.h>
#include <mono/metadata/image.h>

G_BEGIN_DECLS

typedef gboolean (*MonoStackWalk)     (MonoMethod *method, gint32 native_offset, gint32 il_offset, gboolean managed, gpointer data);

MonoMethod *
mono_get_method             (MonoImage *image, guint32 token, MonoClass *klass);

MonoMethod *
mono_get_method_full        (MonoImage *image, guint32 token, MonoClass *klass,
			     MonoGenericContext *context);

MonoMethod *
mono_get_method_constrained (MonoImage *image, guint32 token, MonoClass *constrained_class,
			     MonoGenericContext *context, MonoMethod **cil_method);

void               
mono_free_method           (MonoMethod *method);

MonoMethodSignature*
mono_method_get_signature_full (MonoMethod *method, MonoImage *image, guint32 token,
				MonoGenericContext *context);

MonoMethodSignature* 
mono_method_get_signature  (MonoMethod *method, MonoImage *image, guint32 token);

MonoMethodSignature* 
mono_method_signature      (MonoMethod *method);

MonoMethodHeader* 
mono_method_get_header     (MonoMethod *method);

const char*
mono_method_get_name       (MonoMethod *method);

MonoClass*
mono_method_get_class      (MonoMethod *method);

guint32
mono_method_get_token      (MonoMethod *method);

guint32
mono_method_get_flags      (MonoMethod *method, guint32 *iflags);

guint32
mono_method_get_index      (MonoMethod *method);

MonoImage *
mono_load_image            (const char *fname, MonoImageOpenStatus *status);

void
mono_add_internal_call     (const char *name, gconstpointer method);

gpointer
mono_lookup_internal_call (MonoMethod *method);

void
mono_dllmap_insert (MonoImage *assembly, const char *dll, const char *func, const char *tdll, const char *tfunc);

gpointer
mono_lookup_pinvoke_call (MonoMethod *method, const char **exc_class, const char **exc_arg);

void
mono_method_get_param_names (MonoMethod *method, const char **names);

guint32
mono_method_get_param_token (MonoMethod *method, int idx);

void
mono_method_get_marshal_info (MonoMethod *method, MonoMarshalSpec **mspecs);

gboolean
mono_method_has_marshal_info (MonoMethod *method);

MonoMethod*
mono_method_get_last_managed  (void);

void
mono_stack_walk         (MonoStackWalk func, gpointer user_data);

/* Use this if the IL offset is not needed: it's faster */
void
mono_stack_walk_no_il   (MonoStackWalk func, gpointer user_data);

G_END_DECLS

#endif

