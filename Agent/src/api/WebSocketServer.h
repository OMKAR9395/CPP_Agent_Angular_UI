#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/websocket.hpp>
#include <nlohmann/json.hpp>

#include "core/EventBus.h"

namespace agent {

class WebSocketServer {
 public:
  WebSocketServer(EventBus& event_bus, std::string host, std::uint16_t port);
  ~WebSocketServer();

  bool Start(std::string* error_message = nullptr);
  void Stop();

 private:
  struct ClientConnection {
    explicit ClientConnection(boost::asio::ip::tcp::socket&& socket);

    std::mutex io_mutex;
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws;
    bool subscribed_events = true;
    bool subscribed_serial_data = true;
    std::string session_filter;
    std::atomic<bool> alive{true};
  };

  void AcceptLoop();
  void HandleSocket(boost::asio::ip::tcp::socket socket);
  void HandleClientLoop(const std::shared_ptr<ClientConnection>& client);
  void RemoveClient(const std::shared_ptr<ClientConnection>& client);
  void BroadcastEvent(const nlohmann::json& event);

  EventBus& event_bus_;
  std::string host_;
  std::uint16_t port_ = 0;

  boost::asio::io_context io_context_;
  std::unique_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
  std::atomic<bool> running_{false};
  std::thread accept_thread_;
  std::mutex client_threads_mutex_;
  std::vector<std::thread> client_threads_;

  mutable std::mutex clients_mutex_;
  std::vector<std::weak_ptr<ClientConnection>> clients_;
  int event_subscription_id_ = 0;
};

}  // namespace agent
