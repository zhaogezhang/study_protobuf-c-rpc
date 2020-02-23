#include <string.h>
#include <assert.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdarg.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "protobuf-c-rpc.h"
#include "protobuf-c-rpc-data-buffer.h"

#define protobuf_c_rpc_assert(x) assert(x)

#undef TRUE
#define TRUE 1
#undef FALSE
#define FALSE 0

/* 把指定的 UINT 类型变量转换成与其对应的 UINT * 类型指针变量 */
#define UINT_TO_POINTER(ui)      ((void*)(uintptr_t)(ui))

/* 把指定的 UINT * 类型指针变量转换成与其对应的 UINT 类型变量 */
#define POINTER_TO_UINT(ptr)     ((unsigned)(uintptr_t)(ptr))

#define MAX_FAILED_MSG_LENGTH   512

typedef enum
{
  PROTOBUF_C_RPC_CLIENT_STATE_INIT,
  PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP,
  PROTOBUF_C_RPC_CLIENT_STATE_CONNECTING,
  PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED,
  PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING,
  PROTOBUF_C_RPC_CLIENT_STATE_FAILED,               /* if no autoreconnect */
  PROTOBUF_C_RPC_CLIENT_STATE_DESTROYED
} ProtobufC_RPC_ClientState;

typedef struct _Closure Closure;
struct _Closure
{
  /* these will be NULL for unallocated request ids */
  const ProtobufCMessageDescriptor *response_type;
  ProtobufCClosure closure;

  /* this is the next free element's request id, or 0 for none */  
  /* 当前系统是以 request_id 为键值通过单链表的方式把所有空闲 closure 连接起来的 */
  void *closure_data;
};

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

/* 定义了当前系统 RPC 客户端实例数据结构 */
struct _ProtobufC_RPC_Client
{
  /* 表示当前 RPC 客户端使用的 ProtobufC 服务实例，包含了支持的方法等信息 */
  ProtobufCService base_service;

  /* 表示当前 RPC 客户端使用的输入输出数据包缓冲区 */
  ProtobufCRPCDataBuffer incoming;
  ProtobufCRPCDataBuffer outgoing;

  /* 表示当前 RPC 客户端使用的内存分配器指针 */
  ProtobufCAllocator *allocator;

  /* 表示当前 RPC 客户端使用的 Dispatch 实例指针 */
  ProtobufCRPCDispatch *dispatch;

  /* 表示当前 RPC 客户端使用的 socket 地址类型 */
  ProtobufC_RPC_AddressType address_type;

  /* 用来记录当前 RPC 连接的服务端的主机名和端口号信息 */
  char *name;

  /* 表示当前 RPC 客户端使用的 socket 套接字文件描述符 */
  ProtobufC_RPC_FD fd;

  /* 表示当前 RPC 客户端在连接失败后是否尝试自动重新连接 */
  protobuf_c_boolean autoreconnect;

  /* 表示当前 RPC 客户端在连接失败后多长时间开始尝试自动重新连接 */
  unsigned autoreconnect_millis;

  /* 通过主机名查找与其对应的 IP 地址信息，如果查找成功则尝试连接对端服务器
	 实现函数为 trivial_sync_libc_resolver */
  ProtobufC_RPC_NameLookup_Func resolver;

  /* 表示当前 RPC 客户端实例的错误处理函数以及函数参数 */
  ProtobufC_RPC_Error_Func error_handler;
  void *error_handler_data;

  /* 表示当前 RPC 客户端实例使用的数据序列化和反序列化函数指针 */
  ProtobufC_RPC_Protocol rpc_protocol;

  /* 记录了 RPC 客户端当前状态信息 */
  ProtobufC_RPC_ClientState state;

  /* 定义了 RPC 客户端不同状态下使用的数据结构 */
  union {
    struct {
      ProtobufCRPCDispatchIdle *idle;
    } init;
    struct {
      protobuf_c_boolean pending;
      protobuf_c_boolean destroyed_while_pending;
      uint16_t port;
    } name_lookup;
	
    struct {
	  /* 表示当前 RPC 客户端的 closures 数组大小 */
      unsigned closures_alloced;

	  /* 记录当前 RPC 客户端的 closures 数组的第一个空闲元素的索引值
	     当前系统是以 request_id 为键值通过单链表的方式把所有空闲 closure 连接起来的 */
      unsigned first_free_request_id;
	  
      /* 记录当前 RPC 客户端的 closures 数组首地址，indexed by (request_id-1) */
      Closure *closures;
    } connected;
	
    struct {
      ProtobufCRPCDispatchTimer *timer;
      char *error_message;
    } failed_waiting;
    struct {
      char *error_message;
    } failed;
  } info;
};

static void begin_name_lookup (ProtobufC_RPC_Client *client);
static void destroy_client_rpc (ProtobufCService *service);

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
  protobuf_c_rpc_assert (flags >= 0);
  fcntl (fd, F_SETFL, flags | O_NONBLOCK);
}

