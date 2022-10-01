/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <spdk/env.h>
#include <spdk/log.h>
#include <spdk/nvme.h>
#include <spdk/stdinc.h>
#include <spdk/string.h>
#include <spdk/vmd.h>

// Lab2
#include <stdint.h>
#include <inttypes.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

//LAB1
#include <unistd.h>
#include <time.h>

#define RX_RING_SIZE 1024
#define TX_RING_SIZE 1024

#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define LAB2_PORT_ID 2

/* The storage request is either reading a sector or writing a sector. */
enum opcode { READ = 0, WRITE };

struct request_packet {
        /* Request fields. */
        enum opcode op;
        uint64_t lba;         /* The LBA of the request. */
        uint8_t *req_data;    /* The request data (valid for write requests). */
};

struct response_packet {
        /* Request fields. */
        enum opcode op;
        uint64_t lba;         /* The LBA of the request. */
        uint8_t *req_data;    /* The request data (valid for write requests). */
};

struct req_context {
        /* Request metadata. */
        struct rte_ether_hdr *ether_hdr; /* The ether frame header of the request containing source and destination MAC addresses. */
        bool is_success;

        /* Request fields. */
        enum opcode op;
        uint64_t lba;         /* The LBA of the request. */
        uint8_t *req_data;    /* The request data (valid for write requests). */

        /* Response fields. */
        int rc;               /* The return code. */
        uint8_t *resp_data;   /* The response data (valid for read requests). */
};

static struct spdk_nvme_ctrlr *selected_ctrlr;
static struct spdk_nvme_ns *selected_ns;
/* PUT YOUR CODE HERE */
struct callback_args {
	volatile bool done;
        struct req_context *req_ctx;
	char *buf;
};

static struct spdk_nvme_qpair *qpair;
static struct callback_args cb_args;

struct rte_mempool *mbuf_pool;
struct rte_mbuf *bufs[BURST_SIZE];

//LAB 2
/*
 * Initializes a given port using global settings and with the RX buffers
 * coming from the mbuf_pool passed as a parameter.
 */

/* Main functional part of port initialization. 8< */
static inline int
port_init(uint16_t port, struct rte_mempool *mbuf_pool) {
	struct rte_eth_conf port_conf;
	const uint16_t rx_rings = 1, tx_rings = 1;
	uint16_t nb_rxd = RX_RING_SIZE;
	uint16_t nb_txd = TX_RING_SIZE;
	int retval;
	uint16_t q;
	struct rte_eth_dev_info dev_info;
	struct rte_eth_txconf txconf;

	if (!rte_eth_dev_is_valid_port(port))
		return -1;

        // LAB1: Only use port1
	if (port != LAB2_PORT_ID) return -1;

	memset(&port_conf, 0, sizeof(struct rte_eth_conf));

	retval = rte_eth_dev_info_get(port, &dev_info);
	if (retval != 0) {
		printf("Error during getting device (port %u) info: %s\n",
				port, strerror(-retval));
		return retval;
	}

	if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
		port_conf.txmode.offloads |=
			RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

	/* Configure the Ethernet device. */
	retval = rte_eth_dev_configure(port, rx_rings, tx_rings, &port_conf);
	if (retval != 0)
		return retval;

	retval = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
	if (retval != 0)
		return retval;

	/* Allocate and set up 1 RX queue per Ethernet port. */
	for (q = 0; q < rx_rings; q++) {
		retval = rte_eth_rx_queue_setup(port, q, nb_rxd,
				rte_eth_dev_socket_id(port), NULL, mbuf_pool);
		if (retval < 0)
			return retval;
	}

	txconf = dev_info.default_txconf;
	txconf.offloads = port_conf.txmode.offloads;
	/* Allocate and set up 1 TX queue per Ethernet port. */
	for (q = 0; q < tx_rings; q++) {
		retval = rte_eth_tx_queue_setup(port, q, nb_txd,
				rte_eth_dev_socket_id(port), &txconf);
		if (retval < 0)
			return retval;
	}

	/* Starting Ethernet port. 8< */
	retval = rte_eth_dev_start(port);
	/* >8 End of starting of ethernet port. */
	if (retval < 0)
		return retval;

	/* Display the port MAC address. */
	struct rte_ether_addr addr;
	retval = rte_eth_macaddr_get(port, &addr);
	if (retval != 0)
		return retval;

	printf("Port %u MAC: %02" PRIx8 " %02" PRIx8 " %02" PRIx8
			   " %02" PRIx8 " %02" PRIx8 " %02" PRIx8 "\n",
			port, RTE_ETHER_ADDR_BYTES(&addr));

	/* Enable RX in promiscuous mode for the Ethernet device. */
	retval = rte_eth_promiscuous_enable(port);
	/* End of setting RX port in promiscuous mode. */
	if (retval != 0)
		return retval;

	return 0;
}
/* >8 End of main functional part of port initialization. */

