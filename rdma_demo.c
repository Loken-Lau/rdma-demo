#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <infiniband/verbs.h>

// 硬件常量配置
#define PORT 1
#define GID_INDEX 1 // Soft-RoCE 通常使用 Index 1 (IPv4)

// 交换信息的结构体 (用于人工复制粘贴)
struct QPInfo {
    uint32_t qp_num;
    uint16_t lid;
    union ibv_gid gid;
    uint64_t addr;
    uint32_t rkey;
};

// 全局资源容器
struct Context {
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    char *buf;
    struct ibv_port_attr port_attr;
};

// 初始化 RDMA 资源
void init_ctx(struct Context *ctx, int size) {
    // 1. 获取设备列表
    int num_devices;
    struct ibv_device **dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) { perror("Get Device failed"); exit(1); }

    // 2. 打开第一个设备 (通常就是 rxe0)
    ctx->ctx = ibv_open_device(dev_list[0]);
    if (!ctx->ctx) { perror("Open Device failed"); exit(1); }
    printf("Create Context on device: %s\n", ibv_get_device_name(dev_list[0]));

    // 3. 分配保护域 (PD)
    ctx->pd = ibv_alloc_pd(ctx->ctx);

    // 4. 分配内存并注册 (MR)
    // 注意：我们要模拟 Client 直接写 Server，所以权限要开全
    ctx->buf = malloc(size);
    memset(ctx->buf, 0, size); // 清零
    ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size,
                         IBV_ACCESS_LOCAL_WRITE | 
                         IBV_ACCESS_REMOTE_WRITE | 
                         IBV_ACCESS_REMOTE_READ);
    if (!ctx->mr) { perror("Reg MR failed"); exit(1); }

    // 5. 创建完成队列 (CQ)
    ctx->cq = ibv_create_cq(ctx->ctx, 16, NULL, NULL, 0);

    // 6. 创建队列对 (QP)
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = ctx->cq,
        .recv_cq = ctx->cq,
        .cap = { .max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC // 可靠连接 (Reliable Connection)
    };
    ctx->qp = ibv_create_qp(ctx->pd, &qp_attr);
    if (!ctx->qp) { perror("Create QP failed"); exit(1); }
    
    // 获取端口属性 (为了拿 LID/GID)
    ibv_query_port(ctx->ctx, PORT, &ctx->port_attr);
}

// 状态机转换: RESET -> INIT -> RTR -> RTS
void modify_qp(struct Context *ctx, struct QPInfo local, struct QPInfo remote) {
    struct ibv_qp_attr attr;
    int flags;

    // 1. RESET -> INIT
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = PORT;
    attr.pkey_index = 0;
    attr.qp_access_flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE;
    flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    
    if (ibv_modify_qp(ctx->qp, &attr, flags)) { perror("Failed to modify to INIT"); exit(1); }

    // 2. INIT -> RTR (Ready to Receive) - 需要远程信息
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.dest_qp_num = remote.qp_num;
    attr.rq_psn = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    
    // RoCE v2 必须设置 GID
    attr.ah_attr.is_global = 1;
    attr.ah_attr.grh.dgid = remote.gid;
    attr.ah_attr.grh.sgid_index = GID_INDEX;
    attr.ah_attr.grh.hop_limit = 1;
    attr.ah_attr.sl = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = PORT;

    flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
            IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;

    if (ibv_modify_qp(ctx->qp, &attr, flags)) { perror("Failed to modify to RTR"); exit(1); }

    // 3. RTR -> RTS (Ready to Send)
    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = 0;
    attr.max_rd_atomic = 1;
    
    flags = IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | 
            IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC;

    if (ibv_modify_qp(ctx->qp, &attr, flags)) { perror("Failed to modify to RTS"); exit(1); }
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <server|client>\n", argv[0]);
        return 1;
    }
    int is_server = (strcmp(argv[1], "server") == 0);

    struct Context ctx;
    init_ctx(&ctx, 1024); // 申请 1KB 内存

    // 准备本地信息
    struct QPInfo local_info;
    local_info.qp_num = ctx.qp->qp_num;
    local_info.lid = ctx.port_attr.lid;
    local_info.addr = (uintptr_t)ctx.buf;
    local_info.rkey = ctx.mr->rkey;
    ibv_query_gid(ctx.ctx, PORT, GID_INDEX, &local_info.gid);

    // --- 第一步：人工交换信息 ---
    printf("\n========= LOCAL INFO (Copy this to other side) =========\n");
    printf("QPN: %u\n", local_info.qp_num);
    printf("GID_Subnet: %llu\n", (unsigned long long)local_info.gid.global.subnet_prefix);
    printf("GID_Interface: %llu\n", (unsigned long long)local_info.gid.global.interface_id);
    printf("ADDR: %lu\n", local_info.addr);
    printf("RKEY: %u\n", local_info.rkey);
    printf("========================================================\n");

    struct QPInfo remote_info;
    printf("\n>>> Enter REMOTE info (Order: QPN GID_Subnet GID_Interface ADDR RKEY):\n");
    scanf("%u %llu %llu %lu %u", 
        &remote_info.qp_num, 
        (unsigned long long *)&remote_info.gid.global.subnet_prefix,
        (unsigned long long *)&remote_info.gid.global.interface_id,
        &remote_info.addr,
        &remote_info.rkey);

    // --- 第二步：建立连接 (Modify QP) ---
    modify_qp(&ctx, local_info, remote_info);
    printf("QP is in RTS state! Ready to transfer.\n");

    if (is_server) {
        // --- Server 逻辑: 躺平等待 ---
        // 往自己的内存写个初始值，证明内存是我的
        strcpy(ctx.buf, "Server: I am waiting for data...");
        
        printf("Server: Memory content BEFORE: '%s'\n", ctx.buf);
        printf("Server: Waiting 10 seconds for Client to write...\n");
        
        // 轮询 10 次，每次 1 秒，看看内存变了没
        for(int i=0; i<10; i++) {
            sleep(1);
            printf("Server memory [%d]: %s\n", i, ctx.buf);
            if (strncmp(ctx.buf, "Client", 6) == 0) {
                printf("\n🎉 SUCCESS! Data changed detected!\n");
                break;
            }
        }
    } else {
        // --- Client 逻辑: 主动写入 ---
        strcpy(ctx.buf, "Client: Hello RDMA World!"); // 这是本地数据
        
        struct ibv_sge sge;
        sge.addr = (uintptr_t)ctx.buf;
        sge.length = strlen(ctx.buf) + 1;
        sge.lkey = ctx.mr->lkey;

        struct ibv_send_wr wr, *bad_wr;
        memset(&wr, 0, sizeof(wr));
        wr.wr_id = 1;
        wr.opcode = IBV_WR_RDMA_WRITE; // <--- 重点：RDMA WRITE
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.send_flags = IBV_SEND_SIGNALED;
        
        // 目标是 Server 的地址
        wr.wr.rdma.remote_addr = remote_info.addr;
        wr.wr.rdma.rkey = remote_info.rkey;

        printf("Client: Writing '%s' to remote memory...\n", ctx.buf);
        if (ibv_post_send(ctx.qp, &wr, &bad_wr)) {
            perror("Post Send failed");
        }

        // 等待完成
        struct ibv_wc wc;
        while (ibv_poll_cq(ctx.cq, 1, &wc) == 0) {}
        if (wc.status == IBV_WC_SUCCESS) {
            printf("Client: Write Success!\n");
        } else {
            printf("Client: Failed status %d\n", wc.status);
        }
    }

    return 0;
}