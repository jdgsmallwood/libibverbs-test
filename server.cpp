#include <cstring>
#include <infiniband/verbs.h>
#include <iostream>
#include <unistd.h>

int main() {
  // 1. Open device
  ibv_device **dev_list = ibv_get_device_list(nullptr);
  ibv_context *ctx = ibv_open_device(dev_list[0]);

  // 2. Protection domain
  ibv_pd *pd = ibv_alloc_pd(ctx);

  // 3. Completion queue
  ibv_cq *cq = ibv_create_cq(ctx, 1, nullptr, nullptr, 0);

  // 4. Create RC QP
  ibv_qp_init_attr qp_attr{};
  qp_attr.send_cq = cq;
  qp_attr.recv_cq = cq;
  qp_attr.qp_type = IBV_QPT_UD;
  qp_attr.cap.max_send_wr = 1;
  qp_attr.cap.max_recv_wr = 1;
  qp_attr.cap.max_send_sge = 1;
  qp_attr.cap.max_recv_sge = 1;

  ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
  std::cout << "Add mem\n";
  // 5. Memory
  char buf[16 + 40] = {};
  ibv_mr *mr = ibv_reg_mr(pd, buf, sizeof(buf),
                          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

  // 6. Move QP to INIT
  ibv_qp_attr attr{};
  attr.qp_state = IBV_QPS_INIT;
  attr.port_num = 1;
  attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;

  attr.qkey = 0x11111111;
  attr.pkey_index = 0;
  // ibv_modify_qp(qp, &attr,
  //              IBV_QP_STATE | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS |
  //                IBV_QP_PKEY_INDEX);
  //

  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_PORT | IBV_QP_QKEY |
                        IBV_QP_PKEY_INDEX)) {
    perror("ibv_modify_qp qp INIT");
    return 1;
  }

  // Print QP state after each transition
  ibv_qp_attr query_attr;
  ibv_qp_init_attr query_init_attr;
  ibv_query_qp(qp, &query_attr, IBV_QP_STATE, &query_init_attr);
  std::cout << "QP state: " << query_attr.qp_state << std::endl;
  // 7. Post receive
  ibv_sge sge{};
  sge.addr = (uintptr_t)buf;
  sge.length = sizeof(buf) + 40;
  sge.lkey = mr->lkey;

  ibv_recv_wr rr{};
  rr.sg_list = &sge;
  rr.num_sge = 1;
  rr.next = nullptr;
  ibv_recv_wr *bad_rr;
  ibv_post_recv(qp, &rr, &bad_rr);

  // 8. Print connection info for client
  std::cout << "Server QP number: " << qp->qp_num << std::endl;
  std::cout << "Send this to client" << std::endl;

  uint32_t client_qpn;
  std::cout << "Enter client QP number: ";
  std::cin >> client_qpn;

  // 9. Move QP to RTR
  //
  ibv_gid my_gid;
  ibv_query_gid(ctx, 1, 1, &my_gid);

  attr.qp_state = IBV_QPS_RTR;
  attr.dest_qp_num = client_qpn;
  attr.rq_psn = 0;
  attr.path_mtu = IBV_MTU_256;
  attr.ah_attr.is_global = 1;      // Enable global routing
  attr.ah_attr.grh.dgid = my_gid;  // Destination GID
  attr.ah_attr.grh.sgid_index = 1; // Source GID index (your -g 1)
  attr.ah_attr.grh.hop_limit = 1;  // Or appropriate hop limit
  attr.ah_attr.port_num = 1;
  attr.ah_attr.sl = 0; // Service level

  ibv_modify_qp(qp, &attr,
                IBV_QP_STATE); //| IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
                               // IBV_QP_PATH_MTU | IBV_QP_AV);

  ibv_query_qp(qp, &query_attr, IBV_QP_STATE, &query_init_attr);
  std::cout << "QP state: " << query_attr.qp_state << std::endl;
  // 10. Move QP to RTS
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = 0;
  ibv_modify_qp(qp, &attr, IBV_QP_STATE | IBV_QP_SQ_PSN);

  ibv_query_qp(qp, &query_attr, IBV_QP_STATE, &query_init_attr);
  std::cout << "QP state: " << query_attr.qp_state << std::endl;
  // 11. Poll completion queue
  ibv_wc wc;
  while (ibv_poll_cq(cq, 1, &wc) == 0) {
  }

  std::cout << "Server received: " << buf + 40 << std::endl;
  std::cout << "Completion: status=" << wc.status << " opcode=" << wc.opcode
            << " vendor_err=" << wc.vendor_err << std::endl;
  // 12. Cleanup
  ibv_dereg_mr(mr);
  ibv_destroy_qp(qp);
  ibv_destroy_cq(cq);
  ibv_dealloc_pd(pd);
  ibv_close_device(ctx);
  ibv_free_device_list(dev_list);
}
