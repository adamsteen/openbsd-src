/*
 * Copyright (c) 2019 Martijn van Duren <martijn@openbsd.org>
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
#include <sys/socket.h>

#include <arpa/inet.h>

#include <ctype.h>
#include <endian.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "agentx.h"

#define AGENTX_PDU_HEADER 20

static int agentx_pdu_need(struct agentx *, size_t);
static int agentx_pdu_header(struct agentx *,
    enum agentx_pdu_type, uint8_t, uint32_t, uint32_t, uint32_t,
    struct agentx_ostring *);
static uint32_t agentx_packetid(struct agentx *);
static uint32_t agentx_pdu_queue(struct agentx *);
static int agentx_pdu_add_uint16(struct agentx *, uint16_t);
static int agentx_pdu_add_uint32(struct agentx *, uint32_t);
static int agentx_pdu_add_uint64(struct agentx *, uint64_t);
static int agentx_pdu_add_oid(struct agentx *, struct agentx_oid *, int);
static int agentx_pdu_add_str(struct agentx *, struct agentx_ostring *);
static int agentx_pdu_add_varbindlist( struct agentx *, struct agentx_varbind *,
    size_t);
static uint16_t agentx_pdutoh16(struct agentx_pdu_header *, uint8_t *);
static uint32_t agentx_pdutoh32(struct agentx_pdu_header *, uint8_t *);
static uint64_t agentx_pdutoh64(struct agentx_pdu_header *, uint8_t *);
static ssize_t agentx_pdutooid(struct agentx_pdu_header *, struct agentx_oid *,
    uint8_t *, size_t);
static ssize_t agentx_pdutoostring(struct agentx_pdu_header *,
    struct agentx_ostring *, uint8_t *, size_t);
static ssize_t agentx_pdutovarbind(struct agentx_pdu_header *,
    struct agentx_varbind *, uint8_t *, size_t);

struct agentx *
agentx_new(int fd)
{
	struct agentx *ax;

	if (fd == -1) {
		errno = EINVAL;
		return NULL;
	}

	if ((ax = calloc(1, sizeof(*ax))) == NULL)
		return NULL;
	ax->ax_fd = fd;
	if ((ax->ax_rbuf = malloc(512)) == NULL)
		goto fail;
	ax->ax_rbsize = 512;
	ax->ax_byteorder = AGENTX_BYTE_ORDER_NATIVE;

	return ax;

fail:
	free(ax);
	return NULL;
}

void
agentx_free(struct agentx *ax)
{
	if (ax == NULL)
		return;
	close(ax->ax_fd);
	free(ax->ax_rbuf);
	free(ax->ax_wbuf);
	free(ax->ax_packetids);
	free(ax);
}

struct agentx_pdu *
agentx_recv(struct agentx *ax)
{
	struct agentx_pdu *pdu;
	struct agentx_pdu_header header;
	struct agentx_pdu_response *response;
	struct agentx_varbind *varbind;
	struct agentx_pdu_searchrangelist *srl = NULL;
	struct agentx_pdu_varbindlist *vbl;
	struct agentx_searchrange *sr;
	size_t rbsize, packetidx = 0, i, rawlen;
	ssize_t nread;
	uint8_t *u8;
	uint8_t *rbuf;
	int found;

	/* Only read a single packet at a time to make sure libevent triggers */
	if (ax->ax_rblen < AGENTX_PDU_HEADER) {
		if ((nread = read(ax->ax_fd, ax->ax_rbuf + ax->ax_rblen,
		    AGENTX_PDU_HEADER - ax->ax_rblen)) == 0) {
			errno = ECONNRESET;
			return NULL;
		}
		if (nread == -1)
			return NULL;
		ax->ax_rblen += nread;
		if (ax->ax_rblen < AGENTX_PDU_HEADER) {
			errno = EAGAIN;
			return NULL;
		}
	}
	u8 = ax->ax_rbuf;
	header.aph_version = *u8++;
	header.aph_type = *u8++;
	header.aph_flags = *u8++;
	u8++;
	header.aph_sessionid = agentx_pdutoh32(&header, u8);
	u8 += 4;
	header.aph_transactionid = agentx_pdutoh32(&header, u8);
	u8 += 4;
	header.aph_packetid = agentx_pdutoh32(&header, u8);
	u8 += 4;
	header.aph_plength = agentx_pdutoh32(&header, u8);

	if (header.aph_version != 1) {
		errno = EPROTO;
		return NULL;
	}
	if (ax->ax_rblen < AGENTX_PDU_HEADER + header.aph_plength) {
		if (AGENTX_PDU_HEADER + header.aph_plength > ax->ax_rbsize) {
			rbsize = (((AGENTX_PDU_HEADER + header.aph_plength)
			    / 512) + 1) * 512;
			if ((rbuf = recallocarray(ax->ax_rbuf, ax->ax_rbsize,
			    rbsize, sizeof(*rbuf))) == NULL)
				return NULL;
			ax->ax_rbsize = rbsize;
			ax->ax_rbuf = rbuf;
		}
		nread = read(ax->ax_fd, ax->ax_rbuf + ax->ax_rblen,
		    header.aph_plength - (ax->ax_rblen - AGENTX_PDU_HEADER));
		if (nread == 0)
			errno = ECONNRESET;
		if (nread <= 0)
			return NULL;
		ax->ax_rblen += nread;
		if (ax->ax_rblen < AGENTX_PDU_HEADER + header.aph_plength) {
			errno = EAGAIN;
			return NULL;
		}
	}

	if ((pdu = calloc(1, sizeof(*pdu))) == NULL)
		return NULL;

	memcpy(&(pdu->ap_header), &header, sizeof(header));

#if defined(AGENTX_DEBUG) && defined(AGENTX_DEBUG_VERBOSE)
	{
		char chars[4];
		int print = 1;

		fprintf(stderr, "received packet:\n");
		for (i = 0; i < pdu->ap_header.aph_plength + AGENTX_PDU_HEADER;
		    i++) {
			fprintf(stderr, "%02hhx ", ax->ax_rbuf[i]);
			chars[i % 4] = ax->ax_rbuf[i];
			if (!isprint(ax->ax_rbuf[i]))
				print = 0;
			if (i % 4 == 3) {
				if (print)
					fprintf(stderr, "%.4s", chars);
				fprintf(stderr, "\n");
				print = 1;
			}
		}
	}
