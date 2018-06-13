/*
 *
 * Copyright (c) 2018 The University of Waikato, Hamilton, New Zealand.
 * All rights reserved.
 *
 * This file is part of OpenLI.
 *
 * This code has been developed by the University of Waikato WAND
 * research group. For further information please see http://www.wand.net.nz/
 *
 * OpenLI is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenLI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 */

#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libtrace_parallel.h>
#include <assert.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "etsili_core.h"
#include "collector.h"
#include "collector_sync_voip.h"
#include "collector_export.h"
#include "configparser.h"
#include "logger.h"
#include "intercept.h"
#include "netcomms.h"
#include "util.h"
#include "ipmmiri.h"


static inline void push_single_voipstreamintercept(libtrace_message_queue_t *q,
        rtpstreaminf_t *orig) {

    rtpstreaminf_t *copy;
    openli_pushed_t msg;

    copy = deep_copy_rtpstream(orig);
    if (!copy) {
        logger(LOG_DAEMON,
                "OpenLI: unable to copy RTP stream in sync thread.");
        return;
    }

    memset(&msg, 0, sizeof(openli_pushed_t));
    msg.type = OPENLI_PUSH_IPMMINTERCEPT;
    msg.data.ipmmint = copy;

    libtrace_message_queue_put(q, (void *)(&msg));
}

static void push_halt_active_voipstreams(libtrace_message_queue_t *q,
        voipintercept_t *vint, int epollfd) {

    rtpstreaminf_t *cin = NULL;
    char *streamdup;
    openli_pushed_t msg;

    if (vint->active_cins == NULL) {
        return;
    }

    for (cin = vint->active_cins; cin != NULL; cin=cin->hh.next) {
        if (cin->active == 0) {
            continue;
        }
        streamdup = strdup(cin->streamkey);
        memset(&msg, 0, sizeof(openli_pushed_t));
        msg.type = OPENLI_PUSH_HALT_IPMMINTERCEPT;
        msg.data.rtpstreamkey = streamdup;

        libtrace_message_queue_put(q, (void *)(&msg));

        /* If we were already about to time this intercept out, make sure
         * we kill the timer.
         */
        if (cin->timeout_ev) {
            struct epoll_event ev;
            sync_epoll_t *timerev = (sync_epoll_t *)(cin->timeout_ev);
            if (epoll_ctl(epollfd, EPOLL_CTL_DEL, timerev->fd, &ev) == -1) {
                logger(LOG_DAEMON, "OpenLI: unable to remove RTP stream timeout event for %s from epoll: %s",
                        cin->streamkey, strerror(errno));
            }
            close(timerev->fd);
            free(timerev);
            cin->timeout_ev = NULL;

        }
    }
}

void push_voipintercept_halt_to_threads(collector_sync_t *sync,
        voipintercept_t *vint) {

    sync_sendq_t *sendq, *tmp;

    HASH_ITER(hh, (sync_sendq_t *)(sync->glob->syncsendqs), sendq, tmp) {
        push_halt_active_voipstreams(sendq->q, vint,
                sync->glob->sync_epollfd);
    }
}

void push_all_active_voipstreams(libtrace_message_queue_t *q,
        voipintercept_t *vint) {

    rtpstreaminf_t *cin = NULL;

    if (vint->active_cins == NULL) {
        return;
    }

    for (cin = vint->active_cins; cin != NULL; cin=cin->hh.next) {
        if (cin->active == 0) {
            continue;
        }

        push_single_voipstreamintercept(q, cin);
    }

}

