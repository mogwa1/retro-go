#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <driver/gpio.h>
#include <driver/i2s.h>
#include <string.h>
#include <unistd.h>

#include "rg_system.h"
#include "rg_audio.h"

#if RG_AUDIO_USE_INT_DAC
#include <driver/dac.h>
#endif

static const rg_audio_sink_t sinks[] = {
    {RG_AUDIO_SINK_DUMMY,   0, "Dummy"},
#if RG_AUDIO_USE_INT_DAC
    {RG_AUDIO_SINK_I2S_DAC, 0, "Speaker"},
#endif
#if RG_AUDIO_USE_EXT_DAC
    {RG_AUDIO_SINK_I2S_EXT, 0, "Ext DAC"},
#endif
    // {RG_AUDIO_SINK_BT_A2DP, 0, "Bluetooth"},
};

static rg_audio_t audio;
static SemaphoreHandle_t audioDevLock;
static int64_t dummyBusyUntil = 0;

static const char *SETTING_OUTPUT = "AudioSink";
static const char *SETTING_VOLUME = "Volume";
static const char *SETTING_FILTER = "AudioFilter";

#ifdef RG_TARGET_QTPY_GAMER
    #define SPEAKER_DAC_MODE I2S_DAC_CHANNEL_LEFT_EN
#else
    #define SPEAKER_DAC_MODE I2S_DAC_CHANNEL_BOTH_EN
#endif

#define ACQUIRE_DEVICE(timeout) ({int x=xSemaphoreTake(audioDevLock, timeout);if(!x)RG_LOGE("Failed to acquire lock!\n");x;})
#define RELEASE_DEVICE() xSemaphoreGive(audioDevLock);


void rg_audio_init(int sampleRate)
{
    RG_ASSERT(audio.sink == NULL, "Audio sink already initialized!");

    if (audioDevLock == NULL)
        audioDevLock = xSemaphoreCreateMutex();

    ACQUIRE_DEVICE(1000);

    int sinkType = (int)rg_settings_get_number(NS_GLOBAL, SETTING_OUTPUT, sinks[RG_COUNT(sinks) > 1 ? 1 : 0].type);
    for (size_t i = 0; i < RG_COUNT(sinks); ++i)
    {
        if (!audio.sink || sinks[i].type == sinkType)
            audio.sink = &sinks[i];
    }
    audio.filter = (int)rg_settings_get_number(NS_GLOBAL, SETTING_FILTER, 0);
    audio.volume = (int)rg_settings_get_number(NS_GLOBAL, SETTING_VOLUME, 50);
    audio.sampleRate = sampleRate;

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = sampleRate,
        .bits_per_sample = 16,
#ifdef RG_TARGET_ESPLAY_S3
	    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
	    .communication_format = I2S_COMM_FORMAT_STAND_I2S | I2S_COMM_FORMAT_STAND_MSB,
	    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1, // ESP_INTR_FLAG_LEVEL1
	    .dma_buf_count = 8, // Goal is to have ~800 samples over 2-8 buffers (3x270 or 5x180 are pretty good)
	    .dma_buf_len = 534, // The unit is stereo samples (4 bytes) (optimize for 533 usage)
	    .use_apll = false, // S3 cant use apll
#else
	    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
		.intr_alloc_flags = 0, // ESP_INTR_FLAG_LEVEL1
        .dma_buf_count = 4, // Goal is to have ~800 samples over 2-8 buffers (3x270 or 5x180 are pretty good)
        .dma_buf_len = 180, // The unit is stereo samples (4 bytes) (optimize for 533 usage)
        .use_apll = true, // External DAC may care about accuracy
#endif
    };
    esp_err_t ret = ESP_FAIL;

    if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC)
    {
    #if RG_AUDIO_USE_INT_DAC
        i2s_config.mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN;
        i2s_config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
        i2s_config.use_apll = false;
        if ((ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL)) == ESP_OK)
        {
            ret = i2s_set_dac_mode(SPEAKER_DAC_MODE);
        }
    #else
        RG_LOGE("This device does not support internal DAC mode!\n");
    #endif
    }
    else if (audio.sink->type == RG_AUDIO_SINK_I2S_EXT)
    {
        if ((ret = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL)) == ESP_OK)
        {
            ret = i2s_set_pin(I2S_NUM_0, &(i2s_pin_config_t) {
                .bck_io_num = RG_GPIO_SND_I2S_BCK,
                .ws_io_num = RG_GPIO_SND_I2S_WS,
                .data_out_num = RG_GPIO_SND_I2S_DATA,
                .data_in_num = GPIO_NUM_NC
            });
        }
    }
    else if (audio.sink->type == RG_AUDIO_SINK_DUMMY)
    {
        ret = ESP_OK;
    }

    #ifdef RG_GPIO_SND_AMP_ENABLE
        gpio_set_direction(RG_GPIO_SND_AMP_ENABLE, GPIO_MODE_OUTPUT);
        gpio_set_level(RG_GPIO_SND_AMP_ENABLE, audio.muted ? 0 : 1);
    #endif

    if (ret == ESP_OK)
    {
        RG_LOGI("Audio ready. sink='%s', samplerate=%d, volume=%d\n",
            audio.sink->name, audio.sampleRate, audio.volume);
    }
    else
    {
        RG_LOGE("Failed to initialize audio. sink='%s', samplerate=%d, err=0x%x\n",
            audio.sink->name, audio.sampleRate, ret);
        audio.sink = &sinks[0];
    }

    RELEASE_DEVICE();
}

