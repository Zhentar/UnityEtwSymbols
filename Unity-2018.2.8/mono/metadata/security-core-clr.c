/*
 * security-core-clr.c: CoreCLR security
 *
 * Authors:
 *	Mark Probst <mark.probst@gmail.com>
 *	Sebastien Pouliot  <sebastien@ximian.com>
 *
 * Copyright 2007-2009 Novell, Inc (http://www.novell.com)
 */

#include <mono/metadata/class-internals.h>
#include <mono/metadata/security-manager.h>
#include <mono/metadata/assembly.h>
#include <mono/metadata/appdomain.h>
#include <mono/metadata/verify-internals.h>
#include <mono/metadata/object.h>
#include <mono/metadata/exception.h>

#include "security-core-clr.h"

gboolean mono_security_core_clr_test = FALSE;

static MonoClass*
security_critical_attribute (void)
{
	static MonoClass *class = NULL;

	if (!class) {
		class = mono_class_from_name (mono_defaults.corlib, "System.Security", 
			"SecurityCriticalAttribute");
	}
	g_assert (class);
	return class;
}

static MonoClass*
security_safe_critical_attribute (void)
{
	static MonoClass *class = NULL;

	if (!class) {
		class = mono_class_from_name (mono_defaults.corlib, "System.Security", 
			"SecuritySafeCriticalAttribute");
	}
	g_assert (class);
	return class;
}

/*
 * mono_security_core_clr_check_inheritance:
 *
 *	Determine if the specified class can inherit from its parent using 
 * 	the CoreCLR inheritance rules.
 *
 *	Base Type	Allow Derived Type
 *	------------	------------------
 *	Transparent	Transparent, SafeCritical, Critical
 *	SafeCritical	SafeCritical, Critical
 *	Critical	Critical
 *
 *	Reference: http://msdn.microsoft.com/en-us/magazine/cc765416.aspx#id0190030
 */
void
mono_security_core_clr_check_inheritance (MonoClass *class)
{
	MonoSecurityCoreCLRLevel class_level, parent_level;
	MonoClass *parent = class->parent;

	if (!parent)
		return;

	class_level = mono_security_core_clr_class_level (class);
	parent_level = mono_security_core_clr_class_level (parent);

	if (class_level < parent_level)
		mono_class_set_failure (class, MONO_EXCEPTION_TYPE_LOAD, NULL);
}

/*
 * mono_security_core_clr_check_override:
 *
 *	Determine if the specified override can "legally" override the 
 *	specified base method using the CoreCLR inheritance rules.
 *
 *	Base (virtual/interface)	Allowed override
 *	------------------------	-------------------------
 *	Transparent			Transparent, SafeCritical
 *	SafeCritical			Transparent, SafeCritical
 *	Critical			Critical
 *
 *	Reference: http://msdn.microsoft.com/en-us/magazine/cc765416.aspx#id0190030
 */
void
mono_security_core_clr_check_override (MonoClass *class, MonoMethod *override, MonoMethod *base)
{
	MonoSecurityCoreCLRLevel base_level = mono_security_core_clr_method_level (base, FALSE);
	MonoSecurityCoreCLRLevel override_level = mono_security_core_clr_method_level (override, FALSE);
	/* if the base method is decorated with [SecurityCritical] then the overrided method MUST be too */
	if (base_level == MONO_SECURITY_CORE_CLR_CRITICAL) {
		if (override_level != MONO_SECURITY_CORE_CLR_CRITICAL)
			mono_class_set_failure (class, MONO_EXCEPTION_TYPE_LOAD, NULL);
	} else {
		/* base is [SecuritySafeCritical] or [SecurityTransparent], override MUST NOT be [SecurityCritical] */
		if (override_level == MONO_SECURITY_CORE_CLR_CRITICAL)
			mono_class_set_failure (class, MONO_EXCEPTION_TYPE_LOAD, NULL);
	}
}