static int update_rtp_stream(collector_sync_t *sync, rtpstreaminf_t *rtp,
        voipintercept_t *vint, char *ipstr, char *portstr, uint8_t dir) {

    uint32_t port;
    struct sockaddr_storage *saddr;
    int family, i;
    int updaterequired = 1;
    sync_sendq_t *sendq, *tmp;

    errno = 0;
    port = strtoul(portstr, NULL, 0);

    if (errno != 0 || port > 65535) {
        logger(LOG_DAEMON, "OpenLI: invalid RTP port number: %s", portstr);
        return -1;
    }

    convert_ipstr_to_sockaddr(ipstr, &(saddr), &(family));

    /* If we get here, the RTP stream is not in our list. */
    if (dir == ETSI_DIR_FROM_TARGET) {
        if (rtp->targetaddr) {
            /* has the address or port changed? should we warn? */
            free(rtp->targetaddr);
        }
        rtp->ai_family = family;
        rtp->targetaddr = saddr;
        rtp->targetport = (uint16_t)port;

    } else {
        if (rtp->otheraddr) {
            /* has the address or port changed? should we warn? */
            free(rtp->otheraddr);
        }
        rtp->ai_family = family;
        rtp->otheraddr = saddr;
        rtp->otherport = (uint16_t)port;
    }

    /* Not got the full 5-tuple for the RTP stream yet */
    if (!rtp->targetaddr || !rtp->otheraddr) {
        return 0;
    }

    if (!updaterequired) {
        return 0;
    }
    /* If we get here, we need to push the RTP stream details to the
     * processing threads. */
    HASH_ITER(hh, (sync_sendq_t *)(sync->glob->syncsendqs), sendq, tmp) {
        if (rtp->active == 0) {
            push_single_voipstreamintercept(sendq->q, rtp);
        }
    }
    rtp->active = 1;
    return 0;
}

/* TODO very similar to code in intercept.c */
static inline void remove_cin_callid_from_map(voipcinmap_t **cinmap,
        char *callid) {

    voipcinmap_t *c;
    HASH_FIND(hh_callid, *cinmap, callid, strlen(callid), c);
    if (c) {
        HASH_DELETE(hh_callid, *cinmap, c);
        free(c->callid);
        free(c);
    }
}

static inline voipcinmap_t *update_cin_callid_map(voipcinmap_t **cinmap,
        char *callid, voipintshared_t *vshared) {

    voipcinmap_t *newcinmap;

    newcinmap = (voipcinmap_t *)malloc(sizeof(voipcinmap_t));
    if (!newcinmap) {
        logger(LOG_DAEMON,
                "OpenLI: out of memory in collector_sync thread.");
        return NULL;
    }
    newcinmap->callid = strdup(callid);
    newcinmap->shared = vshared;
    if (newcinmap->shared) {
        newcinmap->shared->refs ++;
    }

    HASH_ADD_KEYPTR(hh_callid, *cinmap, newcinmap->callid,
            strlen(newcinmap->callid), newcinmap);
    return newcinmap;
}

static inline voipsdpmap_t *update_cin_sdp_map(voipintercept_t *vint,
        sip_sdp_identifier_t *sdpo, voipintshared_t *vshared) {

    voipsdpmap_t *newsdpmap;

    newsdpmap = (voipsdpmap_t *)malloc(sizeof(voipsdpmap_t));
    if (!newsdpmap) {
        logger(LOG_DAEMON,
                "OpenLI: out of memory in collector_sync thread.");
        return NULL;
    }
    newsdpmap->sdpkey.sessionid = sdpo->sessionid;
    newsdpmap->sdpkey.version = sdpo->version;
    newsdpmap->shared = vshared;
    if (newsdpmap->shared) {
        newsdpmap->shared->refs ++;
    }

    HASH_ADD_KEYPTR(hh_sdp, vint->cin_sdp_map, &(newsdpmap->sdpkey),
            sizeof(sip_sdp_identifier_t), newsdpmap);

    return newsdpmap;
}

static int create_new_voipcin(rtpstreaminf_t **activecins, uint32_t cin_id,
        voipintercept_t *vint) {

    rtpstreaminf_t *newcin;

    newcin = create_rtpstream(vint, cin_id);

    if (!newcin) {
        logger(LOG_DAEMON,
                "OpenLI: out of memory while creating new RTP stream");
        return -1;
    }
    HASH_ADD_KEYPTR(hh, *activecins, newcin->streamkey,
            strlen(newcin->streamkey), newcin);
    return 0;

}

static int sipid_matches_target(libtrace_list_t *targets,
        openli_sip_identity_t *sipid) {

    libtrace_list_node_t *n;

    n = targets->head;
    while (n) {
        openli_sip_identity_t *x = *((openli_sip_identity_t **) (n->data));
        n = n->next;

        if (strcmp(x->username, sipid->username) != 0) {
            continue;
        }

        if (x->realm == NULL || strcmp(x->realm, sipid->realm) == 0) {
            return 1;
        }
    }
    return 0;
}

