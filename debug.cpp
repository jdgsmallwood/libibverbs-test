#include <errno.h>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void print_device_caps(struct ibv_context *context) {
  struct ibv_device_attr device_attr;

  if (ibv_query_device(context, &device_attr)) {
    fprintf(stderr, "Failed to query device attributes\n");
    return;
  }

  printf("Device capabilities:\n");
  printf("  Max QPs: %d\n", device_attr.max_qp);
  printf("  Max CQs: %d\n", device_attr.max_cq);
  printf("  Max MRs: %d\n", device_attr.max_mr);
  printf("  Device cap flags: 0x%llx\n",
         (unsigned long long)device_attr.device_cap_flags);

  // Check for raw packet support
  if (device_attr.device_cap_flags & IBV_DEVICE_RAW_MULTI) {
    printf("  ✓ Raw packet support detected\n");
  } else {
    printf("  ✗ Raw packet support NOT detected\n");
  }
}

void print_port_info(struct ibv_context *context, int port_num) {
  struct ibv_port_attr port_attr;

  if (ibv_query_port(context, port_num, &port_attr)) {
    fprintf(stderr, "Failed to query port %d\n", port_num);
    return;
  }

  printf("Port %d info:\n", port_num);
  printf("  State: %s\n", port_attr.state == IBV_PORT_ACTIVE ? "ACTIVE"
                          : port_attr.state == IBV_PORT_DOWN ? "DOWN"
                          : port_attr.state == IBV_PORT_INIT ? "INIT"
                                                             : "OTHER");
  printf("  Max MTU: %d\n", port_attr.max_mtu);
  printf("  Active MTU: %d\n", port_attr.active_mtu);
  printf("  Link layer: %s\n",
         port_attr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "InfiniBand"
         : port_attr.link_layer == IBV_LINK_LAYER_ETHERNET ? "Ethernet"
                                                           : "Unknown");
}

int test_qp_creation(struct ibv_context *context, struct ibv_pd *pd,
                     struct ibv_cq *cq) {
  struct ibv_qp_init_attr qp_attr = {};
  struct ibv_qp *qp;

  printf("\nTesting different QP types:\n");

  // Test different QP types
  enum ibv_qp_type qp_types[] = {IBV_QPT_RC, IBV_QPT_UD, IBV_QPT_RAW_PACKET};
  const char *qp_names[] = {"RC", "UD", "RAW_PACKET"};

  for (int i = 0; i < 3; i++) {
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = qp_types[i];
    qp_attr.cap.max_send_wr = 1;
    qp_attr.cap.max_recv_wr = 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    qp = ibv_create_qp(pd, &qp_attr);
    if (qp) {
      printf("  ✓ %s QP creation: SUCCESS\n", qp_names[i]);
      ibv_destroy_qp(qp);
    } else {
      printf("  ✗ %s QP creation: FAILED (errno: %d, %s)\n", qp_names[i], errno,
             strerror(errno));
    }
  }

  return 0;
}

int main() {
  struct ibv_device **dev_list;
  struct ibv_context *context;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  int num_devices;

  printf("=== RDMA Device Debug Info ===\n\n");

  // Get device list
  dev_list = ibv_get_device_list(&num_devices);
  if (!dev_list || num_devices == 0) {
    fprintf(stderr, "No RDMA devices found\n");
    return 1;
  }

  printf("Found %d RDMA device(s):\n", num_devices);
  for (int i = 0; i < num_devices; i++) {
    printf("  %d: %s (%s)\n", i, ibv_get_device_name(dev_list[i]),
           ibv_node_type_str(dev_list[i]->node_type));
  }
  printf("\n");

  // Test first device
  context = ibv_open_device(dev_list[0]);
  if (!context) {
    fprintf(stderr, "Failed to open device\n");
    ibv_free_device_list(dev_list);
    return 1;
  }

  printf("Testing device: %s\n\n", ibv_get_device_name(dev_list[0]));

  // Print device capabilities
  print_device_caps(context);
  printf("\n");

  // Print port info
  print_port_info(context, 1);
  printf("\n");

  // Create basic resources for QP testing
  pd = ibv_alloc_pd(context);
  if (!pd) {
    fprintf(stderr, "Failed to allocate PD\n");
    ibv_close_device(context);
    ibv_free_device_list(dev_list);
    return 1;
  }

  cq = ibv_create_cq(context, 10, NULL, NULL, 0);
  if (!cq) {
    fprintf(stderr, "Failed to create CQ\n");
    ibv_dealloc_pd(pd);
    ibv_close_device(context);
    ibv_free_device_list(dev_list);
    return 1;
  }

  // Test QP creation
  test_qp_creation(context, pd, cq);

  // Cleanup
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(context);
  ibv_free_device_list(dev_list);

  return 0;
}