#endif

	u8 = (ax->ax_rbuf) + AGENTX_PDU_HEADER;
	rawlen = pdu->ap_header.aph_plength;
	if (pdu->ap_header.aph_flags & AGENTX_PDU_FLAG_NON_DEFAULT_CONTEXT) {
		nread = agentx_pdutoostring(&header, &(pdu->ap_context), u8,
		    rawlen);
		if (nread == -1)
			goto fail;
		rawlen -= nread;
		u8 += nread;
	}

	switch (pdu->ap_header.aph_type) {
	case AGENTX_PDU_TYPE_GETBULK:
		if (rawlen < 4) {
			errno = EPROTO;
			goto fail;
		}
		pdu->ap_payload.ap_getbulk.ap_nonrep =
		    agentx_pdutoh16(&header, u8);
		u8 += 2;
		pdu->ap_payload.ap_getbulk.ap_maxrep =
		    agentx_pdutoh16(&header, u8);
		u8 += 2;
		srl = &(pdu->ap_payload.ap_getbulk.ap_srl);
		rawlen -= 4;
		/* FALLTHROUGH */
	case AGENTX_PDU_TYPE_GET:
	case AGENTX_PDU_TYPE_GETNEXT:
		if (pdu->ap_header.aph_type != AGENTX_PDU_TYPE_GETBULK)
			srl = &(pdu->ap_payload.ap_srl);
		while (rawlen > 0 ) {
			srl->ap_nsr++;
			sr = reallocarray(srl->ap_sr, srl->ap_nsr, sizeof(*sr));
			if (sr == NULL)
				goto fail;
			srl->ap_sr = sr;
			sr += (srl->ap_nsr - 1);
			if ((nread = agentx_pdutooid(&header, &(sr->asr_start),
			    u8, rawlen)) == -1)
				goto fail;
			rawlen -= nread;
			u8 += nread;
			if ((nread = agentx_pdutooid(&header, &(sr->asr_stop),
			    u8, rawlen)) == -1)
				goto fail;
			rawlen -= nread;
			u8 += nread;
		}
		break;
	case AGENTX_PDU_TYPE_TESTSET:
		vbl = &(pdu->ap_payload.ap_vbl);
		while (rawlen > 0) {
			varbind = recallocarray(vbl->ap_varbind,
			    vbl->ap_nvarbind, vbl->ap_nvarbind + 1,
			    sizeof(*(vbl->ap_varbind)));
			if (varbind == NULL)
				goto fail;
			vbl->ap_varbind = varbind;
			nread = agentx_pdutovarbind(&header,
			    &(vbl->ap_varbind[vbl->ap_nvarbind]), u8, rawlen);
			if (nread == -1)
				goto fail;
			vbl->ap_nvarbind++;
			u8 += nread;
			rawlen -= nread;
		}
		break;
	case AGENTX_PDU_TYPE_COMMITSET:
	case AGENTX_PDU_TYPE_UNDOSET:
	case AGENTX_PDU_TYPE_CLEANUPSET:
		if (rawlen != 0) {
			errno = EPROTO;
			goto fail;
		}
		break;
	case AGENTX_PDU_TYPE_RESPONSE:
		if (ax->ax_packetids != NULL) {
			found = 0;
			for (i = 0; ax->ax_packetids[i] != 0; i++) {
				if (ax->ax_packetids[i] ==
				    pdu->ap_header.aph_packetid) {
					packetidx = i;
					found = 1;
				}
			}
			if (found) {
				ax->ax_packetids[packetidx] =
				    ax->ax_packetids[i - 1];
				ax->ax_packetids[i - 1] = 0;
			} else {
				errno = EPROTO;
				goto fail;
			}
		}
		if (rawlen < 8) {
			errno = EPROTO;
			goto fail;
		}
		response = &(pdu->ap_payload.ap_response);
		response->ap_uptime = agentx_pdutoh32(&header, u8);
		u8 += 4;
		response->ap_error = agentx_pdutoh16(&header, u8);
		u8 += 2;
		response->ap_index = agentx_pdutoh16(&header, u8);
		u8 += 2;
		rawlen -= 8;
		while (rawlen > 0) {
			varbind = recallocarray(response->ap_varbindlist,
			    response->ap_nvarbind, response->ap_nvarbind + 1,
			    sizeof(*(response->ap_varbindlist)));
			if (varbind == NULL)
				goto fail;
			response->ap_varbindlist = varbind;
			nread = agentx_pdutovarbind(&header,
			    &(response->ap_varbindlist[response->ap_nvarbind]),
			    u8, rawlen);
			if (nread == -1)
				goto fail;
			response->ap_nvarbind++;
			u8 += nread;
			rawlen -= nread;
		}
		break;
	default:
		pdu->ap_payload.ap_raw = malloc(pdu->ap_header.aph_plength);
		if (pdu->ap_payload.ap_raw == NULL)
			goto fail;
		memcpy(pdu->ap_payload.ap_raw, ax->ax_rbuf + AGENTX_PDU_HEADER,
		    pdu->ap_header.aph_plength);
		break;
	}

	ax->ax_rblen = 0;

	return pdu;
fail:
	agentx_pdu_free(pdu);
	return NULL;
}

static int
agentx_pdu_need(struct agentx *ax, size_t need)
{
	uint8_t *wbuf;
	size_t wbsize;

	if (ax->ax_wbtlen >= ax->ax_wbsize) {
		wbsize = ((ax->ax_wbtlen / 512) + 1) * 512;
		wbuf = recallocarray(ax->ax_wbuf, ax->ax_wbsize, wbsize, 1);
		if (wbuf == NULL) {
			ax->ax_wbtlen = ax->ax_wblen;
			return -1;
		}
		ax->ax_wbsize = wbsize;
		ax->ax_wbuf = wbuf;
	}

	return 0;
}

