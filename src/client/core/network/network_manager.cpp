#include "network_manager.hpp"
#include "shared/packet-serializer.hpp"
#include "shared/crypto.hpp"
#include "shared/utils.hpp"
#include "system/security_manager.hpp"
#include "system/resource_manager.hpp"
#include "system/logger.hpp"
#include "samp/samp.hpp"
#include "samp/components/netgame.hpp"
#include "samp/hooks/netgame.hpp"
#include "shared/events.hpp"
#include "shared/query_port.hpp"
#include "shared/version.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <mswsock.h>
#endif

constexpr int PORT_QUERY_RETRY_INTERVAL_MS = 500;
constexpr int CONNECT_RETRY_INTERVAL_MS = 1000;
constexpr int KCP_UPDATE_INTERVAL_MS = 10;
constexpr int KCP_SAFE_MTU = 1200;
constexpr uint64_t RECONNECT_BACKOFF_MS = 2000;

static uint64_t SteadyNowMs()
{
	return static_cast<uint64_t>(
		std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count());
}

static int kcp_client_output_callback(const char* buf, int len, ikcpcb* /*kcp*/, void* user)
{
	auto* self = static_cast<NetworkManager*>(user);
	self->SendRaw(buf, len);
	return 0;
}

NetworkManager::NetworkManager(ResourceManager& resource) : resource_(resource), socket_(io_context_), connect_timer_(io_context_), kcp_update_timer_(io_context_)
{

}

NetworkManager::~NetworkManager()
{
	Shutdown();
}

void NetworkManager::SetSessionActiveHandler(SessionActiveHandler handler)
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    session_handler_ = std::move(handler);
}

bool NetworkManager::CanAttemptConnect() const
{
	return SteadyNowMs() >= reconnect_backoff_until_ms_.load(std::memory_order_acquire);
}

bool NetworkManager::Initialize(const std::string& ip, unsigned short port)
{
	return Initialize(ip, 0, port);
}

bool NetworkManager::Initialize(const std::string& ip, unsigned short game_port, unsigned short fallback_cef_port)
{
	try {
		SecurityManager::Initialize();

		asio::ip::address addr;
		asio::error_code parse_ec;
		auto ipv4 = asio::ip::make_address_v4(ip, parse_ec);
		if (!parse_ec)
		{
			addr = ipv4;
		}
		else
		{
			asio::ip::udp::resolver resolver(io_context_);
			const unsigned short resolve_port = game_port != 0 ? game_port : fallback_cef_port;
			auto resolved = *resolver.resolve(asio::ip::udp::v4(), ip, std::to_string(resolve_port)).begin();
			addr = resolved.endpoint().address();
		}

		game_endpoint_ = game_port != 0
			? asio::ip::udp::endpoint(addr, game_port)
			: asio::ip::udp::endpoint{};
		fallback_cef_port_ = fallback_cef_port;
		server_endpoint_ = asio::ip::udp::endpoint(addr, fallback_cef_port);
	}
	catch (const std::exception& e) {
		LOG_ERROR("[NetworkManager] Initialization failed: {}", e.what());
		return false;
	}

	non_cef_server_.store(false);
	failed_join_cycles_ = 0;
	reconnect_backoff_until_ms_.store(0, std::memory_order_release);
	FireSessionActive(false);

	if (game_endpoint_.port() != 0)
	{
		LOG_INFO("[NetworkManager] Initialized for game server {}:{} (auto-detecting cef_port, fallback {}).",
			game_endpoint_.address().to_string().c_str(), game_endpoint_.port(), fallback_cef_port_);
	}
	else
	{
		LOG_INFO("[NetworkManager] Initialized for server at {}:{}",
			server_endpoint_.address().to_string().c_str(), server_endpoint_.port());
	}
	return true;
}

void NetworkManager::Shutdown()
{
	Disconnect();

	if (network_thread_.joinable() && network_thread_.get_id() != std::this_thread::get_id()) {
		network_thread_.join();
	}
}

