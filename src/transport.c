#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "dbg.h"
#include "scopetypes.h"
#include "os.h"
#include "transport.h"

// Yuck.  Avoids naming conflict between our src/wrap.c and libssl.a
#define SSL_read SCOPE_SSL_read
#define SSL_write SCOPE_SSL_write
#include "openssl/ssl.h"
#include "openssl/err.h"
#undef SSL_read
#undef SSL_write

struct _transport_t
{
    cfg_transport_t type;
    int (*access)(const char *, int);
    ssize_t (*send)(int, const void *, size_t, int);
    int (*open)(const char *, int, ...);
    int (*dup2)(int, int);
    int (*close)(int);
    int (*fcntl)(int, int, ...);
    size_t (*fwrite)(const void *, size_t, size_t, FILE *);
    int (*socket)(int, int, int);
    int (*connect)(int, const struct sockaddr *, socklen_t);
    int (*getaddrinfo)(const char *, const char *,
                       const struct addrinfo *,
                       struct addrinfo **);
    int (*origGetaddrinfo)(const char *, const char *,
                           const struct addrinfo *,
                           struct addrinfo **);
    int (*fclose)(FILE*);
    FILE *(*fdopen)(int, const char *);
    int (*select)(int, fd_set *, fd_set *, fd_set *, struct timeval *);
    union {
        struct {
            int sock;
            fd_set pending_connect;
            char *host;
            char *port;
            struct sockaddr_storage gai_addr;
            struct {
                // Configuration
                unsigned enable;
                unsigned validateserver;
                char *cacertpath;
                // Operational params
                SSL_CTX *ctx;
                SSL *ssl;
            } tls;
        } net;
        struct {
            char *path;
            FILE *stream;
            int stdout;  // Flag to indicate that stream is stdout
            int stderr;  // Flag to indicate that stream is stderr
            cfg_buffer_t buf_policy;
        } file;
    };
};

// This is *not* realtime safe; it's shared between all transports in a
// process.  It's used by scopeGetaddrinfo() to avoid a bug seen in
// node.js processes.  See transportReconnect() below for details.
static struct addrinfo *g_cached_addr = NULL;

// This mutex avoids a race condition between:
//    1) using the tls subsystem and
//    2) destroying the tls subsystem
// If a deadlock is ever seen, consider changing the initializer to
//    PTHREAD_RECURSIVE_MUTEX_INITIALIZER -or-
//    PTHREAD_ERRORCHECK_MUTEX_INITIALIZER
static pthread_mutex_t g_tls_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_tls_calls_are_safe = TRUE;  // until handle_tls_destroy() is called

static inline void
enterCriticalSection(void)
{
    if (pthread_mutex_lock(&g_tls_lock)) {
        DBG(NULL);
    }
}

static inline void
exitCriticalSection(void)
{
    if (pthread_mutex_unlock(&g_tls_lock)) {
        DBG(NULL);
    }
}

static transport_t*
newTransport()
{
    transport_t *t;

    t = calloc(1, sizeof(transport_t));
    if (!t) {
        DBG(NULL);
        return NULL;
    }

    if ((t->access = dlsym(RTLD_NEXT, "access")) == NULL) goto out;
    if ((t->send = dlsym(RTLD_NEXT, "send")) == NULL) goto out;
    if ((t->open = dlsym(RTLD_NEXT, "open")) == NULL) goto out;
    if ((t->dup2 = dlsym(RTLD_NEXT, "dup2")) == NULL) goto out;
    if ((t->close = dlsym(RTLD_NEXT, "close")) == NULL) goto out;
    if ((t->fcntl = dlsym(RTLD_NEXT, "fcntl")) == NULL) goto out;
    if ((t->fwrite = dlsym(RTLD_NEXT, "fwrite")) == NULL) goto out;
    if ((t->socket = dlsym(RTLD_NEXT, "socket")) == NULL) goto out;
    if ((t->connect = dlsym(RTLD_NEXT, "connect")) == NULL) goto out;
    if ((t->getaddrinfo = dlsym(RTLD_NEXT, "getaddrinfo")) == NULL) goto out;
    t->origGetaddrinfo = t->getaddrinfo;  // store a copy
    if ((t->fclose = dlsym(RTLD_NEXT, "fclose")) == NULL) goto out;
    if ((t->fdopen = dlsym(RTLD_NEXT, "fdopen")) == NULL) goto out;
    if ((t->select = dlsym(RTLD_NEXT, "select")) == NULL) goto out;
    return t;

  out:
    DBG("access=%p send=%p open=%p dup2=%p close=%p "
        "fcntl=%p fwrite=%p socket=%p connect=%p "
        "getaddrinfo=%p fclose=%p fdopen=%p select=%p",
        t->access, t->send, t->open, t->dup2, t->close,
        t->fcntl, t->fwrite, t->socket, t->connect,
        t->getaddrinfo, t->fclose, t->fdopen, t->select);
    free(t);
    return NULL;
}

