#include "tju_tcp.h"
#include "log.h"
#include "myQueue.c"
#include "retran.c"

// 套接字的关闭状态
int is_closing = 0;

/*
创建 TCP socket
初始化对应的结构体
设置初始状态为 CLOSED
*/
FILE *log_fp;

/*
* 完整的套接字初始化过程，适合在自定义的TCP/IP协议栈中 使用
*/
tju_tcp_t *tju_socket() {
   // 分配空间，初始化
    tju_tcp_t *sock = (tju_tcp_t *) malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_len = 0;
    sock->sent_len = 0; // 发送状态相关的字段
    sock->allto = 0;
    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;
    pthread_mutex_init(&(sock->sending_buffer_empty_lock), NULL);
    if (pthread_cond_init(&sock->wait_cond, NULL) != 0) {
        exit(-1);
    }
    // 为窗口分配内存
    sock->window.wnd_recv = NULL;
    sock->window.wnd_send = (sender_window_t *) malloc(sizeof(sender_window_t));
    sock->window.wnd_recv = (receiver_window_t *) malloc(sizeof(receiver_window_t));
    // 拥塞控制相关变量
    sock->window.wnd_send->base = 1;
    sock->window.wnd_send->nextseq = 1;
    sock->window.wnd_send->rwnd = TCP_RECVWN_SIZE / MAX_DLEN;
    sock->window.wnd_send->cwnd = 1;
    sock->window.wnd_send->ssthresh = 16;
    pthread_mutex_init(&(sock->window.wnd_send->ack_cnt_lock), NULL);
    sock->window.wnd_send->ack_cnt = 0;

    sock->window.wnd_send->timeout.tv_sec = 0;
    sock->window.wnd_send->timeout.tv_usec = 500000;
    sock->window.wnd_send->estmated_rtt = 500000;
    sock->window.wnd_send->dev_rtt = 0;
    sock->window.wnd_recv->expect_seq = 1;

    // 分配缓冲区
    int buf_max_size = 1024 * 1024 * 60; 
    char hostname[8];
    gethostname(hostname, 8);
    if (strcmp(hostname, "server") == 0)
        sock->received_buf = (char *) malloc(buf_max_size); 
    else if (strcmp(hostname, "client") == 0)
        sock->sending_buf = (char *) malloc(buf_max_size); 

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t *sock, tju_sock_addr bind_addr) {
    int hash = bind_addr.port;

    // 判断端口是否可用
    if (bhash[hash] == 0) {
        bhash[hash] = 1; // set bhash to 1
        sock->bind_addr = bind_addr;
        return 0;
    } else {
        return -1; // port is not available
    }
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t *sock) {
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
   
    listen_socks[hashval] = sock;
    initQueue(&(sock->socket_queue.half_conn_socks));
    initQueue(&(sock->socket_queue.fully_conn_socks));
    return 0;
}

/**
 * 接受连接
 * 返回与客户端通信用的socket
 * 这里返回的socket一定是已经完成3次握手建立了连接的socket
 * 因为只要该函数返回, 用户就可以马上使用该socket进行send和recv,
 * 也就是说，无可连接socket时阻塞等待，一旦建立一个则返回成功的socket
 */
tju_tcp_t *tju_accept(tju_tcp_t *listen_sock) {
    // 如果LISTEN的socket全连接队列有队列（阻塞等待）
    while (isQueueEmpty(&(listen_sock->socket_queue.fully_conn_socks)));     
    // 取出全连接队列中的节点
    DataNode *new_conn_node = popQueue(&(listen_sock->socket_queue.fully_conn_socks)); 
    // 返回该节点
    tju_tcp_t *new_conn = new_conn_node->socket_ptr;
   return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t *sock, tju_sock_addr target_addr) {

    // 首先为socket分配一个空闲的端口
    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network(CLIENT_IP);
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;

    // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet
    //    sock->state = ESTABLISHED;

    // 将SYN发送给服务端
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock; // note that established_socks doesn't
    tcp_connection_management_message_to_layer3(local_addr.port, target_addr.port,
                                                CLIENT_CONN_SEQ, 0, SYN_FLAG_MASK, CONN_MODE_SEND);

    // 初始化并启动连接重传机制
     create_conn_retrans_thread(sock);

    //改变状态
    sock->state = SYN_SENT;

    // 阻塞等待连接
    while (sock->state != ESTABLISHED);

    // TCP连接成功，终止定时器和线程
    terminate_conn_timer_and_thread(sock);

    // 创建发送和重传线程
    create_sending_and_retrans_thread(hashval, 1004, 1005);

    fflush(stdout);
    sleep(1);
    return 0;
}

