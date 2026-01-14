cmake_minimum_required(VERSION 3.10)
project(HighPerfProxy VERSION 0.1.0 LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Compiler Options
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O3)
endif()

# Include Directories
include_directories(include)

# Threads Support
find_package(Threads REQUIRED)

option(PROXY_WITH_ZLIB "Enable gzip/deflate support (zlib)" ON)
option(PROXY_WITH_OPENSSL "Enable TLS termination support (OpenSSL)" ON)
option(PROXY_WITH_URING "Enable io_uring poller support (liburing)" ON)

if(PROXY_WITH_URING)
    find_path(LIBURING_INCLUDE_DIR liburing.h)
    find_library(LIBURING_LIBRARY uring)
    if(NOT LIBURING_INCLUDE_DIR OR NOT LIBURING_LIBRARY)
        message(FATAL_ERROR "liburing not found. Install liburing development package (e.g. liburing-dev) or configure with -DPROXY_WITH_URING=OFF")
    endif()
endif()

if(PROXY_WITH_ZLIB)
    find_package(ZLIB QUIET)
    if(NOT ZLIB_FOUND)
        message(FATAL_ERROR "ZLIB not found. Install zlib development package (e.g. zlib1g-dev) or configure with -DPROXY_WITH_ZLIB=OFF")
    endif()
endif()

if(PROXY_WITH_OPENSSL)
    find_package(OpenSSL QUIET)
    if(NOT OpenSSL_FOUND)
        message(FATAL_ERROR "OpenSSL not found. Install OpenSSL development package (e.g. libssl-dev) or configure with -DPROXY_WITH_OPENSSL=OFF")
    endif()
endif()

# Source Files (To be expanded as we implement modules)
set(SOURCES
    src/common/Logger.cpp
    src/common/Config.cpp
    src/common/PluginManager.cpp
    src/network/Channel.cpp
    src/network/Poller.cpp
    src/network/EpollPoller.cpp
    src/network/PollPoller.cpp
    src/network/SelectPoller.cpp
    # src/network/UringPoller.cpp  (conditionally added below)
    src/network/EventLoop.cpp
    src/network/EventLoopThread.cpp
    src/network/EventLoopThreadPool.cpp
    src/network/InetAddress.cpp
    src/network/Connector.cpp
    src/network/TcpClient.cpp
    src/network/Socket.cpp
    src/network/Acceptor.cpp
    src/network/Buffer.cpp
    src/network/TcpConnection.cpp
    src/network/TcpServer.cpp
    src/network/TlsContext.cpp
    src/network/UdpProxyServer.cpp
    src/common/SlabAllocator.cpp
    src/common/BuddyAllocator.cpp
    src/common/MemoryPool.cpp
	    src/protocol/HttpContext.cpp
		    src/protocol/HttpServer.cpp
		    src/protocol/Cookie.cpp
		    src/protocol/HttpResponseContext.cpp
		    src/protocol/HttpBatcher.cpp
		    src/protocol/GrpcFramer.cpp
		    src/protocol/Hpack.cpp
		    src/protocol/Http2Connection.cpp
		    src/protocol/ProtobufLite.cpp
		    src/protocol/Compression.cpp
		    src/protocol/RewriteRules.cpp
		    src/protocol/TrafficMirror.cpp
		    src/protocol/Cache.cpp
		    src/balancer/ConsistentHashBalancer.cpp
	    src/balancer/RoundRobinBalancer.cpp
	    src/balancer/LeastConnectionsBalancer.cpp
	    src/balancer/LeastQueueBalancer.cpp
	    src/balancer/ResponseTimeWeightedBalancer.cpp
	    src/balancer/GpuAwareBalancer.cpp
		    src/balancer/HttpHealthChecker.cpp
		    src/balancer/AiServiceChecker.cpp
		    src/balancer/WarmupChecker.cpp
		    src/balancer/TcpHealthChecker.cpp
		    src/balancer/ScriptHealthChecker.cpp
	    src/balancer/ServiceDiscovery.cpp
	    src/balancer/BackendConnectionPool.cpp
    src/balancer/BackendManager.cpp
    src/balancer/BackendSession.cpp
    src/monitor/Stats.cpp
    src/monitor/CongestionControl.cpp
    src/monitor/AlertManager.cpp
    src/monitor/HistoryStore.cpp
    src/monitor/TokenBucket.cpp
    src/monitor/AccessControl.cpp
    src/monitor/AuditLogger.cpp
    src/monitor/PerKeyRateLimiter.cpp
    src/monitor/PerKeyConnectionLimiter.cpp
    src/ProxyServer.cpp
)

