// bench/echo_coro — M9-08: the bench/echo workload served by a coroutine
// session (make_coro_server + ConnRef recv/send awaiters) instead of the
// callback handler. The load side, protocol, and CLI are literally the same
// file — echo.cpp compiled with AFX_ECHO_CORO_SERVER — so
//
//     bench_echo      --server epoll --conns 8 --mode closed
//     bench_echo_coro --server epoll --conns 8 --mode closed
//
// measure the ADR-0005 layering cost directly: identical offered load, only
// the session shape differs.

#define AFX_ECHO_CORO_SERVER 1
#include "echo.cpp"  // NOLINT: benchmark variant shares the whole harness
