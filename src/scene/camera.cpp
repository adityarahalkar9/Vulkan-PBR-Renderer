#include "scene/camera.hpp"

#include <glm/gtc/matrix_transform.hpp>

void CameraController::update(GLFWwindow* window, float dt) {
    (void)dt;
    double x = 0, y = 0;
    glfwGetCursorPos(window, &x, &y);
    double dx = haveLast_ ? x - lastX_ : 0.0;
    double dy = haveLast_ ? y - lastY_ : 0.0;
    lastX_ = x;
    lastY_ = y;
    haveLast_ = true;

    const bool left = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool mid = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
    const bool shift = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;

    if(left && !shift) {
        yaw_ -= static_cast<float>(dx) * 0.005f;
        pitch_ = std::clamp(pitch_ + static_cast<float>(dy) * 0.005f, -1.53f, 1.53f);
    }
    else if((mid || (left && shift)) && dist_ > 0.01f) {
        // Pan along camera right/up.
        glm::vec3 dir(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_));
        glm::vec3 right = glm::normalize(glm::cross(dir, glm::vec3(0, 1, 0)));
        glm::vec3 up = glm::normalize(glm::cross(right, dir));
        float k = dist_ * 0.0015f;
        target_ += right * static_cast<float>(-dx) * k + up * static_cast<float>(dy) * k;
    }

    double sx = 0, sy = 0;
    glfwGetScrollOffset(window, &sx, &sy);
    if(sy != 0.0) dist_ = std::clamp(dist_ * static_cast<float>(std::pow(1.12, -sy)), 0.5f, 300.0f);
}

glm::vec3 CameraController::position() const {
    glm::vec3 dir(std::cos(pitch_) * std::sin(yaw_), std::sin(pitch_), std::cos(pitch_) * std::cos(yaw_));
    return target_ + dir * dist_;
}

glm::mat4 CameraController::view() const {
    return glm::lookAt(position(), target_, glm::vec3(0, 1, 0));
}

glm::mat4 CameraController::proj() const {
    glm::mat4 p = glm::perspective(fovY, std::max(aspect_, 0.1f), 0.05f, 1000.0f);
    p[1][1] *= -1.0f; // Vulkan clip-space Y points down
    return p;
}

void CameraController::frameScene(const AABB& bounds) {
    if(!bounds.valid()) return;
    target_ = bounds.center();
    dist_ = std::clamp(bounds.radius() * 2.6f, 1.0f, 250.0f);
}