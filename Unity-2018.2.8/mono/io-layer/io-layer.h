/*
 * io-layer.h: Include the right files depending on platform.  This
 * file is the only entry point into the io-layer library.
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2002 Ximian, Inc.
 */

#ifndef _MONO_IOLAYER_IOLAYER_H_
#define _MONO_IOLAYER_IOLAYER_H_

#if defined(__WIN32__)
/* Native win32 */
#define __USE_W32_SOCKETS
#if (_WIN32_WINNT < 0x0502)
/* GetProcessId is available on Windows XP SP1 and later.
 * Windows SDK declares it unconditionally.
 * MinGW declares for Windows XP and later.
 * Declare as __GetProcessId for unsupported targets. */
#define GetProcessId __GetProcessId
#endif
#include <winsock2.h>
#include <windows.h>
#include <winbase.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <shlobj.h>
#include <mswsock.h>
#if (_WIN32_WINNT < 0x0502)
#undef GetProcessId
#endif
#ifndef HAVE_GETPROCESSID
#ifdef _MSC_VER
#include <winternl.h>
#ifndef NT_SUCCESS
#define NT_SUCCESS(status) ((NTSTATUS) (status) >= 0)
#endif /* !NT_SUCCESS */
#else /* !_MSC_VER */
#include <ddk/ntddk.h>
#include <ddk/ntapi.h>
#endif /* _MSC_VER */
#endif /* !HAVE_GETPROCESSID */
#else	/* EVERYONE ELSE */
#include "mono/io-layer/wapi.h"
#include "mono/io-layer/uglify.h"
#endif /* PLATFORM_WIN32 */

#endif /* _MONO_IOLAYER_IOLAYER_H_ */