ssize_t
agentx_send(struct agentx *ax)
{
	ssize_t nwrite;

	if (ax->ax_wblen != ax->ax_wbtlen) {
		errno = EALREADY;
		return -1;
	}

	if (ax->ax_wblen == 0)
		return 0;

#if defined(AGENTX_DEBUG) && defined(AGENTX_DEBUG_VERBOSE)
	{
		size_t i;
		char chars[4];
		int print = 1;

		fprintf(stderr, "sending packet:\n");
		for (i = 0; i < ax->ax_wblen; i++) {
			fprintf(stderr, "%02hhx ", ax->ax_wbuf[i]);
			chars[i % 4] = ax->ax_wbuf[i];
			if (!isprint(ax->ax_wbuf[i]))
				print = 0;
			if (i % 4 == 3) {
				if (print)
					fprintf(stderr, "%.4s", chars);
				fprintf(stderr, "\n");
				print = 1;
			}
		}
	}
#endif

	if ((nwrite = send(ax->ax_fd, ax->ax_wbuf, ax->ax_wblen,
	    MSG_NOSIGNAL | MSG_DONTWAIT)) == -1)
		return -1;

	memmove(ax->ax_wbuf, ax->ax_wbuf + nwrite, ax->ax_wblen - nwrite);
	ax->ax_wblen -= nwrite;
	ax->ax_wbtlen = ax->ax_wblen;

	return ax->ax_wblen;
}

uint32_t
agentx_open(struct agentx *ax, uint8_t timeout, struct agentx_oid *oid,
    struct agentx_ostring *descr)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_OPEN, 0, 0, 0, 0,
	    NULL) == -1)
		return 0;
	agentx_pdu_need(ax, 4);
	ax->ax_wbuf[ax->ax_wbtlen++] = timeout;
	memset(&(ax->ax_wbuf[ax->ax_wbtlen]), 0, 3);
	ax->ax_wbtlen += 3;
	if (agentx_pdu_add_oid(ax, oid, 0) == -1)
		return 0;
	if (agentx_pdu_add_str(ax, descr) == -1)
		return 0;

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_close(struct agentx *ax, uint32_t sessionid,
    enum agentx_close_reason reason)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_CLOSE, 0, sessionid, 0, 0,
	    NULL) == -1)
		return 0;

	if (agentx_pdu_need(ax, 4) == -1)
		return 0;
	ax->ax_wbuf[ax->ax_wbtlen++] = (uint8_t)reason;
	memset(&(ax->ax_wbuf[ax->ax_wbtlen]), 0, 3);
	ax->ax_wbtlen += 3;

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_indexallocate(struct agentx *ax, uint8_t flags, uint32_t sessionid,
    struct agentx_ostring *context, struct agentx_varbind *vblist, size_t nvb)
{
	if (flags & ~(AGENTX_PDU_FLAG_NEW_INDEX | AGENTX_PDU_FLAG_ANY_INDEX)) {
		errno = EINVAL;
		return 0;
	}

	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_INDEXALLOCATE, flags,
	    sessionid, 0, 0, context) == -1)
		return 0;

	if (agentx_pdu_add_varbindlist(ax, vblist, nvb) == -1)
		return 0;

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_indexdeallocate(struct agentx *ax, uint32_t sessionid,
    struct agentx_ostring *context, struct agentx_varbind *vblist, size_t nvb)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_INDEXDEALLOCATE, 0,
	    sessionid, 0, 0, context) == -1)
		return 0;

	if (agentx_pdu_add_varbindlist(ax, vblist, nvb) == -1)
		return 0;

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_addagentcaps(struct agentx *ax, uint32_t sessionid,
    struct agentx_ostring *context, struct agentx_oid *id,
    struct agentx_ostring *descr)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_ADDAGENTCAPS, 0,
	    sessionid, 0, 0, context) == -1)
		return 0;
	if (agentx_pdu_add_oid(ax, id, 0) == -1)
		return 0;
	if (agentx_pdu_add_str(ax, descr) == -1)
		return 0;

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_removeagentcaps(struct agentx *ax, uint32_t sessionid,
    struct agentx_ostring *context, struct agentx_oid *id)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_REMOVEAGENTCAPS, 0,
	    sessionid, 0, 0, context) == -1)
		return 0;
	if (agentx_pdu_add_oid(ax, id, 0) == -1)
		return 0;

	return agentx_pdu_queue(ax);

}

uint32_t
agentx_register(struct agentx *ax, uint8_t flags, uint32_t sessionid,
    struct agentx_ostring *context, uint8_t timeout, uint8_t priority,
    uint8_t range_subid, struct agentx_oid *subtree, uint32_t upperbound)
{
	if (flags & ~(AGENTX_PDU_FLAG_INSTANCE_REGISTRATION)) {
		errno = EINVAL;
		return 0;
	}

	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_REGISTER, flags,
	    sessionid, 0, 0, context) == -1)
		return 0;

	if (agentx_pdu_need(ax, 4) == -1)
		return 0;
	ax->ax_wbuf[ax->ax_wbtlen++] = timeout;
	ax->ax_wbuf[ax->ax_wbtlen++] = priority;
	ax->ax_wbuf[ax->ax_wbtlen++] = range_subid;
	ax->ax_wbuf[ax->ax_wbtlen++] = 0;
	if (agentx_pdu_add_oid(ax, subtree, 0) == -1)
		return 0;
	if (range_subid != 0) {
		if (agentx_pdu_add_uint32(ax, upperbound) == -1)
			return 0;
	}

	return agentx_pdu_queue(ax);
}

uint32_t
agentx_unregister(struct agentx *ax, uint32_t sessionid,
    struct agentx_ostring *context, uint8_t priority, uint8_t range_subid,
    struct agentx_oid *subtree, uint32_t upperbound)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_UNREGISTER, 0,
	    sessionid, 0, 0, context) == -1)
		return 0;

	if (agentx_pdu_need(ax, 4) == -1)
		return 0;
	ax->ax_wbuf[ax->ax_wbtlen++] = 0;
	ax->ax_wbuf[ax->ax_wbtlen++] = priority;
	ax->ax_wbuf[ax->ax_wbtlen++] = range_subid;
	ax->ax_wbuf[ax->ax_wbtlen++] = 0;
	if (agentx_pdu_add_oid(ax, subtree, 0) == -1)
		return 0;
	if (range_subid != 0) {
		if (agentx_pdu_add_uint32(ax, upperbound) == -1)
			return 0;
	}

	return agentx_pdu_queue(ax);
}

