/*
 * Copyright (c) 2004-2009 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 * 
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#define __KPI__
//#include <sys/kpi_interface.h>

#include <sys/param.h>
#include <sys/mbuf.h>
#include <sys/socket.h>
#include <kern/debug.h>
#include <libkern/OSAtomic.h>
#include <kern/kalloc.h>
#include <string.h>
#include <netinet/in.h>

#include "net/net_str_id.h"

static const mbuf_flags_t mbuf_flags_mask = MBUF_EXT | MBUF_PKTHDR | MBUF_EOR |
				MBUF_BCAST | MBUF_MCAST | MBUF_FRAG | MBUF_FIRSTFRAG |
				MBUF_LASTFRAG | MBUF_PROMISC;

void* mbuf_data(mbuf_t mbuf)
{
	return mbuf->m_data;
}

void* mbuf_datastart(mbuf_t mbuf)
{
	if (mbuf->m_flags & M_EXT)
		return mbuf->m_ext.ext_buf;
	if (mbuf->m_flags & M_PKTHDR)
		return mbuf->m_pktdat;
	return mbuf->m_dat;
}

errno_t mbuf_setdata(mbuf_t mbuf, void* data, size_t len)
{
	size_t	start = (size_t)((char*)mbuf_datastart(mbuf));
	size_t	maxlen = mbuf_maxlen(mbuf);
	
	if ((size_t)data < start || ((size_t)data) + len > start + maxlen)
		return EINVAL;
	mbuf->m_data = data;
	mbuf->m_len = len;
	
	return 0;
}

errno_t mbuf_align_32(mbuf_t mbuf, size_t len)
{
	if ((mbuf->m_flags & M_EXT) != 0 && m_mclhasreference(mbuf))
		return ENOTSUP;
	mbuf->m_data = mbuf_datastart(mbuf);
	mbuf->m_data += ((mbuf_trailingspace(mbuf) - len) &~ (sizeof(u_int32_t) - 1));
	
	return 0;
}

addr64_t mbuf_data_to_physical(void* ptr)
{
	return (addr64_t)(uintptr_t)mcl_to_paddr(ptr);
}

errno_t mbuf_get(mbuf_how_t how, mbuf_type_t type, mbuf_t *mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	*mbuf = m_get(how, type);
	
	return (*mbuf == NULL) ? ENOMEM : 0;
}

errno_t mbuf_gethdr(mbuf_how_t how, mbuf_type_t type, mbuf_t *mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	*mbuf = m_gethdr(how, type);
	
	return (*mbuf == NULL) ? ENOMEM : 0;
}

errno_t
mbuf_attachcluster(mbuf_how_t how, mbuf_type_t type, mbuf_t *mbuf,
    caddr_t extbuf, void (*extfree)(caddr_t , u_int, caddr_t),
    size_t extsize, caddr_t extarg)
{
	if (extbuf == NULL || extfree == NULL || extsize == 0)
		return (EINVAL);

	if ((*mbuf = m_clattach(mbuf != NULL ? *mbuf : NULL, type, extbuf,
	    extfree, extsize, extarg, how)) == NULL)
		return (ENOMEM);

	return (0);
}

errno_t
mbuf_alloccluster(mbuf_how_t how, size_t *size, caddr_t *addr)
{
	if (size == NULL || *size == 0 || addr == NULL)
		return (EINVAL);

	*addr = NULL;

	/* Jumbo cluster pool not available? */
	if (*size > NBPG && njcl == 0)
		return (ENOTSUP);

	if (*size <= MCLBYTES && (*addr = m_mclalloc(how)) != NULL)
		*size = MCLBYTES;
	else if (*size > MCLBYTES && *size <= NBPG &&
	    (*addr = m_bigalloc(how)) != NULL)
		*size = NBPG;
	else if (*size > NBPG && *size <= M16KCLBYTES &&
	    (*addr = m_16kalloc(how)) != NULL)
		*size = M16KCLBYTES;
	else
		*size = 0;

	if (*addr == NULL)
		return (ENOMEM);

	return (0);
}

