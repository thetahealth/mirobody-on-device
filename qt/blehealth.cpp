#include "blehealth.hpp"

#include "apiclient.hpp"
#include "blehealth_codec.hpp"

#include <QBluetoothDeviceDiscoveryAgent>
#include <QLowEnergyController>
#include <QLowEnergyService>
#include <QLowEnergyCharacteristic>
#include <QLowEnergyDescriptor>
#include <QVariant>
#include <QVariantMap>

#if QT_CONFIG(permissions)
#include <QGuiApplication>
#include <QPermissions>
#endif

namespace {

// The pure GATT decoding lives in blehealth_codec.* (Qt-Core-only, unit-tested);
// this file only bridges Qt Bluetooth's QBluetoothUuid to the codec's 16-bit ids.
quint16 uuid16(const QBluetoothUuid& uuid) {
    bool ok = false;
    const quint16 u = uuid.toUInt16(&ok);
    return ok ? u : 0;
}

} // namespace

using mirobody::ble::isSupportedService;
using mirobody::ble::isSupportedMeasurement;

//------------------------------------------------------------------------------

BleHealth::BleHealth(ApiClient* api, QObject* parent)
    : QObject(parent), api_(api) {}

BleHealth::~BleHealth() { teardown(); }

void BleHealth::setStatus(const QString& s) {
    if (status_ == s) return;
    status_ = s;
    emit statusChanged();
}
void BleHealth::setScanning(bool s) {
    if (scanning_ == s) return;
    scanning_ = s;
    emit scanningChanged();
}
void BleHealth::setConnected(bool c) {
    if (connected_ == c) return;
    connected_ = c;
    emit connectedChanged();
}

