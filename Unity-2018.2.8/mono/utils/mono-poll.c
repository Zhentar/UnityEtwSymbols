#include "mono-poll.h"
#include "mono/metadata/threads.h"
#include <errno.h>

#if defined(HAVE_POLL) && !defined(__APPLE__)
int
mono_poll (mono_pollfd *ufds, unsigned int nfds, int timeout)
{
	return poll (ufds, nfds, timeout);
}
#else

int
mono_poll (mono_pollfd *ufds, unsigned int nfds, int timeout)
{
	struct timeval tv, *tvptr;
	int i, fd, events, affected, count;
	fd_set rfds, wfds, efds;
	int nexc = 0;
	int maxfd = 0;
	int shouldrepeat = 0;

	if (timeout < 0)
	{
		timeout = 1000;
		shouldrepeat = 1;
	}

	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;
	tvptr = &tv;

	FD_ZERO (&rfds);
	FD_ZERO (&wfds);
	FD_ZERO (&efds);

	for (i = 0; i < nfds; i++) {
		ufds [i].revents = 0;
		fd = ufds [i].fd;
		if (fd < 0)
			continue;

#ifdef PLATFORM_WIN32
		// FIXME: nexc is always 0
		if (nexc >= FD_SETSIZE) {
			ufds [i].revents = MONO_POLLNVAL;
			return 1;
		}
#else
		if (fd > FD_SETSIZE) {
			ufds [i].revents = MONO_POLLNVAL;
			return 1;
		}
#endif

		events = ufds [i].events;
		if ((events & MONO_POLLIN) != 0)
			FD_SET (fd, &rfds);

		if ((events & MONO_POLLOUT) != 0)
			FD_SET (fd, &wfds);

		FD_SET (fd, &efds);
		nexc++;
		if (fd > maxfd)
			maxfd = fd;
			
	}

	affected = select (maxfd + 1, &rfds, &wfds, &efds, tvptr);
	if (affected == -1) {
#ifdef PLATFORM_WIN32
		int error = WSAGetLastError ();
		switch (error) {
		case WSAEFAULT: errno = EFAULT; break;
		case WSAEINVAL: errno = EINVAL; break;
		case WSAEINTR: errno = EINTR; break;
		/* case WSAEINPROGRESS: errno = EINPROGRESS; break; */
		case WSAEINPROGRESS: errno = EINTR; break;
		case WSAENOTSOCK: errno = EBADF; break;
#ifdef ENOSR
		case WSAENETDOWN: errno = ENOSR; break;
#endif
		default: errno = 0;
		}
#endif

		return -1;
	}

	// If we timed out of the wait, 
	// we want to signal the caller to yield to thread interruptions
	if (affected == 0 && shouldrepeat)
	{
		mono_thread_interruption_checkpoint ();
		errno = EINTR;
		return -1;
	}

	count = 0;
	for (i = 0; i < nfds && affected > 0; i++) {
		fd = ufds [i].fd;
		if (fd < 0)
			continue;

		events = ufds [i].events;
		if ((events & MONO_POLLIN) != 0 && FD_ISSET (fd, &rfds)) {
			ufds [i].revents |= MONO_POLLIN;
			affected--;
		}

		if ((events & MONO_POLLOUT) != 0 && FD_ISSET (fd, &wfds)) {
			ufds [i].revents |= MONO_POLLOUT;
			affected--;
		}

		if (FD_ISSET (fd, &efds)) {
			ufds [i].revents |= MONO_POLLERR;
			affected--;
		}

		if (ufds [i].revents != 0)
			count++;
	}

	return count;
}

#endif

