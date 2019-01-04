/*
 * threadpool.c: global thread pool
 *
 * Authors:
 *   Dietmar Maurer (dietmar@ximian.com)
 *   Gonzalo Paniagua Javier (gonzalo@ximian.com)
 *
 * Copyright 2001-2003 Ximian, Inc (http://www.ximian.com)
 * Copyright 2004-2009 Novell, Inc (http://www.novell.com)
 */

#include <config.h>
#include <glib.h>

#define THREADS_PER_CPU	10 /* 20 + THREADS_PER_CPU * number of CPUs */
#define THREAD_EXIT_TIMEOUT 1000
#define INITIAL_QUEUE_LENGTH 128

#include <mono/metadata/domain-internals.h>
#include <mono/metadata/tabledefs.h>
#include <mono/metadata/threads.h>
#include <mono/metadata/threads-types.h>
#include <mono/metadata/threadpool-internals.h>
#include <mono/metadata/exception.h>
#include <mono/metadata/file-io.h>
#include <mono/metadata/monitor.h>
#include <mono/metadata/mono-mlist.h>
#include <mono/metadata/marshal.h>
#include <mono/metadata/socket-io.h>
#include <mono/io-layer/io-layer.h>
#include <mono/metadata/gc-internal.h>
#include <mono/utils/mono-time.h>
#include <mono/utils/mono-proclib.h>
#include <errno.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <fcntl.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <string.h>
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#include <mono/utils/mono-poll.h>
#ifdef HAVE_EPOLL
#include <sys/epoll.h>
#endif

#ifndef DISABLE_SOCKETS
#include "mono/io-layer/socket-wrappers.h"
#endif

#include "threadpool.h"

#define THREAD_WANTS_A_BREAK(t) ((t->state & (ThreadState_StopRequested | \
						ThreadState_SuspendRequested)) != 0)

#define THREAD_ABORT_REQUESTED(t) ((t->state & ThreadState_AbortRequested) != 0)

#undef EPOLL_DEBUG

/* maximum number of worker threads */
static int mono_max_worker_threads;
static int mono_min_worker_threads;
static int mono_io_max_worker_threads;
static int mono_io_min_worker_threads;

/* current number of worker threads */
static int mono_worker_threads = 0;
static int io_worker_threads = 0;

/* current number of busy threads */
static int busy_worker_threads = 0;
static int busy_io_worker_threads;

/* mono_thread_pool_init called */
static int tp_inited;
/* started idle threads */
static int tp_idle_started;

static CRITICAL_SECTION ares_lock;
static CRITICAL_SECTION io_queue_lock;
static int pending_io_items;

typedef struct {
	CRITICAL_SECTION io_lock; /* access to sock_to_state */
	int inited;
	int pipe [2];
	MonoGHashTable *sock_to_state;

	HANDLE new_sem; /* access to newpfd and write side of the pipe */
	mono_pollfd *newpfd;
	gboolean epoll_disabled;
#ifdef HAVE_EPOLL
	int epollfd;
#endif
} SocketIOData;

static SocketIOData socket_io_data;

/* we append a job */
static HANDLE job_added;
static HANDLE io_job_added;

/* Keep in sync with the System.MonoAsyncCall class which provides GC tracking */
typedef struct {
	MonoObject         object;
	MonoMethodMessage *msg;
	MonoMethod        *cb_method;
	MonoDelegate      *cb_target;
	MonoObject        *state;
	MonoObject        *res;
	MonoArray         *out_args;
	/* This is a HANDLE, we use guint64 so the managed object layout remains constant */
	guint64           wait_event;
} ASyncCall;

typedef struct {
	MonoArray *array;
	int first_elem;
	int next_elem;
} TPQueue;

static void async_invoke_thread (gpointer data);
static void append_job (CRITICAL_SECTION *cs, TPQueue *list, MonoObject *ar);
static void start_thread_or_queue (MonoAsyncResult *ares);
static void start_tpthread (MonoAsyncResult *ares);
static void mono_async_invoke (MonoAsyncResult *ares);
static MonoObject* dequeue_job (CRITICAL_SECTION *cs, TPQueue *list);
static void free_queue (TPQueue *list);

static TPQueue async_call_queue = {NULL, 0, 0};
static TPQueue async_io_queue = {NULL, 0, 0};

static MonoClass *async_call_klass;
static MonoClass *socket_async_call_klass;
static MonoClass *process_async_call_klass;

/* Hooks */
static MonoThreadPoolFunc tp_start_func;
static MonoThreadPoolFunc tp_finish_func;
static gpointer tp_hooks_user_data;
static MonoThreadPoolItemFunc tp_item_begin_func;
static MonoThreadPoolItemFunc tp_item_end_func;
static gpointer tp_item_user_data;

#define INIT_POLLFD(a, b, c) {(a)->fd = b; (a)->events = c; (a)->revents = 0;}
enum {
	AIO_OP_FIRST,
	AIO_OP_ACCEPT = 0,
	AIO_OP_CONNECT,
	AIO_OP_RECEIVE,
	AIO_OP_RECEIVEFROM,
	AIO_OP_SEND,
	AIO_OP_SENDTO,
	AIO_OP_RECV_JUST_CALLBACK,
	AIO_OP_SEND_JUST_CALLBACK,
	AIO_OP_READPIPE,
	AIO_OP_LAST
};

#ifdef DISABLE_SOCKETS
#define socket_io_cleanup(x)
#else
static void
socket_io_cleanup (SocketIOData *data)
{
	gint release;

	if (data->inited == 0)
		return;

	EnterCriticalSection (&data->io_lock);
	data->inited = 0;
#ifdef PLATFORM_WIN32
	closesocket (data->pipe [0]);
	closesocket (data->pipe [1]);
#else
	close (data->pipe [0]);
	close (data->pipe [1]);
#endif
	data->pipe [0] = -1;
	data->pipe [1] = -1;
	if (data->new_sem)
		CloseHandle (data->new_sem);
	data->new_sem = NULL;
	mono_g_hash_table_destroy (data->sock_to_state);
	data->sock_to_state = NULL;
	free_queue (&async_io_queue);
	release = (gint) InterlockedCompareExchange (&io_worker_threads, 0, -1);
	if (io_job_added)
		ReleaseSemaphore (io_job_added, release, NULL);
	g_free (data->newpfd);
	data->newpfd = NULL;
#ifdef HAVE_EPOLL
	if (FALSE == data->epoll_disabled)
		close (data->epollfd);
#endif
	LeaveCriticalSection (&data->io_lock);
}

static int
get_event_from_state (MonoSocketAsyncResult *state)
{
	switch (state->operation) {
	case AIO_OP_ACCEPT:
	case AIO_OP_RECEIVE:
	case AIO_OP_RECV_JUST_CALLBACK:
	case AIO_OP_RECEIVEFROM:
	case AIO_OP_READPIPE:
		return MONO_POLLIN;
	case AIO_OP_SEND:
	case AIO_OP_SEND_JUST_CALLBACK:
	case AIO_OP_SENDTO:
	case AIO_OP_CONNECT:
		return MONO_POLLOUT;
	default: /* Should never happen */
		g_print ("get_event_from_state: unknown value in switch!!!\n");
		return 0;
	}
}

