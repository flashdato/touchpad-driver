/*
 * descriptor.c - HID descriptor validation, report descriptor parsing, and the
 * descriptor this driver presents to HIDClass:
 *
 *   full  : the chip's own descriptor (precision touchpad + mouse + config)
 *   mouse : only the chip's mouse top-level collection, for when Windows does
 *           not accept the device as a precision touchpad
 */

#include "htixtp.h"

static __forceinline USHORT
HtpGet16(
    _In_reads_bytes_(2) const UCHAR* Buffer
    )
{
    return (USHORT)(Buffer[0] | (Buffer[1] << 8));
}

ULONG
HtpChecksum(
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
    )
{
    ULONG hash = 2166136261u;   /* FNV-1a */
    ULONG i;

    for (i = 0; i < Length; i++) {
        hash ^= Data[i];
        hash *= 16777619u;
    }
    return hash ^ Length;
}

BOOLEAN
HtpParseHidDescriptor(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(HTP_HID_DESC_LENGTH) const UCHAR* Raw,
    _In_ BOOLEAN Apply
    )
{
    USHORT descLength      = HtpGet16(Raw + 0);
    USHORT bcdVersion      = HtpGet16(Raw + 2);
    USHORT reportDescLen   = HtpGet16(Raw + 4);
    USHORT reportDescReg   = HtpGet16(Raw + 6);
    USHORT inputReg        = HtpGet16(Raw + 8);
    USHORT maxInput        = HtpGet16(Raw + 10);
    USHORT outputReg       = HtpGet16(Raw + 12);
    USHORT maxOutput       = HtpGet16(Raw + 14);
    USHORT commandReg      = HtpGet16(Raw + 16);
    USHORT dataReg         = HtpGet16(Raw + 18);
    USHORT vendorId        = HtpGet16(Raw + 20);
    USHORT productId       = HtpGet16(Raw + 22);
    USHORT versionId       = HtpGet16(Raw + 24);

    /* Same checks as Linux i2c-hid: wHIDDescLength must be 30, bcdVersion 1.00. */
    if (descLength != HTP_HID_DESC_LENGTH || bcdVersion != 0x0100) {
        return FALSE;
    }
    if (reportDescLen == 0 || reportDescLen > HTP_MAX_REPORT_DESC) {
        return FALSE;
    }
    if (maxInput < 2 || commandReg == 0 || dataReg == 0) {
        return FALSE;
    }

    if (Apply) {
        Ctx->ReportDescLengthDev = reportDescLen;
        Ctx->ReportDescRegister  = reportDescReg;
        Ctx->InputRegister       = inputReg;
        Ctx->MaxInputLength      = maxInput;
        Ctx->OutputRegister      = outputReg;
        Ctx->MaxOutputLength     = maxOutput;
        Ctx->CommandRegister     = commandReg;
        Ctx->DataRegister        = dataReg;
        Ctx->VendorId            = vendorId;
        Ctx->ProductId           = productId;
        Ctx->VersionId           = versionId;
    }
    return TRUE;
}

/*
 * Short-item parser. Finds:
 *  - the byte range of the Generic Desktop / Mouse top-level collection, plus
 *    the global items in effect before it, so it can be presented on its own;
 *  - the Input Mode feature field (digitizer page 0x0D, usage 0x52), its report
 *    ID, bit offset, size and the total length of that feature report.
 */
