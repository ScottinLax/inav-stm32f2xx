/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "platform.h"

#include "build/debug.h"

#include "common/axis.h"
#include "common/filter.h"
#include "common/maths.h"
#include "common/utils.h"

#include "config/feature.h"
#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/pwm_output.h"
#include "drivers/pwm_mapping.h"
#include "drivers/time.h"

#include "fc/config.h"
#include "fc/rc_controls.h"
#include "fc/runtime_config.h"

#include "flight/failsafe.h"
#include "flight/imu.h"
#include "flight/mixer.h"
#include "flight/pid.h"
#include "flight/servos.h"

#include "rx/rx.h"


//#define MIXER_DEBUG

uint8_t motorCount;

int16_t motor[MAX_SUPPORTED_MOTORS];
int16_t motor_disarmed[MAX_SUPPORTED_MOTORS];

bool motorLimitReached = false;

PG_REGISTER_WITH_RESET_TEMPLATE(flight3DConfig_t, flight3DConfig, PG_MOTOR_3D_CONFIG, 0);

PG_RESET_TEMPLATE(flight3DConfig_t, flight3DConfig,
    .deadband3d_low = 1406,
    .deadband3d_high = 1514,
    .neutral3d = 1460
);


PG_REGISTER_WITH_RESET_TEMPLATE(mixerConfig_t, mixerConfig, PG_MIXER_CONFIG, 0);

PG_RESET_TEMPLATE(mixerConfig_t, mixerConfig,
    .mixerMode = MIXER_QUADX,
    .yaw_motor_direction = 1,
    .yaw_jump_prevention_limit = 200
);

#ifdef BRUSHED_MOTORS
#define DEFAULT_PWM_PROTOCOL    PWM_TYPE_BRUSHED
#define DEFAULT_PWM_RATE        16000
#define DEFAULT_MIN_THROTTLE    1000
#else
#define DEFAULT_PWM_PROTOCOL    PWM_TYPE_STANDARD
#define DEFAULT_PWM_RATE        400
#define DEFAULT_MIN_THROTTLE    1150
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(motorConfig_t, motorConfig, PG_MOTOR_CONFIG, 0);

PG_RESET_TEMPLATE(motorConfig_t, motorConfig,
    .minthrottle = DEFAULT_MIN_THROTTLE,
    .motorPwmProtocol = DEFAULT_PWM_PROTOCOL,
    .motorPwmRate = DEFAULT_PWM_RATE,
    .maxthrottle = 1850,
    .mincommand = 1000
);

static motorMixer_t currentMixer[MAX_SUPPORTED_MOTORS];

PG_REGISTER_ARRAY(motorMixer_t, MAX_SUPPORTED_MOTORS, customMotorMixer, PG_MOTOR_MIXER, 0);

static const motorMixer_t mixerQuadX[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },          // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },          // FRONT_L
};
#ifndef USE_QUAD_MIXER_ONLY
static const motorMixer_t mixerTricopter[] = {
    { 1.0f,  0.0f,  1.333333f,  0.0f },     // REAR
    { 1.0f, -1.0f, -0.666667f,  0.0f },     // RIGHT
    { 1.0f,  1.0f, -0.666667f,  0.0f },     // LEFT
};

static const motorMixer_t mixerQuadP[] = {
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR
    { 1.0f, -1.0f,  0.0f,  1.0f },          // RIGHT
    { 1.0f,  1.0f,  0.0f,  1.0f },          // LEFT
    { 1.0f,  0.0f, -1.0f, -1.0f },          // FRONT
};

#ifndef DISABLE_UNCOMMON_MIXERS
static const motorMixer_t mixerVtail4[] = {
    { 1.0f,  -0.58f,  0.58f, 1.0f },        // REAR_R
    { 1.0f,  -0.46f, -0.39f, -0.5f },       // FRONT_R
    { 1.0f,  0.58f,  0.58f, -1.0f },        // REAR_L
    { 1.0f,  0.46f, -0.39f, 0.5f },         // FRONT_L
};

static const motorMixer_t mixerAtail4[] = {
    { 1.0f,  0.0f,  1.0f,  1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  0.0f },          // FRONT_R
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -0.0f },          // FRONT_L
};

