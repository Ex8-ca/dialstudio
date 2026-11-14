// TEA codec and 20-byte packet framing for the Ulanzi D100H vendor channel.
#pragma once

#include <QByteArray>

namespace TeaCodec {

constexpr int PacketSize = 20;

// The fixed 16-byte key shared by every D100H (see research/PROTOCOL-kriomant.md §3).
QByteArray key();

// Original TEA, 32 rounds, little-endian words. Whole 8-byte blocks are
// processed independently; trailing bytes pass through untouched, which is how
// bytes 16-19 of a vendor packet stay in the clear.
QByteArray encrypt(const QByteArray &data);
QByteArray decrypt(const QByteArray &data);

// Zero-pads a plaintext prefix to 20 bytes and encrypts it for the wire.
QByteArray buildPacket(const QByteArray &plaintextPrefix);

} // namespace TeaCodec
