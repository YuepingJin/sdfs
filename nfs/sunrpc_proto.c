

#include <arpa/inet.h>
#include <rpc/auth.h>
#include <errno.h>

#define DBG_SUBSYS S_YRPC

#include "configure.h"
#include "job_dock.h"
#include "net_global.h"
#include "sunrpc_proto.h"
#include "job_tracker.h"
#include "../net/net_events.h"
#include "../../ynet/sock/sock_tcp.h"
#include "ynet_rpc.h"
#include "dbg.h"
#include "xdr.h"
#include "nfs_job_context.h"
#include "nfs_events.h"
#include "nfs_state_machine.h"
#include "xdr_nfs.h"

const char job_sunrpc_request[] = "sunrpc_request";

extern event_job_t sunrpc_nfs3_handler;
extern event_job_t sunrpc_mount_handler;
extern event_job_t sunrpc_acl_handler;
extern event_job_t sunrpc_nlm_handler;

jobtracker_t *sunrpc_jobtracker;

#define NFS3_WRITE 7

typedef struct {
        struct list_head hook;
        buffer_t buf;
        sockid_t sockid;
} rpcreq_t;

typedef struct {
        struct list_head rpcreq_list;
        int rpcreq_count;
        sy_spinlock_t lock;
        sem_t sem;
} rpcreq_queue_t;

static int __sunrpc_request_handler(const sockid_t *sockid,
                                    buffer_t *buf);

static rpcreq_queue_t __rpcreq_queue__;
rpcreq_queue_t *rpcreq_queue = &__rpcreq_queue__;

int sunrpc_pack_len(void *buf, uint32_t len, int *msg_len, int *io_len)
{
        uint32_t *length, _len, credlen, verilen, headlen;
        sunrpc_request_t *req;
        auth_head_t *cred, *veri;
        void *msg;
        
        if (len < sizeof(sunrpc_request_t)) {
                DERROR("less then sunrpc_head_t\n");

                return 0;
        }

        length = buf;

        DBUG("sunrpc request len %u, is last %u\n", ntohl(*length) ^ (1 << 31),
             ntohl(*length) & (1 << 31));

        if (ntohl(*length) & (1 << 31)) {
                _len = (ntohl(*length) ^ (1 << 31)) + sizeof(uint32_t);
                DBUG("_len %u\n", _len);
        } else {
                _len = (ntohl(*length)) + sizeof(uint32_t);
                DBUG("_len else %u\n", _len);
        }

        req = buf;

	//if (ntohl(req->procedure) == NFS3_WRITE) {
	if (0) {
#if 0
		(void) msg;
		(void) veri;
                (void) cred;
                (void) headlen;
                (void) verilen;
                (void) credlen;

                *io_len = (_len / 1024) * 1024;
                *msg_len = _len - *io_len;
#else
                cred = buf + sizeof(*req);
                credlen = ntohl(cred->length);

                veri = (void *)cred + credlen + sizeof(*cred);
                verilen = ntohl(veri->length);

                msg = (void *)veri + verilen + sizeof(*veri);

                headlen =  msg - buf + sizeof(fileid_t) + sizeof(uint64_t)
                        + sizeof(uint32_t)  * 4;

                *msg_len = headlen;
                *io_len = _len - headlen;

                YASSERT(_len > headlen);
                DBUG("nfs write msg %u io %u\n", *msg_len, *io_len);
#endif
        } else {
                *msg_len =  _len;
                *io_len = 0;
        }

        DBUG("msg_len: %u, io_len: %u\n", *msg_len, *io_len);

        return 0;
}