void
mbuf_freecluster(caddr_t addr, size_t size)
{
	if (size != MCLBYTES && size != NBPG && size != M16KCLBYTES)
		panic("%s: invalid size (%ld) for cluster %p", __func__,
		    size, (void *)addr);

	if (size == MCLBYTES)
		m_mclfree(addr);
	else if (size == NBPG)
		m_bigfree(addr, NBPG, NULL);
	else if (njcl > 0)
		m_16kfree(addr, M16KCLBYTES, NULL);
	else
		panic("%s: freeing jumbo cluster to an empty pool", __func__);
}

errno_t
mbuf_getcluster(mbuf_how_t how, mbuf_type_t type, size_t size, mbuf_t* mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	errno_t	error = 0;
	int	created = 0;

	if (mbuf == NULL)
		return EINVAL;
	if (*mbuf == NULL) {
		*mbuf = m_get(how, type);
		if (*mbuf == NULL)
			return ENOMEM;
		created = 1;
	}
	/*
	 * At the time this code was written, m_{mclget,mbigget,m16kget}
	 * would always return the same value that was passed in to it.
	 */
	if (size == MCLBYTES) {
		*mbuf = m_mclget(*mbuf, how);
	} else if (size == NBPG) {
		*mbuf = m_mbigget(*mbuf, how);
	} else if (size == M16KCLBYTES) {
		if (njcl > 0) {
			*mbuf = m_m16kget(*mbuf, how);
		} else {
			/* Jumbo cluster pool not available? */
			error = ENOTSUP;
			goto out;
		}
	} else {
		error = EINVAL;
		goto out;
	}
	if (*mbuf == NULL || ((*mbuf)->m_flags & M_EXT) == 0)
		error = ENOMEM;
out:
	if (created && error != 0) {
		mbuf_free(*mbuf);
		*mbuf = NULL;
	}
	return error;	
}

errno_t mbuf_mclget(mbuf_how_t how, mbuf_type_t type, mbuf_t *mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	errno_t	error = 0;
	int		created = 0;
	if (mbuf == NULL) return EINVAL;
	if (*mbuf == NULL) {
		error = mbuf_get(how, type, mbuf);
		if (error)
			return error;
		created = 1;
	}
	
	/*
	 * At the time this code was written, m_mclget would always
	 * return the same value that was passed in to it.
	 */
	*mbuf = m_mclget(*mbuf, how);
	
	if (created && ((*mbuf)->m_flags & M_EXT) == 0) {
		mbuf_free(*mbuf);
		*mbuf = NULL;
	}
	if (*mbuf == NULL || ((*mbuf)->m_flags & M_EXT) == 0)
		error = ENOMEM;
	return error;	
}


errno_t mbuf_getpacket(mbuf_how_t how, mbuf_t *mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	errno_t	error = 0;
	
	*mbuf = m_getpacket_how(how);
	
	if (*mbuf == NULL) {
		if (how == MBUF_WAITOK)
			error = ENOMEM;
		else
			error = EWOULDBLOCK;
	}
	
	return error;
}

mbuf_t mbuf_free(mbuf_t mbuf)
{
	return m_free(mbuf);
}

void mbuf_freem(mbuf_t mbuf)
{
	m_freem(mbuf);
}

int	mbuf_freem_list(mbuf_t mbuf)
{
	return m_freem_list(mbuf);
}

size_t mbuf_leadingspace(const mbuf_t mbuf)
{
	return m_leadingspace(mbuf);
}

size_t mbuf_trailingspace(const mbuf_t mbuf)
{
	return m_trailingspace(mbuf);
}

/* Manipulation */
errno_t mbuf_copym(const mbuf_t src, size_t offset, size_t len,
				   mbuf_how_t how, mbuf_t *new_mbuf)
{
	/* Must set *mbuf to NULL in failure case */
	*new_mbuf = m_copym(src, offset, len, how);
	
	return (*new_mbuf == NULL) ? ENOMEM : 0;
}

errno_t	mbuf_dup(const mbuf_t src, mbuf_how_t how, mbuf_t *new_mbuf)
{
	/* Must set *new_mbuf to NULL in failure case */
	*new_mbuf = m_dup(src, how);
	
	return (*new_mbuf == NULL) ? ENOMEM : 0;
}

