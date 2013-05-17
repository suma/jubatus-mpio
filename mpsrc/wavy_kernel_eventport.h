//
// mpio wavy kernel eventport
//
// Copyright (C) 2008-2010 FURUHASHI Sadayuki
// Copyright (C) 2013 Shuzo Kashihara
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//
#ifndef MP_WAVY_KERNEL_EVENTPORT_H__
#define MP_WAVY_KERNEL_EVENTPORT_H__

#include "jubatus/mp/exception.h"
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <port.h>
#include <sys/resource.h>

#ifndef MP_WAVY_KERNEL_EVPORT_XIDENT_MAX
#define MP_WAVY_KERNEL_EVPORT_XIDENT_MAX 256
#endif

namespace mp {
namespace wavy {


static const short EVKERNEL_READ  = POLLIN;
static const short EVKERNEL_WRITE = POLLOUT;


class kernel {
public:
	kernel() : m_ep(port_create())
	{
		if(m_ep < 0) {
			throw system_error(errno, "failed to initialize event port");
		}

		struct rlimit rbuf;
		if(::getrlimit(RLIMIT_NOFILE, &rbuf) < 0) {
			::close(m_ep);
			throw system_error(errno, "getrlimit() failed");
		}
		m_fdmax = rbuf.rlim_cur;

		for(unsigned int i=0; i < MP_WAVY_KERNEL_EVPORT_XIDENT_MAX; ++i) {
			m_fds[i] = -1;
		}

		memset(m_xident, 0, sizeof(m_xident));
		memset(m_ptr, 0, sizeof(m_ptr));
		m_xident_index = 0;
	}

	~kernel()
	{
		::close(m_ep);
	}

	size_t max() const
	{
		return m_fdmax + MP_WAVY_KERNEL_EVPORT_XIDENT_MAX;
	}
private:
	int alloc_xident()
	{
		for(unsigned int i=0; i < MP_WAVY_KERNEL_EVPORT_XIDENT_MAX*2; ++i) {
			unsigned int x = __sync_fetch_and_add(&m_xident_index, 1) % MP_WAVY_KERNEL_EVPORT_XIDENT_MAX;
			if(__sync_bool_compare_and_swap(&m_xident[x], false, true)) {
				return m_fdmax + x;
			}
		}
		errno = EMFILE;
		return -1;
	}


	int alloc_xident(int fd) {
		int xident = alloc_xident();
		if(xident >= 0) {
			m_fds[xident - m_fdmax] = fd;
		}
		return xident;
	}

	void set_xident_ptr(int xident, uintptr_t ptr) {
		if(xident >= 0) {
			m_ptr[xident - m_fdmax] = ptr;
		}
	}

	void set_xident(int xident, int fd) {
		if(xident >= 0) {
			m_fds[xident - m_fdmax] = fd;
		}
	}

	int xident_fd(int xident) const {
		const int index = xident - m_fdmax;
		if(0 <= index && index < MP_WAVY_KERNEL_EVPORT_XIDENT_MAX) {
			return m_fds[index];
		}
		errno = EMFILE;	// FIXME?
		return -1;
	}

	uintptr_t xident_ptr(int xident) const {
		const int index = xident - m_fdmax;
		if(0 <= index && index < MP_WAVY_KERNEL_EVPORT_XIDENT_MAX) {
			return m_ptr[index];
		}
		return 0;
	}

	bool free_xident(int xident)
	{
		bool* xv = m_xident + (xident - m_fdmax);
		// FIXME cas?
		if(*xv) {
			*xv = false;
			return true;
		}
		return false;
	}

 public:
	class event {
	public:
		event() { }
		explicit event(port_event_t ev) : portev(ev) { }
		~event() { }

		int ident() const {
			return (int)portev.portev_user;
		}

	private:
		port_event_t portev;
		friend class kernel;
	};


	int add_fd(int fd, short event)
	{
		return ::port_associate(m_ep, PORT_SOURCE_FD, fd, event, (void*)fd);
	}

	int remove_fd(int fd, short event)
	{
		return ::port_dissociate(m_ep, PORT_SOURCE_FD, fd);
	}


	class timer {
	public:
		timer() : xident(-1) { }
		~timer() {
			if(xident >= 0) {
				kern->remove_timer(xident);
				kern->free_xident(xident);
			}
		}

		int ident() const { return xident; }

		int activate() {
			return ::timer_settime(id, 0, &itimer, NULL);
		}

	private:
		int xident;
		timer_t id;
		struct itimerspec itimer;
		kernel* kern;
		friend class kernel;
		timer(const timer&);
	};