void NetworkManager::Connect(int playerid)
{
    LOG_INFO("[CLIENT] Connect() called with playerid={}", playerid);

    if (network_thread_.joinable() && network_thread_.get_id() != std::this_thread::get_id())
    {
        network_thread_.join();
        CleanupTransport();
    }

    const bool should_discover_port = (game_endpoint_.port() != 0);
    const ConnectionState initial_state = should_discover_port
        ? ConnectionState::DISCOVERING_PORT
        : ConnectionState::SENDING_JOIN;

    ConnectionState expected = ConnectionState::DISCONNECTED;
    if (!state_.compare_exchange_strong(expected, initial_state, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        LOG_WARN("[CLIENT] Connect() rejected: already in state {}", (int)state_.load());
        return;
    }

    LOG_DEBUG("[CLIENT] Current non_cef_server={}", non_cef_server_.load());

	if (non_cef_server_.load())
    {
        state_.store(ConnectionState::DISCONNECTED, std::memory_order_release);
		return;
    }

	playerid_ = playerid;
	query_attempts_ = 0;
	join_attempts_ = 0;
	finalize_attempts_ = 0;
	logged_unexpected_endpoint_ = false;

	LOG_INFO("[CLIENT] Connecting to server for playerid {}...", playerid_);

	try {
		if (io_context_.stopped()) io_context_.restart();

		asio::error_code ec;
		socket_.open(asio::ip::udp::v4(), ec);
		if (ec) throw std::runtime_error("Socket open failed: " + ec.message());
#ifdef _WIN32
		{
			BOOL new_behavior = FALSE;
			DWORD bytes_returned = 0;
			::WSAIoctl(
				socket_.native_handle(),
				SIO_UDP_CONNRESET,
				&new_behavior,
				sizeof(new_behavior),
				nullptr,
				0,
				&bytes_returned,
				nullptr,
				nullptr);
		}
#endif
		socket_.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), 0), ec);
		if (ec) throw std::runtime_error("Socket bind failed: " + ec.message());

		LOG_INFO("[CLIENT] Socket opened and bound to local port {}.", socket_.local_endpoint().port());
		DoReceive();

		auto keys = SecurityManager::GenerateKeys();
		client_public_key_ = std::move(keys.public_key);
		client_private_key_ = std::move(keys.private_key);

		if (should_discover_port)
		{
			asio::post(io_context_, [this]() { DoSendPortQuery(); });
		}
		else
		{
			asio::post(io_context_, [this]() { DoSendRequestJoin(); });
		}

		if (!network_thread_.joinable()) {
			network_thread_ = std::thread([this]() {
				LOG_INFO("[CLIENT] Network thread started.");

				for (;;)
				{
					try
					{
						io_context_.run();
						break;
					}
					catch (const std::exception& e)
					{
						LOG_ERROR("[CLIENT] Exception on the network thread: {}", e.what());
					}
					catch (...)
					{
						LOG_ERROR("[CLIENT] Unknown exception on the network thread.");
					}
				}

				LOG_INFO("[CLIENT] Network thread finished.");
			});
		}
	}
	catch (const std::exception& e) {
		LOG_ERROR("[CLIENT] Connection process failed to start: {}", e.what());
		Disconnect();
	}
}

void NetworkManager::Disconnect()
{
    ConnectionState prev = state_.exchange(ConnectionState::DISCONNECTED, std::memory_order_acq_rel);
	if (prev == ConnectionState::DISCONNECTED)
		return;

	LOG_INFO("[CLIENT] Disconnecting...");
	FireSessionActive(false);

    if (network_thread_.joinable() && network_thread_.get_id() == std::this_thread::get_id())
    {
        asio::post(io_context_, [this]() {
            CleanupTransport();
        });
        return;
    }

    if (network_thread_.joinable())
    {
        asio::post(io_context_, [this]() {
            CleanupTransport();
        });

        network_thread_.join();
    }

    CleanupTransport();
}

