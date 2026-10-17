/*
 * htixtp - HID over I2C driver for the Hantick 0911:5288 touchpad
 *          (ACPI SYNA3602 / "SYNA360", used in the Infinix INBook X3 XL422 and
 *          several other low-cost laptops).
 *
 * Windows' inbox hidi2c.sys cannot keep this chip alive: the chip drops off the
 * I2C bus after enumeration and never sends input. Linux works with it because
 * it (1) does not wait for a reset interrupt the chip never sends, (2) retries
 * transfers the chip NACKs while waking up, and (3) never puts it to sleep while
 * the system is running.
 *
 * This driver replaces hidi2c.sys for this one device. It is a KMDF lower filter
 * under Microsoft's pass-through driver mshidkmdf.sys (the documented way to
 * write a HID minidriver in KMDF) and talks to the chip itself through the SPB
 * (I2C) controller, with the same quirks Linux uses plus self-recovery.
 */

#pragma once

#include <ntddk.h>
#include <wdf.h>
#include <hidport.h>
#include <ntstrsafe.h>
#include <spb.h>
#define RESHUB_USE_HELPER_ROUTINES
#include <reshub.h>

#define HTP_VERSION_STRING          L"1.2.0"
#define HTP_POOL_TAG                'pTxH'

/* Limits */
#define HTP_HID_DESC_LENGTH         30
#define HTP_MAX_REPORT_DESC         4096
#define HTP_MAX_PRESENTED_DESC      (HTP_MAX_REPORT_DESC + 128)
#define HTP_XFER_BUFFER_SIZE        (HTP_MAX_REPORT_DESC + 64)
#define HTP_MAX_INPUT_LENGTH        512
#define HTP_FIFO_DEPTH              8
#define HTP_LOG_LINES               64
#define HTP_LOG_CHARS               128
#define HTP_MAX_SAVED_FEATURES      8
#define HTP_MAX_SAVED_FEATURE_LEN   64
#define HTP_MAX_REPORT_IDS          256

/* HID over I2C opcodes */
#define HTP_OP_RESET                0x01
#define HTP_OP_GET_REPORT           0x02
#define HTP_OP_SET_REPORT           0x03
#define HTP_OP_SET_POWER            0x08

#define HTP_REPORT_TYPE_INPUT       0x01
#define HTP_REPORT_TYPE_OUTPUT      0x02
#define HTP_REPORT_TYPE_FEATURE     0x03

#define HTP_POWER_ON                0x00
#define HTP_POWER_SLEEP             0x01

/* Configuration values (DWORDs under the device's "Device Parameters" key) */
#define HTP_MODE_AUTO               0   /* precision touchpad; fall back to mouse if Windows rejects it */
#define HTP_MODE_FULL               1   /* always present the full (precision touchpad) descriptor */
#define HTP_MODE_MOUSE              2   /* present only the mouse collection */

#define HTP_POLL_OFF                0   /* interrupt driven */
#define HTP_POLL_ONLY               1   /* ignore the interrupt, poll the chip */
#define HTP_POLL_BOTH               2   /* interrupt plus polling */

typedef enum _HTP_STATE {
    HtpStateStopped = 0,
    HtpStateReady,
    HtpStateRecovering,
    HtpStateNoResponse
} HTP_STATE;

typedef struct _HTP_STATS {
    volatile LONG I2cOk;
    volatile LONG I2cFail;
    volatile LONG InitOk;
    volatile LONG InitFail;
    volatile LONG WakeAttemptsLast;
    volatile LONG Recoveries;
    volatile LONG Interrupts;
    volatile LONG InputReads;
    volatile LONG ReportsDelivered;
    volatile LONG ReportsQueued;
    volatile LONG ReportsDropped;
    volatile LONG EmptyReads;
    volatile LONG BadReports;
    volatile LONG DuplicatesDropped;
    volatile LONG FeatureGetOk;
    volatile LONG FeatureGetFail;
    volatile LONG FeatureSetOk;
    volatile LONG FeatureSetFail;
    volatile LONG ReadRequests;
    volatile LONG HealthChecks;
    volatile LONG HealthFails;
    volatile LONG PollReads;
    volatile LONG LastError;
} HTP_STATS;

