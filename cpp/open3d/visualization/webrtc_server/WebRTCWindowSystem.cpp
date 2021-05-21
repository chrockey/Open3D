// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2020 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/visualization/webrtc_server/WebRTCWindowSystem.h"

#include <p2p/base/basic_packet_socket_factory.h>
#include <p2p/base/stun_server.h>
#include <p2p/base/turn_server.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#include <chrono>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "open3d/core/Tensor.h"
#include "open3d/utility/Console.h"
#include "open3d/utility/Helper.h"
#include "open3d/utility/IJsonConvertible.h"
#include "open3d/visualization/gui/Application.h"
#include "open3d/visualization/gui/Events.h"
#include "open3d/visualization/gui/Window.h"
#include "open3d/visualization/webrtc_server/HttpServerRequestHandler.h"
#include "open3d/visualization/webrtc_server/PeerConnectionManager.h"

namespace open3d {
namespace visualization {
namespace webrtc_server {

// List of ICE servers (STUN or TURN). STUN servers is used by default. In
// certain network configurations TURN servers are required to forward WebRTC
// traffic.
static const std::list<std::string> s_public_ice_servers{
        // "stun:stun.l.google.com:19302",
};
static const std::list<std::string> s_open3d_ice_servers{
        // "turn:user:password@34.69.27.100:3478",
        // "turn:user:password@34.69.27.100:3478?transport=tcp",
};

static std::string GetEnvWebRTCIP() {
    if (const char *env_p = std::getenv("WEBRTC_IP")) {
        return std::string(env_p);
    } else {
        return "localhost";
    }
}

static std::string GetEnvWebRTCPublicIP() {
    if (const char *env_p = std::getenv("WEBRTC_PUBLIC_IP")) {
        return std::string(env_p);
    } else {
        return "localhost";
    }
}

static std::string GetEnvWebRTCPort() {
    if (const char *env_p = std::getenv("WEBRTC_PORT")) {
        return std::string(env_p);
    } else {
        return "8888";
    }
}

// clang-format off
/// Get custom STUN server address from WEBRTC_STUN_SERVER environment variable.
/// If there are more than one server, separate them with ";".
/// Example usage:
/// 1. Set WEBRTC_STUN_SERVER to:
///    - UDP only
///      WEBRTC_STUN_SERVER="turn:user:password@$(curl -s ifconfig.me):3478"
///    - TCP only
///      WEBRTC_STUN_SERVER="turn:user:password@$(curl -s ifconfig.me):3478?transport=tcp"
///    - UDP and TCP
///      WEBRTC_STUN_SERVER="turn:user:password@$(curl -s ifconfig.me):3478;turn:user:password@$(curl -s ifconfig.me):3478?transport=tcp"
/// 2. Start your TURN server binding to a local IP address and port
/// 3. Set router configurations to forward your local IP address and port to
///    the public IP address and port.
// clang-format on
static std::string GetCustomSTUNServer() {
    if (const char *env_p = std::getenv("WEBRTC_STUN_SERVER")) {
        return std::string(env_p);
    } else {
        return "";
    }
}

struct WebRTCWindowSystem::Impl {
    std::unordered_map<WebRTCWindowSystem::OSWindow, std::string>
            os_window_to_uid_;
    std::string GenerateUID() {
        static std::atomic<size_t> count{0};
        return "window_" + std::to_string(count++);
    }

    // HTTP handshake server settings.
    bool http_handshake_enabled_ = true;
    std::string http_address_;  // Used when http_handshake_enabled_ == true.
    std::string web_root_;      // Used when http_handshake_enabled_ == true.

    // PeerConnectionManager is used for setting up connections and managing API
    // call entry points.
    std::unique_ptr<PeerConnectionManager> peer_connection_manager_ = nullptr;

