# NET

`net`库是基于`folly::future`,`boost::asio`基础之上的一个网络层的封装.

---

使用本框架前,需要预先了解的知识:

[http介绍](https://www.runoob.com/http/http-tutorial.html) [ASIO](https://www.boost.org/doc/libs/1_72_0/doc/html/boost_asio.html) [boost.beast](https://www.boost.org/doc/libs/1_72_0/libs/beast/doc/html/index.html)

---

## 设计层次

- `session`&`unary`
- `websocket`&`http`&`ips`&`stp`
- `ipc_stream`&`tcp_stream`

---

自底向上而言,`stream`层提供统一的流式抽象.此处的流被认为有以下特征:

- 信元顺序到达
- 信元不会丢失
- 支持异步读写
- 感知对端状态

`stream`层目前只支持2种,即`tcp`与`ipc`.

注意在`stream`层没有消息的概念.每次传输并不一定传输一个完整大小的数据包.

---

中间层为传输协议层.在`tcp`上采用了`http`与`websocket`.在`ipc`上采用了`http`与`自制协议`.

对于请求回应而言,`http`协议提供的`header`&`body`功能已经比较完善,且一般情况下请求回应做信令控制使用,因此不必追求过高的序列化性能.未节省代码复杂程度,故在`ipc`上也采用此模式.

对于`session`而言,`websocket`中有许多机制提供了对服务器的保护功能,比如掩膜`MASK`.这些技术在本机通讯(`IPC`)上不仅不需要,反而成为了性能瓶颈.因此需要使用一套更为简洁的协议去适应该高速传输的模式.

---

上层提供了统一的抽象应用.比如请求回应`unary`与双向流`session`的概念,方便使用.

总体上来看,`net`库应当是一个内部基础通讯框架,只是在`tcp`上以`restful`样式提供了统一对外接口.

---

## 设计理念

基于简介易用的原则.

- `net`库中任意一次通讯,都是依赖于一条独立的底层`stream`.因此:
  - 相互之间不会受到干扰.
  - 可以感知对端断线.
  - 避免复杂断线重连处理.
- `net`库仅使用一个线程驱动`asio`.

---

## 设计简介

```c++
using Headers = std::unordered_map<std::string, std::string>;
struct Message {
    std::string body;
    Headers headers;

    struct status {
        unsigned int code = 200;
        std::string reason;
    } status;
};
```

`http`消息是贯穿全文的一个主要的数据结构.其各字段显而易见,不再做过多描述.

```c++
template<typename SCHEME>
struct Protocol {
};

template<>
struct Protocol<ipc> {
    using ProtocolType = ipc;
    using SocketType = ProtocolType::socket;
    using EndpointType = ProtocolType::endpoint;
    using AcceptorType = ProtocolType::acceptor;
    using StreamType = SocketType;
};

template<>
struct Protocol<tcp> {
    using ProtocolType = tcp;
    using SocketType = ProtocolType::socket;
    using EndpointType = ProtocolType::endpoint;
    using AcceptorType = ProtocolType::acceptor;
    using StreamType = boost::beast::tcp_stream;
};
```

全文使用类型萃取机制分离不同协议的不同处理部分,使用继承机制将他们组合在一起.

```c++
template<template<typename> class FUNC, typename... Args>
auto RunFuncWithScheme(string_view scheme, Args&& ... args) {
    using namespace ecv::net;
    if (scheme == Ipc::scheme || scheme == Ipc::unary || scheme == Ipc::stream)
        return FUNC<ipc>()(std::forward<Args>(args)...);
    else if (scheme == Tcp::scheme || scheme == Tcp::unary || scheme == Tcp::stream)
        return FUNC<tcp>()(std::forward<Args>(args)...);
    else
        throw ecv::net::Exception("invalid scheme");
    //can not go to here
    return FUNC<tcp>()(std::forward<Args>(args)...);
}
```

使用上述函数按协议自动分离.

---

### 客户端

#### Unary

`unary`表示标准的请求回应操作.其在客户端中为:

```c++
static folly::SemiFuture<Message> TransactUnary(
        std::string_view method,
        std::string_view host,
        std::string_view target = {},
        const Message& request = {});
```

内部流程为:

1. 创建对应底层`stream`
2. 转化`Message`为`http.request`,并填充必要字段
3. 设置中断回调
4. 发送`http`请求,获得`http`回应.
5. 将`http.response`转化为`Message`

---

#### Stream

```c++
static folly::SemiFuture<std::unique_ptr<Session>> TransactStream(
        std::string_view host,
        std::string_view target = {},
        const Headers& headers = {});
```

内部流程为:

1. 创建对应`session`
2. 进行握手

---

### 服务器

服务器层次如下:

- `server`
- `wsService`,`ipcService`
- `Engine`

其中,`enigne`是基类,存储了`unary`,`Session`的回调函数等基本处理信息.负责最基本的,统一的功能.

`wsService`等是`Engine`的子类,里面提供了因协议处理方式不同而特殊化的处理内容.

`server`持有一个`Engine`智能指针,负责管理其生命周期.

其工作流程如下:

1. 启动`asio`协程监听指定`socket`.
2. 检测到客户端进入时,在底层`stream`中读取`http`请求
3. 若符合升级`session`的协议要求,则做`session`处理,否则做`unary`处理
   - 检查`engine`中的`unary`列表,若不存在则`404`,否则进行处理
   - 检查`engine`中的`session`列表,若不存在则`404`,否则进行处理
     - 对`session`进行`accpet`,此步骤对应客户端的`handshake`
     - 调用处理回调函数
4. 对`unary`请求做`keep-alive`循环处理.

---

### Session

`Session`是本库中的主要概念.其层次如下:

- `WsSession`,`IpcSession`
- `BaseSession`
- `Session`

---

`Session`是一个抽象接口,其描述了对外使用的`api`.在`net_1.0.0`时,本不具备`IsOpen()&Close()`接口.其设计理念认为`Session`本身是`RALL`机制的,当`Session`释放时,自动关闭.但是实际上这个想法是不正确的.

例如一个`Mutex`,其自身只有2种状态,且其接口无论`lock() || unlock()`均不会造成`Mutex`失效的问题.此时使用此机制无可厚非.但是相对而言,`IO`可能会受限于其他外部影响.无论是网络读写还是文件读写等,其`read()||write()`接口都可能导致句柄本身失效.一个失效的句柄应该被上层用户感知并且择机释放.

综上所述,`Session`存在2种释放条件:

- 用户主动关闭
- `IO`异常导致的不可用

想要自动化释放确有办法,使用二对`weak-shared`智能指针在`io`操作函数与存储`session`的容器中相互关联.每次使用时通过`weak`函数观察对端是否存在.但是这样一个结构比较繁杂且容易出错.因此在`net_1.1`中添加了 `IsOpen()`接口辅助检查. 用户可以择机遍历容器,关闭已经 `close`的`session`.其使用更加简便.

在`net1.1`中,添加了主动`close(string_view)`接口,允许流在关闭时传达一条信息,以区分主动断线或被动断线.

---

`BaseSession`是实现了所有`Session`所需要的公有功能.

- `DEBUG`模式下单`Session`多线程`读/写`检测.
- `Session`状态控制(`IsOpen`).
- 数据合法性检查.

在检测到某一次`IO`存在异常时,会将此`Session`设置为不可用.

`BaseSession`还为其子类`Session`规定了握手(`handshake`)与`Accept`.

---

`WsSession`是`boost.beast`的封装.

---

`IpcSession`是简单的自有协议.该协议设计时参考了`ZMQ`的协议设计.

对于握手与接受环节,同`websocket`协议,使用到了`http`协议作为载体.该`Session`的主要性能压力在于高速数据传输,而握手环节本身并未立即开始传输,主要是用于约定一些行为.且正常情况下不会频繁断开/重建连接.因此此处采用`http`作为握手载体并不会对真实使用时的性能造成大量影响.

由于我们的使用环境比较单一,因此没有必要采取`ZMQ`的分帧逻辑.

> ZMQ Frame

```c++
|flags|size|data|
|  8b  | 64b|   ...  |
```

`ZMQ.flags`包含:

- `more` 标识是否有后续信息
- `large_flag` 标识是否是大数据(>255)
- `command_flag`用于`zmtp`控制

`ZMQ`在封装消息时:

- 小于`255B`的数据使用`unsigned char`标识其大小,且`large_flag =  0`
- 大于`255B`时,使用`uint64`标识其大小,以网络字节序(大端)发送,且置`large_flag = 1`

> NET Frame

```
|size|data|
| 64b|   ...  |
```

由于上层用户使用`Session`传输的数据,大小一般均大于`255B`,因此省去了小数据标识.

特别的,由于要传输错误信息,约定使用`int64`来传输信息(大端 ).当值为负数时,标识此消息为主动关闭信息.在接受到该信息后应立即抛出,并关闭流.

由于`ipc`底层可以感知到对端的变化,因此诸如`ping`,`Pong`等控制信息均可以省略,进而省去了`flags`字段.

---

## Boost 相关

本节主要叙述一些`boost`相关的技术.

#### use_folly_future

头文件`asio_utio.hpp`源自`asio::use_future.hpp`.原文件讲述的是如何经过一系列的封装使得`asio`的结果使用`std::future`进行返回.本头文件在其基础上进行了修改,主要作用是使`asio`支持`folly::future`.其使用方法如下:

```c++
folly::SemiFuture<size_t> size = boost::asio::write(stream, data,util::use_folly_future);
folly::SemiFuture<Unit> unit = boost::asio::write(stream, data,util::use_folly_future([](const size_t& sz){
    //do some thing
    //得到结果后先处理回调再返回新结果
}));
```

对于`use_folly_future`结构体的真实执行结果如下:

1. `asio_handler_invoke`
   1. `promise_creator`创建`promise`
   2. `promise_executor`执行`asio.callback`
      - 调用`promise_invoker`进行真正的执行
2. `async_result`返回异步处理的结果
   - 结果是`folly::future`,由`promise_creator`定义类型并返回.

由于此头文件是由`use_future`改造而来,有部分冗余的部分,在之后可以酌情增减.修改一些不必要结构体.在`perf`中,此部分的创建与析构是一个性能瓶颈,约使用`10%`的性能.当前模板体系下无法合理添加内存池优化.

---

#### IOCExecutor

`asio::io_context`是`asio`中用于用户回调执行的类.如何合理使用`io_context`是一大性能优化关键.`io_context`有`3`种优化思路:

- 单`io_context`多线程`run`.
- 多`io_context`轮转使用.
- 综上,多`io_context`轮转,每个`io_context`多线程调度.

在实际使用中,发现单线程执行`io_context`,并开启单线程优化时,同协程模型.若回调函数无巨大计算量需求,此时该模式快于多线程单`ioc`.

不太建议使用单`ioc`多线程执行模型.做底层通讯库时,最大的计算量在序列化与反序列化,其实无需多线程.其中带来的线程锁反而减缓了运行速度.

多`ioc`,每个`ioc`单线程执行是一个不错的选择.但是需要注意有时候,虽然回调函数可以及时执行,但是`asio`的服务还是全局唯一的(例如一个 `ipc.socket`读写是一种服务,`tcp.socket`是另一种服务).考虑这部分带来的性能影响.

本库中取巧的`io_context`包装为`folly::Executor`,然而实际上将一些后续的事务处理插入`io`处理器不是一个很合理的做法.在极端情况下有较大的性能影响.本库是因为性能足够的情况下取巧的将其安排在内.

需要注意:

- `boost.ucontent`: `300'000'000r/s`
- `folly.fiber`:`4'000'000 r/s`
- ` boost.asio`:`1'000'000 r/s`

与他们的基础库`ucontext`相比,切换速度已经大打折扣,另外,`io_context`的`post`接口仅仅有`500Kr/s`的速度.也就是说,`io_context`即使全速运作,仅能启动`500k`次新任务.且`folly::future`的每一次`defer`或`then`均算一次.此处也是一个性能瓶颈,可以考虑优化.

在`ioc`中等待`folly::future`是一个复杂的过程.但是有一个简单的解决方案.

```c++
template<typename T>
inline void WaitForFutureReady(folly::SemiFuture<T>& future, boost::asio::io_context& ex, yield_context yield) {
    while (!future.isReady()) boost::asio::post(ex, yield);
}
```

曾尝试用`yield`内置的执行器指针进行`post`.但是没有效果.此方法需要上升到最基础的`ioc`中进行`yield`切换.此过程进行的次数优先,且会导致`cpu`高占用率.但是其并不会影响性能.相反 ,工作协程越多时其代价越小.

---

#### Stream

`asio`底层的`stream`支持多线程访问,但是这并不安全.由于`asio`本身不含队列 ,因此在多线程写入时可能会导致数据错位.因此需要避免这种情况的发生.本库在`DEBUG`模式下使用原子锁进行了检查.

对于客户端而言,没有必要一定做多路复用.当`http`做载体时,使用`asio`自带的多路复用机制,即每次通讯使用一条流,也可以达到`8000`左右的`iops`.此速度与`libuv`相似.而用户态的多路复用反而增加了阻塞,锁等其他的复杂度.其性能反而不能更优.

---

#### 制作自己的异步调用组合

```c++
template<typename AsyncReadStream, typename ReadHandler>
inline decltype(auto) 
AsyncRead(AsyncReadStream& stream, std::string& msg, ReadHandler&& handler) {
    static_assert(boost::beast::is_async_read_stream<AsyncReadStream>::value,
                  "AsyncReadStream type requirements not met");
    return boost::asio::async_initiate<ReadHandler, void(std::exception_ptr, std::size_t)>(
        detail::run_read_frame_op{}, handler, &stream, &msg);
}
```

上述是最新的`asio`异步函数写法,该写法将兼容`c++20`的协程.所有的`asio`异步函数均采用此写法.调用后使用仿函数`run_read_frame_op`进行异步初始化操作,并根据`ReadHandler`类型自动返回异步结果.可以参见`boost.beast`.

```c++
struct run_read_frame_op {
    template<typename Handler, typename AsyncReadStream>
    void operator()(Handler&& h, AsyncReadStream* s, std::string* m) const {
        //从Handler获取执行器
        auto executor = boost::asio::get_associated_executor(h);
        //在指定执行器中启动有栈协程.
        boost::asio::spawn(executor, [](boost::asio::yield_context yield) mutable {
            //在协程中执行一些任务
            std::exception_ptr ex_ptr;
            try {
           		//do something
            } catch (std::exception& e) {
                ex_ptr = std::current_exception();
            }
            h(ex_ptr, bytes);  //此处返回的结果需要与初始化时相对应
        });
    }
```

