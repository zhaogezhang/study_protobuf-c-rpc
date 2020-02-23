#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/un.h>

#include "protobuf-c-rpc.h"
#include "protobuf-c-rpc-data-buffer.h"
#include "gsklistmacros.h"

#undef TRUE
#define TRUE 1
#undef FALSE
#define FALSE 0

#define MAX_FAILED_MSG_LENGTH   512

/* enabled for efficiency, can be useful to disable for debugging */
#define RECYCLE_REQUESTS                1

/* === Server === */
typedef struct _ServerRequest ServerRequest;
typedef struct _ServerConnection ServerConnection;
struct _ServerRequest
{
  uint32_t request_id;                  /* in little-endian */
  uint32_t method_index;                /* in native-endian */
  
  /* 指向了当前 RPC 服务请求所属 RPC 服务连接指针 */
  ServerConnection *conn;

  /* 指向了当前 RPC 服务请求所属 RPC 服务端实例指针 */
  ProtobufC_RPC_Server *server;
  
  union {
    /* if conn != NULL, then the request is alive: */
    struct { ServerRequest *prev, *next; } alive;

    /* if conn == NULL, then the request is defunct: */
    struct { ProtobufCAllocator *allocator; } defunct;

    /* well, if it is in the recycled list, then it's recycled :/ */
    struct { ServerRequest *next; } recycled;
  } info;
};

/* 用来保存 RPC 服务连接信息 */
struct _ServerConnection
{
  int fd;

  /* 当前服务连接的输入输出数据包缓存空间 */
  ProtobufCRPCDataBuffer incoming, outgoing;

  /* 指向当前 RPC 服务连接所属的 RPC 服务端实例指针 */
  ProtobufC_RPC_Server *server;

  /* 系统通过双链表的方式把所有的服务连接链接起来 */
  ServerConnection *prev, *next;

  /* 记录当前服务端 RCP 连接接收到的待处理请求信息 */
  unsigned n_pending_requests;
  ServerRequest *first_pending_request, *last_pending_request;
};


/* When we get a response in the wrong thread,
   we proxy it over a system pipe.  Actually, we allocate one
   of these structures and pass the pointer over the pipe.  */
typedef struct _ProxyResponse ProxyResponse;
struct _ProxyResponse
{
  ServerRequest *request;
  unsigned len; /* the length of the data follows the structure */
  /* data follows the structure */
};

/* 定义了 RPC 服务端实例数据结构 */
struct _ProtobufC_RPC_Server
{
  /* 定义了当前 RPC 服务端实例使用的 Dispatch 实例数据结构，主要用来处理事件监测及处理等操作 */
  ProtobufCRPCDispatch *dispatch;
  
  ProtobufCAllocator *allocator;

  /* 指向了当前 RPC 服务端实例所属的 ProtobufC 服务实例，包含了当前服务支持的方法信息 */
  ProtobufCService *underlying;

  /* 表示当前 RPC 服务端实例在数据通信时使用的 socket 地址类型 */
  ProtobufC_RPC_AddressType address_type;
  
  char *bind_name;

  /* 通过链表的方式把当前 RPC 服务端实例维持的有效服务连接链接起来 */
  ServerConnection *first_connection, *last_connection;
  
  ProtobufC_RPC_FD listening_fd;

  /* 通过链表的方式把当前 RPC 服务端实例需要回收的服务请求信息链接起来 */
  ServerRequest *recycled_requests;

  /* multithreading support */
  /* 用来判断当前线程是否为 RPC 代理线程的功能函数指针 */
  ProtobufC_RPC_IsRpcThreadFunc is_rpc_thread_func;
  
  void * is_rpc_thread_data;

  /* 表示当前当前 RPC 服务端实例使用的管道文件描述符，用来和代理线程通信
     proxy_pipe[0] 为读数据文件描述符，工作在非阻塞模式，proxy_pipe[1] 为
     写数据文件描述符，工作在阻塞模式 */
  int proxy_pipe[2];

  /* 表示代理线程用来存储还未处理的数据缓冲区中当前有效数据长度 */
  unsigned proxy_extra_data_len;

  /* 表示代理线程用来存储还未处理的数据缓冲区地址 */
  uint8_t proxy_extra_data[sizeof (void*)];

  /* RPC 服务实例的错误处理函数指针和函数参数数据 */
  ProtobufC_RPC_Error_Func error_handler;
  void *error_handler_data;

  /* 表示当前 RPC 服务端实例使用的数据序列化和反序列化数据处理函数 */
  ProtobufC_RPC_Protocol rpc_protocol;

  /* configuration */
  /* 表示当前 RPC 服务端实例最多同时可以挂起的 RPC 服务请求个数 */
  unsigned max_pending_requests_per_connection;
};

/*********************************************************************************************************
** 函数名称: errno_is_ignorable
** 功能描述: 判断指定的错误码是否可以被忽略
** 输	 入: e - 指定的错误码
** 输	 出: 1 - 可以忽略
**         : 0 - 不可以忽略
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static inline protobuf_c_boolean
errno_is_ignorable (int e)
{
#ifdef EWOULDBLOCK              /* for windows */
  if (e == EWOULDBLOCK)
    return 1;
#endif
  return e == EINTR || e == EAGAIN;
}