/*
 * get_caller_no_reflection_related:
 *
 *	Find the first managed caller that is either:
 *	(a) located outside the platform code assemblies; or
 *	(b) not related to reflection and delegates
 *
 *	Returns TRUE to stop the stackwalk, FALSE to continue to the next frame.
 */
static gboolean
get_caller_no_reflection_related (MonoMethod *m, gint32 no, gint32 ilo, gboolean managed, gpointer data)
{
	MonoMethod **dest = data;
	const char *ns;

	/* skip unmanaged frames */
	if (!managed)
		return FALSE;

	if (m->wrapper_type != MONO_WRAPPER_NONE)
		return FALSE;

	/* quick out (any namespace not starting with an 'S' */
	ns = m->klass->name_space;
	if (!ns || (*ns != 'S')) {
		*dest = m;
		return TRUE;
	}

	/* stop if the method is not part of platform code */
	if (!mono_security_core_clr_is_platform_image (m->klass->image)) {
		*dest = m;
		return TRUE;
	}

	/* any number of calls inside System.Reflection are allowed */
	if (strcmp (ns, "System.Reflection") == 0)
		return FALSE;

	/* any number of calls inside System.Reflection are allowed */
	if (strcmp (ns, "System.Reflection.Emit") == 0)
		return FALSE;

	/* calls from System.Delegate are also possible and allowed */
	if (strcmp (ns, "System") == 0) {
		const char *kname = m->klass->name;
		if ((*kname == 'A') && (strcmp (kname, "Activator") == 0))
			return FALSE;

		/* unlike most Invoke* cases InvokeMember is not inside System.Reflection[.Emit] but is SecuritySafeCritical */
		if (((*kname == 'T') && (strcmp (kname, "Type") == 0)) || 
			((*kname == 'M') && (strcmp (kname, "MonoType")) == 0)) {

			/* if calling InvokeMember then we can't stop the stackwalk here and need to look at the caller */
			if (strcmp (m->name, "InvokeMember") == 0)
				return FALSE;
		}

		/* the security check on the delegate is made at creation time, not at invoke time */
		if (((*kname == 'D') && (strcmp (kname, "Delegate") == 0)) || 
			((*kname == 'M') && (strcmp (kname, "MulticastDelegate")) == 0)) {

			/* if we're invoking then we can stop our stack walk */
			if (strcmp (m->name, "DynamicInvoke") != 0)
				return FALSE;
		}
	}

	if (m == *dest) {
		*dest = NULL;
		return FALSE;
	}

	*dest = m;
	return TRUE;
}

/*
 * get_reflection_caller:
 * 
 *	Walk to the first managed method outside:
 *	- System.Reflection* namespaces
 *	- System.[MulticastDelegate]Delegate or Activator type
 *	- platform code
 *	and return a pointer to its MonoMethod.
 *
 *	This is required since CoreCLR checks needs to be done on this "real" caller.
 */
static MonoMethod*
get_reflection_caller (void)
{
	MonoMethod *m = NULL;
	mono_stack_walk_no_il (get_caller_no_reflection_related, &m);
	if (!m)
		g_warning ("could not find a caller outside reflection");
	return m;
}

/*
 * check_field_access:
 *
 *	Return TRUE if the caller method can access the specified field, FALSE otherwise.
 */
static gboolean
check_field_access (MonoMethod *caller, MonoClassField *field)
{
	/* if get_reflection_caller returns NULL then we assume the caller has NO privilege */
	if (caller) {
		MonoClass *klass = (mono_field_get_flags (field) & FIELD_ATTRIBUTE_STATIC) ? NULL : mono_field_get_parent (field);
		return mono_method_can_access_field_full (caller, field, klass);
	}
	return FALSE;
}

/*
 * check_method_access:
 *
 *	Return TRUE if the caller method can access the specified callee method, FALSE otherwise.
 */
static gboolean
check_method_access (MonoMethod *caller, MonoMethod *callee)
{
	/* if get_reflection_caller returns NULL then we assume the caller has NO privilege */
	if (caller) {
		MonoClass *klass = (callee->flags & METHOD_ATTRIBUTE_STATIC) ? NULL : callee->klass;
		return mono_method_can_access_method_full (caller, callee, klass);
	}
	return FALSE;
}