errno_t mbuf_prepend(mbuf_t *orig, size_t len, mbuf_how_t how)
{
	/* Must set *orig to NULL in failure case */
	*orig = m_prepend_2(*orig, len, how);
	
	return (*orig == NULL) ? ENOMEM : 0;
}

errno_t mbuf_split(mbuf_t src, size_t offset,
					mbuf_how_t how, mbuf_t *new_mbuf)
{
	/* Must set *new_mbuf to NULL in failure case */
	*new_mbuf = m_split(src, offset, how);
	
	return (*new_mbuf == NULL) ? ENOMEM : 0;
}

errno_t mbuf_pullup(mbuf_t *mbuf, size_t len)
{
	/* Must set *mbuf to NULL in failure case */
	*mbuf = m_pullup(*mbuf, len);
	
	return (*mbuf == NULL) ? ENOMEM : 0;
}

errno_t mbuf_pulldown(mbuf_t src, size_t *offset, size_t len, mbuf_t *location)
{
	/* Must set *location to NULL in failure case */
	int new_offset;
	*location = m_pulldown(src, *offset, len, &new_offset);
	*offset = new_offset;
	
	return (*location == NULL) ? ENOMEM : 0;
}

void mbuf_adj(mbuf_t mbuf, int len)
{
	m_adj(mbuf, len);
}

errno_t mbuf_adjustlen(mbuf_t m, int amount)
{
	/* Verify m_len will be valid after adding amount */
	if (amount > 0) {
		int		used = (size_t)mbuf_data(m) - (size_t)mbuf_datastart(m) +
					   m->m_len;
		
		if ((size_t)(amount + used) > mbuf_maxlen(m))
			return EINVAL;
	}
	else if (-amount > m->m_len) {
		return EINVAL;
	}
	
	m->m_len += amount;
	return 0;
}

mbuf_t
mbuf_concatenate(mbuf_t dst, mbuf_t src)
{
	if (dst == NULL)
		return (NULL);

	m_cat(dst, src);

	/* return dst as is in the current implementation */
	return (dst);
}
errno_t mbuf_copydata(const mbuf_t m0, size_t off, size_t len, void* out_data)
{
	/* Copied m_copydata, added error handling (don't just panic) */
	int count;
	mbuf_t	m = m0;

	while (off > 0) {
		if (m == 0)
			return EINVAL;
		if (off < (size_t)m->m_len)
			break;
		off -= m->m_len;
		m = m->m_next;
	}
	while (len > 0) {
		if (m == 0)
			return EINVAL;
		count = m->m_len - off > len ? len : m->m_len - off;
		bcopy(mtod(m, caddr_t) + off, out_data, count);
		len -= count;
		out_data = ((char*)out_data) + count;
		off = 0;
		m = m->m_next;
	}
	
	return 0;
}

int mbuf_mclhasreference(mbuf_t mbuf)
{
	if ((mbuf->m_flags & M_EXT))
		return m_mclhasreference(mbuf);
	else
		return 0;
}


/* mbuf header */
mbuf_t mbuf_next(const mbuf_t mbuf)
{
	return mbuf->m_next;
}

errno_t mbuf_setnext(mbuf_t mbuf, mbuf_t next)
{
	if (next && ((next)->m_nextpkt != NULL ||
		(next)->m_type == MT_FREE)) return EINVAL;
	mbuf->m_next = next;
	
	return 0;
}

mbuf_t mbuf_nextpkt(const mbuf_t mbuf)
{
	return mbuf->m_nextpkt;
}

void mbuf_setnextpkt(mbuf_t mbuf, mbuf_t nextpkt)
{
	mbuf->m_nextpkt = nextpkt;
}

size_t mbuf_len(const mbuf_t mbuf)
{
	return mbuf->m_len;
}

void mbuf_setlen(mbuf_t mbuf, size_t len)
{
	mbuf->m_len = len;
}

size_t mbuf_maxlen(const mbuf_t mbuf)
{
	if (mbuf->m_flags & M_EXT)
		return mbuf->m_ext.ext_size;
	return &mbuf->m_dat[MLEN] - ((char*)mbuf_datastart(mbuf));
}

