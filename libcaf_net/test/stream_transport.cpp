/******************************************************************************
 *                       ____    _    _____                                   *
 *                      / ___|  / \  |  ___|    C++                           *
 *                     | |     / _ \ | |_       Actor                         *
 *                     | |___ / ___ \|  _|      Framework                     *
 *                      \____/_/   \_|_|                                      *
 *                                                                            *
 * Copyright 2011-2019 Dominik Charousset                                     *
 *                                                                            *
 * Distributed under the terms and conditions of the BSD 3-Clause License or  *
 * (at your option) under the terms and conditions of the Boost Software      *
 * License 1.0. See accompanying files LICENSE and LICENSE_ALTERNATIVE.       *
 *                                                                            *
 * If you did not receive a copy of the license files, see                    *
 * http://opensource.org/licenses/BSD-3-Clause and                            *
 * http://www.boost.org/LICENSE_1_0.txt.                                      *
 ******************************************************************************/

#define CAF_SUITE stream_transport

#include "caf/net/stream_transport.hpp"

#include "caf/net/test/host_fixture.hpp"
#include "caf/test/dsl.hpp"

#include "caf/binary_deserializer.hpp"
#include "caf/binary_serializer.hpp"
#include "caf/byte.hpp"
#include "caf/byte_buffer.hpp"
#include "caf/detail/scope_guard.hpp"
#include "caf/make_actor.hpp"
#include "caf/net/actor_proxy_impl.hpp"
#include "caf/net/multiplexer.hpp"
#include "caf/net/socket_guard.hpp"
#include "caf/net/socket_manager.hpp"
#include "caf/net/stream_socket.hpp"
#include "caf/span.hpp"

using namespace caf;
using namespace caf::net;

namespace {
constexpr string_view hello_manager = "hello manager!";

struct fixture : test_coordinator_fixture<>, host_fixture {
  using byte_buffer_ptr = std::shared_ptr<byte_buffer>;

  fixture() : recv_buf(1024), shared_buf{std::make_shared<byte_buffer>()} {
    mpx = std::make_shared<multiplexer>();
    if (auto err = mpx->init())
      CAF_FAIL("mpx->init failed: " << err);
    mpx->set_thread_id();
    CAF_CHECK_EQUAL(mpx->num_socket_managers(), 1u);
    auto sockets = unbox(make_stream_socket_pair());
    send_socket_guard.reset(sockets.first);
    recv_socket_guard.reset(sockets.second);
    if (auto err = nonblocking(recv_socket_guard.socket(), true))
      CAF_FAIL("nonblocking returned an error: " << err);
  }

  bool handle_io_event() override {
    return mpx->poll_once(false);
  }

  settings config;
  multiplexer_ptr mpx;
  byte_buffer recv_buf;
  socket_guard<stream_socket> send_socket_guard;
  socket_guard<stream_socket> recv_socket_guard;
  byte_buffer_ptr shared_buf;
};

class dummy_application {
  using byte_buffer_ptr = std::shared_ptr<byte_buffer>;

public:
  explicit dummy_application(byte_buffer_ptr rec_buf)
    : rec_buf_(std::move(rec_buf)){
      // nop
    };

  ~dummy_application() = default;

  template <class Parent>
  error init(Parent& parent, const settings&) {
    parent.configure_read(receive_policy::exactly(hello_manager.size()));
    return none;
  }

  template <class Parent>
  bool prepare_send(Parent&) {
    // TODO what does this function do?
    return false;
  }

  template <class Parent>
  bool done_sending(Parent&) {
    // TODO what does this function do?
    return false;
  }

  template <class Parent>
  error write_message(Parent& parent,
                      std::unique_ptr<endpoint_manager_queue::message> msg) {
    auto payload_buf = parent.next_payload_buffer();
    binary_serializer sink{parent.system(), payload_buf};
    if (auto err = sink(msg->msg->payload))
      CAF_FAIL("serializing failed: " << err);
    parent.write_packet(payload_buf);
    return none;
  }

