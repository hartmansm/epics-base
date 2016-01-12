/*************************************************************************\
* Copyright (c) 2002 The University of Chicago, as Operator of Argonne
*     National Laboratory.
* Copyright (c) 2002 The Regents of the University of California, as
*     Operator of Los Alamos National Laboratory.
* EPICS BASE is distributed subject to a Software License Agreement found
* in file LICENSE that is included with this distribution.
\*************************************************************************/

/*
 * $Revision-Id$
 *
 *  Author: Jeffrey O. Hill
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <errno.h>

#include "addrList.h"
#include "epicsEvent.h"
#include "epicsMutex.h"
#include "epicsSignal.h"
#include "epicsStdio.h"
#include "epicsTime.h"
#include "errlog.h"
#include "freeList.h"
#include "osiPoolStatus.h"
#include "osiSock.h"
#include "taskwd.h"
#include "cantProceed.h"

#define epicsExportSharedSymbols
#include "dbChannel.h"
#include "dbCommon.h"
#include "dbEvent.h"
#include "db_field_log.h"
#include "dbServer.h"
#include "rsrv.h"

#define GLBLSOURCE
#include "server.h"

epicsThreadPrivateId rsrvCurrentClient;

/*
 *
 *  req_server()
 *
 *  CA server task
 *
 *  Waits for connections at the CA port and spawns a task to
 *  handle each of them
 *
 */
static void req_server (void *pParm)
{
    rsrv_iface_config *conf = pParm;
    SOCKET IOC_sock;

    taskwdInsert ( epicsThreadGetIdSelf (), NULL, NULL );

    IOC_sock = conf->tcp;

    /* listen and accept new connections */
    if ( listen ( IOC_sock, 20 ) < 0 ) {
        char sockErrBuf[64];
        epicsSocketConvertErrnoToString (
            sockErrBuf, sizeof ( sockErrBuf ) );
        errlogPrintf ( "CAS: Listen error %s\n",
            sockErrBuf );
        epicsSocketDestroy (IOC_sock);
        epicsThreadSuspendSelf ();
    }

    epicsEventSignal(castcp_startStopEvent);

    while (TRUE) {
        SOCKET clientSock;
        struct sockaddr     sockAddr;
        osiSocklen_t        addLen = sizeof(sockAddr);

        while (castcp_ctl == ctlPause) {
            epicsThreadSleep(0.1);
        }

        clientSock = epicsSocketAccept ( IOC_sock, &sockAddr, &addLen );
        if ( clientSock == INVALID_SOCKET ) {
            char sockErrBuf[64];
            epicsSocketConvertErrnoToString (
                sockErrBuf, sizeof ( sockErrBuf ) );
            errlogPrintf("CAS: Client accept error was \"%s\"\n",
                sockErrBuf );
            epicsThreadSleep(15.0);
            continue;
        }
        else {
            epicsThreadId id;
            struct client *pClient;

            /* socket passed in is closed if unsuccessful here */
            pClient = create_tcp_client ( clientSock );
            if ( ! pClient ) {
                epicsThreadSleep ( 15.0 );
                continue;
            }

            LOCK_CLIENTQ;
            ellAdd ( &clientQ, &pClient->node );
            UNLOCK_CLIENTQ;

            id = epicsThreadCreate ( "CAS-client", epicsThreadPriorityCAServerLow,
                    epicsThreadGetStackSize ( epicsThreadStackBig ),
                    camsgtask, pClient );
            if ( id == 0 ) {
                LOCK_CLIENTQ;
                ellDelete ( &clientQ, &pClient->node );
                UNLOCK_CLIENTQ;
                destroy_tcp_client ( pClient );
                errlogPrintf ( "CAS: task creation for new client failed\n" );
                epicsThreadSleep ( 15.0 );
                continue;
            }
        }
    }
}

static
int tryBind(SOCKET sock, const osiSockAddr* addr, const char *name)
{
    if(bind(sock, &addr->ia, sizeof(*addr))<0) {
        char sockErrBuf[64];
        if(errno!=SOCK_EADDRINUSE)
        {
            epicsSocketConvertErrnoToString (
                        sockErrBuf, sizeof ( sockErrBuf ) );
            errlogPrintf ( "CAS: %s bind error: \"%s\"\n",
                           name, sockErrBuf );
            epicsThreadSuspendSelf ();
        }
        return -1;
    } else
        return 0;
}

/* need to collect a set of TCP sockets, one for each interface,
 * which are bound to the same TCP port number.
 * Needed to avoid the complications and confusion of different TCP
 * ports for each interface (name server and beacon sender would need
 * to know this).
 */
