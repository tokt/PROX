/*
  Copyright(c) 2010-2016 Intel Corporation.
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the
      distribution.
    * Neither the name of Intel Corporation nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#ifndef _MBUF_UTILS_H_
#define _MBUF_UTILS_H_

#include <string.h>

#include <rte_ip.h>
#include <rte_version.h>
#include <rte_ether.h>

static void init_mbuf_seg(struct rte_mbuf *mbuf)
{
#if RTE_VERSION >= RTE_VERSION_NUM(1,8,0,0)
	mbuf->nb_segs = 1;
#else
	mbuf->pkt.nb_segs = 1;
#endif
	rte_mbuf_refcnt_set(mbuf, 1);
}

static uint16_t pkt_len_to_wire_size(uint16_t pkt_len)
{
	return (pkt_len < 60? 60 : pkt_len) + ETHER_CRC_LEN + 20;
}

static uint16_t mbuf_wire_size(const struct rte_mbuf *mbuf)
{
	uint16_t pkt_len = rte_pktmbuf_pkt_len(mbuf);

	return pkt_len_to_wire_size(pkt_len);
}

static uint16_t mbuf_calc_padlen(const struct rte_mbuf *mbuf, void *pkt, struct ipv4_hdr *ipv4)
{
	uint16_t pkt_len = rte_pktmbuf_pkt_len(mbuf);
	uint16_t ip_offset = (uint8_t *)ipv4 - (uint8_t*)pkt;
	uint16_t ip_total_len = rte_be_to_cpu_16(ipv4->total_length);

	return pkt_len - ip_total_len - ip_offset;
}

#endif /* _MBUF_UTILS_H_ */
