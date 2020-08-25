#pragma once

#include <optix.h>

#include "scene/geometry_result.hpp"

namespace moana {

class HibiscusGeometry {
public:
    GeometryResult buildAcceleration(OptixDeviceContext context);
};

}
