#include "teacodec.h"

#include <QtEndian>

namespace {

constexpr quint32 Delta = 0x9e3779b9u;

struct Key {
    quint32 k[4];
};

const Key &teaKey() {
    static const Key key = [] {
        const QByteArray bytes = TeaCodec::key();
        Key result;
        for (int i = 0; i < 4; ++i)
            result.k[i] = qFromLittleEndian<quint32>(bytes.constData() + i * 4);
        return result;
    }();
    return key;
}

void processBlock(char *block, bool encrypt) {
    const auto &k = teaKey().k;
    quint32 v0 = qFromLittleEndian<quint32>(block);
    quint32 v1 = qFromLittleEndian<quint32>(block + 4);
    if (encrypt) {
        quint32 sum = 0;
        for (int i = 0; i < 32; ++i) {
            sum += Delta;
            v0 += ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
            v1 += ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
        }
    } else {
        quint32 sum = 0xc6ef3720u;
        for (int i = 0; i < 32; ++i) {
            v1 -= ((v0 << 4) + k[2]) ^ (v0 + sum) ^ ((v0 >> 5) + k[3]);
            v0 -= ((v1 << 4) + k[0]) ^ (v1 + sum) ^ ((v1 >> 5) + k[1]);
            sum -= Delta;
        }
    }
    qToLittleEndian(v0, block);
    qToLittleEndian(v1, block + 4);
}

QByteArray transform(const QByteArray &data, bool encrypt) {
    QByteArray out = data;
    out.detach();
    for (int offset = 0; offset + 8 <= out.size(); offset += 8)
        processBlock(out.data() + offset, encrypt);
    return out;
}

} // namespace

namespace TeaCodec {

QByteArray key() {
    return QByteArray::fromHex("cabaa5ca6d8a2abcba9e5acaca8bb89b");
}

QByteArray encrypt(const QByteArray &data) { return transform(data, true); }
QByteArray decrypt(const QByteArray &data) { return transform(data, false); }

QByteArray buildPacket(const QByteArray &plaintextPrefix) {
    QByteArray packet = plaintextPrefix.left(PacketSize);
    packet.append(QByteArray(PacketSize - packet.size(), '\0'));
    return encrypt(packet);
}

} // namespace TeaCodec
