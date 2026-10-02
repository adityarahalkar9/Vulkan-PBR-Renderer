#pragma once
#include "core/types.hpp"

#include <glm/glm.hpp>

class Frustum {
public:
    static Frustum fromMatrix(const glm::mat4& m);
    bool testAABB(const AABB& box) const; // conservative positive-vertex test

private:
    glm::vec4 planes_[6];
};