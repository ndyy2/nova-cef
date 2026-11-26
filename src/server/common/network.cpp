#include "network.hpp"
#include "logger.hpp"

#include <shared/utils.hpp>

namespace
{
    asio::ip::udp::endpoint MakeBindEndpoint(const std::string& bind_address, unsigned short port)
    {
        if (!bind_address.empty())
        {
            std::error_code ec;
            auto addr = asio::ip::make_address_v4(bind_address, ec);
            if (!ec)
            {
                return asio::ip::udp::endpoint(addr, port);
            }

            LOG_WARN("[Network] Invalid IPv4 bind address '%s'. Falling back to 0.0.0.0.", bind_address.c_str());
        }

        return asio::ip::udp::endpoint(asio::ip::udp::v4(), port);
    }
}

NetworkServer::NetworkServer(unsigned short port,
                             asio::io_context& context,
                             PacketHandler handler,
                             KcpTickHandler kcp_tick_handler,
                             const std::string& bind_address)
    : handler_(std::move(handler)),
      kcp_tick_handler_(std::move(kcp_tick_handler)),
      io_context_(context),
      socket_(io_context_, MakeBindEndpoint(bind_address, port)),
      kcp_update_timer_(context)
{
}

asio::ip::udp::endpoint NetworkServer::LocalEndpoint() const
{
    std::error_code ec;
    return socket_.local_endpoint(ec);
}

NetworkServer::~NetworkServer()
{
    Stop();
}

void NetworkServer::Start()
{
    if (running_)
        return;

    running_ = true;

    DoReceive();
    DoKcpUpdate();
}

void NetworkServer::Stop()
{

    if (!running_.exchange(false))
        return;

    std::error_code error_code;

    kcp_update_timer_.cancel();

    error_code.clear();
    socket_.cancel(error_code);
    socket_.close(error_code);
}

void NetworkServer::SendTo(const asio::ip::udp::endpoint& addr, const char* data, int length)
{
    if (!running_)
        return;

    auto send_buffer = std::make_shared<std::vector<char>>(data, data + length);
	// Pawn natives may flush KCP from the open.mp thread. Keep every operation
	// on the shared UDP socket serialized by its single ASIO io_context thread.
	asio::dispatch(io_context_, [this, addr, send_buffer]()
	{
		if (!running_)
			return;

		socket_.async_send_to(asio::buffer(*send_buffer),
			addr,
			[send_buffer](std::error_code ec, std::size_t)
			{
				if (ec && ec != asio::error::operation_aborted)
				{
					LOG_ERROR("[Network] Async send error: %s", ec.message().c_str());
				}
			});
	});
}

void NetworkServer::DoReceive()
{
    socket_.async_receive_from(asio::buffer(recv_buffer_),
        remote_endpoint_,
        [this](std::error_code ec, std::size_t bytes_recvd)
        {
            if (running_ && !ec && bytes_recvd > 0)
            {
                try { 
                    handler_(remote_endpoint_, recv_buffer_.data(), (int)bytes_recvd); 
                }
                catch (const std::exception& e) { 
                    LOG_ERROR("[Network] handler exception: %s", e.what()); 
                }
                catch (...) { 
                    LOG_ERROR("[Network] handler unknown exception"); 
                }
            }

            if (running_ && ec != asio::error::operation_aborted)
            {
                DoReceive();
            }
        });
}

void NetworkServer::DoKcpUpdate()
{
    if (!running_)
        return;

    if (kcp_tick_handler_)
    {
        kcp_tick_handler_(iclock());
    }

    kcp_update_timer_.expires_after(std::chrono::milliseconds(10));
    kcp_update_timer_.async_wait(
        [this](const std::error_code& ec)
        {
            if (ec == asio::error::operation_aborted) 
                return;

            if (!ec && running_)
            {
                DoKcpUpdate();
            }
        });
}