/*
 * Some apps require that a set of fds, usually low numbers, 0-20,
 * must exist. Therefore, we don't want to allow the kernel to
 * give us the next available fd. We need to place the fd in a
 * range that is likely not to affect an app. 
 *
 * We look for an available fd starting at a relatively high
 * range and work our way down until we find one we can get.
 * Then, we force the use of the available fd. 
 */
static int
placeDescriptor(int fd, transport_t *t)
{
    if (!t || !t->fcntl || !t->dup2 || !t->close) return -1;

    // next_fd_to_try avoids reusing file descriptors.
    // Without this, we've had problems where the buffered stream for
    // g_log has it's fd closed and reopened by another transport which
    // causes the mis-routing of data.
    static int next_fd_to_try = DEFAULT_FD;
    if (next_fd_to_try < DEFAULT_MIN_FD) {
        next_fd_to_try = DEFAULT_FD;
    }

    int i, dupfd;

    for (i = next_fd_to_try; i >= DEFAULT_MIN_FD; i--) {
        if ((t->fcntl(i, F_GETFD) == -1) && (errno == EBADF)) {

            // This fd is available, try to dup it
            if ((dupfd = t->dup2(fd, i)) == -1) continue;
            t->close(fd);

            // Set close on exec. (dup2 does not preserve FD_CLOEXEC)
            int flags = t->fcntl(dupfd, F_GETFD, 0);
            if (t->fcntl(dupfd, F_SETFD, flags | FD_CLOEXEC) == -1) {
                DBG("%d", dupfd);
            }

            next_fd_to_try = dupfd - 1;
            return dupfd;
        }
    }
    DBG("%d", t->type);
    t->close(fd);
    return -1;
}

int
transportSetFD(int fd, transport_t *trans)
{
    if (!trans) return -1;

    return placeDescriptor(fd, trans);
}

cfg_transport_t
transportType(transport_t *trans)
{
    if (!trans) return (cfg_transport_t)-1;

    return trans->type;
}

int
transportConnection(transport_t *trans)
{
    if (!trans) return -1;
    switch(trans->type) {
        case CFG_UDP:
        case CFG_TCP:
            return trans->net.sock;
        case CFG_FILE:
            if (trans->file.stream) {
                return fileno(trans->file.stream);
            } else {
                return -1;
            }
        case CFG_UNIX:
        case CFG_SYSLOG:
        case CFG_SHM:
            break;
        default:
            DBG(NULL);
    }

    return -1;
}

int
transportNeedsConnection(transport_t *trans)
{
    if (!trans) return 0;
    switch (trans->type) {
        case CFG_UDP:
        case CFG_TCP:
            if ((trans->net.sock == -1) ||
                (trans->net.tls.enable && !trans->net.tls.ssl)) return 1;
            return osNeedsConnect(trans->net.sock);
        case CFG_FILE:
            // This checks to see if our file descriptor has been
            // closed by our process.  (errno == EBADF) Stream buffering
            // makes it harder to know when this has happened.
            if ((trans->file.stream) &&
                   (trans->fcntl(fileno(trans->file.stream), F_GETFD) == -1)) {
                DBG(NULL);
                transportDisconnect(trans);
            }
            return (trans->file.stream == NULL);
        case CFG_UNIX:
        case CFG_SYSLOG:
        case CFG_SHM:
            break;
        default:
            DBG(NULL);
    }
    return 0;
}

const char *
rootCertFile(transport_t *trans)
{
    // Based off of this: https://golang.org/src/crypto/x509/root_linux.go
    const char* rootFileList[] = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian/Ubuntu/Gentoo etc.
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora/RHEL 6
        "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
        "/etc/pki/tls/cacert.pem",                           // OpenELEC
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS/RHEL 7
        "/etc/ssl/cert.pem",                                 // Alpine Linux
        NULL
    };

    const char *file;
    for (file=rootFileList[0]; file; file++) {
        if (!trans->access (file, R_OK)) {
            return file;
        }
    }

    // Didn't find it from the list above.
    return NULL;
}

static void
shutdownTlsSession(transport_t *trans)
{
    // Grab the lock to show that the tls subsystem is in use.
    enterCriticalSection();

    if (g_tls_calls_are_safe) {
        if (trans->net.tls.ssl) {
            SSL_shutdown(trans->net.tls.ssl);
            SSL_free(trans->net.tls.ssl);
            trans->net.tls.ssl = NULL;
        }
        if (trans->net.tls.ctx) {
            SSL_CTX_free(trans->net.tls.ctx);
            trans->net.tls.ctx = NULL;
        }
    }

    // Release the lock
    exitCriticalSection();

    if (trans->net.sock != -1) {
        trans->close(trans->net.sock);
        trans->net.sock = -1;
    }
}

