/*
 * Process Hacker Online Checks - 
 *   uploader
 * 
 * Copyright (C) 2010 wj32
 * 
 * This file is part of Process Hacker.
 * 
 * Process Hacker is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Process Hacker is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Process Hacker.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <phdk.h>
#include <wininet.h>
#include "onlnchk.h"
#include "resource.h"

#define UM_LAUNCH_COMMAND (WM_APP + 1)
#define UM_ERROR (WM_APP + 2)

typedef struct _UPLOAD_CONTEXT
{
    LONG RefCount;

    PPH_STRING FileName;
    ULONG Service;
    HWND WindowHandle;
    HANDLE ThreadHandle;

    PPH_STRING LaunchCommand;
    PPH_STRING ErrorMessage;
} UPLOAD_CONTEXT, *PUPLOAD_CONTEXT;

INT_PTR CALLBACK UploadDlgProc(      
    __in HWND hwndDlg,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    );

PUPLOAD_CONTEXT CreateUploadContext()
{
    PUPLOAD_CONTEXT context;

    context = PhAllocate(sizeof(UPLOAD_CONTEXT));
    memset(context, 0, sizeof(UPLOAD_CONTEXT));
    context->RefCount = 1;

    return context;
}

VOID ReferenceUploadContext(
    __inout PUPLOAD_CONTEXT Context
    )
{
    _InterlockedIncrement(&Context->RefCount);
}

VOID DereferenceUploadContext(
    __inout PUPLOAD_CONTEXT Context
    )
{
    if (_InterlockedDecrement(&Context->RefCount) == 0)
    {
        PhSwapReference(&Context->FileName, NULL);
        PhSwapReference(&Context->LaunchCommand, NULL);
        PhSwapReference(&Context->ErrorMessage, NULL);

        PhFree(Context);
    }
}

VOID UploadToOnlineService(
    __in HWND hWnd,
    __in PPH_STRING FileName,
    __in ULONG Service
    )
{
    PUPLOAD_CONTEXT context;

    context = CreateUploadContext();

    PhSwapReference(&context->FileName, FileName);
    context->Service = Service;

    DialogBoxParam(
        PluginInstance->DllBase,
        MAKEINTRESOURCE(IDD_PROGRESS),
        hWnd,
        UploadDlgProc,
        (LPARAM)context
        );

    DereferenceUploadContext(context);
}

static VOID RaiseUploadError(
    __in PUPLOAD_CONTEXT Context,
    __in PWSTR Error,
    __in_opt ULONG ErrorCode
    )
{
    PPH_STRING errorMessage = NULL;

    if (ErrorCode)
    {
        errorMessage = PhGetWin32Message(ErrorCode);

        if (!errorMessage)
            errorMessage = PhFormatString(L"Error %u", ErrorCode);
    }

    if (errorMessage)
        Context->ErrorMessage = PhConcatStrings(3, Error, L": ", errorMessage->Buffer);
    else
        Context->ErrorMessage = PhConcatStrings2(Error, L".");

    PhSwapReference(&errorMessage, NULL);

    if (Context->WindowHandle)
        PostMessage(Context->WindowHandle, UM_ERROR, 0, 0);
}

static NTSTATUS UploadWorkerThreadStart(
    __in PVOID Parameter
    )
{
    PUPLOAD_CONTEXT context = Parameter;

    switch (context->Service)
    {
    case UPLOAD_SERVICE_VIRUSTOTAL:
    case UPLOAD_SERVICE_JOTTI:
        {
            NTSTATUS status;
            PPH_STRING userAgent;
            HANDLE fileHandle = NULL;
            ULONG fileSize;
            HINTERNET internetHandle = NULL;
            HINTERNET connectHandle = NULL;
            HINTERNET requestHandle = NULL;
            PPH_STRING boundary = NULL;
            PPH_ANSI_STRING boundaryAnsi = NULL;
            PPH_FULL_STRING headers = NULL;
            PPH_ANSI_STRING baseFileNameAnsi;
            PUCHAR data = NULL;
            ULONG dataLength = 0;
            ULONG dataCursor = 0;

            // Open the file and check its size.

            status = PhCreateFileWin32(
                &fileHandle,
                context->FileName->Buffer,
                FILE_GENERIC_READ,
                0,
                FILE_SHARE_READ | FILE_SHARE_DELETE,
                FILE_OPEN,
                FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT
                );

            if (NT_SUCCESS(status))
            {
                LARGE_INTEGER fileSize64;

                if (NT_SUCCESS(status = PhGetFileSize(fileHandle, &fileSize64)))
                {
                    if (fileSize64.QuadPart > 20 * 1024 * 1024) // 20 MB
                    {
                        RaiseUploadError(context, L"The file is too large (over 20 MB)", 0);
                        goto ExitCleanup;
                    }

                    fileSize = fileSize64.LowPart;
                }
            }

            if (!NT_SUCCESS(status))
            {
                RaiseUploadError(context, L"Unable to open the file", RtlNtStatusToDosError(status));
                goto ExitCleanup;
            }

            // Create a user agent string.
            {
                PPH_STRING phVersion;

                phVersion = PhGetPhVersion();
                userAgent = PhConcatStrings2(L"Process Hacker ", phVersion->Buffer);
                PhDereferenceObject(phVersion);
            }

            // Create the internet handle.

            internetHandle = InternetOpen(userAgent->Buffer, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
            PhDereferenceObject(userAgent);

            if (!internetHandle)
            {
                RaiseUploadError(context, L"Unable to initialize internet access", GetLastError());
                goto ExitCleanup;
            }

            // Set the timeouts.
            {
                ULONG timeout = 5 * 60 * 1000; // 5 minutes

                InternetSetOption(internetHandle, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(ULONG));
                InternetSetOption(internetHandle, INTERNET_OPTION_SEND_TIMEOUT, &timeout, sizeof(ULONG));
                InternetSetOption(internetHandle, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(ULONG));
                InternetSetOption(internetHandle, INTERNET_OPTION_DATA_SEND_TIMEOUT, &timeout, sizeof(ULONG));
                InternetSetOption(internetHandle, INTERNET_OPTION_DATA_RECEIVE_TIMEOUT, &timeout, sizeof(ULONG));
            }

            // Connect to the online service.

            connectHandle = InternetConnect(
                internetHandle,
                context->Service == UPLOAD_SERVICE_VIRUSTOTAL ? L"www.virustotal.com" : L"virusscan.jotti.org",
                80,
                NULL,
                NULL,
                INTERNET_SERVICE_HTTP,
                0,
                0
                );

            if (!connectHandle)
            {
                RaiseUploadError(context, L"Unable to connect to the service", GetLastError());
                goto ExitCleanup;
            }

            // Create the request.

            {
                static PWSTR acceptTypes[2] = { L"*/*", NULL };

                requestHandle = HttpOpenRequest(
                    connectHandle,
                    L"POST",
                    context->Service == UPLOAD_SERVICE_VIRUSTOTAL ? L"/vt/en/recepcionf" : L"/processupload.php",
                    L"HTTP/1.1",
                    L"",
                    acceptTypes,
                    INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_AUTO_REDIRECT,
                    0
                    );
            }

            if (!requestHandle)
            {
                RaiseUploadError(context, L"Unable to create the request", GetLastError());
                goto ExitCleanup;
            }

            // Create the boundary.

            {
                static ULONG seed = 0;

                boundary = PhFormatString(
                    L"------------------------%I64u",
                    (ULONG64)RtlRandomEx(&seed) | ((ULONG64)RtlRandomEx(&seed) << 31)
                    );
                boundaryAnsi = PhCreateAnsiStringFromUnicodeEx(boundary->Buffer, boundary->Length);
            }

            // Create the data.

            {
                PSTR contentDispositionPart1 = "Content-Disposition: form-data; name=\"";
                PSTR fileNameFieldName;
                PSTR contentDispositionPart2 = "\"; filename=\"";
                PSTR contentType = "Content-Type: application/octet-stream\r\n\r\n";
                PPH_STRING baseFileName;
                IO_STATUS_BLOCK isb;

                baseFileName = PhGetBaseName(context->FileName);
                baseFileNameAnsi = PhCreateAnsiStringFromUnicodeEx(baseFileName->Buffer, baseFileName->Length);
                PhDereferenceObject(baseFileName);

                fileNameFieldName = context->Service == UPLOAD_SERVICE_VIRUSTOTAL ? "archivo" : "scanfile";

                dataLength = 2; // --
                dataLength += boundaryAnsi->Length;
                dataLength += 2; // \r\n
                dataLength += (ULONG)strlen(contentDispositionPart1);
                dataLength += (ULONG)strlen(fileNameFieldName);
                dataLength += (ULONG)strlen(contentDispositionPart2);
                dataLength += baseFileNameAnsi->Length;
                dataLength += 3; // \"\r\n
                dataLength += (ULONG)strlen(contentType);
                dataLength += fileSize;
                dataLength += 4; // \r\n--
                dataLength += boundaryAnsi->Length;
                dataLength += 6; // --\r\n\r\n

                data = PhAllocatePage(dataLength, NULL);

                if (!data)
                {
                    RaiseUploadError(context, L"Unable to allocate memory", 0);
                    goto ExitCleanup;
                }

                // Boundary
                memcpy(&data[dataCursor], "--", 2);
                dataCursor += 2;
                memcpy(&data[dataCursor], boundaryAnsi->Buffer, boundaryAnsi->Length);
                dataCursor += boundaryAnsi->Length;
                memcpy(&data[dataCursor], "\r\n", 2);
                dataCursor += 2;

                // Content Disposition
                memcpy(&data[dataCursor], contentDispositionPart1, strlen(contentDispositionPart1));
                dataCursor += (ULONG)strlen(contentDispositionPart1);
                memcpy(&data[dataCursor], fileNameFieldName, strlen(fileNameFieldName));
                dataCursor += (ULONG)strlen(fileNameFieldName);
                memcpy(&data[dataCursor], contentDispositionPart2, strlen(contentDispositionPart2));
                dataCursor += (ULONG)strlen(contentDispositionPart2);
                memcpy(&data[dataCursor], baseFileNameAnsi->Buffer, baseFileNameAnsi->Length);
                dataCursor += baseFileNameAnsi->Length;
                memcpy(&data[dataCursor], "\"\r\n", 3);
                dataCursor += 3;

                // Content Type
                memcpy(&data[dataCursor], contentType, strlen(contentType));
                dataCursor += (ULONG)strlen(contentType);

                // File contents
                status = NtReadFile(fileHandle, NULL, NULL, NULL, &isb, &data[dataCursor], fileSize, NULL, NULL);
                dataCursor += fileSize;

                if (!NT_SUCCESS(status))
                {
                    RaiseUploadError(context, L"Unable to read the file", RtlNtStatusToDosError(status));
                    goto ExitCleanup;
                }

                // Boundary
                memcpy(&data[dataCursor], "\r\n--", 4);
                dataCursor += 4;
                memcpy(&data[dataCursor], boundaryAnsi->Buffer, boundaryAnsi->Length);
                dataCursor += boundaryAnsi->Length;
                memcpy(&data[dataCursor], "--\r\n\r\n", 6);
                dataCursor += 6;

                assert(dataCursor == dataLength);
            }

            // Create and add the header.

            {
                headers = PhCreateFullString2(100);

                PhAppendFullString2(
                    headers,
                    L"Content-Type: multipart/form-data; boundary="
                    );
                PhAppendFullString(headers, boundary);
                PhAppendFullString2(headers, L"\r\n");
                PhAppendFormatFullString(headers, L"Content-Length: %u\r\n", dataLength);

                HttpAddRequestHeaders(
                    requestHandle,
                    headers->Buffer,
                    (ULONG)headers->Length,
                    HTTP_ADDREQ_FLAG_REPLACE | HTTP_ADDREQ_FLAG_ADD
                    );
            }

            // Send the request.

            if (!HttpSendRequest(requestHandle, NULL, 0, data, dataLength))
            {
                RaiseUploadError(context, L"Unable to send the request", GetLastError());
                goto ExitCleanup;
            }

            // Get the new location.

            {
                UCHAR buffer[PAGE_SIZE];
                ULONG bufferSize;
                ULONG index;

                bufferSize = sizeof(buffer);
                index = 0;

                if (context->Service == UPLOAD_SERVICE_VIRUSTOTAL)
                {
                    if (!HttpQueryInfo(requestHandle, HTTP_QUERY_LOCATION, buffer, &bufferSize, &index))
                    {
                        RaiseUploadError(context, L"Unable to complete the request (please try again after a few minutes)", GetLastError());
                        goto ExitCleanup;
                    }

                    context->LaunchCommand = PhCreateString((PWSTR)buffer);
                }
                else
                {
                    PSTR hrefEquals;
                    PSTR quote;

                    // This service returns some JavaScript that redirects the user to the new location.

                    if (!InternetReadFile(requestHandle, buffer, sizeof(buffer) - 1, &bufferSize))
                    {
                        RaiseUploadError(context, L"Unable to complete the request", GetLastError());
                        goto ExitCleanup;
                    }

                    // Make sure the buffer is null-terminated.
                    buffer[bufferSize] = 0;

                    // The JavaScript looks like this:
                    // top.location.href="...";
                    hrefEquals = strstr(buffer, "href=\"");

                    if (hrefEquals)
                    {
                        hrefEquals += 6;
                        quote = strchr(hrefEquals, '\"');

                        if (quote)
                        {
                            context->LaunchCommand = PhFormatString(
                                L"http://virusscan.jotti.org%.*S",
                                quote - hrefEquals,
                                hrefEquals
                                );
                        }
                    }
                    else
                    {
                        PSTR tooManyFiles;

                        tooManyFiles = strstr(buffer, "Too many files");

                        if (tooManyFiles)
                        {
                            RaiseUploadError(
                                context,
                                L"Unable to scan the file:\n\n"
                                L"Too many files have been scanned from this IP in a short period. "
                                L"Please try again later",
                                0
                                );
                            goto ExitCleanup;
                        }
                    }

                    if (!context->LaunchCommand)
                    {
                        RaiseUploadError(context, L"Unable to complete the request (please try again after a few minutes)", 0);
                        goto ExitCleanup;
                    }
                }

                PostMessage(context->WindowHandle, UM_LAUNCH_COMMAND, 0, 0);
            }