static inline int lookup_sip_callid(collector_sync_t *sync, char *callid) {

    voipcinmap_t *lookup;

    HASH_FIND(hh_callid, sync->knowncallids, callid, strlen(callid), lookup);
    if (!lookup) {
        return 0;
    }
    return 1;
}

static voipintshared_t *create_new_voip_session(collector_sync_t *sync,
        char *callid, sip_sdp_identifier_t *sdpo, voipintercept_t *vint) {

    voipintshared_t *vshared = NULL;
    uint32_t cin_id = 0;

    cin_id = hashlittle(callid, strlen(callid), 0xbeefface);
    if (create_new_voipcin(&(vint->active_cins), cin_id, vint) == -1) {
        return NULL;
    }

    vshared = (voipintshared_t *)malloc(sizeof(voipintshared_t));
    vshared->cin = cin_id;
    vshared->iriseqno = 0;
    vshared->refs = 0;

    if (update_cin_callid_map(&(vint->cin_callid_map), callid,
                vshared) == NULL) {
        free(vshared);
        return NULL;
    }

    if (update_cin_callid_map(&(sync->knowncallids), callid, NULL) == NULL) {
        remove_cin_callid_from_map(&(vint->cin_callid_map), callid);
        free(vshared);
        return NULL;
    }

    if (sdpo->sessionid != 0 || sdpo->version != 0) {
        if (update_cin_sdp_map(vint, sdpo, vshared) == NULL) {
            remove_cin_callid_from_map(&(vint->cin_callid_map), callid);
            remove_cin_callid_from_map(&(sync->knowncallids), callid);

            free(vshared);
            return NULL;
        }
    }
    return vshared;
}

static inline voipintshared_t *check_sip_auth_fields(collector_sync_t *sync,
        voipintercept_t *vint, char *callid, sip_sdp_identifier_t *sdpo,
        uint8_t isproxy) {

    int i, authcount, ret;
    openli_sip_identity_t authid;
    voipintshared_t *vshared = NULL;

    i = authcount = 0;
    do {
        if (isproxy) {
            ret = get_sip_proxy_auth_identity(sync->sipparser, i, &authcount,
                    &authid);
        } else {
            ret = get_sip_auth_identity(sync->sipparser, i, &authcount,
                    &authid);
        }

        if (ret == -1) {
            break;
        }
        if (ret > 0) {
            if (sipid_matches_target(vint->targets, &authid)) {
                vshared = create_new_voip_session(sync, callid, sdpo,
                        vint);
                break;
            }
        }
        i ++;
    } while (i < authcount);
    return vshared;
}

static int process_sip_183sessprog(collector_sync_t *sync,
        rtpstreaminf_t *thisrtp, voipintercept_t *vint,
        etsili_iri_type_t *iritype) {

    char *cseqstr, *ipstr, *portstr;

    cseqstr = get_sip_cseq(sync->sipparser);

    if (thisrtp->invitecseq && strcmp(thisrtp->invitecseq,
                cseqstr) == 0) {

        ipstr = get_sip_media_ipaddr(sync->sipparser);
        portstr = get_sip_media_port(sync->sipparser);

        if (ipstr && portstr) {
            if (update_rtp_stream(sync, thisrtp, vint, ipstr,
                        portstr, 1) == -1) {
                logger(LOG_DAEMON,
                        "OpenLI: error adding new RTP stream for LIID %s (%s:%s)",
                        vint->common.liid, ipstr, portstr);
                free(cseqstr);
                return -1;
            }
            free(thisrtp->invitecseq);
            thisrtp->invitecseq = NULL;
        }
    }
    free(cseqstr);
    return 0;
}