if(PROXY_WITH_URING)
    list(APPEND SOURCES src/network/UringPoller.cpp)
endif()

# Create a library for the core logic
add_library(proxy_lib STATIC ${SOURCES})
target_link_libraries(proxy_lib PRIVATE Threads::Threads dl)

if(PROXY_WITH_URING)
    target_include_directories(proxy_lib PRIVATE ${LIBURING_INCLUDE_DIR})
    target_link_libraries(proxy_lib PRIVATE ${LIBURING_LIBRARY})
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_URING=1)
else()
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_URING=0)
endif()

if(PROXY_WITH_ZLIB)
    target_link_libraries(proxy_lib PRIVATE ZLIB::ZLIB)
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_ZLIB=1)
else()
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_ZLIB=0)
endif()

if(PROXY_WITH_OPENSSL)
    target_link_libraries(proxy_lib PRIVATE OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_OPENSSL=1)
else()
    target_compile_definitions(proxy_lib PRIVATE PROXY_WITH_OPENSSL=0)
endif()

# Executable
add_executable(proxy_server src/main.cpp)
target_link_libraries(proxy_server PRIVATE proxy_lib)

# Example plugin (shared library)
add_library(proxy_example_plugin SHARED plugins/example_plugin.cpp)
set_target_properties(proxy_example_plugin PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    OUTPUT_NAME proxy_example_plugin
)

# Tests
add_executable(test_eventloop tests/unit/test_eventloop.cpp)
target_link_libraries(test_eventloop PRIVATE proxy_lib)

add_executable(test_tcpserver tests/integration/test_tcpserver.cpp)
target_link_libraries(test_tcpserver PRIVATE proxy_lib)

add_executable(test_allocator tests/unit/test_allocator.cpp)
target_link_libraries(test_allocator PRIVATE proxy_lib)

add_executable(test_buddy_allocator tests/unit/test_buddy_allocator.cpp)
target_link_libraries(test_buddy_allocator PRIVATE proxy_lib)

add_executable(test_http tests/unit/test_http.cpp)
target_link_libraries(test_http PRIVATE proxy_lib)

add_executable(test_balancer tests/unit/test_balancer.cpp)
target_link_libraries(test_balancer PRIVATE proxy_lib)

add_executable(test_balancer_health tests/unit/test_balancer_health.cpp)
target_link_libraries(test_balancer_health PRIVATE proxy_lib)

add_executable(test_backend_manager tests/unit/test_backend_manager.cpp)
target_link_libraries(test_backend_manager PRIVATE proxy_lib)

add_executable(test_httpserver tests/integration/test_httpserver.cpp)
target_link_libraries(test_httpserver PRIVATE proxy_lib)

add_executable(test_multithread tests/integration/test_multithread.cpp)
target_link_libraries(test_multithread PRIVATE proxy_lib)

add_executable(test_tcpclient tests/integration/test_tcpclient.cpp)
target_link_libraries(test_tcpclient PRIVATE proxy_lib)

add_executable(test_token_bucket tests/unit/test_token_bucket.cpp)
target_link_libraries(test_token_bucket PRIVATE proxy_lib)

add_executable(test_congestion_control tests/unit/test_congestion_control.cpp)
target_link_libraries(test_congestion_control PRIVATE proxy_lib)

add_executable(test_plugin_manager tests/unit/test_plugin_manager.cpp)
target_link_libraries(test_plugin_manager PRIVATE proxy_lib)
add_dependencies(test_plugin_manager proxy_example_plugin)

add_executable(test_select_poller tests/unit/test_select_poller.cpp)
target_link_libraries(test_select_poller PRIVATE proxy_lib)

add_executable(test_conn_limit tests/integration/test_conn_limit.cpp)
target_link_libraries(test_conn_limit PRIVATE proxy_lib)

