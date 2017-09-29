/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define ALOG_TAG "Sensors"

#include <hardware/sensors.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <cstring>

#include <linux/input.h>

#include <utils/Atomic.h>
#include <utils/Log.h>

#include "sensors.h"

#include "LightSensor.h"
#include "ProximitySensor.h"
#include "AkmSensor.h"
#include "GyroSensor.h"

/*****************************************************************************/

#define DELAY_OUT_TIME 0x7FFFFFFF

#define LIGHT_SENSOR_POLLTIME    2000000000


#define SENSORS_ACCELERATION     (1<<ID_A)
#define SENSORS_MAGNETIC_FIELD   (1<<ID_M)
#define SENSORS_ORIENTATION      (1<<ID_O)
#define SENSORS_LIGHT            (1<<ID_L)
#define SENSORS_PROXIMITY        (1<<ID_P)
#define SENSORS_GYROSCOPE        (1<<ID_GY)

#define SENSORS_ACCELERATION_HANDLE       0
#define SENSORS_MAGNETIC_FIELD_HANDLE     1
#define SENSORS_ORIENTATION_HANDLE        2
#define SENSORS_LIGHT_HANDLE              3
#define SENSORS_PROXIMITY_HANDLE          4
#define SENSORS_GYROSCOPE_HANDLE          5
#define SENSORS_SIGNIFICANT_MOTION_HANDLE 6

#define AKM_FTRACE 0
#define AKM_DEBUG 0
#define AKM_DATA 0

/*****************************************************************************/

/* The SENSORS Module */
static const struct sensor_t sSensorList[] = {
        { "K3DH Acceleration Sensor",
          "STMicroelectronics",
          1, SENSORS_ACCELERATION_HANDLE,
          SENSOR_TYPE_ACCELEROMETER, RANGE_A, RESOLUTION_A, 0.25f, 15000, 0, 0,
          SENSOR_STRING_TYPE_ACCELEROMETER, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "AK8975 Magnetic field Sensor",
          "Asahi Kasei Microdevices",
          1, SENSORS_MAGNETIC_FIELD_HANDLE,
          SENSOR_TYPE_MAGNETIC_FIELD, 2000.0f, CONVERT_M, 6.0f, 30000, 0, 0,
          SENSOR_STRING_TYPE_MAGNETIC_FIELD, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "AK8975 Orientation Sensor",
          "Asahi Kasei Microdevices",
          1, SENSORS_ORIENTATION_HANDLE,
          SENSOR_TYPE_ORIENTATION, 360.0f, CONVERT_O, 7.8f, 30000, 0, 0,
          SENSOR_STRING_TYPE_ORIENTATION, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "GP2A Light Sensor",
          "Sharp",
          1, SENSORS_LIGHT_HANDLE,
          SENSOR_TYPE_LIGHT, 3000.0f, 1.0f, 0.75f, 0, 0, 0,
          SENSOR_STRING_TYPE_LIGHT, "", 0, SENSOR_FLAG_ON_CHANGE_MODE, { } },
        { "GP2A Proximity Sensor",
          "Sharp",
          1, SENSORS_PROXIMITY_HANDLE,
          SENSOR_TYPE_PROXIMITY, 5.0f, 5.0f, 0.75f, 0, 0, 0,
          SENSOR_STRING_TYPE_PROXIMITY, "", 0, SENSOR_FLAG_WAKE_UP | SENSOR_FLAG_ON_CHANGE_MODE, { } },
        { "K3G Gyroscope Sensor",
          "STMicroelectronics",
          1, SENSORS_GYROSCOPE_HANDLE,
          SENSOR_TYPE_GYROSCOPE, RANGE_GYRO, CONVERT_GYRO, 6.1f, 15000, 0, 0,
          SENSOR_STRING_TYPE_GYROSCOPE, "", 0, SENSOR_FLAG_CONTINUOUS_MODE, { } },
        { "Movement Detection Sensor",
          "STMicroelectronics",
          1, SENSORS_SIGNIFICANT_MOTION_HANDLE,
          SENSOR_TYPE_SIGNIFICANT_MOTION, 1.0f, 1.0f, 0.01f, 0, 0, 0,
          SENSOR_STRING_TYPE_SIGNIFICANT_MOTION, 0, 0, SENSOR_FLAG_ONE_SHOT_MODE | SENSOR_FLAG_WAKE_UP, { } },
};


static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device);


static int sensors__get_sensors_list(struct sensors_module_t* module,
                                     struct sensor_t const** list) 
{
        *list = sSensorList;
        return ARRAY_SIZE(sSensorList);
}

