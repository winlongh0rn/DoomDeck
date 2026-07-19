#include "CaptureEngine.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

// COM identifies this interface by UUID. Giving it a local C++ name avoids
// SDK/header namespace differences around IDirect3DDxgiInterfaceAccess.
struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
    DoomDeckDirect3DDxgiInterfaceAccess : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(
        REFIID iid,
        void** object) = 0;
};

namespace
{
    static void DebugLog(const char* m){ SYSTEMTIME st{}; GetLocalTime(&st); std::cerr<<"["<<st.wHour<<":"<<st.wMinute<<":"<<st.wSecond<<"."<<st.wMilliseconds<<"][T"<<GetCurrentThreadId()<<"] "<<m<<"\n"; }

    constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\DoomDeckFrames";
    constexpr wchar_t kInputPipeName[] = L"\\\\.\\pipe\\DoomDeckInput";
    constexpr std::uint32_t kProtocolVersion = 1;
    constexpr std::uint32_t kOutputWidth = 720;
    constexpr std::uint32_t kOutputHeight = 432;
    constexpr std::uint32_t kOutputChannels = 4;
    constexpr int kMaximumFramesPerSecond = 15;

#pragma pack(push, 1)
    struct FrameHeader
    {
        char magic[4];                 // "DDRF"
        std::uint32_t version;         // 1
        std::uint32_t width;           // 720
        std::uint32_t height;          // 432
        std::uint32_t channels;        // 4 (RGBA)
        std::uint32_t payloadSize;     // width * height * channels
        std::uint64_t frameNumber;
    };
#pragma pack(pop)

    static_assert(sizeof(FrameHeader) == 32);

#pragma pack(push, 1)
    struct InputPacket
    {
        std::uint8_t action;  // 0 = press, 1 = release
        std::uint8_t control; // 0 through 14
    };
#pragma pack(pop)

    static_assert(sizeof(InputPacket) == 2);

    enum class InputAction : std::uint8_t
    {
        Press = 0,
        Release = 1
    };

    enum class DoomControl : std::uint8_t
    {
        PreviousWeapon = 0,
        Run = 1,
        Fire = 2,
        Use = 3,
        NextWeapon = 4,

        Map = 5,
        TurnLeft = 6,
        Forward = 7,
        TurnRight = 8,
        Escape = 9,

        QuickSave = 10,
        StrafeLeft = 11,
        Backward = 12,
        StrafeRight = 13,
        QuickLoad = 14,

        Count = 15
    };

    constexpr std::size_t kDoomControlCount =
        static_cast<std::size_t>(DoomControl::Count);

    WORD VirtualKeyForControl(DoomControl control)
    {
        switch (control)
        {
        case DoomControl::PreviousWeapon:
            return static_cast<WORD>('Q');

        case DoomControl::Run:
            return VK_SHIFT;

        case DoomControl::Fire:
            return VK_CONTROL;

        case DoomControl::Use:
            return VK_SPACE;

        case DoomControl::NextWeapon:
            return static_cast<WORD>('E');

        case DoomControl::Map:
            return VK_TAB;

        case DoomControl::TurnLeft:
            return VK_LEFT;

        case DoomControl::Forward:
            return VK_UP;

        case DoomControl::TurnRight:
            return VK_RIGHT;

        case DoomControl::Escape:
            return VK_ESCAPE;

        case DoomControl::QuickSave:
            return VK_F6;

        case DoomControl::StrafeLeft:
            return VK_OEM_COMMA;

        case DoomControl::Backward:
            return VK_DOWN;

        case DoomControl::StrafeRight:
            return VK_OEM_PERIOD;

        case DoomControl::QuickLoad:
            return VK_F9;

        default:
            return 0;
        }
    }