static
SOCKET* rsrv_grap_tcp(unsigned short *port)
{
    SOCKET *socks;
    osiSockAddr scratch;

    socks = mallocMustSucceed(ellCount(&casIntfAddrList)*sizeof(*socks), "rsrv_grap_tcp");

    /* start with preferred port */
    memset(&scratch, 0, sizeof(scratch));
    scratch.ia.sin_family = AF_INET;
    scratch.ia.sin_port = htons(*port);

    while(1) {
        ELLNODE *cur;
        unsigned i, ok = 1;

        for(i=0; i<ellCount(&casIntfAddrList); i++)
            socks[i] = INVALID_SOCKET;

        for (i=0, cur=ellFirst(&casIntfAddrList); cur; i++, cur=ellNext(cur))
        {
            SOCKET tcpsock;
            osiSockAddr ifaceAddr = ((osiSockAddrNode *)cur)->addr;

            scratch.ia.sin_addr = ifaceAddr.ia.sin_addr;

            tcpsock = socks[i] = epicsSocketCreate (AF_INET, SOCK_STREAM, 0);
            if(tcpsock==INVALID_SOCKET)
                cantProceed("rsrv ran out of sockets during initialization");

            epicsSocketEnableAddressReuseDuringTimeWaitState ( tcpsock );

            if(bind(tcpsock, &scratch.sa, sizeof(scratch))==0) {
                if(scratch.ia.sin_port==0) {
                    /* use first socket to pick a random port */
                    assert(i==0);
                    osiSocklen_t alen = sizeof(ifaceAddr);
                    if(getsockname(tcpsock, &ifaceAddr.sa, &alen)) {
                        char sockErrBuf[64];
                        epicsSocketConvertErrnoToString (
                            sockErrBuf, sizeof ( sockErrBuf ) );
                        errlogPrintf ( "CAS: getsockname error was \"%s\"\n",
                            sockErrBuf );
                        epicsThreadSuspendSelf ();
                        ok = 0;
                        break;
                    }
                    scratch.ia.sin_port = ifaceAddr.ia.sin_port;
                    assert(scratch.ia.sin_port!=0);
                }
            } else {
                /* bind fails.  React harshly to unexpected errors to avoid an infinite loop */
                if(errno==SOCK_EADDRNOTAVAIL) {
                    char name[40];
                    ipAddrToDottedIP(&scratch.ia, name, sizeof(name));
                    printf("Skipping %s which is not an interface address\n", name);
                    ellDelete(&casIntfAddrList, cur);
                    free(cur);
                    ok = 0;
                    break;
                }
                if(errno!=SOCK_EADDRINUSE && errno!=SOCK_EADDRNOTAVAIL) {
                    char name[40];
                    char sockErrBuf[64];
                    epicsSocketConvertErrnoToString (
                        sockErrBuf, sizeof ( sockErrBuf ) );
                    ipAddrToDottedIP(&scratch.ia, name, sizeof(name));
                    errlogPrintf ( "CAS: Socket bind %s error was \"%s\"\n",
                        name, sockErrBuf );
                    epicsThreadSuspendSelf ();
                }
                ok = 0;
                break;
            }
        }

        if (ok) {
            assert(scratch.ia.sin_port!=0);
            *port = ntohs(scratch.ia.sin_port);

            break;
        } else {

            for(i=0; i<ellCount(&casIntfAddrList); i++) {
                /* cleanup any ports actually bound */
                if(socks[i]!=INVALID_SOCKET) {
                    epicsSocketDestroy(socks[i]);
                    socks[i] = INVALID_SOCKET;
                }
            }

            scratch.ia.sin_port=0; /* next iteration starts with a random port */
        }
    }

    return socks;
}

static dbServer rsrv_server = {
    ELLNODE_INIT,
    "rsrv",
    casr,
    casStatsFetch,
    casClientInitiatingCurrentThread
};

/*
 * rsrv_init ()
 */
