/*
 * Copyright (C) 2019 by Sukchan Lee <acetcom@gmail.com>
 *
 * This file is part of Open5GS.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pgw-event.h"
#include "pgw-context.h"
#include "pgw-gtp-path.h"
#include "pgw-fd-path.h"
#include "pgw-s5c-build.h"
#include "pgw-s5c-handler.h"

#include "ipfw/ipfw2.h"

void pgw_s5c_handle_create_session_request(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_create_session_request_t *req)
{
    int rv;
    uint8_t cause_value = 0;
    ogs_gtp_f_teid_t *sgw_s5c_teid, *sgw_s5u_teid;
    ogs_gtp_node_t *sgw = NULL;
    pgw_bearer_t *bearer = NULL;
    ogs_gtp_bearer_qos_t bearer_qos;
    ogs_gtp_ambr_t *ambr = NULL;
    ogs_gtp_uli_t uli;
    uint16_t decoded = 0;

    ogs_assert(xact);
    ogs_assert(req);

    ogs_debug("[PGW] Create Session Reqeust");

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    if (sess) {
        bearer = pgw_default_bearer_in_sess(sess);
        ogs_assert(bearer);
    }
    if (!bearer) {
        ogs_warn("No Context");
        cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
    }

    if (req->imsi.presence == 0) {
        ogs_error("No IMSI");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->sender_f_teid_for_control_plane.presence == 0) {
        ogs_error("No TEID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created.presence == 0) {
        ogs_error("No Bearer");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created.bearer_level_qos.presence == 0) {
        ogs_error("No EPS Bearer QoS");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->bearer_contexts_to_be_created.s5_s8_u_sgw_f_teid.presence == 0) {
        ogs_error("No TEID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (req->user_location_information.presence == 0) {
        ogs_error("No User Location Inforamtion");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(xact, sess ? sess->sgw_s5c_teid : 0,
                OGS_GTP_CREATE_SESSION_RESPONSE_TYPE, cause_value);
        ogs_pkbuf_free(gtpbuf);
        return;
    }
    
    /* Set IMSI */
    sess->imsi_len = req->imsi.len;
    memcpy(sess->imsi, req->imsi.data, sess->imsi_len);
    ogs_buffer_to_bcd(sess->imsi, sess->imsi_len, sess->imsi_bcd);

    /* Control Plane(DL) : SGW-S5C */
    sgw_s5c_teid = req->sender_f_teid_for_control_plane.data;
    ogs_assert(sgw_s5c_teid);
    sess->sgw_s5c_teid = ntohl(sgw_s5c_teid->teid);

    /* Control Plane(DL) : SGW-S5U */
    sgw_s5u_teid = req->bearer_contexts_to_be_created.s5_s8_u_sgw_f_teid.data;
    ogs_assert(sgw_s5u_teid);
    bearer->sgw_s5u_teid = ntohl(sgw_s5u_teid->teid);

    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    ogs_debug("    SGW_S5U_TEID[%d] PGW_S5U_TEID[%d]",
            bearer->sgw_s5u_teid, bearer->pgw_s5u_teid);

    sgw = ogs_gtp_node_find_by_f_teid(&pgw_self()->sgw_s5u_list, sgw_s5u_teid);
    if (!sgw) {
        sgw = ogs_gtp_node_add(&pgw_self()->sgw_s5u_list, sgw_s5u_teid,
            pgw_self()->gtpu_port,
            ogs_config()->parameter.no_ipv4,
            ogs_config()->parameter.no_ipv6,
            ogs_config()->parameter.prefer_ipv4);
        ogs_assert(sgw);

        rv = ogs_gtp_connect(
                pgw_self()->gtpu_sock, pgw_self()->gtpu_sock6, sgw);
        ogs_assert(rv == OGS_OK);
    }
    /* Setup GTP Node */
    OGS_SETUP_GTP_NODE(bearer, sgw);

    decoded = ogs_gtp_parse_bearer_qos(&bearer_qos,
        &req->bearer_contexts_to_be_created.bearer_level_qos);
    ogs_assert(req->bearer_contexts_to_be_created.bearer_level_qos.len ==
            decoded);
    sess->pdn.qos.qci = bearer_qos.qci;
    sess->pdn.qos.arp.priority_level = bearer_qos.priority_level;
    sess->pdn.qos.arp.pre_emption_capability =
                    bearer_qos.pre_emption_capability;
    sess->pdn.qos.arp.pre_emption_vulnerability =
                    bearer_qos.pre_emption_vulnerability;

    /* Set AMBR if available */
    if (req->aggregate_maximum_bit_rate.presence) {
        /*
         * Ch 8.7. Aggregate Maximum Bit Rate(AMBR) in TS 29.274 V15.9.0
         *
         * AMBR is defined in clause 9.9.4.2 of 3GPP TS 24.301 [23],
         * but it shall be encoded as shown in Figure 8.7-1 as
         * Unsigned32 binary integer values in kbps (1000 bits per second).
         */
        ambr = req->aggregate_maximum_bit_rate.data;
        sess->pdn.ambr.downlink = be32toh(ambr->downlink) * 1000;
        sess->pdn.ambr.uplink = be32toh(ambr->uplink) * 1000;
    }
    
    /* Set User Location Information */
    decoded = ogs_gtp_parse_uli(&uli, &req->user_location_information);
    ogs_assert(req->user_location_information.len == decoded);
    memcpy(&sess->tai.plmn_id, &uli.tai.plmn_id, sizeof(uli.tai.plmn_id));
    sess->tai.tac = uli.tai.tac;
    memcpy(&sess->e_cgi.plmn_id, &uli.e_cgi.plmn_id, sizeof(uli.e_cgi.plmn_id));
    sess->e_cgi.cell_id = uli.e_cgi.cell_id;

    pgw_gx_send_ccr(sess, xact, gtpbuf,
        OGS_DIAM_GX_CC_REQUEST_TYPE_INITIAL_REQUEST);
}

