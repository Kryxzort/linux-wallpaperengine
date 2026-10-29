#pragma once

#include "WallpaperEngine/Audio/AudioStream.h"

namespace WallpaperEngine {
class Context;
}

namespace WallpaperEngine::Audio {
class AudioStream;

namespace Drivers {
    /**
     * Base class for audio driver implementations
     */
    class AudioDriver {
    public:
	explicit AudioDriver (Context& applicationContext);

	virtual ~AudioDriver () = default;
	/**
	 * Registers the given stream in the driver for playing
	 *
	 * @param stream The stream, or nullptr for a slot that plays nothing until replaceStream fills it
	 */
	virtual int addStream (AudioStream* stream) = 0;

	/**
	 *
	 * @param streamId The stream to stop playing
	 */
	virtual void removeStream (int streamId) = 0;

	/**
	 * Swaps the stream a slot plays (a sound object's next file); the previous one is no longer
	 * touched by the driver once this returns, so the caller can delete it
	 *
	 * @param streamId The slot from addStream
	 * @param stream The new stream, or nullptr to play nothing
	 */
	virtual void replaceStream (int streamId, AudioStream* stream) = 0;

	/**
	 * Updates status of the different audio settings
	 */
	virtual void update ();

	/**
	 * TODO: MAYBE THIS SHOULD BE OUR OWN DEFINITIONS INSTEAD OF LIBRARY SPECIFIC ONES?
	 *
	 * @return The audio format the driver supports
	 */
	[[nodiscard]] virtual AVSampleFormat getFormat () const = 0;
	/**
	 * @return The sample rate the driver supports
	 */
	[[nodiscard]] virtual int getSampleRate () const = 0;
	/**
	 * @return The channels the driver supports
	 */
	[[nodiscard]] virtual int getChannels () const = 0;
	/**
	 * @return The application context under which the audio driver is initialized
	 */
	Context& getContext () const;

    private:
	Context& m_context;
    };
} // namespace Drivers
} // namespace WallpaperEngine::Audio