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

#include <string.h>
#include <locale.h>
#include <unistd.h>
#include <signal.h>

#include <rte_cycles.h>
#include <rte_atomic.h>
#include <rte_table_hash.h>
#include <rte_memzone.h>
#include <rte_errno.h>

#include "prox_malloc.h"
#include "run.h"
#include "main.h"
#include "log.h"
#include "quit.h"
#include "clock.h"
#include "defines.h"
#include "version.h"
#include "prox_args.h"
#include "prox_assert.h"
#include "prox_cfg.h"
#include "prox_shared.h"
#include "prox_port_cfg.h"
#include "toeplitz.h"
#include "hash_utils.h"
#include "handle_lb_net.h"
#include "prox_cksum.h"
#include "thread_nop.h"
#include "thread_generic.h"
#include "thread_pipeline.h"

#if RTE_VERSION < RTE_VERSION_NUM(1,8,0,0)
#define RTE_CACHE_LINE_SIZE CACHE_LINE_SIZE
#endif

uint8_t lb_nb_txrings = 0xff;
struct rte_ring *ctrl_rings[RTE_MAX_LCORE*MAX_TASKS_PER_CORE];

static void __attribute__((noreturn)) prox_usage(const char *prgname)
{
	plog_info("\nUsage: %s [-f CONFIG_FILE] [-a|-e] [-s|-i] [-w DEF] [-u] [-t]\n"
		  "\t-f CONFIG_FILE : configuration file to load, ./prox.cfg by default\n"
		  "\t-l LOG_FILE : log file name, ./prox.log by default\n"
		  "\t-p : include PID in log file name if default log file is used\n"
		  "\t-o DISPLAY: Set display to use, can be 'curses' (default), 'cli' or 'none'\n"
		  "\t-a : autostart all cores (by default)\n"
		  "\t-v verbosity : initial logging verbosity\n"
		  "\t-e : don't autostart\n"
		  "\t-n : Create NULL devices instead of using PCI devices, useful together with -i\n"
		  "\t-s : check configuration file syntax and exit\n"
		  "\t-i : check initialization sequence and exit\n"
		  "\t-u : Listen on UDS /tmp/prox.sock\n"
		  "\t-t : Listen on TCP port 8474\n"
		  "\t-q : Pass argument to Lua interpreter, useful to define variables\n"
		  "\t-w : define variable using syntax varname=value\n"
		  "\t     takes precedence over variables defined in CONFIG_FILE\n"
		  "\t-k : Log statistics to file \"stats_dump\" in current directory\n"
		  "\t-d : Run as daemon, the parent process will block until PROX is not initialized\n"
		  "\t-z : Ignore CPU topology, implies -i\n"
		  "\t-r : Change initial screen refresh rate. If set to a lower than 0.001 seconds,\n"
		  "\t	  screen refreshing will be disabled\n"
		  , prgname);
	exit(EXIT_FAILURE);
}

static void check_mixed_normal_pipeline(void)
{
	struct lcore_cfg *lconf = NULL;
	uint32_t lcore_id = -1;

	while (prox_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];

		int all_thread_nop = 1;
		int generic = 0;
		int pipeline = 0;
		for (uint8_t task_id = 0; task_id < lconf->n_tasks_all; ++task_id) {
			struct task_args *targ = &lconf->targs[task_id];
			all_thread_nop = all_thread_nop &&
				targ->task_init->thread_x == thread_nop;

			pipeline = pipeline || targ->task_init->thread_x == thread_pipeline;
			generic = generic || targ->task_init->thread_x == thread_generic;
		}
		PROX_PANIC(generic && pipeline, "Can't run both pipeline and normal thread on same core\n");

		if (all_thread_nop)
			lconf->thread_x = thread_nop;
		else {
			lconf->thread_x = thread_generic;
		}
	}
}

static void check_missing_rx(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	while (core_targ_next(&lconf, &targ, 0) == 0) {
		PROX_PANIC((targ->flags & TASK_ARG_RX_RING) && targ->rx_rings[0] == 0 && !targ->tx_opt_ring_task,
			   "Configuration Error - Core %u task %u Receiving from ring, but nobody xmitting to this ring\n", lconf->id, targ->id);
		if (targ->nb_rxports == 0 && targ->nb_rxrings == 0) {
			PROX_PANIC(!task_init_flag_set(targ->task_init, TASK_FEATURE_NO_RX),
				   "\tCore %u task %u: no rx_ports and no rx_rings configured while required by mode %s\n", lconf->id, targ->id, targ->task_init->mode_str);
		}
	}
}