void pgw_s5c_handle_delete_session_request(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_pkbuf_t *gtpbuf, ogs_gtp_delete_session_request_t *req)
{
    ogs_debug("[PGW] Delete Session Request");

    ogs_assert(xact);
    ogs_assert(req);

    if (!sess) {
        ogs_warn("No Context");
        ogs_gtp_send_error_message(xact, sess ? sess->sgw_s5c_teid : 0,
                OGS_GTP_DELETE_SESSION_RESPONSE_TYPE,
                OGS_GTP_CAUSE_CONTEXT_NOT_FOUND);
        ogs_pkbuf_free(gtpbuf);
        return;
    }

    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    pgw_gx_send_ccr(sess, xact, gtpbuf,
        OGS_DIAM_GX_CC_REQUEST_TYPE_TERMINATION_REQUEST);
}

void pgw_s5c_handle_create_bearer_response(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp_create_bearer_response_t *req)
{
    int rv;
    ogs_gtp_f_teid_t *sgw_s5u_teid, *pgw_s5u_teid;
    ogs_gtp_node_t *sgw = NULL;
    pgw_bearer_t *bearer = NULL;

    ogs_assert(xact);
    ogs_assert(sess);
    ogs_assert(req);

    ogs_debug("[PGW] Create Bearer Response");
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    if (req->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        return;
    }
    if (req->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        return;
    }
    if (req->bearer_contexts.s5_s8_u_pgw_f_teid.presence == 0) {
        ogs_error("No PGW TEID");
        return;
    }
    if (req->bearer_contexts.s5_s8_u_sgw_f_teid.presence == 0) {
        ogs_error("No SGW TEID");
        return;
    }

    /* Correlate with PGW-S5U-TEID */
    pgw_s5u_teid = req->bearer_contexts.s5_s8_u_pgw_f_teid.data;
    ogs_assert(pgw_s5u_teid);

    /* Find the Bearer by PGW-S5U-TEID */
    bearer = pgw_bearer_find_by_pgw_s5u_teid(ntohl(pgw_s5u_teid->teid));
    ogs_assert(bearer);

    /* Set EBI */
    bearer->ebi = req->bearer_contexts.eps_bearer_id.u8;

    /* Data Plane(DL) : SGW-S5U */
    sgw_s5u_teid = req->bearer_contexts.s5_s8_u_sgw_f_teid.data;
    bearer->sgw_s5u_teid = ntohl(sgw_s5u_teid->teid);
    sgw = ogs_gtp_node_find_by_f_teid(&pgw_self()->sgw_s5u_list, sgw_s5u_teid);
    if (!sgw) {
        sgw = ogs_gtp_node_add(&pgw_self()->sgw_s5u_list, sgw_s5u_teid,
            pgw_self()->gtpu_port,
            ogs_config()->parameter.no_ipv4,
            ogs_config()->parameter.no_ipv6,
            ogs_config()->parameter.prefer_ipv4);
        ogs_assert(sgw);

        rv = ogs_gtp_connect(pgw_self()->gtpu_sock, pgw_self()->gtpu_sock6, sgw);
        ogs_assert(rv == OGS_OK);
    }
    /* Setup GTP Node */
    OGS_SETUP_GTP_NODE(bearer, sgw);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);
    
    ogs_debug("[PGW] Create Bearer Response : SGW[0x%x] --> PGW[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
}

