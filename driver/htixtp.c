/*
 * htixtp.c - WDF plumbing: driver/device setup, PnP and power, the HID
 * minidriver IOCTLs that HIDClass sends through mshidkmdf.sys, the interrupt
 * and the two timers (polling and maintenance).
 */

#include "htixtp.h"

static PCWSTR g_StringManufacturer = L"Hantick";
static PCWSTR g_StringProduct      = L"I2C HID touchpad (htixtp)";
static PCWSTR g_StringSerial       = L"0911-5288";

/* ------------------------------------------------------------------------ */
/* Driver / device creation                                                  */
/* ------------------------------------------------------------------------ */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, HtpEvtDeviceAdd);
    config.DriverPoolTag = HTP_POOL_TAG;

    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}

static NTSTATUS
HtpCreateTimer(
    _In_  WDFDEVICE           Device,
    _In_  PFN_WDF_TIMER       Callback,
    _Out_ WDFTIMER*           Timer
    )
{
    WDF_TIMER_CONFIG      timerConfig;
    WDF_OBJECT_ATTRIBUTES attributes;

    /* One-shot, passive-level timers; the callbacks re-arm themselves. */
    WDF_TIMER_CONFIG_INIT(&timerConfig, Callback);
    timerConfig.AutomaticSerialization = FALSE;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject   = Device;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    return WdfTimerCreate(&timerConfig, &attributes, Timer);
}

