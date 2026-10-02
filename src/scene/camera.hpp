#pragma once
#include "core/types.hpp"

#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

class CameraController {
public:
    void update(GLFWwindow* window, float dt);
    glm::mat4 view() const;
    glm::mat4 proj() const;
    glm::vec3 position() const;

    void setAspect(float a) { aspect_ = a; }
    void frameScene(const AABB& bounds);

    float fovY = glm::radians(50.0f);

private:
    glm::vec3 target_{0.0f, 0.8f, 0.0f};
    float yaw_ = 0.6f, pitch_ = 0.35f, dist_ = 10.0f;
    float aspect_ = 16.0f / 9.0f;
    double lastX_ = 0.0, lastY_ = 0.0;
    bool haveLast_ = false;
};