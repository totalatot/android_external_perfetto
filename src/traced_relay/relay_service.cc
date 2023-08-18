/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "src/traced_relay/relay_service.h"

#include <memory>

#include "perfetto/base/build_config.h"
#include "perfetto/base/logging.h"
#include "perfetto/base/task_runner.h"
#include "perfetto/ext/base/file_utils.h"
#include "perfetto/ext/base/hash.h"
#include "perfetto/ext/base/string_utils.h"
#include "perfetto/ext/base/unix_socket.h"
#include "perfetto/ext/base/utils.h"
#include "protos/perfetto/ipc/wire_protocol.gen.h"
#include "src/ipc/buffered_frame_deserializer.h"
#include "src/traced_relay/socket_relay_handler.h"

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

using ::perfetto::protos::gen::IPCFrame;

namespace perfetto {

RelayService::RelayService(base::TaskRunner* task_runner)
    : task_runner_(task_runner), machine_id_hint_(GetMachineIdHint()) {}

void RelayService::Start(const char* listening_socket_name,
                         const char* client_socket_name) {
  auto sock_family = base::GetSockFamily(listening_socket_name);
  listening_socket_ =
      base::UnixSocket::Listen(listening_socket_name, this, task_runner_,
                               sock_family, base::SockType::kStream);
  bool producer_socket_listening =
      listening_socket_ && listening_socket_->is_listening();
  if (!producer_socket_listening) {
    PERFETTO_FATAL("Failed to listen to socket %s", listening_socket_name);
  }

  // Save |client_socket_name| for opening new client connection to remote
  // service when a local producer connects.
  client_socket_name_ = client_socket_name;
}

void RelayService::OnNewIncomingConnection(
    base::UnixSocket* listen_socket,
    std::unique_ptr<base::UnixSocket> server_conn) {
  PERFETTO_DCHECK(listen_socket == listening_socket_.get());

  // Create a connection to the host to pair with |listen_conn|.
  auto sock_family = base::GetSockFamily(client_socket_name_.c_str());
  auto client_conn =
      base::UnixSocket::Connect(client_socket_name_, this, task_runner_,
                                sock_family, base::SockType::kStream);

  // Pre-queue the SetPeerIdentity request. By enqueueing it into the buffer,
  // this will be sent out as first frame as soon as we connect to the real
  // traced.
  //
  // This code pretends that we received a SetPeerIdentity frame from the
  // connecting producer (while instead we are just forging it). The host traced
  // will only accept only one SetPeerIdentity request pre-queued here.
  IPCFrame ipc_frame;
  ipc_frame.set_request_id(0);
  auto* set_peer_identity = ipc_frame.mutable_set_peer_identity();
#if PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID)
  set_peer_identity->set_pid(server_conn->peer_pid_linux());
#endif
  set_peer_identity->set_uid(
      static_cast<int32_t>(server_conn->peer_uid_posix()));

  set_peer_identity->set_machine_id_hint(machine_id_hint_);

  // Buffer the SetPeerIdentity request.
  auto req = ipc::BufferedFrameDeserializer::Serialize(ipc_frame);
  SocketWithBuffer server, client;
  PERFETTO_CHECK(server.available_bytes() >= req.size());
  memcpy(server.buffer(), req.data(), req.size());
  server.EnqueueData(req.size());

  // Shut down all callbacks associated with the socket in preparation for the
  // transfer to |socket_relay_handler_|.
  server.sock = server_conn->ReleaseSocket();
  auto new_socket_pair =
      std::make_unique<SocketPair>(std::move(server), std::move(client));
  pending_connections_.push_back(
      {std::move(new_socket_pair), std::move(client_conn)});
}

void RelayService::OnConnect(base::UnixSocket* self, bool connected) {
  // This only happens when the client connection is connected or has failed.
  auto it =
      std::find_if(pending_connections_.begin(), pending_connections_.end(),
                   [&](const PendingConnection& pending_conn) {
                     return pending_conn.connecting_client_conn.get() == self;
                   });
  PERFETTO_CHECK(it != pending_connections_.end());
  // Need to remove the element in |pending_connections_| regardless of
  // |connected|.
  auto remover = base::OnScopeExit([&]() { pending_connections_.erase(it); });

  if (!connected)
    return;  // This closes both sockets in PendingConnection.

  // Shut down event handlers and pair with a server connection.
  it->socket_pair->second.sock = self->ReleaseSocket();

  // Transfer the socket pair to SocketRelayHandler.
  socket_relay_handler_.AddSocketPair(std::move(it->socket_pair));
}

void RelayService::OnDisconnect(base::UnixSocket*) {
  PERFETTO_DFATAL("Should be unreachable.");
}

void RelayService::OnDataAvailable(base::UnixSocket*) {
  PERFETTO_DFATAL("Should be unreachable.");
}

std::string RelayService::GetMachineIdHint(
    bool use_pseudo_boot_id_for_testing) {
  // Gets kernel boot ID if possible.
  std::string boot_id;
  if (!use_pseudo_boot_id_for_testing &&
      base::ReadFile("/proc/sys/kernel/random/boot_id", &boot_id)) {
    return base::StripSuffix(boot_id, "\n");
  }

#if PERFETTO_BUILDFLAG(PERFETTO_OS_ANDROID) || \
    PERFETTO_BUILDFLAG(PERFETTO_OS_LINUX) ||   \
    PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
  auto get_pseudo_boot_id = []() -> std::string {
    base::Hasher hasher;
    const char* dev_path = "/dev";
    // Generate a pseudo-unique identifier for the current machine.
    // Source 1: system boot timestamp from the creation time of /dev inode.
#if PERFETTO_BUILDFLAG(PERFETTO_OS_APPLE)
    // Mac or iOS, just use stat(2).
    struct stat stat_buf {};
    int rc = PERFETTO_EINTR(stat(dev_path, &stat_buf));
    if (rc == -1)
      return std::string();
    hasher.Update(reinterpret_cast<const char*>(&stat_buf.st_birthtimespec),
                  sizeof(stat_buf.st_birthtimespec));
#else
    // Android or Linux, use statx(2)
    struct statx stat_buf {};
    auto rc = PERFETTO_EINTR(syscall(__NR_statx, /*dirfd=*/-1, dev_path,
                                     /*flags=*/0, STATX_BTIME, &stat_buf));
    if (rc == -1)
      return std::string();
    hasher.Update(reinterpret_cast<const char*>(&stat_buf.stx_btime),
                  sizeof(stat_buf.stx_btime));
#endif

    // Source 2: uname(2).
    utsname kernel_info{};
    if (uname(&kernel_info) == -1)
      return std::string();

    // Create a non-cryptographic digest of bootup timestamp and everything in
    // utsname.
    hasher.Update(reinterpret_cast<const char*>(&kernel_info),
                  sizeof(kernel_info));
    return base::Uint64ToHexStringNoPrefix(hasher.digest());
  };

  auto pseudo_boot_id = get_pseudo_boot_id();
  if (!pseudo_boot_id.empty())
    return pseudo_boot_id;
#endif

  // If all above failed, return nothing.
  return std::string();
}

}  // namespace perfetto
