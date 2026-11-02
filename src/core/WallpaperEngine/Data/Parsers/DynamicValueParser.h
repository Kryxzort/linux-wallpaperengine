#pragma once

#include "../JSON.h"
#include "../Model/Types.h"

#include <memory>
#include <vector>

namespace WallpaperEngine::Data::Parsers {
using JSON = WallpaperEngine::Data::JSON::JSON;
using namespace WallpaperEngine::Data::Model;

class DynamicValueParser {
public:
    static Model::DynamicValueUniquePtr parse (const JSON& data, const Properties& properties, bool expectColor);
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
	~AnimationCollector ();
	AnimationCollector (const AnimationCollector&) = delete;
	AnimationCollector& operator= (const AnimationCollector&) = delete;

    private:
	std::vector<Model::AnimatedValue>* m_previous;
    };
};
}