static inline int __auth_unix(auth_unix_t *auth_unix, char *buf, int buflen)
{
        xdr_t xdr_auth;
        buffer_t _buf_tmp;

        xdr_auth.op = __XDR_DECODE;
        xdr_auth.buf = &_buf_tmp;
        mbuffer_init(xdr_auth.buf, 0);

        mbuffer_copy(xdr_auth.buf, buf, buflen);

        __xdr_uint32(&xdr_auth, &auth_unix->stamp);
        __xdr_string(&xdr_auth, &auth_unix->machinename, 255);
        __xdr_uint32(&xdr_auth, &auth_unix->uid);
        __xdr_uint32(&xdr_auth, &auth_unix->gid);

        DBUG("stamp: %u, name: %s, uid: %u, gid: %u\n",
                        auth_unix->stamp, auth_unix->machinename,
                        auth_unix->uid, auth_unix->gid);

        mbuffer_free(xdr_auth.buf);
        /*auth_unix = (auth_unix_t *)credbuf;*/
        /*xdr_authunix_parms(credbuf+sizeof(auth_head_t), auth_unix);*/

        return 0;
}


#if 0
{
        int ret;
        rpc_request_t *rpc_request;
        const msgid_t *msgid;
        net_prog_t *prog;
        net_request_handler handler;

#ifdef HAVE_STATIC_ASSERT
        static_assert(sizeof(*rpc_request)  < sizeof(mem_cache128_t), "rpc_request_t");
#endif

        rpc_request = mem_cache_calloc(MEM_CACHE_128, 0);
        if (!rpc_request) {
                ret = ENOMEM;
                GOTO(err_ret, ret);
        }

        rpc_request->sockid = *sockid;
        rpc_request->msgid = *msgid;
        rpc_request->nid.id = 0;

        mbuffer_init(&rpc_request->buf, 0);
        mbuffer_merge(&rpc_request->buf, buf);

        schedule_task_new("sunrpc", handler, rpc_request, 0);

        return 0;
err_ret:
        return ret;
}
#endif

int __sunrpc_exec(rpcreq_t *rpcreq)
{
        int ret;
        sunrpc_request_t *req;
        buffer_t *buf;
        job_t *job;
        auth_head_t cred, veri;
        auth_unix_t auth_unix;
        char credbuf[MAX_AUTH_BYTES];
        char veribuf[MAX_AUTH_BYTES];
        uint32_t is_last;
        //net_handle_t *nh;

        buf = &rpcreq->buf;

        ret = job_create(&job, sunrpc_jobtracker, job_sunrpc_request);
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        req = (void *)job->buf;

        ret = mbuffer_popmsg(buf, req, sizeof(sunrpc_request_t));
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        is_last = ntohl(req->length) & (1 << 31);

        if (is_last)
                req->length = ntohl(req->length) ^ (1 << 31);
        else
                req->length = ntohl(req->length);

        req->xid = req->xid;
        req->msgtype = ntohl(req->msgtype);
        req->rpcversion = ntohl(req->rpcversion);
        req->program = ntohl(req->program);
        req->progversion = ntohl(req->progversion);
        req->procedure = ntohl(req->procedure);

        if (unlikely(is_last != (uint32_t)(1 << 31))) {
                DERROR("%u:%u\n", is_last, (uint32_t)(1 << 31));

                DERROR("rpc request len %u xid %u version %u prog %u version"
                                " %u procedure %u\n", req->length, req->xid,
                                req->rpcversion, req->program, req->progversion,
                                req->procedure);

                UNIMPLEMENTED(__DUMP__);
        }

        DBUG("rpc request len %u xid %u version %u prog %u version"
                        " %u procedure %u\n", req->length, req->xid,
                        req->rpcversion, req->program, req->progversion,
                        req->procedure);

        mbuffer_get(buf, &cred, sizeof(auth_head_t));
        cred.flavor = ntohl(cred.flavor);
        cred.length = ntohl(cred.length);

        ret = mbuffer_popmsg(buf, credbuf, cred.length + sizeof(auth_head_t));
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        if (cred.length) {
                ret = __auth_unix(&auth_unix, credbuf+sizeof(auth_head_t), cred.length);
                if (unlikely(ret))
                        GOTO(err_ret, ret); //GOTO???

                job->uid = auth_unix.uid;
                job->gid = auth_unix.gid;
                yfree((void**)&auth_unix.machinename);
        }

        mbuffer_get(buf, &veri, sizeof(auth_head_t));
        veri.flavor = ntohl(veri.flavor);
        veri.length = ntohl(veri.length);

        DBUG("cred %u len %u veri %u len %u\n", cred.flavor, cred.length,
                        veri.flavor, veri.length);

        ret = mbuffer_popmsg(buf, veribuf, veri.length + sizeof(auth_head_t));
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        mbuffer_init(&job->request, 0);

        mbuffer_merge(&job->request, buf);

        DBUG("request len %u\n", job->request.len);

        memset(&job->net, 0x0, sizeof(job->net));
        job->sock = rpcreq->sockid;

        job->msgid.idx = req->xid;
        job->msgid.tabid = 0;

        sunrpc_request_handler(job, NULL, NULL);

        return 0;
err_ret:
        return ret;
}