void BleHealth::startScan() {
#if QT_CONFIG(permissions)
    // macOS (and Qt 6.6+ elsewhere) gate Bluetooth behind a runtime permission.
    QBluetoothPermission perm;
    perm.setCommunicationModes(QBluetoothPermission::Access);
    switch (qApp->checkPermission(perm)) {
        case Qt::PermissionStatus::Undetermined:
            qApp->requestPermission(perm, this, [this](const QPermission&) { startScan(); });
            return;
        case Qt::PermissionStatus::Denied:
            setStatus(QStringLiteral("Bluetooth permission denied"));
            return;
        case Qt::PermissionStatus::Granted:
            break;
    }
#endif
    if (scanning_) return;
    deviceList_.clear();
    found_.clear();
    emit devicesChanged();

    if (!agent_) {
        agent_ = new QBluetoothDeviceDiscoveryAgent(this);
        agent_->setLowEnergyDiscoveryTimeout(8000);
        connect(agent_, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
                this, &BleHealth::onDeviceDiscovered);
        connect(agent_, &QBluetoothDeviceDiscoveryAgent::finished,
                this, &BleHealth::onScanFinished);
        connect(agent_, &QBluetoothDeviceDiscoveryAgent::errorOccurred, this,
                [this](QBluetoothDeviceDiscoveryAgent::Error) {
                    setStatus(agent_->errorString());
                    setScanning(false);
                });
    }
    setStatus(QStringLiteral("Scanning…"));
    setScanning(true);
    agent_->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void BleHealth::stopScan() {
    if (agent_) agent_->stop();
    setScanning(false);
}

void BleHealth::onDeviceDiscovered(const QBluetoothDeviceInfo& info) {
    if (!(info.coreConfigurations() & QBluetoothDeviceInfo::LowEnergyCoreConfiguration))
        return;

    // A sensor is interesting if it advertises a supported service, or (fallback,
    // since adverts often omit UUIDs) if it simply has a name.
    bool supported = false;
    for (const QBluetoothUuid& u : info.serviceUuids()) {
        if (isSupportedService(uuid16(u))) { supported = true; break; }
    }
    if (!supported && info.name().isEmpty()) return;

    // Core Bluetooth (macOS) hides the MAC and exposes an opaque device UUID; fall
    // back to it so every platform has a stable id.
    const QString id = info.address().isNull() ? info.deviceUuid().toString()
                                               : info.address().toString();
    for (const QBluetoothDeviceInfo& seen : found_)
        if (seen.deviceUuid() == info.deviceUuid() && seen.address() == info.address())
            return;                          // already listed

    found_.append(info);
    deviceList_.append(QVariantMap{
        {QStringLiteral("name"), info.name().isEmpty() ? id : info.name()},
        {QStringLiteral("id"), id},
        {QStringLiteral("supported"), supported}});
    emit devicesChanged();
}

void BleHealth::onScanFinished() {
    setScanning(false);
    setStatus(deviceList_.isEmpty() ? QStringLiteral("No devices found")
                                    : QStringLiteral("Scan complete"));
}

void BleHealth::connectDevice(int index) {
    if (index < 0 || index >= found_.size()) return;
    teardown();

    controller_ = QLowEnergyController::createCentral(found_.at(index), this);
    connect(controller_, &QLowEnergyController::connected, this, [this]() {
        setStatus(QStringLiteral("Discovering services…"));
        controller_->discoverServices();
    });
    connect(controller_, &QLowEnergyController::disconnected, this, [this]() {
        setConnected(false);
        setStatus(QStringLiteral("Disconnected"));
    });
    connect(controller_, &QLowEnergyController::discoveryFinished,
            this, &BleHealth::onDiscoveryFinished);
    connect(controller_, &QLowEnergyController::errorOccurred, this,
            [this](QLowEnergyController::Error) {
                setStatus(controller_ ? controller_->errorString()
                                      : QStringLiteral("Connection error"));
            });

    setStatus(QStringLiteral("Connecting…"));
    controller_->connectToDevice();
}

void BleHealth::onDiscoveryFinished() {
    bool any = false;
    const QList<QBluetoothUuid> uuids = controller_->services();
    for (const QBluetoothUuid& uuid : uuids) {
        if (isSupportedService(uuid16(uuid))) { any = true; configureService(uuid); }
    }
    if (!any)
        setStatus(QStringLiteral("No supported health service on this device"));
    else
        setConnected(true);
}

void BleHealth::configureService(const QBluetoothUuid& uuid) {
    QLowEnergyService* service = controller_->createServiceObject(uuid, this);
    if (!service) return;
    services_.append(service);
    connect(service, &QLowEnergyService::stateChanged, this,
            [this, service](QLowEnergyService::ServiceState state) {
                if (state == QLowEnergyService::RemoteServiceDiscovered)
                    subscribe(service);
            });
    connect(service, &QLowEnergyService::characteristicChanged, this,
            [this](const QLowEnergyCharacteristic& c, const QByteArray& value) {
                onCharacteristicChanged(c.uuid(), value);
            });
    service->discoverDetails();
}

void BleHealth::subscribe(QLowEnergyService* service) {
    for (const QLowEnergyCharacteristic& c : service->characteristics()) {
        if (!isSupportedMeasurement(uuid16(c.uuid()))) continue;
        // Enable the CCCD: Notify (0x0100) for streaming chars (heart rate),
        // Indicate (0x0200) for one-shot chars (blood pressure, thermometer).
        const QLowEnergyDescriptor cccd =
            c.descriptor(QBluetoothUuid::DescriptorType::ClientCharacteristicConfiguration);
        if (!cccd.isValid()) continue;
        const QByteArray on = (c.properties() & QLowEnergyCharacteristic::Notify)
                                  ? QByteArray::fromHex("0100")
                                  : QByteArray::fromHex("0200");
        service->writeDescriptor(cccd, on);
        setStatus(QStringLiteral("Streaming readings…"));
    }
}

void BleHealth::onCharacteristicChanged(const QBluetoothUuid& charUuid, const QByteArray& value) {
    for (const mirobody::ble::Reading& r : mirobody::ble::decode(uuid16(charUuid), value)) {
        emit reading(r.label, r.value, r.unit);
        if (!r.observation.isEmpty()) post(r.observation);
    }
}

void BleHealth::post(const QByteArray& obs) {
    if (!api_) return;
    api_->postRaw(QStringLiteral("/fhir/Observation"),
                  QByteArrayLiteral("application/fhir+json"), obs,
                  [this](bool ok, int) {
                      if (ok) ++posted_; else ++failed_;
                      emit countsChanged();
                  });
}

void BleHealth::disconnectDevice() {
    teardown();
    setConnected(false);
    setStatus(QStringLiteral("Disconnected"));
}

void BleHealth::teardown() {
    qDeleteAll(services_);
    services_.clear();
    if (controller_) {
        controller_->disconnectFromDevice();
        controller_->deleteLater();
        controller_ = nullptr;
    }
}
