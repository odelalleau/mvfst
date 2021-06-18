/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 */

#include <quic/state/DatagramHandlers.h>

namespace quic {

void handleDatagram(QuicConnectionStateBase& conn, DatagramFrame& frame) {
  if (conn.datagramState.readBuffer.size() >=
          conn.datagramState.maxReadBufferSize ||
      // TODO(lniccolini) update max datagram frame size
      // https://github.com/quicwg/datagram/issues/3
      // For now, max_datagram_size > 0 means the peer supports datagram frames
      conn.datagramState.maxReadFrameSize == 0) {
    frame.data.move();
    return;
  }
  conn.datagramState.readBuffer.emplace_back(std::move(frame.data));
}

} // namespace quic