/*********************************************************************************************************
** 函数名称: set_fd_nonblocking
** 功能描述: 设置指定的文件描述符为非阻塞模式
** 输	 入: fd - 指定的文件描述符
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
set_fd_nonblocking(int fd)
{
  int flags = fcntl (fd, F_GETFL);

  if (flags >= 0)
    fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

/*********************************************************************************************************
** 函数名称: error_handler
** 功能描述: 打印指定的错误消息
** 输	 入: message - 需要打印的错误信息
**         : error_func_data - 出错函数名
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
error_handler (ProtobufC_RPC_Error_Code code,
               const char              *message,
               void                    *error_func_data)
{
  fprintf (stderr, "*** error: %s: %s\n",
           (char*) error_func_data, message);
}

#define GET_PENDING_REQUEST_LIST(conn) \
  ServerRequest *, conn->first_pending_request, conn->last_pending_request, info.alive.prev, info.alive.next
#define GET_CONNECTION_LIST(server) \
  ServerConnection *, server->first_connection, server->last_connection, prev, next

/*********************************************************************************************************
** 函数名称: server_connection_close
** 功能描述: 关闭指定的服务连接并释放其占用的内存资源以及未处理的服务请求占用的内存资源
** 输	 入: conn - 指定的服务连接指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
server_connection_close (ServerConnection *conn)
{
  ProtobufCAllocator *allocator = conn->server->allocator;

  /* general cleanup */
  /* 从指定的 Dispatch 实例中释放和指定描述符对应的 FDNotify 和 FDNotifyChange 成员
     并关闭指定文件描述符代表的文件 */
  protobuf_c_rpc_dispatch_close_fd (conn->server->dispatch, conn->fd);
  conn->fd = -1;

  /* 释放输入和输出缓存空间中所有的缓存数据块占用的内存资源 */
  protobuf_c_rpc_data_buffer_clear (&conn->incoming);
  protobuf_c_rpc_data_buffer_clear (&conn->outgoing);

  /* remove this connection from the server's list */
  GSK_LIST_REMOVE (GET_CONNECTION_LIST (conn->server), conn);

  /* disassocate all the requests from the connection */
  while (conn->first_pending_request != NULL)
    {
      ServerRequest *req = conn->first_pending_request;
      conn->first_pending_request = req->info.alive.next;
      req->conn = NULL;
      req->info.defunct.allocator = allocator;
    }

  /* free the connection itself */
  allocator->free (allocator, conn);
}

/*********************************************************************************************************
** 函数名称: server_failed_literal
** 功能描述: 处理指定的 RPC 服务端错误信息
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : code - 错误码
**         : msg - 和错误码相关的错误信息
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
server_failed_literal (ProtobufC_RPC_Server *server,
                       ProtobufC_RPC_Error_Code code,
                       const char *msg)
{
  if (server->error_handler != NULL)
    server->error_handler (code, msg, server->error_handler_data);
}

#if 0
static void
server_failed (ProtobufC_RPC_Server *server,
               ProtobufC_RPC_Error_Code code,
               const char           *format,
               ...)
{
  va_list args;
  char buf[MAX_FAILED_MSG_LENGTH];
  va_start (args, format);
  vsnprintf (buf, sizeof (buf), format, args);
  buf[sizeof(buf)-1] = 0;
  va_end (args);

  server_failed_literal (server, code, buf);
}
#endif

/*********************************************************************************************************
** 函数名称: address_to_name
** 功能描述: 把指定的地址转换成与其对应的点分十进制格式
** 输	 入: addr - 指定的 socket 地址
**         : addr_len - 指定的 socket 地址长度
**         : name_out_buf_length - 地址名缓冲区长度
** 输	 出: name_out - 和指定地址对应的点分十进制格式数据
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static protobuf_c_boolean
address_to_name (const struct sockaddr *addr,
                 unsigned               addr_len,
                 char                  *name_out,
                 unsigned               name_out_buf_length)
{
  if (addr->sa_family == PF_INET)
    {
      /* convert to dotted address + port */
      const struct sockaddr_in *addr_in = (const struct sockaddr_in *) addr;
      const uint8_t *addr2 = (const uint8_t *) &(addr_in->sin_addr);
      uint16_t port = htons (addr_in->sin_port);
      snprintf (name_out, name_out_buf_length,
                "%u.%u.%u.%u:%u",
                addr2[0], addr2[1], addr2[2], addr2[3], port);
      return TRUE;
    }
  return FALSE;
}

/*********************************************************************************************************
** 函数名称: server_connection_failed
** 功能描述: 指定的 RPC 服务连接失败处理函数
** 输	 入: conn - 连接失败的 RPC 服务连接指针
**         : code - 错误码
**         : format - 格式化错误信息
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
server_connection_failed (ServerConnection *conn,
                          ProtobufC_RPC_Error_Code code,
                          const char       *format,
                          ...)
{
  char remote_addr_name[64];
  char msg[MAX_FAILED_MSG_LENGTH];
  char *msg_end = msg + sizeof (msg);
  char *msg_at;
  struct sockaddr addr;
  socklen_t addr_len = sizeof (addr);
  va_list args;

  /* if we can, find the remote name of this connection */
  if (getpeername (conn->fd, &addr, &addr_len) == 0
   && address_to_name (&addr, addr_len, remote_addr_name, sizeof (remote_addr_name)))
    snprintf (msg, sizeof (msg), "connection to %s from %s: ",
              conn->server->bind_name, remote_addr_name);
  else
    snprintf (msg, sizeof (msg), "connection to %s: ",
              conn->server->bind_name);
  
  msg[sizeof(msg)-1] = 0;
  msg_at = strchr (msg, 0);

  /* do vsnprintf() */
  va_start (args, format);
  vsnprintf(msg_at, msg_end - msg_at, format, args);
  va_end (args);
  msg[sizeof(msg)-1] = 0;

  /* invoke server error hook */
  server_failed_literal (conn->server, code, msg);

  /* 关闭指定的服务连接并释放其占用的内存资源以及未处理的服务请求占用的内存资源 */
  server_connection_close (conn);
}

