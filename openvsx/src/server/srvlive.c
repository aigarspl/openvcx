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


#define PREPROCESS_FILE_LEN_MAX      8192
#define DEFAULT_EMBED_WIDTH           720
#define DEFAULT_EMBED_HEIGHT          480
#define MIN_EMBED_WIDTH               320
#define MIN_EMBED_HEIGHT              240

#define GET_STREAMER_FROM_CONN(pc)  ((pc)->pStreamerCfg0 ? (pc)->pStreamerCfg0 : (pc)->pStreamerCfg1)

void srv_lock_conn_mutexes(CLIENT_CONN_T *pConn, int lock) {
  if(lock) {
    if(pConn->pMtx0) {
      pthread_mutex_lock(pConn->pMtx0);
    }
    if(pConn->pMtx1) {
      pthread_mutex_lock(pConn->pMtx1);
    }
  } else {
    if(pConn->pMtx0) {
      pthread_mutex_unlock(pConn->pMtx0);
    }
    if(pConn->pMtx1) {
      pthread_mutex_unlock(pConn->pMtx1);
    }
  }
}

static int http_check_pass(const CLIENT_CONN_T *pConn) {
  const char *parg;
  char tmp[128];
  int rc = 0;

  if(pConn->pCfg->livepwd &&
     (!(parg = conf_find_keyval((const KEYVAL_PAIR_T *) &pConn->phttpReq->uriPairs, "pass")) ||
       strcmp(pConn->pCfg->livepwd, parg))) {

    LOG(X_WARNING("Invalid password for :%d%s from %s:%d (%d char given)"),
         ntohs(INET_PORT(pConn->pListenCfg->sa)), pConn->phttpReq->url,
         FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)),
         parg ? strlen(parg) : 0);

    rc = -1;
  }

  return rc;
}

static int http_sub_htmlvars(const char *pIn, unsigned int lenIn, char *pOut, 
                             unsigned int lenOut, const char *pSubs[]) {
  unsigned int idxSub = 0;
  unsigned int idxInSub;
  unsigned int idxIn = 1;  
  unsigned int idxOut = 1;  

  if(!pIn || !pOut || !pSubs) {
    return -1;
  }

  //
  // Substition string is marked by '$$...$$'
  //

  pOut[0] = pIn[0];

  while(idxIn < lenIn) {

    if(idxOut >= lenOut) {
      return -1;
    }

    if(pIn[idxIn] == '$' && pIn[idxIn - 1] == '$') {

      if(!pSubs[idxSub]) {
        LOG(X_ERROR("Substition argument missing for index[%d]"), idxSub);
        return -1;
      }

      idxOut--;
      idxInSub = 0;
      while(pSubs[idxSub][idxInSub] != '\0') { 
        if(idxOut >= lenOut) {
          return -1;
        }
        pOut[idxOut++] = pSubs[idxSub][idxInSub++];
      }
      idxSub++;

      idxIn+=2;
      while(idxIn < lenIn && !(pIn[idxIn] == '$' && pIn[idxIn -1] == '$')) {
        idxIn++;
      }
    } else {
      pOut[idxOut++] = pIn[idxIn];
    }

    idxIn++;
  }

  pOut[idxOut] = '\0';

  return idxOut;
}

static int update_indexfile(const char *path, char *pOut, unsigned int lenOut, const char *pSubs[]) {
  int rc = 0;
  FILE_OFFSET_T idx = 0;
  FILE_STREAM_T fileStream;
  unsigned int lenread;
  char buf[PREPROCESS_FILE_LEN_MAX];

  if(OpenMediaReadOnly(&fileStream, path) != 0) {
    return -1;
  }

  if(fileStream.size > sizeof(buf)) {
    LOG(X_ERROR("Unable to pre-process file %s size %d exceeds limitation"), path, sizeof(buf));
    rc = -1;
  } else {

    while(idx < fileStream.size && idx < sizeof(buf)) {

      if((lenread = (unsigned int) (fileStream.size - idx)) > sizeof(buf) - idx) {
        lenread = sizeof(buf);
      }

      if(ReadFileStream(&fileStream, &buf[idx], lenread) != lenread) {
        rc = -1;
        break;
      }
      idx += lenread;
    }
  }

  CloseMediaFile(&fileStream);

  if(rc == 0) {
    if((rc = http_sub_htmlvars(buf, idx, pOut, lenOut, pSubs)) < 0) {
      LOG(X_ERROR("Failed to pre-process file %s"), path);
    }
  }

  return rc;
}
#define VID_WIDTH_HEIGHT_STR_SZ  32

static void get_width_height(const STREAMER_STATUS_T *pStatus, 
                             unsigned int outidx, 
                             char *strwidth, 
                             char *strheight) {

  int width, height;
  float aspectr;

//TODO: get width when retrieving dynamic / sdp catpure.. post sdp update via pStreamerCfg->status;
//TODO: this should be common code that goes in xcode/...
  if(pStatus && (width = pStatus->vidProps[outidx].resH) > 0 && (height = pStatus->vidProps[outidx].resV) > 0) {

    if(width < MIN_EMBED_WIDTH || height < MIN_EMBED_HEIGHT) {

      aspectr = (float) width / height;

      if(width < MIN_EMBED_WIDTH) {
        width = MIN_EMBED_WIDTH;
        height = ((float)width / aspectr);
      }
      if(height < MIN_EMBED_HEIGHT) {
        height = MIN_EMBED_HEIGHT;
        width = (float) height * aspectr; 
      }

      if(width & 0x01) {
        width++;
      }
      if(height & 0x01) {
        height++;
      }
    }

    snprintf(strwidth, VID_WIDTH_HEIGHT_STR_SZ, "%d", width);
    snprintf(strheight, VID_WIDTH_HEIGHT_STR_SZ, "%d", height);

  } else {

    snprintf(strwidth, VID_WIDTH_HEIGHT_STR_SZ, "%d", DEFAULT_EMBED_WIDTH);
    snprintf(strheight, VID_WIDTH_HEIGHT_STR_SZ, "%d", DEFAULT_EMBED_HEIGHT);

  }
}