static void
handle_tls_destroy(void)
{
    // Spin to make sure we don't allow our tls subsystem to be destructed
    // while the tls library is actively being used.
    enterCriticalSection();

    // this records that this function has been called.
    g_tls_calls_are_safe = FALSE;

    // Release the lock
    exitCriticalSection();

    scopeLog("detected beginning of process exit sequence", -1, CFG_LOG_INFO);
}

static int
establishTlsSession(transport_t *trans)
{
    if (!trans || trans->net.sock == -1) return FALSE;
    scopeLog("establishing tls session", trans->net.sock, CFG_LOG_INFO);

    // Grab the lock to show that the tls subsystem is in use.
    enterCriticalSection();
    if (!g_tls_calls_are_safe) goto err;

    // Register a handler once - to be called when OPENSSL is being destructed.
    static int destroy_was_registered = FALSE;
    if (!destroy_was_registered) {
        OPENSSL_atexit(handle_tls_destroy);
        destroy_was_registered = TRUE;
    }

    trans->net.tls.ctx = SSL_CTX_new(TLS_method());
    if (!trans->net.tls.ctx) {
        char msg[512] = {0};
        char err[256] = {0};
        ERR_error_string_n(ERR_peek_last_error() , err, sizeof(err));
        snprintf(msg, sizeof(msg), "error creating tls context: %s", err);
        scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        goto err;
    }

    // If the configuration provides a cacertpath, use it.
    // Otherwise, find a distro-specific root cert file.
    const char *cafile = trans->net.tls.cacertpath;
    if (!cafile) cafile = rootCertFile(trans);

    long loc_rv = SSL_CTX_load_verify_locations(trans->net.tls.ctx, cafile, NULL);
    if (trans->net.tls.validateserver && !loc_rv) {
        char msg[512] = {0};
        char err[256] = {0};
        ERR_error_string_n(ERR_peek_last_error() , err, sizeof(err));
        snprintf(msg, sizeof(msg), "error setting tls cacertpath: \"%s\" : %s", cafile, err);
        scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        // We're not treating this as a hard error at this point.
        // Let the process proceed; validation below will likely fail
        // and might provide more meaningful info.
    }

    trans->net.tls.ssl = SSL_new(trans->net.tls.ctx);
    if (!trans->net.tls.ssl) {
        char msg[512] = {0};
        char err[256] = {0};
        ERR_error_string_n(ERR_peek_last_error() , err, sizeof(err));
        snprintf(msg, sizeof(msg), "error creating tls session: %s", err);
        scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        goto err;
    }

    if (!SSL_set_fd(trans->net.tls.ssl, trans->net.sock)) {
        char msg[512] = {0};
        char err[256] = {0};
        ERR_error_string_n(ERR_peek_last_error() , err, sizeof(err));
        snprintf(msg, sizeof(msg), "error setting tls on socket: %d : %s", trans->net.sock, err);
        scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        goto err;
    }

    ERR_clear_error(); // to make SSL_get_error reliable
    int con_rv = SSL_connect(trans->net.tls.ssl);
    if (con_rv != 1) {
        char msg[512] = {0};
        char err[256] = {0};
        int ssl_err = SSL_get_error(trans->net.tls.ssl, con_rv);
        ERR_error_string_n(ssl_err, err, sizeof(err));
        snprintf(msg, sizeof(msg), "error establishing tls connection: %s", err);
        scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        if (ssl_err == SSL_ERROR_SSL || ssl_err == SSL_ERROR_SYSCALL) {
            ERR_error_string_n(ERR_peek_last_error() , err, sizeof(err));
            snprintf(msg, sizeof(msg), "error establishing tls connection: %s %d", err, errno);
            scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
        }
        goto err;
    }

    if (trans->net.tls.validateserver) {
        // Just test that we received a server cert
        X509* cert = SSL_get_peer_certificate(trans->net.tls.ssl);
        if (cert) {
            X509_free(cert);  // Looks good.  Free it immediately
        } else {
            scopeLog("error accessing peer certificate for tls server validation",
                                                  trans->net.sock, CFG_LOG_INFO);
            goto err;
        }

        long ver_rc = SSL_get_verify_result(trans->net.tls.ssl);
        if (ver_rc != X509_V_OK) {
            char msg[512] = {0};
            const char *err = X509_verify_cert_error_string(ver_rc);
            snprintf(msg, sizeof(msg), "tls server validation failed : \"%s\"", err);
            scopeLog(msg, trans->net.sock, CFG_LOG_INFO);
            goto err;
        }
    }

    // free the lock
    exitCriticalSection();

    scopeLog("tls session established", trans->net.sock, CFG_LOG_INFO);
    return TRUE;
err:
    // free the lock
    exitCriticalSection();

    shutdownTlsSession(trans);
    return FALSE;
}

