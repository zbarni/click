// -*- c-basic-offset: 4; related-file-name: "../include/click/ip6address.hh" -*-
/*
 * ip6address.{cc,hh} -- an IP6 address class
 * Peilei Fan, Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include <click/glue.hh>
#include <click/ip6address.hh>
#include <click/ipaddress.hh>
#include <click/straccum.hh>
#include <click/confparse.hh>
CLICK_DECLS

IP6Address::IP6Address(const String &str)
{
    static_assert(sizeof(*this) == 16, "IPAddress has the wrong size.");
    static_assert(sizeof(click_in6_addr) == 16, "click_in6_addr has the wrong size.");
    static_assert(sizeof(struct click_ip6) == 40, "click_ip6 has the wrong size.");
    if (!cp_ip6_address(str, this))
	memset(&_addr, 0, sizeof(_addr));
}

IP6Address
IP6Address::make_prefix(int prefix_len)
{
    assert(prefix_len >= 0 && prefix_len <= 128);
    IP6Address a = IP6Address::uninitialized_t();
    int i;
    for (i = 0; i < 4 && prefix_len >= 32; ++i, prefix_len -= 32)
	a._addr.s6_addr32[i] = 0xFFFFFFFFU;
    if (i < 4 && prefix_len > 0) {
	a._addr.s6_addr32[i] = htonl(0xFFFFFFFFU << (32 - prefix_len));
	++i;
    }
    for (; i < 4; ++i)
	a._addr.s6_addr32[i] = 0;
    return a;
}

IP6Address
IP6Address::make_inverted_prefix(int prefix_len)
{
    assert(prefix_len >= 0 && prefix_len <= 128);
    IP6Address a = IP6Address::uninitialized_t();
    int i;
    for (i = 0; i < 4 && prefix_len >= 32; ++i, prefix_len -= 32)
	a._addr.s6_addr32[i] = 0;
    if (i < 4 && prefix_len > 0) {
	a._addr.s6_addr32[i] = htonl(0xFFFFFFFFU >> prefix_len);
	++i;
    }
    for (; i < 4; ++i)
	a._addr.s6_addr32[i] = 0xFFFFFFFFU;
    return a;
}

int
IP6Address::mask_to_prefix_len() const
{
    // check that prefix is 0xFFFFFFFF
    int word = 0;
    while (word < 4 && _addr.s6_addr32[word] == 0xFFFFFFFFU)
	word++;
    if (word == 4)
	return 128;

    // check that suffix is zeros
    for (int zero_word = word + 1; zero_word < 4; ++zero_word)
	if (_addr.s6_addr32[zero_word] != 0)
	    return -1;

    // check swing word
    int prefix = IPAddress(_addr.s6_addr32[word]).mask_to_prefix_len();
    return prefix + (prefix >= 0 ? word * 32 : 0);
}

bool
IP6Address::ether_address(EtherAddress &mac) const
{
    /*
     * embedded mac address look like this:
     * nnnn:nnnn:nnnn:nnnn:xxxx:xxFF:FExx:xxxx
     * where xx's are the mac address.
     */
    if (_addr.s6_addr[11] == 0xFF && _addr.s6_addr[12] == 0xFE) {
	unsigned char *d = mac.data();
	d[0] = _addr.s6_addr[8];
	d[1] = _addr.s6_addr[9];
	d[2] = _addr.s6_addr[10];
	d[3] = _addr.s6_addr[13];
	d[4] = _addr.s6_addr[14];
	d[5] = _addr.s6_addr[15];
	return true;
    } else
	return false;
}

bool
IP6Address::ip4_address(IPAddress &ip4) const
{
    if (_addr.s6_addr32[0] == 0 && _addr.s6_addr32[1] == 0
	&& (_addr.s6_addr32[2] == 0 || _addr.s6_addr32[2] == htonl(0x0000FFFFU))) {
	ip4 = IPAddress(_addr.s6_addr32[3]);
	return true;
    } else
	return false;
}

void
IP6Address::unparse(StringAccum &sa) const
{
    // Unparse according to RFC 5952
    const uint32_t *a32 = data32();
    const uint16_t *a16 = data16();
    const uint8_t *a8 = data();

    // :: and special IPv4 addresses
    if (a32[0] == 0 && a32[1] == 0) {
	if (a32[2] == 0 && a32[3] == 0) {
	    sa.append("::", 2);
	    return;
	} else if (a32[2] == 0) {
	    sa.snprintf(18, "::%d.%d.%d.%d", a8[12], a8[13], a8[14], a8[15]);
	    return;
	} else {
	    sa.snprintf(23, "::ffff:%d.%d.%d.%d", a8[12], a8[13], a8[14], a8[15]);
	    return;
	}
    }

    // find the longest sequences of zero fields; if two sequences have equal
    // length, choose the first
    int zp = 0, zl = 0, lzp = 0;
    for (int p = 0; p < 8; ++p)
	if (a16[p] != 0)
	    lzp = p + 1;
	else if (p + 1 - lzp > zl) {
	    zp = lzp;
	    zl = p + 1 - lzp;
	}

    for (int p = 0; p < 8; ++p)
	if (p == zp && zl > 1) {
	    p += zl - 1;
	    sa.append("::", p == 7 ? 2 : 1);
	} else
	    sa.snprintf(5, p ? ":%x" : "%x", ntohs(a16[p]));
}