static void check_cfg_consistent(void)
{
	check_missing_rx();
	check_mixed_normal_pipeline();
}

static void plog_all_rings(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	while (core_targ_next(&lconf, &targ, 0) == 0) {
		for (uint8_t ring_idx = 0; ring_idx < targ->nb_rxrings; ++ring_idx) {
			plog_info("\tCore %u, task %u, rx_ring[%u] %p\n", lconf->id, targ->id, ring_idx, targ->rx_rings[ring_idx]);
		}
	}
}

static int chain_flag_state(struct task_args *targ, uint32_t flag, int is_set)
{
	if (task_init_flag_set(targ->task_init, flag) == is_set)
		return 1;

	int ret = 0;

	for (uint32_t i = 0; i < targ->n_prev_tasks; ++i) {
		ret = chain_flag_state(targ->prev_tasks[i], flag, is_set);
		if (ret)
			return 1;
	}
	return 0;
}

static void configure_if_tx_queues(struct task_args *targ, uint8_t socket)
{
	uint8_t if_port;

	for (uint8_t i = 0; i < targ->nb_txports; ++i) {
		if_port = targ->tx_port_queue[i].port;

		PROX_PANIC(if_port == OUT_DISCARD, "port misconfigured, exiting\n");

		PROX_PANIC(!prox_port_cfg[if_port].active, "\tPort %u not used, skipping...\n", if_port);

		int dsocket = prox_port_cfg[if_port].socket;
		if (dsocket != -1 && dsocket != socket) {
			plog_warn("TX core on socket %d while device on socket %d\n", socket, dsocket);
		}

		if (prox_port_cfg[if_port].tx_ring[0] == '\0') {  // Rings-backed port can use single queue
			targ->tx_port_queue[i].queue = prox_port_cfg[if_port].n_txq;
			prox_port_cfg[if_port].n_txq++;
		} else {
			prox_port_cfg[if_port].n_txq = 1;
			targ->tx_port_queue[i].queue = 0;
		}
		/* Set the ETH_TXQ_FLAGS_NOREFCOUNT flag if none of
		   the tasks up to the task transmitting to the port
		   does not use refcnt. */
		if (!chain_flag_state(targ, TASK_FEATURE_TXQ_FLAGS_REFCOUNT, 1)) {
			prox_port_cfg[if_port].tx_conf.txq_flags = ETH_TXQ_FLAGS_NOREFCOUNT;
			plog_info("\t\tEnabling No refcnt on port %d\n", if_port);
		}
		else {
			plog_info("\t\tRefcnt used on port %d\n", if_port);
		}

		/* By default OFFLOAD is enabled, but if the whole
		   chain has NOOFFLOADS set all the way until the
		   first task that receives from a port, it will be
		   disabled for the destination port. */
		if (chain_flag_state(targ, TASK_FEATURE_TXQ_FLAGS_NOOFFLOADS, 1)) {
			prox_port_cfg[if_port].tx_conf.txq_flags |= ETH_TXQ_FLAGS_NOOFFLOADS;
			plog_info("\t\tDisabling TX offloads on port %d\n", if_port);
		} else {
			plog_info("\t\tEnabling TX offloads on port %d\n", if_port);
		}

		/* By default NOMULTSEGS is disabled, as drivers/NIC might split packets on RX
		   It should only be enabled when we know for sure that the RX does not split packets.
		   Set the ETH_TXQ_FLAGS_NOMULTSEGS flag if none of the tasks up to the task
		   transmitting to the port does not use multsegs. */
		if (!chain_flag_state(targ, TASK_FEATURE_TXQ_FLAGS_NOMULTSEGS, 0)) {
			prox_port_cfg[if_port].tx_conf.txq_flags |= ETH_TXQ_FLAGS_NOMULTSEGS;
			plog_info("\t\tEnabling No MultiSegs on port %d\n", if_port);
		}
		else {
			plog_info("\t\tMultiSegs used on port %d\n", if_port);
		}
	}
}

