/*	$OpenBSD: ofp13.c,v 1.7 2016/09/26 12:33:04 rzalamena Exp $	*/

/*
 * Copyright (c) 2013-2016 Reyk Floeter <reyk@openbsd.org>
 * Copyright (c) 2016 Rafael Zalamena <rzalamena@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/socket.h>

#include <net/if.h>
#include <net/if_arp.h>
#include <netinet/in.h>
#include <netinet/if_ether.h>
#include <netinet/tcp.h>
#include <netmpls/mpls.h>

#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <imsg.h>
#include <event.h>

#include "ofp.h"
#include "switchd.h"
#include "ofp_map.h"

int	 ofp13_validate(struct switchd *,
	    struct sockaddr_storage *, struct sockaddr_storage *,
	    struct ofp_header *, struct ibuf *);

int	 ofp13_hello(struct switchd *, struct switch_connection *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_echo_request(struct switchd *, struct switch_connection *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_validate_error(struct switchd *,
	    struct sockaddr_storage *, struct sockaddr_storage *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_error(struct switchd *, struct switch_connection *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_validate_oxm(struct switchd *, struct ofp_ox_match *,
	    struct ofp_header *, struct ibuf *, off_t);
int	 ofp13_validate_packet_in(struct switchd *,
	    struct sockaddr_storage *, struct sockaddr_storage *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_packet_match(struct ibuf *, struct packet *, struct ofp_match *);
int	 ofp13_packet_in(struct switchd *, struct switch_connection *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_flow_removed(struct switchd *, struct switch_connection *,
	    struct ofp_header *, struct ibuf *);
int	 ofp13_validate_packet_out(struct switchd *,
	    struct sockaddr_storage *, struct sockaddr_storage *,
	    struct ofp_header *, struct ibuf *);

struct ofp_group_mod *
	    ofp13_group(struct switch_connection *, struct ibuf *,
	    uint32_t, uint16_t, uint8_t);
struct ofp_bucket *
	    ofp13_bucket(struct ibuf *, uint16_t, uint32_t, uint32_t);

int	 action_new(struct ibuf *, uint16_t);
int	 action_group(struct ibuf *, uint32_t);
int	 action_output(struct ibuf *, uint32_t, uint16_t);
int	 action_push(struct ibuf *, uint16_t, uint16_t);
int	 action_pop_vlan(struct ibuf *);
int	 action_pop_mpls(struct ibuf *, uint16_t);
int	 action_copyttlout(struct ibuf *);
int	 action_copyttlin(struct ibuf *);
int	 action_decnwttl(struct ibuf *);
struct ofp_action_set_field *
	    action_setfield(struct ibuf *ibuf);

struct ofp_ox_match *
	    oxm_get(struct ibuf *, uint16_t, int, uint8_t);
int	 oxm_inport(struct ibuf *, uint32_t);
int	 oxm_inphyport(struct ibuf *, uint32_t);
int	 oxm_metadata(struct ibuf *, int, uint64_t, uint64_t);
int	 oxm_etheraddr(struct ibuf *, int, uint8_t *, uint8_t *);
int	 oxm_ethertype(struct ibuf *, uint16_t);
int	 oxm_vlanvid(struct ibuf *, int, uint16_t, uint16_t);
int	 oxm_vlanpcp(struct ibuf *, uint8_t);
int	 oxm_ipdscp(struct ibuf *, uint8_t);
int	 oxm_ipecn(struct ibuf *, uint8_t);
int	 oxm_ipproto(struct ibuf *, uint8_t);
int	 oxm_ipaddr(struct ibuf *, int, int, uint32_t, uint32_t);
int	 oxm_tcpport(struct ibuf *, int, uint16_t);
int	 oxm_udpport(struct ibuf *, int, uint16_t);
int	 oxm_sctpport(struct ibuf *, int, uint16_t);
int	 oxm_icmpv4type(struct ibuf *, uint8_t);
int	 oxm_icmpv4code(struct ibuf *, uint8_t);
int	 oxm_arpop(struct ibuf *, uint16_t);
int	 oxm_arpaddr(struct ibuf *, int, int, uint32_t, uint32_t);
int	 oxm_arphaddr(struct ibuf *, int, uint8_t *, uint8_t *);
int	 oxm_ipv6addr(struct ibuf *, int, struct in6_addr *, struct in6_addr *);
int	 oxm_ipv6flowlabel(struct ibuf *, int, uint32_t, uint32_t);
int	 oxm_icmpv6type(struct ibuf *, uint8_t);
int	 oxm_icmpv6code(struct ibuf *, uint8_t);
int	 oxm_ipv6ndtarget(struct ibuf *, struct in6_addr *);
int	 oxm_ipv6ndlinkaddr(struct ibuf *, int, uint8_t *);
int	 oxm_mplslabel(struct ibuf *, uint32_t);
int	 oxm_mplstc(struct ibuf *, uint8_t);
int	 oxm_mplsbos(struct ibuf *, uint8_t);
int	 oxm_tunnelid(struct ibuf *, int, uint64_t, uint64_t);
int	 oxm_ipv6exthdr(struct ibuf *, int, uint16_t, uint16_t);

struct ofp_callback ofp13_callbacks[] = {
	{ OFP_T_HELLO,			ofp13_hello, NULL },
	{ OFP_T_ERROR,			NULL, ofp13_validate_error },
	{ OFP_T_ECHO_REQUEST,		ofp13_echo_request, NULL },
	{ OFP_T_ECHO_REPLY,		NULL, NULL },
	{ OFP_T_EXPERIMENTER,		NULL, NULL },
	{ OFP_T_FEATURES_REQUEST,	NULL, NULL },
	{ OFP_T_FEATURES_REPLY,		NULL, NULL },
	{ OFP_T_GET_CONFIG_REQUEST,	NULL, NULL },
	{ OFP_T_GET_CONFIG_REPLY,	NULL, NULL },
	{ OFP_T_SET_CONFIG,		NULL, NULL },
	{ OFP_T_PACKET_IN,		ofp13_packet_in,
					ofp13_validate_packet_in },
	{ OFP_T_FLOW_REMOVED,		ofp13_flow_removed, NULL },
	{ OFP_T_PORT_STATUS,		NULL, NULL },
	{ OFP_T_PACKET_OUT,		NULL, ofp13_validate_packet_out },
	{ OFP_T_FLOW_MOD,		NULL, NULL },
	{ OFP_T_GROUP_MOD,		NULL, NULL },
	{ OFP_T_PORT_MOD,		NULL, NULL },
	{ OFP_T_TABLE_MOD,		NULL, NULL },
	{ OFP_T_MULTIPART_REQUEST,	NULL, NULL },
	{ OFP_T_MULTIPART_REPLY,	NULL, NULL },
	{ OFP_T_BARRIER_REQUEST,	NULL, NULL },
	{ OFP_T_BARRIER_REPLY,		NULL, NULL },
	{ OFP_T_QUEUE_GET_CONFIG_REQUEST, NULL, NULL },
	{ OFP_T_QUEUE_GET_CONFIG_REPLY,	NULL, NULL },
	{ OFP_T_ROLE_REQUEST,		NULL, NULL },
	{ OFP_T_ROLE_REPLY,		NULL, NULL },
	{ OFP_T_GET_ASYNC_REQUEST,	NULL, NULL },
	{ OFP_T_GET_ASYNC_REPLY,	NULL, NULL },
	{ OFP_T_SET_ASYNC,		NULL, NULL },
	{ OFP_T_METER_MOD,		NULL, NULL },
};

int
ofp13_validate(struct switchd *sc,
    struct sockaddr_storage *src, struct sockaddr_storage *dst,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	uint8_t	type;

	if (ofp_validate_header(sc, src, dst, oh, OFP_V_1_3) != 0) {
		log_debug("\tinvalid header");
		return (-1);
	}
	if (ibuf == NULL) {
		/* The response packet buffer is optional */
		return (0);
	}
	type = oh->oh_type;
	if (ofp13_callbacks[type].validate != NULL &&
	    ofp13_callbacks[type].validate(sc, src, dst, oh, ibuf) != 0) {
		log_debug("\tinvalid packet");
		return (-1);
	}
	return (0);
}