int
transportDisconnect(transport_t *trans)
{
    if (!trans) return 0;
    switch (trans->type) {
        case CFG_UDP:
        case CFG_TCP:
            // appropriate for both tls and non-tls connections...
            shutdownTlsSession(trans);
            int i;
            for (i=0; i<FD_SETSIZE; i++) {
                if (!FD_ISSET(i, &trans->net.pending_connect)) continue;
                trans->close(i);
                FD_CLR(i, &trans->net.pending_connect);
            }
            break;
        case CFG_FILE:
            if (!trans->file.stdout && !trans->file.stderr) {
                if (trans->file.stream) trans->fclose(trans->file.stream);
            }
            trans->file.stream = NULL;
            break;
        case CFG_UNIX:
        case CFG_SYSLOG:
        case CFG_SHM:
            break;
        default:
            DBG(NULL);
    }
    return 0;
}


// We've observed that node.js processes can hang from spinlocks
// in glibc's getaddrinfo:
//
//      #0  __lll_lock_wait_private ()
//                 at ../sysdeps/unix/sysv/linux/x86_64/lowlevellock.S:95
//      #1  in get_locked_global () at resolv_conf.c:90
//      #2  resolv_conf_get_1 () at resolv_conf.c:200
//      #3  __resolv_conf_get () at resolv_conf.c:359
//      #4  in context_alloc () at resolv_context.c:137
//      #5  context_get (preinit=false) at resolv_context.c:181
//      #6  __GI___resolv_context_get () at resolv_context.c:195
//      #7  in gaih_inet () at ../sysdeps/posix/getaddrinfo.c:767
//      #8  in __GI_getaddrinfo () at ../sysdeps/posix/getaddrinfo.c:2300
//      #9  in socketConnectionStart () at src/transport.c:339
//      #10 in transportConnect () at src/transport.c:514
//      #11 in transportCreateTCP () at src/transport.c:549
//      #12 in initTransport (cfg=0x5446bb0, t=CFG_CTL) at src/cfgutils.c:1516
//      #13 in initCtl (cfg=0x5446bb0) at src/cfgutils.c:1609
//      #14 in doReset () at src/wrap.c:637
//      #15 in fork () at src/wrap.c:3361
//      #16 in uv_spawn () at ../deps/uv/src/unix/process.c:489
//
// Here's our own version that returns an address from a previously
// successful connection.  Look ma, no spinlocks!  See transportReconnect()
// below for more info.
static int
scopeGetaddrinfo(const char *node, const char *service,
                  const struct addrinfo *hints,
                  struct addrinfo **res)
{
    if (!res) return 1;
    *res = g_cached_addr;
    return (g_cached_addr) ? 0 : 1; // 0 is successful
}

static struct addrinfo *
getExistingConnectionAddr(transport_t *trans)
{
    struct addrinfo *ai = NULL;

    if (transportNeedsConnection(trans) || trans->type != CFG_TCP) goto err;

    // Allocate what we need to be compatible with freeaddrinfo()
    ai = calloc(1, sizeof(struct addrinfo));
    if (!ai) {
        DBG(NULL);
        goto err;
    }

    // Clear the address value
    socklen_t addrsize = sizeof(trans->net.gai_addr);
    struct sockaddr *addr = (struct sockaddr*)&trans->net.gai_addr;
    memset(addr, 0, addrsize);

    // lookup the address
    if (getpeername(trans->net.sock, addr, &addrsize)) {
        DBG(NULL);
        goto err;
    }

    // Set all the fields
    ai->ai_flags = 0;                  // Unused by us
    ai->ai_family = addr->sa_family;
    ai->ai_socktype = SOCK_STREAM;
    ai->ai_protocol = IPPROTO_TCP;
    ai->ai_addrlen = addrsize;
    ai->ai_addr = addr;
    ai->ai_canonname = NULL;           // Unused by us
    ai->ai_next = NULL;

    return ai;

err:
    if (ai) free(ai);
    return NULL;
}