    const char* ControlName(DoomControl control)
    {
        switch (control)
        {
        case DoomControl::PreviousWeapon: return "Previous Weapon";
        case DoomControl::Run: return "Run";
        case DoomControl::Fire: return "Fire";
        case DoomControl::Use: return "Use";
        case DoomControl::NextWeapon: return "Next Weapon";
        case DoomControl::Map: return "Map";
        case DoomControl::TurnLeft: return "Turn Left";
        case DoomControl::Forward: return "Forward";
        case DoomControl::TurnRight: return "Turn Right";
        case DoomControl::Escape: return "Escape";
        case DoomControl::QuickSave: return "Quick Save";
        case DoomControl::StrafeLeft: return "Strafe Left";
        case DoomControl::Backward: return "Backward";
        case DoomControl::StrafeRight: return "Strafe Right";
        case DoomControl::QuickLoad: return "Quick Load";
        default: return "Unknown";
        }
    }

    bool IsExtendedVirtualKey(WORD virtualKey)
    {
        switch (virtualKey)
        {
        case VK_LEFT:
        case VK_RIGHT:
        case VK_UP:
        case VK_DOWN:
        case VK_HOME:
        case VK_END:
        case VK_PRIOR:
        case VK_NEXT:
        case VK_INSERT:
        case VK_DELETE:
        case VK_DIVIDE:
        case VK_NUMLOCK:
            return true;

        default:
            return false;
        }
    }

    bool SendVirtualKey(WORD virtualKey, bool pressed)
    {
        if (virtualKey == 0)
        {
            return false;
        }

        /*
         * Scan-code injection is more reliable for games than sending only a
         * virtual-key value. It also behaves more like a physical keyboard.
         */
        UINT scanCode = MapVirtualKeyW(
            virtualKey,
            MAPVK_VK_TO_VSC_EX);

        if (scanCode == 0)
        {
            std::cerr
                << "MapVirtualKeyW failed for virtual key "
                << virtualKey
                << ".\n";
            return false;
        }

        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = 0;
        input.ki.wScan = static_cast<WORD>(scanCode & 0xFF);
        input.ki.dwFlags = KEYEVENTF_SCANCODE;

        if (IsExtendedVirtualKey(virtualKey) ||
            (scanCode & 0xFF00) == 0xE000)
        {
            input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
        }

        if (!pressed)
        {
            input.ki.dwFlags |= KEYEVENTF_KEYUP;
        }

        SetLastError(ERROR_SUCCESS);

        const UINT sent = SendInput(1, &input, sizeof(input));

        if (sent != 1)
        {
            const DWORD error = GetLastError();

            std::cerr
                << "SendInput injected 0 events for virtual key "
                << virtualKey
                << "; GetLastError="
                << error
                << ". This commonly means the target game is running"
                << " as administrator while DoomDeckCapture is not.\n";

            return false;
        }

        return true;
    }

    class InputServer
    {
    public:
        explicit InputServer(std::atomic_bool& stopRequested)
            : m_stopRequested(stopRequested)
        {
            m_thread = std::thread([this]()
            {
                Run();
            });
        }

        ~InputServer()
        {
            Stop();
        }

        InputServer(const InputServer&) = delete;
        InputServer& operator=(const InputServer&) = delete;

        void Stop()
        {
            bool expected = false;

            if (!m_stopping.compare_exchange_strong(expected, true))
            {
                return;
            }

            if (m_thread.joinable())
            {
                // Wake the thread if it is blocked in ConnectNamedPipe or
                // ReadFile. ReleaseAllKeys() is also called before it exits.
                CancelSynchronousIo(m_thread.native_handle());
                m_thread.join();
            }
        }

