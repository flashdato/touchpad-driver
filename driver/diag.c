/*
 * diag.c - settings, descriptor cache, and diagnostics.
 *
 * Everything lives under the touchpad's hardware key:
 *   HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\<id>\<instance>\Device Parameters
 * Settings are read when the device starts; the control script (tpctl.ps1)
 * writes them and restarts the device. Diagnostics (state, counters, the last
 * lines of the driver log) are written back every few seconds so the script
 * can show what the driver sees.
 */

#include "htixtp.h"

typedef struct _HTP_STAT_NAME {
    PCWSTR Name;
    SIZE_T Offset;
} HTP_STAT_NAME;

static const HTP_STAT_NAME g_StatNames[] = {
    { L"HtpStat_I2cOk",             FIELD_OFFSET(HTP_STATS, I2cOk) },
    { L"HtpStat_I2cFail",           FIELD_OFFSET(HTP_STATS, I2cFail) },
    { L"HtpStat_InitOk",            FIELD_OFFSET(HTP_STATS, InitOk) },
    { L"HtpStat_InitFail",          FIELD_OFFSET(HTP_STATS, InitFail) },
    { L"HtpStat_WakeAttemptsLast",  FIELD_OFFSET(HTP_STATS, WakeAttemptsLast) },
    { L"HtpStat_Recoveries",        FIELD_OFFSET(HTP_STATS, Recoveries) },
    { L"HtpStat_Interrupts",        FIELD_OFFSET(HTP_STATS, Interrupts) },
    { L"HtpStat_InputReads",        FIELD_OFFSET(HTP_STATS, InputReads) },
    { L"HtpStat_ReportsDelivered",  FIELD_OFFSET(HTP_STATS, ReportsDelivered) },
    { L"HtpStat_ReportsQueued",     FIELD_OFFSET(HTP_STATS, ReportsQueued) },
    { L"HtpStat_ReportsDropped",    FIELD_OFFSET(HTP_STATS, ReportsDropped) },
    { L"HtpStat_EmptyReads",        FIELD_OFFSET(HTP_STATS, EmptyReads) },
    { L"HtpStat_BadReports",        FIELD_OFFSET(HTP_STATS, BadReports) },
    { L"HtpStat_DuplicatesDropped", FIELD_OFFSET(HTP_STATS, DuplicatesDropped) },
    { L"HtpStat_FeatureGetOk",      FIELD_OFFSET(HTP_STATS, FeatureGetOk) },
    { L"HtpStat_FeatureGetFail",    FIELD_OFFSET(HTP_STATS, FeatureGetFail) },
    { L"HtpStat_FeatureSetOk",      FIELD_OFFSET(HTP_STATS, FeatureSetOk) },
    { L"HtpStat_FeatureSetFail",    FIELD_OFFSET(HTP_STATS, FeatureSetFail) },
    { L"HtpStat_ReadRequests",      FIELD_OFFSET(HTP_STATS, ReadRequests) },
    { L"HtpStat_HealthChecks",      FIELD_OFFSET(HTP_STATS, HealthChecks) },
    { L"HtpStat_HealthFails",       FIELD_OFFSET(HTP_STATS, HealthFails) },
    { L"HtpStat_PollReads",         FIELD_OFFSET(HTP_STATS, PollReads) },
    { L"HtpStat_LastError",         FIELD_OFFSET(HTP_STATS, LastError) },
};

VOID
HtpLog(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_z_ _Printf_format_string_ PCWSTR Format,
    ...
    )
{
    WCHAR     line[HTP_LOG_CHARS];
    size_t    used = 0;
    va_list   args;
    ULONGLONG ms;
    ULONG     slot;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }

    ms = HtpNowMs();
    if (!NT_SUCCESS(RtlStringCchPrintfW(line, HTP_LOG_CHARS, L"%I64u.%03u  ", ms / 1000, (ULONG)(ms % 1000)))) {
        return;
    }
    if (!NT_SUCCESS(RtlStringCchLengthW(line, HTP_LOG_CHARS, &used)) || used >= HTP_LOG_CHARS - 1) {
        return;
    }

    va_start(args, Format);
    (VOID)RtlStringCchVPrintfW(line + used, HTP_LOG_CHARS - used, Format, args);   /* truncation is fine */
    va_end(args);
    line[HTP_LOG_CHARS - 1] = L'\0';

    WdfSpinLockAcquire(Ctx->LogLock);
    if (Ctx->LogCount < HTP_LOG_LINES) {
        slot = (Ctx->LogHead + Ctx->LogCount) % HTP_LOG_LINES;
        Ctx->LogCount++;
    } else {
        slot = Ctx->LogHead;
        Ctx->LogHead = (Ctx->LogHead + 1) % HTP_LOG_LINES;
    }
    RtlCopyMemory(Ctx->Log[slot], line, sizeof(line));
    WdfSpinLockRelease(Ctx->LogLock);

    InterlockedExchange(&Ctx->DiagDirty, 1);
    KdPrint(("htixtp: %ws\n", line));
}

