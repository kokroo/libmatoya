// This Source Code Form is subject to the terms of the MIT License.
// If a copy of the MIT License was not distributed with this file,
// You can obtain one at https://spdx.org/licenses/MIT.html.

#include "matoya.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include "audio-common.h"

#include <aaudio/AAudio.h>

struct MTY_Audio {
	struct audio_common cmn;

	AAudioStreamBuilder *builder;
	AAudioStream *stream;

	MTY_Mutex *mutex;
	bool flushing;
	bool playing;

	uint32_t min_buffer_size;
	uint32_t max_buffer_size;
	uint8_t *buffer;
	size_t size;
};

static void audio_error(AAudioStream *stream, void *userData, aaudio_result_t error)
{
	MTY_Log("'AAudioStream' error %d", error);
}

static aaudio_data_callback_result_t audio_callback(AAudioStream *stream, void *userData,
	void *audioData, int32_t numFrames)
{
	MTY_Audio *ctx = userData;

	MTY_MutexLock(ctx->mutex);

	size_t want_size = numFrames * ctx->cmn.stats.frame_size;

	if (ctx->playing && ctx->size >= want_size) {
		memcpy(audioData, ctx->buffer, want_size);
		ctx->size -= want_size;

		memmove(ctx->buffer, ctx->buffer + want_size, ctx->size);

	} else {
		memset(audioData, 0, want_size);
	}

	MTY_MutexUnlock(ctx->mutex);

	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

MTY_Audio *MTY_AudioCreate(MTY_AudioFormat format, uint32_t minBuffer,
	uint32_t maxBuffer, const char *deviceID, bool fallback)
{
	MTY_Audio *ctx = MTY_Alloc(1, sizeof(MTY_Audio));
	audio_common_init(&ctx->cmn, format, minBuffer, maxBuffer);
	ctx->min_buffer_size = ctx->cmn.stats.min_buffer * ctx->cmn.stats.frame_size;
	ctx->max_buffer_size = ctx->cmn.stats.max_buffer * ctx->cmn.stats.frame_size;

	ctx->mutex = MTY_MutexCreate();
	ctx->buffer = MTY_Alloc(ctx->cmn.stats.buffer_size, 1);

	return ctx;
}

void MTY_AudioDestroy(MTY_Audio **audio)
{
	if (!audio || !*audio)
		return;

	MTY_Audio *ctx = *audio;

	MTY_AudioReset(ctx);

	MTY_MutexDestroy(&ctx->mutex);
	MTY_Free(ctx->buffer);

	MTY_Free(ctx);
	*audio = NULL;
}

void MTY_AudioReset(MTY_Audio *ctx)
{
	if (ctx->stream) {
		MTY_MutexLock(ctx->mutex);

		ctx->playing = false;
		ctx->flushing = false;
		ctx->size = 0;

		MTY_MutexUnlock(ctx->mutex);

		AAudioStream_requestStop(ctx->stream);
		AAudioStream_close(ctx->stream);
		ctx->stream = NULL;
	}

	if (ctx->builder) {
		AAudioStreamBuilder_delete(ctx->builder);
		ctx->builder = NULL;
	}
}

uint32_t MTY_AudioGetQueued(MTY_Audio *ctx)
{
	return (ctx->size / ctx->cmn.stats.frame_size) / ctx->cmn.format.sampleRate * 1000;
}

static void audio_start(MTY_Audio *ctx)
{
	if (!ctx->builder) {
		AAudio_createStreamBuilder(&ctx->builder);
		AAudioStreamBuilder_setDeviceId(ctx->builder, AAUDIO_UNSPECIFIED);
		AAudioStreamBuilder_setSampleRate(ctx->builder, ctx->cmn.format.sampleRate);
		AAudioStreamBuilder_setChannelCount(ctx->builder, ctx->cmn.format.channels);
		// XXX: Setting channel mask via AAudioStreamBuilder_setChannelMask requires bumping up android platform from 28 to 32.
		//      We have decided not to do this as of 11/06/2024 so as not to exclude a significant portion of users.
		AAudioStreamBuilder_setFormat(ctx->builder, ctx->cmn.format.sampleFormat == MTY_AUDIO_SAMPLE_FORMAT_FLOAT
			? AAUDIO_FORMAT_PCM_FLOAT : AAUDIO_FORMAT_PCM_I16);
		AAudioStreamBuilder_setPerformanceMode(ctx->builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
		AAudioStreamBuilder_setErrorCallback(ctx->builder, audio_error, ctx);
		AAudioStreamBuilder_setDataCallback(ctx->builder, audio_callback, ctx);
	}

	if (!ctx->stream) {
		AAudioStreamBuilder_openStream(ctx->builder, &ctx->stream);
		AAudioStream_requestStart(ctx->stream);
	}
}

void MTY_AudioQueue(MTY_Audio *ctx, const int16_t *frames, uint32_t count)
{
	size_t data_size = count * ctx->cmn.stats.frame_size;

	audio_start(ctx);

	MTY_MutexLock(ctx->mutex);

	if (ctx->size + data_size >= ctx->max_buffer_size)
		ctx->flushing = true;

	size_t minimum_request = AAudioStream_getFramesPerBurst(ctx->stream) * ctx->cmn.stats.frame_size;
	if (ctx->flushing && ctx->size < minimum_request) {
		memset(ctx->buffer, 0, ctx->size);
		ctx->size = 0;
	}

	if (ctx->size == 0) {
		ctx->playing = false;
		ctx->flushing = false;
	}

	if (!ctx->flushing && data_size + ctx->size <= ctx->cmn.stats.buffer_size) {
		memcpy(ctx->buffer + ctx->size, frames, data_size);
		ctx->size += data_size;
	}

	if (ctx->size >= ctx->min_buffer_size)
		ctx->playing = true;

	MTY_MutexUnlock(ctx->mutex);
}