int
ofp13_validate_oxm(struct switchd *sc, struct ofp_ox_match *oxm,
    struct ofp_header *oh, struct ibuf *ibuf, off_t off)
{
	uint16_t	 class;
	uint8_t		 type;
	uint32_t	 port;
	void		*ptr;

	/* match element is always followed by data */
	if (oxm->oxm_length == 0)
		return (0);

	type = OFP_OXM_GET_FIELD(oxm);
	class = ntohs(oxm->oxm_class);

	log_debug("\tox match class %s type %s length %u",
	    print_map(class, ofp_oxm_map),
	    print_map(type, ofp_xm_t_map),
	    oxm->oxm_length);

	switch (class) {
	case OFP_OXM_C_OPENFLOW_BASIC:
		switch (type) {
		case OFP_XM_T_IN_PORT:
			off += sizeof(*oxm);
			if ((ptr = ibuf_seek(ibuf, off, sizeof(port))) == NULL)
				return (-1);
			port = *(uint32_t *)ptr;
			log_debug("\t\tport %u", ntohl(port));
			break;
		}
		break;
	}

	return (0);
}

int
ofp13_validate_packet_in(struct switchd *sc,
    struct sockaddr_storage *src, struct sockaddr_storage *dst,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	struct ofp_packet_in	*pin;
	struct ofp_match	*om;
	struct ofp_ox_match	*oxm;
	uint8_t			*p;
	ssize_t			 len, mlen;
	off_t			 moff, off;

	off = 0;
	if ((pin = ibuf_seek(ibuf, off, sizeof(*pin))) == NULL)
		return (-1);
	log_debug("\tbuffer %d length %u reason %s table %u cookie 0x%016llx",
	    ntohl(pin->pin_buffer_id),
	    ntohs(pin->pin_total_len),
	    print_map(ntohs(pin->pin_reason), ofp_pktin_map),
	    pin->pin_table_id,
	    be64toh(pin->pin_cookie));
	off += offsetof(struct ofp_packet_in, pin_match);

	om = &pin->pin_match;
	mlen = ntohs(om->om_length);
	log_debug("\tmatch type %s length %zu (padded to %zu)",
	    print_map(ntohs(om->om_type), ofp_match_map),
	    mlen, OFP_ALIGN(mlen) + ETHER_ALIGN);

	/* current match offset, aligned offset after all matches */
	moff = off + sizeof(*om);
	off += OFP_ALIGN(mlen) + ETHER_ALIGN;

	switch (htons(om->om_type)) {
	case OFP_MATCH_OXM:
		do {
			if ((oxm = ibuf_seek(ibuf, moff, sizeof(*oxm))) == NULL)
				return (-1);
			if (ofp13_validate_oxm(sc, oxm, oh, ibuf, moff) == -1)
				return (-1);
			moff += sizeof(*oxm) + oxm->oxm_length;
			mlen -= sizeof(*oxm) + oxm->oxm_length;
		} while (mlen > 0 && oxm->oxm_length);
		break;
	case OFP_MATCH_STANDARD:
		/* deprecated */
		break;
	}

	/* Calculate offset from the beginning */
	len = ntohs(pin->pin_total_len);
	if ((p = ibuf_seek(ibuf, off, len)) == NULL)
		return (-1);
	if (sc->sc_tap != -1)
		(void)write(sc->sc_tap, p, len);
	return (0);
}