static int get_output_idx(const CLIENT_CONN_T *pConn, 
                          const char *url, 
                          char *stroutidx, 
                          unsigned int *poutidx) {
  int rc = 0;
  int ioutidx;
  char tmp[128];
  const STREAMER_CFG_T *pStreamerCfg = NULL;

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  //
  // Obtain the requested xcode output outidx from the HTTP URI such as '/rtmp/2'
  //
  if((ioutidx = outfmt_getoutidx(pConn->phttpReq->puri, NULL)) < 0 ||
     ioutidx >= IXCODE_VIDEO_OUT_MAX) {
    ioutidx = 0;
  } else if(ioutidx > 0) {

    if(!pStreamerCfg || !pStreamerCfg->xcode.vid.out[ioutidx].active) {
      LOG(X_ERROR("Output format index[%d] not active"), ioutidx);
      rc = -1;
    } else {
      if(stroutidx) {
        sprintf(stroutidx, "/%d", ioutidx + 1);
      }
      LOG(X_DEBUG("Set %s index file output format index to[%d] url:'%s', for %s:%d"), url, ioutidx,
        pConn->phttpReq->puri, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    }
  }
  *poutidx = ioutidx;

  return rc;
}

static int update_proto_indexfile(CLIENT_CONN_T *pConn, 
                                 STREAMER_CFG_T *pStreamerCfg, 
                                 const char *idxFile, 
                                 const char *rsrcurl, 
                                 unsigned int outidx, 
                                 const char *subs[],
                                 char *strwidth,
                                 char *strheight,
                                 char *pOut, 
                                 unsigned int lenOut) {
  int rc = 0;
  char path[VSX_MAX_PATH_LEN];
  struct stat st;

  if(mediadb_prepend_dir(pConn->pCfg->pMediaDb->homeDir, idxFile, path, sizeof(path)) != 0) {
    return -1;
  } else if(fileops_stat(path, &st) != 0) {
    LOG(X_ERROR("Unable to stat index file '%s'"), path);
    return -1;
  }

  //if(pStreamerCfg && pStreamerCfg->action.liveFmts.out[STREAMER_OUTFMT_IDX_RTMP].do_outfmt) {
  if(strwidth && strheight) {

    strwidth[0] = '\0';
    strheight[0] = '\0';
    get_width_height(pStreamerCfg ? &pStreamerCfg->status : NULL, outidx, strwidth, strheight);
  }
  rc = update_indexfile(path, pOut, lenOut, subs);

  return rc;
}

typedef int (* CB_UPDATE_PROTO_INDEXFILE) (CLIENT_CONN_T *, STREAMER_CFG_T *, const char *, \
                                            unsigned int, char *pOut, unsigned int);

static int update_rtmp_indexfile(CLIENT_CONN_T *pConn, 
                                 STREAMER_CFG_T *pStreamerCfg, 
                                 const char *rsrcUrl, 
                                 unsigned int outidx, 
                                 char *pOut, 
                                 unsigned int lenOut) {

  char strwidth[VID_WIDTH_HEIGHT_STR_SZ];
  char strheight[VID_WIDTH_HEIGHT_STR_SZ];
  const char *subs[4];

  subs[0] = strwidth;
  subs[1] = strheight;
  subs[2] = rsrcUrl;
  subs[3] = NULL;

  return update_proto_indexfile(pConn, pStreamerCfg, VSX_RTMP_INDEX_HTML, rsrcUrl, outidx,
                  subs, strwidth, strheight, pOut, lenOut);
}

static int update_flv_indexfile(CLIENT_CONN_T *pConn, 
                                 STREAMER_CFG_T *pStreamerCfg, 
                                 const char *rsrcUrl, 
                                 unsigned int outidx, 
                                 char *pOut, 
                                 unsigned int lenOut) {

  char strwidth[VID_WIDTH_HEIGHT_STR_SZ];
  char strheight[VID_WIDTH_HEIGHT_STR_SZ];
  const char *subs[4];

  subs[0] = strwidth;
  subs[1] = strheight;
  subs[2] = rsrcUrl;
  subs[3] = NULL;

  return update_proto_indexfile(pConn, pStreamerCfg, VSX_FLASH_HTTP_INDEX_HTML, rsrcUrl, outidx,
                  subs, strwidth, strheight, pOut, lenOut);
}

static int update_mkv_indexfile(CLIENT_CONN_T *pConn, 
                                STREAMER_CFG_T *pStreamerCfg,
                                const char *rsrcUrl, 
                                unsigned int outidx, 
                                char *pOut, 
                                unsigned int lenOut) {

  char strwidth[VID_WIDTH_HEIGHT_STR_SZ];
  char strheight[VID_WIDTH_HEIGHT_STR_SZ];
  const char *subs[7];

  subs[0] = strwidth;
  subs[1] = strheight;
  subs[2] = strwidth;
  subs[3] = strheight;
  subs[4] = rsrcUrl;
  subs[5] = CONTENT_TYPE_WEBM;
  subs[6] = NULL;

  return update_proto_indexfile(pConn, pStreamerCfg, VSX_MKV_INDEX_HTML, rsrcUrl, outidx,
                  subs, strwidth, strheight, pOut, lenOut);
}

static int update_rtsp_indexfile(CLIENT_CONN_T *pConn, 
                                 STREAMER_CFG_T *pStreamerCfg,
                                 const char *rsrcUrl, 
                                 unsigned int outidx,
                                 char *pOut, 
                                 unsigned int lenOut) {
  const char *subs[4];

  subs[0] = rsrcUrl;
  subs[1] = rsrcUrl;
  subs[2] = rsrcUrl;
  subs[3] = NULL;

  return update_proto_indexfile(pConn, NULL, VSX_RTSP_INDEX_HTML, rsrcUrl, 0,
                  subs, NULL, NULL, pOut, lenOut);
}


static int update_httplive_indexfile(CLIENT_CONN_T *pConn, 
                                     STREAMER_CFG_T *pStreamerCfg,
                                     const char *rsrcUrl, 
                                     unsigned int outidx, 
                                     char *pOut, 
                                     unsigned int lenOut) {

  char strwidth[VID_WIDTH_HEIGHT_STR_SZ];
  char strheight[VID_WIDTH_HEIGHT_STR_SZ];
  const char *subs[4];

  subs[0] = strwidth;
  subs[1] = strheight;
  subs[2] = rsrcUrl;
  subs[3] = NULL;

  return update_proto_indexfile(pConn, pStreamerCfg, VSX_HTTPLIVE_INDEX_HTML, rsrcUrl, outidx,
                  subs, strwidth, strheight, pOut, lenOut);
}


static int update_dash_indexfile(CLIENT_CONN_T *pConn, 
                                 STREAMER_CFG_T *pStreamerCfg,
                                 const char *rsrcUrl, 
                                 unsigned int outidx, 
                                 char *pOut, 
                                 unsigned int lenOut) {

  char strwidth[VID_WIDTH_HEIGHT_STR_SZ];
  char strheight[VID_WIDTH_HEIGHT_STR_SZ];
  const char *subs[4];

  subs[0] = strwidth;
  subs[1] = strheight;
  subs[2] = rsrcUrl;
  subs[3] = NULL;

  return update_proto_indexfile(pConn, pStreamerCfg, VSX_DASH_INDEX_HTML, rsrcUrl, outidx,
                  subs, strwidth, strheight, pOut, lenOut);
}

static int get_token_uriparam(char *tokenstr, unsigned int sztokenstr, const CLIENT_CONN_T *pConn) {
  int rc = 0;

  rc = srv_write_authtoken(tokenstr, sztokenstr, pConn->pListenCfg->pAuthTokenId, 
                conf_find_keyval((const KEYVAL_PAIR_T *) &pConn->phttpReq->uriPairs, VSX_URI_TOKEN_QUERY_PARAM), 1);
  return rc;
}

const SRV_LISTENER_CFG_T *srv_ctrl_findlistener(const SRV_LISTENER_CFG_T *arrCfgs,
                                                unsigned int maxCfgs, enum URL_CAPABILITY urlCap,
                                                int ssl, int preferredPort) {
  unsigned int idx;
  const SRV_LISTENER_CFG_T *pListener = NULL;

  if(arrCfgs) {

    for(idx = 0; idx < maxCfgs; idx++) {

      VSX_DEBUG_LIVE( 
        LOG(X_DEBUGV("LIVE - srv_ctrl_findlistener search idx[%d]/%d: 0x%x, active: %d, search-ssl: %d "
               " netflags: 0x%x, urlCap: 0x%x & urlCap: 0x%x, listen.port: %d == preferred: %d"), 
          idx, maxCfgs, &arrCfgs[idx], arrCfgs[idx].active, ssl, arrCfgs[idx].netflags, 
          arrCfgs[idx].urlCapabilities, urlCap, htons(INET_PORT(arrCfgs[idx].sa)), preferredPort); );

      //
      // Find by matching by URL_CAP
      //
      if(arrCfgs[idx].active && (arrCfgs[idx].urlCapabilities & urlCap)) {

        //if(netflagsMask == 0 || (arrCfgs[idx].netflags & netflagsMask) == netflags) {
        if(ssl == -1 || (ssl && (arrCfgs[idx].netflags & NETIO_FLAG_SSL_TLS)) ||
                        (!ssl && ((arrCfgs[idx].netflags & NETIO_FLAG_PLAINTEXT) || 
                                  !(arrCfgs[idx].netflags & NETIO_FLAG_SSL_TLS)))) {

          if(preferredPort == 0 || preferredPort == htons(INET_PORT(arrCfgs[idx].sa))) {
            VSX_DEBUG_LIVE( LOG(X_DEBUGV("LIVE - srv_ctrl_findlistener %smatch"), pListener ? "better " : "" ); );
            pListener = &arrCfgs[idx];
            if(preferredPort == 0) { 
              break;
            }
          }

        }

      }
    }

  }

  return pListener;
}

static const SRV_LISTENER_CFG_T *findlistener(CLIENT_CONN_T *pConn,
                                              enum URL_CAPABILITY urlCap,
                                              int preferredPort,
                                              int *pismedialinkssl) {
  const SRV_LISTENER_CFG_T *pListener = NULL;
  NETIO_FLAG_T ssl = 0;

  ssl = (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? 1 : 0;
  //VSX_DEBUG_LIVE( LOG(X_DEBUG("LIVE - findlistener conn-netflags: 0x%x, ssl: %d"), pConn->pListenCfg->netflags, ssl); );

  //
  // Try to find an SSL/TLS listener first
  //
  if(!(pListener = srv_ctrl_findlistener(pConn->pCfg->pListenMedia, SRV_LISTENER_MAX, 
                                  urlCap, ssl, preferredPort))) {

    ssl = !ssl;
    pListener = srv_ctrl_findlistener(pConn->pCfg->pListenMedia, SRV_LISTENER_MAX, 
                                      urlCap, ssl, preferredPort);
  }

  if(pismedialinkssl) {
    *pismedialinkssl = ssl;
  }

  return pListener;
}


static int resp_index_file(CLIENT_CONN_T *pConn, 
                           const char *pargfile, 
                           int is_remoteargfile,
                           const SRV_LISTENER_CFG_T *pListenCfg,
                           int urlCap,
                           char *purloutbuf) {

  int rc = 0;
  unsigned int idx = 0;
  char rsrcUrl[HTTP_URL_LEN];
  char tmp[VSX_MAX_PATH_LEN];
  char tokenstr[16 + META_FILE_TOKEN_LEN];
  char stroutidx[32];
  const char *strerror = NULL;
  unsigned int outidx = 0;
  const char *protoUrl = NULL;
  const char *protoLiveUrl = NULL;
  int *p_action = NULL;
  const char *fileExt = NULL;
  CB_UPDATE_PROTO_INDEXFILE cbUpdateProtoIndexFile = NULL;
  unsigned char buf[PREPROCESS_FILE_LEN_MAX];
  STREAMER_CFG_T *pStreamerCfg = NULL;
  int preferredPort = 0;
  int ismedialinkssl = (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? 1 : 0;

  if(pListenCfg) {
    // We're likely called from mgr context
    ismedialinkssl = (pListenCfg->netflags & NETIO_FLAG_SSL_TLS) ? 1 : 0;
  }

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  switch(urlCap) {

    case URL_CAP_FLVLIVE:
      protoUrl = VSX_FLV_URL;
      protoLiveUrl = VSX_FLVLIVE_URL;
      p_action = pStreamerCfg ? &pStreamerCfg->action.do_flvlive : NULL;
      fileExt = ".flv";
      cbUpdateProtoIndexFile = update_flv_indexfile;
      break;

    case URL_CAP_MKVLIVE:
      protoUrl = VSX_MKV_URL;
      protoLiveUrl = VSX_MKVLIVE_URL;
      p_action = pStreamerCfg ? &pStreamerCfg->action.do_mkvlive : NULL;
      fileExt = ".webm";
      cbUpdateProtoIndexFile = update_mkv_indexfile;
      break;

    case URL_CAP_RTMPLIVE:
    case URL_CAP_RTMPTLIVE:
    case (URL_CAP_RTMPLIVE | URL_CAP_RTMPTLIVE):

      protoUrl = VSX_RTMP_URL;
      protoLiveUrl = VSX_LIVE_URL;
      p_action = pStreamerCfg ? &pStreamerCfg->action.do_rtmplive : NULL;
      fileExt = NULL;
      cbUpdateProtoIndexFile = update_rtmp_indexfile;

      if(urlCap == URL_CAP_RTMPTLIVE) {

        preferredPort = pListenCfg ? htons(INET_PORT(pListenCfg->sa)) : htons(INET_PORT(pConn->pListenCfg->sa));
        VSX_DEBUG_LIVE( LOG(X_DEBUG("LIVE - RTMP try URL_CAP_RTMPTLIVE on http port, then rtmp port")); );

        //
        // /rtmpt Try to find tunnel listener on HTTP port of this live handler
        //
        if(!(pListenCfg = findlistener(pConn, urlCap, preferredPort, &ismedialinkssl))) {
          // Try to find tunnel listener on default RTMP port
          pListenCfg = findlistener(pConn, urlCap, RTMP_PORT_DEFAULT, &ismedialinkssl);
        }

      } else {

        VSX_DEBUG_LIVE( LOG(X_DEBUG("LIVE - RTMP try URL_CAP_RTMPLIVE on http port, then rtmp port")); );

        if(!(pListenCfg = findlistener(pConn, URL_CAP_RTMPLIVE, preferredPort, &ismedialinkssl))) {
          pListenCfg = findlistener(pConn, URL_CAP_RTMPLIVE, RTMP_PORT_DEFAULT, &ismedialinkssl);
        }

        if(!pListenCfg) {
          VSX_DEBUG_LIVE( LOG(X_DEBUG("LIVE - RTMP try urlCap: 0x%x on http port, then rtmp port"), urlCap); );
          if(!(pListenCfg = findlistener(pConn, urlCap, htons(INET_PORT(pConn->pListenCfg->sa)), &ismedialinkssl))) {
            pListenCfg = findlistener(pConn, urlCap, RTMP_PORT_DEFAULT, &ismedialinkssl);
          }
        }

      }

      //if(!pListenCfg) {
      //  urlCap = URL_CAP_RTMPTLIVE;
      //}

        VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - resp_index_file RTMP urlCap: 0x%x, preferredPort: %d, pListenCfg: 0x%x, ismedialinkssl: %d, rtmp: 0x%x, rtmpt: 0x%x"), urlCap, preferredPort, pListenCfg, ismedialinkssl, URL_CAP_RTMPLIVE, URL_CAP_RTMPTLIVE); );

      break;

    case URL_CAP_RTSPLIVE:
      protoUrl = VSX_RTSP_URL;
      //protoLiveUrl = VSX_LIVE_URL;
      protoLiveUrl = VSX_RTSPLIVE_URL;
      p_action = pStreamerCfg ? &pStreamerCfg->action.do_rtsplive : NULL;
      fileExt = NULL;
      cbUpdateProtoIndexFile = update_rtsp_indexfile;

      preferredPort = RTSP_PORT_DEFAULT;
      pListenCfg = findlistener(pConn, URL_CAP_RTSPLIVE, preferredPort, &ismedialinkssl);

      break;

    default:
      return -1;
  }

  //
  // Find the correct listener for the live data source
  //
  if(!pListenCfg) {
    pListenCfg = findlistener(pConn, urlCap, 0, &ismedialinkssl);
  }

  //
  // Construct the underlying media stream URL accessible at the '/flvlive' URL
  //
  stroutidx[0] = '\0';
  rc = get_output_idx(pConn, protoUrl, stroutidx, &outidx);

  if(p_action && !(*p_action)) {
    snprintf(tmp, sizeof(tmp), "%s not enabled", protoUrl);
    LOG(X_ERROR("%s"), tmp);
    strerror = tmp;
    rc = -1;
  } else if(!pListenCfg || INET_PORT(pListenCfg->sa) == 0) {
    LOG(X_ERROR(" address / port substitution not set for %s, urlCap: 0x%x"), protoUrl, urlCap);
    rc = -1;
  } else {

    //
    // Append any security token to the media link URL
    //
    tokenstr[0] = '\0';
    get_token_uriparam(tokenstr, sizeof(tokenstr), pConn);

    if(pargfile && pargfile[0] != '\0') {
      while(pargfile[idx] == '/') {
        idx++;
      }
      snprintf(tmp, sizeof(tmp), "%s%s%s%s", is_remoteargfile ? "" : "/", &pargfile[idx], 
                                             tokenstr[0] != '\0' ? "?" : "", tokenstr);

      if(!strncmp(VSX_MEDIA_URL, tmp, strlen(VSX_MEDIA_URL))) {
        //
        // Most likely we're called from mgr context with static file /media context
        //
        protoLiveUrl = ""; 
      } 

    } else {

      // Workaround flashplayer (flv/rtmp) bug where '&' is not handled in query string for key with no value
      snprintf(tmp, sizeof(tmp), "/live%s?%s%s%d", fileExt ? fileExt : "", tokenstr, 
              tokenstr[0] != '\0' ? ((HAVE_URL_CAP_RTMP(urlCap) || urlCap == URL_CAP_FLVLIVE) ? "," : "&") : "",
              (int) (random() % RAND_MAX));
    }

    if(is_remoteargfile) {
      //
      // The media resource is located on a remote server
      //
      snprintf(rsrcUrl, sizeof(rsrcUrl), "%s", tmp);
    } else {

      if((urlCap & URL_CAP_RTMPTLIVE) && !(urlCap & URL_CAP_RTMPLIVE)) {
        snprintf(rsrcUrl, sizeof(rsrcUrl), URL_RTMPT_FMT_STR"%s%s%s",
               URL_HTTP_FMT_ARGS(pListenCfg, ismedialinkssl), protoLiveUrl, stroutidx, tmp);
      } else if(HAVE_URL_CAP_RTMP(urlCap)) {
        snprintf(rsrcUrl, sizeof(rsrcUrl), URL_RTMP_FMT_STR"%s%s%s",
               URL_HTTP_FMT_ARGS(pListenCfg, ismedialinkssl), protoLiveUrl, stroutidx, tmp);
      } else if(urlCap == URL_CAP_RTSPLIVE) {
        snprintf(rsrcUrl, sizeof(rsrcUrl), URL_RTSP_FMT_STR"%s%s%s",
               URL_HTTP_FMT_ARGS(pListenCfg, ismedialinkssl), protoLiveUrl, stroutidx, tmp);
      } else {
        snprintf(rsrcUrl, sizeof(rsrcUrl), URL_HTTP_FMT_STR"%s%s%s",
               URL_HTTP_FMT_ARGS(pListenCfg, ismedialinkssl), protoLiveUrl, stroutidx, tmp);
      }
    }
  }

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - Created media link URL: '%s', proto: '%s', pargfile: '%s', "
                             "stroutidx: '%s', tmp: '%s', '%s', is_remote: %d"), 
                              rsrcUrl, protoUrl, pargfile, stroutidx, tmp, protoLiveUrl, is_remoteargfile));

  if(rc >= 0 && purloutbuf) {
    //
    // Don't return any HTML response but only the URL
    //
    strncpy(purloutbuf, rsrcUrl, HTTP_URL_LEN - 1);
    rc = 0;
  } else if(rc >= 0 && (rc = cbUpdateProtoIndexFile(pConn, pStreamerCfg, rsrcUrl,  outidx, (char *) buf,
                                 sizeof(buf))) > 0) {
    rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_OK, buf, rc);
  } else {
    http_resp_error(&pConn->sd, pConn->phttpReq, HTTP_STATUS_SERVERERROR, 0, strerror, NULL);
    rc = -1;
  }

  return rc;
}

