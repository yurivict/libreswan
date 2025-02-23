/*
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009-2010 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2018 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012,2016-2017 Antony Antony <appu@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2014-2019 Andrew Cagney <cagney@gnu.org>
 * Copyright (C) 2017 Antony Antony <antony@phenome.org>
 * Copyright (C) 2020 Yulia Kuzovkova <ukuzovkova@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 */

#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "sysdep.h"
#include "constants.h"

#include "defs.h"
#include "id.h"
#include "x509.h"
#include "pluto_x509.h"
#include "certs.h"
#include "connections.h"        /* needs id.h */
#include "state.h"
#include "packet.h"
#include "crypto.h"
#include "ike_alg.h"
#include "log.h"
#include "demux.h"      /* needs packet.h */
#include "ikev2.h"
#include "ipsec_doi.h"  /* needs demux.h and state.h */
#include "timer.h"
#include "whack.h"      /* requires connections.h */
#include "server.h"
#include "vendor.h"
#include "kernel.h"
#include "host_pair.h"
#include "addresspool.h"
#include "rnd.h"
#include "ip_address.h"
#include "ikev2_send.h"
#include "ikev2_message.h"
#include "ikev2_ts.h"
#include "ip_info.h"
#ifdef USE_XFRM_INTERFACE
# include "kernel_xfrm_interface.h"
#endif
#include "ikev2_cp.h"
#include "ikev2_child.h"
#include "ike_alg_dh.h"
#include "pluto_stats.h"

stf_status ikev2_child_sa_respond(struct ike_sa *ike,
				  struct child_sa *child,
				  struct msg_digest *md,
				  struct pbs_out *outpbs)
{
	pexpect(child->sa.st_establishing_sa == IPSEC_SA); /* never grow up */
	enum isakmp_xchg_types isa_xchg = md->hdr.isa_xchg;
	struct connection *c = child->sa.st_connection;

	if (md->chain[ISAKMP_NEXT_v2CP] != NULL) {
		if (c->spd.that.has_lease) {
			if (!emit_v2_child_configuration_payload(child, outpbs)) {
				return STF_INTERNAL_ERROR;
			}
		} else {
			dbg("#%lu %s ignoring unexpected v2CP payload",
			    child->sa.st_serialno, child->sa.st_state->name);
		}
	}

	/* start of SA out */
	{
		/* ??? this code won't support AH + ESP */
		struct ipsec_proto_info *proto_info
			= ikev2_child_sa_proto_info(child, c->policy);
		proto_info->our_spi = ikev2_child_sa_spi(&c->spd, c->policy,
							 child->sa.st_logger);
		chunk_t local_spi = THING_AS_CHUNK(proto_info->our_spi);
		if (!ikev2_emit_sa_proposal(outpbs,
					    child->sa.st_accepted_esp_or_ah_proposal,
					    &local_spi)) {
			dbg("problem emitting accepted proposal");
			return STF_INTERNAL_ERROR;
		}
	}

	if (isa_xchg == ISAKMP_v2_CREATE_CHILD_SA) {
		/* send NONCE */
		struct ikev2_generic in = {
			.isag_critical = build_ikev2_critical(false, ike->sa.st_logger),
		};
		pb_stream pb_nr;
		if (!out_struct(&in, &ikev2_nonce_desc, outpbs, &pb_nr) ||
		    !out_hunk(child->sa.st_nr, &pb_nr, "IKEv2 nonce"))
			return STF_INTERNAL_ERROR;

		close_output_pbs(&pb_nr);

		/*
		 * XXX: shouldn't this be conditional on the local end
		 * having computed KE and not what the remote sent?
		 */
		if (md->chain[ISAKMP_NEXT_v2KE] != NULL)  {
			if (!emit_v2KE(&child->sa.st_gr, child->sa.st_oakley.ta_dh, outpbs))
				return STF_INTERNAL_ERROR;
		}
	}

	if (md->pd[PD_v2N_USE_TRANSPORT_MODE] != NULL) {
		dbg("received USE_TRANSPORT_MODE");
		child->sa.st_seen_use_transport = TRUE;
	}