int
ofp13_validate_packet_out(struct switchd *sc,
    struct sockaddr_storage *src, struct sockaddr_storage *dst,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	struct ofp_packet_out		*pout;
	size_t				 len;
	off_t				 off;
	struct ofp_action_header	*ah;
	struct ofp_action_output	*ao;

	off = 0;
	if ((pout = ibuf_seek(ibuf, off, sizeof(*pout))) == NULL) {
		log_debug("%s: seek failed: length %zd",
		    __func__, ibuf_length(ibuf));
		return (-1);
	}

	log_debug("\tbuffer %d port %s "
	    "actions length %u",
	    ntohl(pout->pout_buffer_id),
	    print_map(ntohl(pout->pout_in_port), ofp_port_map),
	    ntohs(pout->pout_actions_len));
	len = ntohl(pout->pout_actions_len);

	off += sizeof(*pout);
	while ((ah = ibuf_seek(ibuf, off, len)) != NULL &&
	    ntohs(ah->ah_len) >= (uint16_t)sizeof(*ah)) {
		switch (ntohs(ah->ah_type)) {
		case OFP_ACTION_OUTPUT:
			ao = (struct ofp_action_output *)ah;
			log_debug("\t\taction type %s length %d "
			    "port %s max length %d",
			    print_map(ntohs(ao->ao_type), ofp_action_map),
			    ntohs(ao->ao_len),
			    print_map(ntohs(ao->ao_port), ofp_port_map),
			    ntohs(ao->ao_max_len));
			break;
		default:
			log_debug("\t\taction type %s length %d",
			    print_map(ntohs(ah->ah_type), ofp_action_map),
			    ntohs(ah->ah_len));
			break;
		}
		if (pout->pout_buffer_id == (uint32_t)-1)
			break;
		off += ntohs(ah->ah_len);
	}

	return (0);
}

int
ofp13_validate_error(struct switchd *sc,
    struct sockaddr_storage *src, struct sockaddr_storage *dst,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	struct ofp_error		*err;
	off_t				 off;
	const char			*code;

	off = 0;
	if ((err = ibuf_seek(ibuf, off, sizeof(*err))) == NULL) {
		log_debug("%s: seek failed: length %zd",
		    __func__, ibuf_length(ibuf));
		return (-1);
	}

	switch (ntohs(err->err_type)) {
	case OFP_ERRTYPE_FLOW_MOD_FAILED:
		code = print_map(ntohs(err->err_code), ofp_errflowmod_map);
		break;
	case OFP_ERRTYPE_BAD_MATCH:
		code = print_map(ntohs(err->err_code), ofp_errmatch_map);
		break;
	case OFP_ERRTYPE_BAD_INSTRUCTION:
		code = print_map(ntohs(err->err_code), ofp_errinst_map);
		break;
	case OFP_ERRTYPE_BAD_REQUEST:
		code = print_map(ntohs(err->err_code), ofp_errreq_map);
		break;
	default:
		code = NULL;
		break;
	}

	log_debug("\terror type %s code %u%s%s",
	    print_map(ntohs(err->err_type), ofp_errtype_map),
	    ntohs(err->err_code),
	    code == NULL ? "" : ": ",
	    code == NULL ? "" : code);

	return (0);
}

int
ofp13_input(struct switchd *sc, struct switch_connection *con,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	if (ofp13_validate(sc, &con->con_peer, &con->con_local, oh, ibuf) != 0)
		return (-1);

	if (ofp13_callbacks[oh->oh_type].cb == NULL) {
		log_debug("message not supported: %s",
		    print_map(oh->oh_type, ofp_t_map));
		return (-1);
	}
	if (ofp13_callbacks[oh->oh_type].cb(sc, con, oh, ibuf) != 0)
		return (-1);

	return (0);
}

int
ofp13_hello(struct switchd *sc, struct switch_connection *con,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	if (switch_add(con) == NULL) {
		log_debug("%s: failed to add switch", __func__);
		ofp_close(con);
		return (-1);
	}

	/* Echo back the received Hello packet */
	oh->oh_version = OFP_V_1_3;
	oh->oh_length = htons(sizeof(*oh));
	oh->oh_xid = htonl(con->con_xidnxt++);
	if (ofp13_validate(sc, &con->con_local, &con->con_peer, oh, NULL) != 0)
		return (-1);
	ofp_send(con, oh, NULL);

	return (0);
}

int
ofp13_echo_request(struct switchd *sc, struct switch_connection *con,
    struct ofp_header *oh, struct ibuf *ibuf)
{
	/* Echo reply */
	oh->oh_type = OFP_T_ECHO_REPLY;
	if (ofp13_validate(sc, &con->con_local, &con->con_peer, oh, NULL) != 0)
		return (-1);
	ofp_send(con, oh, NULL);

	return (0);
}

int
ofp13_packet_match(struct ibuf *ibuf, struct packet *pkt, struct ofp_match *om)
{
	struct ether_header	*eh = pkt->pkt_eh;
	int			 unalignedsize;
	size_t			 padsize, startpos, endpos, omlen;

	if (eh == NULL)
		return (-1);

	startpos = ibuf->wpos;
	if (oxm_etheraddr(ibuf, 1, eh->ether_shost, NULL) == -1)
		return (-1);
	if (oxm_etheraddr(ibuf, 0, eh->ether_dhost, NULL) == -1)
		return (-1);
	endpos = ibuf->wpos;

	omlen = sizeof(*om) + (endpos - startpos);
	unalignedsize = (omlen % 8);
	if (unalignedsize)
		padsize = 8 - unalignedsize;
	else
		padsize = 0;

	om->om_type = htons(OFP_MATCH_OXM);
	om->om_length = htons(omlen);
	if (padsize && ibuf_advance(ibuf, padsize) == NULL)
		return (-1);

	return (0);
}

int
ofp13_packet_in(struct switchd *sc, struct switch_connection *con,
    struct ofp_header *ih, struct ibuf *ibuf)
{
	struct ofp_packet_in		*pin;
	struct ofp_packet_out		*pout;
	struct ofp_flow_mod		*fm;
	struct ofp_header		*oh;
	struct ofp_match		*om;
	struct ofp_ox_match		*oxm;
	struct packet			 pkt;
	struct ibuf			*obuf = NULL;
	int				 ret = -1;
	ssize_t				 len, mlen;
	uint32_t			 srcport = 0, dstport;
	int				 addflow = 0, sendbuffer = 0;
	off_t			 	 off, moff;
	void				*ptr;
	struct ofp_instruction_actions	*ia;