int tju_send(tju_tcp_t *sock, const void *buffer, int len) {
    while (pthread_mutex_lock(&(sock->send_lock)) != 0); // 加锁
    // 避免覆盖之前的数据
    memcpy(sock->sending_buf + sock->sending_len, buffer, len);
    sock->sending_len += len;
    sock->allto += len;
    pthread_mutex_unlock(&(sock->send_lock)); // 解锁
    return 0;
}

int tju_recv(tju_tcp_t *sock, void *buffer, int len) {
    while (sock->received_len <= 0) {}
  
    while (pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len) { // 从中读取len长度的数据
        read_len = len;
    } else {
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    sock->received_len -= read_len;
    sock->received_buf += read_len;

    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return read_len;
}

// 关闭连接
void Close(tju_tcp_t *sock) // a wrapper function
{
    do {
        // 将所有建立的端口置空
        int _hashval = cal_hash(sock->established_local_addr.ip, sock->established_local_addr.port,
                                sock->established_remote_addr.ip, sock->established_remote_addr.port);
        established_socks[_hashval] = NULL; // TODO mem leak
    } while (0);
    // 停止所有计时器
    conn_stopTimer();
}

int TimeWaitTimeout = 0;
tju_tcp_t *time_wait_sock = NULL;
// 对TIMEWAIT时产生的信号进行处理
void time_wait_handler(int signo) {
    TimeWaitTimeout = 1;
    time_wait_sock->state = CLOSED;
    // 释放相关的所有资源
    Close(time_wait_sock);
}

int tju_handle_packet(tju_tcp_t *sock, char * pkt) {
    static int duplicate_ack_count = 0;

    uint32_t pkt_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
    uint8_t pkt_flag = get_flags(pkt);
    uint32_t seq_num = get_seq(pkt);
    uint32_t ack_num = get_ack(pkt);
    uint16_t pkt_adv_win = get_advertised_window(pkt);
    // 状态机
    switch (sock->state) {
        // 三次握手，连接建立
        // 监听
        case LISTEN:
            do {
                // 从半连接队列中取出一个报文
                int _hashval = cal_hash(sock->established_local_addr.ip, sock->established_local_addr.port, sock->established_remote_addr.ip, sock->established_remote_addr.port);
                DataNode *_node = pop_via_hashval(&(sock->socket_queue.half_conn_socks), _hashval);
                if (_node == NULL) {
                    // 节点为空，当前没有对应半连接socket
                    if (pkt_flag & SYN_FLAG_MASK) {
                        // 第一次握手的响应
                        // 收到SYN，发送SYN-ACK
                        tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port,
                        SERVER_CONN_SEQ, seq_num + 1, SYN_FLAG_MASK | ACK_FLAG_MASK, CONN_MODE_SEND);

                        // 创建半连接socket，重传线程
                        tju_tcp_t *new_halfconn_sock = tju_socket();
                        memcpy(new_halfconn_sock, sock, sizeof(tju_tcp_t));
                        new_halfconn_sock->established_remote_addr = sock->established_remote_addr; // soft copy, since struct has no ptr
                        new_halfconn_sock->established_local_addr = sock->established_local_addr;
                        create_conn_retrans_thread(sock);
                        // 修改状态
                        new_halfconn_sock->state = SYN_RECV;

                        enqueue(&(sock->socket_queue.half_conn_socks), new_halfconn_sock);
                    } else {}
             } else { // 节点不为空，有半连接队列，已经有了前两次握手
                    // 收到第三次握手，就是SYN_RECV时收到ACK-FLAG的状态
                    if (pkt_flag & ACK_FLAG_MASK) 
                    {
                         tju_tcp_t *new_fully_sock = _node->socket_ptr;
                         // 连接建立之后，终止重传的计时器和相关线程
                         terminate_conn_timer_and_thread(sock);
                         // 进入ESTABLISH装态，同时入队
                        new_fully_sock->state = ESTABLISHED;
                        enqueue(&(sock->socket_queue.fully_conn_socks), new_fully_sock);
                    } else if (pkt_flag & SYN_FLAG_MASK)
                    { // 收到SYN-FALG，第二次握手的ACKclient端没有收到
                       // 重新入队，重新发送SYN-ACK，同时重启计时器
                        enqueue(&(sock->socket_queue.half_conn_socks), _node->socket_ptr);
                        tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port, 0, 0,  0, CONN_MODE_RESEND);
                        conn_stopTimer();
                        conn_startTimer(); 
                    } else { }
              }
            } while (0);
            break;
        case SYN_SENT:
            // 成功收到SYN和ACK，进行第二次握手的接收
            if (pkt_flag & SYN_FLAG_MASK && pkt_flag & ACK_FLAG_MASK) {
                tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port,
                ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_SEND);
                terminate_conn_timer_and_thread(sock);
                 sock->state = ESTABLISHED;
            }else {}
            break;
           case SYN_RECV:
                    if (pkt_flag & ACK_FLAG_MASK) /// received SYN-ACK-ACK from client, handshake 3
                       {
                            /// dequeue and enqueue
                            int hashval = cal_hash(sock->established_local_addr.ip,sock->established_local_addr.port,
                                                   sock->established_remote_addr.ip,sock->established_remote_addr.port);
                            DataNode *node = pop_via_hashval(&(sock->socket_queue.half_conn_socks), hashval);
                            tju_tcp_t * new_fully_sock = node->socket_ptr;
                            new_fully_sock->state = ESTABLISHED;
                            enqueue(&(sock->socket_queue.fully_conn_socks), new_fully_sock);
                            /// state transition
                            sock->state = ESTABLISHED;
                        } 
                        break;

            /*ESTABLISHED时可能发生的情况*/
        case ESTABLISHED:
            // 接收FIN 发送ACK，状态变为close_wait
            if (pkt_flag & FIN_FLAG_MASK) 
            {
                is_closing = 1;
                tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
                                                            sock->established_remote_addr.port,
                                                            ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_SEND);
                /// state transition
                sock->state = CLOSE_WAIT;
                goto CLOSE_WAIT_lbl;
                // 发送了SYN-ACK作为第二步响应，服务器没有收到重传
            } else if (pkt_flag & SYN_FLAG_MASK && pkt_flag & ACK_FLAG_MASK) 
            {
                tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port, 0, 0,  0, CONN_MODE_RESEND);  
            } else if (pkt_flag == NO_FLAG) {
               // 收到没有标志位的数据包
                while (pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁
                // 数据包的序列号和服务器窗口序列号匹配时
                // 将数据包的内容复制到接收缓冲区，更新序列号
                if (seq_num == sock->window.wnd_recv->expect_seq) {
                    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
                    memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
                    sock->received_len += data_len;
                    sock->window.wnd_recv->expect_seq = seq_num + data_len;

                    uint32_t seq = sock->window.wnd_send->nextseq;
                    uint32_t ack = sock->window.wnd_recv->expect_seq;
                    char *msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                                                  seq, ack,  DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK,
                                                  (TCP_RECVWN_SIZE - sock->received_len) / MAX_DLEN, 0, NULL, 0);
                    sendToLayer3(msg, DEFAULT_HEADER_LEN);
                    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
                    free(msg);
                    return 0;
                } else { 
                    // 确认机制，数据包内容不匹配时
                    uint32_t seq = sock->window.wnd_send->nextseq;
                    uint32_t ack = sock->window.wnd_recv->expect_seq;

                    tju_packet_t *pkt = create_packet(sock->established_local_addr.port,  sock->established_remote_addr.port, seq, ack,
                                                      DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK,  (TCP_RECVWN_SIZE - sock->received_len) / MAX_DLEN, 0, NULL, 0);
                    char *msg = packet_to_buf(pkt);
                    sendToLayer3(msg, DEFAULT_HEADER_LEN);  // ACK
                     pthread_mutex_unlock(&(sock->recv_lock)); // 解锁
                     free(msg);
                     free_packet(pkt);
                     return 0;
                }
            }
                // 收到的是ACK报文
            else if (pkt_flag == ACK_FLAG_MASK) {
                while (pthread_mutex_lock(&(sock->send_lock)) != 0); 
                if (ack_num < sock->window.wnd_send->base) { }
                  // 表示开始收到重复ACK
                else if (ack_num == sock->window.wnd_send->base) {
                    duplicate_ack_count++;
                    if(duplicate_ack_count >= 3)  {
                       duplicate_ack_count = 0;
                        RETRANS=1;
                     }
                } else {
                    duplicate_ack_count = 0; 
                    uint32_t free_len = ack_num - sock->window.wnd_send->base;
                    sock->window.wnd_send->base = ack_num; 
                    if (sock->window.wnd_send->base == sock->window.wnd_send->nextseq) {
                        stopTimer();
                    } else {
                        stopTimer();
                        startTimer(sock);
                    }
                    sock->sending_len -= free_len;
                    sock->sent_len -= free_len;
                    sock->sending_buf += free_len; 
                    sock->window.wnd_send->rwnd = pkt_adv_win;
                   }
                pthread_mutex_unlock(&(sock->send_lock)); // 解锁
                return 0;
            }
            break;
            ///***************************//
            /// closing connection ---------------------------------------------------------------------------------------------------
        case FIN_WAIT_1:
            // 接收第二次挥手
            if (pkt_flag & ACK_FLAG_MASK && !(pkt_flag & FIN_FLAG_MASK)) 
            {
                conn_stopTimer();
                sock->state = FIN_WAIT_2;
                // 没有ACK的FIN，状态变为CLOSING
            } else if (!(pkt_flag & ACK_FLAG_MASK) && pkt_flag & FIN_FLAG_MASK) 
            {
               tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
                                                            sock->established_remote_addr.port,
                                                            ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_SEND);
                sock->state = CLOSING;
                // 二三次挥手重叠
            } else if (pkt_flag & ACK_FLAG_MASK && pkt_flag & FIN_FLAG_MASK) 
            {
                 tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
                                                            sock->established_remote_addr.port,
                                                            ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_SEND);
                sock->state = TIME_WAIT;
                goto TIME_WAIT_lbl;
            }
            break;
        case FIN_WAIT_2:
            // 收到第三次挥手
            if (pkt_flag & FIN_FLAG_MASK) 
            {
                tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
                                                            sock->established_remote_addr.port,
                                                            ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_SEND);
                
                sock->state = TIME_WAIT;
                goto TIME_WAIT_lbl;
            }
            break;
        case TIME_WAIT: // handles incoming packet(incoming FIN, send ACK)
            tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port,
                    ack_num, seq_num + 1, ACK_FLAG_MASK, CONN_MODE_RESEND);
            return 0;
        TIME_WAIT_lbl:
            // 设置定时器，注册信号处理函数
            alarm(5);// TODO, should be 2 MSL, whatever
            signal(SIGALRM, time_wait_handler);
            time_wait_sock = sock;
            return 0;
        CLOSE_WAIT_lbl:
            // 等待所有待发送的数据被发送，然后发送一个FIN-ACK数据包，并开始超时重传机制。
             while (sock->sending_len != 0) {}
            CloseInitiated = 1;
            tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
                                                        sock->established_remote_addr.port,
                                                        ack_num, seq_num + 1, FIN_FLAG_MASK|ACK_FLAG_MASK, CONN_MODE_SEND);
            stopTimer();
            create_conn_retrans_thread(sock);
            conn_startTimer();
            
            sock->state = LAST_ACK;
            break;
        case LAST_ACK:
            // 收到第四次挥手
            if (pkt_flag & ACK_FLAG_MASK) 
            {
                /// state transition
                sock->state = CLOSED;
                goto CLOSED_lbl;
            }
            break;
        case CLOSING:
            if (pkt_flag & ACK_FLAG_MASK) {
                /// state transition
                sock->state = TIME_WAIT;
                goto TIME_WAIT_lbl;
            }
            break;
        CLOSED_lbl:
            Close(sock);
            return 0;
        default:
           
            return -1;
    }
    return 0;
}

