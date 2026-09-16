# Crashpad
Small windows exception handler, programmed in CPP.

# Ready to use
Simply include into your project and call Diagnostics::Initialize() from your main thread.

## Usage Examples

### Initialize Diagnostics

Initialize diagnostics once when your application starts:

```cpp
#include "Diagnostics.h"

int main()
{
    Diagnostics::Initialize();

    Diagnostics::Log("INFO", "Application started.");

    return 0;
}
```

---

### Basic Logging

```cpp
Diagnostics::Log("INFO", "Application initialized.");
Diagnostics::Log("WARNING", "Using fallback configuration.");
Diagnostics::Log("ERROR", "Failed to load resource.");
Diagnostics::Log("DX12", "DirectX 12 device initialized.");
Diagnostics::Log("SHADER", "Shader compilation completed.");
```

---

### Throwing a Diagnostic Error

```cpp
if (!resource)
{
    throw Diagnostics::Error("Resource was null.");
}
```

Example during initialization:

```cpp
if (!ImGui::CreateContext())
{
    throw Diagnostics::Error("ImGui::CreateContext failed.");
}
```

---

### Checking an HRESULT

```cpp
HRESULT hr = CreateDXGIFactory2(
    0,
    IID_PPV_ARGS(&factory)
);

Diagnostics::CheckHRESULT(
    hr,
    "CreateDXGIFactory2"
);
```

HRESULT checks can also be used directly around DX12 calls:

```cpp
Diagnostics::CheckHRESULT(
    g_pd3dDevice->CreateFence(
        0,
        D3D12_FENCE_FLAG_NONE,
        IID_PPV_ARGS(&g_fence)
    ),
    "CreateFence"
);
```

---

### Checking Win32 Errors

Win32 errors can be converted into HRESULTs using `HRESULT_FROM_WIN32`:

```cpp
HANDLE eventHandle = CreateEvent(
    nullptr,
    FALSE,
    FALSE,
    nullptr
);

if (!eventHandle)
{
    Diagnostics::CheckHRESULT(
        HRESULT_FROM_WIN32(GetLastError()),
        "CreateEvent"
    );
}
```

Another example:

```cpp
if (!RegisterClassExW(&wc))
{
    Diagnostics::CheckHRESULT(
        HRESULT_FROM_WIN32(GetLastError()),
        "RegisterClassExW"
    );
}
```

---

### Handling Uncaught Exceptions

A top-level exception boundary can report any exception that escapes the application:

```cpp
int main()
{
    try
    {
        Diagnostics::Initialize();

        RunApplication();

        Diagnostics::Log(
            "INFO",
            "Application exited normally."
        );

        return 0;
    }
    catch (...)
    {
        Diagnostics::ExitFromCurrentException(
            "main"
        );
    }
}
```

For a Windows application:

```cpp
int APIENTRY wWinMain(
    HINSTANCE,
    HINSTANCE,
    LPWSTR,
    int
)
{
    try
    {
        Diagnostics::Initialize();

        return RunApplication();
    }
    catch (...)
    {
        Diagnostics::ExitFromCurrentException(
            "wWinMain"
        );
    }
}
```

---

### DirectX 12 Helper Macro

A helper macro can automatically record the expression that failed:

```cpp
void CheckD3D(
    HRESULT result,
    const char* operation,
    std::source_location location =
        std::source_location::current()
)
{
    Diagnostics::CheckHRESULT(
        result,
        operation,
        location
    );
}

#define DX_CHECK(call) CheckD3D((call), #call)
```

Usage:

```cpp
DX_CHECK(
    D3D12CreateDevice(
        nullptr,
        D3D_FEATURE_LEVEL_11_0,
        IID_PPV_ARGS(&g_pd3dDevice)
    )
);

DX_CHECK(
    g_pd3dCommandList->Reset(
        commandAllocator,
        nullptr
    )
);

DX_CHECK(
    g_pd3dCommandQueue->Signal(
        fence,
        fenceValue
    )
);
```

---

### Logging the DX12 Device Removal Reason

```cpp
HRESULT result = g_pSwapChain->Present(1, 0);

if (FAILED(result))
{
    HRESULT reason =
        g_pd3dDevice->GetDeviceRemovedReason();

    if (FAILED(reason))
    {
        char message[128];

        sprintf_s(
            message,
            "DX12 device removal reason: 0x%08lX",
            static_cast<unsigned long>(reason)
        );

        Diagnostics::Log(
            "ERROR",
            message
        );
    }

    Diagnostics::CheckHRESULT(
        result,
        "IDXGISwapChain::Present"
    );
}
```

---

### Logging DX12 Debug Layer Messages

```cpp
Microsoft::WRL::ComPtr<ID3D12InfoQueue> queue;

if (SUCCEEDED(
    g_pd3dDevice->QueryInterface(
        IID_PPV_ARGS(&queue)
    )))
{
    const UINT64 count =
        queue->GetNumStoredMessages();

    for (UINT64 i = 0; i < count; ++i)
    {
        SIZE_T size = 0;

        queue->GetMessage(
            i,
            nullptr,
            &size
        );

        std::vector<unsigned char> storage(size);

        auto* message =
            reinterpret_cast<D3D12_MESSAGE*>(
                storage.data()
            );

        if (SUCCEEDED(
            queue->GetMessage(
                i,
                message,
                &size
            )))
        {
            Diagnostics::Log(
                "DX12",
                message->pDescription
            );
        }
    }

    queue->ClearStoredMessages();
}
```

---

### Complete Example

```cpp
#include "Diagnostics.h"

int RunApplication()
{
    Diagnostics::Log(
        "INFO",
        "Initializing application."
    );

    HRESULT hr = SomeHRESULTReturningFunction();

    Diagnostics::CheckHRESULT(
        hr,
        "SomeHRESULTReturningFunction"
    );

    bool initialized = InitializeSomething();

    if (!initialized)
    {
        throw Diagnostics::Error(
            "InitializeSomething failed."
        );
    }

    Diagnostics::Log(
        "INFO",
        "Initialization completed."
    );

    return 0;
}

int main()
{
    try
    {
        Diagnostics::Initialize();

        const int result = RunApplication();

        Diagnostics::Log(
            "INFO",
            "Application exited normally."
        );

        return result;
    }
    catch (...)
    {
        Diagnostics::ExitFromCurrentException(
            "main"
        );
    }
}
```
