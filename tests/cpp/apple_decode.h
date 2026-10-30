// Apple's own decode of an audio file (macOS ExtAudioFile, AudioToolbox: the decoder family
// AVAudioPlayer played the game's MP3s through, ADR 0046), as 16-bit interleaved PCM at the
// file's rate and channel count: the independent reference of the music tests
// (mp3_real_test.cpp, game_audio_real_test.cpp). macOS only; link AudioToolbox and
// CoreFoundation.
#ifndef MAPLETHIEF_TESTS_CPP_APPLE_DECODE_H
#define MAPLETHIEF_TESTS_CPP_APPLE_DECODE_H

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace apple_decode {

// Apple's decode of `path` as 16-bit interleaved PCM at the file's rate and channel count.
inline bool decode(const std::filesystem::path &path, std::vector<std::int16_t> *out,
                   std::uint32_t *channels) {
    const std::string p = path.string();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8 *>(p.c_str()), static_cast<CFIndex>(p.size()), false);
    ExtAudioFileRef f = nullptr;
    const OSStatus st = ExtAudioFileOpenURL(url, &f);
    CFRelease(url);
    if (st != noErr) {
        return false;
    }
    AudioStreamBasicDescription file{};
    UInt32 size = sizeof file;
    ExtAudioFileGetProperty(f, kExtAudioFileProperty_FileDataFormat, &size, &file);
    AudioStreamBasicDescription client{};
    client.mSampleRate = file.mSampleRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = file.mChannelsPerFrame;
    client.mBitsPerChannel = 16;
    client.mBytesPerFrame = 2 * file.mChannelsPerFrame;
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = client.mBytesPerFrame;
    ExtAudioFileSetProperty(f, kExtAudioFileProperty_ClientDataFormat, sizeof client, &client);
    *channels = file.mChannelsPerFrame;
    std::vector<std::int16_t> buf(std::size_t{8192} * *channels);
    out->clear();
    for (;;) {
        AudioBufferList list{};
        list.mNumberBuffers = 1;
        list.mBuffers[0].mNumberChannels = *channels;
        list.mBuffers[0].mDataByteSize = static_cast<UInt32>(buf.size() * sizeof(std::int16_t));
        list.mBuffers[0].mData = buf.data();
        UInt32 frames = 8192;
        if (ExtAudioFileRead(f, &frames, &list) != noErr) {
            ExtAudioFileDispose(f);
            return false;
        }
        if (frames == 0) {
            break;
        }
        out->insert(out->end(), buf.begin(), buf.begin() + std::size_t{frames} * *channels);
    }
    ExtAudioFileDispose(f);
    return true;
}

} // namespace apple_decode

#endif

#endif // MAPLETHIEF_TESTS_CPP_APPLE_DECODE_H