/*
 * Send the response back to the client using DPDK.
 *
 * This function should be invoked by SPDK's callback functions.
 * For the first step, use a mock implementation here to test main_loop().
 */
static void send_resp_to_client(struct callback_args *cb_args) {
        /* PUT YOUR CODE HERE */
        printf("\nLOGGING: Send Response to Client\n");
        struct rte_ether_addr ether_src;
        struct rte_ether_hdr *ether_hdr = rte_pktmbuf_mtod_offset(bufs[0], struct rte_ether_hdr *, 0);

        //ether frame
        rte_ether_addr_copy(&ether_hdr->src_addr, &ether_src);
        rte_ether_addr_copy(&ether_hdr->dst_addr, &ether_hdr->src_addr);
        rte_ether_addr_copy(&ether_src, &ether_hdr->dst_addr);

        rte_eth_tx_burst(LAB2_PORT_ID, 0, bufs, 1);
}

static void write_complete(void *args, const struct spdk_nvme_cpl *completion) {
        printf("\nLOGGING: Write complete.\n");
        int rc;
        struct callback_args *args_ptr = args;
        
        /* Check if there's an error for the write request. */
        if (spdk_nvme_cpl_is_error(completion)) {
                spdk_nvme_qpair_print_completion(
                    qpair, (struct spdk_nvme_cpl *)completion);
                fprintf(stderr, "I/O error status: %s\n",
                        spdk_nvme_cpl_get_status_string(&completion->status));
                fprintf(stderr, "Failed to write, aborting run\n");
                exit(1);
        }
}

static void read_complete(void *args, const struct spdk_nvme_cpl *completion) {
        printf("\nLOGGING: Read complete.\n");
	struct callback_args *args_ptr = args;

        /* Check if there's an error for the read request. */
        if (spdk_nvme_cpl_is_error(completion)) {
                spdk_nvme_qpair_print_completion(
                    qpair, (struct spdk_nvme_cpl *)completion);
                fprintf(stderr, "I/O error status: %s\n",
                        spdk_nvme_cpl_get_status_string(&completion->status));
                fprintf(stderr, "Failed to read, aborting run\n");
                exit(1);
        }

        /* Unblock the while loop in main_loop(). */
        args_ptr->done = true;
        printf("\nLOGGING: [data=%s]\n", args_ptr->buf);
        send_resp_to_client(args_ptr);
}

/*
 * Try to receive a storage request from the client using DPDK.
 *
 * For the first step, use a mock implementation here to test main_loop().
 * 
 * Should populate the passed pointers with the relevant data.
 */