    private:
        void Run()
        {
            while (!m_stopping.load() && !m_stopRequested.load())
            {
                DebugLog("Creating input pipe...");
                HANDLE pipe = CreateNamedPipeW(
                    kInputPipeName,
                    // Node's net.Socket opens Windows named pipes as duplex
                    // streams. An inbound-only server makes Node observe an
                    // immediate EOF on its read side, so it repeatedly closes
                    // and reconnects. Keep the pipe duplex even though this
                    // helper currently only reads control packets.
                    PIPE_ACCESS_DUPLEX,
                    PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                    1,
                    0,
                    4096,
                    0,
                    nullptr);

                if (pipe == INVALID_HANDLE_VALUE)
                {
                    const DWORD error = GetLastError();

                    if (!m_stopping.load())
                    {
                        std::cerr
                            << "CreateNamedPipeW for DoomDeck input failed"
                            << " with error "
                            << error
                            << ".\n";
                    }

                    return;
                }

                DebugLog("Input pipe created.");
                std::wcerr
                    << L"Waiting for DoomDeck controls on "
                    << kInputPipeName
                    << L" ...\n";

                const BOOL connected = ConnectNamedPipe(pipe, nullptr);
                const DWORD connectError =
                    connected ? ERROR_SUCCESS : GetLastError();

                if (!connected && connectError != ERROR_PIPE_CONNECTED)
                {
                    CloseHandle(pipe);

                    if (m_stopping.load() ||
                        connectError == ERROR_OPERATION_ABORTED)
                    {
                        break;
                    }

                    std::cerr
                        << "DoomDeck input ConnectNamedPipe failed with error "
                        << connectError
                        << ". Retrying.\n";

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(250));
                    continue;
                }

                std::cerr << "DoomDeck controls connected.\n";
                ReadClient(pipe);

                ReleaseAllKeys();
                DisconnectNamedPipe(pipe);
                CloseHandle(pipe);

                if (!m_stopping.load() && !m_stopRequested.load())
                {
                    std::cerr
                        << "DoomDeck controls disconnected. Waiting for"
                        << " reconnection.\n";
                }
            }

            ReleaseAllKeys();
        }

        void ReadClient(HANDLE pipe)
        {
            std::array<std::uint8_t, sizeof(InputPacket)> packetBytes{};
            std::size_t bytesCollected = 0;

            while (!m_stopping.load() && !m_stopRequested.load())
            {
                DWORD bytesRead = 0;

                const BOOL result = ReadFile(
                    pipe,
                    packetBytes.data() + bytesCollected,
                    static_cast<DWORD>(
                        packetBytes.size() - bytesCollected),
                    &bytesRead,
                    nullptr);

                if (!result || bytesRead == 0)
                {
                    const DWORD error = GetLastError();

                    if (error != ERROR_BROKEN_PIPE &&
                        error != ERROR_PIPE_NOT_CONNECTED &&
                        error != ERROR_OPERATION_ABORTED &&
                        !m_stopping.load())
                    {
                        std::cerr
                            << "DoomDeck input ReadFile failed with error "
                            << error
                            << ".\n";
                    }

                    return;
                }

                bytesCollected += bytesRead;

                if (bytesCollected < packetBytes.size())
                {
                    continue;
                }

                InputPacket packet{};
                std::memcpy(
                    &packet,
                    packetBytes.data(),
                    sizeof(packet));

                bytesCollected = 0;
                HandlePacket(packet);
            }
        }

        void HandlePacket(const InputPacket& packet)
        {
            if (packet.action >
                    static_cast<std::uint8_t>(InputAction::Release) ||
                packet.control >=
                    static_cast<std::uint8_t>(DoomControl::Count))
            {
                std::cerr
                    << "Ignored invalid DoomDeck input packet: action "
                    << static_cast<unsigned int>(packet.action)
                    << ", control "
                    << static_cast<unsigned int>(packet.control)
                    << ".\n";

                return;
            }

            const auto control =
                static_cast<DoomControl>(packet.control);
            const auto index =
                static_cast<std::size_t>(control);
            const bool pressed =
                packet.action ==
                static_cast<std::uint8_t>(InputAction::Press);

            // Ignore duplicate downs and duplicate ups. This also protects
            // against key-repeat behavior from the Stream Deck software.
            if (m_pressed[index] == pressed)
            {
                return;
            }

            const WORD virtualKey = VirtualKeyForControl(control);

            std::cerr
                << (pressed ? "DOWN " : "UP   ")
                << ControlName(control)
                << " (VK "
                << virtualKey
                << ")\n";

            if (SendVirtualKey(virtualKey, pressed))
            {
                m_pressed[index] = pressed;
            }
        }

