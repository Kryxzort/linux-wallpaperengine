#include "DynamicValueParser.h"

#include "../Model/DynamicValue.h"
#include "../Model/PropertyAnimation.h"
#include "UserSettingParser.h"

#include <climits>
#include <sstream>

using namespace WallpaperEngine::Data::Parsers;
using namespace WallpaperEngine::Data::Model;

namespace {
thread_local std::vector<AnimatedValue>* g_animatedValues = nullptr;

// a `back`/`front` handle: an object, enabled unless `"enabled": false`; a disabled one is (0, 0)
bool parseHandle (const JSON& key, const char* name, glm::vec2& out) {
    const auto it = key.find (name);

    if (it == key.end () || !it->is_object ()) {
	return false;
    }
    if (const auto enabled = it->find ("enabled"); enabled != it->end () && enabled->is_boolean () && !enabled->get<bool> ()) {
	return false;
    }

    const auto x = it->find ("x");
    const auto y = it->find ("y");

    out.x = x != it->end () && x->is_number () ? x->get<float> () : 0.0f;
    out.y = y != it->end () && y->is_number () ? y->get<float> () : 0.0f;
    return true;
}

PropertyAnimation::Curve parseCurve (const JSON& keys, float offset) {
    PropertyAnimation::Curve curve;
    int last = INT_MIN;

    for (const auto& key : keys) {
	if (!key.is_object ()) {
	    continue;
	}

	const auto value = key.find ("value");
	const auto frame = key.find ("frame");

	if (value == key.end () || frame == key.end () || !value->is_number () || !frame->is_number ()) {
	    continue;
	}

	PropertyAnimation::Keyframe keyframe {
	    .frame = static_cast<int> (frame->get<double> ()),
	    .value = value->get<float> () + offset,
	};

	// keys must move forward in time, one that doesn't is dropped
	if (keyframe.frame <= last) {
	    continue;
	}

	const auto step = key.find ("step");

	keyframe.step = step != key.end () && step->is_boolean () && step->get<bool> ();
	keyframe.backEnabled = parseHandle (key, "back", keyframe.back);
	keyframe.frontEnabled = parseHandle (key, "front", keyframe.front);
	last = keyframe.frame;
	curve.push_back (keyframe);
    }

    return curve;
}
} // namespace

DynamicValueParser::AnimationCollector::AnimationCollector (std::vector<AnimatedValue>& out) :
    m_previous (g_animatedValues) {
    g_animatedValues = &out;
}

DynamicValueParser::AnimationCollector::~AnimationCollector () { g_animatedValues = this->m_previous; }

PropertyAnimationSharedPtr DynamicValueParser::parseAnimation (const JSON& animation, const JSON& value) {
    if (!animation.is_object ()) {
	return nullptr;
    }

    const auto options = animation.find ("options");

    if (options == animation.end () || !options->is_object ()) {
	return nullptr;
    }

    const auto fpsIt = options->find ("fps");
    const auto lengthIt = options->find ("length");

    if (fpsIt == options->end () || lengthIt == options->end () || !fpsIt->is_number () || !lengthIt->is_number ()) {
	return nullptr;
    }

    const float fps = fpsIt->get<float> ();
    const int length = static_cast<int> (lengthIt->get<double> ());

    if (fps <= 0.0f || static_cast<float> (length) / fps <= 0.0f) {
	return nullptr;
    }

    const auto flag = [&options] (const char* name) {
	const auto it = options->find (name);
	return it != options->end () && it->is_boolean () && it->get<bool> ();
    };
    const auto text = [] (const JSON& obj, const char* name) {
	const auto it = obj.find (name);
	return it != obj.end () && it->is_string () ? it->get<std::string> () : std::string ();
    };

    // `relative`: the curves are offsets from the property's value — WE adds an "x y z" string value to c0..c2
    // (a value that's a number, or a string of fewer than three numbers, adds nothing)
    float offsets [3] = { 0.0f, 0.0f, 0.0f };

    if (animation.contains ("relative") && value.is_string ()) {
	std::istringstream in (value.get<std::string> ());
	float x, y, z;

	if (in >> x >> y >> z) {
	    offsets [0] = x;
	    offsets [1] = y;
	    offsets [2] = z;
	}
    }

    std::vector<PropertyAnimation::Curve> curves;

    for (const char* name : { "c0", "c1", "c2", "c3" }) {
	const auto it = animation.find (name);

	if (it == animation.end () || !it->is_array ()) {
	    break;
	}

	curves.push_back (parseCurve (*it, curves.size () < 3 ? offsets [curves.size ()] : 0.0f));
    }

    return std::make_shared<PropertyAnimation> (
	text (*options, "name"), fps, length, text (*options, "mode"), flag ("startpaused"), flag ("wraploop"),
	std::move (curves)
    );
}

