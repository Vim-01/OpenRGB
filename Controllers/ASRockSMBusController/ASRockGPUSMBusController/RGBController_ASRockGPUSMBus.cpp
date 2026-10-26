/*---------------------------------------------------------*\
| RGBController_ASRockGPUSMBus.cpp                          |
|                                                           |
|   RGBController for ASRock GPU RGB controllers            |
|                                                           |
|   Based on SignalRGB ASRock GPU.js plugin                 |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "RGBController_ASRockGPUSMBus.h"
#include <chrono>
#include <thread>

using namespace std::chrono_literals;

/**------------------------------------------------------------------*\
    @name ASRock GPU SMBus
    @category GPU
    @type I2C
    @save :white_check_mark:
    @direct :white_check_mark:
    @effects :white_check_mark:
    @detectors DetectASRockGPUSMBusControllers
    @comment
\*-------------------------------------------------------------------*/

typedef struct
{
    const char*     name;
    unsigned char   value;
    unsigned int    flags;
    unsigned int    color_mode;
    unsigned int    num_colors;
} asrock_gpu_mode_def;

static const asrock_gpu_mode_def asrock_gpu_mode_list[] =
{
    { "Off",            ASROCK_GPU_MODE_OFF,            MODE_FLAG_MANUAL_SAVE,                                                                               MODE_COLORS_NONE,          0 },
    { "Static",         ASROCK_GPU_MODE_STATIC,         MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE,                     MODE_COLORS_PER_LED,       0 },
    { "Breathing",      ASROCK_GPU_MODE_BREATHING,      MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_MANUAL_SAVE, MODE_COLORS_MODE_SPECIFIC, 1 },
    { "Strobe",         ASROCK_GPU_MODE_STROBE,         MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_MANUAL_SAVE, MODE_COLORS_MODE_SPECIFIC, 1 },
    { "Spectrum Cycle", ASROCK_GPU_MODE_SPECTRUM_CYCLE, MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE,                             MODE_COLORS_NONE,          0 },
    { "Random",         ASROCK_GPU_MODE_RANDOM,         MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE,                             MODE_COLORS_NONE,          0 },
    { "Stack",          ASROCK_GPU_MODE_STACK,          MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_MANUAL_SAVE, MODE_COLORS_MODE_SPECIFIC, 1 },
    { "Scan",           ASROCK_GPU_MODE_SCAN,           MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_HAS_MODE_SPECIFIC_COLOR | MODE_FLAG_MANUAL_SAVE, MODE_COLORS_MODE_SPECIFIC, 1 },
    { "Rainbow Wave",   ASROCK_GPU_MODE_RAINBOW_WAVE,   MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE,                             MODE_COLORS_NONE,          0 },
    { "Rainbow",        ASROCK_GPU_MODE_RAINBOW,        MODE_FLAG_HAS_SPEED | MODE_FLAG_HAS_BRIGHTNESS | MODE_FLAG_MANUAL_SAVE,                             MODE_COLORS_NONE,          0 },
};

#define ASROCK_GPU_NUM_MODES (sizeof(asrock_gpu_mode_list) / sizeof(asrock_gpu_mode_list[0]))

RGBController_ASRockGPUSMBus::RGBController_ASRockGPUSMBus(ASRockGPUSMBusController* controller_ptr)
{
    controller  = controller_ptr;

    name        = controller->GetDeviceName();
    vendor      = "ASRock";
    type        = DEVICE_TYPE_GPU;
    description = "ASRock GPU RGB Controller";
    location    = controller->GetDeviceLocation();

    SetupZones();
    SetupModes();
}

RGBController_ASRockGPUSMBus::~RGBController_ASRockGPUSMBus()
{
    Shutdown();

    delete controller;
}

void RGBController_ASRockGPUSMBus::SetupModes()
{
    /*-----------------------------------------------------*\
    | Set up device and per-zone modes                      |
    \*-----------------------------------------------------*/
    for(unsigned int mode_idx = 0; mode_idx < ASROCK_GPU_NUM_MODES; mode_idx++)
    {
        mode new_mode;
        new_mode.name           = asrock_gpu_mode_list[mode_idx].name;
        new_mode.value          = asrock_gpu_mode_list[mode_idx].value;
        new_mode.flags          = asrock_gpu_mode_list[mode_idx].flags;
        new_mode.color_mode     = asrock_gpu_mode_list[mode_idx].color_mode;
        new_mode.speed_min      = ASROCK_GPU_SPEED_MIN;
        new_mode.speed_max      = ASROCK_GPU_SPEED_MAX;
        new_mode.speed          = ASROCK_GPU_SPEED_DEFAULT;
        new_mode.brightness_min = ASROCK_GPU_BRIGHTNESS_MIN;
        new_mode.brightness_max = ASROCK_GPU_BRIGHTNESS_MAX;
        new_mode.brightness     = ASROCK_GPU_BRIGHTNESS_DEFAULT;

        if(asrock_gpu_mode_list[mode_idx].num_colors > 0)
        {
            new_mode.colors_min = asrock_gpu_mode_list[mode_idx].num_colors;
            new_mode.colors_max = asrock_gpu_mode_list[mode_idx].num_colors;
            new_mode.colors.resize(asrock_gpu_mode_list[mode_idx].num_colors);
            new_mode.colors[0]  = ToRGBColor(255, 255, 255);
        }

        modes.push_back(new_mode);

        for(unsigned int zone_idx = 0; zone_idx < zones.size(); zone_idx++)
        {
            zones[zone_idx].modes.push_back(new_mode);
        }
    }

    /*-----------------------------------------------------*\
    | Direct mode applies to entire device for streaming    |
    \*-----------------------------------------------------*/
    mode Direct;
    Direct.name                 = "Direct";
    Direct.value                = ASROCK_GPU_MODE_DIRECT;
    Direct.flags                = MODE_FLAG_HAS_PER_LED_COLOR | MODE_FLAG_REQUIRES_ENTIRE_DEVICE;
    Direct.color_mode           = MODE_COLORS_PER_LED;
    modes.push_back(Direct);

    active_mode = 1;
}