static int
get_events_from_list (MonoMList *list)
{
	MonoSocketAsyncResult *state;
	int events = 0;

	while (list && (state = (MonoSocketAsyncResult *)mono_mlist_get_data (list))) {
		events |= get_event_from_state (state);
		list = mono_mlist_next (list);
	}

	return events;
}

#define ICALL_RECV(x)	ves_icall_System_Net_Sockets_Socket_Receive_internal (\
				(SOCKET)(gssize)x->handle, x->buffer, x->offset, x->size,\
				 x->socket_flags, &x->error);

#define ICALL_SEND(x)	ves_icall_System_Net_Sockets_Socket_Send_internal (\
				(SOCKET)(gssize)x->handle, x->buffer, x->offset, x->size,\
				 x->socket_flags, &x->error);

#endif /* !DISABLE_SOCKETS */

static void
unregister_job (MonoAsyncResult *obj)
{
}

static void
threadpool_jobs_inc (MonoObject *obj)
{
	if (obj)
		InterlockedIncrement (&obj->vtable->domain->threadpool_jobs);
}

static gboolean
threadpool_jobs_dec (MonoObject *obj)
{
	MonoDomain *domain = obj->vtable->domain;
	int remaining_jobs = InterlockedDecrement (&domain->threadpool_jobs);
	if (remaining_jobs == 0 && domain->cleanup_semaphore) {
		ReleaseSemaphore (domain->cleanup_semaphore, 1, NULL);
		return TRUE;
	}
	return FALSE;
}

#ifndef DISABLE_SOCKETS
static void
async_invoke_io_thread (gpointer data)
{
	MonoDomain *domain;
	MonoThread *thread;
	const gchar *version;
	int workers_io, min_io;

	thread = mono_thread_current ();
 	if (tp_start_func)
 		tp_start_func (tp_hooks_user_data);

	version = mono_get_runtime_info ()->framework_version;
	for (;;) {
		MonoSocketAsyncResult *state;
		MonoAsyncResult *ar;

		state = (MonoSocketAsyncResult *) data;
		if (state) {
			InterlockedDecrement (&pending_io_items);
			ar = state->ares;
			switch (state->operation) {
			case AIO_OP_RECEIVE:
				state->total = ICALL_RECV (state);
				break;
			case AIO_OP_SEND:
				state->total = ICALL_SEND (state);
				break;
			}

			/* worker threads invokes methods in different domains,
			 * so we need to set the right domain here */
			domain = ((MonoObject *)ar)->vtable->domain;

			g_assert (domain);

			if (domain->state == MONO_APPDOMAIN_UNLOADED || domain->state == MONO_APPDOMAIN_UNLOADING) {
				threadpool_jobs_dec ((MonoObject *)ar);
				unregister_job (ar);
				data = NULL;
			} else {
				mono_thread_push_appdomain_ref (domain);
				if (threadpool_jobs_dec ((MonoObject *)ar)) {
					unregister_job (ar);
					data = NULL;
					mono_thread_pop_appdomain_ref ();
					continue;
				}
				if (mono_domain_set (domain, FALSE)) {
					/* ASyncCall *ac; */

					if (tp_item_begin_func)
						tp_item_begin_func (tp_item_user_data);
					mono_async_invoke (ar);
					if (tp_item_end_func)
						tp_item_end_func (tp_item_user_data);
					/*
					ac = (ASyncCall *) ar->object_data;
					if (ac->msg->exc != NULL)
						mono_unhandled_exception (ac->msg->exc);
					*/
					mono_domain_set (mono_get_root_domain (), TRUE);
				}
				mono_thread_pop_appdomain_ref ();
				InterlockedDecrement (&busy_io_worker_threads);
				/* If the callee changes the background status, set it back to TRUE */
				if (*version != '1' && !mono_thread_test_state (thread , ThreadState_Background))
					ves_icall_System_Threading_Thread_SetState (thread, ThreadState_Background);
			}
		}

		data = dequeue_job (&io_queue_lock, &async_io_queue);
	
		if (!data && !mono_runtime_is_shutting_down() && !THREAD_ABORT_REQUESTED(thread)) {
			guint32 wr;
			int timeout = THREAD_EXIT_TIMEOUT;
			guint32 start_time = mono_msec_ticks ();
			
			do {
				wr = WaitForSingleObjectEx (io_job_added, (guint32)timeout, TRUE);
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			
				timeout -= mono_msec_ticks () - start_time;
			
				if (wr != WAIT_TIMEOUT && wr != WAIT_IO_COMPLETION)
					data = dequeue_job (&io_queue_lock, &async_io_queue);
			}
			while (!data && timeout > 0 && !mono_runtime_is_shutting_down() && !THREAD_ABORT_REQUESTED(thread));
		}

		if (!data) {
			workers_io = (int) InterlockedCompareExchange (&io_worker_threads, 0, -1); 
			min_io = (int) InterlockedCompareExchange (&mono_io_min_worker_threads, 0, -1); 
	
			while (!data && workers_io <= min_io && !mono_runtime_is_shutting_down() && !THREAD_ABORT_REQUESTED(thread)) {
				WaitForSingleObjectEx (io_job_added, INFINITE, TRUE);
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			
				data = dequeue_job (&io_queue_lock, &async_io_queue);
				workers_io = (int) InterlockedCompareExchange (&io_worker_threads, 0, -1); 
				min_io = (int) InterlockedCompareExchange (&mono_io_min_worker_threads, 0, -1); 
			}
		}
	
		if (!data) {
			InterlockedDecrement (&io_worker_threads);
 			if (tp_finish_func)
 				tp_finish_func (tp_hooks_user_data);
			return;
		}
		
		InterlockedIncrement (&busy_io_worker_threads);
	}

	g_assert_not_reached ();
}

static void
start_io_thread_or_queue (MonoSocketAsyncResult *ares)
{
	int busy, worker;

	busy = (int) InterlockedCompareExchange (&busy_io_worker_threads, 0, -1);
	worker = (int) InterlockedCompareExchange (&io_worker_threads, 0, -1); 
	if (worker <= ++busy &&
	    worker < mono_io_max_worker_threads) {
		InterlockedIncrement (&busy_io_worker_threads);
		InterlockedIncrement (&io_worker_threads);
		threadpool_jobs_inc ((MonoObject *)ares);
		mono_thread_create_internal (mono_get_root_domain (), async_invoke_io_thread, ares, TRUE);
	} else {
		append_job (&io_queue_lock, &async_io_queue, (MonoObject*)ares);
		ReleaseSemaphore (io_job_added, 1, NULL);
	}
}

