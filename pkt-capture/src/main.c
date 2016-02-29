/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <getopt.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_malloc.h>
#include <rte_debug.h>
#include <rte_prefetch.h>
#include <rte_distributor.h>

#define RX_RING_SIZE 256
#define TX_RING_SIZE 512
#define NUM_MBUFS ((64*1024)-1)
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32
#define RTE_RING_SZ 1024

// uncommnet below line to enable debug logs
#define DEBUG

// logging setup
#ifdef DEBUG
#define LOG_LEVEL RTE_LOG_DEBUG
#define LOG_DEBUG(log_type, fmt, args...) do {	\
	RTE_LOG(DEBUG, log_type, fmt, ##args);		\
} while (0)
#else
#define LOG_LEVEL RTE_LOG_INFO
#define LOG_DEBUG(log_type, fmt, args...) do {} while (0)
#endif

#define RTE_LOGTYPE_DISTRAPP RTE_LOGTYPE_USER1

// mask of enabled ports
static uint32_t enabled_port_mask;
volatile uint8_t quit_signal;
volatile uint8_t quit_signal_rx;

/*
 * tracks packet processing stats
 */
static volatile struct app_stats {
	struct {
		uint64_t rx_pkts;
		uint64_t returned_pkts;
		uint64_t enqueued_pkts;
	} rx __rte_cache_aligned;
} app_stats;

static const struct rte_eth_conf port_conf_default = {
	.rxmode = {
		.mq_mode = ETH_MQ_RX_RSS,
		.max_rx_pkt_len = ETHER_MAX_LEN,
	},
	.txmode = {
		.mq_mode = ETH_MQ_TX_NONE,
	},
	.rx_adv_conf = {
		.rss_conf = {
			.rss_hf = ETH_RSS_IP | ETH_RSS_UDP | ETH_RSS_TCP | ETH_RSS_SCTP,
		}
	},
};

/*
 * Initialize a port using global settings and with the rx buffers
 * coming from the mbuf_pool passed as parameter
 */
static inline int port_init(uint8_t port, struct rte_mempool *mbuf_pool)
{
	struct rte_eth_conf port_conf = port_conf_default;
	int retval;
	uint16_t q;
  const uint16_t rxRings = 1;
  const uint16_t txRings = 1;

	if (port >= rte_eth_dev_count()) {
		printf("Error: Port %"PRIu8" does not exist; only %u known port(s)\n",
			port, rte_eth_dev_count());
    return -1;
  }

	retval = rte_eth_dev_configure(port, rxRings, txRings, &port_conf);
	if (retval != 0) {
		printf("Error: Unable to configure port %"PRIu8"\n", port);
		return retval;
  }

  // setup the receive rings
	for (q = 0; q < rxRings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, RX_RING_SIZE,
						  rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0) {
			return retval;
    }
	}

	// setup the transmit rings
	for (q = 0; q < txRings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, TX_RING_SIZE,
			rte_eth_dev_socket_id(port), NULL);

		if (retval < 0) {
			return retval;
		}
}

  // start the receive and transmit units on the device
	retval = rte_eth_dev_start(port);
	if (retval < 0) {
		return retval;
  }

  // retrieve information about the device
	struct rte_eth_link link;
	rte_eth_link_get_nowait(port, &link);
	if (!link.link_status) {
		sleep(1);
		rte_eth_link_get_nowait(port, &link);
	}

  // if still no link information, must be down
	if (!link.link_status) {
		printf("Error: Link down on port %"PRIu8"\n", port);
		return 0;
	}

  // print diagnostics
	struct ether_addr addr;
	rte_eth_macaddr_get(port, &addr);
	printf("Port %u MAC: %02"PRIx8" %02"PRIx8" %02"PRIx8
			" %02"PRIx8" %02"PRIx8" %02"PRIx8"\n",
			(unsigned)port,
			addr.addr_bytes[0], addr.addr_bytes[1],
			addr.addr_bytes[2], addr.addr_bytes[3],
			addr.addr_bytes[4], addr.addr_bytes[5]);

  // enable promisc mode
	rte_eth_promiscuous_enable(port);
	return 0;
}

struct lcore_params {
	unsigned worker_id;
	unsigned num_workers;
	struct rte_distributor *d;
	struct rte_mempool *mem_pool;
};

static void quit_workers(struct rte_distributor *d, struct rte_mempool *p, unsigned num_workers)
{
	unsigned i;
	struct rte_mbuf *bufs[num_workers];
	rte_mempool_get_bulk(p, (void *)bufs, num_workers);

	for (i = 0; i < num_workers; i++) {
		bufs[i]->hash.rss = i << 1;
  }

	rte_distributor_process(d, bufs, num_workers);
	rte_mempool_put_bulk(p, (void *)bufs, num_workers);
}

