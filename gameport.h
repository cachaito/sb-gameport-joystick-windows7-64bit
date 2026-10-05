#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>     // HID_DESCRIPTOR, HID_XFER_PACKET, IOCTL_HID_* (jak w samplu hidusbfx2 z WDK 7600)

//
// PCI function of the SB Live! game port (SB0100)
//
#define GAMEPORT_VEN_ID     0x1102
#define GAMEPORT_DEV_ID     0x7002

//
// Analog game port timing (NE558 one-shot, C = 10 nF, pot 0..100 kOhm):
//   pulse_us = 24.2 + 0.0111 * R_ohm   ->  ~24 us .. ~1150 us
//
#define GAMEPORT_DATA_OFFSET       1       // port base + 1 (classic 0x201)
#define GAMEPORT_TRIGGER_VALUE     0xFF    // any write fires the one-shots
#define GAMEPORT_AXIS_TIMEOUT_US   1200    // > max time => axis unplugged
#define GAMEPORT_AXIS_MIN_US       24      // pulse time at R = 0
#define GAMEPORT_AXIS_MAX_US       1150    // pulse time at R = 100 k
#define GAMEPORT_POLL_PERIOD_MS    10      // sampling rate: 100 Hz

//
// Input report (4 bytes, no report ID):
//   [0] = X, [1] = Y, [2] = Z (throttle ring, gameport axis 4),
//   [3] = buttons (bit0 = B1, bit1 = B2)
//
#define INPUT_REPORT_SIZE  4

//
// Livelock guard: hidclass resubmits every completed READ immediately,
// in the completing context (our DPC). Without a cap, the completion
// loop would never see the queue empty, the DPC would never return,
// and the whole system would freeze (no bugcheck on Win7).
//
#define GAMEPORT_MAX_READS_PER_TICK 8

typedef struct _DEVICE_CONTEXT {
    PUCHAR      PortBase;                   // translated I/O range start
    ULONG       PortLength;
    PUCHAR      PortData;                   // PortBase + GAMEPORT_DATA_OFFSET
    WDFQUEUE    ReadQueue;                  // manual queue for IOCTL_HID_READ_REPORT
    WDFTIMER    PollTimer;
    WDFSPINLOCK DataLock;
    ULONG       PollPeriodMs;
    LONGLONG    QpcFrequency;   // QPC ticks per second (cached, boot-constant)
    LONGLONG    TimeoutTicks;   // axis timeout precomputed in QPC ticks
    UCHAR       InputReport[INPUT_REPORT_SIZE];
    ULONG       DebugBadReads;   // reads rejected (error log counter only)
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

//
// driver.c
//
DRIVER_INITIALIZE                      DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD              EvtDeviceAdd;

//
// device.c
//
EVT_WDF_DEVICE_PREPARE_HARDWARE        EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE        EvtDeviceReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY               EvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT                EvtDeviceD0Exit;
EVT_WDF_TIMER                          EvtPollTimer;

VOID
GameportReadAxes(
    IN PDEVICE_CONTEXT Context
    );

VOID
GameportCompletePendingReads(
    IN PDEVICE_CONTEXT Context
    );

//
// hid.c
//
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL EvtIoInternalDeviceControl;

NTSTATUS
RequestCopyFromBuffer(
    IN WDFREQUEST Request,
    IN PVOID      SourceBuffer,
    IN size_t     NumBytesToCopyFrom
    );