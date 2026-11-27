#include "cabbird/websocket_server.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cabbird {
namespace {

constexpr std::string_view kWebSocketMagic =
    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t kMaximumHandshakeBytes = 16U * 1024U;
constexpr std::size_t kMaximumTextBytes = 1024U * 1024U;
constexpr std::size_t kMaximumQueuedMessages = 16U;
constexpr std::size_t kMaximumClients = 8U;
constexpr std::size_t kMaximumQueuedFramesPerClient = kMaximumQueuedMessages;
constexpr std::size_t kMaximumQueuedBytesPerClient =
    kMaximumQueuedFramesPerClient * (kMaximumTextBytes + 10U);
constexpr auto kFinalDrainTimeout = std::chrono::milliseconds(1000);
constexpr std::uint64_t kPortRequestPortMask = 0xFFFFU;
constexpr unsigned kPortRequestGenerationShift = 16U;

CabbirdStatusV1 Status(const std::uint32_t code) noexcept {
    return {code, 0, {}};
}

bool CaseInsensitiveEquals(const std::string_view left, const std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index{}; index != left.size(); ++index) {
        const unsigned char a = static_cast<unsigned char>(left[index]);
        const unsigned char b = static_cast<unsigned char>(right[index]);
        if ((a >= 'A' && a <= 'Z' ? a + ('a' - 'A') : a) !=
            (b >= 'A' && b <= 'Z' ? b + ('a' - 'A') : b)) {
            return false;
        }
    }
    return true;
}

std::string_view Trim(std::string_view value) noexcept {
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
        value.remove_suffix(1);
    }
    return value;
}

std::optional<std::string> HeaderValue(
    const std::string_view request, const std::string_view wanted, const bool singleton = false) {
    std::optional<std::string> result;
    std::size_t cursor = request.find("\r\n");
    if (cursor == std::string_view::npos) return std::nullopt;
    cursor += 2U;
    while (cursor < request.size()) {
        const std::size_t line_end = request.find("\r\n", cursor);
        const std::string_view line = request.substr(
            cursor, line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor);
        const std::size_t colon = line.find(':');
        if (colon != std::string_view::npos &&
            CaseInsensitiveEquals(Trim(line.substr(0, colon)), wanted)) {
            const std::string_view value = Trim(line.substr(colon + 1U));
            if (value.empty() || (singleton && result)) return std::nullopt;
            if (result) {
                result->push_back(',');
                result->append(value);
            } else {
                result = std::string(value);
            }
        }
        if (line_end == std::string_view::npos) break;
        cursor = line_end + 2U;
    }
    return result;
}

bool ContainsToken(std::string_view value, const std::string_view wanted) noexcept {
    while (!value.empty()) {
        const std::size_t comma = value.find(',');
        if (CaseInsensitiveEquals(Trim(value.substr(0, comma)), wanted)) return true;
        if (comma == std::string_view::npos) break;
        value.remove_prefix(comma + 1U);
    }
    return false;
}

bool IsWebSocketRequestLine(const std::string_view line) noexcept {
    if (!line.starts_with("GET ")) return false;
    const std::size_t separator = line.find(' ', 4U);
    if (separator == std::string_view::npos || separator == 4U ||
        line.substr(4U, separator - 4U).find('\t') != std::string_view::npos) return false;
    const std::string_view version = line.substr(separator + 1U);
    if (!version.starts_with("HTTP/")) return false;
    unsigned major{}, minor{};
    const char* const end = version.data() + version.size();
    const auto parsed_major = std::from_chars(version.data() + 5U, end, major);
    if (parsed_major.ec != std::errc{} || parsed_major.ptr == end || *parsed_major.ptr != '.') {
        return false;
    }
    const auto parsed_minor = std::from_chars(parsed_major.ptr + 1U, end, minor);
    return parsed_minor.ec == std::errc{} && parsed_minor.ptr == end &&
        (major > 1U || (major == 1U && minor >= 1U));
}