static void recv_req_from_client(struct req_context *ctx) {
	/* PUT YOUR CODE HERE */
        printf("\nLOGGING: Receive Request from Client\n");
	// struct req_context *ctx = malloc(sizeof *ctx);

        // struct rte_mbuf *bufs[BURST_SIZE];
        uint16_t nb_rx = 0;
        
        while (nb_rx == 0) {
                nb_rx = rte_eth_rx_burst(LAB2_PORT_ID, 0,
                        bufs, BURST_SIZE);
        }

        printf("\nLOGGING: Received RX Burst\n");

        // TODO: Remove sanity check
        char *data;

        data = rte_pktmbuf_mtod(bufs[0], char*, 0);
        char *prtp = (char *)data;
        uint16_t counter = 0;
        while (counter < 30) {
        	printf("%02hhx ", *prtp);
        	++counter;
        	if (counter % 4 == 0)
        		printf("\n");
        	++prtp;
        }
        /*
        received:
        40 50 68 0a 
        00 20 00 00 
        40 50 08 d6 
        1c 00 00 00 
        80 00 01 00 
        01 00 02 00 
        00 00 00 00 
        00 00 
        */

        // struct rte_ether_hdr *ether_hdr;
        // struct rte_ether_addr ether_src;
        // struct request_packet *req_pkt = malloc(sizeof(*req_pkt));

        printf("\nLOGGING: Retrieving Header Information\n");
        ctx->ether_hdr = rte_pktmbuf_mtod_offset(bufs[0], struct rte_ether_hdr *, 0);
        printf("\nLOGGING: Retrieving Request Information\n");
        // req_pkt = rte_pktmbuf_mtod_offset(bufs[0], struct request_packet *, sizeof(struct rte_ether_hdr));
        char *data = rte_pktmbuf_mtod_offset(bufs[0], char *, sizeof(struct rte_ether_hdr));
        // printf("\nLOGGING: Populating Context Values [lba=%lu]\n", req_pkt->lba);
        memcpy(&ctx->lba, data, sizeof(ctx->lba));
        printf("\nLOGGING: Populated Context Values [lba=%lu]\n", ctx->lba);
        // ctx->lba = req_pkt->lba;
        memcpy(&ctx->op, &data[sizeof(ctx->lba)], sizeof(ctx->op));
        printf("\nLOGGING: Populated Context Values [op=%d]\n", ctx->op);
        // ctx->op = req_pkt->op;
        printf("\nLOGGING: Populating Context Values [data]\n");
        // ctx->req_data = malloc(sizeof(req_pkt->req_data)/sizeof(req_pkt->req_data[0]));
        ctx->req_data = malloc(8);
        memcpy(ctx->req_data, &data[sizeof(ctx->lba)+sizeof(ctx->op)], 8);
        printf("\nLOGGING: Received Context Information [op=%d, lba=%lu, req_data=%hhu]\n", ctx->op, ctx->lba, ctx->req_data[0]);
}

/* 
 * Try to drain the completion queue and trigger callbacks.
 */
static void spdk_process_completions() {
	/* PUT YOUR CODE HERE */
        printf("\nLOGGING: QPair Completions.\n");
        spdk_nvme_qpair_process_completions(qpair, 0);
}

/*
 * Process the read request using SPDK.
 */
static void handle_read_req(struct req_context *ctx) {
	/* PUT YOUR CODE HERE */
        printf("\nLOGGING: Process Read Request\n");
        if (ctx->op != READ) {
                fprintf(stderr, "Invalid context for read operation [ctx_op=%d, ctx_data=%d]\n", ctx->op, *(ctx->req_data));
                exit(1);
        }

        int rc;
	// struct callback_args cb_args;
        // int sector_sz;

        /* Get the sector size. */
        // sector_sz = spdk_nvme_ns_get_sector_size(selected_ns);
        
        /* Allocate a DMA-safe host memory buffer. */
        // printf("\nLOGGING: SPDK ZMalloc in Read\n");
        // cb_args.buf = spdk_zmalloc(sector_sz, sector_sz, NULL,
        //                            SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

        if (!cb_args.buf) {
                fprintf(stderr, "Failed to allocate buffer\n");
                return;
        }
        cb_args.done = false;
        cb_args.req_ctx = ctx;

        /* Now submit a cmd to read data from the 1st sector. */
        rc = spdk_nvme_ns_cmd_read(
            selected_ns, qpair,
            cb_args.buf,  /* The buffer to store the read data */
            0,             /* Starting LBA to read the data */
            1,             /* Length in sectors */
            read_complete, /* Callback to invoke when the read is done. */
            &cb_args,       /* Argument to pass to the callback. */
	    0);
        if (rc != 0) {
                fprintf(stderr, "Failed to submit read cmd\n");
                exit(1);
        }

}

/*
 * Process the write request using SPDK.
 */