static void configure_if_rx_queues(struct task_args *targ, uint8_t socket)
{
	for (int i = 0; i < targ->nb_rxports; i++) {
		uint8_t if_port = targ->rx_port_queue[i].port;

		if (if_port == OUT_DISCARD) {
			return;
		}

		PROX_PANIC(!prox_port_cfg[if_port].active, "Port %u not used, aborting...\n", if_port);

		if(prox_port_cfg[if_port].rx_ring[0] != '\0') {
			prox_port_cfg[if_port].n_rxq = 0;
		}

		targ->rx_port_queue[i].queue = prox_port_cfg[if_port].n_rxq;
		prox_port_cfg[if_port].pool[targ->rx_port_queue[i].queue] = targ->pool;
		prox_port_cfg[if_port].pool_size[targ->rx_port_queue[i].queue] = targ->nb_mbuf - 1;
		prox_port_cfg[if_port].n_rxq++;

		int dsocket = prox_port_cfg[if_port].socket;
		if (dsocket != -1 && dsocket != socket) {
			plog_warn("RX core on socket %d while device on socket %d\n", socket, dsocket);
		}
	}
}

static void configure_if_queues(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;
	uint8_t socket;

	while (core_targ_next(&lconf, &targ, 0) == 0) {
		socket = rte_lcore_to_socket_id(lconf->id);

		configure_if_tx_queues(targ, socket);
		configure_if_rx_queues(targ, socket);
	}
}

static const char *gen_ring_name(void)
{
	static char retval[] = "XX";
	static const char* ring_names =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"[\\]^_`!\"#$%&'()*+,-./:;<="
		">?@{|}0123456789";
	static int idx2 = 0;

	int idx = idx2;

	retval[0] = ring_names[idx % strlen(ring_names)];
	idx /= strlen(ring_names);
	retval[1] = idx ? ring_names[(idx - 1) % strlen(ring_names)] : 0;

	idx2++;

	return retval;
}

static int task_is_master(struct task_args *targ)
{
	return !targ->lconf;
}

struct ring_init_stats {
	uint32_t n_pkt_rings;
	uint32_t n_ctrl_rings;
	uint32_t n_opt_rings;
};

static uint32_t ring_init_stats_total(const struct ring_init_stats *ris)
{
	return ris->n_pkt_rings + ris->n_ctrl_rings + ris->n_opt_rings;
}

static uint32_t count_incoming_tasks(uint32_t lcore_worker, uint32_t dest_task)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;
	uint32_t ret = 0;
	struct core_task ct;

	while (core_targ_next(&lconf, &targ, 0) == 0) {
		for (uint8_t idxx = 0; idxx < MAX_PROTOCOLS; ++idxx) {
			for (uint8_t ridx = 0; ridx < targ->core_task_set[idxx].n_elems; ++ridx) {
				ct = targ->core_task_set[idxx].core_task[ridx];

				if (dest_task == ct.task && lcore_worker == ct.core)
					ret++;
			}
		}
	}
	return ret;
}

static struct rte_ring *get_existing_ring(uint32_t lcore_id, uint32_t task_id)
{
	if (!prox_core_active(lcore_id, 0))
		return NULL;

	struct lcore_cfg *lconf = &lcore_cfg[lcore_id];

	if (task_id >= lconf->n_tasks_all)
		return NULL;

	if (lconf->targs[task_id].nb_rxrings == 0)
		return NULL;

	return lconf->targs[task_id].rx_rings[0];
}

static void init_ring_between_tasks(struct lcore_cfg *lconf, struct task_args *starg,
				    const struct core_task ct, uint8_t ring_idx, int idx,
				    struct ring_init_stats *ris)
{
	uint8_t socket;
	struct rte_ring *ring = NULL;
	struct lcore_cfg *lworker;
	struct task_args *dtarg;

	PROX_ASSERT(prox_core_active(ct.core, 0));
	lworker = &lcore_cfg[ct.core];

	/* socket used is the one that the sending core resides on */
	socket = rte_lcore_to_socket_id(lconf->id);

	plog_info("\t\tCreating ring on socket %u with size %u\n"
		  "\t\t\tsource core, task and socket = %u, %u, %u\n"
		  "\t\t\tdestination core, task and socket = %u, %u, %u\n"
		  "\t\t\tdestination worker id = %u\n",
		  socket, starg->ring_size,
		  lconf->id, starg->id, socket,
		  ct.core, ct.task, rte_lcore_to_socket_id(ct.core),
		  ring_idx);

