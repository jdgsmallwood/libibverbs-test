#include <cstdlib>
#include <cstring>
#include <infiniband/verbs.h>
#include <iostream>

int main() {
  // 1. Open device
  std::cout << "Opening device\n";
  ibv_device **dev_list = ibv_get_device_list(nullptr);
  if (!dev_list) {
    perror("ibv_get_device_list");
    return 1;
  }
  std::cout << "Available devices:\n";
  for (int i = 0; dev_list[i]; ++i) {
    std::cout << "  [" << i << "] " << ibv_get_device_name(dev_list[i]) << "\n";
  }
  ibv_context *ctx = ibv_open_device(dev_list[0]);
  if (!ctx) {
    perror("ibv_open_device");
    return 1;
  }
  std::cout << "Opened device: " << ibv_get_device_name(dev_list[0]) << "\n";

  // 2. Allocate protection domain
  std::cout << "Allocating protection domain\n";
  ibv_pd *pd = ibv_alloc_pd(ctx);
  if (!pd) {
    perror("ibv_alloc_pd");
    return 1;
  }

  // 3. Create completion queue
  ibv_cq *send_cq = ibv_create_cq(ctx, 1, nullptr, nullptr, 0);
  if (!send_cq) {
    perror("ibv_create_cq send");
    return 1;
  }
  ibv_cq *recv_cq = ibv_create_cq(ctx, 1, nullptr, nullptr, 0);
  if (!recv_cq) {
    perror("ibv_create_cq recv");
    return 1;
  }

  // 4. Create QP
  std::cout << "Create QP...\n";
  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq = send_cq;
  qp_attr.recv_cq = send_cq;
  qp_attr.qp_type = IBV_QPT_UD;
  qp_attr.cap.max_send_wr = 1;
  qp_attr.cap.max_recv_wr = 1;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ibv_qp *qp1 = ibv_create_qp(pd, &qp_attr);
  if (!qp1) {
    perror("ibv_create_qp qp1");
    return 1;
  }
  qp_attr.send_cq = recv_cq;
  qp_attr.recv_cq = recv_cq;
  ibv_qp *qp2 = ibv_create_qp(pd, &qp_attr);
  if (!qp2) {
    perror("ibv_create_qp qp2");
    return 1;
  }

  // 5. Allocate memory
  std::cout << "Allocate memory...\n";
  char send_buf[2048] = "Hello verbs";
  char recv_buf[2048] = {};

  std::cout << "Register w/ protection domain...\n";
  ibv_mr *send_mr =
      ibv_reg_mr(pd, send_buf, sizeof(send_buf), IBV_ACCESS_LOCAL_WRITE);
  if (!send_mr) {
    perror("ibv_reg_mr send");
    return 1;
  }
  ibv_mr *recv_mr =
      ibv_reg_mr(pd, recv_buf, sizeof(recv_buf),
                 IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!recv_mr) {
    perror("ibv_reg_mr recv");
    return 1;
  }

  std::cout << "Move to init...\n";
  ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = 1;
  attr.qkey = 0x11111111;
  attr.pkey_index = 0;
  if (ibv_modify_qp(qp1, &attr,
                    IBV_QP_STATE | IBV_QP_PORT | IBV_QP_QKEY |
                        IBV_QP_PKEY_INDEX)) {
    perror("ibv_modify_qp qp1 INIT");
    return 1;
  }
  if (ibv_modify_qp(qp2, &attr,
                    IBV_QP_STATE | IBV_QP_PORT | IBV_QP_QKEY |
                        IBV_QP_PKEY_INDEX)) {
    perror("ibv_modify_qp qp2 INIT");
    return 1;
  }

  std::cout << "Move to RTR\n";
  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTR;
  if (ibv_modify_qp(qp1, &attr, IBV_QP_STATE)) {
    perror("ibv_modify_qp qp1 RTR");
    return 1;
  }
  if (ibv_modify_qp(qp2, &attr, IBV_QP_STATE)) {
    perror("ibv_modify_qp qp2 RTR");
    return 1;
  }

  std::cout << "Move to RTS\n";
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = 0;
  if (ibv_modify_qp(qp1, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    perror("ibv_modify_qp qp1 RTS");
    return 1;
  }
  if (ibv_modify_qp(qp2, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN)) {
    perror("ibv_modify_qp qp2 RTS");
    return 1;
  }

  std::cout << "Create receive\n";
  ibv_sge sge_recv{};
  sge_recv.addr = (uintptr_t)recv_buf;
  sge_recv.length = sizeof(recv_buf);
  sge_recv.lkey = recv_mr->lkey;

  ibv_recv_wr recv_wr{};
  recv_wr.sg_list = &sge_recv;
  recv_wr.num_sge = 1;
  recv_wr.next = nullptr;
  ibv_recv_wr *bad_recv;
  if (ibv_post_recv(qp2, &recv_wr, &bad_recv)) {
    perror("ibv_post_recv");
    return 1;
  }

  std::cout << "Create AH...\n";
  union ibv_gid gid;
  if (ibv_query_gid(ctx, 1, 1, &gid)) {
    perror("ibv_query_gid");
    return 1;
  }

  ibv_ah_attr ah_attr{};
  ah_attr.is_global = 1;
  ah_attr.dlid = 0;
  ah_attr.sl = 0;
  ah_attr.port_num = 1;
  ah_attr.grh.dgid = gid;
  ah_attr.grh.flow_label = 0;
  ah_attr.grh.sgid_index = 1;
  ah_attr.grh.hop_limit = 64;
  ah_attr.grh.traffic_class = 0;

  ibv_ah *ah = ibv_create_ah(pd, &ah_attr);
  if (!ah) {
    perror("ibv_create_ah failed");
    return 1;
  }

  printf("GID: ");
  for (int i = 0; i < 16; i++) {
    printf("%02x", gid.raw[i]);
    if (i == 7)
      printf(":");
  }
  printf("\n");

  std::cout << "Create send\n";
  ibv_sge sge_send{};
  sge_send.addr = (uintptr_t)send_buf;
  sge_send.length = sizeof(send_buf);
  sge_send.lkey = send_mr->lkey;

  std::cout << "Create send WR\n";
  ibv_send_wr send_wr{};
  send_wr.sg_list = &sge_send;
  send_wr.num_sge = 1;
  send_wr.opcode = IBV_WR_SEND;
  send_wr.send_flags = IBV_SEND_SIGNALED;
  send_wr.wr.ud.ah = ah;
  send_wr.wr.ud.remote_qpn = qp2->qp_num;
  send_wr.wr.ud.remote_qkey = 0x11111111;
  send_wr.next = nullptr;

  ibv_send_wr *bad_send;
  std::cout << "posting send...\n";
  if (ibv_post_send(qp1, &send_wr, &bad_send)) {
    perror("ibv_post_send");
    return 1;
  }

  std::cout << "Create completion";
  ibv_wc wc;
  int num_completions = 0;
  while (num_completions == 0) {
    num_completions = ibv_poll_cq(recv_cq, 1, &wc);
  }

  std::cout << "Finished...\n";
  std::cout << "Received message: " << recv_buf << std::endl;

  if (num_completions > 0) {
    if (wc.status == IBV_WC_SUCCESS) {
      std::cout << "Completion received successfully!\n";
      std::cout << "WC opcode: " << wc.opcode << std::endl;
      std::cout << "WC byte length: " << wc.byte_len << std::endl;
      std::cout << "WC QP number: " << wc.qp_num << std::endl;
      if (wc.byte_len > 40) {
        std::cout << "Received message: '" << (recv_buf + 40) << "'"
                  << std::endl;
        std::cout << "Raw bytes received: " << (wc.byte_len - 40) << std::endl;
      } else if (wc.byte_len > 0) {
        std::cout << "Received data too short (includes only GRH): "
                  << wc.byte_len << " bytes" << std::endl;
      } else {
        std::cout << "No data received" << std::endl;
      }
    } else {
      std::cout << "Completion ERROR: " << ibv_wc_status_str(wc.status)
                << std::endl;
      std::cout << "WC opcode: " << wc.opcode << std::endl;
      std::cout << "WC vendor error: " << wc.vendor_err << std::endl;
    }
  } else {
    std::cout << "Timeout waiting for completion" << std::endl;
  }

  // Cleanup
  ibv_dereg_mr(send_mr);
  ibv_dereg_mr(recv_mr);
  ibv_destroy_qp(qp1);
  ibv_destroy_qp(qp2);
  ibv_destroy_cq(send_cq);
  ibv_destroy_cq(recv_cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(dev_list);

  return 0;
}