	if ((pin = ibuf_getdata(ibuf, sizeof(*pin))) == NULL)
		return (-1);

	/* We only handle no matches right now. */
	if (pin->pin_reason != OFP_PKTIN_REASON_NO_MATCH)
		return (-1);

	bzero(&pkt, sizeof(pkt));
	len = ntohs(pin->pin_total_len);

	/* very basic way of getting the source port */
	om = &pin->pin_match;
	mlen = ntohs(om->om_length);
	off = (OFP_ALIGN(mlen) + ETHER_ALIGN) - sizeof(pin->pin_match);
	moff = ibuf_dataoffset(ibuf);

	do {
		if ((oxm = ibuf_seek(ibuf, moff, sizeof(*oxm))) == NULL)
			return (-1);

		/* Find IN_PORT */
		switch (ntohs(oxm->oxm_class)) {
		case OFP_OXM_C_OPENFLOW_BASIC:
			switch (OFP_OXM_GET_FIELD(oxm)) {
			case OFP_XM_T_IN_PORT:
				moff += sizeof(*oxm);
				if ((ptr = ibuf_seek(ibuf, moff,
				    sizeof(srcport))) == NULL)
					return (-1);
				srcport = htonl(*(uint32_t *)ptr);
				mlen = 0; /* break loop */
				break;
			default:
				/* ignore unsupported match types */
				break;
			}
		default:
			/* ignore unsupported match classes */
			break;
		}
		moff += sizeof(*oxm) + oxm->oxm_length;
		mlen -= sizeof(*oxm) + oxm->oxm_length;
	} while (mlen > 0 && oxm->oxm_length);

	/* Skip all matches and seek to the packet */
	if (ibuf_getdata(ibuf, off) == NULL)
		return (-1);

	if (packet_input(sc, con->con_switch,
	    srcport, &dstport, ibuf, len, &pkt) == -1 ||
	    dstport > OFP_PORT_MAX) {
		/* fallback to flooding */
		dstport = OFP_PORT_FLOOD;
	} else if (srcport == dstport) {
		/*
		 * silently drop looping packet
		 * (don't use OFP_PORT_INPUT here)
		 */
		dstport = OFP_PORT_ANY;
	}

	if (dstport <= OFP_PORT_MAX)
		addflow = 1;

	if ((obuf = ibuf_static()) == NULL)
		goto done;

 again:
	if (addflow) {
		if ((fm = ibuf_advance(obuf, sizeof(*fm))) == NULL)
			goto done;

		oh = &fm->fm_oh;
		fm->fm_cookie = 0; /* XXX should we set a cookie? */
		fm->fm_command = htons(OFP_FLOWCMD_ADD);
		fm->fm_idle_timeout = htons(sc->sc_cache_timeout);
		fm->fm_hard_timeout = 0; /* permanent */
		fm->fm_priority = 0;
		fm->fm_buffer_id = pin->pin_buffer_id;
		fm->fm_flags = htons(OFP_FLOWFLAG_SEND_FLOW_REMOVED);
		if (pin->pin_buffer_id == OFP_PKTOUT_NO_BUFFER)
			sendbuffer = 1;

		/* Write flow matches to create an entry. */
		if (ofp13_packet_match(obuf, &pkt, &fm->fm_match) == -1)
			goto done;

		/*
		 * Write the instruction action header and add the output
		 * action.
		 */
		if ((ia = ibuf_advance(obuf, sizeof(*ia))) == NULL ||
		    action_output(obuf, dstport, 0) == -1)
			goto done;

		ia->ia_type = htons(OFP_INSTRUCTION_T_APPLY_ACTIONS);
		ia->ia_len = htons(sizeof(*ia) +
		    sizeof(struct ofp_action_output));
	} else {
		if ((pout = ibuf_advance(obuf, sizeof(*pout))) == NULL)
			goto done;

		oh = &pout->pout_oh;
		pout->pout_buffer_id = pin->pin_buffer_id;
		pout->pout_in_port = htonl(srcport);
		pout->pout_actions_len =
		    htons(sizeof(struct ofp_action_output));

		if (action_output(obuf, dstport, 0) == -1)
			goto done;

		/* Add optional packet payload */
		if (pin->pin_buffer_id == OFP_PKTOUT_NO_BUFFER &&
		    imsg_add(obuf, pkt.pkt_buf, pkt.pkt_len) == -1)
			goto done;
	}

	/* Set output header */
	oh->oh_version = OFP_V_1_3;
	oh->oh_length = htons(ibuf_length(obuf));
	oh->oh_type = addflow ? OFP_T_FLOW_MOD : OFP_T_PACKET_OUT;
	oh->oh_xid = htonl(con->con_xidnxt++);

	if (ofp13_validate(sc, &con->con_local, &con->con_peer, oh, obuf) != 0)
		return (-1);

	ofp_send(con, NULL, obuf);

	if (sendbuffer) {
		ibuf_release(obuf);

		/* loop to output the packet again */
		addflow = sendbuffer = 0;
		if ((obuf = ibuf_static()) == NULL)
			goto done;
		goto again;
	}

	ret = 0;
 done:
	ibuf_release(obuf);
	return (ret);
}

int
ofp13_flow_removed(struct switchd *sc, struct switch_connection *con,
    struct ofp_header *ih, struct ibuf *ibuf)
{
	struct ofp_flow_removed		*fr;

	if ((fr = ibuf_getdata(ibuf, sizeof(*fr))) == NULL)
		return (-1);