int
agentx_response(struct agentx *ax, uint32_t sessionid, uint32_t transactionid,
    uint32_t packetid, struct agentx_ostring *context, uint32_t sysuptime,
    uint16_t error, uint16_t index, struct agentx_varbind *vblist, size_t nvb)
{
	if (agentx_pdu_header(ax, AGENTX_PDU_TYPE_RESPONSE, 0, sessionid,
	    transactionid, packetid, context) == -1)
		return -1;

	if (agentx_pdu_add_uint32(ax, sysuptime) == -1 ||
	    agentx_pdu_add_uint16(ax, error) == -1 ||
	    agentx_pdu_add_uint16(ax, index) == -1)
		return -1;

	if (agentx_pdu_add_varbindlist(ax, vblist, nvb) == -1)
		return -1;
	if (agentx_pdu_queue(ax) == 0)
		return -1;
	return 0;
}

void
agentx_pdu_free(struct agentx_pdu *pdu)
{
	size_t i;
	struct agentx_pdu_response *response;

	if (pdu->ap_header.aph_flags & AGENTX_PDU_FLAG_NON_DEFAULT_CONTEXT)
		free(pdu->ap_context.aos_string);

	switch (pdu->ap_header.aph_type) {
	case AGENTX_PDU_TYPE_GET:
	case AGENTX_PDU_TYPE_GETNEXT:
	case AGENTX_PDU_TYPE_GETBULK:
		free(pdu->ap_payload.ap_srl.ap_sr);
		break;
	case AGENTX_PDU_TYPE_RESPONSE:
		response = &(pdu->ap_payload.ap_response);
		for (i = 0; i < response->ap_nvarbind; i++)
			agentx_varbind_free(&(response->ap_varbindlist[i]));
		free(response->ap_varbindlist);
		break;
	default:
		free(pdu->ap_payload.ap_raw);
		break;
	}
	free(pdu);
}

void
agentx_varbind_free(struct agentx_varbind *varbind)
{
	switch (varbind->avb_type) {
	case AGENTX_DATA_TYPE_OCTETSTRING:
	case AGENTX_DATA_TYPE_IPADDRESS:
	case AGENTX_DATA_TYPE_OPAQUE:
		free(varbind->avb_data.avb_ostring.aos_string);
		break;
	default:
		break;
	}
}

const char *
agentx_error2string(enum agentx_pdu_error error)
{
	static char buffer[64];
	switch (error) {
	case AGENTX_PDU_ERROR_NOERROR:
		return "No error";
	case AGENTX_PDU_ERROR_GENERR:
		return "Generic error";
	case AGENTX_PDU_ERROR_NOACCESS:
		return "No access";
	case AGENTX_PDU_ERROR_WRONGTYPE:
		return "Wrong type";
	case AGENTX_PDU_ERROR_WRONGLENGTH:
		return "Wrong length";
	case AGENTX_PDU_ERROR_WRONGENCODING:
		return "Wrong encoding";
	case AGENTX_PDU_ERROR_WRONGVALUE:
		return "Wrong value";
	case AGENTX_PDU_ERROR_NOCREATION:
		return "No creation";
	case AGENTX_PDU_ERROR_INCONSISTENTVALUE:
		return "Inconsistent value";
	case AGENTX_PDU_ERROR_RESOURCEUNAVAILABLE:
		return "Resource unavailable";
	case AGENTX_PDU_ERROR_COMMITFAILED:
		return "Commit failed";
	case AGENTX_PDU_ERROR_UNDOFAILED:
		return "Undo failed";
	case AGENTX_PDU_ERROR_NOTWRITABLE:
		return "Not writable";
	case AGENTX_PDU_ERROR_INCONSISTENTNAME:
		return "Inconsistent name";
	case AGENTX_PDU_ERROR_OPENFAILED:
		return "Open Failed";
	case AGENTX_PDU_ERROR_NOTOPEN:
		return "Not open";
	case AGENTX_PDU_ERROR_INDEXWRONGTYPE:
		return "Index wrong type";
	case AGENTX_PDU_ERROR_INDEXALREADYALLOCATED:
		return "Index already allocated";
	case AGENTX_PDU_ERROR_INDEXNONEAVAILABLE:
		return "Index none available";
	case AGENTX_PDU_ERROR_INDEXNOTALLOCATED:
		return "Index not allocated";
	case AGENTX_PDU_ERROR_UNSUPPORTEDCONETXT:
		return "Unsupported context";
	case AGENTX_PDU_ERROR_DUPLICATEREGISTRATION:
		return "Duplicate registration";
	case AGENTX_PDU_ERROR_UNKNOWNREGISTRATION:
		return "Unkown registration";
	case AGENTX_PDU_ERROR_UNKNOWNAGENTCAPS:
		return "Unknown agent capabilities";
	case AGENTX_PDU_ERROR_PARSEERROR:
		return "Parse error";
	case AGENTX_PDU_ERROR_REQUESTDENIED:
		return "Request denied";
	case AGENTX_PDU_ERROR_PROCESSINGERROR:
		return "Processing error";
	}
	snprintf(buffer, sizeof(buffer), "Unknown error: %d", error);
	return buffer;
}