int rsrv_init (void)
{
    long maxBytesAsALong;
    long status;
    unsigned short udp_port, beacon_port;
    SOCKET *socks;

    clientQlock = epicsMutexMustCreate();

    ellInit ( &clientQ );
    freeListInitPvt ( &rsrvClientFreeList, sizeof(struct client), 8 );
    freeListInitPvt ( &rsrvChanFreeList, sizeof(struct channel_in_use), 512 );
    freeListInitPvt ( &rsrvEventFreeList, sizeof(struct event_ext), 512 );
    freeListInitPvt ( &rsrvSmallBufFreeListTCP, MAX_TCP, 16 );
    initializePutNotifyFreeList ();

    epicsSignalInstallSigPipeIgnore ();

    rsrvCurrentClient = epicsThreadPrivateCreate ();

    dbRegisterServer(&rsrv_server);

    if ( envGetConfigParamPtr ( &EPICS_CAS_SERVER_PORT ) ) {
        ca_server_port = envGetInetPortConfigParam ( &EPICS_CAS_SERVER_PORT,
            (unsigned short) CA_SERVER_PORT );
    }
    else {
        ca_server_port = envGetInetPortConfigParam ( &EPICS_CA_SERVER_PORT,
            (unsigned short) CA_SERVER_PORT );
    }
    udp_port = ca_server_port;

    if (envGetConfigParamPtr(&EPICS_CAS_BEACON_PORT)) {
        beacon_port = envGetInetPortConfigParam (&EPICS_CAS_BEACON_PORT,
            (unsigned short) CA_REPEATER_PORT );
    }
    else {
        beacon_port = envGetInetPortConfigParam (&EPICS_CA_REPEATER_PORT,
            (unsigned short) CA_REPEATER_PORT );
    }

    status =  envGetLongConfigParam ( &EPICS_CA_MAX_ARRAY_BYTES, &maxBytesAsALong );
    if ( status || maxBytesAsALong < 0 ) {
        errlogPrintf ( "CAS: EPICS_CA_MAX_ARRAY_BYTES was not a positive integer\n" );
        rsrvSizeofLargeBufTCP = MAX_TCP;
    }
    else {
        /* allow room for the protocol header so that they get the array size they requested */
        static const unsigned headerSize = sizeof ( caHdr ) + 2 * sizeof ( ca_uint32_t );
        ca_uint32_t maxBytes = ( unsigned ) maxBytesAsALong;
        if ( maxBytes < 0xffffffff - headerSize ) {
            maxBytes += headerSize;
        }
        else {
            maxBytes = 0xffffffff;
        }
        if ( maxBytes < MAX_TCP ) {
            errlogPrintf ( "CAS: EPICS_CA_MAX_ARRAY_BYTES was rounded up to %u\n", MAX_TCP );
            rsrvSizeofLargeBufTCP = MAX_TCP;
        }
        else {
            rsrvSizeofLargeBufTCP = maxBytes;
        }
    }
    freeListInitPvt ( &rsrvLargeBufFreeListTCP, rsrvSizeofLargeBufTCP, 1 );
    ellInit ( &casIntfAddrList );
    ellInit ( &beaconAddrList );
    pCaBucket = bucketCreate(CAS_HASH_TABLE_SIZE);
    if (!pCaBucket)
        cantProceed("RSRV failed to allocate ID lookup table\n");

    addAddrToChannelAccessAddressList ( &casIntfAddrList,
        &EPICS_CAS_INTF_ADDR_LIST, ca_server_port, 0 );
    if (ellCount(&casIntfAddrList) == 0) {
        osiSockAddrNode *pNode = (osiSockAddrNode *) callocMustSucceed( 1, sizeof(*pNode), "rsrv_init" );
        pNode->addr.ia.sin_family = AF_INET;
        pNode->addr.ia.sin_addr.s_addr = htonl ( INADDR_ANY );
        pNode->addr.ia.sin_port = htons ( ca_server_port );
        ellAdd ( &casIntfAddrList, &pNode->node );
    }

    castcp_startStopEvent = epicsEventMustCreate(epicsEventEmpty);
    casudp_startStopEvent = epicsEventMustCreate(epicsEventEmpty);
    beacon_startStopEvent = epicsEventMustCreate(epicsEventEmpty);
    castcp_ctl = ctlPause;

    /* Thread priorites
     * Now starting per interface
     *  TCP Listener: epicsThreadPriorityCAServerLow-2
     *  Name receiver: epicsThreadPriorityCAServerLow-4
     * Now starting global
     *  Beacon sender: epicsThreadPriorityCAServerLow-3
     * Started later per TCP client
     *  TCP receiver: epicsThreadPriorityCAServerLow
     *  TCP sender : epicsThreadPriorityCAServerLow-1
     */
    {
        unsigned i;
        threadPrios[0] = epicsThreadPriorityCAServerLow;

        for(i=1; i<NELEMENTS(threadPrios); i++)
        {
            if(epicsThreadBooleanStatusSuccess!=epicsThreadHighestPriorityLevelBelow(
                        threadPrios[i-1], &threadPrios[i]))
            {
                /* on failure use the lowest known */
                threadPrios[i] = threadPrios[i-1];
            }
        }
    }

    {
        unsigned short sport = ca_server_port;
        socks = rsrv_grap_tcp(&sport);

        if ( sport != ca_server_port ) {
            ca_server_port = sport;
            errlogPrintf ( "cas warning: Configured TCP port was unavailable.\n");
            errlogPrintf ( "cas warning: Using dynamically assigned TCP port %hu,\n",
                ca_server_port );
            errlogPrintf ( "cas warning: but now two or more servers share the same UDP port.\n");
            errlogPrintf ( "cas warning: Depending on your IP kernel this server may not be\n" );
            errlogPrintf ( "cas warning: reachable with UDP unicast (a host's IP in EPICS_CA_ADDR_LIST)\n" );
        }
    }

    /* start servers (TCP and UDP(s) for each interface.
     */
    {
        ELLNODE *cur;
        int i;

        for (i=0, cur=ellFirst(&casIntfAddrList); cur; i++, cur=ellNext(cur))
        {
            char ifaceName[40];
            rsrv_iface_config *conf;

            conf = callocMustSucceed(1, sizeof(*conf), "rsrv_init");

            conf->tcpAddr = ((osiSockAddrNode *)cur)->addr;
            conf->tcpAddr.ia.sin_port = htons(ca_server_port);
            conf->tcp = socks[i];
            socks[i] = INVALID_SOCKET;

            ipAddrToDottedIP (&conf->tcpAddr.ia, ifaceName, sizeof(ifaceName));

            conf->udp = conf->udpbcast = conf->udpbeacon = INVALID_SOCKET;

            /* create and bind UDP beacon socket */

            conf->udpbeacon = epicsSocketCreate(AF_INET, SOCK_DGRAM, 0);
            if(conf->udpbeacon==INVALID_SOCKET)
                cantProceed("rsrv_init ran out of udp sockets for beacon at %s", ifaceName);

            /* beacon sender binds to a random port, and won't actually receive anything */
            conf->udpbeaconRx = conf->tcpAddr;
            conf->udpbeaconRx.ia.sin_port = 0;

            if(tryBind(conf->udpbeacon, &conf->udpbeaconRx, "UDP beacon socket"))
                goto cleanup;


            {
                int intTrue = 1;
                if (setsockopt (conf->udpbeacon, SOL_SOCKET, SO_BROADCAST,
                                (char *)&intTrue, sizeof(intTrue))<0) {
                    errlogPrintf ("CAS: online socket set up error\n");
                    epicsThreadSuspendSelf ();
                }

                /*
                 * this connect is to supress a warning message on Linux
                 * when we shutdown the read side of the socket. If it
                 * fails (and it will on old ip kernels) we just ignore
                 * the failure.
                 */
                osiSockAddr sockAddr;
                sockAddr.ia.sin_family = AF_UNSPEC;
                sockAddr.ia.sin_port = htons ( 0 );
                sockAddr.ia.sin_addr.s_addr = htonl (0);
                connect ( conf->udpbeacon, & sockAddr.sa, sizeof ( sockAddr.sa ) );
                shutdown ( conf->udpbeacon, SHUT_RD );
            }

            /* find interface broadcast address */
            {
                ELLLIST bcastList = ELLLIST_INIT;
                osiSockAddrNode *pNode;

                osiSockDiscoverBroadcastAddresses (&bcastList,
                                                   conf->udpbeacon, &conf->udpbeaconRx); // match addr

                if(ellCount(&bcastList)==0) {
                    cantProceed("Can't find broadcast address of interface %s\n", ifaceName);
                } else if(ellCount(&bcastList)>1 && conf->udpbeaconRx.ia.sin_addr.s_addr!=htonl(INADDR_ANY)) {
                    printf("Interface %s has more than one broadcast address?\n", ifaceName);
                }

                pNode = (osiSockAddrNode*)ellFirst(&bcastList);

                /* beacons are sent to a well known port w/ the iface bcast addr */
                conf->udpbeaconTx = conf->udpbeaconRx;
                conf->udpbeaconTx.ia.sin_addr = pNode->addr.ia.sin_addr;
                conf->udpbeaconTx.ia.sin_port = htons(beacon_port);

                if(connect(conf->udpbeacon, &conf->udpbeaconTx.sa, sizeof(conf->udpbeaconTx))!=0)
                {
                    char sockErrBuf[64], buf[40];
                    epicsSocketConvertErrnoToString (
                        sockErrBuf, sizeof ( sockErrBuf ) );
                    ipAddrToDottedIP (&pNode->addr.ia, buf, sizeof(buf));
                    cantProceed( "%s: CA beacon routing (connect to \"%s\") error was \"%s\"\n",
                        __FILE__, buf, sockErrBuf);
                }

                /* TODO: free bcastList */
            }

            /* create and bind UDP name receiver socket(s) */

            conf->udp = epicsSocketCreate(AF_INET, SOCK_DGRAM, 0);
            if(conf->udp==INVALID_SOCKET)
                cantProceed("rsrv_init ran out of udp sockets");

            conf->udpAddr = conf->tcpAddr;
            conf->udpAddr.ia.sin_port = htons(udp_port);

            epicsSocketEnableAddressUseForDatagramFanout ( conf->udp );

            if(tryBind(conf->udp, &conf->udpAddr, "UDP unicast socket"))
                goto cleanup;

#if !defined(_WIN32)
            /* An oddness of BSD sockets (not winsock) is that binding to
             * INADDR_ANY will receive unicast and broadcast, but binding to
             * a specific interface address receives only unicast.  The trick
             * is to bind a second socket to the interface broadcast address,
             * which will then receive only broadcasts.
             */
            if(conf->udpAddr.ia.sin_addr.s_addr!=htonl(INADDR_ANY)) {

                conf->udpbcast = epicsSocketCreate(AF_INET, SOCK_DGRAM, 0);
                if(conf->udpbcast==INVALID_SOCKET)
                    cantProceed("rsrv_init ran out of udp sockets for bcast");

                conf->udpbcastAddr = conf->udpAddr;
                conf->udpbcastAddr.ia.sin_addr = conf->udpbeaconTx.ia.sin_addr;

                epicsSocketEnableAddressUseForDatagramFanout ( conf->udpbcast );

                if(tryBind(conf->udpbcast, &conf->udpbcastAddr, "UDP Socket bcast"))
                    goto cleanup;
            }

            ellAdd(&servers, &conf->node);

#endif /* !defined(_WIN32) */

            /* have all sockets, time to start some threads */

            epicsThreadMustCreate("CAS-TCP", threadPrios[2],
                    epicsThreadGetStackSize(epicsThreadStackMedium),
                    &req_server, conf);

            epicsEventMustWait(castcp_startStopEvent);

            epicsThreadMustCreate("CAS-UDP", threadPrios[4],
                    epicsThreadGetStackSize(epicsThreadStackMedium),
                    &cast_server, conf);

            epicsEventMustWait(casudp_startStopEvent);

#if !defined(_WIN32)
            if(conf->udpbcast != INVALID_SOCKET) {
                conf->startbcast = 1;

                epicsThreadMustCreate("CAS-UDP2", threadPrios[4],
                        epicsThreadGetStackSize(epicsThreadStackMedium),
                        &cast_server, conf);

                epicsEventMustWait(casudp_startStopEvent);

                conf->startbcast = 0;
            }
#endif /* !defined(_WIN32) */

            continue;
        cleanup:
            epicsSocketDestroy(conf->tcp);
            if(conf->udp!=INVALID_SOCKET) epicsSocketDestroy(conf->udp);
            if(conf->udpbcast!=INVALID_SOCKET) epicsSocketDestroy(conf->udpbcast);
            if(conf->udpbeacon!=INVALID_SOCKET) epicsSocketDestroy(conf->udpbeacon);
            free(conf);
        }
    }

    /* servers list is considered read-only from this point */

    epicsThreadMustCreate("CAS-beacon", threadPrios[3],
            epicsThreadGetStackSize(epicsThreadStackSmall),
            &rsrv_online_notify_task, NULL);

    epicsEventMustWait(beacon_startStopEvent);

    return RSRV_OK;
}