	log_debug("%s: cookie:%llu priority:%d reason:%d tableid:%d "
	    "duration(%u sec, %u nsec) idleto:%d hard:%d packet:%llu byte:%llu",
	    __func__, be64toh(fr->fr_cookie), ntohs(fr->fr_priority),
	    fr->fr_reason, fr->fr_table_id, ntohl(fr->fr_duration_sec),
	    ntohl(fr->fr_duration_nsec), ntohs(fr->fr_idle_timeout),
	    ntohs(fr->fr_hard_timeout),
	    be64toh(fr->fr_packet_count), be64toh(fr->fr_byte_count));

	switch (fr->fr_reason) {
	case OFP_FLOWREM_REASON_IDLE_TIMEOUT:
		log_debug("\tReason: IDLE TIMEOUT");
		break;
	case OFP_FLOWREM_REASON_HARD_TIMEOUT:
		log_debug("\tReason: HARD TIMEOUT");
		break;
	case OFP_FLOWREM_REASON_DELETE:
		log_debug("\tReason: DELETE");
		break;
	case OFP_FLOWREM_REASON_GROUP_DELETE:
		log_debug("\tReason: GROUP DELETE");
		break;
	default:
		log_debug("\tReason: UNKNOWN");
		break;
	}

	return (0);
}

/*
 * The valid commands for groups are:
 * OFP_GROUPCMD_{ADD,MODIFY,DELETE}
 *
 * The valid type for groups are:
 * OFP_GROUP_T_{ALL,SELECT,INDIRECT,FAST_FAILOVER}
 *
 * You have to update the gm->gm_oh.oh_length = htons(ibuf_length(ibuf));
 */
struct ofp_group_mod *
ofp13_group(struct switch_connection *con, struct ibuf *ibuf,
    uint32_t gid, uint16_t cmd, uint8_t type)
{
	struct ofp_group_mod		*gm;
	struct ofp_header		*oh;

	if ((gm = ibuf_advance(ibuf, sizeof(*gm))) == NULL)
		return (NULL);

	oh = &gm->gm_oh;
	oh->oh_version = OFP_V_1_3;
	oh->oh_type = OFP_T_GROUP_MOD;
	oh->oh_xid = htonl(con->con_xidnxt++);
	gm->gm_command = htons(cmd);
	gm->gm_type = type;
	gm->gm_group_id = htonl(gid);
	return (gm);
}

/* Remember to update b->b_len. */
struct ofp_bucket *
ofp13_bucket(struct ibuf *ibuf, uint16_t weight, uint32_t watchport,
    uint32_t watchgroup)
{
	struct ofp_bucket		*b;

	if ((b = ibuf_advance(ibuf, sizeof(*b))) == NULL)
		return (NULL);

	b->b_weight = htons(weight);
	b->b_watch_port = htonl(watchport);
	b->b_watch_group = htonl(watchgroup);
	return (b);
}

/* Appends an action with just the generic header. */
int
action_new(struct ibuf *ibuf, uint16_t type)
{
	struct ofp_action_header	*ah;

	if ((ah = ibuf_advance(ibuf, sizeof(*ah))) == NULL)
		return (-1);

	ah->ah_type = htons(type);
	ah->ah_len = htons(sizeof(*ah));
	return (0);
}

int
action_group(struct ibuf *ibuf, uint32_t group)
{
	struct ofp_action_group		*ag;

	if ((ag = ibuf_advance(ibuf, sizeof(*ag))) == NULL)
		return (-1);

	ag->ag_type = htons(OFP_ACTION_GROUP);
	ag->ag_len = sizeof(*ag);
	ag->ag_group_id = htonl(group);
	return (0);
}

int
action_output(struct ibuf *ibuf, uint32_t port, uint16_t maxlen)
{
	struct ofp_action_output	*ao;

	if ((ao = ibuf_advance(ibuf, sizeof(*ao))) == NULL)
		return (-1);

	ao->ao_type = htons(OFP_ACTION_OUTPUT);
	ao->ao_len = htons(sizeof(*ao));
	ao->ao_port = htonl(port);
	ao->ao_max_len = htons(maxlen);
	return (0);
}

/*
 * This action pushes VLAN/MPLS/PBB tags into the outermost part of the
 * packet. When the type is X ethertype must be Y:
 * - OFP_ACTION_PUSH_VLAN: ETHERTYPE_VLAN or ETHERTYPE_QINQ.
 * - OFP_ACTION_PUSH_MPLS: ETHERTYPE_MPLS or ETHERTYPE_MPLSCAST.
 * - OFP_ACTION_PUSH_PBB: ETHERTYPE_??? (0x88E7).
 */
int
action_push(struct ibuf *ibuf, uint16_t type, uint16_t ethertype)
{
	struct ofp_action_push		*ap;

	if ((ap = ibuf_advance(ibuf, sizeof(*ap))) == NULL)
		return (-1);

	ap->ap_type = htons(type);
	ap->ap_len = htons(sizeof(*ap));
	ap->ap_ethertype = htons(ethertype);
	return (0);
}

/* 
 * This action only pops the outermost VLAN tag and only one at a time,
 * you can only pop multiple VLANs with an action list that is only
 * availiable for OFP_INSTRUCTION_T_APPLY_ACTIONS.
 */
int
action_pop_vlan(struct ibuf *ibuf)
{
	return (action_new(ibuf, OFP_ACTION_POP_VLAN));
}

/*
 * Use this with caution since this will pop MPLS shim regardless of the
 * BoS bit state.
 */
int
action_pop_mpls(struct ibuf *ibuf, uint16_t ethertype)
{
	struct ofp_action_pop_mpls	*apm;

	if ((apm = ibuf_advance(ibuf, sizeof(*apm))) == NULL)
		return (-1);

	apm->apm_type = htons(OFP_ACTION_POP_MPLS);
	apm->apm_len = htons(sizeof(*apm));
	apm->apm_ethertype = htons(ethertype);
	return (0);
}

int
action_copyttlout(struct ibuf *ibuf)
{
	return (action_new(ibuf, OFP_ACTION_COPY_TTL_OUT));
}