	if (md->pd[PD_v2N_IPCOMP_SUPPORTED] != NULL) {
		struct pbs_in pbs = md->pd[PD_v2N_IPCOMP_SUPPORTED]->pbs;
		size_t len = pbs_left(&pbs);
		struct ikev2_notify_ipcomp_data n_ipcomp;

		dbg("received v2N_IPCOMP_SUPPORTED of length %zd", len);

		diag_t d = pbs_in_struct(&pbs, &ikev2notify_ipcomp_data_desc,
					 &n_ipcomp, sizeof(n_ipcomp), NULL);
		if (d != NULL) {
			llog_diag(RC_LOG, child->sa.st_logger, &d, "%s", "");
			return STF_FATAL;
		}

		if (n_ipcomp.ikev2_notify_ipcomp_trans != IPCOMP_DEFLATE) {
			log_state(RC_LOG_SERIOUS, &child->sa,
				  "Unsupported IPCOMP compression method %d",
				  n_ipcomp.ikev2_notify_ipcomp_trans); /* enum_name this later */
			return STF_FATAL;
		}
		if (n_ipcomp.ikev2_cpi < IPCOMP_FIRST_NEGOTIATED) {
			log_state(RC_LOG_SERIOUS, &child->sa,
				  "Illegal IPCOMP CPI %d", n_ipcomp.ikev2_cpi);
			return STF_FATAL;
		}
		if ((c->policy & POLICY_COMPRESS) == LEMPTY) {
			dbg("Ignored IPCOMP request as connection has compress=no");
			child->sa.st_ipcomp.present = false;
		} else {
			dbg("Received compression CPI=%d", htonl(n_ipcomp.ikev2_cpi));
			//child->sa.st_ipcomp.attrs.spi = uniquify_peer_cpi((ipsec_spi_t)htonl(n_ipcomp.ikev2_cpi), cst, 0);
			child->sa.st_ipcomp.attrs.spi = htonl((ipsec_spi_t)n_ipcomp.ikev2_cpi);
			child->sa.st_ipcomp.attrs.transattrs.ta_comp = n_ipcomp.ikev2_notify_ipcomp_trans;
			child->sa.st_ipcomp.attrs.mode = ENCAPSULATION_MODE_TUNNEL; /* always? */
			child->sa.st_ipcomp.present = true;
		}
	} else if (c->policy & POLICY_COMPRESS) {
		dbg("policy suggested compression, but peer did not offer support");
	}

	if (md->pd[PD_v2N_ESP_TFC_PADDING_NOT_SUPPORTED] != NULL) {
		dbg("received ESP_TFC_PADDING_NOT_SUPPORTED");
		child->sa.st_seen_no_tfc = true;
	}

	{
		/* verify if transport / tunnel mode is matches */
		if ((c->policy & POLICY_TUNNEL) == LEMPTY) {
			/* we should have received transport mode request - and send one */
			if (!child->sa.st_seen_use_transport) {
				log_state(RC_LOG, &child->sa,
					  "policy dictates Transport Mode, but peer requested Tunnel Mode");
				return STF_FAIL + v2N_NO_PROPOSAL_CHOSEN;
			}
		} else {
			if (child->sa.st_seen_use_transport) {
				/* RFC allows us to ignore their (wrong) request for transport mode */
				log_state(RC_LOG, &child->sa,
					  "policy dictates Tunnel Mode, ignoring peer's request for Transport Mode");
			}
		}

		/*
		 * XXX: see above notes on 'role' - this must be the
		 * SA_RESPONDER.
		 */
		stf_status ret = v2_emit_ts_payloads(child, outpbs, c);

		if (ret != STF_OK)
			return ret;	/* should we delete_state cst? */
	}