/*********************************************************************************************************
** 函数名称: create_server_request
** 功能描述: 为指定的 RPC 服务连接根据指定的参数创建一个新的 RPC 服务请求并添加到这个服务连接的
**         : pending 链表上并返回新创建的服务请求结构指针
** 输	 入: conn - 指定的 RPC 服务连接指针
**         : request_id - 指定的服务请求 ID
**         : method_index - 指定的服务请求方法 ID
** 输	 出: rv - 新创建的服务请求结构指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ServerRequest *
create_server_request (ServerConnection *conn,
                       uint32_t          request_id,
                       uint32_t          method_index)
{
  ServerRequest *rv;

  /* 获取一个 RPC 服务请求数据结构空间 */
  if (conn->server->recycled_requests != NULL)
    {
      rv = conn->server->recycled_requests;
      conn->server->recycled_requests = rv->info.recycled.next;
    }
  else
    {
      ProtobufCAllocator *allocator = conn->server->allocator;
      rv = allocator->alloc (allocator, sizeof (ServerRequest));
    }
  
  rv->server = conn->server;
  rv->conn = conn;
  rv->request_id = request_id;
  rv->method_index = method_index;
  conn->n_pending_requests++;

  /* 把新创建的 RPC 服务请求添加到指定服务连接的 pending 链表上 */
  GSK_LIST_APPEND (GET_PENDING_REQUEST_LIST (conn), rv);
  
  return rv;
}

/*********************************************************************************************************
** 函数名称: free_server_request
** 功能描述: 释放指定的 RPC 服务端实例中指定的服务请求占用的内存资源
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : request - 需要释放的 RPC 服务请求结构指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
free_server_request (ProtobufC_RPC_Server *server,
                     ServerRequest        *request)
{
#if RECYCLE_REQUESTS
  /* recycle request */
  request->info.recycled.next = server->recycled_requests;
  server->recycled_requests = request;
#else
  /* free the request immediately */
  server->allocator->free (server->allocator, request);
#endif
}

/*********************************************************************************************************
** 函数名称: uint32_to_le
** 功能描述: 把指定的本机字节序的 uint32_t 类型变量转换成小端格式
** 输	 入: le - 指定的 uint32_t 类型变量
** 输	 出: uint32_t - 对应的小段格式变量
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static uint32_t
uint32_to_le (uint32_t le)
{
#if !defined(WORDS_BIGENDIAN)
  return le;
#else
  return (le << 24) | (le >> 24)
       | ((le >> 8) & 0xff00)
       | ((le << 8) & 0xff0000);
#endif
}
#define uint32_from_le uint32_to_le             /* make the code more readable, i guess */

static void handle_server_connection_events (int fd,
                                             unsigned events,
                                             void *data);

/*********************************************************************************************************
** 函数名称: server_connection_response_closure
** 功能描述: 把 RPC 服务端对指定的 RPC 客户端的服务请求响应消息发送出去
** 输	 入: message - 要发送给 RPC 客户端的响应消息指针
**         : closure_data - RPC 客户端发送的服务请求数据指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
server_connection_response_closure (const ProtobufCMessage *message,
                                    void                   *closure_data)
{
  ServerRequest *request = closure_data;
  ProtobufC_RPC_Server *server = request->server;
  protobuf_c_boolean must_proxy = 0;
  ProtobufCAllocator *allocator = server->allocator;

  /* 判断我们是否需要通过代理线程来发送响应数据到 RPC 客户端 */
  if (server->is_rpc_thread_func != NULL)
    {
      must_proxy = !server->is_rpc_thread_func (server,
                                                server->dispatch,
                                                server->is_rpc_thread_data);
    }


  uint8_t buffer_slab[512];

  /* 声明并初始化一个 protobuf-c 对象数据缓冲区结构，用来存储对象数据 */
  ProtobufCBufferSimple buffer_simple = PROTOBUF_C_BUFFER_SIMPLE_INIT (buffer_slab);

  /* 定义并初始化发送给 RPC 客户端的响应数据包的负载数据 */
  ProtobufC_RPC_Payload payload = { request->method_index,
                                    request->request_id,
                                    (ProtobufCMessage *)message };

  /* 对需要发送给 RPC 客户端的响应数据进行序列化操作 */
  ProtobufC_RPC_Protocol_Status status =
    server->rpc_protocol.serialize_func (server->underlying->descriptor,
          allocator, &buffer_simple.base, payload);

  if (status != PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS)
    {
      server_failed_literal (server, PROTOBUF_C_RPC_ERROR_CODE_BAD_REQUEST,
                             "error serializing the response");
      return;
    }

  if (must_proxy)
    {
      /* 申请并初始化一个 ProxyResponse 数据结构 */
      ProxyResponse *pr = allocator->alloc (allocator, sizeof (ProxyResponse) + buffer_simple.len);
      int rv;
      pr->request = request;
      pr->len = buffer_simple.len;
      memcpy (pr + 1, buffer_simple.data, buffer_simple.len);

      /* write pointer to proxy pipe */
	  /* 把需要发送的响应数据包指针发送给代理线程，发送的数据是组装好的 ProxyResponse 结构体指针
	     代理线程的数据处理函数为 handle_proxy_pipe_readable */
retry_write:
      rv = write (server->proxy_pipe[1], &pr, sizeof (void*));
      if (rv < 0)
        {
          if (errno == EINTR || errno == EAGAIN)
            goto retry_write;
          server_failed_literal (server, PROTOBUF_C_RPC_ERROR_CODE_PROXY_PROBLEM,
                                 "error writing to proxy-pipe");
          allocator->free (allocator, pr);
        }
      else if (rv < sizeof (void *))
        {
          server_failed_literal (server, PROTOBUF_C_RPC_ERROR_CODE_PROXY_PROBLEM,
                                 "partial write to proxy-pipe");
          allocator->free (allocator, pr);
        }
    }
  else if (request->conn == NULL)
    {
      /* defunct request */
      allocator->free (allocator, request);
      free_server_request (server, request);
    }
  else
    {
      ServerConnection *conn = request->conn;
      protobuf_c_boolean must_set_output_watch = (conn->outgoing.size == 0);

	  /* 向当前 RPC 服务连接的发送缓存空间末尾位置追加指定长度的数据 */
      protobuf_c_rpc_data_buffer_append (&conn->outgoing, buffer_simple.data, buffer_simple.len);
	  
      if (must_set_output_watch) {
		/* 为指定的 Dispatch 实例注册一个指定参数集的 FDNotify 成员用来监测并处理
		   当前 RPC 连接的文件描述符的 READABLE 和 WRITABLE 事件 */
        protobuf_c_rpc_dispatch_watch_fd (conn->server->dispatch,
                                      conn->fd,
                                      PROTOBUF_C_RPC_EVENT_READABLE|PROTOBUF_C_RPC_EVENT_WRITABLE,
                                      handle_server_connection_events,
                                      conn);
		}

	  /* 把当前已经处理的 RPC 服务请求从当前连接的服务请求 pending 链表中移除 */
      GSK_LIST_REMOVE (GET_PENDING_REQUEST_LIST (conn), request);
      conn->n_pending_requests--;

      /* 释放指定的 RPC 服务端实例中指定的服务请求占用的内存资源 */
      free_server_request (server, request);
    }
  
  /* 释放指定的 ProtobufCBufferSimple 对象占用的所有内存资源 */
  PROTOBUF_C_BUFFER_SIMPLE_CLEAR (&buffer_simple);
}

