#pragma once
#include "assets/assettypes.hpp"

#include <string>
#include <vector>

struct PointLight {
    glm::vec3 pos{0.0f};
    glm::vec3 color{1.0f};
    float intensity = 20.0f;
    float radius = 12.0f;
};

class Scene {
public:
    Scene();

    void update(float dt, bool animate);
    uint32_t addEntity(uint32_t model, const std::string& name, const glm::mat4& matrix,
                       int32_t materialOverride, const AABB& modelBounds);
    void clear();

    std::vector<Entity> entities;
    std::vector<PointLight> lights;
    AABB bounds;

private:
    float time_ = 0.0f;
};