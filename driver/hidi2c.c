/*
 * hidi2c.c - HID over I2C protocol for this chip, with the quirks Linux uses
 * for Hantick 0911:5288 plus extra robustness:
 *
 *   - every transfer goes through one wait lock and has a 250 ms timeout;
 *   - the chip is woken with repeated register reads and an escalating delay
 *     (it NACKs while asleep; Windows' hidi2c gives up after one try);
 *   - RESET is followed by a fixed delay instead of waiting for the reset
 *     interrupt, which this chip never sends (Linux I2C_HID_QUIRK_NO_IRQ_AFTER_RESET);
 *   - SET_POWER(SLEEP) is never sent while the system runs;
 *   - SET_POWER(ON) is retried and followed by a settle delay (as Linux does).
 */

#include "htixtp.h"

#define HTP_XFER_TIMEOUT_MS     250

static const ULONG g_WakeDelaysMs[] = { 0, 1, 1, 2, 4, 8, 15, 30, 50, 100, 150, 200, 300 };

ULONGLONG
HtpNowMs(VOID)
{
    return KeQueryInterruptTime() / 10000ULL;
}

VOID
HtpSleepMs(
    _In_ ULONG Milliseconds
    )
{
    LARGE_INTEGER interval;

    if (Milliseconds == 0) {
        return;
    }
    interval.QuadPart = -10000LL * (LONGLONG)Milliseconds;
    KeDelayExecutionThread(KernelMode, FALSE, &interval);
}

VOID
HtpRequestMaintenance(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG DelayMs
    )
{
    if (Ctx->InD0) {
        WdfTimerStart(Ctx->MaintTimer, WDF_REL_TIMEOUT_IN_MS(DelayMs));
    }
}

VOID
HtpNoteFailure(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ NTSTATUS Status
    )
{
    LONG failures = InterlockedIncrement(&Ctx->ConsecutiveFailures);

    if (failures == 1) {
        HtpLog(Ctx, L"I2C transfer failed: 0x%08X", (ULONG)Status);
    }

    if (failures >= 3 && InterlockedCompareExchange(&Ctx->DeviceReady, 0, 1) == 1) {
        HtpLog(Ctx, L"chip stopped answering after %d failed transfers (0x%08X); recovering",
               failures, (ULONG)Status);
        InterlockedExchange(&Ctx->RecoveryRequested, 1);
        HtpRequestMaintenance(Ctx, 10);
    }
}

/* ------------------------------------------------------------------------ */
/* Raw SPB transfers (caller holds I2cLock, PASSIVE_LEVEL)                    */
/* ------------------------------------------------------------------------ */

static NTSTATUS
HtpSpbTransferLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_opt_(WriteLength) PUCHAR Write,
    _In_ ULONG WriteLength,
    _Out_writes_bytes_opt_(ReadLength) PUCHAR Read,
    _In_ ULONG ReadLength,
    _Out_opt_ PULONG BytesRead
    )
{
    NTSTATUS                 status;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_MEMORY_DESCRIPTOR    descriptor;
    ULONG_PTR                bytes = 0;
    ULONG                    readBytes = 0;

    if (BytesRead != NULL) {
        *BytesRead = 0;
    }

    if (!Ctx->SpbOpen) {
        return STATUS_DEVICE_NOT_READY;
    }

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(HTP_XFER_TIMEOUT_MS));

    if (WriteLength != 0 && ReadLength != 0 && Write != NULL && Read != NULL) {
        /* Register read: write the register address, repeated start, read. */
        SPB_TRANSFER_LIST_AND_ENTRIES(2) sequence;

        SPB_TRANSFER_LIST_INIT(&(sequence.List), 2);
        {
            ULONG index = 0;
            sequence.List.Transfers[index] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
                SpbTransferDirectionToDevice, 0, Write, WriteLength);
            sequence.List.Transfers[index + 1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(
                SpbTransferDirectionFromDevice, 0, Read, ReadLength);
        }

        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, &sequence, sizeof(sequence));
        status = WdfIoTargetSendIoctlSynchronously(Ctx->SpbTarget, NULL, IOCTL_SPB_EXECUTE_SEQUENCE,
                                                   &descriptor, NULL, &options, &bytes);
        if (NT_SUCCESS(status)) {
            /* Information is the total byte count; accept controllers that report only the read part. */
            if (bytes == 0 || bytes >= (ULONG_PTR)WriteLength + ReadLength || bytes == ReadLength) {
                readBytes = ReadLength;
            } else if (bytes > WriteLength) {
                readBytes = (ULONG)(bytes - WriteLength);
            }
        }
    } else if (WriteLength != 0 && Write != NULL) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, Write, WriteLength);
        status = WdfIoTargetSendWriteSynchronously(Ctx->SpbTarget, NULL, &descriptor, NULL, &options, &bytes);
    } else if (ReadLength != 0 && Read != NULL) {
        WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&descriptor, Read, ReadLength);
        status = WdfIoTargetSendReadSynchronously(Ctx->SpbTarget, NULL, &descriptor, NULL, &options, &bytes);
        if (NT_SUCCESS(status)) {
            readBytes = (bytes == 0) ? ReadLength : (ULONG)min(bytes, (ULONG_PTR)ReadLength);
        }
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Ctx->Stats.I2cOk);
        Ctx->LastGoodXferMs = HtpNowMs();
        if (BytesRead != NULL) {
            *BytesRead = readBytes;
        }
    } else {
        InterlockedIncrement(&Ctx->Stats.I2cFail);
        InterlockedExchange(&Ctx->Stats.LastError, (LONG)status);
    }
    return status;
}