/*********************************************************************************************************
** 函数名称: handle_autoreconnect_timeout
** 功能描述: 重新连接超时处理函数，用来尝试重新连接 RPC 服务端
** 输	 入: dispatch - RPC Dispatch 实例指针
**         : func_data - RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_autoreconnect_timeout (ProtobufCRPCDispatch *dispatch,
                          void              *func_data)
{
  ProtobufC_RPC_Client *client = func_data;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING);
  client->allocator->free (client->allocator,
                           client->info.failed_waiting.error_message);

  /* 开始执行指定的 RPC 客户端通过主机名查找与其对应的 IP 地址信息操作并尝试连接对端服务器 */
  begin_name_lookup (client);
}

/*********************************************************************************************************
** 函数名称: client_failed
** 功能描述: 客户端执行失败处理函数
** 输	 入: dispatch - RPC Dispatch 实例指针
**         : func_data - RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
client_failed (ProtobufC_RPC_Client *client,
               const char           *format_str,
               ...)
{
  va_list args;
  char buf[MAX_FAILED_MSG_LENGTH];
  size_t msg_len;
  char *msg;
  size_t n_closures = 0;
  Closure *closures = NULL;
  
  switch (client->state)
    {
    case PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP:
      protobuf_c_rpc_assert (!client->info.name_lookup.pending);
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTING:
      /* nothing to do */
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED:
      n_closures = client->info.connected.closures_alloced;
      closures = client->info.connected.closures;
      break;

      /* should not get here */
    case PROTOBUF_C_RPC_CLIENT_STATE_INIT:
    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING:
    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED:
    case PROTOBUF_C_RPC_CLIENT_STATE_DESTROYED:
      protobuf_c_rpc_assert (FALSE);
      break;
    }
  
  if (client->fd >= 0)
    {
      /* 从指定的 Dispatch 实例中释放和指定描述符对应的 FDNotify 和 FDNotifyChange
         成员并关闭指定文件描述符代表的文件 */
      protobuf_c_rpc_dispatch_close_fd (client->dispatch, client->fd);
      client->fd = -1;
    }

  /* 释放指定缓存空间中所有的缓存数据块占用的内存资源并设置缓存空间数据结构到复位状态 */
  protobuf_c_rpc_data_buffer_reset (&client->incoming);
  protobuf_c_rpc_data_buffer_reset (&client->outgoing);

  /* Compute the message */
  va_start (args, format_str);
  vsnprintf (buf, sizeof (buf), format_str, args);
  va_end (args);
  buf[sizeof(buf)-1] = 0;
  msg_len = strlen (buf);
  msg = client->allocator->alloc (client->allocator, msg_len + 1);
  strncpy (msg, buf, msg_len);
  msg[msg_len] = '\0';

  /* go to one of the failed states */
  if (client->autoreconnect)
    {
      client->state = PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING;

	  /* 向指定的 Dispatch 实例中添加一个指定的毫秒级的超时定时器实例，在定时器超时处理函数中
	     尝试重新连接 RPC 服务端 */
      client->info.failed_waiting.timer
        = protobuf_c_rpc_dispatch_add_timer_millis (client->dispatch,
                                                client->autoreconnect_millis,
                                                handle_autoreconnect_timeout,
                                                client);
      client->info.failed_waiting.error_message = msg;
    }
  else
    {
      client->state = PROTOBUF_C_RPC_CLIENT_STATE_FAILED;
      client->info.failed.error_message = msg;
    }

  /* we defer calling the closures to avoid
     any re-entrancy issues (e.g. people further RPC should
     not see a socket in the "connected" state-- at least,
     it shouldn't be accessing the array of closures that we are considering */
  if (closures != NULL)
    {
      unsigned i;

      for (i = 0; i < n_closures; i++)
        if (closures[i].response_type != NULL)
          closures[i].closure (NULL, closures[i].closure_data);
      client->allocator->free (client->allocator, closures);
    }
}

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
** 函数名称: set_state_connected
** 功能描述: 设置指定的 RPC 客户端到 CONNECTED 状态
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
set_state_connected (ProtobufC_RPC_Client *client)
{
  client->state = PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED;

  client->info.connected.closures_alloced = 1;
  client->info.connected.first_free_request_id = 1;
  client->info.connected.closures = client->allocator->alloc (client->allocator, sizeof (Closure));
  client->info.connected.closures[0].closure = NULL;
  client->info.connected.closures[0].response_type = NULL;
  client->info.connected.closures[0].closure_data = UINT_TO_POINTER (0);
}

