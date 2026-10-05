#include "gameport.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text( PAGE, EvtDevicePrepareHardware )
#pragma alloc_text( PAGE, EvtDeviceReleaseHardware )
#endif

static
UCHAR
ScaleAxis(
    IN LONGLONG PulseUs
    )
{
    if (PulseUs <= GAMEPORT_AXIS_MIN_US) {
        return 0;
    }
    if (PulseUs >= GAMEPORT_AXIS_MAX_US) {
        return 255;
    }
    return (UCHAR)(((PulseUs - GAMEPORT_AXIS_MIN_US) * 255) /
                   (GAMEPORT_AXIS_MAX_US - GAMEPORT_AXIS_MIN_US));
}

NTSTATUS
EvtDevicePrepareHardware(
    IN WDFDEVICE      Device,
    IN WDFCMRESLIST    ResourcesTranslated,
    IN WDFCMRESLIST    ResourcesRaw
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    ULONG           i;
    ULONG           count;
    BOOLEAN         found = FALSE;
    LARGE_INTEGER   freq;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    PAGED_CODE();

    count = WdfCmResourceListGetCount(ResourcesTranslated);

    for (i = 0; i < count; i++) {

        PCM_PARTIAL_RESOURCE_DESCRIPTOR desc =
            WdfCmResourceListGetDescriptor(ResourcesTranslated, i);

        if (desc == NULL) {
            continue;
        }

        if (desc->Type == CmResourceTypePort && !found) {
            //
            // Translated I/O port range of the game port PCI function.
            // On x86/x64 these remain real port I/O addresses
            // (typically 0x200..0x207).
            //
            ctx->PortBase   = (PUCHAR)(ULONG_PTR)desc->u.Port.Start.QuadPart;
            ctx->PortLength = desc->u.Port.Length;
            found = TRUE;
        }
    }

    if (!found) {
        KdPrint(("GameportHid: no port resource!\n"));
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    ctx->PortData = ctx->PortBase + GAMEPORT_DATA_OFFSET;

    //
    // Cache the QPC frequency (fixed for the whole boot) and the axis
    // timeout precomputed in ticks: the polling loop then compares
    // counters only - no setup arithmetic per sample.
    //
    KeQueryPerformanceCounter(&freq);
    ctx->QpcFrequency = freq.QuadPart;
    ctx->TimeoutTicks = (freq.QuadPart * GAMEPORT_AXIS_TIMEOUT_US) / 1000000;

    KdPrint(("GameportHid: port base 0x%p, length %lu, data reg 0x%p\n",
             ctx->PortBase, ctx->PortLength, ctx->PortData));

    return STATUS_SUCCESS;
}

NTSTATUS
EvtDeviceReleaseHardware(
    IN WDFDEVICE      Device,
    IN WDFCMRESLIST    ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);
    PAGED_CODE();

    ctx->PortBase = NULL;
    ctx->PortData = NULL;
    return STATUS_SUCCESS;
}

NTSTATUS
EvtDeviceD0Entry(
    IN WDFDEVICE             Device,
    IN WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    WdfTimerStart(ctx->PollTimer, WDF_REL_TIMEOUT_IN_MS(ctx->PollPeriodMs));
    return STATUS_SUCCESS;
}

NTSTATUS
EvtDeviceD0Exit(
    IN WDFDEVICE            Device,
    IN WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfTimerStop(ctx->PollTimer, FALSE);
    return STATUS_SUCCESS;
}

//
// Polling timer: sample the port, then complete pending HID reads.
//
VOID
EvtPollTimer(
    IN WDFTIMER Timer
    )
{
    WDFDEVICE       device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_CONTEXT ctx = GetDeviceContext(device);

    if (ctx->PortData == NULL) {
        return;
    }

    GameportReadAxes(ctx);
    GameportCompletePendingReads(ctx);
}

//
// Elapsed time in microseconds between T0 and Now. Called only on axis
// bit transitions (max 3 per sample) - never per loop iteration.
//
static
LONGLONG
ElapsedUs(
    IN LONGLONG Now,
    IN LONGLONG T0,
    IN LONGLONG Frequency
    )
{
    return ((Now - T0) * 1000000) / Frequency;
}

