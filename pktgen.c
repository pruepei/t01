/*
 * Copyright 2016 <copyright holder> <email>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <ifaddrs.h>
#include <net/ethernet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netinet/tcp.h>
#include <netinet/ether.h>
#include <netinet/if_ether.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <arpa/inet.h>

#include "pktgen.h"
#include "rule.h"
#include "ndpi_protocol_ids.h"

struct dns_pkt			//固定长度的12个字节
{
	u_int16_t trans_id;
	__u16 qr:1, opcode:4, aa:1, tc:1, rd:1, ra:1, zero:3, rcode:4;
	u_int16_t question;
	u_int16_t answer;
	u_int16_t authority;
	u_int16_t additional;
};

struct queries {
	u_int16_t type;
	u_int16_t class;
};

#pragma pack(1)
struct answers {
	u_int16_t type;
	u_int16_t class;
	u_int32_t ttl;		//生存时间
	u_int16_t data_length;	//若addr是解析后的域名,一般长度为4;
	u_int32_t addr;		//域名转换
};
#pragma pack()

struct pptp_setlink {
	u_int16_t length;
	u_int16_t m_type;
	u_int32_t cookie;
	u_int16_t c_type;	//通过这个判断是set-link-info 
	u_int16_t reserve1;
	u_int16_t id;
	u_int16_t reserve2;
	u_int32_t s_accm;
	u_int32_t r_accm;
};

static inline unsigned long csum_tcpudp_nofold(unsigned long saddr,
					       unsigned long daddr,
					       unsigned short len,
					       unsigned short proto,
					       unsigned int sum)
{
	asm("addl %1, %0\n"	/* 累加daddr */
	    "adcl %2, %0\n"	/* 累加saddr */
	    "adcl %3, %0\n"	/* 累加len(2字节), proto, 0 */
	    "adcl $0, %0\n"	/*加上进位 */
:	    "=r"(sum)
:	    "g"(daddr), "g"(saddr), "g"((ntohs(len) << 16) + proto * 256),
	    "0"(sum));
	return sum;
}

static inline unsigned int csum_fold(unsigned int sum)
{
	sum = (sum & 0xffff) + (sum >> 16);	//将高16 叠加到低16
	sum = (sum & 0xffff) + (sum >> 16);	//将产生的进位叠加到 低16
	return (unsigned short)~sum;
}

static inline unsigned short csum_tcpudp_magic(unsigned long saddr,
					       unsigned long daddr,
					       unsigned short len,
					       unsigned short proto,
					       unsigned int sum)
{
	return csum_fold(csum_tcpudp_nofold(saddr, daddr, len, proto, sum));
}

static inline unsigned short tcp_v4_check(int len, unsigned long saddr,
					  unsigned long daddr, unsigned base)
{
	return csum_tcpudp_magic(saddr, daddr, len, IPPROTO_TCP, base);
}

static inline unsigned short udp_v4_check(int len, unsigned long saddr,
					  unsigned long daddr, unsigned base)
{
	return csum_tcpudp_magic(saddr, daddr, len, IPPROTO_UDP, base);
}

static inline unsigned short from32to16(unsigned int x)
{
	/* add up 16-bit and 16-bit for 16+c bit */
	x = (x & 0xffff) + (x >> 16);
	/* add up carry.. */
	x = (x & 0xffff) + (x >> 16);
	return x;
}

static inline unsigned int do_csum(const unsigned char *buff, int len)
{
	int odd;
	unsigned int result = 0;

	if (len <= 0)
		goto out;
	odd = 1 & (unsigned long)buff;
	if (odd) {
#ifdef __LITTLE_ENDIAN
		result += (*buff << 8);
#else
		result = *buff;
#endif
		len--;
		buff++;
	}
	if (len >= 2) {
		if (2 & (unsigned long)buff) {
			result += *(unsigned short *)buff;
			len -= 2;
			buff += 2;
		}
		if (len >= 4) {
			const unsigned char *end = buff + ((unsigned)len & ~3);
			unsigned int carry = 0;
			do {
				unsigned int w = *(unsigned int *)buff;
				buff += 4;
				result += carry;
				result += w;
				carry = (w > result);
			} while (buff < end);
			result += carry;
			result = (result & 0xffff) + (result >> 16);
		}
		if (len & 2) {
			result += *(unsigned short *)buff;
			buff += 2;
		}
	}
	if (len & 1)
#ifdef __LITTLE_ENDIAN
		result += *buff;
#else
		result += (*buff << 8);
#endif
	result = from32to16(result);
	if (odd)
		result = ((result >> 8) & 0xff) | ((result & 0xff) << 8);
out:
	return result;
}