bool IsWebSocketKey(const std::string_view key) noexcept {
    // A 16-byte nonce has 22 base64 digits followed by two padding bytes.
    if (key.size() != 24U || !key.ends_with("==")) return false;
    for (const char digit : key.substr(0U, 22U)) {
        if (!((digit >= 'A' && digit <= 'Z') || (digit >= 'a' && digit <= 'z') ||
              (digit >= '0' && digit <= '9') || digit == '+' || digit == '/')) return false;
    }
    return key[21] == 'A' || key[21] == 'Q' || key[21] == 'g' || key[21] == 'w';
}

std::string Base64Encode(const std::span<const std::uint8_t> bytes) {
    constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t index{}; index < bytes.size(); index += 3U) {
        const std::uint32_t first = bytes[index];
        const bool has_second = index + 1U < bytes.size();
        const bool has_third = index + 2U < bytes.size();
        const std::uint32_t second = has_second ? bytes[index + 1U] : 0U;
        const std::uint32_t third = has_third ? bytes[index + 2U] : 0U;
        const std::uint32_t block = (first << 16U) | (second << 8U) | third;
        result.push_back(kAlphabet[(block >> 18U) & 0x3FU]);
        result.push_back(kAlphabet[(block >> 12U) & 0x3FU]);
        result.push_back(has_second ? kAlphabet[(block >> 6U) & 0x3FU] : '=');
        result.push_back(has_third ? kAlphabet[block & 0x3FU] : '=');
    }
    return result;
}

std::optional<std::string> WebSocketAcceptValue(const std::string_view key) {
    const std::string source = std::string(key) + std::string(kWebSocketMagic);
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) < 0) {
        return std::nullopt;
    }
    DWORD object_size{};
    DWORD returned{};
    const NTSTATUS object_status = BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
        &returned, 0);
    if (object_status < 0 || returned != sizeof(object_size)) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::vector<std::uint8_t> object(object_size);
    BCRYPT_HASH_HANDLE hash{};
    const NTSTATUS create_status = BCryptCreateHash(
        algorithm, &hash, object.data(), static_cast<ULONG>(object.size()), nullptr, 0, 0);
    if (create_status < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return std::nullopt;
    }
    std::array<std::uint8_t, 20> digest{};
    const NTSTATUS hash_status = BCryptHashData(
        hash, reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
        static_cast<ULONG>(source.size()), 0);
    const NTSTATUS finish_status = hash_status < 0
        ? hash_status
        : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finish_status < 0) return std::nullopt;
    return Base64Encode(digest);
}

bool SendAll(const SOCKET socket, const std::string_view bytes) noexcept {
    std::size_t sent{};
    while (sent < bytes.size()) {
        const std::size_t remaining = bytes.size() - sent;
        const int chunk = static_cast<int>((std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
        const int result = send(socket, bytes.data() + sent, chunk, 0);
        if (result <= 0) return false;
        sent += static_cast<std::size_t>(result);
    }
    return true;
}

bool SetNonBlocking(const SOCKET socket) noexcept {
    u_long enabled = 1;
    return ioctlsocket(socket, FIONBIO, &enabled) == 0;
}

std::string BuildTextFrame(const std::string_view message) {
    std::string frame;
    frame.reserve(message.size() + 10U);
    frame.push_back(static_cast<char>(0x81U));
    if (message.size() <= 125U) {
        frame.push_back(static_cast<char>(message.size()));
    } else if (message.size() <= 0xFFFFU) {
        frame.push_back(static_cast<char>(126U));
        frame.push_back(static_cast<char>((message.size() >> 8U) & 0xFFU));
        frame.push_back(static_cast<char>(message.size() & 0xFFU));
    } else {
        frame.push_back(static_cast<char>(127U));
        const std::uint64_t size = message.size();
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((size >> shift) & 0xFFU));
        }
    }
    frame.append(message);
    return frame;
}

void CloseSocket(const SOCKET socket) noexcept {
    if (socket != INVALID_SOCKET) {
        shutdown(socket, SD_BOTH);
        closesocket(socket);
    }
}