#if defined(SENSORS_DEVICE_API_VERSION_1_4)
static int sensors__set_operation_mode(unsigned int mode)
{
    (void)mode;
    return 0;
}
#endif

static struct hw_module_methods_t sensors_module_methods = {
        .open = open_sensors
};

struct sensors_module_t HAL_MODULE_INFO_SYM = {
        .common = {
             .tag= HARDWARE_MODULE_TAG,
             .module_api_version = 1,
             .hal_api_version = 0,
             .id = SENSORS_HARDWARE_MODULE_ID,
             .name = "Samsung Sensor module",
             .author = "Samsung Electronic Company",
             .methods = &sensors_module_methods,
        },
        .get_sensors_list = sensors__get_sensors_list,
#if defined(SENSORS_DEVICE_API_VERSION_1_4)
         set_operation_mode: sensors__set_operation_mode,
#endif
};

struct sensors_poll_context_t {
    struct sensors_poll_device_1 device; // must be first

    sensors_poll_context_t();
    //for cppcheck "noCopyConstructor"
    sensors_poll_context_t(const sensors_poll_context_t & other);
    ~sensors_poll_context_t();
    int activate(int handle, int enabled);
    int setDelay(int handle, int64_t ns);
    int pollEvents(sensors_event_t* data, int count);
    int batch(int handle, int flags, int64_t sampling_period_ns, int64_t max_report_latency_ns);
    int flush(int handle);
    int get_sensors_list(struct sensor_t const** list);
#if defined(SENSORS_DEVICE_API_VERSION_1_4)
    int inject_sensor_data(const sensors_event_t *data);
#endif

private:
    enum {
        light           = 0,
        proximity       = 1,
        akm             = 2,
        gyro            = 3,
        numSensorDrivers,
        numFds,
    };

    static const size_t wake = numFds - 1;
    static const char WAKE_MESSAGE = 'W';
    struct pollfd mPollFds[numFds];
    int mWritePipeFd;
    SensorBase* mSensors[numSensorDrivers];

    int handleToDriver(int handle) const {
        switch (handle) {
            case ID_A:
            case ID_M:
            case ID_O:
            case ID_SM:
                return akm;
            case ID_P:
                return proximity;
            case ID_L:
                return light;
            case ID_GY:
                return gyro;
        }
        return -EINVAL;
    }
};

/*****************************************************************************/

sensors_poll_context_t::sensors_poll_context_t()
{
    mSensors[light] = new LightSensor();
    mPollFds[light].fd = mSensors[light]->getFd();
    mPollFds[light].events = POLLIN;
    mPollFds[light].revents = 0;

    mSensors[proximity] = new ProximitySensor();
    mPollFds[proximity].fd = mSensors[proximity]->getFd();
    mPollFds[proximity].events = POLLIN;
    mPollFds[proximity].revents = 0;

    mSensors[akm] = new AkmSensor();
    mPollFds[akm].fd = mSensors[akm]->getFd();
    mPollFds[akm].events = POLLIN;
    mPollFds[akm].revents = 0;

    mSensors[gyro] = new GyroSensor();
    mPollFds[gyro].fd = mSensors[gyro]->getFd();
    mPollFds[gyro].events = POLLIN;
    mPollFds[gyro].revents = 0;

    int wakeFds[2];
    int result = pipe(wakeFds);
    ALOGE_IF(result<0, "error creating wake pipe (%s)", strerror(errno));
    fcntl(wakeFds[0], F_SETFL, O_NONBLOCK);
    fcntl(wakeFds[1], F_SETFL, O_NONBLOCK);
    mWritePipeFd = wakeFds[1];

    mPollFds[wake].fd = wakeFds[0];
    mPollFds[wake].events = POLLIN;
    mPollFds[wake].revents = 0;
}

sensors_poll_context_t::~sensors_poll_context_t() {
    for (int i=0 ; i<numSensorDrivers ; i++) {
        delete mSensors[i];
    }
    close(mPollFds[wake].fd);
    close(mWritePipeFd);
}

int sensors_poll_context_t::activate(int handle, int enabled) {
    int index = handleToDriver(handle);
    if (index < 0) return index;
    if (index == gyro && enabled == 0) {
        usleep(200*1000);
    }
    int err =  mSensors[index]->enable(handle, enabled);
    if (enabled && !err) {
        const char wakeMessage(WAKE_MESSAGE);
        int result = write(mWritePipeFd, &wakeMessage, 1);
        ALOGE_IF(result<0, "error sending wake message (%s)", strerror(errno));
    }
    return err;
}