	if (ct.type) {
		struct rte_ring **dring = NULL;

		if (ct.type == CTRL_TYPE_MSG)
			dring = &lworker->ctrl_rings_m[ct.task];
		else if (ct.type == CTRL_TYPE_PKT) {
			dring = &lworker->ctrl_rings_p[ct.task];
			starg->flags |= TASK_ARG_CTRL_RINGS_P;
		}

		if (*dring == NULL)
			ring = rte_ring_create(gen_ring_name(), starg->ring_size, socket, RING_F_SC_DEQ);
		else
			ring = *dring;
		PROX_PANIC(ring == NULL, "Cannot create ring to connect I/O core %u with worker core %u\n", lconf->id, ct.core);

		starg->tx_rings[starg->tot_n_txrings_inited] = ring;
		starg->tot_n_txrings_inited++;
		*dring = ring;
		if (lconf->id == prox_cfg.master) {
			ctrl_rings[ct.core*MAX_TASKS_PER_CORE + ct.task] = ring;
		}

		plog_info("\t\tCore %u task %u to -> core %u task %u ctrl_ring %s %p %s\n",
			  lconf->id, starg->id, ct.core, ct.task, ct.type == CTRL_TYPE_PKT?
			  "pkt" : "msg", ring, ring->name);
		ris->n_ctrl_rings++;
		return;
	}

	dtarg = &lworker->targs[ct.task];
	lworker->targs[ct.task].worker_thread_id = ring_idx;
	PROX_ASSERT(dtarg->flags & TASK_ARG_RX_RING);
	PROX_ASSERT(ct.task < lworker->n_tasks_all);

	/* If all the following conditions are met, the ring can be
	   optimized away. */
	if (!task_is_master(starg) && starg->lconf->id == dtarg->lconf->id &&
	    starg->nb_txrings == 1 && idx == 0 && dtarg->task &&
	    dtarg->tot_rxrings == 1 && starg->task == dtarg->task - 1) {
		plog_info("\t\tOptimizing away ring on core %u from task %u to task %u\n",
			  dtarg->lconf->id, starg->task, dtarg->task);
		/* No need to set up ws_mbuf. */
		starg->tx_opt_ring = 1;
		/* During init of destination task, the buffer in the
		   source task will be initialized. */
		dtarg->tx_opt_ring_task = starg;
		ris->n_opt_rings++;
		++dtarg->nb_rxrings;
		return;
	}

	int ring_created = 1;
	/* Only create multi-producer rings if configured to do so AND
	   there is only one task sending to the task */
	if (prox_cfg.flags & DSF_MP_RINGS && count_incoming_tasks(ct.core, ct.task) > 1) {
		ring = get_existing_ring(ct.core, ct.task);

		if (ring) {
			plog_info("\t\tCore %u task %u creatign MP ring %p to core %u task %u\n",
				  lconf->id, starg->id, ring, ct.core, ct.task);
			ring_created = 0;
		}
		else {
			ring = rte_ring_create(gen_ring_name(), starg->ring_size, socket, RING_F_SC_DEQ);
			plog_info("\t\tCore %u task %u using MP ring %p from core %u task %u\n",
				  lconf->id, starg->id, ring, ct.core, ct.task);
		}
	}
	else
		ring = rte_ring_create(gen_ring_name(), starg->ring_size, socket, RING_F_SP_ENQ | RING_F_SC_DEQ);

	PROX_PANIC(ring == NULL, "Cannot create ring to connect I/O core %u with worker core %u\n", lconf->id, ct.core);

	starg->tx_rings[starg->tot_n_txrings_inited] = ring;
	starg->tot_n_txrings_inited++;

	if (ring_created) {
		PROX_ASSERT(dtarg->nb_rxrings < MAX_RINGS_PER_TASK);
		dtarg->rx_rings[dtarg->nb_rxrings] = ring;
		++dtarg->nb_rxrings;
	}
	dtarg->nb_slave_threads = starg->core_task_set[idx].n_elems;
	dtarg->lb_friend_core = lconf->id;
	dtarg->lb_friend_task = starg->id;
	plog_info("\t\tWorker thread %d has core %d, task %d as a lb friend\n", ct.core, lconf->id, starg->id);
	plog_info("\t\tCore %u task %u tx_ring[%u] -> core %u task %u rx_ring[%u] %p %s %u WT\n",
		  lconf->id, starg->id, ring_idx, ct.core, ct.task, dtarg->nb_rxrings, ring, ring->name,
		  dtarg->nb_slave_threads);
	++ris->n_pkt_rings;
}

