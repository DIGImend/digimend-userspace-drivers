#include "config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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

#define LIBUSB_FAILURE(_fmt, _args...) \
    GENERIC_FAILURE(_fmt ": %s", ##_args, libusb_strerror(err))

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

#define LIBUSB_FAILURE_CLEANUP(_fmt, _args...) \
    do {                                        \
        LIBUSB_FAILURE(_fmt, ##_args);          \
        goto cleanup;                           \
    } while (0)

#define LIBUSB_GUARD(_expr, _fmt, _args...) \
    do {                                            \
        err = _expr;                                \
        if (err != LIBUSB_SUCCESS)                  \
            LIBUSB_FAILURE_CLEANUP(_fmt, ##_args);  \
    } while (0)


static void LIBUSB_CALL
interrupt_transfer_cb(struct libusb_transfer *transfer)
{
    enum libusb_error err;
    int idx;

    assert(transfer != NULL);

    switch (transfer->status)
    {
        case LIBUSB_TRANSFER_COMPLETED:
            /* Dump the result */
            for (idx = 0; idx < transfer->actual_length; idx++) {
                fprintf(stderr, "%s%02hhx", (idx == 0 ? "" : " "),
                        transfer->buffer[idx]);
            }
            fprintf(stderr, "\n");
            /* Resubmit the transfer */
            err = libusb_submit_transfer(transfer);
            if (err != LIBUSB_SUCCESS) {
                LIBUSB_FAILURE("resubmit a transfer");
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
        LIBUSB_FAILURE_CLEANUP("retrieve device list");
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
            LIBUSB_FAILURE_CLEANUP("detach kernel driver from interface #0");
        }

        /* Detach interface 1 */
        err = libusb_detach_kernel_driver(handle, 1);
        if (err == LIBUSB_SUCCESS) {
            iface1_detached = true;
        } else if (err != LIBUSB_ERROR_NOT_FOUND) {
            LIBUSB_FAILURE_CLEANUP("detach kernel driver from interface #1");
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
            LIBUSB_FAILURE_CLEANUP("get configuration string descriptor");
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
            LIBUSB_FAILURE_CLEANUP("set report protocol on interface 0");
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
            LIBUSB_FAILURE_CLEANUP("set report protocol on interface 1");
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
            LIBUSB_FAILURE_CLEANUP("set infinite idle on interface 0");
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
            LIBUSB_FAILURE_CLEANUP("set infinite idle on interface 1");
        }

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
                                       NULL,
                                       /* Timeout */
                                       0);
        fprintf(stderr, "Starting transfers!\n");
        /* Submit first transfer */
        LIBUSB_GUARD(libusb_submit_transfer(transfer),
                     "submit a transfer");

        /* Run transfers */
        while (true) {
            err = libusb_handle_events(ctx);
            if (err != LIBUSB_SUCCESS && err != LIBUSB_ERROR_INTERRUPTED)
                LIBUSB_FAILURE_CLEANUP("handle transfer events");
        }
    }

    result = 0;
cleanup:

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