/*********************************************************************************************************
** 函数名称: handle_client_fd_connect_events
** 功能描述: 处理指定的 socket 文件描述符上的连接建立完成事件
** 输	 入: fd - 指定的 socket 文件描述符
**         : events - 未使用
**         : callback_data - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_client_fd_connect_events (int         fd,
                                 unsigned    events,
                                 void       *callback_data)
{
  ProtobufC_RPC_Client *client = callback_data;
  socklen_t size_int = sizeof (int);
  int fd_errno = EINVAL;

  /* 获取指定的 socket 文件描述符执行状态错误码信息 */
  if (getsockopt (fd, SOL_SOCKET, SO_ERROR, &fd_errno, &size_int) < 0)
    {
      /* Note: this behavior is vaguely hypothetically broken,
       *       in terms of ignoring getsockopt's error;
       *       however, this shouldn't happen, and EINVAL is ok if it does.
       *       Furthermore some broken OS's return an error code when
       *       fetching SO_ERROR!
       */
    }

  /* 如果成功建立了 socket 通信连接则设置当前客户端 RPC 到 CONNECTED 状态 */
  if (fd_errno == 0)
    {
      /* goto state CONNECTED */
      protobuf_c_rpc_dispatch_watch_fd (client->dispatch,
                                    client->fd,
                                    0, NULL, NULL);
      set_state_connected (client);
    }
  else if (errno_is_ignorable (fd_errno))
    {
      /* remain in CONNECTING state */
      return;
    }
  else
    {
      /* Call error handler */
      client_failed (client,
                     "failed connecting to server: %s",
                     strerror (fd_errno));
    }
}

/*********************************************************************************************************
** 函数名称: begin_connecting
** 功能描述: 根据指定的 RPC 服务端地址信息尝试连接 RPC 服务端
** 输	 入: client - 指定的 RPC 客户端实例指针
**         : address - 指定的 RPC 服务端地址信息
**         : addr_len - 指定的 RPC 服务端地址信息长度
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
begin_connecting (ProtobufC_RPC_Client *client,
                  struct sockaddr      *address,
                  size_t                addr_len)
{
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP);

  client->state = PROTOBUF_C_RPC_CLIENT_STATE_CONNECTING;
  
  client->fd = socket (address->sa_family, SOCK_STREAM, 0);
  if (client->fd < 0)
    {
      client_failed (client, "error creating socket: %s", strerror (errno));
      return;
    }

  /* 设置指定的文件描述符为非阻塞模式 */
  set_fd_nonblocking (client->fd);
  
  if (connect (client->fd, address, addr_len) < 0)
    {
      if (errno == EINPROGRESS)
        {
          /* register interest in fd */
		  /* 为指定的 Dispatch 实例注册 READABLE 和 WRITABLE 监听事件，用来检测发起的
		     连接请求执行完成事件 */
          protobuf_c_rpc_dispatch_watch_fd (client->dispatch,
                                        client->fd,
                                        PROTOBUF_C_RPC_EVENT_READABLE|PROTOBUF_C_RPC_EVENT_WRITABLE,
                                        handle_client_fd_connect_events,
                                        client);
          return;
        }
      close (client->fd);
      client->fd = -1;
      client_failed (client, "error connecting to remote host: %s", strerror (errno));
      return;
    }

  /* 设置当前的 RPC 客户端到 CONNECTED 状态 */
  set_state_connected (client);
}

/*********************************************************************************************************
** 函数名称: handle_name_lookup_success
** 功能描述: 在通过主机名查找与其对应的 IP 地址信息成功时调用，用来尝试连接对端服务器
** 输	 入: address - 指定的 RPC 服务端地址信息
**         : callback_data - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_name_lookup_success (const uint8_t *address,
                            void          *callback_data)
{
  ProtobufC_RPC_Client *client = callback_data;
  struct sockaddr_in addr;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP);
  protobuf_c_rpc_assert (client->info.name_lookup.pending);
  
  client->info.name_lookup.pending = 0;
  
  if (client->info.name_lookup.destroyed_while_pending)
    {
      destroy_client_rpc (&client->base_service);
      return;
    }

  /* 构建待连接的 RPC 服务端地址信息 */
  memset (&addr, 0, sizeof (addr));
  addr.sin_family = AF_INET;
  memcpy (&addr.sin_addr, address, 4);
  addr.sin_port = htons (client->info.name_lookup.port);

  /* 根据指定的 RPC 服务端地址信息尝试连接 RPC 服务端 */
  begin_connecting (client, (struct sockaddr *) &addr, sizeof (addr));
}

