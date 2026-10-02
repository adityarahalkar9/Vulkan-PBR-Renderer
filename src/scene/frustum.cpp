#include "scene/frustum.hpp"

Frustum Frustum::fromMatrix(const glm::mat4& m) {
    // Gribb-Hartmann extraction, row form for column-major glm.
    auto row = [&m](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    glm::vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

    Frustum f;
    f.planes_[0] = r3 + r0; // left
    f.planes_[1] = r3 - r0; // right
    f.planes_[2] = r3 + r1; // bottom
    f.planes_[3] = r3 - r1; // top
    f.planes_[4] = r3 + r2; // near
    f.planes_[5] = r3 - r2; // far
    for(auto& p : f.planes_)
        p /= std::max(glm::length(glm::vec3(p)), 1e-8f);
    return f;
}

bool Frustum::testAABB(const AABB& box) const {
    for(const auto& p : planes_) {
        glm::vec3 pv(p.x > 0 ? box.max.x : box.min.x,
                     p.y > 0 ? box.max.y : box.min.y,
                     p.z > 0 ? box.max.z : box.min.z);
        if(glm::dot(pv, glm::vec3(p)) + p.w < 0.0f)
            return false; // the AABB's most positive vertex is outside this plane
    }
    return true;
}