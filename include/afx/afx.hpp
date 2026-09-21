#pragma once

// AffiniX — umbrella header. Layers depend downward only (DESIGN.md §5).

// L0 sys
#include "afx/sys/affinity.hpp"
#include "afx/sys/annotations.hpp"
#include "afx/sys/clock.hpp"
#include "afx/sys/endian.hpp"
#include "afx/sys/error.hpp"
#include "afx/sys/inline_fn.hpp"
#include "afx/sys/result.hpp"
#include "afx/sys/topology.hpp"
#include "afx/sys/types.hpp"

// L1 backend
#include "afx/backend/backend.hpp"
#include "afx/backend/epoll.hpp"
#include "afx/backend/sim.hpp"
#ifdef AFX_WITH_URING
#include "afx/backend/uring.hpp"
#endif

// L2 core
#include "afx/core/context.hpp"
#include "afx/core/event_manager.hpp"
#include "afx/core/flight_recorder.hpp"
#include "afx/core/handle_table.hpp"
#include "afx/core/io_buffer.hpp"
#include "afx/core/stats.hpp"
#include "afx/core/timer_wheel.hpp"

// L3 itc
#include "afx/itc/barrier.hpp"
#include "afx/itc/channel.hpp"
#include "afx/itc/fan.hpp"
#include "afx/itc/mailbox.hpp"
#include "afx/itc/mpsc_ring.hpp"
#include "afx/itc/sharded.hpp"
#include "afx/itc/spsc_ring.hpp"

// L4 net
#include "afx/net/connection.hpp"
#include "afx/net/protocol.hpp"
#include "afx/net/sock_addr.hpp"
#include "afx/net/socket.hpp"
#include "afx/net/tcp_client.hpp"
#include "afx/net/tcp_server.hpp"

// L5 coroutines (opt-in layer — ADR-0005)
#include "afx/coro/combine.hpp"
#include "afx/coro/conn.hpp"
#include "afx/coro/ops.hpp"
#include "afx/coro/scope.hpp"
#include "afx/coro/task.hpp"

// deterministic simulation (M10 — test/sim layer)
#include "afx/sim/net.hpp"
#include "afx/sim/runtime.hpp"

// runtime
#include "afx/runtime.hpp"