	if (child->sa.st_seen_use_transport) {
		if (c->policy & POLICY_TUNNEL) {
			log_state(RC_LOG, &child->sa,
				  "Local policy is tunnel mode - ignoring request for transport mode");
		} else {
			dbg("Local policy is transport mode and received USE_TRANSPORT_MODE");
			if (child->sa.st_esp.present) {
				child->sa.st_esp.attrs.mode =
					ENCAPSULATION_MODE_TRANSPORT;
			}
			if (child->sa.st_ah.present) {
				child->sa.st_ah.attrs.mode =
					ENCAPSULATION_MODE_TRANSPORT;
			}
			/* In v2, for parent, protoid must be 0 and SPI must be empty */
			if (!emit_v2N(v2N_USE_TRANSPORT_MODE, outpbs))
				return STF_INTERNAL_ERROR;
		}
	} else {
		/* the peer wants tunnel mode */
		if ((c->policy & POLICY_TUNNEL) == LEMPTY) {
			log_state(RC_LOG_SERIOUS, &child->sa,
				  "Local policy is transport mode, but peer did not request that");
			return STF_FAIL + v2N_NO_PROPOSAL_CHOSEN;
		}
	}

	if (c->send_no_esp_tfc) {
		dbg("Sending ESP_TFC_PADDING_NOT_SUPPORTED");
		if (!emit_v2N(v2N_ESP_TFC_PADDING_NOT_SUPPORTED, outpbs))
			return STF_INTERNAL_ERROR;
	}

	if (child->sa.st_ipcomp.present) {
		/* logic above decided to enable IPCOMP */
		if (!emit_v2N_ipcomp_supported(child, outpbs))
			return STF_INTERNAL_ERROR;
	}

	ikev2_derive_child_keys(child);
	return STF_OK;
}

static void ikev2_set_domain(pb_stream *cp_a_pbs, struct child_sa *child)
{
	bool responder = (child->sa.st_state->kind != STATE_PARENT_I2);
	bool ignore = LIN(POLICY_IGNORE_PEER_DNS, child->sa.st_connection->policy);

	if (!responder) {
		char *safestr = cisco_stringify(cp_a_pbs, "INTERNAL_DNS_DOMAIN",
						ignore, child->sa.st_logger);
		if (safestr != NULL) {
			append_st_cfg_domain(&child->sa, safestr);
		}
	} else {
		log_state(RC_LOG, &child->sa,
			  "initiator INTERNAL_DNS_DOMAIN CP ignored");
	}
}

static bool ikev2_set_dns(struct pbs_in *cp_a_pbs, struct child_sa *child,
			  const struct ip_info *af)
{
	struct connection *c = child->sa.st_connection;
	bool ignore = LIN(POLICY_IGNORE_PEER_DNS, c->policy);

	if (c->policy & POLICY_OPPORTUNISTIC) {
		log_state(RC_LOG, &child->sa,
			  "ignored INTERNAL_IP%d_DNS CP payload for Opportunistic IPsec",
			  af->ip_version);
		return true;
	}

	ip_address ip;
	diag_t d = pbs_in_address(cp_a_pbs, &ip, af, "INTERNAL_IP_DNS CP payload");
	if (d != NULL) {
		llog_diag(RC_LOG, child->sa.st_logger, &d, "%s", "");
		return false;
	}

	/* i.e. all zeros */
	if (address_is_any(ip)) {
		address_buf ip_str;
		log_state(RC_LOG, &child->sa,
			  "ERROR INTERNAL_IP%d_DNS %s is invalid",
			  af->ip_version, ipstr(&ip, &ip_str));
		return false;
	}

	bool responder = (child->sa.st_state->kind != STATE_PARENT_I2);
	if (!responder) {
		address_buf ip_buf;
		const char *ip_str = ipstr(&ip, &ip_buf);

		log_state(RC_LOG, &child->sa,
			  "received %sINTERNAL_IP%d_DNS %s",
			  ignore ? "and ignored " : "",
			  af->ip_version, ip_str);
		if (!ignore)
			append_st_cfg_dns(&child->sa, ip_str);
	} else {
		log_state(RC_LOG, &child->sa,
			  "initiator INTERNAL_IP%d_DNS CP ignored",
			  af->ip_version);
	}

	return true;
}