static const motorMixer_t mixerY4[] = {
    { 1.0f,  0.0f,  1.0f, -1.0f },          // REAR_TOP CW
    { 1.0f, -1.0f, -1.0f,  0.0f },          // FRONT_R CCW
    { 1.0f,  0.0f,  1.0f,  1.0f },          // REAR_BOTTOM CCW
    { 1.0f,  1.0f, -1.0f,  0.0f },          // FRONT_L CW
};

#if (MAX_SUPPORTED_MOTORS >= 6)
static const motorMixer_t mixerHex6H[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },     // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },     // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },     // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },     // FRONT_L
    { 1.0f,  0.0f,  0.0f,  0.0f },     // RIGHT
    { 1.0f,  0.0f,  0.0f,  0.0f },     // LEFT
};

static const motorMixer_t mixerY6[] = {
    { 1.0f,  0.0f,  1.333333f,  1.0f },     // REAR
    { 1.0f, -1.0f, -0.666667f, -1.0f },     // RIGHT
    { 1.0f,  1.0f, -0.666667f, -1.0f },     // LEFT
    { 1.0f,  0.0f,  1.333333f, -1.0f },     // UNDER_REAR
    { 1.0f, -1.0f, -0.666667f,  1.0f },     // UNDER_RIGHT
    { 1.0f,  1.0f, -0.666667f,  1.0f },     // UNDER_LEFT
};

static const motorMixer_t mixerHex6P[] = {
    { 1.0f, -0.866025f,  0.5f,  1.0f },     // REAR_R
    { 1.0f, -0.866025f, -0.5f, -1.0f },     // FRONT_R
    { 1.0f,  0.866025f,  0.5f,  1.0f },     // REAR_L
    { 1.0f,  0.866025f, -0.5f, -1.0f },     // FRONT_L
    { 1.0f,  0.0f,      -1.0f,  1.0f },     // FRONT
    { 1.0f,  0.0f,       1.0f, -1.0f },     // REAR
};
#endif

#if (MAX_SUPPORTED_MOTORS >= 8)
static const motorMixer_t mixerOctoFlatP[] = {
    { 1.0f,  0.707107f, -0.707107f,  1.0f },    // FRONT_L
    { 1.0f, -0.707107f, -0.707107f,  1.0f },    // FRONT_R
    { 1.0f, -0.707107f,  0.707107f,  1.0f },    // REAR_R
    { 1.0f,  0.707107f,  0.707107f,  1.0f },    // REAR_L
    { 1.0f,  0.0f, -1.0f, -1.0f },              // FRONT
    { 1.0f, -1.0f,  0.0f, -1.0f },              // RIGHT
    { 1.0f,  0.0f,  1.0f, -1.0f },              // REAR
    { 1.0f,  1.0f,  0.0f, -1.0f },              // LEFT
};

static const motorMixer_t mixerOctoFlatX[] = {
    { 1.0f,  1.0f, -0.414178f,  1.0f },      // MIDFRONT_L
    { 1.0f, -0.414178f, -1.0f,  1.0f },      // FRONT_R
    { 1.0f, -1.0f,  0.414178f,  1.0f },      // MIDREAR_R
    { 1.0f,  0.414178f,  1.0f,  1.0f },      // REAR_L
    { 1.0f,  0.414178f, -1.0f, -1.0f },      // FRONT_L
    { 1.0f, -1.0f, -0.414178f, -1.0f },      // MIDFRONT_R
    { 1.0f, -0.414178f,  1.0f, -1.0f },      // REAR_R
    { 1.0f,  1.0f,  0.414178f, -1.0f },      // MIDREAR_L
};

static const motorMixer_t mixerOctoX8[] = {
    { 1.0f, -1.0f,  1.0f, -1.0f },          // REAR_R
    { 1.0f, -1.0f, -1.0f,  1.0f },          // FRONT_R
    { 1.0f,  1.0f,  1.0f,  1.0f },          // REAR_L
    { 1.0f,  1.0f, -1.0f, -1.0f },          // FRONT_L
    { 1.0f, -1.0f,  1.0f,  1.0f },          // UNDER_REAR_R
    { 1.0f, -1.0f, -1.0f, -1.0f },          // UNDER_FRONT_R
    { 1.0f,  1.0f,  1.0f, -1.0f },          // UNDER_REAR_L
    { 1.0f,  1.0f, -1.0f,  1.0f },          // UNDER_FRONT_L
};
#endif
#endif //DISABLE_UNCOMMON_MIXERS

