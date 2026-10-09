#include <fltKernel.h>
#include <builddef.h>

#define VELOXDISK_BLOCK      (64 * 1024)
#define VELOXDISK_MAX_SECS   2048

typedef struct _VOLUME_CONTEXT
{
    PFLT_FILTER Filter;
    BOOLEAN     Active;
    ULONG64     VolSerial;
    ULONG64     BytesRead;
    ULONG64     BytesFromCache;
    ULONG64     BytesWritten;
    ULONG64     BytesToSource;

} VOLUME_CONTEXT, *PVOLUME_CONTEXT;

// 统计用 post-op 回调 (骨架阶段只计量, 不改数据流)。
// 注意: 普通(非 minicallback)post-op 回调必须返回 FLT_POSTOP_COMPLETE;
// 之前写的是 FLT_POSTOP_FINISHED_PROCESSING —— 那个返回值只属于
// mini-completion 回调, 普通 post-op 返回它会让过滤管理器误判处理状态。
// 早期版本里的 PreOperationRead/Write 是无操作占位 (从不拦截), 且引用的
// FILE_CONTEXT 从未注册, 一并删除, 统计全部移到 post-op 完成点。
FLT_POSTOP_CALLBACK_STATUS
PostOperationRead(
    _Inout_ PFLT_CALLBACK_DATA    Data,
    _Inout_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_optional_ PFLT_COMPLETION_CONTEXT CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);
    PVOLUME_CONTEXT Vcb = (PVOLUME_CONTEXT)FltObjects->Instance->Context[0];
    if (Vcb && Vcb->Active && Data->IoStatus.Status == STATUS_SUCCESS)
        InterlockedAdd64((volatile LONG64*)&Vcb->BytesRead, (LONG64)Data->Iopb->Parameters.MsRead.Length);
    return FLT_POSTOP_COMPLETE;
}

FLT_POSTOP_CALLBACK_STATUS
PostOperationWrite(
    _Inout_ PFLT_CALLBACK_DATA    Data,
    _Inout_ PCFLT_RELATED_OBJECTS FltObjects,
    _Flt_optional_ PFLT_COMPLETION_CONTEXT CompletionContext,
    _In_ FLT_POST_OPERATION_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(CompletionContext);
    UNREFERENCED_PARAMETER(Flags);
    PVOLUME_CONTEXT Vcb = (PVOLUME_CONTEXT)FltObjects->Instance->Context[0];
    if (Vcb && Vcb->Active) {
        InterlockedAdd64((volatile LONG64*)&Vcb->BytesWritten, (LONG64)Data->Iopb->Parameters.MsWrite.Length);
        if (Data->IoStatus.Status == STATUS_SUCCESS)
            InterlockedAdd64((volatile LONG64*)&Vcb->BytesToSource, (LONG64)Data->Iopb->Parameters.MsWrite.Length);
    }
    return FLT_POSTOP_COMPLETE;
}

NTSTATUS
InstanceSetup(
    _In_ PCFLT_RELATED_OBJECTS FltObjects,
    _In_ FLT_INSTANCE_SETUP_FLAGS Flags,
    _In_ DEVICE_TYPE VolumeDeviceType,
    _In_ FLT_FILESYSTEM_TYPE VolumeFilesystemType)
{
    UNREFERENCED_PARAMETER(Flags);
    UNREFERENCED_PARAMETER(VolumeDeviceType);
    UNREFERENCED_PARAMETER(VolumeFilesystemType);
    PVOLUME_CONTEXT Vcb;

    Vcb = (PVOLUME_CONTEXT)FltAllocateContext(
        FltObjects->Filter, sizeof(VOLUME_CONTEXT),
        FLT_CONTEXT_FLAG_NONE, FltObjects->Instance);
    if (!Vcb)
        return STATUS_INSUFFICIENT_RESOURCES;

    Vcb->Filter    = FltObjects->Filter;
    Vcb->Active    = TRUE;
    Vcb->VolSerial = 0;
    Vcb->BytesRead = Vcb->BytesFromCache = 0;
    Vcb->BytesWritten = Vcb->BytesToSource = 0;

    FltObjects->Instance->Context[0] = Vcb;
    return STATUS_SUCCESS;
}

VOID
InstanceCleanup(_In_ PCFLT_RELATED_OBJECTS FltObjects, _In_ FLT_INSTANCE_TEARDOWN_FLAGS Flags)
{
    UNREFERENCED_PARAMETER(Flags);
    PVOLUME_CONTEXT Vcb = (PVOLUME_CONTEXT)FltObjects->Instance->Context[0];
    if (Vcb) {

        FltObjects->Instance->Context[0] = NULL;
        FltDeleteContext(FltObjects->Filter, Vcb);
    }
}

static const FLT_OPERATION_REGISTRATION Operations[] = {
    {
        IRP_MJ_READ,
        0,
        NULL,
        PostOperationRead
    },
    {
        IRP_MJ_WRITE,
        0,
        NULL,
        PostOperationWrite
    },
    {
        IRP_MJ_OPERATION_END
    }
};

static const FLT_REGISTRATION Registration = {
    sizeof(FLT_REGISTRATION),
    FLT_REGISTRATION_VERSION,
    0,
    NULL,
    Operations,
    NULL,
    InstanceSetup,
    NULL,
    InstanceCleanup,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS Status;
    PFLT_FILTER Filter = NULL;

    Status = FltRegisterFilter(DriverObject, RegistryPath,
                               &Registration, &Filter);
    if (NT_SUCCESS(Status)) {
        Status = FltStartFiltering(Filter);
        if (!NT_SUCCESS(Status)) {

            FltUnregisterFilter(Filter);
        }
    }
    return Status;
}
