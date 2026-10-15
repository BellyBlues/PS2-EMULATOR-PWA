// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// libpcap stand-ins for the WebAssembly build. Pages cannot capture or inject raw
// network packets, so every call fails and the PCAP adapter never initialises.
// DEV9 networking on the web would need a socket-free backend (e.g. relayed over a
// WebSocket); until then only the "Sockets" API could ever be wired up.

#include "pcap.h"

#include <cstdio>

static constexpr const char* NO_PCAP = "Packet capture is not available in the browser";
static char s_no_pcap_error[] = "Packet capture is not available in the browser";

extern "C" {

int pcap_findalldevs(pcap_if_t** alldevs, char* errbuf)
{
	*alldevs = nullptr;
	std::snprintf(errbuf, PCAP_ERRBUF_SIZE, "%s", NO_PCAP);
	return -1;
}

pcap_t* pcap_open_live(const char* device, int snaplen, int promisc, int to_ms, char* errbuf)
{
	std::snprintf(errbuf, PCAP_ERRBUF_SIZE, "%s", NO_PCAP);
	return nullptr;
}

void pcap_close(pcap_t* p)
{
}

int pcap_next_ex(pcap_t* p, struct pcap_pkthdr** pkt_header, const u_char** pkt_data)
{
	return PCAP_ERROR;
}

int pcap_setfilter(pcap_t* p, struct bpf_program* fp)
{
	return PCAP_ERROR;
}

int pcap_setnonblock(pcap_t* p, int nonblock, char* errbuf)
{
	std::snprintf(errbuf, PCAP_ERRBUF_SIZE, "%s", NO_PCAP);
	return PCAP_ERROR;
}

int pcap_sendpacket(pcap_t* p, const u_char* buf, int size)
{
	return PCAP_ERROR;
}

char* pcap_geterr(pcap_t* p)
{
	return s_no_pcap_error;
}

int pcap_compile(pcap_t* p, struct bpf_program* fp, const char* str, int optimize, bpf_u_int32 netmask)
{
	return PCAP_ERROR;
}

void pcap_freecode(struct bpf_program* fp)
{
}

int pcap_datalink(pcap_t* p)
{
	return PCAP_ERROR;
}

const char* pcap_datalink_val_to_name(int dlt)
{
	return nullptr;
}

} // extern "C"