mbuf_type_t mbuf_type(const mbuf_t mbuf)
{
	return mbuf->m_type;
}

errno_t mbuf_settype(mbuf_t mbuf, mbuf_type_t new_type)
{
	if (new_type == MBUF_TYPE_FREE) return EINVAL;
	
	m_mchtype(mbuf, new_type);
	
	return 0;
}

mbuf_flags_t mbuf_flags(const mbuf_t mbuf)
{
	return mbuf->m_flags & mbuf_flags_mask;
}

errno_t mbuf_setflags(mbuf_t mbuf, mbuf_flags_t flags)
{
	if ((flags & ~mbuf_flags_mask) != 0) return EINVAL;
	mbuf->m_flags = flags |
		(mbuf->m_flags & ~mbuf_flags_mask);
	
	return 0;
}

errno_t mbuf_setflags_mask(mbuf_t mbuf, mbuf_flags_t flags, mbuf_flags_t mask)
{
	if (((flags | mask) & ~mbuf_flags_mask) != 0) return EINVAL;
	
	mbuf->m_flags = (flags & mask) | (mbuf->m_flags & ~mask);
	
	return 0;
}

errno_t mbuf_copy_pkthdr(mbuf_t dest, const mbuf_t src)
{
	if (((src)->m_flags & M_PKTHDR) == 0)
		return EINVAL;
	
	m_copy_pkthdr(dest, src);
	
	return 0;
}

size_t mbuf_pkthdr_len(const mbuf_t mbuf)
{
	return mbuf->m_pkthdr.len;
}

void mbuf_pkthdr_setlen(mbuf_t mbuf, size_t len)
{
	mbuf->m_pkthdr.len = len;
}

void mbuf_pkthdr_adjustlen(mbuf_t mbuf, int amount)
{
	mbuf->m_pkthdr.len += amount;
}

ifnet_t mbuf_pkthdr_rcvif(const mbuf_t mbuf)
{
	// If we reference count ifnets, we should take a reference here before returning
	return mbuf->m_pkthdr.rcvif;
}

errno_t mbuf_pkthdr_setrcvif(mbuf_t mbuf, ifnet_t ifnet)
{
	/* May want to walk ifnet list to determine if interface is valid */
	mbuf->m_pkthdr.rcvif = (struct ifnet*)ifnet;
	return 0;
}

void* mbuf_pkthdr_header(const mbuf_t mbuf)
{
	return mbuf->m_pkthdr.header;
}

void mbuf_pkthdr_setheader(mbuf_t mbuf, void *header)
{
	mbuf->m_pkthdr.header = (void*)header;
}

void
mbuf_inbound_modified(mbuf_t mbuf)
{
	/* Invalidate hardware generated checksum flags */
	mbuf->m_pkthdr.csum_flags = 0;
}

extern void in_cksum_offset(struct mbuf* m, size_t ip_offset);
extern void in_delayed_cksum_offset(struct mbuf *m, int ip_offset);

void
mbuf_outbound_finalize(mbuf_t mbuf, u_int32_t protocol_family, size_t protocol_offset)
{
	if ((mbuf->m_pkthdr.csum_flags &
		 (CSUM_DELAY_DATA | CSUM_DELAY_IP | CSUM_TCP_SUM16)) == 0)
		return;
	
	/* Generate the packet in software, client needs it */
	switch (protocol_family) {
		case PF_INET:
			if (mbuf->m_pkthdr.csum_flags & CSUM_TCP_SUM16) {
				/*
				 * If you're wondering where this lovely code comes
				 * from, we're trying to undo what happens in ip_output.
				 * Look for CSUM_TCP_SUM16 in ip_output.
				 */
				u_int16_t	first, second;
				mbuf->m_pkthdr.csum_flags &= ~CSUM_TCP_SUM16;
				mbuf->m_pkthdr.csum_flags |= CSUM_TCP;
				first = mbuf->m_pkthdr.csum_data >> 16;
				second = mbuf->m_pkthdr.csum_data & 0xffff;
				mbuf->m_pkthdr.csum_data = first - second;
			}
			if (mbuf->m_pkthdr.csum_flags & CSUM_DELAY_DATA) {
				in_delayed_cksum_offset(mbuf, protocol_offset);
			}
			
			if (mbuf->m_pkthdr.csum_flags & CSUM_DELAY_IP) {
				in_cksum_offset(mbuf, protocol_offset);
			}
			
			mbuf->m_pkthdr.csum_flags &= ~(CSUM_DELAY_DATA | CSUM_DELAY_IP);
			break;
	
		default:
			/*
			 * Not sure what to do here if anything.
			 * Hardware checksum code looked pretty IPv4 specific.
			 */
			if ((mbuf->m_pkthdr.csum_flags & (CSUM_DELAY_DATA | CSUM_DELAY_IP)) != 0)
				panic("mbuf_outbound_finalize - CSUM flags set for non-IPv4 packet (%u)!\n", protocol_family);
	}
}