int make_http_redirect_packet(const char *target_url, const char *hdr,
			      char *result, int len)
{
	struct ether_header *src_eth = (struct ether_header *)hdr;
	struct iphdr *src_iph = (struct iphdr *)(src_eth + 1);
	struct tcphdr *src_tcph = (struct tcphdr *)(src_iph + 1);
	struct ether_header *eth = (struct ether_header *)result;
	struct iphdr *iph = (struct iphdr *)(eth + 1);
	struct tcphdr *tcph = (struct tcphdr *)(iph + 1);
	char *payload = (char *)(tcph + 1);

	const char *http_redirect_header = "HTTP/1.1 301 Moved Permanently\r\n"
	    "Connection: keep-alive\r\n"
	    "Location: http://%s\r\n"
	    "Content-Type: text/html\r\n"
	    "Content-length: 0\r\n" "Cache-control: no-cache\r\n" "\r\n";
	int data_len =
	    snprintf(payload, len - sizeof(*eth) - sizeof(*iph) - sizeof(*tcph),
		     http_redirect_header, target_url);

	memcpy(eth->ether_shost, src_eth->ether_dhost, 6);
	memcpy(eth->ether_dhost, src_eth->ether_shost, 6);
	eth->ether_type = htons(0x0800);

	//Fill in the IP Header
	iph->ihl = src_iph->ihl;
	iph->version = src_iph->version;
	iph->tos = 0;
	iph->tot_len =
	    htons(sizeof(struct iphdr) + sizeof(struct tcphdr) + data_len);
	iph->id = htons(36742);	//Id of this packet
	iph->frag_off = htons(16384);
	iph->ttl = 128;
	iph->protocol = IPPROTO_TCP;
	iph->check = 0;		//Set to 0 before calculating checksum
	iph->saddr = src_iph->daddr;
	iph->daddr = src_iph->saddr;
	//Ip checksum
	iph->check = csum_fold(do_csum((unsigned char *)iph, 20));

	//TCP Header
	tcph->source = src_tcph->dest;
	tcph->dest = src_tcph->source;

	tcph->doff = 5;
	tcph->seq = src_tcph->ack_seq;
	tcph->ack_seq =
	    htonl(ntohl(src_tcph->seq) + ntohs(src_iph->tot_len) -
		  4 * src_tcph->doff - sizeof(*src_iph));

	tcph->fin = 0;
	tcph->syn = 0;
	tcph->rst = 0;
	tcph->psh = 1;
	tcph->ack = 1;
	tcph->urg = 0;

	tcph->window = htons(64240);	/* maximum allowed window size */
	tcph->check = 0;
	tcph->urg_ptr = 0;

	tcph->check =
	    tcp_v4_check(sizeof(struct tcphdr) + data_len, iph->saddr,
			 iph->daddr, do_csum((unsigned char *)(tcph),
					     sizeof(struct tcphdr) + data_len));

	return sizeof(struct ether_header) + sizeof(struct iphdr) +
	    sizeof(struct tcphdr) + data_len;
}

int make_dns_spoof_packet(const char *target, const char *hdr, char *datagram,
			  int datagram_len)
{
	struct ether_header *eh = (struct ether_header *)hdr;
	struct iphdr *ippkt = (struct iphdr *)(eh + 1);
	struct udphdr *udppkt = (struct udphdr *)(ippkt + 1);
	struct dns_pkt *dnspkt = (struct dns_pkt *)(udppkt + 1);
	char *domain = (char *)dnspkt + sizeof(struct dns_pkt);
	int domain_len = strlen(domain) + 1;
	struct ether_header *eth = (struct ether_header *)datagram;
	struct iphdr *iph = (struct iphdr *)(eth + 1);
	struct udphdr *udph = (struct udphdr *)(iph + 1);
	struct dns_pkt *dnsh = (struct dns_pkt *)(udph + 1);
	char *data_1 = (char *)dnsh + sizeof(struct dns_pkt);
	struct queries *queries_h = (struct queries *)(data_1 + domain_len);
	char *data_2 = (char *)(queries_h + 1);
	struct answers *answers_h = (struct answers *)(data_2 + 2);
	int data_len =
	    sizeof(struct dns_pkt) + domain_len + sizeof(struct queries) + 2 +
	    sizeof(struct answers);

	//Fill in the MAC Header 
	memcpy(eth->ether_shost, eh->ether_dhost, 6);
	memcpy(eth->ether_dhost, eh->ether_shost, 6);
	eth->ether_type = htons(0x0800);

	//Fill in the IP Header
	iph->ihl = 5;
	iph->version = 4;
	iph->tos = 0;
	iph->tot_len =
	    htons((sizeof(struct iphdr) + sizeof(struct udphdr)) + data_len);
	iph->id = htons(36742);	//Id of this packet
	iph->frag_off = htons(16384);
	iph->ttl = 128;
	iph->protocol = 17;	//IPPROTO_UDP;
	iph->check = 0;		//Set to 0 before calculating checksum
	iph->saddr = ippkt->daddr;
	iph->daddr = ippkt->saddr;
	iph->check = csum_fold(do_csum((unsigned char *)iph, 20));

	//Fill in the UDP Header
	udph->source = udppkt->dest;
	udph->dest = udppkt->source;
	udph->len = htons(sizeof(struct udphdr) + data_len);
	udph->check = 0;

	//DNS header init
	dnsh->trans_id = dnspkt->trans_id;
	dnsh->qr = 1;
	dnsh->opcode = dnspkt->opcode;
	dnsh->aa = 0;
	dnsh->tc = 0;
	dnsh->rd = 1;
	dnsh->ra = dnspkt->question;
	dnsh->zero = dnspkt->zero;
	dnsh->rcode = 0;
	dnsh->question = htons(1);
	dnsh->answer = htons(1);
	dnsh->authority = 0;
	dnsh->additional = 0;

	//构建Query
	strncpy(data_1, domain, domain_len);
	queries_h->type = htons(1);
	queries_h->class = htons(1);

	//构建Answer
	*(char *)data_2 = 192;	//0xc0
	*((char *)data_2 + 1) = sizeof(struct dns_pkt);	//0x0c
	answers_h->type = htons(1);
	answers_h->class = htons(1);
	answers_h->ttl = htonl(30);
	answers_h->data_length = htons(4);
	answers_h->addr = inet_addr(target);

	udph->check =
	    udp_v4_check(sizeof(struct udphdr) + data_len, iph->saddr,
			 iph->daddr, do_csum((unsigned char *)(udph),
					     sizeof(struct udphdr) + data_len));

	return sizeof(struct ether_header) + sizeof(struct iphdr) +
	    sizeof(struct udphdr) + data_len;
}