VOID
HtpSetState(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ HTP_STATE State
    )
{
    Ctx->State = State;
    InterlockedExchange(&Ctx->DiagDirty, 1);
}

static ULONG
HtpQueryULong(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ ULONG  Default,
    _In_ ULONG  Minimum,
    _In_ ULONG  Maximum
    )
{
    UNICODE_STRING name;
    ULONG          value;

    RtlInitUnicodeString(&name, Name);
    if (!NT_SUCCESS(WdfRegistryQueryULong(Key, &name, &value))) {
        return Default;
    }
    if (value < Minimum) {
        value = Minimum;
    }
    if (value > Maximum) {
        value = Maximum;
    }
    return value;
}

VOID
HtpReadSettings(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WDFKEY   key;
    NTSTATUS status;

    /* Defaults */
    Ctx->HidDescAddress       = 0x20;     /* from the laptop's ACPI _DSM for TPD0 */
    Ctx->DescriptorModeConfig = HTP_MODE_AUTO;
    Ctx->PollingConfig        = HTP_POLL_OFF;
    Ctx->PollIntervalMs       = 8;
    Ctx->HealthCheckMs        = 5000;
    Ctx->SleepOnSuspend       = 0;
    Ctx->ResetDelayMs         = 100;
    Ctx->PowerOnDelayMs       = 60;
    Ctx->WakeTimeoutMs        = 4000;
    Ctx->FirstWakeTimeoutMs   = 10000;
    Ctx->AutoMouseFlag        = 0;

    status = WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                      WDF_NO_OBJECT_ATTRIBUTES, &key);
    if (NT_SUCCESS(status)) {
        Ctx->HidDescAddress       = HtpQueryULong(key, L"HtpHidDescAddress",    Ctx->HidDescAddress, 0, 0xFFFF);
        Ctx->DescriptorModeConfig = HtpQueryULong(key, L"HtpDescriptorMode",    Ctx->DescriptorModeConfig, 0, 2);
        Ctx->PollingConfig        = HtpQueryULong(key, L"HtpPolling",           Ctx->PollingConfig, 0, 2);
        Ctx->PollIntervalMs       = HtpQueryULong(key, L"HtpPollIntervalMs",    Ctx->PollIntervalMs, 2, 100);
        Ctx->HealthCheckMs        = HtpQueryULong(key, L"HtpHealthCheckMs",     Ctx->HealthCheckMs, 0, 60000);
        Ctx->SleepOnSuspend       = HtpQueryULong(key, L"HtpSleepOnSuspend",    Ctx->SleepOnSuspend, 0, 1);
        Ctx->ResetDelayMs         = HtpQueryULong(key, L"HtpResetDelayMs",      Ctx->ResetDelayMs, 0, 2000);
        Ctx->PowerOnDelayMs       = HtpQueryULong(key, L"HtpPowerOnDelayMs",    Ctx->PowerOnDelayMs, 0, 2000);
        Ctx->WakeTimeoutMs        = HtpQueryULong(key, L"HtpWakeTimeoutMs",     Ctx->WakeTimeoutMs, 100, 30000);
        Ctx->FirstWakeTimeoutMs   = HtpQueryULong(key, L"HtpFirstWakeTimeoutMs", Ctx->FirstWakeTimeoutMs, 100, 60000);
        Ctx->AutoMouseFlag        = HtpQueryULong(key, L"HtpAutoMouse",         Ctx->AutoMouseFlag, 0, 1);
        WdfRegistryClose(key);
    }

    if (Ctx->HealthCheckMs != 0 && Ctx->HealthCheckMs < 1000) {
        Ctx->HealthCheckMs = 1000;
    }

    HtpLog(Ctx, L"settings: hid-desc 0x%X, mode %u, polling %u (%u ms), health %u ms, sleep %u, reset %u ms, power-on %u ms, wake %u/%u ms, auto-mouse %u",
           Ctx->HidDescAddress, Ctx->DescriptorModeConfig, Ctx->PollingConfig, Ctx->PollIntervalMs,
           Ctx->HealthCheckMs, Ctx->SleepOnSuspend, Ctx->ResetDelayMs, Ctx->PowerOnDelayMs,
           Ctx->WakeTimeoutMs, Ctx->FirstWakeTimeoutMs, Ctx->AutoMouseFlag);
}