static bool ikev2_set_ia(pb_stream *cp_a_pbs, struct child_sa *child,
			 const struct ip_info *af, bool *seen_an_address)
{
	struct connection *c = child->sa.st_connection;

	ip_address ip;
	diag_t d = pbs_in_address(cp_a_pbs, &ip, af, "INTERNAL_IP_ADDRESS");
	if (d != NULL) {
		llog_diag(RC_LOG, child->sa.st_logger, &d, "%s", "");
		return false;
	}

	/*
	 * if (af->af == AF_INET6) pbs_in_address only reads 16 bytes.
	 * There should be one more byte in the pbs, 17th byte is prefix length.
	 */

	if (address_is_any(ip)) {
		ipstr_buf ip_str;
		log_state(RC_LOG, &child->sa,
			  "ERROR INTERNAL_IP%d_ADDRESS %s is invalid",
			  af->ip_version, ipstr(&ip, &ip_str));
		return false;
	}

	ipstr_buf ip_str;
	log_state(RC_LOG, &child->sa,
		  "received INTERNAL_IP%d_ADDRESS %s%s",
		  af->ip_version, ipstr(&ip, &ip_str),
		  *seen_an_address ? "; discarded" : "");


	bool responder = child->sa.st_state->kind != STATE_PARENT_I2;
	if (responder) {
		log_state(RC_LOG, &child->sa, "bogus responder CP ignored");
		return true;
	}

	if (*seen_an_address) {
		return true;
	}

	*seen_an_address = true;
	c->spd.this.has_client = true;
	c->spd.this.has_internal_address = true;

	if (c->spd.this.cat) {
		dbg("CAT is set, not setting host source IP address to %s",
		    ipstr(&ip, &ip_str));
		ip_address this_client_prefix = selector_prefix(c->spd.this.client);
		if (address_eq_address(this_client_prefix, ip)) {
			/*
			 * The address we received is same as this
			 * side should we also check the host_srcip.
			 */
			dbg("#%lu %s[%lu] received INTERNAL_IP%d_ADDRESS that is same as this.client.addr %s. Will not add CAT iptable rules",
			    child->sa.st_serialno, c->name, c->instance_serial,
			    af->ip_version, ipstr(&ip, &ip_str));
		} else {
			c->spd.this.client = selector_from_address(ip);
			child->sa.st_ts_this = ikev2_end_to_ts(&c->spd.this, child);
			c->spd.this.has_cat = true; /* create iptable entry */
		}
	} else {
		c->spd.this.client = selector_from_address(ip);
		/* only set sourceip= value if unset in configuration */
		if (address_is_unset(&c->spd.this.host_srcip) ||
		    address_is_any(c->spd.this.host_srcip)) {
			dbg("setting host source IP address to %s",
			    ipstr(&ip, &ip_str));
			c->spd.this.host_srcip = ip;
		}
	}

	return true;
}