/*
 * mono_security_core_clr_ensure_reflection_access_field:
 *
 *	Ensure that the specified field can be used with reflection since 
 *	Transparent code cannot access to Critical fields and can only use
 *	them if they are visible from it's point of view.
 *
 *	A FieldAccessException is thrown if the field is cannot be accessed.
 */
void
mono_security_core_clr_ensure_reflection_access_field (MonoClassField *field)
{
	MonoMethod *caller = get_reflection_caller ();
	/* CoreCLR restrictions applies to Transparent code/caller */
	if (mono_security_core_clr_method_level (caller, TRUE) != MONO_SECURITY_CORE_CLR_TRANSPARENT)
		return;

	/* if the target field is in a non-platform assembly, everything goes, because that can't do no harm anyway */
	if (!mono_security_core_clr_is_platform_image (mono_field_get_parent(field)->image))
		return;

	/* Transparent code cannot [get|set]value on Critical fields */
	if (mono_security_core_clr_class_level (mono_field_get_parent (field)) == MONO_SECURITY_CORE_CLR_CRITICAL)
		mono_raise_exception (mono_get_exception_field_access ());

	/* also it cannot access a fields that is not visible from it's (caller) point of view */
	if (!check_field_access (caller, field))
		mono_raise_exception (mono_get_exception_field_access ());
}

/*
 * mono_security_core_clr_ensure_reflection_access_method:
 *
 *	Ensure that the specified method can be used with reflection since
 *	Transparent code cannot call Critical methods and can only call them
 *	if they are visible from it's point of view.
 *
 *	A MethodAccessException is thrown if the field is cannot be accessed.
 */
void
mono_security_core_clr_ensure_reflection_access_method (MonoMethod *method)
{
	MonoMethod *caller = get_reflection_caller ();
	/* CoreCLR restrictions applies to Transparent code/caller */
	if (mono_security_core_clr_method_level (caller, TRUE) != MONO_SECURITY_CORE_CLR_TRANSPARENT)
		return;

	/* if the target method is in a non-platform assembly, everything goes, because that can't do no harm anyway */
	if (!mono_security_core_clr_is_platform_image (method->klass->image))
		return;

	/* Transparent code cannot invoke, even using reflection, Critical code */
	if (mono_security_core_clr_method_level (method, TRUE) == MONO_SECURITY_CORE_CLR_CRITICAL)
		mono_raise_exception (mono_get_exception_method_access ());

	/* also it cannot invoke a method that is not visible from it's (caller) point of view */
	if (!check_method_access (caller, method))
		mono_raise_exception (mono_get_exception_method_access ());
}

/*
 * can_avoid_corlib_reflection_delegate_optimization:
 *
 *	Mono's mscorlib use delegates to optimize PropertyInfo and EventInfo
 *	reflection calls. This requires either a bunch of additional, and not
 *	really required, [SecuritySafeCritical] in the class libraries or 
 *	(like this) a way to skip them. As a bonus we also avoid the stack
 *	walk to find the caller.
 *
 *	Return TRUE if we can skip this "internal" delegate creation, FALSE
 *	otherwise.
 */
static gboolean
can_avoid_corlib_reflection_delegate_optimization (MonoMethod *method)
{
	if (!mono_security_core_clr_is_platform_image (method->klass->image))
		return FALSE;

	if (strcmp (method->klass->name_space, "System.Reflection") != 0)
		return FALSE;

	if (strcmp (method->klass->name, "MonoProperty") == 0) {
		if ((strcmp (method->name, "GetterAdapterFrame") == 0) || strcmp (method->name, "StaticGetterAdapterFrame") == 0)
			return TRUE;
	} else if (strcmp (method->klass->name, "EventInfo") == 0) {
		if ((strcmp (method->name, "AddEventFrame") == 0) || strcmp (method->name, "StaticAddEventAdapterFrame") == 0)
			return TRUE;
	}

	return FALSE;
}