int srv_ctrl_flvlive(CLIENT_CONN_T *pConn) {
  int rc = 0;
  FLVSRV_CTXT_T flvCtxt;
  struct timeval tv0, tv1;
  STREAMER_CFG_T *pStreamerCfg = NULL;
  STREAMER_OUTFMT_T *pLiveFmt = NULL;
  unsigned int numQFull = 0;
  PKTQUEUE_CONFIG_T qCfg;
  int outidx;
  OUTFMT_CFG_T *pOutFmt = NULL;
  STREAM_STATS_T *pstats = NULL;
  char tmp[128];
  unsigned char buf[64];

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);
  //
  // pStreamerCfg may be null if we have been invoked from src/mgr/srvmvgr.c context
  //

  if(pStreamerCfg && pStreamerCfg->action.liveFmts.out[STREAMER_OUTFMT_IDX_FLV].do_outfmt) {
    pLiveFmt = &pStreamerCfg->action.liveFmts.out[STREAMER_OUTFMT_IDX_FLV];
  }

  //
  // Auto increment the flv slot count buffer queue if a buffer delay is given
  //
  memcpy(&qCfg, &pLiveFmt->qCfg, sizeof(qCfg));
  if(pLiveFmt->bufferDelaySec > 0) {
    qCfg.maxPkts += (MIN(10, pLiveFmt->bufferDelaySec) * 50); 
    LOG(X_DEBUG("Increased flv queue(id:%d) from %d to %d for %.3f s buffer delay"), 
        qCfg.id, pLiveFmt->qCfg.maxPkts, qCfg.maxPkts, pLiveFmt->bufferDelaySec);
  }

  //
  // Obtain the requested xcode output outidx specified in the URI such as '/flv/2'
  //
  if((outidx = outfmt_getoutidx(pConn->phttpReq->puri, NULL)) < 0 || outidx >= IXCODE_VIDEO_OUT_MAX) {
    outidx = 0;
  } else if(outidx > 0) {
    if(pStreamerCfg && !pStreamerCfg->xcode.vid.out[outidx].active) {
      LOG(X_ERROR("Output format index[%d] not active"), outidx);
      pLiveFmt = NULL;
    } else {
      LOG(X_DEBUG("Set "VSX_FLVLIVE_URL" output format index to[%d] url:'%s', for %s:%d"), outidx,
        pConn->phttpReq->puri, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    }
  }

  if(pLiveFmt) {

    if(pStreamerCfg && pStreamerCfg->pMonitor && pStreamerCfg->pMonitor->active) {
      if(!(pstats = stream_monitor_createattach(pStreamerCfg->pMonitor, (const struct sockaddr *) &pConn->sd.sa, 
                                               STREAM_METHOD_FLVLIVE, STREAM_MONITOR_ABR_NONE))) {
      }
    }

    //
    // Add a livefmt cb
    //
    pOutFmt = outfmt_setCb(pLiveFmt, flvsrv_addFrame, &flvCtxt, &qCfg, pstats, 1, 
                           pStreamerCfg ? pStreamerCfg->frameThin : 0, &numQFull);
  }

  if(pOutFmt) {

    memset(&flvCtxt, 0, sizeof(flvCtxt));
    flvCtxt.pSd = &pConn->sd;
    flvCtxt.pReq = pConn->phttpReq;
    flvCtxt.avBufferDelaySec = pLiveFmt->bufferDelaySec;
    flvCtxt.pnovid = &pStreamerCfg->novid;
    flvCtxt.pnoaud = &pStreamerCfg->noaud;
    flvCtxt.requestOutIdx = outidx;
    flvCtxt.av.vid.pStreamerCfg = pStreamerCfg;
    flvCtxt.av.aud.pStreamerCfg = pStreamerCfg;

    if(flvsrv_init(&flvCtxt, MAX(pLiveFmt->qCfg.maxPktLen, pLiveFmt->qCfg.growMaxPktLen)) < 0) {
      //
      // Remove the livefmt cb
      //
      outfmt_removeCb(pOutFmt);
      pOutFmt = NULL;
    }

  }

  if(pOutFmt) {

    //
    // Unpause the outfmt callback mechanism now that flvsrv_init was called
    //
    outfmt_pause(pOutFmt, 0);

    LOG(X_INFO("Starting flvlive stream[%d] %d/%d to %s:%d"), pOutFmt->cbCtxt.idx, numQFull + 1,
           pLiveFmt->max, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    //
    // Set outbound QOS
    //
    // TODO: make QOS configurable
    net_setqos(NETIOSOCK_FD(pConn->sd.netsocket), (const struct sockaddr *) &pConn->sd.sa, DSCP_AF36);

    gettimeofday(&tv0, NULL);

    rc = flvsrv_sendHttpResp(&flvCtxt);

    //
    // Sleep here on the http socket until a socket error or
    // an error from when the frame callback flvsrv_addFrame is invoked.
    //
    while(!g_proc_exit && flvCtxt.state > FLVSRV_STATE_ERROR) {

      //if(net_issockremotelyclosed(pConn->sd.socket, 1)) {
      //  LOG(X_DEBUG("flvlive connection from %s:%d has been remotely closed"),
      //     inet_ntoa(pConn->sd.sain.sin_addr), ntohs(pConn->sd.sain.sin_port) );
      //  break;
      //}

      if((rc = netio_recvnb(&pConn->sd.netsocket, buf, sizeof(buf), 500)) < 0) {
        break;
      } else if(rc == 0 && STUNSOCK(pConn->sd.netsocket).rcvclosed) {
        break;
      }
    }

    if(flvCtxt.state <= FLVSRV_STATE_ERROR) {
      //
      // Force srv_cmd_proc loop to exit even for "Connection: Keep-Alive"
      //
      rc = -1;
    }

    gettimeofday(&tv1, NULL);

    LOG(X_DEBUG("Finished sending flvlive %llu bytes to %s:%d (%.1fKb/s)"), flvCtxt.totXmit,
             FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)),
             (double) flvCtxt.totXmit / (((tv1.tv_sec - tv0.tv_sec) * 1000) +
                       ((tv1.tv_usec - tv0.tv_usec) / 1000)) * 7.8125);

    LOG(X_INFO("Ending flvlive stream[%d] to %s:%d"), pOutFmt->cbCtxt.idx,
             FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    //
    // Remove the livefmt cb
    //
    outfmt_removeCb(pOutFmt);

    flvsrv_close(&flvCtxt);

  } else {

    if(pstats) {
      //
      // Destroy automatically detaches the stats from the monitor linked list
      //
      stream_stats_destroy(&pstats, NULL);
    }

    LOG(X_WARNING("No flvlive resource available (max:%d) for %s:%d"), (pLiveFmt ? pLiveFmt->max : 0),
                  FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    rc = -1;
  }

  // TODO: this should return < 0 to force http connection to end
  return rc;
}

int srv_ctrl_mkvlive(CLIENT_CONN_T *pConn) {
  int rc = 0;
  MKVSRV_CTXT_T mkvCtxt;
  struct timeval tv0, tv1;
  STREAMER_CFG_T *pStreamerCfg = NULL;
  STREAMER_OUTFMT_T *pLiveFmt = NULL;
  unsigned int numQFull = 0;
  PKTQUEUE_CONFIG_T qCfg;
  int outidx;
  OUTFMT_CFG_T *pOutFmt = NULL;
  STREAM_STATS_T *pstats = NULL;
  double duration;
  char tmp[128];
  unsigned char buf[64];

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);
  //
  // pStreamerCfg may be null if we have been invoked from src/mgr/srvmvgr.c context
  //

  if(pStreamerCfg && pStreamerCfg->action.liveFmts.out[STREAMER_OUTFMT_IDX_MKV].do_outfmt) {
    pLiveFmt = &pStreamerCfg->action.liveFmts.out[STREAMER_OUTFMT_IDX_MKV];
  }

  //
  // Auto increment the mkv slot count buffer queue if a buffer delay is given
  //
  memcpy(&qCfg, &pLiveFmt->qCfg, sizeof(qCfg));
  if(pLiveFmt->bufferDelaySec > 0) {
    qCfg.maxPkts += (MIN(10, pLiveFmt->bufferDelaySec) * 50); 
    LOG(X_DEBUG("Increased mkv queue(id:%d) from %d to %d for %.3f s buffer delay"), 
        qCfg.id, pLiveFmt->qCfg.maxPkts, qCfg.maxPkts, pLiveFmt->bufferDelaySec);
  }

  //
  // Obtain the requested xcode output outidx specified in the URI such as '/mkv/2'
  //
  if((outidx = outfmt_getoutidx(pConn->phttpReq->puri, NULL)) < 0 || outidx >= IXCODE_VIDEO_OUT_MAX) {
    outidx = 0;
  } else if(outidx > 0) {
    if(pStreamerCfg && !pStreamerCfg->xcode.vid.out[outidx].active) {
      LOG(X_ERROR("Output format index[%d] not active"), outidx);
      pLiveFmt = NULL;
    } else {
      LOG(X_DEBUG("Set "VSX_MKVLIVE_URL" output format index to[%d] url:'%s', for %s:%d"), outidx,
        pConn->phttpReq->puri, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    }
  }

  if(pLiveFmt) {

    if(pStreamerCfg && pStreamerCfg->pMonitor && pStreamerCfg->pMonitor->active) {
      if(!(pstats = stream_monitor_createattach(pStreamerCfg->pMonitor, (const struct sockaddr *) &pConn->sd.sa, 
                                               STREAM_METHOD_MKVLIVE, STREAM_MONITOR_ABR_NONE))) {
      }
    }

    //
    // Add a livefmt cb
    //
    pOutFmt = outfmt_setCb(pLiveFmt, mkvsrv_addFrame, &mkvCtxt, &qCfg, pstats, 1, 
                           pStreamerCfg ? pStreamerCfg->frameThin : 0, &numQFull);
  }

  if(pOutFmt) {

    memset(&mkvCtxt, 0, sizeof(mkvCtxt));
    mkvCtxt.pSd = &pConn->sd;
    mkvCtxt.pReq = pConn->phttpReq;
    mkvCtxt.avBufferDelaySec = pLiveFmt->bufferDelaySec;
    mkvCtxt.pnovid = &pStreamerCfg->novid;
    mkvCtxt.pnoaud = &pStreamerCfg->noaud;
    mkvCtxt.requestOutIdx = outidx;
    mkvCtxt.av.vid.pStreamerCfg = pStreamerCfg;
    mkvCtxt.av.aud.pStreamerCfg = pStreamerCfg;
    mkvCtxt.faoffset_mkvpktz = pStreamerCfg->status.faoffset_mkvpktz;

    if(mkvsrv_init(&mkvCtxt, MAX(pLiveFmt->qCfg.maxPktLen, pLiveFmt->qCfg.growMaxPktLen)) < 0) {
      //
      // Remove the livefmt cb
      //
      outfmt_removeCb(pOutFmt);
      pOutFmt = NULL;
    }

  }

  if(pOutFmt) {

    //
    // Unpause the outfmt callback mechanism now that mkvsrv_init was called
    //
    outfmt_pause(pOutFmt, 0);

    LOG(X_INFO("Starting mkvlive stream[%d] %d/%d to %s:%d"), pOutFmt->cbCtxt.idx, numQFull + 1,
           pLiveFmt->max, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    //
    // Set outbound QOS
    //
    // TODO: make QOS configurable
    net_setqos(NETIOSOCK_FD(pConn->sd.netsocket), (const struct sockaddr *) &pConn->sd.sa, DSCP_AF36);

    gettimeofday(&tv0, NULL);

    rc = mkvsrv_sendHttpResp(&mkvCtxt, CONTENT_TYPE_MATROSKA);

    //
    // Sleep here on the http socket until a socket error or
    // an error from when the frame callback mkvsrv_addFrame is invoked.
    //
    while(!g_proc_exit && mkvCtxt.state > MKVSRV_STATE_ERROR) {

      //if(net_issockremotelyclosed(pConn->sd.socket, 1)) {
      //  LOG(X_DEBUG("mkvlive connection from %s:%d has been remotely closed"),
      //     inet_ntoa(pConn->sd.sain.sin_addr), ntohs(pConn->sd.sain.sin_port) );
      //  break;
      //}

      if((rc = netio_recvnb(&pConn->sd.netsocket, buf, sizeof(buf), 500)) < 0) {
        break;
      } else if(rc == 0 && STUNSOCK(pConn->sd.netsocket).rcvclosed) {
        break;
      }
    }

    if(mkvCtxt.state <= MKVSRV_STATE_ERROR) {
      //
      // Force srv_cmd_proc loop to exit even for "Connection: Keep-Alive"
      //
      rc = -1;
    }

    gettimeofday(&tv1, NULL);

    duration = ((tv1.tv_sec - tv0.tv_sec) * 1000) +  ((tv1.tv_usec - tv0.tv_usec) / 1000);
    LOG(X_DEBUG("Finished sending mkvlive %llu bytes to %s:%d (%.1fKb/s)"), mkvCtxt.totXmit,
             FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)),
             duration > 0 ? (double) mkvCtxt.totXmit / duration  * 7.8125 : 0);

    LOG(X_INFO("Ending mkvlive stream[%d] to %s:%d"), pOutFmt->cbCtxt.idx,
             FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    //
    // Remove the livefmt cb
    //
    outfmt_removeCb(pOutFmt);

    mkvsrv_close(&mkvCtxt);

  } else {

    if(pstats) {
      //
      // Destroy automatically detaches the stats from the monitor linked list
      //
      stream_stats_destroy(&pstats, NULL);
    }

    LOG(X_WARNING("No mkvlive resource available (max:%d) for %s:%d"),
        (pLiveFmt ? pLiveFmt->max : 0),
        FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_SERVERERROR,
      (unsigned char *) HTTP_STATUS_STR_SERVERERROR, strlen(HTTP_STATUS_STR_SERVERERROR));

    rc = -1;
  }

  // TODO: this should return < 0 to force http connection to end
  return rc;

}

int srv_ctrl_tslive(CLIENT_CONN_T *pConn, HTTP_STATUS_T *pHttpStatus) {
  int rc = 0;
  unsigned int idx = 0;
  int liveQIdx = -1;
  unsigned int numQFull = 0;
  STREAMER_LIVEQ_T *pLiveQ;
  STREAM_STATS_T *pstats = NULL;
  PKTQUEUE_T *pQueue = NULL;
  unsigned int queueSz = 0;
  const char *parg;
  char tmp[128];
  char resp[512];
  unsigned char *presp = NULL;
  //HTTP_STATUS_T statusCode = HTTP_STATUS_OK;
  int lenresp = 0;
  int outidx;
  unsigned int maxQ = 0;
  STREAMER_CFG_T *pStreamerCfg = NULL;

  *pHttpStatus = HTTP_STATUS_SERVICEUNAVAIL;

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  //
  // pStreamerCfg may be null if we have been invoked from src/mgr/srvmvgr.c context
  //
  if(!pStreamerCfg) {
    LOG(X_ERROR("tslive may not be enabled"));
    return -1;
  }

  //
  // Validate any URI (un-)secure token  the request
  //
  if((rc = http_check_pass(pConn)) < 0) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return rc;
  } 

  //
  // Obtain the requested xcode output outidx specified in the URI such as '/tslive/2'
  //
  if((outidx = outfmt_getoutidx(pConn->phttpReq->puri, NULL)) < 0 ||
     outidx >= IXCODE_VIDEO_OUT_MAX) {
    outidx = 0;
  } else if(outidx > 0) {
    if(pStreamerCfg && !pStreamerCfg->xcode.vid.out[outidx].active) {
       LOG(X_ERROR("Output format index[%d] not active"), outidx);
       return -1;
    } else {
      LOG(X_DEBUG("Set "VSX_TSLIVE_URL" output format index to[%d] url:'%s', for %s:%d"), outidx,
        pConn->phttpReq->puri, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    }
  }
  
  //statusCode = HTTP_STATUS_SERVICEUNAVAIL;
  srv_lock_conn_mutexes(pConn, 1);

  if(!(pLiveQ = &pStreamerCfg->liveQs[outidx])) {
    rc = -1;
  }

  if(rc == 0 && !pStreamerCfg->action.do_tslive) {

    LOG(X_WARNING("tslive not currently enabled for request from %s:%d"),
           FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

    if((lenresp = snprintf(resp, sizeof(resp), "Live stream not currently enabled")) > 0) {
      presp = (unsigned char *) resp;
    }
    rc = -1;
  }

  if(rc == 0) {
    if(pLiveQ->pQs) {
      maxQ = pLiveQ->max;
    }
    queueSz = pLiveQ->qCfg.maxPkts;
  }

  if(rc == 0 && (parg = conf_find_keyval((const KEYVAL_PAIR_T *) &pConn->phttpReq->uriPairs, "queue"))) {
    if((queueSz = atoi(parg)) > pLiveQ->qCfg.growMaxPkts) {
      queueSz = pLiveQ->qCfg.growMaxPkts;
    }
    LOG(X_INFO("Custom tslive queue size set to %u for %s:%d"), queueSz, 
         FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
  }

  // TODO pktqueue size >= sizeof(PACKETGEN_PKT_UDP_T::data)
  if(rc == 0) {
    if((pQueue = pktqueue_create(queueSz, 
                                 pLiveQ->qCfg.maxPktLen, 0, 0, 0, 0, 1, 1)) == NULL) {
      rc = -1;
      *pHttpStatus = HTTP_STATUS_SERVERERROR;
      LOG(X_ERROR("Failed to create %s queue %d x %d for %s:%d"), VSX_TSLIVE_URL, queueSz, 
        pLiveQ->qCfg.maxPktLen, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    } else {
      //
      // Set concataddenum to allow filling up as much of each slot as possible,
      // if the reader hasn't yet gobbled up the prior write
      //
      pQueue->cfg.concataddenum = pLiveQ->qCfg.concataddenum;
      LOG(X_DEBUG("Created %s queue %d x %d for %s:%d"), VSX_TSLIVE_URL, queueSz, 
        pLiveQ->qCfg.maxPktLen, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
    }
  }

  if(rc == 0) {

    //
    // Find an available liveQ queue instance 
    //
    pthread_mutex_lock(&pLiveQ->mtx);
    if(pLiveQ->numActive <  maxQ) {
      for(idx = 0; idx < maxQ; idx++) {
        if(pLiveQ->pQs[idx] == NULL) {

          //
          // Create any stream throughput statistics meters
          //
          if(pStreamerCfg->pMonitor && pStreamerCfg->pMonitor->active) {

            if(!(pstats = stream_monitor_createattach(pStreamerCfg->pMonitor, (const struct sockaddr *) &pConn->sd.sa, 
                                                     STREAM_METHOD_TSLIVE, STREAM_MONITOR_ABR_NONE))) {
            } else {
              pQueue->pstats = &pstats->throughput_rt[0];
            }
          }

          liveQIdx = idx;

          pLiveQ->pQs[liveQIdx] = pQueue;
          pLiveQ->numActive++;

          if(pConn->pStreamerCfg1) {
            pConn->pStreamerCfg1->liveQs[outidx].pQs[liveQIdx] = pQueue;
            pConn->pStreamerCfg1->liveQs[outidx].numActive++;

          }
          pQueue->cfg.id = pLiveQ->qCfg.id + liveQIdx;
          break;
        }
        numQFull++;
      }
    }
    pthread_mutex_unlock(&pLiveQ->mtx);

    if(liveQIdx < 0) {
      LOG(X_ERROR("No tslive queue (max:%d) for %s:%d"), maxQ, 
          FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
      if((lenresp = snprintf(resp, sizeof(resp), 
                     "All available resources currently in use")) > 0) {
        presp = (unsigned char *) resp;
      }
      rc = -1;
    }

  }

  srv_lock_conn_mutexes(pConn, 0);

  //
  // Upon any error send HTTP error status code and quit
  //
  if(rc != 0) {
    if(presp) {
      http_resp_send(&pConn->sd, pConn->phttpReq, *pHttpStatus, presp, lenresp);
      *pHttpStatus = HTTP_STATUS_OK;
    }
    if(pQueue) {
      pktqueue_destroy(pQueue);
    }
    return rc;
  }

  //
  // Set outbound QOS
  //
  // TODO: make QOS configurable
  net_setqos(NETIOSOCK_FD(pConn->sd.netsocket), (const struct sockaddr *) &pConn->sd.sa, DSCP_AF36);

  LOG(X_INFO("Starting tslive stream[%d] %d/%d to %s:%d"), liveQIdx, numQFull + 1, maxQ,
           FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

  //
  // Request an IDR from the underling encoder
  //
  if(pStreamerCfg) {
    //
    // Set the requested IDR time a bit into the future because there may be a slight
    // delay until the queue reader starts reading mpeg2-ts packets
    //
    streamer_requestFB(pStreamerCfg, outidx, ENCODER_FBREQ_TYPE_FIR, 200, REQUEST_FB_FROM_LOCAL);
  }

  *pHttpStatus = HTTP_STATUS_OK;
  rc = http_resp_sendtslive(&pConn->sd, pConn->phttpReq, pQueue, CONTENT_TYPE_MP2TS);

  LOG(X_INFO("Ending tslive stream[%d] to %s:%d"), liveQIdx,
           FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));

  srv_lock_conn_mutexes(pConn, 1);
  pthread_mutex_lock(&pLiveQ->mtx);

  if(pLiveQ->numActive > 0) {
    pLiveQ->numActive--;
  }
  if(pLiveQ->pQs) {
    pLiveQ->pQs[liveQIdx] = NULL;
  }
  if(pConn->pStreamerCfg1) {
    if(pLiveQ->numActive > 0) {
      pLiveQ->numActive--;
    }
    if(pConn->pStreamerCfg1->liveQs[outidx].pQs) {
      pConn->pStreamerCfg1->liveQs[outidx].pQs[liveQIdx] = NULL;
    }
  }
  pthread_mutex_unlock(&pLiveQ->mtx);
  srv_lock_conn_mutexes(pConn, 0);

  if(pQueue) {
    pktqueue_destroy(pQueue);
  }
  if(pstats) {
    //
    // Destroy automatically detaches the stats from the monitor linked list
    //
    stream_stats_destroy(&pstats, NULL);
  }

  // TODO: this should return < 0 to force http connection to end
  return rc;
}

static int get_requested_outidx(const CLIENT_CONN_T *pConn, STREAMER_CFG_T *pStreamerCfg, 
                                const char **ppuri, char *urlbuf, int *phaveoutidx, int *poutidx) {
  int rc = 0;
  size_t sz = 0;
  const char *pend;
  char tmp[128];

  *ppuri = pConn->phttpReq->puri;
  pend = *ppuri;

  //
  //
  // Obtain the requested xcode output outidx specified in the URI such as '/httplive/2' or '/httplive/prof_2'
  //
  if((*poutidx = outfmt_getoutidx(*ppuri, &pend)) < 0 || *poutidx >= IXCODE_VIDEO_OUT_MAX) {
    *poutidx = 0;
  } else {

    if(pend && pend != *ppuri && strlen(pend) > 0) {
      *phaveoutidx = 1;
    }

    //
    // Cut out the end of the URL containing the outidx '/httplive[/2]' or '/httplive[/prof_2]'
    //
    strncpy(urlbuf, *ppuri, HTTP_URL_LEN);
    if((sz = (pend - *ppuri)) < HTTP_URL_LEN) {
      urlbuf[sz] = '\0';
    }
    *ppuri = urlbuf;
    if(*poutidx > 0) {
      if(pStreamerCfg && !pStreamerCfg->xcode.vid.out[*poutidx].active) {
        LOG(X_ERROR("Output format index[%d] not active"), *poutidx);
        rc = -1;
      } else {
        LOG(X_DEBUG("Set output format index to[%d] url:'%s', for %s:%d"), *poutidx,
            pConn->phttpReq->puri, FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)));
      }
    }
  }

  return rc;
}

int get_requested_path(const char *uriPrefix, 
                              const char *puri,
                              const char **ppargpath, 
                              const char **ppargrsrc) {
  int rc = 0;
  size_t sz;

  if((sz = strlen(uriPrefix)) > strlen(puri)) {
    return -1;
  }

  *ppargpath = &puri[sz];

  while(**ppargpath == '/') {
    (*ppargpath)++;
  }
  sz = strlen(*ppargpath);
  while(sz > 0 && (*ppargpath)[sz - 1] != '/') {
    sz--;
  }
  *ppargrsrc = &(*ppargpath)[sz];

  return rc;
}

static int send_mediafile(CLIENT_CONN_T *pConn, 
                          const char *path, 
                          HTTP_STATUS_T *pHttpStatus, 
                          const char *contentType) {
                   
  int rc = 0;
  FILE_STREAM_T fileStream;
  HTTP_MEDIA_STREAM_T mediaStream;

  memset(&fileStream, 0, sizeof(fileStream));
  if(rc == 0 && OpenMediaReadOnly(&fileStream, path) < 0) {
    *pHttpStatus = HTTP_STATUS_NOTFOUND;
    return -1;
  }

  memset(&mediaStream, 0, sizeof(mediaStream));
  mediaStream.pFileStream = &fileStream;

  //
  // Get the media file properties, like size, content type,
  //
  if(srv_ctrl_initmediafile(&mediaStream, 0) < 0) {
    CloseMediaFile(&fileStream);
    *pHttpStatus = HTTP_STATUS_NOTFOUND;
    return -1;
  }

  //
  // Check and override the content type
  //
  if(contentType) {
    mediaStream.pContentType = contentType;
  }

  *pHttpStatus = HTTP_STATUS_OK;
  rc = http_resp_sendmediafile(pConn, pHttpStatus, &mediaStream,
  0, 0);
  // HTTP_THROTTLERATE, 1);
  // .5, 4);

  //fprintf(stderr, "RESP_SENDMEDIAFILE '%s' rc:%d\n", fileStream.filename, rc);

  CloseMediaFile(&fileStream);

  return rc;
}

int srv_ctrl_mooflive(CLIENT_CONN_T *pConn, 
                      const char *uriPrefix, 
                      const char *virtFilePath,
                      const char *filePath, 
                      HTTP_STATUS_T *pHttpStatus) {
  int rc = 0;
  const char *pargrsrc = NULL;
  const char *pargpath  = NULL;
  char hostpath[128];
  char tokenstr[16 + META_FILE_TOKEN_LEN];
  const char *outdir;
  const char *contentType = NULL;
  const char *ext;
  char stroutidx[32];
  char path[VSX_MAX_PATH_LEN];
  unsigned char buf[PREPROCESS_FILE_LEN_MAX];
  size_t sz = 0;
  size_t sz2;
  const SRV_LISTENER_CFG_T *pListenMedia = NULL;
  char url[HTTP_URL_LEN];
  const char *puri;
  int haveoutidx = 0;
  int outidx = 0;
  STREAMER_CFG_T *pStreamerCfg = NULL;
  int ismedialinkssl = 0;

  if(!pConn || !uriPrefix) {
    return -1;
  }

  ismedialinkssl = (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? 1 : 0;
  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);
  //
  // pStreamerCfg may be null if we have been invoked from src/mgr/srvmvgr.c context
  //

  if(pStreamerCfg && !pStreamerCfg->action.do_moofliverecord) {
    LOG(X_ERROR(VSX_DASH_URL" DASH not enabled"));
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  }

  stroutidx[0] = '\0';

  //
  // Remove any trailing '/2' stream outidx designation and alter the URI, storing it in 'url'
  //
  if((rc = get_requested_outidx(pConn, pStreamerCfg, &puri, url, &haveoutidx, &outidx)) < 0) {
    *pHttpStatus = HTTP_STATUS_SERVERERROR;
    return -1;
  } else if(haveoutidx) {
    snprintf(stroutidx, sizeof(stroutidx), "%d", outidx + 1);
  }

  //
  // Set pargpath to proceed the '/dash' in the URI
  // Set pargsrc to the resource name without any leading directory path
  //
  if(rc >= 0 && (rc = get_requested_path(uriPrefix, puri, &pargpath, &pargrsrc)) < 0) {
    *pHttpStatus = HTTP_STATUS_SERVERERROR;
    return -1;
  }

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_mooflive uri: '%s', virt-file: '%s', on-disk: '%s', "
                  "prefix: '%s', outidx: %d, pargrsrc: '%s', pargpath: '%s'"),
                   pConn->phttpReq->puri, virtFilePath, filePath, uriPrefix, outidx, pargrsrc, pargpath));

  //
  // Return rsrc/dash_embed.html file
  //
  if(rc == 0 && (!pargrsrc || pargrsrc [0] == '\0')) {

    //
    // Validate & Authenticate the request
    //
    if((rc = http_check_pass(pConn)) < 0) {
      *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      return rc;
    }

    //
    // Find the correct listener for the flvlive FLV data source
    //
    if(!pListenMedia) {
      pListenMedia = findlistener(pConn, URL_CAP_MOOFLIVE, 0, &ismedialinkssl);
      //
      // pListenMedia may be null when called from mgr context
      //
    }

    if(!filePath || !virtFilePath) {
      snprintf(path, sizeof(path), VSX_DASH_URL"%s%s", pargpath ? "/" : "", pargpath ? pargpath : "");
      virtFilePath = filePath = path;
    }

    if(pListenMedia) {
      snprintf(hostpath, sizeof(hostpath), URL_HTTP_FMT_STR, URL_HTTP_FMT_ARGS(pListenMedia, ismedialinkssl));
    } else {
      hostpath[0] = '\0';
    }

    if((sz = strlen(virtFilePath)) < 1) {
      sz = 1;
    }

    //
    // Return the default DASH MPD file, which is stored in the html output directory
    //
    if(rc >= 0 &&
       get_token_uriparam(tokenstr, sizeof(tokenstr), pConn) >= 0 &&
       snprintf(url, sizeof(url), "%s%s%s%s"DASH_MPD_DEFAULT_NAME"%s%s",
                hostpath, virtFilePath, virtFilePath[sz - 1] == '/' ? "" : "/", stroutidx,
                tokenstr[0] != '\0' ? "?" : "", tokenstr) > 0 &&
       (rc = update_dash_indexfile(pConn, pStreamerCfg, url, outidx, (char *) buf, sizeof(buf))) > 0) {

      VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_mooflive sending substituted index file with media: '%s'"
                                   ", hostpath: '%s', virtFilePath: '%s'"), url, hostpath, virtFilePath));

      rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_OK, buf, rc);
    } else {
      *pHttpStatus = HTTP_STATUS_SERVERERROR;
      rc = -1; 
    }

    return rc;
  }

  if(!filePath) {
    filePath = pargpath;
  }

  if(rc == 0) {
    mediadb_fixdirdelims((char *) filePath, DIR_DELIMETER);
  }

  path[0] = '\0';
  outdir = pConn->pCfg->pMoofCtxts[outidx]->dashInitCtxt.outdir;

  if(rc >= 0 && filePath) {

    if(strlen(filePath) <= 4) {
      *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      return -1;
    }

    sz = strlen(filePath);
    sz2 = strlen("."HTTPLIVE_TS_NAME_EXT);

    //
    // Check for default synonym 'dash/default.mpd'
    //
    //if(!strncmp(filePath, DASH_MPD_DEFAULT_ALIAS_NAME, strlen(DASH_MPD_DEFAULT_ALIAS_NAME) + 1)) {
    //  filePath = pargrsrc = DASH_MPD_DEFAULT_NAME;
    //} else 
    if(!strncasecmp(&filePath[sz - sz2], "."HTTPLIVE_TS_NAME_EXT, strlen("."HTTPLIVE_TS_NAME_EXT))) {
      //
      // Load anything with a ".ts" extension from /html/httplive 'outdir_ts'
      //
      outdir = pConn->pCfg->pMoofCtxts[outidx]->dashInitCtxt.outdir_ts;
    }

  }

  if(!outdir) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  //
  // Convert the URI path to the html/[dash | httplive] output directory file path
  //
  } else if(rc >= 0 && mediadb_prepend_dir(outdir, filePath, path, sizeof(path)) < 0) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  }

  //fprintf(stderr, "HTTP DASH REQUEST /dash uri:'%s' ('%s'), pargpath:'%s', pargsrc:'%s' rc:%d prfx:'%s' fp:'%s', path:'%s',outdir:'%s', outdir_ts:'%s'\n", pConn->phttpReq->puri, puri, pargpath, pargrsrc, rc, uriPrefix, filePath, path, pConn->pCfg->pMoofCtxts[outidx]->dashInitCtxt.outdir, pConn->pCfg->pMoofCtxts[outidx]->dashInitCtxt.outdir_ts);

  if(rc >= 0) {

    sz = strlen(filePath);
    sz2 = strlen(DASH_MPD_EXT);
    if(!strcasecmp(&filePath[sz - sz2], DASH_MPD_EXT)) {

      //
      // Send an MPD file created in the html output directory
      //
      rc = http_resp_sendfile(pConn, pHttpStatus, path, CONTENT_TYPE_DASH_MPD, HTTP_ETAG_AUTO);

    } else {

      //
      // Check the URL_CAP_MOOFLIVE capability because it is not necessary if only
      // returning the main index html
      //
      if(!(pConn->pListenCfg->urlCapabilities & URL_CAP_MOOFLIVE)) {
        *pHttpStatus = HTTP_STATUS_FORBIDDEN;
        return -1;
      }

      //
      // For .m4s moof data files, there is no way to tell if the mp4 file is audio or video
      // related, even by introspecting it, so we try to look at the filename to see if it belongs
      // the the mpd audio adaptation set and set the content type accordingly
      //
      if((ext = strutil_getFileExtension(path)) && !strcmp(ext, DASH_DEFAULT_SUFFIX_M4S) &&
         (ext = mpd_path_get_adaptationtag(path))) {
        if(ext[0] == DASH_MP4ADAPTATION_TAG_AUD[0]) {
          contentType = CONTENT_TYPE_M4A;
        } else if(ext[0] == DASH_MP4ADAPTATION_TAG_VID[0]) {
          contentType = CONTENT_TYPE_M4V;
        }
      }

      // 
      // Send a media file segment from the html output directory
      //
      rc = send_mediafile(pConn, path, pHttpStatus, contentType);
    }

  }

  return rc;
}