/*********************************************************************************************************
** 函数名称: handle_name_lookup_failure
** 功能描述: 在通过主机名查找与其对应的 IP 地址信息失败时调用，用来释放当前客户端占用的资源并打印信息
** 输	 入: error_message - 指定的错误信息
**         : callback_data - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_name_lookup_failure (const char    *error_message,
                            void          *callback_data)
{
  ProtobufC_RPC_Client *client = callback_data;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP);
  protobuf_c_rpc_assert (client->info.name_lookup.pending);
  
  client->info.name_lookup.pending = 0;
  
  if (client->info.name_lookup.destroyed_while_pending)
    {
      destroy_client_rpc (&client->base_service);
      return;
    }
  
  client_failed (client, "name lookup failed (for name from %s): %s", client->name, error_message);
}

/*********************************************************************************************************
** 函数名称: begin_name_lookup
** 功能描述: 开始执行指定的 RPC 客户端通过主机名查找与其对应的 IP 地址信息操作并尝试连接对端服务器
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
begin_name_lookup (ProtobufC_RPC_Client *client)
{
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_INIT
                 ||  client->state == PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING
                 ||  client->state == PROTOBUF_C_RPC_CLIENT_STATE_FAILED);

  /* 更新当前 RPC 客户端实例状态到 NAME_LOOKUP */
  client->state = PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP;
  client->info.name_lookup.pending = 0;
  
  switch (client->address_type)
    {
    case PROTOBUF_C_RPC_ADDRESS_LOCAL:
      {
        struct sockaddr_un addr;
        addr.sun_family = AF_UNIX;
        strncpy (addr.sun_path, client->name, sizeof (addr.sun_path));

		/* 根据指定的地址信息开始尝试连接 RPC 服务端 */
        begin_connecting (client, (struct sockaddr *) &addr,
                          sizeof (addr));
        return;
      }

    case PROTOBUF_C_RPC_ADDRESS_TCP:
      {
        /* parse hostname:port from client->name */
        const char *colon = strchr (client->name, ':');
        char *host;
        unsigned port;
		
        if (colon == NULL)
          {
            client_failed (client,
                           "name '%s' does not have a : in it (supposed to be HOST:PORT)",
                           client->name);
            return;
          }

		/* 从客户端实例的 client->name 中解析出我们需要的 RPC 服务端主机名和端口号信息 */
        host = client->allocator->alloc (client->allocator, colon + 1 - client->name);
        memcpy (host, client->name, colon - client->name);
        host[colon - client->name] = 0;
        port = atoi (colon + 1);

        client->info.name_lookup.pending = 1;
        client->info.name_lookup.destroyed_while_pending = 0;
        client->info.name_lookup.port = port;

		/* 通过主机名查找与其对应的 IP 地址信息，如果查找成功则尝试连接对端服务器
		   实现函数为 trivial_sync_libc_resolver */
        client->resolver (client->dispatch,
                          host,
                          handle_name_lookup_success,
                          handle_name_lookup_failure,
                          client);

        /* cleanup */
        client->allocator->free (client->allocator, host);
        return;
      }
    default:
      assert (0);
    }
}

/*********************************************************************************************************
** 函数名称: handle_init_idle
** 功能描述: 开始处理指定的 RPC 客户端实例的初始化流程
** 输	 入: dispatch - 未使用
**         : data - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_init_idle (ProtobufCRPCDispatch *dispatch,
                  void              *data)
{
  ProtobufC_RPC_Client *client = data;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_INIT);

  /* 开始执行指定的 RPC 客户端通过主机名查找与其对应的 IP 地址信息操作并尝试连接对端服务器 */
  begin_name_lookup (client);
}

/*********************************************************************************************************
** 函数名称: grow_closure_array
** 功能描述: 扩充指定的 RPC 客户端的 closures 数组为之前的 2 倍
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
grow_closure_array (ProtobufC_RPC_Client *client)
{
  /* resize array */
  unsigned old_size = client->info.connected.closures_alloced;
  unsigned new_size = old_size * 2;
  unsigned i;

  /* 把当前 RPC 客户端以前的 closures 数据复制到新申请的内存块中 */
  Closure *new_closures = client->allocator->alloc (client->allocator, sizeof (Closure) * new_size);
  memcpy (new_closures,
          client->info.connected.closures,
          sizeof (Closure) * old_size);

  /* build new free list */
  /* 初始化新申请的 closures 数据结构 */
  for (i = old_size; i < new_size - 1; i++)
    {
      new_closures[i].response_type = NULL;
      new_closures[i].closure = NULL;
      new_closures[i].closure_data = UINT_TO_POINTER (i+2);
    }
  
  new_closures[i].closure_data = UINT_TO_POINTER (client->info.connected.first_free_request_id);
  new_closures[i].response_type = NULL;
  new_closures[i].closure = NULL;
  client->info.connected.first_free_request_id = old_size + 1;

  client->allocator->free (client->allocator, client->info.connected.closures);
  client->info.connected.closures = new_closures;
  client->info.connected.closures_alloced = new_size;
}

/*********************************************************************************************************
** 函数名称: uint32_to_le
** 功能描述: 把指定的本机字节序的 uint32_t 变量转换成与其对应的小端格式
** 输	 入: le - 指定的 uint32_t 变量
** 输	 出: uint32_t - 转换后的小端格式变量值
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

/*********************************************************************************************************
** 函数名称: enqueue_request
** 功能描述: 把指定方法的服务请求数据追加到指定的 RPC 客户端实例的 client->outgoing 缓冲区末尾位置
**         : 并把这个方法的 closure 添加到指定的 RPC 客户端实例的 closure 数组队列中
** 输	 入: client - 指定的 RPC 客户端实例指针
**         : method_index - 指定的 method 索引值
**         : input - 和指定的 method 相关的负载数据
**         : closure - 
**         : closure_data - 
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
enqueue_request (ProtobufC_RPC_Client *client,
                 unsigned          method_index,
                 const ProtobufCMessage *input,
                 ProtobufCClosure  closure,
                 void             *closure_data)
{
  const ProtobufCServiceDescriptor *desc = client->base_service.descriptor;
  const ProtobufCMethodDescriptor *method = desc->methods + method_index;

  protobuf_c_rpc_assert (method_index < desc->n_methods);

  /* Allocate request_id */
  //protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED);
  /* 如果当前的 RPC 客户端的 closures 数组已用尽则对其进行扩容 */
  if (client->info.connected.first_free_request_id == 0)
    grow_closure_array (client);

  uint32_t request_id = client->info.connected.first_free_request_id;

  /* Serialize the message */
  ProtobufC_RPC_Payload payload = {method_index,
                                   request_id,
                                   (ProtobufCMessage *)input};

  /* 对指定的服务请求数据执行序列化并追加到指定的 client->outgoing 缓冲区末尾位置
     实现函数为 client_serialize */
  client->rpc_protocol.serialize_func (desc, client->allocator,
        &client->outgoing.base, payload);

  /* Add closure to request-tree */
  Closure *cl = client->info.connected.closures + (request_id - 1);
  client->info.connected.first_free_request_id = POINTER_TO_UINT (cl->closure_data);
  
  cl->response_type = method->output;
  cl->closure = closure;
  cl->closure_data = closure_data;
}