VOID
HtpWriteRegistryULong(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ PCWSTR Name,
    _In_ ULONG Value
    )
{
    WDFKEY         key;
    UNICODE_STRING name;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    if (NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                            WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        RtlInitUnicodeString(&name, Name);
        (VOID)WdfRegistryAssignULong(key, &name, Value);
        WdfRegistryClose(key);
    }
}

static VOID
HtpAssignString(
    _In_ WDFKEY Key,
    _In_ PCWSTR Name,
    _In_ PCWSTR Value
    )
{
    UNICODE_STRING name;

    RtlInitUnicodeString(&name, Name);
    (VOID)WdfRegistryAssignValue(Key, &name, REG_SZ, (ULONG)((wcslen(Value) + 1) * sizeof(WCHAR)), (PVOID)Value);
}

static PCWSTR
HtpStateName(
    _In_ HTP_STATE State
    )
{
    switch (State) {
    case HtpStateReady:      return L"Ready";
    case HtpStateRecovering: return L"Recovering";
    case HtpStateNoResponse: return L"NoResponse";
    default:                 return L"Stopped";
    }
}

VOID
HtpWriteDiagnostics(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WDFKEY         key;
    UNICODE_STRING name;
    ULONG          i;
    PWCHAR         multi;
    SIZE_T         multiChars = (SIZE_T)HTP_LOG_LINES * HTP_LOG_CHARS + 2;
    SIZE_T         pos = 0;
    WCHAR          input[64];

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        return;
    }
    InterlockedExchange(&Ctx->DiagDirty, 0);

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    HtpAssignString(key, L"HtpVersion", HTP_VERSION_STRING);
    HtpAssignString(key, L"HtpState", HtpStateName(Ctx->State));
    HtpAssignString(key, L"HtpMode",
                    !Ctx->DescriptorsValid ? L"none" :
                    (Ctx->PresentedMode == HTP_MODE_MOUSE ? L"mouse" : L"precision-touchpad"));

    (VOID)RtlStringCchPrintfW(input, ARRAYSIZE(input), L"%s%s%s",
                              Ctx->Interrupt != NULL ? L"interrupt" : L"",
                              (Ctx->Interrupt != NULL && Ctx->PollingActive) ? L"+" : L"",
                              Ctx->PollingActive ? L"polling" : L"");
    HtpAssignString(key, L"HtpInput", input);

    for (i = 0; i < ARRAYSIZE(g_StatNames); i++) {
        LONG value = *(volatile LONG*)((PUCHAR)&Ctx->Stats + g_StatNames[i].Offset);
        RtlInitUnicodeString(&name, g_StatNames[i].Name);
        (VOID)WdfRegistryAssignULong(key, &name, (ULONG)value);
    }

    RtlInitUnicodeString(&name, L"HtpChipVidPid");
    (VOID)WdfRegistryAssignULong(key, &name, ((ULONG)Ctx->VendorId << 16) | Ctx->ProductId);
    RtlInitUnicodeString(&name, L"HtpChipVersion");
    (VOID)WdfRegistryAssignULong(key, &name, Ctx->VersionId);
    RtlInitUnicodeString(&name, L"HtpReportDescLength");
    (VOID)WdfRegistryAssignULong(key, &name, Ctx->DevReportDescLength);
    RtlInitUnicodeString(&name, L"HtpPtpActivated");
    (VOID)WdfRegistryAssignULong(key, &name, Ctx->PtpActivated ? 1 : 0);

    /* Log as REG_MULTI_SZ, oldest line first. */
    multi = (PWCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, multiChars * sizeof(WCHAR), HTP_POOL_TAG);
    if (multi != NULL) {
        WdfSpinLockAcquire(Ctx->LogLock);
        for (i = 0; i < Ctx->LogCount; i++) {
            ULONG  slot = (Ctx->LogHead + i) % HTP_LOG_LINES;
            size_t length = 0;

            if (NT_SUCCESS(RtlStringCchLengthW(Ctx->Log[slot], HTP_LOG_CHARS, &length)) && length > 0 &&
                pos + length + 2 < multiChars) {
                RtlCopyMemory(multi + pos, Ctx->Log[slot], length * sizeof(WCHAR));
                pos += length;
                multi[pos++] = L'\0';
            }
        }
        WdfSpinLockRelease(Ctx->LogLock);

        if (pos == 0) {
            multi[pos++] = L'\0';
        }
        multi[pos++] = L'\0';

        RtlInitUnicodeString(&name, L"HtpLog");
        (VOID)WdfRegistryAssignValue(key, &name, REG_MULTI_SZ, (ULONG)(pos * sizeof(WCHAR)), multi);
        ExFreePoolWithTag(multi, HTP_POOL_TAG);
    }

    WdfRegistryClose(key);
}