static void init_rings(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *starg;
	struct ring_init_stats ris = {0};

	while (core_targ_next(&lconf, &starg, 1) == 0) {
		plog_info("\t*** Initializing rings on core %u, task %u ***\n", lconf->id, starg->id);
		for (uint8_t idx = 0; idx < MAX_PROTOCOLS; ++idx) {
			for (uint8_t ring_idx = 0; ring_idx < starg->core_task_set[idx].n_elems; ++ring_idx) {
				PROX_ASSERT(ring_idx < MAX_WT_PER_LB);
				PROX_ASSERT(starg->tot_n_txrings_inited < MAX_RINGS_PER_TASK);

				struct core_task ct = starg->core_task_set[idx].core_task[ring_idx];
				init_ring_between_tasks(lconf, starg, ct, ring_idx, idx, &ris);
			}
		}
	}

	plog_info("\tInitialized %d rings:\n"
		  "\t\tNumber of packet rings: %u\n"
		  "\t\tNumber of control rings: %u\n"
		  "\t\tNumber of optimized rings: %u\n",
		  ring_init_stats_total(&ris),
		  ris.n_pkt_rings,
		  ris.n_ctrl_rings,
		  ris.n_opt_rings);
}

static void shuffle_mempool(struct rte_mempool* mempool, uint32_t nb_mbuf)
{
	struct rte_mbuf** pkts = prox_zmalloc(nb_mbuf * sizeof(*pkts), rte_socket_id());
	uint64_t got = 0;

	while (rte_mempool_get_bulk(mempool, (void**)(pkts + got), 1) == 0)
		++got;

	while (got) {
		int idx;
		do {
			idx = rand() % nb_mbuf - 1;
		} while (pkts[idx] == 0);

		rte_mempool_put_bulk(mempool, (void**)&pkts[idx], 1);
		pkts[idx] = 0;
		--got;
	};
	prox_free(pkts);
}

static void setup_mempools_unique_per_socket(void)
{
	uint32_t flags = 0;
	char name[64];
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	struct rte_mempool     *pool[MAX_SOCKETS];
	uint32_t mbuf_count[MAX_SOCKETS] = {0};
	uint32_t nb_cache_mbuf[MAX_SOCKETS] = {0};
	uint32_t mbuf_size[MAX_SOCKETS] = {0};

	while (core_targ_next_early(&lconf, &targ, 0) == 0) {
		uint8_t socket = rte_lcore_to_socket_id(lconf->id);
		PROX_ASSERT(socket < MAX_SOCKETS);

		if (targ->mbuf_size_set_explicitely)
			flags = MEMPOOL_F_NO_SPREAD;
		if ((!targ->mbuf_size_set_explicitely) && (targ->task_init->mbuf_size != 0)) {
			targ->mbuf_size = targ->task_init->mbuf_size;
		}
		if (targ->rx_port_queue[0].port != OUT_DISCARD) {
			struct prox_port_cfg* port_cfg = &prox_port_cfg[targ->rx_port_queue[0].port];
			PROX_ASSERT(targ->nb_mbuf != 0);
			mbuf_count[socket] += targ->nb_mbuf;
			if (nb_cache_mbuf[socket] == 0)
				nb_cache_mbuf[socket] = targ->nb_cache_mbuf;
			else {
				PROX_PANIC(nb_cache_mbuf[socket] != targ->nb_cache_mbuf,
					   "all mbuf_cache must have the same size if using a unique mempool per socket\n");
			}
			if (mbuf_size[socket] == 0)
				mbuf_size[socket] = targ->mbuf_size;
			else {
				PROX_PANIC(mbuf_size[socket] != targ->mbuf_size,
					   "all mbuf_size must have the same size if using a unique mempool per socket\n");
			}
			if ((!targ->mbuf_size_set_explicitely) && (strcmp(port_cfg->driver_name, "rte_vmxnet3_pmd") == 0)) {
				if (mbuf_size[socket] < MBUF_SIZE + RTE_PKTMBUF_HEADROOM)
					mbuf_size[socket] = MBUF_SIZE + RTE_PKTMBUF_HEADROOM;
			}
		}
	}
	for (int i = 0 ; i < MAX_SOCKETS; i++) {
		if (mbuf_count[i] != 0) {
			sprintf(name, "socket_%u_pool", i);
			pool[i] = rte_mempool_create(name,
						     mbuf_count[i] - 1, mbuf_size[i],
						     nb_cache_mbuf[i],
						     sizeof(struct rte_pktmbuf_pool_private),
						     rte_pktmbuf_pool_init, NULL,
						     prox_pktmbuf_init, NULL,
						     i, flags);
			PROX_PANIC(pool[i] == NULL, "\t\tError: cannot create mempool for socket %u\n", i);
			plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", pool[i],
				  mbuf_count[i], mbuf_size[i], nb_cache_mbuf[i], i);

			if (prox_cfg.flags & DSF_SHUFFLE) {
				shuffle_mempool(pool[i], mbuf_count[i]);
			}
		}
	}

	lconf = NULL;
	while (core_targ_next_early(&lconf, &targ, 0) == 0) {
		uint8_t socket = rte_lcore_to_socket_id(lconf->id);

		if (targ->rx_port_queue[0].port != OUT_DISCARD) {
			/* use this pool for the interface that the core is receiving from */
			/* If one core receives from multiple ports, all the ports use the same mempool */
			targ->pool = pool[socket];
			/* Set the number of mbuf to the number of the unique mempool, so that the used and free work */
			targ->nb_mbuf = mbuf_count[socket];
			plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", targ->pool,
				  targ->nb_mbuf, mbuf_size[socket], targ->nb_cache_mbuf, socket);
		}
	}
}