/*
 * mono_security_core_clr_ensure_delegate_creation:
 *
 *	Return TRUE if a delegate can be created on the specified method. 
 *	CoreCLR also affect the binding, so throwOnBindFailure must be 
 * 	FALSE to let this function return (FALSE) normally, otherwise (if
 *	throwOnBindFailure is TRUE) it will throw an ArgumentException.
 *
 *	A MethodAccessException is thrown if the specified method is not
 *	visible from the caller point of view.
 */
gboolean
mono_security_core_clr_ensure_delegate_creation (MonoMethod *method, gboolean throwOnBindFailure)
{
	MonoMethod *caller;

	/* note: mscorlib creates delegates to avoid reflection (optimization), we ignore those cases */
	if (can_avoid_corlib_reflection_delegate_optimization (method))
		return TRUE;

	caller = get_reflection_caller ();

	/* if the caller is excluded from the coreclr system, it can do whatever it wants */
	if (!mono_security_core_clr_enabled_for_method(caller))
		return TRUE;

	/* if the "real" caller is not Transparent then it do can anything */
	if (mono_security_core_clr_method_level (caller, TRUE) != MONO_SECURITY_CORE_CLR_TRANSPARENT)
		return TRUE;

	/* otherwise it (as a Transparent caller) cannot create a delegate on a Critical method... */
	if (mono_security_core_clr_method_level (method, TRUE) == MONO_SECURITY_CORE_CLR_CRITICAL) {
		/* but this throws only if 'throwOnBindFailure' is TRUE */
		if (!throwOnBindFailure)
			return FALSE;

		mono_raise_exception (mono_get_exception_argument ("method", "Transparent code cannot call Critical code"));
	}
	
	// lucas added the platform check. If the target is not in a platform assembly, everything is fine.
	if (!mono_security_core_clr_is_platform_image (method->klass->image))
		return TRUE;

	/* also it cannot create the delegate on a method that is not visible from it's (caller) point of view */
	if (!check_method_access (caller, method))
		mono_raise_exception (mono_get_exception_method_access ());

	return TRUE;
}

/*
 * mono_security_core_clr_ensure_dynamic_method_resolved_object:
 *
 *	Called from mono_reflection_create_dynamic_method (reflection.c) to add some extra checks required for CoreCLR.
 *	Dynamic methods needs to check to see if the objects being used (e.g. methods, fields) comes from platform code
 *	and do an accessibility check in this case. Otherwise (i.e. user/application code) can be used without this extra
 *	accessbility check.
 */
MonoException*
mono_security_core_clr_ensure_dynamic_method_resolved_object (gpointer ref, MonoClass *handle_class)
{
	/* XXX find/create test cases for other handle_class XXX */
	if (handle_class == mono_defaults.fieldhandle_class) {
		MonoClassField *field = (MonoClassField*) ref;
		MonoClass *klass = mono_field_get_parent (field);
		/* fields coming from platform code have extra protection (accessibility check) */
		if (mono_security_core_clr_is_platform_image (klass->image)) {
			MonoMethod *caller = get_reflection_caller ();
			/* XXX Critical code probably can do this / need some test cases (safer off otherwise) XXX */
			if (!check_field_access (caller, field))
				return mono_get_exception_field_access ();
		}
	} else if (handle_class == mono_defaults.methodhandle_class) {
		MonoMethod *method = (MonoMethod*) ref;
		/* methods coming from platform code have extra protection (accessibility check) */
		if (mono_security_core_clr_is_platform_image (method->klass->image)) {
			MonoMethod *caller = get_reflection_caller ();
			/* XXX Critical code probably can do this / need some test cases (safer off otherwise) XXX */
			if (!check_method_access (caller, method))
				return mono_get_exception_method_access ();
		}
	}
	return NULL;
}

/*
 * mono_security_core_clr_can_access_internals
 *
 *	Check if we allow [InternalsVisibleTo] to work between two images.
 */
