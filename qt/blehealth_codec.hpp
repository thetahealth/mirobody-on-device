#pragma once

// Pure GATT / IEEE-11073 measurement decoding for the BLE health path, split out of
// blehealth.cpp so it is unit-testable without Qt Bluetooth (it depends only on Qt
// Core: QByteArray / QJson / QDateTime). `BleHealth` feeds raw notification bytes to
// `decode()` and POSTs each Reading's `observation`. The Android `GattHealthCodec` and
// iOS `GattHealthCodec` are the siblings of this file; keep the three in step.

#include <QByteArray>
#include <QString>

#include <vector>

namespace mirobody { namespace ble {

// Standard SIG 16-bit UUIDs (see src/health/README.md's BLE GATT table).
constexpr quint16 kSvcHeartRate   = 0x180D;
constexpr quint16 kSvcBloodPress  = 0x1810;
constexpr quint16 kSvcThermometer = 0x1809;

constexpr quint16 kChrHeartRate   = 0x2A37;  // Heart Rate Measurement
constexpr quint16 kChrBloodPress  = 0x2A35;  // Blood Pressure Measurement
constexpr quint16 kChrTemperature = 0x2A1C;  // Temperature Measurement

inline bool isSupportedService(quint16 u) {
    return u == kSvcHeartRate || u == kSvcBloodPress || u == kSvcThermometer;
}
inline bool isSupportedMeasurement(quint16 u) {
    return u == kChrHeartRate || u == kChrBloodPress || u == kChrTemperature;
}

// A decoded reading: `value` in `unit` for the UI, plus the FHIR Observation body to
// POST (empty => nothing to persist).
struct Reading {
    QString    label;
    double     value;
    QString    unit;
    QByteArray observation;
};

// Decode one measurement notification (`charUuid` is the 16-bit characteristic id)
// into zero or more readings. Unknown characteristics / malformed payloads yield {}.
std::vector<Reading> decode(quint16 charUuid, const QByteArray& value);

}} // namespace mirobody::ble
