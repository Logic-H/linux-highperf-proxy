#pragma once

#include <memory>
#include <functional>
#include <chrono>

namespace proxy {
namespace network {

class TcpConnection;
class Buffer; // We will implement Buffer later, but we need declaration

using TcpConnectionPtr = std::shared_ptr<TcpConnection>;
using ConnectionCallback = std::function<void(const TcpConnectionPtr&)>;
using CloseCallback = std::function<void(const TcpConnectionPtr&)>;
using WriteCompleteCallback = std::function<void(const TcpConnectionPtr&)>;

// TODO: Buffer implementation needed for MessageCallback
// For now, let's use a simplified signature or void* if necessary, 
// but standard pattern is (conn, buffer, timestamp)
// Since Buffer is not ready, we will define it when we implement TcpConnection.
// Actually, let's include Buffer.h forward declaration.

using MessageCallback = std::function<void(const TcpConnectionPtr&,
                                           Buffer*,
                                           std::chrono::system_clock::time_point)>;

using HighWaterMarkCallback = std::function<void(const TcpConnectionPtr&, size_t)>;

} // namespace network
} // namespace proxy
