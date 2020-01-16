# AMRPC

AMRPC(`async magic remote procedure call `).

- 简介易用,纯异步接口.
- `IPC`,`TCP`无缝切换
- 全面支持标准`http`&`websocket`.

---

使用本框架前,需要预先了解的知识:

[folly.future]: https://github.com/facebook/folly/blob/master/folly/docs/Futures.md
[folly.executors]: https://github.com/facebook/folly/blob/master/folly/docs/Executors.md
[why use future]: https://code.facebook.com/posts/1661982097368498/futures-for-c-11-at-facebook/

- 服务器与客户端均可以运行在`tcp`模式与`ipc`模式下.当使用地址`tcp://`启动则运作在`tcp`模式下,使用`ipc://`启动则运行在`ipc`模式下.

- 对于服务器而言,其各项服务使用方法名(`method`)进行区分.
  - 方法名必须以`/`起始,中途可以添加任意多个`/`进行分层.
  - 同一个服务器中的方法名必须是唯一的.
- 对于客户端而言,连接时需要指明服务器地址(`host`)以及对应的方法(`method`);

---

## 通讯模式

`amrpc`提供2种的通讯模型(`rpc`,`publish`).

---

### rpc

rpc通讯类似与基本的`http`请求回应.向远程服务发起一个请求 ,并且期望得到一个回应.但是对于`http`请求有过多的其他细节,很多时候客户不太关心其中的细节,而只是想以一种类似于本地函数的方式去进行调用.对于其他细节,以异常的形式抛出.

```c++
RemoteFunction<string(int)> func("tcp://127.0.0.1:57000","/to_string");
```

`RemoteFunction`内部只存储了远端服务器的描述信息而并不发起真实的连接.对于`rpc`而言,每次访问都是独立的.

上一次的结果并不会影响到此次调用.

```c++
//folly::SemiFuture<folly::Unit> Enabled();
func.Enabled();
```

`Enabled()`可以实时的检查远端是否存在.由于其本质上需要发起一次连接,因此过程并不是立即完成的.

```c++
folly::SemiFuture<string> res = func(int(1));
```

进行调用时以 `future`返回结果.

---

对于服务器而言,在启动后需要进行`rpc`的注册.

```c++
amrpc::Server server("tcp://127.0.0.1:57000");
server.AddRpc<string(int)>("/to_string", [](int data) {
    return std::to_string(data);
});
```

在服务器中注册的回调必须是无阻塞的,其运行时间必须小于`50ms`.

对于简单的处理,可以直接返回结果.该调用将在`amrpc`执行线程中运算.

对于复杂的处理,用户需要返回`SemiFuture`或者`Future`.`amrpc`将会自动处理此`future`.

当回调函数抛出异常时,框架会捕获异常并发送至客户端.

---

### Publish

推送对于客户端而言并非难题.对于单个推送而言,`amrpc`推送建立在点对点连接之上,可以实时的感知对端的变化.

对于这种`p2p`模式之上建立的推送,同时管理多路推送的接受与控制是一个比较专业化的问题.对于不太熟悉异步服务设计的用户而言,使用多线程接受推送数据会有一定的性能损失.而使用队列存储数据会使得原本使用`future-promise`天然分离的推送数据又糅合在一起.

因此,`amrpc`决定使用内置的事件驱动器去驱动回调函数,隔离复杂操作.显然,回调函数不能过长时间的阻塞.

```c++
 amrpc::Puller puller = Pull<string>("tcp://127.0.0.1:57000","/publish",[](folly::Try<string>&& t){
      //do something
 });
```

返回的句柄(台湾翻译:把手)`Puller`是一个类似与`Signal::Connection`的`RALL`机制的管理句柄.当其析构时,会自动断开相关的推送底层流.请注意,这里的回调函数的参数是`folly::Try`而并不是直接的`valueType`.是因为当某一次推送由于意外原因出错时,可以将错误原因返回给用户.