/*********************************************************************************************************
** 函数名称: get_rcvd_message_descriptor
** 功能描述: 获取和指定的 RPC 负载数据相关的方法的输入消息描述符指针，在数据反序列化时使用
** 输	 入: payload - 指定的 RPC 负载数据
**         : data - 指向了当前接收消息的 RPC 服务连接
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static const ProtobufCMessageDescriptor *
get_rcvd_message_descriptor (const ProtobufC_RPC_Payload *payload, void *data)
{
   ServerConnection *conn = data;
   ProtobufCService *service = conn->server->underlying;

   /* 获取和指定的 RPC 负载数据相关的方法索引值 */
   uint32_t method_index = payload->method_index;

   if (method_index >= service->descriptor->n_methods)
   {
      server_connection_failed (conn,
            PROTOBUF_C_RPC_ERROR_CODE_BAD_REQUEST,
            "bad method_index %u", method_index);
      return NULL;
   }

   return service->descriptor->methods[method_index].input;
}

/*********************************************************************************************************
** 函数名称: handle_server_connection_events
** 功能描述: 用来处理指定的文件描述符代表的 RPC 服务连接上发送的事件（READABLE 和 WRITABLE）
** 输	 入: fd - 指定的文件描述符
**         : events - 当前待处理的事件类型
**         : data - 和当前事件相关的数据
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_server_connection_events (int fd,
                                 unsigned events,
                                 void *data)
{
  ServerConnection *conn = data;
  ProtobufCService *service = conn->server->underlying;
  ProtobufCAllocator *allocator = conn->server->allocator;

  /* 如果当前 RPC 服务连接发生的 WRITABLE 事件并且当前 RPC 服务连接发送缓冲区有待发送的数据 */
  if ((events & PROTOBUF_C_RPC_EVENT_WRITABLE) != 0
    && conn->outgoing.size > 0)
    {
      /* 从指定的缓存空间头部开始读取尽可能多的数据并通过 writev 写入到指定的文件中
         然后“释放”掉被处理数据的缓存数据块占用的内存资源 */
      int write_rv = protobuf_c_rpc_data_buffer_writev (&conn->outgoing, fd);
      if (write_rv < 0)
        {
          if (!errno_is_ignorable (errno))
            {
              server_connection_failed (conn,
                                        PROTOBUF_C_RPC_ERROR_CODE_CLIENT_TERMINATED,
                                        "writing to file-descriptor: %s",
                                        strerror (errno));
              return;
            }
        }

	  /* 为指定的 Dispatch 实例注册一个指定参数集的 FDNotify 成员用来监测并处理
		 当前 RPC 连接的文件描述符的 READABLE 事件 */
      if (conn->outgoing.size == 0)
        protobuf_c_rpc_dispatch_watch_fd (conn->server->dispatch, conn->fd, PROTOBUF_C_RPC_EVENT_READABLE,
                                      handle_server_connection_events, conn);
    }
	
  /* 处理当前 RPC 服务连接发生的 READABLE 事件 */
  if (events & PROTOBUF_C_RPC_EVENT_READABLE)
    {
      /* 从指定的文件描述符中尝试读取 8192 字节数数据并追加到指定的缓存空间中 */
      int read_rv = protobuf_c_rpc_data_buffer_read_in_fd (&conn->incoming, fd);
      if (read_rv < 0)
        {
          if (!errno_is_ignorable (errno))
            {
              server_connection_failed (conn,
                                        PROTOBUF_C_RPC_ERROR_CODE_CLIENT_TERMINATED,
                                        "reading from file-descriptor: %s",
                                        strerror (errno));
              return;
            }
        }
      else if (read_rv == 0)
        {
          if (conn->first_pending_request != NULL)
            server_connection_failed (conn,
                                      PROTOBUF_C_RPC_ERROR_CODE_CLIENT_TERMINATED,
                                      "closed while calls pending");
          else
            server_connection_close (conn);
          return;
        }
      else
        while (conn->incoming.size > 0)
          {
            /* Deserialize the buffer */
		    /* 对从 RPC 客户端接收到的数据执行反序列化操作，函数实现是 server_deserialize */
            ProtobufC_RPC_Payload payload = {0};
            ProtobufC_RPC_Protocol_Status status =
              conn->server->rpc_protocol.deserialize_func (service->descriptor,
                                                           allocator,
                                                           &conn->incoming,
                                                           &payload,
                                                           get_rcvd_message_descriptor,
                                                           data);
			
            if (status == PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER)
              break;

            if (status == PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED)
            {
              server_connection_failed (conn,
                                        PROTOBUF_C_RPC_ERROR_CODE_BAD_REQUEST,
                                        "error deserializing client request");
              return;
            }

            /* Invoke service (note that it may call back immediately) */
			/* 为指定的 RPC 服务连接根据指定的参数创建一个新的 RPC 服务请求并添加到这个服务连接的
               pending 链表上并返回新创建的服务请求结构指针 */
            ServerRequest *server_request = create_server_request (conn,
                                                                   payload.request_id,
                                                                   payload.method_index);

		    /* 调用 RPC 客户端请求的指定索引值的方法 */
            service->invoke (service, payload.method_index, payload.message,
                             server_connection_response_closure, server_request);

            /* clean up */
			/* 释放指定的 ProtobufC 消息实例结构占用的内存资源 */
            if (payload.message)
              protobuf_c_message_free_unpacked (payload.message, allocator);
          }
    }
}