//
// The heart of the project: measure RC pulse widths of the NE558.
//
// Classic PC game port: writing to the data register (0x201) triggers all
// four one-shots; axis bits (bits 0..3 of 0x201) stay high until the
// capacitor charges through the joystick potentiometer. Buttons are
// bits 4..7, active LOW.
//
VOID
GameportReadAxes(
    IN PDEVICE_CONTEXT ctx
    )
{
    PUCHAR        port = ctx->PortData;
    LARGE_INTEGER t0;
    LARGE_INTEGER now;
    LONGLONG      deadline;
    LONGLONG      xUs = GAMEPORT_AXIS_TIMEOUT_US;
    LONGLONG      yUs = GAMEPORT_AXIS_TIMEOUT_US;
    LONGLONG      zUs = GAMEPORT_AXIS_TIMEOUT_US;
    BOOLEAN       xDone = FALSE;
    BOOLEAN       yDone = FALSE;
    BOOLEAN       zDone = FALSE;
    BOOLEAN       firstPass = TRUE;
    UCHAR         buttons = 0;
    UCHAR         b;
    UCHAR         report[INPUT_REPORT_SIZE];

    //
    // Fire the one-shots and measure all axes in one pass. The timeout
    // deadline is precomputed in QPC ticks (PrepareHardware), so the
    // loop below only reads and compares - no 64-bit division unless
    // an axis bit actually falls.
    //
    WRITE_PORT_UCHAR(port, GAMEPORT_TRIGGER_VALUE);
    t0 = KeQueryPerformanceCounter(NULL);
    deadline = t0.QuadPart + ctx->TimeoutTicks;

    while (!(xDone && yDone && zDone)) {

        b = READ_PORT_UCHAR(port);
        now = KeQueryPerformanceCounter(NULL);

        //
        // Buttons (bits 4..7, active LOW -> invert; ST50 uses 1 and 2)
        // are combinational: sample them from the very first read
        // instead of waiting out the axis pass (~1 ms less latency).
        //
        if (firstPass) {
            buttons = b;
            firstPass = FALSE;
        }

        if (!xDone && (b & 0x01) == 0) {
            xUs = ElapsedUs(now.QuadPart, t0.QuadPart, ctx->QpcFrequency);
            xDone = TRUE;
        }
        if (!yDone && (b & 0x02) == 0) {
            yUs = ElapsedUs(now.QuadPart, t0.QuadPart, ctx->QpcFrequency);
            yDone = TRUE;
        }
        if (!zDone && (b & 0x08) == 0) {
            zUs = ElapsedUs(now.QuadPart, t0.QuadPart, ctx->QpcFrequency);
            zDone = TRUE;
        }

        if (now.QuadPart >= deadline) {
            break;              // axis unplugged / broken pot
        }
    }

    report[0] = ScaleAxis(xUs);
    report[1] = ScaleAxis(yUs);
    report[2] = ScaleAxis(zUs);
    report[3] = (UCHAR)((~buttons >> 4) & 0x03);

    WdfSpinLockAcquire(ctx->DataLock);
    RtlCopyMemory(ctx->InputReport, report, INPUT_REPORT_SIZE);
    WdfSpinLockRelease(ctx->DataLock);
}

//
// hidclass keeps 1-2 IOCTL_HID_READ_REPORT requests pending; we complete
// them (all) with the latest sample. This is the vhidmini/hidusbfx2
// "manual queue + timer" pattern.
//
VOID
GameportCompletePendingReads(
    IN PDEVICE_CONTEXT ctx
    )
{
    NTSTATUS   status;
    WDFREQUEST  request;
    PVOID      outBuf;
    ULONG      handled = 0;

    for (;;) {

        //
        // Livelock guard: hidclass resubmits each completed read
        // immediately, in the completing context (our DPC). Without a cap
        // this loop would never see the queue empty, the DPC would never
        // return, and the whole system would freeze.
        //
        if (handled >= GAMEPORT_MAX_READS_PER_TICK) {
            break;
        }

        status = WdfIoQueueRetrieveNextRequest(ctx->ReadQueue, &request);
        if (!NT_SUCCESS(status)) {
            break;              // STATUS_NO_MORE_ENTRIES
        }

        //
        // The ONLY write path: the request's OUTPUT buffer, obtained
        // through WDF (method-aware).
        //
        status = WdfRequestRetrieveOutputBuffer(request,
                                                INPUT_REPORT_SIZE,
                                                &outBuf,
                                                NULL);
        if (NT_SUCCESS(status)) {

            WdfSpinLockAcquire(ctx->DataLock);
            RtlCopyMemory(outBuf, ctx->InputReport, INPUT_REPORT_SIZE);
            WdfSpinLockRelease(ctx->DataLock);

            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS,
                                              INPUT_REPORT_SIZE);
            handled++;
            continue;
        }

        //
        // Error path only - log so failures stay visible in DebugView.
        //
        ctx->DebugBadReads++;
        KdPrint(("GameportHid: read REJECTED (#%lu): OutRetr=0x%08X\n",
                 ctx->DebugBadReads, status));

        WdfRequestComplete(request, status);
        handled++;
    }
}