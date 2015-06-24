/** <!--
 *
 *  Copyright (C) 2014 OpenVCX openvcx@gmail.com
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Affero General Public License as
 *  published by the Free Software Foundation, either version 3 of the
 *  License, or (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Affero General Public License for more details.
 *
 *  You should have received a copy of the GNU Affero General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  If you would like this software to be made available to you under an 
 *  alternate license please email openvcx@gmail.com for more information.
 *
 * -->
 */


#include "vsx_common.h"


typedef void (*THREAD_FUNC_WRAP) (void *);

typedef struct THREAD_FUNC_WRAPPER_ARG {
  THREAD_FUNC_WRAP thread_func;
  POOL_T *pConnPool;
  CLIENT_CONN_T *pConn;
  //pthread_cond_t *pcond;
  int flags;
  char                        tid_tag[LOGUTIL_TAG_LENGTH];
} THREAD_FUNC_WRAPPER_ARG_T;

void thread_func_wrapper(void *pArg) {
  THREAD_FUNC_WRAPPER_ARG_T wrap;
  int rc = 0;

  memcpy(&wrap, pArg, sizeof(THREAD_FUNC_WRAPPER_ARG_T));
  ((THREAD_FUNC_WRAPPER_ARG_T *)pArg)->flags = 0;

  logutil_tid_add(pthread_self(), wrap.tid_tag);

  //pthread_cond_broadcast(((THREAD_FUNC_WRAPPER_ARG_T *) pArg)->pcond);

  //fprintf(stderr, "%u THREAD_FUNC pConn:0x%x inuse:%d\n", pthread_self(), wrap.pConn, wrap.pConn->pool.inuse);

  //
  // Handle any SSL handshaking on the connection thread, to avoid blocking the accept loop
  //
  if((wrap.pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS)) {

    if((rc = netio_acceptssl(&wrap.pConn->sd.netsocket)) < 0) {
      LOG(X_ERROR("Closing non-SSL connection on port %d from %s:%d"), 
        htons(wrap.pConn->pListenCfg->sain.sin_port), inet_ntoa(wrap.pConn->sd.sain.sin_addr), 
        htons(wrap.pConn->sd.sain.sin_port));
    }
  }

  if(rc == 0) {
    //
    // Call the wrapper's thread procedure
    //
    wrap.thread_func(wrap.pConn);
  }

  //fprintf(stderr, "%d THREAD_FUNC DONE pConn:0x%x inuse:%d\n", pthread_self(), wrap.pConn, wrap.pConn->pool.inuse);

  netio_closesocket(&wrap.pConn->sd.netsocket);
  pool_return(wrap.pConnPool, &wrap.pConn->pool);

  logutil_tid_remove(pthread_self());

  //fprintf(stderr, "%u THREAD_FUNC DONE pConn:0x%x inuse:%d\n", pthread_self(), wrap.pConn, wrap.pConn->pool.inuse);

}