void rg_audio_deinit(void)
{
    if (!audio.sink)
        return;

    // We'll go ahead even if we can't acquire the lock...
    ACQUIRE_DEVICE(1000);

    if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC)
    {
    #if RG_AUDIO_USE_INT_DAC
        i2s_driver_uninstall(I2S_NUM_0);
        if (SPEAKER_DAC_MODE & I2S_DAC_CHANNEL_RIGHT_EN)
            dac_output_disable(DAC_CHANNEL_1);
        if (SPEAKER_DAC_MODE & I2S_DAC_CHANNEL_LEFT_EN)
            dac_output_disable(DAC_CHANNEL_2);
        dac_i2s_disable();
    #else
        RG_LOGE("This device does not support internal DAC mode!\n");
    #endif
    }
    else if (audio.sink->type == RG_AUDIO_SINK_I2S_EXT)
    {
        i2s_driver_uninstall(I2S_NUM_0);
        gpio_reset_pin(RG_GPIO_SND_I2S_BCK);
        gpio_reset_pin(RG_GPIO_SND_I2S_DATA);
        gpio_reset_pin(RG_GPIO_SND_I2S_WS);
    }

    #ifdef RG_GPIO_SND_AMP_ENABLE
        gpio_reset_pin(RG_GPIO_SND_AMP_ENABLE);
    #endif

    RG_LOGI("Audio terminated. sink='%s'\n", audio.sink->name);
    audio.sink = NULL;

    RELEASE_DEVICE();
}