/* Result of parsing the device's report descriptor */
typedef struct _HTP_DESC_INFO {
    BOOLEAN UsesReportIds;

    BOOLEAN HasMouse;
    ULONG   MouseStart;             /* byte range of the mouse top-level collection */
    ULONG   MouseEnd;
    UCHAR   MousePrefix[64];        /* global items in effect before that range */
    ULONG   MousePrefixLength;
    UCHAR   MouseReportId;

    BOOLEAN HasInputMode;           /* digitizer page 0x0D, usage 0x52 (feature) */
    UCHAR   InputModeReportId;
    ULONG   InputModeBitOffset;     /* bit offset after the report ID byte */
    ULONG   InputModeBitSize;
    ULONG   InputModeReportLength;  /* bytes including the report ID byte */

    ULONG   TopLevelCollections;
} HTP_DESC_INFO;

typedef struct _HTP_SAVED_FEATURE {
    UCHAR  ReportId;
    USHORT Length;
    UCHAR  Data[HTP_MAX_SAVED_FEATURE_LEN];
} HTP_SAVED_FEATURE;

typedef struct _DEVICE_CONTEXT {
    WDFDEVICE       Device;
    WDFQUEUE        DefaultQueue;
    WDFQUEUE        ReadQueue;

    /* Bus */
    WDFIOTARGET     SpbTarget;
    LARGE_INTEGER   SpbConnectionId;
    BOOLEAN         SpbFound;
    BOOLEAN         SpbOpen;
    WDFWAITLOCK     I2cLock;            /* serializes every bus transaction */
    PUCHAR          XferBuffer;         /* read buffer, used under I2cLock */
    PUCHAR          CmdBuffer;          /* write buffer, used under I2cLock */

    /* Interrupt / polling */
    WDFINTERRUPT    Interrupt;
    BOOLEAN         InterruptResourceFound;
    volatile LONG   InterruptDisabledByUs;
    BOOLEAN         PollingActive;
    WDFTIMER        PollTimer;
    WDFTIMER        MaintTimer;

    /* Configuration */
    ULONG           HidDescAddress;
    ULONG           DescriptorModeConfig;
    ULONG           PollingConfig;
    ULONG           PollIntervalMs;
    ULONG           HealthCheckMs;
    ULONG           SleepOnSuspend;
    ULONG           ResetDelayMs;
    ULONG           PowerOnDelayMs;
    ULONG           WakeTimeoutMs;
    ULONG           FirstWakeTimeoutMs;
    ULONG           AutoMouseFlag;      /* set by the driver after an automatic fallback */

    /* Chip (as last read from the device) */
    UCHAR           DevHidDesc[HTP_HID_DESC_LENGTH];
    USHORT          ReportDescLengthDev;
    USHORT          ReportDescRegister;
    USHORT          InputRegister;
    USHORT          MaxInputLength;
    USHORT          OutputRegister;
    USHORT          MaxOutputLength;
    USHORT          CommandRegister;
    USHORT          DataRegister;
    USHORT          VendorId;
    USHORT          ProductId;
    USHORT          VersionId;
    PUCHAR          DevReportDesc;      /* raw report descriptor (device or cache) */
    ULONG           DevReportDescLength;
    ULONG           DevReportDescChecksum;
    BOOLEAN         CacheLoaded;
    UCHAR           CacheHidDesc[HTP_HID_DESC_LENGTH];
    PUCHAR          CacheReportDesc;
    ULONG           CacheReportDescLength;

    /* What HIDClass was given */
    BOOLEAN         DescriptorsValid;
    ULONG           PresentedMode;      /* HTP_MODE_FULL or HTP_MODE_MOUSE */
    ULONG           PresentedChecksum;  /* checksum of the raw descriptor it was built from */
    BOOLEAN         PresentedFromCache;
    PUCHAR          PresentedDesc;
    ULONG           PresentedDescLength;
    HID_DESCRIPTOR  HidDescriptor;
    HID_DEVICE_ATTRIBUTES HidAttributes;
    HTP_DESC_INFO   DescInfo;

    /* Runtime state */
    volatile LONG   InD0;
    volatile LONG   DeviceReady;
    volatile LONG   RecoveryRequested;
    volatile LONG   ConsecutiveFailures;
    volatile LONG   ConsecutiveBadReports;
    volatile LONG   RestartRequested;
    ULONG           RecoveryBackoffMs;
    ULONGLONG       StartTimeMs;
    ULONGLONG       LastGoodXferMs;
    BOOLEAN         PtpActivated;       /* Windows sent a SET_FEATURE (precision touchpad stack is active) */
    BOOLEAN         AutoFallbackDone;
    HTP_STATE       State;

    /* Feature reports Windows set, replayed after any re-initialization */
    HTP_SAVED_FEATURE SavedFeatures[HTP_MAX_SAVED_FEATURES];
    ULONG           SavedFeatureCount;
    WDFSPINLOCK     SavedFeatureLock;

    /* Input FIFO for reports that arrive while no read request is pending */
    WDFSPINLOCK     FifoLock;
    UCHAR           Fifo[HTP_FIFO_DEPTH][HTP_MAX_INPUT_LENGTH];
    USHORT          FifoLength[HTP_FIFO_DEPTH];
    ULONG           FifoHead;
    ULONG           FifoCount;

    /* Polling duplicate suppression */
    UCHAR           LastPolled[HTP_MAX_INPUT_LENGTH];
    ULONG           LastPolledLength;

    /* Diagnostics */
    HTP_STATS       Stats;
    WDFSPINLOCK     LogLock;
    WCHAR           Log[HTP_LOG_LINES][HTP_LOG_CHARS];
    ULONG           LogHead;
    ULONG           LogCount;
    volatile LONG   DiagDirty;
    LONG            LastStatsSignature;
    ULONG           ParseFeatureBits[HTP_MAX_REPORT_IDS];   /* scratch for the descriptor parser */
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext);

