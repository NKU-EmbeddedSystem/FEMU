/*
 * doorbell.c — Cylon doorbell: minimal MSI-X PCI device (D3 Phase 2).
 *
 * One MSI-X vector on an exclusive BAR0 (table-only BAR). No MMIO registers:
 * the doorbell semantics live in the mailbox (engine publishes DONE, then
 * raises the vector). The guest driver cylon_doorbell.ko matches
 * vendor/device, requests the IRQ, and exposes an IRQ counter via
 * /dev/cylon-db (poll()/read()).
 *
 * Engine hook: cylon_doorbell_notify() is called by the PNM thread after
 * DONE publish + BI retrap. msix_notify() is a no-op while MSI-X is masked
 * (guest driver not loaded), so the polling path stays the default and
 * byte-identical. The device is optional on the QEMU command line; without
 * -device cylon-doorbell the notify is a NULL-check no-op.
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/femu/femu.h"
#include "hw/femu/cylon/doorbell.h"

#define CYLON_DB_VENDOR_ID  0x1b36  /* Red Hat (QEMU) vendor space */
#define CYLON_DB_DEVICE_ID  0xbf00  /* experimental slot; single device in VM */
#define CYLON_DB_VECTORS    1

typedef struct CylonDoorbellState {
    PCIDevice parent_obj;
} CylonDoorbellState;

#define CYLON_DOORBELL(x) OBJECT_CHECK(CylonDoorbellState, (x), TYPE_CYLON_DOORBELL)

/* Global singleton: the PNM engine thread reaches the device without any QOM
 * lookup in the job-completion path. Set at realize, cleared at exit. */
static CylonDoorbellState *g_db;

void cylon_doorbell_notify(void)
{
    CylonDoorbellState *s = g_db;
    if (s) {
        msix_notify(&s->parent_obj, 0);
    }
}

static void doorbell_realize(PCIDevice *pd, Error **errp)
{
    CylonDoorbellState *s = CYLON_DOORBELL(pd);
    int ret;

    /* registers the exclusive BAR0 internally */
    ret = msix_init_exclusive_bar(pd, CYLON_DB_VECTORS, 0, errp);
    if (ret < 0) {
        return;
    }
    /* mark vector 0 usable: msix_notify() silently drops notifies for
     * vectors without msix_entry_used[] set (silent-no-op bug) */
    msix_vector_use(pd, 0);
    g_db = s;
}

static void doorbell_exit(PCIDevice *dev)
{
    if (g_db == CYLON_DOORBELL(dev)) {
        g_db = NULL;
    }
    msix_uninit_exclusive_bar(dev);
}

static void doorbell_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = doorbell_realize;
    k->exit = doorbell_exit;
    k->vendor_id = CYLON_DB_VENDOR_ID;
    k->device_id = CYLON_DB_DEVICE_ID;
    k->revision = 0;
    k->class_id = 0xff;                     /* PCI_CLASS_OTHERS */
    dc->desc = "Cylon CXL-SSD mailbox doorbell (MSI-X, 1 vector)";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo doorbell_info = {
    .name          = TYPE_CYLON_DOORBELL,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(CylonDoorbellState),
    .class_init    = doorbell_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { }
    },
};

static void doorbell_register_types(void)
{
    type_register_static(&doorbell_info);
}

type_init(doorbell_register_types);