  template <class Parent>
  size_t consume(Parent&, span<const byte> data, span<const byte>) {
    rec_buf_->clear();
    rec_buf_->insert(rec_buf_->begin(), data.begin(), data.end());
    CAF_MESSAGE("Received " << rec_buf_->size()
                            << " bytes in dummy_application");
    return rec_buf_->size();
  }

  template <class Parent>
  void resolve(Parent& parent, string_view path, const actor& listener) {
    actor_id aid = 42;
    auto hid = string_view("0011223344556677889900112233445566778899");
    auto nid = unbox(make_node_id(42, hid));
    actor_config cfg;
    endpoint_manager_ptr ptr{&parent.manager()};
    auto p = make_actor<actor_proxy_impl, strong_actor_ptr>(
      aid, nid, &parent.system(), cfg, std::move(ptr));
    anon_send(listener, resolve_atom_v, std::string{path.begin(), path.end()},
              p);
  }

  template <class Parent>
  void timeout(Parent&, const std::string&, uint64_t) {
    // nop
  }

  template <class Parent>
  void new_proxy(Parent&, actor_id) {
    // nop
  }

  template <class Parent>
  void local_actor_down(Parent&, actor_id, const error&) {
    // nop
  }

  static void handle_error(sec code) {
    CAF_FAIL("handle_error called with " << CAF_ARG(code));
  }

  template <class Parent>
  static void abort(Parent&, const error& reason) {
    CAF_FAIL("abort called with " << CAF_ARG(reason));
  }

private:
  byte_buffer_ptr rec_buf_;
};

} // namespace

CAF_TEST_FIXTURE_SCOPE(endpoint_manager_tests, fixture)

CAF_TEST(receive) {
  auto mgr = make_socket_manager<dummy_application, stream_transport>(
    recv_socket_guard.release(), mpx, shared_buf);
  CAF_CHECK_EQUAL(mgr->init(config), none);
  CAF_CHECK_EQUAL(mpx->num_socket_managers(), 2u);
  CAF_CHECK_EQUAL(
    static_cast<size_t>(
      write(send_socket_guard.socket(), as_bytes(make_span(hello_manager)))),
    hello_manager.size());
  CAF_MESSAGE("wrote " << hello_manager.size() << " bytes.");
  run();
  CAF_CHECK_EQUAL(string_view(reinterpret_cast<char*>(shared_buf->data()),
                              shared_buf->size()),
                  hello_manager);
}
/*
CAF_TEST(resolve and proxy communication) {
  using transport_type = stream_transport<dummy_application>;
  auto mgr = make_endpoint_manager(
    mpx, sys,
    transport_type{send_socket_guard.release(), dummy_application{shared_buf}});
  CAF_CHECK_EQUAL(mgr->init(), none);
  run();
  mgr->resolve(unbox(make_uri("test:/id/42")), self);
  run();
  self->receive(
    [&](resolve_atom, const std::string&, const strong_actor_ptr& p) {
      CAF_MESSAGE("got a proxy, send a message to it");
      self->send(actor_cast<actor>(p), "hello proxy!");
    },
    after(std::chrono::seconds(0)) >>
      [&] { CAF_FAIL("manager did not respond with a proxy."); });
  run();
  auto read_res = read(recv_socket_guard.socket(), recv_buf);
  if (read_res < 0)
    CAF_FAIL("read() returned an error: " << last_socket_error_as_string);
  else if (read_res == 0)
    CAF_FAIL("read() returned 0 (socket closed)");
  recv_buf.resize(static_cast<size_t>(read_res));
  CAF_MESSAGE("receive buffer contains " << recv_buf.size() << " bytes");
  message msg;
  binary_deserializer source{sys, recv_buf};
  CAF_CHECK_EQUAL(source(msg), none);
  if (msg.match_elements<std::string>())
    CAF_CHECK_EQUAL(msg.get_as<std::string>(0), "hello proxy!");
  else
    CAF_ERROR("expected a string, got: " << to_string(msg));
}*/

CAF_TEST_FIXTURE_SCOPE_END()