static __forceinline VOID
HtpPut16(
    _Out_writes_bytes_(2) PUCHAR Buffer,
    _In_ ULONG Value
    )
{
    Buffer[0] = (UCHAR)(Value & 0xFF);
    Buffer[1] = (UCHAR)((Value >> 8) & 0xFF);
}

/* Command register + opcode (+ report ID byte for IDs >= 15). Returns bytes written. */
static ULONG
HtpEncodeCommand(
    _Out_writes_bytes_(5) PUCHAR Buffer,
    _In_ USHORT CommandRegister,
    _In_ UCHAR Opcode,
    _In_ UCHAR ReportType,
    _In_ UCHAR ReportId
    )
{
    ULONG n = 0;

    HtpPut16(Buffer, CommandRegister);
    n = 2;
    if (ReportId < 0x0F) {
        Buffer[n++] = (UCHAR)((ReportType << 4) | ReportId);
        Buffer[n++] = Opcode;
    } else {
        Buffer[n++] = (UCHAR)((ReportType << 4) | 0x0F);
        Buffer[n++] = Opcode;
        Buffer[n++] = ReportId;
    }
    return n;
}

/* ------------------------------------------------------------------------ */
/* Chip bring-up (caller holds I2cLock)                                       */
/* ------------------------------------------------------------------------ */

static NTSTATUS
HtpReadHidDescriptorLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _Out_writes_bytes_(HTP_HID_DESC_LENGTH) PUCHAR Out
    )
{
    NTSTATUS status;
    ULONG    got = 0;

    HtpPut16(Ctx->CmdBuffer, Ctx->HidDescAddress);
    status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, 2, Ctx->XferBuffer, HTP_HID_DESC_LENGTH, &got);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (got < HTP_HID_DESC_LENGTH) {
        return STATUS_DEVICE_DATA_ERROR;
    }
    RtlCopyMemory(Out, Ctx->XferBuffer, HTP_HID_DESC_LENGTH);
    return STATUS_SUCCESS;
}

static NTSTATUS
HtpSetPowerLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UCHAR PowerState
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG    attempt;
    ULONG    length;

    if (Ctx->CommandRegister == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    for (attempt = 0; attempt < 5; attempt++) {
        length = HtpEncodeCommand(Ctx->CmdBuffer, Ctx->CommandRegister, HTP_OP_SET_POWER, 0, PowerState);
        status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, length, NULL, 0, NULL);
        if (NT_SUCCESS(status)) {
            break;
        }
        /* Some chips NACK the first command while waking; Linux retries after ~400 us. */
        HtpSleepMs(1 + attempt * 2);
    }

    if (NT_SUCCESS(status) && PowerState == HTP_POWER_ON) {
        HtpSleepMs(Ctx->PowerOnDelayMs);
    }
    return status;
}