#if (MAX_SUPPORTED_MOTORS >= 6)
static const motorMixer_t mixerHex6X[] = {
    { 1.0f, -0.5f,  0.866025f,  1.0f },     // REAR_R
    { 1.0f, -0.5f, -0.866025f,  1.0f },     // FRONT_R
    { 1.0f,  0.5f,  0.866025f, -1.0f },     // REAR_L
    { 1.0f,  0.5f, -0.866025f, -1.0f },     // FRONT_L
    { 1.0f, -1.0f,  0.0f,      -1.0f },     // RIGHT
    { 1.0f,  1.0f,  0.0f,       1.0f },     // LEFT
};
#endif

static const motorMixer_t mixerDualProp[] = {
    { 1.0f,  0.0f,  0.0f, 0.0f },
    { 1.0f,  0.0f,  0.0f, 0.0f },
};

// Keep synced with mixerMode_e
const mixer_t mixers[] = {
    // motors, use servo, motor mixer
    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=true },                // entry 0
    { .motorCount=3, .useServo=true,  .motor=mixerTricopter, .enabled=true },      // MIXER_TRI
    { .motorCount=4, .useServo=false, .motor=mixerQuadP, .enabled=true },          // MIXER_QUADP
    { .motorCount=4, .useServo=false, .motor=mixerQuadX, .enabled=true },          // MIXER_QUADX

    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },               // MIXER_BICOPTER
    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },               // MIXER_GIMBAL -> this mixer was never implemented in CF, use feature(FEATURE_SERVO_TILT) instead
    #if !defined(DISABLE_UNCOMMON_MIXERS) && (MAX_SUPPORTED_MOTORS >= 6)
        { .motorCount=6, .useServo=false, .motor=mixerY6, .enabled=true },         // MIXER_Y6
        { .motorCount=6, .useServo=false, .motor=mixerHex6P, .enabled=true },      // MIXER_HEX6
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_Y6
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_HEX6
    #endif
    { .motorCount=2, .useServo=true,  .motor=mixerDualProp, .enabled=true },       // MIXER_FLYING_WING
    #if !defined(DISABLE_UNCOMMON_MIXERS)
        { .motorCount=4, .useServo=false, .motor=mixerY4, .enabled=true },         // MIXER_Y4
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_Y4
    #endif
    #if (MAX_SUPPORTED_MOTORS >= 6)
        { .motorCount=6, .useServo=false, .motor=mixerHex6X, .enabled=true },      // MIXER_HEX6X
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_HEX6X
    #endif
    #if !defined(DISABLE_UNCOMMON_MIXERS) && (MAX_SUPPORTED_MOTORS >= 8)
        { .motorCount=8, .useServo=false, .motor=mixerOctoX8, .enabled=true },     // MIXER_OCTOX8
        { .motorCount=8, .useServo=false, .motor=mixerOctoFlatP, .enabled=true },  // MIXER_OCTOFLATP
        { .motorCount=8, .useServo=false, .motor=mixerOctoFlatX, .enabled=true },  // MIXER_OCTOFLATX
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_OCTOX8
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_OCTOFLATP
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_OCTOFLATX
    #endif
    { .motorCount=2, .useServo=true,  .motor=mixerDualProp, .enabled=true },       // * MIXER_AIRPLANE
    { .motorCount=0, .useServo=true,  .motor=NULL, .enabled=false },               // * MIXER_HELI_120_CCPM -> disabled, never fully implemented in CF
    { .motorCount=0, .useServo=true,  .motor=NULL, .enabled=false },               // * MIXER_HELI_90_DEG -> disabled, never fully implemented in CF
    #if !defined(DISABLE_UNCOMMON_MIXERS)
        { .motorCount=4, .useServo=false, .motor=mixerVtail4, .enabled=true },     // MIXER_VTAIL4
    #if (MAX_SUPPORTED_MOTORS >= 6)
        { .motorCount=6, .useServo=false, .motor=mixerHex6H, .enabled=true },      // MIXER_HEX6H
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_HEX6H
    #endif
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_VTAIL4
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_HEX6H
    #endif
    { .motorCount=0, .useServo=true,  .motor=NULL, .enabled=false },               // * MIXER_PPM_TO_SERVO -> looks like this is not implemented at all
    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },               // MIXER_DUALCOPTER
    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },               // MIXER_SINGLECOPTER
    #if !defined(DISABLE_UNCOMMON_MIXERS)
        { .motorCount=4, .useServo=false, .motor=mixerAtail4, .enabled=true },     // MIXER_ATAIL4
    #else
        { .motorCount=0, .useServo=false, .motor=NULL, .enabled=false },           // MIXER_ATAIL4
    #endif
    { .motorCount=0, .useServo=false, .motor=NULL, .enabled=true },                // MIXER_CUSTOM
    { .motorCount=2, .useServo=true,  .motor=NULL, .enabled=true },                // MIXER_CUSTOM_AIRPLANE
    { .motorCount=3, .useServo=true,  .motor=NULL, .enabled=true },                // MIXER_CUSTOM_TRI
};
#endif // USE_QUAD_MIXER_ONLY