VOID
HtpLoadDescriptorCache(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WDFKEY         key;
    UNICODE_STRING name;
    ULONG          length = 0;
    ULONG          type = 0;
    NTSTATUS       status;

    Ctx->CacheLoaded = FALSE;
    Ctx->CacheReportDescLength = 0;

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_READ,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&name, L"HtpCacheHidDesc");
    status = WdfRegistryQueryValue(key, &name, HTP_HID_DESC_LENGTH, Ctx->CacheHidDesc, &length, &type);
    if (NT_SUCCESS(status) && type == REG_BINARY && length == HTP_HID_DESC_LENGTH &&
        HtpParseHidDescriptor(Ctx, Ctx->CacheHidDesc, FALSE)) {

        ULONG expected = Ctx->CacheHidDesc[4] | ((ULONG)Ctx->CacheHidDesc[5] << 8);

        RtlInitUnicodeString(&name, L"HtpCacheReportDesc");
        length = 0;
        status = WdfRegistryQueryValue(key, &name, HTP_MAX_REPORT_DESC, Ctx->CacheReportDesc, &length, &type);
        if (NT_SUCCESS(status) && type == REG_BINARY && length == expected) {
            Ctx->CacheReportDescLength = length;
            Ctx->CacheLoaded = TRUE;
        }
    }

    WdfRegistryClose(key);
    HtpLog(Ctx, L"saved descriptor: %s", Ctx->CacheLoaded ? L"present" : L"none");
}

VOID
HtpSaveDescriptorCache(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    WDFKEY         key;
    UNICODE_STRING name;

    if (Ctx->DevReportDescLength == 0) {
        return;
    }
    if (Ctx->CacheLoaded &&
        Ctx->CacheReportDescLength == Ctx->DevReportDescLength &&
        RtlEqualMemory(Ctx->CacheReportDesc, Ctx->DevReportDesc, Ctx->DevReportDescLength) &&
        RtlEqualMemory(Ctx->CacheHidDesc, Ctx->DevHidDesc, HTP_HID_DESC_LENGTH)) {
        return;
    }

    if (!NT_SUCCESS(WdfDeviceOpenRegistryKey(Ctx->Device, PLUGPLAY_REGKEY_DEVICE, KEY_SET_VALUE,
                                             WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }

    RtlInitUnicodeString(&name, L"HtpCacheHidDesc");
    (VOID)WdfRegistryAssignValue(key, &name, REG_BINARY, HTP_HID_DESC_LENGTH, Ctx->DevHidDesc);
    RtlInitUnicodeString(&name, L"HtpCacheReportDesc");
    (VOID)WdfRegistryAssignValue(key, &name, REG_BINARY, Ctx->DevReportDescLength, Ctx->DevReportDesc);
    WdfRegistryClose(key);

    RtlCopyMemory(Ctx->CacheHidDesc, Ctx->DevHidDesc, HTP_HID_DESC_LENGTH);
    RtlCopyMemory(Ctx->CacheReportDesc, Ctx->DevReportDesc, Ctx->DevReportDescLength);
    Ctx->CacheReportDescLength = Ctx->DevReportDescLength;
    Ctx->CacheLoaded = TRUE;
    HtpLog(Ctx, L"saved the chip's descriptors for future starts");
}