/*********************************************************************************************************
** 函数名称: get_rcvd_message_descriptor
** 功能描述: 获取指定的 RPC 客户端实例指定的 request_id 的 ProtobufC 消息描述符指针
** 输	 入: payload - 指定的 RPC 负载数据指针
**         : data - 指定的 RPC 客户端实例指针
** 输	 出: closure->response_type - 获取到的 ProtobufC 消息描述符指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static const ProtobufCMessageDescriptor *
get_rcvd_message_descriptor (const ProtobufC_RPC_Payload *payload, void *data)
{
   ProtobufC_RPC_Client *client = (ProtobufC_RPC_Client *)data;
   uint32_t request_id = payload->request_id;

   /* lookup request by id */
   if (request_id > client->info.connected.closures_alloced
         || request_id == 0
         || client->info.connected.closures[request_id-1].response_type == NULL)
   {
      client_failed (client, "bad request-id in response from server");
      return NULL;
   }

   Closure *closure = client->info.connected.closures + (request_id - 1);
   return closure->response_type;
}

/*********************************************************************************************************
** 函数名称: handle_client_fd_events
** 功能描述: 用来处理指定的 RPC 客户端文件描述符上的事件
** 输	 入: fd - 指定的 RPC 客户端文件描述符
**         : events - 指定的 RPC 客户端文件描述符上的事件
**         : func_data - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
handle_client_fd_events (int                fd,
                         unsigned           events,
                         void              *func_data)
{
  ProtobufC_RPC_Client *client = func_data;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED);
  
  if (events & PROTOBUF_C_RPC_EVENT_WRITABLE)
    {
      /* 从指定的缓存空间头部开始读取尽可能多的数据并通过 writev 写入到指定的文件中
         然后“释放”掉被处理数据的缓存数据块占用的内存资源 */
      int write_rv = protobuf_c_rpc_data_buffer_writev (&client->outgoing,
                                                    client->fd);
      if (write_rv < 0 && !errno_is_ignorable (errno))
        {
          client_failed (client,
                         "writing to file-descriptor: %s",
                         strerror (errno));
          return;
        }

      if (client->outgoing.size == 0)
      	{
      	  /* 为指定的 Dispatch 实例注册一个 READABLE 监测事件，用来处理 RPC 客户端接收到的数据 */
          protobuf_c_rpc_dispatch_watch_fd (client->dispatch, client->fd,
                                      PROTOBUF_C_RPC_EVENT_READABLE,
                                      handle_client_fd_events, client);
      	}
    }
  
  if (events & PROTOBUF_C_RPC_EVENT_READABLE)
    {
      /* do read */
	  /* 从指定的文件描述符中尝试读取 8192 字节数数据并追加到指定的缓存空间中 */
      int read_rv = protobuf_c_rpc_data_buffer_read_in_fd (&client->incoming,
                                                       client->fd);
      if (read_rv < 0)
        {
          if (!errno_is_ignorable (errno))
            {
              client_failed (client,
                             "reading from file-descriptor: %s",
                             strerror (errno));
            }
        }
      else if (read_rv == 0)
        {
          /* handle eof */
          client_failed (client,
                         "got end-of-file from server [%u bytes incoming, %u bytes outgoing]",
                         client->incoming.size, client->outgoing.size);
        }
      else
        {
          /* try processing buffer */
          while (client->incoming.size > 0)
            {
              /* Deserialize the buffer */
              ProtobufC_RPC_Payload payload = {0};

			  /* 对从 RPC 服务端接收到的数据进行反序列化操作，具体实现函数为 client_deserialize */
              ProtobufC_RPC_Protocol_Status status =
                client->rpc_protocol.deserialize_func (client->base_service.descriptor,
                                                       client->allocator,
                                                       &client->incoming,
                                                       &payload,
                                                       get_rcvd_message_descriptor,
                                                       (void *) client);

              if (status == PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER)
                break;

              if (status == PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED)
              {
                client_failed (client, "error deserialing server response");
                return;
              }

              /* invoke closure */
              Closure *closure = client->info.connected.closures + (payload.request_id - 1);
              closure->closure (payload.message, closure->closure_data);

			  /* 把用完的 closure 归还给当前 RPC 客户端实例的 closure->closure_data 数组
			     当前系统是以 request_id 为键值通过单链表的方式把所有空闲 closure 连接起来的 */
              closure->response_type = NULL;
              closure->closure = NULL;
              closure->closure_data = UINT_TO_POINTER (client->info.connected.first_free_request_id);
              client->info.connected.first_free_request_id = payload.request_id;

              /* clean up */
			  /* 释放指定的 ProtobufC 消息实例结构占用的内存资源 */
              if (payload.message)
                protobuf_c_message_free_unpacked (payload.message, client->allocator);
            }
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
** 函数名称: client_serialize
** 功能描述: 把指定的 RPC 负载数据序列化并追加到指定的 ProtobufC 缓冲区末尾位置
** 输	 入: descriptor - 未使用
**         : allocator - 指定的内存分配器指针
**         : payload - 需要发送的 RPC 负载数据
** 输	 出: out_buffer - 指定的发送缓冲区
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS - 操作成功
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED - 操作失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ProtobufC_RPC_Protocol_Status client_serialize (const ProtobufCServiceDescriptor *descriptor,
                                                      ProtobufCAllocator *allocator,
                                                      ProtobufCBuffer *out_buffer,
                                                      ProtobufC_RPC_Payload payload)
{
   if (!out_buffer)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   if (!protobuf_c_message_check (payload.message))
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   size_t message_length = protobuf_c_message_get_packed_size (payload.message);
   uint32_t header[3];
   header[0] = uint32_to_le (payload.method_index);
   header[1] = uint32_to_le (message_length);
   header[2] = payload.request_id;
   out_buffer->append (out_buffer, sizeof (header), (const uint8_t *) header);

   /* 把指定的消息数据序列化并追加到指定的 ProtobufC 缓冲区末尾位置 */
   size_t packed_length = protobuf_c_message_pack_to_buffer (payload.message,
                                                             out_buffer);
   if (packed_length != message_length)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   return PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS;
}

/*********************************************************************************************************
** 函数名称: client_deserialize
** 功能描述: 解析指定的 ProtobufC 序列化数据并把解析结果存储到指定的 RPC 负载数据中 
** 输	 入: descriptor - 未使用
**         : allocator - 指定的内存分配器指针
**         : in_buffer - 从客户端接收到的输入数据包指针
**         : get_descriptor - 获取和指定的 RPC 负载数据相关的方法的输入消息描述符指针
**         : get_descriptor_data - 指向了当前接收消息的 RPC 服务连接
** 输	 出: payload - 存储接收到的 RPC 负载数据信息
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS - 操作成功
**         : PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED - 操作失败
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static ProtobufC_RPC_Protocol_Status client_deserialize (const ProtobufCServiceDescriptor *descriptor,
                                                        ProtobufCAllocator    *allocator,
                                                        ProtobufCRPCDataBuffer       *in_buffer,
                                                        ProtobufC_RPC_Payload *payload,
                                                        ProtobufC_RPC_Get_Descriptor get_descriptor,
                                                        void *get_descriptor_data)
{
   if (!allocator || !in_buffer || !payload)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

   uint32_t header[4];
   if (in_buffer->size < sizeof (header))
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER;

   /* try processing buffer */
   protobuf_c_rpc_data_buffer_peek (in_buffer, header, sizeof (header));
   uint32_t status_code = uint32_from_le (header[0]);
   payload->method_index = uint32_from_le (header[1]);
   uint32_t message_length = uint32_from_le (header[2]);
   payload->request_id = header[3];           /* already native-endian */

   if (sizeof (header) + message_length > in_buffer->size)
      return PROTOBUF_C_RPC_PROTOCOL_STATUS_INCOMPLETE_BUFFER;

   /* Discard the RPC header */
   protobuf_c_rpc_data_buffer_discard (in_buffer, sizeof (header));

   ProtobufCMessage *msg;
   if (status_code == PROTOBUF_C_RPC_STATUS_CODE_SUCCESS)
   {
      /* read message and unpack */
      const ProtobufCMessageDescriptor *desc = get_descriptor (payload, get_descriptor_data);
      if (!desc)
         return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

      uint8_t *packed_data = allocator->alloc (allocator, message_length);

      if (!packed_data && message_length > 0)
         return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;

      if (message_length !=
          protobuf_c_rpc_data_buffer_read (in_buffer, packed_data, message_length))
      {
         if (packed_data)
            allocator->free (allocator, packed_data);
         return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;
      }

      /* TODO: use fast temporary allocator */
      msg = protobuf_c_message_unpack (desc,
            allocator,
            message_length,
            packed_data);

      if (packed_data)
         allocator->free (allocator, packed_data);
      if (msg == NULL)
         return PROTOBUF_C_RPC_PROTOCOL_STATUS_FAILED;
   }
   else
   {
      /* Server did not send a response message */
      protobuf_c_rpc_assert (message_length == 0);
      msg = NULL;
   }
   payload->message = msg;

   return PROTOBUF_C_RPC_PROTOCOL_STATUS_SUCCESS;
}

/*********************************************************************************************************
** 函数名称: update_connected_client_watch
** 功能描述: 根据指定的 RPC 客户端实例的数据包状态更新与其对应的文件描述符监测事件
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
update_connected_client_watch (ProtobufC_RPC_Client *client)
{
  unsigned events = PROTOBUF_C_RPC_EVENT_READABLE;
  protobuf_c_rpc_assert (client->state == PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED);
  protobuf_c_rpc_assert (client->fd >= 0);
  
  if (client->outgoing.size > 0)
    events |= PROTOBUF_C_RPC_EVENT_WRITABLE;
  
  protobuf_c_rpc_dispatch_watch_fd (client->dispatch,
                                client->fd,
                                events,
                                handle_client_fd_events, client);
}

/*********************************************************************************************************
** 函数名称: invoke_client_rpc
** 功能描述: 调用指定的 RPC 客户端实例中的指定的方法
** 输	 入: service - 指定的 RPC 客户端实例指针
**         : method_index - 需要调用的 method 索引值
**         : input - 和指定的 method 相关的负载数据
**         : closure - 
**         : closure_data - 
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
invoke_client_rpc (ProtobufCService *service,
                   unsigned          method_index,
                   const ProtobufCMessage *input,
                   ProtobufCClosure  closure,
                   void             *closure_data)
{
  ProtobufC_RPC_Client *client = (ProtobufC_RPC_Client *) service;
  protobuf_c_rpc_assert (service->invoke == invoke_client_rpc);
  
  switch (client->state)
    {
    case PROTOBUF_C_RPC_CLIENT_STATE_INIT:
    case PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP:
    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTING:
      enqueue_request (client, method_index, input, closure, closure_data);
      break;

    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED:
      {
        int had_outgoing = (client->outgoing.size > 0);
        enqueue_request (client, method_index, input, closure, closure_data);
        if (!had_outgoing)
          update_connected_client_watch (client);
      }
      break;

    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING:
    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED:
    case PROTOBUF_C_RPC_CLIENT_STATE_DESTROYED:
      closure (NULL, closure_data);
      break;
    }
}

/*********************************************************************************************************
** 函数名称: destroy_client_rpc
** 功能描述: 销毁指定的 RPC 客户端服务实例以及占用的所有资源
** 输	 入: service - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
destroy_client_rpc (ProtobufCService *service)
{
  ProtobufC_RPC_Client *client = (ProtobufC_RPC_Client *) service;
  ProtobufC_RPC_ClientState state = client->state;
  unsigned i;
  unsigned n_closures = 0;
  Closure *closures = NULL;
  
  switch (state)
    {
    case PROTOBUF_C_RPC_CLIENT_STATE_INIT:
	  /* 把指定的 idle functions 实例从其所属的 Dispatch 实例中移除 */
      protobuf_c_rpc_dispatch_remove_idle (client->info.init.idle);
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_NAME_LOOKUP:
      if (client->info.name_lookup.pending)
        {
          client->info.name_lookup.destroyed_while_pending = 1;
          return;
        }
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTING:
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED:
      n_closures = client->info.connected.closures_alloced;
      closures = client->info.connected.closures;
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED_WAITING:
	  /* 把指定的超时定时器实例从其所属的 Dispatch 实例中移除 */
      protobuf_c_rpc_dispatch_remove_timer (client->info.failed_waiting.timer);
      client->allocator->free (client->allocator, client->info.failed_waiting.timer);
      client->allocator->free (client->allocator, client->info.failed_waiting.error_message);
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_FAILED:
      client->allocator->free (client->allocator, client->info.failed.error_message);
      break;
    case PROTOBUF_C_RPC_CLIENT_STATE_DESTROYED:
      protobuf_c_rpc_assert (0);
      break;
    }
  
  if (client->fd >= 0)
    {
      /* 从指定的 Dispatch 实例中释放和指定文件描述符对应的 FDNotify 和 FDNotifyChange 成员
         并关闭指定文件描述符代表的文件 */
      protobuf_c_rpc_dispatch_close_fd (client->dispatch, client->fd);
      client->fd = -1;
    }

  /* 释放指定缓存空间中所有的缓存数据块占用的内存资源 */
  protobuf_c_rpc_data_buffer_clear (&client->incoming);
  protobuf_c_rpc_data_buffer_clear (&client->outgoing);
  
  client->state = PROTOBUF_C_RPC_CLIENT_STATE_DESTROYED;
  client->allocator->free (client->allocator, client->name);

  /* free closures only once we are in the destroyed state */
  for (i = 0; i < n_closures; i++)
    if (closures[i].response_type != NULL)
      closures[i].closure (NULL, closures[i].closure_data);
	
  if (closures)
    client->allocator->free (client->allocator, closures);

  client->allocator->free (client->allocator, client);
}