bool isMixerEnabled(mixerMode_e mixerMode)
{
#ifdef USE_QUAD_MIXER_ONLY
    UNUSED(mixerMode);
    return true;
#else
    return mixers[mixerMode].enabled;
#endif
}

#ifdef USE_SERVOS
void mixerUpdateStateFlags(void)
{
    const mixerMode_e currentMixerMode = mixerConfig()->mixerMode;

    // set flag that we're on something with wings
    if (currentMixerMode == MIXER_FLYING_WING ||
        currentMixerMode == MIXER_AIRPLANE ||
        currentMixerMode == MIXER_CUSTOM_AIRPLANE
    ) {
        ENABLE_STATE(FIXED_WING);
    } else {
        DISABLE_STATE(FIXED_WING);
    }

    if (currentMixerMode == MIXER_AIRPLANE || currentMixerMode == MIXER_CUSTOM_AIRPLANE) {
        ENABLE_STATE(FLAPERON_AVAILABLE);
    } else {
        DISABLE_STATE(FLAPERON_AVAILABLE);
    }
}

void mixerUsePWMIOConfiguration(void)
{
    motorCount = 0;

    const mixerMode_e currentMixerMode = mixerConfig()->mixerMode;
    if (currentMixerMode == MIXER_CUSTOM || currentMixerMode == MIXER_CUSTOM_TRI || currentMixerMode == MIXER_CUSTOM_AIRPLANE) {
        // load custom mixer into currentMixer
        for (int i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
            // check if done
            if (customMotorMixer(i)->throttle == 0.0f)
                break;
            currentMixer[i] = *customMotorMixer(i);
            motorCount++;
        }
    } else {
        motorCount = MIN(mixers[currentMixerMode].motorCount, pwmGetOutputConfiguration()->motorCount);
        // copy motor-based mixers
        if (mixers[currentMixerMode].motor) {
            for (int i = 0; i < motorCount; i++)
                currentMixer[i] = mixers[currentMixerMode].motor[i];
        }
    }

    // in 3D mode, mixer gain has to be halved
    if (feature(FEATURE_3D)) {
        if (motorCount > 1) {
            for (int i = 0; i < motorCount; i++) {
                currentMixer[i].pitch *= 0.5f;
                currentMixer[i].roll *= 0.5f;
                currentMixer[i].yaw *= 0.5f;
            }
        }
    }

    mixerResetDisarmedMotors();
}
#else
void mixerUsePWMIOConfiguration(void)
{
    motorCount = 4;
    for (int i = 0; i < motorCount; i++) {
        currentMixer[i] = mixerQuadX[i];
    }
    mixerResetDisarmedMotors();
}
#endif