DynamicValueUniquePtr DynamicValueParser::parse (const JSON& data, const Properties& properties, bool expectColor) {
    auto value = std::make_unique<DynamicValue> ();
    auto valueIt = data;
    std::optional<std::string> scriptSource = std::nullopt;
    std::optional<JSON> scriptPropsJson = std::nullopt;

    if (data.is_object ()) {
	const auto user = data.optional ("user");
	const auto script = data.optional ("script");
	valueIt = data.require ("value", "User setting must have a value");

	if (script.has_value () && !script->is_null ()) {
	    scriptSource = script->get<std::string> ();
	    scriptPropsJson = data.optional ("scriptproperties");
	}
    }

    // actual value parsing
    if (valueIt.is_string ()) {
	if (expectColor) {
	    value->update (Builders::ColorBuilder::parse (valueIt), DynamicValue::UpdateSource::Initialization);
	} else {
	    std::string str = valueIt;
	    int size = Builders::VectorBuilder::preparseSize (str);

	    if (size == 1) {
		// scalar? text value?
		std::size_t parsed = 0;
		try {
		    float f = std::stof (str, &parsed);

		    if (parsed == str.size ()) {
			value->update (f, DynamicValue::UpdateSource::Initialization);
		    } else {
			value->update (str, DynamicValue::UpdateSource::Initialization);
		    }
		} catch (const std::exception&) {
		    value->update (str, DynamicValue::UpdateSource::Initialization);
		}
	    } else if (size == 2) {
		value->update (static_cast<glm::vec2> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else if (size == 3) {
		value->update (static_cast<glm::vec3> (valueIt), DynamicValue::UpdateSource::Initialization);
	    } else {
		value->update (static_cast<glm::vec4> (valueIt), DynamicValue::UpdateSource::Initialization);
	    }
	}
    } else if (valueIt.is_number_integer ()) {
	value->update (valueIt.get<int> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_number_float ()) {
	value->update (valueIt.get<float> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_boolean ()) {
	value->update (valueIt.get<bool> (), DynamicValue::UpdateSource::Initialization);
    } else if (valueIt.is_null ()) {
	// null value with no connection to property
	value->update (DynamicValue::UpdateSource::Initialization);
    }

    if (scriptSource.has_value ()) {
	std::map<std::string, UserSettingUniquePtr> scriptProps;

	if (scriptPropsJson.has_value () && scriptPropsJson->is_object ()) {
	    for (const auto& [key, propData] : scriptPropsJson->items ()) {
		scriptProps[key] = UserSettingParser::parse (propData, properties);
	    }
	}

	value->setProperties (std::move (scriptProps));
	value->setScriptSource (scriptSource.value ());
    }

    if (data.is_object ()) {
	if (const auto animation = data.optional ("animation"); animation.has_value ()) {
	    if (auto parsed = parseAnimation (*animation, valueIt); parsed != nullptr) {
		value->setAnimation (std::move (parsed));

		if (g_animatedValues != nullptr) {
		    g_animatedValues->push_back ({ .value = value.get (), .alive = value->getAliveFlag () });
		}
	    }
	}
    }

    return value;
}