add_executable(test_idle_cleanup tests/integration/test_idle_cleanup.cpp)
target_link_libraries(test_idle_cleanup PRIVATE proxy_lib)

add_executable(test_connector_retry tests/integration/test_connector_retry.cpp)
target_link_libraries(test_connector_retry PRIVATE proxy_lib)

add_executable(test_memory_stats tests/unit/test_memory_stats.cpp)
target_link_libraries(test_memory_stats PRIVATE proxy_lib)

add_executable(test_hugepage_slab tests/unit/test_hugepage_slab.cpp)
target_link_libraries(test_hugepage_slab PRIVATE proxy_lib)

add_executable(test_script_health_checker tests/unit/test_script_health_checker.cpp)
target_link_libraries(test_script_health_checker PRIVATE proxy_lib)

add_executable(test_udp_proxy tests/integration/test_udp_proxy.cpp)
target_link_libraries(test_udp_proxy PRIVATE proxy_lib)

add_executable(test_leastconn tests/unit/test_leastconn.cpp)
target_link_libraries(test_leastconn PRIVATE proxy_lib)

add_executable(test_rtw tests/unit/test_rtw.cpp)
target_link_libraries(test_rtw PRIVATE proxy_lib)

add_executable(test_cookie tests/unit/test_cookie.cpp)
target_link_libraries(test_cookie PRIVATE proxy_lib)

add_executable(test_access_control tests/unit/test_access_control.cpp)
target_link_libraries(test_access_control PRIVATE proxy_lib)

add_executable(test_audit_logger tests/unit/test_audit_logger.cpp)
target_link_libraries(test_audit_logger PRIVATE proxy_lib)

add_executable(test_per_ip_rate_limit tests/unit/test_per_ip_rate_limit.cpp)
target_link_libraries(test_per_ip_rate_limit PRIVATE proxy_lib)

add_executable(test_stats_json tests/unit/test_stats_json.cpp)
target_link_libraries(test_stats_json PRIVATE proxy_lib)

	add_executable(test_http_health_checker tests/unit/test_http_health_checker.cpp)
	target_link_libraries(test_http_health_checker PRIVATE proxy_lib)

	add_executable(test_ai_service_check tests/unit/test_ai_service_check.cpp)
	target_link_libraries(test_ai_service_check PRIVATE proxy_lib)

	add_executable(test_model_affinity tests/unit/test_model_affinity.cpp)
	target_link_libraries(test_model_affinity PRIVATE proxy_lib)

	add_executable(test_warmup tests/unit/test_warmup.cpp)
	target_link_libraries(test_warmup PRIVATE proxy_lib)

add_executable(test_passive_failover tests/unit/test_passive_failover.cpp)
target_link_libraries(test_passive_failover PRIVATE proxy_lib)

add_executable(test_conn_limit_user_service tests/integration/test_conn_limit_user_service.cpp)
target_link_libraries(test_conn_limit_user_service PRIVATE proxy_lib)

add_executable(test_per_path_rate_limit tests/integration/test_per_path_rate_limit.cpp)
target_link_libraries(test_per_path_rate_limit PRIVATE proxy_lib)

add_executable(test_stats_backends tests/unit/test_stats_backends.cpp)
target_link_libraries(test_stats_backends PRIVATE proxy_lib)

add_executable(test_http_keepalive_chunked tests/integration/test_http_keepalive_chunked.cpp)
target_link_libraries(test_http_keepalive_chunked PRIVATE proxy_lib)

add_executable(test_leastqueue tests/unit/test_leastqueue.cpp)
target_link_libraries(test_leastqueue PRIVATE proxy_lib)

add_executable(test_gpu_balancer tests/unit/test_gpu_balancer.cpp)
target_link_libraries(test_gpu_balancer PRIVATE proxy_lib)