ExitCleanup:
            if (data)
                PhFreePage(data);
            if (baseFileNameAnsi)
                PhDereferenceObject(baseFileNameAnsi);
            if (headers)
                PhDereferenceObject(headers);
            if (boundaryAnsi)
                PhDereferenceObject(boundaryAnsi);
            if (boundary)
                PhDereferenceObject(boundary);
            if (requestHandle)
                InternetCloseHandle(requestHandle);
            if (connectHandle)
                InternetCloseHandle(connectHandle);
            if (internetHandle)
                InternetCloseHandle(internetHandle);
            if (fileHandle)
                NtClose(fileHandle);
        }
        break;
    }

    DereferenceUploadContext(context);

    return STATUS_SUCCESS;
}

INT_PTR CALLBACK UploadDlgProc(      
    __in HWND hwndDlg,
    __in UINT uMsg,
    __in WPARAM wParam,
    __in LPARAM lParam
    )
{
    PUPLOAD_CONTEXT context;

    if (uMsg == WM_INITDIALOG)
    {
        context = (PUPLOAD_CONTEXT)lParam;
        SetProp(hwndDlg, L"Context", (HANDLE)context);
    }
    else
    {
        context = (PUPLOAD_CONTEXT)GetProp(hwndDlg, L"Context");

        if (uMsg == WM_DESTROY)
            RemoveProp(hwndDlg, L"Context");
    }

    if (!context)
        return FALSE;

    switch (uMsg)
    {
    case WM_INITDIALOG:
        {
            PhCenterWindow(hwndDlg, GetParent(hwndDlg));
            SetDlgItemText(hwndDlg, IDC_MESSAGE, L"Uploading...");

            context->WindowHandle = hwndDlg;

            ReferenceUploadContext(context);
            context->ThreadHandle = PhCreateThread(0, UploadWorkerThreadStart, context);
        }
        break;
    case WM_DESTROY:
        {
            context->WindowHandle = NULL;
        }
        break;
    case WM_COMMAND:
        {
            switch (LOWORD(wParam))
            {
            case IDCANCEL:
                EndDialog(hwndDlg, IDCANCEL);
                break;
            }
        }
        break;
    case UM_LAUNCH_COMMAND:
        {
            PhShellExecute(hwndDlg, context->LaunchCommand->Buffer, NULL);
            EndDialog(hwndDlg, IDOK);
        }
        break;
    case UM_ERROR:
        {
            PhShowError(hwndDlg, L"%s", context->ErrorMessage->Buffer);
            EndDialog(hwndDlg, IDCANCEL);
        }
        break;
    }

    return FALSE;
}