        void ReleaseAllKeys()
        {
            for (std::size_t index = 0;
                 index < m_pressed.size();
                 ++index)
            {
                if (!m_pressed[index])
                {
                    continue;
                }

                const auto control =
                    static_cast<DoomControl>(index);

                SendVirtualKey(
                    VirtualKeyForControl(control),
                    false);

                m_pressed[index] = false;
            }
        }

        std::atomic_bool& m_stopRequested;
        std::atomic_bool m_stopping = false;
        std::array<bool, kDoomControlCount> m_pressed{};
        std::thread m_thread;
    };

    struct D3DResources
    {
        winrt::com_ptr<ID3D11Device> device;
        winrt::com_ptr<ID3D11DeviceContext> context;

        winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice
            winrtDevice{nullptr};
    };

    class PipeHandle
    {
    public:
        PipeHandle() = default;

        ~PipeHandle()
        {
            Close();
        }

        PipeHandle(const PipeHandle&) = delete;
        PipeHandle& operator=(const PipeHandle&) = delete;

        bool CreateAndWaitForClient()
        {
            Close();

            DebugLog("Creating frame pipe...");
            m_handle = CreateNamedPipeW(
                kPipeName,
                PIPE_ACCESS_OUTBOUND,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                1,
                4 * 1024 * 1024,
                0,
                0,
                nullptr);

            if (m_handle == INVALID_HANDLE_VALUE)
            {
                std::cerr
                    << "CreateNamedPipeW failed with error "
                    << GetLastError()
                    << ".\n";

                return false;
            }

            DebugLog("Frame pipe created.");
            std::wcerr
                << L"Waiting for DoomDeck on "
                << kPipeName
                << L" ...\n";

            const BOOL connected = ConnectNamedPipe(m_handle, nullptr);

            if (!connected && GetLastError() != ERROR_PIPE_CONNECTED)
            {
                std::cerr
                    << "ConnectNamedPipe failed with error "
                    << GetLastError()
                    << ".\n";

                Close();
                return false;
            }

            std::cerr << "DoomDeck connected. Streaming 720x432 RGBA frames.\n";
            return true;
        }

        bool WriteAll(const void* data, std::size_t size)
        {
            if (m_handle == INVALID_HANDLE_VALUE)
            {
                return false;
            }

            const auto* bytes = static_cast<const std::uint8_t*>(data);
            std::size_t totalWritten = 0;

            while (totalWritten < size)
            {
                const std::size_t bytesRemaining = size - totalWritten;
                const DWORD chunkSize =
                    bytesRemaining > static_cast<std::size_t>(0x7fffffff)
                        ? 0x7fffffff
                        : static_cast<DWORD>(bytesRemaining);

                DWORD bytesWritten = 0;

                const BOOL result = WriteFile(
                    m_handle,
                    bytes + totalWritten,
                    chunkSize,
                    &bytesWritten,
                    nullptr);

                if (!result || bytesWritten == 0)
                {
                    return false;
                }

                totalWritten += bytesWritten;
            }

            return true;
        }

        void Close()
        {
            if (m_handle != INVALID_HANDLE_VALUE)
            {
                DisconnectNamedPipe(m_handle);
                CloseHandle(m_handle);
                m_handle = INVALID_HANDLE_VALUE;
            }
        }

    private:
        HANDLE m_handle = INVALID_HANDLE_VALUE;
    };

    winrt::Windows::Graphics::Capture::GraphicsCaptureItem
    CreateCaptureItem(HWND hwnd)
    {
        auto factory =
            winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                ::IGraphicsCaptureItemInterop>();

        winrt::Windows::Graphics::Capture::GraphicsCaptureItem item{nullptr};

        winrt::check_hresult(
            factory->CreateForWindow(
                hwnd,
                winrt::guid_of<
                    winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                winrt::put_abi(item)));

        return item;
    }

    D3DResources CreateD3DResources()
    {
        D3DResources resources;

        UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifdef _DEBUG
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

        D3D_FEATURE_LEVEL featureLevel{};

        HRESULT result = D3D11CreateDevice(
            nullptr,
            D3D_DRIVER_TYPE_HARDWARE,
            nullptr,
            flags,
            nullptr,
            0,
            D3D11_SDK_VERSION,
            resources.device.put(),
            &featureLevel,
            resources.context.put());

#ifdef _DEBUG
        if (result == DXGI_ERROR_SDK_COMPONENT_MISSING)
        {
            flags &= ~D3D11_CREATE_DEVICE_DEBUG;
            resources.device = nullptr;
            resources.context = nullptr;

            result = D3D11CreateDevice(
                nullptr,
                D3D_DRIVER_TYPE_HARDWARE,
                nullptr,
                flags,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                resources.device.put(),
                &featureLevel,
                resources.context.put());
        }
#endif

        winrt::check_hresult(result);

        winrt::com_ptr<IDXGIDevice> dxgiDevice;
        winrt::check_hresult(
            resources.device->QueryInterface(dxgiDevice.put()));

        winrt::com_ptr<::IInspectable> inspectableDevice;
        winrt::check_hresult(
            CreateDirect3D11DeviceFromDXGIDevice(
                dxgiDevice.get(),
                inspectableDevice.put()));

        resources.winrtDevice =
            inspectableDevice.as<
                winrt::Windows::Graphics::DirectX::Direct3D11::
                    IDirect3DDevice>();

        return resources;
    }

    winrt::com_ptr<ID3D11Texture2D>
    GetTextureFromSurface(
        const winrt::Windows::Graphics::DirectX::Direct3D11::
            IDirect3DSurface& surface)
    {
        auto access =
            surface.as<DoomDeckDirect3DDxgiInterfaceAccess>();

        winrt::com_ptr<ID3D11Texture2D> texture;

        winrt::check_hresult(
            access->GetInterface(
                __uuidof(ID3D11Texture2D),
                texture.put_void()));

        return texture;
    }

    class FrameWriter
    {
    public:
        FrameWriter(
            ID3D11Device* device,
            ID3D11DeviceContext* context,
            PipeHandle& pipe,
            std::atomic_bool& stopRequested)
            : m_device(device),
              m_context(context),
              m_pipe(pipe),
              m_stopRequested(stopRequested),
              m_capturePixels(
                  static_cast<std::size_t>(kOutputWidth) *
                  static_cast<std::size_t>(kOutputHeight) *
                  kOutputChannels),
              m_pendingPixels(m_capturePixels.size()),
              m_sendPixels(m_capturePixels.size())
        {
            m_lastSubmitted =
                std::chrono::steady_clock::now() -
                std::chrono::milliseconds(1000 / kMaximumFramesPerSecond);

            m_writerThread = std::thread([this]()
            {
                WriterLoop();
            });
        }

        ~FrameWriter()
        {
            Stop();
        }

        FrameWriter(const FrameWriter&) = delete;
        FrameWriter& operator=(const FrameWriter&) = delete;

        void Stop()
        {
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);

                if (m_writerStopping)
                {
                    return;
                }

                m_writerStopping = true;
                m_hasPendingFrame = false;
            }

            m_queueCondition.notify_all();

            if (m_writerThread.joinable())
            {
                // If the pipe client stopped reading while a synchronous
                // WriteFile call was in progress, cancel that I/O so shutdown
                // cannot hang forever.
                CancelSynchronousIo(m_writerThread.native_handle());
                m_writerThread.join();
            }
        }

        void ProcessFrame(
            ID3D11Texture2D* sourceTexture,
            int contentWidth,
            int contentHeight)
        {
            if (!sourceTexture || m_stopRequested.load())
            {
                return;
            }

            // FrameArrived can be called again while the previous callback is
            // still finishing. Serialize the D3D immediate-context work and
            // apply the 15 FPS limit here.
            std::lock_guard<std::mutex> captureLock(m_captureMutex);

            const auto now = std::chrono::steady_clock::now();
            const auto minimumFrameInterval =
                std::chrono::milliseconds(1000 / kMaximumFramesPerSecond);

            if (now - m_lastSubmitted < minimumFrameInterval)
            {
                return;
            }

            D3D11_TEXTURE2D_DESC sourceDescription{};
            sourceTexture->GetDesc(&sourceDescription);

            EnsureStagingTexture(sourceDescription);

            m_context->CopyResource(
                m_stagingTexture.get(),
                sourceTexture);

            D3D11_MAPPED_SUBRESOURCE mapped{};

            winrt::check_hresult(
                m_context->Map(
                    m_stagingTexture.get(),
                    0,
                    D3D11_MAP_READ,
                    0,
                    &mapped));

            const int textureWidth =
                static_cast<int>(sourceDescription.Width);
            const int textureHeight =
                static_cast<int>(sourceDescription.Height);

            const int sourceWidth =
                contentWidth < textureWidth ? contentWidth : textureWidth;
            const int sourceHeight =
                contentHeight < textureHeight ? contentHeight : textureHeight;

            if (sourceWidth <= 0 || sourceHeight <= 0)
            {
                m_context->Unmap(m_stagingTexture.get(), 0);
                return;
            }

            const auto* sourceBytes =
                static_cast<const std::uint8_t*>(mapped.pData);

            // Nearest-neighbour resize from captured BGRA to packed
            // 720x432 RGBA. The native helper performs this once so Node can
            // immediately split the already-sized image into Stream Deck keys.
            for (std::uint32_t outputY = 0;
                 outputY < kOutputHeight;
                 ++outputY)
            {
                const int sourceY =
                    static_cast<int>(
                        (static_cast<std::uint64_t>(outputY) *
                         static_cast<std::uint64_t>(sourceHeight)) /
                        kOutputHeight);

                const auto* sourceRow =
                    sourceBytes +
                    static_cast<std::size_t>(sourceY) * mapped.RowPitch;

                auto* destinationRow =
                    m_capturePixels.data() +
                    static_cast<std::size_t>(outputY) *
                        static_cast<std::size_t>(kOutputWidth) *
                        kOutputChannels;

                for (std::uint32_t outputX = 0;
                     outputX < kOutputWidth;
                     ++outputX)
                {
                    const int sourceX =
                        static_cast<int>(
                            (static_cast<std::uint64_t>(outputX) *
                             static_cast<std::uint64_t>(sourceWidth)) /
                            kOutputWidth);

                    const auto* sourcePixel =
                        sourceRow +
                        static_cast<std::size_t>(sourceX) * 4;

                    auto* destinationPixel =
                        destinationRow +
                        static_cast<std::size_t>(outputX) * 4;

                    destinationPixel[0] = sourcePixel[2]; // R
                    destinationPixel[1] = sourcePixel[1]; // G
                    destinationPixel[2] = sourcePixel[0]; // B
                    destinationPixel[3] = sourcePixel[3]; // A
                }
            }

            m_context->Unmap(m_stagingTexture.get(), 0);

            FrameHeader header{};
            std::memcpy(header.magic, "DDRF", 4);
            header.version = kProtocolVersion;
            header.width = kOutputWidth;
            header.height = kOutputHeight;
            header.channels = kOutputChannels;
            header.payloadSize =
                static_cast<std::uint32_t>(m_capturePixels.size());
            header.frameNumber = m_nextFrameNumber++;

            {
                std::lock_guard<std::mutex> queueLock(m_queueMutex);

                if (m_writerStopping)
                {
                    return;
                }

                // Overwrite any unsent frame. That keeps latency low when the
                // Stream Deck renderer is slower than capture.
                m_pendingHeader = header;
                std::copy(
                    m_capturePixels.begin(),
                    m_capturePixels.end(),
                    m_pendingPixels.begin());
                m_hasPendingFrame = true;
            }

            m_lastSubmitted = now;
            m_queueCondition.notify_one();
        }

    private:
        void WriterLoop()
        {
            while (true)
            {
                FrameHeader header{};

                {
                    std::unique_lock<std::mutex> lock(m_queueMutex);

                    m_queueCondition.wait(
                        lock,
                        [this]()
                        {
                            return m_writerStopping || m_hasPendingFrame;
                        });

                    if (m_writerStopping)
                    {
                        return;
                    }

                    header = m_pendingHeader;
                    std::copy(
                        m_pendingPixels.begin(),
                        m_pendingPixels.end(),
                        m_sendPixels.begin());
                    m_hasPendingFrame = false;
                }

                const bool wroteHeader =
                    m_pipe.WriteAll(&header, sizeof(header));

                const bool wrotePixels =
                    wroteHeader &&
                    m_pipe.WriteAll(
                        m_sendPixels.data(),
                        m_sendPixels.size());

                if (!wrotePixels)
                {
                    const DWORD error = GetLastError();

                    if (error != ERROR_OPERATION_ABORTED)
                    {
                        std::cerr
                            << "DoomDeck disconnected from the frame pipe"
                            << " (WriteFile error "
                            << error
                            << ").\n";
                    }

                    m_stopRequested.store(true);
                    return;
                }

                if (header.frameNumber % 30 == 0)
                {
                    std::cerr
                        << "Sent frame "
                        << header.frameNumber
                        << ".\n";
                }
            }
        }

        void EnsureStagingTexture(
            const D3D11_TEXTURE2D_DESC& sourceDescription)
        {
            if (m_stagingTexture &&
                m_stagingWidth == sourceDescription.Width &&
                m_stagingHeight == sourceDescription.Height &&
                m_stagingFormat == sourceDescription.Format)
            {
                return;
            }

            D3D11_TEXTURE2D_DESC stagingDescription =
                sourceDescription;

            stagingDescription.BindFlags = 0;
            stagingDescription.MiscFlags = 0;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

            m_stagingTexture = nullptr;

            winrt::check_hresult(
                m_device->CreateTexture2D(
                    &stagingDescription,
                    nullptr,
                    m_stagingTexture.put()));

            m_stagingWidth = sourceDescription.Width;
            m_stagingHeight = sourceDescription.Height;
            m_stagingFormat = sourceDescription.Format;
        }

        ID3D11Device* m_device = nullptr;
        ID3D11DeviceContext* m_context = nullptr;
        PipeHandle& m_pipe;
        std::atomic_bool& m_stopRequested;

        winrt::com_ptr<ID3D11Texture2D> m_stagingTexture;
        UINT m_stagingWidth = 0;
        UINT m_stagingHeight = 0;
        DXGI_FORMAT m_stagingFormat = DXGI_FORMAT_UNKNOWN;

        std::vector<std::uint8_t> m_capturePixels;
        std::vector<std::uint8_t> m_pendingPixels;
        std::vector<std::uint8_t> m_sendPixels;

        FrameHeader m_pendingHeader{};
        std::uint64_t m_nextFrameNumber = 0;
        std::chrono::steady_clock::time_point m_lastSubmitted;

        std::mutex m_captureMutex;
        std::mutex m_queueMutex;
        std::condition_variable m_queueCondition;
        bool m_hasPendingFrame = false;
        bool m_writerStopping = false;
        std::thread m_writerThread;
    };
}