#ifndef USE_QUAD_MIXER_ONLY
void mixerLoadMix(int index, motorMixer_t *customMixers)
{
    // we're 1-based
    index++;
    // clear existing
    for (int i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
        customMixers[i].throttle = 0.0f;
    }

    // do we have anything here to begin with?
    if (mixers[index].motor != NULL) {
        for (int i = 0; i < mixers[index].motorCount; i++) {
            customMixers[i] = mixers[index].motor[i];
        }
    }
}

#endif

void mixerResetDisarmedMotors(void)
{
    // set disarmed motor values
    for (int i = 0; i < MAX_SUPPORTED_MOTORS; i++) {
        motor_disarmed[i] = feature(FEATURE_3D) ? flight3DConfig()->neutral3d : motorConfig()->mincommand;
    }
}

void writeMotors(void)
{
    for (int i = 0; i < motorCount; i++) {
        pwmWriteMotor(i, motor[i]);
    }
}

void writeAllMotors(int16_t mc)
{
    // Sends commands to all motors
    for (int i = 0; i < motorCount; i++) {
        motor[i] = mc;
    }
    writeMotors();
}

void stopMotors(void)
{
    writeAllMotors(feature(FEATURE_3D) ? flight3DConfig()->neutral3d : motorConfig()->mincommand);

    delay(50); // give the timers and ESCs a chance to react.
}

void stopPwmAllMotors()
{
    pwmShutdownPulsesForAllMotors(motorCount);
}

