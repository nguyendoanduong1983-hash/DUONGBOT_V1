#ifndef XIAOZHI_ROBOT_HARDWARE_CONFIG_H_
#define XIAOZHI_ROBOT_HARDWARE_CONFIG_H_

#include <driver/gpio.h>
#include <driver/i2c_types.h>

#define AUDIO_INPUT_SAMPLE_RATE 16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000
// INMP441 microphone.
#define AUDIO_I2S_MIC_GPIO_WS GPIO_NUM_1
#define AUDIO_I2S_MIC_GPIO_SCK GPIO_NUM_2
#define AUDIO_I2S_MIC_GPIO_DIN GPIO_NUM_3

// MAX98357A/MAX98567A-compatible I2S amplifier wiring.
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_40
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_39
#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_38

// TTP223 with jumper A: idle high (LED on), touched low, connected to GPIO0.
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define BUTTON_ACTIVE_HIGH false

// Small Edison/status LED.
#define BUILTIN_LED_GPIO GPIO_NUM_19
#define BUILTIN_LED_OUTPUT_INVERT false
#define BUILTIN_LED_LEDC_TIMER LEDC_TIMER_2
#define BUILTIN_LED_LEDC_CHANNEL LEDC_CHANNEL_2
#define BUILTIN_LED_STATUS_PROFILE_EDISON true

// L298N: motor A is right (IN1/IN2); motor B is left (IN3/IN4).
#define MOTOR_LEFT_IN1 GPIO_NUM_45   // IN3
#define MOTOR_LEFT_IN2 GPIO_NUM_47   // IN4
#define MOTOR_RIGHT_IN1 GPIO_NUM_21  // IN1
#define MOTOR_RIGHT_IN2 GPIO_NUM_14  // IN2
// The left motor is mounted with opposite electrical polarity to the right motor.
#define MOTOR_LEFT_DIRECTION_INVERTED true

// ESP32-S3-CAM DVP connector. This matches bread-compact-wifi-s3cam.
#define CAMERA_PIN_D0 GPIO_NUM_11
#define CAMERA_PIN_D1 GPIO_NUM_9
#define CAMERA_PIN_D2 GPIO_NUM_8
#define CAMERA_PIN_D3 GPIO_NUM_10
#define CAMERA_PIN_D4 GPIO_NUM_12
#define CAMERA_PIN_D5 GPIO_NUM_18
#define CAMERA_PIN_D6 GPIO_NUM_17
#define CAMERA_PIN_D7 GPIO_NUM_16
#define CAMERA_PIN_XCLK GPIO_NUM_15
#define CAMERA_PIN_PCLK GPIO_NUM_13
#define CAMERA_PIN_VSYNC GPIO_NUM_6
#define CAMERA_PIN_HREF GPIO_NUM_7
#define CAMERA_PIN_SIOC GPIO_NUM_5
#define CAMERA_PIN_SIOD GPIO_NUM_4
#define CAMERA_PIN_PWDN GPIO_NUM_NC
#define CAMERA_PIN_RESET GPIO_NUM_NC
#define CAMERA_XCLK_FREQ_HZ 20000000

// Primary shared I2C bus: camera SCCB + environment sensors.
#define PRIMARY_I2C_PORT I2C_NUM_1
#define PRIMARY_I2C_SDA_PIN CAMERA_PIN_SIOD
#define PRIMARY_I2C_SCL_PIN CAMERA_PIN_SIOC
#define DISTANCE_SENSOR_I2C_ADDRESS 0x29
#define AHT20_I2C_ADDRESS 0x38
#define BMP280_I2C_ADDRESS_PRIMARY 0x76
#define BMP280_I2C_ADDRESS_FALLBACK 0x77
#define BH1750_I2C_ADDRESS_PRIMARY 0x23
#define BH1750_I2C_ADDRESS_FALLBACK 0x5C

// Shared auxiliary I2C bus: SSD1306 + INA219 + MPU6050 + downward-facing VL53L0X.
#define AUXILIARY_I2C_SDA_PIN GPIO_NUM_41
#define AUXILIARY_I2C_SCL_PIN GPIO_NUM_42
#define AUXILIARY_I2C_PORT I2C_NUM_0
#define DISTANCE_SENSOR_I2C_PORT AUXILIARY_I2C_PORT
#define DISTANCE_SENSOR_I2C_SDA_PIN AUXILIARY_I2C_SDA_PIN
#define DISTANCE_SENSOR_I2C_SCL_PIN AUXILIARY_I2C_SCL_PIN
#define SECONDARY_OLED_SDA_PIN AUXILIARY_I2C_SDA_PIN
#define SECONDARY_OLED_SCL_PIN AUXILIARY_I2C_SCL_PIN
#define SECONDARY_OLED_I2C_ADDRESS 0x3C
#define SECONDARY_OLED_WIDTH 128
#define SECONDARY_OLED_HEIGHT 64
#define SECONDARY_OLED_FLIP_180 true

// INA219 breakout with both A0/A1 jumpers open and an R005 (0.005 ohm) shunt.
#define INA219_I2C_ADDRESS 0x40
#define INA219_SHUNT_RESISTANCE_OHMS 0.005f

// Optional MPU6050 on the shared auxiliary bus. It follows the same probe-first lifecycle as the
// INA219; the sampling task starts only when at least one auxiliary sensor is detected.
#define MPU6050_I2C_ADDRESS 0x68
// Gyro Z is the yaw axis for the current flat, under-chassis MPU6050 mounting.
// Positive corrected yaw must correspond to a right turn; flip this sign after
// the first hardware direction check if the installed module is mirrored.
#define MPU6050_YAW_AXIS 2
#define MPU6050_YAW_SIGN 1.0f

#endif  // XIAOZHI_ROBOT_HARDWARE_CONFIG_H_
