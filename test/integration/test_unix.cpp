// test/integration/test_unix.cpp — M11-05/06: Unix-domain stream sockets
// (echo over a filesystem path through the same TcpServer/TcpClient API)
// and SCM_RIGHTS descriptor passing (foundation for hot restart: an fd
// handed across a unix conn and usable by the receiver).

#include <doctest/doctest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "../proto.hpp"
#include "../real_env.hpp"
#include "afx/afx.hpp"

using namespace afx;
using afx::test::echo_frame;
using afx::test::EchoHeader;
using afx::test::EchoMsg;
using afx::test::EchoProto;

namespace {

std::string tmp_path(const char* tag) {
    return std::string("/tmp/afx-test-") + tag + "-" +
           std::to_string(::getpid());
}

struct UnixCleanup {
    std::string path;
    ~UnixCleanup() {
        if (!path.empty()) ::unlink(path.c_str());
    }
};

}  // namespace

TEST_CASE("unix: SockAddr::unix_domain makes an AF_UNIX address") {
    auto a = SockAddr::unix_domain("/tmp/afx-unit.sock");
    REQUIRE(a.has_value());
    CHECK(a->family() == AF_UNIX);
    CHECK(a->unix_path() == "/tmp/afx-unit.sock");
    CHECK(a->to_string() == "unix:/tmp/afx-unit.sock");

    // Too long for sun_path.
    std::string long_path(200, 'x');
    CHECK_FALSE(SockAddr::unix_domain(long_path).has_value());
}

AFX_BACKEND_TEST_CASE("unix: echo over a filesystem path", EM) {
    std::string path = tmp_path("echo");
    auto emp = test::make_real_em<EM>(
        EventManagerConfig{.name = "unix-echo", .wait = WaitStrategy::Spin});
    REQUIRE(emp);
    EM& em = *emp;

    ServerConfig sc;
    sc.bind = *SockAddr::unix_domain(path);

    Handlers<EchoProto> sh;
    sh.on_messages = [&](ConnId id, std::span<const EchoMsg> ms) {
        auto* c = Connection<EchoProto, EM>::resolve(em, id);
        REQUIRE(c);
        for (auto& m : ms) {
            std::array<std::byte, sizeof(EchoHeader)> hdr;
            std::memcpy(hdr.data(), &m.header, sizeof(hdr));
            std::array<ByteSpan, 2> parts{ByteSpan(hdr.data(), hdr.size()),
                                          m.body};
            (void)c->send_scatter(parts);
        }
    };

    auto srv = em.template make_server<EchoProto>(sc, std::move(sh));
    REQUIRE(srv.has_value());
    REQUIRE((*srv)->open().has_value());
    CHECK((*srv)->bound_addr().family() == AF_UNIX);

    ClientConfig cc;
    cc.unix_target = *SockAddr::unix_domain(path);

    std::atomic<int> got{0};
    Handlers<EchoProto> ch;
    ch.on_open = [&](ConnId id, Peer) {
        auto* c = Connection<EchoProto, EM>::resolve(em, id);
        REQUIRE(c);
        auto f = echo_frame("hello-unix");
        (void)c->send(ByteSpan(f.data(), f.size()));
    };
    ch.on_messages = [&](ConnId, std::span<const EchoMsg> ms) {
        got += int(ms.size());
        em.stop();  // client side decides — the echo must land first
    };

    auto cli = em.template make_client<EchoProto>(cc, std::move(ch));
    REQUIRE(cli.has_value());
    REQUIRE((*cli)->start().has_value());
    em.run();

    CHECK(got.load() == 1);
    // Teardown unlinks the socket file (stop_accepting in ~TcpServer via
    // EM destruction of owned objects).
    emp.reset();
    struct stat st {};
    CHECK(::stat(path.c_str(), &st) < 0);
}

AFX_BACKEND_TEST_CASE("unix: SCM_RIGHTS passes an open fd across the conn",
                      EM) {
    std::string path = tmp_path("rights");
    std::string fpath = tmp_path("payload");
    UnixCleanup guard{fpath};

    {
        int w = ::open(fpath.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
        REQUIRE(w >= 0);
        const char content[] = "fd-passed-contents";
        REQUIRE(::write(w, content, sizeof(content) - 1) ==
                ssize_t(sizeof(content) - 1));
        ::close(w);
    }
    int file_fd = ::open(fpath.c_str(), O_RDONLY);
    REQUIRE(file_fd >= 0);

    auto emp = test::make_real_em<EM>(
        EventManagerConfig{.name = "unix-fd", .wait = WaitStrategy::Spin});
    REQUIRE(emp);
    EM& em = *emp;

    ServerConfig sc;
    sc.bind = *SockAddr::unix_domain(path);
    sc.fd_passing = true;

    std::atomic<int> got_fd{-1};
    Handlers<EchoProto> sh;
    sh.on_fds = [&](ConnId, std::span<const int> fds) {
        if (!fds.empty()) got_fd = fds[0];
        em.stop();
    };
    // The rights ride one payload byte; the framer still sees it as a
    // message — drain it so the byte doesn't stall the stream.
    sh.on_messages = [](ConnId, std::span<const EchoMsg>) {};

    auto srv = em.template make_server<EchoProto>(sc, std::move(sh));
    REQUIRE(srv.has_value());
    REQUIRE((*srv)->open().has_value());

    ClientConfig cc;
    cc.unix_target = *SockAddr::unix_domain(path);

    Handlers<EchoProto> ch;
    ch.on_open = [&](ConnId id, Peer) {
        auto* c = Connection<EchoProto, EM>::resolve(em, id);
        REQUIRE(c);
        const char mark[] = "x";
        auto r =
            c->send_fds(ByteSpan(reinterpret_cast<const std::byte*>(mark), 1),
                        std::span<const int>(&file_fd, 1));
        CHECK(r.has_value());
    };

    auto cli = em.template make_client<EchoProto>(cc, std::move(ch));
    REQUIRE(cli.has_value());
    REQUIRE((*cli)->start().has_value());
    em.run();

    REQUIRE(got_fd.load() >= 0);
    char buf[64]{};
    ssize_t nr = ::read(got_fd.load(), buf, sizeof(buf));
    REQUIRE(nr > 0);
    CHECK(std::string_view(buf, std::size_t(nr)) == "fd-passed-contents");
    ::close(got_fd.load());
    ::close(file_fd);
}