int
action_copyttlin(struct ibuf *ibuf)
{
	return (action_new(ibuf, OFP_ACTION_COPY_TTL_IN));
}

int
action_decnwttl(struct ibuf *ibuf)
{
	return (action_new(ibuf, OFP_ACTION_DEC_NW_TTL));
}

/*
 * This function should be used with the oxm_*() family.
 *
 * After filling the action_setfield() with oxms you have to set the
 * asf_len with htons(size_of_oxms).
 */
struct ofp_action_set_field *
action_setfield(struct ibuf *ibuf)
{
	struct ofp_action_set_field	*asf;

	if ((asf = ibuf_advance(ibuf, sizeof(*asf))) == NULL)
		return (NULL);

	asf->asf_type = htons(OFP_ACTION_SET_FIELD);
	return (asf);
}

struct ofp_ox_match *
oxm_get(struct ibuf *ibuf, uint16_t field, int hasmask, uint8_t len)
{
	struct ofp_ox_match	*oxm;
	size_t			 oxmlen;

	/*
	 * When the mask is used we must always reserve double the space,
	 * because the mask field is the same size of the value.
	 */
	if (hasmask)
		len = len * 2;

	oxmlen = sizeof(*oxm) + len;
	if ((oxm = ibuf_advance(ibuf, oxmlen)) == NULL)
		return (NULL);

	oxm->oxm_class = htons(OFP_OXM_C_OPENFLOW_BASIC);
	oxm->oxm_length = len;
	OFP_OXM_SET_FIELD(oxm, field);
	if (hasmask)
		OFP_OXM_SET_HASMASK(oxm);

	return (oxm);
}

/*
 * OpenFlow port where the packet where received.
 * May be a physical port, a logical port or the reserved port OFPP_LOCAL.
 */
int
oxm_inport(struct ibuf *ibuf, uint32_t in_port)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IN_PORT, 0,
	    sizeof(in_port))) == NULL)
		return (-1);

	in_port = htonl(in_port);
	memcpy(oxm->oxm_value, &in_port, sizeof(in_port));
	return (0);
}

/*
 * Physical port on which the packet was received.
 * Requires: oxm_inport.
 */
int
oxm_inphyport(struct ibuf *ibuf, uint32_t in_phy_port)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IN_PHY_PORT, 0,
	    sizeof(in_phy_port))) == NULL)
		return (-1);

	in_phy_port = htonl(in_phy_port);
	memcpy(oxm->oxm_value, &in_phy_port, sizeof(in_phy_port));
	return (0);
}

/* Table metadata. */
int
oxm_metadata(struct ibuf *ibuf, int hasmask, uint64_t metadata, uint64_t mask)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_META, hasmask,
	    sizeof(metadata))) == NULL)
		return (-1);

	metadata = htobe64(metadata);
	memcpy(oxm->oxm_value, &metadata, sizeof(metadata));
	if (hasmask) {
		mask = htobe64(mask);
		memcpy(oxm->oxm_value + sizeof(metadata), &mask, sizeof(mask));
	}

	return (0);
}

int
oxm_etheraddr(struct ibuf *ibuf, int issrc, uint8_t *addr, uint8_t *mask)
{
	struct ofp_ox_match	*oxm;
	int			 type;
	int			 hasmask = (mask != NULL);

	type = issrc ? OFP_XM_T_ETH_SRC : OFP_XM_T_ETH_DST;
	if ((oxm = oxm_get(ibuf, type, hasmask, ETHER_ADDR_LEN)) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, addr, ETHER_ADDR_LEN);
	if (hasmask)
		memcpy(oxm->oxm_value + ETHER_ADDR_LEN, mask, ETHER_ADDR_LEN);

	return (0);
}

int
oxm_ethertype(struct ibuf *ibuf, uint16_t type)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ETH_TYPE, 0, sizeof(type))) == NULL)
		return (-1);

	type = htons(type);
	memcpy(oxm->oxm_value, &type, sizeof(type));
	return (0);
}

int
oxm_vlanvid(struct ibuf *ibuf, int hasmask, uint16_t vid, uint16_t mask)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_VLAN_VID, hasmask,
	    sizeof(vid))) == NULL)
		return (-1);

	/* VID uses only the 13 least significant bits. */
	vid &= 0x1fff;
	vid = htons(vid);
	memcpy(oxm->oxm_value, &vid, sizeof(vid));
	if (hasmask) {
		mask &= 0x1fff;
		mask = htons(mask);
		memcpy(oxm->oxm_value + sizeof(vid), &mask, sizeof(mask));
	}

	return (0);
}

/*
 * 802.1Q Prio from the outermost tag.
 *
 * Requires: oxm_vlanvid.
 */
int
oxm_vlanpcp(struct ibuf *ibuf, uint8_t pcp)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_VLAN_PCP, 0, sizeof(pcp))) == NULL)
		return (-1);

	/* PCP only uses the lower 3 bits. */
	pcp &= 0x07;
	memcpy(oxm->oxm_value, &pcp, sizeof(pcp));
	return (0);
}

/*
 * The Diff Serv Code Point (DSCP) bits avaliable in IPv4 ToS field or
 * IPv6 Traffic Class field.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipdscp(struct ibuf *ibuf, uint8_t dscp)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IP_DSCP, 0, sizeof(dscp))) == NULL)
		return (-1);

	/* Only the 6 lower bits have meaning. */
	dscp &= 0x3F;
	memcpy(oxm->oxm_value, &dscp, sizeof(dscp));
	return (0);
}

/*
 * The ECN (Explicit Congestion Notification) bits of IP headers.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipecn(struct ibuf *ibuf, uint8_t ecn)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IP_ECN, 0, sizeof(ecn))) == NULL)
		return (-1);

	/* Only the 2 most significant bits have meaning. */
	ecn &= 0x03;
	memcpy(oxm->oxm_value, &ecn, sizeof(ecn));
	return (0);
}

