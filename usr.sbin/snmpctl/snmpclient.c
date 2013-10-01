/*	$OpenBSD: snmpclient.c,v 1.5 2013/10/01 17:20:39 reyk Exp $	*/

/*
 * Copyright (c) 2013 Reyk Floeter <reyk@openbsd.org>
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

#include <sys/queue.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/tree.h>

#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <netdb.h>
#include <errno.h>
#include <event.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <poll.h>
#include <err.h>
#include <pwd.h>

#include "snmpd.h"
#include "mib.h"
#include "ber.h"
#include "parser.h"

struct snmpc {
	size_t			 sc_root_len;
	struct ber_oid		 sc_root_oid;
	struct ber_oid		 sc_last_oid;
	struct ber_oid		 sc_oid;
	struct sockaddr_storage	 sc_addr;
	socklen_t		 sc_addr_len;
	u_int32_t		 sc_msgid;
	int			 sc_fd;
	int			 sc_retry;
	int			 sc_retry_max;
	const char		*sc_community;
	int			 sc_version;
	int			 sc_nresp;
};

#define	SNMPC_RETRY_MAX		3
#define SNMPC_COMMUNITY		"public"
#define SNMPC_OID		"system"
#define SNMPC_MAXREPETITIONS	10

void	 snmpc_request(struct snmpc *, u_long);
int	 snmpc_response(struct snmpc *);
int	 snmpc_sendreq(struct snmpc *, u_long);
int	 snmpc_recvresp(int, int, u_int32_t, struct ber_element **);

void
snmpclient(struct parse_result *res)
{
	struct snmpc		 sc;
	struct addrinfo		 hints, *ai, *ai0;
	int			 s;
	int			 error;
	struct passwd		*pw;

	bzero(&sc, sizeof(sc));

	/* Get client configuration */
	if (res->oid == NULL)
		res->oid = strdup(SNMPC_OID);
	if (res->community == NULL)
		res->community = strdup(SNMPC_COMMUNITY);
	if (res->version == -1 || res->version > SNMP_V2)
		res->version = SNMP_V2;
	if (res->oid == NULL || res->community == NULL)
		err(1, "strdup");

	/* Checks */
	if ((res->action == BULKWALK) && (res->version < SNMP_V2))
		errx(1, "invalid version for bulkwalk");

	/* Resolve target host name */
	bzero(&hints, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_socktype = SOCK_DGRAM;
	error = getaddrinfo(res->host, "snmp", &hints, &ai0);
	if (error)
		errx(1, "%s", gai_strerror(error));
	s = -1;
	for (ai = ai0; ai; ai = ai->ai_next) {
		if ((s = socket(ai->ai_family, ai->ai_socktype,
                    ai->ai_protocol)) == -1) {
			continue;
		}
		/* use first available address */
 		break;
	}
	if (s == -1)
		errx(1, "invalid host");

	bcopy(ai->ai_addr, &sc.sc_addr, ai->ai_addrlen);
	sc.sc_addr_len = ai->ai_addrlen;
	freeaddrinfo(ai0);

	sc.sc_fd = s;
	sc.sc_retry_max = SNMPC_RETRY_MAX;
	sc.sc_community = res->community;
	sc.sc_version = res->version;

	/*
	 * Set up the root OID and get the prefix length to shorten the
	 * printed OID strings of the children.
	 */
	if (smi_string2oid(res->oid, &sc.sc_oid) == -1)
		errx(1, "oid");

	bcopy(&sc.sc_oid, &sc.sc_root_oid, sizeof(sc.sc_root_oid));
	bcopy(&sc.sc_oid, &sc.sc_last_oid, sizeof(sc.sc_last_oid));
	if (sc.sc_oid.bo_n > 2)
		sc.sc_root_len = sc.sc_oid.bo_n - 1;

	/*
	 * Drop privileges to mitigate the risk when running as root.
	 */
	if (geteuid() == 0) {
		if ((pw = getpwnam(SNMPD_USER)) == NULL)
			err(1, "snmpctl: getpwnam");
#ifndef DEBUG
		if (chroot(pw->pw_dir) == -1)
			err(1, "snmpctl: chroot");
		if (chdir("/") == -1)
			err(1, "snmpctl: chdir(\"/\")");
		if (setgroups(1, &pw->pw_gid) ||
		    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
		    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
			err(1, "snmpctl: cannot drop privileges");
#endif
	}

	if (res->action == GET)
		snmpc_request(&sc, SNMP_C_GETREQ);
	else if (res->action == BULKWALK)
		snmpc_request(&sc, SNMP_C_GETBULKREQ);
	else
		snmpc_request(&sc, SNMP_C_GETNEXTREQ);

	close(sc.sc_fd);
}

void
snmpc_request(struct snmpc *sc, u_long type)
{
	struct pollfd		 pfd[1];
	int			 nfds, ret;

	/* Send SNMP request */
	if (snmpc_sendreq(sc, type) == -1)
		err(1, "request failed");

	/* Wait for response */
	pfd[0].fd = sc->sc_fd;
	pfd[0].events = POLLIN;
	nfds = poll(pfd, 1, 3 * 1000);
	if (nfds == -1 || (pfd[0].revents & (POLLERR|POLLHUP|POLLNVAL)))
		errx(1, "poll error");
	if (nfds == 0) {
		if (sc->sc_retry++ < sc->sc_retry_max) {
			warnx("time out, retry %d/%d",
			    sc->sc_retry, sc->sc_retry_max);
			snmpc_request(sc, type);
			return;
		}
		errx(1, "time out");
	}
	sc->sc_retry = 0;

	if ((ret = snmpc_response(sc)) != 0) { 
		if (ret == -1)
			err(1, "response");
		return;
	}	

	if (type == SNMP_C_GETREQ)
		return;

	snmpc_request(sc, type);
}

int
snmpc_response(struct snmpc *sc)
{
	char			 buf[BUFSIZ];
	struct ber_element	*resp = NULL, *s, *e;
	char			*value = NULL, *p;
	int			 c, ret = 0;

	/* Receive response */
	if (snmpc_recvresp(sc->sc_fd, sc->sc_version,
	    sc->sc_msgid, &resp) == -1)
		return (-1);

	if (ber_scanf_elements(resp, "{SS{SSSS{e}}", &s) != 0)
		goto fail;

	for (c = 0; s != NULL; s = s->be_next, c++) {
		if (ber_scanf_elements(s, "{oe}", &sc->sc_oid, &e) != 0)
			goto fail;

		if (c && ber_oid_cmp(&sc->sc_oid, &sc->sc_last_oid) == 0)
			continue;

		/* Break if the returned OID is not a child of the root. */
		if (sc->sc_nresp &&
		    (ber_oid_cmp(&sc->sc_root_oid, &sc->sc_oid) != 2 ||
		    e->be_type == BER_TYPE_NULL)) {
			ret = 1; 
			break;
		}		

		if ((value = smi_print_element(e)) != NULL) {
			smi_oid2string(&sc->sc_oid, buf, sizeof(buf),
			    sc->sc_root_len);
			p = buf;
			if (*p != '\0')
				printf("%s=%s\n", p, value);
			else
				printf("%s\n", value);
			free(value);
		}
		bcopy(&sc->sc_oid, &sc->sc_last_oid, sizeof(sc->sc_last_oid));
	}

	sc->sc_nresp++;

	ber_free_elements(resp);
	return (ret);

 fail:
	if (resp != NULL)
		ber_free_elements(resp);
	errno = EINVAL;
	return (-1);
}

int
snmpc_sendreq(struct snmpc *sc, u_long type)
{
	struct ber_element	*root, *b;
	struct ber		 ber;
	ssize_t			 len;
	u_int8_t		*ptr;
	int			 erroridx = 0;

	if (type == SNMP_C_GETBULKREQ)
		erroridx = SNMPC_MAXREPETITIONS;

	/* SNMP header */
	sc->sc_msgid = arc4random();
	if ((root = ber_add_sequence(NULL)) == NULL ||
	    (b = ber_printf_elements(root, "ds{tddd{{O0}}",
	    sc->sc_version, sc->sc_community, BER_CLASS_CONTEXT, type,
	    sc->sc_msgid, 0, erroridx, &sc->sc_oid)) == NULL) {
		errno = EINVAL;
		return (-1);
	}

#ifdef DEBUG
	fprintf(stderr, "REQUEST(%lu):\n", type);
	smi_debug_elements(root);
#endif

	bzero(&ber, sizeof(ber));
	ber.fd = -1;

	len = ber_write_elements(&ber, root);
	if (ber_get_writebuf(&ber, (void *)&ptr) < 1)
		return (-1);

	if (sendto(sc->sc_fd, ptr, len, 0,
	    (struct sockaddr *)&sc->sc_addr, sc->sc_addr_len) == -1)
		return (-1);

	return (0);
}

int
snmpc_recvresp(int s, int msgver, u_int32_t msgid,
    struct ber_element **respptr)
{
	char			 buf[READ_BUF_SIZE];
	ssize_t			 rlen;
	struct ber		 ber;
	struct ber_element	*resp = NULL;
	char			*comn;
	long long		 ver, id;

	if ((rlen = recv(s, buf, sizeof(buf), MSG_WAITALL)) == -1)
		return (-1);

	bzero(&ber, sizeof(ber));
	ber.fd = -1;
	ber_set_application(&ber, smi_application);
	ber_set_readbuf(&ber, buf, rlen);

#ifdef DEBUG
	fprintf(stderr, "RESPONSE (%ld bytes):\n", rlen);
#endif

	resp = ber_read_elements(&ber, NULL);
	if (resp == NULL)
		goto fail;

#ifdef DEBUG
	smi_debug_elements(resp);
#endif

	if (ber_scanf_elements(resp, "{is{i", &ver, &comn, &id) != 0)
		goto fail;
	if (!(msgver == (int)ver && msgid == (u_int32_t)id))
		goto fail;

	*respptr = resp;
	return (0);

 fail:
	if (resp != NULL)
		ber_free_elements(resp);
	errno = EINVAL;
	return (-1);
}