int __sunrpc_needwait(int *used, int *total)
{
        int ret, qos;

        ret = job_used(used, total);
        if (unlikely(ret))
                YASSERT(0);

        qos = nfsconf.job_qos;
        if (unlikely(nfsconf.job_qos > gloconf.jobdock_size/4)) {
                qos = gloconf.jobdock_size/4;
                DBUG("job_qos need under 1/4 * job_dock_size, qos was set to %d\n", qos);
        }

        if (*used > qos) {
                return 1;
        }

        return 0;
}

static int __sunrpc_count()
{
        int count;

        sy_spin_lock(&rpcreq_queue->lock);
        count = rpcreq_queue->rpcreq_count;
        sy_spin_unlock(&rpcreq_queue->lock);

        return count;
}

void *__sunrpc_worker(void *arg)
{
        int ret, used, total, count;
        rpcreq_t *rpcreq;
        struct list_head rpcreq_list;
        struct list_head *pos, *n;

        (void)arg;
        INIT_LIST_HEAD(&rpcreq_list);

        while (1) {
                ret = _sem_wait(&rpcreq_queue->sem);
                if (ret)
                        UNIMPLEMENTED(__DUMP__);

                sy_spin_lock(&rpcreq_queue->lock);
                if (list_empty(&rpcreq_queue->rpcreq_list)) {
                        sy_spin_unlock(&rpcreq_queue->lock);
                        DBUG("queue empty wait\n");
                        /*usleep(100000);*/
                        continue;
                }

                count = rpcreq_queue->rpcreq_count;
                list_splice_tail_init(&rpcreq_queue->rpcreq_list, &rpcreq_list);
                rpcreq_queue->rpcreq_count = 0;

                sy_spin_unlock(&rpcreq_queue->lock);

                list_for_each_safe(pos, n, &rpcreq_list) {
retry:
                        if (__sunrpc_needwait(&used, &total)) {
                                DWARN("job busy queue %d job %d/%d\n",
                                                count + __sunrpc_count(), used, total);
                                usleep(1000);
                                goto retry;
                        }

                        rpcreq = (rpcreq_t *)pos;
                        list_del(pos);
                        ret = __sunrpc_exec(rpcreq);
                        if (ret)
                                YASSERT(0);
                        yfree((void **)&rpcreq);
                }
        }
}

void __sunrpc_add(rpcreq_t *rpcreq)
{
                sy_spin_lock(&rpcreq_queue->lock);

                list_add(&rpcreq->hook, &rpcreq_queue->rpcreq_list);
                rpcreq_queue->rpcreq_count++;

                sy_spin_unlock(&rpcreq_queue->lock);
                sem_post(&rpcreq_queue->sem);
}

int sunrpc_init()
{
        int ret;
        pthread_attr_t ta;
        pthread_t th;

        INIT_LIST_HEAD(&rpcreq_queue->rpcreq_list);
        rpcreq_queue->rpcreq_count = 0;
        sy_spin_init(&rpcreq_queue->lock);

        ret = sem_init(&rpcreq_queue->sem, 0, 0);
        if (ret)
                GOTO(err_ret, ret);

        pthread_attr_init(&ta);
        pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);

        ret = pthread_create(&th, &ta, __sunrpc_worker, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}



