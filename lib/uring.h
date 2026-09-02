#ifndef _URING_H_
#define _URING_H_

#include <string.h>

#include "socket.h"

struct uring_req;

typedef void uring_req_handler_fn(struct uring_req *, int32_t res, uint32_t flags);

struct uring_req {
	uring_req_handler_fn *handler;
};

struct uring_req_sendmsg {
	struct uring_req req;
	struct msghdr mh;
	struct sockaddr_storage ss; // destination address
	struct iovec iov[0]; // provided by outer container
};

TYPED_GQUEUE(sendmsg, struct uring_req_sendmsg);


struct uring_methods {
	ssize_t (*sendmsg)(socket_t *, struct uring_req_sendmsg *)
		__attribute__((nonnull(1, 2)));

	unsigned int (*thread_loop)(void);

	// possibly stack storage
	void *(*__alloc_req)(void *, size_t); 
	void (*free)(struct uring_req *);

	// to transfer from possibly stack to heap for queuing
	struct uring_req_sendmsg *(*__alloc_dup)(struct uring_req_sendmsg *, size_t);
	void (*dup_free)(struct uring_req *);
};

extern __thread struct uring_methods uring_methods;

INLINE void uring_req_free(struct uring_req *r, int32_t res, uint32_t flags) {
	uring_methods.free(r);
}
INLINE void uring_req_release(struct uring_req *r) {
	r->handler(r, 0, 0);
}

__attribute__((nonnull(1, 2, 3)))
INLINE void uring_sendmsg_prepare(socket_t *s, const endpoint_t *e, struct uring_req_sendmsg *r) {
	s->family->endpoint2sockaddr(&r->ss, e);
	r->mh.msg_name = &r->ss;
	r->mh.msg_namelen = s->family->sockaddr_size;
}

__attribute__((nonnull(1, 2, 3)))
INLINE ssize_t uring_sendmsg_fail(socket_t *s, const endpoint_t *e, struct uring_req_sendmsg *r) {
	uring_sendmsg_prepare(s, e, r);
	return uring_methods.sendmsg(s, r);
}

__attribute__((nonnull(1, 2, 3)))
INLINE ssize_t uring_sendmsg(socket_t *s, const endpoint_t *e, struct uring_req_sendmsg *r) {
	uring_sendmsg_prepare(s, e, r);

	ssize_t ret = uring_methods.sendmsg(s, r);
	if (ret < 0)
		uring_req_release(&r->req);

	return ret;
}


#define uring_alloc_sendmsg(sv, fn) ({ \
			__typeof__(sv) __ret = uring_methods.__alloc_req((sv), sizeof(*(sv))); \
			memset(sv, 0, sizeof(*(sv))); \
			__ret->req.req.handler = (fn); \
			__ret; \
		})

#define uring_dup_sendmsg(sv) uring_methods.__alloc_dup(&(sv)->req, sizeof(*(sv)))


#ifdef HAVE_LIBURING

#include "bufferpool.h"

void uring_thread_init(void);
void uring_thread_cleanup(void);

struct poller_item;
struct poller *uring_poller_new(void);
void uring_poller_free(struct poller **pp);
void uring_poller_add_waker(struct poller *p);
void uring_poller_wake(struct poller *p);
unsigned int uring_poller_poll(struct poller *);
void uring_poller_clear(struct poller *);

bool uring_poller_add_item(struct poller *p, struct poller_item *i);
bool uring_poller_del_item(struct poller *p, int fd);
void uring_poller_blocked(struct poller *p, void *fdp);
bool uring_poller_isblocked(struct poller *p, void *fdp);
void uring_poller_error(struct poller *p, void *fdp);
bool uring_poller_del_item_callback(struct poller *p, int fd, void (*callback)(void *), void *arg);

#else

static inline void uring_thread_init(void) { }
static inline void uring_thread_cleanup(void) { }

#endif

#endif
