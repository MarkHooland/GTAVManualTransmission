#pragma once

#define DISPLAY_VERSION "v4.6.1"
#define NUM_GEARS 8
#define MAX_GEAR 7

#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif

#include <string>
#include <vector>

#include <inc/types.h>

const std::string mtDir = "\\ManualTransmission";

class VehicleData;
struct Color;
class ScriptSettings;

//http://pastebin.com/Kj9t38KF
enum eRadioStations {
    LosSantosRockRadio,
    NonStopPopFM,
    RadioLosSantos,
    ChannelX,
    WestCoastTalkRadio,
    RebelRadio,
    SoulwaxFM,
    EastLosFM,
    WestCoastClassics,
    BlueArk,
    WorldideFM,
    FlyLoFM,
    TheLowdown,
    TheLab,
    RadioMirrorPark,
    Space1032,
    VinewoodBlvdRadio,
    SelfRadio,
    BlaineCountyRadio,
    RadioOff = 255
};

enum eDecorType {
    DECOR_TYPE_FLOAT = 1,
    DECOR_TYPE_BOOL,
    DECOR_TYPE_INT,
    DECOR_TYPE_UNK,
    DECOR_TYPE_TIME
};

void update_npc();

///////////////////////////////////////////////////////////////////////////////
//                           Helper functions/tools
///////////////////////////////////////////////////////////////////////////////

void drawHUD();
void drawDebugInfo();
void crossScript();
void drawVehicleWheelInfo();
void drawInputWheelInfo();

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Mod control
///////////////////////////////////////////////////////////////////////////////

void clearVehicleData();
void resetSteeringMultiplier();
void toggleManual();
void initWheel();
void stopForceFeedback();
void updateLastInputDevice();

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Shifting
///////////////////////////////////////////////////////////////////////////////

void setShiftMode(int shiftMode);
void cycleShiftMode();
void shiftTo(int gear, bool autoClutch);
void functionHShiftTo(int i);
void functionHShiftKeyboard();
void functionHShiftWheel();
void functionSShift();
void functionAShift();

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Gearbox features
///////////////////////////////////////////////////////////////////////////////

void functionClutchCatch();
void functionEngStall();
void functionEngDamage();
void functionEngBrake();
void functionEngLock();
//void manageBrakePatch();

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Gearbox control
///////////////////////////////////////////////////////////////////////////////

void fakeRev(bool customThrottle = false, float customThrottleVal = 0.0f);
void handleRPM();
//void functionTruckLimiting();
void functionLimiter();

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Reverse/Pedal handling
///////////////////////////////////////////////////////////////////////////////

void functionAutoReverse();
void functionRealReverse();
void handlePedalsDefault(float logiThrottleVal, float logiBrakeVal);
void handlePedalsRealReverse(float logiThrottleVal, float logiBrakeVal);

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Buttons
///////////////////////////////////////////////////////////////////////////////

void checkWheelButtons();

///////////////////////////////////////////////////////////////////////////////
//                    Wheel input and force feedback
///////////////////////////////////////////////////////////////////////////////

void playFFBGround();
void playFFBWater();
void doWheelSteering();

///////////////////////////////////////////////////////////////////////////////
//                             Misc features
///////////////////////////////////////////////////////////////////////////////

void functionAutoLookback();
void functionAutoGear1();
void functionHillGravity();
void functionHidePlayerInFPV();

///////////////////////////////////////////////////////////////////////////////
//                              Script entry
///////////////////////////////////////////////////////////////////////////////
void readSettings();
void ScriptMain();
void NPCMain();


///////////////////////////////////////////////////////////////////////////////
//                              Menu-related
///////////////////////////////////////////////////////////////////////////////
void update_menu();
void clearAxis(std::string axis);
void clearButton(std::string button);
void clearWheelToKey();
void clearHShifter();
void clearKeyboardKey(std::string button);
void clearControllerButton(std::string button);
void clearLControllerButton(std::string button);

bool configAxis(std::string confTag);
bool configWheelToKey();
bool configButton(std::string confTag);
bool configHPattern();
bool configKeyboardKey(const std::string &confTag);
bool configControllerButton(const std::string &confTag);
bool configLControllerButton(const std::string &confTag);

void updateSteeringMultiplier();

void onMenuInit();
void onMenuClose();

///////////////////////////////////////////////////////////////////////////////
//                        Script-specific utils
///////////////////////////////////////////////////////////////////////////////
std::vector<float> getDrivenWheelsSpeeds(std::vector<float> wheelSpeeds);


void     blockButtons();
void startStopEngine();

void drawSteeringLines(float steeringAngle);
float getSteeringAngle(Vehicle v);
std::vector<bool> getWheelLockups(Vehicle handle);