用户可能会觉得由框架自动进行断线重连是一种比较合适的手段.但是合适的重连退避时间往往由上层业务决定.而且有时某些由服务器发起的主动关闭意味着不得重连,例如访问一个不存在的`path`.如果需要由框架智能执行断线重连,需要为上述的行为引入一大批接口.但这与`amrpc`的简洁易用的设计理念所矛盾.

对于服务器而言,推送其实是一个统一的繁杂的枯燥的处理过程.涉及到数据的分发,数据的转化,客户端状态的监控以及如何实时的关闭不正常的客户端.并且以上所有功能必须是异步的.因此`amrpc`帮助用户完成这部分的功能.

使用时,需要先注册一个推送的方法(`method`).

```c++
amrpc::Server server("tcp://127.0.0.1:57000");
server.AddPublish<string>("/nagging",100);
```

只有在服务器注册以后,客户端才可能正常的监听到此推送.第二个数字标明各客户端的队列容量上线(high-watermarks),默认值为`10`,可以视推送频率适当调整.由于各个客户端的接受能力不同,推送需要使用最高水位线监视客户端的状态.当达到最高水位线时,即队列满时,服务器认为该客户端接受能力受限,关闭与此客户端的连接.

```c++
string data("hello world");
server.Publish("/nagging",move(data));
server.Publish("/nagging","hello world");
```

使用推送接口推出数据.推送接口是多线程安全的.并且推送数据严格按照调用顺序进行数据推送.

```c++
folly::dynamic json;
// fill json 
server.Publish("/nagging", json);
```

从理论上来说,Publish接口接受任何类型的数据并且尝试转换成注册时使用的数据,因此上述调用是合法的.但是应避免这么做,并且严格按照注册(`AddPublish`)时使用的数据类型进行推送.保留这项允许任意类型推送的功能仅适用于对`amrpc`内部运作有所了解的高级用户.

由于推送的底层实现是流,因此与一般的推送不同,服务器可以实时感知到在线的客户端的个数.某些时候用户可能需要根据在线客户端的个数调整运行策略.

```c++
size_t size = server.GetPullerSize("/nagging");
```

注意当无此推送时,此函数会抛出异常.

当用户需要关闭推送时,可以调用`Del`接口,服务器会删除此接口,不在允许新客户端进入,同时关闭与现有客户端的连接.

```c++
server. Del("/nagging");
```

服务器内部含有反射接口:`/debug/reflection`,允许对现有的接口进行反射,如:

```json
{
    "publish": {
        "/nagging":"std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > [/nagging]()"
    },
  "rpc": {
    "/test": "std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > [/test] (TestMsg)"
  }
}
```

---

### 内部数据类型

对于二进制信息,我们约定使用`amrpc::Bytes`进行存储.`Bytes`继承自`std::string`并提供转换函数.

对于文本信息,我们约定使用`std::string`进行存储.

对于普通的结构体,需要使用`AMRPC_DEFINE`宏进行注册.

对于枚举类型,使用`MSGPACK_ADD_ENUM`宏进行注册.(此注册必须在全局命名空间下进行).

```c++
struct Data{
    int i;
    string bin;
    AMRPC_DEFINE(i,bin);
};

struct Pack{
    int i;
    Data data;
    AMRPC_DEFINE(i,data);
};
```

当 发生结构体嵌套,且需要提取嵌套结构,并且丢弃最外层信息 时,需要如下操作.需要注意如下使用细节:

```c++
Data cdata,mdata;
{
	Pack pack = GetPackFromServer();
    cdata = AMRPC_COPY(pack,data);
	mdata = AMRPC_MOVE(pack,data);
}
//use cdata here
```

这是因为最外层结构内含了真实的数据的智能指针.若直接抛弃,形如`string_view`一类的数据 将无法使用.但是`int`,`string`等拷贝赋值的成员不会受到影响.

---

## 与Restful的对应关系