/* Default RPC Protocol implementation:
 *    client issues request with header:
 *         method_index              32-bit little-endian
 *         message_length            32-bit little-endian
 *         request_id                32-bit any-endian
 *    server responds with header:
 *         status_code               32-bit little-endian
 *         method_index              32-bit little-endian
 *         message_length            32-bit little-endian
 *         request_id                32-bit any-endian
 */
/*********************************************************************************************************
** 函数名称: server_serialize
** 功能描述: 把指定的 RPC 负载数据序列化并追加到指定的 ProtobufC 缓冲区的末尾位置
** 输	 入: descriptor - 未使用
**         : allocator - 指定的内存分配器指针
**         : payload - 需要发送的 RPC 负载数据
** 输	 出: out_buffer - 指定的发送缓冲区
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS - 操作成功
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED - 操作失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ProtobufC_RPC_Protocol_Status
server_serialize (const ProtobufCServiceDescriptor *descriptor,
                  ProtobufCAllocator *allocator,
                  ProtobufCBuffer *out_buffer,
                  ProtobufC_RPC_Payload payload)
{
   if (!out_buffer)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   uint32_t header[4];
   if (!protobuf_c_message_check (payload.message))
   {
      /* send failed response */
      header[0] = uint32_to_le (PROTOBUF_C_RPC_STATUS_CODE_SERVICE_FAILED);
      header[1] = uint32_to_le (payload.method_index);
      header[2] = 0;            /* no message */
      header[3] = payload.request_id;
      out_buffer->append (out_buffer, sizeof (header), (uint8_t *)header);
   }
   else
   {
      /* send success response */
      /* 计算指定的消息数据在序列化后需要占用的字节数 */
      size_t message_length = protobuf_c_message_get_packed_size (payload.message);
	  
      header[0] = uint32_to_le (PROTOBUF_C_RPC_STATUS_CODE_SUCCESS);
      header[1] = uint32_to_le (payload.method_index);
      header[2] = uint32_to_le (message_length);
      header[3] = payload.request_id;

      out_buffer->append (out_buffer, sizeof (header), (uint8_t *)header);

	  /* 把指定的消息数据序列化并追加到指定的 ProtobufC 缓冲区末尾位置 */
      if (protobuf_c_message_pack_to_buffer (payload.message, out_buffer)
            != message_length)
         return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;
   }
   return PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS;
}

/*********************************************************************************************************
** 函数名称: server_deserialize
** 功能描述: 解析指定的 ProtobufC 序列化数据并把解析结果存储到指定的 RPC 负载数据中 
** 输	 入: descriptor - 未使用
**         : allocator - 指定的内存分配器指针
**         : in_buffer - 从客户端接收到的输入数据包指针
**         : payload - 接收到的 RPC 负载数据
**         : get_descriptor - 获取和指定的 RPC 负载数据相关的方法的输入消息描述符指针
**         : get_descriptor_data - 指向了当前接收消息的 RPC 服务连接
** 输	 出: payload - 存储接收到的 RPC 负载数据信息
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS - 操作成功
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED - 操作失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ProtobufC_RPC_Protocol_Status
server_deserialize (const ProtobufCServiceDescriptor *descriptor,
                    ProtobufCAllocator    *allocator,
                    ProtobufCRPCDataBuffer *in_buffer,
                    ProtobufC_RPC_Payload *payload,
                    ProtobufC_RPC_Get_Descriptor get_descriptor,
                    void *get_descriptor_data)
{
   if (!allocator || !in_buffer || !payload)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   uint32_t header[3];
   if (in_buffer->size < sizeof (header))
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER;

   uint32_t message_length;
   uint8_t *packed_data;
   ProtobufCMessage *message;

   /* 从指定的缓存空间头部开始读取指定长度数据到指定的缓冲区中但是“不释放”掉被读出
      数据的缓存数据块占用的内存资源 */
   protobuf_c_rpc_data_buffer_peek (in_buffer, header, sizeof (header));
   
   payload->method_index = uint32_from_le (header[0]);
   message_length = uint32_from_le (header[1]);
   /* store request_id in whatever endianness it comes in */
   payload->request_id = header[2];

   if (in_buffer->size < sizeof (header) + message_length)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER;

   /* 获取和指定的 RPC 负载数据相关的方法的输入消息描述符指针，函数实现是 get_rcvd_message_descriptor */
   const ProtobufCMessageDescriptor *desc =
      get_descriptor (payload, get_descriptor_data);
   if (!desc)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   /* Read message */
   /* 从指定的缓存空间头部开始丢弃指定长度的数据并“释放”掉被丢弃数据的缓存数据块占用的内存资源 */
   protobuf_c_rpc_data_buffer_discard (in_buffer, sizeof (header));
   packed_data = allocator->alloc (allocator, message_length);

   /* 从指定的缓存空间头部开始读取指定长度数据到指定的缓冲区中并“释放”掉被读出数据的缓存数据块
      占用的内存资源 */
   protobuf_c_rpc_data_buffer_read (in_buffer, packed_data, message_length);

   /* Unpack message */
   /* 解析指定的 ProtobufC 序列化数据并把解析结果存储到指定的 ProtobufC 消息实例结构中 */
   message = protobuf_c_message_unpack (desc, allocator, message_length, packed_data);
   allocator->free (allocator, packed_data);
   if (message == NULL)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   payload->message = message;

   return PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS;
}