static NTSTATUS
HtpWakeLocked(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG TimeoutMs,
    _In_ BOOLEAN AllowAbort,
    _Out_writes_bytes_(HTP_HID_DESC_LENGTH) PUCHAR Descriptor
    )
{
    NTSTATUS  status = STATUS_DEVICE_NOT_CONNECTED;
    ULONGLONG start = HtpNowMs();
    ULONG     attempt = 0;

    for (;;) {
        ULONG delay = (attempt < ARRAYSIZE(g_WakeDelaysMs)) ? g_WakeDelaysMs[attempt] : 500;

        HtpSleepMs(delay);
        if (AllowAbort && !Ctx->InD0) {
            status = STATUS_CANCELLED;
            break;
        }

        status = HtpReadHidDescriptorLocked(Ctx, Descriptor);
        attempt++;

        if (NT_SUCCESS(status)) {
            if (HtpParseHidDescriptor(Ctx, Descriptor, FALSE)) {
                break;
            }
            status = STATUS_DEVICE_DATA_ERROR;
        }

        if (attempt <= 3 || (attempt % 10) == 0) {
            HtpLog(Ctx, L"wake attempt %u: 0x%08X", attempt, (ULONG)status);
        }

        /* If the command register is known, also send SET_POWER(ON): some chips wake on any command. */
        if ((attempt & 1) && Ctx->CommandRegister != 0) {
            ULONG length = HtpEncodeCommand(Ctx->CmdBuffer, Ctx->CommandRegister, HTP_OP_SET_POWER, 0, HTP_POWER_ON);
            (VOID)HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, length, NULL, 0, NULL);
        }

        if ((HtpNowMs() - start) >= TimeoutMs) {
            break;
        }
    }

    InterlockedExchange(&Ctx->Stats.WakeAttemptsLast, (LONG)attempt);
    if (NT_SUCCESS(status) && attempt > 1) {
        HtpLog(Ctx, L"chip answered after %u attempts (%I64u ms)", attempt, HtpNowMs() - start);
    }
    return status;
}

static NTSTATUS
HtpResetLocked(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG    attempt;
    ULONG    length;
    ULONG    readLength;
    ULONG    got = 0;

    for (attempt = 0; attempt < 3; attempt++) {
        length = HtpEncodeCommand(Ctx->CmdBuffer, Ctx->CommandRegister, HTP_OP_RESET, 0, 0);
        status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, length, NULL, 0, NULL);
        if (NT_SUCCESS(status)) {
            break;
        }
        HtpSleepMs(2 + attempt * 3);
    }
    if (!NT_SUCCESS(status)) {
        HtpLog(Ctx, L"RESET command failed: 0x%08X", (ULONG)status);
        return status;
    }

    /* This chip sends no interrupt after reset: wait a fixed time instead (Linux: 100 ms). */
    HtpSleepMs(Ctx->ResetDelayMs);

    /* Drain the input register (the reset response, if the chip queued one). */
    readLength = min((ULONG)Ctx->MaxInputLength, (ULONG)HTP_MAX_INPUT_LENGTH);
    if (readLength >= 2) {
        NTSTATUS drain = HtpSpbTransferLocked(Ctx, NULL, 0, Ctx->XferBuffer, readLength, &got);
        if (NT_SUCCESS(drain) && got >= 2) {
            ULONG field = Ctx->XferBuffer[0] | ((ULONG)Ctx->XferBuffer[1] << 8);
            HtpLog(Ctx, L"reset done (input register length field %u)", field);
        } else {
            HtpLog(Ctx, L"reset done (drain read: 0x%08X)", (ULONG)drain);
        }
    }
    return STATUS_SUCCESS;
}

static NTSTATUS
HtpReadReportDescriptorLocked(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG    attempt;
    ULONG    got = 0;
    ULONG    length = Ctx->ReportDescLengthDev;

    if (length == 0 || length > HTP_MAX_REPORT_DESC) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    for (attempt = 0; attempt < 4; attempt++) {
        HtpPut16(Ctx->CmdBuffer, Ctx->ReportDescRegister);
        status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, 2, Ctx->XferBuffer, length, &got);
        if (NT_SUCCESS(status)) {
            if (got >= length) {
                RtlCopyMemory(Ctx->DevReportDesc, Ctx->XferBuffer, length);
                Ctx->DevReportDescLength   = length;
                Ctx->DevReportDescChecksum = HtpChecksum(Ctx->DevReportDesc, length);
                return STATUS_SUCCESS;
            }
            status = STATUS_DEVICE_DATA_ERROR;
        }
        HtpSleepMs(10 * (attempt + 1));
    }

    HtpLog(Ctx, L"reading the report descriptor failed: 0x%08X", (ULONG)status);
    return status;
}

