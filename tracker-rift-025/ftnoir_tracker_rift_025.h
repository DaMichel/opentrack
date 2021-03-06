#pragma once
#include "ui_ftnoir_rift_clientcontrols_025.h"
#include "api/plugin-api.hpp"
#include "options/options.hpp"
#include <OVR.h>
#include <cmath>
#include <memory>
#include <QMessageBox>
#include <QWaitCondition>
using namespace options;

struct settings : opts {
    value<bool> useYawSpring;
    value<double> constant_drift, persistence, deadzone;
    settings() :
        opts("Rift-025"),
        useYawSpring(b, "yaw-spring", false),
        constant_drift(b, "constant-drift", 0.000005),
        persistence(b, "persistence", 0.99999),
        deadzone(b, "deadzone", 0.02)
    {}
};

class Rift_Tracker : public ITracker
{
public:
    Rift_Tracker();
    virtual ~Rift_Tracker() override;
    void start_tracker(QFrame *) override;
    void data(double *data) override;
private:
    double old_yaw;
    settings s;
    static bool isInitialised;
    OVR::DeviceManager* pManager;
    OVR::SensorDevice* pSensor;
    OVR::SensorFusion* pSFusion;
};

class TrackerControls: public ITrackerDialog
{
    Q_OBJECT
public:
    TrackerControls();

    void register_tracker(ITracker *) {}
    void unregister_tracker() {}

private:
    Ui::UIRiftControls ui;
    settings s;
private slots:
    void doOK();
    void doCancel();
};

class FTNoIR_TrackerDll : public Metadata
{
public:
    QString name() { return QString("Oculus Rift runtime 0.2.5 -- HMD"); }
    QIcon icon() { return QIcon(":/images/rift_tiny.png"); }
};

