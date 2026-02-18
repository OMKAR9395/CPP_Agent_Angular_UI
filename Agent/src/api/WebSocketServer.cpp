#include "api/WebSocketServer.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include <boost/asio/error.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

namespace agent {
namespace {

bool IsWouldBlock(const boost::system::error_code& ec) {
  return ec == boost::asio::error::would_block || ec == boost::asio::error::try_again;
}

}  // namespace

WebSocketServer::ClientConnection::ClientConnection(boost::asio::ip::tcp::socket&& socket)
    : ws(std::move(socket)) {}

WebSocketServer::WebSocketServer(EventBus& event_bus, std::string host, std::uint16_t port)
    : event_bus_(event_bus), host_(std::move(host)), port_(port), io_context_(1) {}

WebSocketServer::~WebSocketServer() {
  Stop();
}

bool WebSocketServer::Start(std::string* error_message) {
  if (running_.exchange(true)) {
    return true;
  }

  try {
    const auto address = boost::asio::ip::make_address(host_);
    const boost::asio::ip::tcp::endpoint endpoint(address, port_);

    acceptor_ = std::make_unique<boost::asio::ip::tcp::acceptor>(io_context_);
    acceptor_->open(endpoint.protocol());
    acceptor_->set_option(boost::asio::socket_base::reuse_address(true));
    acceptor_->bind(endpoint);
    acceptor_->listen(boost::asio::socket_base::max_listen_connections);

    event_subscription_id_ = event_bus_.Subscribe(
        [this](const nlohmann::json& event) { BroadcastEvent(event); });
    accept_thread_ = std::thread(&WebSocketServer::AcceptLoop, this);
    return true;
  } catch (const std::exception& ex) {
    running_.store(false);
    if (error_message != nullptr) {
      *error_message = ex.what();
    }
    return false;
  }
}

void WebSocketServer::Stop() {
  if (!running_.exchange(false)) {
    return;
  }

  if (acceptor_ != nullptr) {
    boost::system::error_code ec;
    acceptor_->close(ec);
  }
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }

  std::vector<std::thread> client_threads;
  {
    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    client_threads.swap(client_threads_);
  }
  for (auto& thread : client_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  if (event_subscription_id_ != 0) {
    event_bus_.Unsubscribe(event_subscription_id_);
    event_subscription_id_ = 0;
  }

  std::vector<std::shared_ptr<ClientConnection>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    for (const auto& weak : clients_) {
      if (auto client = weak.lock()) {
        clients.push_back(client);
      }
    }
    clients_.clear();
  }

  for (const auto& client : clients) {
    client->alive.store(false);
    std::lock_guard<std::mutex> lock(client->io_mutex);
    boost::system::error_code ec;
    client->ws.close(boost::beast::websocket::close_code::normal, ec);
  }
}

void WebSocketServer::AcceptLoop() {
  while (running_.load()) {
    boost::asio::ip::tcp::socket socket(io_context_);
    boost::system::error_code ec;
    acceptor_->accept(socket, ec);
    if (ec) {
      if (!running_.load()) {
        break;
      }
      continue;
    }

    std::lock_guard<std::mutex> lock(client_threads_mutex_);
    client_threads_.emplace_back(
        [this, sock = std::move(socket)]() mutable { HandleSocket(std::move(sock)); });
  }
}

void WebSocketServer::HandleSocket(boost::asio::ip::tcp::socket socket) {
  namespace beast = boost::beast;
  namespace http = boost::beast::http;
  namespace websocket = boost::beast::websocket;

  boost::system::error_code ec;
  beast::flat_buffer buffer;
  http::request<http::string_body> req;
  http::read(socket, buffer, req, ec);
  if (ec) {
    return;
  }

  if (!websocket::is_upgrade(req) || req.target() != "/ws") {
    http::response<http::string_body> res{http::status::not_found, req.version()};
    res.set(http::field::content_type, "application/json");
    res.body() = R"({"ok":false,"error":"NOT_FOUND"})";
    res.prepare_payload();
    http::write(socket, res, ec);
    socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
    return;
  }

  auto client = std::make_shared<ClientConnection>(std::move(socket));
  {
    std::lock_guard<std::mutex> lock(client->io_mutex);
    client->ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::server));
    client->ws.accept(req, ec);
    if (ec) {
      return;
    }
    client->ws.binary(false);
    client->ws.next_layer().non_blocking(true, ec);
  }

  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    clients_.push_back(client);
  }

  HandleClientLoop(client);
  RemoveClient(client);
}