static int process_sip_200ok(collector_sync_t *sync, rtpstreaminf_t *thisrtp,
        voipintercept_t *vint, etsili_iri_type_t *iritype) {

    char *ipstr, *portstr, *cseqstr;

    cseqstr = get_sip_cseq(sync->sipparser);

    if (thisrtp->invitecseq && strcmp(thisrtp->invitecseq,
                cseqstr) == 0) {

        ipstr = get_sip_media_ipaddr(sync->sipparser);
        portstr = get_sip_media_port(sync->sipparser);

        if (ipstr && portstr) {
            if (update_rtp_stream(sync, thisrtp, vint, ipstr,
                        portstr, 1) == -1) {
                logger(LOG_DAEMON,
                        "OpenLI: error adding new RTP stream for LIID %s (%s:%s)",
                        vint->common.liid, ipstr, portstr);
                free(cseqstr);
                return -1;
            }
            free(thisrtp->invitecseq);
            thisrtp->invitecseq = NULL;
        }
    } else if (thisrtp->byecseq && strcmp(thisrtp->byecseq,
                cseqstr) == 0 && thisrtp->byematched == 0) {
        sync_epoll_t *timeout = (sync_epoll_t *)calloc(1,
                sizeof(sync_epoll_t));

        /* Call for this session should be over */
        thisrtp->timeout_ev = (void *)timeout;
        timeout->fdtype = SYNC_EVENT_SIP_TIMEOUT;
        timeout->fd = epoll_add_timer(sync->glob->sync_epollfd,
                30, timeout);
        timeout->ptr = thisrtp;

        thisrtp->byematched = 1;
        *iritype = ETSILI_IRI_END;
    }
    free(cseqstr);
    return 0;
}

static int process_sip_other(collector_sync_t *sync, char *callid,
        sip_sdp_identifier_t *sdpo, libtrace_packet_t *pkt) {

    voipintercept_t *vint, *tmp;
    voipcinmap_t *findcin;
    voipintshared_t *vshared;
    char rtpkey[256];
    rtpstreaminf_t *thisrtp;
    etsili_iri_type_t iritype = ETSILI_IRI_REPORT;
    int exportcount = 0;
    int ret;

    HASH_ITER(hh_liid, sync->voipintercepts, vint, tmp) {

        /* Is this call ID associated with this intercept? */
        HASH_FIND(hh_callid, vint->cin_callid_map, callid, strlen(callid),
                findcin);

        if (!findcin) {
            continue;
        }

        vshared = findcin->shared;

        snprintf(rtpkey, 256, "%s-%u", vint->common.liid, vshared->cin);
        HASH_FIND(hh, vint->active_cins, rtpkey, strlen(rtpkey), thisrtp);
        if (thisrtp == NULL) {
            logger(LOG_DAEMON,
                    "OpenLI: unable to find %u in the active call list for %s",
                    vshared->cin, vint->common.liid);
            continue;
        }

        /* Check for a new RTP stream announcement in a 200 OK */
        if (sip_is_200ok(sync->sipparser)) {
            if (process_sip_200ok(sync, thisrtp, vint, &iritype) < 0) {
                continue;
            }
        }

        /* Check for 183 Session Progress, as this can contain RTP info */
        if (sip_is_183sessprog(sync->sipparser)) {
            if (process_sip_183sessprog(sync, thisrtp, vint, &iritype) < 0) {
                continue;
            }
        }

        /* Check for a BYE */
        if (sip_is_bye(sync->sipparser) && !thisrtp->byematched) {
            if (thisrtp->byecseq) {
                free(thisrtp->byecseq);
            }
            thisrtp->byecseq = get_sip_cseq(sync->sipparser);
        }

        if (thisrtp->byematched && iritype != ETSILI_IRI_END) {
            /* All post-END IRIs must be REPORTs */
            iritype = ETSILI_IRI_REPORT;
        }

        /* Wrap this packet up in an IRI and forward it on to the exporter */
        ret = ipmm_iri(pkt, sync->glob, &(sync->encoder), &(sync->exportq),
                vint, vshared, iritype, OPENLI_IPMMIRI_SIP);
        if (ret == -1) {
            logger(LOG_DAEMON,
                    "OpenLI: error while trying to export IRI containing SIP packet.");
            return -1;
        }
        exportcount += ret;
    }
    return exportcount;

}