/*
 * The IP protocol byte.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipproto(struct ibuf *ibuf, uint8_t proto)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IP_PROTO, 0, sizeof(proto))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, &proto, sizeof(proto));
	return (0);
}

/*
 * The IPv4 address source/destination.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP).
 */
int
oxm_ipaddr(struct ibuf *ibuf, int issrc, int hasmask, uint32_t addr,
    uint32_t mask)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_IPV4_SRC : OFP_XM_T_IPV4_DST;
	if ((oxm = oxm_get(ibuf, type, hasmask, sizeof(addr))) == NULL)
		return (-1);

	addr = htonl(addr);
	memcpy(oxm->oxm_value, &addr, sizeof(addr));
	if (hasmask) {
		mask = htonl(mask);
		memcpy(oxm->oxm_value + sizeof(addr), &mask, sizeof(mask));
	}

	return (0);
}

/*
 * The TCP source/destination port.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6)
 * and oxm_ipproto(IPPROTO_TCP).
 */
int
oxm_tcpport(struct ibuf *ibuf, int issrc, uint16_t port)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_TCP_SRC : OFP_XM_T_TCP_DST;
	if ((oxm = oxm_get(ibuf, type, 0, sizeof(port))) == NULL)
		return (-1);

	port = htons(port);
	memcpy(oxm->oxm_value, &port, sizeof(port));
	return (0);
}

/*
 * The UDP source/destination port.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6)
 * and oxm_ipproto(IPPROTO_UDP).
 */
int
oxm_udpport(struct ibuf *ibuf, int issrc, uint16_t port)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_UDP_SRC : OFP_XM_T_UDP_DST;
	if ((oxm = oxm_get(ibuf, type, 0, sizeof(port))) == NULL)
		return (-1);

	port = htons(port);
	memcpy(oxm->oxm_value, &port, sizeof(port));
	return (0);
}

/*
 * The SCTP source/destination port.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IP) or oxm_ethertype(ETHERTYPE_IPV6)
 * and oxm_ipproto(IPPROTO_??? -- 132).
 */
int
oxm_sctpport(struct ibuf *ibuf, int issrc, uint16_t port)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_SCTP_SRC : OFP_XM_T_SCTP_DST;
	if ((oxm = oxm_get(ibuf, type, 0, sizeof(port))) == NULL)
		return (-1);

	port = htons(port);
	memcpy(oxm->oxm_value, &port, sizeof(port));
	return (0);
}

/*
 * The ICMPv4 type in the ICMP header.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP) and oxm_ipproto(IPPROTO_ICMP).
 */
int
oxm_icmpv4type(struct ibuf *ibuf, uint8_t type)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ICMPV4_TYPE, 0,
	    sizeof(type))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, &type, sizeof(type));
	return (0);
}

/*
 * The ICMPv4 code in the ICMP header.
 *
 * Requires: oxm_ethertype(ETHERTYPE_IP) and oxm_ipproto(IPPROTO_ICMP).
 */
int
oxm_icmpv4code(struct ibuf *ibuf, uint8_t code)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ICMPV4_CODE, 0,
	    sizeof(code))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, &code, sizeof(code));
	return (0);
}

/*
 * ARP opcode.
 *
 * Requires: oxm_ethertype(ETHERTYPE_ARP).
 */
int
oxm_arpop(struct ibuf *ibuf, uint16_t op)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ARP_OP, 0, sizeof(op))) == NULL)
		return (-1);

	op = htons(op);
	memcpy(oxm->oxm_value, &op, sizeof(op));
	return (0);
}

/*
 * ARP source/target protocol address.
 *
 * Requires: oxm_ethertype(ETHERTYPE_ARP).
 */
int
oxm_arpaddr(struct ibuf *ibuf, int issrc, int hasmask, uint32_t addr,
    uint32_t mask)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_ARP_SPA : OFP_XM_T_ARP_TPA;
	if ((oxm = oxm_get(ibuf, type, hasmask, sizeof(addr))) == NULL)
		return (-1);

	addr = htonl(addr);
	memcpy(oxm->oxm_value, &addr, sizeof(addr));
	if (hasmask) {
		mask = htonl(mask);
		memcpy(oxm->oxm_value + sizeof(addr), &mask, sizeof(mask));
	}

	return (0);
}

/*
 * ARP source/target hardware address.
 *
 * Requires: oxm_ethertype(ETHERTYPE_ARP).
 */
int
oxm_arphaddr(struct ibuf *ibuf, int issrc, uint8_t *addr, uint8_t *mask)
{
	struct ofp_ox_match	*oxm;
	int			 type;
	int			 hasmask = (mask != NULL);

	type = issrc ? OFP_XM_T_ARP_SHA : OFP_XM_T_ARP_THA;
	if ((oxm = oxm_get(ibuf, type, hasmask, ETHER_ADDR_LEN)) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, addr, ETHER_ADDR_LEN);
	if (hasmask)
		memcpy(oxm->oxm_value + ETHER_ADDR_LEN, mask, ETHER_ADDR_LEN);

	return (0);
}

/*
 * The source or destination of the IPv6 address.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipv6addr(struct ibuf *ibuf, int issrc, struct in6_addr *addr,
    struct in6_addr *mask)
{
	struct ofp_ox_match	*oxm;
	int			 type;
	int			 hasmask = (mask != NULL);

	type = issrc ? OFP_XM_T_IPV6_SRC : OFP_XM_T_IPV6_DST;
	if ((oxm = oxm_get(ibuf, type, hasmask, sizeof(*addr))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, addr, sizeof(*addr));
	if (hasmask)
		memcpy(oxm->oxm_value + sizeof(*addr), mask, sizeof(*mask));

	return (0);
}

/*
 * The IPv6 flow label field.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipv6flowlabel(struct ibuf *ibuf, int hasmask, uint32_t flowlabel,
    uint32_t mask)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IPV6_FLABEL, hasmask,
	    sizeof(flowlabel))) == NULL)
		return (-1);

	/*
         * 12 most significants bits forced to 0 and only the 20 lowers
         * bits have meaning.
	 */
	flowlabel &= 0x000FFFFFU;
	flowlabel = htonl(flowlabel);
	memcpy(oxm->oxm_value, &flowlabel, sizeof(flowlabel));
	if (hasmask) {
		mask &= 0x000FFFFFU;
		mask = htonl(mask);
		memcpy(oxm->oxm_value + sizeof(flowlabel), &mask, sizeof(mask));
	}

	return (0);
}

