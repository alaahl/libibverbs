/* Copyright (c) 2005 Topspin Communications.  All rights reserved.
 * Copyright (c) 2009-2016 Mellanox Technologies.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED
 * TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if HAVE_CONFIG_H
	#include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>

#include "config.h"
#include "pingpong.h"

enum {
	PP_RECV_WRID = 1,
	PP_SEND_WRID = 2,
	PP_CQE_WAIT  = 3,
};

static struct test_params app_params;  // make command line args global

struct pingpong_context {
	struct ibv_context	*context;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr;
	struct ibv_cq		*scq;
	struct ibv_cq		*rcq;
	struct ibv_qp		*qp;

	struct ibv_qp		*mqp;
	struct ibv_cq		*mcq;

	void			*buf;
	int			size;
	int			rx_depth;

	int			scnt;
	int			rcnt;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
};

struct test_params {
	int		port;
	int		ib_port;
	int		size;
	enum ibv_mtu	mtu;
	int		rx_depth;
	int		iters;
	int		sl;
	char		ib_devname[128];
	char		servername[128];
};

void set_default_test_params(struct test_params *v)
{
	memset(v, 0, sizeof(struct test_params));
	v->port		= 18515;
	v->ib_port	= 1;
	v->size		= 4096;
	v->mtu		= IBV_MTU_1024;
	v->rx_depth	= 500;
	v->iters	= 1000;
	v->sl		= 0;
}

static int pp_connect_ctx(struct pingpong_context *ctx,
			    struct ibv_qp *qp,
			    int port,
			    int my_psn,
			    enum ibv_mtu mtu,
			    int sl,
			    struct pingpong_dest *dest)
{
	struct ibv_qp_attr attr = { 0 };
	attr.qp_state			= IBV_QPS_RTR,
	attr.path_mtu			= mtu,
	attr.dest_qp_num		= dest->qpn,
	attr.rq_psn			= dest->psn,
	attr.max_dest_rd_atomic		= 1,
	attr.min_rnr_timer		= 12,
	attr.ah_attr.dlid		= dest->lid,
	attr.ah_attr.sl			= sl,
	attr.ah_attr.port_num		= port;

	if (ibv_modify_qp(qp, &attr,
			  IBV_QP_STATE			|
			  IBV_QP_AV			|
			  IBV_QP_PATH_MTU		|
			  IBV_QP_DEST_QPN		|
			  IBV_QP_RQ_PSN			|
			  IBV_QP_MAX_DEST_RD_ATOMIC	|
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state		= IBV_QPS_RTS;
	attr.timeout		= 14;
	attr.retry_cnt		= 7;
	attr.rnr_retry		= 7;
	attr.sq_psn		= my_psn;
	attr.max_rd_atomic	= 1;
	if (ibv_modify_qp(qp, &attr,
			  IBV_QP_STATE			|
			  IBV_QP_TIMEOUT		|
			  IBV_QP_RETRY_CNT		|
			  IBV_QP_RNR_RETRY		|
			  IBV_QP_SQ_PSN			|
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername,
						   int port,
						   const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest = NULL;

	if (asprintf(&service, "%d", port) < 0)
		return NULL;

	n = getaddrinfo(servername, service, &hints, &res);
	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
				break;
			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}

	sprintf(msg, "%04x:%06x:%06x", my_dest->lid, my_dest->qpn, my_dest->psn);
	if (write(sockfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		goto out;
	}

	if (read(sockfd, msg, sizeof msg) != sizeof msg) {
		perror("client read");
		fprintf(stderr, "Couldn't read remote address\n");
		goto out;
	}

	if (write(sockfd, "done", sizeof "done") != sizeof("done")) {
		fprintf(stderr, "Couldn't send \"done\" msg\n");
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn);

out:
	close(sockfd);
	return rem_dest;
}

static struct pingpong_dest *pp_server_exch_dest(struct pingpong_context *ctx,
						 int ib_port,
						 enum ibv_mtu mtu,
						 int port,
						 int sl,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_flags    = AI_PASSIVE,
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000"];
	int n;
	int sockfd = -1, connfd;
	struct pingpong_dest *rem_dest = NULL;

	if (asprintf(&service, "%d", port) < 0) {
		return NULL;
	}

	n = getaddrinfo(NULL, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for port %d\n", gai_strerror(n), port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
			n = 1;

			setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &n, sizeof n);

			if (!bind(sockfd, t->ai_addr, t->ai_addrlen))
				break;

			close(sockfd);
			sockfd = -1;
		}
	}

	freeaddrinfo(res);
	free(service);

	if (sockfd < 0) {
		fprintf(stderr, "Couldn't listen to port %d\n", port);
		return NULL;
	}

	listen(sockfd, 1);
	connfd = accept(sockfd, NULL, 0);
	close(sockfd);

	if (connfd < 0) {
		fprintf(stderr, "accept() failed\n");
		return NULL;
	}

	n = read(connfd, msg, sizeof msg);
	if (n != sizeof msg) {
		perror("server read");
		fprintf(stderr, "%d/%d: Couldn't read remote address\n", n, (int) sizeof msg);
		goto out;
	}

	rem_dest = malloc(sizeof *rem_dest);
	if (!rem_dest)
		goto out;

	sscanf(msg, "%x:%x:%x", &rem_dest->lid, &rem_dest->qpn, &rem_dest->psn);

	if (pp_connect_ctx(ctx, ctx->qp, ib_port, my_dest->psn, mtu, sl, rem_dest)) {
		fprintf(stderr, "Couldn't connect to remote QP\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	sprintf(msg, "%04x:%06x:%06x", my_dest->lid, my_dest->qpn, my_dest->psn);
	if (write(connfd, msg, sizeof msg) != sizeof msg) {
		fprintf(stderr, "Couldn't send local address\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

	/* expecting msg "done" */
	if (read(connfd, msg, sizeof(msg)) <= 0) {
		fprintf(stderr, "Couldn't read \"done\" msg\n");
		free(rem_dest);
		rem_dest = NULL;
		goto out;
	}