const char *
agentx_pdutype2string(enum agentx_pdu_type type)
{
	static char buffer[64];
	switch(type) {
	case AGENTX_PDU_TYPE_OPEN:
		return "agentx-Open-PDU";
	case AGENTX_PDU_TYPE_CLOSE:
		return "agentx-Close-PDU";
	case AGENTX_PDU_TYPE_REGISTER:
		return "agentx-Register-PDU";
	case AGENTX_PDU_TYPE_UNREGISTER:
		return "agentx-Unregister-PDU";
	case AGENTX_PDU_TYPE_GET:
		return "agentx-Get-PDU";
	case AGENTX_PDU_TYPE_GETNEXT:
		return "agentx-GetNext-PDU";
	case AGENTX_PDU_TYPE_GETBULK:
		return "agentx-GetBulk-PDU";
	case AGENTX_PDU_TYPE_TESTSET:
		return "agentx-TestSet-PDU";
	case AGENTX_PDU_TYPE_COMMITSET:
		return "agentx-CommitSet-PDU";
	case AGENTX_PDU_TYPE_UNDOSET:
		return "agentx-UndoSet-PDU";
	case AGENTX_PDU_TYPE_CLEANUPSET:
		return "agentx-CleanupSet-PDU";
	case AGENTX_PDU_TYPE_NOTIFY:
		return "agentx-Notify-PDU";
	case AGENTX_PDU_TYPE_PING:
		return "agentx-Ping-PDU";
	case AGENTX_PDU_TYPE_INDEXALLOCATE:
		return "agentx-IndexAllocate-PDU";
	case AGENTX_PDU_TYPE_INDEXDEALLOCATE:
		return "agentx-IndexDeallocate-PDU";
	case AGENTX_PDU_TYPE_ADDAGENTCAPS:
		return "agentx-AddAgentCaps-PDU";
	case AGENTX_PDU_TYPE_REMOVEAGENTCAPS:
		return "agentx-RemoveAgentCaps-PDU";
	case AGENTX_PDU_TYPE_RESPONSE:
		return "agentx-Response-PDU";
	}
	snprintf(buffer, sizeof(buffer), "Unknown type: %d", type);
	return buffer;
}

const char *
agentx_closereason2string(enum agentx_close_reason reason)
{
	static char buffer[64];

	switch (reason) {
	case AGENTX_CLOSE_OTHER:
		return "Undefined reason";
	case AGENTX_CLOSEN_PARSEERROR:
		return "Too many AgentX parse errors from peer";
	case AGENTX_CLOSE_PROTOCOLERROR:
		return "Too many AgentX protocol errors from peer";
	case AGENTX_CLOSE_TIMEOUTS:
		return "Too many timeouts waiting for peer";
	case AGENTX_CLOSE_SHUTDOWN:
		return "shutting down";
	case AGENTX_CLOSE_BYMANAGER:
		return "Manager shuts down";
	}
	snprintf(buffer, sizeof(buffer), "Unknown reason: %d", reason);
	return buffer;
}

const char *
agentx_oid2string(struct agentx_oid *oid)
{
	return agentx_oidrange2string(oid, 0, 0);
}

const char *
agentx_oidrange2string(struct agentx_oid *oid, uint8_t range_subid,
    uint32_t upperbound)
{
	static char buf[1024];
	char *p;
	size_t i, rest;
	int ret;

	rest = sizeof(buf);
	p = buf;
	for (i = 0; i < oid->aoi_idlen; i++) {
		if (range_subid != 0 && range_subid - 1 == (uint8_t)i)
			ret = snprintf(p, rest, ".[%u-%u]", oid->aoi_id[i],
			    upperbound);
		else
			ret = snprintf(p, rest, ".%u", oid->aoi_id[i]);
		if ((size_t) ret >= rest) {
			snprintf(buf, sizeof(buf), "Couldn't parse oid");
			return buf;
		}
		p += ret;
		rest -= (size_t) ret;
	}
	return buf;
}

const char *
agentx_varbind2string(struct agentx_varbind *vb)
{
	static char buf[1024];
	char tmpbuf[1024];
	size_t i, bufleft;
	int ishex = 0;
	char *p;
	int ret;

	switch (vb->avb_type) {
	case AGENTX_DATA_TYPE_INTEGER:
		snprintf(buf, sizeof(buf), "%s: (int)%u",
		    agentx_oid2string(&(vb->avb_oid)), vb->avb_data.avb_uint32);
		break;
	case AGENTX_DATA_TYPE_OCTETSTRING:
		for (i = 0;
		    i < vb->avb_data.avb_ostring.aos_slen && !ishex; i++) {
			if (!isprint(vb->avb_data.avb_ostring.aos_string[i]))
				ishex = 1;
		}
		if (ishex) {
			p = tmpbuf;
			bufleft = sizeof(tmpbuf);
			for (i = 0;
			    i < vb->avb_data.avb_ostring.aos_slen; i++) {
				ret = snprintf(p, bufleft, " %02hhX",
				    vb->avb_data.avb_ostring.aos_string[i]);
				if (ret >= (int) bufleft) {
					p = strrchr(p, ' ');
					strlcpy(p, "...", 4);
					break;
				}
				p += 3;
				bufleft -= 3;
			}
			ret = snprintf(buf, sizeof(buf), "%s: (hex-string)%s",
			    agentx_oid2string(&(vb->avb_oid)), tmpbuf);
			if (ret >= (int) sizeof(buf)) {
				p  = strrchr(buf, ' ');
				strlcpy(p, "...", 4);
			}
		} else {
			ret = snprintf(buf, sizeof(buf), "%s: (string)",
			    agentx_oid2string(&(vb->avb_oid)));
			if (ret >= (int) sizeof(buf)) {
				snprintf(buf, sizeof(buf), "<too large OID>: "
				    "(string)<too large string>");
				break;
			}
			p = buf + ret;
			bufleft = (int) sizeof(buf) - ret;
			if (snprintf(p, bufleft, "%.*s",
			    vb->avb_data.avb_ostring.aos_slen,
			    vb->avb_data.avb_ostring.aos_string) >=
			    (int) bufleft) {
				p = buf + sizeof(buf) - 4;
				strlcpy(p, "...", 4);
			}
		}
		break;
	case AGENTX_DATA_TYPE_NULL:
		snprintf(buf, sizeof(buf), "%s: <null>",
		    agentx_oid2string(&(vb->avb_oid)));
		break;
	case AGENTX_DATA_TYPE_OID:
		strlcpy(tmpbuf,
		    agentx_oid2string(&(vb->avb_data.avb_oid)), sizeof(tmpbuf));
		snprintf(buf, sizeof(buf), "%s: (oid)%s",
		    agentx_oid2string(&(vb->avb_oid)), tmpbuf);
		break;
	case AGENTX_DATA_TYPE_IPADDRESS:
		if (vb->avb_data.avb_ostring.aos_slen != 4) {
			snprintf(buf, sizeof(buf), "%s: (ipaddress)<invalid>",
			    agentx_oid2string(&(vb->avb_oid)));
			break;
		}
		if (inet_ntop(PF_INET, vb->avb_data.avb_ostring.aos_string,
		    tmpbuf, sizeof(tmpbuf)) == NULL) {
			snprintf(buf, sizeof(buf), "%s: (ipaddress)"
			    "<unparseable>: %s",
			    agentx_oid2string(&(vb->avb_oid)),
			    strerror(errno));
			break;
		}
		snprintf(buf, sizeof(buf), "%s: (ipaddress)%s",
		    agentx_oid2string(&(vb->avb_oid)), tmpbuf);
		break;
	case AGENTX_DATA_TYPE_COUNTER32:
		snprintf(buf, sizeof(buf), "%s: (counter32)%u",
		    agentx_oid2string(&(vb->avb_oid)), vb->avb_data.avb_uint32);
		break;
	case AGENTX_DATA_TYPE_GAUGE32:
		snprintf(buf, sizeof(buf), "%s: (gauge32)%u",
		    agentx_oid2string(&(vb->avb_oid)), vb->avb_data.avb_uint32);
		break;
	case AGENTX_DATA_TYPE_TIMETICKS:
		snprintf(buf, sizeof(buf), "%s: (timeticks)%u",
		    agentx_oid2string(&(vb->avb_oid)), vb->avb_data.avb_uint32);
		break;
	case AGENTX_DATA_TYPE_OPAQUE:
		p = tmpbuf;
		bufleft = sizeof(tmpbuf);
		for (i = 0;
		    i < vb->avb_data.avb_ostring.aos_slen; i++) {
			ret = snprintf(p, bufleft, " %02hhX",
			    vb->avb_data.avb_ostring.aos_string[i]);
			if (ret >= (int) bufleft) {
				p = strrchr(p, ' ');
				strlcpy(p, "...", 4);
				break;
			}
			p += 3;
			bufleft -= 3;
		}
		ret = snprintf(buf, sizeof(buf), "%s: (opaque)%s",
		    agentx_oid2string(&(vb->avb_oid)), tmpbuf);
		if (ret >= (int) sizeof(buf)) {
			p  = strrchr(buf, ' ');
			strlcpy(p, "...", 4);
		}
		break;
	case AGENTX_DATA_TYPE_COUNTER64:
		snprintf(buf, sizeof(buf), "%s: (counter64)%"PRIu64,
		    agentx_oid2string(&(vb->avb_oid)), vb->avb_data.avb_uint64);
		break;
	case AGENTX_DATA_TYPE_NOSUCHOBJECT:
		snprintf(buf, sizeof(buf), "%s: <noSuchObject>",
		    agentx_oid2string(&(vb->avb_oid)));
		break;
	case AGENTX_DATA_TYPE_NOSUCHINSTANCE:
		snprintf(buf, sizeof(buf), "%s: <noSuchInstance>",
		    agentx_oid2string(&(vb->avb_oid)));
		break;
	case AGENTX_DATA_TYPE_ENDOFMIBVIEW:
		snprintf(buf, sizeof(buf), "%s: <endOfMibView>",
		    agentx_oid2string(&(vb->avb_oid)));
		break;
	}
	return buf;
}

