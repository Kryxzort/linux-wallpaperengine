#include <algorithm>
#include <ranges>

#include <SDL.h>

#include "HostAudioDriver.h"

#include "WallpaperEngine/Audio/AudioPlayingDetector.h"
#include "WallpaperEngine/Context.h"

using namespace WallpaperEngine::Audio;
using namespace WallpaperEngine::Audio::Drivers;

HostAudioDriver::HostAudioDriver (Context& applicationContext, const int sampleRate, const int channels) :
    AudioDriver (applicationContext), m_sampleRate (std::clamp (sampleRate, 8000, 192000)),
    m_channels (std::clamp (channels, 1, 2)) { }

HostAudioDriver::~HostAudioDriver () {
    std::lock_guard lock (this->m_streamsMutex);

    for (const auto* buffer : this->m_streams | std::views::values) {
	delete buffer;
    }

    this->m_streams.clear ();
}

int HostAudioDriver::addStream (AudioStream* stream) {
    std::lock_guard lock (this->m_streamsMutex);
    const int newStreamId = this->m_lastStreamID++;

    this->m_streams.insert_or_assign (newStreamId, new HostAudioBuffer { .stream = stream });

    return newStreamId;
}

void HostAudioDriver::removeStream (const int streamId) {
    std::lock_guard lock (this->m_streamsMutex);
    const auto it = this->m_streams.find (streamId);

    if (it == this->m_streams.end ()) {
	return;
    }

    delete it->second;
    this->m_streams.erase (it);
}

void HostAudioDriver::replaceStream (const int streamId, AudioStream* stream) {
    std::lock_guard lock (this->m_streamsMutex);
    const auto it = this->m_streams.find (streamId);

    if (it == this->m_streams.end ()) {
	return;
    }

    // the previous stream's decoded rest is dropped with it
    it->second->stream = stream;
    it->second->size = 0;
    it->second->index = 0;
}

int HostAudioDriver::streamCount () {
    std::lock_guard lock (this->m_streamsMutex);

    return static_cast<int> (this->m_streams.size ());
}

size_t HostAudioDriver::pull (float* out, const size_t frames) {
    if (out == nullptr || frames == 0) {
	return 0;
    }

    const size_t samples = frames * this->m_channels;

    std::fill_n (out, samples, 0.0f);

    // the configuration's mute check pauses every stream, exactly like the SDL driver's callback
    if (this->getContext ().audio->getDetector ().anythingPlaying ()) {
	return frames;
    }

    std::lock_guard lock (this->m_streamsMutex);

    for (auto* buffer : this->m_streams | std::views::values) {
	if (buffer->stream == nullptr || !buffer->stream->isInitialized () || buffer->stream->isPlayedOut ()) {
	    continue;
	}

	// the stream's own level (a sound object's volume²; not clamped, > 1 boosts)
	const float gain = buffer->stream->getGain ();
	size_t written = 0;

	while (written < samples && this->getContext ().isRunning) {
	    if (buffer->index >= buffer->size) {
		// decodeFrame blocks on an empty packet queue; a pull must never wait for decoding, so an
		// empty queue is silence for the rest of this pull (and wakes the read thread)
		if (buffer->stream->isQueueEmpty ()) {
		    // the read thread is done and everything it queued is mixed: the file has played out
		    if (buffer->stream->isReadEnded () && buffer->stream->isQueueEmpty ()) {
			buffer->stream->markPlayedOut ();
		    }

		    SDL_CondSignal (buffer->stream->getWaitCondition ());
		    break;
		}

		const int bytes = buffer->stream->decodeFrame (buffer->data, sizeof (buffer->data));

		buffer->index = 0;

		if (bytes < 0) {
		    // decode error: a short stretch of silence, like the SDL driver
		    buffer->size = 256;
		    std::fill_n (reinterpret_cast<float*> (buffer->data), buffer->size, 0.0f);
		} else {
		    buffer->size = std::min (static_cast<size_t> (bytes), sizeof (buffer->data)) / sizeof (float);
		}

		if (buffer->size == 0) {
		    break;
		}
	    }

	    const auto* source = reinterpret_cast<const float*> (buffer->data) + buffer->index;
	    const size_t count = std::min (samples - written, buffer->size - buffer->index);

	    for (size_t i = 0; i < count; i++) {
		out[written + i] += source[i] * gain;
	    }

	    written += count;
	    buffer->index += count;
	}
    }

    return frames;
}

AVSampleFormat HostAudioDriver::getFormat () const { return AV_SAMPLE_FMT_FLT; }

int HostAudioDriver::getSampleRate () const { return this->m_sampleRate; }

int HostAudioDriver::getChannels () const { return this->m_channels; }