gboolean
mono_security_core_clr_can_access_internals (MonoImage *accessing, MonoImage* accessed)
{
	/* are we trying to access internals of a platform assembly ? if not this is acceptable */
	if (!mono_security_core_clr_is_platform_image (accessed))
		return TRUE;

	/* we can't let everyone with the right name and public key token access the internals of platform code.
	 * (Silverlight can rely on the strongname signature of the assemblies, but Mono does not verify them)
	 * However platform code is fully trusted so it can access the internals of other platform code assemblies */
	if (mono_security_core_clr_is_platform_image (accessing))
		return TRUE;

	/* catch-22: System.Xml needs access to mscorlib's internals (e.g. ArrayList) but is not considered platform code.
	 * Promoting it to platform code would create another issue since (both Mono/Moonlight or MS version of) 
	 * System.Xml.Linq.dll (an SDK, not platform, assembly) needs access to System.Xml.dll internals (either ). 
	 * The solution is to trust, even transparent code, in the plugin directory to access platform code internals */
	if (!accessed->assembly->basedir || !accessing->assembly->basedir)
		return FALSE;
	return (strcmp (accessed->assembly->basedir, accessing->assembly->basedir) == 0);
}

/*
 * mono_security_core_clr_level_from_cinfo:
 *
 *	Return the MonoSecurityCoreCLRLevel that match the attribute located
 *	in the specified custom attributes. If no attribute is present it 
 *	defaults to MONO_SECURITY_CORE_CLR_TRANSPARENT, which is the default
 *	level for all code under the CoreCLR.
 */
static MonoSecurityCoreCLRLevel
mono_security_core_clr_level_from_cinfo (MonoCustomAttrInfo *cinfo, MonoImage *image)
{
	int level = MONO_SECURITY_CORE_CLR_TRANSPARENT;

	if (cinfo && mono_custom_attrs_has_attr (cinfo, security_safe_critical_attribute ()))
		level = MONO_SECURITY_CORE_CLR_SAFE_CRITICAL;
	if (cinfo && mono_custom_attrs_has_attr (cinfo, security_critical_attribute ()))
		level = MONO_SECURITY_CORE_CLR_CRITICAL;

	return level;
}

/*
 * mono_security_core_clr_class_level_no_platform_check:
 *
 *	Return the MonoSecurityCoreCLRLevel for the specified class, without 
 *	checking for platform code. This help us avoid multiple redundant 
 *	checks, e.g.
 *	- a check for the method and one for the class;
 *	- a check for the class and outer class(es) ...
 */
static MonoSecurityCoreCLRLevel
mono_security_core_clr_class_level_no_platform_check (MonoClass *class)
{
	MonoSecurityCoreCLRLevel level = MONO_SECURITY_CORE_CLR_TRANSPARENT;
	MonoCustomAttrInfo *cinfo = mono_custom_attrs_from_class (class);

	/* if the accessing assembly is excluded from coreclr rules, it can do whatever it feels like, and be called from anything.*/
	if (!mono_security_core_clr_enabled_for_class(class))
		return MONO_SECURITY_CORE_CLR_SAFE_CRITICAL;

	if (cinfo) {
		level = mono_security_core_clr_level_from_cinfo (cinfo, class->image);
		if (!cinfo->cached)
			mono_custom_attrs_free (cinfo);
	}

	if (level == MONO_SECURITY_CORE_CLR_TRANSPARENT && class->nested_in)
		level = mono_security_core_clr_class_level_no_platform_check (class->nested_in);

	return level;
}

/*
 * mono_security_core_clr_class_level:
 *
 *	Return the MonoSecurityCoreCLRLevel for the specified class.
 */
MonoSecurityCoreCLRLevel
mono_security_core_clr_class_level (MonoClass *class)
{
	/* non-platform code is always Transparent - whatever the attributes says */
	if (!mono_security_core_clr_test && !mono_security_core_clr_is_platform_image (class->image))
		return MONO_SECURITY_CORE_CLR_TRANSPARENT;

	return mono_security_core_clr_class_level_no_platform_check (class);
}

gboolean
mono_security_core_clr_enabled_for_method(MonoMethod* method)
{
	return mono_security_core_clr_enabled_for_class(method->klass);
}