bool ikev2_parse_cp_r_body(struct payload_digest *cp_pd, struct child_sa *child)
{
	struct ikev2_cp *cp =  &cp_pd->payload.v2cp;
	struct connection *c = child->sa.st_connection;
	pb_stream *attrs = &cp_pd->pbs;

	dbg("#%lu %s[%lu] parsing ISAKMP_NEXT_v2CP payload",
	    child->sa.st_serialno, c->name, c->instance_serial);

	if (child->sa.st_state->kind == STATE_PARENT_I2 && cp->isacp_type !=  IKEv2_CP_CFG_REPLY) {
		log_state(RC_LOG_SERIOUS, &child->sa,
			  "ERROR expected IKEv2_CP_CFG_REPLY got a %s",
			  enum_name(&ikev2_cp_type_names, cp->isacp_type));
		return FALSE;
	}

	if (child->sa.st_state->kind == STATE_PARENT_R1 && cp->isacp_type !=  IKEv2_CP_CFG_REQUEST) {
		log_state(RC_LOG_SERIOUS, &child->sa,
			  "ERROR expected IKEv2_CP_CFG_REQUEST got a %s",
			  enum_name(&ikev2_cp_type_names, cp->isacp_type));
		return FALSE;
	}

	bool seen_internal_address = false;
	while (pbs_left(attrs) > 0) {
		struct ikev2_cp_attribute cp_a;
		pb_stream cp_a_pbs;

		diag_t d = pbs_in_struct(attrs, &ikev2_cp_attribute_desc,
					 &cp_a, sizeof(cp_a), &cp_a_pbs);
		if (d != NULL) {
			llog_diag(RC_LOG_SERIOUS, child->sa.st_logger, &d,
				 "ERROR malformed CP attribute");
			return false;
		}

		switch (cp_a.type) {
		case IKEv2_INTERNAL_IP4_ADDRESS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_ia(&cp_a_pbs, child, &ipv4_info,
					  &seen_internal_address)) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "ERROR malformed INTERNAL_IP4_ADDRESS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP4_DNS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_dns(&cp_a_pbs, child, &ipv4_info)) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "ERROR malformed INTERNAL_IP4_DNS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP6_ADDRESS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_ia(&cp_a_pbs, child, &ipv6_info,
					  &seen_internal_address)) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "ERROR malformed INTERNAL_IP6_ADDRESS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_IP6_DNS | ISAKMP_ATTR_AF_TLV:
			if (!ikev2_set_dns(&cp_a_pbs, child, &ipv6_info)) {
				log_state(RC_LOG_SERIOUS, &child->sa,
					  "ERROR malformed INTERNAL_IP6_DNS attribute");
				return FALSE;
			}
			break;

		case IKEv2_INTERNAL_DNS_DOMAIN | ISAKMP_ATTR_AF_TLV:
			ikev2_set_domain(&cp_a_pbs, child); /* can't fail */
			break;

		default:
			log_state(RC_LOG, &child->sa,
				  "unknown attribute %s length %u",
				  enum_name(&ikev2_cp_attribute_type_names, cp_a.type),
				  cp_a.len);
			break;
		}
	}
	return TRUE;
}

bool ikev2_process_childs_sa_payload(const char *what,
				     struct ike_sa *ike, struct child_sa *child,
				     struct msg_digest *md, bool expect_accepted_proposal)
{
	struct connection *c = child->sa.st_connection;
	struct payload_digest *const sa_pd = md->chain[ISAKMP_NEXT_v2SA];
	enum isakmp_xchg_types isa_xchg = md->hdr.isa_xchg;
	struct ipsec_proto_info *proto_info =
		ikev2_child_sa_proto_info(child, c->policy);
	stf_status ret;

	struct ikev2_proposals *child_proposals;
	if (isa_xchg == ISAKMP_v2_CREATE_CHILD_SA) {
		const struct dh_desc *default_dh = (c->policy & POLICY_PFS) != LEMPTY
			? ike->sa.st_oakley.ta_dh
			: &ike_alg_dh_none;
		child_proposals = get_v2_create_child_proposals(c, what, default_dh,
								child->sa.st_logger);
	} else {
		child_proposals = get_v2_ike_auth_child_proposals(c, what,
								  child->sa.st_logger);
	}

	ret = ikev2_process_sa_payload(what,
				       &sa_pd->pbs,
				       /*expect_ike*/ FALSE,
				       /*expect_spi*/ TRUE,
				       expect_accepted_proposal,
				       LIN(POLICY_OPPORTUNISTIC, c->policy),
				       &child->sa.st_accepted_esp_or_ah_proposal,
				       child_proposals, child->sa.st_logger);

	if (ret != STF_OK) {
		LLOG_JAMBUF(RC_LOG_SERIOUS, child->sa.st_logger, buf) {
			jam_string(buf, what);
			jam(buf, " failed, responder SA processing returned ");
			jam_v2_stf_status(buf, ret);
		}
		if (child->sa.st_sa_role == SA_RESPONDER) {
			pexpect(ret > STF_FAIL);
			record_v2N_response(child->sa.st_logger, ike, md,
					    ret - STF_FAIL, NULL,
					    ENCRYPTED_PAYLOAD);
		}
		return false;
	}

