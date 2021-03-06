/*
 * ITEmou.c: xen mouse pci card emulator
 */

#include "hw.h"
#include "exec/memory.h"
#include "ui/console.h"
#include "xenmou.h"
#include <linux/input.h>
#include "pci/pci.h"

//#define DEBUG_XENMOU

#ifdef DEBUG_XENMOU
# define DEBUG_MSG(fmt, ...)                            \
      do {                                              \
          fprintf(stdout, "[Xenmou][%s(%d)]: " fmt,     \
                  __func__, __LINE__, ## __VA_ARGS__ ); \
      } while (0)
#else
# define DEBUG_MSG(fmt, ...) \
      do {                   \
      } while (0)
#endif

#ifdef DEBUG_XENMOU
# define ERROR_MSG(fmt, ...)                              \
      do {                                                \
          fprintf(stderr, "[Xenmou][%s(%d)]: Error:" fmt, \
          __func__, __LINE__, ##  __VA_ARGS__ );          \
      } while (0)
#else
# define ERROR_MSG(fmt, ...)                                         \
      do {                                                           \
          fprintf(stderr, "[Xenmou]: Error:" fmt, ##  __VA_ARGS__ ); \
      } while (0)
#endif

#define EVENT_PAGES             2 // Number of pages of events
#define EVENT_REGION_SIZE       (TARGET_PAGE_SIZE * EVENT_PAGES)

// First page is used for register
#define XENMOU_EVENT_OFFSET     (TARGET_PAGE_SIZE)

// Number of events (first entry is for rptr and wtr)
#define XENMOU_EVENT_NUM \
  (((EVENT_PAGES * TARGET_PAGE_SIZE) / EVENT_N_BYTES) - 1)

#define DEVPROP_REGION_SIZE     (TARGET_PAGE_SIZE)
#define XENMOU_DEVPROP_OFFSET   (XENMOU_EVENT_OFFSET + EVENT_REGION_SIZE)

#define NEXT(a)                 (((a)+1) % XENMOU_EVENT_NUM)

#define MMIO_BAR_SIZE            0x4000
#if 0
#define MMIO_CPU_MAPPED_SIZE     0x1000

#define PCI_FREQUENCY            33000000L
#endif

#define MAXSLOTS        64
#define ABS_WORDS       2
#define REL_WORDS       1
#define KEY_WORDS       3

#define KEY_START       0x100

#define	EV_DEV          0x6
#define	DEV_SET		0x1
#define	DEV_CONF	0x2
#define	DEV_RESET	0x3

#define SLOT_NOT_SET    -2

/* Device Properties: this structure is available on the RAM for the Guest
 * to get the device property and information */
typedef struct {
   char name[40]; // TODO: Why does it need 40 bytes length ? instead of 26 ?
   uint32_t evbits;
   uint32_t absbits[ABS_WORDS];
   uint32_t relbits[REL_WORDS];
   uint32_t buttonbits[KEY_WORDS];
} device_property;

typedef struct {
    uint32_t flags_and_revision;
    uint32_t x_and_y;
} __attribute__((__packed__)) XenMouEvent;

typedef struct {
    uint16_t type;
    uint16_t code;
    uint32_t value;
} __attribute__((__packed__)) XenMouEventRecord;

typedef struct PCIXenMouState {
    PCIDevice pci_dev;
    MemoryRegion mmio_bar;
    MemoryRegion event_region;
    MemoryRegion devprop_region;

    uint32_t isr;

    int acceleration;
    int enable_device, enable_v2, enable_device_interrupts;

    uint32_t wptr;

    int last_buttons;
    int8_t num_dev;
    /**
     * Be carefull: dmbus slot is unsigned. Dunno why we use signed but
     * it seems XenMou use sometime negative
     */
    int8_t slot;
    int8_t bad_ver;
    QEMUPutMouseEntry *relative_handler;
    QEMUPutMouseEntry *absolute_handler;
} PCIXenMouState;

static void xenmou_push_config(PCIXenMouState *m);

static void xenmou_update_irq(PCIXenMouState *m)
{
    if (!m->enable_device_interrupts) {
        qemu_set_irq(m->pci_dev.irq[0], 0);
    } else {
        qemu_set_irq(m->pci_dev.irq[0], m->isr ? 1 : 0);
    }
}

static void xenmou_mmio_write8(void *opaque, hwaddr addr,
                               uint32_t val)
{
    DEBUG_MSG("mmio_write8(%p, 0x"TARGET_FMT_plx", 0x%x)\n",
	      opaque, addr, val);
    /* Only dwords */
}

static void xenmou_mmio_write16(void *opaque, hwaddr addr,
                                uint32_t val)
{
    DEBUG_MSG("mmio_write16(%p, 0x"TARGET_FMT_plx", 0x%x)\n",
	      opaque, addr, val);
    /* Only dwords */
}

static uint32_t *xenmou_get_rptr_guest(PCIXenMouState *xm)
{
    DEBUG_MSG("RAM addr rptr_guest = 0x"RAM_ADDR_FMT"\n",
              memory_region_get_ram_addr(&xm->event_region));

    return memory_region_get_ram_ptr(&xm->event_region);
}

static uint32_t *xenmou_get_wptr_guest(PCIXenMouState *xm)
{
    uint32_t *ptr;

    DEBUG_MSG("RAM addr wptr_guest = 0x"RAM_ADDR_FMT"\n",
              memory_region_get_ram_addr(&xm->event_region) + sizeof(uint32_t));
    ptr = memory_region_get_ram_ptr(&xm->event_region);

    return (ptr + 1);
}

static XenMouEvent *xenmou_get_event_queue(PCIXenMouState *xm)
{
    uint8_t *ptr;

    DEBUG_MSG("RAM addr event_queue = 0x"RAM_ADDR_FMT"\n",
              memory_region_get_ram_addr(&xm->event_region) + EVENT_N_BYTES);
    ptr = memory_region_get_ram_ptr(&xm->event_region);

    return (XenMouEvent *)(ptr + EVENT_N_BYTES);
}

static device_property *xenmou_get_devprop(PCIXenMouState *xm)
{
    DEBUG_MSG("RAM addr devprop = 0x"RAM_ADDR_FMT"\n",
              memory_region_get_ram_addr(&xm->devprop_region));

    return memory_region_get_ram_ptr(&xm->devprop_region);
}

static int xenmou_inject(PCIXenMouState *xm, int x, int y, uint32_t flags)
{
    XenMouEvent *ev;

    if (NEXT(xm->wptr) == *(xenmou_get_rptr_guest(xm))) {
        DEBUG_MSG("event received but ring full\n");
        return 1;
    }

    ev = &(xenmou_get_event_queue(xm))[xm->wptr];
    ev->x_and_y = x | (y << 16);
    ev->flags_and_revision = flags | (1 << 16);
    DEBUG_MSG("shipping(%d, %d, %04x)\n", x, y, flags);

    xm->wptr = NEXT(xm->wptr);
    *(xenmou_get_wptr_guest(xm)) = xm->wptr;

    return 1;
}

/* ***xenmou 2 ************************************************************* */

static void interrupt(PCIXenMouState *x)
{
    if (x->enable_device_interrupts) {
        x->isr |= XMOU_ISR_INT;
        xenmou_update_irq(x);
    }
}

static void xenmou_inject_record(PCIXenMouState *xm, uint16_t type,
                                 uint16_t code, int32_t value)
{
    XenMouEventRecord *rec = NULL;

    if (NEXT(xm->wptr) == *xenmou_get_rptr_guest(xm)) {
        DEBUG_MSG("event received but ring full\n");
        return;
    }

    rec = (XenMouEventRecord *)(&((xenmou_get_event_queue(xm))[xm->wptr]));

    rec->type = type;
    rec->code = code;
    rec->value = value;

    DEBUG_MSG("shipping(%x, %x, %x)\n", type, code, value);

    xm->wptr = NEXT(xm->wptr);
    *(xenmou_get_wptr_guest(xm)) = xm->wptr;

    return;
}

static void xenmou_direct_event_handler(void *opaque, uint16_t type,
                                        uint16_t code, int32_t value)
{
    PCIXenMouState *x = opaque;

    xenmou_inject_record(x, type, code, value);
    if (type == EV_SYN) {
        interrupt(x);
    }
}

/* ***end xenmou 2 ********************************************************* */

static void xenmou_event(void *opaque, int x, int y,
                         int z, int buttons_state, int absolute)
{
    PCIXenMouState *xm = opaque;
    int bdiff = xm->last_buttons;
    int schedule_irq = 0;

    buttons_state &=
      MOUSE_EVENT_LBUTTON | MOUSE_EVENT_RBUTTON | MOUSE_EVENT_MBUTTON;

    bdiff ^= buttons_state;

    if (bdiff & MOUSE_EVENT_LBUTTON) {
        schedule_irq += xenmou_inject(xm, 0, 0,
				      (buttons_state & MOUSE_EVENT_LBUTTON) ?
				      LEFT_BUTTON_DOW : LEFT_BUTTON_U);
    }

    if (bdiff & MOUSE_EVENT_MBUTTON) {
        schedule_irq += xenmou_inject(xm, 0, 0,
				      (buttons_state & MOUSE_EVENT_MBUTTON) ?
				      MIDDLE_BUTTON_DOW : MIDDLE_BUTTON_U);
    }

    if (bdiff & MOUSE_EVENT_RBUTTON) {
        schedule_irq += xenmou_inject(xm, 0, 0,
				      (buttons_state & MOUSE_EVENT_RBUTTON) ?
				      RIGHT_BUTTON_DOW : RIGHT_BUTTON_U);
    }
    xm->last_buttons=buttons_state;

    if (absolute) {
        x &=0x7fff;
        x <<= 1;

        y &= 0x7fff;
        y <<= 1;

        schedule_irq += xenmou_inject(xm, x, y, ABSOLUTE);
    } else {
        if (x || y) {
            schedule_irq += xenmou_inject(xm, x, y, RELATIVE);
        }
    }

    if (schedule_irq && xm->enable_device_interrupts) {
        xm->isr |= XMOU_ISR_INT;
        xenmou_update_irq(xm);
    }

    DEBUG_MSG("WRITE_PTR=%d READ_PTR=%d events_max=%d event_queue=%p "
	      "isr=%08x sched_irq=%d\n",
              xm->wptr, *(xenmou_get_rptr_guest(xm)),
              XENMOU_EVENT_NUM, xenmou_get_event_queue(xm),
              xm->isr, schedule_irq);
}

static void xenmou_abs_event(void *opaque, int x, int y,
                             int z, int buttons_state)
{
    xenmou_event(opaque, x, y, z, buttons_state, 1);
}

static void controlbits(PCIXenMouState *x, uint32_t val)
{
    int device_enabled = (x->enable_device);
    int abs_handled = ((!x->enable_v2) && device_enabled);

    x->enable_device = val & XMOU_CONTROL_XMOU_EN;
    x->enable_device_interrupts = val & XMOU_CONTROL_INT_EN;
    xenmou_update_irq(x);

    if (device_enabled == x->enable_device) {
        DEBUG_MSG("the device is already enable\n");
        return;
    }

    if (x->enable_device) {
        if (x->enable_v2) {
            DEBUG_MSG("direct event set up\n");
            xen_input_set_direct_event_handler(&xenmou_direct_event_handler, x);
            xenmou_push_config(x);
        } else {
            if (!abs_handled) {
                DEBUG_MSG("adding qemu mouse event handlers\n");
                x->absolute_handler =
                    qemu_add_mouse_event_handler(xenmou_abs_event,
                                                 x, 1, "Xen Mouse");
		/* When we add a mouse event handler, it is added at the TAIL
		 * of the list and will not be use. Then, to activate it, we
		 * use the following call which place it to the HEAD of the
		 * mouse event list */
                qemu_activate_mouse_event_handler(x->absolute_handler);
            }
            xen_input_set_direct_event_handler(NULL, x);
        }
    } else {
        DEBUG_MSG("disable device\n");
        xen_input_set_direct_event_handler(NULL, x);
        if (x->absolute_handler) {
            DEBUG_MSG("removing qemu mouse event handlers\n");
            qemu_remove_mouse_event_handler(x->absolute_handler);
            x->absolute_handler = NULL;
        }
    }

    xen_input_abs_enabled(x->enable_device);
}

static void xenmou_mmio_write32(void *opaque, hwaddr addr,
                                uint32_t val)
{
    PCIXenMouState *x = opaque;

    DEBUG_MSG("mmio_write32(%p, 0x"TARGET_FMT_plx", 0x%x)\n", x, addr, val);

    switch (addr & (TARGET_PAGE_MASK - 1)) {
    case XMOU_CONTROL:      /* 0x00100 */
        controlbits(x, val);
        break;
    case XMOU_ACCELERATION:	/* 0x0010C */
        x->acceleration = val;
        break;
    case XMOU_ISR:          /* 0x00110 */
        x->isr &= ~val;
        xenmou_update_irq(x);
        break;
    case XMOU_CLIENT_REV:   /* 0x00118 */
        if (!x->enable_device) {
            if (val == 2) {
                x->enable_v2 = val;
            } else {
                x->bad_ver = 1;
                x->enable_v2 = XENMOU_CURRENT_REV;
            }
        }
        break;
    default:
        ERROR_MSG("Unexpected Control value 0x%x write at 0x"TARGET_FMT_plx"\n",
                  val, (addr & TARGET_PAGE_MASK));
    }
}

static uint32_t xenmou_mmio_read32(void *opaque, hwaddr addr)
{
    PCIXenMouState *x = opaque;

    DEBUG_MSG("mmio_read32(%p, 0x"TARGET_FMT_plx")\n", opaque, addr);

    switch (addr & (TARGET_PAGE_SIZE - 1)) {
    case XMOU_MAGIC:             /* 0x00000 */
        return XMOU_MAGIC_VALUE; /* 0x584D4F55 */
    case XMOU_REV:               /* 0x00004 */
        return (x->enable_v2 ? XENMOU_CURRENT_REV : 1);
    case XMOU_CONTROL:           /* 0x00100 */
        return (x->enable_device_interrupts |
                x->enable_device | x->enable_device);
    case XMOU_EVENT_SIZE:        /* 0x00104 */
        return EVENT_N_BYTES;
    case XMOU_EVENT_NPAGES:      /* 0x00108 */
        return EVENT_PAGES;
    case XMOU_ACCELERATION:      /* 0x0010C */
        return 0xffffffff;
    case XMOU_ISR:               /* 0x00110 */
        return x->isr;
    case XMOU_CONF_SIZE:         /* 0x00114 */
        return sizeof(device_property);
    case XMOU_CLIENT_REV:        /* 0x00118 */
        return (x->bad_ver) ? 0 : x->enable_v2;
    default:
        return 0xffffffff;
    }
}

static uint32_t xenmou_mmio_read8(void *opaque, hwaddr addr)
{
    uint32_t d;

    DEBUG_MSG("mmio_read8(%p, 0x"TARGET_FMT_plx")\n", opaque, addr);
    d = xenmou_mmio_read32(opaque, addr & ~3);

    addr &= 3;
    addr <<= 3;
    d >>= addr;

    return d & 0xff;
}

static uint32_t xenmou_mmio_read16(void *opaque, hwaddr addr)
{
    uint32_t d;

    DEBUG_MSG("mmio_read16(%p, 0x"TARGET_FMT_plx")\n", opaque, addr);
    d = xenmou_mmio_read32(opaque, addr & ~3);

    if (addr & 0x2) {
        d >>= 16;
    }
    return d && 0xffff;
}

static void xenmou_setslot(void *opaque, uint8_t slot)
{
    PCIXenMouState *x = opaque;

    x->slot = slot;
    DEBUG_MSG("xenmou_setslot - is %d\n", x->slot);
}

static void xenmou_config(void *opaque, InputConfig *c)
{
    PCIXenMouState *x = opaque;
    int slot = c->slot;
    int i = 0;
    int ev = 0;
    uint8_t *nextbits = NULL;
    device_property *dp = NULL;
    device_property *devprop = xenmou_get_devprop(x);

    DEBUG_MSG("xenmou_config. Found '%s', Slot = %d, EVbits 0x%x\n",
              c->name, slot, c->evbits);

    if (slot > MAXSLOTS) {
        ERROR_MSG("xenmou_config for slot %d - slot number too large.\n", slot);
        return;
    }

    if (slot >= x->num_dev) {
        for (i = x->num_dev; i < slot; i++) {
            devprop[i].evbits = 0;
        }
        x->num_dev = slot + 1;
    }

    dp = &(devprop[c->slot]);

    ev = c->evbits;
    nextbits = (uint8_t *)c->bits;

    dp->evbits=ev;
    /* Use a maximum length of 26 because of the InputConfig definition
     * in libdmbus... */
    strncpy(dp->name, c->name, 26);

    if (ev & (1 << EV_ABS)) {
	    DEBUG_MSG("xenmou_config. absbits 0x%016llX.\n",
		      *((uint64_t*)nextbits));
            memcpy(dp->absbits, nextbits, sizeof(dp->absbits));
            nextbits += sizeof(dp->absbits);
	}

    if (ev & (1 << EV_REL)) {
        DEBUG_MSG("xenmou_config. relbits 0x%08X.\n",*((uint32_t*)nextbits));
        memcpy(dp->relbits, nextbits, sizeof(dp->relbits));
        nextbits += sizeof(dp->relbits);
    }

    if (ev & (1 << EV_KEY)) {
        DEBUG_MSG("xenmou_config. buttonbits 0x%08X %08X %08X .\n",
		  ((uint32_t*)nextbits)[2], ((uint32_t*)nextbits)[1],
		  ((uint32_t*)nextbits)[0]);
        memcpy(dp->buttonbits, nextbits, sizeof(dp->buttonbits));
        /*	nextbits += sizeof(dp->buttonbits); */
    }

    if (x->enable_v2) {
        xenmou_inject_record(x, EV_DEV,  DEV_CONF, c->slot);
        interrupt(x);
    }
}

#define RESET_ALL 0xFF

static void xenmou_config_reset(void *opaque, uint8_t slot)
{
    PCIXenMouState *x = opaque;
    device_property *dp;
    device_property *devprop = xenmou_get_devprop(x);
    int i;

    if (slot == RESET_ALL) {
        DEBUG_MSG("xenmou_config reset for all (%d).\n", x->num_dev);

        for (i = 0; i < x->num_dev; i++) {
            devprop[i].evbits = 0;
        }
        x->num_dev = 0;
    }
    else {
        if (slot > MAXSLOTS) {
            ERROR_MSG("xenmou_config reset for slot %d out of range.\n", slot);
            return;
	}
        DEBUG_MSG("xenmou_config reset for slot %d.\n", slot);
        dp = &(devprop[slot]);
        dp->evbits = 0;
    }

    if (x->enable_v2) {
        xenmou_inject_record(x, EV_DEV, DEV_RESET, slot);
        interrupt(x);
    }
}

static void xenmou_push_config(PCIXenMouState *x)
{
    int  i;
    device_property *devprop = xenmou_get_devprop(x);

    xenmou_inject_record(x, EV_DEV, DEV_RESET, RESET_ALL);

    for (i = 0; i < x->num_dev; i++) {
        if (devprop[i].evbits) {
            DEBUG_MSG("xenmou_push_config pushing config for slot %d.\n", i);
            xenmou_inject_record(x, EV_DEV,  DEV_CONF, i);
        }
    }

    if (x->slot != SLOT_NOT_SET) {
        DEBUG_MSG("xenmou_push_config slot is %d.\n", x->slot);
        xenmou_inject_record(x, EV_DEV,  DEV_SET, x->slot);
    } else {
        DEBUG_MSG("error: xenmou_push_config Not Slot to send!\n");
    }

    interrupt(x);
}

static uint64_t xenmou_mro_read(void *opaque, hwaddr addr, uint32_t size)
{
    uint64_t ret = 0x0ULL;

    switch (size) {
    case 1:
        ret = xenmou_mmio_read8(opaque, addr);
        break;
    case 2:
        ret = xenmou_mmio_read16(opaque, addr);
        break;
    case 4:
        ret = xenmou_mmio_read32(opaque, addr);
        break;
    default:
        break;
    }

    return ret;
}

static void xenmou_mro_write(void *opaque, hwaddr addr,
                             uint64_t data, uint32_t size)
{
    switch (size) {
    case 1:
        xenmou_mmio_write8(opaque, addr, data & 0x00FF);
        break;
    case 2:
        xenmou_mmio_write16(opaque, addr, data & 0xFFFF);
        break;
    case 4:
        xenmou_mmio_write32(opaque, addr, data & 0xFFFFFFFF);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps xenmou_mmio_handler = {
    .read = xenmou_mro_read,
    .write = xenmou_mro_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
#if 0
    .old_mmio = {
        .read = {
            xenmou_mmio_read8,
            xenmou_mmio_read16,
            xenmou_mmio_read32,
        },
        .write = {
            xenmou_mmio_write8,
            xenmou_mmio_write16,
            xenmou_mmio_write32,
        },
    },
#endif
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int xenmou_post_load(void *opaque, int version_id)
{
    /* TODO: do we need a post load after resume? */
    return 0;
}

static const VMStateDescription vmstate_xenmou = {
    .name = "xenmou",
    .version_id = 4,
    .minimum_version_id = 4,
    .minimum_version_id_old = 4,
    .post_load = xenmou_post_load,
    .fields = (VMStateField []) {
        VMSTATE_PCI_DEVICE(pci_dev, PCIXenMouState),
        VMSTATE_END_OF_LIST()
    }
};

static void xenmou_reset(DeviceState *dev)
{
    PCIXenMouState *m = DO_UPCAST(PCIXenMouState, pci_dev.qdev, dev);
    void *ptr;

    DEBUG_MSG("xenmou reset\n");

    m->last_buttons = 0;
    m->acceleration = 1;

    m->enable_device = 0;
    m->enable_v2 = 0;
    m->bad_ver = 0;
    m->enable_device_interrupts = 0;
    m->isr = 0;
    xenmou_update_irq(m);
    m->wptr=0;

    /* Reset event region and device properties region */
    ptr = memory_region_get_ram_ptr(&m->event_region);
    memset(ptr, 0, EVENT_REGION_SIZE);
    ptr = memory_region_get_ram_ptr(&m->devprop_region);
    memset(ptr, 0, DEVPROP_REGION_SIZE);

    *(xenmou_get_rptr_guest(m)) = 0;
    *(xenmou_get_wptr_guest(m)) = 0;
}

static int xenmou_initfn(PCIDevice *dev)
{
    PCIXenMouState *d = DO_UPCAST(PCIXenMouState, pci_dev, dev);
    uint8_t *pci_conf;

    DEBUG_MSG("init started\n");

    pci_conf = d->pci_dev.config;

    pci_config_set_interrupt_pin(pci_conf, 1); /* Interrupt pin 0 */

    /* Register mmio bar 0 */
    memory_region_init_io(&d->mmio_bar, &xenmou_mmio_handler, d,
                          "xenmou-mmio", MMIO_BAR_SIZE);

    memory_region_init_ram(&d->event_region, "xenmou-event",
                           EVENT_REGION_SIZE);
    memory_region_add_subregion(&d->mmio_bar, XENMOU_EVENT_OFFSET,
                                &d->event_region);

    memory_region_init_ram(&d->devprop_region, "xenmou-devprop",
                           DEVPROP_REGION_SIZE);
    memory_region_add_subregion(&d->mmio_bar, XENMOU_DEVPROP_OFFSET,
                                &d->devprop_region);
    pci_register_bar(&d->pci_dev, 0, PCI_BASE_ADDRESS_MEM_TYPE_32,
                     &d->mmio_bar);

    DEBUG_MSG("registered IO region\n");

    d->num_dev = 0;
    d->slot = SLOT_NOT_SET;

    DEBUG_MSG("set input handlers\n");
    xen_input_set_handlers(xenmou_setslot, xenmou_config,
                           xenmou_config_reset, d);

    DEBUG_MSG("init completed\n");

    return 0;
}

static void xenmou_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->init = xenmou_initfn;
    k->vendor_id = PCI_VENDOR_ID_XEN;
    k->device_id = PCI_DEVICE_ID_XENMOU;
    k->class_id = PCI_CLASS_INPUT_DEVICE;
    k->subsystem_vendor_id = PCI_VENDOR_ID_XEN;
    k->subsystem_id = PCI_DEVICE_ID_XENMOU;
    k->revision = 1;
    dc->desc = "XEN mouse pci device";
    dc->reset = xenmou_reset;
    dc->vmsd = &vmstate_xenmou;
}

static TypeInfo xenmou_info = {
    .name           = "xenmou",
    .parent         = TYPE_PCI_DEVICE,
    .instance_size  = sizeof(PCIXenMouState),
    .class_init     = xenmou_class_init,
};

static void xenmou_register_types(void)
{
    type_register_static(&xenmou_info);
}

type_init(xenmou_register_types)