void pgw_s5c_handle_update_bearer_response(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp_update_bearer_response_t *req)
{
    int rv;

    ogs_assert(xact);
    ogs_assert(sess);
    ogs_assert(req);

    ogs_debug("[PGW] Update Bearer Request");
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    if (req->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        return;
    }
    if (req->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        return;
    }

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);
    
    ogs_debug("[PGW] Update Bearer Response : SGW[0x%x] --> PGW[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
}

void pgw_s5c_handle_delete_bearer_response(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp_delete_bearer_response_t *req)
{
    int rv;
    pgw_bearer_t *bearer = NULL;

    ogs_assert(xact);
    ogs_assert(sess);
    ogs_assert(req);

    ogs_debug("[PGW] Delete Bearer Request");
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);
    if (req->bearer_contexts.presence == 0) {
        ogs_error("No Bearer");
        return;
    }
    if (req->bearer_contexts.eps_bearer_id.presence == 0) {
        ogs_error("No EPS Bearer ID");
        return;
    }

    bearer = pgw_bearer_find_by_ebi(
            sess, req->bearer_contexts.eps_bearer_id.u8);
    ogs_assert(bearer);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);
    
    ogs_debug("[PGW] Delete Bearer Response : SGW[0x%x] --> PGW[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    pgw_bearer_remove(bearer);
}