NTSTATUS
HtpEvtDeviceAdd(
    _In_    WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
    )
{
    NTSTATUS                     status;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES        attributes;
    WDFDEVICE                    device;
    PDEVICE_CONTEXT              ctx;
    WDF_IO_QUEUE_CONFIG          queueConfig;
    WDFMEMORY                    memory;

    UNREFERENCED_PARAMETER(Driver);

    /* Lower filter under mshidkmdf.sys; HIDClass stays the power policy owner. */
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware              = HtpEvtPrepareHardware;
    pnpCallbacks.EvtDeviceReleaseHardware              = HtpEvtReleaseHardware;
    pnpCallbacks.EvtDeviceD0Entry                      = HtpEvtD0Entry;
    pnpCallbacks.EvtDeviceD0ExitPreInterruptsDisabled  = HtpEvtD0ExitPreInterruptsDisabled;
    pnpCallbacks.EvtDeviceD0Exit                       = HtpEvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    ctx = GetDeviceContext(device);
    ctx->Device = device;
    ctx->State  = HtpStateStopped;
    ctx->RecoveryBackoffMs = 200;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfWaitLockCreate(&attributes, &ctx->I2cLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->FifoLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->LogLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &ctx->SavedFeatureLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Non-paged buffers, freed together with the device. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, HTP_POOL_TAG, HTP_XFER_BUFFER_SIZE,
                             &memory, (PVOID*)&ctx->XferBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, HTP_POOL_TAG, HTP_XFER_BUFFER_SIZE,
                             &memory, (PVOID*)&ctx->CmdBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, HTP_POOL_TAG, HTP_MAX_REPORT_DESC,
                             &memory, (PVOID*)&ctx->DevReportDesc);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, HTP_POOL_TAG, HTP_MAX_REPORT_DESC,
                             &memory, (PVOID*)&ctx->CacheReportDesc);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfMemoryCreate(&attributes, NonPagedPoolNx, HTP_POOL_TAG, HTP_MAX_PRESENTED_DESC,
                             &memory, (PVOID*)&ctx->PresentedDesc);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* The I2C target is created once and opened in PrepareHardware. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfIoTargetCreate(device, &attributes, &ctx->SpbTarget);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /*
     * Default queue for the HID IOCTLs. Passive execution level: GET/SET_FEATURE
     * talk to the chip synchronously, and HIDClass may send requests at DISPATCH.
     * Requests this filter does not handle (other major codes) are passed down
     * automatically by the framework.
     */
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoInternalDeviceControl = HtpEvtInternalDeviceControl;
    queueConfig.PowerManaged = WdfFalse;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;
    status = WdfIoQueueCreate(device, &queueConfig, &attributes, &ctx->DefaultQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Manual queue holding IOCTL_HID_READ_REPORT until input arrives. */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    queueConfig.PowerManaged = WdfFalse;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->ReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = HtpCreateTimer(device, HtpEvtPollTimer, &ctx->PollTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = HtpCreateTimer(device, HtpEvtMaintTimer, &ctx->MaintTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    HtpLog(ctx, L"htixtp %s loaded", HTP_VERSION_STRING);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* PnP                                                                       */
/* ------------------------------------------------------------------------ */

NTSTATUS
HtpEvtPrepareHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesRaw,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT  ctx = GetDeviceContext(Device);
    ULONG            count;
    ULONG            i;
    ULONG            interruptIndex = 0;
    NTSTATUS         status;
    WDF_IO_TARGET_OPEN_PARAMS openParams;
    DECLARE_UNICODE_STRING_SIZE(devicePath, RESOURCE_HUB_PATH_SIZE);

    HtpReadSettings(ctx);

    ctx->SpbFound = FALSE;
    ctx->InterruptResourceFound = FALSE;

    count = WdfCmResourceListGetCount(ResourcesTranslated);
    for (i = 0; i < count; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR descriptor = WdfCmResourceListGetDescriptor(ResourcesTranslated, i);
        if (descriptor == NULL) {
            continue;
        }

        switch (descriptor->Type) {
        case CmResourceTypeConnection:
            if (descriptor->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
                descriptor->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C &&
                !ctx->SpbFound) {
                ctx->SpbConnectionId.LowPart  = descriptor->u.Connection.IdLowPart;
                ctx->SpbConnectionId.HighPart = descriptor->u.Connection.IdHighPart;
                ctx->SpbFound = TRUE;
            }
            break;

        case CmResourceTypeInterrupt:
            if (!ctx->InterruptResourceFound) {
                ctx->InterruptResourceFound = TRUE;
                interruptIndex = i;
            }
            break;

        default:
            break;
        }
    }

    HtpLog(ctx, L"resources: i2c=%s interrupt=%s",
           ctx->SpbFound ? L"yes" : L"NO",
           ctx->InterruptResourceFound ? L"yes" : L"no");

    if (!ctx->SpbFound) {
        HtpWriteDiagnostics(ctx);
        return STATUS_NOT_FOUND;
    }

    status = RESOURCE_HUB_CREATE_PATH_FROM_ID(&devicePath,
                                              ctx->SpbConnectionId.LowPart,
                                              ctx->SpbConnectionId.HighPart);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&openParams, &devicePath, (GENERIC_READ | GENERIC_WRITE));
    openParams.ShareAccess       = 0;
    openParams.CreateDisposition = FILE_OPEN;
    openParams.FileAttributes    = FILE_ATTRIBUTE_NORMAL;

    status = WdfIoTargetOpen(ctx->SpbTarget, &openParams);
    if (!NT_SUCCESS(status)) {
        HtpLog(ctx, L"opening the I2C controller failed: 0x%08X", (ULONG)status);
        HtpWriteDiagnostics(ctx);
        return status;
    }
    ctx->SpbOpen = TRUE;

    /* Interrupt (unless polling-only was chosen). Falls back to polling if it can't be created. */
    ctx->Interrupt = NULL;
    ctx->PollingActive = (ctx->PollingConfig != HTP_POLL_OFF);

    if (ctx->PollingConfig != HTP_POLL_ONLY && ctx->InterruptResourceFound) {
        WDF_INTERRUPT_CONFIG interruptConfig;

        WDF_INTERRUPT_CONFIG_INIT(&interruptConfig, HtpEvtInterruptIsr, NULL);
        interruptConfig.PassiveHandling     = TRUE;
        interruptConfig.InterruptTranslated = WdfCmResourceListGetDescriptor(ResourcesTranslated, interruptIndex);
        interruptConfig.InterruptRaw        = WdfCmResourceListGetDescriptor(ResourcesRaw, interruptIndex);

        status = WdfInterruptCreate(Device, &interruptConfig, WDF_NO_OBJECT_ATTRIBUTES, &ctx->Interrupt);
        if (!NT_SUCCESS(status)) {
            HtpLog(ctx, L"interrupt could not be created (0x%08X); using polling", (ULONG)status);
            ctx->Interrupt = NULL;
            ctx->PollingActive = TRUE;
        }
    } else if (ctx->PollingConfig != HTP_POLL_ONLY) {
        HtpLog(ctx, L"no interrupt resource; using polling");
        ctx->PollingActive = TRUE;
    }

    HtpLog(ctx, L"input: %s%s", ctx->Interrupt ? L"interrupt" : L"",
           ctx->PollingActive ? (ctx->Interrupt ? L" + polling" : L"polling") : L"");

    HtpLoadDescriptorCache(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
HtpEvtReleaseHardware(
    _In_ WDFDEVICE    Device,
    _In_ WDFCMRESLIST ResourcesTranslated
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(ResourcesTranslated);

    if (ctx->Interrupt != NULL) {
        WdfObjectDelete(ctx->Interrupt);
        ctx->Interrupt = NULL;
    }

    WdfWaitLockAcquire(ctx->I2cLock, NULL);
    if (ctx->SpbOpen) {
        WdfIoTargetClose(ctx->SpbTarget);
        ctx->SpbOpen = FALSE;
    }
    WdfWaitLockRelease(ctx->I2cLock);

    HtpSetState(ctx, HtpStateStopped);
    HtpWriteDiagnostics(ctx);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Power                                                                     */
/* ------------------------------------------------------------------------ */

static VOID
HtpFlushFifo(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WdfSpinLockAcquire(Ctx->FifoLock);
    Ctx->FifoHead  = 0;
    Ctx->FifoCount = 0;
    WdfSpinLockRelease(Ctx->FifoLock);
    Ctx->LastPolledLength = 0;
}

NTSTATUS
HtpEvtD0Entry(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);
    NTSTATUS        status;
    ULONG           wakeTimeout;
    ULONG           maintDelay;

    HtpLog(ctx, L"power up (previous WDF power state %d)", (int)PreviousState);

    ctx->StartTimeMs = HtpNowMs();
    InterlockedExchange(&ctx->InterruptDisabledByUs, 0);
    InterlockedExchange(&ctx->ConsecutiveFailures, 0);
    InterlockedExchange(&ctx->ConsecutiveBadReports, 0);
    InterlockedExchange(&ctx->RestartRequested, 0);
    ctx->RecoveryBackoffMs = 200;
    HtpFlushFifo(ctx);

    /* Without any descriptor yet (first start, no cache) give the chip longer to answer. */
    wakeTimeout = (!ctx->DescriptorsValid && !ctx->CacheLoaded) ? ctx->FirstWakeTimeoutMs : ctx->WakeTimeoutMs;

    status = HtpInitializeChip(ctx, wakeTimeout, FALSE);

    if (NT_SUCCESS(status)) {
        if (!ctx->DescriptorsValid) {
            status = HtpBuildPresentedDescriptors(ctx, FALSE);
            if (!NT_SUCCESS(status)) {
                HtpLog(ctx, L"descriptor from the chip is unusable (0x%08X)", (ULONG)status);
                HtpWriteDiagnostics(ctx);
                return status;
            }
            HtpSaveDescriptorCache(ctx);
        } else if (ctx->DevReportDescChecksum != ctx->PresentedChecksum) {
            HtpLog(ctx, L"chip descriptor differs from the one Windows has; restarting the device");
            InterlockedExchange(&ctx->RestartRequested, 1);
        }

        InterlockedExchange(&ctx->DeviceReady, 1);
        InterlockedExchange(&ctx->RecoveryRequested, 0);
        HtpSetState(ctx, HtpStateReady);

        if (ctx->PresentedMode == HTP_MODE_MOUSE) {
            HtpApplyMouseMode(ctx);
        } else {
            HtpReplaySavedFeatures(ctx);
        }
    } else {
        InterlockedExchange(&ctx->DeviceReady, 0);

        if (!ctx->DescriptorsValid) {
            if (ctx->CacheLoaded) {
                HtpLog(ctx, L"chip not answering; starting with the saved descriptor and retrying in the background");
                RtlCopyMemory(ctx->DevHidDesc, ctx->CacheHidDesc, HTP_HID_DESC_LENGTH);
                (VOID)HtpParseHidDescriptor(ctx, ctx->CacheHidDesc, TRUE);
                RtlCopyMemory(ctx->DevReportDesc, ctx->CacheReportDesc, ctx->CacheReportDescLength);
                ctx->DevReportDescLength   = ctx->CacheReportDescLength;
                ctx->DevReportDescChecksum = HtpChecksum(ctx->DevReportDesc, ctx->DevReportDescLength);

                status = HtpBuildPresentedDescriptors(ctx, TRUE);
                if (!NT_SUCCESS(status)) {
                    HtpWriteDiagnostics(ctx);
                    return status;
                }
            } else {
                HtpLog(ctx, L"chip never answered on I2C (0x%08X) and no saved descriptor exists", (ULONG)status);
                HtpSetState(ctx, HtpStateNoResponse);
                HtpWriteDiagnostics(ctx);
                return STATUS_DEVICE_NOT_CONNECTED;
            }
        }

        InterlockedExchange(&ctx->RecoveryRequested, 1);
        HtpSetState(ctx, HtpStateRecovering);
    }

    InterlockedExchange(&ctx->InD0, 1);

    maintDelay = (ctx->RecoveryRequested || ctx->RestartRequested) ? 50 : 2000;
    WdfTimerStart(ctx->MaintTimer, WDF_REL_TIMEOUT_IN_MS(maintDelay));

    if (ctx->PollingActive) {
        WdfTimerStart(ctx->PollTimer, WDF_REL_TIMEOUT_IN_MS(ctx->PollIntervalMs));
    }

    HtpWriteDiagnostics(ctx);
    return STATUS_SUCCESS;
}

NTSTATUS
HtpEvtD0ExitPreInterruptsDisabled(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    /* Stop all background work before the framework disables the interrupt. */
    InterlockedExchange(&ctx->InD0, 0);
    WdfTimerStop(ctx->MaintTimer, TRUE);
    WdfTimerStop(ctx->PollTimer, TRUE);
    /* A callback that was already running may have re-armed itself; stop again. */
    WdfTimerStop(ctx->MaintTimer, TRUE);
    WdfTimerStop(ctx->PollTimer, TRUE);

    return STATUS_SUCCESS;
}

NTSTATUS
HtpEvtD0Exit(
    _In_ WDFDEVICE              Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(Device);

    HtpLog(ctx, L"power down (target WDF power state %d)", (int)TargetState);

    /*
     * Linux and this driver never send SET_POWER(SLEEP) by default: this chip
     * can wedge after a sleep/wake cycle. Optional, for battery life only.
     */
    if (ctx->SleepOnSuspend && ctx->DeviceReady) {
        (VOID)HtpSetPower(ctx, HTP_POWER_SLEEP);
    }

    InterlockedExchange(&ctx->DeviceReady, 0);
    InterlockedExchange(&ctx->InterruptDisabledByUs, 0);
    HtpFlushFifo(ctx);
    HtpSetState(ctx, HtpStateStopped);
    HtpWriteDiagnostics(ctx);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */
/* Interrupt and timers                                                      */
/* ------------------------------------------------------------------------ */

BOOLEAN
HtpEvtInterruptIsr(
    _In_ WDFINTERRUPT Interrupt,
    _In_ ULONG        MessageID
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfInterruptGetDevice(Interrupt));

    UNREFERENCED_PARAMETER(MessageID);

    InterlockedIncrement(&ctx->Stats.Interrupts);

    if (!ctx->InD0 || !ctx->DeviceReady) {
        /* Level-triggered line and a chip we can't talk to yet: don't spin. */
        HtpSleepMs(5);
        return TRUE;
    }

    HtpServiceInput(ctx, FALSE);
    return TRUE;
}

VOID
HtpEvtPollTimer(
    _In_ WDFTIMER Timer
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfTimerGetParentObject(Timer));

    if (!ctx->InD0) {
        return;
    }

    if (ctx->DeviceReady) {
        InterlockedIncrement(&ctx->Stats.PollReads);
        HtpServiceInput(ctx, TRUE);
    }

    if (ctx->InD0 && ctx->PollingActive) {
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(ctx->PollIntervalMs));
    }
}

static BOOLEAN
HtpRecover(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NTSTATUS status;

    InterlockedIncrement(&Ctx->Stats.Recoveries);
    HtpSetState(Ctx, HtpStateRecovering);

    /* Keep a stuck level-triggered line from hammering the ISR while we work. */
    if (Ctx->Interrupt != NULL && InterlockedCompareExchange(&Ctx->InterruptDisabledByUs, 1, 0) == 0) {
        WdfInterruptDisable(Ctx->Interrupt);
    }

    status = HtpInitializeChip(Ctx, Ctx->WakeTimeoutMs, TRUE);
    if (!NT_SUCCESS(status)) {
        HtpLog(Ctx, L"recovery failed (0x%08X); next try in %u ms", (ULONG)status, Ctx->RecoveryBackoffMs);
        HtpSetState(Ctx, HtpStateNoResponse);
        return FALSE;
    }

    if (Ctx->DevReportDescChecksum != Ctx->PresentedChecksum) {
        HtpLog(Ctx, L"chip descriptor differs from the one Windows has; restarting the device");
        InterlockedExchange(&Ctx->RestartRequested, 1);
    } else if (Ctx->PresentedFromCache) {
        HtpLog(Ctx, L"chip answered and matches the saved descriptor");
    }

    InterlockedExchange(&Ctx->ConsecutiveFailures, 0);
    InterlockedExchange(&Ctx->ConsecutiveBadReports, 0);
    InterlockedExchange(&Ctx->DeviceReady, 1);
    InterlockedExchange(&Ctx->RecoveryRequested, 0);
    Ctx->RecoveryBackoffMs = 200;
    HtpSetState(Ctx, HtpStateReady);

    if (Ctx->PresentedMode == HTP_MODE_MOUSE) {
        HtpApplyMouseMode(Ctx);
    } else {
        HtpReplaySavedFeatures(Ctx);
    }

    if (Ctx->Interrupt != NULL && Ctx->InD0 &&
        InterlockedCompareExchange(&Ctx->InterruptDisabledByUs, 0, 1) == 1) {
        WdfInterruptEnable(Ctx->Interrupt);
    }

    HtpLog(Ctx, L"recovered");
    return TRUE;
}

static VOID
HtpCheckAutoFallback(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    ULONGLONG elapsed;

    if (Ctx->DescriptorModeConfig != HTP_MODE_AUTO ||
        Ctx->PresentedMode != HTP_MODE_FULL ||
        Ctx->PtpActivated ||
        Ctx->AutoFallbackDone ||
        !Ctx->DescInfo.HasMouse ||
        !Ctx->DescInfo.HasInputMode ||
        Ctx->Stats.ReadRequests == 0) {
        return;
    }

    elapsed = HtpNowMs() - Ctx->StartTimeMs;
    if (elapsed < 45000) {
        return;
    }

    /*
     * The device has been up for 45 s, HIDClass is reading from it, but the
     * Windows precision touchpad stack never configured it. Windows rejected
     * it as a precision touchpad: present it as a plain mouse instead.
     */
    Ctx->AutoFallbackDone = TRUE;
    HtpLog(Ctx, L"Windows did not enable precision touchpad mode; switching to mouse mode");
    HtpWriteRegistryULong(Ctx, L"HtpAutoMouse", 1);
    HtpWriteDiagnostics(Ctx);
    WdfDeviceSetFailed(Ctx->Device, WdfDeviceFailedAttemptRestart);
}

VOID
HtpEvtMaintTimer(
    _In_ WDFTIMER Timer
    )
{
    PDEVICE_CONTEXT ctx = GetDeviceContext(WdfTimerGetParentObject(Timer));
    ULONG           next = 2000;

    if (!ctx->InD0) {
        return;
    }

    if (ctx->RestartRequested) {
        InterlockedExchange(&ctx->RestartRequested, 0);
        HtpWriteDiagnostics(ctx);
        WdfDeviceSetFailed(ctx->Device, WdfDeviceFailedAttemptRestart);
        return;
    }

    if (ctx->RecoveryRequested || !ctx->DeviceReady) {
        if (HtpRecover(ctx)) {
            next = 1000;
        } else {
            next = ctx->RecoveryBackoffMs;
            ctx->RecoveryBackoffMs = min(ctx->RecoveryBackoffMs * 2, 5000);
        }
    } else {
        ULONGLONG now = HtpNowMs();

        if (ctx->HealthCheckMs != 0 && (now - ctx->LastGoodXferMs) >= ctx->HealthCheckMs) {
            NTSTATUS status;

            InterlockedIncrement(&ctx->Stats.HealthChecks);
            status = HtpProbeChip(ctx);
            if (!NT_SUCCESS(status)) {
                InterlockedIncrement(&ctx->Stats.HealthFails);
                HtpLog(ctx, L"health check: chip stopped answering (0x%08X)", (ULONG)status);
                InterlockedExchange(&ctx->DeviceReady, 0);
                InterlockedExchange(&ctx->RecoveryRequested, 1);
                next = 10;
            }
        }

        HtpCheckAutoFallback(ctx);
        if (ctx->HealthCheckMs != 0) {
            next = min(next, ctx->HealthCheckMs);
        }
    }

    {
        /* Write diagnostics when something was logged or a counter moved. */
        LONG signature = ctx->Stats.I2cOk + ctx->Stats.I2cFail + ctx->Stats.Interrupts +
                         ctx->Stats.InputReads + ctx->Stats.ReportsDelivered + ctx->Stats.ReadRequests +
                         ctx->Stats.FeatureGetOk + ctx->Stats.FeatureSetOk + ctx->Stats.PollReads;
        if (ctx->DiagDirty || signature != ctx->LastStatsSignature) {
            ctx->LastStatsSignature = signature;
            HtpWriteDiagnostics(ctx);
        }
    }

    if (ctx->InD0) {
        if (ctx->RecoveryRequested || ctx->RestartRequested) {
            next = min(next, ctx->RecoveryBackoffMs);
        }
        WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(next));
    }
}