static MonoMList *
process_io_event (MonoMList *list, int event)
{
	MonoSocketAsyncResult *state;
	MonoMList *oldlist;

	oldlist = list;
	state = NULL;
	while (list) {
		state = (MonoSocketAsyncResult *) mono_mlist_get_data (list);
		if (get_event_from_state (state) == event)
			break;
		
		list = mono_mlist_next (list);
	}

	if (list != NULL) {
		oldlist = mono_mlist_remove_item (oldlist, list);
#ifdef EPOLL_DEBUG
		g_print ("Dispatching event %d on socket %d\n", event, state->handle);
#endif
		if (!(mono_object_domain (state)->state == MONO_APPDOMAIN_UNLOADING || mono_object_domain (state)->state == MONO_APPDOMAIN_UNLOADED)) {
			InterlockedIncrement (&pending_io_items);
			start_io_thread_or_queue (state);
		}
	}

	return oldlist;
}

static int
mark_bad_fds (mono_pollfd *pfds, int nfds)
{
	int i, ret;
	mono_pollfd *pfd;
	int count = 0;

	for (i = 0; i < nfds; i++) {
		pfd = &pfds [i];
		if (pfd->fd == -1)
			continue;

		ret = mono_poll (pfd, 1, 0);
		if (ret == -1 && errno == EBADF) {
			pfd->revents |= MONO_POLLNVAL;
			count++;
		} else if (ret == 1) {
			count++;
		}
	}

	return count;
}

static void
socket_io_poll_main (gpointer p)
{
#define INITIAL_POLLFD_SIZE	1024
#define POLL_ERRORS (MONO_POLLERR | MONO_POLLHUP | MONO_POLLNVAL)
	SocketIOData *data = p;
	mono_pollfd *pfds;
	gint maxfd = 1;
	gint allocated;
	gint i;
	MonoThread *thread;

	thread = mono_thread_current ();

	allocated = INITIAL_POLLFD_SIZE;
	pfds = g_new0 (mono_pollfd, allocated);
	INIT_POLLFD (pfds, data->pipe [0], MONO_POLLIN);
	for (i = 1; i < allocated; i++)
		INIT_POLLFD (&pfds [i], -1, 0);

	while (1) {
		int nsock = 0;
		mono_pollfd *pfd;
		char one [1];
		MonoMList *list;

		do {
			if (nsock == -1) {
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			}

			nsock = mono_poll (pfds, maxfd, -1);
		} while (nsock == -1 && errno == EINTR);

		/* 
		 * Apart from EINTR, we only check EBADF, for the rest:
		 *  EINVAL: mono_poll() 'protects' us from descriptor
		 *      numbers above the limit if using select() by marking
		 *      then as MONO_POLLERR.  If a system poll() is being
		 *      used, the number of descriptor we're passing will not
		 *      be over sysconf(_SC_OPEN_MAX), as the error would have
		 *      happened when opening.
		 *
		 *  EFAULT: we own the memory pointed by pfds.
		 *  ENOMEM: we're doomed anyway
		 *
		 */

		if (nsock == -1 && errno == EBADF) {
			pfds->revents = 0; /* Just in case... */
			nsock = mark_bad_fds (pfds, maxfd);
		}

		if ((pfds->revents & POLL_ERRORS) != 0) {
			/* We're supposed to die now, as the pipe has been closed */
			g_free (pfds);
			socket_io_cleanup (data);
			return;
		}

		/* Got a new socket */
		if ((pfds->revents & MONO_POLLIN) != 0) {
			int nread;

			for (i = 1; i < allocated; i++) {
				pfd = &pfds [i];
				if (pfd->fd == -1 || data->newpfd == NULL ||
				    pfd->fd == data->newpfd->fd)
					break;
			}

			if (i == allocated) {
				mono_pollfd *oldfd;

				oldfd = pfds;
				i = allocated;
				allocated = allocated * 2;
				pfds = g_renew (mono_pollfd, oldfd, allocated);
				g_free (oldfd);
				for (; i < allocated; i++)
					INIT_POLLFD (&pfds [i], -1, 0);
			}
#ifndef PLATFORM_WIN32
			nread = read (data->pipe [0], one, 1);
#else
			nread = recv ((SOCKET) data->pipe [0], one, 1, 0);
#endif
			if (nread <= 0) {
				g_free (pfds);
				return; /* we're closed */
			}

			INIT_POLLFD (&pfds [i], data->newpfd->fd, data->newpfd->events);
			ReleaseSemaphore (data->new_sem, 1, NULL);
			if (i >= maxfd)
				maxfd = i + 1;
			nsock--;
		}

		if (nsock == 0)
			continue;

		EnterCriticalSection (&data->io_lock);
		if (data->inited == 0) {
			g_free (pfds);
			LeaveCriticalSection (&data->io_lock);
			return; /* cleanup called */
		}

		for (i = 1; i < maxfd && nsock > 0; i++) {
			pfd = &pfds [i];
			if (pfd->fd == -1 || pfd->revents == 0)
				continue;

			nsock--;
			list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (pfd->fd));
			if (list != NULL && (pfd->revents & (MONO_POLLIN | POLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLIN);
			}

			if (list != NULL && (pfd->revents & (MONO_POLLOUT | POLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLOUT);
			}

			if (list != NULL) {
				mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (pfd->fd), list);
				pfd->events = get_events_from_list (list);
			} else {
				mono_g_hash_table_remove (data->sock_to_state, GINT_TO_POINTER (pfd->fd));
				pfd->fd = -1;
				if (i == maxfd - 1)
					maxfd--;
			}
		}
		LeaveCriticalSection (&data->io_lock);
	}
}

#ifdef HAVE_EPOLL
#define EPOLL_ERRORS (EPOLLERR | EPOLLHUP)
static void
socket_io_epoll_main (gpointer p)
{
	SocketIOData *data;
	int epollfd;
	MonoThread *thread;
	struct epoll_event *events, *evt;
	const int nevents = 512;
	int ready = 0, i;

	data = p;
	epollfd = data->epollfd;
	thread = mono_thread_current ();
	events = g_new0 (struct epoll_event, nevents);

	while (1) {
		do {
			if (ready == -1) {
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			}
#ifdef EPOLL_DEBUG
			g_print ("epoll_wait init\n");
#endif
			ready = epoll_wait (epollfd, events, nevents, -1);
#ifdef EPOLL_DEBUG
			{
			int err = errno;
			g_print ("epoll_wait end with %d ready sockets (%d %s).\n", ready, err, (err) ? g_strerror (err) : "");
			errno = err;
			}
#endif
		} while (ready == -1 && errno == EINTR);

		if (ready == -1) {
			int err = errno;
			g_free (events);
			if (err != EBADF)
				g_warning ("epoll_wait: %d %s\n", err, g_strerror (err));

			close (epollfd);
			return;
		}

		EnterCriticalSection (&data->io_lock);
		if (data->inited == 0) {
#ifdef EPOLL_DEBUG
			g_print ("data->inited == 0\n");
#endif
			g_free (events);
			close (epollfd);
			return; /* cleanup called */
		}

		for (i = 0; i < ready; i++) {
			int fd;
			MonoMList *list;

			evt = &events [i];
			fd = evt->data.fd;
			list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (fd));
#ifdef EPOLL_DEBUG
			g_print ("Event %d on %d list length: %d\n", evt->events, fd, mono_mlist_length (list));
#endif
			if (list != NULL && (evt->events & (EPOLLIN | EPOLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLIN);
			}

			if (list != NULL && (evt->events & (EPOLLOUT | EPOLL_ERRORS)) != 0) {
				list = process_io_event (list, MONO_POLLOUT);
			}

			if (list != NULL) {
				mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (fd), list);
				evt->events = get_events_from_list (list);
#ifdef EPOLL_DEBUG
				g_print ("MOD %d to %d\n", fd, evt->events);
#endif
				if (epoll_ctl (epollfd, EPOLL_CTL_MOD, fd, evt)) {
					if (epoll_ctl (epollfd, EPOLL_CTL_ADD, fd, evt) == -1) {
#ifdef EPOLL_DEBUG
						int err = errno;
						g_message ("epoll_ctl(MOD): %d %s fd: %d events: %d", err, g_strerror (err), fd, evt->events);
						errno = err;
#endif
					}
				}
			} else {
				mono_g_hash_table_remove (data->sock_to_state, GINT_TO_POINTER (fd));
#ifdef EPOLL_DEBUG
				g_print ("DEL %d\n", fd);
#endif
				epoll_ctl (epollfd, EPOLL_CTL_DEL, fd, evt);
			}
		}
		LeaveCriticalSection (&data->io_lock);
	}
}
#endif