/* htixtp.c */
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD HtpEvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE HtpEvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE HtpEvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY HtpEvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT_PRE_INTERRUPTS_DISABLED HtpEvtD0ExitPreInterruptsDisabled;
EVT_WDF_DEVICE_D0_EXIT HtpEvtD0Exit;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL HtpEvtInternalDeviceControl;
EVT_WDF_INTERRUPT_ISR HtpEvtInterruptIsr;
EVT_WDF_TIMER HtpEvtPollTimer;
EVT_WDF_TIMER HtpEvtMaintTimer;
VOID HtpDeliverReport(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR* Data, _In_ ULONG Length);

/* hidi2c.c - bus protocol, chip bring-up, input */
ULONGLONG HtpNowMs(VOID);
VOID      HtpSleepMs(_In_ ULONG Milliseconds);
NTSTATUS  HtpInitializeChip(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG WakeTimeoutMs, _In_ BOOLEAN AllowAbort);
NTSTATUS  HtpProbeChip(_In_ PDEVICE_CONTEXT Ctx);
NTSTATUS  HtpSetPower(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR PowerState);
NTSTATUS  HtpGetReport(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR ReportType, _In_ UCHAR ReportId,
                       _Out_writes_bytes_(BufferLength) PUCHAR Buffer, _In_ ULONG BufferLength,
                       _Out_ PULONG BytesReturned);
NTSTATUS  HtpSetReport(_In_ PDEVICE_CONTEXT Ctx, _In_ UCHAR ReportType, _In_ UCHAR ReportId,
                       _In_reads_bytes_(BufferLength) PUCHAR Buffer, _In_ ULONG BufferLength);
NTSTATUS  HtpWriteOutputReport(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(BufferLength) PUCHAR Buffer,
                               _In_ ULONG BufferLength);
VOID      HtpServiceInput(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN FromPoll);
VOID      HtpReplaySavedFeatures(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpApplyMouseMode(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpNoteFailure(_In_ PDEVICE_CONTEXT Ctx, _In_ NTSTATUS Status);
VOID      HtpRequestMaintenance(_In_ PDEVICE_CONTEXT Ctx, _In_ ULONG DelayMs);

/* descriptor.c - report descriptor parsing and the presented descriptor */
ULONG     HtpChecksum(_In_reads_bytes_(Length) const UCHAR* Data, _In_ ULONG Length);
VOID      HtpParseReportDescriptor(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(Length) const UCHAR* Desc,
                                   _In_ ULONG Length, _Out_ HTP_DESC_INFO* Info);
NTSTATUS  HtpBuildPresentedDescriptors(_In_ PDEVICE_CONTEXT Ctx, _In_ BOOLEAN FromCache);
BOOLEAN   HtpParseHidDescriptor(_In_ PDEVICE_CONTEXT Ctx, _In_reads_bytes_(HTP_HID_DESC_LENGTH) const UCHAR* Raw,
                                _In_ BOOLEAN Apply);

/* diag.c - log, statistics and settings in the registry */
VOID      HtpLog(_In_ PDEVICE_CONTEXT Ctx, _In_z_ _Printf_format_string_ PCWSTR Format, ...);
VOID      HtpSetState(_In_ PDEVICE_CONTEXT Ctx, _In_ HTP_STATE State);
VOID      HtpReadSettings(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpWriteDiagnostics(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpLoadDescriptorCache(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpSaveDescriptorCache(_In_ PDEVICE_CONTEXT Ctx);
VOID      HtpWriteRegistryULong(_In_ PDEVICE_CONTEXT Ctx, _In_ PCWSTR Name, _In_ ULONG Value);