void mixTable(void)
{
    int16_t input[3];   // RPY, range [-500:+500]
    // Allow direct stick input to motors in passthrough mode on airplanes
    if (STATE(FIXED_WING) && FLIGHT_MODE(PASSTHRU_MODE)) {
        // Direct passthru from RX
        input[ROLL] = rcCommand[ROLL];
        input[PITCH] = rcCommand[PITCH];
        input[YAW] = rcCommand[YAW];
    }
    else {
        input[ROLL] = axisPID[ROLL];
        input[PITCH] = axisPID[PITCH];
        input[YAW] = axisPID[YAW];

        if (motorCount >= 4 && mixerConfig()->yaw_jump_prevention_limit < YAW_JUMP_PREVENTION_LIMIT_HIGH) {
            // prevent "yaw jump" during yaw correction
            input[YAW] = constrain(input[YAW], -mixerConfig()->yaw_jump_prevention_limit - ABS(rcCommand[YAW]), mixerConfig()->yaw_jump_prevention_limit + ABS(rcCommand[YAW]));
        }
    }

    // Initial mixer concept by bdoiron74 reused and optimized for Air Mode
    int16_t rpyMix[MAX_SUPPORTED_MOTORS];
    int16_t rpyMixMax = 0; // assumption: symetrical about zero.
    int16_t rpyMixMin = 0;

    // motors for non-servo mixes
    for (int i = 0; i < motorCount; i++) {
        rpyMix[i] =
            input[PITCH] * currentMixer[i].pitch +
            input[ROLL] * currentMixer[i].roll +
            -mixerConfig()->yaw_motor_direction * input[YAW] * currentMixer[i].yaw;

        if (rpyMix[i] > rpyMixMax) rpyMixMax = rpyMix[i];
        if (rpyMix[i] < rpyMixMin) rpyMixMin = rpyMix[i];
    }

    int16_t rpyMixRange = rpyMixMax - rpyMixMin;
    int16_t throttleRange, throttleCommand;
    int16_t throttleMin, throttleMax;
    static int16_t throttlePrevious = 0;   // Store the last throttle direction for deadband transitions

    // Find min and max throttle based on condition.
    if (feature(FEATURE_3D)) {
        if (!ARMING_FLAG(ARMED)) throttlePrevious = rxConfig()->midrc; // When disarmed set to mid_rc. It always results in positive direction after arming.

        if ((rcCommand[THROTTLE] <= (rxConfig()->midrc - rcControlsConfig()->deadband3d_throttle))) { // Out of band handling
            throttleMax = flight3DConfig()->deadband3d_low;
            throttleMin = motorConfig()->minthrottle;
            throttlePrevious = throttleCommand = rcCommand[THROTTLE];
        } else if (rcCommand[THROTTLE] >= (rxConfig()->midrc + rcControlsConfig()->deadband3d_throttle)) { // Positive handling
            throttleMax = motorConfig()->maxthrottle;
            throttleMin = flight3DConfig()->deadband3d_high;
            throttlePrevious = throttleCommand = rcCommand[THROTTLE];
        } else if ((throttlePrevious <= (rxConfig()->midrc - rcControlsConfig()->deadband3d_throttle)))  { // Deadband handling from negative to positive
            throttleCommand = throttleMax = flight3DConfig()->deadband3d_low;
            throttleMin = motorConfig()->minthrottle;
        } else {  // Deadband handling from positive to negative
            throttleMax = motorConfig()->maxthrottle;
            throttleCommand = throttleMin = flight3DConfig()->deadband3d_high;
        }
    } else {
        throttleCommand = rcCommand[THROTTLE];
        throttleMin = motorConfig()->minthrottle;
        throttleMax = motorConfig()->maxthrottle;
    }

    throttleRange = throttleMax - throttleMin;

    #define THROTTLE_CLIPPING_FACTOR    0.33f
    if (rpyMixRange > throttleRange) {
        motorLimitReached = true;
        float mixReduction = (float)throttleRange / rpyMixRange;

        for (int i = 0; i < motorCount; i++) {
            rpyMix[i] =  mixReduction  * rpyMix[i];
        }

        // Allow some clipping on edges to soften correction response
        throttleMin = throttleMin + (throttleRange / 2) - (throttleRange * THROTTLE_CLIPPING_FACTOR / 2);
        throttleMax = throttleMin + (throttleRange / 2) + (throttleRange * THROTTLE_CLIPPING_FACTOR / 2);
    } else {
        motorLimitReached = false;
        throttleMin = MIN(throttleMin + (rpyMixRange / 2), throttleMin + (throttleRange / 2) - (throttleRange * THROTTLE_CLIPPING_FACTOR / 2));
        throttleMax = MAX(throttleMax - (rpyMixRange / 2), throttleMin + (throttleRange / 2) + (throttleRange * THROTTLE_CLIPPING_FACTOR / 2));
    }

    // Now add in the desired throttle, but keep in a range that doesn't clip adjusted
    // roll/pitch/yaw. This could move throttle down, but also up for those low throttle flips.
    if (ARMING_FLAG(ARMED)) {
        for (int i = 0; i < motorCount; i++) {
            motor[i] = rpyMix[i] + constrain(throttleCommand * currentMixer[i].throttle, throttleMin, throttleMax);

            if (failsafeIsActive()) {
                motor[i] = constrain(motor[i], motorConfig()->mincommand, motorConfig()->maxthrottle);
            } else if (feature(FEATURE_3D)) {
                if (throttlePrevious <= (rxConfig()->midrc - rcControlsConfig()->deadband3d_throttle)) {
                    motor[i] = constrain(motor[i], motorConfig()->minthrottle, flight3DConfig()->deadband3d_low);
                } else {
                    motor[i] = constrain(motor[i], flight3DConfig()->deadband3d_high, motorConfig()->maxthrottle);
                }
            } else {
                motor[i] = constrain(motor[i], motorConfig()->minthrottle, motorConfig()->maxthrottle);
            }

            // Motor stop handling
            if (feature(FEATURE_MOTOR_STOP) && ARMING_FLAG(ARMED)) {
                bool failsafeMotorStop = failsafeRequiresMotorStop();
                bool navMotorStop = !failsafeIsActive() && STATE(NAV_MOTOR_STOP_OR_IDLE);
                bool userMotorStop = !failsafeIsActive() && (rcData[THROTTLE] < rxConfig()->mincheck);
                if (failsafeMotorStop || navMotorStop || userMotorStop) {
                    if (feature(FEATURE_3D)) {
                        motor[i] = rxConfig()->midrc;
                    }
                    else {
                        motor[i] = motorConfig()->mincommand;
                    }
                }
            }
        }
    } else {
        for (int i = 0; i < motorCount; i++) {
            motor[i] = motor_disarmed[i];
        }
    }
}