/*********************************************************************************************************
** 函数名称: handle_server_listener_readable
** 功能描述: 用来处理在 socket 套接字上监听到的连接请求事件
** 输	 入: fd - 监听到连接请求的 socket 套接字文件描述符
**         : events - 未使用
**         : data - 处理 RPC 客户端连接请求的 RPC 服务端实例
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_server_listener_readable (int fd,
                                 unsigned events,
                                 void *data)
{
  ProtobufC_RPC_Server *server = data;
  struct sockaddr addr;
  socklen_t addr_len = sizeof (addr);
  int new_fd = accept (fd, &addr, &addr_len);
  ServerConnection *conn;
  ProtobufCAllocator *allocator = server->allocator;
  
  if (new_fd < 0)
    {
      if (errno_is_ignorable (errno))
        return;
      fprintf (stderr, "error accept()ing file descriptor: %s\n",
               strerror (errno));
      return;
    }

  /* 创建并初始化一个新的 RPC 服务连接 */
  conn = allocator->alloc (allocator, sizeof (ServerConnection));
  conn->fd = new_fd;
  protobuf_c_rpc_data_buffer_init (&conn->incoming, server->allocator);
  protobuf_c_rpc_data_buffer_init (&conn->outgoing, server->allocator);
  conn->n_pending_requests = 0;
  conn->first_pending_request = conn->last_pending_request = NULL;
  conn->server = server;

  /* 把新创建的 RPC 服务连接添加到指定的 RPC 服务端实例结构的 RPC 服务链表上 */
  GSK_LIST_APPEND (GET_CONNECTION_LIST (server), conn);
  
  /* 为指定的 Dispatch 实例注册一个 READABLE 监听事件，用来处理新创建的 RPC 服务连接的服务请求数据 */
  protobuf_c_rpc_dispatch_watch_fd (server->dispatch, conn->fd, PROTOBUF_C_RPC_EVENT_READABLE,
                                handle_server_connection_events, conn);
}

/*********************************************************************************************************
** 函数名称: server_new_from_fd
** 功能描述: 通过指定的参数创建一个 RPC 服务端实例用来监听客户端发起的连接请求
** 输	 入: listening_fd - 用来监听连接请求的 socket 套接字文件描述符
**         : address_type - 新创建的 RPC 服务端实例使用的地址类型
**         : bind_name - 新创建的 RPC 服务端实例使用的绑定名称
**         : service - 新创建的 RPC 服务端实例所属的 ProtobufC 服务实例指针
**         : orig_dispatch - 新创建的 RPC 服务端实例使用的 Dispatch 实例指针
** 输	 出: server - 新创建的 RPC 服务端实例指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ProtobufC_RPC_Server *
server_new_from_fd (ProtobufC_RPC_FD              listening_fd,
                    ProtobufC_RPC_AddressType address_type,
                    const char               *bind_name,
                    ProtobufCService         *service,
                    ProtobufCRPCDispatch       *orig_dispatch)
{
  ProtobufCRPCDispatch *dispatch = orig_dispatch ? orig_dispatch : protobuf_c_rpc_dispatch_default ();
  ProtobufCAllocator *allocator = protobuf_c_rpc_dispatch_peek_allocator (dispatch);

  /* 创建并初始化一个新的 RPC 服务端实例 */
  ProtobufC_RPC_Server *server = allocator->alloc (allocator, sizeof (ProtobufC_RPC_Server));
  server->dispatch = dispatch;
  server->allocator = allocator;
  server->underlying = service;
  server->first_connection = server->last_connection = NULL;
  server->max_pending_requests_per_connection = 32;
  server->address_type = address_type;
  server->error_handler = error_handler;
  server->error_handler_data = "protobuf-c rpc server";
  server->listening_fd = listening_fd;
  server->recycled_requests = NULL;
  server->is_rpc_thread_func = NULL;
  server->is_rpc_thread_data = NULL;
  server->proxy_pipe[0] = server->proxy_pipe[1] = -1;
  server->proxy_extra_data_len = 0;
  ProtobufC_RPC_Protocol default_rpc_protocol = {server_serialize, server_deserialize};
  server->rpc_protocol = default_rpc_protocol;

  size_t name_len = strlen (bind_name);
  server->bind_name = allocator->alloc (allocator, name_len + 1);
  if (!server->bind_name)
     return NULL;
  strncpy (server->bind_name, bind_name, name_len);
  server->bind_name[name_len] = '\0';

  /* 设置指定的文件描述符为非阻塞模式 */
  set_fd_nonblocking (listening_fd);

  /* 为指定的 Dispatch 实例注册一个 READABLE 监测事件，用来处理 RPC 客户端发起的连接请求 */
  protobuf_c_rpc_dispatch_watch_fd (dispatch, listening_fd, PROTOBUF_C_RPC_EVENT_READABLE,
                                handle_server_listener_readable, server);
  return server;
}

/* this function is for handling the common problem
   that we bind over-and-over again to the same
   unix path.
   
   ideally, you'd think the OS's SO_REUSEADDR flag would
   cause this to happen, but it doesn't,
   at least on my linux 2.6 box.

   in fact, we really need a way to test without
   actually connecting to the remote server,
   which might annoy it.

   XXX: we should survey what others do here... like x-windows...
 */
