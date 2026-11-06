#pragma once

#include "WallpaperEngine/Data/Model/Color.h"

#include <glm/vec4.hpp>
#include <string>

namespace WallpaperEngine::Data::Builders {
class ColorBuilder {
public:
    /**
     * White color constant
     */
    static const Model::Color White;
    /**
     * Black color constant
     */
    static const Model::Color Black;

    static WallpaperEngine::Data::Model::Color parse (const std::string& value, float alpha = 1.0f);
    /**
     * A colour user property's value (project.json `"type": "color"`, or one the host sets): WE's colour picker
     * stores 0..1 floats and writes whole ones without a point, so "1 1 1" is white and "1 0 0" red (117 colour
     * properties in the owner's 96 scenes, none with a component above 1). A component above 1 still reads as 0..255
     */
    static WallpaperEngine::Data::Model::Color parseProperty (const std::string& value);
    /**
     * An object's colour field (an image, text or light layer's `color`): WE's vec3 field parser (GH 0x1401d0bc0)
     * reads up to three space-separated components with atof, missing ones 0, alpha 1 — whole numbers are no
     * different, so "1 1 1" is white (not 1/255) and "255 0 0" is 255 times red. A "#rrggbb" string keeps LWE's hex
     * reading (WE's atof gives 0 there; no scene writes one)
     */
    static WallpaperEngine::Data::Model::Color parseObject (const std::string& value);
};
}