static void handle_write_req(struct req_context *ctx) {
	/* PUT YOUR CODE HERE */
        printf("\nLOGGING: Process Write Request\n");
        if (ctx->op != WRITE || *(ctx->req_data) != 8) {
                fprintf(stderr, "Dummy context improperly set up [ctx_op=%d, ctx_data=%d]\n", ctx->op, *(ctx->req_data));
                exit(1);
        }

        int rc;
	// struct callback_args cb_args;
        int sector_sz;

        /* Get the sector size. */
        sector_sz = spdk_nvme_ns_get_sector_size(selected_ns);
        
        /* Allocate a DMA-safe host memory buffer. */
        // printf("\nLOGGING: SPDK ZMalloc in Write\n");
        // cb_args.buf = spdk_zmalloc(sector_sz, sector_sz, NULL,
        //                            SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

        // if (!cb_args.buf) {
        //         fprintf(stderr, "Failed to allocate buffer\n");
        //         return;
        // }
        cb_args.done = false;

        /* Write the string into the buffer.  */
        snprintf(cb_args.buf, sector_sz, "%s", "Hello world!\n");

        /* Submit a cmd to write data into the 1st sector. */
        rc = spdk_nvme_ns_cmd_write(
            selected_ns, qpair,
	    ctx->req_data,    /* The data to write */
            0,              /* Starting LBA to write the data */
            1,              /* Length in sectors */
            write_complete, /* Callback to invoke when the write is done. */
            &cb_args,       /* Argument to pass to the callback. */
            0);
        if (rc != 0) {
                fprintf(stderr, "Failed to submit write cmd [error_code=%d]\n", rc);
                exit(1);
        }
}

/*
 * The main application logic.
 */
static void main_loop(void) {
	// struct req_context *dummy_ctx;
	struct req_context *ctx = malloc(sizeof *ctx);
        ctx->req_data = malloc(sizeof ctx->req_data);


	/* PUT YOUR CODE HERE */
        printf("\nLOGGING: Attempt qpair alloc\n");

        qpair = spdk_nvme_ctrlr_alloc_io_qpair(selected_ctrlr, NULL, 0);
        if (!qpair) {
                fprintf(stderr, "Failed to create SPDK queue pair\n");
                exit(1);
        }

        printf("\nLOGGING: Qpair alloc success\n");

        /* Dummy req_context */
        // uint8_t dummy_data = 8;
        // dummy_ctx->lba = 0;
        // dummy_ctx->op = 1; // Write first
        // dummy_ctx->req_data = &dummy_data;
        // dummy_ctx->rc;
        // dummy_ctx->resp_data;
	
        printf("\nLOGGING: SPDK ZMalloc\n");
        int sector_sz = spdk_nvme_ns_get_sector_size(selected_ns);
        cb_args.buf = spdk_zmalloc(sector_sz, sector_sz, NULL,
                                   SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);

        if (!cb_args.buf) {
                fprintf(stderr, "Failed to allocate buffer\n");
                exit(1);
        }

	/* The main event loop. */
	while (1) {
                //TODO: Remove test block
                printf("\nLOGGING: Process context\n");
                bufs[0] = rte_pktmbuf_alloc(mbuf_pool);
                recv_req_from_client(ctx);
                // ctx = dummy_ctx;
                if (ctx) {
                        if (ctx->op == READ) {
                                handle_read_req(ctx);
                        } else {
                                handle_write_req(ctx);
                        }
                }
                spdk_process_completions();

                //TODO: Remove test block
                // if (ctx->op == READ) {
                //         ctx->op = WRITE;
                // } else {
                //         ctx->op = READ;
                // }
                rte_pktmbuf_free(bufs[0]);
                sleep(3);
	}
}

/*
 * Will be called once per NVMe device found in the system.
 */
static bool probe_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                     struct spdk_nvme_ctrlr_opts *opts) {
        if (!selected_ctrlr) {
                printf("Attaching to %s\n", trid->traddr);
        }

        return !selected_ctrlr;
}

/*
 * Will be called for devices for which probe_cb returned true once that NVMe
 * controller has been attached to the userspace driver.
 */
