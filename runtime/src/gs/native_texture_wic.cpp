#include "native_texture.hpp"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace ps2::gs::native_texture
{

bool decode_png(const std::uint8_t *data, std::size_t size, s_image &output)
{
    output = {};
    constexpr std::uint8_t signature[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!data || size < sizeof signature || size > 64u * 1024u * 1024u
        || std::memcmp(data, signature, sizeof signature) != 0)
        return false;

    // WIC is included with Windows. Do not silently disable PNG replacement
    // when a separately installed zlib package is absent from the toolchain.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(init) && init != RPC_E_CHANGED_MODE)
        return false;
    const bool uninitialize = SUCCEEDED(init);

    bool success = false;
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;

        HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result))
            result = factory->CreateStream(&stream);
        if (SUCCEEDED(result))
            result = stream->InitializeFromMemory(
                const_cast<BYTE *>(reinterpret_cast<const BYTE *>(data)),
                static_cast<DWORD>(size));
        if (SUCCEEDED(result))
            result = factory->CreateDecoderFromStream(stream.Get(), nullptr,
                WICDecodeMetadataCacheOnLoad, &decoder);
        if (SUCCEEDED(result))
            result = decoder->GetFrame(0, &frame);

        UINT width = 0, height = 0;
        if (SUCCEEDED(result))
            result = frame->GetSize(&width, &height);
        if (SUCCEEDED(result) && (!width || !height
            || width > 4096 || height > 4096))
            result = E_INVALIDARG;

        if (SUCCEEDED(result))
            result = factory->CreateFormatConverter(&converter);
        if (SUCCEEDED(result))
            result = converter->Initialize(frame.Get(),
                GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                nullptr, 0.0, WICBitmapPaletteTypeCustom);

        if (SUCCEEDED(result))
        {
            const std::size_t bytes = static_cast<std::size_t>(width) * height * 4u;
            if (bytes <= std::numeric_limits<UINT>::max())
            {
                std::vector<std::uint8_t> pixels(bytes);
                result = converter->CopyPixels(nullptr, width * 4u,
                    static_cast<UINT>(bytes), pixels.data());
                if (SUCCEEDED(result))
                {
                    output.width = width;
                    output.height = height;
                    output.rgba = std::move(pixels);
                    success = true;
                }
            }
        }
    }

    if (uninitialize)
        CoUninitialize();
    return success;
}

}
#endif
