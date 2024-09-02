#include "global.h"
#include "tju_tcp.h"

// socket queue 
/*
 * ��ʼ������
 */
void initQueue(sockQueue* q) {
    q->front = NULL;
    q->rear = NULL;
    q->queue_size = 0;
}

/*
 * ���г���
 */
int QueueLength(sockQueue* q) {
    return q->queue_size;
}

/**
 * �ж϶����Ƿ�Ϊ��
 */
int isQueueEmpty(sockQueue* q) {
    return q->queue_size == 0;
}

/*
 * �ж϶����Ƿ�Ϊ��
 */
int isQueueFull(sockQueue* q) {
    return q->queue_size == MAX_SOCK;
}

/*
 * ���
 */
int enqueue(sockQueue* q, tju_tcp_t* socket_ptr) {
    if (isQueueFull(q)) {
        return -1;
    }
    else {
        DataNode* new_node = (DataNode*)malloc(sizeof(DataNode));
        new_node->socket_ptr = socket_ptr;
        new_node->sock_hashval = cal_hash(socket_ptr->established_local_addr.ip,
            socket_ptr->established_local_addr.port,
            socket_ptr->established_remote_addr.ip,
            socket_ptr->established_remote_addr.port); // TODO may need to fix
        new_node->next = NULL;
        if (isQueueEmpty(q)) // queue is empty
        {
            q->front = new_node;
            q->rear = new_node;
        }
        else {
            q->rear->next = new_node;
        }
        q->queue_size++;
        return 0;
    }
}

/*
 * ����.
 */
DataNode* popQueue(sockQueue* q) {
    if (isQueueEmpty(q)) {
        return NULL;
    }
    else {
        DataNode* temp = q->front;
        q->front = q->front->next;
        q->queue_size--;
        if (q->front == NULL) {
            q->rear = NULL;
        }
        return temp;
    }
}

/*
 * ����ĳ����ϣֵ����
 */
DataNode* pop_via_hashval(sockQueue* q, int hashval) {
    if (isQueueEmpty(q)) {
        return NULL;
    }
    else {
        DataNode* i, * j; // j is the previous node of i   queue-----j----i----
        for (i = q->front, j = NULL; i != NULL; i = i->next, j = i) {
            if (i->sock_hashval == hashval) {
                if (j == NULL) {
                    q->front = i->next;
                }
                else {
                    j->next = i->next;
                }
                if (i->next == NULL) {
                    q->rear = j;
                }

                q->queue_size--;
                return i;
            }
        }

        return NULL;
    }
}