int rsrv_run (void)
{
    castcp_ctl = ctlRun;
    casudp_ctl = ctlRun;
    beacon_ctl = ctlRun;

    return RSRV_OK;
}

int rsrv_pause (void)
{
    beacon_ctl = ctlPause;
    casudp_ctl = ctlPause;
    castcp_ctl = ctlPause;

    return RSRV_OK;
}

static unsigned countChanListBytes (
    struct client *client, ELLLIST * pList )
{
    struct channel_in_use   * pciu;
    unsigned                bytes_reserved = 0;

    epicsMutexMustLock ( client->chanListLock );
    pciu = ( struct channel_in_use * ) pList->node.next;
    while ( pciu ) {
        bytes_reserved += sizeof(struct channel_in_use);
        bytes_reserved += sizeof(struct event_ext)*ellCount( &pciu->eventq );
        bytes_reserved += rsrvSizeOfPutNotify ( pciu->pPutNotify );
        pciu = ( struct channel_in_use * ) ellNext( &pciu->node );
    }
    epicsMutexUnlock ( client->chanListLock );

    return bytes_reserved;
}

static void showChanList (
    struct client * client, unsigned level, ELLLIST * pList )
{
    struct channel_in_use * pciu;
    epicsMutexMustLock ( client->chanListLock );
    pciu = (struct channel_in_use *) pList->node.next;
    while ( pciu ){
        dbChannelShow ( pciu->dbch, level, 8 );
        printf( "          # on eventq=%d, access=%c%c\n",
            ellCount ( &pciu->eventq ),
            asCheckGet ( pciu->asClientPVT ) ? 'r': '-',
            rsrvCheckPut ( pciu ) ? 'w': '-' );
        pciu = ( struct channel_in_use * ) ellNext ( &pciu->node );
    }
    epicsMutexUnlock ( client->chanListLock );
}