/*
 * select/poll wake up when a socket is closed, but epoll just removes
 * the socket from its internal list without notification.
 */
void
mono_thread_pool_remove_socket (int sock)
{
	MonoMList *list, *next;
	MonoSocketAsyncResult *state;

	if (socket_io_data.inited == FALSE)
		return;

	EnterCriticalSection (&socket_io_data.io_lock);
	list = mono_g_hash_table_lookup (socket_io_data.sock_to_state, GINT_TO_POINTER (sock));
	if (list) {
		mono_g_hash_table_remove (socket_io_data.sock_to_state, GINT_TO_POINTER (sock));
	}
	LeaveCriticalSection (&socket_io_data.io_lock);
	
	while (list) {
		state = (MonoSocketAsyncResult *) mono_mlist_get_data (list);
		if (state->operation == AIO_OP_RECEIVE)
			state->operation = AIO_OP_RECV_JUST_CALLBACK;
		else if (state->operation == AIO_OP_SEND)
			state->operation = AIO_OP_SEND_JUST_CALLBACK;

		next = mono_mlist_remove_item (list, list);
		list = process_io_event (list, MONO_POLLIN);
		if (list)
			process_io_event (list, MONO_POLLOUT);

		list = next;
	}
}

#ifdef PLATFORM_WIN32
static DWORD WINAPI
connect_hack (LPVOID x)
{
	struct sockaddr_in *addr = (struct sockaddr_in *) x;
	int count = 0;

	while (connect ((SOCKET) socket_io_data.pipe [1], (SOCKADDR *) addr, sizeof (struct sockaddr_in))) {
		Sleep (500);
		if (++count > 3) {
			g_warning ("Error initializing async. sockets %d.\n", WSAGetLastError ());
			g_assert (WSAGetLastError ());
		}
	}
}
#endif

static void
socket_io_init (SocketIOData *data)
{
#ifdef PLATFORM_WIN32
	struct sockaddr_in server;
	struct sockaddr_in client;
	SOCKET srv;
	int len;
	HANDLE connect_handle;
#endif
	int inited;

	inited = InterlockedCompareExchange (&data->inited, -1, -1);
	if (inited == 1)
		return;

	EnterCriticalSection (&data->io_lock);
	inited = InterlockedCompareExchange (&data->inited, -1, -1);
	if (inited == 1) {
		LeaveCriticalSection (&data->io_lock);
		return;
	}

#ifdef HAVE_EPOLL
	data->epoll_disabled = (g_getenv ("MONO_DISABLE_AIO") != NULL);
	if (FALSE == data->epoll_disabled) {
		data->epollfd = epoll_create (256);
		data->epoll_disabled = (data->epollfd == -1);
		if (data->epoll_disabled && g_getenv ("MONO_DEBUG"))
			g_message ("epoll_create() failed. Using plain poll().");
	} else {
		data->epollfd = -1;
	}
#else
	data->epoll_disabled = TRUE;
#endif

#ifndef PLATFORM_WIN32
	if (data->epoll_disabled) {
		if (pipe (data->pipe) != 0) {
			int err = errno;
			perror ("mono");
			g_assert (err);
		}
	} else {
		data->pipe [0] = -1;
		data->pipe [1] = -1;
	}
#else
	srv = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	g_assert (srv != INVALID_SOCKET);
	data->pipe [1] = socket (AF_INET, SOCK_STREAM, IPPROTO_TCP);
	g_assert (data->pipe [1] != INVALID_SOCKET);

	server.sin_family = AF_INET;
	server.sin_addr.s_addr = inet_addr ("127.0.0.1");
	server.sin_port = 0;
	if (bind (srv, (SOCKADDR *) &server, sizeof (server))) {
		g_print ("%d\n", WSAGetLastError ());
		g_assert (1 != 0);
	}

	len = sizeof (server);
	getsockname (srv, (SOCKADDR *) &server, &len);
	listen (srv, 1);
	connect_handle = mono_create_thread (NULL, 0, connect_hack, &server, 0, NULL);
	len = sizeof (server);
	data->pipe [0] = accept (srv, (SOCKADDR *) &client, &len);
	g_assert (data->pipe [0] != INVALID_SOCKET);
	closesocket (srv);
	WaitForSingleObject (connect_handle, INFINITE);
	CloseHandle (connect_handle);
#endif
	data->sock_to_state = mono_g_hash_table_new_type (g_direct_hash, g_direct_equal, MONO_HASH_VALUE_GC);

	if (data->epoll_disabled) {
		data->new_sem = CreateSemaphore (NULL, 1, 1, NULL);
		g_assert (data->new_sem != NULL);
	}
	io_job_added = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
	g_assert (io_job_added != NULL);
	if (data->epoll_disabled) {
		mono_thread_create_internal (mono_get_root_domain (), socket_io_poll_main, data, TRUE);
	}
#ifdef HAVE_EPOLL
	else {
		mono_thread_create_internal (mono_get_root_domain (), socket_io_epoll_main, data, TRUE);
	}
#endif
	InterlockedCompareExchange (&data->inited, 1, 0);
	LeaveCriticalSection (&data->io_lock);
}

static void
socket_io_add_poll (MonoSocketAsyncResult *state)
{
	int events;
	char msg [1];
	MonoMList *list;
	SocketIOData *data = &socket_io_data;
	int w;

#if defined(PLATFORM_MACOSX) || defined(PLATFORM_BSD) || defined(PLATFORM_WIN32) || defined(PLATFORM_SOLARIS)
	/* select() for connect() does not work well on the Mac. Bug #75436. */
	/* Bug #77637 for the BSD 6 case */
	/* Bug #78888 for the Windows case */
	if (state->operation == AIO_OP_CONNECT && state->blocking == TRUE) {
		start_io_thread_or_queue (state);
		return;
	}
#endif
	WaitForSingleObject (data->new_sem, INFINITE);
	if (data->newpfd == NULL)
		data->newpfd = g_new0 (mono_pollfd, 1);

	EnterCriticalSection (&data->io_lock);
	/* FIXME: 64 bit issue: handle can be a pointer on windows? */
	list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (state->handle));
	if (list == NULL) {
		list = mono_mlist_alloc ((MonoObject*)state);
	} else {
		list = mono_mlist_append (list, (MonoObject*)state);
	}

	events = get_events_from_list (list);
	INIT_POLLFD (data->newpfd, GPOINTER_TO_INT (state->handle), events);
	mono_g_hash_table_replace (data->sock_to_state, GINT_TO_POINTER (state->handle), list);
	LeaveCriticalSection (&data->io_lock);
	*msg = (char) state->operation;