/**
 * Master distribution logic that receives a packet and distributes it to a
 * worker.
 */
static int receive_packets(struct lcore_params *p)
{
	struct rte_distributor *d = p->d;
	struct rte_mempool *mem_pool = p->mem_pool;
	const uint8_t nb_ports = rte_eth_dev_count();
	const int socket_id = rte_socket_id();
	uint8_t port;

  // check for cross-socket communication
  for (port = 0; port < nb_ports; port++) {

    // skip ports that are not enabled
		if ((enabled_port_mask & (1 << port)) == 0) {
      continue;
    }

    if (rte_eth_dev_socket_id(port) > 0 && rte_eth_dev_socket_id(port) != socket_id) {
      printf("Warning: Port %u on different socket from thread; performance will suffer\n", port);
    }
	}

	printf("\nCore %u doing packet RX.\n", rte_lcore_id());
	port = 0;
	while (!quit_signal_rx) {

		// skip to the next enabled port
		if ((enabled_port_mask & (1 << port)) == 0) {
			if (++port == nb_ports) {
        port = 0;
      }
			continue;
		}

    // receive a 'burst' of many packets
		struct rte_mbuf *bufs[BURST_SIZE*2];
		const uint16_t nb_rx = rte_eth_rx_burst(port, 0, bufs, BURST_SIZE);
		app_stats.rx.rx_pkts += nb_rx;

    // distribute the packets amongst all workers
		rte_distributor_process(d, bufs, nb_rx);

    // track packets completed by the workers
		const uint16_t nb_ret = rte_distributor_returned_pkts(d, bufs, BURST_SIZE*2);
		app_stats.rx.returned_pkts += nb_ret;
		if (unlikely(nb_ret == 0)) {
			continue;
    }

    // wrap-around to the first port
		if (++port == nb_ports) {
      port = 0;
    }
	}

  // flush distributor process
	rte_distributor_process(d, NULL, 0);
	rte_distributor_flush(d);

  // notify workers that it is quitting time
	quit_signal = 1;
	quit_workers(d, mem_pool, p->num_workers);

	return 0;
}

/*
 * Send packets to a Kafka broker.
 */
static int send_packets(struct lcore_params *p) {
	struct rte_distributor *d = p->d;
	const unsigned id = p->worker_id;
	struct rte_mbuf *buf = NULL;

	printf("Notice: Core %u is a worker core.\n", rte_lcore_id());
	while (!quit_signal) {
		buf = rte_distributor_get_pkt(d, id, buf);

    // TODO: do work?? send to kafka?
    printf("Received packet: worker = %u, core = %u, pkt_len = %u, data_len = %u \n",
			id, rte_lcore_id(), buf->pkt_len, buf->data_len);

	}
	return 0;
}

static void print_stats(void)
{
	struct rte_eth_stats eth_stats;
	unsigned i;

	printf("\nThread stats:\n");
	printf(" - Received:    %"PRIu64"\n", app_stats.rx.rx_pkts);
	printf(" - Processed:   %"PRIu64"\n", app_stats.rx.returned_pkts);
	printf(" - Enqueued:    %"PRIu64"\n", app_stats.rx.enqueued_pkts);

	for (i = 0; i < rte_eth_dev_count(); i++) {
		rte_eth_stats_get(i, &eth_stats);
		printf("\nPort %u stats:\n", i);
		printf(" - Pkts in:   %"PRIu64"\n", eth_stats.ipackets);
		printf(" - Pkts out:  %"PRIu64"\n", eth_stats.opackets);
		printf(" - In Errs:   %"PRIu64"\n", eth_stats.ierrors);
		printf(" - Out Errs:  %"PRIu64"\n", eth_stats.oerrors);
		printf(" - Mbuf Errs: %"PRIu64"\n", eth_stats.rx_nombuf);
	}
}

/*
 * Print usage information to the user.
 */
static void print_usage(const char *prgname)
{
	printf("%s [EAL options] -- -p PORTMASK\n"
			"  -p PORTMASK: hexadecimal bitmask of ports to configure\n",
			prgname);
}

/*
 * Parse the 'portmask' command line argument.
 */
static int parse_portmask(const char *portmask)
{
	char *end = NULL;
	unsigned long pm;

	// parse hexadecimal string
	pm = strtoul(portmask, &end, 16);

	if ((portmask[0] == '\0') || (end == NULL) || (*end != '\0')) {
    return -1;
  } else if (pm == 0) {
    return -1;
  } else {
    return pm;
  }
}

/**
 * Parse the command line arguments passed to the application.
 */