int srv_ctrl_rtmp(CLIENT_CONN_T *pConn, 
                  const char *pargfile, 
                  int is_remoteargfile, 
                  const char *rsrcUrl, 
                  const SRV_LISTENER_CFG_T *pListenRtmp,
                  unsigned int outidx,
                  STREAM_METHOD_T streamMethod) {
  STREAMER_CFG_T *pStreamerCfg = NULL;

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  VSX_DEBUG_LIVE( LOG(X_DEBUG("LIVE - srv_ctrl_rtmp file: '%s', is_remoteargfile: %d, outidx: %d, method: %s, "
                              "pListen: 0x%x, urlCap: 0x%x"), pargfile, is_remoteargfile, outidx, 
               devtype_methodstr(streamMethod), pListenRtmp, pListenRtmp ? pListenRtmp->urlCapabilities : -1););

  //
  // Return the rsrc/rtmp_embed.html file with all substitions
  //
  return resp_index_file(pConn, pargfile, is_remoteargfile, pListenRtmp, 
           streamMethod == STREAM_METHOD_RTMPT ? URL_CAP_RTMPTLIVE : (URL_CAP_RTMPLIVE | URL_CAP_RTMPTLIVE), 
           NULL);
}

int srv_ctrl_rtsp(CLIENT_CONN_T *pConn, 
                  const char *pargfile, 
                  int is_remoteargfile, 
                  const SRV_LISTENER_CFG_T *pListenMedia) {
  int rc = 0;
  STREAMER_CFG_T *pStreamerCfg = NULL;

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_rtsp file: '%s', is_remoteargfile: %d, connection: %s"), pargfile, is_remoteargfile, (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? "ssl" : "plain-text"); );

  //
  // Return the rsrc/rtsp_embed.html file with all substitions
  //
  return resp_index_file(pConn, pargfile, is_remoteargfile, pListenMedia, URL_CAP_RTSPLIVE, NULL);

  return rc;
}

