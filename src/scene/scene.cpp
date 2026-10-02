#include "scene/scene.hpp"

Scene::Scene() {
    lights.push_back({{6, 2.5f, 2}, {1.0f, 0.25f, 0.2f}, 22.0f, 14.0f});
    lights.push_back({{-5, 3.0f, -4}, {0.2f, 1.0f, 0.35f}, 18.0f, 14.0f});
    lights.push_back({{0, 2.0f, 6}, {0.3f, 0.45f, 1.0f}, 16.0f, 12.0f});
}

void Scene::update(float dt, bool animate) {
    if(!animate) return;
    time_ += dt;
    for(size_t i = 0; i < lights.size(); ++i) {
        float a = time_ * (0.6f + 0.13f * static_cast<float>(i)) + static_cast<float>(i) * 2.1f;
        float r = 5.5f + 1.5f * static_cast<float>(i);
        lights[i].pos = {std::cos(a) * r, 2.2f + std::sin(time_ * 0.7f + static_cast<float>(i)) * 1.2f,
                         std::sin(a) * r};
    }
}

uint32_t Scene::addEntity(uint32_t model, const std::string& name, const glm::mat4& matrix,
                          int32_t materialOverride, const AABB& modelBounds) {
    Entity e;
    e.model = model;
    e.materialOverride = materialOverride;
    e.modelMatrix = matrix;
    e.name = name;
    e.worldAABB = modelBounds.transformed(matrix);
    bounds.expand(e.worldAABB);
    entities.push_back(std::move(e));
    return static_cast<uint32_t>(entities.size() - 1);
}

void Scene::clear() {
    entities.clear();
    bounds = {};
}