static int parse_args(int argc, char **argv)
{
	int opt;
	char **argvopt;
	int option_index;
	char *prgname = argv[0];
	static struct option lgopts[] = {
		{ NULL, 0, 0, 0 }
	};

  // parse arguments to this application
	argvopt = argv;
	while ((opt = getopt_long(argc, argvopt, "p:", lgopts, &option_index)) != EOF) {
		switch (opt) {

		// portmask
		case 'p':
			enabled_port_mask = parse_portmask(optarg);
			if (enabled_port_mask == 0) {
				printf("Error: Invalid portmask: '%s'\n", optarg);
				print_usage(prgname);
				return -1;
			}
			break;

		default:
      printf("Error: Invalid argument: '%s'\n", optarg);
			print_usage(prgname);
			return -1;
		}
	}

	if (optind <= 1) {
		print_usage(prgname);
		return -1;
	}

	argv[optind-1] = prgname;

  // reset getopt lib
	optind = 0;
	return 0;
}

/*
 * Handles interrupt signals.
 */
static void sig_handler(int sig_num)
{
	printf("Notice: Exiting on signal '%d'\n", sig_num);

  // set quit flag for rx thread to exit
	quit_signal_rx = 1;
}

/**
 * Get it going.
 */
int main(int argc, char *argv[])
{
  unsigned lcore_id;
  unsigned nb_ports;
  unsigned worker_id = 0;
  unsigned nb_workers;
  uint8_t port_id;
  uint8_t nb_ports_available;

	struct rte_mempool *mbuf_pool;
	struct rte_distributor *d;

  // catch interrupt
	signal(SIGINT, sig_handler);

	// initialize the environment
	int ret = rte_eal_init(argc, argv);
	if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error: Problem during initialization: %i\n", ret);
  }

  // advance past the environmental settings
	argc -= ret;
	argv += ret;

	// parse arguments to the application
	ret = parse_args(argc, argv);
	if (ret < 0) {
    rte_exit(EXIT_FAILURE, "Error: Invalid distributor parameters\n");
  }

  // check number of ethernet devices
	nb_ports = rte_eth_dev_count();
	if (nb_ports == 0) {
		rte_exit(EXIT_FAILURE, "Error: No ethernet ports detected\n");
  }

  // check number of available logical cores for workers
  nb_workers = rte_lcore_count() - 1;
	if (nb_workers < 1) {
		rte_exit(EXIT_FAILURE, "Error: Minimum 2 logical cores required. \n");
  }

  // create memory pool
	mbuf_pool = rte_pktmbuf_pool_create("mbuf-pool", NUM_MBUFS * nb_ports,
    MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	if (mbuf_pool == NULL) {
		rte_exit(EXIT_FAILURE, "Error: Cannot create memory pool for packets\n");
  }

  // initialize each specified ethernet ports
	nb_ports_available = nb_ports;
	for (port_id = 0; port_id < nb_ports; port_id++) {

		// skip over ports that are not enabled
		if ((enabled_port_mask & (1 << port_id)) == 0) {
			printf("Warning: Skipping over disabled port '%d'\n", port_id);
			nb_ports_available--;
			continue;
		}

		// initialize the port
    printf("Notice: Initializing port %u\n", (unsigned) port_id);
		if (port_init(port_id, mbuf_pool) != 0) {
			rte_exit(EXIT_FAILURE, "Cannot initialize port %"PRIu8"\n", port_id);
    }
	}

  // ensure that we were able to initialize enough ports
	if (!nb_ports_available) {
		rte_exit(EXIT_FAILURE, "Error: No available enabled ports. Portmask set?\n");
	}

  // the distributor will dispatch packets to 1 or more workers
	d = rte_distributor_create("master", rte_socket_id(), nb_workers);
	if (d == NULL) {
    rte_exit(EXIT_FAILURE, "Error: Unable to create distributor\n");
  }

  // launch workers on each logical core
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		struct lcore_params *p = rte_malloc(NULL, sizeof(*p), 0);
    if (!p) {
      rte_panic("Error: rte_malloc failure\n");
    }

    // launch the worker process
		printf("Notice: Launching worker on core %u\n", lcore_id);
		*p = (struct lcore_params){ worker_id, nb_workers, d, mbuf_pool };
		rte_eal_remote_launch((lcore_function_t *)send_packets, p, lcore_id);

		worker_id++;
	}

	// start distributing packets on the master
	struct lcore_params p = { 0, nb_workers, d, mbuf_pool };
	receive_packets(&p);

  // wait for each worker to complete
	RTE_LCORE_FOREACH_SLAVE(lcore_id) {
		if (rte_eal_wait_lcore(lcore_id) < 0) {
      return -1;
    }
	}

	print_stats();
	return 0;
}