/*********************************************************************************************************
** 函数名称: begin_name_lookup
** 功能描述: 通过主机名查找与其对应的 IP 地址信息，如果查找成功则尝试连接对端服务器
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
static void
trivial_sync_libc_resolver (ProtobufCRPCDispatch *dispatch,
                            const char        *name,
                            ProtobufC_RPC_NameLookup_Found found_func,
                            ProtobufC_RPC_NameLookup_Failed failed_func,
                            void *callback_data)
{
  struct hostent *ent;

  /* 用域名或主机名获取与其对应的 IP 地址信息 */
  ent = gethostbyname (name);
  if (ent == NULL)
    failed_func (hstrerror (h_errno), callback_data);
  else
    found_func ((const uint8_t *) ent->h_addr_list[0], callback_data);
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_new
** 功能描述: 通过指定的参数创建并初始化一个 RPC 客户端实例结构
** 输	 入: type - 新的 RPC 客户端使用的 socket 地址类型
**         : name - 当前 RPC 客户端需要连接的 RPC 服务端的主机名
**         : descriptor - 为新的 RPC 客户端指定的 ProtobufC 服务结构指针
**         : orig_dispatch - 新的 RPC 客户端使用的 Dispatch 实例指针
** 输	 出: ProtobufCService * - 新创建的 RPC 客户端实例结构指针
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
ProtobufCService *protobuf_c_rpc_client_new (ProtobufC_RPC_AddressType type,
                                             const char               *name,
                                             const ProtobufCServiceDescriptor *descriptor,
                                             ProtobufCRPCDispatch       *orig_dispatch)
{
  ProtobufCRPCDispatch *dispatch = orig_dispatch ? orig_dispatch : protobuf_c_rpc_dispatch_default ();
  ProtobufCAllocator *allocator = protobuf_c_rpc_dispatch_peek_allocator (dispatch);
  ProtobufC_RPC_Client *rv = allocator->alloc (allocator, sizeof (ProtobufC_RPC_Client));
  
  rv->base_service.descriptor = descriptor;
  rv->base_service.invoke = invoke_client_rpc;
  rv->base_service.destroy = destroy_client_rpc;
  protobuf_c_rpc_data_buffer_init (&rv->incoming, allocator);
  protobuf_c_rpc_data_buffer_init (&rv->outgoing, allocator);
  rv->allocator = allocator;
  rv->dispatch = dispatch;
  rv->address_type = type;
  rv->state = PROTOBUF_C_RPC_CLIENT_STATE_INIT;
  rv->fd = -1;
  rv->autoreconnect = 1;
  rv->autoreconnect_millis = 2*1000;
  rv->resolver = trivial_sync_libc_resolver;
  rv->error_handler = error_handler;
  rv->error_handler_data = "protobuf-c rpc client";
  rv->info.init.idle = protobuf_c_rpc_dispatch_add_idle (dispatch, handle_init_idle, rv);
  ProtobufC_RPC_Protocol default_rpc_protocol = {client_serialize, client_deserialize};
  rv->rpc_protocol = default_rpc_protocol;

  size_t name_len = strlen (name);
  rv->name = allocator->alloc (allocator, name_len + 1);
  if (!rv->name)
     return NULL;
  
  strncpy (rv->name, name, name_len);
  rv->name[name_len] = '\0';

  return &rv->base_service;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_is_connected
** 功能描述: 判断指定的 RPC 客户端是否处于 CONNECTED 状态
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: TRUE - 处于 CONNECTED 状态
**         : FALSE - 没处于 CONNECTED 状态
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
protobuf_c_boolean
protobuf_c_rpc_client_is_connected (ProtobufC_RPC_Client *client)
{
  return client->state == PROTOBUF_C_RPC_CLIENT_STATE_CONNECTED;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_set_autoreconnect_period
** 功能描述: 启动指定的 RPC 客户端连接失败后自动尝试重新连接功能已经自动尝试重新连接的间隔时间毫秒数
** 输	 入: client - 指定的 RPC 客户端实例指针
**         : millis - 指定的自动尝试重新连接的间隔时间毫秒数
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_client_set_autoreconnect_period (ProtobufC_RPC_Client *client,
                                            unsigned millis)
{
  client->autoreconnect = 1;
  client->autoreconnect_millis = millis;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_set_error_handler
** 功能描述: 设置指定的 RPC 客户端的错误处理函数以及函数参数
** 输	 入: client - 指定的 RPC 客户端实例指针
**         : func - 指定的错误处理函数指针
**         : func_data - 指定的错误处理函数参数指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_client_set_error_handler (ProtobufC_RPC_Client *client,
                                         ProtobufC_RPC_Error_Func func,
                                         void                    *func_data)
{
  client->error_handler = func;
  client->error_handler_data = func_data;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_disable_autoreconnect
** 功能描述: 关闭指定的 RPC 客户端在连接失败后自动尝试重新连接的功能
** 输	 入: client - 指定的 RPC 客户端实例指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_client_disable_autoreconnect (ProtobufC_RPC_Client *client)
{
  client->autoreconnect = 0;
}

/*********************************************************************************************************
** 函数名称: protobuf_c_rpc_client_set_rpc_protocol
** 功能描述: 设置指定的 RPC 客户端使用的数据序列化和反序列化函数指针
** 输	 入: client - 指定的 RPC 客户端实例指针
**         : protocol - 指定的数据序列化和反序列化函数指针
** 输	 出: 
** 全局变量: 
** 调用模块: 
*********************************************************************************************************/
void
protobuf_c_rpc_client_set_rpc_protocol (ProtobufC_RPC_Client *client,
                                        ProtobufC_RPC_Protocol protocol)
{
   client->rpc_protocol = protocol;
}
