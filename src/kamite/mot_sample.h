#pragma once

#include <cstddef>
#include <cstdint>

// The game's MOT motions ("mtb3", tools/models/mot.py) sampled on the host: one channel at a frame, from the bytes of
// a motion wherever they are (guest RAM, a file read off the disc). Plus the rotations the motions use (Euler
// R = Rz Ry Rx, radians, as a part's +0x100) and their quaternions. Shared by style_pose (the arm swing) and Strafing
// Tiger's side motions.
namespace kamite::mot_sample
{
    // The value of `component` (0..2 translation x / y / z, 3..5 rotation x / y / z) of `bone` (255 = the root) at
    // frame t of the motion at mot[0..size): false when the motion doesn't key it or keys it in an encoding not known
    // here (0x00 constant, 0x10 Hermite are; the last record wins, as the game's table does). Before the first key:
    // its value; past the last: the last.
    bool sample(const uint8_t *mot, size_t size, uint32_t bone, uint32_t component, float t, float &out);
    // "mtb3" and its header inside size.
    bool valid(const uint8_t *mot, size_t size);
    uint32_t frames(const uint8_t *mot); // the u16 frame count (the loop is frames - 1 long)

    struct Quat
    {
        float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    };
    void eulerMatrix(const float e[3], float r[3][3]); // Rz Ry Rx
    Quat matrixQuat(const float r[3][3]);
    void quatMatrix(const Quat &q, float r[3][3]);
    Quat eulerQuat(const float e[3]);
    Quat slerp(const Quat &a, Quat b, float u);
    // The Euler angles of r nearest `near` (each wound within half a turn of it); false if none rebuilds r.
    bool eulerNear(const float r[3][3], const float near[3], float out[3]);
}