#ifndef PLATFORM_WIN32
	w = write (data->pipe [1], msg, 1);
	w = w;
#else
	send ((SOCKET) data->pipe [1], msg, 1, 0);
#endif
}

#ifdef HAVE_EPOLL
static gboolean
socket_io_add_epoll (MonoSocketAsyncResult *state)
{
	MonoMList *list;
	SocketIOData *data = &socket_io_data;
	struct epoll_event event;
	int epoll_op, ievt;
	int fd;

	memset (&event, 0, sizeof (struct epoll_event));
	fd = GPOINTER_TO_INT (state->handle);
	EnterCriticalSection (&data->io_lock);
	list = mono_g_hash_table_lookup (data->sock_to_state, GINT_TO_POINTER (fd));
	if (list == NULL) {
		list = mono_mlist_alloc ((MonoObject*)state);
		epoll_op = EPOLL_CTL_ADD;
	} else {
		list = mono_mlist_append (list, (MonoObject*)state);
		epoll_op = EPOLL_CTL_MOD;
	}

	ievt = get_events_from_list (list);
	if ((ievt & MONO_POLLIN) != 0)
		event.events |= EPOLLIN;
	if ((ievt & MONO_POLLOUT) != 0)
		event.events |= EPOLLOUT;

	mono_g_hash_table_replace (data->sock_to_state, state->handle, list);
	event.data.fd = fd;
#ifdef EPOLL_DEBUG
	g_print ("%s %d with %d\n", epoll_op == EPOLL_CTL_ADD ? "ADD" : "MOD", fd, event.events);
#endif
	if (epoll_ctl (data->epollfd, epoll_op, fd, &event) == -1) {
		int err = errno;
		if (epoll_op == EPOLL_CTL_ADD && err == EEXIST) {
			epoll_op = EPOLL_CTL_MOD;
			if (epoll_ctl (data->epollfd, epoll_op, fd, &event) == -1) {
				g_message ("epoll_ctl(MOD): %d %s\n", err, g_strerror (err));
			}
		}
	}

	LeaveCriticalSection (&data->io_lock);
	return TRUE;
}
#endif

static void
socket_io_add (MonoAsyncResult *ares, MonoSocketAsyncResult *state)
{
	socket_io_init (&socket_io_data);
	MONO_OBJECT_SETREF (state, ares, ares);
#ifdef HAVE_EPOLL
	if (socket_io_data.epoll_disabled == FALSE) {
		if (socket_io_add_epoll (state))
			return;
	}
#endif
	socket_io_add_poll (state);
}

static gboolean
socket_io_filter (MonoObject *target, MonoObject *state)
{
	gint op;
	MonoSocketAsyncResult *sock_res = (MonoSocketAsyncResult *) state;
	MonoClass *klass;

	if (target == NULL || state == NULL)
		return FALSE;

	if (socket_async_call_klass == NULL) {
		klass = target->vtable->klass;
		/* Check if it's SocketAsyncCall in System.Net.Sockets
		 * FIXME: check the assembly is signed correctly for extra care
		 */
		if (klass->name [0] == 'S' && strcmp (klass->name, "SocketAsyncCall") == 0 
				&& strcmp (mono_image_get_name (klass->image), "System") == 0
				&& klass->nested_in && strcmp (klass->nested_in->name, "Socket") == 0)
			socket_async_call_klass = klass;
	}

	if (process_async_call_klass == NULL) {
		klass = target->vtable->klass;
		/* Check if it's AsyncReadHandler in System.Diagnostics.Process
		 * FIXME: check the assembly is signed correctly for extra care
		 */
		if (klass->name [0] == 'A' && strcmp (klass->name, "AsyncReadHandler") == 0 
				&& strcmp (mono_image_get_name (klass->image), "System") == 0
				&& klass->nested_in && strcmp (klass->nested_in->name, "Process") == 0)
			process_async_call_klass = klass;
	}
	/* return both when socket_async_call_klass has not been seen yet and when
	 * the object is not an instance of the class.
	 */
	if (target->vtable->klass != socket_async_call_klass && target->vtable->klass != process_async_call_klass)
		return FALSE;

	op = sock_res->operation;
	if (op < AIO_OP_FIRST || op >= AIO_OP_LAST)
		return FALSE;

	return TRUE;
}
#endif /* !DISABLE_SOCKETS */

static void
mono_async_invoke (MonoAsyncResult *ares)
{
	ASyncCall *ac = (ASyncCall *)ares->object_data;
	MonoThread *thread = NULL;
	MonoObject *res, *exc = NULL;
	MonoArray *out_args = NULL;

	if (ares->execution_context) {
		/* use captured ExecutionContext (if available) */
		thread = mono_thread_current ();
		MONO_OBJECT_SETREF (ares, original_context, mono_thread_get_execution_context ());
		mono_thread_set_execution_context (ares->execution_context);
	} else {
		ares->original_context = NULL;
	}

	ac->msg->exc = NULL;
	res = mono_message_invoke (ares->async_delegate, ac->msg, &exc, &out_args);
	MONO_OBJECT_SETREF (ac, res, res);
	MONO_OBJECT_SETREF (ac, msg->exc, exc);
	MONO_OBJECT_SETREF (ac, out_args, out_args);

	ares->completed = 1;

	/* call async callback if cb_method != null*/
	if (ac->cb_method) {
		MonoObject *exc = NULL;
		void *pa = &ares;
		mono_runtime_invoke (ac->cb_method, ac->cb_target, pa, &exc);
		/* 'exc' will be the previous ac->msg->exc if not NULL and not
		 * catched. If catched, this will be set to NULL and the
		 * exception will not be printed. */
		MONO_OBJECT_SETREF (ac->msg, exc, exc);
	}

	/* restore original thread execution context if flow isn't suppressed, i.e. non null */
	if (ares->original_context) {
		mono_thread_set_execution_context (ares->original_context);
		ares->original_context = NULL;
	}

	/* notify listeners */
	mono_monitor_enter ((MonoObject *) ares);
	if (ares->handle != NULL) {
		ac->wait_event = (gsize) mono_wait_handle_get_handle ((MonoWaitHandle *) ares->handle);
		SetEvent ((gpointer)(gsize)ac->wait_event);
	}
	mono_monitor_exit ((MonoObject *) ares);
	if (ares->gchandle) {
		mono_gchandle_free (ares->gchandle);
		ares->gchandle = 0;
	}
}

