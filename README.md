# tju_tcp

# TCP



## 一 项目环境搭建

### 项目环境：

**vagrant：**

Vagrant 是一个开源工具，主要用于创建和管理轻量级、可移植的开发环境。通过 Vagrant，你可以快速创建虚拟机，并在其中配置和运行你的应用程序，而无需担心与主机操作系统的兼容性问题。

**VirtualBox：**

VirtualBox 是一个免费的开源虚拟化软件，允许用户在单一的主机操作系统上运行多个操作系统。它由 Oracle 公司开发和维护，是 Vagrant 最常用的虚拟化提供商之一。



该项目中，采用的是vagrant的2.2.18版本和virtualBox的6.1.26版本。

**vagrant**使用**virtualBox**作为虚拟化提供商从而管理虚拟机。通过vagrant，用户可以用简单的命令创建和配置基于virtualBox的虚拟机。

然后需要配置**vagrantFile**和添加并使用自定义的**Box(虚拟机镜像)**。之后便可以开启虚拟机。



总结一下常见报错的解决思路：

1.

[virtualBox 不能为虚拟电脑打开一个新任务Failed to get device handle and/or partition ID-CSDN博客](https://blog.csdn.net/weixin_42094764/article/details/125101332)

[VirtualBox 不能为虚拟电脑打开一个新任务(Failed to get device handle and/or partition ID for 000000000153a220) 解决办法-CSDN博客](https://blog.csdn.net/weixin_43431218/article/details/126582968)

2.

[解决Vagrant报错b:48:in `join‘: incompatible character encodings: GBK and UTF-8 (Encoding::Compatib-CSDN博客](https://blog.csdn.net/qq_34626094/article/details/125985514?csdn_share_tail={"type"%3A"blog"%2C"rType"%3A"article"%2C"rId"%3A"125985514"%2C"source"%3A"u010357280"}&fromshare=blogdetail)



### 计网整体结构：

### 1 kernel.c

> [!Note]
>
> #include "global.h"
> #include "tju_packet.h"
> #include "tju_tcp.h"

##### 1.1 startSimulation()clien和server启动模拟环境，初始化网络环境，启动一个后台线程接收数据包。

初始化listen_socks[]和established_socks[]

获取主机名，创建UDP套接字，设置套接字选项，绑定到指定端口

启动后台线程时，涉及

##### 1.1.1 receiver_thread(),从UDP套接字接收模拟的TCP数据包，确保接收完成后交给TCP协议栈处理。

##### 1.1.1.1 onTCPPocket(),通过接收数据包中的IP和端口信息，在已建立连接和监听的套接字查找，然后根据响应的套接字处理

##### 1.1.1.1.1 tju_handle_packet(),根据对应监听的套接字，对TCP数据包进行处理 tju_tcp.h



##### 1.2 sendToLayer3(),通过判断当前主机是客户端还是服务端，决定数据包发送给哪个目标IP地址，模拟网络层和传输层的交互。



### 2 client.c

##### 2.1 tju_connect()

### 3 server.c

##### 3.1 tju_bind()

##### 3.2 tju_listen()

##### 3.3 tju_accept()



### 4 tju_tcp.c

> [!Note]
>
> #include "global.h"
> #include "tju_packet.h"
> #include "kernel.h"

##### socket queue

##### myQueue.c

##### TCP连接建立过程中的超时重传

##### retran.c

##### TCP连接中的超时重传和发送

##### 拥塞控制



## 二 代码结构

#### 总体设计

该TCP协议主要分为五个模块。分别是连接管理，可靠数据传输，流量控制，拥塞控制以及连接关闭。

- 连接管理：该模块主要需要实现TCP的“三次握手”过程，同时对每次握手的丢包进行适当的处理。
- 可靠数据传输：该模块需要实现滑动窗口超时重传，快速重传，序列号，ACK等机制去应对数据包传输过程中可能出现的问题，使得实验能够在应用层使用UDP实现TCP的基本功能。
- 流量控制：通过“持续计时器机制”和窗口cwnd让发送方根据接收方的实际能力控制发送的数据量。
- 拥塞控制：通过设置发送方窗口避免发送方的数据动态填满整个网络。

### 三次握手和四次挥手

##### Q1 tju_handle_packet在什么情况下得到调用的？

> 背景：刚开始研究三次握手时多个代码文件之间的逻辑没有理清楚，客户端进行调用connect发送第一次握手，由服务端处于bind和listen阶段，然后后面就是accept直接跳到第三次握手结束之后的接收了。那么中间请求的接收和发送是如何发生了，根据代码提示可以知到是在handle_packet函数中，但是在server.c和client.c中都没有对该函数的调用，那么这个函数大概是如何被挂起在后台运行的。

该问题答案**详见上面代码整体结构部分**。

最后总结过程，在TCP中，

- 首先是`client`端调用`connect`函数，生成`SYN`报文发送给`server`端进行**第一次握手**；`client`进入`SYN_SENT`状态，server处于`LISTEN`状态。

- 其中，调用`sendToLayer`函数时，在`tju_handle_packet`中，

  `server`端处于`listen`状态，会接收`client`端的连接，回复`SYN_ACK`包，同时将半连接的`socket`加入到`half_conn_socks`队列中，服务器进入`SYN_RECV`状态。(**第二次握手**)

  `client`端处于`SYN_SENT`状态，客户端已经发送`SYN`包，等待服务器的`SYN_ACK`响应，收到包后，`client`发送ACK包，将状态转换成`ESTABLISHED`。

- 最后`server`端调用`accept`函数从半连接队列中取出一个完成三次握手的连接请求，将其全部转移到全连接队列中。`accept`会阻塞直到有请求到达，才会重新开始三次握手之后的正式通信。

##### Q2 如何实现在tju_handle_packet中的fsm图？

> FSM中，如果是基础的三次握手和四次挥手，不考虑丢包延迟，每次的握手和挥手都按照理想状态下运行的过程是很好实现的，问题在于如何解决丢包和延迟的情况。
>
> 比如，三次握手时每次握手中可能存在的数据包丢失
>
> 四次挥手时如果两边同时关闭怎么处理

四次挥手如果同时关闭，直接从FIN_WAIT_1跳到CLOSING即可。

### 四次挥手

- `client`端调用`tju_close()`进行主动关闭，同时发送`FIN`报文。
- `server`端接收到`FIN`报文，发送`ACK`报文作为回复。
- `client`端收到`ACK`后不再主动发送数据，只接收数据。`server`端在一段时间后调用`tju_close()`，发送FIN报文。
- `client`端收到`FIN`报文，并且返回`ACK`报文作为回复，一段时间后资源得到释放。

### 可靠数据传输
### 拥塞控制