void NetworkManager::DoSendPortQuery()
{
	if (state_ != ConnectionState::DISCOVERING_PORT)
		return;

	query_attempts_++;
	if (query_attempts_ > MAX_QUERY_ATTEMPTS)
	{
		BeginJoinWithPort(fallback_cef_port_, "query timeout -> default offset");
		return;
	}

	const auto ipv4_bytes = game_endpoint_.address().to_v4().to_bytes();
	const auto query_pkt = cef_query::BuildSampRulesQuery(ipv4_bytes, game_endpoint_.port());
	auto buffer = std::make_shared<std::vector<char>>(query_pkt.begin(), query_pkt.end());

	LOG_DEBUG("[CLIENT] Querying server {}:{} for cef_port rule (attempt {}/{})...",
		game_endpoint_.address().to_string().c_str(), game_endpoint_.port(), query_attempts_, MAX_QUERY_ATTEMPTS);

	if (socket_.is_open())
	{
		socket_.async_send_to(
			asio::buffer(*buffer), game_endpoint_,
			[buffer](std::error_code ec, std::size_t) {
				if (ec && ec != asio::error::operation_aborted)
				{
					LOG_WARN("[CLIENT] Port query UDP send error: {}", ec.message());
				}
			});
	}

	connect_timer_.expires_after(std::chrono::milliseconds(PORT_QUERY_RETRY_INTERVAL_MS));
	connect_timer_.async_wait([this](const std::error_code& ec) {
		if (!ec && state_ == ConnectionState::DISCOVERING_PORT)
		{
			DoSendPortQuery();
		}
	});
}

void NetworkManager::BeginJoinWithPort(uint16_t port, const char* source)
{
	ConnectionState expected = ConnectionState::DISCOVERING_PORT;
	if (!state_.compare_exchange_strong(expected, ConnectionState::SENDING_JOIN, std::memory_order_acq_rel, std::memory_order_acquire))
		return;

	if (port == 0)
	{
		LOG_ERROR("[CLIENT] No valid CEF port available ({}). Disconnecting.", source);
		non_cef_server_.store(true);
		Disconnect();
		return;
	}

	server_endpoint_ = asio::ip::udp::endpoint(game_endpoint_.address(), port);
	LOG_INFO("[CLIENT] Resolved CEF server endpoint {}:{} ({}).",
		server_endpoint_.address().to_string().c_str(), server_endpoint_.port(), source);

	DoSendRequestJoin();
}

void NetworkManager::DoSendRequestJoin()
{
	if (state_ != ConnectionState::SENDING_JOIN) 
		return;

	join_attempts_++;
	if (join_attempts_ > MAX_JOIN_ATTEMPTS)
	{
		failed_join_cycles_++;
		if (failed_join_cycles_ >= MAX_FAILED_JOIN_CYCLES)
		{
			LOG_WARN("[CLIENT] No handshake after {} attempts ({} cycles) -> assuming non-CEF server. Disconnecting.",
				MAX_JOIN_ATTEMPTS, failed_join_cycles_);
			non_cef_server_.store(true);
		}
		else
		{
			LOG_WARN("[CLIENT] No handshake after {} attempts (cycle {}/{}) -> backing off {}ms before retrying.",
				MAX_JOIN_ATTEMPTS, failed_join_cycles_, MAX_FAILED_JOIN_CYCLES, RECONNECT_BACKOFF_MS);
			reconnect_backoff_until_ms_.store(SteadyNowMs() + RECONNECT_BACKOFF_MS, std::memory_order_release);
		}
		Disconnect();
		return;
	}

	LOG_INFO("[CLIENT] Sending RequestJoin packet (attempt {}/{})...", join_attempts_, MAX_JOIN_ATTEMPTS);

	RequestJoinPacket pkt{ playerid_, PLUGIN_VERSION_U32 };
	SendPacket(PacketType::RequestJoin, pkt);

	connect_timer_.expires_after(std::chrono::milliseconds(CONNECT_RETRY_INTERVAL_MS));
	connect_timer_.async_wait([this](const std::error_code& ec) {
		if (!ec) {
			LOG_WARN("[CLIENT] Connection timer expired, retrying join...");
			DoSendRequestJoin();
		}
	});
}

