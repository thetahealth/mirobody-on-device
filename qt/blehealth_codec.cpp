#include "blehealth_codec.hpp"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cmath>

namespace mirobody { namespace ble {
namespace {

QString nowIso() {
    // FHIR effectiveDateTime as an ISO-8601 UTC instant. Most measurement chars carry
    // no timestamp, so stamp the receive time (the mobile readers do the same).
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

// IEEE-11073 16-bit SFLOAT (blood pressure); 4-bit signed exponent + 12-bit signed
// mantissa. Special-value codes map to NaN.
double sfloat(quint16 raw) {
    const int m = raw & 0x0FFF;
    if (m == 0x07FF || m == 0x0800 || m == 0x0801 || m == 0x0802 || m == 0x07FE)
        return std::nan("");                 // NaN / NRes / Reserved / +-INFINITY
    int mantissa = m;
    int exponent = raw >> 12;
    if (exponent >= 0x0008) exponent -= 16;  // signed 4-bit
    if (mantissa >= 0x0800) mantissa -= 4096; // signed 12-bit
    return mantissa * std::pow(10.0, exponent);
}

// IEEE-11073 32-bit FLOAT (temperature); 8-bit signed exponent + 24-bit signed mantissa.
double float32(quint32 raw) {
    qint32 mantissa = raw & 0x00FFFFFF;
    const qint8 exponent = static_cast<qint8>((raw >> 24) & 0xFF);
    if (mantissa >= 0x800000) mantissa -= 0x01000000; // signed 24-bit
    return mantissa * std::pow(10.0, static_cast<double>(exponent));
}

// Serialize one FHIR R4 Observation, matching the shape src/health/vendor_fhir.cpp
// writes (LOINC code, observation-category, UCUM valueQuantity). The server scopes the
// resource to the authenticated user, so no `subject` is needed.
QByteArray observation(const char* loinc, const char* display, const char* category,
                       double value, bool integral, const char* unit, const char* ucum) {
    QJsonObject cat{{QStringLiteral("coding"), QJsonArray{QJsonObject{
        {QStringLiteral("system"), QStringLiteral("http://terminology.hl7.org/CodeSystem/observation-category")},
        {QStringLiteral("code"),   QString::fromLatin1(category)}}}}};
    QJsonObject code{{QStringLiteral("coding"), QJsonArray{QJsonObject{
        {QStringLiteral("system"),  QStringLiteral("http://loinc.org")},
        {QStringLiteral("code"),    QString::fromLatin1(loinc)},
        {QStringLiteral("display"), QString::fromLatin1(display)}}}}};
    QJsonObject qty{
        {QStringLiteral("value"),  integral ? QJsonValue(static_cast<qint64>(llround(value)))
                                            : QJsonValue(value)},
        {QStringLiteral("unit"),   QString::fromLatin1(unit)},
        {QStringLiteral("system"), QStringLiteral("http://unitsofmeasure.org")},
        {QStringLiteral("code"),   QString::fromLatin1(ucum)}};
    QJsonObject obs{
        {QStringLiteral("resourceType"), QStringLiteral("Observation")},
        {QStringLiteral("status"),       QStringLiteral("final")},
        {QStringLiteral("category"),     QJsonArray{cat}},
        {QStringLiteral("code"),         code},
        {QStringLiteral("effectiveDateTime"), nowIso()},
        {QStringLiteral("valueQuantity"),     qty}};
    return QJsonDocument(obs).toJson(QJsonDocument::Compact);
}

const quint8* bytes(const QByteArray& v) {
    return reinterpret_cast<const quint8*>(v.constData());
}

// Heart Rate Measurement (0x2A37): flags byte, then HR as uint8 or uint16 (LE).
std::vector<Reading> decodeHeartRate(const QByteArray& v) {
    std::vector<Reading> out;
    if (v.size() < 2) return out;
    const quint8* d = bytes(v);
    double hr;
    if (d[0] & 0x01) {                       // bit0: 16-bit value format
        if (v.size() < 3) return out;
        hr = d[1] | (d[2] << 8);
    } else {
        hr = d[1];
    }
    out.push_back({QStringLiteral("Heart rate"), hr, QStringLiteral("bpm"),
                   observation("8867-4", "Heart rate", "vital-signs", hr, true,
                               "beats/minute", "/min")});
    return out;
}

// Blood Pressure Measurement (0x2A35): flags, then systolic/diastolic/MAP as SFLOAT.
std::vector<Reading> decodeBloodPressure(const QByteArray& v) {
    std::vector<Reading> out;
    if (v.size() < 7) return out;
    const quint8* d = bytes(v);
    const bool kpa = d[0] & 0x01;            // bit0: 0 = mmHg, 1 = kPa
    const char* unit = kpa ? "kPa" : "mmHg";
    const char* ucum = kpa ? "kPa" : "mm[Hg]";
    auto rd = [&](int off) { return sfloat(d[off] | (d[off + 1] << 8)); };
    const double sys = rd(1), dia = rd(3), map = rd(5);
    if (!std::isnan(sys))
        out.push_back({QStringLiteral("Systolic"), sys, QString::fromLatin1(unit),
                       observation("8480-6", "Systolic blood pressure", "vital-signs",
                                   sys, true, unit, ucum)});
    if (!std::isnan(dia))
        out.push_back({QStringLiteral("Diastolic"), dia, QString::fromLatin1(unit),
                       observation("8462-4", "Diastolic blood pressure", "vital-signs",
                                   dia, true, unit, ucum)});
    if (!std::isnan(map))
        out.push_back({QStringLiteral("Mean arterial"), map, QString::fromLatin1(unit),
                       observation("8478-0", "Mean blood pressure", "vital-signs",
                                   map, true, unit, ucum)});
    return out;
}

// Temperature Measurement (0x2A1C): flags, then temperature as 32-bit FLOAT.
std::vector<Reading> decodeTemperature(const QByteArray& v) {
    std::vector<Reading> out;
    if (v.size() < 5) return out;
    const quint8* d = bytes(v);
    const bool fahrenheit = d[0] & 0x01;     // bit0: 0 = Celsius, 1 = Fahrenheit
    const quint32 raw = static_cast<quint32>(d[1]) | (static_cast<quint32>(d[2]) << 8) |
                        (static_cast<quint32>(d[3]) << 16) | (static_cast<quint32>(d[4]) << 24);
    const double t = float32(raw);
    if (std::isnan(t)) return out;
    out.push_back({QStringLiteral("Temperature"), t,
                   fahrenheit ? QStringLiteral("°F") : QStringLiteral("°C"),
                   observation("8310-5", "Body temperature", "vital-signs", t, false,
                               fahrenheit ? "F" : "Cel", fahrenheit ? "[degF]" : "Cel")});
    return out;
}

} // namespace

std::vector<Reading> decode(quint16 charUuid, const QByteArray& v) {
    switch (charUuid) {
        case kChrHeartRate:   return decodeHeartRate(v);
        case kChrBloodPress:  return decodeBloodPressure(v);
        case kChrTemperature: return decodeTemperature(v);
        default:              return {};
    }
}

}} // namespace mirobody::ble
