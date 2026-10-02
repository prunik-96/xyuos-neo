#ifndef XHCI_H
#define XHCI_H

#include <stdint.h>

// xHCI: the machine's USB host controllers, the hubs on them and the devices
// behind those. Polled, no interrupts.
//
// Every controller is brought up -- a board like the B650M has several, and
// which socket on the case leads to which is anybody's guess. Devices are set
// up wherever they are, straight on a controller's port or behind any number
// of hubs, at start-up and whenever one is plugged in later; one pulled out
// is let go of. Keyboards, mice, flash drives, a phone's USB tethering and
// hubs are driven; anything else is listed and left alone.

// Find and start every controller and everything plugged into them. Returns
// 1 if a keyboard is ready, 0 otherwise. Safe to call when there is no xHCI
// (it just returns 0), so it can run alongside the PS/2 path.
int xhci_init(void);

// Take the keyboards' and mice's reports, and notice ports and hubs that have
// something new. Called from the timer tick.
void xhci_poll(void);

// Set up what was plugged in and let go of what was pulled out since the last
// call. Waits on the devices (a hub port takes a good part of a second), so it
// is called from the bootstrap core's idle loop, never from an interrupt.
// Cheap when there is nothing to do.
void usb_service(void);

// What happened, for the desktop to say so: one plugged in or pulled out.
#define USB_KIND_OTHER   0
#define USB_KIND_HUB     1
#define USB_KIND_KBD     2
#define USB_KIND_MOUSE   3
#define USB_KIND_COMBO   4      // a keyboard and a mouse on one plug: a receiver
#define USB_KIND_DISK    5
#define USB_KIND_NET     6      // a phone's USB tethering
struct usb_news {
    int attached;               // 1 plugged in, 0 pulled out
    int kind;
    uint32_t mib;               // a disk's size, 0 if not known
    char name[48];              // what the device calls itself, may be empty
};
// 1 and the oldest news in *n, or 0 if there is none.
int xhci_news(struct usb_news *n);

// What the controllers found, for the device list. xhci_present() is 1 once a
// keyboard or mouse is up; the other two count the HID devices bound.
int xhci_present(void);
int xhci_keyboard_count(void);
int xhci_mouse_present(void);

// Every device on every controller, for the device manager.
struct usb_info {
    int kind;                   // USB_KIND_*
    int speed;                  // 1 full, 2 low, 3 high, 4 super
    int depth;                  // hubs between it and the controller
    uint16_t vid, pid;
    char name[48];
};
int usb_list(struct usb_info *out, int max);

// --- USB Mass Storage (flash drives) -----------------------------------------
// Bulk-Only-Transport disks. Each has an index that stays its own for as long
// as it is plugged in; one pulled out leaves a hole (usb_disk_id 0) rather
// than renumbering the rest. Read/write logical blocks over SCSI READ(10)/
// WRITE(10). Return 1 on success, 0 on failure.
int usb_disk_count(void);                                   // highest index + 1
int usb_disk_present(void);                                 // any plugged in
// A number for this plugging-in of disk `dev`, never reused; 0 if there is no
// disk there now. A filesystem keeps it to notice its disk went away.
uint32_t usb_disk_id(int dev);
const char *usb_disk_name(int dev);
int usb_disk_capacity(int dev, uint32_t *blocks, uint32_t *bsize);
int usb_disk_read(int dev, uint32_t lba, uint32_t count, void *buf);
int usb_disk_write(int dev, uint32_t lba, uint32_t count, const void *buf);

// --- USB RNDIS network adapter (phone USB tethering, or QEMU usb-net) --------
// Set up when found. The stack (kernel/net) reaches it via the usbnet
// nic_driver_t, which wraps these.
int rndis_present(void);                    // 1 once init + handshake succeeded
const uint8_t *rndis_mac(void);             // 6-byte MAC
int rndis_send(const void *frame, int len); // 0 on success
int rndis_recv(void *buf, int max);         // frame length, or 0 if none
const char *rndis_status(void);             // last handshake stage (diagnostics)
int rndis_last_cc(void);                     // last control completion code

#endif