int srvlisten_loop(SRV_LISTENER_CFG_T *pListenCfg, void *thread_func) {

  int sainlen;
  THREAD_FUNC_WRAPPER_ARG_T wrapArg;
  CLIENT_CONN_T *pConn;
  SOCKET_DESCR_T sdclient;
  //struct sockaddr_in saSrv;
  int rc = -1;
  const char *s;
  //pthread_cond_t cond;
  pthread_mutex_t mtx;
  TIME_VAL tv0, tv1;

#if defined(__APPLE__) 
  int sockopt = 0;
#endif // __APPLE__

  if(!pListenCfg || !pListenCfg->pnetsockSrv || !pListenCfg->pConnPool || !thread_func || 
     pListenCfg->pConnPool->numElements <= 0) {
    return -1;
  }

  //memset(&saSrv, 0, sizeof(saSrv));
  memset(&sdclient.netsocket, 0, sizeof(sdclient.netsocket));
  //sainlen = sizeof(saSrv);

  //if(getsockname(pListenCfg->pnetsockSrv->sock, (struct sockaddr *) &saSrv,  (socklen_t *) &sainlen) != 0) {
  //  LOG(X_ERROR("getsockopt failed on server socket"));
  //}

  pthread_mutex_init(&mtx, NULL);
  //pthread_cond_init(&cond, NULL);

  while(!g_proc_exit) {

    sainlen = sizeof(sdclient.sain);
    if((NETIOSOCK_FD(sdclient.netsocket)= accept(PNETIOSOCK_FD(pListenCfg->pnetsockSrv), (struct sockaddr *) &sdclient.sain, 
                                         (socklen_t *) &sainlen)) == INVALID_SOCKET) {
      if(!g_proc_exit) {
        LOG(X_ERROR("%saccept failed on %s:%d"), 
             ((pListenCfg->pnetsockSrv->flags & NETIO_FLAG_SSL_TLS) ? "SSL " : ""),
                 //inet_ntoa(saSrv.sin_addr), ntohs(saSrv.sin_port));
                 inet_ntoa(pListenCfg->sain.sin_addr), ntohs(pListenCfg->sain.sin_port));
      }
      break;
    }

    if(g_proc_exit) {
      break;
    }

    sdclient.netsocket.flags = pListenCfg->pnetsockSrv->flags;

    //
    // Find an available client thread to process the client request
    //
    if((pConn = (CLIENT_CONN_T *) pool_get(pListenCfg->pConnPool)) == NULL) {
      LOG(X_WARNING("No available connection for %s:%d (max:%d) on port %d"), 
           inet_ntoa(sdclient.sain.sin_addr), ntohs(sdclient.sain.sin_port), 
           pListenCfg->pConnPool->numElements, ntohs(pListenCfg->sain.sin_port));

      netio_closesocket(&sdclient.netsocket);
      continue;
    }

#if defined(__APPLE__) 
    sockopt = 1;
    if(setsockopt(NETIOSOCK_FD(sdclient.netsocket), SOL_SOCKET, SO_NOSIGPIPE,
                 (char*) &sockopt, sizeof(sockopt)) != 0) {
      LOG(X_ERROR("Failed to set SO_NOSIGPIPE"));
    }
#endif // __APPLE__

    //pConn->psrvsaListen = &saSrv;
    //memcpy(&pConn->srvsaListen, &saSrv, sizeof(pConn->srvsaListen));

    LOG(X_DEBUG("Accepted connection on port %d from %s:%d"), 
        htons(pListenCfg->sain.sin_port), inet_ntoa(sdclient.sain.sin_addr), 
        htons(sdclient.sain.sin_port));

    pthread_attr_init(&pConn->attr);
    pthread_attr_setdetachstate(&pConn->attr, PTHREAD_CREATE_DETACHED);
    memset(&pConn->sd.netsocket, 0, sizeof(pConn->sd.netsocket));
    NETIO_SET(pConn->sd.netsocket, sdclient.netsocket);
    memcpy(&pConn->sd.sain, &sdclient.sain, sizeof(struct sockaddr_in));
    pConn->pListenCfg = pListenCfg;
    NETIOSOCK_FD(sdclient.netsocket) = INVALID_SOCKET;
    //pConn->nonce[0] = '\0';
    //pConn->opaque[0] = '\0';

    wrapArg.thread_func = thread_func;
    wrapArg.pConnPool = pListenCfg->pConnPool;
    wrapArg.pConn = pConn;
    wrapArg.flags = 1;
    wrapArg.tid_tag[0] = '\0';
    if((s = logutil_tid_lookup(pthread_self(), 0)) && s[0] != '\0') {
      snprintf(wrapArg.tid_tag, sizeof(wrapArg.tid_tag), "%s-%u", s, pConn->pool.id);
    }
    //wrapArg.pcond = &cond;

  //fprintf(stderr, "%d CALLING wrap: 0x%x pConn:0x%x\n", pthread_self(), &wrapArg, wrapArg.pConn);

    if(pthread_create(&pConn->ptd,
                    &pConn->attr,
                    (void *) thread_func_wrapper,
                    (void *) &wrapArg) != 0) {
      LOG(X_ERROR("Unable to create connection handler thread on port %d from %s:%d"), 
          htons(pListenCfg->sain.sin_port), inet_ntoa(sdclient.sain.sin_addr), 
          htons(sdclient.sain.sin_port));
      netio_closesocket(&pConn->sd.netsocket);
      pool_return(pListenCfg->pConnPool, &pConn->pool);
      wrapArg.flags = 0;
      //pthread_cond_broadcast(&cond);
      break;
    }

    pthread_attr_destroy(&pConn->attr);

    //
    // be careful not to reuse the same wrapArg instance 
    // since the stack variable arguments could get
    // overwritten by the next loop iteration, before the thread proc is 
    // invoked
    //
    //fprintf(stderr, "wait start\n");
    tv0 = timer_GetTime();
    //if(wrapArg.flags == 1) {

      //
      // It seems that calling pthread_cond_wait here to check if the thread creation is
      // complete is not completely reliable and portable, so we do the lame way 
      // of sleeping and polling.
      //
      //pthread_cond_wait(&cond, &mtx);

      while(wrapArg.flags == 1) {
        usleep(100);
        if(((tv1 = timer_GetTime()) - tv0) / TIME_VAL_MS > 1000) {
          LOG(X_WARNING("Abandoning wait for connection thread start on port %d from %s:%d"),
               htons(pListenCfg->sain.sin_port),
              inet_ntoa(pListenCfg->sain.sin_addr), ntohs(pListenCfg->sain.sin_port));
          break;
        } 
      }
      //fprintf(stderr, "THREAD STARTED AFTER %lld ns\n", (timer_GetTime() - tv0));
    //}

    //fprintf(stderr, "wait done\n");

    //while(wrapArg.flag == 1) {
    //  usleep(10000); 
    //}

  }

  //pthread_cond_destroy(&cond);
  pthread_mutex_destroy(&mtx);

  return rc;
}