int
agentx_oid_cmp(struct agentx_oid *o1, struct agentx_oid *o2)
{
	size_t i, min;

	min = o1->aoi_idlen < o2->aoi_idlen ? o1->aoi_idlen : o2->aoi_idlen;
	for (i = 0; i < min; i++) {
		if (o1->aoi_id[i] < o2->aoi_id[i])
			return -1;
		if (o1->aoi_id[i] > o2->aoi_id[i])
			return 1;
	}
	/* o1 is parent of o2 */
	if (o1->aoi_idlen < o2->aoi_idlen)
		return -2;
	/* o1 is child of o2 */
	if (o1->aoi_idlen > o2->aoi_idlen)
		return 2;
	return 0;
}

int
agentx_oid_add(struct agentx_oid *oid, uint32_t value)
{
	if (oid->aoi_idlen == AGENTX_OID_MAX_LEN)
		return -1;
	oid->aoi_id[oid->aoi_idlen++] = value;
	return 0;
}

static uint32_t
agentx_pdu_queue(struct agentx *ax)
{
	struct agentx_pdu_header header;
	uint32_t packetid, plength;
	size_t wbtlen = ax->ax_wbtlen;

	header.aph_flags = ax->ax_byteorder == AGENTX_BYTE_ORDER_BE ?
	    AGENTX_PDU_FLAG_NETWORK_BYTE_ORDER : 0;
	packetid = agentx_pdutoh32(&header, &(ax->ax_wbuf[ax->ax_wblen + 12]));
	plength = (ax->ax_wbtlen - ax->ax_wblen) - AGENTX_PDU_HEADER;
	ax->ax_wbtlen = ax->ax_wblen + 16;
	(void)agentx_pdu_add_uint32(ax, plength);

	ax->ax_wblen = ax->ax_wbtlen = wbtlen;

	return packetid;
}

static int
agentx_pdu_header(struct agentx *ax, enum agentx_pdu_type type, uint8_t flags,
    uint32_t sessionid, uint32_t transactionid, uint32_t packetid,
    struct agentx_ostring *context)
{
	if (ax->ax_wblen != ax->ax_wbtlen) {
		errno = EALREADY;
		return -1;
	}

	ax->ax_wbtlen = ax->ax_wblen;
	if (agentx_pdu_need(ax, 4) == -1)
		return -1;
	ax->ax_wbuf[ax->ax_wbtlen++] = 1;
	ax->ax_wbuf[ax->ax_wbtlen++] = (uint8_t) type;
	if (context != NULL)
		flags |= AGENTX_PDU_FLAG_NON_DEFAULT_CONTEXT;
	if (ax->ax_byteorder == AGENTX_BYTE_ORDER_BE)
		flags |= AGENTX_PDU_FLAG_NETWORK_BYTE_ORDER;
	ax->ax_wbuf[ax->ax_wbtlen++] = flags;
	ax->ax_wbuf[ax->ax_wbtlen++] = 0;
	if (packetid == 0)
		packetid = agentx_packetid(ax);
	if (agentx_pdu_add_uint32(ax, sessionid) == -1 ||
	    agentx_pdu_add_uint32(ax, transactionid) == -1 ||
	    agentx_pdu_add_uint32(ax, packetid) == -1 ||
	    agentx_pdu_need(ax, 4) == -1)
		return -1;
	ax->ax_wbtlen += 4;
	if (context != NULL) {
		if (agentx_pdu_add_str(ax, context) == -1)
			return -1;
	}