void NetworkManager::DoSendHandshakeFinalize()
{
	if (state_ != ConnectionState::AWAITING_ACCEPTANCE)
		return;

	finalize_attempts_++;
	if (finalize_attempts_ > MAX_FINALIZE_ATTEMPTS)
	{
		LOG_WARN("[CLIENT] No JoinResponse after {} HandshakeFinalize attempts -> disconnecting to retry.", MAX_FINALIZE_ATTEMPTS);
		reconnect_backoff_until_ms_.store(SteadyNowMs() + 1000ULL, std::memory_order_release);
		Disconnect();
		return;
	}

	LOG_INFO("[CLIENT] Sending HandshakeFinalize (attempt {}/{})...", finalize_attempts_, MAX_FINALIZE_ATTEMPTS);
	SendPacket(PacketType::HandshakeFinalize, pending_finalize_pkt_);

	connect_timer_.expires_after(std::chrono::milliseconds(CONNECT_RETRY_INTERVAL_MS));
	connect_timer_.async_wait([this](const std::error_code& ec) {
		if (!ec && state_ == ConnectionState::AWAITING_ACCEPTANCE) {
			LOG_WARN("[CLIENT] HandshakeFinalize timer expired, retrying...");
			DoSendHandshakeFinalize();
		}
	});
}

void NetworkManager::DoReceive()
{
	socket_.async_receive_from(
		asio::buffer(recv_buffer_), remote_endpoint_,
		[this](std::error_code ec, std::size_t bytes_recvd) {
			if (!ec && bytes_recvd > 0) {
				if (state_ == ConnectionState::DISCOVERING_PORT) {
					if (remote_endpoint_ == game_endpoint_) {
						uint16_t discovered_port = 0;
						const auto parse_result = cef_query::ParseSampRulesCefPort(
							recv_buffer_.data(), bytes_recvd, discovered_port);

						if (parse_result == cef_query::QueryParseResult::Found) {
							connect_timer_.cancel();
							BeginJoinWithPort(discovered_port, "server query rule cef_port");
						}
						else if (parse_result == cef_query::QueryParseResult::NotFound) {
							connect_timer_.cancel();
							BeginJoinWithPort(fallback_cef_port_, "default offset (no cef_port rule)");
						}
					}
				}
				else if (remote_endpoint_ == server_endpoint_) {
					HandleRawMessage(recv_buffer_.data(), bytes_recvd);
				}
				else if (!logged_unexpected_endpoint_ &&
					(state_ == ConnectionState::SENDING_JOIN || state_ == ConnectionState::AWAITING_ACCEPTANCE)) {
					logged_unexpected_endpoint_ = true;
					LOG_WARN("[CLIENT] Ignored UDP packet ({} bytes) from unexpected endpoint {}:{} (expected {}:{}).",
						bytes_recvd,
						remote_endpoint_.address().to_string().c_str(), remote_endpoint_.port(),
						server_endpoint_.address().to_string().c_str(), server_endpoint_.port());
				}
			}
			if (state_ != ConnectionState::DISCONNECTED) {
				DoReceive();
			}
		}
	);
}