当服务器运行在`tcp`模式下时,非`amrpc`客户端可以用`restful`形式进行访问.因此当服务器编写完毕时,可以在`tcp`模式下运行,然后使用成熟的`Rest Client`进行调试.

---

### 数据类型

本节讲述各种数据类型.

|     cpp      |       web       |
| :----------: | :-------------: |
|  任意struct  | msgpack 或 json |
|    string    |     string      |
| amrpc::Bytes |  base64 string  |
|   enum枚举   |       int       |

在`cpp`中声明的任意`struct`将会使用`msgpack`压缩.当成员是`amrpc::Bytes`或`vector<std::Byte>`时,该成员在序列化时会执行`base64`编码.而普通`string`会原样发送,因此请注意不要填写二进制数据. 

当来自`web`的客户端需求具体数据类型时,具体的转化关系如下:

- `msgpack`<=>`json`
  - msgpack与json可以互相转化
- `json`=`text`
  - `json`与`text`的内容相同,但是返回结果中的标记是不同的.
- `msgpack`=>`bin`
  - 当服务器中注册的是`msgpack`时,`bin`为二进制的`msgpack`序列化数据
  - `bin`不允许转化为`msgpack`
- `json`=>`bin`
  - 当服务器中注册的是`text`时,`bin`为`json`文本数据
  - `bin`不允许转化为`text`

---

### rpc

对于`rpc`而言,其映射为`http`请求回应.

> server.cpp

```c++
server.AddRpc<string(int)>("/to_string", [](int data) {
    return std::to_string(data);
});
```

> client.html

```http
###
GET http://127.0.0.1:57000/to_string
Content-Type: text/plain
Accept: application/json
Connection: close

[123]
```

- `amrpc`服务器不区分`verb`类型(`GET`,`POST`等).
- `Content-Type`表示请求的数据类型
  - `application/x-msgpack`
  - `application/json`
  - `text/plain`   
  - `application/octet-stream` 
- `Accept`表示想要接受的数据类型,并且作为`response`的`Content-Type`返回,支持类型同上.
- `Connection`将于`keep-alive`共同使用,服务器支持`tcp`复用.
- 当`body`是`json`数据时,需要使用`json`数组包裹所有传入参数,即使参数仅有一个.
- 当服务器出错时,返回`500`错误.注意: 使用`vscode.REST client`测试时,遇到`500`错误会自动重试`3`次.

---

### publish

推送使用`websocket`进行接入.

> client.js

```javascript
 var ws = new WebSocket("ws://127.0.0.1:57000/nagging", "ecv_amrpc_json");
```

支持`JavaScript`原生`websocket`.

- `websocket`子协议: 子协议支持4种类型,用于指定想要接受的推送数据的数据类型.功能同`rpc.Accept`.
  - `ecv_amrpc_msgpack`
  - `ecv_amrpc_json`
  - `ecv_amrpc_text`
  - `ecv_amrpc_bin` 
    - 任何非上述`3`种类型的数据均会指定为此类型.
    - 返回值的内容由服务器推送时产生的原始数据决定.

---

## 性能分析参考

当系统允许对线程设置名称时,`amrpc`会做如下设置:

- `amrpc`事务处理线程`amrpc_evb`.
  - 用户的部分回调函数会在此线程排队执行,注意不要长时间阻塞(`DEBUG`模式下有时间检测).
  - 此线程的使用量随同时处理的`io`个数,数据结构的复杂程度增加.
  - 但是在绝大多数情况下此线程不应该有较大的使用量.

- `ecv.net`网络库处理线程`net_evb`.
  - 用户无法深入使用此库.
  - 此线程在部分时刻使用自旋锁进行等待,因此在某些时刻即使任务量极小,其也可能会满载.
  - 若`net`线程长期满载,考虑通讯方式是否合理,是否需要切换通讯方式.
  - 注意此线程为全局线程,多个客户端或服务器共享此线程,添加多个服务器并不会使性能提升.