/*
 *  log_one_client ()
 */
static void log_one_client (struct client *client, unsigned level)
{
    char                    *pproto;
    double                  send_delay;
    double                  recv_delay;
    char                    *state[] = {"up", "down"};
    epicsTimeStamp          current;
    char                    clientHostName[256];

    ipAddrToDottedIP (&client->addr, clientHostName, sizeof(clientHostName));

    if(client->proto == IPPROTO_UDP){
        pproto = "UDP";
    }
    else if(client->proto == IPPROTO_TCP){
        pproto = "TCP";
    }
    else{
        pproto = "UKN";
    }

    epicsTimeGetCurrent(&current);
    send_delay = epicsTimeDiffInSeconds(&current,&client->time_at_last_send);
    recv_delay = epicsTimeDiffInSeconds(&current,&client->time_at_last_recv);

    printf ( "%s %s(%s): User=\"%s\", V%u.%u, %d Channels, Priority=%u\n",
        pproto,
        clientHostName,
        client->pHostName ? client->pHostName : "",
        client->pUserName ? client->pUserName : "",
        CA_MAJOR_PROTOCOL_REVISION,
        client->minor_version_number,
        ellCount(&client->chanList) +
            ellCount(&client->chanPendingUpdateARList),
        client->priority );
    if ( level >= 1 ) {
        printf ("\tTask Id=%p, Socket FD=%d\n",
            (void *) client->tid, client->sock);
        printf(
        "\tSecs since last send %6.2f, Secs since last receive %6.2f\n",
            send_delay, recv_delay);
        printf(
        "\tUnprocessed request bytes=%u, Undelivered response bytes=%u\n",
            client->recv.cnt - client->recv.stk,
            client->send.stk ); 
        printf(
        "\tState=%s%s%s\n",
            state[client->disconnect?1:0],
            client->send.type == mbtLargeTCP ? " jumbo-send-buf" : "",
            client->recv.type == mbtLargeTCP ? " jumbo-recv-buf" : "");
    }

    if ( level >= 2u ) {
        unsigned bytes_reserved = 0;
        bytes_reserved += sizeof(struct client);
        bytes_reserved += countChanListBytes (
                            client, & client->chanList );
        bytes_reserved += countChanListBytes (
                        client, & client->chanPendingUpdateARList );
        printf( "\t%d bytes allocated\n", bytes_reserved);
        showChanList ( client, level - 2u, & client->chanList );
        showChanList ( client, level - 2u, & client->chanPendingUpdateARList );
    }

    if ( level >= 3u ) {
        printf( "\tSend Lock\n");
        epicsMutexShow(client->lock,1);
        printf( "\tPut Notify Lock\n");
        epicsMutexShow (client->putNotifyLock,1);
        printf( "\tAddress Queue Lock\n");
        epicsMutexShow (client->chanListLock,1);
        printf( "\tEvent Queue Lock\n");
        epicsMutexShow (client->eventqLock,1);
        printf( "\tBlock Semaphore\n");
        epicsEventShow (client->blockSem,1);
    }
}

/*
 *  casr()
 */
void casr (unsigned level)
{
    size_t bytes_reserved;
    struct client *client;

    if ( ! clientQlock ) {
        return;
    }

    printf ("Channel Access Server V%s\n",
        CA_VERSION_STRING ( CA_MINOR_PROTOCOL_REVISION ) );

    LOCK_CLIENTQ
    client = (struct client *) ellNext ( &clientQ.node );
    if (client) {
        printf("Connected circuits:\n");
    }
    else {
        printf("No clients connected.\n");
    }
    while (client) {
        log_one_client(client, level);
        client = (struct client *) ellNext(&client->node);
    }

    if (level>=2) {
        rsrv_iface_config *client = (rsrv_iface_config *) ellFirst ( &servers );
        while (client) {
            char    buf[40];

            printf("Server interface\n");

            ipAddrToDottedIP (&client->tcpAddr.ia, buf, sizeof(buf));
            printf(" TCP listener %s\n", buf);

            ipAddrToDottedIP (&client->udpAddr.ia, buf, sizeof(buf));
            printf(" UDP receiver 1 %s\n", buf);

#if !defined(_WIN32)
            if(client->udpbcast!=INVALID_SOCKET) {
                ipAddrToDottedIP (&client->udpbcastAddr.ia, buf, sizeof(buf));
                printf(" UDP receiver 2 %s\n", buf);
            }
#endif

            ipAddrToDottedIP (&client->udpbeaconRx.ia, buf, sizeof(buf));
            printf(" UDP beacon socket bound %s\n", buf);

            ipAddrToDottedIP (&client->udpbeaconTx.ia, buf, sizeof(buf));
            printf(" UDP beacon destination %s\n", buf);

            client = (rsrv_iface_config *) ellNext(&client->node);
        }
    }
    UNLOCK_CLIENTQ

    if (level>=2u) {
        bytes_reserved = 0u;
        bytes_reserved += sizeof (struct client) *
                    freeListItemsAvail (rsrvClientFreeList);
        bytes_reserved += sizeof (struct channel_in_use) *
                    freeListItemsAvail (rsrvChanFreeList);
        bytes_reserved += sizeof(struct event_ext) *
                    freeListItemsAvail (rsrvEventFreeList);
        bytes_reserved += MAX_TCP *
                    freeListItemsAvail ( rsrvSmallBufFreeListTCP );
        bytes_reserved += rsrvSizeofLargeBufTCP *
                    freeListItemsAvail ( rsrvLargeBufFreeListTCP );
        bytes_reserved += rsrvSizeOfPutNotify ( 0 ) *
                    freeListItemsAvail ( rsrvPutNotifyFreeList );
        printf( "There are currently %u bytes on the server's free list\n",
            (unsigned int) bytes_reserved);
        printf( "%u client(s), %u channel(s), %u event(s) (monitors) %u putNotify(s)\n",
            (unsigned int) freeListItemsAvail ( rsrvClientFreeList ),
            (unsigned int) freeListItemsAvail ( rsrvChanFreeList ),
            (unsigned int) freeListItemsAvail ( rsrvEventFreeList ),
            (unsigned int) freeListItemsAvail ( rsrvPutNotifyFreeList ));
        printf( "%u small buffers (%u bytes ea), and %u jumbo buffers (%u bytes ea)\n",
            (unsigned int) freeListItemsAvail ( rsrvSmallBufFreeListTCP ),
            MAX_TCP,
            (unsigned int) freeListItemsAvail ( rsrvLargeBufFreeListTCP ),
            rsrvSizeofLargeBufTCP );
        printf( "The server's resource id conversion table:\n");
        LOCK_CLIENTQ;
        bucketShow (pCaBucket);
        UNLOCK_CLIENTQ;
        printf ( "The server's array size limit is %u bytes max\n",
            rsrvSizeofLargeBufTCP );

        printChannelAccessAddressList (&beaconAddrList);
    }
}