static void
start_idle_threads (MonoAsyncResult *data)
{
	int needed;
	int existing;

	needed = (int) InterlockedCompareExchange (&mono_min_worker_threads, 0, -1); 
	do {
		existing = (int) InterlockedCompareExchange (&mono_worker_threads, 0, -1); 
		if ((needed - existing) > 0) {
			start_tpthread (data);
			if (data) 
				threadpool_jobs_dec ((MonoObject*)data);
			data = NULL;
			SleepEx (500, TRUE);
		}
	} while ((needed - existing) > 0);

	/* If we don't start any thread here, make sure 'data' is processed. */
	if (data != NULL) {
		start_thread_or_queue (data);
		threadpool_jobs_dec ((MonoObject*)data);
	}
}

static void
start_tpthread (MonoAsyncResult *data)
{
	InterlockedIncrement (&mono_worker_threads);
	InterlockedIncrement (&busy_worker_threads);
	threadpool_jobs_inc ((MonoObject *)data);
	mono_thread_create_internal (mono_get_root_domain (), async_invoke_thread, data, TRUE);
}

void
mono_thread_pool_init ()
{
	int threads_per_cpu = THREADS_PER_CPU;
	int cpu_count;

	if ((int) InterlockedCompareExchange (&tp_inited, 1, 0) == 1)
		return;

	MONO_GC_REGISTER_ROOT (socket_io_data.sock_to_state);
	InitializeCriticalSection (&socket_io_data.io_lock);
	InitializeCriticalSection (&ares_lock);
	InitializeCriticalSection (&io_queue_lock);
	job_added = CreateSemaphore (NULL, 0, 0x7fffffff, NULL);
	g_assert (job_added != NULL);
	if (g_getenv ("MONO_THREADS_PER_CPU") != NULL) {
		threads_per_cpu = atoi (g_getenv ("MONO_THREADS_PER_CPU"));
		if (threads_per_cpu <= 0)
			threads_per_cpu = THREADS_PER_CPU;
	}

	cpu_count = mono_cpu_count ();
	mono_max_worker_threads = 20 + threads_per_cpu * cpu_count;
	mono_min_worker_threads = cpu_count; /* 1 idle thread per cpu */
	mono_io_max_worker_threads = mono_max_worker_threads / 2;
	if (mono_io_max_worker_threads < 16)
		mono_io_max_worker_threads = 16;
	mono_io_min_worker_threads = cpu_count;

	async_call_klass = mono_class_from_name (mono_defaults.corlib, "System", "MonoAsyncCall");
	g_assert (async_call_klass);
}

MonoAsyncResult *
mono_thread_pool_add (MonoObject *target, MonoMethodMessage *msg, MonoDelegate *async_callback,
		      MonoObject *state)
{
	MonoDomain *domain = mono_domain_get ();
	MonoAsyncResult *ares;
	ASyncCall *ac;

	ac = (ASyncCall*)mono_object_new (mono_domain_get (), async_call_klass);
	MONO_OBJECT_SETREF (ac, msg, msg);
	MONO_OBJECT_SETREF (ac, state, state);

	if (async_callback) {
		ac->cb_method = mono_get_delegate_invoke (((MonoObject *)async_callback)->vtable->klass);
		MONO_OBJECT_SETREF (ac, cb_target, async_callback);
	}

	ares = mono_async_result_new (domain, NULL, ac->state, NULL, (MonoObject*)ac);
	MONO_OBJECT_SETREF (ares, async_delegate, target);
	// It seems like garbage collect likes to nuke ares inappropriately. The handle prevents this.
	ares->gchandle = mono_gchandle_new (ares, FALSE);

	EnterCriticalSection (&ares_lock);
	if (domain->state == MONO_APPDOMAIN_UNLOADED || domain->state == MONO_APPDOMAIN_UNLOADING) {
		LeaveCriticalSection (&ares_lock);
		return ares;
	}
	LeaveCriticalSection (&ares_lock);

#ifndef DISABLE_SOCKETS
	if (socket_io_filter (target, state)) {
		socket_io_add (ares, (MonoSocketAsyncResult *) state);
		return ares;
	}
#endif
	
	start_thread_or_queue (ares);
	return ares;
}

static void
start_thread_or_queue (MonoAsyncResult *ares)
{
	int busy, worker;

	if ((int) InterlockedCompareExchange (&tp_idle_started, 1, 0) == 0) {
		threadpool_jobs_inc ((MonoObject*)ares);
		mono_thread_create_internal (mono_get_root_domain (), start_idle_threads, ares, TRUE);
		return;
	}

	busy = (int) InterlockedCompareExchange (&busy_worker_threads, 0, -1);
	worker = (int) InterlockedCompareExchange (&mono_worker_threads, 0, -1); 
	if (worker <= ++busy &&
	    worker < mono_max_worker_threads) {
		start_tpthread (ares);
	} else {
		append_job (&mono_delegate_section, &async_call_queue, (MonoObject*)ares);
		ReleaseSemaphore (job_added, 1, NULL);
	}
}

MonoObject *
mono_thread_pool_finish (MonoAsyncResult *ares, MonoArray **out_args, MonoObject **exc)
{
	ASyncCall *ac;

	*exc = NULL;
	*out_args = NULL;

	/* check if already finished */
	mono_monitor_enter ((MonoObject *) ares);
	
	if (ares->endinvoke_called) {
		*exc = (MonoObject *)mono_exception_from_name (mono_defaults.corlib, "System", 
					      "InvalidOperationException");
		mono_monitor_exit ((MonoObject *) ares);
		return NULL;
	}

	ares->endinvoke_called = 1;
	ac = (ASyncCall *)ares->object_data;

	g_assert (ac != NULL);

	/* wait until we are really finished */
	if (!ares->completed) {
		if (ares->handle == NULL) {
			ac->wait_event = (gsize)CreateEvent (NULL, TRUE, FALSE, NULL);
			g_assert(ac->wait_event != 0);
			MONO_OBJECT_SETREF (ares, handle, (MonoObject *) mono_wait_handle_new (mono_object_domain (ares), (gpointer)(gsize)ac->wait_event));
		}
		mono_monitor_exit ((MonoObject *) ares);
		WaitForSingleObjectEx ((gpointer)(gsize)ac->wait_event, INFINITE, TRUE);
	} else {
		mono_monitor_exit ((MonoObject *) ares);
	}

	*exc = ac->msg->exc; /* FIXME: GC add write barrier */
	*out_args = ac->out_args;

	return ac->res;
}

void
mono_thread_pool_cleanup (void)
{
	gint release;

	EnterCriticalSection (&mono_delegate_section);
	free_queue (&async_call_queue);
	release = (gint) InterlockedCompareExchange (&mono_worker_threads, 0, -1);
	LeaveCriticalSection (&mono_delegate_section);
	if (job_added)
		ReleaseSemaphore (job_added, release, NULL);

	socket_io_cleanup (&socket_io_data);
}

static void
null_array (MonoArray *a, int first, int last)
{
	/* We must null the old array because it might
	   contain cross-appdomain references, which
	   will crash the GC when the domains are
	   unloaded. */
	memset (mono_array_addr (a, MonoObject*, first), 0, sizeof (MonoObject*) * (last - first));
}