int srv_ctrl_httplive(CLIENT_CONN_T *pConn, 
                      const char *uriPrefix, 
                      const char *virtFilePath, 
                      const char *filePath,
                      HTTP_STATUS_T *pHttpStatus) {
  int rc = 0;
  const char *pargrsrc = NULL;
  const char *pargpath  = NULL;
  char tokenstr[16 + META_FILE_TOKEN_LEN];
  char hostpath[256];
  char path[VSX_MAX_PATH_LEN];
  struct stat st;
  unsigned char *presp = NULL;
  unsigned int lenresp = 0;
  const SRV_LISTENER_CFG_T *pListenMedia = NULL;
  char url[HTTP_URL_LEN];
  const char *puri, *pend;
  unsigned char buf[PREPROCESS_FILE_LEN_MAX];
  size_t sz;
  int haveoutidx = 0;
  int outidx;
  STREAMER_CFG_T *pStreamerCfg = NULL;
  int ismedialinkssl = 0;

  if(!pConn || !uriPrefix) {
    return -1;
  }
/*
  if(!filePath) {
    filePath = virtFilePath;
  } else if(!virtFilePath) {
    virtFilePath = filePath;
  }
*/

  ismedialinkssl = (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? 1 : 0;
  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  if(pStreamerCfg && !pStreamerCfg->action.do_httplive) {
    LOG(X_ERROR(VSX_HTTPLIVE_URL" httplive not enabled"));
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  }

  //
  // Remove any trailing '/2' stream outidx designation and alter the URI, storing it in 'url'
  //
  if((rc = get_requested_outidx(pConn, pStreamerCfg, &puri, url, &haveoutidx, &outidx)) < 0) {
    *pHttpStatus = HTTP_STATUS_SERVERERROR;
  }

  //
  // Set pargpath to proceed the '/httplive' in the URI
  // Set pargsrc to the resource name without any leading directory path
  //
  if(rc >= 0 && (rc = get_requested_path(uriPrefix, puri, &pargpath, &pargrsrc)) < 0) {
    *pHttpStatus = HTTP_STATUS_SERVERERROR;
  }

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_httplive uri: '%s', virt-file: '%s', on-disk: '%s', "
        "prefix: '%s', haveoutidx: %d, outidx: %d, pargrsrc: '%s', pargpath: '%s'"), 
         pConn->phttpReq->puri, virtFilePath, filePath, uriPrefix, haveoutidx, outidx, pargrsrc, pargpath));

  //
  // Return rsrc/httplive_embed.html file
  //
  if(rc == 0 && (!pargrsrc || pargrsrc [0] == '\0')) {

    //
    // Validate & Authenticate the request
    //
    if((rc = http_check_pass(pConn)) < 0) {
      *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      return rc;
    } 

    presp = buf;
    lenresp = sizeof(buf);

    //
    // Check for the existance of a default httplive/index.html
    //
    if(mediadb_prepend_dir(pConn->pCfg->pHttpLiveDatas[outidx]->dir,
                 HTTPLIVE_INDEX_HTML, path, sizeof(path)) == 0 && fileops_stat(path, &st) == 0) {

      VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_httplive sending pre-existing index file path: '%s'"), path));

      rc = http_resp_sendfile(pConn, pHttpStatus, path, CONTENT_TYPE_TEXTHTML, NULL);

    } else {

      if(!filePath || !virtFilePath) {
        snprintf(path, sizeof(path), VSX_HTTPLIVE_URL"%s%s", pargpath ? "/" : "", pargpath ? pargpath : "");
        virtFilePath = filePath = path;
      }
      
      //
      // If multiple output xcode is set, by default return master m3u8 containing bitrate
      // specific m3u8 playlists
      //
      pend = pConn->pCfg->pHttpLiveDatas[outidx]->fileprefix;
      if(!haveoutidx && pConn->pCfg->pHttpLiveDatas[0]->count > 1) {
        pend = HTTPLIVE_MULTIBITRATE_NAME_PRFX;
      }

      //
      // Find the correct listener for the data source
      // and construct the hostname url for the .m3u8 master playlist
      //
      if(!pListenMedia) {
        pListenMedia = findlistener(pConn, URL_CAP_TSHTTPLIVE, 0, &ismedialinkssl);
        //
        // pListenMedia may be null when called from mgr context
        //
      }

      if(pListenMedia) {
        snprintf(hostpath, sizeof(hostpath), URL_HTTP_FMT_STR, URL_HTTP_FMT_ARGS(pListenMedia, ismedialinkssl));
      } else {
        hostpath[0] = '\0';
      }

      if((sz = strlen(virtFilePath)) < 1) {
        sz = 1;
      }

      //
      // Use html/rsrc/httplive_embed.html
      // 
      if(rc >= 0 && 
         get_token_uriparam(tokenstr, sizeof(tokenstr), pConn) >= 0 &&
         snprintf(url, sizeof(url), "%s%s%s%s"HTTPLIVE_PL_NAME_EXT"%s%s",  
                  hostpath, virtFilePath, virtFilePath[sz - 1] == '/' ? "" : "/", pend,
                  tokenstr[0] != '\0' ? "?" : "", tokenstr) > 0 &&

         (rc = update_httplive_indexfile(pConn, pStreamerCfg, url, outidx,
                                         (char *) buf, sizeof(buf))) > 0) {

        VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_httplive sending substituted index file with media: '%s'"
               ", hostpath: '%s', virtFilePath: '%s' ( ext: '%s', count: %d) "), 
               url, hostpath, virtFilePath, pend, pConn->pCfg->pHttpLiveDatas[0]->count));

        rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_OK, buf, rc);

      } else {
        //
        // Generate an index.html document 
        //
        if((rc = httplive_getindexhtml(hostpath, virtFilePath, pend, presp, &lenresp)) >= 0) {

          VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_httplive sending auto-generated index file")));

          rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_OK, presp, lenresp); 

        } else {
          LOG(X_WARNING("%s - HTTPLive not available"), HTTP_STATUS_STR_NOTFOUND);
            *pHttpStatus = HTTP_STATUS_SERVERERROR;
            rc = -1;
        }
      }

    }

    return rc;
  }

  //
  // Check the URL_CAP_TSHTTPLIVE capability because it is not necessary if only 
  // returning the main index html 
  //
  if(!(pConn->pListenCfg->urlCapabilities & URL_CAP_TSHTTPLIVE)) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  }

  if(!filePath) {
    filePath = pargpath;
  }

  if(rc >= 0) {
    mediadb_fixdirdelims((char *) filePath, DIR_DELIMETER);
  }

  if(rc >= 0 && mediadb_prepend_dir(pConn->pCfg->pHttpLiveDatas[outidx]->dir, filePath, path, sizeof(path)) < 0) {
    *pHttpStatus = HTTP_STATUS_NOTFOUND;
    rc = -1;
  }

  if(rc >= 0) {
    VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_httplive sending media file: '%s' "), path));
    rc = send_mediafile(pConn, path, pHttpStatus, NULL);
  }

  return rc;
}