void WebSocketServer::HandleClientLoop(const std::shared_ptr<ClientConnection>& client) {
  using namespace std::chrono_literals;
  namespace beast = boost::beast;
  namespace websocket = boost::beast::websocket;

  while (running_.load() && client->alive.load()) {
    beast::flat_buffer buffer;
    boost::system::error_code ec;

    {
      std::lock_guard<std::mutex> lock(client->io_mutex);
      client->ws.read(buffer, ec);
    }

    if (IsWouldBlock(ec)) {
      std::this_thread::sleep_for(50ms);
      continue;
    }
    if (ec == websocket::error::closed) {
      break;
    }
    if (ec) {
      break;
    }

    nlohmann::json msg;
    try {
      msg = nlohmann::json::parse(beast::buffers_to_string(buffer.data()));
    } catch (...) {
      continue;
    }

    if (msg.contains("subscribe") && msg["subscribe"].is_array()) {
      bool want_events = false;
      bool want_serial = false;
      for (const auto& item : msg["subscribe"]) {
        if (!item.is_string()) {
          continue;
        }
        const auto value = item.get<std::string>();
        if (value == "events") {
          want_events = true;
        } else if (value == "serial_data") {
          want_serial = true;
        }
      }
      std::lock_guard<std::mutex> lock(client->io_mutex);
      client->subscribed_events = want_events;
      client->subscribed_serial_data = want_serial;
    }

    if (msg.contains("session_id") && msg["session_id"].is_string()) {
      std::lock_guard<std::mutex> lock(client->io_mutex);
      client->session_filter = msg["session_id"].get<std::string>();
    }
  }

  client->alive.store(false);
  std::lock_guard<std::mutex> lock(client->io_mutex);
  boost::system::error_code ec;
  client->ws.close(websocket::close_code::normal, ec);
}

void WebSocketServer::RemoveClient(const std::shared_ptr<ClientConnection>& client) {
  std::lock_guard<std::mutex> lock(clients_mutex_);
  clients_.erase(std::remove_if(clients_.begin(), clients_.end(),
                                [&](const std::weak_ptr<ClientConnection>& weak) {
                                  auto shared = weak.lock();
                                  return !shared || shared == client;
                                }),
                 clients_.end());
}

void WebSocketServer::BroadcastEvent(const nlohmann::json& event) {
  const std::string type = event.value("type", "");
  const std::string event_session = event.value("session_id", "");

  std::vector<std::shared_ptr<ClientConnection>> clients;
  {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    std::vector<std::weak_ptr<ClientConnection>> compacted;
    compacted.reserve(clients_.size());

    for (const auto& weak : clients_) {
      if (auto client = weak.lock()) {
        clients.push_back(client);
        compacted.push_back(client);
      }
    }
    clients_ = std::move(compacted);
  }

  const std::string payload = event.dump();
  for (const auto& client : clients) {
    std::lock_guard<std::mutex> lock(client->io_mutex);
    if (!client->alive.load()) {
      continue;
    }

    if (type == "serial_data" && !client->subscribed_serial_data) {
      continue;
    }
    if (type != "serial_data" && !client->subscribed_events) {
      continue;
    }
    if (!client->session_filter.empty() && !event_session.empty() &&
        client->session_filter != event_session) {
      continue;
    }

    boost::system::error_code ec;
    client->ws.write(boost::asio::buffer(payload), ec);
    if (ec) {
      client->alive.store(false);
    }
  }
}

}  // namespace agent