// This is expected to be called by child processses that
// may have inherited connected transports from their parent
// processes.  i.e. fork()->doReset() path
// As a caution, because of its use of g_cached_addr, it's
// *not* reentrant.
int
transportReconnect(transport_t *trans)
{
    if (!trans) return 0;

    switch (trans->type) {
        case CFG_TCP:
            // Since TCP is connection-oriented, we want to disconnect
            // and reconnect so child processes can have distinct
            // connections from their parents.  However, we can't use
            // glibc's getaddrinfo lest we introduce hangs in node.js
            // processes.  So, if a transport has an existing connection,
            // grab the address from that connection and substitute in our
            // own getaddrinfo for this situation.

            g_cached_addr = getExistingConnectionAddr(trans);
            transportDisconnect(trans);          // Never keep the parents connection.
            if (g_cached_addr) {
                trans->getaddrinfo = scopeGetaddrinfo;
                transportConnect(trans);         // Will use g_cached_addr
                trans->getaddrinfo = trans->origGetaddrinfo;
            }

            break;
        case CFG_UDP:
        case CFG_FILE:
        case CFG_SYSLOG:
        case CFG_SHM:
            // Everything else is a no-op.  These can all share
            // the parent's transport.
            break;
        default:
            DBG(NULL);
    }
    return 0;
}

static int
setSocketBlocking(transport_t *trans, int sock, bool block)
{
    if (!trans) return 0;

    int current_flags = trans->fcntl(sock, F_GETFL, NULL);
    if (current_flags < 0) return FALSE;

    int desired_flags;
    if (block) {
        desired_flags = current_flags & ~O_NONBLOCK;
    } else {
        desired_flags = current_flags | O_NONBLOCK;
    }

    // We're successful; the flag is as desired
    if (current_flags == desired_flags) return TRUE;

    // fcntl returns 0 if successful
    return (trans->fcntl(sock, F_SETFL, desired_flags) == 0);
}

static int
socketConnectIsPending(transport_t *trans)
{
    int i;
    for (i=0; i<FD_SETSIZE; i++) {
        if (FD_ISSET(i, &trans->net.pending_connect)) return TRUE;
    }
    return FALSE;
}

static int
checkPendingSocketStatus(transport_t *trans)
{
    if (!trans) return 0;
    int rc;
    struct timeval tv = {0};
    fd_set pending_results = trans->net.pending_connect;
    rc = trans->select(FD_SETSIZE, NULL, &pending_results, NULL, &tv);
    if (rc < 0) {
        DBG(NULL);
        transportDisconnect(trans);
        return 0;
    } else if (rc == 0) {
        // No new status is available
        return 0;
    }

    int i;
    rc = 0;
    for (i=0; i<FD_SETSIZE; i++) {
        if (!FD_ISSET(i, &pending_results)) continue;

        // If we can't get socket status, or the status is an error, close the
        // socket that failed to connect and remove it from the pending list.
        int opt;
        socklen_t optlen = sizeof(opt);
        if ((getsockopt(i, SOL_SOCKET, SO_ERROR, (void*)(&opt), &optlen) < 0)
            || opt) {
            scopeLog("connect failed", i, CFG_LOG_INFO);

            FD_CLR(i, &trans->net.pending_connect);
            trans->close(i);
            continue;
        }

        scopeLog("connect successful", i, CFG_LOG_INFO);

        // Hey!  We found one that will work!
        // Move this descriptor up out of the way
        FD_CLR(i, &trans->net.pending_connect);
        trans->net.sock = placeDescriptor(i, trans);
        if (trans->net.sock == -1) continue;

        // Set the TCP socket to blocking
        if ((trans->type == CFG_TCP) && !setSocketBlocking(trans, trans->net.sock, TRUE)) {
            DBG("%d %s %s", trans->net.sock, trans->net.host, trans->net.port);
        }

        // We have a connected socket!  Woot!
        // Do the tls stuff for this connection as needed.
        if (trans->net.tls.enable) {
            // when successful, we'll have a connected tls socket.
            // when not, this will cleanup, disconnecting the socket.
            establishTlsSession(trans);
        }

        break;
    }

    // If we were successful, we can stop looking.  Clean up pending sockets.
    if (trans->net.sock != -1) {
        rc = 1;
        for (i=0; i<FD_SETSIZE; i++) {
            if (FD_ISSET(i, &trans->net.pending_connect)) {
                scopeLog("abandoning connect due to previous success", i, CFG_LOG_INFO);
                trans->close(i);
                FD_CLR(i, &trans->net.pending_connect);
            }
        }
    }

    return rc;
}