static int process_sip_invite(collector_sync_t *sync, char *callid,
        sip_sdp_identifier_t *sdpo, libtrace_packet_t *pkt) {

    voipintercept_t *vint, *tmp;
    voipcinmap_t *findcin;
    voipsdpmap_t *findsdp = NULL;
    voipintshared_t *vshared;
    openli_sip_identity_t touriid, authid;
    char rtpkey[256];
    rtpstreaminf_t *thisrtp;
    char *ipstr, *portstr, *cseqstr;
    int exportcount = 0;
    etsili_iri_type_t iritype = ETSILI_IRI_REPORT;
    int ret;

    if (get_sip_to_uri_identity(sync->sipparser, &touriid) < 0) {
        logger(LOG_DAEMON,
                "OpenLI: unable to derive SIP identity from To: URI");
        return -1;
    }

    HASH_ITER(hh_liid, sync->voipintercepts, vint, tmp) {
        vshared = NULL;

        /* Is this a call ID we've seen already? */
        HASH_FIND(hh_callid, vint->cin_callid_map, callid, strlen(callid),
                findcin);

        if (sdpo->version != 0 || sdpo->sessionid != 0) {
            HASH_FIND(hh_sdp, vint->cin_sdp_map, sdpo, sizeof(sdpo),
                    findsdp);
        }

        if (findcin) {
            if (findsdp) {
                assert(findsdp->shared->cin == findcin->shared->cin); // XXX
            } else if (sdpo->version != 0 || sdpo->sessionid != 0) {
                /* New session ID for this call ID */
                if (update_cin_sdp_map(vint, sdpo, findcin->shared) == NULL) {
                    // XXX ERROR

                }
            }

            vshared = findcin->shared;
            iritype = ETSILI_IRI_CONTINUE;

        } else if (findsdp) {
            /* New call ID but already seen this session */
            if (update_cin_callid_map(&(vint->cin_callid_map), callid,
                        findsdp->shared) == NULL) {
                // XXX ERROR
            }
            vshared = findsdp->shared;
            iritype = ETSILI_IRI_CONTINUE;

        } else {
            /* Doesn't match an existing intercept, but could match one of
             * our target identities */

            /* Try the To: uri first */
            if (sipid_matches_target(vint->targets, &touriid)) {

                vshared = create_new_voip_session(sync, callid, sdpo,
                        vint);
            } else {
                vshared = check_sip_auth_fields(sync, vint, callid, sdpo, 1);
                if (!vshared) {
                    vshared = check_sip_auth_fields(sync, vint, callid, sdpo,
                            0);
                }
            }
            iritype = ETSILI_IRI_BEGIN;
        }

        if (!vshared) {
            continue;
        }

        snprintf(rtpkey, 256, "%s-%u", vint->common.liid, vshared->cin);
        HASH_FIND(hh, vint->active_cins, rtpkey, strlen(rtpkey), thisrtp);
        if (thisrtp == NULL) {
            logger(LOG_DAEMON,
                    "OpenLI: unable to find %u in the active call list for %s",
                    vshared->cin, vint->common.liid);
            continue;
        }

        ipstr = get_sip_media_ipaddr(sync->sipparser);
        portstr = get_sip_media_port(sync->sipparser);

        if (ipstr && portstr) {
            if (update_rtp_stream(sync, thisrtp, vint, ipstr, portstr,
                        0) == -1) {
                logger(LOG_DAEMON,
                        "OpenLI: error adding new RTP stream for LIID %s (%s:%s)",
                        vint->common.liid, ipstr, portstr);
                continue;
            }
        }

        if (thisrtp->invitecseq != NULL) {
            free(thisrtp->invitecseq);
        }
        thisrtp->invitecseq = get_sip_cseq(sync->sipparser);


        /* Wrap this packet up in an IRI and forward it on to the exporter */
        ret = ipmm_iri(pkt, sync->glob, &(sync->encoder), &(sync->exportq),
                vint, vshared, iritype, OPENLI_IPMMIRI_SIP);
        if (ret == -1) {
            logger(LOG_DAEMON,
                    "OpenLI: error while trying to export IRI containing SIP packet.");
            continue;
        }
        exportcount += ret;
    }
    return exportcount;

}