/*
 * destroy_client ()
 */
void destroy_client ( struct client *client )
{
    if ( ! client ) {
        return;
    }

    if ( client->tid != 0 ) {
        taskwdRemove ( client->tid );
    }

    if ( client->sock != INVALID_SOCKET ) {
        epicsSocketDestroy ( client->sock );
    }

    if ( client->proto == IPPROTO_TCP ) {
        if ( client->send.buf ) {
            if ( client->send.type == mbtSmallTCP ) {
                freeListFree ( rsrvSmallBufFreeListTCP,  client->send.buf );
            }
            else if ( client->send.type == mbtLargeTCP ) {
                freeListFree ( rsrvLargeBufFreeListTCP,  client->send.buf );
            }
            else {
                errlogPrintf ( "CAS: Corrupt send buffer free list type code=%u during client cleanup?\n",
                    client->send.type );
            }
        }
        if ( client->recv.buf ) {
            if ( client->recv.type == mbtSmallTCP ) {
                freeListFree ( rsrvSmallBufFreeListTCP,  client->recv.buf );
            }
            else if ( client->recv.type == mbtLargeTCP ) {
                freeListFree ( rsrvLargeBufFreeListTCP,  client->recv.buf );
            }
            else {
                errlogPrintf ( "CAS: Corrupt recv buffer free list type code=%u during client cleanup?\n",
                    client->send.type );
            }
        }
    }
    else if ( client->proto == IPPROTO_UDP ) {
        if ( client->send.buf ) {
            free ( client->send.buf );
        }
        if ( client->recv.buf ) {
            free ( client->recv.buf );
        }
    }

    if ( client->eventqLock ) {
        epicsMutexDestroy ( client->eventqLock );
    }

    if ( client->chanListLock ) {
        epicsMutexDestroy ( client->chanListLock );
    }

    if ( client->putNotifyLock ) {
        epicsMutexDestroy ( client->putNotifyLock );
    }

    if ( client->lock ) {
        epicsMutexDestroy ( client->lock );
    }

    if ( client->blockSem ) {
        epicsEventDestroy ( client->blockSem );
    }

    if ( client->pUserName ) {
        free ( client->pUserName );
    }

    if ( client->pHostName ) {
        free ( client->pHostName );
    }

    freeListFree ( rsrvClientFreeList, client );
}

static void destroyAllChannels (
    struct client * client, ELLLIST * pList )
{
    if ( !client->chanListLock || !client->eventqLock ) {
        return;
    }

    while ( TRUE ) {
        struct event_ext        *pevext;
        int                     status;
        struct channel_in_use   *pciu;

        epicsMutexMustLock ( client->chanListLock );
        pciu = (struct channel_in_use *) ellGet ( pList );
        epicsMutexUnlock ( client->chanListLock );

        if ( ! pciu ) {
            break;
        }

        while ( TRUE ) {
            /*
            * AS state change could be using this list
            */
            epicsMutexMustLock ( client->eventqLock );
            pevext = (struct event_ext *) ellGet ( &pciu->eventq );
            epicsMutexUnlock ( client->eventqLock );

            if ( ! pevext ) {
                break;
            }

            if ( pevext->pdbev ) {
                db_cancel_event (pevext->pdbev);
            }
            freeListFree (rsrvEventFreeList, pevext);
        }
        rsrvFreePutNotify ( client, pciu->pPutNotify );
        LOCK_CLIENTQ;
        status = bucketRemoveItemUnsignedId ( pCaBucket, &pciu->sid);
        rsrvChannelCount--;
        UNLOCK_CLIENTQ;
        if ( status != S_bucket_success ) {
            errPrintf ( status, __FILE__, __LINE__,
                "Bad id=%d at close", pciu->sid);
        }
        status = asRemoveClient(&pciu->asClientPVT);
        if ( status && status != S_asLib_asNotActive ) {
            printf ( "bad asRemoveClient() status was %x \n", status );
            errPrintf ( status, __FILE__, __LINE__, "asRemoveClient" );
        }

        dbChannelDelete(pciu->dbch);
        freeListFree ( rsrvChanFreeList, pciu );
    }
}

