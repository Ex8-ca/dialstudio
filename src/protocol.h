// Message types, command builders and the parser for the D100H vendor channel.
#pragma once

#include <QByteArray>
#include <QString>

namespace Protocol {

// Physical layout names. key1..key3 is the top row left to right, key4/key5 the
// upper side keys, key6/key7 the lower side keys (wire IDs 06 and 08).
QString keyName(int key);               // 1..7 -> "key1".."key7"
int keyFromWireId(quint8 id);           // 01-06 -> 1-6, 08 -> 7, else 0
quint8 wireIdFromKey(int key);

struct Message {
    enum class Kind {
        Unknown,
        KeyDown,       // key = 1..7
        KeyUp,
        KnobClick,     // fired once, on 07 01; 07 00 is reported as Ignored
        Rotate,        // direction = +1 cw / -1 ccw, pressed = knob held
        Offline,       // IDs 41-44: online mode lapsed
        Ignored,       // knob click release, heartbeat acks we do not surface
        Heartbeat,     // 06 01 23 10 xx
        Haptics,       // on
        Sleep,         // seconds
        Firmware,      // text, customCode
        SerialFragment,// index, text
        Battery,       // millivolts, percent, flags
    };

    Kind kind = Kind::Unknown;
    int key = 0;
    int direction = 0;
    bool pressed = false;
    bool on = false;
    bool writeAck = false;     // reply to a set command rather than a read
    int seconds = 0;
    int index = 0;
    int millivolts = 0;
    int percent = 0;
    int lowBattery = 0;
    bool powerOff = false;
    bool charging = false;
    bool full = false;
    QString text;
    QString customCode;
    QByteArray raw;            // decrypted 20 bytes
};

// Decrypts a 20-byte notify value and parses it. Anything of the wrong length
// is Unknown; unknown prefixes are never errors (pitfall 5).
Message parse(const QByteArray &encrypted);
Message parsePlain(const QByteArray &plain);

// Wire-ready (encrypted) commands.
QByteArray heartbeat();
QByteArray readHaptics();
QByteArray setHaptics(bool on);
QByteArray readSleep();
QByteArray setSleep(int seconds);
QByteArray readFirmware();
QByteArray readSerial();

// Control event name used by the CLI and IPC, e.g. "key1 press".
QString describe(const Message &message);

} // namespace Protocol