int update_sip_state(collector_sync_t *sync, libtrace_packet_t *pkt) {

    char *callid, *sessid, *sessversion;
    openli_sip_identity_t authid, touriid;
    sip_sdp_identifier_t sdpo;
    int iserr = 0;
    int ret, authcount, i;

    callid = get_sip_callid(sync->sipparser);
    sessid = get_sip_session_id(sync->sipparser);
    sessversion = get_sip_session_version(sync->sipparser);

    if (callid == NULL) {
        iserr = 1;
        goto sipgiveup;
    }

    if (sessid != NULL) {
        errno = 0;
        sdpo.sessionid = strtoul(sessid, NULL, 0);
        if (errno != 0) {
            logger(LOG_DAEMON, "OpenLI: invalid session ID in SIP packet %s",
                    sessid);
            sessid = NULL;
            sdpo.sessionid = 0;
        }
    } else {
        sdpo.sessionid = 0;
    }

    if (sessversion != NULL) {
        errno = 0;
        sdpo.version = strtoul(sessversion, NULL, 0);
        if (errno != 0) {
            logger(LOG_DAEMON, "OpenLI: invalid version in SIP packet %s",
                    sessid);
            sessversion = NULL;
            sdpo.version = 0;
        }
    } else {
        sdpo.version = 0;
    }

    ret = 0;
    if (sip_is_invite(sync->sipparser)) {
        if ((ret = process_sip_invite(sync, callid, &sdpo, pkt)) < 0) {
            iserr = 1;
            goto sipgiveup;
        }
    } else if (lookup_sip_callid(sync, callid) != 0) {
        /* SIP packet matches a "known" call of interest */
        if ((ret = process_sip_other(sync, callid, &sdpo, pkt)) < 0) {
            iserr = 1;
            goto sipgiveup;
        }
    }

    if (ret > 0) {
        /* Increment ref count for the packet and send a packet fin message
         * so the exporter knows when to decrease the ref count */
        openli_export_recv_t msg;
        trace_increment_packet_refcount(pkt);
        memset(&msg, 0, sizeof(openli_export_recv_t));
        msg.type = OPENLI_EXPORT_PACKET_FIN;
        msg.data.packet = pkt;
        libtrace_message_queue_put(&(sync->exportq), (void *)(&msg));
        return 1;
    }
sipgiveup:

    if (iserr) {
        return -1;
    }
    return 0;

}

int halt_voipintercept(collector_sync_t *sync, uint8_t *intmsg,
        uint16_t msglen) {

    voipintercept_t *vint, torem;
    sync_sendq_t *sendq, *tmp;
    int i;

    if (decode_voipintercept_halt(intmsg, msglen, &torem) == -1) {
        logger(LOG_DAEMON,
                "OpenLI: received invalid VOIP intercept withdrawal from provisioner.");
        return -1;
    }

    HASH_FIND(hh_liid, sync->voipintercepts, torem.common.liid,
            torem.common.liid_len, vint);
    if (!vint) {
        logger(LOG_DAEMON,
                "OpenLI: received withdrawal for VOIP intercept %s but it is not present in the sync intercept list?",
                torem.common.liid);
        return 0;
    }

    logger(LOG_DAEMON, "OpenLI: sync thread withdrawing VOIP intercept %s",
            torem.common.liid);

    push_voipintercept_halt_to_threads(sync, vint);
    HASH_DELETE(hh_liid, sync->voipintercepts, vint);
    free_single_voipintercept(vint);
    return 0;
}