/* ------------------------------------------------------------------------ */
/* HID minidriver IOCTLs                                                     */
/* ------------------------------------------------------------------------ */

static NTSTATUS
HtpCopyToRequest(
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(Length) const VOID* Source,
    _In_ size_t Length
    )
{
    NTSTATUS  status;
    WDFMEMORY memory;
    size_t    capacity;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WdfMemoryGetBuffer(memory, &capacity);
    if (capacity < Length) {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)Source, Length);
    if (NT_SUCCESS(status)) {
        WdfRequestSetInformation(Request, Length);
    }
    return status;
}

/* HID_XFER_PACKET lives in Irp->UserBuffer for these IOCTLs (see the vhidmini2 sample). */
static NTSTATUS
HtpGetXferPacket(
    _In_  WDFREQUEST       Request,
    _In_  BOOLEAN          ToDevice,
    _Out_ HID_XFER_PACKET* Packet
    )
{
    WDF_REQUEST_PARAMETERS params;
    PIRP                   irp;
    size_t                 length;

    RtlZeroMemory(Packet, sizeof(*Packet));

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);

    length = ToDevice ? params.Parameters.DeviceIoControl.InputBufferLength
                      : params.Parameters.DeviceIoControl.OutputBufferLength;
    if (length < sizeof(HID_XFER_PACKET)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    irp = WdfRequestWdmGetIrp(Request);
    if (irp == NULL || irp->UserBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlCopyMemory(Packet, irp->UserBuffer, sizeof(HID_XFER_PACKET));
    if (Packet->reportBuffer == NULL || Packet->reportBufferLen == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    return STATUS_SUCCESS;
}

static VOID
HtpCompleteReadWithData(
    _In_ WDFREQUEST Request,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    NTSTATUS  status;
    WDFMEMORY memory;
    size_t    capacity = 0;
    size_t    copied   = 0;

    status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (NT_SUCCESS(status)) {
        WdfMemoryGetBuffer(memory, &capacity);
        copied = min((size_t)Length, capacity);
        status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)Data, copied);
    }
    WdfRequestCompleteWithInformation(Request, status, NT_SUCCESS(status) ? copied : 0);
}