void NetworkManager::HandleRawMessage(const char* data, size_t len)
{
	std::vector<std::vector<uint8_t>> messages;
	bool handled_by_kcp = false;
	{
		std::lock_guard<std::mutex> lock(kcp_mutex_);
		if (kcp_instance_) {
			handled_by_kcp = true;

			int input_res = ikcp_input(kcp_instance_, data, static_cast<long>(len));
			if (input_res < 0) {
				LOG_WARN("[KCP] ikcp_input error: {}.", input_res);
			}

			ReceiveKcpMessagesLocked(messages);
		}
	}

	if (handled_by_kcp) {
		DispatchKcpMessages(messages);
		return;
	}

	NetworkPacket packet;
	if (!DeserializePacket(data, len, packet)) {
		LOG_WARN("[CLIENT] Failed to deserialize raw packet of size {}.", len);
		return;
	}

	LOG_INFO("[CLIENT] Raw packet deserialized, type: {}", static_cast<int>(packet.type));

	switch (packet.type)
	{
		case PacketType::HandshakeChallenge:
		{
			if (state_ != ConnectionState::SENDING_JOIN) 
				return;

			connect_timer_.cancel();
			state_ = ConnectionState::AWAITING_ACCEPTANCE;

			const auto& challenge = std::get<HandshakeChallengePacket>(packet.payload);
			if (challenge.server_public_key.size() != crypto_kx_PUBLICKEYBYTES)
			{
				LOG_ERROR("[CLIENT] Invalid server public key ({} bytes). Disconnecting.", challenge.server_public_key.size());
				Disconnect();
				return;
			}

			auto session_keys = SecurityManager::GenerateClientSessionKeys(
				client_public_key_, client_private_key_, challenge.server_public_key);
			if (session_keys.rx.empty()) { Disconnect(); return; }
			rx_key_ = std::move(session_keys.rx);
			tx_key_ = std::move(session_keys.tx);
			sodium_memzero(client_private_key_.data(), client_private_key_.size());

			pending_finalize_pkt_ = HandshakeFinalizePacket{};
			pending_finalize_pkt_.cookie = challenge.cookie;
			pending_finalize_pkt_.client_public_key = client_public_key_;
			finalize_attempts_ = 0;

			DoSendHandshakeFinalize();
			break;
		}
		case PacketType::JoinResponse:
		{
			if (state_ != ConnectionState::AWAITING_ACCEPTANCE && state_ != ConnectionState::SENDING_JOIN)
				return;

			connect_timer_.cancel();
			pending_finalize_pkt_ = HandshakeFinalizePacket{};
			finalize_attempts_ = 0;

			const auto& response = std::get<JoinResponsePacket>(packet.payload);
			if (!response.accepted) {
				const uint32_t sv = response.server_version;
				const uint32_t cv = response.client_version;
				std::string msg = response.message;
				
				if (msg.empty()) 
					msg = "Join rejected";

				LOG_ERROR("[CLIENT] Join rejected by server: {} (reason={}, client={}, server={})",
					msg,
					(int)response.reject_reason,
					VersionToString(cv),
					VersionToString(sv));

				non_cef_server_.store(true);
				Disconnect();
				return;
			}

			LOG_INFO("[CLIENT] Join accepted! Initializing KCP with conv id {}.", response.kcp_conv_id);

			non_cef_server_.store(false);
			failed_join_cycles_ = 0;
			reconnect_backoff_until_ms_.store(0, std::memory_order_release);
			FireSessionActive(true);

			{
				std::lock_guard<std::mutex> lock(kcp_mutex_);
				kcp_instance_ = ikcp_create(response.kcp_conv_id, this);
				kcp_instance_->output = kcp_client_output_callback;
				ikcp_setmtu(kcp_instance_, KCP_SAFE_MTU);
				ikcp_nodelay(kcp_instance_, 1, 10, 2, 1);
				ikcp_wndsize(kcp_instance_, 128, 128);
			}

			if (rx_key_.empty() || tx_key_.empty()) {
				LOG_ERROR("[CLIENT] Session keys not initialized!");
				Disconnect();
				return;
			}

			auto* netGame = GetComponent<NetGameComponent>();
			if (netGame) {
				resource_.OnConnect(netGame->GetIp(), netGame->GetPort());
			}

			if (!response.manifest_json.empty()) {
				resource_.OnManifestReceived(response.manifest_json);
			}

			state_ = ConnectionState::CONNECTED;

			DoKcpUpdate();

			PacketHandler handler;
			{
				std::lock_guard lock(handler_mutex_);
				handler = packet_handler_;
			}

			if (handler)
				handler(packet);

			break;
		}
		default:
			LOG_WARN("[CLIENT] Received unexpected raw packet of type {} during handshake.", static_cast<int>(packet.type));
			break;
	}
}

void NetworkManager::ReceiveKcpMessagesLocked(std::vector<std::vector<uint8_t>>& out)
{
	while (kcp_instance_)
	{
		const int peek_size = ikcp_peeksize(kcp_instance_);
		if (peek_size <= 0)
			break;

		std::vector<char> kcp_buffer(static_cast<size_t>(peek_size));
		const int msg_size = ikcp_recv(kcp_instance_, kcp_buffer.data(), peek_size);
		if (msg_size <= 0)
			break;

		std::vector<uint8_t> decrypted = DecryptPacket({ kcp_buffer.begin(), kcp_buffer.begin() + msg_size }, rx_key_);
		if (decrypted.empty()) {
			LOG_WARN("[CLIENT] Failed to decrypt KCP packet.");
			continue;
		}

		out.push_back(std::move(decrypted));
	}
}

void NetworkManager::DispatchKcpMessages(const std::vector<std::vector<uint8_t>>& messages)
{
	for (const auto& decrypted : messages)
	{
		NetworkPacket packet;
		if (!DeserializePacket(reinterpret_cast<const char*>(decrypted.data()), decrypted.size(), packet)) {
			LOG_WARN("[CLIENT] Failed to deserialize decrypted KCP packet.");
			continue;
		}

		PacketHandler handler;
		{ std::lock_guard lock(handler_mutex_); handler = packet_handler_; }
		if (handler) handler(packet);
	}
}