/*
 * Full bring-up: wake, SET_POWER(ON), RESET (no IRQ wait), SET_POWER(ON),
 * read the report descriptor. Holds the I2C lock for the whole sequence.
 */
NTSTATUS
HtpInitializeChip(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ ULONG WakeTimeoutMs,
    _In_ BOOLEAN AllowAbort
    )
{
    NTSTATUS status;
    UCHAR    descriptor[HTP_HID_DESC_LENGTH];

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);

    status = HtpWakeLocked(Ctx, WakeTimeoutMs, AllowAbort, descriptor);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(Ctx->DevHidDesc, descriptor, HTP_HID_DESC_LENGTH);
        (VOID)HtpParseHidDescriptor(Ctx, descriptor, TRUE);

        status = HtpSetPowerLocked(Ctx, HTP_POWER_ON);
        if (!NT_SUCCESS(status)) {
            HtpLog(Ctx, L"SET_POWER(ON) failed: 0x%08X", (ULONG)status);
        }
    }

    if (NT_SUCCESS(status)) {
        status = HtpResetLocked(Ctx);
    }

    if (NT_SUCCESS(status)) {
        /* Linux sends SET_POWER(ON) again after reset; failure here is tolerated. */
        (VOID)HtpSetPowerLocked(Ctx, HTP_POWER_ON);
        status = HtpReadReportDescriptorLocked(Ctx);
    }

    WdfWaitLockRelease(Ctx->I2cLock);

    if (NT_SUCCESS(status)) {
        InterlockedIncrement(&Ctx->Stats.InitOk);
        HtpLog(Ctx, L"chip ready: VID %04X PID %04X version %04X, report descriptor %u bytes, max input %u",
               Ctx->VendorId, Ctx->ProductId, Ctx->VersionId, Ctx->DevReportDescLength, Ctx->MaxInputLength);
    } else {
        InterlockedIncrement(&Ctx->Stats.InitFail);
        InterlockedExchange(&Ctx->Stats.LastError, (LONG)status);
    }
    return status;
}

/* Cheap liveness check: read the HID descriptor again (retry once, like Linux's probe). */
NTSTATUS
HtpProbeChip(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    NTSTATUS status;
    UCHAR    descriptor[HTP_HID_DESC_LENGTH];

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);
    status = HtpReadHidDescriptorLocked(Ctx, descriptor);
    if (!NT_SUCCESS(status)) {
        HtpSleepMs(1);
        status = HtpReadHidDescriptorLocked(Ctx, descriptor);
    }
    WdfWaitLockRelease(Ctx->I2cLock);

    if (NT_SUCCESS(status) && !RtlEqualMemory(descriptor, Ctx->DevHidDesc, HTP_HID_DESC_LENGTH)) {
        HtpLog(Ctx, L"health check: HID descriptor changed (chip reset itself?)");
        status = STATUS_DEVICE_DATA_ERROR;
    }
    return status;
}

NTSTATUS
HtpSetPower(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UCHAR PowerState
    )
{
    NTSTATUS status;

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);
    status = HtpSetPowerLocked(Ctx, PowerState);
    WdfWaitLockRelease(Ctx->I2cLock);
    return status;
}

/* ------------------------------------------------------------------------ */
/* Reports                                                                   */
/* ------------------------------------------------------------------------ */