static int
socketConnectionStart(transport_t *trans)
{
    struct addrinfo* addr_list = NULL;
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;     // IPv4 or IPv6

    switch (trans->type) {
        case CFG_UDP:
            hints.ai_socktype = SOCK_DGRAM;  // For UDP
            hints.ai_protocol = IPPROTO_UDP; // For UDP
            break;
        case CFG_TCP:
            hints.ai_socktype = SOCK_STREAM; // For TCP
            hints.ai_protocol = IPPROTO_TCP; // For TCP
            break;
        default:
            DBG(NULL);
            return 1;
    }

    char *logmsg = NULL;
    char *type = (trans->type == CFG_UDP) ? "udp" : "tcp";
    if (asprintf(&logmsg, "getting DNS info for %s %s:%s",
             type, trans->net.host, trans->net.port) != -1) {
        scopeLog(logmsg, -1, CFG_LOG_INFO);
        if (logmsg) free(logmsg);
    }

    if (trans->getaddrinfo(trans->net.host,
                           trans->net.port,
                           &hints, &addr_list)) return 0;

    // Loop through the addresses until one works
    struct addrinfo* addr;
    for (addr = addr_list; addr; addr = addr->ai_next) {
        int sock;
        sock = trans->socket(addr->ai_family,
                             addr->ai_socktype,
                             addr->ai_protocol);

        if (sock == -1) continue;

        // Set the socket to close on exec
        int flags = trans->fcntl(sock, F_GETFD, 0);
        if (trans->fcntl(sock, F_SETFD, flags | FD_CLOEXEC) == -1) {
            DBG("%d %s %s", sock, trans->net.host, trans->net.port);
        }

        // Connect will hang in some cases; start by setting non-blocking
        if (!setSocketBlocking(trans, sock, FALSE)) {
            DBG("%d %s %s", sock, trans->net.host, trans->net.port);
            transportDisconnect(trans);
            continue;
        }

        void *addrptr = NULL;
        unsigned short *portptr = NULL;
        if (addr->ai_family == AF_INET) {
            struct sockaddr_in *addr4_ptr;
            addr4_ptr = (struct sockaddr_in *)addr->ai_addr;
            addrptr = &addr4_ptr->sin_addr;
            portptr = &addr4_ptr->sin_port;
        } else if (addr->ai_family == AF_INET6) {
            struct sockaddr_in6 *addr6_ptr;
            addr6_ptr = (struct sockaddr_in6 *)addr->ai_addr;
            addrptr = &addr6_ptr->sin6_addr;
            portptr = &addr6_ptr->sin6_port;
        } else {
            DBG("%d %s %s %d", sock, trans->net.host, trans->net.port, addr->ai_family);
            trans->close(sock);
            continue;
        }
        char addrstr[INET6_ADDRSTRLEN];
        inet_ntop(addr->ai_family, addrptr, addrstr, sizeof(addrstr));
        unsigned short port = ntohs(*portptr);

        errno = 0;
        if (trans->connect(sock,
                           addr->ai_addr,
                           addr->ai_addrlen) == -1) {

            if (errno != EINPROGRESS) {
                char *logmsg = NULL;
                if (asprintf(&logmsg, "connect to %s:%d failed", addrstr, port) != -1) {
                    scopeLog(logmsg, sock, CFG_LOG_INFO);
                    if (logmsg) free(logmsg);
                }

                // We could create a sock, but not connect.  Clean up.
                trans->close(sock);
                continue;
            }

            char *logmsg = NULL;
            if (asprintf(&logmsg, "connect to %s:%d is pending", addrstr, port) != -1) {
                scopeLog(logmsg, sock, CFG_LOG_INFO);
                if (logmsg) free(logmsg);
            }

            FD_SET(sock, &trans->net.pending_connect);
            continue;
        }


        if (trans->type == CFG_UDP) {
            char *logmsg = NULL;
            if (asprintf(&logmsg, "connect to %s:%d was successful", addrstr, port) != -1) {
                scopeLog(logmsg, sock, CFG_LOG_INFO);
                if (logmsg) free(logmsg);
            }

            // connect on udp sockets normally succeeds immediately.
            trans->net.sock = placeDescriptor(sock, trans);
            if (trans->net.sock != -1) break;
        } else {
            DBG(NULL); // with non-blocking tcp sockets, we always expect -1
        }
    }

    if (addr_list) freeaddrinfo(addr_list);

    return (trans->net.sock != -1);
}

static int
transportConnectFile(transport_t *t)
{
    // if stdout/stderr, set stream and skip everything else in the function.
    if (t->file.stdout) {
        t->file.stream = stdout;
        return 1;
    } else if (t->file.stderr) {
        t->file.stream = stderr;
        return 1;
    }

    int fd;
    fd = t->open(t->file.path, O_CREAT|O_WRONLY|O_APPEND|O_CLOEXEC, 0666);
    if (fd == -1) {
        DBG("%s", t->file.path);
        transportDisconnect(t);
        return 0;
    }

    // Move this descriptor up out of the way
    if ((fd = placeDescriptor(fd, t)) == -1) {
        transportDisconnect(t);
        return 0;
    }

    // Needed because umask affects open permissions
    if (fchmod(fd, 0666) == -1) {
        DBG("%d %s", fd, t->file.path);
    }

    FILE* f;
    if (!(f = t->fdopen(fd, "a"))) {
        transportDisconnect(t);
        return 0;
    }
    t->file.stream = f;

    // Fully buffer the output unless we're told not to.
    // I expect line buffering to be useful when we're debugging crashes or
    // or if many applications are configured to write to the same files.
    int buf_mode = _IOFBF;
    switch (t->file.buf_policy) {
        case CFG_BUFFER_FULLY:
            buf_mode = _IOFBF;
            break;
        case CFG_BUFFER_LINE:
            buf_mode = _IOLBF;
            break;
        default:
            DBG("%d", t->file.buf_policy);
    }
    if (setvbuf(t->file.stream, NULL, buf_mode, BUFSIZ)) {
        DBG(NULL);
    }

    return (t->file.stream != NULL);
}