VOID
HtpParseReportDescriptor(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_reads_bytes_(Length) const UCHAR* Desc,
    _In_ ULONG Length,
    _Out_ HTP_DESC_INFO* Info
    )
{
    ULONG   i = 0;
    ULONG   depth = 0;
    ULONG   usagePage = 0;
    ULONG   reportSize = 0;
    ULONG   reportCount = 0;
    ULONG   reportId = 0;
    ULONG   usages[16];
    ULONG   usageCount = 0;
    ULONG   usageMin = 0;
    ULONG   usageMax = 0;
    BOOLEAN haveMin = FALSE;
    BOOLEAN haveMax = FALSE;
    ULONG   globalOffset[10];
    ULONG   globalSize[10];
    BOOLEAN globalSet[10];
    ULONG   snapOffset[10];
    ULONG   snapSize[10];
    BOOLEAN snapSet[10];
    BOOLEAN inSegment = FALSE;
    ULONG   segmentStart = 0;
    ULONG   topStart = 0;
    ULONG   topPage = 0;
    ULONG   topUsage = 0;
    ULONG   topIdAtStart = 0;
    BOOLEAN topFirstIdSet = FALSE;
    UCHAR   topFirstId = 0;
    ULONG*  featureBits = Ctx->ParseFeatureBits;
    ULONG   k;

    RtlZeroMemory(Info, sizeof(*Info));
    RtlZeroMemory(usages, sizeof(usages));
    RtlZeroMemory(globalOffset, sizeof(globalOffset));
    RtlZeroMemory(globalSize, sizeof(globalSize));
    RtlZeroMemory(globalSet, sizeof(globalSet));
    RtlZeroMemory(snapOffset, sizeof(snapOffset));
    RtlZeroMemory(snapSize, sizeof(snapSize));
    RtlZeroMemory(snapSet, sizeof(snapSet));
    RtlZeroMemory(featureBits, sizeof(Ctx->ParseFeatureBits));

    while (i < Length) {
        UCHAR prefix = Desc[i];
        ULONG itemStart = i;
        ULONG size;
        ULONG type;
        ULONG tag;
        ULONG data = 0;

        if (prefix == 0xFE) {
            /* Long item: 0xFE, size, tag, data. */
            if (i + 2 >= Length) {
                break;
            }
            i += 3 + Desc[i + 1];
            continue;
        }

        size = prefix & 0x03;
        if (size == 3) {
            size = 4;
        }
        type = (prefix >> 2) & 0x03;
        tag  = (prefix >> 4) & 0x0F;

        if (i + 1 + size > Length) {
            break;
        }
        for (k = 0; k < size; k++) {
            data |= (ULONG)Desc[i + 1 + k] << (8 * k);
        }

        /* First item of a new top-level segment: remember the globals in effect. */
        if (depth == 0 && !inSegment) {
            inSegment = TRUE;
            segmentStart = itemStart;
            RtlCopyMemory(snapOffset, globalOffset, sizeof(snapOffset));
            RtlCopyMemory(snapSize, globalSize, sizeof(snapSize));
            RtlCopyMemory(snapSet, globalSet, sizeof(snapSet));
        }

        switch (type) {
        case 0: /* main */
            if (tag == 0x0A) {                         /* Collection */
                if (depth == 0) {
                    ULONG usage = (usageCount > 0) ? usages[0] : 0;
                    topStart = segmentStart;
                    topPage = (usage >> 16) ? (usage >> 16) : usagePage;
                    topUsage = usage & 0xFFFF;
                    topIdAtStart = reportId;
                    topFirstIdSet = FALSE;
                    Info->TopLevelCollections++;
                }
                depth++;
            } else if (tag == 0x0C) {                  /* End Collection */
                if (depth > 0) {
                    depth--;
                }
                if (depth == 0) {
                    ULONG topEnd = i + 1 + size;

                    if (!Info->HasMouse && topPage == 0x01 && topUsage == 0x02 && topEnd > topStart) {
                        ULONG t;

                        Info->HasMouse      = TRUE;
                        Info->MouseStart    = topStart;
                        Info->MouseEnd      = topEnd;
                        Info->MouseReportId = topFirstIdSet ? topFirstId : (UCHAR)topIdAtStart;
                        Info->MousePrefixLength = 0;
                        for (t = 0; t < 10; t++) {
                            ULONG itemLength = 1 + snapSize[t];
                            if (snapSet[t] &&
                                Info->MousePrefixLength + itemLength <= sizeof(Info->MousePrefix) &&
                                snapOffset[t] + itemLength <= Length) {
                                RtlCopyMemory(Info->MousePrefix + Info->MousePrefixLength,
                                              Desc + snapOffset[t], itemLength);
                                Info->MousePrefixLength += itemLength;
                            }
                        }
                    }
                    inSegment = FALSE;
                }
            } else if (tag == 0x0B) {                  /* Feature */
                ULONG rid = reportId & 0xFF;
                for (k = 0; k < reportCount && k < 4096; k++) {
                    ULONG usage;
                    ULONG page;

                    if (usageCount > 0) {
                        usage = usages[min(k, usageCount - 1)];
                    } else if (haveMin && haveMax && usageMin + k <= usageMax) {
                        usage = usageMin + k;
                    } else {
                        usage = 0;
                    }
                    page = (usage >> 16) ? (usage >> 16) : usagePage;

                    if (!Info->HasInputMode && page == 0x0D && (usage & 0xFFFF) == 0x52) {
                        Info->HasInputMode       = TRUE;
                        Info->InputModeReportId  = (UCHAR)rid;
                        Info->InputModeBitOffset = featureBits[rid] + k * reportSize;
                        Info->InputModeBitSize   = reportSize;
                    }
                }
                featureBits[rid] += reportSize * reportCount;
            }
            /* Local items only apply to the next main item. */
            usageCount = 0;
            haveMin = FALSE;
            haveMax = FALSE;
            usageMin = 0;
            usageMax = 0;
            break;

        case 1: /* global */
            if (tag < 10) {
                globalOffset[tag] = itemStart;
                globalSize[tag]   = size;
                globalSet[tag]    = TRUE;
            }
            switch (tag) {
            case 0:  usagePage = data; break;
            case 7:  reportSize = data; break;
            case 8:
                reportId = data;
                Info->UsesReportIds = TRUE;
                if (depth > 0 && !topFirstIdSet) {
                    topFirstIdSet = TRUE;
                    topFirstId = (UCHAR)data;
                }
                break;
            case 9:  reportCount = data; break;
            default: break;
            }
            break;

        case 2: /* local */
            if (tag == 0) {
                if (usageCount < ARRAYSIZE(usages)) {
                    usages[usageCount++] = (size == 4) ? data : (data & 0xFFFF);
                }
            } else if (tag == 1) {
                usageMin = data;
                haveMin = TRUE;
            } else if (tag == 2) {
                usageMax = data;
                haveMax = TRUE;
            }
            break;

        default:
            break;
        }

        i += 1 + size;
    }

    if (Info->HasInputMode) {
        Info->InputModeReportLength = 1 + (featureBits[Info->InputModeReportId] + 7) / 8;
    }
}