/* Called by the input path with one report (starting with the report ID). */
VOID
HtpDeliverReport(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    WDFREQUEST request = NULL;
    NTSTATUS   status;

    if (Length == 0 || Length > HTP_MAX_INPUT_LENGTH) {
        return;
    }

    WdfSpinLockAcquire(Ctx->FifoLock);
    status = WdfIoQueueRetrieveNextRequest(Ctx->ReadQueue, &request);
    if (!NT_SUCCESS(status)) {
        ULONG slot;

        request = NULL;
        if (Ctx->FifoCount == HTP_FIFO_DEPTH) {
            /* Drop the oldest report. */
            Ctx->FifoHead = (Ctx->FifoHead + 1) % HTP_FIFO_DEPTH;
            Ctx->FifoCount--;
            InterlockedIncrement(&Ctx->Stats.ReportsDropped);
        }
        slot = (Ctx->FifoHead + Ctx->FifoCount) % HTP_FIFO_DEPTH;
        RtlCopyMemory(Ctx->Fifo[slot], Data, Length);
        Ctx->FifoLength[slot] = (USHORT)Length;
        Ctx->FifoCount++;
        InterlockedIncrement(&Ctx->Stats.ReportsQueued);
    }
    WdfSpinLockRelease(Ctx->FifoLock);

    if (request != NULL) {
        HtpCompleteReadWithData(request, Data, Length);
        InterlockedIncrement(&Ctx->Stats.ReportsDelivered);
    }
}