void RGBController_ASRockGPUSMBus::SetupZones()
{
    /*-----------------------------------------------------*\
    | Set up zones - one zone per channel                   |
    \*-----------------------------------------------------*/
    for(unsigned int i = 0; i < controller->num_channels; i++)
    {
        uint8_t channel_idx = controller->enabled_channels[i];

        zone new_zone;
        new_zone.type       = ZONE_TYPE_SINGLE;
        new_zone.leds_min   = 1;
        new_zone.leds_max   = 1;
        new_zone.leds_count = 1;

        /*-------------------------------------------------*\
        | Channel names from SignalRGB                      |
        \*-------------------------------------------------*/
        if(channel_idx == ASROCK_GPU_CHANNEL_ARGB_HEADER)
        {
            new_zone.name = "ARGB Header";
        }
        else if(channel_idx == ASROCK_GPU_CHANNEL_TOP_SIDE)
        {
            new_zone.name = "Top Side";
        }
        else if(channel_idx == ASROCK_GPU_CHANNEL_FAN)
        {
            new_zone.name = "Fan";
        }
        else if(channel_idx == ASROCK_GPU_CHANNEL_BOTTOM_SHROUD)
        {
            new_zone.name = "Bottom Shroud";
        }
        else
        {
            new_zone.name = "Channel " + std::to_string(channel_idx);
        }

        zones.push_back(new_zone);

        /*-------------------------------------------------*\
        | Set up LED for this zone                          |
        \*-------------------------------------------------*/
        led new_led;
        new_led.name = new_zone.name;
        leds.push_back(new_led);
    }

    SetupColors();
}

void RGBController_ASRockGPUSMBus::ResizeZone(int /*zone*/, int /*new_size*/)
{
    /*-----------------------------------------------------*\
    | This device does not support resizing zones           |
    \*-----------------------------------------------------*/
}

void RGBController_ASRockGPUSMBus::DeviceUpdateLEDs()
{
    if(modes[active_mode].value == ASROCK_GPU_MODE_DIRECT)
    {
        /*-----------------------------------------------------*\
        | Send single atomic broadcast frame for the GPU        |
        \*-----------------------------------------------------*/
        if(!colors.empty())
        {
            RGBColor color = colors[0];
            controller->SetDirectColor(RGBGetRValue(color),
                                       RGBGetGValue(color),
                                       RGBGetBValue(color));
        }
    }
    else
    {
        DeviceUpdateMode();
    }
}

void RGBController_ASRockGPUSMBus::DeviceUpdateZoneLEDs(int zone)
{
    if(zone < 0 || (unsigned int)zone >= controller->num_channels || (unsigned int)zone >= zones.size())
    {
        return;
    }

    if(modes[active_mode].value == ASROCK_GPU_MODE_DIRECT)
    {
        RGBColor color = colors[zone];

        controller->SetDirectColor(RGBGetRValue(color),
                                   RGBGetGValue(color),
                                   RGBGetBValue(color));
    }
    else
    {
        DeviceUpdateZoneMode(zone);
    }
}

void RGBController_ASRockGPUSMBus::DeviceUpdateSingleLED(int led)
{
    DeviceUpdateZoneLEDs(led);
}

void RGBController_ASRockGPUSMBus::DeviceUpdateMode()
{
    for(unsigned int zone_idx = 0; zone_idx < zones.size(); zone_idx++)
    {
        DeviceUpdateZoneMode(zone_idx);
    }
}

void RGBController_ASRockGPUSMBus::DeviceUpdateZoneMode(int zone)
{
    if(zone < 0 || (unsigned int)zone >= controller->num_channels || (unsigned int)zone >= zones.size())
    {
        return;
    }

    /*-----------------------------------------------------*\
    | Use the zone's active mode if set, else device mode   |
    \*-----------------------------------------------------*/
    const mode* zone_mode = &modes[active_mode];

    if((zones[zone].active_mode >= 0) && ((unsigned int)zones[zone].active_mode < zones[zone].modes.size()))
    {
        zone_mode = &zones[zone].modes[zones[zone].active_mode];
    }

    RGBColor color = 0;

    if(zone_mode->color_mode == MODE_COLORS_PER_LED)
    {
        color = (zone < (int)colors.size()) ? colors[zone] : 0;
    }
    else if(zone_mode->color_mode == MODE_COLORS_MODE_SPECIFIC)
    {
        if(!zone_mode->colors.empty())
        {
            color = zone_mode->colors[0];
        }
    }

    uint8_t speed = (zone_mode->flags & MODE_FLAG_HAS_SPEED)
                  ? (uint8_t)zone_mode->speed
                  : ASROCK_GPU_SPEED_DEFAULT;

    uint8_t brightness = (zone_mode->flags & MODE_FLAG_HAS_BRIGHTNESS)
                       ? (uint8_t)zone_mode->brightness
                       : ASROCK_GPU_BRIGHTNESS_DEFAULT;

    uint8_t channel_idx = controller->enabled_channels[zone];

    controller->SetChannelMode(channel_idx,
                               (uint8_t)zone_mode->value,
                               speed,
                               brightness,
                               RGBGetRValue(color),
                               RGBGetGValue(color),
                               RGBGetBValue(color));
}

void RGBController_ASRockGPUSMBus::DeviceSaveMode()
{
    DeviceUpdateMode();
    controller->SaveConfig();
    std::this_thread::sleep_for(20ms);
}