/* NOTE: stolen from gsk, obviously */
/*********************************************************************************************************
** 函数名称: _gsk_socket_address_local_maybe_delete_stale_socket
** 功能描述: 用于处理我们反复绑定到同一 UNIX 文件
** 输	 入: path - 指定的 unix 文件路径
**         : addr - 指定的 socket 地址
**         : addr_len - 指定的 socket 地址长度
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
_gsk_socket_address_local_maybe_delete_stale_socket (const char *path,
                                                     struct sockaddr *addr,
                                                     unsigned addr_len)
{
  int fd;
  struct stat statbuf;
  
  if (stat (path, &statbuf) < 0)
    return;
  
  if (!S_ISSOCK (statbuf.st_mode))
    {
      fprintf (stderr, "%s existed but was not a socket\n", path);
      return;
    }

  fd = socket (PF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return;

  /* 设置指定的文件描述符为非阻塞模式 */
  set_fd_nonblocking (fd);

  /* 对于非阻塞的 connect 的连接，其不会在调用 connect 的时候进行阻塞，而是立即返回
     如果返回 0，说明调用 connect 的时候还没来得及让 connect 返回便已经建立了连接，
     而如果返回 -1，errno 会记录一个错误码，如果该错误码为 EINPROGRESS，则说明该连接
     还正在建立，并不能说明该连接出错 */
  if (connect (fd, addr, addr_len) < 0)
    {
      if (errno == EINPROGRESS)
        {
          close (fd);
          return;
        }
    }
  else
    {
      close (fd);
      return;
    }

  /* ok, we should delete the stale socket */
  close (fd);
  
  if (unlink (path) < 0)
    fprintf (stderr, "unable to delete %s: %s\n",
             path, strerror(errno));
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_server_new
** 功能描述: 通过指定的参数创建一个 RPC 服务端实例用来监听客户端发起的连接请求
** 输	 入: type - 指定新创建的 RPC 服务端实例使用的地址类型
**         : name - 表示某个文件（AF_UNIX）或某个端口（AF_INET）
**         : service - 新创建的 RPC 服务端实例所属的 ProtobufC 服务实例指针
**         : dispatch - 新创建的 RPC 服务端实例使用的 Dispatch 实例指针
** 输	 出: ProtobufC_RPC_Server * - 新创建的 RPC 服务端实例指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
ProtobufC_RPC_Server *
protobuf_c_rpc_server_new       (ProtobufC_RPC_AddressType type,
                                 const char               *name,
                                 ProtobufCService         *service,
                                 ProtobufCRPCDispatch       *dispatch)
{
  int fd = -1;
  int protocol_family;
  struct sockaddr *address;
  socklen_t address_len;
  struct sockaddr_un addr_un;
  struct sockaddr_in addr_in;

  /* 处理不同类型的 socket 地址信息 */
  switch (type)
    {
    case PROTOBUF_C_RPC_ADDRESS_LOCAL:
      protocol_family = PF_UNIX;
      memset (&addr_un, 0, sizeof (addr_un));
      addr_un.sun_family = AF_UNIX;
      strncpy (addr_un.sun_path, name, sizeof (addr_un.sun_path));
      address_len = sizeof (addr_un);
      address = (struct sockaddr *) (&addr_un);
      _gsk_socket_address_local_maybe_delete_stale_socket (name,
                                                           address,
                                                           address_len);
      break;
    case PROTOBUF_C_RPC_ADDRESS_TCP:
      protocol_family = PF_INET;
      memset (&addr_in, 0, sizeof (addr_in));
      addr_in.sin_family = AF_INET;
      {
        unsigned port = atoi (name);
        addr_in.sin_port = htons (port);
      }
      address_len = sizeof (addr_in);
      address = (struct sockaddr *) (&addr_in);
      break;
    default:
      return NULL;
    }

  fd = socket (protocol_family, SOCK_STREAM, 0);
  if (fd < 0)
    {
      fprintf (stderr, "protobuf_c_rpc_server_new: socket() failed: %s\n",
               strerror (errno));
      return NULL;
    }
  if (bind (fd, address, address_len) < 0)
    {
      fprintf (stderr, "protobuf_c_rpc_server_new: error binding to port: %s\n",
               strerror (errno));
      return NULL;
    }
  if (listen (fd, 255) < 0)
    {
      fprintf (stderr, "protobuf_c_rpc_server_new: listen() failed: %s\n",
               strerror (errno));
      return NULL;
    }

  /* 通过指定的参数创建一个 RPC 服务端实例用来监听客户端发起的连接请求 */
  return server_new_from_fd (fd, type, name, service, dispatch);
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_server_destroy
** 功能描述: 销毁指定的 RPC 服务端实例并释放其占用的所有资源
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : destroy_underlying - 表示是否需要销毁当前 RPC 服务端实例所属的 ProtobufC 服务实例
** 输	 出: rv - 表示当前 RPC 服务端实例所属的 ProtobufC 服务实例指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
ProtobufCService *
protobuf_c_rpc_server_destroy (ProtobufC_RPC_Server *server,
                               protobuf_c_boolean    destroy_underlying)
{
  ProtobufCService *rv = destroy_underlying ? NULL : server->underlying;

  /* 关闭当前 RPC 服务端实例维持的所有有效 RPC 服务连接并释放其占用的资源 */
  while (server->first_connection != NULL)
    server_connection_close (server->first_connection);

  if (server->address_type == PROTOBUF_C_RPC_ADDRESS_LOCAL)
    unlink (server->bind_name);
  server->allocator->free (server->allocator, server->bind_name);

  /* 回收当前 RPC 服务端实例需要回收的所有服务请求占用的资源 */
  while (server->recycled_requests != NULL)
    {
      ServerRequest *req = server->recycled_requests;
      server->recycled_requests = req->info.recycled.next;
      server->allocator->free (server->allocator, req);
    }

  /* 从指定的 Dispatch 实例中释放和指定描述符对应的 FDNotify 和 FDNotifyChange 成员
     并关闭指定文件描述符代表的文件 */
  protobuf_c_rpc_dispatch_close_fd (server->dispatch, server->listening_fd);

  /* 销毁当前 RPC 服务端实例所属的 ProtobufC 服务实例 */
  if (destroy_underlying)
    protobuf_c_service_destroy (server->underlying);

  server->allocator->free (server->allocator, server);

  return rv;
}

/* Number of proxied requests to try to grab in a single read */
/* 定义当前系统代理线程用来存储代理请求的缓冲区大小，即最多可缓存的代理请求数量 */
#define PROXY_BUF_SIZE   256

/*********************************************************************************************************
** 函数名称: handle_proxy_pipe_readable
** 功能描述: 读取其他线程通过指定的管道文件描述符发送给代理线程的数据，并根据这些数据把需要处理并
**         : 发送的服务请求响应数据放到与其对应的服务连接的输出数据包缓存空间末尾位置
** 输	 入: fd - 表示其他线程用来给代理线程发送数据的管道文件描述符
**         : events - 未使用
**         : callback_data - 发送代理请求的 RPC 服务端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_proxy_pipe_readable (ProtobufC_RPC_FD   fd,
                            unsigned       events,
                            void          *callback_data)
{
  int nread;
  ProtobufC_RPC_Server *server = callback_data;
  ProtobufCAllocator *allocator = server->allocator;
  union {
    char buf[sizeof(void*) * PROXY_BUF_SIZE];
    ProxyResponse *responses[PROXY_BUF_SIZE];
  } u;
  unsigned amt, i;

  /* 获取上一次代理线程还未处理完的数据 */
  memcpy (u.buf, server->proxy_extra_data, server->proxy_extra_data_len);
  
  /* 读取其他线程通过管道文件描述符发送给当前代理线程的数据，读到的数据是组装好的 ProxyResponse 结构体指针
     数据发送函数的实现为 server_connection_response_closure */
  nread = read (fd, u.buf + server->proxy_extra_data_len, sizeof (u.buf) - server->proxy_extra_data_len);
  if (nread <= 0)
    return;             /* TODO: handle 0 and non-retryable errors separately */

  /* 表示当前代理线程一共需要处理的数据字节数 */
  amt = server->proxy_extra_data_len + nread;

  /* 处理当前代理线程需要发送的每一个服务请求响应数据，把需要发送的响应数据放到与其对应
   * 的服务连接的输出数据包缓存空间末尾位置 */
  for (i = 0; i < amt / sizeof(void*); i++)
    {
      ProxyResponse *pr = u.responses[i];
      ServerRequest *request = pr->request;
      if (request->conn == NULL)
        {
          /* defunct request */
          allocator->free (allocator, request);
        }
      else
        {
          ServerConnection *conn = request->conn;
          protobuf_c_boolean must_set_output_watch = (conn->outgoing.size == 0);

		  /* 向当前服务连接的输出数据包缓存空间末尾位置追加当前需要发送的服务请求响应的负载数据 */
          protobuf_c_rpc_data_buffer_append (&conn->outgoing, (pr+1), pr->len);
		  
          if (must_set_output_watch) 
          	{
          	  /* 为指定的 Dispatch 实例注册一个指定参数集的 FDNotify 成员用来监测并处理
		         当前 RPC 连接的文件描述符的 READABLE 和 WRITABLE 事件 */
              protobuf_c_rpc_dispatch_watch_fd (conn->server->dispatch,
                                          conn->fd,
                                          PROTOBUF_C_RPC_EVENT_READABLE|PROTOBUF_C_RPC_EVENT_WRITABLE,
                                          handle_server_connection_events,
                                          conn);
          	}
		  
          GSK_LIST_REMOVE (GET_PENDING_REQUEST_LIST (conn), request);
          conn->n_pending_requests--;

          /* 释放指定的 RPC 服务端实例中指定的服务请求占用的内存资源 */
          free_server_request (conn->server, request);
        }
      allocator->free (allocator, pr);
    }

  /* 把本次未处理的数据放到当前代理线程的私有数据缓冲区中，在下次处理数据的时候继续处理 */
  memcpy (server->proxy_extra_data, u.buf + i * sizeof(void*), amt - i * sizeof(void*));
  server->proxy_extra_data_len = amt - i * sizeof(void*);
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_server_configure_threading
** 功能描述: 配置指定的 RPC 服务端实例代理线程相关参数并创建与其他线程通信的管道
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : func - 用来判断当前线程是否为代理线程的功能函数指针
**         : is_rpc_data - 为判断当前线程是否为代理线程的功能函数指定的参数数据
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_server_configure_threading (ProtobufC_RPC_Server *server,
                                           ProtobufC_RPC_IsRpcThreadFunc func,
                                           void          *is_rpc_data)
{
  server->is_rpc_thread_func = func;
  server->is_rpc_thread_data = is_rpc_data;
retry_pipe:
  if (pipe (server->proxy_pipe) < 0)
    {
      if (errno == EINTR || errno == EAGAIN)
        goto retry_pipe;
      server_failed_literal (server, PROTOBUF_C_RPC_ERROR_CODE_PROXY_PROBLEM,
                             "error creating pipe for thread-proxying");
      return;
    }

  /* make the read side non-blocking, since we will use it from the main-loop;
     leave the write side blocking, since it will be used from foreign threads */
  set_fd_nonblocking (server->proxy_pipe[0]);

  /* 为指定的 Dispatch 实例注册一个 READABLE 监测事件，用来处理其他线程发送给代理线程的数据 */
  protobuf_c_rpc_dispatch_watch_fd (server->dispatch, server->proxy_pipe[0],
                                PROTOBUF_C_RPC_EVENT_READABLE,
                                handle_proxy_pipe_readable, server);
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_server_set_error_handler
** 功能描述: 配置指定的 RPC 服务实例的错误处理函数指针和函数参数数据
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : func - 指定的错误处理函数
**         : error_func_data - 为错误处理函数指定的参数数据指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_server_set_error_handler (ProtobufC_RPC_Server *server,
                                         ProtobufC_RPC_Error_Func func,
                                         void                 *error_func_data)
{
  server->error_handler = func;
  server->error_handler_data = error_func_data;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_server_set_rpc_protocol
** 功能描述: 配置指定的 RPC 服务实例使用的序列化和反序列化函数指针
** 输	 入: server - 指定的 RPC 服务端实例指针
**         : protocol - 指定的序列化和反序列化函数指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_server_set_rpc_protocol (ProtobufC_RPC_Server *server,
                                        ProtobufC_RPC_Protocol protocol)
{
   server->rpc_protocol = protocol;
}
