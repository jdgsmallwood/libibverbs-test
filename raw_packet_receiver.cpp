#include <arpa/inet.h>
#include <infiniband/verbs.h>
#include <net/ethernet.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define UDP_PORT 12345

struct rdma_context {
  struct ibv_device **dev_list;
  struct ibv_context *context;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_qp *qp;
  struct ibv_mr *mr;
  char *buffer;
};

int init_rdma_context(struct rdma_context *ctx, int gid_idx) {
  int num_devices;

  // Get device list
  ctx->dev_list = ibv_get_device_list(&num_devices);
  if (!ctx->dev_list || num_devices == 0) {
    fprintf(stderr, "No RDMA devices found\n");
    return -1;
  }

  // Open device
  ctx->context = ibv_open_device(ctx->dev_list[0]);
  if (!ctx->context) {
    fprintf(stderr, "Failed to open device\n");
    return -1;
  }

  // Allocate protection domain
  ctx->pd = ibv_alloc_pd(ctx->context);
  if (!ctx->pd) {
    fprintf(stderr, "Failed to allocate PD\n");
    return -1;
  }

  // Create completion queue
  ctx->cq = ibv_create_cq(ctx->context, 10, NULL, NULL, 0);
  if (!ctx->cq) {
    fprintf(stderr, "Failed to create CQ\n");
    return -1;
  }

  // Allocate buffer
  ctx->buffer = (char *)malloc(BUFFER_SIZE);
  if (!ctx->buffer) {
    fprintf(stderr, "Failed to allocate buffer\n");
    return -1;
  }

  // Register memory region
  ctx->mr = ibv_reg_mr(ctx->pd, ctx->buffer, BUFFER_SIZE,
                       IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!ctx->mr) {
    fprintf(stderr, "Failed to register MR\n");
    return -1;
  }

  // Create raw packet queue pair
  struct ibv_qp_init_attr qp_attr = {};
  qp_attr.send_cq = ctx->cq;
  qp_attr.recv_cq = ctx->cq;
  qp_attr.qp_type = IBV_QPT_RAW_PACKET;
  qp_attr.cap.max_send_wr = 10;
  qp_attr.cap.max_recv_wr = 10;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
  if (!ctx->qp) {
    fprintf(stderr, "Failed to create QP\n");
    return -1;
  }

  // Transition QP to INIT state
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = 1;

  if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE | IBV_QP_PORT)) {
    fprintf(stderr, "Failed to modify QP to INIT\n");
    return -1;
  }

  // Transition QP to RTR state
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;

  if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
    fprintf(stderr, "Failed to modify QP to RTR\n");
    return -1;
  }

  // Transition QP to RTS state
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;

  if (ibv_modify_qp(ctx->qp, &attr, IBV_QP_STATE)) {
    fprintf(stderr, "Failed to modify QP to RTS\n");
    return -1;
  }

  return 0;
}

int post_recv(struct rdma_context *ctx) {
  struct ibv_sge sge = {};
  sge.addr = (uintptr_t)ctx->buffer;
  sge.length = BUFFER_SIZE;
  sge.lkey = ctx->mr->lkey;

  struct ibv_recv_wr wr = {};
  wr.wr_id = 1;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  struct ibv_recv_wr *bad_wr;
  return ibv_post_recv(ctx->qp, &wr, &bad_wr);
}

void cleanup_rdma_context(struct rdma_context *ctx) {
  if (ctx->qp)
    ibv_destroy_qp(ctx->qp);
  if (ctx->mr)
    ibv_dereg_mr(ctx->mr);
  if (ctx->buffer)
    free(ctx->buffer);
  if (ctx->cq)
    ibv_destroy_cq(ctx->cq);
  if (ctx->pd)
    ibv_dealloc_pd(ctx->pd);
  if (ctx->context)
    ibv_close_device(ctx->context);
  if (ctx->dev_list)
    ibv_free_device_list(ctx->dev_list);
}

int main(int argc, char *argv[]) {
  struct rdma_context ctx = {};
  int gid_idx = (argc > 1) ? atoi(argv[1]) : 0;

  printf("RDMA Raw Packet Server starting (GID index: %d)...\n", gid_idx);

  if (init_rdma_context(&ctx, gid_idx) < 0) {
    fprintf(stderr, "Failed to initialize RDMA context\n");
    return 1;
  }

  // Post receive work request
  if (post_recv(&ctx) < 0) {
    fprintf(stderr, "Failed to post receive\n");
    cleanup_rdma_context(&ctx);
    return 1;
  }

  printf("Server listening for packets...\n");

  // Poll for completion
  while (1) {
    struct ibv_wc wc;
    int ret = ibv_poll_cq(ctx.cq, 1, &wc);

    if (ret > 0) {
      if (wc.status == IBV_WC_SUCCESS) {
        printf("Received packet of %d bytes\n", wc.byte_len);

        // Parse Ethernet header
        struct ether_header *eth = (struct ether_header *)ctx.buffer;
        if (ntohs(eth->ether_type) == ETHERTYPE_IP) {
          struct iphdr *ip =
              (struct iphdr *)(ctx.buffer + sizeof(struct ether_header));
          if (ip->protocol == IPPROTO_UDP) {
            struct udphdr *udp =
                (struct udphdr *)(ctx.buffer + sizeof(struct ether_header) +
                                  sizeof(struct iphdr));
            char *payload = ctx.buffer + sizeof(struct ether_header) +
                            sizeof(struct iphdr) + sizeof(struct udphdr);
            int payload_len = wc.byte_len - sizeof(struct ether_header) -
                              sizeof(struct iphdr) - sizeof(struct udphdr);

            printf("UDP packet from port %d to port %d\n", ntohs(udp->source),
                   ntohs(udp->dest));
            printf("Payload (%d bytes): %.*s\n", payload_len, payload_len,
                   payload);
          }
        }

        // Post another receive
        post_recv(&ctx);
      } else {
        fprintf(stderr, "Completion with error: %s\n",
                ibv_wc_status_str(wc.status));
      }
    } else if (ret < 0) {
      fprintf(stderr, "Poll CQ failed\n");
      break;
    }

    usleep(1000); // Small delay to prevent busy waiting
  }

  cleanup_rdma_context(&ctx);
  return 0;
}