static int reconfigure_packet_filter(pgw_pf_t *pf, ogs_gtp_tft_t *tft, int i)
{
    int j;

    for (j = 0; j < tft->pf[i].num_of_component; j++) {
        switch(tft->pf[i].component[j].type) {
        case GTP_PACKET_FILTER_PROTOCOL_IDENTIFIER_NEXT_HEADER_TYPE:
            pf->rule.proto = tft->pf[i].component[j].proto;
            break;
        case GTP_PACKET_FILTER_IPV4_REMOTE_ADDRESS_TYPE:
            pf->rule.ipv4_remote = 1;
            pf->rule.ip.remote.addr[0] = tft->pf[i].component[j].ipv4.addr;
            pf->rule.ip.remote.mask[0] = tft->pf[i].component[j].ipv4.mask;
            break;
        case GTP_PACKET_FILTER_IPV4_LOCAL_ADDRESS_TYPE:
            pf->rule.ipv4_local = 1;
            pf->rule.ip.local.addr[0] = tft->pf[i].component[j].ipv4.addr;
            pf->rule.ip.local.mask[0] = tft->pf[i].component[j].ipv4.mask;
            break;
        case GTP_PACKET_FILTER_IPV6_REMOTE_ADDRESS_TYPE:
            pf->rule.ipv6_remote = 1;
            memcpy(pf->rule.ip.remote.addr,
                tft->pf[i].component[j].ipv6_mask.addr,
                sizeof(pf->rule.ip.remote.addr));
            memcpy(pf->rule.ip.remote.mask,
                tft->pf[i].component[j].ipv6_mask.mask,
                sizeof(pf->rule.ip.remote.mask));
            break;
        case GTP_PACKET_FILTER_IPV6_REMOTE_ADDRESS_PREFIX_LENGTH_TYPE:
            pf->rule.ipv6_remote = 1;
            memcpy(pf->rule.ip.remote.addr,
                tft->pf[i].component[j].ipv6_mask.addr,
                sizeof(pf->rule.ip.remote.addr));
            n2mask((struct in6_addr *)pf->rule.ip.remote.mask,
                tft->pf[i].component[j].ipv6.prefixlen);
            break;
        case GTP_PACKET_FILTER_IPV6_LOCAL_ADDRESS_TYPE:
            pf->rule.ipv6_local = 1;
            memcpy(pf->rule.ip.local.addr,
                tft->pf[i].component[j].ipv6_mask.addr,
                sizeof(pf->rule.ip.local.addr));
            memcpy(pf->rule.ip.local.mask,
                tft->pf[i].component[j].ipv6_mask.mask,
                sizeof(pf->rule.ip.local.mask));
            break;
        case GTP_PACKET_FILTER_IPV6_LOCAL_ADDRESS_PREFIX_LENGTH_TYPE:
            pf->rule.ipv6_local = 1;
            memcpy(pf->rule.ip.local.addr,
                tft->pf[i].component[j].ipv6_mask.addr,
                sizeof(pf->rule.ip.local.addr));
            n2mask((struct in6_addr *)pf->rule.ip.local.mask,
                tft->pf[i].component[j].ipv6.prefixlen);
            break;
        case GTP_PACKET_FILTER_SINGLE_LOCAL_PORT_TYPE:
            pf->rule.port.local.low = pf->rule.port.local.high =
                    tft->pf[i].component[j].port.low;
            break;
        case GTP_PACKET_FILTER_SINGLE_REMOTE_PORT_TYPE:
            pf->rule.port.remote.low = pf->rule.port.remote.high =
                    tft->pf[i].component[j].port.low;
            break;
        case GTP_PACKET_FILTER_LOCAL_PORT_RANGE_TYPE:
            pf->rule.port.local.low = tft->pf[i].component[j].port.low;
            pf->rule.port.local.high = tft->pf[i].component[j].port.high;
            break;
        case GTP_PACKET_FILTER_REMOTE_PORT_RANGE_TYPE:
            pf->rule.port.remote.low = tft->pf[i].component[j].port.low;
            pf->rule.port.remote.high = tft->pf[i].component[j].port.high;
            break;
        default:
            ogs_error("Unknown Packet Filter Type(%d)",
                    tft->pf[i].component[j].type);
            return OGS_ERROR;
        }
    }

    return j;
}

