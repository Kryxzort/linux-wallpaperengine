#pragma once

#include "../JSON.h"
#include "../Model/Types.h"

#include <memory>
#include <string>
#include <vector>

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class DynamicValueParser {
public:
    /**
     * `expectString`: a string value stays a string (a text object's text), never a number or a vector.
     * `expectColor`: an object's colour field, read as WE does (ColorBuilder::parseObject: floats, a number is all
     * three); with `wholeColorBytes` LWE's older rule instead (no decimal point = 0..255, a particle colorrandom's)
     */
    static Model::DynamicValueUniquePtr parse (
	const JSON& data, const Properties& properties, bool expectColor, bool expectString = false,
	bool wholeColorBytes = false
    );
    /**
     * Parses a property's `animation` object (G01/G02 of docs/player/spikes/we-lwe-gap-audit.md) the way
     * Wallpaper Engine 2.8 does: `options` needs `fps` > 0 and `length` > 0 or there is no animation; curves
     * `c0`..`c3` in order (a missing one ends the list); `relative` offsets c0..c2 by the property's `value` when
     * that is an "x y z" string
     *
     * @return The animation, or nullptr when it doesn't describe one
     */
    static Model::PropertyAnimationSharedPtr parseAnimation (const JSON& animation, const JSON& value);

    /**
     * While one of these lives (on this thread), every value parsed with an animation is appended to `out`
     */
    class AnimationCollector {
    public:
	explicit AnimationCollector (std::vector<Model::AnimatedValue>& out);
	/** links each animation with an `options.parent.key` to its sibling's (same owner, that key) */
	~AnimationCollector ();
	AnimationCollector (const AnimationCollector&) = delete;
	AnimationCollector& operator= (const AnimationCollector&) = delete;

	struct Named {
	    Model::PropertyAnimationSharedPtr animation;
	    const void* owner;
	    std::string key;
	};

    private:
	std::vector<Model::AnimatedValue>* m_previous;
	std::vector<Named>* m_previousNamed;
	std::vector<Named> m_named;
    };

    /**
     * The property being parsed: the JSON object holding it (an object, an effect pass's constantshadervalues) and its
     * key — what an animation's `options.parent.key` names its parent by
     */
    class PropertyScope {
    public:
	PropertyScope (const void* owner, const std::string& key);
	~PropertyScope ();
	PropertyScope (const PropertyScope&) = delete;
	PropertyScope& operator= (const PropertyScope&) = delete;

    private:
	const void* m_owner;
	const std::string* m_key;
    };
};
}