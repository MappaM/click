// -*- c-basic-offset: 4; related-file-name: "../include/click/netmapdevice.hh" -*-
/*
 * netmapinfo.{cc,hh} -- library for interfacing with netmap
 * Eddie Kohler, Luigi Rizzo, Tom Barbette
 *
 * Copyright (c) 2012 Eddie Kohler
 * Copyright (c) 2015 Tom Barbette
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
#include <click/netmapdevice.hh>
#include <sys/mman.h>
#include <sys/ioctl.h>

/****************************
 * NetmapBufQ
 ****************************/
NetmapBufQ::NetmapBufQ() : _head(0),_count(0){

}

NetmapBufQ::~NetmapBufQ() {

}

/**
 * Initizlize the per-thread pools and the cleanup pool
 */
int NetmapBufQ::static_initialize(struct nm_desc* nmd) {
	if (!nmd) {
		click_chatter("Error:Null netmap descriptor in NetmapBufQ::static_initialize!");
		return 1;
	}

	//Only initilize once
	if (buf_size)
		return 0;

	buf_size = nmd->some_ring->nr_buf_size;
	buf_start = reinterpret_cast<unsigned char *>(nmd->buf_start);
	buf_end = reinterpret_cast<unsigned char *>(nmd->buf_end);
	max_index = (buf_end - buf_start) / buf_size;

	if (!netmap_buf_pools) {
		netmap_buf_pools = new NetmapBufQ*[click_max_cpu_ids()];

		for (int i = 0; i < click_max_cpu_ids(); i++) {
			netmap_buf_pools[i] = new NetmapBufQ();
		}
	}

	return 0;
}

/**
 * Empty all NetmapBufQ and the global ring. Return all netmap buffers in a list
 */
uint32_t NetmapBufQ::static_cleanup()
{
	for (unsigned int i = 1; i < click_max_cpu_ids(); i++) {
		if (netmap_buf_pools[i]) {
			delete netmap_buf_pools[i];
			netmap_buf_pools[i] = NULL;
		}
	}

	while (global_buffer_list > 0) {
		uint32_t idx=global_buffer_list;
		global_buffer_list = BUFFER_NEXT_LIST(global_buffer_list);
		netmap_buf_pools[0]->insert_all(idx);
	}

	uint32_t idx = 0;
	if (netmap_buf_pools[0]->_count > 0) {
		if (netmap_buf_pools[0]->_count == netmap_buf_pools[0]->count_buffers(idx))
			click_chatter("Error on cleanup of netmap buffer ! Expected %d buffer, got %d",netmap_buf_pools[0]->_count,netmap_buf_pools[0]->count_buffers(idx));
		else
			click_chatter("Freeing %d netmap buffers",netmap_buf_pools[0]->_count);
		idx = netmap_buf_pools[0]->_head;
		netmap_buf_pools[0]->_head = 0;
		netmap_buf_pools[0]->_count = 0;
	}
	delete netmap_buf_pools[0];
	netmap_buf_pools[0] = 0;
	return idx;
}

/**
 * Insert all netmap buffers inside the global list
 */
void NetmapBufQ::global_insert_all(uint32_t idx, int count) {
	//Cut packets in global pools
	while (count >= NETMAP_PACKET_POOL_SIZE) {
		int c = 0;
		BUFFER_NEXT_LIST(idx) = global_buffer_list;
		global_buffer_list = idx;
		uint32_t *p = 0;
		while (c < NETMAP_PACKET_POOL_SIZE) {
			p = reinterpret_cast<uint32_t*>((unsigned char *)buf_start +
					idx * buf_size);
			idx = *p;
			c++;
		}
		*p = 0;
		count -= NETMAP_PACKET_POOL_SIZE;
	}

	//Add remaining buffer to the local pool
	if (count > 0) {
		NetmapBufQ::local_pool()->insert_all(idx, true);
	}
}

/***************************
 * NetmapDevice
 ***************************/

NetmapDevice::NetmapDevice(String ifname) : ifname(ifname),_use_count(0),n_queues(0) {
	n_refs = 0;
}

NetmapDevice::~NetmapDevice() {
	nics.remove(ifname);

	click_chatter("Detaching netmap device %s",ifname.c_str());
	if (parent_nmd) {
		for (int i = 0; i < n_queues; i++) {
			nm_close(nmds[i]);
			nmds[i] = NULL;
		}

		if (some_nmd != parent_nmd)
			nm_close(parent_nmd);
		parent_nmd = NULL;
	};
}

NetmapDevice* NetmapDevice::open(String ifname) {
	NetmapDevice* d = nics.find(ifname);
	if (d == NULL) {
		d = new NetmapDevice(ifname);
		if (d->initialize() != 0) {
			return NULL;
		}
		nics.insert(ifname,d);
	}
	d->_use_count++;
	return d;
}

