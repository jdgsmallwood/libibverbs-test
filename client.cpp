#include <cstring>
#include <infiniband/verbs.h>
#include <iostream>
#include <unistd.h>

int main() {
  // 1. Open device

  ibv_device **dev_list = ibv_get_device_list(nullptr);
  std::cout << (*dev_list) << std::endl;
  ibv_context *ctx = ibv_open_device(dev_list[0]);

  // 2. Protection domain
  ibv_pd *pd = ibv_alloc_pd(ctx);

  // 3. Completion queue
  ibv_cq *cq = ibv_create_cq(ctx, 1, nullptr, nullptr, 0);

  // 4. Create RC QP
  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq = cq;
  qp_attr.recv_cq = cq;
  qp_attr.qp_type = IBV_QPT_RC;
  qp_attr.cap.max_send_wr = 1;
  qp_attr.cap.max_recv_wr = 1;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ibv_qp *qp = ibv_create_qp(pd, &qp_attr);

  // 5. Memory
  char buf[16] = "Hello RC";
  ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf), IBV_ACCESS_LOCAL_WRITE);

  // 6. Move QP to INIT
  ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = 1;
  ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_PORT);

  // 7. Print QP number for server
  std::cout << "Client QP number: " << qp->qp_num << std::endl;
  std::cout << "Enter server QP number: ";
  uint32_t server_qpn;
  std::cin >> server_qpn;

  // 8. Move QP to RTR
  attr.qp_state = IBV_QPS_RTR;
  attr.dest_qp_num = server_qpn;
  attr.rq_psn = 0;
  attr.path_mtu = IBV_MTU_256;
  ibv_modify_qp(qp, &attr,
                IBV_QP_STATE | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                    IBV_QP_PATH_MTU);

  // 9. Move QP to RTS
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = 0;
  ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);

  // 10. Post send
  ibv_sge sge{};
  sge.addr = (uintptr_t)buf;
  sge.length = sizeof(buf);
  sge.lkey = mr->lkey;

  ibv_send_wr wr{};
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_SEND;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.next = nullptr;

  ibv_send_wr *bad_wr;
  ibv_post_send(qp, &wr, &bad_wr);

  // 11. Poll completion queue
  ibv_wc wc;
  while (ibv_poll_cq(cq, 1, &wc) == 0) {
  }

  std::cout << "Client sent message." << std::endl;

  // 12. Cleanup
  ibv_dereg_mr(mr);
  ibv_destroy_qp(qp);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(dev_list);
}
