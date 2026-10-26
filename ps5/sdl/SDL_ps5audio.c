/*
 * James Pond 2 for the PlayStation 5 - SDL3's audio driver for the console.
 *
 * One playback device: an AudioOut main port, 48 kHz stereo 16-bit, a grain
 * of 256 frames. sceAudioOutOutput blocks while the console plays the grain
 * before, which paces SDL's audio thread.
 *
 * SPDX-License-Identifier: Zlib
 */
#include "SDL_internal.h"

#ifdef SDL_AUDIO_DRIVER_PS5

#include "audio/SDL_audiodev_c.h"
#include "audio/SDL_sysaudio.h"

int sceAudioOutInit(void);
int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t grain, uint32_t rate, uint32_t format);
int sceAudioOutOutput(int32_t port, const void *samples);
int sceAudioOutClose(int32_t port);

#define PS5_AUDIO_GRAIN               256
#define PS5_AUDIO_ALREADY_INITIALISED ((int)0x8026000e)
#define PS5_AUDIO_USER_SYSTEM         0xff
#define PS5_AUDIO_PORT_MAIN           0
#define PS5_AUDIO_FORMAT_S16_STEREO   1

struct SDL_PrivateAudioData
{
    int port;
    Uint8 *buffer;
};

static bool PS5AUDIO_OpenDevice(SDL_AudioDevice *device)
{
    int init;

    device->hidden = (struct SDL_PrivateAudioData *)SDL_calloc(1, sizeof(*device->hidden));
    if (!device->hidden) {
        return false;
    }
    device->hidden->port = -1;

    device->spec.format = SDL_AUDIO_S16LE;
    device->spec.channels = 2;
    device->spec.freq = 48000;
    device->sample_frames = PS5_AUDIO_GRAIN;
    SDL_UpdatedAudioDeviceFormat(device);

    device->hidden->buffer = (Uint8 *)SDL_aligned_alloc(64, device->buffer_size);
    if (!device->hidden->buffer) {
        return false;
    }
    SDL_memset(device->hidden->buffer, 0, device->buffer_size);

    init = sceAudioOutInit();
    if (init < 0 && init != PS5_AUDIO_ALREADY_INITIALISED) {
        return SDL_SetError("sceAudioOutInit: 0x%08x", (unsigned)init);
    }
    device->hidden->port = sceAudioOutOpen(PS5_AUDIO_USER_SYSTEM, PS5_AUDIO_PORT_MAIN, 0, PS5_AUDIO_GRAIN, 48000,
                                           PS5_AUDIO_FORMAT_S16_STEREO);
    if (device->hidden->port < 0) {
        return SDL_SetError("sceAudioOutOpen: 0x%08x", (unsigned)device->hidden->port);
    }
    return true;
}

static bool PS5AUDIO_PlayDevice(SDL_AudioDevice *device, const Uint8 *buffer, int buffer_size)
{
    return sceAudioOutOutput(device->hidden->port, buffer) >= 0;
}

static bool PS5AUDIO_WaitDevice(SDL_AudioDevice *device)
{
    return true; // PlayDevice blocks
}

static Uint8 *PS5AUDIO_GetDeviceBuf(SDL_AudioDevice *device, int *buffer_size)
{
    return device->hidden->buffer;
}

static void PS5AUDIO_CloseDevice(SDL_AudioDevice *device)
{
    if (device->hidden) {
        if (device->hidden->port >= 0) {
            (void)sceAudioOutOutput(device->hidden->port, NULL); // drain
            (void)sceAudioOutClose(device->hidden->port);
        }
        SDL_aligned_free(device->hidden->buffer);
        SDL_free(device->hidden);
        device->hidden = NULL;
    }
}

static bool PS5AUDIO_Init(SDL_AudioDriverImpl *impl)
{
    impl->OpenDevice = PS5AUDIO_OpenDevice;
    impl->PlayDevice = PS5AUDIO_PlayDevice;
    impl->WaitDevice = PS5AUDIO_WaitDevice;
    impl->GetDeviceBuf = PS5AUDIO_GetDeviceBuf;
    impl->CloseDevice = PS5AUDIO_CloseDevice;
    impl->OnlyHasDefaultPlaybackDevice = true;
    return true;
}

AudioBootStrap PS5AUDIO_bootstrap = {
    "ps5", "PlayStation 5 audio driver", PS5AUDIO_Init, false, false
};

#endif // SDL_AUDIO_DRIVER_PS5