void NetworkManager::CleanupTransport()
{
    asio::error_code ignored;

    connect_timer_.cancel();
    kcp_update_timer_.cancel();

    {
        std::lock_guard<std::mutex> lock(kcp_mutex_);
        if (kcp_instance_)
        {
            ikcp_release(kcp_instance_);
            kcp_instance_ = nullptr;
        }
    }

    if (socket_.is_open())
        socket_.close(ignored);

    rx_key_.clear();
    tx_key_.clear();
    client_public_key_.clear();
    client_private_key_.clear();
    pending_finalize_pkt_ = HandshakeFinalizePacket{};
    query_attempts_ = 0;
    join_attempts_ = 0;
    finalize_attempts_ = 0;
    playerid_ = -1;
}

void NetworkManager::FireSessionActive(bool active)
{
    SessionActiveHandler handler;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        handler = session_handler_;
    }

    if (handler)
        handler(active);
}

void NetworkManager::SendPacket(PacketType type, const PacketPayload& payload)
{
	LOG_DEBUG("[CLIENT] SendPacket called, type={}", static_cast<int>(type));

	NetworkPacket packet{ type, payload };
	std::string raw;

	if (!SerializePacket(packet, raw)) {
		LOG_ERROR("[CLIENT] Failed to serialize packet type {}", static_cast<int>(type));
		return;
	}

	std::lock_guard<std::mutex> lock(kcp_mutex_);

	if (kcp_instance_ && state_ == ConnectionState::CONNECTED) {
		if (tx_key_.empty()) {
			LOG_ERROR("[CLIENT] tx_key_ is empty!");
			return;
		}

		if (tx_key_.size() != 32) {
			LOG_ERROR("[CLIENT] tx_key_ has invalid size: {}", tx_key_.size());
			return;
		}

		std::vector<uint8_t> encrypted = EncryptPacket({ raw.begin(), raw.end() }, tx_key_);

		if (encrypted.empty()) {
			LOG_ERROR("[CLIENT] Failed to encrypt packet");
			return;
		}

		int sent = ikcp_send(kcp_instance_,
			reinterpret_cast<const char*>(encrypted.data()),
			static_cast<int>(encrypted.size()));

		if (sent < 0) {
			LOG_ERROR("[CLIENT] ikcp_send failed with error: {}", sent);
			return;
		}

		ikcp_flush(kcp_instance_);
	}
	else {
		SendRaw(raw.data(), static_cast<int>(raw.size()));
	}
}

void NetworkManager::DoKcpUpdate()
{
	std::lock_guard<std::mutex> lock(kcp_mutex_);

	if (state_ != ConnectionState::CONNECTED || !kcp_instance_)
		return;

	ikcp_update(kcp_instance_, iclock());

	kcp_update_timer_.expires_after(std::chrono::milliseconds(KCP_UPDATE_INTERVAL_MS));
	kcp_update_timer_.async_wait([this](const std::error_code& ec) { if (!ec) DoKcpUpdate(); });
}

void NetworkManager::SendRaw(const char* data, int size) {
	auto buffer = std::make_shared<std::vector<char>>(data, data + size);

	asio::post(io_context_, [this, buffer]() {
		if (!socket_.is_open())
			return;

		socket_.async_send_to(
			asio::buffer(*buffer), server_endpoint_,
			[buffer](std::error_code ec, std::size_t) {
				if (ec) {
					LOG_ERROR("[CLIENT] UDP send error: {}", ec.message());
				}
			}
		);
	});
}

void NetworkManager::SetPacketHandler(PacketHandler handler) 
{
	std::lock_guard lock(handler_mutex_);
	packet_handler_ = std::move(handler);
}

void NetworkManager::SendBrowserCreateResult(int browserId, bool success, int code, const std::string& reason)
{
	ClientEmitEventPacket event;
    event.name = CefEvent::Client::BrowserCreateResult;
    event.browserId = browserId;
    event.args.emplace_back(success);
    event.args.emplace_back(code);
    event.args.emplace_back(reason);

	SendPacket(PacketType::ClientEmitEvent, event);
}