add_executable(test_dynamic_register tests/integration/test_dynamic_register.cpp)
target_link_libraries(test_dynamic_register PRIVATE proxy_lib)

	add_executable(test_websocket_upgrade tests/integration/test_websocket_upgrade.cpp)
	target_link_libraries(test_websocket_upgrade PRIVATE proxy_lib)

	add_executable(test_alert_webhook tests/integration/test_alert_webhook.cpp)
	target_link_libraries(test_alert_webhook PRIVATE proxy_lib)

	add_executable(test_alert_channels tests/integration/test_alert_channels.cpp)
	target_link_libraries(test_alert_channels PRIVATE proxy_lib)

    add_executable(test_alert_anomaly tests/integration/test_alert_anomaly.cpp)
    target_link_libraries(test_alert_anomaly PRIVATE proxy_lib)

add_executable(test_backend_conn_pool tests/integration/test_backend_conn_pool.cpp)
target_link_libraries(test_backend_conn_pool PRIVATE proxy_lib)

add_executable(test_http_batching tests/integration/test_http_batching.cpp)
target_link_libraries(test_http_batching PRIVATE proxy_lib)

add_executable(test_service_discovery tests/integration/test_service_discovery.cpp)
target_link_libraries(test_service_discovery PRIVATE proxy_lib)

add_executable(test_ddos_accept_limit tests/integration/test_ddos_accept_limit.cpp)
target_link_libraries(test_ddos_accept_limit PRIVATE proxy_lib)

add_executable(test_http2_basic tests/integration/test_http2_basic.cpp)
target_link_libraries(test_http2_basic PRIVATE proxy_lib)

add_executable(test_grpc_h2c tests/integration/test_grpc_h2c.cpp)
target_link_libraries(test_grpc_h2c PRIVATE proxy_lib)

add_executable(test_protocol_conversion tests/integration/test_protocol_conversion.cpp)
target_link_libraries(test_protocol_conversion PRIVATE proxy_lib)

add_executable(test_compression_conversion tests/integration/test_compression_conversion.cpp)
target_link_libraries(test_compression_conversion PRIVATE proxy_lib)

add_executable(test_rewrite_rules tests/integration/test_rewrite_rules.cpp)
target_link_libraries(test_rewrite_rules PRIVATE proxy_lib)

add_executable(test_traffic_mirror tests/integration/test_traffic_mirror.cpp)
target_link_libraries(test_traffic_mirror PRIVATE proxy_lib)

add_executable(test_api_aggregate tests/integration/test_api_aggregate.cpp)
target_link_libraries(test_api_aggregate PRIVATE proxy_lib)

add_executable(test_cache_memcached tests/integration/test_cache_memcached.cpp)
target_link_libraries(test_cache_memcached PRIVATE proxy_lib)

add_executable(test_cache_redis tests/integration/test_cache_redis.cpp)
target_link_libraries(test_cache_redis PRIVATE proxy_lib)

add_executable(test_model_version_routing tests/integration/test_model_version_routing.cpp)
target_link_libraries(test_model_version_routing PRIVATE proxy_lib)

add_executable(test_batch_split tests/integration/test_batch_split.cpp)
target_link_libraries(test_batch_split PRIVATE proxy_lib)

add_executable(test_streaming_response tests/integration/test_streaming_response.cpp)
target_link_libraries(test_streaming_response PRIVATE proxy_lib)

add_executable(test_priority_queue tests/integration/test_priority_queue.cpp)
target_link_libraries(test_priority_queue PRIVATE proxy_lib)

add_executable(test_tls_acme tests/integration/test_tls_acme.cpp)
target_link_libraries(test_tls_acme PRIVATE proxy_lib)

add_executable(test_history tests/integration/test_history.cpp)
target_link_libraries(test_history PRIVATE proxy_lib)

add_executable(test_config_as_code tests/integration/test_config_as_code.cpp)
target_link_libraries(test_config_as_code PRIVATE proxy_lib)

add_executable(test_plugin_http tests/integration/test_plugin_http.cpp)
target_link_libraries(test_plugin_http PRIVATE proxy_lib)
add_dependencies(test_plugin_http proxy_example_plugin)

add_executable(test_l4_tunnel tests/integration/test_l4_tunnel.cpp)
target_link_libraries(test_l4_tunnel PRIVATE proxy_lib)

add_executable(test_fair_queue tests/integration/test_fair_queue.cpp)
target_link_libraries(test_fair_queue PRIVATE proxy_lib)

add_executable(test_edf tests/integration/test_edf.cpp)
target_link_libraries(test_edf PRIVATE proxy_lib)
