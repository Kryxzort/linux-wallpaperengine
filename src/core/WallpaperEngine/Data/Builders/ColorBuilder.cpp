#include "ColorBuilder.h"
#include "VectorBuilder.h"

#include <algorithm>
#include <cstdlib>
#include <format>
#include <glm/glm.hpp>
#include <glm/vec3.hpp>

const WallpaperEngine::Data::Model::Color WallpaperEngine::Data::Builders::ColorBuilder::White
    = WallpaperEngine::Data::Model::Color (1.0f, 1.0f, 1.0f, 1.0f);
const WallpaperEngine::Data::Model::Color WallpaperEngine::Data::Builders::ColorBuilder::Black
    = WallpaperEngine::Data::Model::Color (0.0f, 0.0f, 0.0f, 1.0f);

WallpaperEngine::Data::Model::Color
WallpaperEngine::Data::Builders::ColorBuilder::parseProperty (const std::string& value) {
    auto copy = value;
    std::ranges::replace (copy, ',', ' ');

    if (copy.find ('#') != 0 && copy.find ('.') == std::string::npos) {
	const int vectorSize = VectorBuilder::preparseSize (copy);

	if (vectorSize == 3 || vectorSize == 4) {
	    const glm::ivec4 whole = vectorSize == 3 ? glm::ivec4 (VectorBuilder::parse<glm::ivec3> (copy), 1)
						     : VectorBuilder::parse<glm::ivec4> (copy);

	    if (glm::all (glm::lessThanEqual (whole, glm::ivec4 (1))) && glm::all (glm::greaterThanEqual (whole, glm::ivec4 (0)))) {
		return Model::Color (glm::vec4 (whole));
	    }
	}
    }

    return parse (value);
}

WallpaperEngine::Data::Model::Color
WallpaperEngine::Data::Builders::ColorBuilder::parseObject (const std::string& value) {
    if (value.find ('#') == 0) {
	return parse (value);
    }

    // GH 0x1401d0bc0: atof at the start, then at the first non-space after each run of spaces, three times
    float components[3] = { 0.0f, 0.0f, 0.0f };
    const char* cursor = value.c_str ();

    for (int i = 0; i < 3 && *cursor != '\0'; i++) {
	components[i] = static_cast<float> (std::atof (cursor));

	while (*cursor != '\0' && *cursor != ' ') {
	    cursor++;
	}
	while (*cursor == ' ') {
	    cursor++;
	}
    }

    return { components[0], components[1], components[2], 1.0f };
}

WallpaperEngine::Data::Model::Color
WallpaperEngine::Data::Builders::ColorBuilder::parse (const std::string& value, float alpha) {
    auto copy = value;

    // replace the actual separators with spaces to normalize them
    if (copy.find (',') != std::string::npos) {
	// replace comma separator with spaces so it's
	std::ranges::replace (copy, ',', ' ');
    }

    // hex colors should be converted to int colors
    if (copy.find ('#') == 0) {
	auto number = copy.substr (1);

	// expand short css notation into the right one
	// support for css notation
	if (number.size () == 3) {
	    number = std::format (
		"{}{}{}{}{}{}{:02x}", number.at (0), number.at (0), number.at (1), number.at (1), number.at (2),
		number.at (2), static_cast<int> (alpha * 255)
	    );
	} else if (number.size () == 4) {
	    number = std::format (
		"{}{}{}{}{}{}{}{}", number.at (0), number.at (0), number.at (1), number.at (1), number.at (2),
		number.at (2), number.at (3), number.at (3)
	    );
	} else if (number.size () != 6 && number.size () != 8) {
	    sLog.exception ("Invalid CSS color notation for ", value);
	}

	// parse hex color
	const auto color = std::stoi (number, nullptr, 16);

	return WallpaperEngine::Data::Model::Color (
	    (color >> 24 & 0xFF) / 255.0f, (color >> 16 & 0xFF) / 255.0f, (color >> 8 & 0xFF) / 255.0f,
	    (color & 0xFF) / 255.0f
	);
    }

    int vectorSize = VectorBuilder::preparseSize (copy);

    if (vectorSize != 3 && vectorSize != 4) {
	throw std::invalid_argument ("Invalid color value");
    }

    if (copy.find ('.') == std::string::npos) {
	const auto final = vectorSize == 3 ? glm::ivec4 (VectorBuilder::parse<glm::ivec3> (copy), alpha * 255)
					   : VectorBuilder::parse<glm::ivec4> (copy);

	return { final.r / 255.0f, final.g / 255.0f, final.b / 255.0f, final.a / 255.0f };
    }

    return Model::Color (
	vectorSize == 3 ? glm::vec4 (VectorBuilder::parse<glm::vec3> (copy), alpha)
			: VectorBuilder::parse<glm::vec4> (copy)
    );
}