	int add_timer(timer* tm, const timespec* value, const timespec* interval)
	{
		int xident = alloc_xident();
		if(xident < 0) {
			return -1;
		}

		struct sigevent sigev;
		port_notify_t pn;
		pn.portnfy_port = m_ep;
		pn.portnfy_user = (void*)xident;
		timer_t timer_id;
		::memset(&sigev, 0, sizeof(sigev));
		sigev.sigev_notify = SIGEV_PORT;
		sigev.sigev_value.sival_ptr = &pn;
		if(timer_create(CLOCK_REALTIME, &sigev, &timer_id)) {
			free_xident(xident);
			timer_delete(timer_id);
			return -1;
		}

		set_xident(xident, timer_id);
		set_xident_ptr(xident, (uintptr_t)tm);

		struct itimerspec itimer;
		::memset(&itimer, 0, sizeof(itimer));
		if(interval) {
			itimer.it_interval = *interval;
		}
		if(value) {
			itimer.it_value = *value;
		} else {
			itimer.it_value = itimer.it_interval;
		}

		tm->xident = xident;
		tm->id = timer_id;
		tm->itimer = itimer;
		tm->kern = this;

		if(tm->activate()) {
			free_xident(xident);
			timer_delete(timer_id);
			tm->xident = -1;
			return -1;
		}

		return xident;
	}

	int remove_timer(int xident)
	{
		timer_t id = xident_fd(xident);
		if(id < 0) {
			return -1;
		}
		return timer_delete(id);
	}

	static int read_timer(event e)
	{
		return 0;
	}

	class signal {
	public:
		signal() : xident(-1) { }
		~signal() {
			if(xident >= 0) {
				kern->remove_signal(xident);
				kern->free_xident(xident);
			}
		}

		int ident() const { return xident; }

	private:
		int xident;
		kernel* kern;
		friend class kernel;
		signal(const signal&);
	};

	int add_signal(signal* sg, int signo)
	{
		int xident = alloc_xident();
		if(xident < 0) {
			return -1;
		}

		// TODO: implemented by port_send(3)

		sg->xident = xident;
		sg->kern = this;
		return xident;
	}

	int remove_signal(int ident)
	{
		return 0;
	}

	static int read_signal(event e)
	{
		return 0;
	}

	int add_kernel(kernel* kern)
	{
		if(add_fd(kern->m_ep, EVKERNEL_READ) < 0) {
			return -1;
		}
		return kern->m_ep;
	}

	int ident() const
	{
		return m_ep;
	}


	class backlog {
	public:
		backlog()
		{
			buf = (port_event_t*)::calloc(
					sizeof(port_event_t),
					MP_WAVY_KERNEL_BACKLOG_SIZE);
			if(!buf) { throw std::bad_alloc(); }
		}

		~backlog()
		{
			::free(buf);
		}

		event operator[] (int n)
		{
			return event(buf[n]);
		}

	private:
		port_event_t* buf;
		friend class kernel;
		backlog(const backlog&);
	};

	int wait(backlog* result)
	{
		uint_t nget = 1;
		if(port_getn(m_ep, result->buf, MP_WAVY_KERNEL_BACKLOG_SIZE, &nget, NULL)) {
			// For unix portability
			if(errno == ETIME) {
				errno = EINTR;
			}
			return -1;
		}
		return nget;
	}

	int wait(backlog* result, int timeout_msec)
	{
		struct timespec ts;
		ts.tv_sec  = timeout_msec / 1000;
		ts.tv_nsec = (timeout_msec % 1000) * 1000000;
		uint_t nget = 1;

		if(port_getn(m_ep, result->buf, MP_WAVY_KERNEL_BACKLOG_SIZE, &nget, &ts)) {
			// For unix portability
			if(errno == ETIME) {
				errno = EINTR;
			}
			return -1;
		}
		return nget;
	}

	int reactivate(event e)
	{
		switch(e.portev.portev_source) {
		case PORT_SOURCE_FD: {
				// READ, WRITE
				int fd = e.ident();
				int type = e.portev.portev_events;
				printf("PORT_SOURCE_FD: fd %d ev %d\n", fd, type);
				if(type == POLLIN) {
					return add_fd(e.ident(), POLLIN);
				} else if(type == POLLOUT) {
					return add_fd(e.ident(), POLLOUT);
				}

				return -1;
			}

		case PORT_SOURCE_TIMER: {
				timer* tm = (timer*)xident_ptr(e.ident());
				return tm->activate();
			}

		// TODO: PORT_SOURCE_ALERT(for signal)
		
		default:
			return -1;
		}
	}

	int remove(event e)
	{
		int ident = e.ident();
		switch(e.portev.portev_source) {
		case PORT_SOURCE_FD:
			return ::port_dissociate(m_ep, PORT_SOURCE_FD, ident);
		case PORT_SOURCE_TIMER:
			return 0;
		default:
			return -1;
		}
	}

private:
	int m_ep;
	size_t m_fdmax;

	bool m_xident[MP_WAVY_KERNEL_EVPORT_XIDENT_MAX];
	int m_fds[MP_WAVY_KERNEL_EVPORT_XIDENT_MAX];
	uintptr_t m_ptr[MP_WAVY_KERNEL_EVPORT_XIDENT_MAX];
	unsigned int m_xident_index;

private:
	kernel(const kernel&);
};


}  // namespace wavy
}  // namespace mp

#endif /* wavy_kernel_eventport.h */
