#include "gameport.h"

//
// HID report descriptor: joystick, 2 axes (X, Y) 0..255, 2 buttons.
// Extend USAGE_MAXIMUM / REPORT_COUNT if your stick has more axes
// or buttons (game port supports up to 4 axes + 4 buttons).
//
static const UCHAR G_ReportDescriptor[] = {
    0x05, 0x01,        // USAGE_PAGE (Generic Desktop)
    0x09, 0x04,        //   USAGE (Joystick)
    0xA1, 0x01,        //   COLLECTION (Application)
    0xA1, 0x00,        //     COLLECTION (Physical)
    0x09, 0x30,        //       USAGE (X)
    0x09, 0x31,        //       USAGE (Y)
    0x09, 0x32,        //       USAGE (Z - throttle ring, axis 4)
    0x15, 0x00,        //       LOGICAL_MINIMUM (0)
    0x26, 0xFF, 0x00,  //       LOGICAL_MAXIMUM (255)
    0x75, 0x08,        //       REPORT_SIZE (8)
    0x95, 0x03,        //       REPORT_COUNT (3)
    0x81, 0x02,        //       INPUT (Data,Var,Abs)
    0x05, 0x09,        //       USAGE_PAGE (Button)
    0x19, 0x01,        //       USAGE_MINIMUM (Button 1)
    0x29, 0x02,        //       USAGE_MAXIMUM (Button 2)
    0x15, 0x00,        //       LOGICAL_MINIMUM (0)
    0x25, 0x01,        //       LOGICAL_MAXIMUM (1)
    0x75, 0x01,        //       REPORT_SIZE (1)
    0x95, 0x02,        //       REPORT_COUNT (2)
    0x81, 0x02,        //       INPUT (Data,Var,Abs)
    0x95, 0x06,        //       REPORT_COUNT (6)   ; pad to byte
    0x81, 0x03,        //       INPUT (Cnst,Var,Abs)
    0xC0,              //     END_COLLECTION
    0xC0               //   END_COLLECTION
};

//
// Standard HID descriptor pointing at the report descriptor above.
//
static const HID_DESCRIPTOR G_HidDescriptor = {
    0x09,                     // bLength (literal! sizeof() may pad to 10)
    0x21,                     // bDescriptorType = HID
    0x0110,                   // bcdHID = 1.10
    0x00,                     // bCountry
    0x01,                     // bNumDescriptors
    {
        {
            0x22,                     // bReportType = report descriptor
            sizeof(G_ReportDescriptor) // wReportLength
        }
    }
};

//
// Helper: copy a small buffer into the request's output buffer and
// complete the request (pattern from vhidmini).
//
NTSTATUS
RequestCopyFromBuffer(
    IN WDFREQUEST Request,
    IN PVOID      SourceBuffer,
    IN size_t     NumBytesToCopyFrom
    )
{
    NTSTATUS status;
    PVOID    destBuffer;

    status = WdfRequestRetrieveOutputBuffer(Request,
                                            NumBytesToCopyFrom,
                                            &destBuffer,
                                            NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return status;
    }

    RtlCopyMemory(destBuffer, SourceBuffer, NumBytesToCopyFrom);

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                      NumBytesToCopyFrom);
    return STATUS_SUCCESS;
}

VOID
EvtIoInternalDeviceControl(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request,
    IN size_t     OutputBufferLength,
    IN size_t     InputBufferLength,
    IN ULONG      IoControlCode
    )
{
    NTSTATUS        status;
    WDFDEVICE       device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = GetDeviceContext(device);

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        //
        // hidclass first asks for the HID descriptor.
        //
        RequestCopyFromBuffer(Request,
                              (PVOID)&G_HidDescriptor,
                              sizeof(G_HidDescriptor));
        return;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        RequestCopyFromBuffer(Request,
                              (PVOID)G_ReportDescriptor,
                              sizeof(G_ReportDescriptor));
        return;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES: {
        HID_DEVICE_ATTRIBUTES attrs;

        RtlZeroMemory(&attrs, sizeof(attrs));
        attrs.Size          = sizeof(attrs);
        attrs.VendorID      = GAMEPORT_VEN_ID;   // shows as VID_1102
        attrs.ProductID     = GAMEPORT_DEV_ID;    // shows as PID_7002
        attrs.VersionNumber = 0x0100;

        RequestCopyFromBuffer(Request, &attrs, sizeof(attrs));
        return;
    }

    case IOCTL_HID_READ_REPORT:
        //
        // Park the request; the polling timer completes it with a
        // fresh sample (GameportCompletePendingReads).
        //
        status = WdfRequestForwardToIoQueue(Request, ctx->ReadQueue);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    default:
        //
        // GET_STRING, GET/SET_FEATURE, WRITE_REPORT, GET_INPUT_REPORT,
        // SEND_IDLE_NOTIFICATION... - not needed for a basic stick.
        // Add string support here if you want a friendly name in joy.cpl.
        //
        WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
        return;
    }
}