    std::thread webrtc_thread_;
    bool sever_started_ = false;
};

std::shared_ptr<WebRTCWindowSystem> WebRTCWindowSystem::GetInstance() {
    static std::shared_ptr<WebRTCWindowSystem> instance(new WebRTCWindowSystem);
    return instance;
}

WebRTCWindowSystem::WebRTCWindowSystem()
    : BitmapWindowSystem(
#if !defined(__APPLE__) && !defined(_WIN32) && !defined(_WIN64)
              BitmapWindowSystem::Rendering::HEADLESS
#else
              BitmapWindowSystem::Rendering::NORMAL
#endif
              ),
      impl_(new WebRTCWindowSystem::Impl()) {

    // impl_->web_root_ is filled at StartWebRTCServer. It relies on
    // GetResourcePath(), which happens after Application::Initialize().
    impl_->http_handshake_enabled_ = true;
    impl_->http_address_ = GetEnvWebRTCIP() + ":" + GetEnvWebRTCPort();

    // Server->client send frame.
    auto draw_callback = [this](const gui::Window *window,
                                std::shared_ptr<core::Tensor> im) -> void {
        OnFrame(GetWindowUID(window->GetOSWindow()), im);
    };
    SetOnWindowDraw(draw_callback);
}

WebRTCWindowSystem::~WebRTCWindowSystem() {
    impl_->peer_connection_manager_ = nullptr;
    rtc::Thread::Current()->Quit();
}

WebRTCWindowSystem::OSWindow WebRTCWindowSystem::CreateOSWindow(
        gui::Window *o3d_window,
        int width,
        int height,
        const char *title,
        int flags) {
    // No-op if the server is already running.
    StartWebRTCServer();
    WebRTCWindowSystem::OSWindow os_window = BitmapWindowSystem::CreateOSWindow(
            o3d_window, width, height, title, flags);
    std::string window_uid = impl_->GenerateUID();
    impl_->os_window_to_uid_.insert({os_window, window_uid});
    utility::LogInfo("Window {} created.", window_uid);
    return os_window;
}

void WebRTCWindowSystem::DestroyWindow(OSWindow w) {
    std::string window_uid = impl_->os_window_to_uid_.at(w);
    CloseWindowConnections(window_uid);
    impl_->os_window_to_uid_.erase(w);
    BitmapWindowSystem::DestroyWindow(w);
    utility::LogInfo("Window {} destroyed.", window_uid);
}

std::vector<std::string> WebRTCWindowSystem::GetWindowUIDs() const {
    std::vector<std::string> uids;
    for (const auto &it : impl_->os_window_to_uid_) {
        uids.push_back(it.second);
    }
    return uids;
}

std::string WebRTCWindowSystem::GetWindowUID(
        WebRTCWindowSystem::OSWindow w) const {
    if (impl_->os_window_to_uid_.count(w) == 0) {
        return "window_undefined";
    } else {
        return impl_->os_window_to_uid_.at(w);
    }
}

WebRTCWindowSystem::OSWindow WebRTCWindowSystem::GetOSWindowByUID(
        const std::string &uid) const {
    // This can be optimized by adding a bi-directional map, but it may not be
    // worth it since we typically don't have lots of windows.
    for (const auto &it : impl_->os_window_to_uid_) {
        if (it.second == uid) {
            return it.first;
        };
    }
    return nullptr;
}

void WebRTCWindowSystem::StartWebRTCServer() {
    if (!impl_->sever_started_) {
        auto start_webrtc_thread = [this]() {
            // Ensure Application::Initialize() is called before this.
            std::string resource_path(
                    gui::Application::GetInstance().GetResourcePath());
            impl_->web_root_ = resource_path + "/html";

            // Logging settings.
            // src/rtc_base/logging.h: LS_VERBOSE, LS_ERROR
            rtc::LogMessage::LogToDebug((rtc::LoggingSeverity)rtc::LS_ERROR);

            rtc::LogMessage::LogTimestamps();
            rtc::LogMessage::LogThreads();

            // PeerConnectionManager manages all WebRTC connections.
            rtc::Thread *thread = rtc::Thread::Current();
            rtc::InitializeSSL();
            Json::Value config;
            std::list<std::string> ice_servers;
            ice_servers.insert(ice_servers.end(), s_public_ice_servers.begin(),
                               s_public_ice_servers.end());
            if (!GetCustomSTUNServer().empty()) {
                std::vector<std::string> custom_servers =
                        utility::SplitString(GetCustomSTUNServer(), ";");
                ice_servers.insert(ice_servers.end(), custom_servers.begin(),
                                   custom_servers.end());
            }
            ice_servers.insert(ice_servers.end(), s_open3d_ice_servers.begin(),
                               s_open3d_ice_servers.end());
            utility::LogInfo("ICE servers: {}", ice_servers);

            impl_->peer_connection_manager_ =
                    std::make_unique<PeerConnectionManager>(
                            ice_servers, config["urls"], ".*", "");
            if (!impl_->peer_connection_manager_->InitializePeerConnection()) {
                utility::LogError("InitializePeerConnection() failed.");
            }

            // CivetWeb server is used for WebRTC handshake. This is enabled
            // when running as a standalone application, and is disabled when
            // running in Jupyter.
            if (impl_->http_handshake_enabled_) {
                utility::LogInfo("WebRTC HTTP server handshake mode enabled.");
                std::vector<std::string> options{"document_root",
                                                 impl_->web_root_,
                                                 "enable_directory_listing",
                                                 "no",
                                                 "additional_header",
                                                 "X-Frame-Options: SAMEORIGIN",
                                                 "access_control_allow_origin",
                                                 "*",
                                                 "listening_ports",
                                                 impl_->http_address_,
                                                 "enable_keep_alive",
                                                 "yes",
                                                 "keep_alive_timeout_ms",
                                                 "1000",
                                                 "decode_url",
                                                 "no"};
                try {
                    // PeerConnectionManager provides callbacks for the Civet
                    // server.
                    std::map<std::string,
                             HttpServerRequestHandler::HttpFunction>
                            func = impl_->peer_connection_manager_
                                           ->GetHttpApi();

                    // Internal address.
                    std::unique_ptr<cricket::TurnServer> turn_server;
                    rtc::SocketAddress int_addr;
                    int_addr.FromString(GetEnvWebRTCIP() + ":3478");
                    turn_server.reset(new cricket::TurnServer(thread));
                    // UDP.
                    rtc::AsyncUDPSocket *udp_socket =
                            rtc::AsyncUDPSocket::Create(thread->socketserver(),
                                                        int_addr);
                    if (udp_socket) {
                        std::cout << "TURN Listening UDP at: "
                                  << int_addr.ToString() << std::endl;
                        turn_server->AddInternalSocket(udp_socket,
                                                       cricket::PROTO_UDP);
                    }
                    // TCP.
                    rtc::AsyncSocket *tcp_socket =
                            thread->socketserver()->CreateAsyncSocket(
                                    AF_INET, SOCK_STREAM);
                    if (tcp_socket) {
                        std::cout << "TURN Listening TCP at: "
                                  << int_addr.ToString() << std::endl;
                        tcp_socket->Bind(int_addr);
                        tcp_socket->Listen(5);
                        turn_server->AddInternalServerSocket(
                                tcp_socket, cricket::PROTO_TCP);
                    }
                    // External address.
                    rtc::SocketAddress ext_addr;
                    ext_addr.FromString(GetEnvWebRTCPublicIP() + ":3478");
                    std::cout << "TURN external addr: " << ext_addr.ToString()
                              << std::endl;
                    turn_server->SetExternalSocketFactory(
                            new rtc::BasicPacketSocketFactory(),
                            rtc::SocketAddress(ext_addr.ipaddr(), 0));

                    // Main loop for Civet server.
                    utility::LogInfo("Open3D WebVisualizer is serving at {}.",
                                     impl_->http_address_);
                    utility::LogInfo(
                            "Set WEBRTC_IP and WEBRTC_PORT environment "
                            "variable to "
                            "customize server address.",
                            impl_->http_address_);
                    HttpServerRequestHandler civet_server(func, options);
                    thread->Run();
                } catch (const CivetException &ex) {
                    utility::LogError("Cannot start Civet server: {}",
                                      ex.what());
                }
            } else {
                utility::LogInfo("WebRTC Jupyter handshake mode enabled.");
                thread->Run();
            }
            rtc::CleanupSSL();
        };
        impl_->webrtc_thread_ = std::thread(start_webrtc_thread);
        impl_->sever_started_ = true;
    }
}

void WebRTCWindowSystem::OnDataChannelMessage(const std::string &message) {
    utility::LogDebug("WebRTCWindowSystem::OnDataChannelMessage: {}", message);
    try {
        const Json::Value value = utility::StringToJson(message);
        const std::string window_uid = value.get("window_uid", "").asString();
        if (value.get("class_name", "").asString() == "MouseEvent" &&
            window_uid != "") {
            gui::MouseEvent me;
            if (me.FromJson(value)) {
                PostMouseEvent(GetOSWindowByUID(window_uid), me);
            }
        } else if (value.get("class_name", "").asString() == "ResizeEvent" &&
                   window_uid != "") {
            const int height = value.get("height", 0).asInt();
            const int width = value.get("width", 0).asInt();
            if (height <= 0 || width <= 0) {
                utility::LogWarning(
                        "Invalid heigh {} or width {}, ResizeEvent ignored.",
                        height, width);
            }
            utility::LogDebug("ResizeEvent {}: ({}, {})", window_uid, height,
                              width);
            SetWindowSize(GetOSWindowByUID(window_uid), width, height);
        }
    } catch (...) {
        utility::LogInfo(
                "OnDataChannelMessage: {}. Message cannot be parsed, or "
                "the target GUI event failed to execute.",
                message);
    }
}

void WebRTCWindowSystem::OnFrame(const std::string &window_uid,
                                 const std::shared_ptr<core::Tensor> &im) {
    impl_->peer_connection_manager_->OnFrame(window_uid, im);
}

void WebRTCWindowSystem::SendInitFrames(const std::string &window_uid) {
    utility::LogInfo("Sending init frames to {}.", window_uid);
    static const int s_max_initial_frames = 5;
    static const int s_sleep_between_frames_ms = 100;
    for (int i = 0; i < s_max_initial_frames; ++i) {
        PostRedrawEvent(GetOSWindowByUID(window_uid));
        std::this_thread::sleep_for(
                std::chrono::milliseconds(s_sleep_between_frames_ms));
        utility::LogDebug("Sent init frames #{} to {}.", i, window_uid);
    }
}

std::string WebRTCWindowSystem::CallHttpAPI(const std::string &entry_point,
                                            const std::string &query_string,
                                            const std::string &data) const {
    utility::LogInfo("[Called HTTP API (custom handshake)] {}", entry_point);

    std::string query_string_trimmed = "";
    if (!query_string.empty() && query_string[0] == '?') {
        query_string_trimmed =
                query_string.substr(1, query_string.length() - 1);
    }

    std::string result = "";
    if (entry_point == "/api/getMediaList") {
        result = utility::JsonToString(
                impl_->peer_connection_manager_->GetMediaList());
    } else if (entry_point == "/api/getIceServers") {
        result = utility::JsonToString(
                impl_->peer_connection_manager_->GetIceServers());
    } else if (entry_point == "/api/getIceCandidate") {
        std::string peerid;
        if (!query_string_trimmed.empty()) {
            CivetServer::getParam(query_string_trimmed.c_str(), "peerid",
                                  peerid);
        }
        result = utility::JsonToString(
                impl_->peer_connection_manager_->GetIceCandidateList(peerid));
    } else if (entry_point == "/api/hangup") {
        std::string peerid;
        if (!query_string_trimmed.empty()) {
            CivetServer::getParam(query_string_trimmed.c_str(), "peerid",
                                  peerid);
        }
        result = utility::JsonToString(
                impl_->peer_connection_manager_->HangUp(peerid));
    } else if (entry_point == "/api/call") {
        std::string peerid;
        std::string url;
        std::string options;
        if (!query_string_trimmed.empty()) {
            CivetServer::getParam(query_string_trimmed.c_str(), "peerid",
                                  peerid);
            CivetServer::getParam(query_string_trimmed.c_str(), "url", url);
            CivetServer::getParam(query_string_trimmed.c_str(), "options",
                                  options);
        }
        result = utility::JsonToString(impl_->peer_connection_manager_->Call(
                peerid, url, options, utility::StringToJson(data)));
    } else if (entry_point == "/api/addIceCandidate") {
        std::string peerid;
        if (!query_string_trimmed.empty()) {
            CivetServer::getParam(query_string_trimmed.c_str(), "peerid",
                                  peerid);
        }
        result = utility::JsonToString(
                impl_->peer_connection_manager_->AddIceCandidate(
                        peerid, utility::StringToJson(data)));
    }

    utility::LogDebug("entry_point: {}", entry_point);
    utility::LogDebug("query_string_trimmed: {}", query_string_trimmed);
    utility::LogDebug("data: {}", data);
    utility::LogDebug("result: {}", result);

    return result;
}

void WebRTCWindowSystem::EnableWebRTC() {
    utility::LogInfo("WebRTC GUI backend enabled.");
    gui::Application::GetInstance().SetWindowSystem(GetInstance());
}

void WebRTCWindowSystem::DisableHttpHandshake() {
    utility::LogInfo("WebRTCWindowSystem: HTTP handshake server disabled.");
    impl_->http_handshake_enabled_ = false;
}

void WebRTCWindowSystem::CloseWindowConnections(const std::string &window_uid) {
    impl_->peer_connection_manager_->CloseWindowConnections(window_uid);
}

}  // namespace webrtc_server
}  // namespace visualization
}  // namespace open3d