errno_t
mbuf_set_vlan_tag(
	mbuf_t mbuf,
	u_int16_t vlan)
{
	mbuf->m_pkthdr.csum_flags |= CSUM_VLAN_TAG_VALID;
	mbuf->m_pkthdr.vlan_tag = vlan;
	
	return 0;
}

errno_t
mbuf_get_vlan_tag(
	mbuf_t mbuf,
	u_int16_t *vlan)
{
	if ((mbuf->m_pkthdr.csum_flags & CSUM_VLAN_TAG_VALID) == 0)
		return ENXIO; // No vlan tag set
	
	*vlan = mbuf->m_pkthdr.vlan_tag;
	
	return 0;
}

errno_t
mbuf_clear_vlan_tag(
	mbuf_t mbuf)
{
	mbuf->m_pkthdr.csum_flags &= ~CSUM_VLAN_TAG_VALID;
	mbuf->m_pkthdr.vlan_tag = 0;
	
	return 0;
}

static const mbuf_csum_request_flags_t mbuf_valid_csum_request_flags = 
	MBUF_CSUM_REQ_IP | MBUF_CSUM_REQ_TCP | MBUF_CSUM_REQ_UDP | MBUF_CSUM_REQ_SUM16;

errno_t
mbuf_set_csum_requested(
	mbuf_t mbuf,
	mbuf_csum_request_flags_t request,
	u_int32_t value)
{
	request &= mbuf_valid_csum_request_flags;
	mbuf->m_pkthdr.csum_flags = (mbuf->m_pkthdr.csum_flags & 0xffff0000) | request;
	mbuf->m_pkthdr.csum_data = value;
	
	return 0;
}

static const mbuf_tso_request_flags_t mbuf_valid_tso_request_flags = 
	MBUF_TSO_IPV4 | MBUF_TSO_IPV6;

errno_t
mbuf_get_tso_requested(
	mbuf_t mbuf,
	mbuf_tso_request_flags_t *request,
	u_int32_t *value)
{
	if (mbuf == NULL || (mbuf->m_flags & M_PKTHDR) == 0 ||
			request == NULL || value == NULL)
		return EINVAL;

	*request = mbuf->m_pkthdr.csum_flags;
	*request &= mbuf_valid_tso_request_flags;
	if (*request && value != NULL) 
		*value = mbuf->m_pkthdr.tso_segsz;
	
	return 0;
}

errno_t
mbuf_get_csum_requested(
	mbuf_t mbuf,
	mbuf_csum_request_flags_t *request,
	u_int32_t *value)
{
	*request = mbuf->m_pkthdr.csum_flags;
	*request &= mbuf_valid_csum_request_flags;
	if (value != NULL) {
		*value = mbuf->m_pkthdr.csum_data;
	}
	
	return 0;
}

errno_t
mbuf_clear_csum_requested(
	mbuf_t mbuf)
{
	mbuf->m_pkthdr.csum_flags &= 0xffff0000;
	mbuf->m_pkthdr.csum_data = 0;
	
	return 0;
}

static const mbuf_csum_performed_flags_t mbuf_valid_csum_performed_flags = 
	MBUF_CSUM_DID_IP | MBUF_CSUM_IP_GOOD | MBUF_CSUM_DID_DATA |
	MBUF_CSUM_PSEUDO_HDR | MBUF_CSUM_TCP_SUM16;