int srv_ctrl_flv(CLIENT_CONN_T *pConn, 
                 const char *pargfile,  
                 int is_remoteargfile, 
                 const SRV_LISTENER_CFG_T *pListenMedia, 
                 char *urloutbuf) {

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_flv file: '%s', is_remoteargfile: %d, connection: %s, listener-netflags: 0x%x"), pargfile, is_remoteargfile, (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? "ssl" : "plain-text", pListenMedia ? pListenMedia->netflags : 0););

  //
  // Return the rsrc/http_embed.html file with all substitions
  //
  return  resp_index_file(pConn, pargfile, is_remoteargfile, pListenMedia, URL_CAP_FLVLIVE, urloutbuf);
}

int srv_ctrl_mkv(CLIENT_CONN_T *pConn, 
                 const char *pargfile, 
                 int is_remoteargfile, 
                 const SRV_LISTENER_CFG_T *pListenMedia,
                 char *urloutbuf) {

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_mkv file: '%s', is_remoteargfile: %d, connection: %s, listener-netflags: 0x%x"), pargfile, is_remoteargfile, (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? "ssl" : "plain-text", pListenMedia ? pListenMedia->netflags : 0););


  //
  // Return the rsrc/mkv_embed.html file with all substitions
  //
  return resp_index_file(pConn, pargfile, is_remoteargfile, pListenMedia, URL_CAP_MKVLIVE, urloutbuf);

}

