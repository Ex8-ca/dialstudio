#include "testmain.h"

#include <QSet>
#include <QSignalSpy>

#include "inputstate.h"

namespace {
struct Fired { QString control, slot; int steps; };
}

class InputStateTests : public QObject {
    Q_OBJECT

    QSet<QString> bound;
    QList<Fired> fired;
    QStringList raw;

    InputState *make(const QSet<QString> &bindings, const QString &accel = "off", int longMs = 120, int coalesceMs = 40) {
        bound = bindings;
        fired.clear();
        raw.clear();
        auto *s = new InputState(this);
        InputState::Settings settings;
        settings.longPressMs = longMs;
        settings.coalesceMs = coalesceMs;
        settings.acceleration = accel;
        s->setSettings(settings);
        s->setBindingQuery([this](const QString &c, const QString &slot) { return bound.contains(c + "/" + slot); });
        connect(s, &InputState::fire, this, [this](const QString &c, const QString &slot, int n) { fired.append({c, slot, n}); });
        connect(s, &InputState::rawEvent, this, [this](const QString &t) { raw << t; });
        return s;
    }

private slots:
    void plainPressFiresImmediately() {
        std::unique_ptr<InputState> s(make({"key1/press"}));
        s->keyDown(1);
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].control, QString("key1"));
        QCOMPARE(fired[0].slot, QString("press"));
        s->keyUp(1);
        QCOMPARE(raw, (QStringList{"key1 press", "key1 release"}));
    }

    void shortHoldWithLongPressBindingFiresPressOnRelease() {
        std::unique_ptr<InputState> s(make({"key2/press", "key2/long_press"}));
        s->keyDown(2);
        QVERIFY(fired.isEmpty());          // must wait: it may become a long press
        QTest::qWait(30);
        s->keyUp(2);
        QCOMPARE(fired.size(), 1);         // press fires on release; the unbound release slot stays silent
        QCOMPARE(fired[0].slot, QString("press"));
    }

    void longHoldFiresLongPressNotPress() {
        std::unique_ptr<InputState> s(make({"key2/press", "key2/long_press"}));
        s->keyDown(2);
        QTest::qWait(220);
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].slot, QString("long_press"));
        QVERIFY(raw.contains("key2 long_press"));
        s->keyUp(2);
        for (const auto &f : fired) QVERIFY(f.slot != "press");
    }

    void chordTracksHeldKeysInOrder() {
        std::unique_ptr<InputState> s(make({}));
        s->keyDown(1);
        s->keyDown(3);
        s->keyDown(7);
        QCOMPARE(s->heldKeys(), (QList<int>{1, 3, 7}));
        s->keyUp(3);
        QCOMPARE(s->heldKeys(), (QList<int>{1, 7}));
        s->keyDown(1);                      // duplicate down is ignored
        QCOMPARE(s->heldKeys(), (QList<int>{1, 7}));
    }

    void allSevenKeysHeldAtOnce() {
        std::unique_ptr<InputState> s(make({}));
        for (int k = 1; k <= 7; ++k) s->keyDown(k);
        QCOMPARE(s->heldKeys().size(), 7);
        for (int k = 7; k >= 1; --k) s->keyUp(k);
        QVERIFY(s->heldKeys().isEmpty());
    }

    void knobClickFires() {
        std::unique_ptr<InputState> s(make({"knob/click"}));
        s->knobClick();
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].control, QString("knob"));
        QCOMPARE(fired[0].slot, QString("click"));
        QCOMPARE(raw, (QStringList{"knob click"}));
    }

    void rotationIsCoalescedIntoOneFire() {
        std::unique_ptr<InputState> s(make({"knob/rotate_cw"}, "off", 500, 40));
        for (int i = 0; i < 3; ++i) { s->rotate(+1, false); QTest::qWait(5); }
        QVERIFY(fired.isEmpty());
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].slot, QString("rotate_cw"));
        QCOMPARE(fired[0].steps, 3);
        QCOMPARE(raw, (QStringList{"rotate cw steps=3"}));
    }

    void slowDetentsAreSeparateFires() {
        std::unique_ptr<InputState> s(make({"knob/rotate_ccw"}, "off", 500, 20));
        s->rotate(-1, false);
        QTest::qWait(80);
        s->rotate(-1, false);
        QTest::qWait(80);
        QCOMPARE(fired.size(), 2);
        QCOMPARE(fired[0].steps, 1);
        QCOMPARE(fired[1].steps, 1);
    }

    void directionChangeFlushesFirst() {
        std::unique_ptr<InputState> s(make({"knob/rotate_cw", "knob/rotate_ccw"}));
        s->rotate(+1, false);
        s->rotate(+1, false);
        s->rotate(-1, false);
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 2, 500);
        QCOMPARE(fired[0].slot, QString("rotate_cw"));
        QCOMPARE(fired[0].steps, 2);
        QCOMPARE(fired[1].slot, QString("rotate_ccw"));
    }

    void accelerationBoostsFastSpinningOnly() {
        QCOMPARE(InputState::accelerate("off", 4, true), 4);
        QCOMPARE(InputState::accelerate("mild", 1, true), 1);
        QCOMPARE(InputState::accelerate("mild", 4, false), 4);
        QCOMPARE(InputState::accelerate("mild", 4, true), 6);
        QCOMPARE(InputState::accelerate("strong", 4, true), 10);
        std::unique_ptr<InputState> s(make({"knob/rotate_cw"}, "mild"));
        for (int i = 0; i < 4; ++i) s->rotate(+1, false);       // back to back = fast
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].steps, 6);
        QCOMPARE(raw.last(), QString("rotate cw steps=4"));      // raw count is not boosted
    }

    void pressRotateUsesItsOwnBinding() {
        std::unique_ptr<InputState> s(make({"knob/rotate_cw", "knob/press_rotate_cw"}));
        s->rotate(+1, true);
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].slot, QString("press_rotate_cw"));
        QCOMPARE(raw[0], QString("press_rotate cw steps=1"));
    }

    void pressRotateFallsThroughToPlainRotate() {
        std::unique_ptr<InputState> s(make({"knob/rotate_ccw"}));
        s->rotate(-1, true);
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].slot, QString("rotate_ccw"));
    }

    void pressRotateWithNoBindingAtAllDoesNothing() {
        std::unique_ptr<InputState> s(make({}));
        s->rotate(+1, true);
        QTest::qWait(100);
        QVERIFY(fired.isEmpty());
        QCOMPARE(raw.size(), 1);         // still visible in `events`
    }

    void modifierRotateTakesPrecedenceAndCancelsThePress() {
        std::unique_ptr<InputState> s(make({"key4/press", "knob/rotate_cw", "knob/rotate_cw@key4"}));
        s->keyDown(4);
        QVERIFY(fired.isEmpty());        // press is deferred: key4 may be a modifier
        s->rotate(+1, false);
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].slot, QString("rotate_cw@key4"));
        s->keyUp(4);
        for (const auto &f : fired) QVERIFY2(f.slot != "press", "press must be cancelled after use as modifier");
    }

    void modifierWithoutRotationStillPresses() {
        std::unique_ptr<InputState> s(make({"key4/press", "knob/rotate_cw@key4"}));
        s->keyDown(4);
        s->keyUp(4);
        QCOMPARE(fired.size(), 1);
        QCOMPARE(fired[0].slot, QString("press"));
    }

    void unrelatedHeldKeyDoesNotChangeRotation() {
        std::unique_ptr<InputState> s(make({"knob/rotate_cw", "knob/rotate_cw@key4"}));
        s->keyDown(2);
        s->rotate(+1, false);
        QTRY_COMPARE_WITH_TIMEOUT(fired.size(), 1, 500);
        QCOMPARE(fired[0].slot, QString("rotate_cw"));
    }

    void resetForgetsHeldKeysAndPendingRotation() {
        std::unique_ptr<InputState> s(make({"knob/rotate_cw", "key1/long_press"}));
        s->keyDown(1);
        s->rotate(+1, false);
        s->reset();
        QVERIFY(s->heldKeys().isEmpty());
        QTest::qWait(200);
        QVERIFY(fired.isEmpty());        // neither the long press nor the rotation fires
        s->keyUp(1);                     // a stale release after reconnect is ignored
        QCOMPARE(raw, (QStringList{"key1 press"}));
    }
};

REGISTER_TEST(InputStateTests)
#include "tests_inputstate.moc"