CaptureEngine::CaptureEngine(HWND hwnd)
    : m_hwnd(hwnd)
{
}

bool CaptureEngine::Initialize()
{
    try
    {
        using GraphicsCaptureSession =
            winrt::Windows::Graphics::Capture::GraphicsCaptureSession;

        using Direct3D11CaptureFramePool =
            winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;

        using DirectXPixelFormat =
            winrt::Windows::Graphics::DirectX::DirectXPixelFormat;

        if (!m_hwnd || !IsWindow(m_hwnd))
        {
            std::cerr << "Invalid UZDoom window handle.\n";
            return false;
        }

        if (!GraphicsCaptureSession::IsSupported())
        {
            std::cerr << "Windows Graphics Capture is not supported.\n";
            return false;
        }

        std::atomic_bool stopRequested = false;

        // Start the control pipe immediately. The Stream Deck plugin connects
        // to both pipes and automatically retries until each one exists.
        InputServer inputServer(stopRequested);

        PipeHandle pipe;

        if (!pipe.CreateAndWaitForClient())
        {
            inputServer.Stop();
            return false;
        }

        auto captureItem = CreateCaptureItem(m_hwnd);
        D3DResources d3d = CreateD3DResources();

        const auto initialSize = captureItem.Size();

        if (initialSize.Width <= 0 || initialSize.Height <= 0)
        {
            std::cerr << "Capture item has an invalid size.\n";
            return false;
        }

        std::cerr
            << "Capture item size: "
            << initialSize.Width
            << " x "
            << initialSize.Height
            << "\n";

        auto framePool =
            Direct3D11CaptureFramePool::CreateFreeThreaded(
                d3d.winrtDevice,
                DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2,
                initialSize);

        auto session = framePool.CreateCaptureSession(captureItem);

        // The mouse pointer is not useful on a 15-key Doom display.
        session.IsCursorCaptureEnabled(false);

        FrameWriter frameWriter(
            d3d.device.get(),
            d3d.context.get(),
            pipe,
            stopRequested);

        const auto frameToken =
            framePool.FrameArrived(
                [&](Direct3D11CaptureFramePool const& sender,
                    winrt::Windows::Foundation::IInspectable const&)
                {
                    try
                    {
                        auto frame = sender.TryGetNextFrame();

                        if (!frame || stopRequested.load())
                        {
                            return;
                        }

                        const auto contentSize = frame.ContentSize();
                        auto texture =
                            GetTextureFromSurface(frame.Surface());

                        frameWriter.ProcessFrame(
                            texture.get(),
                            contentSize.Width,
                            contentSize.Height);
                    }
                    catch (const winrt::hresult_error& error)
                    {
                        std::wcerr
                            << L"Frame capture error: "
                            << error.message().c_str()
                            << L"\n";

                        stopRequested.store(true);
                    }
                    catch (const std::exception& error)
                    {
                        std::cerr
                            << "Frame capture error: "
                            << error.what()
                            << "\n";

                        stopRequested.store(true);
                    }
                });

        std::cerr << "Starting continuous Windows Graphics Capture...\n";
        session.StartCapture();

        while (!stopRequested.load() && IsWindow(m_hwnd))
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(100));
        }

        session.Close();
        framePool.FrameArrived(frameToken);
        framePool.Close();
        frameWriter.Stop();
        pipe.Close();

        // Releases any simulated keys that are still held and shuts down the
        // input pipe thread.
        stopRequested.store(true);
        inputServer.Stop();

        if (!IsWindow(m_hwnd))
        {
            std::cerr << "UZDoom window closed.\n";
        }

        return true;
    }
    catch (const winrt::hresult_error& error)
    {
        std::wcerr
            << L"Windows capture error: "
            << error.message().c_str()
            << L"\n";

        return false;
    }
    catch (const std::exception& error)
    {
        std::cerr
            << "Capture error: "
            << error.what()
            << "\n";

        return false;
    }
}