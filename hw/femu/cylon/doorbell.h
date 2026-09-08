/*
 * doorbell.h — Cylon doorbell: minimal MSI-X PCI device for guest-visible
 * job-completion interrupts (D3 Phase 2).
 *
 * The PNM engine thread calls cylon_doorbell_notify() after publishing DONE
 * (+ BI retrap). The device is a QEMU-side singleton; when absent (default)
 * the notify is a no-op and clients fall back to polling/sleeping. The guest
 * side is a tiny PCI driver + misc chardev (/dev/cylon-db) that poll()s on
 * the IRQ count.
 */
#ifndef CYLON_DOORBELL_H
#define CYLON_DOORBELL_H

#include "qom/object.h"

#define TYPE_CYLON_DOORBELL "cylon-doorbell"

/* Engine-side hook: raise the MSI-X (no-op when the device is absent or its
 * MSI-X is still masked — i.e., before the guest driver is loaded). */
void cylon_doorbell_notify(void);

#endif /* CYLON_DOORBELL_H */