errno_t
mbuf_set_csum_performed(
	mbuf_t mbuf,
	mbuf_csum_performed_flags_t performed,
	u_int32_t value)
{
	performed &= mbuf_valid_csum_performed_flags;
	mbuf->m_pkthdr.csum_flags = (mbuf->m_pkthdr.csum_flags & 0xffff0000) | performed;
	mbuf->m_pkthdr.csum_data = value;
	
	return 0;
}

errno_t
mbuf_get_csum_performed(
	mbuf_t mbuf,
	mbuf_csum_performed_flags_t *performed,
	u_int32_t *value)
{
	*performed = mbuf->m_pkthdr.csum_flags & mbuf_valid_csum_performed_flags;
	*value = mbuf->m_pkthdr.csum_data;
	
	return 0;
}

errno_t
mbuf_clear_csum_performed(
	mbuf_t mbuf)
{
	mbuf->m_pkthdr.csum_flags &= 0xffff0000;
	mbuf->m_pkthdr.csum_data = 0;
	
	return 0;
}

errno_t
mbuf_inet_cksum(mbuf_t mbuf, int protocol, u_int32_t offset, u_int32_t length,
    u_int16_t *csum)
{
	if (mbuf == NULL || length == 0 || csum == NULL ||
	   (u_int32_t)mbuf->m_pkthdr.len < (offset + length))
		return (EINVAL);

	*csum = inet_cksum(mbuf, protocol, offset, length);
	return (0);
}

#if INET6
errno_t
mbuf_inet6_cksum(mbuf_t mbuf, int protocol, u_int32_t offset, u_int32_t length,
    u_int16_t *csum)
{
	if (mbuf == NULL || length == 0 || csum == NULL ||
	   (u_int32_t)mbuf->m_pkthdr.len < (offset + length))
		return (EINVAL);

	*csum = inet6_cksum(mbuf, protocol, offset, length);
	return (0);
}
#else /* INET6 */
errno_t
mbuf_inet6_cksum(__unused mbuf_t mbuf, __unused int protocol,
		__unused u_int32_t offset, __unused u_int32_t length,
		__unused u_int16_t *csum)
{
	panic("mbuf_inet6_cksum() doesn't exist on this platform\n");
	return (0);
}

u_int16_t
inet6_cksum(__unused struct mbuf *m, __unused unsigned int nxt,
		__unused unsigned int off, __unused unsigned int len)
{
	panic("inet6_cksum() doesn't exist on this platform\n");
	return (0);
}

void nd6_lookup_ipv6(void);
void
nd6_lookup_ipv6(void)
{
	panic("nd6_lookup_ipv6() doesn't exist on this platform\n");
}

int
in6addr_local(__unused struct in6_addr *a)
{
	panic("in6addr_local() doesn't exist on this platform\n");
	return (0);
}

void nd6_storelladdr(void);
void
nd6_storelladdr(void)
{
	panic("nd6_storelladdr() doesn't exist on this platform\n");
}
#endif /* INET6 */

/*
 * Mbuf tag KPIs
 */

#define MTAG_FIRST_ID FIRST_KPI_STR_ID

errno_t
mbuf_tag_id_find(
	const char		*string,
	mbuf_tag_id_t	*out_id)
{
	return net_str_id_find_internal(string, out_id, NSI_MBUF_TAG, 1);
}

errno_t
mbuf_tag_allocate(
	mbuf_t			mbuf,
	mbuf_tag_id_t	id,
	mbuf_tag_type_t	type,
	size_t			length,
	mbuf_how_t		how,
	void**			data_p)
{
	struct m_tag *tag;
	u_int32_t mtag_id_first, mtag_id_last;
	
	if (data_p != NULL)
		*data_p = NULL;
	
	/* Sanity check parameters */
	(void) net_str_id_first_last(&mtag_id_first, &mtag_id_last, NSI_MBUF_TAG);
	if (mbuf == NULL || (mbuf->m_flags & M_PKTHDR) == 0 || id < mtag_id_first ||
		id > mtag_id_last || length < 1 || (length & 0xffff0000) != 0 ||
		data_p == NULL) {
		return EINVAL;
	}
	
	/* Make sure this mtag hasn't already been allocated */
	tag = m_tag_locate(mbuf, id, type, NULL);
	if (tag != NULL) {
		return EEXIST;
	}
	
	/* Allocate an mtag */
	tag = m_tag_alloc(id, type, length, how);
	if (tag == NULL) {
		return how == M_WAITOK ? ENOMEM : EWOULDBLOCK;
	}
	
	/* Attach the mtag and set *data_p */
	m_tag_prepend(mbuf, tag);
	*data_p = tag + 1;
	
	return 0;
}