NTSTATUS
HtpGetReport(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UCHAR ReportType,
    _In_ UCHAR ReportId,
    _Out_writes_bytes_(BufferLength) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PULONG BytesReturned
    )
{
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    ULONG    readLength;
    ULONG    commandLength;
    ULONG    got = 0;
    ULONG    attempt;

    *BytesReturned = 0;

    if (BufferLength == 0 || (ReportId == 0 && BufferLength < 2)) {
        return STATUS_INVALID_PARAMETER;
    }

    /* Response: 2-byte length, then the report (with its ID byte when ID != 0). */
    readLength = (ReportId != 0) ? (BufferLength + 2) : (BufferLength + 1);
    if (readLength > HTP_XFER_BUFFER_SIZE) {
        readLength = HTP_XFER_BUFFER_SIZE;
    }

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);

    for (attempt = 0; attempt < 3; attempt++) {
        commandLength = HtpEncodeCommand(Ctx->CmdBuffer, Ctx->CommandRegister, HTP_OP_GET_REPORT, ReportType, ReportId);
        HtpPut16(Ctx->CmdBuffer + commandLength, Ctx->DataRegister);
        commandLength += 2;

        status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, commandLength, Ctx->XferBuffer, readLength, &got);
        if (NT_SUCCESS(status)) {
            break;
        }
        HtpSleepMs(1 + attempt);
    }

    if (NT_SUCCESS(status)) {
        ULONG responseLength = Ctx->XferBuffer[0] | ((ULONG)Ctx->XferBuffer[1] << 8);
        ULONG available;

        if (responseLength <= 2 || responseLength == 0xFFFF || got <= 2) {
            status = STATUS_DEVICE_DATA_ERROR;
        } else {
            available = min(responseLength, got) - 2;
            if (ReportId != 0) {
                ULONG copy = min(available, BufferLength);
                RtlCopyMemory(Buffer, Ctx->XferBuffer + 2, copy);
                if (Buffer[0] != ReportId) {
                    status = STATUS_DEVICE_DATA_ERROR;
                } else {
                    *BytesReturned = copy;
                }
            } else {
                ULONG copy = min(available, BufferLength - 1);
                Buffer[0] = 0;
                RtlCopyMemory(Buffer + 1, Ctx->XferBuffer + 2, copy);
                *BytesReturned = copy + 1;
            }
        }
    }

    WdfWaitLockRelease(Ctx->I2cLock);

    if (!NT_SUCCESS(status) && status != STATUS_DEVICE_DATA_ERROR) {
        HtpNoteFailure(Ctx, status);
    } else if (NT_SUCCESS(status)) {
        InterlockedExchange(&Ctx->ConsecutiveFailures, 0);
    }
    return status;
}

/* Strip the placeholder report ID byte Windows puts in front when the ID is 0. */
static NTSTATUS
HtpPayload(
    _In_ UCHAR ReportId,
    _In_reads_bytes_(BufferLength) PUCHAR Buffer,
    _In_ ULONG BufferLength,
    _Out_ PUCHAR* Payload,
    _Out_ PULONG PayloadLength
    )
{
    if (ReportId == 0) {
        if (BufferLength < 1) {
            return STATUS_INVALID_PARAMETER;
        }
        *Payload = Buffer + 1;
        *PayloadLength = BufferLength - 1;
    } else {
        *Payload = Buffer;
        *PayloadLength = BufferLength;
    }
    return STATUS_SUCCESS;
}

NTSTATUS
HtpSetReport(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ UCHAR ReportType,
    _In_ UCHAR ReportId,
    _In_reads_bytes_(BufferLength) PUCHAR Buffer,
    _In_ ULONG BufferLength
    )
{
    NTSTATUS status;
    PUCHAR   payload;
    ULONG    payloadLength;
    ULONG    n;
    ULONG    attempt;

    status = HtpPayload(ReportId, Buffer, BufferLength, &payload, &payloadLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (payloadLength + 16 > HTP_XFER_BUFFER_SIZE) {
        return STATUS_BUFFER_OVERFLOW;
    }

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);

    for (attempt = 0; attempt < 3; attempt++) {
        n = HtpEncodeCommand(Ctx->CmdBuffer, Ctx->CommandRegister, HTP_OP_SET_REPORT, ReportType, ReportId);
        HtpPut16(Ctx->CmdBuffer + n, Ctx->DataRegister);
        n += 2;
        HtpPut16(Ctx->CmdBuffer + n, payloadLength + 2);
        n += 2;
        RtlCopyMemory(Ctx->CmdBuffer + n, payload, payloadLength);
        n += payloadLength;

        status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, n, NULL, 0, NULL);
        if (NT_SUCCESS(status)) {
            break;
        }
        HtpSleepMs(1 + attempt);
    }

    WdfWaitLockRelease(Ctx->I2cLock);

    if (!NT_SUCCESS(status)) {
        HtpNoteFailure(Ctx, status);
    } else {
        InterlockedExchange(&Ctx->ConsecutiveFailures, 0);
    }
    return status;
}

