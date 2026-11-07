#pragma once

#include <cstdint>
#include <string>

// Procedural animated face renderer based on the vendored FluxGarage RoboEyes library.
// Render() writes directly into the SSD1306 page-addressed framebuffer.
class RoboEyesFaceRenderer {
public:
    RoboEyesFaceRenderer();
    ~RoboEyesFaceRenderer();

    RoboEyesFaceRenderer(const RoboEyesFaceRenderer&) = delete;
    RoboEyesFaceRenderer& operator=(const RoboEyesFaceRenderer&) = delete;

    bool Initialize(int width, int height);
    void SetEmotion(const std::string& emotion);
    void Render(uint8_t* page_framebuffer, int width, int height);

private:
    struct Impl;
    Impl* impl_ = nullptr;
    std::string emotion_;
};
