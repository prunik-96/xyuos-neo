#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// A minimal xHCI (USB 3.x host controller) driver, just enough to run a boot-
// protocol USB keyboard. Polling only, no interrupts. This is what lets the
// keyboard work on UEFI-only hardware, where there is no PS/2 emulation at
// port 0x60 and the PS/2 driver sees nothing.
//
// Deliberately narrow: one keyboard on one root-hub port, HID boot protocol
// (fixed 8-byte reports), no hubs, no hot-plug beyond the first device seen.

// Find and initialise the controller and the first keyboard on it. Returns 1
// if a keyboard is ready, 0 otherwise. Safe to call when there is no xHCI (it
// just returns 0), so it can run alongside the PS/2 path.
int xhci_init(void);

// Poll the keyboard's interrupt endpoint for new HID reports and feed any key
// presses into the keyboard event queue. Called from the timer tick.
void xhci_poll(void);

// What the controller found, for the device list. xhci_present() is 1 once a
// controller is up; the other two count the HID devices bound to it.
int xhci_present(void);
int xhci_keyboard_count(void);
int xhci_mouse_present(void);

// --- USB Mass Storage (flash drives on the same xHCI) -----------------------
// Bulk-Only-Transport disks are picked up during xhci_init (alongside the
// keyboard); a board may have several (boot stick + data stick), indexed
// 0..usb_disk_count()-1. Read/write 512-byte logical blocks over SCSI
// READ(10)/WRITE(10). Return 1 on success, 0 on failure.
int usb_disk_count(void);
int usb_disk_present(void);                                 // count > 0
int usb_disk_capacity(int dev, uint32_t *blocks, uint32_t *bsize);
int usb_disk_read(int dev, uint32_t lba, uint32_t count, void *buf);
int usb_disk_write(int dev, uint32_t lba, uint32_t count, const void *buf);

// --- USB RNDIS network adapter (phone USB tethering, or QEMU usb-net) --------
// Configured during xhci_init if present. The stack (kernel/net) reaches it via
// the usbnet nic_driver_t, which wraps these.
int rndis_present(void);                    // 1 once init + handshake succeeded
const uint8_t *rndis_mac(void);             // 6-byte MAC
int rndis_send(const void *frame, int len); // 0 on success
int rndis_recv(void *buf, int max);         // frame length, or 0 if none
const char *rndis_status(void);             // last handshake stage (diagnostics)
int rndis_last_cc(void);                     // last control completion code

#endif