static int __sunrpc_pack_handler(const nid_t *nid, const sockid_t *sockid, buffer_t *buf)
{
        sunrpc_proto_t type;
        rpcreq_t *rpcreq;

        (void) nid;
        (void) rpcreq;
        
        mbuffer_get(buf, &type, sizeof(sunrpc_proto_t));

        type.msgtype = ntohl(type.msgtype);

        DBUG("get sunrpc pack %d msg %d id %u\n", ntohl(type.length) ^ (1 << 31),
             type.msgtype, ntohl(type.xid));

        switch (type.msgtype) {
        case SUNRPC_REQ_MSG:
#if 1
                __sunrpc_request_handler(sockid, buf);
#else
                //在__sunrpc_exec中释放
                ret = ymalloc((void **)&rpcreq, sizeof(rpcreq_t ));
                if (ret)
                        YASSERT(0);

                mbuffer_init(&rpcreq->buf, 0);
                mbuffer_merge(&rpcreq->buf, buf);
                rpcreq->sockid = *sockid;
                INIT_LIST_HEAD(&rpcreq->hook);
                __sunrpc_add(rpcreq);
#endif

                break;
        case SUNRPC_REP_MSG:
                DERROR("bad msgtype\n");

                YASSERT(0);
                //break;
        default:
                DERROR("bad msgtype\n");

                YASSERT(0);
        }

        return 0;
}

int sunrpc_request_handler(job_t *job, void *sock, void *context)
{
        int ret;
        sunrpc_request_t *req;

        (void) sock;
        (void) context;

        req = (void *)job->buf;

        DBUG("program %u version %u\n", req->program, req->progversion);

        if (req->program == MOUNTPROG && (req->progversion == MOUNTVERS3 ||
                                          req->progversion == MOUNTVERS1)) {
                ret = sunrpc_mount_handler(job);
                if (ret)
                        GOTO(err_ret, ret); //GOTO???
        } else if (req->program == NFS3_PROGRAM && req->progversion == NFS_V3) {
                ret = sunrpc_nfs3_handler(job);
                if (ret)
                        GOTO(err_ret, ret); //GOTO???
        } else if (req->program == ACL_PROGRAM) {
                ret = sunrpc_acl_handler(job);
                if (ret)
                        GOTO(err_ret, ret); //GOTO???
       
        } else if (req->program == NLM_PROGRAM) {
                DINFO("NLM REQUEST\n");
                ret = sunrpc_nlm_handler(job);
                if (ret)
                        GOTO(err_ret, ret); //GOTO???
        }else{
                DERROR("we got wrong prog %u v%u, halt\n", req->program,
                       req->progversion);  //XXX: handle this --gray
        }

        return 0;
err_ret:
        return ret;
}

int sunrpc_accept_handler(void *_sock, void *context)
{
        int ret;
        net_proto_t proto;
        net_handle_t nh;
        ynet_sock_conn_t *sock = _sock;
        int fd = sock->nh.u.sd.sd;
        
        (void) context;

        DBUG("new conn for sd %d\n", fd);

        _memset(&proto, 0x0, sizeof(net_proto_t));

        proto.head_len = sizeof(sunrpc_request_t);
        proto.reader = net_events_handle_read;
        proto.writer = net_events_handle_write;
        proto.pack_len = sunrpc_pack_len;
        proto.pack_handler = __sunrpc_pack_handler;
        //proto.prog[0].handler = sunrpc_request_handler;
        proto.jobtracker = sunrpc_jobtracker;

        ret = sdevent_accept(fd, &nh, &proto, YNET_RPC_NONBLOCK);
        if (ret)
                GOTO(err_ret, ret);

#if 0
        ret = sdevents_align(&nh, 4);
        if (ret)
                GOTO(err_ret, ret);
#endif

        ret = sdevent_add(&nh, NULL, Y_EPOLL_EVENTS, NULL, NULL);
        if (ret)
                GOTO(err_ret, ret);

        return 0;
err_ret:
        return ret;
}