String
IP6Address::unparse() const
{
    const uint32_t *a32 = data32();
    if (a32[0] == 0 && a32[1] == 0 && a32[2] == 0 && a32[3] == 0)
	return String::make_stable("::", 2);
    else {
	StringAccum sa;
	unparse(sa);
	return sa.take_string();
    }
}

String
IP6Address::unparse_expanded() const
{
    const uint16_t *a16 = data16();
    char buf[48];
    sprintf(buf, "%x:%x:%x:%x:%x:%x:%x:%x",
	    ntohs(a16[0]), ntohs(a16[1]), ntohs(a16[2]), ntohs(a16[3]),
	    ntohs(a16[4]), ntohs(a16[5]), ntohs(a16[6]), ntohs(a16[7]));
    return String(buf);
}


// those two  methods will calculate the checksum field of ICMP6 Message.

// The checksum is the 16-bit one's complement
// of the one's complement sum of the entire ICMPv6 message starting with the
// ICMPv6 message type field, prepended with a "pseudo-header" of IPv6 header
// fields, as specified in [IPv6, section 8.1]. The Next Header value used in
// the pseudo-header is 58 (i.e. 0x3a). (NOTE: the inclusion of a pseudo-header in the
// ICMPv6 checksum is a change from IPv4; see [RFC 2460] for the rationale
// for this change.)
// A "pseudo-header" include src and dst address of ipv6 header, packet length,
// protocal field (for ICMP, it is 58) field. Packet length is the
// payloadlength from the IPv6 header, minus the length of any extension
// header present between the IPv6 header and the upper-layer header.

// The following methods only differ at how it deal with ip6 address, i.e. add 32 bit
// a time or 16 bits a time.


uint16_t
in6_fast_cksum(const struct click_in6_addr *saddr,
               const struct click_in6_addr *daddr,
               uint16_t len,
               uint8_t proto,
               uint16_t ori_csum,
               const unsigned char *addr,
               uint16_t len2)
{
	uint16_t ulen;
	uint16_t uproto;
	uint16_t answer = 0;
	uint32_t csum =0;
	uint32_t carry;


	//get the sum of source and destination address
	for (int i=0; i<4; i++) {

	  csum += ntohl(saddr->s6_addr32[i]);
	  carry = (csum < ntohl(saddr->s6_addr32[i]));
	  csum += carry;
	}

	for (int i=0; i<4; i++) {

	   csum += ntohl(daddr->s6_addr32[i]);
	   carry = (csum < ntohl(daddr->s6_addr32[i]));
	   csum += carry;
	}

	//get the sum of other fields:  packet length, protocal
	ulen = ntohs(len);
	csum += ulen;

	uproto = proto;
	csum += uproto;

	//get the sum of the ICMP6 package
	uint16_t nleft = ntohs(len2);
	const uint16_t *w = (const uint16_t *)addr;
	while (nleft > 1)  {
	    uint16_t w2=*w++;
	    csum += ntohs(w2);
	    nleft -=2;
	 }

	 //mop up an odd byte, if necessary
	  if (nleft == 1) {
	    *(unsigned char *)(&answer) = *(const unsigned char *)w ;
	    csum += ntohs(answer);
	  }
	  csum -= ntohs(ori_csum); //get rid of the effect of ori_csum in the calculation

	  // fold >=32-bit csum to 16-bits
	  while (csum>>16) {
	    csum = (csum & 0xffff) + (csum >> 16);
	  }

	  answer = ~csum;          // truncate to 16 bits
	  return answer;
}


//This is the slow way for in6_cksum
unsigned short
in6_cksum(const struct click_in6_addr *saddr,
	  const struct click_in6_addr *daddr,
	  uint16_t len,
	  uint8_t proto,
	  uint16_t ori_csum,
	  unsigned char *addr,
	  uint16_t len2)
{
	uint16_t ulen;
	uint16_t uproto;
	uint16_t answer = 0;
	uint32_t csum =0;


	//get the sum of source and destination address
	for (int i=0; i<8; i++) {
	  csum += ntohs(saddr->s6_addr16[i]);
	}

	for (int i=0; i<8; i++) {
	   csum += ntohs(daddr->s6_addr16[i]);
	}

	//get the sum of other fields:  packet length, protocal
	ulen = ntohs(len);
	csum += ulen;

	uproto = proto;
	csum += uproto;

	//get the sum of the ICMP6 package
	uint16_t nleft = ntohs(len2);
	const uint16_t *w = (const uint16_t *)addr;
	while (nleft > 1)  {
	    uint16_t w2=*w++;
	    csum += ntohs(w2);
	    nleft -=2;
	 }

	 //mop up an odd byte, if necessary
	  if (nleft == 1) {
	    *(unsigned char *)(&answer) = *(const unsigned char *)w ;
	    csum += ntohs(answer);
	  }
	  csum -= ntohs(ori_csum); //get rid of the effect of ori_csum in the calculation

	  // fold >=32-bit csum to 16-bits
	  while (csum>>16) {
	    csum = (csum & 0xffff) + (csum >> 16);
	  }

	  answer = ~csum;          // truncate to 16 bits
	  return answer;
}

CLICK_ENDDECLS
