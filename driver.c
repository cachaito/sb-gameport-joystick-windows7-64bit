#include "gameport.h"

#ifdef ALLOC_PRAGMA
#pragma alloc_text( PAGE, EvtDeviceAdd )
#endif

NTSTATUS
DriverEntry(
    IN PDRIVER_OBJECT   DriverObject,
    IN PUNICODE_STRING  RegistryPath
    )
{
    NTSTATUS            status;
    WDF_DRIVER_CONFIG   config;

    KdPrint(("GameportHid: build %s %s\n", __DATE__, __TIME__));

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &config,
                             WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GameportHid: WdfDriverCreate failed 0x%08X\n", status));
    }

    return status;
}

NTSTATUS
EvtDeviceAdd(
    IN WDFDRIVER         Driver,
    IN PWDFDEVICE_INIT   DeviceInit
    )
{
    NTSTATUS                     status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDF_IO_QUEUE_CONFIG          queueConfig;
    WDF_TIMER_CONFIG             timerConfig;
    WDFDEVICE                    device;
    PDEVICE_CONTEXT              ctx;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    //
    // We are a HID minidriver operating UNDER the inbox mshidkmdf.sys
    // pass-through driver (INF sets UpperFilters = mshidkmdf). Marking
    // ourselves a filter makes hidclass the power policy owner.
    // (Same as WDK sample src\hid\hidusbfx2.)
    //
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry         = EvtDeviceD0Entry;
    pnpCallbacks.EvtDeviceD0Exit          = EvtDeviceD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("GameportHid: WdfDeviceCreate failed 0x%08X\n", status));
        return status;
    }

    ctx = GetDeviceContext(device);
    ctx->PollPeriodMs = GAMEPORT_POLL_PERIOD_MS;
    RtlZeroMemory(ctx->InputReport, sizeof(ctx->InputReport));

    //
    // Spinlock protecting InputReport
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->DataLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Default queue: internal device control (IOCTL_HID_* from hidclass,
    // passed down by mshidkmdf). PowerManaged = FALSE is important for
    // HID internal IOCTLs.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = EvtIoInternalDeviceControl;
    queueConfig.PowerManaged = FALSE;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              NULL);   // default queue, no handle needed
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Manual queue: IOCTL_HID_READ_REPORT is parked here and completed
    // from the polling timer with a fresh sample.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = FALSE;

    status = WdfIoQueueCreate(device,
                              &queueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &ctx->ReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    //
    // Periodic polling timer (runs at DISPATCH_LEVEL)
    //
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EvtPollTimer, ctx->PollPeriodMs);

    status = WdfTimerCreate(&timerConfig, &attributes, &ctx->PollTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return STATUS_SUCCESS;
}