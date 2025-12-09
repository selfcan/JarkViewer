#pragma once

#include "jarkUtils.h"
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;


class VideoDecoder {
private:
    // RAII 封装 MFStartup/MFShutdown
    class MFSession {
    public:
        MFSession() { MFStartup(MF_VERSION); }
        ~MFSession() { MFShutdown(); }
    };

public:
    static std::vector<cv::Mat> DecodeVideoFrames(const uint8_t* videoBuffer, size_t size) {
        // 确保 MF 环境初始化（利用 RAII 自动析构）
        MFSession mfSession;
        std::vector<cv::Mat> frames;
        HRESULT hr = S_OK;

        // 1. 创建 ByteStream
        ComPtr<IMFByteStream> pByteStream;
        hr = MFCreateTempFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_DELETE_IF_EXIST, MF_FILEFLAGS_NONE, &pByteStream);
        if (FAILED(hr)) { JARK_LOG("Failed to create byte stream"); return frames; }

        ULONG bytesWritten = 0;
        hr = pByteStream->Write(videoBuffer, (ULONG)size, &bytesWritten);
        if (FAILED(hr)) return frames;

        // 重置指针
        hr = pByteStream->SetCurrentPosition(0);
        if (FAILED(hr)) return frames;

        // 2. 创建 SourceReader
        ComPtr<IMFAttributes> pAttributes;
        MFCreateAttributes(&pAttributes, 1);
        // 开启硬件加速（显卡解码可能会产生 Stride 对齐问题，这正是我们需要修复的）
        pAttributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);

        // 修正颜色范围转换（可选，防止画面发灰）
        pAttributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

        ComPtr<IMFSourceReader> pSourceReader;
        hr = MFCreateSourceReaderFromByteStream(pByteStream.Get(), pAttributes.Get(), &pSourceReader);
        if (FAILED(hr)) { JARK_LOG("Failed to create SourceReader"); return frames; }

        // 3. 配置解码格式
        GUID outputFormat = ConfigureDecoder(pSourceReader.Get());
        if (outputFormat == GUID_NULL) return frames;

        // 4. 获取视频尺寸信息
        ComPtr<IMFMediaType> pCurrentType;
        hr = pSourceReader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pCurrentType);
        if (FAILED(hr)) return frames;

        UINT32 width = 0, height = 0;
        MFGetAttributeSize(pCurrentType.Get(), MF_MT_FRAME_SIZE, &width, &height);

        // 获取旋转信息
        UINT32 rotation = 0;
        pCurrentType->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation);

        JARK_LOG("Decoding video: {}x{}, Rotation: {}", width, height, rotation);

        // 5. 解码循环
        while (true) {
            DWORD streamFlags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> pSample;

            hr = pSourceReader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                nullptr,
                &streamFlags,
                &timestamp,
                &pSample
            );

            if (FAILED(hr)) break;
            if (streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) break;

            if (pSample) {
                // 核心修复在 ConvertSampleToMat 内部
                cv::Mat frame = ConvertSampleToMat(pSample.Get(), width, height, outputFormat, rotation);
                if (!frame.empty()) {
                    frames.emplace_back(std::move(frame));
                }
            }
        }

        return frames;
    }

private:
    static GUID ConfigureDecoder(IMFSourceReader* pReader) {
        ComPtr<IMFMediaType> pType;
        HRESULT hr = MFCreateMediaType(&pType);

        pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);

        // 优先尝试 YUY2。而 NV12 在部分视频上有绿边现象（可能与硬件解码有关）
        GUID formats[] = { MFVideoFormat_YUY2, MFVideoFormat_NV12, MFVideoFormat_RGB32 };

        for (const auto& fmt : formats) {
            pType->SetGUID(MF_MT_SUBTYPE, fmt);
            hr = pReader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, pType.Get());
            if (SUCCEEDED(hr)) {
                return fmt;
            }
        }

        JARK_LOG("Could not set a supported output format");
        return GUID_NULL;
    }

    static cv::Mat ConvertSampleToMat(IMFSample* pSample, UINT32 width, UINT32 height, const GUID& subtype, UINT32 rotation) {
        ComPtr<IMFMediaBuffer> pBuffer;

        HRESULT hr = pSample->ConvertToContiguousBuffer(&pBuffer);
        if (FAILED(hr)) return cv::Mat();

        BYTE* pData = nullptr;
        DWORD maxLength = 0, currentLength = 0;
        LONG stride = 0;

        ComPtr<IMF2DBuffer> p2DBuffer;
        hr = pBuffer.As(&p2DBuffer);

        if (SUCCEEDED(hr) && p2DBuffer) {
            hr = p2DBuffer->Lock2D(&pData, &stride);
            if (FAILED(hr)) return cv::Mat();
        }
        else {
            hr = pBuffer->Lock(&pData, &maxLength, &currentLength);
            if (FAILED(hr)) return cv::Mat();
            if (subtype == MFVideoFormat_RGB32) stride = width * 4;
            else if (subtype == MFVideoFormat_YUY2) stride = width * 2;
            else if (subtype == MFVideoFormat_NV12) stride = width; // Y 平面 stride
        }

        cv::Mat result;
        bool locked2D = (p2DBuffer != nullptr);

        // 处理负 Stride (通常出现在 RGB32 底部向上的位图中)
        bool flipVertically = false;
        if (stride < 0) {
            stride = -stride;
            flipVertically = true;
        }

        try {
            if (subtype == MFVideoFormat_YUY2) {
                // YUY2: 2 bytes per pixel, 1 plane
                cv::Mat yuvRaw(height, width, CV_8UC2, pData, stride);
                cv::cvtColor(yuvRaw, result, cv::COLOR_YUV2BGR_YUY2);
            }
            else if (subtype == MFVideoFormat_NV12) {
                // NV12: Y plane (Height) + UV plane (Height/2)
                cv::Mat yuvRaw(height * 3 / 2, width, CV_8UC1, pData, stride);
                cv::cvtColor(yuvRaw, result, cv::COLOR_YUV2BGR_NV12);
            }
            else if (subtype == MFVideoFormat_RGB32) {
                // RGB32: 4 bytes per pixel
                cv::Mat bgraRaw(height, width, CV_8UC4, pData, stride);
                // 仅做拷贝或转换
                cv::cvtColor(bgraRaw, result, cv::COLOR_BGRA2BGR);
            }
        }
        catch (...) {
            JARK_LOG("OpenCV Exception during color conversion");
        }

        // 解锁
        if (locked2D) {
            p2DBuffer->Unlock2D();
        }
        else {
            pBuffer->Unlock();
        }

        if (result.empty()) return result;

        // 处理旋转
        if (rotation != MFVideoRotationFormat_0) {
            cv::Mat rotated;
            if (rotation == MFVideoRotationFormat_90)
                cv::rotate(result, rotated, cv::ROTATE_90_CLOCKWISE);
            else if (rotation == MFVideoRotationFormat_180)
                cv::rotate(result, rotated, cv::ROTATE_180);
            else if (rotation == MFVideoRotationFormat_270)
                cv::rotate(result, rotated, cv::ROTATE_90_COUNTERCLOCKWISE);

            return rotated;
        }

        if (flipVertically && subtype == MFVideoFormat_RGB32) {
            cv::flip(result, result, 0);
        }

        return result;
    }
};