int make_pptp_rst_packet(const char *hdr, char *datagram, int datagram_len)
{
	struct ether_header *eh = (struct ether_header *)hdr;
	struct iphdr *ippkt = (struct iphdr *)(eh + 1);
	struct tcphdr *tcppkt = (struct tcphdr *)(ippkt + 1);
	struct pptp_setlink *pptppkt = (struct pptp_setlink *)(tcppkt + 1);
	//ETHER header
	struct ether_header *eth = (struct ether_header *)datagram;
	//IP header
	struct iphdr *iph = (struct iphdr *)(eth + 1);
	//TCP header
	struct tcphdr *tcph = (struct tcphdr *)(iph + 1);

	memcpy(eth->ether_shost, eh->ether_dhost, 6);
	memcpy(eth->ether_dhost, eh->ether_shost, 6);
	eth->ether_type = htons(0x0800);

	//////ip_header_init
	iph->ihl = 5;
	iph->version = 4;
	iph->tos = 0;
	iph->tot_len =
	    htons((int)(sizeof(struct iphdr) + sizeof(struct tcphdr)));
	iph->id = htons(36742);	//Id of this packet
	iph->frag_off = htons(16384);
	iph->ttl = 128;
	iph->protocol = 6;	//IPPROTO_TCP;
	iph->check = 0;		//Set to 0 before calculating checksum
	iph->saddr = ippkt->daddr;
	iph->daddr = ippkt->saddr;

	//Ip checksum
	iph->check = csum_fold(do_csum((unsigned char *)iph, 20));

	//////_tcp_header_init
	tcph->source = tcppkt->dest;
	tcph->dest = tcppkt->source;
	tcph->doff = 5;
	tcph->seq = tcppkt->ack_seq;
	tcph->ack_seq =
	    htonl(ntohl(tcppkt->seq) + ntohs(ippkt->tot_len) -
		  4 * tcppkt->doff - sizeof(*ippkt));
	tcph->fin = 1;
	tcph->syn = 0;
	tcph->rst = 0;
	tcph->psh = 0;
	tcph->ack = 1;
	tcph->urg = 0;
	tcph->window = htons(64240);	/* maximum allowed window size */
	tcph->check = 0;	//leave checksum 0 now, filled later by pseudo header
	tcph->urg_ptr = 0;

	//Now the TCP checksum 
	tcph->check =
	    tcp_v4_check(sizeof(struct tcphdr), iph->saddr,
			 iph->daddr, do_csum((unsigned char *)(tcph),
					     sizeof(struct tcphdr)));

	return sizeof(struct ether_header) + sizeof(struct iphdr) +
	    sizeof(struct tcphdr);
}

int make_packet(const struct rule *rule, const char *hdr, char *packet, int len)
{
	if (rule->action == T01_ACTION_REDIRECT
	    && rule->master_protocol == NDPI_PROTOCOL_HTTP)
		return make_http_redirect_packet(rule->action_params[0], hdr,
						 packet, len);
	else if (rule->action == T01_ACTION_CONFUSE
		 && rule->master_protocol == NDPI_PROTOCOL_DNS)
		return make_dns_spoof_packet(rule->action_params[0], hdr,
					     packet, len);
	else if (rule->action == T01_ACTION_REJECT
		 && rule->master_protocol == NDPI_PROTOCOL_PPTP)
		return make_pptp_rst_packet(hdr, packet, len);
	return 0;
}