[[nodiscard]] SOCKET CreateListener(const std::uint16_t port) noexcept {
    SOCKET listening = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listening == INVALID_SOCKET) return INVALID_SOCKET;

    // Multiple runtimes can coexist on this machine. A busy port must reject
    // startup/rebind instead of sharing a listener with another process.
    const BOOL exclusive = TRUE;
    if (setsockopt(listening, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0) {
        CloseSocket(listening);
        return INVALID_SOCKET;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listening, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listening, SOMAXCONN) == SOCKET_ERROR || !SetNonBlocking(listening)) {
        CloseSocket(listening);
        return INVALID_SOCKET;
    }
    return listening;
}

[[nodiscard]] constexpr std::uint64_t PackPortRequest(
    const std::uint64_t generation, const std::uint16_t port) noexcept {
    return (generation << kPortRequestGenerationShift) | port;
}

[[nodiscard]] constexpr std::uint64_t PortRequestGeneration(
    const std::uint64_t request) noexcept {
    return request >> kPortRequestGenerationShift;
}

[[nodiscard]] constexpr std::uint16_t PortRequestPort(const std::uint64_t request) noexcept {
    return static_cast<std::uint16_t>(request & kPortRequestPortMask);
}

}  // namespace

struct WebSocketServer::Impl final {
    explicit Impl(const std::uint16_t requested_port) : port(requested_port) {}

    struct Client final {
        SOCKET socket{INVALID_SOCKET};
        std::deque<std::string> frames;
        std::size_t front_offset{};
        std::size_t queued_bytes{};
        std::string incoming;
        bool closing{};
    };

    std::atomic<std::uint16_t> port{};
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> pending_port_request{};
    std::atomic<SOCKET> listener{INVALID_SOCKET};
    std::atomic<SOCKET> handshaking_client{INVALID_SOCKET};
    std::atomic_bool running{};
    std::atomic_bool winsock_ready{};
    std::atomic<std::uint32_t> connected_clients{};
    std::atomic<std::uint64_t> published_messages{};
    std::atomic<std::uint64_t> dropped_messages{};
    mutable std::mutex lifecycle_mutex;
    std::thread worker;
    std::mutex queue_mutex;
    std::deque<std::string> outbound;
    CabbirdWebSocketServiceV1 service{};

    void RebindPendingListener() noexcept {
        const std::uint64_t request = pending_port_request.exchange(0U, std::memory_order_acq_rel);
        const std::uint16_t requested = PortRequestPort(request);
        const std::uint64_t request_generation = PortRequestGeneration(request);
        if (requested == 0U || request_generation != generation.load(std::memory_order_acquire) ||
            requested == port.load(std::memory_order_acquire)) {
            return;
        }

        std::scoped_lock lock(lifecycle_mutex);
        if (!running.load(std::memory_order_acquire) ||
            request_generation != generation.load(std::memory_order_acquire) ||
            requested == port.load(std::memory_order_acquire)) {
            return;
        }
        const SOCKET replacement = CreateListener(requested);
        if (replacement == INVALID_SOCKET) return;

        const SOCKET previous = listener.exchange(replacement, std::memory_order_acq_rel);
        port.store(requested, std::memory_order_release);
        CloseSocket(previous);
    }