static void setup_mempool_for_rx_task(struct lcore_cfg *lconf, struct task_args *targ)
{
	const uint8_t socket = rte_lcore_to_socket_id(lconf->id);
	struct prox_port_cfg *port_cfg = &prox_port_cfg[targ->rx_port_queue[0].port];
	const struct rte_memzone *mz;
	struct rte_mempool *mp = NULL;
	uint32_t flags = 0;
	char memzone_name[64];
	char name[64];

	/* mbuf size can be set
	 *  - from config file (highest priority, overwriting any other config) - should only be used as workaround
	 *  - through each 'mode', overwriting the default mbuf_size
	 *  - defaulted to MBUF_SIZE i.e. 1518 Bytes
	 * Except is set expliciteky, ensure that size is big enough for vmxnet3 driver
	 */
	if (targ->mbuf_size_set_explicitely) {
		flags = MEMPOOL_F_NO_SPREAD;
		/* targ->mbuf_size already set */
	}
	else if (targ->task_init->mbuf_size != 0) {
		/* mbuf_size not set through config file but set through mode */
		targ->mbuf_size = targ->task_init->mbuf_size;
	}
	else if (strcmp(port_cfg->driver_name, "rte_vmxnet3_pmd") == 0) {
		if (targ->mbuf_size < MBUF_SIZE + RTE_PKTMBUF_HEADROOM)
			targ->mbuf_size = MBUF_SIZE + RTE_PKTMBUF_HEADROOM;
	}

	/* allocate memory pool for packets */
	PROX_ASSERT(targ->nb_mbuf != 0);

	if (targ->pool_name[0] == '\0') {
		sprintf(name, "core_%u_port_%u_pool", lconf->id, targ->id);
	}

	snprintf(memzone_name, sizeof(memzone_name)-1, "MP_%s", targ->pool_name);
	mz = rte_memzone_lookup(memzone_name);

	if (mz != NULL) {
		mp = (struct rte_mempool*)mz->addr;

		targ->nb_mbuf = mp->size;
		targ->pool = mp;
	}

#ifdef RTE_LIBRTE_IVSHMEM_FALSE
	if (mz != NULL && mp != NULL && mp->phys_addr != mz->ioremap_addr) {
		/* Init mbufs with ioremap_addr for dma */
		mp->phys_addr = mz->ioremap_addr;
		mp->elt_pa[0] = mp->phys_addr + (mp->elt_va_start - (uintptr_t)mp);

		struct prox_pktmbuf_reinit_args init_args;
		init_args.mp = mp;
		init_args.lconf = lconf;

		uint32_t elt_sz = mp->elt_size + mp->header_size + mp->trailer_size;
		rte_mempool_obj_iter((void*)mp->elt_va_start, mp->size, elt_sz, 1,
				     mp->elt_pa, mp->pg_num, mp->pg_shift, prox_pktmbuf_reinit, &init_args);
	}
#endif

	/* Use this pool for the interface that the core is
	   receiving from if one core receives from multiple
	   ports, all the ports use the same mempool */
	if (targ->pool == NULL) {
		plog_info("\t\tCreating mempool with name '%s'\n", name);
		targ->pool = rte_mempool_create(name,
						targ->nb_mbuf - 1, targ->mbuf_size,
						targ->nb_cache_mbuf,
						sizeof(struct rte_pktmbuf_pool_private),
						rte_pktmbuf_pool_init, NULL,
						prox_pktmbuf_init, lconf,
						socket, flags);
	}

	PROX_PANIC(targ->pool == NULL,
		   "\t\tError: cannot create mempool for core %u port %u: %s\n", lconf->id, targ->id, rte_strerror(rte_errno));

	plog_info("\t\tMempool %p size = %u * %u cache %u, socket %d\n", targ->pool,
		  targ->nb_mbuf, targ->mbuf_size, targ->nb_cache_mbuf, socket);
	if (prox_cfg.flags & DSF_SHUFFLE) {
		shuffle_mempool(targ->pool, targ->nb_mbuf);
	}
}