static VOID
HtpHandleReadReport(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ WDFREQUEST      Request
    )
{
    UCHAR    report[HTP_MAX_INPUT_LENGTH];
    ULONG    length = 0;
    BOOLEAN  haveReport = FALSE;
    NTSTATUS status = STATUS_SUCCESS;

    InterlockedIncrement(&Ctx->Stats.ReadRequests);

    WdfSpinLockAcquire(Ctx->FifoLock);
    if (Ctx->FifoCount > 0) {
        length = Ctx->FifoLength[Ctx->FifoHead];
        RtlCopyMemory(report, Ctx->Fifo[Ctx->FifoHead], length);
        Ctx->FifoHead = (Ctx->FifoHead + 1) % HTP_FIFO_DEPTH;
        Ctx->FifoCount--;
        haveReport = TRUE;
    } else {
        status = WdfRequestForwardToIoQueue(Request, Ctx->ReadQueue);
    }
    WdfSpinLockRelease(Ctx->FifoLock);

    if (haveReport) {
        HtpCompleteReadWithData(Request, report, length);
        InterlockedIncrement(&Ctx->Stats.ReportsDelivered);
    } else if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
    }
}

static VOID
HtpSaveFeature(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UCHAR ReportId,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    ULONG i;

    if (Length == 0 || Length > HTP_MAX_SAVED_FEATURE_LEN) {
        return;
    }

    WdfSpinLockAcquire(Ctx->SavedFeatureLock);
    for (i = 0; i < Ctx->SavedFeatureCount; i++) {
        if (Ctx->SavedFeatures[i].ReportId == ReportId) {
            break;
        }
    }
    if (i == Ctx->SavedFeatureCount && Ctx->SavedFeatureCount < HTP_MAX_SAVED_FEATURES) {
        Ctx->SavedFeatureCount++;
    }
    if (i < Ctx->SavedFeatureCount) {
        Ctx->SavedFeatures[i].ReportId = ReportId;
        Ctx->SavedFeatures[i].Length   = (USHORT)Length;
        RtlCopyMemory(Ctx->SavedFeatures[i].Data, Data, Length);
    }
    WdfSpinLockRelease(Ctx->SavedFeatureLock);
}

