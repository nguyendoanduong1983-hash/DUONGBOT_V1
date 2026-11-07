# RoboEyes vendor source

The face renderer uses the upstream **FluxGarage RoboEyes for OLED Displays** library, version 1.1.1, by Dennis Hoelscher.

- Upstream project: https://github.com/FluxGarage/RoboEyes
- Source used: `src/FluxGarage_RoboEyes.h`
- License: GPL-3.0-or-later; see `LICENSE.txt`.

The renderer adapter in `main/robot/display/roboeyes_face_renderer.cc` supplies the small monochrome drawing interface needed by this ESP-IDF project and maps the robot's emotion names to RoboEyes moods and animations.