int
transportConnect(transport_t *trans)
{
    if (!trans) return 1;

    // We're already connected.  Do nothing.
    if (!transportNeedsConnection(trans)) return 1;

    switch (trans->type) {
        case CFG_UDP:
        case CFG_TCP:
            if (!socketConnectIsPending(trans)) {
                // socketConnectionStart can directly connect (udp).
                // If it does, we're done.
                if (socketConnectionStart(trans)) return 1;
            }
            // Check to see if the a pending connetion has been successful.
            return checkPendingSocketStatus(trans);
        case CFG_FILE:
            return transportConnectFile(trans);
        default:
            DBG(NULL);
    }

    return 1;
}

transport_t *
transportCreateTCP(const char *host, const char *port)
{
    transport_t* trans = NULL;

    if (!host || !port) return trans;

    trans = newTransport();
    if (!trans) return trans;

    trans->type = CFG_TCP;
    trans->net.sock = -1;
    FD_ZERO(&trans->net.pending_connect);
    trans->net.host = strdup(host);
    trans->net.port = strdup(port);

    if (!trans->net.host || !trans->net.port) {
        DBG(NULL);
        transportDestroy(&trans);
        return trans;
    }

    transportConnect(trans);

    return trans;
}

transport_t*
transportCreateUdp(const char* host, const char* port)
{
    transport_t* t = NULL;

    if (!host || !port) return t;

    t = newTransport();
    if (!t) return t;

    t->type = CFG_UDP;
    t->net.sock = -1;
    FD_ZERO(&t->net.pending_connect);
    t->net.host = strdup(host);
    t->net.port = strdup(port);

    if (!t->net.host || !t->net.port) {
        DBG(NULL);
        transportDestroy(&t);
        return t;
    }

    transportConnect(t);

    return t;
}

transport_t*
transportCreateFile(const char* path, cfg_buffer_t buf_policy)
{
    transport_t *t;

    if (!path) return NULL;
    t = newTransport();
    if (!t) return NULL; 

    t->type = CFG_FILE;
    t->file.path = strdup(path);
    if (!t->file.path) {
        DBG("%s", path);
        transportDestroy(&t);
        return t;
    }
    t->file.buf_policy = buf_policy;

    // See if path is "stdout" or "stderr"
    t->file.stdout = !strcmp(path, "stdout");
    t->file.stderr = !strcmp(path, "stderr");

    transportConnect(t);

    return t;
}

transport_t*
transportCreateUnix(const char* path)
{
    if (!path) return NULL;
    transport_t* t = calloc(1, sizeof(transport_t));
    if (!t) {
        DBG(NULL);
        return NULL;
    }

    t->type = CFG_UNIX;

    return t;
}

transport_t*
transportCreateSyslog(void)
{
    transport_t* t = calloc(1, sizeof(transport_t));
    if (!t) {
        DBG(NULL);
        return NULL;
    }

    t->type = CFG_SYSLOG;

    return t;
}

transport_t*
transportCreateShm()
{
    transport_t* t = calloc(1, sizeof(transport_t));
    if (!t) {
        DBG(NULL);
        return NULL;
    }

    t->type = CFG_SHM;

    return t;
}

void
transportDestroy(transport_t** transport)
{
    if (!transport || !*transport) return;

    transport_t* t = *transport;
    switch (t->type) {
        case CFG_UDP:
        case CFG_TCP:
            transportDisconnect(t);
            if (t->net.host) free (t->net.host);
            if (t->net.port) free (t->net.port);
            if (t->net.tls.cacertpath) free(t->net.tls.cacertpath);
            break;
        case CFG_UNIX:
            break;
        case CFG_FILE:
            if (t->file.path) free(t->file.path);
            if (!t->file.stdout && !t->file.stderr) {
                // if stdout/stderr, we didn't open stream, so don't close it
                if (t->file.stream) t->fclose(t->file.stream);
            }
            break;
        case CFG_SYSLOG:
            break;
        case CFG_SHM:
            break;
        default:
            DBG("%d", t->type);
    }
    free(t);
    *transport = NULL;
}