static int srv_ctrl_rsrc(CLIENT_CONN_T *pConn,
                         const char *pargfile,
                         int is_remoteargfile,
                         const char *rsrcUrl,
                         const SRV_LISTENER_CFG_T *pListenRtmp,
                         unsigned int outidx) {
  int rc = 0;
  char path[VSX_MAX_PATH_LEN];
  size_t sz = 0;
  const char *contentType = NULL;
  STREAMER_CFG_T *pStreamerCfg = NULL;

  pStreamerCfg = GET_STREAMER_FROM_CONN(pConn);

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_rsrc file: '%s', is_remoteargfile: %d, outidx: %d"),
                             pargfile, is_remoteargfile, outidx));

  if(pargfile && pargfile[0] != '\0') {

    //
    // Return a static file from the html directory
    //

    if(mediadb_prepend_dir2(pConn->pCfg->pMediaDb->homeDir,
           VSX_RSRC_HTML_PATH, pargfile, (char *) path, sizeof(path)) < 0) {
      rc = http_resp_send(&pConn->sd, pConn->phttpReq, HTTP_STATUS_NOTFOUND,
         (unsigned char *) HTTP_STATUS_STR_NOTFOUND, strlen(HTTP_STATUS_STR_NOTFOUND));

    } else {

      if((sz = strlen(path)) > 5) {
        if(strncasecmp(&path[sz - 4], ".swf", 4) == 0) {
          contentType = CONTENT_TYPE_SWF;
        } else if(strncasecmp(&path[sz - 5], ".html", 5) == 0) {
          contentType = CONTENT_TYPE_TEXTHTML;
        }
      }
      //fprintf(stderr, "path:'%s' ct:'%s'\n", path, contentType);
      rc = http_resp_sendfile(pConn, NULL, path, contentType, HTTP_ETAG_AUTO);

    }

    return rc;
  } else {

    //
    // Return the rsrc/rtmp_embed.html file with all substitions
    //
    return resp_index_file(pConn, pargfile, is_remoteargfile, pListenRtmp, 0, NULL);
  }

  return rc;
}


static STREAM_METHOD_T getBestMethod(const STREAM_DEVICE_T *pdevtype, const CLIENT_CONN_T *pConn) {
  unsigned int idx;
  const STREAMER_CFG_T *pStreamerCfg = NULL;

  if(pConn->pStreamerCfg0) {
    pStreamerCfg = pConn->pStreamerCfg0;
  } else if(pConn->pStreamerCfg1) {
    pStreamerCfg = pConn->pStreamerCfg1;
  } else {
    return pdevtype->methods[0];
  }

  //
  // Check if the preferred streaming output method is available
  //
  for(idx = 0; idx < STREAM_DEVICE_METHODS_MAX; idx++) {

    if(!((pdevtype->methods[idx] == STREAM_METHOD_MKVLIVE && !pStreamerCfg->action.do_mkvlive) ||
       (pdevtype->methods[idx] == STREAM_METHOD_FLVLIVE && !pStreamerCfg->action.do_flvlive) ||
       (pdevtype->methods[idx] == STREAM_METHOD_RTMP && !pStreamerCfg->action.do_rtmplive) ||
       (pdevtype->methods[idx] == STREAM_METHOD_HTTPLIVE && !pStreamerCfg->action.do_httplive) ||
       (pdevtype->methods[idx] == STREAM_METHOD_TSLIVE && !pStreamerCfg->action.do_tslive) ||
       ((pdevtype->methods[idx] == STREAM_METHOD_RTSP ||
         pdevtype->methods[idx] == STREAM_METHOD_RTSP_INTERLEAVED ||
         pdevtype->methods[idx] == STREAM_METHOD_RTSP_HTTP) && !pStreamerCfg->action.do_rtsplive) ||
       (pdevtype->methods[idx] == STREAM_METHOD_DASH && !pStreamerCfg->action.do_moofliverecord))) {
      break;
    }
  }

  if(idx >= STREAM_DEVICE_METHODS_MAX || (idx > 0 && (pdevtype->methods[idx] == STREAM_METHOD_UNKNOWN ||
     pdevtype->methods[idx] == STREAM_METHOD_INVALID))) {
    idx--;
  }

  return pdevtype->methods[idx];
}