static int halt_single_rtpstream(collector_sync_t *sync, rtpstreaminf_t *rtp) {
    int i;

    struct epoll_event ev;
    voipcinmap_t *cin_callid, *tmp;
    voipsdpmap_t *cin_sdp, *tmp2;
    sync_sendq_t *sendq, *tmp3;

    if (rtp->timeout_ev) {
        sync_epoll_t *timerev = (sync_epoll_t *)(rtp->timeout_ev);
        if (epoll_ctl(sync->glob->sync_epollfd, EPOLL_CTL_DEL, timerev->fd,
                &ev) == -1) {
            logger(LOG_DAEMON, "OpenLI: unable to remove RTP stream timeout event for %s from epoll: %s",
                    rtp->streamkey, strerror(errno));
        }
        close(timerev->fd);
        free(timerev);
        rtp->timeout_ev = NULL;
    }


    if (rtp->active) {
        HASH_ITER(hh, (sync_sendq_t *)(sync->glob->syncsendqs), sendq, tmp3) {
           openli_pushed_t msg;
           memset(&msg, 0, sizeof(openli_pushed_t));
           msg.type = OPENLI_PUSH_HALT_IPMMINTERCEPT;
           msg.data.rtpstreamkey = strdup(rtp->streamkey);
           libtrace_message_queue_put(sendq->q, (void *)(&msg));
        }
    }

    HASH_DEL(rtp->parent->active_cins, rtp);

    /* TODO this is painful, maybe include reverse references in the shared
     * data structure?
     */
    HASH_ITER(hh_callid, rtp->parent->cin_callid_map, cin_callid, tmp) {
        if (cin_callid->shared->cin == rtp->cin) {
            HASH_DELETE(hh_callid, rtp->parent->cin_callid_map, cin_callid);
            free(cin_callid->callid);
            cin_callid->shared->refs --;
            if (cin_callid->shared->refs == 0) {
                free(cin_callid->shared);
                break;
            }
            free(cin_callid);
        }
    }

    HASH_ITER(hh_sdp, rtp->parent->cin_sdp_map, cin_sdp, tmp2) {
        int stop = 0;
        if (cin_sdp->shared->cin == rtp->cin) {
            HASH_DELETE(hh_sdp, rtp->parent->cin_sdp_map, cin_sdp);
            cin_sdp->shared->refs --;
            if (cin_sdp->shared->refs == 0) {
                free(cin_sdp->shared);
                stop = 1;
            }
            free(cin_sdp);
            if (stop) {
                break;
            }
        }
    }

    free_single_voip_cin(rtp);

    return 0;
}

static inline void disable_sip_target(voipintercept_t *vint,
        openli_sip_identity_t *sipid) {

    openli_sip_identity_t *newid, *iter;
    libtrace_list_node_t *n;

    n = vint->targets->head;
    while (n) {
        iter = *((openli_sip_identity_t **)(n->data));
        if (are_sip_identities_same(iter, sipid)) {
            iter->active = 0;
            iter->awaitingconfirm = 0;
            if (iter->realm) {
                logger(LOG_DAEMON,
                        "OpenLI: collector is withdrawing SIP target %s@%s for LIID %s.",
                        iter->username, iter->realm, vint->common.liid);
            } else {
                logger(LOG_DAEMON,
                        "OpenLI: collector is withdrawing SIP target %s@* for LIID %s.",
                        iter->username, vint->common.liid);
            }

            break;
        }
        n = n->next;
    }
}

static inline void add_new_sip_target_to_list(voipintercept_t *vint,
        openli_sip_identity_t *sipid) {

    openli_sip_identity_t *newid, *iter;
    libtrace_list_node_t *n;

    /* First, check if this ID is already in the list. If so, we can
     * just confirm it as being still active. If not, add it to the
     * list.
     *
     * TODO consider a hashmap instead if we often get more than 2 or
     * 3 targets per intercept?
     */
    n = vint->targets->head;
    while (n) {
        iter = *((openli_sip_identity_t **)(n->data));
        if (are_sip_identities_same(iter, sipid)) {
            if (iter->active == 0) {
                if (iter->realm) {
                    logger(LOG_DAEMON,
                            "OpenLI: collector re-enabled SIP target %s@%s for LIID %s.",
                            iter->username, iter->realm, vint->common.liid);
                } else {
                    logger(LOG_DAEMON,
                            "OpenLI: collector re-enabled SIP target %s@* for LIID %s.",
                            iter->username, vint->common.liid);
                }

                iter->active = 1;
            }
            iter->awaitingconfirm = 0;
            return;
        }
        n = n->next;
    }

    newid = (openli_sip_identity_t *)calloc(1, sizeof(openli_sip_identity_t));
    newid->realm = sipid->realm;
    newid->realm_len = sipid->realm_len;
    newid->username = sipid->username;
    newid->username_len = sipid->username_len;
    newid->awaitingconfirm = 0;
    newid->active = 1;

    sipid->realm = NULL;
    sipid->username = NULL;

    libtrace_list_push_back(vint->targets, &newid);

    if (newid->realm) {
        logger(LOG_DAEMON,
                "OpenLI: collector received new SIP target %s@%s for LIID %s.",
                newid->username, newid->realm, vint->common.liid);
    } else {
        logger(LOG_DAEMON,
                "OpenLI: collector received new SIP target %s@* for LIID %s.",
                newid->username, vint->common.liid);
    }

}

