# My-Libevent-Notes

Libevent 主要统一三类事件的异步操作：I/O 事件、Signal 事件、Timer 事件。

I/O 事件和 Signal 事件都由 I/O 复用机制管理。

其中，Signal 事件被转换为 I/O 事件，即在 Signal 事件被触发时，handler 向 socketpair 中的一端写数据，另一端则可以检测到可读事件。

Timer 事件由数据结构 Timer Heap 管理，其中最小超时时间决定了 I/O 复用的超时时间，即在此时间内即使没有 I/O 事件发生，也返回 I/O 复用函数，如 poll、epoll等。

关于版本 1.4.15 参考文档 libevent源码深度剖析.pdf

关于版本 2.x 参考文档 libevent-book