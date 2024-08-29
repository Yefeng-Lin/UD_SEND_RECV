#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>
#include <infiniband/verbs.h>

#define BUFFER_SIZE 1024

struct ibv_ah       *ah;
uint32_t        remote_qpn;
uint32_t        remote_qkey;

void die(const char *reason) {
    perror(reason);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv) {
    struct rdma_event_channel *ec = NULL;
    struct rdma_cm_id *conn = NULL;
    struct rdma_cm_event *event = NULL;
    struct ibv_pd *pd = NULL;
    struct ibv_cq *cq = NULL;
    struct ibv_qp_init_attr qp_attr;
    struct rdma_conn_param conn_param = {0};
    char *buffer = NULL;

    ec = rdma_create_event_channel();
    if (!ec) die("rdma_create_event_channel");

    if (rdma_create_id(ec, &conn, NULL, RDMA_PS_UDP)) {
        die("rdma_create_id");
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);  // 服务器端口
    addr.sin_addr.s_addr = inet_addr("11.2.4.9");  // 服务器IP

    if (rdma_resolve_addr(conn, NULL, (struct sockaddr *)&addr, 2000)) {
        die("rdma_resolve_addr");
    }

    if (rdma_get_cm_event(ec, &event)) {
        die("rdma_get_cm_event");
    }

    if (event->event != RDMA_CM_EVENT_ADDR_RESOLVED) {
        die("地址解析失败");
    }

    rdma_ack_cm_event(event);

    if (rdma_resolve_route(conn, 2000)) {
        die("rdma_resolve_route");
    }

    if (rdma_get_cm_event(ec, &event)) {
        die("rdma_get_cm_event");
    }

    if (event->event != RDMA_CM_EVENT_ROUTE_RESOLVED) {
        die("路由解析失败");
    }

    rdma_ack_cm_event(event);

    pd = ibv_alloc_pd(conn->verbs);
    if (!pd) die("ibv_alloc_pd");

    cq = ibv_create_cq(conn->verbs, 10, NULL, NULL, 0);
    if (!cq) die("ibv_create_cq");

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = cq;
    qp_attr.recv_cq = cq;
    qp_attr.qp_type = IBV_QPT_UD;
    qp_attr.cap.max_send_wr = 10;
    qp_attr.cap.max_recv_wr = 10;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;

    if (rdma_create_qp(conn, pd, &qp_attr)) {
        die("rdma_create_qp");
    }

    buffer = malloc(BUFFER_SIZE);
    if (!buffer) die("无法分配内存");

    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, BUFFER_SIZE, IBV_ACCESS_LOCAL_WRITE);
    if (!mr) die("ibv_reg_mr");

    conn_param.initiator_depth = 1;
    conn_param.responder_resources = 1;
    if (rdma_connect(conn, &conn_param)) {
        die("rdma_connect");
    }

    // 调用 rdma_get_cm_event 来等待 RDMA_CM_EVENT_ESTABLISHED 事件
    if (rdma_get_cm_event(ec, &event)) {
        die("rdma_get_cm_event");
    }

    if (event->event != RDMA_CM_EVENT_ESTABLISHED) {
        die("连接未建立");
    }
    remote_qpn = event->param.ud.qp_num;
    remote_qkey = event->param.ud.qkey;
    ah = ibv_create_ah(pd, &event->param.ud.ah_attr);

    rdma_ack_cm_event(event);

    strcpy(buffer, "Hello, RDMA!");

    struct ibv_sge sge;
    sge.addr = (uintptr_t)buffer;
    sge.length = strlen(buffer) + 1;
    sge.lkey = mr->lkey;

    struct ibv_send_wr send_wr = {0}, *bad_send_wr = NULL;
    send_wr.sg_list = &sge;
    send_wr.num_sge = 1;
    send_wr.opcode = IBV_WR_SEND;
    send_wr.send_flags = IBV_SEND_SIGNALED;

    send_wr.wr.ud.ah = ah;
    send_wr.wr.ud.remote_qkey = remote_qkey;
    send_wr.wr.ud.remote_qpn = remote_qpn;

    int ret = ibv_post_send(conn->qp, &send_wr, &bad_send_wr);
    if (ret) {
        fprintf(stderr, "ibv_post_send error: %d\n", ret);
        die("ibv_post_send");
    }

    printf("消息已发送: %s\n", buffer);

    rdma_disconnect(conn);
    rdma_destroy_qp(conn);
    ibv_dereg_mr(mr);
    free(buffer);
    rdma_destroy_id(conn);
    ibv_dealloc_pd(pd);
    rdma_destroy_event_channel(ec);

    return 0;
}