int sunrpc_accept(int srv_sd)
{
        int ret, sd;
        struct sockaddr_in sin;
        socklen_t alen;
        net_proto_t proto;
        net_handle_t nh;

        _memset(&sin, 0, sizeof(sin));
        alen = sizeof(struct sockaddr_in);

        sd = accept(srv_sd, &sin, &alen);
        if (sd < 0 ) {
                ret = errno; 
                GOTO(err_ret, ret);
        }

        ret = tcp_sock_tuning(sd, 1, YNET_RPC_NONBLOCK);
        if (ret)
                GOTO(err_ret, ret);
        
        _memset(&proto, 0x0, sizeof(net_proto_t));
        proto.head_len = sizeof(sunrpc_request_t);
        proto.reader = net_events_handle_read;
        proto.writer = net_events_handle_write;
        proto.pack_len = sunrpc_pack_len;
        proto.pack_handler = __sunrpc_pack_handler;
        //proto.prog[0].handler = sunrpc_request_handler;
        proto.jobtracker = sunrpc_jobtracker;

        nh.type = NET_HANDLE_TRANSIENT;
        nh.u.sd.sd = sd;

        ret = sdevent_open(&nh, &proto);
        if (ret)
                GOTO(err_sd, ret);

        ret = sdevent_add(&nh, NULL, Y_EPOLL_EVENTS_LISTEN, NULL, NULL);
        if (ret)
                GOTO(err_sd, ret);

        return 0;
err_sd:
        close(sd);
err_ret:
        return ret;
}

static int __sunrpc_request_handler(const sockid_t *sockid,
                                    buffer_t *buf)
{
        int ret;
        uid_t uid;
        gid_t gid;
        sunrpc_request_t req;
        auth_head_t cred, veri;
        auth_unix_t auth_unix;
        char credbuf[MAX_AUTH_BYTES];
        char veribuf[MAX_AUTH_BYTES];
        uint32_t is_last;
        //net_handle_t *nh;

        ret = mbuffer_popmsg(buf, &req, sizeof(req));
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        is_last = ntohl(req.length) & (1 << 31);

        if (is_last)
                req.length = ntohl(req.length) ^ (1 << 31);
        else
                req.length = ntohl(req.length);

        req.xid = req.xid;
        req.msgtype = ntohl(req.msgtype);
        req.rpcversion = ntohl(req.rpcversion);
        req.program = ntohl(req.program);
        req.progversion = ntohl(req.progversion);
        req.procedure = ntohl(req.procedure);

        if (unlikely(is_last != (uint32_t)(1 << 31))) {
                DERROR("%u:%u\n", is_last, (uint32_t)(1 << 31));

                DERROR("rpc request len %u xid %u version %u prog %u version"
                                " %u procedure %u\n", req.length, req.xid,
                                req.rpcversion, req.program, req.progversion,
                                req.procedure);

                UNIMPLEMENTED(__DUMP__);
        }

        DBUG("rpc request len %u xid %u version %u prog %u version"
                        " %u procedure %u\n", req.length, req.xid,
                        req.rpcversion, req.program, req.progversion,
                        req.procedure);

        mbuffer_get(buf, &cred, sizeof(auth_head_t));
        cred.flavor = ntohl(cred.flavor);
        cred.length = ntohl(cred.length);

        ret = mbuffer_popmsg(buf, credbuf, cred.length + sizeof(auth_head_t));
        if (unlikely(ret))
                GOTO(err_ret, ret); //GOTO???

        if (cred.length) {
                ret = __auth_unix(&auth_unix, credbuf+sizeof(auth_head_t), cred.length);
                if (unlikely(ret))
                        GOTO(err_ret, ret); //GOTO???

                uid = auth_unix.uid;
                gid = auth_unix.gid;
                yfree((void**)&auth_unix.machinename);
        }

        mbuffer_get(buf, &veri, sizeof(auth_head_t));
        veri.flavor = ntohl(veri.flavor);
        veri.length = ntohl(veri.length);

        DBUG("cred %u len %u veri %u len %u\n", cred.flavor, cred.length,
                        veri.flavor, veri.length);

        ret = mbuffer_popmsg(buf, veribuf, veri.length + sizeof(auth_head_t));
        if (unlikely(ret))
                UNIMPLEMENTED(__DUMP__);

        nfs_newtask(sockid, &req, uid, gid, buf);

        return 0;
err_ret:
        return ret;
}
