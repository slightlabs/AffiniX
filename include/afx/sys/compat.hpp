#pragma once

// Portability shims for BSD/macOS (M11-07). Linux-only syscall surface is
// defined to its no-op equivalent here so shared code compiles everywhere;
// behaviour differences live behind AFX_HAVE_KQUEUE / __linux__ guards.

#include <sys/socket.h>

// MSG_NOSIGNAL doesn't exist on macOS/BSD — SO_NOSIGPIPE at socket creation
// is the equivalent there (wired in sock::create). Zero makes the flag a
// compile-time no-op.
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// accept4 is Linux/FreeBSD; macOS has only accept() + fcntl.
#if defined(__APPLE__) && !defined(AFX_HAVE_ACCEPT4)
#define AFX_NEEDS_ACCEPT4_SHIM 1
#endif