gboolean
mono_security_core_clr_enabled_for_class(MonoClass* klass)
{
	return mono_security_core_clr_enabled_for_image(klass->image);
}

gboolean
mono_security_core_clr_enabled_for_image(MonoImage* image)
{
	return 1;
	/*
	if (strstr(image->name,"UnityEditor") != NULL)
		return 0;
	else
		return 1;*/
}

/*
 * mono_security_core_clr_method_level:
 *
 *	Return the MonoSecurityCoreCLRLevel for the specified method.
 *	If with_class_level is TRUE then the type (class) will also be
 *	checked, otherwise this will only report the information about
 *	the method itself.
 */
MonoSecurityCoreCLRLevel
mono_security_core_clr_method_level (MonoMethod *method, gboolean with_class_level)
{
	MonoCustomAttrInfo *cinfo;
	MonoSecurityCoreCLRLevel level = MONO_SECURITY_CORE_CLR_TRANSPARENT;

	/* if get_reflection_caller returns NULL then we assume the caller has NO privilege */
	if (!method)
		return level;

	/* non-platform code is always Transparent - whatever the attributes says */
	if (!mono_security_core_clr_test && !mono_security_core_clr_is_platform_image (method->klass->image))
		return level;

	/* methods excluded from the coreclr system can do whatever they feel like */
	if (!mono_security_core_clr_enabled_for_method(method))
		return MONO_SECURITY_CORE_CLR_SAFE_CRITICAL;

	cinfo = mono_custom_attrs_from_method (method);
	if (cinfo) {
		level = mono_security_core_clr_level_from_cinfo (cinfo, method->klass->image);
		if (!cinfo->cached)
			mono_custom_attrs_free (cinfo);
	}

	if (with_class_level && level == MONO_SECURITY_CORE_CLR_TRANSPARENT)
		level = mono_security_core_clr_class_level (method->klass);

	return level;
}

/*
 * mono_security_core_clr_is_platform_image:
 *
 *   Return the (cached) boolean value indicating if this image represent platform code
 */
gboolean
mono_security_core_clr_is_platform_image (MonoImage *image)
{
	return image && image->core_clr_platform_code;
}

/*
 * default_platform_check:
 *
 *	Default platform check. Always TRUE for current corlib (minimum 
 *	trust-able subset) otherwise return FALSE. Any real CoreCLR host
 *	should provide its own callback to define platform code (i.e.
 *	this default is meant for test only).
 */
static gboolean
default_platform_check (const char *image_name)
{
	if (mono_defaults.corlib) {
		return (strcmp (mono_defaults.corlib->name, image_name) == 0);
	} else {
		/* this can get called even before we load corlib (e.g. the EXE itself) */
		const char *corlib = "mscorlib.dll";
		int ilen = strlen (image_name);
		int clen = strlen (corlib);
		return ((ilen >= clen) && (strcmp ("mscorlib.dll", image_name + ilen - clen) == 0));
	}
}

static MonoCoreClrPlatformCB platform_callback = default_platform_check;

/*
 * mono_security_core_clr_determine_platform_image:
 *
 *	Call the supplied callback (from mono_security_set_core_clr_platform_callback) 
 *	to determine if this image represents platform code.
 */
gboolean
mono_security_core_clr_determine_platform_image (MonoImage *image)
{
	return platform_callback (image->name);
}

/*
 * mono_security_enable_core_clr:
 *
 *   Enable the verifier and the CoreCLR security model
 */
void
mono_security_enable_core_clr ()
{
	mono_verifier_set_mode (MONO_VERIFIER_MODE_VERIFIABLE);
	mono_security_set_mode (MONO_SECURITY_MODE_CORE_CLR);
}

/*
 * mono_security_set_core_clr_platform_callback:
 *
 *	Set the callback function that will be used to determine if an image
 *	is part, or not, of the platform code.
 */
void
mono_security_set_core_clr_platform_callback (MonoCoreClrPlatformCB callback)
{
	platform_callback = callback;
}