int NetmapDevice::initialize() {
	struct nm_desc* nmd;
	struct nm_desc* base_nmd = (struct nm_desc*)calloc(1,sizeof(struct nm_desc));

	base_nmd->self = base_nmd;
	strcpy(base_nmd->req.nr_name,&(ifname.c_str()[7]));
	base_nmd->req.nr_flags = NR_REG_SW;
	if (NetmapDevice::some_nmd != NULL) { //Having same netmap space is a lot easier...
		base_nmd->mem = NetmapDevice::some_nmd->mem;
		base_nmd->memsize = NetmapDevice::some_nmd->memsize;
		base_nmd->req.nr_arg2 = NetmapDevice::some_nmd->req.nr_arg2;
		base_nmd->req.nr_arg3 = 0;
		base_nmd->done_mmap = NetmapDevice::some_nmd->done_mmap;
		NetmapDevice::some_nmd->req.nr_flags = NR_REG_SW;
		nmd = nm_open(ifname.c_str(), NULL, NM_OPEN_NO_MMAP | NM_OPEN_IFNAME, base_nmd);
	} else {
		base_nmd->req.nr_arg3 = NetmapDevice::global_alloc;
		if (base_nmd->req.nr_arg3 % NETMAP_PACKET_POOL_SIZE != 0)
			base_nmd->req.nr_arg3 = ((base_nmd->req.nr_arg3 / NETMAP_PACKET_POOL_SIZE) + 1) * NETMAP_PACKET_POOL_SIZE;
		//Ensure we have at least a batch per thread + 1
		if (NETMAP_PACKET_POOL_SIZE * (click_nthreads + 1) > base_nmd->req.nr_arg3)
			base_nmd->req.nr_arg3 = NETMAP_PACKET_POOL_SIZE * (click_nthreads + 1);
		nmd = nm_open(ifname.c_str(), NULL, NM_OPEN_IFNAME | NM_OPEN_ARG3, base_nmd);
		NetmapDevice::some_nmd = nmd;
	}
	if (!nmd)
		return -1;
	parent_nmd = nmd;

	if (parent_nmd->nifp->ni_name[0] == '\0')
		strcpy(parent_nmd->nifp->ni_name,&(ifname.c_str()[7]));

	if (nmd == NULL) {
		click_chatter("Unable to open %s: %s", ifname.c_str(), strerror(errno));
		return 1;
	}

	//Allocate packet pools if not already done
	if (NetmapBufQ::static_initialize(nmd) != 0) {
		nm_close(nmd);
		return -1;
	}

	if (nmd->req.nr_arg3 > 0) {
		click_chatter("Allocated %d buffers from Netmap buffer pool",nmd->req.nr_arg3);
		NetmapBufQ::global_insert_all(nmd->nifp->ni_bufs_head,nmd->req.nr_arg3);
		nmd->nifp->ni_bufs_head = 0;
		nmd->req.nr_arg3 = 0;
	}

	n_queues = nmd->nifp->ni_rx_rings;
	nmds.resize(n_queues);

	for (int i = 0; i < n_queues; i++) {
		struct nm_desc child_nmd = *nmd; //Copy mem, arg2, ...
		child_nmd.self = &child_nmd;
		child_nmd.req.nr_flags = NR_REG_ONE_NIC;
		child_nmd.req.nr_ringid =  i | NETMAP_NO_TX_POLL;

		int flags = NM_OPEN_IFNAME | NM_OPEN_NO_MMAP;

		struct nm_desc* thread_nm = nm_open(ifname.c_str(), NULL, flags,  &child_nmd);

		if (thread_nm == NULL) {
			return -1;
		}

		nmds[i] = thread_nm;
	}

	if (base_nmd != NULL) {
		free(base_nmd);
		base_nmd = NULL;
	}

	return 0;
}

void NetmapDevice::destroy() {
		--_use_count;
		if (_use_count == 0)
			delete this;
}

void NetmapDevice::static_cleanup() {
	uint32_t idx = NetmapBufQ::static_cleanup();
	if (idx != 0) {
		if (some_nmd) {
			some_nmd->nifp->ni_bufs_head = idx;
			nm_close(some_nmd);
			some_nmd = 0;
		} else {
			click_chatter("No NMD set and netmap packet not released !");
		}
	}
}

NetmapBufQ** NetmapBufQ::netmap_buf_pools = 0;
unsigned int NetmapBufQ::buf_size = 0;
unsigned char* NetmapBufQ::buf_start = 0;
unsigned char* NetmapBufQ::buf_end = 0;
uint32_t NetmapBufQ::max_index = 0;

struct nm_desc* NetmapBufQ::_some_nmd = 0;

Spinlock NetmapBufQ::global_buffer_lock;
uint32_t NetmapBufQ::global_buffer_list = 0;

int NetmapBufQ::messagelimit = 0;
HashMap<String,NetmapDevice*> NetmapDevice::nics;
struct nm_desc* NetmapDevice::some_nmd = 0;

int NetmapDevice::global_alloc = 0;

CLICK_ENDDECLS