int srv_ctrl_live(CLIENT_CONN_T *pConn, HTTP_STATUS_T *pHttpStatus, const char *accessUri) {
  int rc = 0;
  const char *pargfile = NULL;
  const char *parg = NULL;
  const char *p;
  int have_outidx = 0;
  const STREAM_DEVICE_T *pdevtype = NULL;
  size_t sz = 0;
  char tmp[128];
  int check_token = 1;
  STREAM_METHOD_T streamMethod = STREAM_METHOD_UNKNOWN;

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_live uri: '%s', urlCap: 0x%x"), 
                             pConn->phttpReq->puri, pConn->pListenCfg->urlCapabilities);); 

  //rsrcurl[0] = '\0';

  //
  // Validate & Authenticate the request
  //
  if((rc = http_check_pass(pConn)) < 0) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return rc;
  }

  if(!strncasecmp(pConn->phttpReq->puri, VSX_RTSP_URL, strlen(VSX_RTSP_URL))) {

    streamMethod = STREAM_METHOD_RTSP;
    sz = strlen(VSX_RTSP_URL);
    if(!accessUri) {
      accessUri = VSX_RTSP_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_RTMPT_URL, strlen(VSX_RTMPT_URL))) {

    streamMethod = STREAM_METHOD_RTMPT;
    sz = strlen(VSX_RTMPT_URL);
    if(!accessUri) {
      accessUri = VSX_RTMPT_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_RTMP_URL, strlen(VSX_RTMP_URL))) {

    streamMethod = STREAM_METHOD_RTMP;
    sz = strlen(VSX_RTMP_URL);
    if(!accessUri) {
      accessUri = VSX_RTMP_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_FLV_URL, strlen(VSX_FLV_URL))) {

    streamMethod = STREAM_METHOD_FLVLIVE;
    sz = strlen(VSX_FLV_URL);
    if(!accessUri) {
      accessUri = VSX_FLV_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_MKV_URL, strlen(VSX_MKV_URL))) {

    streamMethod = STREAM_METHOD_MKVLIVE;
    sz = strlen(VSX_MKV_URL);
    if(!accessUri) {
      accessUri = VSX_MKV_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_WEBM_URL, strlen(VSX_WEBM_URL))) {

    streamMethod = STREAM_METHOD_MKVLIVE;
    sz = strlen(VSX_WEBM_URL);
    if(!accessUri) {
      accessUri = VSX_WEBM_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_HTTPLIVE_URL, strlen(VSX_HTTPLIVE_URL))) {

    streamMethod = STREAM_METHOD_HTTPLIVE;
    sz = strlen(VSX_HTTPLIVE_URL);
    if(!accessUri) {
      accessUri = VSX_HTTPLIVE_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_DASH_URL, strlen(VSX_DASH_URL))) {

    streamMethod = STREAM_METHOD_DASH;
    sz = strlen(VSX_DASH_URL);
    if(!accessUri) {
      accessUri = VSX_DASH_URL;
    }

  } else if(!strncasecmp(pConn->phttpReq->puri, VSX_RSRC_URL, strlen(VSX_RSRC_URL))) {

    streamMethod = STREAM_METHOD_NONE;
    sz = strlen(VSX_RSRC_URL);
    if(!accessUri) {
      accessUri = VSX_RSRC_URL;
    }
    check_token = 0;

  } else {

    sz = strlen(VSX_LIVE_URL);
    if(!accessUri) {
      accessUri = VSX_LIVE_URL;
    }

  }

  VSX_DEBUG_LIVE(LOG(X_DEBUG("LIVE - srv_ctrl_live uri: '%s', urlCap: 0x%x, accessUri: '%s', streamMethod: %s"), 
      pConn->phttpReq->puri, pConn->pListenCfg->urlCapabilities, accessUri, devtype_methodstr(streamMethod));); 

  if(check_token && srv_check_authtoken(pConn->pListenCfg, pConn->phttpReq, 
                                        streamMethod == STREAM_METHOD_FLVLIVE ? 1 : 0) != 0) {
    *pHttpStatus = HTTP_STATUS_FORBIDDEN;
    return -1;
  }

  if(sz <= strlen(pConn->phttpReq->puri)) { 
    pargfile = &pConn->phttpReq->puri[sz];

    while(*pargfile == '/') {
      pargfile++;
    }

    //
    // Check if the URL contains an xcode outidx, such as '/live/2' similar to outfmt_getoutidx
    //
    p = pargfile;
    while(*p != '/' && *p != '\0') {
      if(*p >= '0' && *p <= '9') {
        have_outidx = 1;
      } else {
        have_outidx = 0;
        break;
      }
      p++;
    }

    if(have_outidx) {
      while(*p== '/') {
        p++;
      }
      pargfile = p;

    }

  } else {
    rc = -1;
  }

  //fprintf(stderr, "URL...'%s' '%s' have_outidx:%d\n", pConn->phttpReq->puri, pargfile ? pargfile : "<NULL>", have_outidx);

  if(pConn->pCfg->livepwd &&
   (!(parg = conf_find_keyval((const KEYVAL_PAIR_T *) &pConn->phttpReq->uriPairs, "pass")) ||
     strcmp(pConn->pCfg->livepwd, parg))) {

    LOG(X_WARNING("live invalid password from %s:%d (%d char given)"),
       FORMAT_NETADDR(pConn->sd.sa, tmp, sizeof(tmp)), ntohs(INET_PORT(pConn->sd.sa)),
       parg ? strlen(parg) : 0);

    rc = http_resp_error(&pConn->sd, pConn->phttpReq, HTTP_STATUS_FORBIDDEN, 0, NULL, NULL);
    return -1;
  }

  //
  // If the URL does not contain the desired access method (such as /live), get the 
  // User-Agent and lookup the device type to get the streaming method
  //
  if(streamMethod == STREAM_METHOD_UNKNOWN &&
     (parg = conf_find_keyval((const KEYVAL_PAIR_T *) &pConn->phttpReq->reqPairs,
                                    HTTP_HDR_USER_AGENT))) {

    //parg = "Mozilla/5.0 (compatible; MSIE 9.0; Windows Phone OS 7.5; Trident/5.0; IEMobile/9.0; SAMSUNG; SGH-i937)";

    if(!(pdevtype = devtype_finddevice(parg, 1))) {
      LOG(X_ERROR("No device type found for "HTTP_HDR_USER_AGENT": '%s'"), parg);
      rc = http_resp_error(&pConn->sd, pConn->phttpReq, HTTP_STATUS_SERVERERROR,
                             0, NULL, NULL);
      return -1;
    }

    streamMethod = getBestMethod(pdevtype, pConn);

    LOG(X_DEBUG("Found streaming method: '%s' devname: '%s' for "HTTP_HDR_USER_AGENT": '%s'"), 
               devtype_methodstr(streamMethod), pdevtype->name, parg);
  }



  //
  // Deliver the content via the desired access method
  //
  switch(streamMethod) {

    case STREAM_METHOD_HTTPLIVE:

      if(pConn->pListenCfg->urlCapabilities & (URL_CAP_LIVE | URL_CAP_TSHTTPLIVE)) {
        return srv_ctrl_httplive(pConn, accessUri, NULL, NULL, pHttpStatus);
      } else {
        *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      }
      break;

    case STREAM_METHOD_DASH:

      if(pConn->pListenCfg->urlCapabilities & (URL_CAP_LIVE | URL_CAP_MOOFLIVE)) {
        return srv_ctrl_mooflive(pConn, accessUri, NULL, NULL, pHttpStatus);
      } else {
        *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      }
      break;

    case STREAM_METHOD_TSLIVE:

      if(pConn->pListenCfg->urlCapabilities & URL_CAP_TSLIVE) {
        return srv_ctrl_tslive(pConn, pHttpStatus);
      } else {
        *pHttpStatus = HTTP_STATUS_FORBIDDEN;
      }
      break;

    case STREAM_METHOD_RTSP:
    case STREAM_METHOD_RTSP_INTERLEAVED:
    case STREAM_METHOD_RTSP_HTTP:

      rc = srv_ctrl_rtsp(pConn, pargfile, 0, NULL);
      break;


    case STREAM_METHOD_FLVLIVE:

      rc = srv_ctrl_flv(pConn, pargfile, 0, NULL, NULL);
      break;

    case STREAM_METHOD_MKVLIVE:

      rc = srv_ctrl_mkv(pConn, pargfile, 0, NULL, NULL);
      break;

    case STREAM_METHOD_PROGDOWNLOAD:
    case STREAM_METHOD_FLASHHTTP:

      LOG(X_ERROR("Streaming method '%s' not suitable for live delivery."), devtype_methodstr(streamMethod));
      rc = -1;
      break;

    case STREAM_METHOD_RTMP:
    case STREAM_METHOD_RTMPT:

      //
      // URL /rtmp is intentionally ambiguous to enable RTMP or RTMPT
      //
      rc = srv_ctrl_rtmp(pConn, pargfile, 0, NULL, NULL, 0, streamMethod);
      break;

    case STREAM_METHOD_NONE:
    default:

      rc = srv_ctrl_rsrc(pConn, pargfile, 0, NULL, NULL, 0);
      break;
  }

  return rc;
}

SRV_REQ_PEEK_TYPE_T srv_ctrl_peek(CLIENT_CONN_T *pConn, HTTP_PARSE_CTXT_T *pHdrCtxt) {

  SRV_REQ_PEEK_TYPE_T rc = SRV_REQ_PEEK_TYPE_UNKNOWN;
  char tmps[2][128];
  int is_rtmpt = 0;

  if(!pConn || !pHdrCtxt || !pHdrCtxt->pbuf) {
    return SRV_REQ_PEEK_TYPE_INVALID;
  }

  VSX_DEBUG_HTTP( LOG(X_DEBUGV("HTTP - srv_cmd_proc calling netio_recvnb_exact %d ..."), SRV_REQ_PEEK_SIZE); );

  //
  // Read the first 16 bytes to see if the connection is HTTP, RTMP or RTMPT
  //
  if(SRV_REQ_PEEK_SIZE > 0) {

    if((rc = netio_recvnb_exact(&pConn->sd.netsocket, (unsigned char *) pHdrCtxt->pbuf,
                                SRV_REQ_PEEK_SIZE, HTTP_REQUEST_TIMEOUT_SEC * 1000)) != SRV_REQ_PEEK_SIZE) {

      if(NETIOSOCK_FD(pConn->sd.netsocket) != INVALID_SOCKET) {
        if(rc == 0 && STUNSOCK(pConn->sd.netsocket).rcvclosed) {
          pHdrCtxt->rcvclosed = 1;
        } else {
          rc = http_resp_error(&pConn->sd, pConn->phttpReq, HTTP_STATUS_BADREQUEST, 1, NULL, NULL);
        }
      }
      return SRV_REQ_PEEK_TYPE_INVALID;
    }

    VSX_DEBUG_HTTP(
        LOG(X_DEBUG("HTTP - srv_cmd_proc recv (peek): %d"), SRV_REQ_PEEK_SIZE);
        LOGHEXT_DEBUG(pHdrCtxt->pbuf, SRV_REQ_PEEK_SIZE); );

    //
    // Check if the connection is for RTMP or RTMPT
    //
    if((is_rtmpt = rtmpt_istunneled((unsigned char *) pHdrCtxt->pbuf, SRV_REQ_PEEK_SIZE)) ||
        pHdrCtxt->pbuf[0] == RTMP_HANDSHAKE_HDR) {
      return is_rtmpt ? SRV_REQ_PEEK_TYPE_RTMPT : SRV_REQ_PEEK_TYPE_RTMP;
    } else if(rtsp_isrtsp((unsigned char *) pHdrCtxt->pbuf, SRV_REQ_PEEK_SIZE)) {
      return SRV_REQ_PEEK_TYPE_RTSP;
    } else if((pConn->pListenCfg->urlCapabilities & ~(URL_CAP_RTMPLIVE | URL_CAP_RTMPTLIVE |
                                                      URL_CAP_RTSPLIVE)) == 0) {

      LOG(X_ERROR("Listener %s:%d not enabled for http%s method to %s:%d"),
          FORMAT_NETADDR(pConn->pListenCfg->sa, tmps[1], sizeof(tmps[1])), ntohs(INET_PORT(pConn->pListenCfg->sa)),
          (pConn->sd.netsocket.flags & NETIO_FLAG_SSL_TLS) ? "s" : "",
          FORMAT_NETADDR(pConn->sd.sa, tmps[0], sizeof(tmps[0])), ntohs(INET_PORT(pConn->sd.sa)));
      LOGHEXT_DEBUG(pHdrCtxt->pbuf, SRV_REQ_PEEK_SIZE);
      return SRV_REQ_PEEK_TYPE_INVALID;
    }

  }

  HTTP_PARSE_CTXT_RESET(*pHdrCtxt);
  pHdrCtxt->idxbuf = SRV_REQ_PEEK_SIZE;

  return SRV_REQ_PEEK_TYPE_HTTP;
}