static void
append_job (CRITICAL_SECTION *cs, TPQueue *list, MonoObject *ar)
{
	if (mono_runtime_is_shutting_down())
		return;

	threadpool_jobs_inc (ar); 

	EnterCriticalSection (cs);
	if (ar->vtable->domain->state == MONO_APPDOMAIN_UNLOADING ||
			ar->vtable->domain->state == MONO_APPDOMAIN_UNLOADED) {
		LeaveCriticalSection (cs);
		return;
	}
	if (list->array && (list->next_elem < mono_array_length (list->array))) {
		mono_array_setref (list->array, list->next_elem, ar);
		list->next_elem++;
		LeaveCriticalSection (cs);
		return;
	}
	if (!list->array) {
		MONO_GC_REGISTER_ROOT (list->array);
		list->array = mono_array_new_cached (mono_get_root_domain (), mono_defaults.object_class, INITIAL_QUEUE_LENGTH);
	} else {
		int count = list->next_elem - list->first_elem;
		/* slide the array or create a larger one if it's full */
		if (list->first_elem) {
			mono_array_memcpy_refs (list->array, 0, list->array, list->first_elem, count);
			null_array (list->array, count, list->next_elem);
		} else {
			MonoArray *olda = list->array;
			MonoArray *newa = mono_array_new_cached (mono_get_root_domain (), mono_defaults.object_class, mono_array_length (list->array) * 2);
			mono_array_memcpy_refs (newa, 0, list->array, list->first_elem, count);
			list->array = newa;
			null_array (olda, list->first_elem, list->next_elem);
		}
		list->first_elem = 0;
		list->next_elem = count;
	}
	mono_array_setref (list->array, list->next_elem, ar);
	list->next_elem++;
	LeaveCriticalSection (cs);
}


static void
clear_queue (CRITICAL_SECTION *cs, TPQueue *list, MonoDomain *domain)
{
	int i, count = 0;
	EnterCriticalSection (cs);
	/*remove*/
	for (i = list->first_elem; i < list->next_elem; ++i) {
		MonoObject *obj = mono_array_get (list->array, MonoObject*, i);
		if (obj->vtable->domain == domain) {
			threadpool_jobs_dec (obj);
			unregister_job ((MonoAsyncResult*)obj);

			mono_array_set (list->array, MonoObject*, i, NULL);
			++count;
		}
	}
	/*compact*/
	if (count) {
		int idx = 0;
		for (i = list->first_elem; i < list->next_elem; ++i) {
			MonoObject *obj = mono_array_get (list->array, MonoObject*, i);
			if (obj)
				mono_array_set (list->array, MonoObject*, idx++, obj);
		}
		list->first_elem = 0;
		list->next_elem = count;
	}
	LeaveCriticalSection (cs);
}

/*
 * Clean up the threadpool of all domain jobs.
 * Can only be called as part of the domain unloading process as
 * it will wait for all jobs to be visible to the interruption code. 
 */
gboolean
mono_thread_pool_remove_domain_jobs (MonoDomain *domain, int timeout)
{
	HANDLE sem_handle;
	int result = TRUE;
	guint32 start_time = 0;

	g_assert (domain->state == MONO_APPDOMAIN_UNLOADING);

	clear_queue (&mono_delegate_section, &async_call_queue, domain);
	clear_queue (&io_queue_lock, &async_io_queue, domain);

	/*
	 * There might be some threads out that could be about to execute stuff from the given domain.
	 * We avoid that by setting up a semaphore to be pulsed by the thread that reaches zero.
	 */
	sem_handle = CreateSemaphore (NULL, 0, 1, NULL);
	
	domain->cleanup_semaphore = sem_handle;
	/*
	 * The memory barrier here is required to have global ordering between assigning to cleanup_semaphone
	 * and reading threadpool_jobs.
	 * Otherwise this thread could read a stale version of threadpool_jobs and wait forever.
	 */
	mono_memory_write_barrier ();

	if (domain->threadpool_jobs && timeout != -1)
		start_time = mono_msec_ticks ();
	while (domain->threadpool_jobs) {
		WaitForSingleObject (sem_handle, timeout);
		if (timeout != -1 && (mono_msec_ticks () - start_time) > timeout) {
			result = FALSE;
			break;
		}
	}

	domain->cleanup_semaphore = NULL;
	CloseHandle (sem_handle);
	return result;
}


static MonoObject*
dequeue_job (CRITICAL_SECTION *cs, TPQueue *list)
{
	MonoObject *ar;
	int count;

	EnterCriticalSection (cs);
	if (!list->array || list->first_elem == list->next_elem) {
		LeaveCriticalSection (cs);
		return NULL;
	}
	ar = mono_array_get (list->array, MonoObject*, list->first_elem);
	mono_array_setref (list->array, list->first_elem, NULL);
	list->first_elem++;
	count = list->next_elem - list->first_elem;
	/* reduce the size of the array if it's mostly empty */
	if (mono_array_length (list->array) > INITIAL_QUEUE_LENGTH && count < (mono_array_length (list->array) / 3)) {
		MonoArray *olda = list->array;
		MonoArray *newa = mono_array_new_cached (mono_get_root_domain (), mono_defaults.object_class, mono_array_length (list->array) / 2);
		mono_array_memcpy_refs (newa, 0, list->array, list->first_elem, count);
		list->array = newa;
		null_array (olda, list->first_elem, list->next_elem);
		list->first_elem = 0;
		list->next_elem = count;
	}
	LeaveCriticalSection (cs);

	return ar;
}

static void
free_queue (TPQueue *list)
{
	if (list->array)
		null_array (list->array, list->first_elem, list->next_elem);
	list->array = NULL;
	list->first_elem = list->next_elem = 0;
}

