/*
 * Copyright (c) 2013-2015 Intel Corporation.  All rights reserved.
 * Copyright (c) 2015 Cisco Systems, Inc.  All rights reserved.
 *
 * This software is available to you under the BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AWV
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <time.h>
#include <netdb.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <shared.h>


#define MAX_POLL_CNT 10


static int alloc_ep_res(struct fi_info *fi)
{
	struct fi_poll_attr poll_attr;
	int ret;

	ret = ft_alloc_active_res(fi);
	if (ret)
		return ret;

	memset(&poll_attr, 0, sizeof poll_attr);
	ret = fi_poll_open(domain, &poll_attr, &pollset);
	if (ret) {
		FT_PRINTERR("fi_poll_open", ret);
		return ret;
	}

	if (txcq) {
		ret = fi_poll_add(pollset, &txcq->fid, 0);
		if (ret)
			goto err;
	}

	if (rxcq) {
		ret = fi_poll_add(pollset, &rxcq->fid, 0);
		if (ret)
			goto err;
	}

	if (txcntr) {
		ret = fi_poll_add(pollset, &txcntr->fid, 0);
		if (ret)
			goto err;
	}

	if (rxcntr) {
		ret = fi_poll_add(pollset, &rxcntr->fid, 0);
		if (ret)
			goto err;
	}

	return 0;
err:
	FT_PRINTERR("fi_poll_add", ret);
	return ret;
}

static int init_fabric(void)
{
	int ret;

	ret = ft_getinfo(hints, &fi);
	if (ret)
		return ret;

	ret = ft_open_fabric_res();
	if (ret)
		return ret;

	ret = alloc_ep_res(fi);
	if (ret)
		return ret;

	ret = ft_init_ep();
	if (ret)
		return ret;
	return 0;
}

static int send_recv()
{
	struct fid_cq *cq;
	void *context[MAX_POLL_CNT];
	struct fi_cq_entry comp;
	int ret;
	int ret_count = 0;
	int i, tx_cntr_done = 0, rx_cntr_done = 0;

	fprintf(stdout, "Posting a send...\n");
	ret = ft_post_tx(ep, remote_fi_addr, tx_size, &tx_ctx);
	if (ret)
		return ret;

	while ((txcq && (tx_cq_cntr < tx_seq)) || (rxcq && (rx_cq_cntr < rx_seq)) ||
	       (txcntr && !tx_cntr_done) || (rxcntr && !rx_cntr_done)) {

		/* Poll send and recv CQs/Cntrs */
		do {
			ret_count = fi_poll(pollset, context, MAX_POLL_CNT);
			if (ret_count < 0) {
				FT_PRINTERR("fi_poll", ret_count);
				return ret_count;
			}
		} while (!ret_count);

		fprintf(stdout, "Retrieved %d event(s)\n", ret_count);

		for (i = 0; i < ret_count; i++) {
			if (context[i] == &txcq) {
				printf("Send completion received\n");
				cq = txcq;
				tx_cq_cntr++;
			} else if (context[i] == &rxcq) {
				printf("Recv completion received\n");
				cq = rxcq;
				rx_cq_cntr++;
			} else if (context[i] == &txcntr) {
				printf("Send counter poll-event\n");
				if (tx_cntr_done) {
					printf("Invalid tx counter event\n");
					return -1;
				}

				if (tx_seq == fi_cntr_read(txcntr)) {
					printf("Send counter done\n");
					tx_cntr_done = 1;
				}
				continue;
			} else if (context[i] == &rxcntr) {
				printf("Recv counter poll-event\n");
				if (rx_cntr_done) {
					printf("Invalid rx counter event\n");
					return -1;
				}

				if (rx_seq == fi_cntr_read(rxcntr)) {
					printf("Receive counter done\n");
					rx_cntr_done = 1;
				}
				continue;
			} else {
				printf("Unknown completion received\n");
				return -1;
			}

			/* Read the completion entry */
			ret = fi_cq_read(cq, &comp, 1);
			if (ret < 0) {
				if (ret == -FI_EAVAIL) {
					ret = ft_cq_readerr(cq);
				} else {
					FT_PRINTERR("fi_cq_read", ret);
				}
				return ret;
			}
		}
	}

	return 0;
}

static int run(void)
{
	int ret;

	ret = init_fabric();
	if (ret)
		return ret;

	ret = ft_init_av();
	if (ret)
		return ret;

	return send_recv();
}

int main(int argc, char **argv)
{
	int op, ret = 0;

	opts = INIT_OPTS;
	opts.options |= FT_OPT_SIZE;

	hints = fi_allocinfo();
	if (!hints)
		return EXIT_FAILURE;

	while ((op = getopt(argc, argv, "h" ADDR_OPTS CS_OPTS INFO_OPTS)) != -1) {
		switch (op) {
		default:
			ft_parse_addr_opts(op, optarg, &opts);
			ft_parseinfo(op, optarg, hints);
			ft_parsecsopts(op, optarg, &opts);
			break;
		case '?':
		case 'h':
			ft_usage(argv[0], "A client-server example that uses poll.\n");
			FT_PRINT_OPTS_USAGE("-t <type>", "completion type [queue, counter]");
			return EXIT_FAILURE;
		}
	}

	if (optind < argc)
		opts.dst_addr = argv[optind];

	hints->ep_attr->type = FI_EP_RDM;
	hints->caps = FI_MSG;
	hints->mode = FI_CONTEXT | FI_LOCAL_MR;

	ret = run();

	ft_free_res();
	return -ret;
}