NTSTATUS
HtpBuildPresentedDescriptors(
    _In_ PDEVICE_CONTEXT Ctx,
    _In_ BOOLEAN FromCache
    )
{
    HTP_DESC_INFO* info = &Ctx->DescInfo;
    ULONG          mode = HTP_MODE_FULL;

    if (Ctx->DevReportDescLength == 0 || Ctx->DevReportDescLength > HTP_MAX_REPORT_DESC) {
        return STATUS_DEVICE_DATA_ERROR;
    }

    HtpParseReportDescriptor(Ctx, Ctx->DevReportDesc, Ctx->DevReportDescLength, info);

    HtpLog(Ctx, L"report descriptor: %u bytes, %u top-level collections, report IDs %s",
           Ctx->DevReportDescLength, info->TopLevelCollections, info->UsesReportIds ? L"yes" : L"no");
    HtpLog(Ctx, L"mouse collection: %s (report %u, bytes %u-%u); input mode field: %s (report %u, bit %u, %u bits, %u bytes)",
           info->HasMouse ? L"yes" : L"no", (ULONG)info->MouseReportId, info->MouseStart, info->MouseEnd,
           info->HasInputMode ? L"yes" : L"no", (ULONG)info->InputModeReportId,
           info->InputModeBitOffset, info->InputModeBitSize, info->InputModeReportLength);

    if (Ctx->DescriptorModeConfig == HTP_MODE_MOUSE ||
        (Ctx->DescriptorModeConfig == HTP_MODE_AUTO && Ctx->AutoMouseFlag)) {
        if (info->HasMouse) {
            mode = HTP_MODE_MOUSE;
        } else {
            HtpLog(Ctx, L"mouse mode requested but the chip has no mouse collection; using the full descriptor");
        }
    }

    if (mode == HTP_MODE_MOUSE) {
        ULONG segmentLength = info->MouseEnd - info->MouseStart;
        ULONG total = info->MousePrefixLength + segmentLength;

        if (segmentLength == 0 || total > HTP_MAX_PRESENTED_DESC || info->MouseEnd > Ctx->DevReportDescLength) {
            HtpLog(Ctx, L"could not extract the mouse collection; using the full descriptor");
            mode = HTP_MODE_FULL;
        } else {
            RtlCopyMemory(Ctx->PresentedDesc, info->MousePrefix, info->MousePrefixLength);
            RtlCopyMemory(Ctx->PresentedDesc + info->MousePrefixLength,
                          Ctx->DevReportDesc + info->MouseStart, segmentLength);
            Ctx->PresentedDescLength = total;
        }
    }

    if (mode == HTP_MODE_FULL) {
        RtlCopyMemory(Ctx->PresentedDesc, Ctx->DevReportDesc, Ctx->DevReportDescLength);
        Ctx->PresentedDescLength = Ctx->DevReportDescLength;
    }

    Ctx->PresentedMode      = mode;
    Ctx->PresentedChecksum  = Ctx->DevReportDescChecksum;
    Ctx->PresentedFromCache = FromCache;

    RtlZeroMemory(&Ctx->HidDescriptor, sizeof(Ctx->HidDescriptor));
    Ctx->HidDescriptor.bLength         = (UCHAR)sizeof(HID_DESCRIPTOR);
    Ctx->HidDescriptor.bDescriptorType = 0x21;   /* HID descriptor */
    Ctx->HidDescriptor.bcdHID          = 0x0100;
    Ctx->HidDescriptor.bCountry        = 0;
    Ctx->HidDescriptor.bNumDescriptors = 1;
    Ctx->HidDescriptor.DescriptorList[0].bReportType   = 0x22;   /* report descriptor */
    Ctx->HidDescriptor.DescriptorList[0].wReportLength = (USHORT)Ctx->PresentedDescLength;

    RtlZeroMemory(&Ctx->HidAttributes, sizeof(Ctx->HidAttributes));
    Ctx->HidAttributes.Size          = sizeof(HID_DEVICE_ATTRIBUTES);
    Ctx->HidAttributes.VendorID      = Ctx->VendorId;
    Ctx->HidAttributes.ProductID     = Ctx->ProductId;
    Ctx->HidAttributes.VersionNumber = Ctx->VersionId;

    Ctx->DescriptorsValid = TRUE;

    HtpLog(Ctx, L"presenting %s descriptor (%u bytes)%s",
           mode == HTP_MODE_MOUSE ? L"MOUSE-ONLY" : L"FULL precision touchpad",
           Ctx->PresentedDescLength, FromCache ? L" from the saved copy" : L"");
    InterlockedExchange(&Ctx->DiagDirty, 1);
    return STATUS_SUCCESS;
}
