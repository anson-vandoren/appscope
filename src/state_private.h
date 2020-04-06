#ifndef __STATE_PRIVATE_H__
#define __STATE_PRIVATE_H__

#include <limits.h>
#include <sys/socket.h>

#define PROTOCOL_STR 16
#define SCOPE_UNIX 99
#define FUNC_MAX 24
#define HDRTYPE_MAX 16

//
// This file contains implementation details for state.c and reporting.c.
// It is expected that state.c and report.c will both directly include
// this file, but it is not recommended that any other file include it.
//

typedef struct {
    uint64_t mtc;
    uint64_t evt;
} counters_element_t;

typedef struct metric_counters_t {
    counters_element_t  openPorts;
    counters_element_t  netConnectionsUdp;
    counters_element_t  netConnectionsTcp;
    counters_element_t  netConnectionsOther;
    counters_element_t  netrxBytes;
    counters_element_t  nettxBytes;
    counters_element_t  readBytes;
    counters_element_t  writeBytes;
    counters_element_t  numSeek;
    counters_element_t  numStat;
    counters_element_t  numOpen;
    counters_element_t  numClose;
    counters_element_t  numDNS;
    counters_element_t  fsDurationNum;
    counters_element_t  fsDurationTotal;
    counters_element_t  connDurationNum;
    counters_element_t  connDurationTotal;
    counters_element_t  dnsDurationNum;
    counters_element_t  dnsDurationTotal;
    counters_element_t  netConnectErrors;
    counters_element_t  netTxRxErrors;
    counters_element_t  netDNSErrors;
    counters_element_t  fsOpenCloseErrors;
    counters_element_t  fsRdWrErrors;
    counters_element_t  fsStatErrors;
} metric_counters;

typedef struct {
    struct {
        int open_close;
        int read_write;
        int stat;
        int seek;
        int error;
    } fs;
    struct {
        int open_close;
        int rx_tx;
        int dns;
        int error;
        int dnserror;
    } net;
} summary_t;

typedef struct evt_type_t {
    metric_t evtype;
} evt_type;

typedef struct protocol_info_t {
    metric_t evtype;
    int fd;
    char header_type[HDRTYPE_MAX];
    char header[8 * 1024];
} protocol_info;

typedef struct stat_err_info_t {
    metric_t evtype;
    metric_t data_type;
    char name[PATH_MAX];
    char funcop[FUNC_MAX];
    metric_counters counters;
} stat_err_info;

typedef struct net_info_t {
    metric_t evtype;
    metric_t data_type;
    int fd;
    int active;
    int type;
    bool urlRedirect;
    bool addrSetLocal;
    bool addrSetRemote;
    bool addrSetUnix;
    counters_element_t numTX;
    counters_element_t numRX;
    counters_element_t txBytes;
    counters_element_t rxBytes;
    bool dnsSend;
    uint64_t startTime;
    counters_element_t numDuration;
    counters_element_t totalDuration;
    uint64_t uid;
    uint64_t lnode;
    uint64_t rnode;
    char dnsName[MAX_HOSTNAME];
    struct sockaddr_storage localConn;
    struct sockaddr_storage remoteConn;
    metric_counters counters;
} net_info;

typedef struct fs_info_t {
    metric_t evtype;
    metric_t data_type;
    int fd;
    int active;
    fs_type_t type;
    counters_element_t numOpen;
    counters_element_t numClose;
    counters_element_t numSeek;
    counters_element_t numRead;
    counters_element_t numWrite;
    counters_element_t readBytes;
    counters_element_t writeBytes;
    counters_element_t numDuration;
    counters_element_t totalDuration;
    uint64_t uid;
    char path[PATH_MAX];
    char funcop[FUNC_MAX];
} fs_info;



// Accessor functions defined in state.c, but used in report.c too.
int get_port(int, int, control_type_t);
int get_port_net(net_info *, int, control_type_t);
bool checkNetEntry(int);
bool checkFSEntry(int);
net_info *getNetEntry(int);
fs_info *getFSEntry(int);

// The hiding of objects forces these to be defined here
void doFSMetric(metric_t, struct fs_info_t *, control_type_t, const char *, ssize_t, const char *);
void doNetMetric(metric_t, struct net_info_t *, control_type_t, ssize_t);
void doUnixEndpoint(int, net_info *);
void resetInterfaceCounts(counters_element_t*);
void addToInterfaceCounts(counters_element_t*, uint64_t);
void subFromInterfaceCounts(counters_element_t*, uint64_t);

// Data that lives in state.c, but is used in report.c too.
extern summary_t g_summary;
extern net_info *g_netinfo;
extern fs_info *g_fsinfo;
extern metric_counters g_ctrs;

#endif // __STATE_PRIVATE_H__