errno_t
mbuf_tag_find(
	mbuf_t			mbuf,
	mbuf_tag_id_t	id,
	mbuf_tag_type_t	type,
	size_t*			length,
	void**			data_p)
{
	struct m_tag *tag;
	u_int32_t mtag_id_first, mtag_id_last;
	
	if (length != NULL)
		*length = 0;
	if (data_p != NULL)
		*data_p = NULL;
	
	/* Sanity check parameters */
	(void) net_str_id_first_last(&mtag_id_first, &mtag_id_last, NSI_MBUF_TAG);
	if (mbuf == NULL || (mbuf->m_flags & M_PKTHDR) == 0 || id < mtag_id_first ||
		id > mtag_id_last || length == NULL || data_p == NULL) {
		return EINVAL;
	}
	
	/* Locate an mtag */
	tag = m_tag_locate(mbuf, id, type, NULL);
	if (tag == NULL) {
		return ENOENT;
	}
	
	/* Copy out the pointer to the data and the lenght value */
	*length = tag->m_tag_len;
	*data_p = tag + 1;
	
	return 0;
}

void
mbuf_tag_free(
	mbuf_t			mbuf,
	mbuf_tag_id_t	id,
	mbuf_tag_type_t	type)
{
	struct m_tag *tag;
	u_int32_t mtag_id_first, mtag_id_last;
	
	/* Sanity check parameters */
	(void) net_str_id_first_last(&mtag_id_first, &mtag_id_last, NSI_MBUF_TAG);
	if (mbuf == NULL || (mbuf->m_flags & M_PKTHDR) == 0 || id < mtag_id_first ||
		id > mtag_id_last)
		return;
	
	tag = m_tag_locate(mbuf, id, type, NULL);
	if (tag == NULL) {
		return;
	}
	
	m_tag_delete(mbuf, tag);
	return;
}

/* mbuf stats */
void mbuf_stats(struct mbuf_stat *stats)
{
	stats->mbufs = mbstat.m_mbufs;
	stats->clusters = mbstat.m_clusters;
	stats->clfree = mbstat.m_clfree;
	stats->drops = mbstat.m_drops;
	stats->wait = mbstat.m_wait;
	stats->drain = mbstat.m_drain;
	__builtin_memcpy(stats->mtypes, mbstat.m_mtypes, sizeof(stats->mtypes));
	stats->mcfail = mbstat.m_mcfail;
	stats->mpfail = mbstat.m_mpfail;
	stats->msize = mbstat.m_msize;
	stats->mclbytes = mbstat.m_mclbytes;
	stats->minclsize = mbstat.m_minclsize;
	stats->mlen = mbstat.m_mlen;
	stats->mhlen = mbstat.m_mhlen;
	stats->bigclusters = mbstat.m_bigclusters;
	stats->bigclfree = mbstat.m_bigclfree;
	stats->bigmclbytes = mbstat.m_bigmclbytes;
}

errno_t
mbuf_allocpacket(mbuf_how_t how, size_t packetlen, unsigned int *maxchunks, mbuf_t *mbuf)
{
	errno_t error;
	struct mbuf *m;
	unsigned int numpkts = 1;
	unsigned int numchunks = maxchunks ? *maxchunks : 0;

	if (packetlen == 0) {
		error = EINVAL;
		goto out;
	}
	m = m_allocpacket_internal(&numpkts, packetlen, maxchunks ? &numchunks : NULL, how, 1, 0);
	if (m == 0) {
		if (maxchunks && *maxchunks && numchunks > *maxchunks)
			error = ENOBUFS;
		else
			error = ENOMEM;
	} else {
		if (maxchunks)
			*maxchunks = numchunks;
		error = 0;
		*mbuf = m;
	}
out:
	return error;
}