/*
 * The ICMPv6 type in ICMP header.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6) and oxm_ipproto(IPPROTO_ICMPV6).
 */
int
oxm_icmpv6type(struct ibuf *ibuf, uint8_t type)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ICMPV6_TYPE, 0,
	    sizeof(type))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, &type, sizeof(type));
	return (0);
}

/*
 * The ICMPv6 code in ICMP header.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6) and oxm_ipproto(IPPROTO_ICMPV6).
 */
int
oxm_icmpv6code(struct ibuf *ibuf, uint8_t code)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_ICMPV6_CODE, 0,
	    sizeof(code))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, &code, sizeof(code));
	return (0);
}

/*
 * The target address in neighbour discovery message.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6), oxm_ipproto(IPPROTO_ICMPV6)
 * and oxm_icmpv6type(ND_NEIGHBOR_SOLICIT) or
 * oxm_icmpv6type(ND_NEIGHBOR_ADVERT).
 */
int
oxm_ipv6ndtarget(struct ibuf *ibuf, struct in6_addr *addr)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IPV6_ND_TARGET, 0,
	    sizeof(*addr))) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, addr, sizeof(*addr));
	return (0);
}

/*
 * The source link-layer address in an IPv6 Neighbour discovery.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6), oxm_ipproto(IPPROTO_ICMPV6)
 * and oxm_icmpv6type(ND_NEIGHBOR_SOLICIT).
 */
int
oxm_ipv6ndlinkaddr(struct ibuf *ibuf, int issrc, uint8_t *addr)
{
	struct ofp_ox_match	*oxm;
	int			 type;

	type = issrc ? OFP_XM_T_IPV6_ND_SLL : OFP_XM_T_IPV6_ND_TLL;
	if ((oxm = oxm_get(ibuf, type, 0, ETHER_ADDR_LEN)) == NULL)
		return (-1);

	memcpy(oxm->oxm_value, addr, ETHER_ADDR_LEN);
	return (0);
}

/*
 * The label in the MPLS shim.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_MPLS) or
 * oxm_ethertype(ETHERTYPE_MPLS_MCAST).
 */
int
oxm_mplslabel(struct ibuf *ibuf, uint32_t label)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_MPLS_LABEL, 0,
	    sizeof(label))) == NULL)
		return (-1);

	label &= MPLS_LABEL_MASK;
	label = htonl(label);
	memcpy(oxm->oxm_value, &label, sizeof(label));
	return (0);
}

/*
 * The TC in the first MPLS shim.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_MPLS) or
 * oxm_ethertype(ETHERTYPE_MPLS_MCAST).
 */
int
oxm_mplstc(struct ibuf *ibuf, uint8_t tc)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_MPLS_TC, 0, sizeof(tc))) == NULL)
		return (-1);

	tc &= 0x07;
	memcpy(oxm->oxm_value, &tc, sizeof(tc));
	return (0);
}

/*
 * The BoS bit in the first MPLS shim.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_MPLS) or
 * oxm_ethertype(ETHERTYPE_MPLS_MCAST).
 */
int
oxm_mplsbos(struct ibuf *ibuf, uint8_t bos)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_MPLS_BOS, 0, sizeof(bos))) == NULL)
		return (-1);

	bos &= 0x01;
	memcpy(oxm->oxm_value, &bos, sizeof(bos));
	return (0);
}

/*
 * Comment shamelessly taken from OpenFlow 1.3.5 specification.
 *
 * Metadata associated with a logical port.
 *
 * If the logical port performs encapsulation and decapsulation, this
 * is the demultiplexing field from the encapsulation header.
 * For example, for a packet received via GRE tunnel including a (32-bit) key,
 * the key is stored in the low 32-bits and the high bits are zeroed.
 * For a MPLS logical port, the low 20 bits represent the MPLS Label.
 * For a VxLAN logical port, the low 24 bits represent the VNI.
 * If the packet is not received through a logical port, the value is 0.
 */
int
oxm_tunnelid(struct ibuf *ibuf, int hasmask, uint64_t id, uint64_t mask)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_TUNNEL_ID, hasmask,
	    sizeof(id))) == NULL)
		return (-1);

	id = htobe64(id);
	memcpy(oxm->oxm_value, &id, sizeof(id));
	if (hasmask) {
		mask = htobe64(mask);
		memcpy(oxm->oxm_value + sizeof(id), &mask, sizeof(mask));
	}
	return (0);
}

/*
 * The IPv6 extension header.
 *
 * Tip: use the OFP_XM_IPV6_EXTHDR_* macros.
 *
 * Requirements: oxm_ethertype(ETHERTYPE_IPV6).
 */
int
oxm_ipv6exthdr(struct ibuf *ibuf, int hasmask, uint16_t exthdr, uint16_t mask)
{
	struct ofp_ox_match	*oxm;

	if ((oxm = oxm_get(ibuf, OFP_XM_T_IPV6_EXTHDR, hasmask,
	    sizeof(exthdr))) == NULL)
		return (-1);

	/* Only the lower 9 bits have meaning. */
	exthdr &= 0x01FF;
	exthdr = htons(exthdr);
	memcpy(oxm->oxm_value, &exthdr, sizeof(exthdr));
	if (hasmask) {
		mask &= 0x01FF;
		mask = htons(mask);
		memcpy(oxm->oxm_value + sizeof(exthdr), &mask, sizeof(mask));
	}
	return (0);
}