NTSTATUS
HtpWriteOutputReport(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(BufferLength) PUCHAR Buffer,
    _In_ ULONG BufferLength
    )
{
    NTSTATUS status;
    PUCHAR   payload;
    ULONG    payloadLength;
    ULONG    n = 0;
    UCHAR    reportId;

    if (BufferLength == 0) {
        return STATUS_INVALID_PARAMETER;
    }
    reportId = Buffer[0];

    if (Ctx->OutputRegister == 0 || Ctx->MaxOutputLength < 2) {
        return HtpSetReport(Ctx, HTP_REPORT_TYPE_OUTPUT, reportId, Buffer, BufferLength);
    }

    status = HtpPayload(reportId, Buffer, BufferLength, &payload, &payloadLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (payloadLength + 8 > HTP_XFER_BUFFER_SIZE) {
        return STATUS_BUFFER_OVERFLOW;
    }

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);
    HtpPut16(Ctx->CmdBuffer, Ctx->OutputRegister);
    n = 2;
    HtpPut16(Ctx->CmdBuffer + n, payloadLength + 2);
    n += 2;
    RtlCopyMemory(Ctx->CmdBuffer + n, payload, payloadLength);
    n += payloadLength;
    status = HtpSpbTransferLocked(Ctx, Ctx->CmdBuffer, n, NULL, 0, NULL);
    WdfWaitLockRelease(Ctx->I2cLock);

    if (!NT_SUCCESS(status)) {
        HtpNoteFailure(Ctx, status);
    }
    return status;
}

/* Read one input report from the chip (after an interrupt, or on a poll). */
VOID
HtpServiceInput(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ BOOLEAN FromPoll
    )
{
    UCHAR    report[HTP_MAX_INPUT_LENGTH];
    ULONG    readLength;
    ULONG    got = 0;
    ULONG    length;
    ULONG    payloadLength;
    PUCHAR   payload;
    NTSTATUS status;

    readLength = min((ULONG)Ctx->MaxInputLength, (ULONG)HTP_MAX_INPUT_LENGTH);
    if (readLength < 2) {
        return;
    }

    WdfWaitLockAcquire(Ctx->I2cLock, NULL);
    if (!Ctx->DeviceReady) {
        WdfWaitLockRelease(Ctx->I2cLock);
        return;
    }
    status = HtpSpbTransferLocked(Ctx, NULL, 0, Ctx->XferBuffer, readLength, &got);
    if (NT_SUCCESS(status)) {
        RtlCopyMemory(report, Ctx->XferBuffer, readLength);
    }
    WdfWaitLockRelease(Ctx->I2cLock);

    InterlockedIncrement(&Ctx->Stats.InputReads);

    if (!NT_SUCCESS(status)) {
        HtpNoteFailure(Ctx, status);
        if (!FromPoll) {
            HtpSleepMs(2);
        }
        return;
    }
    InterlockedExchange(&Ctx->ConsecutiveFailures, 0);

    length = report[0] | ((ULONG)report[1] << 8);
    if (length == 0 || length == 0xFFFF || length == 2) {
        /* Reset response or "no data". */
        InterlockedIncrement(&Ctx->Stats.EmptyReads);
        return;
    }

    if (length < 2 || length > readLength || got < length) {
        LONG bad = InterlockedIncrement(&Ctx->ConsecutiveBadReports);
        InterlockedIncrement(&Ctx->Stats.BadReports);
        if (bad == 1) {
            HtpLog(Ctx, L"malformed input report (length field %u, max %u)", length, readLength);
        }
        if (bad >= 20 && InterlockedCompareExchange(&Ctx->DeviceReady, 0, 1) == 1) {
            HtpLog(Ctx, L"chip keeps sending malformed reports; resetting it");
            InterlockedExchange(&Ctx->RecoveryRequested, 1);
            HtpRequestMaintenance(Ctx, 10);
        }
        return;
    }
    InterlockedExchange(&Ctx->ConsecutiveBadReports, 0);

    payload = report + 2;
    payloadLength = length - 2;

    if (Ctx->PresentedMode == HTP_MODE_MOUSE && Ctx->DescInfo.UsesReportIds &&
        payload[0] != Ctx->DescInfo.MouseReportId) {
        InterlockedIncrement(&Ctx->Stats.ReportsDropped);
        return;
    }

    if (FromPoll) {
        if (payloadLength == Ctx->LastPolledLength && RtlEqualMemory(payload, Ctx->LastPolled, payloadLength)) {
            InterlockedIncrement(&Ctx->Stats.DuplicatesDropped);
            return;
        }
        RtlCopyMemory(Ctx->LastPolled, payload, payloadLength);
        Ctx->LastPolledLength = payloadLength;
    }

    if (Ctx->Stats.ReportsDelivered == 0 && Ctx->Stats.ReportsQueued == 0) {
        HtpLog(Ctx, L"first input report: id %u, %u bytes, %02X %02X %02X %02X %02X %02X",
               (ULONG)payload[0], payloadLength,
               payloadLength > 1 ? payload[1] : 0, payloadLength > 2 ? payload[2] : 0,
               payloadLength > 3 ? payload[3] : 0, payloadLength > 4 ? payload[4] : 0,
               payloadLength > 5 ? payload[5] : 0, payloadLength > 6 ? payload[6] : 0);
    }

    HtpDeliverReport(Ctx, payload, payloadLength);
}