void rg_audio_submit(const rg_audio_sample_t *samples, size_t count)
{
    const int64_t time_start = rg_system_timer();

    if (!audio.sink)
        return;

    if (!samples || !count)
        return;

    if (!ACQUIRE_DEVICE(0))
        return;

    if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC || audio.sink->type == RG_AUDIO_SINK_I2S_EXT)
    {
        float volume = audio.muted ? 0.f : (audio.volume * 0.01f);
        rg_audio_sample_t buffer[180];
        size_t written = 0;
        size_t pos = 0;

        for (size_t i = 0; i < count; ++i)
        {
            int left = samples[i].left * volume;
            int right = samples[i].right * volume;

            // In speaker mode we use left and right as a differential mono output to increase resolution.
            if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC)
            {
                int sample = (left + right) >> 1;
                if (sample > 0x7F00)
                {
                    left  =  0x8000 + (sample - 0x7F00);
                    right = -0x8000 + 0x7F00;
                }
                else if (sample < -0x7F00)
                {
                    left  =  0x8000 + (sample + 0x7F00);
                    right = -0x8000 + -0x7F00;
                }
                else
                {
                    left  =  0x8000;
                    right = -0x8000 + sample;
                }
            }

            // Clipping   (not necessary, we have (int16 * vol) and volume is never more than 1.0)
            // if (left > 32767) left = 32767; else if (left < -32768) left = -32767;
            // if (right > 32767) right = 32767; else if (right < -32768) right = -32767;

            // Queue
            buffer[pos].left = left;
            buffer[pos].right = right;

            if (i == count - 1 || ++pos == RG_COUNT(buffer))
            {
                if (i2s_write(I2S_NUM_0, (void*)buffer, pos * 4, &written, 1000) != ESP_OK)
                    RG_LOGW("I2S Submission error! Written: %d/%d\n", written, pos * 4);
                pos = 0;
            }
        }
    }
    else if (audio.sink->type == RG_AUDIO_SINK_DUMMY)
    {
        // usleep(RG_MAX(dummyBusyUntil - rg_system_timer(), 1000));
        dummyBusyUntil = rg_system_timer() + ((audio.sampleRate * 1000) / count);
    }
    else
    {
        RG_LOGE("Audio sink not implemented: '%s' %d\n", audio.sink->name, audio.sink->type);
    }

    RELEASE_DEVICE();

    audio.counters.busyTime += rg_system_timer() - time_start;
}

const rg_audio_t *rg_audio_get_info(void)
{
    return &audio;
}

const rg_audio_sink_t *rg_audio_get_sinks(size_t *count)
{
    if (count) *count = RG_COUNT(sinks);
    return sinks;
}

const rg_audio_sink_t *rg_audio_get_sink(void)
{
    return audio.sink ?: &sinks[0];
}

void rg_audio_set_sink(rg_sink_type_t sink)
{
    rg_settings_set_number(NS_GLOBAL, SETTING_OUTPUT, sink);
    rg_audio_deinit();
    rg_audio_init(audio.sampleRate);
}

int rg_audio_get_volume(void)
{
    return audio.volume;
}

void rg_audio_set_volume(int percent)
{
    audio.volume = RG_MIN(RG_MAX(percent, 0), 100);
    rg_settings_set_number(NS_GLOBAL, SETTING_VOLUME, audio.volume);
    RG_LOGI("Volume set to %d%%\n", audio.volume);
}

void rg_audio_set_mute(bool mute)
{
    RG_ASSERT(audio.sink != NULL, "Audio device not ready!");

    // if (audio.muted == mute)
    //     return;

    if (!ACQUIRE_DEVICE(1000))
        return;

    #if defined(RG_GPIO_SND_AMP_ENABLE)
        gpio_set_level(RG_GPIO_SND_AMP_ENABLE, !mute);
    #elif defined(RG_TARGET_QTPY_GAMER)
        rg_i2c_gpio_set_level(AW_HEADPHONE_EN, !mute);
    #else
        // nothing to do
    #endif
    if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC || audio.sink->type == RG_AUDIO_SINK_I2S_EXT)
        i2s_zero_dma_buffer(I2S_NUM_0);

    audio.muted = mute;
    RELEASE_DEVICE();
}

int rg_audio_get_sample_rate(void)
{
    return audio.sampleRate;
}

void rg_audio_set_sample_rate(int sampleRate)
{
    RG_ASSERT(audio.sink != NULL, "Audio device not ready!");

    if (audio.sampleRate == sampleRate)
        return;

    if (!ACQUIRE_DEVICE(1000))
        return;

    if (audio.sink->type == RG_AUDIO_SINK_I2S_DAC || audio.sink->type == RG_AUDIO_SINK_I2S_EXT)
    {
        RG_LOGI("i2s_set_sample_rates(%d)\n", sampleRate);
        i2s_set_sample_rates(I2S_NUM_0, sampleRate);
    }

    audio.sampleRate = sampleRate;
    RELEASE_DEVICE();
}