void destroy_tcp_client ( struct client *client )
{
    int                     status;

    if ( CASDEBUG > 0 ) {
        errlogPrintf ( "CAS: Connection %d Terminated\n", client->sock );
    }

    if ( client->evuser ) {
        /*
         * turn off extra labor callbacks from the event thread
         */
        status = db_add_extra_labor_event ( client->evuser, NULL, NULL );
        assert ( ! status );

        /*
         * wait for extra labor in progress to comple
         */
        db_flush_extra_labor_event ( client->evuser );
    }

    destroyAllChannels ( client, & client->chanList );
    destroyAllChannels ( client, & client->chanPendingUpdateARList );

    if ( client->evuser ) {
        db_close_events (client->evuser);
    }

    destroy_client ( client );
}

/*
 * create_client ()
 */
struct client * create_client ( SOCKET sock, int proto )
{
    struct client *client;
    int           spaceAvailOnFreeList;
    size_t        spaceNeeded;

    /*
     * stop further use of server if memory becomes scarse
     */
    spaceAvailOnFreeList =     freeListItemsAvail ( rsrvClientFreeList ) > 0
                            && freeListItemsAvail ( rsrvSmallBufFreeListTCP ) > 0;
    spaceNeeded = sizeof (struct client) + MAX_TCP;
    if ( ! ( osiSufficentSpaceInPool(spaceNeeded) || spaceAvailOnFreeList ) ) {
        epicsSocketDestroy ( sock );
        epicsPrintf ("CAS: no space in pool for a new client (below max block thresh)\n");
        return NULL;
    }

    client = freeListCalloc ( rsrvClientFreeList );
    if ( ! client ) {
        epicsSocketDestroy ( sock );
        epicsPrintf ("CAS: no space in pool for a new client (alloc failed)\n");
        return NULL;
    }

    client->sock = sock;
    client->proto = proto;

    client->blockSem = epicsEventCreate ( epicsEventEmpty );
    client->lock = epicsMutexCreate();
    client->putNotifyLock = epicsMutexCreate();
    client->chanListLock = epicsMutexCreate();
    client->eventqLock = epicsMutexCreate();
    if ( ! client->blockSem || ! client->lock || ! client->putNotifyLock ||
        ! client->chanListLock || ! client->eventqLock ) {
        destroy_client ( client );
        return NULL;
    }

    client->pUserName = NULL;
    client->pHostName = NULL;
    ellInit ( & client->chanList );
    ellInit ( & client->chanPendingUpdateARList );
    ellInit ( & client->putNotifyQue );
    memset ( (char *)&client->addr, 0, sizeof (client->addr) );
    client->tid = 0;

    if ( proto == IPPROTO_TCP ) {
        client->send.buf = (char *) freeListCalloc ( rsrvSmallBufFreeListTCP );
        client->send.maxstk = MAX_TCP;
        client->send.type = mbtSmallTCP;
        client->recv.buf =  (char *) freeListCalloc ( rsrvSmallBufFreeListTCP );
        client->recv.maxstk = MAX_TCP;
        client->recv.type = mbtSmallTCP;
    }
    else if ( proto == IPPROTO_UDP ) {
        client->send.buf = malloc ( MAX_UDP_SEND );
        client->send.maxstk = MAX_UDP_SEND;
        client->send.type = mbtUDP;
        client->recv.buf = malloc ( MAX_UDP_RECV );
        client->recv.maxstk = MAX_UDP_RECV;
        client->recv.type = mbtUDP;
    }
    if ( ! client->send.buf || ! client->recv.buf ) {
        destroy_client ( client );
        return NULL;
    }
    client->send.stk = 0u;
    client->send.cnt = 0u;
    client->recv.stk = 0u;
    client->recv.cnt = 0u;
    client->evuser = NULL;
    client->priority = CA_PROTO_PRIORITY_MIN;
    client->disconnect = FALSE;
    epicsTimeGetCurrent ( &client->time_at_last_send );
    epicsTimeGetCurrent ( &client->time_at_last_recv );
    client->minor_version_number = CA_UKN_MINOR_VERSION;
    client->recvBytesToDrain = 0u;

    return client;
}

void casAttachThreadToClient ( struct client *pClient )
{
    epicsSignalInstallSigAlarmIgnore ();
    epicsSignalInstallSigPipeIgnore ();
    pClient->tid = epicsThreadGetIdSelf ();
    epicsThreadPrivateSet ( rsrvCurrentClient, pClient );
    taskwdInsert ( pClient->tid, NULL, NULL );
}

void casExpandSendBuffer ( struct client *pClient, ca_uint32_t size )
{
    if ( pClient->send.type == mbtSmallTCP && rsrvSizeofLargeBufTCP > MAX_TCP
            && size <= rsrvSizeofLargeBufTCP ) {
        int spaceAvailOnFreeList = freeListItemsAvail ( rsrvLargeBufFreeListTCP ) > 0;
        if ( osiSufficentSpaceInPool(rsrvSizeofLargeBufTCP) || spaceAvailOnFreeList ) {
            char *pNewBuf = ( char * ) freeListCalloc ( rsrvLargeBufFreeListTCP );
            if ( pNewBuf ) {
                memcpy ( pNewBuf, pClient->send.buf, pClient->send.stk );
                freeListFree ( rsrvSmallBufFreeListTCP,  pClient->send.buf );
                pClient->send.buf = pNewBuf;
                pClient->send.maxstk = rsrvSizeofLargeBufTCP;
                pClient->send.type = mbtLargeTCP;
            }
        }
    }
}