int sensors_poll_context_t::setDelay(int handle, int64_t ns) {

    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->setDelay(handle, ns);
}

int sensors_poll_context_t::pollEvents(sensors_event_t* data, int count)
{
    int nbEvents = 0;
    int n = 0;

    do {
        // see if we have some leftover from the last poll()
        for (int i=0 ; count && i<numSensorDrivers ; i++) {
            SensorBase* const sensor(mSensors[i]);
            if ((mPollFds[i].revents & POLLIN) || (sensor->hasPendingEvents())) {
                int nb = sensor->readEvents(data, count);
                if (nb < count) {
                    // no more data for this sensor
                    mPollFds[i].revents = 0;
                }
                count -= nb;
                nbEvents += nb;
                data += nb;
            }
        }

        if (count) {
            // we still have some room, so try to see if we can get
            // some events immediately or just wait if we don't have
            // anything to return
            n = poll(mPollFds, numFds, nbEvents ? 0 : -1);
            if (n<0) {
                ALOGE("poll() failed (%s)", strerror(errno));
                return -errno;
            }
            if (mPollFds[wake].revents & POLLIN) {
                char msg;
                int result = read(mPollFds[wake].fd, &msg, 1);
                ALOGE_IF(result<0, "error reading from wake pipe (%s)", strerror(errno));
                ALOGE_IF(msg != WAKE_MESSAGE, "unknown message on wake queue (0x%02x)", int(msg));
                mPollFds[wake].revents = 0;
            }
        }
        // if we have events and space, go read them
    } while (n && count);

    return nbEvents;
}

int sensors_poll_context_t::batch(int handle, int flags, int64_t sampling_period_ns, int64_t max_report_latency_ns)
{
    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->batch(handle, flags, sampling_period_ns, max_report_latency_ns);
}

int sensors_poll_context_t::flush(int handle)
{
    int index = handleToDriver(handle);
    if (index < 0) return index;
    return mSensors[index]->flush(handle);
}

#if defined(SENSORS_DEVICE_API_VERSION_1_4)
int sensors_poll_context_t::inject_sensor_data(const sensors_event_t *data)
{
    return 0;
}
#endif

/*****************************************************************************/

static int poll__close(struct hw_device_t *dev)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    if (ctx) {
        delete ctx;
    }
    return 0;
}

static int poll__activate(struct sensors_poll_device_t *dev,
        int handle, int enabled) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->activate(handle, enabled);
}

static int poll__setDelay(struct sensors_poll_device_t *dev,
        int handle, int64_t ns) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->setDelay(handle, ns);
}

static int poll__poll(struct sensors_poll_device_t *dev,
        sensors_event_t* data, int count) {
    sensors_poll_context_t *ctx = (sensors_poll_context_t *)dev;
    return ctx->pollEvents(data, count);
}

static int poll__batch(struct sensors_poll_device_1 *dev,
        int handle, int flags, int64_t sampling_period_ns, int64_t max_report_latency_ns)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
    return ctx->batch(handle, flags, sampling_period_ns, max_report_latency_ns);
}

static int poll__flush(struct sensors_poll_device_1 *dev, int sensor_handle)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
    return ctx->flush(sensor_handle);
}

#if defined(SENSORS_DEVICE_API_VERSION_1_4)
static int poll__inject_sensor_data(struct sensors_poll_device_1 *dev, const sensors_event_t *data)
{
    sensors_poll_context_t *ctx = (sensors_poll_context_t *) dev;
    return ctx->inject_sensor_data(data);
}
#endif

/*****************************************************************************/

/** Open a new instance of a sensor device using name */
static int open_sensors(const struct hw_module_t* module, const char* id,
                        struct hw_device_t** device)
{
        int status = -EINVAL;
        sensors_poll_context_t *dev = new sensors_poll_context_t();

        memset(&dev->device, 0, sizeof(sensors_poll_device_1));

        dev->device.common.tag = HARDWARE_DEVICE_TAG;
        dev->device.common.version  = SENSORS_DEVICE_API_VERSION_1_3;
        dev->device.common.module   = const_cast<hw_module_t*>(module);
        dev->device.common.close    = poll__close;
        dev->device.activate        = poll__activate;
        dev->device.setDelay        = poll__setDelay;
        dev->device.poll            = poll__poll;
        dev->device.batch           = poll__batch;
        dev->device.flush           = poll__flush;
#if defined(SENSORS_DEVICE_API_VERSION_1_4)
        dev->device.inject_sensor_data = poll__inject_sensor_data;
#endif

        *device = &dev->device.common;
        status = 0;

        return status;
}