static void
async_invoke_thread (gpointer data)
{
	MonoDomain *domain;
	MonoThread *thread;
	int workers, min;
	const gchar *version;
 
	thread = mono_thread_current ();
 	if (tp_start_func)
 		tp_start_func (tp_hooks_user_data);

	version = mono_get_runtime_info ()->framework_version;
	for (;;) {
		MonoAsyncResult *ar;

		ar = (MonoAsyncResult *) data;
		if (ar) {
			/* worker threads invokes methods in different domains,
			 * so we need to set the right domain here */
			domain = ((MonoObject *)ar)->vtable->domain;

			g_assert (domain);

			if (domain->state == MONO_APPDOMAIN_UNLOADED || domain->state == MONO_APPDOMAIN_UNLOADING) {
				threadpool_jobs_dec ((MonoObject *)ar);
				unregister_job (ar);
				data = NULL;
			} else {
				mono_thread_push_appdomain_ref (domain);
				if (threadpool_jobs_dec ((MonoObject *)ar)) {
					unregister_job (ar);
					data = NULL;
					mono_thread_pop_appdomain_ref ();
					continue;
				}

				if (mono_domain_set (domain, FALSE)) {
					/* ASyncCall *ac; */

					if (tp_item_begin_func)
						tp_item_begin_func (tp_item_user_data);
					mono_async_invoke (ar);
					if (tp_item_end_func)
						tp_item_end_func (tp_item_user_data);
					/*
					ac = (ASyncCall *) ar->object_data;
					if (ac->msg->exc != NULL)
						mono_unhandled_exception (ac->msg->exc);
					*/
					mono_domain_set (mono_get_root_domain (), TRUE);
				}
				mono_thread_pop_appdomain_ref ();
				InterlockedDecrement (&busy_worker_threads);
				/* If the callee changes the background status, set it back to TRUE */
				if (*version != '1' && !mono_thread_test_state (thread , ThreadState_Background))
					ves_icall_System_Threading_Thread_SetState (thread, ThreadState_Background);
			}
		}

		data = dequeue_job (&mono_delegate_section, &async_call_queue);

		if (!data && !mono_runtime_is_shutting_down() && !THREAD_ABORT_REQUESTED(thread)) {
			guint32 wr;
			int timeout = THREAD_EXIT_TIMEOUT;
			guint32 start_time = mono_msec_ticks ();
			
			do {
				wr = WaitForSingleObjectEx (job_added, (guint32)timeout, TRUE);
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			
				timeout -= mono_msec_ticks () - start_time;
			
				if (wr != WAIT_TIMEOUT && wr != WAIT_IO_COMPLETION)
					data = dequeue_job (&mono_delegate_section, &async_call_queue);
			}
			while (!mono_runtime_is_shutting_down() && !data && timeout > 0 && !THREAD_ABORT_REQUESTED(thread));
		}

		if (!data) {
			workers = (int) InterlockedCompareExchange (&mono_worker_threads, 0, -1); 
			min = (int) InterlockedCompareExchange (&mono_min_worker_threads, 0, -1); 
	
			while (!mono_runtime_is_shutting_down() && !data && workers <= min && !THREAD_ABORT_REQUESTED(thread)) {
				WaitForSingleObjectEx (job_added, INFINITE, TRUE);
				if (THREAD_WANTS_A_BREAK (thread))
					mono_thread_interruption_checkpoint ();
			
				data = dequeue_job (&mono_delegate_section, &async_call_queue);
				workers = (int) InterlockedCompareExchange (&mono_worker_threads, 0, -1); 
				min = (int) InterlockedCompareExchange (&mono_min_worker_threads, 0, -1); 
			}
		}
	
		if (!data) {
			InterlockedDecrement (&mono_worker_threads);
 			if (tp_finish_func)
 				tp_finish_func (tp_hooks_user_data);
			return;
		}
		
		InterlockedIncrement (&busy_worker_threads);
	}

	g_assert_not_reached ();
}

void
ves_icall_System_Threading_ThreadPool_GetAvailableThreads (gint *workerThreads, gint *completionPortThreads)
{
	gint busy, busy_io;

	MONO_ARCH_SAVE_REGS;

	busy = (gint) InterlockedCompareExchange (&busy_worker_threads, 0, -1);
	busy_io = (gint) InterlockedCompareExchange (&busy_io_worker_threads, 0, -1);
	*workerThreads = mono_max_worker_threads - busy;
	*completionPortThreads = mono_io_max_worker_threads - busy_io;
}

void
ves_icall_System_Threading_ThreadPool_GetMaxThreads (gint *workerThreads, gint *completionPortThreads)
{
	MONO_ARCH_SAVE_REGS;

	*workerThreads = mono_max_worker_threads;
	*completionPortThreads = mono_io_max_worker_threads;
}

void
ves_icall_System_Threading_ThreadPool_GetMinThreads (gint *workerThreads, gint *completionPortThreads)
{
	gint workers, workers_io;

	MONO_ARCH_SAVE_REGS;

	workers = (gint) InterlockedCompareExchange (&mono_min_worker_threads, 0, -1);
	workers_io = (gint) InterlockedCompareExchange (&mono_io_min_worker_threads, 0, -1);

	*workerThreads = workers;
	*completionPortThreads = workers_io;
}

MonoBoolean
ves_icall_System_Threading_ThreadPool_SetMinThreads (gint workerThreads, gint completionPortThreads)
{
	int max_threads;
	int max_io_threads;

	MONO_ARCH_SAVE_REGS;

	max_threads = InterlockedCompareExchange (&mono_max_worker_threads, -1, -1);
	if (workerThreads <= 0 || workerThreads > max_threads)
		return FALSE;

	max_io_threads = InterlockedCompareExchange (&mono_io_max_worker_threads, -1, -1);
	if (completionPortThreads <= 0 || completionPortThreads > max_io_threads)
		return FALSE;

	InterlockedExchange (&mono_min_worker_threads, workerThreads);
	InterlockedExchange (&mono_io_min_worker_threads, completionPortThreads);
	mono_thread_create_internal (mono_get_root_domain (), start_idle_threads, NULL, TRUE);
	return TRUE;
}

MonoBoolean
ves_icall_System_Threading_ThreadPool_SetMaxThreads (gint workerThreads, gint completionPortThreads)
{
	gint32 min_threads;
	gint32 min_io_threads;
	gint32 cpu_count;

	MONO_ARCH_SAVE_REGS;

	cpu_count = mono_cpu_count ();
	min_threads = InterlockedCompareExchange (&mono_min_worker_threads, -1, -1);
	if (workerThreads < min_threads || workerThreads < cpu_count)
		return FALSE;

	/* We don't really have the concept of completion ports. Do we care here? */
	min_io_threads = InterlockedCompareExchange (&mono_io_min_worker_threads, -1, -1);
	if (completionPortThreads < min_io_threads || completionPortThreads < cpu_count)
		return FALSE;

	InterlockedExchange (&mono_max_worker_threads, workerThreads);
	InterlockedExchange (&mono_io_max_worker_threads, completionPortThreads);
	return TRUE;
}

/**
 * mono_install_threadpool_thread_hooks
 * @start_func: the function to be called right after a new threadpool thread is created. Can be NULL.
 * @finish_func: the function to be called right before a thredpool thread is exiting. Can be NULL.
 * @user_data: argument passed to @start_func and @finish_func.
 *
 * @start_fun will be called right after a threadpool thread is created and @finish_func right before a threadpool thread exits.
 * The calls will be made from the thread itself.
 */
void
mono_install_threadpool_thread_hooks (MonoThreadPoolFunc start_func, MonoThreadPoolFunc finish_func, gpointer user_data)
{
	tp_start_func = start_func;
	tp_finish_func = finish_func;
	tp_hooks_user_data = user_data;
}

/**
 * mono_install_threadpool_item_hooks
 * @begin_func: the function to be called before a threadpool work item processing starts.
 * @end_func: the function to be called after a threadpool work item is finished.
 * @user_data: argument passed to @begin_func and @end_func.
 *
 * The calls will be made from the thread itself and from the same AppDomain
 * where the work item was executed.
 *
 */
void
mono_install_threadpool_item_hooks (MonoThreadPoolItemFunc begin_func, MonoThreadPoolItemFunc end_func, gpointer user_data)
{
	tp_item_begin_func = begin_func;
	tp_item_end_func = end_func;
	tp_item_user_data = user_data;
}