errno_t
mbuf_allocpacket_list(unsigned int numpkts, mbuf_how_t how, size_t packetlen, unsigned int *maxchunks, mbuf_t *mbuf)
{
	errno_t error;
	struct mbuf *m;
	unsigned int numchunks = maxchunks ? *maxchunks : 0;

	if (numpkts == 0) {
		error = EINVAL;
		goto out;
	}
	if (packetlen == 0) {
		error = EINVAL;
		goto out;
	}
	m = m_allocpacket_internal(&numpkts, packetlen, maxchunks ? &numchunks : NULL, how, 1, 0);
	if (m == 0) {
		if (maxchunks && *maxchunks && numchunks > *maxchunks)
			error = ENOBUFS;
		else
			error = ENOMEM;
	} else {
		if (maxchunks)
			*maxchunks = numchunks;
		error = 0;
		*mbuf = m;
	}
out:
	return error;
}



/*
 * mbuf_copyback differs from m_copyback in a few ways:
 * 1) mbuf_copyback will allocate clusters for new mbufs we append
 * 2) mbuf_copyback will grow the last mbuf in the chain if possible
 * 3) mbuf_copyback reports whether or not the operation succeeded
 * 4) mbuf_copyback allows the caller to specify M_WAITOK or M_NOWAIT
 */
errno_t
mbuf_copyback(
	mbuf_t		m,
	size_t		off,
	size_t		len,
	const void	*data,
	mbuf_how_t	how)
{
	size_t	mlen;
	mbuf_t	m_start = m;
	mbuf_t	n;
	int		totlen = 0;
	errno_t		result = 0;
	const char	*cp = data;

	if (m == NULL || len == 0 || data == NULL)
		return EINVAL;
	
	while (off > (mlen = m->m_len)) {
		off -= mlen;
		totlen += mlen;
		if (m->m_next == 0) {
			n = m_getclr(how, m->m_type);
			if (n == 0) {
				result = ENOBUFS;
				goto out;
			}
			n->m_len = MIN(MLEN, len + off);
			m->m_next = n;
		}
		m = m->m_next;
	}
	
	while (len > 0) {
		mlen = MIN(m->m_len - off, len);
		if (mlen < len && m->m_next == NULL && mbuf_trailingspace(m) > 0) {
			size_t	grow = MIN(mbuf_trailingspace(m), len - mlen);
			mlen += grow;
			m->m_len += grow;
		}
		bcopy(cp, off + (char*)mbuf_data(m), (unsigned)mlen);
		cp += mlen;
		len -= mlen;
		mlen += off;
		off = 0;
		totlen += mlen;
		if (len == 0)
			break;
		if (m->m_next == 0) {
			n = m_get(how, m->m_type);
			if (n == NULL) {
				result = ENOBUFS;
				goto out;
			}
			if (len > MINCLSIZE) {
				/* cluter allocation failure is okay, we can grow chain */
				mbuf_mclget(how, m->m_type, &n);
			}
			n->m_len = MIN(mbuf_maxlen(n), len);
			m->m_next = n;
		}
		m = m->m_next;
	}
	
out:
	if ((m_start->m_flags & M_PKTHDR) && (m_start->m_pkthdr.len < totlen))
		m_start->m_pkthdr.len = totlen;
	
	return result;
}

u_int32_t
mbuf_get_mlen(void)
{
	return (_MLEN);
}

u_int32_t
mbuf_get_mhlen(void)
{
	return (_MHLEN);
}

mbuf_priority_t
mbuf_get_priority(struct mbuf *m)
{
#if !PKT_PRIORITY
#pragma unused(m)
	return (MBUF_PRIORITY_NORMAL);
#else /* PKT_PRIORITY */
	mbuf_priority_t prio = MBUF_PRIORITY_NORMAL;

	if (m == NULL || !(m->m_flags & M_PKTHDR))
		return (prio);

	/* Defaults to normal; ignore anything else but background */
	if (m->m_pkthdr.prio == MBUF_PRIORITY_BACKGROUND)
		prio = MBUF_PRIORITY_BACKGROUND;

	return (prio);
#endif /* PKT_PRIORITY */
}