static void setup_mempools_multiple_per_socket(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	while (core_targ_next_early(&lconf, &targ, 0) == 0) {
		if (targ->rx_port_queue[0].port == OUT_DISCARD)
			continue;
		setup_mempool_for_rx_task(lconf, targ);
	}
}

static void setup_mempools(void)
{
	if (prox_cfg.flags & UNIQUE_MEMPOOL_PER_SOCKET)
		setup_mempools_unique_per_socket();
	else
		setup_mempools_multiple_per_socket();
}

static void set_task_lconf(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	while(prox_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->n_tasks_all; ++task_id) {
			lconf->targs[task_id].lconf = lconf;
		}
	}
}

static void set_dest_threads(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	while (core_targ_next(&lconf, &targ, 0) == 0) {
		for (uint8_t idx = 0; idx < MAX_PROTOCOLS; ++idx) {
			for (uint8_t ring_idx = 0; ring_idx < targ->core_task_set[idx].n_elems; ++ring_idx) {
				struct core_task ct = targ->core_task_set[idx].core_task[ring_idx];

				struct task_args *dest_task = core_targ_get(ct.core, ct.task);
				dest_task->prev_tasks[dest_task->n_prev_tasks++] = targ;
			}
		}
	}
}

static void setup_all_task_structs_early_init(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;

	plog_info("\t*** Calling early init on all tasks ***\n");
	while (core_targ_next(&lconf, &targ, 0) == 0) {
		if (targ->task_init->early_init) {
			targ->task_init->early_init(targ);
		}
	}
}

static void setup_all_task_structs(void)
{
	struct lcore_cfg *lconf;
	uint32_t lcore_id = -1;

	while(prox_core_next(&lcore_id, 0) == 0) {
		lconf = &lcore_cfg[lcore_id];
		for (uint8_t task_id = 0; task_id < lconf->n_tasks_all; ++task_id) {
			lconf->tasks_all[task_id] = init_task_struct(&lconf->targs[task_id]);
		}
	}
}

static void init_port_activate(void)
{
	struct lcore_cfg *lconf = NULL;
	struct task_args *targ;
	uint8_t port_id = 0;

	while (core_targ_next_early(&lconf, &targ, 0) == 0) {
		printf("%p %p\n", lconf, targ);
		for (int i = 0; i < targ->nb_rxports; i++) {
			port_id = targ->rx_port_queue[i].port;
			prox_port_cfg[port_id].active = 1;
		}

		for (int i = 0; i < targ->nb_txports; i++) {
			port_id = targ->tx_port_queue[i].port;
			prox_port_cfg[port_id].active = 1;
		}
	}
}

/* Initialize cores and allocate mempools */
static void init_lcores(void)
{
	struct lcore_cfg *lconf = 0;
	uint32_t lcore_id = -1;

	while(prox_core_next(&lcore_id, 0) == 0) {
		uint8_t socket = rte_lcore_to_socket_id(lcore_id);
		PROX_PANIC(socket + 1 > MAX_SOCKETS, "Can't configure core %u (on socket %u). MAX_SOCKET is set to %d\n", lcore_id, socket, MAX_SOCKETS);
	}

	/* need to allocate mempools as the first thing to use the lowest possible address range */
	plog_info("=== Initializing mempools ===\n");
	setup_mempools();

	lcore_cfg_alloc_hp();

	set_dest_threads();
	set_task_lconf();

	plog_info("=== Initializing port addresses ===\n");
	init_port_addr();

	plog_info("=== Initializing queue numbers on cores ===\n");
	configure_if_queues();

	plog_info("=== Initializing rings on cores ===\n");
	init_rings();

	plog_info("=== Checking configuration consistency ===\n");
	check_cfg_consistent();

	plog_all_rings();

	setup_all_task_structs_early_init();
	plog_info("=== Initializing tasks ===\n");
	setup_all_task_structs();
}

