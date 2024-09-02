#include "global.h"
#include "tju_tcp.h"

int TimerStopped = 1;
int ConnTimerStopped = 1;

int CloseInitiated = 0; 

// TCP连接中的超时重传和发送
/*
* 重传处理函数，TCP数据包传输超时调用
*/
void retrans_handler(tju_tcp_t* in_sock) {
    static tju_tcp_t* sock = NULL;
    if (in_sock != NULL) {
        sock = in_sock;
        return;
    }
    else {
        RETRANS = 1;
      }
}

/*
* 信号处理函数，定时器超时被调用
*/
void timeout_handler(int signo) {
    retrans_handler(NULL);
    return;
}

/*
* 启动定时器，监控TCP连接中数据包的传输时间
*/
void startTimer(tju_tcp_t* sock) {
    if (ConnTimerStopped == 0) // signal is already registered by conn_timer
    { return;  }
    TimerStopped = 0;
    struct itimerval tick;
    RETRANS = 0;
    retrans_handler(sock);
    signal(SIGALRM, timeout_handler);
    
    tick.it_value.tv_sec = 0;
    tick.it_value.tv_usec = 50000;
    tick.it_interval.tv_sec = 0;
    tick.it_interval.tv_usec = 0;

    if (setitimer(ITIMER_REAL, &tick, NULL) < 0)
        printf("Set timer failed!\n");
    return;
}

/*
* 停止当前定时器
*/
void stopTimer(void) {
    TimerStopped = 1;
    RETRANS = 0;
    struct itimerval value;
    value.it_value.tv_sec = 0;
    value.it_value.tv_usec = 0;
    value.it_interval.tv_sec = 0;
    value.it_interval.tv_usec = 0;

    setitimer(ITIMER_REAL, &value, NULL);

    return;
}

/*
* 发送线程函数，将数据从缓冲区发送到网络层
* 一定情况下，会分段发送数据，启动定时器监控传输状态
*/
void* sending_thread(void* arg) {
    int hashval = *((int*)arg);
    tju_tcp_t* sock = established_socks[hashval];
    while (1) {
    sending_thread_loop_start:

        if (CloseInitiated) // connection close initiated. don't send any more data
            return NULL;

        sock->window.wnd_send->swnd = min(sock->window.wnd_send->cwnd, sock->window.wnd_send->rwnd);
        uint32_t size = 28 * MAX_DLEN;
        uint32_t base = sock->window.wnd_send->base;
        uint32_t nextseq = sock->window.wnd_send->nextseq;


        if (sock->sent_len < sock->sending_len && nextseq < base + size) {
            while (pthread_mutex_lock(&(sock->send_lock)) != 0); // 给发送缓冲区加锁

            if (sock->sending_len - sock->sent_len <= size - (nextseq - base)) {

                while (sock->sending_len - sock->sent_len > MAX_DLEN) {
                    char* msg;
                    uint32_t seq = nextseq;
                    uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;


                    tju_packet_t* pkt = create_packet(sock->established_local_addr.port,
                        sock->established_remote_addr.port, seq, 1,
                        DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                        sock->sending_buf + sock->sent_len, MAX_DLEN);

                    msg = packet_to_buf(pkt);

                    sendToLayer3(msg, plen);
                    
                    int index = (seq + MAX_DLEN) % 1024;
                    
                    if (base == nextseq) {
                        startTimer(sock);
                    }

                    nextseq += MAX_DLEN;
                    sock->sent_len += MAX_DLEN;

                    free(msg);
                    free_packet(pkt);
                }
                char* msg;
                uint32_t seq = nextseq;
                uint32_t len = sock->sending_len - sock->sent_len;
                uint16_t plen = DEFAULT_HEADER_LEN + (len);

                if (len == 0 && sock->sent_len != 0) {

                    pthread_mutex_unlock(&(sock->send_lock)); // 解锁
                    sock->window.wnd_send->nextseq = nextseq;
                    sock->sent_len = 0;

                    goto sending_thread_loop_start;
                }
                if (len == 0) {

                    pthread_mutex_unlock(&(sock->send_lock)); // 解锁
                    sock->window.wnd_send->nextseq = nextseq;

                    goto sending_thread_loop_start;
                }

                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                    seq, 1,
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                    sock->sending_buf + sock->sent_len, len);
                msg = packet_to_buf(pkt);
                
                sendToLayer3(msg, plen);
                
                if (base == nextseq) {
                    startTimer(sock);
                }
                nextseq += sock->sending_len - sock->sent_len;
                sock->sent_len += sock->sending_len - sock->sent_len;

                sock->window.wnd_send->nextseq = nextseq;


                pthread_mutex_unlock(&(sock->send_lock)); // 解锁

                free(msg);
                free_packet(pkt);
            }
            // 有落在窗口外面的
            else if (sock->sending_len - sock->sent_len > size - (nextseq - base)) {
                while (size - (nextseq - base) > MAX_DLEN) {
                    char* msg;
                    uint32_t seq = nextseq;
                    uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;

                    tju_packet_t* pkt = create_packet(sock->established_local_addr.port,
                        sock->established_remote_addr.port, seq, 1,
                        DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                        sock->sending_buf + sock->sent_len, MAX_DLEN);

                    msg = packet_to_buf(pkt);
                    
                    sendToLayer3(msg, plen);

                    if (base == nextseq) {
                        startTimer(sock);
                    }
                    nextseq += MAX_DLEN;
                    sock->sent_len += MAX_DLEN;

                    free(msg);
                    free_packet(pkt);
                }
                char* msg;
                uint32_t seq = nextseq;
                uint32_t len = size - (nextseq - base);
                uint16_t plen = DEFAULT_HEADER_LEN + len;
                if (len == 0) {
                    pthread_mutex_unlock(&(sock->send_lock)); // 解锁
                    sock->window.wnd_send->nextseq = nextseq;
                    sock->sent_len = sock->sent_len;

                    goto sending_thread_loop_start;

                }

                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                    seq, 1,
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                    sock->sending_buf + sock->sent_len, len);
                msg = packet_to_buf(pkt);
              
                sendToLayer3(msg, plen);
                
                if (base == nextseq) {
                    startTimer(sock);
                }
                nextseq += len;
                sock->sent_len += len;

                sock->window.wnd_send->nextseq = nextseq;

                pthread_mutex_unlock(&(sock->send_lock)); // 解锁

                free(msg);
                free_packet(pkt);
            }
        }
    }
}