void pgw_s5c_handle_bearer_resource_command(
        pgw_sess_t *sess, ogs_gtp_xact_t *xact,
        ogs_gtp_bearer_resource_command_t *cmd)
{
    int rv;
    uint8_t cause_value = 0;

    ogs_gtp_header_t h;
    ogs_pkbuf_t *pkbuf = NULL;

    pgw_bearer_t *bearer = NULL;

    int16_t decoded;
    ogs_gtp_tft_t tft;

    int tft_presence = 0;
    int qos_presence = 0;

    ogs_assert(xact);
    ogs_assert(sess);
    ogs_assert(cmd);

    ogs_debug("[PGW] Bearer Resource Command");
    ogs_debug("    SGW_S5C_TEID[0x%x] PGW_S5C_TEID[0x%x]",
            sess->sgw_s5c_teid, sess->pgw_s5c_teid);

    cause_value = OGS_GTP_CAUSE_REQUEST_ACCEPTED;

    if (cmd->linked_eps_bearer_id.presence == 0) {
        ogs_error("No Linked EPS Bearer ID");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    } else {
        bearer = pgw_bearer_find_by_ebi(
                sess, cmd->linked_eps_bearer_id.u8);
        if (!bearer) {
            ogs_error("No Context for Linked EPS Bearer ID[%d]",
                    cmd->linked_eps_bearer_id.u8);
            cause_value = OGS_GTP_CAUSE_CONTEXT_NOT_FOUND;
        }
    }

    if (cmd->procedure_transaction_id.presence == 0) {
        ogs_error("No PTI");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }
    if (cmd->traffic_aggregate_description.presence == 0) {
        ogs_error("No Traffic aggregate description(TAD)");
        cause_value = OGS_GTP_CAUSE_MANDATORY_IE_MISSING;
    }

    if (cause_value != OGS_GTP_CAUSE_REQUEST_ACCEPTED) {
        ogs_gtp_send_error_message(xact, sess ? sess->sgw_s5c_teid : 0,
                OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE, cause_value);
        return;
    }

    ogs_assert(bearer);

    decoded = ogs_gtp_parse_tft(&tft, &cmd->traffic_aggregate_description);
    ogs_assert(cmd->traffic_aggregate_description.len == decoded);

    if (tft.code == OGS_GTP_TFT_CODE_NO_TFT_OPERATION) {
        /* No operation */

    } else if (tft.code ==
            OGS_GTP_TFT_CODE_REPLACE_PACKET_FILTERS_IN_EXISTING) {
        int i;
        for (i = 0; i < tft.num_of_packet_filter; i++) {
            int num_of_comp = 0;
            pgw_pf_t *pf = NULL;

            pf = pgw_pf_find_by_id(bearer, tft.pf[i].identifier+1);
            if (pf) {
                num_of_comp = reconfigure_packet_filter(pf, &tft, i);
                if (num_of_comp < 0) {
                    ogs_gtp_send_error_message(
                        xact, sess ? sess->sgw_s5c_teid : 0,
                        OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                        OGS_GTP_CAUSE_SEMANTIC_ERROR_IN_THE_TAD_OPERATION);
                    return;
                }
            }

            if (num_of_comp > 0) tft_presence = 1;
        }
    } else if (tft.code ==
            OGS_GTP_TFT_CODE_ADD_PACKET_FILTERS_TO_EXISTING_TFT) {
        int i;
        for (i = 0; i < tft.num_of_packet_filter; i++) {
            int num_of_comp = 0;
            pgw_pf_t *pf = NULL;

            pf = pgw_pf_find_by_id(bearer, tft.pf[i].identifier+1);
            if (!pf)
                pf = pgw_pf_add(bearer, tft.pf[i].precedence);
            ogs_assert(pf);

            num_of_comp = reconfigure_packet_filter(pf, &tft, i);
            if (num_of_comp < 0) {
                ogs_gtp_send_error_message(
                    xact, sess ? sess->sgw_s5c_teid : 0,
                    OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                    OGS_GTP_CAUSE_SEMANTIC_ERROR_IN_THE_TAD_OPERATION);
                return;
            }

            if (num_of_comp > 0) tft_presence = 1;
        }
    } else if (tft.code ==
            OGS_GTP_TFT_CODE_DELETE_PACKET_FILTERS_FROM_EXISTING) {

        /* TODO */
        ogs_gtp_send_error_message(xact, sess ? sess->sgw_s5c_teid : 0,
                OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                OGS_GTP_CAUSE_SERVICE_NOT_SUPPORTED);
        return;
    }

    if (cmd->flow_quality_of_service.presence) {
        ogs_gtp_flow_qos_t flow_qos;

        decoded = ogs_gtp_parse_flow_qos(
                &flow_qos, &cmd->flow_quality_of_service);
        ogs_assert(cmd->flow_quality_of_service.len == decoded);

        bearer->qos.mbr.uplink = flow_qos.ul_mbr;
        bearer->qos.mbr.downlink = flow_qos.dl_mbr;
        bearer->qos.gbr.uplink = flow_qos.ul_gbr;
        bearer->qos.gbr.downlink = flow_qos.dl_gbr;

        qos_presence = 1;
    }

    if (tft_presence == 0 && qos_presence == 0) {
        /* No modification */
        ogs_gtp_send_error_message(xact, sess ? sess->sgw_s5c_teid : 0,
                OGS_GTP_BEARER_RESOURCE_FAILURE_INDICATION_TYPE,
                OGS_GTP_CAUSE_SERVICE_NOT_SUPPORTED);
        return;
    }

    memset(&h, 0, sizeof(ogs_gtp_header_t));
    h.type = OGS_GTP_UPDATE_BEARER_REQUEST_TYPE;
    h.teid = sess->sgw_s5c_teid;

    pkbuf = pgw_s5c_build_update_bearer_request(
            h.type, bearer, cmd->procedure_transaction_id.u8,
            tft_presence ? &tft : NULL, qos_presence);
    ogs_expect_or_return(pkbuf);

    rv = ogs_gtp_xact_update_tx(xact, &h, pkbuf);
    ogs_expect_or_return(rv == OGS_OK);

    rv = ogs_gtp_xact_commit(xact);
    ogs_expect(rv == OGS_OK);
}