void
transportConfigureTls(transport_t *trans, unsigned int enable,
                      unsigned int validateserver, const char *cacertpath)
{
    if (!trans) return;
    switch (trans->type) {
        case CFG_UDP:
        case CFG_TCP:
            trans->net.tls.enable = enable;
            trans->net.tls.validateserver = validateserver;
            trans->net.tls.cacertpath = (cacertpath) ? strdup(cacertpath) : NULL;
            break;
        case CFG_UNIX:
        case CFG_FILE:
        case CFG_SYSLOG:
        case CFG_SHM:
            break;
        default:
            DBG("%d", trans->type);
    }
}

static int
tcpSendPlain(transport_t *trans, const char *msg, size_t len)
{
    if (!trans || transportNeedsConnection(trans)) return -1;

    if (!trans->send) {
        DBG(NULL);
        return -1;
    }
    int flags = 0;
#ifdef __LINUX__
    flags |= MSG_NOSIGNAL;
#endif

    size_t bytes_to_send = len;
    size_t bytes_sent = 0;
    int rc;

    while (bytes_to_send > 0) {
        rc = trans->send(trans->net.sock, &msg[bytes_sent], bytes_to_send, flags);
        if (rc <= 0) break;

        if (rc != bytes_to_send) {
            DBG("rc = %d, bytes_to_send = %zu", rc, bytes_to_send);
        }

        bytes_sent += rc;
        bytes_to_send -= rc;
    }

    if (rc < 0) {
        switch (errno) {
        case EBADF:
        case EPIPE:
            DBG(NULL);
            transportDisconnect(trans);
            transportConnect(trans);
            return -1;
        default:
            DBG(NULL);
        }
    }
    return 0;
}

static int
tcpSendTls(transport_t *trans, const char *msg, size_t len)
{
    if (!trans || transportNeedsConnection(trans)) return -1;

    size_t bytes_to_send = len;
    size_t bytes_sent = 0;
    int rc = 0;
    int err = 0;

    while (bytes_to_send > 0) {
        // Grab the lock to show that the tls subsystem is in use.
        enterCriticalSection();

        rc = 0;
        if (g_tls_calls_are_safe) {
            ERR_clear_error(); // to make SSL_get_error reliable
            rc = SCOPE_SSL_write(trans->net.tls.ssl, &msg[bytes_sent], bytes_to_send);
            if (rc <= 0) {
                err = SSL_get_error(trans->net.tls.ssl, rc);
            }
        }

        // free the lock
        exitCriticalSection();

        if (rc <= 0) {
            DBG("%d %d", err, g_tls_calls_are_safe);
            transportDisconnect(trans);
            transportConnect(trans);
            return -1;
        }

        if (rc != bytes_to_send) {
            DBG("rc = %d, bytes_to_send = %zu", rc, bytes_to_send);
        }

        bytes_sent += rc;
        bytes_to_send -= rc;
    }

    return 0;
}

int
transportSend(transport_t *trans, const char *msg, size_t len)
{
    if (!trans || !msg) return -1;

    switch (trans->type) {
        case CFG_UDP:
            if (trans->net.sock != -1) {
                if (!trans->send) {
                    DBG(NULL);
                    break;
                }
                int rc = trans->send(trans->net.sock, msg, len, 0);

                if (rc < 0) {
                    switch (errno) {
                    case EBADF:
                        DBG(NULL);
                        transportDisconnect(trans);
                        transportConnect(trans);
                        return -1;
                    case EWOULDBLOCK:
                        DBG(NULL);
                        break;
                    default:
                        DBG(NULL);
                    }
                }
            }
            break;
        case CFG_TCP:
            if (trans->net.tls.enable) {
                return tcpSendTls(trans, msg, len);
            } else {
                return tcpSendPlain(trans, msg, len);
            }
            break;
        case CFG_FILE:
            if (trans->file.stream) {
                size_t msg_size = len;
                int bytes = trans->fwrite(msg, 1, msg_size, trans->file.stream);
                if (bytes != msg_size) {
                    if (errno == EBADF) {
                        DBG("%d %d", bytes, msg_size);
                        transportDisconnect(trans);
                        transportConnect(trans);
                        return -1;
                    }
                    DBG("%d %d", bytes, msg_size);
                    return -1;
                }
            }
            break;
        case CFG_UNIX:
        case CFG_SYSLOG:
        case CFG_SHM:
            return -1;
        default:
            DBG("%d", trans->type);
            return -1;
    }
     return 0;
}

int
transportFlush(transport_t* t)
{
    if (!t) return -1;

    switch (t->type) {
        case CFG_UDP:
        case CFG_TCP:
            break;
        case CFG_FILE:
            if (fflush(t->file.stream) == EOF) {
                DBG(NULL);
            }
            break;
        case CFG_UNIX:
        case CFG_SYSLOG:
        case CFG_SHM:
            return -1;
        default:
            DBG("%d", t->type);
            return -1;
    }
    return 0;
}

