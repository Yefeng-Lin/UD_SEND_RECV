#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024

void die(const char *reason) {
    perror(reason);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *listener = NULL;
    struct rdma_cm_event *event = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct rdma_conn_param conn_param = {0};
    char *buffer = NULL;

    ec = rdma_create_event_channel();
    if (!ec) die("rdma_create_event_channel");

    if (rdma_create_id(ec, &listener, NULL, RDMA_PS_UDP)) {
        die("rdma_create_id");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);  // 监听端口

    if (rdma_bind_addr(listener, (struct sockaddr *)&addr)) {
        die("rdma_bind_addr");
    }

    if (rdma_listen(listener, 1)) {
        die("rdma_listen");
    }

    printf("等待连接...\n");

    if (rdma_get_cm_event(ec, &event)) {
        die("rdma_get_cm_event");
    }

    if (event->event != RDMA_CM_EVENT_CONNECT_REQUEST) {
        die("非连接请求事件");
    }

    struct rdma_cm_id *id = event->id;
    rdma_ack_cm_event(event);

    pd = ibv_alloc_pd(id->verbs);
    if (!pd) die("ibv_alloc_pd");

    cq = ibv_create_cq(id->verbs, 10, NULL, NULL, 0);
    if (!cq) die("ibv_create_cq");

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_UD;
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(id, pd, &qp_attr)) {
        die("rdma_create_qp");
    }

    buffer = malloc(BUFFER_SIZE + sizeof(struct ibv_grh));
    if (!buffer) die("无法分配内存");

    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) die("ibv_reg_mr");

    struct ibv_sge sge;
    sge.addr = (uintptr_t)buffer;
    sge.length = BUFFER_SIZE;
    sge.lkey = mr->lkey;

    struct ibv_recv_wr recv_wr = {0}, *bad_recv_wr = NULL;
    recv_wr.sg_list = &sge;
    recv_wr.num_sge = 1;

    if (ibv_post_recv(id->qp, &recv_wr, &bad_recv_wr)) {
        die("ibv_post_recv");
    }

    conn_param.responder_resources = 1;
    conn_param.initiator_depth = 1;
    if (rdma_accept(id, &conn_param)) {
        die("rdma_accept");
    }

    printf("等待消息...\n");

    struct ibv_wc wc;
    printf("准备接收...\n");
    while (ibv_poll_cq(cq, 1, &wc) >= 0) {
        if (wc.status == IBV_WC_SUCCESS) {
            printf("接收到消息: %s\n", buffer + 40);
            break;
        }
    }

    rdma_disconnect(id);
    rdma_destroy_qp(id);
    ibv_dereg_mr(mr);
    free(buffer);
    rdma_destroy_id(id);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(listener);
    rdma_destroy_event_channel(ec);

    return 0;
}