	if (DBGP(DBG_BASE)) {
		DBG_log_ikev2_proposal(what, child->sa.st_accepted_esp_or_ah_proposal);
	}
	if (!ikev2_proposal_to_proto_info(child->sa.st_accepted_esp_or_ah_proposal, proto_info,
					  child->sa.st_logger)) {
		log_state(RC_LOG_SERIOUS, &child->sa,
			  "%s proposed/accepted a proposal we don't actually support!", what);
		return false;
	}

	/*
	 * Update/check the PFS.
	 *
	 * For the responder, go with what ever was negotiated.  For
	 * the initiator, check what was negotiated against what was
	 * sent.
	 *
	 * Because code expects .st_pfs_group to use NULL, and not
	 * &ike_alg_dh_none, to indicate no-DH algorithm, the value
	 * returned by the proposal parser needs to be patched up.
	 */
	const struct dh_desc *accepted_dh =
		proto_info->attrs.transattrs.ta_dh == &ike_alg_dh_none ? NULL
		: proto_info->attrs.transattrs.ta_dh;
	switch (child->sa.st_sa_role) {
	case SA_INITIATOR:
		pexpect(expect_accepted_proposal);
		if (accepted_dh != NULL && accepted_dh != child->sa.st_pfs_group) {
			log_state(RC_LOG_SERIOUS, &child->sa,
				  "expecting %s but remote's accepted proposal includes %s",
				  child->sa.st_pfs_group == NULL ? "no DH" : child->sa.st_pfs_group->common.fqn,
				  accepted_dh->common.fqn);
			return false;
		}
		child->sa.st_pfs_group = accepted_dh;
		break;
	case SA_RESPONDER:
		pexpect(!expect_accepted_proposal);
		pexpect(child->sa.st_sa_role == SA_RESPONDER);
		pexpect(child->sa.st_pfs_group == NULL);
		child->sa.st_pfs_group = accepted_dh;
		break;
	default:
		bad_case(child->sa.st_sa_role);
	}

	/*
	 * Update the state's st_oakley parameters from the proposal,
	 * but retain the previous PRF.  A CHILD_SA always uses the
	 * PRF negotiated when creating initial IKE SA.
	 *
	 * XXX: The mystery is, why is .st_oakley even being updated?
	 * Perhaps it is to prop up code getting the CHILD_SA's PRF
	 * from the child when that code should use the CHILD_SA's IKE
	 * SA; or perhaps it is getting things ready for an IKE SA
	 * re-key?
	 */
	if (isa_xchg == ISAKMP_v2_CREATE_CHILD_SA && child->sa.st_pfs_group != NULL) {
		dbg("updating #%lu's .st_oakley with preserved PRF, but why update?",
			child->sa.st_serialno);
		struct trans_attrs accepted_oakley = proto_info->attrs.transattrs;
		pexpect(accepted_oakley.ta_prf == NULL);
		accepted_oakley.ta_prf = child->sa.st_oakley.ta_prf;
		child->sa.st_oakley = accepted_oakley;
	}

	return true;
}

void v2_child_sa_established(struct child_sa *child)
{
	change_state(&child->sa, STATE_V2_ESTABLISHED_CHILD_SA);
	pstat_sa_established(&child->sa);
	log_ipsec_sa_established("negotiated connection", &child->sa);
	LLOG_JAMBUF(RC_SUCCESS, child->sa.st_logger, buf) {
		jam(buf, "%s", child->sa.st_state->story);
		/* document SA details for admin's pleasure */
		lswlog_child_sa_established(buf, &child->sa);
	}
	v2_schedule_replace_event(&child->sa);
	/*
	 * start liveness checks if set, making sure we only schedule
	 * once when moving from I2->I3 or R1->R2
	 */
	if (dpd_active_locally(&child->sa)) {
		dbg("dpd enabled, scheduling ikev2 liveness checks");
		deltatime_t delay = deltatime_max(child->sa.st_connection->dpd_delay,
						  deltatime(MIN_LIVENESS));
		event_schedule(EVENT_v2_LIVENESS, delay, &child->sa);
	}

}
