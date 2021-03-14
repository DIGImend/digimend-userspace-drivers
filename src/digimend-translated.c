#include "config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/uinput.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpacked"
#include <libusb.h>
#pragma GCC diagnostic pop

/* Define LIBUSB_CALL for libusb <= 1.0.8 */
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif

#define GENERIC_ERROR(_fmt, _args...) \
    fprintf(stderr, _fmt "\n", ##_args)

#define GENERIC_FAILURE(_fmt, _args...) \
    GENERIC_ERROR("Failed to " _fmt, ##_args)

#define LIBUSB_FAILURE(_err, _fmt, _args...) \
    GENERIC_FAILURE(_fmt ": %s", ##_args, libusb_strerror(_err))

#define LIBC_FAILURE(_errno, _fmt, _args...) \
    GENERIC_FAILURE(_fmt ": %s", ##_args, strerror(_errno))

#define ERROR_CLEANUP(_fmt, _args...) \
    do {                                \
        GENERIC_ERROR(_fmt, ##_args);   \
        goto cleanup;                   \
    } while (0)

#define FAILURE_CLEANUP(_fmt, _args...) \
    do {                                \
        GENERIC_FAILURE(_fmt, ##_args); \
        goto cleanup;                   \
    } while (0)

#define LIBUSB_FAILURE_CLEANUP(_err, _fmt, _args...) \
    do {                                                \
        LIBUSB_FAILURE(_err, _fmt, ##_args);            \
        goto cleanup;                                   \
    } while (0)

#define LIBC_FAILURE_CLEANUP(_errno, _fmt, _args...) \
    do {                                                \
        LIBC_FAILURE(_errno, _fmt, ##_args);            \
        goto cleanup;                                   \
    } while (0)

#define LIBUSB_GUARD(_expr, _fmt, _args...) \
    do {                                                    \
        enum libusb_error _err = _expr;                     \
        if (_err != LIBUSB_SUCCESS)                         \
            LIBUSB_FAILURE_CLEANUP(_err, _fmt, ##_args);    \
    } while (0)

#define LIBC_GUARD(_expr, _fmt, _args...) \
    do {                                                \
        int _rc = _expr;                                \
        if (_rc < 0)                                    \
            LIBC_FAILURE_CLEANUP(errno, _fmt, ##_args); \
    } while (0)


static void
send(int fd, uint16_t type, uint16_t code, int32_t value)
{
    struct input_event ev = {.type = type, .code = code, .value = value};
    if (write(fd, &ev, sizeof(ev)) < 0) {
        LIBC_FAILURE(errno, "write event");
    }
}

static void
translate(int fd, const uint8_t *buf, size_t len)
{
    if (len < 12) {
        return;
    }
    if (buf[0] != 8) {
        return;
    }
    if (buf[1] & 0x80) {
        send(fd, EV_ABS, ABS_X,
             (int32_t)buf[2] |
             ((int32_t)buf[3] << 8) |
             ((int32_t)buf[8] << 16));
        send(fd, EV_ABS, ABS_Y,
             (int32_t)buf[4] |
             ((int32_t)buf[5] << 8) |
             ((int32_t)buf[9] << 16));
        send(fd, EV_ABS, ABS_PRESSURE,
             (int32_t)buf[6] | ((int32_t)buf[7] << 8));
        send(fd, EV_ABS, ABS_TILT_X, (int8_t)buf[10]);
        send(fd, EV_ABS, ABS_TILT_Y, -(int8_t)buf[11]);
        send(fd, EV_KEY, BTN_TOOL_PEN, 1);
        send(fd, EV_KEY, BTN_TOUCH, (buf[1] & 1) != 0);
        send(fd, EV_KEY, BTN_STYLUS, (buf[1] & 2) != 0);
        send(fd, EV_KEY, BTN_STYLUS2, (buf[1] & 4) != 0);
    } else {
        send(fd, EV_KEY, BTN_TOOL_PEN, 0);
    }
    send(fd, EV_MSC, MSC_SERIAL, 1098942556);
    send(fd, EV_SYN, SYN_REPORT, 1);
}

static void LIBUSB_CALL
interrupt_transfer_cb(struct libusb_transfer *transfer)
{
    enum libusb_error err;
    int uinput_fd;

    assert(transfer != NULL);
    assert(transfer->user_data != NULL);

    uinput_fd = *(const int *)transfer->user_data;

    switch (transfer->status)
    {
        case LIBUSB_TRANSFER_COMPLETED:
#if 0
            /* Dump the result */
            for (int idx = 0; idx < transfer->actual_length; idx++) {
                fprintf(stderr, "%s%02hhx", (idx == 0 ? "" : " "),
                        transfer->buffer[idx]);
            }
            fprintf(stderr, "\n");
#endif
            /* Translate */
            translate(uinput_fd, transfer->buffer, transfer->actual_length);
            /* Resubmit the transfer */
            err = libusb_submit_transfer(transfer);
            if (err != LIBUSB_SUCCESS) {
                LIBUSB_FAILURE(err, "resubmit a transfer");
            }
            break;

#define MAP(_name, _desc) \
    case LIBUSB_TRANSFER_##_name: \
        GENERIC_ERROR(_desc);  \
        break

        MAP(ERROR,      "Interrupt transfer failed");
        MAP(TIMED_OUT,  "Interrupt transfer timed out");
        MAP(STALL,      "Interrupt transfer halted (endpoint stalled)");
        MAP(NO_DEVICE,  "Device was disconnected");
        MAP(OVERFLOW,   "Interrupt transfer overflowed "
                        "(device sent more data than requested)");
#undef MAP

        case LIBUSB_TRANSFER_CANCELLED:
            break;
        default:
            GENERIC_ERROR("Unknown status of interrupt transfer: %d",
                          transfer->status);
            break;
    }
}


int
main(void)
{
    int result = 1;
    int rc;
    enum libusb_error err;
    libusb_context *ctx = NULL;
    ssize_t num;
    ssize_t idx;
    libusb_device **lusb_list = NULL;
    libusb_device *lusb_dev;
    struct libusb_device_descriptor desc;
    struct libusb_config_descriptor *config = NULL;
    libusb_device_handle *handle = NULL;
    bool iface0_detached = false;
    bool iface1_detached = false;
    bool iface0_claimed = false;
    bool iface1_claimed = false;
    struct libusb_transfer *transfer = NULL;
    uint8_t *buf = NULL;
    size_t len = 0;
    int uinput_fd = -1;
    struct uinput_abs_setup uinput_abs_setup;
    struct uinput_setup uinput_setup;
    bool device_created = false;

    /* Create libusb context */
    LIBUSB_GUARD(libusb_init(&ctx), "create libusb context");

    /* Set libusb debug level to informational only */
#ifdef HAVE_LIBUSB_SET_OPTION
    libusb_set_option(ctx, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
#else
    libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_INFO);
#endif

    /* Retrieve libusb device list */
    num = libusb_get_device_list(ctx, &lusb_list);
    if (num == LIBUSB_ERROR_NO_MEM) {
        LIBUSB_FAILURE_CLEANUP(num, "retrieve device list");
    }

    /* Find and open the devices */
    for (idx = 0; idx < num; idx++)
    {
        lusb_dev = lusb_list[idx];

        LIBUSB_GUARD(libusb_get_device_descriptor(lusb_dev, &desc),
                     "get device descriptor");
        if (desc.idVendor == 0x256c && desc.idProduct == 0x006d) {
            fprintf(stderr, "FOUND!\n");
            break;
        }
    }

    if (idx < num) {
        unsigned char data[64];
        /* Open the device */
        LIBUSB_GUARD(libusb_open(lusb_dev, &handle), "open the device");

        /* Detach interface 0 */
        err = libusb_detach_kernel_driver(handle, 0);
        if (err == LIBUSB_SUCCESS) {
            iface0_detached = true;
        } else if (err != LIBUSB_ERROR_NOT_FOUND) {
            LIBUSB_FAILURE_CLEANUP(err, "detach kernel driver from interface #0");
        }

        /* Detach interface 1 */
        err = libusb_detach_kernel_driver(handle, 1);
        if (err == LIBUSB_SUCCESS) {
            iface1_detached = true;
        } else if (err != LIBUSB_ERROR_NOT_FOUND) {
            LIBUSB_FAILURE_CLEANUP(err, "detach kernel driver from interface #1");
        }

        /* Claim interface 0 */
        LIBUSB_GUARD(
            libusb_claim_interface(handle, 0), "claim claim interface #0"
        );
        iface0_claimed = true;

        /* Claim interface 1 */
        LIBUSB_GUARD(
            libusb_claim_interface(handle, 1), "claim interface #1"
        );
        iface1_claimed = true;

        /* Get configuration string descriptor, enable proprietary mode */
        rc = libusb_get_string_descriptor(
            handle,
            /* descriptor index */
            0xc8,
            /* LANGID, English (United States) */
            0x0409,
            /* data from the descriptor */
            (unsigned char *)data,
            sizeof(data)
        );
        if (rc < 0) {
            LIBUSB_FAILURE_CLEANUP(err, "get configuration string descriptor");
        }
        fprintf(stderr, "Got %d configuration bytes:\n", rc);
        for (idx = 0; idx < rc; idx++) {
            fprintf(stderr, "%s%02hhx", (idx == 0 ? "" : " "), data[idx]);
        }
        fprintf(stderr, "\n");

        /* Set report protocol on interface 0 */
        rc = libusb_control_transfer(handle,
                                     /* host->device, class, interface */
                                     0x21,
                                     /* Set_Protocol */
                                     0x0B,
                                     /* 0 - boot, 1 - report */
                                     1,
                                     /* interface */
                                     0,
                                     /* buffer */
                                     NULL, 0,
                                     /* timeout */
                                     1000);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_PIPE) {
            LIBUSB_FAILURE_CLEANUP(err, "set report protocol on interface 0");
        }

        /* Set report protocol on interface 1 */
        rc = libusb_control_transfer(handle,
                                     /* host->device, class, interface */
                                     0x21,
                                     /* Set_Protocol */
                                     0x0B,
                                     /* 0 - boot, 1 - report */
                                     1,
                                     /* interface */
                                     1,
                                     /* buffer */
                                     NULL, 0,
                                     /* timeout */
                                     1000);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_PIPE) {
            LIBUSB_FAILURE_CLEANUP(err, "set report protocol on interface 1");
        }

        /* Set infinite idle duration on interface 0 */
        rc = libusb_control_transfer(handle,
                                     /* host->device, class, interface */
                                     0x21,
                                     /* Set_Idle */
                                     0x0A,
                                     /* duration for all report IDs */
                                     0 << 8,
                                     /* interface */
                                     0,
                                     /* buffer */
                                     NULL, 0,
                                     /* timeout */
                                     1000);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_PIPE) {
            LIBUSB_FAILURE_CLEANUP(err, "set infinite idle on interface 0");
        }

        /* Set infinite idle duration on interface 1 */
        rc = libusb_control_transfer(handle,
                                     /* host->device, class, interface */
                                     0x21,
                                     /* Set_Idle */
                                     0x0A,
                                     /* duration for all report IDs */
                                     0 << 8,
                                     /* interface */
                                     1,
                                     /* buffer */
                                     NULL, 0,
                                     /* timeout */
                                     1000);
        if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_PIPE) {
            LIBUSB_FAILURE_CLEANUP(err, "set infinite idle on interface 1");
        }

        /* Open uinput */
        uinput_fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinput_fd < 0) {
            LIBC_FAILURE_CLEANUP(errno, "open /dev/uinput");
        }

#define SET_EVBIT(_bit_token) \
    LIBC_GUARD(ioctl(uinput_fd, UI_SET_EVBIT, _bit_token),  \
               "enable uinput %s", #_bit_token)
        SET_EVBIT(EV_SYN);
        SET_EVBIT(EV_KEY);
        SET_EVBIT(EV_REL);
        SET_EVBIT(EV_ABS);
        SET_EVBIT(EV_MSC);
#undef SET_EVBIT

#define SET_KEYBIT(_bit_token) \
    LIBC_GUARD(ioctl(uinput_fd, UI_SET_KEYBIT, _bit_token),  \
               "enable uinput %s", #_bit_token)
        SET_KEYBIT(BTN_LEFT);
        SET_KEYBIT(BTN_RIGHT);
        SET_KEYBIT(BTN_MIDDLE);
        SET_KEYBIT(BTN_SIDE);
        SET_KEYBIT(BTN_EXTRA);
        SET_KEYBIT(BTN_TOOL_PEN);
        SET_KEYBIT(BTN_TOOL_RUBBER);
        SET_KEYBIT(BTN_TOOL_BRUSH);
        SET_KEYBIT(BTN_TOOL_PENCIL);
        SET_KEYBIT(BTN_TOOL_AIRBRUSH);
        SET_KEYBIT(BTN_TOOL_MOUSE);
        SET_KEYBIT(BTN_TOOL_LENS);
        SET_KEYBIT(BTN_TOUCH);
        SET_KEYBIT(BTN_STYLUS);
        SET_KEYBIT(BTN_STYLUS2);
#undef SET_KEYBIT

#define SET_ABSBIT(_bit_token) \
    LIBC_GUARD(ioctl(uinput_fd, UI_SET_ABSBIT, _bit_token),  \
               "enable uinput %s", #_bit_token)
        SET_ABSBIT(ABS_X);
        SET_ABSBIT(ABS_Y);
        SET_ABSBIT(ABS_Z);
        SET_ABSBIT(ABS_RZ);
        SET_ABSBIT(ABS_THROTTLE);
        SET_ABSBIT(ABS_WHEEL);
        SET_ABSBIT(ABS_PRESSURE);
        SET_ABSBIT(ABS_DISTANCE);
        SET_ABSBIT(ABS_TILT_X);
        SET_ABSBIT(ABS_TILT_Y);
        SET_ABSBIT(ABS_MISC);
#undef SET_ABSBIT

#define SET_RELBIT(_bit_token) \
    LIBC_GUARD(ioctl(uinput_fd, UI_SET_RELBIT, _bit_token),  \
               "enable uinput %s", #_bit_token)
        SET_RELBIT(REL_WHEEL);
#undef SET_RELBIT

#define SET_MSCBIT(_bit_token) \
    LIBC_GUARD(ioctl(uinput_fd, UI_SET_MSCBIT, _bit_token),  \
               "enable uinput %s", #_bit_token)
        SET_MSCBIT(MSC_SERIAL);
#undef SET_MSCBIT

        /* Setup X axis */
        uinput_abs_setup = (struct uinput_abs_setup){
            .code = ABS_X,
            .absinfo = {
                .value = 0,
                .minimum = 0,
                .maximum = 50800,
                .resolution = 200,
            },
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_ABS_SETUP, &uinput_abs_setup),
                   "setup X axis");

        /* Setup Y axis */
        uinput_abs_setup = (struct uinput_abs_setup){
            .code = ABS_Y,
            .absinfo = {
                .value = 0,
                .minimum = 0,
                .maximum = 31750,
                .resolution = 200,
            },
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_ABS_SETUP, &uinput_abs_setup),
                   "setup Y axis");

        /* Setup pressure axis */
        uinput_abs_setup = (struct uinput_abs_setup){
            .code = ABS_PRESSURE,
            .absinfo = {
                .value = 0,
                .minimum = 0,
                .maximum = 8191,
            },
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_ABS_SETUP, &uinput_abs_setup),
                   "setup pressure axis");

        /* Setup tilt X axis */
        uinput_abs_setup = (struct uinput_abs_setup){
            .code = ABS_TILT_X,
            .absinfo = {
                .value = 0,
                .minimum = -60,
                .maximum = 60,
            },
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_ABS_SETUP, &uinput_abs_setup),
                   "setup tilt X axis");

        /* Setup tilt Y axis */
        uinput_abs_setup = (struct uinput_abs_setup){
            .code = ABS_TILT_Y,
            .absinfo = {
                .value = 0,
                .minimum = -60,
                .maximum = 60,
            },
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_ABS_SETUP, &uinput_abs_setup),
                   "setup tilt Y axis");

        /* Setup uinput device */
        /* Pose as 056a:0314 Wacom Co., Ltd PTH-451 [Intuos pro (S)] */
        uinput_setup = (struct uinput_setup){
            .id = {
                .bustype = BUS_USB,
                .vendor = 0x056a,
                .product = 0x0314,
                .version = 0x0110,
            },
            .name = "Wacom Intuos Pro S Pen",
        };
        LIBC_GUARD(ioctl(uinput_fd, UI_DEV_SETUP, &uinput_setup),
                   "setup uinput device");

        /* Create uinput device */
        LIBC_GUARD(ioctl(uinput_fd, UI_DEV_CREATE), "create uinput device");
        device_created = true;

        /* Allocate transfer buffer */
        len = 0x40;
        buf = malloc(len);
        if (len > 0 && buf == NULL) {
            FAILURE_CLEANUP("allocate interrupt transfer buffer");
        }

        /* Allocate interrupt transfer */
        transfer = libusb_alloc_transfer(0);
        if (transfer == NULL)
            FAILURE_CLEANUP("allocate a transfer");
        /* Initialize interrupt transfer */
        libusb_fill_interrupt_transfer(transfer,
                                       handle, 0x81,
                                       buf, len,
                                       interrupt_transfer_cb,
                                       /* Callback data */
                                       &uinput_fd,
                                       /* Timeout */
                                       0);

        /* Submit first transfer */
        fprintf(stderr, "Starting transfers!\n");
        LIBUSB_GUARD(libusb_submit_transfer(transfer),
                     "submit a transfer");

        /* Run transfers */
        while (true) {
            err = libusb_handle_events(ctx);
            if (err != LIBUSB_SUCCESS && err != LIBUSB_ERROR_INTERRUPTED)
                LIBUSB_FAILURE_CLEANUP(err, "handle transfer events");
        }
    }

    result = 0;
cleanup:

    if (device_created) {
        ioctl(uinput_fd, UI_DEV_DESTROY);
    }

    if (uinput_fd >= 0) {
        close(uinput_fd);
    }

    libusb_free_transfer(transfer);

    free(buf);

    if (iface1_claimed) {
        libusb_release_interface(handle, 1);
    }
    if (iface0_claimed) {
        libusb_release_interface(handle, 0);
    }
    if (iface1_detached) {
        libusb_attach_kernel_driver(handle, 1);
    }
    if (iface0_detached) {
        libusb_attach_kernel_driver(handle, 0);
    }
    /* Free the configuration descriptor */
    libusb_free_config_descriptor(config);

    /* Close the device */
    libusb_close(handle);

    /* Free the libusb device list along with devices */
    libusb_free_device_list(lusb_list, true);

    /* Destroy the libusb context */
    if (ctx != NULL)
        libusb_exit(ctx);

    return result;
}