static int setup_prox(int argc, char **argv)
{
	if (prox_read_config_file() != 0 ||
	    prox_setup_rte(argv[0]) != 0) {
		return -1;
	}

	if (prox_cfg.flags & DSF_CHECK_SYNTAX) {
		plog_info("=== Configuration file syntax has been checked ===\n\n");
		exit(EXIT_SUCCESS);
	}

	init_port_activate();
	plog_info("=== Initializing rte devices ===\n");
	if (!(prox_cfg.flags & DSF_USE_DUMMY_DEVICES))
		init_rte_ring_dev();
	init_rte_dev(prox_cfg.flags & DSF_USE_DUMMY_DEVICES);
	plog_info("=== Calibrating TSC overhead ===\n");
	clock_init();
	plog_info("\tTSC running at %"PRIu64" Hz\n", rte_get_tsc_hz());

	init_lcores();
	plog_info("=== Initializing ports ===\n");
	init_port_all();

	if (prox_cfg.logbuf_size) {
		prox_cfg.logbuf = prox_zmalloc(prox_cfg.logbuf_size, rte_socket_id());
		PROX_PANIC(prox_cfg.logbuf == NULL, "Failed to allocate memory for logbuf with size = %d\n", prox_cfg.logbuf_size);
	}

	if (prox_cfg.flags & DSF_CHECK_INIT) {
		plog_info("=== Initialization sequence completed ===\n\n");
		exit(EXIT_SUCCESS);
	}

	/* Current way that works to disable DPDK logging */
	FILE *f = fopen("/dev/null", "r");
	rte_openlog_stream(f);
	plog_info("=== PROX started ===\n");
	return 0;
}

static int success = 0;
static void siguser_handler(int signal)
{
	if (signal == SIGUSR1)
		success = 1;
	else
		success = 0;
}

int main(int argc, char **argv)
{
	/* set en_US locale to print big numbers with ',' */
	setlocale(LC_NUMERIC, "en_US.utf-8");

	if (prox_parse_args(argc, argv) != 0){
		prox_usage(argv[0]);
	}

	plog_init(prox_cfg.log_name, prox_cfg.log_name_pid);
	plog_info("=== " PROGRAM_NAME " " VERSION_STR " ===\n");
	plog_info("\tUsing DPDK %s\n", rte_version() + sizeof(RTE_VER_PREFIX));

	if (prox_cfg.flags & DSF_DAEMON) {
		signal(SIGUSR1, siguser_handler);
		signal(SIGUSR2, siguser_handler);
		plog_info("=== Running in Daemon mode ===\n");
		plog_info("\tForking child and waiting for setup completion\n");

		pid_t ppid = getpid();
		pid_t pid = fork();
		if (pid < 0) {
			plog_err("Failed to fork process to run in daemon mode\n");
			return EXIT_FAILURE;
		}

		if (pid == 0) {
			fclose(stdin);
			fclose(stdout);
			fclose(stderr);
			if (setsid() < 0) {
				kill(ppid, SIGUSR2);
				return EXIT_FAILURE;
			}
			if (setup_prox(argc, argv) != 0) {
				kill(ppid, SIGUSR2);
				return EXIT_FAILURE;
			}
			else {
				kill(ppid, SIGUSR1);
				run(prox_cfg.flags);
				return EXIT_SUCCESS;
			}
		}
		else {
			/* Before exiting the parent, wait until the
			   child process has finished setting up */
			pause();
			if (prox_cfg.logbuf) {
				file_print(prox_cfg.logbuf);
			}
			return success? EXIT_SUCCESS : EXIT_FAILURE;
		}
	}

	if (setup_prox(argc, argv) != 0)
		return EXIT_FAILURE;
	run(prox_cfg.flags);
	return EXIT_SUCCESS;
}