/* Re-apply the feature reports Windows set (input mode etc.) after the chip was reset. */
VOID
HtpReplaySavedFeatures(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    HTP_SAVED_FEATURE saved[HTP_MAX_SAVED_FEATURES];
    ULONG             count;
    ULONG             i;

    WdfSpinLockAcquire(Ctx->SavedFeatureLock);
    count = Ctx->SavedFeatureCount;
    RtlCopyMemory(saved, Ctx->SavedFeatures, sizeof(HTP_SAVED_FEATURE) * count);
    WdfSpinLockRelease(Ctx->SavedFeatureLock);

    for (i = 0; i < count; i++) {
        NTSTATUS status = HtpSetReport(Ctx, HTP_REPORT_TYPE_FEATURE, saved[i].ReportId,
                                       saved[i].Data, saved[i].Length);
        HtpLog(Ctx, L"re-applied feature report %u: 0x%08X", (ULONG)saved[i].ReportId, (ULONG)status);
    }
}

/* Mouse mode: make sure the chip reports through its mouse collection (Input Mode = 0). */
VOID
HtpApplyMouseMode(
    _In_ PDEVICE_CONTEXT Ctx
    )
{
    HTP_DESC_INFO* info = &Ctx->DescInfo;
    UCHAR          report[HTP_MAX_SAVED_FEATURE_LEN];
    ULONG          length;
    NTSTATUS       status;

    if (!info->HasInputMode) {
        HtpLog(Ctx, L"mouse mode: no input-mode feature; relying on the chip's power-on default (mouse)");
        return;
    }

    length = info->InputModeReportLength;
    if (length < 2 || length > sizeof(report)) {
        return;
    }

    /*
     * Force Device Mode = 0 (mouse) with a single write and NO feature read.
     * On this chip a feature GET - above all the 256-byte PTP certification
     * report (report 6) that the Windows precision-touchpad stack reads during
     * bring-up - hangs the firmware: it keeps answering plain input reads but
     * NACKs every command until its power is physically cut. That is exactly
     * why Windows (which runs that certification) kills it and Linux (which
     * treats it as a multitouch mouse and never reads report 6) does not.
     * Writing Device Mode 0 is small and safe; the chip already powers on in
     * mouse mode, so even a failed write is non-fatal.
     */
    RtlZeroMemory(report, sizeof(report));
    report[0] = info->InputModeReportId;
    status = HtpSetReport(Ctx, HTP_REPORT_TYPE_FEATURE, info->InputModeReportId, report, length);
    HtpLog(Ctx, L"mouse mode: Device Mode=0 (report %u): 0x%08X", (ULONG)info->InputModeReportId, (ULONG)status);

    /* A failed mode write must not cascade into recovery - the default is mouse anyway. */
    InterlockedExchange(&Ctx->ConsecutiveFailures, 0);
    InterlockedExchange(&Ctx->DeviceReady, 1);
}