static NTSTATUS
HtpGetString(
    _In_ WDFREQUEST Request
    )
{
    WDF_REQUEST_PARAMETERS params;
    ULONG  stringId;
    PCWSTR string;

    WDF_REQUEST_PARAMETERS_INIT(&params);
    WdfRequestGetParameters(Request, &params);
    stringId = PtrToUlong(params.Parameters.DeviceIoControl.Type3InputBuffer) & 0xFFFF;

    switch (stringId) {
    case HID_STRING_ID_IMANUFACTURER: string = g_StringManufacturer; break;
    case HID_STRING_ID_IPRODUCT:      string = g_StringProduct;      break;
    case HID_STRING_ID_ISERIALNUMBER: string = g_StringSerial;       break;
    default:
        return STATUS_INVALID_PARAMETER;
    }

    return HtpCopyToRequest(Request, string, (wcslen(string) + 1) * sizeof(WCHAR));
}

VOID
HtpEvtInternalDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    WDFDEVICE       device = WdfIoQueueGetDevice(Queue);
    PDEVICE_CONTEXT ctx = GetDeviceContext(device);
    NTSTATUS        status = STATUS_NOT_SUPPORTED;
    HID_XFER_PACKET packet;
    ULONG           got = 0;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = ctx->DescriptorsValid
               ? HtpCopyToRequest(Request, &ctx->HidDescriptor, ctx->HidDescriptor.bLength)
               : STATUS_DEVICE_NOT_READY;
        break;

    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = ctx->DescriptorsValid
               ? HtpCopyToRequest(Request, &ctx->HidAttributes, sizeof(HID_DEVICE_ATTRIBUTES))
               : STATUS_DEVICE_NOT_READY;
        break;

    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = ctx->DescriptorsValid
               ? HtpCopyToRequest(Request, ctx->PresentedDesc, ctx->PresentedDescLength)
               : STATUS_DEVICE_NOT_READY;
        break;

    case IOCTL_HID_READ_REPORT:
        HtpHandleReadReport(ctx, Request);
        return;

    case IOCTL_HID_GET_FEATURE:
    case IOCTL_HID_GET_INPUT_REPORT:
        status = HtpGetXferPacket(Request, FALSE, &packet);
        if (!NT_SUCCESS(status)) {
            break;
        }
        if (!ctx->DeviceReady) {
            status = STATUS_DEVICE_NOT_READY;
            InterlockedIncrement(&ctx->Stats.FeatureGetFail);
            break;
        }
        status = HtpGetReport(ctx,
                              (IoControlCode == IOCTL_HID_GET_FEATURE) ? HTP_REPORT_TYPE_FEATURE : HTP_REPORT_TYPE_INPUT,
                              packet.reportId, packet.reportBuffer, packet.reportBufferLen, &got);
        if (NT_SUCCESS(status)) {
            InterlockedIncrement(&ctx->Stats.FeatureGetOk);
            WdfRequestSetInformation(Request, got);
        } else {
            InterlockedIncrement(&ctx->Stats.FeatureGetFail);
            HtpLog(ctx, L"GET_REPORT id %u failed: 0x%08X", (ULONG)packet.reportId, (ULONG)status);
        }
        break;

    case IOCTL_HID_SET_FEATURE:
    case IOCTL_HID_SET_OUTPUT_REPORT:
        status = HtpGetXferPacket(Request, TRUE, &packet);
        if (!NT_SUCCESS(status)) {
            break;
        }

        if (IoControlCode == IOCTL_HID_SET_FEATURE) {
            if (!ctx->PtpActivated) {
                ctx->PtpActivated = TRUE;
                HtpLog(ctx, L"Windows is configuring the touchpad (first SET_FEATURE, id %u)", (ULONG)packet.reportId);
            }
            HtpSaveFeature(ctx, packet.reportId, packet.reportBuffer, packet.reportBufferLen);
        }

        if (!ctx->DeviceReady) {
            /* Remembered above; it is applied when the chip comes back. */
            status = (IoControlCode == IOCTL_HID_SET_FEATURE) ? STATUS_SUCCESS : STATUS_DEVICE_NOT_READY;
        } else {
            status = HtpSetReport(ctx,
                                  (IoControlCode == IOCTL_HID_SET_FEATURE) ? HTP_REPORT_TYPE_FEATURE : HTP_REPORT_TYPE_OUTPUT,
                                  packet.reportId, packet.reportBuffer, packet.reportBufferLen);
        }

        if (NT_SUCCESS(status)) {
            InterlockedIncrement(&ctx->Stats.FeatureSetOk);
            WdfRequestSetInformation(Request, packet.reportBufferLen);
        } else {
            InterlockedIncrement(&ctx->Stats.FeatureSetFail);
            HtpLog(ctx, L"SET_REPORT id %u failed: 0x%08X", (ULONG)packet.reportId, (ULONG)status);
        }
        break;

    case IOCTL_HID_WRITE_REPORT:
        status = HtpGetXferPacket(Request, TRUE, &packet);
        if (!NT_SUCCESS(status)) {
            break;
        }
        status = ctx->DeviceReady ? HtpWriteOutputReport(ctx, packet.reportBuffer, packet.reportBufferLen)
                                  : STATUS_DEVICE_NOT_READY;
        if (NT_SUCCESS(status)) {
            WdfRequestSetInformation(Request, packet.reportBufferLen);
        }
        break;

    case IOCTL_HID_GET_STRING:
        status = HtpGetString(Request);
        break;

    case IOCTL_HID_ACTIVATE_DEVICE:
    case IOCTL_HID_DEACTIVATE_DEVICE:
        status = STATUS_SUCCESS;
        break;

    case IOCTL_HID_GET_INDEXED_STRING:
    case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
    case IOCTL_GET_PHYSICAL_DESCRIPTOR:
        status = STATUS_NOT_SUPPORTED;
        break;

    default:
    {
        /* Not a HID minidriver request: pass it down to the ACPI device unchanged. */
        WDF_REQUEST_SEND_OPTIONS options;

        WdfRequestFormatRequestUsingCurrentType(Request);
        WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
        if (WdfRequestSend(Request, WdfDeviceGetIoTarget(device), &options)) {
            return;
        }
        status = WdfRequestGetStatus(Request);
        break;
    }
    }

    WdfRequestComplete(Request, status);
}