void casExpandRecvBuffer ( struct client *pClient, ca_uint32_t size )
{
    if ( pClient->recv.type == mbtSmallTCP && rsrvSizeofLargeBufTCP > MAX_TCP
            && size <= rsrvSizeofLargeBufTCP) {
        int spaceAvailOnFreeList = freeListItemsAvail ( rsrvLargeBufFreeListTCP ) > 0;
        if ( osiSufficentSpaceInPool(rsrvSizeofLargeBufTCP) || spaceAvailOnFreeList ) {
            char *pNewBuf = ( char * ) freeListCalloc ( rsrvLargeBufFreeListTCP );
            if ( pNewBuf ) {
                assert ( pClient->recv.cnt >= pClient->recv.stk );
                memcpy ( pNewBuf, &pClient->recv.buf[pClient->recv.stk], pClient->recv.cnt - pClient->recv.stk );
                freeListFree ( rsrvSmallBufFreeListTCP,  pClient->recv.buf );
                pClient->recv.buf = pNewBuf;
                pClient->recv.cnt = pClient->recv.cnt - pClient->recv.stk;
                pClient->recv.stk = 0u;
                pClient->recv.maxstk = rsrvSizeofLargeBufTCP;
                pClient->recv.type = mbtLargeTCP;
            }
        }
    }
}

/*
 *  create_tcp_client ()
 */
struct client *create_tcp_client ( SOCKET sock )
{
    int                     status;
    struct client           *client;
    int                     intTrue = TRUE;
    osiSocklen_t            addrSize;
    unsigned                priorityOfEvents;

    /* socket passed in is destroyed here if unsuccessful */
    client = create_client ( sock, IPPROTO_TCP );
    if ( ! client ) {
        return NULL;
    }

    /*
     * see TCP(4P) this seems to make unsolicited single events much
     * faster. I take care of queue up as load increases.
     */
    status = setsockopt ( sock, IPPROTO_TCP, TCP_NODELAY,
                (char *) &intTrue, sizeof (intTrue) );
    if (status < 0) {
        errlogPrintf ( "CAS: TCP_NODELAY option set failed\n" );
        destroy_client ( client );
        return NULL;
    }

    /*
     * turn on KEEPALIVE so if the client crashes
     * this task will find out and exit
     */
    status = setsockopt ( sock, SOL_SOCKET, SO_KEEPALIVE,
                    (char *) &intTrue, sizeof (intTrue) );
    if ( status < 0 ) {
        errlogPrintf ( "CAS: SO_KEEPALIVE option set failed\n" );
        destroy_client ( client );
        return NULL;
    }

    /*
     * some concern that vxWorks will run out of mBuf's
     * if this change is made
     *
     * joh 11-10-98
     */
#if 0
    /*
     * set TCP buffer sizes to be synergistic
     * with CA internal buffering
     */
    i = MAX_MSG_SIZE;
    status = setsockopt ( sock, SOL_SOCKET, SO_SNDBUF, (char *) &i, sizeof (i) );
    if (status < 0) {
        errlogPrintf ( "CAS: SO_SNDBUF set failed\n" );
        destroy_client ( client );
        return NULL;
    }
    i = MAX_MSG_SIZE;
    status = setsockopt ( sock, SOL_SOCKET, SO_RCVBUF, (char *) &i, sizeof (i) );
    if (status < 0) {
        errlogPrintf ( "CAS: SO_RCVBUF set failed\n" );
        destroy_client ( client );
        return NULL;
    }
#endif

    addrSize = sizeof ( client->addr );
    status = getpeername ( sock, (struct sockaddr *)&client->addr,
                    &addrSize );
    if ( status < 0 ) {
        epicsPrintf ("CAS: peer address fetch failed\n");
        destroy_tcp_client (client);
        return NULL;
    }

    client->evuser = (struct event_user *) db_init_events ();
    if ( ! client->evuser ) {
        errlogPrintf ("CAS: unable to init the event facility\n");
        destroy_tcp_client (client);
        return NULL;
    }

    status = db_add_extra_labor_event ( client->evuser, rsrv_extra_labor, client );
    if (status != DB_EVENT_OK) {
        errlogPrintf("CAS: unable to setup the event facility\n");
        destroy_tcp_client (client);
        return NULL;
    }

    {
        epicsThreadBooleanStatus    tbs;

        tbs  = epicsThreadHighestPriorityLevelBelow ( epicsThreadPriorityCAServerLow, &priorityOfEvents );
        if ( tbs != epicsThreadBooleanStatusSuccess ) {
            priorityOfEvents = epicsThreadPriorityCAServerLow;
        }
    }

    status = db_start_events ( client->evuser, "CAS-event",
                NULL, NULL, priorityOfEvents );
    if ( status != DB_EVENT_OK ) {
        errlogPrintf ( "CAS: unable to start the event facility\n" );
        destroy_tcp_client ( client );
        return NULL;
    }

    /*
     * add first version message should it be needed
     */
    rsrv_version_reply ( client );

    if ( CASDEBUG > 0 ) {
        char buf[64];
        ipAddrToDottedIP ( &client->addr, buf, sizeof(buf) );
        errlogPrintf ( "CAS: conn req from %s\n", buf );
    }

    return client;
}

void casStatsFetch ( unsigned *pChanCount, unsigned *pCircuitCount )
{
    LOCK_CLIENTQ;
    {
        int circuitCount = ellCount ( &clientQ );
        if ( circuitCount < 0 ) {
            *pCircuitCount = 0;
        }
        else {
            *pCircuitCount = (unsigned) circuitCount;
        }
        *pChanCount = rsrvChannelCount;
    }
    UNLOCK_CLIENTQ;
}
