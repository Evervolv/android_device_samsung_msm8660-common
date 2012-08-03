/*
 * Copyright (C) 2008 The Android Open Source Project
 * Copyright (C) 2011 <kang@insecure.ws>
 * Copyright (C) 2012 The CyanogenMod Project
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

#define LOG_NDEBUG 0
#define LOG_TAG "lights"
#include <cutils/log.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <hardware/lights.h>

static pthread_once_t g_init = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_notification_blink_support = 0;
static int g_notification_blink_rate_support = 0;
static int g_enable_touchlight = -1;

static char const LCD_FILE[]      = "/sys/class/leds/lcd-backlight/brightness";
static char const BUTTONS_FILE[]  = "/sys/class/misc/melfas_touchkey/brightness";
static char const BUTTONS_POWER[] = "/sys/class/misc/melfas_touchkey/enable_disable";
static char const NOTIFICATION_FILE[] = "/sys/class/misc/backlightnotification/notification_led";
static char const NOTIFICATION_BLINK_FILE[]    = "/sys/class/misc/backlightnotification/blink_control";
static char const NOTIFICATION_BLINK_RATE_FILE[] = "/sys/class/misc/backlightnotification/blink_interval";

void init_globals(void)
{
    pthread_mutex_init(&g_lock, NULL);

    g_notification_blink_support = (access(NOTIFICATION_BLINK_FILE, W_OK) == 0) ? 1 : 0;

    g_notification_blink_rate_support = (access(NOTIFICATION_BLINK_RATE_FILE, W_OK) == 0) ? 1 : 0;
}

static int write_int(char const *path, int value)
{
    int fd;
    static int already_warned = 0;

    ALOGD("write_int: path=\"%s\", value=\"%d\".", path, value);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

void load_settings()
{
    FILE* fp = fopen("/data/.disable_touchlight", "r");
    if (!fp) {
        g_enable_touchlight = 1;
    } else {
        g_enable_touchlight = fgetc(fp) == '1' ? 0 : 1;
        fclose(fp);
    }
}

static int write_str(char const *path, char const *str)
{
    int fd;
    static int already_warned = 0;

    ALOGD("write_str: path=\"%s\", str=\"%s\".", path, str);
    fd = open(path, O_RDWR);

    if (fd >= 0) {
        int amt = write(fd, str, strlen(str));
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("write_int failed to open %s\n", path);
            already_warned = 1;
        }
        return -errno;
    }
}

/* Should check for snprintf truncation, but as these functions only use
 * internal paths, meh. */
static int write_df_int(char const *dir, char const *file, int value)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    return write_int(path, value);
}

static int write_df_str(char const *dir, char const *file, char const *str)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, file);
    return write_str(path, str);
}

static int rgb_to_brightness(struct light_state_t const *state)
{
    int color = state->color & 0x00ffffff;

    return ((77*((color>>16) & 0x00ff))
        + (150*((color>>8) & 0x00ff)) + (29*(color & 0x00ff))) >> 8;
}

static int set_light_battery(struct light_device_t* dev,
            struct light_state_t const* state)
{
    return 0;
}

static int set_light_notifications(struct light_device_t* dev,
            struct light_state_t const* state)
{
       int bln_led_control = state->color & 0x00ffffff ? 1 : 0;
       int res;

       ALOGD("set_light_notification: color=%#010x, klc=%u, flash=%d/%d", state->color,
            bln_led_control, state->flashOnMS, state->flashOffMS);

       pthread_mutex_lock(&g_lock);
       res = write_int(NOTIFICATION_FILE, bln_led_control);

       if (g_notification_blink_support && bln_led_control && state->flashMode) {
           if (g_notification_blink_rate_support) {
               char buffer[10];
               snprintf(buffer, sizeof(buffer), "%d %d", state->flashOnMS, state->flashOffMS);
               res = write_str(NOTIFICATION_BLINK_RATE_FILE, buffer);
           }
           res = write_int(NOTIFICATION_BLINK_FILE, bln_led_control);
       }

       pthread_mutex_unlock(&g_lock);

       return res;
}

static int set_light_backlight(struct light_device_t *dev,
            struct light_state_t const *state)
{
        load_settings();
    int err = 0;
    int brightness = rgb_to_brightness(state);
    ALOGE("DAF set_light_backlight g_enable_touchlight = %d brightness = 0x%08x", g_enable_touchlight, brightness);

    pthread_mutex_lock(&g_lock);
    err = write_int(LCD_FILE, brightness);
    pthread_mutex_unlock(&g_lock);

    return err;
}

static int set_light_keyboard(struct light_device_t *dev,
            struct light_state_t const *state)
{
    return 0;
}

static int set_light_buttons(struct light_device_t *dev,
            struct light_state_t const *state)
{
    int touch_led_control = state->color & 0x00ffffff ? 1 : 2;
    int res = 0;

    ALOGD("set_light_buttons: color=%#010x, tlc=%u.", state->color,
         touch_led_control);

    pthread_mutex_lock(&g_lock);
    if (g_enable_touchlight == -1 || g_enable_touchlight > 0)
        res = write_int(BUTTONS_FILE, touch_led_control);
    pthread_mutex_unlock(&g_lock);

    return res;
}

static int close_lights(struct light_device_t *dev)
{
    ALOGD("close_light is called");
    if (dev)
        free(dev);

    return 0;
}

static int open_lights(const struct hw_module_t *module, char const *name,
                        struct hw_device_t **device)
{
    int (*set_light)(struct light_device_t *dev,
        struct light_state_t const *state);

    ALOGD("open_lights: open with %s", name);

    if (0 == strcmp(LIGHT_ID_BACKLIGHT, name))
        set_light = set_light_backlight;
    else if (0 == strcmp(LIGHT_ID_KEYBOARD, name))
        set_light = set_light_keyboard;
    else if (0 == strcmp(LIGHT_ID_BUTTONS, name))
        set_light = set_light_buttons;
    else if (0 == strcmp(LIGHT_ID_BATTERY, name))
        set_light = set_light_battery;
    else if (0 == strcmp(LIGHT_ID_NOTIFICATIONS, name))
        set_light = set_light_notifications;
    else
        return -EINVAL;

    pthread_once(&g_init, init_globals);

    struct light_device_t *dev = malloc(sizeof(struct light_device_t));
    memset(dev, 0, sizeof(*dev));

    dev->common.tag = HARDWARE_DEVICE_TAG;
    dev->common.version = 0;
    dev->common.module = (struct hw_module_t *)module;
    dev->common.close = (int (*)(struct hw_device_t *))close_lights;
    dev->set_light = set_light;

    *device = (struct hw_device_t *)dev;

    return 0;
}

static struct hw_module_methods_t lights_module_methods = {
    .open =  open_lights,
};

struct hw_module_t HAL_MODULE_INFO_SYM = {
    .tag = HARDWARE_MODULE_TAG,
    .version_major = 1,
    .version_minor = 0,
    .id = LIGHTS_HARDWARE_MODULE_ID,
    .name = "lights Module",
    .author = "The CyanogenMod Project",
    .methods = &lights_module_methods,
};
