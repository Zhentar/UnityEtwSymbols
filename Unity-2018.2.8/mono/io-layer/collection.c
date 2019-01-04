/*
 * collection.c:  Garbage collection for handles
 *
 * Author:
 *	Dick Porter (dick@ximian.com)
 *
 * (C) 2004-2006 Novell, Inc.
 */

#include <config.h>
#include <glib.h>
#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#if defined(PLATFORM_ANDROID)
	#include <sys/mman.h>
#endif
#include <unistd.h>

#include <mono/io-layer/wapi.h>
#include <mono/io-layer/collection.h>
#include <mono/io-layer/handles-private.h>

#define LOGDEBUG(...)
// #define LOGDEBUG(...) g_message(__VA_ARGS__)

static pthread_t collection_thread_id;
static pthread_cond_t collection_thread_wait_cond;
static pthread_mutex_t collection_thread_wait_mutex;
static gboolean collection_thread_enabled;

static gpointer collection_thread (gpointer unused G_GNUC_UNUSED)
{
	struct timespec sleepytime;
	{
		struct timeval now;
		gettimeofday (&now, NULL);
		sleepytime.tv_sec = now.tv_sec;
		sleepytime.tv_nsec = 0;
	}

	pthread_mutex_lock (&collection_thread_wait_mutex);
	while (collection_thread_enabled)
	{
		pthread_cond_timedwait (&collection_thread_wait_cond, &collection_thread_wait_mutex, &sleepytime);

		//_wapi_handle_dump ();
		_wapi_handle_update_refs ();

		/* This is an abuse of the collection thread, but it's
		 * better than creating a new thread just for one more
		 * function.
		 */
		_wapi_process_reap ();

		sleepytime.tv_sec += _WAPI_HANDLE_COLLECTION_UPDATE_INTERVAL;
	}
	pthread_mutex_unlock (&collection_thread_wait_mutex);
	pthread_exit (NULL);

	return(NULL);
}

void _wapi_collection_init (void)
{
	pthread_attr_t attr;
	int ret;
        int set_stacksize = 0;

        pthread_mutex_init(&collection_thread_wait_mutex, NULL);
        pthread_cond_init(&collection_thread_wait_cond, NULL);

		collection_thread_enabled = TRUE;

 retry:
        ret = pthread_attr_init (&attr);
        g_assert (ret == 0);

#if defined(HAVE_PTHREAD_ATTR_SETSTACKSIZE)
        if (set_stacksize == 0) {
			ret = pthread_attr_setstacksize (&attr, MAX (65536, PTHREAD_STACK_MIN));
			g_assert (ret == 0);
        } else if (set_stacksize == 1) {
			ret = pthread_attr_setstacksize (&attr, 131072);
			g_assert (ret == 0);
        }
#endif

        ret = pthread_create (&collection_thread_id, &attr, collection_thread,
                              NULL);
        if (ret != 0 && set_stacksize < 2) {
                set_stacksize++;
                goto retry;
        }
	if (ret != 0) {
		g_error ("%s: Couldn't create handle collection thread: %s",
			 __func__, g_strerror (ret));
	}
}

extern void _wapi_collection_shutdown (void)
{

	// Only clean up if the thread has been enabled
	if(collection_thread_enabled != FALSE)
	{
		// signal and wait for collection thread to fininsh
		pthread_mutex_lock (&collection_thread_wait_mutex);

		collection_thread_enabled = FALSE;

		pthread_cond_signal (&collection_thread_wait_cond);
		pthread_mutex_unlock (&collection_thread_wait_mutex);
		pthread_join (collection_thread_id, NULL);

		// cleanup
		pthread_cond_destroy (&collection_thread_wait_cond);
		pthread_mutex_destroy (&collection_thread_wait_mutex);
	}
}


void _wapi_handle_collect (void)
{
	guint32 count = _wapi_shared_layout->collection_count;
	int i, thr_ret;
	
	LOGDEBUG ("%s: (%d) Starting a collection", __func__, _wapi_getpid ());

	/* Become the collection master */
	thr_ret = _wapi_handle_lock_shared_handles ();
	g_assert (thr_ret == 0);
	
	LOGDEBUG ("%s: (%d) Master set", __func__, _wapi_getpid ());
	
	/* If count has changed, someone else jumped in as master */
	if (count == _wapi_shared_layout->collection_count) {
		guint32 too_old = (guint32)(time(NULL) & 0xFFFFFFFF) - _WAPI_HANDLE_COLLECTION_EXPIRED_INTERVAL;

		for (i = 0; i < _WAPI_HANDLE_INITIAL_COUNT; i++) {
			struct _WapiHandleShared *data;
			
			data = &_wapi_shared_layout->handles[i];
			if (data->timestamp < too_old) {
				LOGDEBUG ("%s: (%d) Deleting handle 0x%x", __func__, _wapi_getpid (), i);
				memset (&_wapi_shared_layout->handles[i], '\0', sizeof(struct _WapiHandleShared));
			}
		}

		for (i = 0; i < _wapi_fileshare_layout->hwm; i++) {
			struct _WapiFileShare *file_share = &_wapi_fileshare_layout->share_info[i];
			
			if (file_share->timestamp < too_old) {
				memset (file_share, '\0',
					sizeof(struct _WapiFileShare));
			}
		}

		InterlockedIncrement ((gint32 *)&_wapi_shared_layout->collection_count);
	}
	
	_wapi_handle_unlock_shared_handles ();

	LOGDEBUG ("%s: (%d) Collection done", __func__, _wapi_getpid ());
}