/*
* 重传线程函数，检测到数据包超时会重传
*/
void* retrans_thread(void* arg) {
    int hashval = *((int*)arg);
    tju_tcp_t* sock = established_socks[hashval];
 
    while (1) {
        if (CloseInitiated) // close initiated, exit
            return NULL;
        if (TimerStopped == 0)
        {
            unsigned int remain_time = ualarm(10000, 0);
            ualarm(remain_time, 0);
            if (remain_time == 0) {
                TimerStopped = 1;
                raise(SIGALRM);
            }
        }

        if (RETRANS) {
            RETRANS = 0;

            while (pthread_mutex_lock(&(sock->send_lock)) != 0); // 给发送缓冲区加锁

            uint32_t retrans_size = sock->window.wnd_send->nextseq - sock->window.wnd_send->base;
            uint32_t retransed_size = 0;

            // 需发送的数据大于MAX_DLEN
            if (retrans_size > MAX_DLEN) {  // TODO retrans

                char* msg;
                uint32_t seq = sock->window.wnd_send->base + retransed_size;
                uint16_t plen = DEFAULT_HEADER_LEN + MAX_DLEN;

                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                    seq, 1,
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                    sock->sending_buf + retransed_size, MAX_DLEN);

                msg = packet_to_buf(pkt);
                
                sendToLayer3(msg, plen);
           
                if (retransed_size == 0) {
                    startTimer(sock);
                }

                retransed_size += MAX_DLEN;
                retrans_size -= MAX_DLEN;

                free(msg);
                free_packet(pkt);

            }
            else {


                char* msg;
                uint32_t seq = sock->window.wnd_send->base + retransed_size;
                uint32_t len = retrans_size;
                uint16_t plen = DEFAULT_HEADER_LEN + len;

                tju_packet_t* pkt = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port,
                    seq, 1,
                    DEFAULT_HEADER_LEN, plen, NO_FLAG, 32, 0,
                    sock->sending_buf + retransed_size, len);

                msg = packet_to_buf(pkt);
               
                sendToLayer3(msg, plen);
                
                if (retransed_size == 0) {
                    startTimer(sock);
                }

                retransed_size += len;
                retrans_size -= len;
               
                free(msg);
                free_packet(pkt);
            }
            pthread_mutex_unlock(&(sock->send_lock)); // 解锁   // TODO retrans
        }
    }
}


// TCP连接建立过程中的超时重传
/*
 * 信号处理函数，处理TCP连接建立中的超时
 */
void conn_timeout_handler(int signo) {
    if (ConnTimerStopped)
        return;
   
    conn_retrans_handler(NULL);
}

/* 启动定时器 */
void conn_startTimer() {
    ConnTimerStopped = 0;
    alarm(1); // TODO
    signal(SIGALRM, conn_timeout_handler);
}

/* 停止之前启动的定时器 */
void conn_stopTimer() {
    ConnTimerStopped = 1;
    alarm(0); //// TODO
}

/*
 * 连接建立超时时，重新发送SYN或者SYN-ACK报文
 */
void conn_retrans_handler(tju_tcp_t* in_sock) {
    static tju_tcp_t* sock = NULL;
    if (in_sock != NULL) {
        sock = in_sock;
        return;
    } else {
        tcp_connection_management_message_to_layer3(sock->established_local_addr.port, sock->established_remote_addr.port, 0, 0, 0, CONN_MODE_RESEND);
        conn_startTimer();
    }
}

/**
 * 初始化并启动连接连接重传机制
 */
void create_conn_retrans_thread(tju_tcp_t* sock) {
    conn_startTimer(); // 设置超时信号处理器
    signal(SIGALRM, conn_timeout_handler);  // 启动定时器
    conn_retrans_handler(sock); // 初始化处理重传
}

/*
 * TCP连接成功建立后调用，终止重传的定时器和相关线程
 */
void terminate_conn_timer_and_thread(tju_tcp_t* sock) {
    conn_stopTimer();
}

// 创建发送和重传线程
int create_sending_and_retrans_thread(int hashval, pthread_t sending_thread_id, pthread_t retrans_thread_id) {
    void* sending_thread_arg = malloc(sizeof(int));
    memcpy(sending_thread_arg, &hashval, sizeof(int));
    int rst1 = pthread_create(&sending_thread_id, NULL, sending_thread, sending_thread_arg);
    if (rst1 < 0) {
        printf("ERROR open sending thread \n");
        exit(-1);
    }

    void* retrans_thread_arg = malloc(sizeof(int));
    memcpy(retrans_thread_arg, &hashval, sizeof(int));
    int rst2 = pthread_create(&retrans_thread_id, NULL, retrans_thread, retrans_thread_arg);
    if (rst2 < 0) {
        printf("ERROR open retrans thread \n");
        exit(-1);
    }
}