out:
	close(connfd);
	return rem_dest;
}

struct pingpong_dest *get_remote_dest(struct pingpong_context *ctx, int is_client,
					struct pingpong_dest *my_dest)
{
	struct pingpong_dest *rem_dest = NULL;

	if (is_client)
		rem_dest = pp_client_exch_dest(app_params.servername, app_params.port, my_dest);
	else
		rem_dest = pp_server_exch_dest(ctx, app_params.ib_port, app_params.mtu, app_params.port, app_params.sl, my_dest);
	return rem_dest;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size, int rx_depth, int port)
{
	struct pingpong_context *ctx;
	long page_size;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx)
		return NULL;

	ctx->size	= size;
	ctx->rx_depth	= rx_depth;

	page_size = sysconf(_SC_PAGESIZE);
	ctx->buf = memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}

	memset(ctx->buf, 0, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
			goto clean_buffer;
	}

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_device;
	}

	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, IBV_ACCESS_LOCAL_WRITE);
	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_pd;
	}

	{
		struct ibv_cq_init_attr_ex attr = { 0 };
		attr.comp_mask	= IBV_CQ_INIT_ATTR_FLAGS;
		attr.flags	= IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN;
		attr.cqe	= rx_depth;

		ctx->rcq = ibv_create_cq_ex(ctx->context, &attr);
		if (!ctx->rcq) {
			fprintf(stderr, "Couldn't create RCQ\n");
			goto clean_mr;
		}
	}

	{
		struct ibv_cq_init_attr_ex attr = { 0 };
		attr.comp_mask	= IBV_CQ_INIT_ATTR_FLAGS;
		attr.flags	= IBV_CREATE_CQ_ATTR_IGNORE_OVERRUN;
		attr.cqe	= 0x10;

		ctx->scq = ibv_create_cq_ex(ctx->context, &attr);
		if (!ctx->scq) {
			fprintf(stderr, "Couldn't create SCQ\n");
			goto clean_rcq;
		}
	}

	{
		struct ibv_qp_init_attr_ex attr = { 0 };
		attr.send_cq		= ctx->scq;
		attr.recv_cq		= ctx->rcq;
		attr.cap.max_send_wr	= 16;
		attr.cap.max_recv_wr	= rx_depth;
		attr.cap.max_send_sge	= 16;
		attr.cap.max_recv_sge	= 16;
		attr.qp_type		= IBV_QPT_RC;
		attr.pd			= ctx->pd;
		attr.comp_mask |= IBV_QP_INIT_ATTR_CREATE_FLAGS | IBV_QP_INIT_ATTR_PD;
		attr.create_flags = IBV_QP_CREATE_CROSS_CHANNEL | IBV_QP_CREATE_MANAGED_SEND;

		ctx->qp = ibv_create_qp_ex(ctx->context, &attr);
		if (!ctx->qp)  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_scq;
		}
	}

	{
		struct ibv_qp_attr attr = { 0 };
		attr.qp_state		= IBV_QPS_INIT;
		attr.port_num		= port;

		if (ibv_modify_qp(ctx->qp, &attr,
				  IBV_QP_STATE		|
				  IBV_QP_PKEY_INDEX	|
				  IBV_QP_PORT		|
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}


	/* Create MQ */
	ctx->mcq = ibv_create_cq(ctx->context, 0x40, NULL, NULL, 0);
	if (!ctx->mcq) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_qp;
	}

	{
		struct ibv_qp_init_attr_ex attr = { 0 };
		attr.send_cq		= ctx->mcq;
		attr.recv_cq		= ctx->mcq;
		attr.cap.max_send_wr	= 0x40;
		attr.cap.max_send_sge	= 1;
		attr.cap.max_recv_sge	= 1;
		attr.qp_type		= IBV_QPT_RC;
		attr.pd			= ctx->pd;
		attr.comp_mask |= IBV_QP_INIT_ATTR_CREATE_FLAGS | IBV_QP_INIT_ATTR_PD;
		attr.create_flags = IBV_QP_CREATE_CROSS_CHANNEL;
		ctx->mqp = ibv_create_qp_ex(ctx->context, &attr);
		if (!ctx->mqp)  {
			fprintf(stderr, "Couldn't create MQP\n");
			goto clean_mcq;
		}
	}

	{
		struct ibv_qp_attr attr = { 0 };
		attr.qp_state        = IBV_QPS_INIT;
		attr.port_num        = port;

		if (ibv_modify_qp(ctx->mqp, &attr,
				    IBV_QP_STATE              |
				    IBV_QP_PKEY_INDEX         |
				    IBV_QP_PORT               |
				    IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_mqp;
		}
	}

	{
		struct ibv_qp_attr qp_attr = { 0 };
		qp_attr.qp_state		= IBV_QPS_RTR;
		qp_attr.path_mtu		= 1;
		qp_attr.dest_qp_num		= ctx->mqp->qp_num;
		qp_attr.max_dest_rd_atomic	= 1;
		qp_attr.min_rnr_timer		= 12;
		qp_attr.ah_attr.port_num	= port;

		if (ibv_modify_qp(ctx->mqp, &qp_attr,
				    IBV_QP_STATE              |
				    IBV_QP_AV                 |
				    IBV_QP_PATH_MTU           |
				    IBV_QP_DEST_QPN           |
				    IBV_QP_RQ_PSN             |
				    IBV_QP_MAX_DEST_RD_ATOMIC |
				    IBV_QP_MIN_RNR_TIMER)) {
			fprintf(stderr, "Failed to modify QP to RTR\n");
			goto clean_mqp;
		}

		qp_attr.qp_state	= IBV_QPS_RTS;
		qp_attr.timeout		= 14;
		qp_attr.retry_cnt	= 7;
		qp_attr.rnr_retry	= 7;
		qp_attr.sq_psn	        = 0;
		qp_attr.max_rd_atomic   = 1;
		if (ibv_modify_qp(ctx->mqp, &qp_attr,
				  IBV_QP_STATE              |
				  IBV_QP_TIMEOUT            |
				  IBV_QP_RETRY_CNT          |
				  IBV_QP_RNR_RETRY          |
				  IBV_QP_SQ_PSN             |
				  IBV_QP_MAX_QP_RD_ATOMIC)) {
			fprintf(stderr, "Failed to modify QP to RTS\n");
			goto clean_mqp;
		}
	}

	return ctx;

clean_mqp:
	ibv_destroy_qp(ctx->mqp);

clean_mcq:
	ibv_destroy_cq(ctx->mcq);

clean_qp:
	ibv_destroy_qp(ctx->qp);

clean_scq:
	ibv_destroy_cq(ctx->scq);

clean_rcq:
	ibv_destroy_cq(ctx->rcq);

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;
}

int pp_close_ctx(struct pingpong_context *ctx)
{
	if (ibv_destroy_qp(ctx->mqp)) {
		fprintf(stderr, "Couldn't destroy mQP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->mcq)) {
		fprintf(stderr, "Couldn't destroy mCQ\n");
		return 1;
	}

	if (ibv_destroy_qp(ctx->qp)) {
		fprintf(stderr, "Couldn't destroy QP\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->rcq)) {
		fprintf(stderr, "Couldn't destroy rCQ\n");
		return 1;
	}

	if (ibv_destroy_cq(ctx->scq)) {
		fprintf(stderr, "Couldn't destroy sCQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
	int rc;

	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id		= PP_RECV_WRID,
		.sg_list	= &list,
		.num_sge	= 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i) {
		rc = ibv_post_recv(ctx->qp, &wr, &bad_wr);
		if (rc)
			return rc;
	}

	return i;
}

static int pp_post_send(struct pingpong_context *ctx, int wait_recv)
{
	int rc;
	struct ibv_wc mwc;
	struct ibv_wc wc;
	struct ibv_send_wr *bad_wr;
	int ne;

	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};

	struct ibv_send_wr wr = {
		.wr_id	    = PP_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode = IBV_WR_SEND,
		.send_flags = IBV_SEND_SIGNALED,
	};

	struct ibv_send_wr wr_en = {
		.wr_id	    = wr.wr_id,
		.sg_list    = NULL,
		.num_sge    = 0,
		.opcode     = IBV_WR_SEND_ENABLE,
		.send_flags = (wait_recv ? 0 : IBV_SEND_SIGNALED),
	};

	struct ibv_send_wr wr_wait = {
		.wr_id	    = ctx->scnt,
		.sg_list    = NULL,
		.num_sge    = 0,
		.opcode = IBV_WR_CQE_WAIT,
		.send_flags = IBV_SEND_SIGNALED,
	};
	rc = ibv_post_send(ctx->qp, &wr, &bad_wr);
	if (rc)
		return rc;

	/* fill in send work enable request */
	wr_en.wr.wqe_enable.qp   = ctx->qp;
	wr_en.wr.wqe_enable.wqe_count = 0;
	wr_en.send_flags |= IBV_SEND_WAIT_EN_LAST;
	rc = ibv_post_send(ctx->mqp, &wr_en, &bad_wr);
	if (rc)
		return rc;

	/* fill in wait work enable request */
	if (wait_recv) {
		wr_wait.wr.cqe_wait.cq   = ctx->rcq;
		wr_wait.wr.cqe_wait.cq_count = 1;
		wr_wait.send_flags |=  IBV_SEND_WAIT_EN_LAST;
		wr_wait.next = NULL;

		rc = ibv_post_send(ctx->mqp, &wr_wait, &bad_wr);
		if (rc)
			return rc;
	}

	do {
		rc = ibv_poll_cq(ctx->mcq, 1, &mwc);
		if (rc < 0)
			return -1;
	} while (rc == 0);

	if (mwc.status != IBV_WC_SUCCESS)
		return -1;

	do {
		ne = ibv_poll_cq(ctx->scq, 1, &wc);
		if (ne < 0) {
			fprintf(stderr, "poll CQ failed %d\n", ne);
			return 1;
		}
	} while (!ne);

	if (wc.status != IBV_WC_SUCCESS) {
		fprintf(stderr, "cqe error status %s (%d v:%d) for count %d\n",
			ibv_wc_status_str(wc.status),
			wc.status, wc.vendor_err,
			ctx->rcnt);
		return 1;
	}

	return 0;
}

static void usage(const char *argv0)
{
	printf("Usage:\n");
	printf("  %s            start a server and wait for connection\n",	       argv0);
	printf("  %s <host>     connect to server at <host>\n", argv0);
	printf("\n");
	printf("Options:\n");
	printf("  -p, --port=<port>           listen on/connect to port <port> (default 18515)\n");
	printf("  -d, --ib-dev=<dev>          use IB device <dev>   (default first device found)\n");
	printf("  -i, --ib-port=<port>        use port <port> of IB device (default 1)\n");
	printf("  -s, --size=<size>           size of message to exchange  (default 4096 minimum 16)\n");
	printf("  -m, --mtu=<size>            path MTU (default 1024)\n");
	printf("  -r, --rx-depth=<dep>        number of receives to post at a time (default 500)\n");
	printf("  -n, --iters=<iters>         number of exchanges (default 1000)\n");
	printf("  -l, --sl=<sl>               service level value\n");
}

int parse_command_line_args(int argc, char*argv[], struct test_params * app_params)
{
	set_default_test_params(app_params);

	while (1) {
		int c;

		static struct option long_options[] = {
			{ .name = "port",	.has_arg = 1, .val = 'p' },
			{ .name = "ib-dev",	.has_arg = 1, .val = 'd' },
			{ .name = "ib-port",	.has_arg = 1, .val = 'i' },
			{ .name = "size",	.has_arg = 1, .val = 's' },
			{ .name = "mtu",	.has_arg = 1, .val = 'm' },
			{ .name = "rx-depth",   .has_arg = 1, .val = 'r' },
			{ .name = "iters",	.has_arg = 1, .val = 'n' },
			{ .name = "sl",		.has_arg = 1, .val = 'l' },
			{ 0 }
		};

		c = getopt_long(argc, argv, "p:d:i:s:m:r:n:l:e",
				long_options, NULL);
		if (c == -1)
			break;

		switch (c) {
		case 'p':
			app_params->port = strtol(optarg, NULL, 0);
			if (app_params->port < 0 || app_params->port > 65535) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'd':
			strncpy(app_params->ib_devname, optarg, sizeof(app_params->ib_devname));
			break;

		case 'i':
			app_params->ib_port = strtol(optarg, NULL, 0);
			if (app_params->ib_port < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 's':
			app_params->size = strtol(optarg, NULL, 0);
			if (app_params->size < 16) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'm':
			app_params->mtu = pp_mtu_to_enum(strtol(optarg, NULL, 0));
			if (app_params->mtu < 0) {
				usage(argv[0]);
				return 1;
			}
			break;

		case 'r':
			app_params->rx_depth = strtol(optarg, NULL, 0);
			break;

		case 'n':
			app_params->iters = strtol(optarg, NULL, 0);
			break;

		case 'l':
			app_params->sl = strtol(optarg, NULL, 0);
			break;

		default:
			usage(argv[0]);
			return 1;
		}
	}

	if (optind == argc - 1) {
		strncpy(app_params->servername, argv[optind], sizeof(app_params->servername));
	}
	else if (optind < argc) {
		usage(argv[0]);
		return 1;
	}

	return 0;
}

void dump_results(struct test_params * app_params, struct timeval *start, struct timeval *end)
{
	float usec = (end->tv_sec - start->tv_sec) * 1000000 + (end->tv_usec - start->tv_usec);
	long long bytes = (long long) app_params->size * app_params->iters * 2;

	printf("%lld bytes in %.2f seconds = %.2f Mbit/sec\n", bytes, usec / 1000000., bytes * 8. / usec);
}

int run_task_pingpong_app(int is_client)
{
	struct ibv_device	**dev_list;
	struct ibv_device	*ib_dev = NULL;
	struct pingpong_context *ctx;
	struct pingpong_dest	my_dest;
	struct pingpong_dest	*rem_dest;
	struct timeval		start, end;
	char			*ib_devname = NULL;
	int 		ret = 0;
	int			routs;
	int			num_cq_events = 0;

	srand48(getpid() * time(NULL));

	dev_list = ibv_get_device_list(NULL);
	if (!dev_list) {
		fprintf(stderr, "No IB devices found\n");
		return 1;
	}

	if (!ib_devname) {
		ib_dev = *dev_list;
		if (!ib_dev) {
			fprintf(stderr, "No IB devices found\n");
			return 1;
		}
	} else {
		int i;
		for (i = 0; dev_list[i]; ++i)
			if (!strcmp(ibv_get_device_name(dev_list[i]),
			    ib_devname))
				break;
		ib_dev = dev_list[i];
		if (!ib_dev) {
			fprintf(stderr, "IB device %s not found\n", ib_devname);
			return 1;
		}
	}

	ctx = pp_init_ctx(ib_dev, app_params.size, app_params.rx_depth, app_params.ib_port);
	if (!ctx)
		return 1;

	routs = pp_post_recv(ctx, ctx->rx_depth);
	if (routs < ctx->rx_depth) {
		fprintf(stderr, "Couldn't post receive (%d)\n", routs);
		return 1;
	}

	my_dest.lid = pp_get_local_lid(ctx->context, app_params.ib_port);
	my_dest.qpn = ctx->qp->qp_num;
	my_dest.psn = lrand48() & 0xffffff;
	if (!my_dest.lid) {
		fprintf(stderr, "Couldn't get local LID\n");
		return 1;
	}

	printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n", my_dest.lid, my_dest.qpn, my_dest.psn);

	rem_dest = (struct pingpong_dest *) get_remote_dest(ctx, is_client, &my_dest);
	if (rem_dest == NULL) {
			fprintf(stderr, "Failed to exchange data with remote destination\n");
			return 1;
	}

	if (!rem_dest)
		return 1;

	printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x\n", rem_dest->lid, rem_dest->qpn, rem_dest->psn);

	if (is_client) {
		if (pp_connect_ctx(ctx, ctx->qp, app_params.ib_port, my_dest.psn, app_params.mtu, app_params.sl, rem_dest))
			return 1;
		if (pp_post_send(ctx, 0)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
	}

	if (gettimeofday(&start, NULL)) {
		perror("gettimeofday");
		return 1;
	}

	ctx->scnt = ctx->rcnt = 0;
	while (ctx->rcnt < app_params.iters && ctx->scnt < app_params.iters) {
		struct ibv_wc wc;
		int ne;

		do {
			ne = ibv_poll_cq(ctx->rcq, 1, &wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}
		} while (ne < 1);

		if (wc.status != IBV_WC_SUCCESS) {
			fprintf(stderr, "cqe error status %s (%d v:%d)"
				" for count %d\n",
				ibv_wc_status_str(wc.status),
				wc.status, wc.vendor_err,
				ctx->rcnt);
			return 1;
		}

		ctx->rcnt++;

		if (pp_post_recv(ctx, 1) < 0) {
			fprintf(stderr, "Couldn't post receive\n");
			return 1;
		}

		if (pp_post_send(ctx, 1)) {
			fprintf(stderr, "Couldn't post send\n");
			return 1;
		}
	}

	if (gettimeofday(&end, NULL)) {
		perror("gettimeofday");
		return 1;
	}

	dump_results(&app_params, &start, &end);

	ibv_ack_cq_events(ctx->rcq, num_cq_events);

	if (pp_close_ctx(ctx))
		return 1;

	ibv_free_device_list(dev_list);

	free(rem_dest);

	return ret;
}

int main(int argc, char **argv)
{
    int ret;
    int is_client;

    ret = parse_command_line_args(argc, argv, &app_params);
    if (ret != 0) {
	fprintf(stderr, "Error parsing command line arguments");
	exit(0);
    }

    is_client = (app_params.servername[0] != 0);
    return run_task_pingpong_app(is_client);
}
