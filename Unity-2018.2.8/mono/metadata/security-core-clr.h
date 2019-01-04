/*
 * security-core-clr.h: CoreCLR security
 *
 * Author:
 *	Mark Probst <mark.probst@gmail.com>
 *
 * (C) 2007 Novell, Inc
 */

#ifndef _MONO_METADATA_SECURITY_CORE_CLR_H_
#define _MONO_METADATA_SECURITY_CORE_CLR_H_

#include <mono/metadata/reflection.h>

typedef enum {
	/* We compare these values as integers, so the order must not
	   be changed. */
	MONO_SECURITY_CORE_CLR_TRANSPARENT = 0,
	MONO_SECURITY_CORE_CLR_SAFE_CRITICAL,
	MONO_SECURITY_CORE_CLR_CRITICAL
} MonoSecurityCoreCLRLevel;

extern gboolean mono_security_core_clr_test;

extern void mono_security_core_clr_check_inheritance (MonoClass *class) MONO_INTERNAL;
extern void mono_security_core_clr_check_override (MonoClass *class, MonoMethod *override, MonoMethod *base) MONO_INTERNAL;

extern void mono_security_core_clr_ensure_reflection_access_field (MonoClassField *field) MONO_INTERNAL;
extern void mono_security_core_clr_ensure_reflection_access_method (MonoMethod *method) MONO_INTERNAL;
extern gboolean mono_security_core_clr_ensure_delegate_creation (MonoMethod *method, gboolean throwOnBindFailure) MONO_INTERNAL;
extern MonoException* mono_security_core_clr_ensure_dynamic_method_resolved_object (gpointer ref, MonoClass *handle_class) MONO_INTERNAL;

extern gboolean mono_security_core_clr_can_access_internals (MonoImage *accessing, MonoImage* accessed) MONO_INTERNAL;

extern MonoSecurityCoreCLRLevel mono_security_core_clr_class_level (MonoClass *class) MONO_INTERNAL;
extern MonoSecurityCoreCLRLevel mono_security_core_clr_method_level (MonoMethod *method, gboolean with_class_level) MONO_INTERNAL;

extern gboolean mono_security_core_clr_is_platform_image (MonoImage *image) MONO_INTERNAL;
extern gboolean mono_security_core_clr_determine_platform_image (MonoImage *image) MONO_INTERNAL;

#endif	/* _MONO_METADATA_SECURITY_CORE_CLR_H_ */
