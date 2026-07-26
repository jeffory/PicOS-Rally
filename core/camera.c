#include "camera.h"

void camera_init(camera_t *cam, const car_t *car) {
    cam->x = car->x; cam->y = car->y;
    cam->shake_x = 0.0f; cam->shake_y = 0.0f;
}

void camera_update(camera_t *cam, const car_t *car, float dt) {
    (void)dt;
    // Velocity look-ahead in the direction of travel.
    float sh = mx_sin(car->heading), ch = mx_cos(car->heading);
    float wvx = sh * car->vx + ch * car->vy;
    float wvy = ch * car->vx - sh * car->vy;
    float lead_x = wvx * 0.35f, lead_y = wvy * 0.35f;
    // Clamp lead magnitude to 14 m (56 px at 4 px/m).
    float m2 = lead_x * lead_x + lead_y * lead_y;
    if (m2 > 14.0f * 14.0f) {
        float inv = 14.0f;
        // cheap rsqrt via one Newton step is overkill; m2 changes slowly
        float mag = 14.0f;
        // normalize: mag = sqrt(m2)
        float guess = m2 * 0.5f;
        for (int i = 0; i < 4; i++) guess = 0.5f * (guess + m2 / guess);
        inv = 14.0f / guess;
        lead_x *= inv; lead_y *= inv;
        mag = guess; (void)mag;
    }
    cam->x = car->x + lead_x + cam->shake_x;
    cam->y = car->y + lead_y + cam->shake_y;
    // Decay shake.
    cam->shake_x *= 0.90f; cam->shake_y *= 0.90f;
    cam->shake_x = mx_clamp(cam->shake_x, -0.75f, 0.75f);
    cam->shake_y = mx_clamp(cam->shake_y, -0.75f, 0.75f);
}

void camera_kick(camera_t *cam, float sx, float sy) {
    cam->shake_x = mx_clamp(cam->shake_x + sx, -0.75f, 0.75f);
    cam->shake_y = mx_clamp(cam->shake_y + sy, -0.75f, 0.75f);
}