    [[nodiscard]] bool Handshake(const SOCKET client) noexcept {
        try {
            DWORD receive_timeout = 1000U;
            u_long blocking = 0;
            if (ioctlsocket(client, FIONBIO, &blocking) != 0) return false;
            static_cast<void>(setsockopt(
                client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)));
            static_cast<void>(setsockopt(
                client, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&receive_timeout), sizeof(receive_timeout)));
            const auto reject = [client](const bool unsupported_version = false) noexcept {
                static_cast<void>(SendAll(client, unsupported_version
                    ? "HTTP/1.1 426 Upgrade Required\r\nSec-WebSocket-Version: 13\r\n"
                      "Connection: close\r\nContent-Length: 0\r\n\r\n"
                    : "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n"));
                return false;
            };
            std::string request;
            request.reserve(1024U);
            std::array<char, 1024> buffer{};
            while (request.size() < kMaximumHandshakeBytes) {
                const int received = recv(client, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received <= 0) return false;
                request.append(buffer.data(), static_cast<std::size_t>(received));
                const std::size_t end = request.find("\r\n\r\n");
                if (end == std::string::npos) continue;
                const std::string_view header(request.data(), end + 2U);
                if (!IsWebSocketRequestLine(header.substr(0U, header.find("\r\n")))) return reject();
                const auto host = HeaderValue(header, "Host", true);
                const auto upgrade = HeaderValue(header, "Upgrade");
                const auto connection = HeaderValue(header, "Connection");
                const auto version = HeaderValue(header, "Sec-WebSocket-Version", true);
                const auto key = HeaderValue(header, "Sec-WebSocket-Key", true);
                if (!host || !upgrade || !ContainsToken(*upgrade, "websocket") ||
                    !connection || !ContainsToken(*connection, "Upgrade") ||
                    !version || !key || !IsWebSocketKey(*key)) return reject();
                if (*version != "13") return reject(true);
                const auto accept = WebSocketAcceptValue(*key);
                if (!accept) return false;
                const std::string response =
                    "HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + *accept + "\r\n\r\n";
                if (!SendAll(client, response)) return false;
                return SetNonBlocking(client);
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    void AcceptClients(std::vector<Client>& clients) noexcept {
        const SOCKET listening = listener.load(std::memory_order_acquire);
        if (listening == INVALID_SOCKET) return;
        for (;;) {
            SOCKET client = accept(listening, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) break;
                return;
            }
            if (clients.size() >= kMaximumClients || !running.load(std::memory_order_acquire)) {
                CloseSocket(client);
                continue;
            }
            handshaking_client.store(client, std::memory_order_release);
            const bool accepted = Handshake(client);
            const SOCKET completed = handshaking_client.exchange(
                INVALID_SOCKET, std::memory_order_acq_rel);
            if (completed != client) continue;
            if (!accepted || !running.load(std::memory_order_acquire)) {
                CloseSocket(client);
                continue;
            }
            try {
                clients.push_back({client});
            } catch (...) {
                CloseSocket(client);
                return;
            }
            connected_clients.store(
                static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
        }
    }

    [[nodiscard]] bool HandleClientFrames(Client& client) {
        while (client.incoming.size() >= 2U) {
            const auto* bytes = reinterpret_cast<const unsigned char*>(client.incoming.data());
            const unsigned opcode = bytes[0] & 0x0FU;
            const bool control = (opcode & 0x08U) != 0;
            // No extensions are negotiated. Client frames are always masked;
            // Ping/Close must be final and fit their protocol-defined bound.
            if ((bytes[0] & 0x70U) != 0 || (bytes[1] & 0x80U) == 0 ||
                (control && (bytes[0] & 0x80U) == 0) ||
                (opcode != 0 && opcode != 1 && opcode != 2 && opcode != 8 && opcode != 9 && opcode != 10)) {
                return false;
            }
            std::uint64_t size = bytes[1] & 0x7FU;
            std::size_t prefix = 2U;
            if (size == 126U || size == 127U) {
                const std::size_t length_bytes = size == 126U ? 2U : 8U;
                if (client.incoming.size() < prefix + length_bytes) return true;
                size = 0;
                for (std::size_t index = 0; index < length_bytes; ++index)
                    size = (size << 8U) | bytes[prefix + index];
                prefix += length_bytes;
            }
            if (size > kMaximumTextBytes || (control && size > 125U) ||
                (opcode == 8U && size == 1U)) return false;
            const std::size_t payload_size = static_cast<std::size_t>(size);
            const std::size_t payload = prefix + 4U;
            if (client.incoming.size() < payload + payload_size) return true;
            if (opcode == 8U || opcode == 9U) {
                const unsigned response_opcode = opcode == 9U ? 10U : 8U;
                std::string response{
                    static_cast<char>(0x80U | response_opcode), static_cast<char>(payload_size)};
                response.reserve(2U + payload_size);
                for (std::size_t index = 0; index < payload_size; ++index)
                    response.push_back(static_cast<char>(bytes[payload + index] ^ bytes[prefix + index % 4U]));
                if (!QueueFrame(client, response)) return false;
                if (opcode == 8U) {
                    client.closing = true;
                    client.incoming.clear();
                    return true;
                }
            }
            // Application messages and Pong require no broadcaster callback.
            client.incoming.erase(0, payload + payload_size);
        }
        return true;
    }

    [[nodiscard]] bool DrainClient(Client& client) noexcept {
        try {
            std::array<char, 2048> buffer{};
            for (;;) {
                const int received = recv(client.socket, buffer.data(), static_cast<int>(buffer.size()), 0);
                if (received > 0) {
                    client.incoming.append(buffer.data(), static_cast<std::size_t>(received));
                    if (!HandleClientFrames(client)) return false;
                    if (client.closing) return true;
                    continue;
                }
                if (received == 0) return false;
                return WSAGetLastError() == WSAEWOULDBLOCK;
            }
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool QueueFrame(Client& client, const std::string& frame) noexcept {
        if (client.frames.size() >= kMaximumQueuedFramesPerClient ||
            frame.size() > kMaximumQueuedBytesPerClient - client.queued_bytes) {
            return false;
        }
        try {
            client.frames.push_back(frame);
            client.queued_bytes += frame.size();
            return true;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool FlushClient(Client& client) noexcept {
        while (!client.frames.empty()) {
            std::string& frame = client.frames.front();
            const std::size_t remaining = frame.size() - client.front_offset;
            const int chunk = static_cast<int>((std::min)(
                remaining, static_cast<std::size_t>((std::numeric_limits<int>::max)())));
            const int sent = send(client.socket, frame.data() + client.front_offset, chunk, 0);
            if (sent > 0) {
                const std::size_t sent_bytes = static_cast<std::size_t>(sent);
                client.front_offset += sent_bytes;
                client.queued_bytes -= sent_bytes;
                if (client.front_offset == frame.size()) {
                    client.frames.pop_front();
                    client.front_offset = 0;
                }
                continue;
            }
            if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) return true;
            return false;
        }
        return !client.closing;
    }

    void FlushWritableClients(std::vector<Client>& clients, fd_set& writable) noexcept {
        for (auto iterator = clients.begin(); iterator != clients.end();) {
            if (FD_ISSET(iterator->socket, &writable) && !FlushClient(*iterator)) {
                CloseSocket(iterator->socket);
                iterator = clients.erase(iterator);
            } else {
                ++iterator;
            }
        }
        connected_clients.store(
            static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
    }

    [[nodiscard]] static bool HasPendingFrames(const std::vector<Client>& clients) noexcept {
        return std::any_of(clients.begin(), clients.end(), [](const Client& client) {
            return !client.frames.empty();
        });
    }

    void DrainPendingClients(std::vector<Client>& clients) noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kFinalDrainTimeout;
        while (!clients.empty() && HasPendingFrames(clients)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return;
            const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
            timeval timeout{};
            timeout.tv_sec = static_cast<long>(remaining.count() / 1000000LL);
            timeout.tv_usec = static_cast<long>(remaining.count() % 1000000LL);
            fd_set writable{};
            for (const Client& client : clients) {
                if (!client.frames.empty()) FD_SET(client.socket, &writable);
            }
            const int ready = select(0, nullptr, &writable, nullptr, &timeout);
            if (ready <= 0) return;
            FlushWritableClients(clients, writable);
        }
    }

    void DeliverQueued(std::vector<Client>& clients) noexcept {
        std::deque<std::string> pending;
        {
            std::scoped_lock lock(queue_mutex);
            pending.swap(outbound);
        }
        if (pending.empty() || clients.empty()) return;
        for (const std::string& message : pending) {
            std::string frame;
            try {
                frame = BuildTextFrame(message);
            } catch (...) {
                dropped_messages.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            for (Client& client : clients) {
                if (client.closing) continue;
                if (!QueueFrame(client, frame)) {
                    dropped_messages.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        connected_clients.store(
            static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
    }

    void Run() noexcept {
        std::vector<Client> clients;
        while (running.load(std::memory_order_acquire)) {
            RebindPendingListener();
            fd_set read_set{};
            fd_set write_set{};
            const SOCKET listening = listener.load(std::memory_order_acquire);
            if (listening != INVALID_SOCKET) FD_SET(listening, &read_set);
            for (const Client& client : clients) {
                if (!client.closing) FD_SET(client.socket, &read_set);
                if (!client.frames.empty()) FD_SET(client.socket, &write_set);
            }
            timeval timeout{};
            timeout.tv_usec = 50000;
            const int ready = select(0, &read_set, &write_set, nullptr, &timeout);
            if (ready > 0) {
                if (listening != INVALID_SOCKET && FD_ISSET(listening, &read_set)) {
                    AcceptClients(clients);
                }
                for (auto iterator = clients.begin(); iterator != clients.end();) {
                    if (FD_ISSET(iterator->socket, &read_set) && !DrainClient(*iterator)) {
                        CloseSocket(iterator->socket);
                        iterator = clients.erase(iterator);
                    } else {
                        ++iterator;
                    }
                }
                connected_clients.store(
                    static_cast<std::uint32_t>(clients.size()), std::memory_order_release);
                FlushWritableClients(clients, write_set);
            }
            DeliverQueued(clients);
        }
        DeliverQueued(clients);
        DrainPendingClients(clients);
        for (const Client& client : clients) CloseSocket(client.socket);
        connected_clients.store(0, std::memory_order_release);
    }
};

namespace {

CabbirdStatusV1 CABBIRD_CALL PublishTextV1(
    void* user, const CabbirdStringViewV1 message) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || (message.data == nullptr && message.size != 0U)) {
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    if (message.size > kMaximumTextBytes) return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    if (message.size != 0U && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            message.data, static_cast<int>(message.size), nullptr, 0) == 0) {
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    if (!server->PublishText({message.data, message.size})) {
        const CabbirdWebSocketServerInfoV1 info = server->Snapshot();
        return Status(info.port == 0U ? CABBIRD_STATUS_V1_UNAVAILABLE
                                      : CABBIRD_STATUS_V1_CONFLICT);
    }
    return Status(CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 CABBIRD_CALL ServerInfoV1(
    void* user, CabbirdWebSocketServerInfoV1* info) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || info == nullptr || info->struct_size < sizeof(*info)) {
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    *info = server->Snapshot();
    return Status(info->port == 0U ? CABBIRD_STATUS_V1_UNAVAILABLE
                                   : CABBIRD_STATUS_V1_OK);
}

CabbirdStatusV1 CABBIRD_CALL SetPortV1(void* user, const std::uint16_t port) noexcept {
    auto* const server = static_cast<WebSocketServer*>(user);
    if (server == nullptr || port == 0U) {
        return Status(CABBIRD_STATUS_V1_INVALID_ARGUMENT);
    }
    return Status(server->RequestPortChange(port)
        ? CABBIRD_STATUS_V1_OK
        : CABBIRD_STATUS_V1_UNAVAILABLE);
}

}  // namespace

WebSocketServer::WebSocketServer(const std::uint16_t port)
    : impl_(std::make_unique<Impl>(port)) {
    impl_->service = {
        sizeof(CabbirdWebSocketServiceV1), CABBIRD_WEBSOCKET_SERVICE_V1_VERSION,
        this, PublishTextV1, ServerInfoV1, SetPortV1};
}

WebSocketServer::~WebSocketServer() {
    Stop();
}

bool WebSocketServer::Start() noexcept {
    if (impl_ == nullptr) return false;
    std::scoped_lock lock(impl_->lifecycle_mutex);
    if (impl_->running.load(std::memory_order_acquire)) return true;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
    impl_->winsock_ready.store(true, std::memory_order_release);
    impl_->generation.fetch_add(1U, std::memory_order_acq_rel);
    SOCKET listening = CreateListener(impl_->port.load(std::memory_order_acquire));
    if (listening == INVALID_SOCKET) {
        WSACleanup();
        impl_->winsock_ready.store(false, std::memory_order_release);
        return false;
    }
    impl_->listener.store(listening, std::memory_order_release);
    impl_->running.store(true, std::memory_order_release);
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
    } catch (...) {
        impl_->running.store(false, std::memory_order_release);
        CloseSocket(impl_->listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        WSACleanup();
        impl_->winsock_ready.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

void WebSocketServer::Stop() noexcept {
    if (impl_ == nullptr) return;
    std::thread worker;
    bool cleanup{};
    {
        std::scoped_lock lock(impl_->lifecycle_mutex);
        if (!impl_->running.exchange(false, std::memory_order_acq_rel)) return;
        CloseSocket(impl_->listener.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        CloseSocket(impl_->handshaking_client.exchange(INVALID_SOCKET, std::memory_order_acq_rel));
        worker = std::move(impl_->worker);
        cleanup = impl_->winsock_ready.exchange(false, std::memory_order_acq_rel);
    }
    if (worker.joinable()) worker.join();
    {
        std::scoped_lock lock(impl_->queue_mutex);
        impl_->outbound.clear();
    }
    if (cleanup) WSACleanup();
}

bool WebSocketServer::PublishText(const std::string_view message) noexcept {
    if (impl_ == nullptr || message.size() > kMaximumTextBytes ||
        !impl_->running.load(std::memory_order_acquire)) {
        return false;
    }
    try {
        std::scoped_lock lock(impl_->queue_mutex);
        if (!impl_->running.load(std::memory_order_acquire)) return false;
        if (impl_->outbound.size() >= kMaximumQueuedMessages) {
            impl_->dropped_messages.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        impl_->outbound.emplace_back(message);
        impl_->published_messages.fetch_add(1, std::memory_order_relaxed);
        return true;
    } catch (...) {
        impl_->dropped_messages.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool WebSocketServer::RequestPortChange(const std::uint16_t port) noexcept {
    if (impl_ == nullptr || port == 0U) return false;
    const std::uint64_t request_generation = impl_->generation.load(std::memory_order_acquire);
    if (request_generation == 0U || !impl_->running.load(std::memory_order_acquire) ||
        impl_->generation.load(std::memory_order_acquire) != request_generation) {
        return false;
    }
    const std::uint64_t request = PackPortRequest(request_generation, port);
    std::uint64_t pending = impl_->pending_port_request.load(std::memory_order_acquire);
    do {
        if (PortRequestGeneration(pending) > request_generation) return false;
    } while (!impl_->pending_port_request.compare_exchange_weak(
        pending, request, std::memory_order_release, std::memory_order_acquire));

    if (impl_->running.load(std::memory_order_acquire) &&
        impl_->generation.load(std::memory_order_acquire) == request_generation) {
        return true;
    }
    std::uint64_t expected = request;
    static_cast<void>(impl_->pending_port_request.compare_exchange_strong(
        expected, 0U, std::memory_order_acq_rel, std::memory_order_acquire));
    return false;
}

CabbirdWebSocketServerInfoV1 WebSocketServer::Snapshot() const noexcept {
    if (impl_ == nullptr) return {sizeof(CabbirdWebSocketServerInfoV1)};
    const bool running = impl_->running.load(std::memory_order_acquire);
    return {
        sizeof(CabbirdWebSocketServerInfoV1),
        running ? impl_->port.load(std::memory_order_acquire) : 0U,
        0,
        impl_->connected_clients.load(std::memory_order_acquire),
        impl_->published_messages.load(std::memory_order_acquire),
        impl_->dropped_messages.load(std::memory_order_acquire)};
}

const CabbirdWebSocketServiceV1* WebSocketServer::Service() const noexcept {
    return impl_ == nullptr ? nullptr : &impl_->service;
}

}  // namespace cabbird