static void attach_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                      struct spdk_nvme_ctrlr *ctrlr,
                      const struct spdk_nvme_ctrlr_opts *opts) {
        int nsid;
        struct spdk_nvme_ns *ns;

        printf("Attached to %s\n", trid->traddr);
        selected_ctrlr = ctrlr;

        /*
         * Iterate through the active NVMe namespaces to get a handle.
         */
        for (nsid = spdk_nvme_ctrlr_get_first_active_ns(ctrlr); nsid != 0;
             nsid = spdk_nvme_ctrlr_get_next_active_ns(ctrlr, nsid)) {
                ns = spdk_nvme_ctrlr_get_ns(ctrlr, nsid);
                if (!ns) {
                        continue;
                }
                printf("  Namespace ID: %d size: %juGB\n",
                       spdk_nvme_ns_get_id(ns),
                       spdk_nvme_ns_get_size(ns) / 1000000000);
                selected_ns = ns;
                break;
        }
}

//LAB 2
static void dpdk_init(void) {
        printf("\nLOGGING: DPDK Initialization\n");

        // struct rte_mempool *mbuf_pool;
	unsigned nb_ports;
	uint16_t portid;

	/* Check that there is an even number of ports to send/receive on. */
        //TODO - FIGURE OUT: is this needed or can we set to only 1 num_mbufs
	nb_ports = rte_eth_dev_count_avail();
        //TODO - PRUNED: even number of ports not needed
	// if (nb_ports < 2 || (nb_ports & 1))
	// 	rte_exit(EXIT_FAILURE, "Error: number of ports must be even\n");

	/* Creates a new mempool in memory to hold the mbufs. */

	/* Allocates mempool to hold the mbufs. 8< */
	mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
		MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
	/* >8 End of allocating mempool to hold mbuf. */

	if (mbuf_pool == NULL)
		rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

	/* Initializing ports. 8< */
	RTE_ETH_FOREACH_DEV(portid) {
		printf("\nLOGGING: [portid=%u]\n", portid);
		// LAB2: Only use port2
		if (portid != LAB2_PORT_ID) continue;
		if (port_init(portid, mbuf_pool) != 0)
			rte_exit(EXIT_FAILURE, "Cannot init port %"PRIu16 "\n",
					portid);
	}
	/* >8 End of initializing all ports. */
        printf("\nLOGGING: Port Initialization Complete\n");


	if (rte_lcore_count() > 1)
		printf("\nWARNING: Too many lcores enabled. Only 1 used.\n");
}

static void cleanup(void) {
        struct spdk_nvme_detach_ctx *detach_ctx = NULL;

        spdk_nvme_detach_async(selected_ctrlr, &detach_ctx);
        if (detach_ctx) {
                spdk_nvme_detach_poll(detach_ctx);
        }
}

int main(int argc, char **argv) {
        int rc;
        struct spdk_env_opts opts;
        struct spdk_nvme_transport_id trid;

        /* Intialize SPDK's library environment. */
        spdk_env_opts_init(&opts);
        if (spdk_env_init(&opts) < 0) {
                fprintf(stderr, "Failed to initialize SPDK env\n");
                return 1;
        }
        printf("Initializing NVMe Controllers\n");

        /*
         * Enumerate VMDs (Intel Volume Management Device) and hook them into
         * the spdk pci subsystem.
         */
        if (spdk_vmd_init()) {
                fprintf(stderr, "Failed to initialize VMD."
                                " Some NVMe devices can be unavailable.\n");
        }

        /*
         * Enumerate the bus indicated by the transport ID and attach the
         * userspace NVMe driver to each device found if desired.
         */
        spdk_nvme_trid_populate_transport(&trid, SPDK_NVME_TRANSPORT_PCIE);
        rc = spdk_nvme_probe(&trid, NULL, probe_cb, attach_cb, NULL);
        if (rc != 0) {
                fprintf(stderr, "Failed to probe nvme device\n");
                rc = 1;
                goto exit;
        }

        if (!selected_ctrlr) {
                fprintf(stderr, "Failed to find NVMe controller\n");
                rc = 1;
                goto exit;
        }

        printf("SPDK initialization completes.\n");

	/* PUT YOUR CODE HERE (DPDK initialization) */
        dpdk_init();

        main_loop();

	/* PUT YOUR CODE HERE (DPDK cleanup) */
        cleanup();
        spdk_vmd_fini();

exit:
        cleanup();
        spdk_env_fini();
        return rc;
}