int new_voip_sip_target(collector_sync_t *sync, uint8_t *intmsg,
		uint16_t msglen) {

    voipintercept_t *vint;
    openli_sip_identity_t sipid;
    char liidspace[1024];

    if (decode_sip_target_announcement(intmsg, msglen, &sipid, liidspace,
            1024) < 0) {
        logger(LOG_DAEMON,
                "OpenLI: received invalid SIP target from provisioner.");
        return -1;
    }

    HASH_FIND(hh_liid, sync->voipintercepts, liidspace, strlen(liidspace),
            vint);
    if (!vint) {
        logger(LOG_DAEMON,
                "OpenLI: received SIP target for unknown VOIP LIID %s.",
                liidspace);
        return -1;
    }

    add_new_sip_target_to_list(vint, &sipid);
    return 0;
}


int withdraw_voip_sip_target(collector_sync_t *sync, uint8_t *intmsg,
        uint16_t msglen) {

    voipintercept_t *vint;
    openli_sip_identity_t sipid;
    char liidspace[1024];

    if (decode_sip_target_announcement(intmsg, msglen, &sipid, liidspace,
            1024) < 0) {
        logger(LOG_DAEMON,
                "OpenLI: received invalid SIP target withdrawal from provisioner.");
        return -1;
    }

    HASH_FIND(hh_liid, sync->voipintercepts, liidspace, strlen(liidspace),
            vint);
    if (!vint) {
        logger(LOG_DAEMON,
                "OpenLI: received SIP target withdrawal for unknown VOIP LIID %s.",
                liidspace);
        return -1;
    }

    disable_sip_target(vint, &sipid);
}

int new_voipintercept(collector_sync_t *sync, uint8_t *intmsg, uint16_t msglen)
{

    voipintercept_t *vint, toadd;
    sync_sendq_t *sendq, *tmp;
    int i;

    if (decode_voipintercept_start(intmsg, msglen, &toadd) == -1) {
        logger(LOG_DAEMON,
                "OpenLI: received invalid VOIP intercept from provisioner.");
        return -1;
    }

    HASH_FIND(hh_liid, sync->voipintercepts, toadd.common.liid,
            toadd.common.liid_len, vint);
    if (vint) {
        vint->internalid = toadd.internalid;
        vint->awaitingconfirm = 0;
        vint->active = 1;
        return 0;
    }

    vint = (voipintercept_t *)malloc(sizeof(voipintercept_t));
    memcpy(vint, &toadd, sizeof(voipintercept_t));
    logger(LOG_DAEMON,
            "OpenLI: received VOIP intercept %s from provisioner.",
            vint->common.liid);

    HASH_ADD_KEYPTR(hh_liid, sync->voipintercepts, vint->common.liid,
            vint->common.liid_len, vint);

    HASH_ITER(hh, (sync_sendq_t *)(sync->glob->syncsendqs), sendq, tmp) {
        /* Forward all active CINs to our collector threads */
        push_all_active_voipstreams(sendq->q, vint);

    }
    return 0;
}

void touch_all_voipintercepts(voipintercept_t *vints) {
    voipintercept_t *v;
    libtrace_list_node_t *n;
    openli_sip_identity_t *sipid;

    for (v = vints; v != NULL; v = v->hh_liid.next) {
        v->awaitingconfirm = 1;

        n = v->targets->head;
        while (n) {
            sipid = *((openli_sip_identity_t **)(n->data));
            if (sipid->active) {
                sipid->awaitingconfirm = 1;
            }
            n = n->next;
        }
    }
}



// vim: set sw=4 tabstop=4 softtabstop=4 expandtab :
