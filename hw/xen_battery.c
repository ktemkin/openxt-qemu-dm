/*
 * Battery management for OpenXT guests.
 *
 * Copyright (C) 2014 Citrix Systems Ltd
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifdef CONFIG_SYSLOG_LOGGING
# include "logging.h"
#endif
#include "hw/xen_battery.h"
#include "xen_backend.h"
#include "xen.h"
#include "pci/pci.h"

/* Uncomment the following line to have debug messages about
 * Battery Management */
/* #define XEN_BATTERY_DEBUG */

#ifdef XEN_BATTERY_DEBUG
# define XBM_DPRINTF(fmt, ...)                            \
    do {                                                  \
        fprintf(stderr, "[BATTERY][%s(%d)]: " fmt,        \
                __func__, __LINE__, ## __VA_ARGS__);      \
    } while (0)
#else
# define XBM_DPRINTF(fmt, ...)                            \
    { }
#endif

# define XBM_ERROR_MSG(fmt, ...)                          \
    do {                                                  \
        fprintf(stderr, "[BATTERY][ERROR][%s(%d)]: " fmt, \
                __func__, __LINE__, ## __VA_ARGS__);      \
    } while (0)

#define MAX_BATTERIES              4

#define BATTERY_PORT_1             0xb2
#define BATTERY_PORT_2             0x86
#define BATTERY_PORT_3             0xb4

#define BATTERY_OP_INIT            0x7b
#define BATTERY_OP_SET_INFO_TYPE   0x7c
#define BATTERY_OP_GET_DATA_LENGTH 0x79
#define BATTERY_OP_GET_DATA        0x7d

/* Describes the different type of MODE managed by this module */
enum xen_battery_mode {
    XEN_BATTERY_MODE_NONE = 0,
    XEN_BATTERY_MODE_PT,
    XEN_BATTERY_MODE_HVM
};

enum xen_battery_selector {
    XEN_BATTERY_TYPE_NONE = 0,
    XEN_BATTERY_TYPE_BIF,
    XEN_BATTERY_TYPE_BST,
    XEN_BATTERY_TYPE_PSR
};

/* From each battery, xenstore provides the Battery Status (_bst) and the
 * battery informatiom (_bif).
 *
 * TODO: _BIF is deprecated in ACPI 4.0: See ACPI spec (chap 10.2.2.1)
 * Include the _BIX. */
struct battery_buffer {
    char *_bst;           /* _BST */
    char *_bif;           /* _BIF */
    uint8_t port_b2_val;  /* Variable to manage BATTERY_PORT_1 */
    uint8_t port_86_val;  /* Variable to manage BATTERY_PORT_2 */
    uint8_t index;        /* Index inside the _BST or _BIF string */
    uint8_t bif_changed;
    /* selector to mark which buffer we should use */
    enum xen_battery_selector _selector;
};

struct xen_battery_manager {
    enum xen_battery_mode mode; /* /[...]/xen_extended_power_mgmt */
    uint8_t battery_present;    /* /pm/battery_present */
    uint8_t ac_adapter_present; /* /pm/ac_adapter */
    uint8_t lid_state;          /* /pm/lid_state */
    struct battery_buffer batteries[MAX_BATTERIES]; /* Battery array */
    uint8_t index;              /* battery selector */

    /* TODO: find a better way than putting a static size */
    MemoryRegion mr[3];         /* MemoryRegion to register IO ops */
};

/* --/ Options /------------------------------------------------------------ */
static enum xen_battery_options_type xen_battery_option = XEN_BATTERY_NONE;

void xen_battery_set_option(unsigned int const opt)
{
    switch (opt) {
    case XEN_BATTERY_XENSTORE:
        xen_battery_option = XEN_BATTERY_XENSTORE;
        break;
    case XEN_BATTERY_NONE:
    /* No battery emulation is the default value
     * Then... fallthrough */
    default:
        xen_battery_option = XEN_BATTERY_NONE;
    }
}

bool xen_battery_get_option(void)
{
    return !!xen_battery_option;
}

/* Read a string from the /pm/'key'
 * set the result in 'return_value'
 * retun 0 in success */
static int32_t xen_battery_pm_read_str(char const *key,
                                       char **return_value)
{
    char path[XEN_BUFSIZE];
    char *value = NULL;

    if (NULL == key || NULL == return_value) {
        XBM_DPRINTF("ERROR, argument couldn't be null\n");
        return -1;
    }

    if (0 > snprintf(path, sizeof(path), "/pm/%s", key)) {
        XBM_DPRINTF("ERROR, snprintf failed\n");
        return -1;
    }

    value = xs_read(xenstore, XBT_NULL, path, NULL);

    if (NULL == value) {
        XBM_DPRINTF("ERROR, unable to read the content of \"%s\"\n", path);
        return -1;
    }

    *return_value = value;

    return 0;
}

/* Read a signed integer from the /pm/'key'
 * set the result in 'return_value'
 * retun 0 in success */
static int32_t xen_battery_pm_read_int(char const *key,
                                       int32_t *return_value)
{
    char path[XEN_BUFSIZE];
    char *value = NULL;

    if ((NULL == key) || (NULL == return_value)) {
        XBM_DPRINTF("ERROR, argument couldn't be null\n");
        return -1;
    }

    if (0 > snprintf(path, sizeof(path), "/pm/%s", key)) {
        XBM_DPRINTF("ERROR, snprintf failed\n");
        return -1;
    }

    value = xs_read(xenstore, XBT_NULL, path, NULL);

    if (NULL == value) {
        XBM_DPRINTF("ERROR, unable to read the content of \"%s\"\n", path);
        return -1;
    }

    *return_value = strtoull(value, NULL, 10);

    free(value);

    return 0;
}

static int32_t
xen_battery_update_battery_present(struct xen_battery_manager *xbm)
{
    int32_t value;

    if (0 != xen_battery_pm_read_int("battery_present", &value)) {
        XBM_DPRINTF("ERROR, unable to update the battery present status\"\n");
        /* in error case, it's preferable to show the worst situation */
        xbm->battery_present = 0;
        return -1;
    }

    xbm->battery_present = value;

    return 0;
}

static int32_t xen_battery_update_ac_adapter(struct xen_battery_manager *xbm)
{
    int32_t value;

    if (0 != xen_battery_pm_read_int("ac_adapter", &value)) {
        XBM_DPRINTF("ERROR, unable to update the ac_adapter present status\n");
        /* in error case, it's preferable to show the worst situation */
        xbm->ac_adapter_present = 0;
        return -1;
    }

    xbm->ac_adapter_present = value;

    return 0;
}

static int32_t xen_battery_update_lid_state(struct xen_battery_manager *xbm)
{
    int32_t value;

    if (0 != xen_battery_pm_read_int("lid_state", &value)) {
        XBM_DPRINTF("ERROR, unable to update the lid_state status\"\n");
        /* in error case, it's preferable to show the worst situation */
        xbm->lid_state = 0;
        return -1;
    }

    xbm->lid_state = value;

    return 0;
}

static int32_t xen_battery_update_bst(struct battery_buffer *battery,
                                      int32_t battery_num)
{
    char *value = NULL;
    char *old_value = NULL;
    char key[6];
    int32_t rc;

    old_value = battery->_bst;

    if (battery_num <= 0) {
        rc = xen_battery_pm_read_str("bst", &value);
    } else {
        memset(key, 0, sizeof(key));

        if (0 > snprintf(key, sizeof(key) - 1, "bst%d", battery_num)) {
            XBM_DPRINTF("ERROR, snprintf failed\n");
            return -1;
        }

        rc = xen_battery_pm_read_str(key, &value);
    }

    if (0 != rc) {
        XBM_DPRINTF("ERROR, unable to read the content of \"/pm/bst%d\"\n",
                    battery_num);
        /* TODO: determine what could be the best way to do that */
        battery->_bst = old_value;
        if (NULL != value) {
            free(value);
        }
        return -1;
    }

    battery->_bst = value;

    if (NULL != old_value) {
        free(old_value);
    }
    return 0;
}


static int32_t xen_battery_update_bif(struct battery_buffer *battery,
                                      int32_t battery_num)
{
    char *value = NULL;
    char *old_value = NULL;
    char key[6];
    int32_t rc;

    old_value = battery->_bif;

    if (battery_num <= 0) {
        rc = xen_battery_pm_read_str("bif", &value);
    } else {
        memset(key, 0, sizeof(key));

        if (0 > snprintf(key, sizeof(key) - 1, "bif%d", battery_num)) {
            XBM_DPRINTF("ERROR, snprintf failed\n");
            return -1;
        }

        rc = xen_battery_pm_read_str(key, &value);
    }

    if (0 != rc) {
        XBM_DPRINTF("ERROR, unable to read the content of \"/pm/bif%d\"\n",
                    battery_num);
        /* TODO: determine what could be the best way to do that */
        battery->_bif = old_value;
        if (NULL != value) {
            free(value);
        }
        return -1;
    }

    if ((NULL != old_value) && (NULL != value) &&
        (strncmp(old_value, value, 70) != 0)) {
        battery->bif_changed = 1;
    }

    battery->_bif = value;
    if (NULL != old_value) {
        free(old_value);
    }
    return 0;
}

static int32_t xen_battery_update_status_info(struct xen_battery_manager *xbm)
{
    int32_t index;

    if (NULL == xbm) {
        XBM_DPRINTF("ERROR, argument couldn't be null\n");
        return -1;
    }

    for (index = 0; index < MAX_BATTERIES; index++) {
        xen_battery_update_bif(&(xbm->batteries[index]), index);
        xen_battery_update_bst(&(xbm->batteries[index]), index);
    }

    return 0;
}

/* This function initializes the mode of the power management. */
static int32_t xen_battery_init_mode(struct xen_battery_manager *xbm)
{
    char dompath[XEN_BUFSIZE];
    char *value = NULL;

    if (NULL == xbm) {
        XBM_DPRINTF("ERROR, argument couldn't be null\n");
        return -1;
    }

    /* xen_extended_power_mgmt xenstore entry indicates whether or not extended
     * power management support is requested for the hvm guest.  Extended power
     * management support includes power management support beyond S3, S4, S5.
     * A value of 1 indicates pass-through pm support where upon pm resources
     * are mapped to the guest as appropriate where as a value of 2 as set in
     * non pass-through mode, requires qemu to take the onus of responding to
     * relevant pm port reads/writes. */
    if (0 > snprintf(dompath, sizeof(dompath),
                     "/local/domain/0/device-model/%d/xen_extended_power_mgmt",
                     xen_domid)) {
        XBM_DPRINTF("ERROR, snprintf failed\n");
        return -1;
    }

    value = xs_read(xenstore, XBT_NULL, dompath, NULL);

    if (NULL == value) {
        XBM_DPRINTF("ERROR, unable to read the content of \"%s\"\n", dompath);
        return -1;
    }

    xbm->mode = strtoull(value, NULL, 10);

    free(value);

    return 0;
}

/* -------/ IO /------------------------------------------------------------
 * IO handlers */

static void battery_port_1_write_op_init(struct battery_buffer *bb)
{
    if (NULL != bb->_bif) {
        free(bb->_bif);
        bb->_bif = NULL;
    }
    if (NULL != bb->_bst) {
        free(bb->_bst);
        bb->_bst = NULL;
    }

    bb->_selector = XEN_BATTERY_TYPE_NONE;
    bb->index = 0;
}

static int battery_port_1_write_op_set_type(struct battery_buffer *bb,
                                            struct xen_battery_manager *xbm)
{
    int ret = 0;

    if (XEN_BATTERY_TYPE_NONE == bb->_selector) {
        switch (bb->port_86_val) {
        case XEN_BATTERY_TYPE_BIF:
            bb->_selector = XEN_BATTERY_TYPE_BIF;
            xen_battery_update_bif(bb, xbm->index);
            XBM_DPRINTF("BATTERY_OP_SET_INFO_TYPE (BIF)\n");
            break;
        case XEN_BATTERY_TYPE_BST:
            bb->_selector = XEN_BATTERY_TYPE_BST;
            xen_battery_update_bst(bb, xbm->index);
            XBM_DPRINTF("BATTERY_OP_SET_INFO_TYPE (BST)\n");
            break;
        case XEN_BATTERY_TYPE_PSR:
            bb->_selector = XEN_BATTERY_TYPE_PSR;
            xen_battery_update_ac_adapter(xbm);
            /* TODO: this operation shouldn't be here: 'GET_DATA' */
            bb->port_86_val = !!xbm->ac_adapter_present;
            XBM_DPRINTF("BATTERY_OP_SET_INFO_TYPE (PSR)\n");
            break;
        case XEN_BATTERY_TYPE_NONE:
            /* NO BREAK HERE: fallthrough */
        default:
            XBM_DPRINTF("ERROR, unknown type :%d\n", bb->port_86_val);
            ret = -1;
        }
    }

    return ret;
}

static void battery_port_1_write(void *opaque, hwaddr addr,
                                 uint64_t val, uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;
    struct battery_buffer *bb;
    char *data = NULL;
    char buf[3];

    bb = &(xbm->batteries[xbm->index]);

    switch (val) {
    case BATTERY_OP_INIT:
    {
        battery_port_1_write_op_init(bb);
        XBM_DPRINTF("BATTERY_OP_INIT\n");
        break;
    }
    case BATTERY_OP_SET_INFO_TYPE:
    {
        battery_port_1_write_op_set_type(bb, xbm);
        break;
    }
    case BATTERY_OP_GET_DATA_LENGTH:
    {
        if (XEN_BATTERY_TYPE_PSR == bb->_selector) {
            /* TODO: return the length 1 ? and implment the GET_DATA
             *       --> Need to update Hvmloader */
            XBM_DPRINTF("BATTERY_OP_GET_DATA_LENGTH (PSR)\n");
            break;
        }
    /* NO BREAK HERE: fallthrough */
    }
    case BATTERY_OP_GET_DATA:
    {
        XBM_DPRINTF("BATTERY_OP_GET_DATA\n");
        if (XEN_BATTERY_TYPE_BST == bb->_selector) {
            data = bb->_bst;
        } else if (XEN_BATTERY_TYPE_BIF == bb->_selector) {
            data = bb->_bif;
        } else {
            break;
        }
        data += bb->index;
        if ((bb->index <= 74) ||
            ((bb->index > 74) && ((*(data - 1)) == '\n'))) {
            snprintf(buf, sizeof(buf), "%s", data);
            bb->port_86_val = (uint8_t)strtoull(buf, NULL, 0x10);
            bb->index += 2;
        } else {
            if (*data == '\n') {
                bb->port_86_val = 0;
            } else {
                bb->port_86_val = *data;
            }
            bb->index++;
        }
        break;
    }
    default:
        XBM_DPRINTF("Unknown cmd: %llu", val);
        break;
    }

    bb->port_b2_val = 0;
}

static uint64_t battery_port_1_read(void *opaque, hwaddr addr, uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;
    XBM_DPRINTF("port_b2 == 0x%02x\n", xbm->batteries[xbm->index].port_b2_val);
    return xbm->batteries[xbm->index].port_b2_val;
}

struct MemoryRegionOps port_1_ops = {
    .read = battery_port_1_read,
    .write = battery_port_1_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
    .valid = {
        .min_access_size = 1,
        .max_access_size = 2,
    },
};

static void battery_port_2_write(void *opaque,
                                 hwaddr addr,
                                 uint64_t val,
                                 uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;
    xbm->batteries[xbm->index].port_86_val = val;
    XBM_DPRINTF("port_86 := 0x%x\n", xbm->batteries[xbm->index].port_86_val);
}

static uint64_t battery_port_2_read(void *opaque, hwaddr addr, uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;
    XBM_DPRINTF("port_86 == 0x%x\n", xbm->batteries[xbm->index].port_86_val);
    return xbm->batteries[xbm->index].port_86_val;
}

struct MemoryRegionOps port_2_ops = {
    .read = battery_port_2_read,
    .write = battery_port_2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* ------/ PORT 3: What's up ? function /----------------------------------- */

static uint64_t battery_port_3_read(void *opaque, hwaddr addr, uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;
    uint64_t system_state = 0x0000000000000000ULL;

    xen_battery_update_battery_present(xbm);

    xen_battery_update_bif(&(xbm->batteries[xbm->index]), xbm->index);

    if (NULL != xbm->batteries[xbm->index]._bif) {
        system_state |= 0x1F;
    }

    if (1 == xbm->batteries[xbm->index].bif_changed) {
        xbm->batteries[xbm->index].bif_changed = 0;
        system_state |= 0x80;
    }

    XBM_DPRINTF("system_state == 0x%02llx\n", system_state);
    return system_state;
}

static void battery_port_3_write(void *opaque, hwaddr addr,
                                 uint64_t val, uint32_t size)
{
    struct xen_battery_manager *xbm = opaque;

    XBM_DPRINTF("opaque(%p) addr(0x%x) val(%llu) size(%u)\n",
                opaque, (uint32_t)addr, val, size);

    if ((val > 0) && (val <= MAX_BATTERIES)) {
        xbm->index = ((uint8_t)val) - 1;
        XBM_DPRINTF("Current battery is %u\n", xbm->index);
    }
}

struct MemoryRegionOps port_3_ops = {
    .read = battery_port_3_read,
    .write = battery_port_3_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 1,
    },
};

/* TODO: The MemoryRegion field isn't dynamically allocated find a way to do
 * the same by keeping it sexy */
struct {
    struct MemoryRegionOps const *ops;
    hwaddr base;
    char const *name;
    uint64_t size;
} opsTab[] = {
    { .ops = &port_1_ops,
      .base = BATTERY_PORT_1,
      .name = "acpi-xbm1",
      .size = 2, },
    { .ops = &port_2_ops,
      .base = BATTERY_PORT_2,
      .name = "acpi-xbm2",
      .size = 2, },
    { .ops = &port_3_ops,
      .base = BATTERY_PORT_3,
      .name = "acpi-xbm3",
      .size = 2, },
    /* /!\ END OF ARRAY */
    { .ops = NULL, .base =  0, .name = NULL, },
};

/* -------/ Initialization /------------------------------------------------ */

/* TODO: check error code
 * TODO: release memory region when qemu is leaving */
static int xen_battery_register_port(struct xen_battery_manager *xbm,
                                     MemoryRegion *parent)
{
    int index;

    for (index = 0; (NULL != opsTab[index].name); index++) {
        memory_region_init_io(&xbm->mr[index], opsTab[index].ops,
                              xbm, opsTab[index].name, opsTab[index].size);
        memory_region_add_subregion(parent, opsTab[index].base,
                                    &xbm->mr[index]);
    }

    return 0;
}

/* Main battery management function
 *
 * TODO: free this allocation
 * TODO: manage PVM */
int32_t xen_battery_init(PCIDevice *device)
{
    uint32_t i;
    struct xen_battery_manager *xbm = NULL;

    xbm = g_malloc0(sizeof(struct xen_battery_manager));
    memset(xbm, 0, sizeof(struct xen_battery_manager));
    for (i = 0; i < MAX_BATTERIES; i++) {
        xbm->batteries[i].bif_changed = 1;
    }

    if (NULL == xbm) {
        XBM_DPRINTF("Unable to initialize a battery_manager\n");
        return -1;
    }

    if (0 != xen_battery_init_mode(xbm)) {
        goto error_init;
    }

    if (0 != xen_battery_update_ac_adapter(xbm)) {
        goto error_init;
    }

    if (0 != xen_battery_update_battery_present(xbm)) {
        goto error_init;
    }

    /* TODO: Is it necessary to failed the battery initalization on LID
     *       error ? */
    xen_battery_update_lid_state(xbm);

    if (0 != xen_battery_update_status_info(xbm)) {
        goto error_init;
    }

    switch (xbm->mode) {
    case XEN_BATTERY_MODE_HVM:
        XBM_DPRINTF("non PT mode\n");
        xen_battery_register_port(xbm, pci_address_space_io(device));
        break;
    case XEN_BATTERY_MODE_PT:
        XBM_ERROR_MSG("TODO, mode Pass Through unsupported\n");
        goto error_init;
        break;
    case XEN_BATTERY_MODE_NONE:
    default:
        XBM_ERROR_MSG("mode (0x%02x) unsupported\n", xbm->mode);
        goto error_init;
    }

    fprintf(stdout, "Battery initialized\n");

    return 0;
error_init:
    free(xbm);
    XBM_ERROR_MSG("unable to initialize the battery emulation\n");
    return -1;
}