	return 0;
}

static uint32_t
agentx_packetid(struct agentx *ax)
{
	uint32_t packetid, *packetids;
	size_t npackets = 0, i;
	int found;

	if (ax->ax_packetids != NULL) {
		for (npackets = 0; ax->ax_packetids[npackets] != 0; npackets++)
			continue;
	}
	if (ax->ax_packetidsize == 0 || npackets == ax->ax_packetidsize - 1) {
		packetids = recallocarray(ax->ax_packetids, ax->ax_packetidsize,
		    ax->ax_packetidsize + 25, sizeof(*packetids));
		if (packetids == NULL)
			return 0;
		ax->ax_packetidsize += 25;
		ax->ax_packetids = packetids;
	}
	do {
		found = 0;
		packetid = arc4random();
		for (i = 0; ax->ax_packetids[i] != 0; i++) {
			if (ax->ax_packetids[i] == packetid) {
				found = 1;
				break;
			}
		}
	} while (packetid == 0 || found);
	ax->ax_packetids[npackets] = packetid;

	return packetid;
}

static int
agentx_pdu_add_uint16(struct agentx *ax, uint16_t value)
{
	if (agentx_pdu_need(ax, sizeof(value)) == -1)
		return -1;

	if (ax->ax_byteorder == AGENTX_BYTE_ORDER_BE)
		value = htobe16(value);
	else
		value = htole16(value);
	memcpy(ax->ax_wbuf + ax->ax_wbtlen, &value, sizeof(value));
        ax->ax_wbtlen += sizeof(value);
        return 0;
}

static int
agentx_pdu_add_uint32(struct agentx *ax, uint32_t value)
{
	if (agentx_pdu_need(ax, sizeof(value)) == -1)
		return -1;

	if (ax->ax_byteorder == AGENTX_BYTE_ORDER_BE)
		value = htobe32(value);
	else
		value = htole32(value);
	memcpy(ax->ax_wbuf + ax->ax_wbtlen, &value, sizeof(value));
        ax->ax_wbtlen += sizeof(value);
        return 0;
}

static int
agentx_pdu_add_uint64(struct agentx *ax, uint64_t value)
{
	if (agentx_pdu_need(ax, sizeof(value)) == -1)
		return -1;

	if (ax->ax_byteorder == AGENTX_BYTE_ORDER_BE)
		value = htobe64(value);
	else
		value = htole64(value);
	memcpy(ax->ax_wbuf + ax->ax_wbtlen, &value, sizeof(value));
        ax->ax_wbtlen += sizeof(value);
        return 0;
}


static int
agentx_pdu_add_oid(struct agentx *ax, struct agentx_oid *oid, int include)
{
	static struct agentx_oid nulloid = {0};
	uint8_t prefix = 0, n_subid, i = 0;

	n_subid = oid->aoi_idlen;

	if (oid == NULL)
		oid = &nulloid;

	if (oid->aoi_idlen > 4 &&
	    oid->aoi_id[0] == 1 && oid->aoi_id[1] == 3 &&
	    oid->aoi_id[2] == 6 && oid->aoi_id[3] == 1 &&
	    oid->aoi_id[4] <= UINT8_MAX) {
		prefix = oid->aoi_id[4];
		i = 5;
	}

	if (agentx_pdu_need(ax, 4) == -1)
		return -1;
	ax->ax_wbuf[ax->ax_wbtlen++] = n_subid - i;
	ax->ax_wbuf[ax->ax_wbtlen++] = prefix;
	ax->ax_wbuf[ax->ax_wbtlen++] = !!include;
	ax->ax_wbuf[ax->ax_wbtlen++] = 0;

	for (; i < n_subid; i++) {
		if (agentx_pdu_add_uint32(ax, oid->aoi_id[i]) == -1)
			return -1;
	}

	return 0;
}

static int
agentx_pdu_add_str(struct agentx *ax, struct agentx_ostring *str)
{
	size_t length, zeroes;

	if (agentx_pdu_add_uint32(ax, str->aos_slen) == -1)
		return -1;

	if ((zeroes = (4 - (str->aos_slen % 4))) == 4)
		zeroes = 0;
	length = str->aos_slen + zeroes;
	if (agentx_pdu_need(ax, length) == -1)
		return -1;

	memcpy(&(ax->ax_wbuf[ax->ax_wbtlen]), str->aos_string, str->aos_slen);
	ax->ax_wbtlen += str->aos_slen;
	memset(&(ax->ax_wbuf[ax->ax_wbtlen]), 0, zeroes);
	ax->ax_wbtlen += zeroes;
	return 0;
}

static int
agentx_pdu_add_varbindlist(struct agentx *ax,
    struct agentx_varbind *vblist, size_t nvb)
{
	size_t i;
	uint16_t temp;

	for (i = 0; i < nvb; i++) {
		temp = (uint16_t) vblist[i].avb_type;
		if (agentx_pdu_add_uint16(ax, temp) == -1 ||
		    agentx_pdu_need(ax, 2))
			return -1;
		memset(&(ax->ax_wbuf[ax->ax_wbtlen]), 0, 2);
		ax->ax_wbtlen += 2;
		if (agentx_pdu_add_oid(ax, &(vblist[i].avb_oid), 0) == -1)
			return -1;
		switch (vblist[i].avb_type) {
		case AGENTX_DATA_TYPE_INTEGER:
		case AGENTX_DATA_TYPE_COUNTER32:
		case AGENTX_DATA_TYPE_GAUGE32:
		case AGENTX_DATA_TYPE_TIMETICKS:
			if (agentx_pdu_add_uint32(ax,
			    vblist[i].avb_data.avb_uint32) == -1)
				return -1;
			break;
		case AGENTX_DATA_TYPE_COUNTER64:
			if (agentx_pdu_add_uint64(ax,
			    vblist[i].avb_data.avb_uint64) == -1)
				return -1;
			break;
		case AGENTX_DATA_TYPE_OCTETSTRING:
		case AGENTX_DATA_TYPE_IPADDRESS:
		case AGENTX_DATA_TYPE_OPAQUE:
			if (agentx_pdu_add_str(ax,
			    &(vblist[i].avb_data.avb_ostring)) == -1)
				return -1;
			break;
		case AGENTX_DATA_TYPE_OID:
			if (agentx_pdu_add_oid(ax,
			    &(vblist[i].avb_data.avb_oid), 1) == -1)
				return -1;
			break;
		case AGENTX_DATA_TYPE_NULL:
		case AGENTX_DATA_TYPE_NOSUCHOBJECT:
		case AGENTX_DATA_TYPE_NOSUCHINSTANCE:
		case AGENTX_DATA_TYPE_ENDOFMIBVIEW:
			break;
		default:
			errno = EINVAL;
			return -1;
		}
	}
	return 0;
}