/*
* 四次挥手
*/
int tju_close(tju_tcp_t *sock) {

    is_closing = 1;
    while (sock->sending_len != 0) { }
    CloseInitiated = 1;
    stopTimer();
    create_conn_retrans_thread(sock);
    conn_startTimer(); // start connection management timer
   
    tcp_connection_management_message_to_layer3(sock->established_local_addr.port,
       sock->established_remote_addr.port, 0, 0,  FIN_FLAG_MASK, CONN_MODE_SEND);
    /// state transition
    sock->state = FIN_WAIT_1;

    while (sock->state != CLOSED); // block until CLOSED

    return 0;
}

// tcp连接，发送到应用层
void tcp_connection_management_message_to_layer3(uint16_t src_port, uint16_t dst_port, uint32_t seqnum, uint32_t acknum,
                                                 uint8_t flag, uint8_t trans_control_flag) {
    static tju_packet_t *last_pkt = NULL;
    tju_packet_t *pkt;
    if (trans_control_flag == CONN_MODE_SEND) { 
        pkt = create_packet(src_port, dst_port, seqnum, acknum, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, flag, 1, 0, NULL, 0);
    } else  {
        pkt = (tju_packet_t *) malloc(sizeof(tju_packet_t));
        memcpy(pkt, last_pkt, sizeof(tju_packet_t)); // copy last packet
    }

    char *msg = packet_to_buf(pkt);
    sendToLayer3(msg, DEFAULT_HEADER_LEN);
    if (last_pkt != NULL) {
        free(last_pkt);
    }
    last_pkt = pkt;
    free(msg);
}
