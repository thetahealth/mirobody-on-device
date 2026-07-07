// Unit tests for the Qt BLE GATT decoder (blehealth_codec.*) — the sibling of the
// Android GattHealthCodecTest and iOS GattHealthCodecTests, with the same hand-built
// GATT / IEEE-11073 payloads and expected LOINC/UCUM/values. No Bluetooth hardware or
// Qt Bluetooth module needed: the codec depends only on Qt Core.

#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include "blehealth_codec.hpp"

using namespace mirobody::ble;

namespace {
QByteArray hb(std::initializer_list<int> bytes) {
    QByteArray a;
    for (int b : bytes) a.append(static_cast<char>(b));
    return a;
}
QJsonObject obsOf(const Reading& r) { return QJsonDocument::fromJson(r.observation).object(); }
QString loincOf(const Reading& r) {
    return obsOf(r).value("code").toObject().value("coding").toArray()
        .at(0).toObject().value("code").toString();
}
QString categoryOf(const Reading& r) {
    return obsOf(r).value("category").toArray().at(0).toObject().value("coding").toArray()
        .at(0).toObject().value("code").toString();
}
QJsonObject qtyOf(const Reading& r) { return obsOf(r).value("valueQuantity").toObject(); }
} // namespace

class BleCodecTest : public QObject {
    Q_OBJECT
private slots:
    void heartRateUint8() {
        const auto r = decode(kChrHeartRate, hb({0x00, 0x48}));   // flags=0, HR=72
        QCOMPARE(static_cast<int>(r.size()), 1);
        QCOMPARE(r[0].value, 72.0);
        QCOMPARE(r[0].unit, QStringLiteral("bpm"));
        QCOMPARE(loincOf(r[0]), QStringLiteral("8867-4"));
        QCOMPARE(categoryOf(r[0]), QStringLiteral("vital-signs"));
        QCOMPARE(qtyOf(r[0]).value("code").toString(), QStringLiteral("/min"));
        QCOMPARE(qtyOf(r[0]).value("value").toInt(), 72);        // integral
    }

    void heartRateUint16() {
        const auto r = decode(kChrHeartRate, hb({0x01, 0x40, 0x01}));  // flags=1, HR=320
        QCOMPARE(static_cast<int>(r.size()), 1);
        QCOMPARE(r[0].value, 320.0);
    }

    void heartRateTruncated() {
        QVERIFY(decode(kChrHeartRate, hb({0x00})).empty());
    }

    void bloodPressureMmHg() {
        // flags=0 (mmHg); systolic=120, diastolic=80, MAP=93 as SFLOAT (exp 0), LE.
        const auto r = decode(kChrBloodPress, hb({0x00, 0x78, 0x00, 0x50, 0x00, 0x5D, 0x00}));
        QCOMPARE(static_cast<int>(r.size()), 3);
        QCOMPARE(r[0].value, 120.0);
        QCOMPARE(loincOf(r[0]), QStringLiteral("8480-6"));       // systolic
        QCOMPARE(r[1].value, 80.0);
        QCOMPARE(loincOf(r[1]), QStringLiteral("8462-4"));       // diastolic
        QCOMPARE(r[2].value, 93.0);
        QCOMPARE(loincOf(r[2]), QStringLiteral("8478-0"));       // MAP
        QCOMPARE(r[0].unit, QStringLiteral("mmHg"));
        QCOMPARE(qtyOf(r[0]).value("code").toString(), QStringLiteral("mm[Hg]"));
    }

    void temperatureCelsius() {
        // flags=0 (Celsius); 36.5 = mantissa 365 (0x00016D), exponent -1 (0xFF): 6D 01 00 FF
        const auto r = decode(kChrTemperature, hb({0x00, 0x6D, 0x01, 0x00, 0xFF}));
        QCOMPARE(static_cast<int>(r.size()), 1);
        QCOMPARE(r[0].value, 36.5);
        QCOMPARE(r[0].unit, QStringLiteral("°C"));
        QCOMPARE(loincOf(r[0]), QStringLiteral("8310-5"));
        QCOMPARE(qtyOf(r[0]).value("code").toString(), QStringLiteral("Cel"));
        QCOMPARE(qtyOf(r[0]).value("value").toDouble(), 36.5);   // non-integral
    }

    void temperatureFahrenheit() {
        // flags=1 (Fahrenheit); 98 = mantissa 98 (0x000062), exponent 0: 62 00 00 00
        const auto r = decode(kChrTemperature, hb({0x01, 0x62, 0x00, 0x00, 0x00}));
        QCOMPARE(static_cast<int>(r.size()), 1);
        QCOMPARE(r[0].value, 98.0);
        QCOMPARE(r[0].unit, QStringLiteral("°F"));
        QCOMPARE(qtyOf(r[0]).value("code").toString(), QStringLiteral("[degF]"));
    }

    void unsupportedCharacteristic() {
        QVERIFY(decode(0x2902, hb({0x00, 0x01})).empty());       // CCCD, not a measurement
    }
};

QTEST_MAIN(BleCodecTest)
#include "blehealth_codec_test.moc"