static uint16_t
agentx_pdutoh16(struct agentx_pdu_header *header, uint8_t *buf)
{
	uint16_t value;

	memcpy(&value, buf, sizeof(value));
	if (header->aph_flags & AGENTX_PDU_FLAG_NETWORK_BYTE_ORDER)
		return be16toh(value);
	return le16toh(value);
}

static uint32_t
agentx_pdutoh32(struct agentx_pdu_header *header, uint8_t *buf)
{
	uint32_t value;

	memcpy(&value, buf, sizeof(value));
	if (header->aph_flags & AGENTX_PDU_FLAG_NETWORK_BYTE_ORDER)
		return be32toh(value);
	return le32toh(value);
}

static uint64_t
agentx_pdutoh64(struct agentx_pdu_header *header, uint8_t *buf)
{
	uint64_t value;

	memcpy(&value, buf, sizeof(value));
	if (header->aph_flags & AGENTX_PDU_FLAG_NETWORK_BYTE_ORDER)
		return be64toh(value);
	return le64toh(value);
}

static ssize_t
agentx_pdutooid(struct agentx_pdu_header *header, struct agentx_oid *oid,
    uint8_t *buf, size_t rawlen)
{
	size_t i = 0;
	ssize_t nread;

	if (rawlen < 4)
		goto fail;
	rawlen -= 4;
	nread = 4;
	oid->aoi_idlen = *buf++;
	if (rawlen < (oid->aoi_idlen * 4))
		goto fail;
	nread += oid->aoi_idlen * 4;
	if (*buf != 0) {
		oid->aoi_id[0] = 1;
		oid->aoi_id[1] = 3;
		oid->aoi_id[2] = 6;
		oid->aoi_id[3] = 1;
		oid->aoi_id[4] = *buf;
		oid->aoi_idlen += 5;
		i = 5;
	}
	buf++;
	oid->aoi_include = *buf;
	for (buf += 2; i < oid->aoi_idlen; i++, buf += 4)
		oid->aoi_id[i] = agentx_pdutoh32(header, buf);

	return nread;

fail:
	errno = EPROTO;
	return -1;
}

static ssize_t
agentx_pdutoostring(struct agentx_pdu_header *header,
    struct agentx_ostring *ostring, uint8_t *buf, size_t rawlen)
{
	ssize_t nread;

	if (rawlen < 4)
		goto fail;

	ostring->aos_slen = agentx_pdutoh32(header, buf);
	rawlen -= 4;
	buf += 4;
	if (ostring->aos_slen > rawlen)
		goto fail;
	if ((ostring->aos_string = malloc(ostring->aos_slen + 1)) == NULL)
		return -1;
	memcpy(ostring->aos_string, buf, ostring->aos_slen);
	ostring->aos_string[ostring->aos_slen] = '\0';

	nread = 4 + ostring->aos_slen;
	if (ostring->aos_slen % 4 != 0)
		nread += 4 - (ostring->aos_slen % 4);

	return nread;

fail:
	errno = EPROTO;
	return -1;
}

static ssize_t
agentx_pdutovarbind(struct agentx_pdu_header *header,
    struct agentx_varbind *varbind, uint8_t *buf, size_t rawlen)
{
	ssize_t nread, rread = 4;

	if (rawlen == 0)
		return 0;

	if (rawlen < 8)
		goto fail;
	varbind->avb_type = agentx_pdutoh16(header, buf);

	buf += 4;
	rawlen -= 4;
	nread = agentx_pdutooid(header, &(varbind->avb_oid), buf, rawlen);
	if (nread == -1)
		return -1;
	rread += nread;
	buf += nread;
	rawlen -= nread;

	switch(varbind->avb_type) {
	case AGENTX_DATA_TYPE_INTEGER:
	case AGENTX_DATA_TYPE_COUNTER32:
	case AGENTX_DATA_TYPE_GAUGE32:
	case AGENTX_DATA_TYPE_TIMETICKS:
		if (rawlen < 4)
			goto fail;
		varbind->avb_data.avb_uint32 = agentx_pdutoh32(header, buf);
		return rread + 4;
	case AGENTX_DATA_TYPE_COUNTER64:
		if (rawlen < 8)
			goto fail;
		varbind->avb_data.avb_uint64 = agentx_pdutoh64(header, buf);
		return rread + 8;
	case AGENTX_DATA_TYPE_OCTETSTRING:
	case AGENTX_DATA_TYPE_IPADDRESS:
	case AGENTX_DATA_TYPE_OPAQUE:
		nread = agentx_pdutoostring(header,
		    &(varbind->avb_data.avb_ostring), buf, rawlen);
		if (nread == -1)
			return -1;
		return nread + rread;
	case AGENTX_DATA_TYPE_OID:
		nread = agentx_pdutooid(header, &(varbind->avb_data.avb_oid),
		    buf, rawlen);
		if (nread == -1)
			return -1;
		return nread + rread;
	case AGENTX_DATA_TYPE_NULL:
	case AGENTX_DATA_TYPE_NOSUCHOBJECT:
	case AGENTX_DATA_TYPE_NOSUCHINSTANCE:
	case AGENTX_DATA_TYPE_ENDOFMIBVIEW:
		return rread;
	}

fail:
	errno = EPROTO;
	return -1;
}
