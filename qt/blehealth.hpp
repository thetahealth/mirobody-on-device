#pragma once

// Direct Bluetooth Low Energy (BLE GATT) health-sensor ingestion for the desktop
// client -- the desktop counterpart of the on-device HealthKit / Health Connect
// readers in the mobile apps. It scans for BLE sensors that expose a STANDARD SIG
// GATT health service, subscribes to the measurement characteristic, decodes each
// reading per the GATT / IEEE-11073 spec, maps it to a FHIR R4 Observation, and
// POSTs it to the server's /fhir/Observation endpoint -- the same ingestion path
// the phones use. The server stays untouched: BLE is a pure client concern,
// because only a machine physically next to the sensor has a radio.
//
// Only standard-profile devices are readable this way (HR straps, BP cuffs,
// thermometers, ...); consumer watches/rings use proprietary/encrypted GATT and
// stay on the cloud vendor clients in src/health/vendor/device. See
// src/health/README.md ("Direct Bluetooth devices").
//
// Qt Bluetooth backs QLowEnergyController with WinRT / Core Bluetooth / BlueZ, so
// this one C++ file covers Windows, macOS, and Linux. Supported today:
//   Heart Rate (0x180D)      -> Heart Rate Measurement   0x2A37
//   Blood Pressure (0x1810)  -> Blood Pressure Measurement 0x2A35
//   Health Thermometer (0x1809) -> Temperature Measurement 0x2A1C
// Adding a service is one row in the decode dispatch in blehealth.cpp.

#include <QObject>
#include <QVariantList>
#include <QString>
#include <QList>
#include <QBluetoothDeviceInfo>
#include <QBluetoothUuid>

#include <functional>

class ApiClient;
class QBluetoothDeviceDiscoveryAgent;
class QLowEnergyController;
class QLowEnergyService;
class QByteArray;

class BleHealth : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY connectedChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY devicesChanged)
    Q_PROPERTY(int posted READ posted NOTIFY countsChanged)
    Q_PROPERTY(int failed READ failed NOTIFY countsChanged)
public:
    explicit BleHealth(ApiClient* api, QObject* parent = nullptr);
    ~BleHealth() override;

    bool         scanning() const { return scanning_; }
    bool         connected() const { return connected_; }
    QString      status() const { return status_; }
    QVariantList devices() const { return deviceList_; }
    int          posted() const { return posted_; }
    int          failed() const { return failed_; }

    // Scan (~8 s) for BLE devices advertising a supported health service, or with
    // a name (health sensors often omit the service UUID from the advert). Fills
    // `devices` as [{name, id, supported}].
    Q_INVOKABLE void startScan();
    Q_INVOKABLE void stopScan();
    // Connect to the discovered device at `index`, discover its services, and
    // subscribe to every supported measurement characteristic it exposes.
    Q_INVOKABLE void connectDevice(int index);
    Q_INVOKABLE void disconnectDevice();

signals:
    void scanningChanged();
    void connectedChanged();
    void statusChanged();
    void devicesChanged();
    void countsChanged();
    // A decoded live reading, for an on-screen readout (`value` already in `unit`).
    void reading(const QString& label, double value, const QString& unit);

private:
    void setStatus(const QString& s);
    void setScanning(bool s);
    void setConnected(bool c);
    void onDeviceDiscovered(const QBluetoothDeviceInfo& info);
    void onScanFinished();
    void onDiscoveryFinished();
    void configureService(const QBluetoothUuid& uuid);
    void subscribe(QLowEnergyService* service);
    void onCharacteristicChanged(const QBluetoothUuid& charUuid, const QByteArray& value);
    void post(const QByteArray& observation);
    void teardown();

    ApiClient*                       api_ = nullptr;
    QBluetoothDeviceDiscoveryAgent*  agent_ = nullptr;
    QLowEnergyController*            controller_ = nullptr;
    QList<QLowEnergyService*>        services_;
    QList<QBluetoothDeviceInfo>      found_;   // index-aligned with deviceList_

    QVariantList deviceList_;
    QString      status_;
    bool         scanning_  = false;
    bool         connected_ = false;
    int          posted_ = 0;
    int          failed_ = 0;
};
