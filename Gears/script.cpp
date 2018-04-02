#define NOMINMAX

#include <string>
#include <iomanip>
#include <algorithm>

#include "../../ScriptHookV_SDK/inc/natives.h"
#include "../../ScriptHookV_SDK/inc/enums.h"
#include "../../ScriptHookV_SDK/inc/main.h"
#include "../../ScriptHookV_SDK/inc/types.h"

#include "script.h"

#include "ScriptSettings.hpp"
#include "VehicleData.hpp"

#include "Input/CarControls.hpp"
#include "Memory/MemoryPatcher.hpp"
#include "Util/Logger.hpp"
#include "Util/Util.hpp"
#include "Util/Paths.h"

#include <menu.h>
#include "Memory/NativeMemory.hpp"
#include "Util/MathExt.h"
#include "ShiftModes.h"
#include "Memory/Offsets.hpp"
#include "MiniPID/MiniPID.h"
#include "Memory/VehicleFlags.h"

const std::string mtPrefix = "~b~Manual Transmission~w~~n~";

const char* decorCurrentGear = "mt_gear";
const char* decorShiftNotice = "mt_shift_indicator";
const char* decorFakeNeutral = "mt_neutral";
const char* decorSetShiftMode = "mt_set_shiftmode";
const char* decorGetShiftMode = "mt_get_shiftmode";

// Camera mods interoperability (via decorators)
const char* decorLookingLeft = "mt_looking_left";
const char* decorLookingRight = "mt_looking_right";
const char* decorLookingBack = "mt_looking_back";

const float g_baseStallSpeed = 0.08f;
const float g_baseCatchMinSpeed = 0.12f;
const float g_baseCatchMaxSpeed = 0.24f;

std::string textureWheelFile;
int textureWheelId;

GameSound gearRattle("DAMAGED_TRUCK_IDLE", nullptr);
int soundID;

std::string absoluteModPath;
std::string settingsGeneralFile;
std::string settingsWheelFile;
std::string settingsStickFile;
std::string settingsMenuFile;

NativeMenu::Menu menu;
CarControls carControls;
ScriptSettings settings;

Player player;
Ped playerPed;
Vehicle vehicle;
Vehicle lastVehicle;
VehicleData vehData;
VehicleExtensions ext;

int prevNotification = 0;
int prevExtShift = 0;

int speedoIndex;
extern std::vector<std::string> speedoTypes;

MiniPID pid(1.0, 0.0, 0.0);

bool g_CustomABS;

bool isPlayerAvailable() {
    if (!PLAYER::IS_PLAYER_CONTROL_ON(player) || 
        PLAYER::IS_PLAYER_BEING_ARRESTED(player, TRUE) || 
        CUTSCENE::IS_CUTSCENE_PLAYING() ||
        !ENTITY::DOES_ENTITY_EXIST(playerPed) || 
        ENTITY::IS_ENTITY_DEAD(playerPed)) {
        return false;
    }
    return true;
}

bool isVehicleAvailable() {
    return vehicle != 0 && 
        ENTITY::DOES_ENTITY_EXIST(vehicle) && 
        playerPed == VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, -1);
}

void update_globals() {
    player = PLAYER::PLAYER_ID();
    playerPed = PLAYER::PLAYER_PED_ID();
    vehicle = PED::GET_VEHICLE_PED_IS_IN(playerPed, false);
}

void update_vehicle() {
    if (!isVehicleAvailable()) {
        clearVehicleData();
        lastVehicle = vehicle;
    }
    else {
        if (vehicle != lastVehicle) {
            clearVehicleData();
        }
        vehData.UpdateValues(ext, vehicle);
    }
    lastVehicle = vehicle;
}

// Read inputs
void update_inputs() {
    updateLastInputDevice();
    carControls.UpdateValues(carControls.PrevInput, false);
}

void update_hud() {
    if (!isPlayerAvailable() || !isVehicleAvailable()) {
        return;
    }

    if (settings.DisplayInfo) {
        drawDebugInfo();
    }
    if (settings.DisplayWheelInfo) {
        drawVehicleWheelInfo();
    }
    if (settings.HUD && vehData.Domain == VehicleDomain::Road &&
        (settings.EnableManual || settings.AlwaysHUD)) {
        drawHUD();
    }
    if (settings.HUD &&
        (vehData.Domain == VehicleDomain::Road || vehData.Domain == VehicleDomain::Water) &&
        (carControls.PrevInput == CarControls::Wheel || settings.AlwaysSteeringWheelInfo) &&
        settings.SteeringWheelInfo && textureWheelId != -1) {
        drawInputWheelInfo();
    }

    if (settings.DisplayFFBInfo) {
        float angle = getSteeringAngle(vehicle);
        drawSteeringLines(angle);
    }
}

void wheelControlWater() {
    checkWheelButtons();
    handlePedalsDefault(carControls.ThrottleVal, carControls.BrakeVal);
    doWheelSteering();
    playFFBWater();
}

void wheelControlRoad() {
    checkWheelButtons();
    if (settings.EnableManual && 
        !(vehData.Class == VehicleClass::Bike && settings.SimpleBike)) {
        handlePedalsRealReverse(carControls.ThrottleVal, carControls.BrakeVal);
    }
    else {
        handlePedalsDefault(carControls.ThrottleVal, carControls.BrakeVal);
    }
    doWheelSteering();
    if (vehData.Amphibious && ENTITY::GET_ENTITY_SUBMERGED_LEVEL(vehicle) > 0.10f) {
        playFFBWater();
    }
    else {
        playFFBGround();
    }
}

// Apply input as controls for selected devices
void update_input_controls() {
    if (!isPlayerAvailable())
        return;

    blockButtons();
    startStopEngine();

    if (!settings.EnableWheel || carControls.PrevInput != CarControls::Wheel) {
        return;
    }

    if (!carControls.WheelAvailable())
        return;

    if (isVehicleAvailable()) {
        switch (vehData.Domain) {
        case VehicleDomain::Road: {
            wheelControlRoad();
            break;
        }
        case VehicleDomain::Water: {
            // TODO: Option to disable for water
            wheelControlWater();
            break;
        }
        case VehicleDomain::Air: {
            // not supported
            break;
        }
        case VehicleDomain::Bicycle: {
            // not supported
            break;
        }
        case VehicleDomain::Rail: {
            // not supported
            break;
        }
        case VehicleDomain::Unknown: {
            break;
        }
        default: { }
        }
    }
}

// don't write with VehicleExtensions and dont set clutch state
void update_misc_features() {
    if (!isPlayerAvailable())
        return;

    if (isVehicleAvailable()) {
        if (settings.CrossScript) {
            crossScript();
        }

        if (settings.AutoLookBack) {
            functionAutoLookback();
        }
    }

    functionHidePlayerInFPV();
}

std::vector<bool> getWheelLockups(Vehicle handle) {
    std::vector<bool> lockups;
    float velocity = ENTITY::GET_ENTITY_VELOCITY(vehicle).y;
    auto wheelsSpeed = ext.GetWheelRotationSpeeds(vehicle);
    for (auto wheelSpeed : wheelsSpeed) {
        if (abs(velocity) > 0.01f && wheelSpeed == 0.0f)
            lockups.push_back(true);
        else
            lockups.push_back(false);
    }
    return lockups;
}

void handleBrakePatch() {
    bool lockedUp = false;
    bool ebrk = ext.GetHandbrake(vehicle);
    bool brn = VEHICLE::IS_VEHICLE_IN_BURNOUT(vehicle);

    auto lockUps = getWheelLockups(vehicle);
    auto susps = ext.GetWheelCompressions(vehicle);

    for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
        if (lockUps[i] && susps[i] > 0.001f)
            lockedUp = true;
    }
    if (ebrk || brn)
        lockedUp = false;

    if (g_CustomABS && lockedUp) {
        if (!MemoryPatcher::BrakeDecrementPatched) {
            MemoryPatcher::PatchBrakeDecrement();
        }
        for (int i = 0; i < lockUps.size(); i++) {
            ext.SetWheelBrakePressure(vehicle, i, ext.GetWheelBrakePressure(vehicle)[i] * 0.9f);
        }
        //showText(0.5, 0.1, 1.0, "~r~ABS ACTIVE~w~");
    }
    else {
        if (vehData.EngBrakeActive || vehData.EngLockActive) {
            if (!MemoryPatcher::BrakeDecrementPatched) {
                MemoryPatcher::PatchBrakeDecrement();
            }
        }
        else if (vehData.InduceBurnout) {
            if (!MemoryPatcher::BrakeDecrementPatched) {
                MemoryPatcher::PatchBrakeDecrement();
            }
            for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
                ext.SetWheelBrakePressure(vehicle, i, 0.0f);
            }
        }
        else {
            if (MemoryPatcher::BrakeDecrementPatched) {
                MemoryPatcher::RestoreBrakeDecrement();
            }
        }
    }
}

// Only when mod is working or writes clutch stuff.
// Called inside manual transmission part!
// Mostly optional stuff.
void update_manual_features() {
    if (settings.HillBrakeWorkaround) {
        functionHillGravity();
    }

    if (carControls.PrevInput != CarControls::InputDevices::Wheel) {
        if (vehData.Class == VehicleClass::Bike && settings.SimpleBike) {
            functionAutoReverse();
        }
        else {
            functionRealReverse();
        }
    }

    if (settings.EngBrake) {
        functionEngBrake();
    }
    else {
        vehData.EngBrakeActive = false;
    }

    // Engine damage: RPM Damage
    if (settings.EngDamage &&
        !vehData.NoClutch) {
        functionEngDamage();
    }

    if (settings.EngLock) {
        functionEngLock();
    }
    else {
        vehData.EngLockActive = false;
    }

    if (!vehData.FakeNeutral &&
        !(settings.SimpleBike && vehData.Class == VehicleClass::Bike) &&
        !vehData.NoClutch) {
        // Stalling
        if (settings.EngStall && settings.ShiftMode == HPattern ||
            settings.EngStallS && settings.ShiftMode == Sequential) {
            functionEngStall();
        }

        // Simulate "catch point"
        // When the clutch "grabs" and the car starts moving without input
        if (settings.ClutchCatching && VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
            functionClutchCatch();
        }
    }

    handleBrakePatch();

    if (gearRattle.Active) {
        if (carControls.ClutchVal > 1.0f - settings.ClutchThreshold ||
            !VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
            gearRattle.Stop();
        }
    }
}

// Manual Transmission part of the mod
void update_manual_transmission() {
    if (!isPlayerAvailable() || !isVehicleAvailable()) {
        return;
    }

    //updateSteeringMultiplier();

    if (carControls.ButtonJustPressed(CarControls::KeyboardControlType::Toggle) ||
        carControls.ButtonHeld(CarControls::WheelControlType::Toggle, 500) ||
        carControls.ButtonHeld(CarControls::ControllerControlType::Toggle) ||
        carControls.PrevInput == CarControls::Controller	&& carControls.ButtonHeld(CarControls::LegacyControlType::Toggle)) {
        toggleManual();
        return;
    }

    if (vehData.Domain != VehicleDomain::Road || !settings.EnableManual)
        return;

    ///////////////////////////////////////////////////////////////////////////
    //          Active whenever Manual is enabled from here
    ///////////////////////////////////////////////////////////////////////////

    if (carControls.ButtonJustPressed(CarControls::KeyboardControlType::ToggleH) ||
        carControls.ButtonJustPressed(CarControls::WheelControlType::ToggleH) ||
        carControls.ButtonHeld(CarControls::ControllerControlType::ToggleH) ||
        carControls.PrevInput == CarControls::Controller	&& carControls.ButtonHeld(CarControls::LegacyControlType::ToggleH)) {
        cycleShiftMode();
    }

    if (MemoryPatcher::TotalPatched != MemoryPatcher::NumGearboxPatches) {
        MemoryPatcher::ApplyGearboxPatches();
    }

    update_manual_features();

    switch(settings.ShiftMode) {
        case Sequential: {
            functionSShift();
            if (settings.AutoGear1) {
                functionAutoGear1();
            }
            break;
        }
        case HPattern: {
            if (carControls.PrevInput == CarControls::Wheel) {
                functionHShiftWheel();
            }
            if (carControls.PrevInput == CarControls::Keyboard ||
                carControls.PrevInput == CarControls::Wheel && settings.HPatternKeyboard) {
                functionHShiftKeyboard();
            }
            break;
        }
        case Automatic: {
            functionAShift();
            break;
        }
        default: break;
    }

    functionLimiter();

    // Finally, update memory each loop
    handleRPM();
    ext.SetGearCurr(vehicle, vehData.LockGear);
    ext.SetGearNext(vehicle, vehData.LockGear);
}

///////////////////////////////////////////////////////////////////////////////
//                            Helper things
///////////////////////////////////////////////////////////////////////////////
void crossScript() {
    if (!isVehicleAvailable())
        return;

    // Current gear
    DECORATOR::DECOR_SET_INT(vehicle, (char *)decorCurrentGear, ext.GetGearCurr(vehicle));

    // Shift indicator: 0 = nothing, 1 = Shift up, 2 = Shift down
    if (vehData.HitLimiter) {
        DECORATOR::DECOR_SET_INT(vehicle, (char *)decorShiftNotice, 1);
    }
    else if (ext.GetGearCurr(vehicle) > 1 && vehData.RPM < 0.4f) {
        DECORATOR::DECOR_SET_INT(vehicle, (char *)decorShiftNotice, 2);
    }
    else if (ext.GetGearCurr(vehicle) == ext.GetGearNext(vehicle)) {
        DECORATOR::DECOR_SET_INT(vehicle, (char *)decorShiftNotice, 0);
    }

    // Simulated Neutral
    if (vehData.FakeNeutral && settings.EnableManual) {
        DECORATOR::DECOR_SET_INT(vehicle, (char *)decorFakeNeutral, 1);
    }
    else {
        DECORATOR::DECOR_SET_INT(vehicle, (char *)decorFakeNeutral, 0);
    }

    // External shifting
    int currExtShift = DECORATOR::DECOR_GET_INT(vehicle, (char *)decorSetShiftMode);
    if (prevExtShift != currExtShift && currExtShift > 0) {
        // 1 Seq, 2 H, 3 Auto
        setShiftMode(currExtShift - 1);
    }
    prevExtShift = currExtShift;

    DECORATOR::DECOR_SET_INT(vehicle, (char *)decorGetShiftMode, static_cast<int>(settings.ShiftMode) + 1);
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Mod control
///////////////////////////////////////////////////////////////////////////////

void clearVehicleData() {
    vehData = VehicleData();
    if (vehData.NoClutch) {
        vehData.FakeNeutral = false;
    }
    else {
        vehData.FakeNeutral = settings.DefaultNeutral;
    }
    gearRattle.Stop();
}

void clearPatches() {
    resetSteeringMultiplier();
    if (MemoryPatcher::TotalPatched != 0) {
        MemoryPatcher::RevertGearboxPatches();
    }
    if (MemoryPatcher::SteerControlPatched) {
        MemoryPatcher::RestoreSteeringControl();
    }
    if (MemoryPatcher::SteerCorrectPatched) {
        MemoryPatcher::RestoreSteeringCorrection();
    }
    if (MemoryPatcher::BrakeDecrementPatched) {
        MemoryPatcher::RestoreBrakeDecrement();
    }
}

void toggleManual() {
    settings.EnableManual = !settings.EnableManual;
    settings.SaveGeneral();
    std::string message = mtPrefix;
    if (settings.EnableManual) {
        message += "Enabled";
    }
    else {
        message += "Disabled";
    }
    showNotification(message, &prevNotification);
    clearVehicleData();
    readSettings();
    initWheel();
    clearPatches();
}

void initWheel() {
    carControls.InitWheel();
    carControls.CheckGUIDs(settings.RegisteredGUIDs);
    carControls.SteerGUID = carControls.WheelAxesGUIDs[static_cast<int>(carControls.SteerAxisType)];
    logger.Write(INFO, "WHEEL: Steering wheel initialization finished");
}

void stopForceFeedback() {
    carControls.StopFFB(settings.LogiLEDs);
}

void update_steeringpatches() {
    bool isCar = vehData.Class == VehicleClass::Car;
    bool useWheel = carControls.PrevInput == CarControls::Wheel;
    if (isCar && settings.PatchSteering &&
        (useWheel || settings.PatchSteeringAlways)) {
        if (!MemoryPatcher::SteerCorrectPatched)
            MemoryPatcher::PatchSteeringCorrection();
    }
    else {
        if (MemoryPatcher::SteerCorrectPatched)
            MemoryPatcher::RestoreSteeringCorrection();
    }

    if (isCar && useWheel && settings.PatchSteeringControl) {
        if (!MemoryPatcher::SteerControlPatched)
            MemoryPatcher::PatchSteeringControl();
    }
    else {
        if (MemoryPatcher::SteerControlPatched)
            MemoryPatcher::RestoreSteeringControl();
    }
    if (isVehicleAvailable()) {
        updateSteeringMultiplier();
    }
}

// From CustomSteering
void updateSteeringMultiplier() {
    float mult = 1;

    if (MemoryPatcher::SteerCorrectPatched) {
        Vector3 vel = ENTITY::GET_ENTITY_VELOCITY(vehicle);
        Vector3 pos = ENTITY::GET_ENTITY_COORDS(vehicle, 1);
        Vector3 motion = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, pos.x + vel.x, pos.y + vel.y, pos.z + vel.z);

        if (motion.y > 3) {
            mult = (0.15f + (powf((1.0f / 1.13f), (abs(motion.y) - 7.2f))));
            if (mult != 0) { mult = floorf(mult * 1000) / 1000; }
            if (mult > 1) { mult = 1; }
        }

        if (carControls.PrevInput == CarControls::Wheel) {
            mult = (1 + (mult - 1) * settings.SteeringReductionWheel);
        }
        else {
            mult = (1 + (mult - 1) * settings.SteeringReductionOther);
        }
    }

    if (carControls.PrevInput == CarControls::Wheel) {
        mult = mult * settings.GameSteerMultWheel;
    }
    else {
        mult = mult * settings.GameSteerMultOther;
    }
    ext.SetSteeringMultiplier(vehicle, mult);
}

void resetSteeringMultiplier() {
    if (vehicle != 0) {
        ext.SetSteeringMultiplier(vehicle, 1.0f);
    }
}

void updateLastInputDevice() {
    if (carControls.PrevInput != carControls.GetLastInputDevice(carControls.PrevInput,settings.EnableWheel)) {
        carControls.PrevInput = carControls.GetLastInputDevice(carControls.PrevInput, settings.EnableWheel);
        std::string message = mtPrefix + "Input: ";
        switch (carControls.PrevInput) {
            case CarControls::Keyboard:
                message += "Keyboard";
                break;
            case CarControls::Controller:
                message += "Controller";
                if (settings.ShiftMode == HPattern && settings.BlockHShift) {
                    message += "~n~Mode: Sequential";
                    setShiftMode(Sequential);
                }
                break;
            case CarControls::Wheel:
                message += "Steering wheel";
                break;
            default: break;
        }
        // Suppress notification when not in car
        if (isVehicleAvailable()) {
            showNotification(message, &prevNotification);
        }
    }
    if (carControls.PrevInput == CarControls::Wheel) {
        CONTROLS::STOP_PAD_SHAKE(0);
    }
    else {
        stopForceFeedback();
    }
}

///////////////////////////////////////////////////////////////////////////////
//                           Mod functions: Shifting
///////////////////////////////////////////////////////////////////////////////

void setShiftMode(int shiftMode) {
    gearRattle.Stop();
    if (shiftMode > 2 || shiftMode < 0)
        return;

    if ((shiftMode == Automatic ||
        shiftMode == Sequential) && ext.GetGearCurr(vehicle) > 1) {
        vehData.FakeNeutral = false;
    }

    if (shiftMode == HPattern &&
        carControls.PrevInput == CarControls::Controller && 
        settings.BlockHShift) {
        settings.ShiftMode = Automatic;
    }
    else {
        settings.ShiftMode = (ShiftModes)shiftMode;
    }

    std::string mode = mtPrefix + "Mode: ";
    // ReSharper disable once CppDefaultCaseNotHandledInSwitchStatement
    switch (settings.ShiftMode) {
        case Sequential: mode += "Sequential"; break;
        case HPattern: mode += "H-Pattern"; break;
        case Automatic: mode += "Automatic"; break;
        default: break;
    }
    showNotification(mode, &prevNotification);
}

void cycleShiftMode() {
    int tempShiftMode = settings.ShiftMode + 1;
    if (tempShiftMode >= SIZEOF_ShiftModes) {
        tempShiftMode = (ShiftModes)0;
    }

    setShiftMode(tempShiftMode);
    settings.SaveGeneral();
}

void shiftTo(int gear, bool autoClutch) {
    if (autoClutch) {
        carControls.ClutchVal = 1.0f;
        CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
    }
    vehData.LockGear = gear;
}
void functionHShiftTo(int i) {
    if (settings.ClutchShiftingH && !vehData.NoClutch) {
        if (carControls.ClutchVal > 1.0f - settings.ClutchThreshold) {
            shiftTo(i, false);
            vehData.FakeNeutral = false;
            gearRattle.Stop();
        }
        else {
            gearRattle.Play(vehicle);
            vehData.FakeNeutral = true;
            if (settings.EngDamage &&
                !vehData.NoClutch) {
                VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
                    vehicle,
                    VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - settings.MisshiftDamage);
            }
        }
    }
    else {
        shiftTo(i, true);
        vehData.FakeNeutral = false;
        gearRattle.Stop();
    }
}

void functionHShiftKeyboard() {
    int clamp = MAX_GEAR;
    if (ext.GetTopGear(vehicle) <= clamp) {
        clamp = ext.GetTopGear(vehicle);
    }
    for (uint8_t i = 0; i <= clamp; i++) {
        if (carControls.ButtonJustPressed(static_cast<CarControls::KeyboardControlType>(i))) {
            functionHShiftTo(i);
        }
    }
    if (carControls.ButtonJustPressed(CarControls::KeyboardControlType::HN) &&
        !vehData.NoClutch) {
        vehData.FakeNeutral = !vehData.FakeNeutral;
    }
}

void functionHShiftWheel() {
    int clamp = MAX_GEAR;
    if (ext.GetTopGear(vehicle) <= clamp) {
        clamp = ext.GetTopGear(vehicle);
    }
    for (uint8_t i = 0; i <= clamp; i++) {
        if (carControls.ButtonJustPressed(static_cast<CarControls::WheelControlType>(i))) {
            functionHShiftTo(i);
        }
    }
    // Bleh
    if (carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H1)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H2)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H3)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H4)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H5)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H6)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::H7)) ||
        carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::HR))
    ) {
        if (settings.ClutchShiftingH &&
            settings.EngDamage &&
            !vehData.NoClutch) {
            if (carControls.ClutchVal < 1.0 - settings.ClutchThreshold) {
                gearRattle.Play(vehicle);
            }
        }
        vehData.FakeNeutral = !vehData.NoClutch;
    }

    if (carControls.ButtonReleased(static_cast<CarControls::WheelControlType>(CarControls::WheelControlType::HR))) {
        shiftTo(1, true);
        vehData.FakeNeutral = !vehData.NoClutch;
    }
}

bool isUIActive() {
    if (PED::IS_PED_RUNNING_MOBILE_PHONE_TASK(playerPed) || menu.IsThisOpen())
        return true;
    return false;
}

void functionSShift() {
    auto xcTapStateUp = carControls.ButtonTapped(CarControls::ControllerControlType::ShiftUp);
    auto xcTapStateDn = carControls.ButtonTapped(CarControls::ControllerControlType::ShiftDown);

    auto ncTapStateUp = carControls.ButtonTapped(CarControls::LegacyControlType::ShiftUp);
    auto ncTapStateDn = carControls.ButtonTapped(CarControls::LegacyControlType::ShiftDown);

    if (settings.IgnoreShiftsUI && isUIActive()) {
        xcTapStateUp = xcTapStateDn = XInputController::TapState::ButtonUp;
        ncTapStateUp = ncTapStateDn = NativeController::TapState::ButtonUp;
    }

    // Shift up
    if (carControls.PrevInput == CarControls::Controller	&& xcTapStateUp == XInputController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Controller	&& ncTapStateUp == NativeController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftUp) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::ShiftUp)) {
        if (vehData.NoClutch) {
            if (ext.GetGearCurr(vehicle) < ext.GetTopGear(vehicle)) {
                shiftTo(vehData.LockGear + 1, true);
            }
            vehData.FakeNeutral = false;
            return;
        }

        // Shift block /w clutch shifting for seq.
        if (settings.ClutchShiftingS && 
            carControls.ClutchVal < 1.0f - settings.ClutchThreshold) {
            return;
        }

        // Reverse to Neutral
        if (ext.GetGearCurr(vehicle) == 0 && !vehData.FakeNeutral) {
            shiftTo(1, true);
            vehData.FakeNeutral = true;
            return;
        }

        // Neutral to 1
        if (ext.GetGearCurr(vehicle) == 1 && vehData.FakeNeutral) {
            vehData.FakeNeutral = false;
            return;
        }

        // 1 to X
        if (ext.GetGearCurr(vehicle) < ext.GetTopGear(vehicle)) {
            shiftTo(vehData.LockGear + 1, true);
            vehData.FakeNeutral = false;
            return;
        }
    }

    // Shift down

    if (carControls.PrevInput == CarControls::Controller	&& xcTapStateDn == XInputController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Controller	&& ncTapStateDn == NativeController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftDown) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::ShiftDown)) {
        if (vehData.NoClutch) {
            if (ext.GetGearCurr(vehicle) > 0) {
                shiftTo(vehData.LockGear - 1, true);
                vehData.FakeNeutral = false;
            }
            return;
        }

        // Shift block /w clutch shifting for seq.
        if (settings.ClutchShiftingS &&
            carControls.ClutchVal < 1.0f - settings.ClutchThreshold) {
            return;
        }

        // 1 to Neutral
        if (ext.GetGearCurr(vehicle) == 1 && !vehData.FakeNeutral) {
            vehData.FakeNeutral = true;
            return;
        }

        // Neutral to R
        if (ext.GetGearCurr(vehicle) == 1 && vehData.FakeNeutral) {
            shiftTo(0, true);
            vehData.FakeNeutral = false;
            return;
        }

        // X to 1
        if (ext.GetGearCurr(vehicle) > 1) {
            shiftTo(vehData.LockGear - 1, true);
            vehData.FakeNeutral = false;
        }
        return;
    }
}

/*
 * Manual part of the automatic transmission (R<->N<->D)
 */
bool subAShiftManual() {
    auto xcTapStateUp = carControls.ButtonTapped(CarControls::ControllerControlType::ShiftUp);
    auto xcTapStateDn = carControls.ButtonTapped(CarControls::ControllerControlType::ShiftDown);

    auto ncTapStateUp = carControls.ButtonTapped(CarControls::LegacyControlType::ShiftUp);
    auto ncTapStateDn = carControls.ButtonTapped(CarControls::LegacyControlType::ShiftDown);

    if (settings.IgnoreShiftsUI && isUIActive()) {
        xcTapStateUp = xcTapStateDn = XInputController::TapState::ButtonUp;
        ncTapStateUp = ncTapStateDn = NativeController::TapState::ButtonUp;
    }

    // Shift up
    if (carControls.PrevInput == CarControls::Controller	&& xcTapStateUp == XInputController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Controller	&& ncTapStateUp == NativeController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftUp) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::ShiftUp)) {
        // Reverse to Neutral
        if (ext.GetGearCurr(vehicle) == 0 && !vehData.FakeNeutral) {
            shiftTo(1, true);
            vehData.FakeNeutral = true;
            return true;
        }

        // Neutral to 1
        if (ext.GetGearCurr(vehicle) == 1 && vehData.FakeNeutral) {
            vehData.FakeNeutral = false;
            return true;
        }
    }

    // Shift down
    if (carControls.PrevInput == CarControls::Controller	&& xcTapStateDn == XInputController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Controller	&& ncTapStateDn == NativeController::TapState::Tapped ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::ShiftDown) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::ShiftDown)) {
        // 1 to Neutral
        if (ext.GetGearCurr(vehicle) == 1 && !vehData.FakeNeutral) {
            vehData.FakeNeutral = true;
            return true;
        }

        // Neutral to R
        if (ext.GetGearCurr(vehicle) == 1 && vehData.FakeNeutral) {
            shiftTo(0, true);
            vehData.FakeNeutral = false;
            return true;
        }
    }
    return false;
}

void functionAShift() { 
    // Manual part
    if (subAShiftManual()) return;
    
    int currGear = ext.GetGearCurr(vehicle);
    if (currGear == 0) return;

    float currSpeed = vehData.SpeedVector.y;
    auto ratios = ext.GetGearRatios(vehicle);
    float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
    float InitialDriveMaxFlatVel = ext.GetInitialDriveMaxFlatVel(vehicle);

    float maxSpeedUpShiftWindow = DriveMaxFlatVel / ratios[currGear];
    float minSpeedUpShiftWindow = InitialDriveMaxFlatVel / ratios[currGear];
    float currGearDelta = maxSpeedUpShiftWindow - minSpeedUpShiftWindow;

    float prevGearTopSpeed = DriveMaxFlatVel / ratios[currGear - 1];
    float prevGearMinSpeed = InitialDriveMaxFlatVel / ratios[currGear - 1];
    float highEndShiftSpeed = fminf(minSpeedUpShiftWindow, prevGearTopSpeed);
    float prevGearDelta = prevGearTopSpeed - prevGearMinSpeed;

    float upshiftSpeed   = map(pow(carControls.ThrottleVal, 6.0f), 0.0f, 1.0f, minSpeedUpShiftWindow, maxSpeedUpShiftWindow);
    float downshiftSpeed = map(pow(carControls.ThrottleVal, 6.0f), 0.0f, 1.0f, prevGearMinSpeed,      highEndShiftSpeed);

    float acceleration = vehData.GetRelativeAcceleration().y / 9.81f;
    float accelExpect = pow(carControls.ThrottleVal, 2.0f) * ratios[currGear] * VEHICLE::GET_VEHICLE_ACCELERATION(vehicle);

    // Shift up.
    if (currGear < ext.GetTopGear(vehicle)) {
        bool shouldShiftUpSPD = currSpeed > upshiftSpeed;
        bool shouldShiftUpACC = acceleration < 0.33f * accelExpect && !vehData.IgnoreAccelerationUpshiftTrigger;

        if (currSpeed > minSpeedUpShiftWindow && (shouldShiftUpSPD || shouldShiftUpACC)) {
            vehData.IgnoreAccelerationUpshiftTrigger = true;
            vehData.PrevUpshiftTime = GetTickCount();
            shiftTo(ext.GetGearCurr(vehicle) + 1, true);
            vehData.FakeNeutral = false;
            vehData.UpshiftSpeedsMod[currGear] = currSpeed;
        }

        if (vehData.IgnoreAccelerationUpshiftTrigger) {
            float upshiftTimeout = 1.0f / *(float*)(ext.GetHandlingPtr(vehicle) + hOffsets.fClutchChangeRateScaleUpShift);
            if (vehData.IgnoreAccelerationUpshiftTrigger && GetTickCount() > vehData.PrevUpshiftTime + (int)(upshiftTimeout*1000.0f)) {
                vehData.IgnoreAccelerationUpshiftTrigger = false;
            }
        }
    }

    // Shift down
    if (currGear > 1) {
        if (currSpeed < downshiftSpeed - prevGearDelta) {
            shiftTo(currGear - 1, true);
            vehData.FakeNeutral = false;
        }
    }

    if (settings.DisplayInfo && !menu.IsThisOpen()) {
        showText(0.01, 0.525, 0.3, "CurrentSpeed: \t" + std::to_string(currSpeed));
        showText(0.01, 0.550, 0.3, "UpshiftSpeed: \t" + std::to_string(upshiftSpeed));
        showText(0.01, 0.575, 0.3, "DnshiftSpeed: \t" + std::to_string(downshiftSpeed - prevGearDelta));
        showText(0.01, 0.600, 0.3, "PrevGearDelta: \t" + std::to_string(prevGearDelta));
        showText(0.01, 0.625, 0.3, "CurrGearDelta: \t" + std::to_string(currGearDelta));
        showText(0.01, 0.650, 0.3, "Accel(Expect): \t" + std::to_string(accelExpect));
        showText(0.01, 0.675, 0.3, "Accel(Actual): \t\t" + std::to_string(acceleration));

    }
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Gearbox features
///////////////////////////////////////////////////////////////////////////////

void functionClutchCatch() {
    float lowSpeed = g_baseCatchMinSpeed * (ext.GetDriveMaxFlatVel(vehicle) / ext.GetGearRatios(vehicle)[ext.GetGearCurr(vehicle)]);
    float mehSpeed = g_baseCatchMaxSpeed * (ext.GetDriveMaxFlatVel(vehicle) / ext.GetGearRatios(vehicle)[ext.GetGearCurr(vehicle)]);

    const float idleThrottle = 0.24f;

    if (carControls.ClutchVal < 1.0f - settings.ClutchThreshold && 
        carControls.ThrottleVal < 0.25f && carControls.BrakeVal < 0.95) {
        if (settings.ShiftMode != HPattern && carControls.BrakeVal > 0.1f || 
            vehData.RPM > 0.25f && vehData.SpeedVector.y >= lowSpeed) {
            return;
        }

        // Forward
        if (ext.GetGearCurr(vehicle) > 0 && vehData.SpeedVector.y < mehSpeed) {
            float throttle = map(vehData.SpeedVector.y, mehSpeed, lowSpeed, 0.0f, idleThrottle);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, throttle);
            ext.SetCurrentRPM(vehicle, 0.21f);
            ext.SetThrottle(vehicle, 0.0f);
        }

        // Reverse
        if (ext.GetGearCurr(vehicle) == 0) {
            float throttle = map(abs(vehData.SpeedVector.y), abs(mehSpeed), abs(lowSpeed), 0.0f, idleThrottle);
            // due to 0.25f deadzone throttle makes reversing anims stop, so its jerky
            // TODO: needs a workaround to prevent jerky reversing anims
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, throttle); 
            ext.SetCurrentRPM(vehicle, 0.21f);
            ext.SetThrottle(vehicle, 0.0f);
        }
    }
}

// im solving the worng problem here help
std::vector<bool> getDrivenWheels() {
    int numWheels = ext.GetNumWheels(vehicle);
    std::vector<bool> wheelsToConsider;
    if (ext.GetDriveBiasFront(vehicle) > 0.0f && ext.GetDriveBiasFront(vehicle) < 1.0f) {
        for (int i = 0; i < numWheels; i++)
            wheelsToConsider.push_back(true);
    }
    else {
        // bikes
        if (numWheels == 2) {
            // Front id = 1, rear id = 0...
            wheelsToConsider.push_back(true);
            wheelsToConsider.push_back(false);
        }
        // normal cars
        else if (numWheels == 4) {
            // fwd
            if (ext.GetDriveBiasFront(vehicle) == 1.0f) {
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
            }
            // rwd
            else if (ext.GetDriveBiasFront(vehicle) == 0.0f) {
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
            }
        }
        // offroad, trucks
        else if (numWheels == 6) {
            // fwd
            if (ext.GetDriveBiasFront(vehicle) == 1.0f) {
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
            }
            // rwd
            else if (ext.GetDriveBiasFront(vehicle) == 0.0f) {
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(false);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
                wheelsToConsider.push_back(true);
            }
        }
        else {
            for (int i = 0; i < numWheels; i++)
                wheelsToConsider.push_back(true);
        }
    }
    return wheelsToConsider;
}


std::vector<float> getDrivenWheelsSpeeds(std::vector<float> wheelSpeeds) {
    std::vector<float> wheelsToConsider;
    if (ext.GetDriveBiasFront(vehicle) > 0.0f && ext.GetDriveBiasFront(vehicle) < 1.0f) {
        wheelsToConsider = wheelSpeeds;
    }
    else {
        // bikes
        if (ext.GetNumWheels(vehicle) == 2) {
            wheelsToConsider.push_back(wheelSpeeds[0]);
        }
        // normal cars
        else if (ext.GetNumWheels(vehicle) == 4) {
            // fwd
            if (ext.GetDriveBiasFront(vehicle) == 1.0f) {
                wheelsToConsider.push_back(wheelSpeeds[0]);
                wheelsToConsider.push_back(wheelSpeeds[1]);
            }
            // rwd
            else if (ext.GetDriveBiasFront(vehicle) == 0.0f) {
                wheelsToConsider.push_back(wheelSpeeds[2]);
                wheelsToConsider.push_back(wheelSpeeds[3]);
            }
        }
        // offroad, trucks
        else if (ext.GetNumWheels(vehicle) == 6) {
            // fwd
            if (ext.GetDriveBiasFront(vehicle) == 1.0f) {
                wheelsToConsider.push_back(wheelSpeeds[0]);
                wheelsToConsider.push_back(wheelSpeeds[1]);
            }
            // rwd
            else if (ext.GetDriveBiasFront(vehicle) == 0.0f) {
                wheelsToConsider.push_back(wheelSpeeds[2]);
                wheelsToConsider.push_back(wheelSpeeds[3]);
                wheelsToConsider.push_back(wheelSpeeds[4]);
                wheelsToConsider.push_back(wheelSpeeds[5]);
            }
        }
        else {
            wheelsToConsider = wheelSpeeds;
        }
    }
    return wheelsToConsider;
}

void functionEngStall() {
    float avgWheelSpeed = abs(avg(getDrivenWheelsSpeeds(ext.GetTyreSpeeds(vehicle))));
    float stallSpeed = g_baseStallSpeed * abs(ext.GetDriveMaxFlatVel(vehicle)/ext.GetGearRatios(vehicle)[ext.GetGearCurr(vehicle)]);
    float stallTime = 0.30f;

    //showText(0.1, 0.1, 1.0, "Stall progress: " + std::to_string(vehData.StallProgress));
    //showText(0.1, 0.2, 1.0, "Stall speed: " + std::to_string(stallSpeed));
    //showText(0.1, 0.3, 1.0, "Actual speed: " + std::to_string(avgWheelSpeed));
    //showText(0.1, 0.4, 1.0, "Stall thres: " + std::to_string(1.0f - settings.StallingThreshold));
    //showText(0.1, 0.5, 1.0, "ClutchVal: " + std::to_string(controls.ClutchVal));

    // this thing is big when the clutch isnt pressed
    float invClutch = 1.0f - carControls.ClutchVal;

    if (invClutch > settings.StallingThreshold &&
        vehData.RPM < 0.25f &&
        avgWheelSpeed < stallSpeed &&
        VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
        vehData.StallProgress += invClutch * 1.0f/stallTime * GAMEPLAY::GET_FRAME_TIME();
    }
    else if (vehData.StallProgress > 0.0f) {
        vehData.StallProgress -= invClutch * 1.0f/stallTime * GAMEPLAY::GET_FRAME_TIME();
    }

    if (vehData.StallProgress > 1.0f) {
        if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
            VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, false, true, true);
        }
        gearRattle.Stop();
        vehData.StallProgress = 0.0f;
    }

    // Simulate push-start
    // We'll just assume the ignition thing is in the "on" position.
    if (avgWheelSpeed > stallSpeed && !VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle) &&
        invClutch > settings.StallingThreshold) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, true, true);
    }
}

void functionEngDamage() {
    if (settings.ShiftMode == Automatic ||
        ext.GetVehicleFlags(vehicle)[1] & eVehicleFlag2::FLAG_IS_ELECTRIC || 
        ext.GetTopGear(vehicle) == 1) {
        return;
    }

    if (ext.GetGearCurr(vehicle) != ext.GetTopGear(vehicle) &&
        vehData.RPM > 0.98f &&
        carControls.ThrottleVal > 0.98f) {
        VEHICLE::SET_VEHICLE_ENGINE_HEALTH(vehicle, 
                                           VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle) - (settings.RPMDamage));
    }
}

void functionEngLock() {
    if (settings.ShiftMode == Automatic ||
        ext.GetVehicleFlags(vehicle)[1] & eVehicleFlag2::FLAG_IS_ELECTRIC ||
        ext.GetTopGear(vehicle) == 1 || 
        ext.GetGearCurr(vehicle) == ext.GetTopGear(vehicle) ||
        vehData.FakeNeutral) {
        vehData.EngLockActive = false;
        gearRattle.Stop();
        return;
    }
    const float reverseThreshold = 2.0f;

    float dashms = abs(ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y);

    float speed = dashms;
    auto ratios = ext.GetGearRatios(vehicle);
    float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
    float maxSpeed = DriveMaxFlatVel / ratios[ext.GetGearCurr(vehicle)];

    float inputMultiplier = (1.0f - carControls.ClutchVal);
    auto wheelsSpeed = avg(getDrivenWheelsSpeeds(ext.GetTyreSpeeds(vehicle)));

    bool wrongDirection = false;
    if (ext.GetGearCurr(vehicle) == 0 && VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > reverseThreshold && wheelsSpeed > reverseThreshold) {
            wrongDirection = true;
        }
    }
    else {
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < -reverseThreshold && wheelsSpeed < -reverseThreshold) {
            wrongDirection = true;
        }
    }

    // Wheels are locking up due to bad (down)shifts
    if ((speed > abs(maxSpeed * 1.15f) + 3.334f || wrongDirection) && inputMultiplier > settings.ClutchThreshold) {
        vehData.EngLockActive = true;
        float lockingForce = 60.0f * inputMultiplier;
        auto wheelsToLock = getDrivenWheels();

        for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
            if (i >= wheelsToLock.size() || wheelsToLock[i]) {
                ext.SetWheelBrakePressure(vehicle, i, lockingForce);
                ext.SetWheelSkidSmokeEffect(vehicle, i, lockingForce);
            }
            else {
                float inpBrakeForce = *reinterpret_cast<float *>(ext.GetHandlingPtr(vehicle) + hOffsets.fBrakeForce) * carControls.BrakeVal;
                ext.SetWheelBrakePressure(vehicle, i, inpBrakeForce);
            }
        }
        fakeRev(true, 1.0f);
        gearRattle.Play(vehicle);
        float oldEngineHealth = VEHICLE::GET_VEHICLE_ENGINE_HEALTH(vehicle);
        float damageToApply = settings.MisshiftDamage * inputMultiplier;
        if (settings.EngDamage) {
            if (oldEngineHealth >= damageToApply) {
                VEHICLE::SET_VEHICLE_ENGINE_HEALTH(
                    vehicle,
                    oldEngineHealth - damageToApply);
            }
            else {
                VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, false, true, true);
            }
        }
        if (settings.DisplayInfo) {
            showText(0.5, 0.80, 0.25, "Eng block @ " + std::to_string(static_cast<int>(inputMultiplier * 100.0f)) + "%");
            showText(0.5, 0.85, 0.25, "Eng block @ " + std::to_string(lockingForce));
        }
    }
    else {
        vehData.EngLockActive = false;
        gearRattle.Stop();
    }
}

void functionEngBrake() {
    // When you let go of the throttle at high RPMs
    float activeBrakeThreshold = settings.EngBrakeThreshold;

    if (vehData.RPM >= activeBrakeThreshold && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > 5.0f && !vehData.FakeNeutral) {
        float handlingBrakeForce = *reinterpret_cast<float *>(ext.GetHandlingPtr(vehicle) + hOffsets.fBrakeForce);
        float inpBrakeForce = handlingBrakeForce * carControls.BrakeVal;

        float throttleMultiplier = 1.0f - carControls.ThrottleVal;
        float clutchMultiplier = 1.0f - carControls.ClutchVal;
        float inputMultiplier = throttleMultiplier * clutchMultiplier;
        if (inputMultiplier > 0.0f) {
            vehData.EngBrakeActive = true;
            float rpmMultiplier = (vehData.RPM - activeBrakeThreshold) / (1.0f - activeBrakeThreshold);
            float engBrakeForce = settings.EngBrakePower * handlingBrakeForce * inputMultiplier * rpmMultiplier;
            auto wheelsToBrake = getDrivenWheels();
            for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
                if (i >= wheelsToBrake.size() || wheelsToBrake[i]) {
                    ext.SetWheelBrakePressure(vehicle, i, engBrakeForce + inpBrakeForce);
                }
                else {
                    ext.SetWheelBrakePressure(vehicle, i, inpBrakeForce);
                }
            }
            if (settings.DisplayInfo) {
                showText(0.85, 0.500, 0.4, "EngBrake:\t\t " + std::to_string(static_cast<int>(inputMultiplier * 100.0f)) + "%", 4);
                showText(0.85, 0.525, 0.4, "Pressure:\t\t " + std::to_string(engBrakeForce), 4);
                showText(0.85, 0.550, 0.4, "BrkInput:\t\t " + std::to_string(inpBrakeForce), 4);
            }
        }
        
    }
    else {
        vehData.EngBrakeActive = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Gearbox control
///////////////////////////////////////////////////////////////////////////////

void fakeRev(bool customThrottle, float customThrottleVal) {
    float throttleVal = customThrottle ? customThrottleVal : carControls.ThrottleVal;
    float timeStep = SYSTEM::TIMESTEP();
    float accelRatio = 2.5f * timeStep;
    float rpmValTemp = vehData.PrevRPM > vehData.RPM ? vehData.PrevRPM - vehData.RPM : 0.0f;
    if (ext.GetGearCurr(vehicle) == 1) {			// For some reason, first gear revs slower
        rpmValTemp *= 2.0f;
    }
    float rpmVal = vehData.RPM +			// Base value
        rpmValTemp +						// Keep it constant
        throttleVal * accelRatio;	// Addition value, depends on delta T
    //showText(0.4, 0.4, 2.0, "FakeRev");
    ext.SetCurrentRPM(vehicle, rpmVal);
}

void handleRPM() {
    float clutch = carControls.ClutchVal;

    // Ignores clutch 
    if (settings.ShiftMode == Automatic ||
        vehData.Class == VehicleClass::Bike && settings.SimpleBike) {
        clutch = 0.0f;
    }

    //bool skip = false;

    // Game wants to shift up. Triggered at high RPM, high speed.
    // Desired result: high RPM, same gear, no more accelerating
    // Result:	Is as desired. Speed may drop a bit because of game clutch.
    // Update 2017-08-12: We know the gear speeds now, consider patching
    // shiftUp completely?
    if (ext.GetGearCurr(vehicle) > 0 &&
        (vehData.HitLimiter && ENTITY::GET_ENTITY_SPEED(vehicle) > 2.0f)) {
        ext.SetThrottle(vehicle, 1.0f);
        fakeRev(false, 0);
        CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
        float counterForce = 0.25f*-(static_cast<float>(ext.GetTopGear(vehicle)) - static_cast<float>(ext.GetGearCurr(vehicle)))/static_cast<float>(ext.GetTopGear(vehicle));
        ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(vehicle, 1, 0.0f, counterForce, 0.0f, true, true, true, true);
        //showText(0.4, 0.1, 1.0, "REV LIM");
        //showText(0.4, 0.15, 1.0, "CF: " + std::to_string(counterForce));
    }
    
    /*
        Game doesn't rev on disengaged clutch in any gear but 1
        This workaround tries to emulate this
        Default: ext.GetClutch(vehicle) >= 0.6: Normal
        Default: ext.GetClutch(vehicle) < 0.6: Nothing happens
        Fix: Map 0.0-1.0 to 0.6-1.0 (clutchdata)
        Fix: Map 0.0-1.0 to 1.0-0.6 (control)
    */
    float finalClutch = 1.0f - clutch;
    
    if (ext.GetGearCurr(vehicle) > 1) {
        finalClutch = map(clutch, 0.0f, 1.0f, 1.0f, 0.6f);

        // The next statement is a workaround for rolling back + brake + gear > 1 because it shouldn't rev then.
        // Also because we're checking on the game Control accel value and not the pedal position
        bool rollingback = ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < 0.0 && 
                           carControls.BrakeVal > 0.1f && 
                           carControls.ThrottleVal > 0.05f;

        // When pressing clutch and throttle, handle clutch and RPM
        if (clutch > 0.4f &&
            carControls.ThrottleVal > 0.05f &&
            !vehData.FakeNeutral && 
            !rollingback) {
            fakeRev(false, 0);
            ext.SetThrottle(vehicle, carControls.ThrottleVal);
        }
        // Don't care about clutch slippage, just handle RPM now
        if (vehData.FakeNeutral) {
            fakeRev(false, 0);
            ext.SetThrottle(vehicle, carControls.ThrottleVal);
        }
    }

    if (vehData.FakeNeutral || clutch >= 1.0f) {
        if (ENTITY::GET_ENTITY_SPEED(vehicle) < 1.0f) {
            finalClutch = -5.0f;
        }
        else {
            finalClutch = -0.5f;
        }
    }

    ext.SetClutch(vehicle, finalClutch);
}

/*
 * Custom limiter thing
 */
void functionLimiter() {
    if (ext.GetGearCurr(vehicle) == ext.GetTopGear(vehicle) || ext.GetGearCurr(vehicle) == 0) {
        vehData.HitLimiter = false;
        return;
    }

    auto ratios = ext.GetGearRatios(vehicle);
    float DriveMaxFlatVel = ext.GetDriveMaxFlatVel(vehicle);
    float maxSpeed = DriveMaxFlatVel / ratios[ext.GetGearCurr(vehicle)];

    if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > maxSpeed) {
        vehData.HitLimiter = true;
    }
    else {
        vehData.HitLimiter = false;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                   Mod functions: Reverse/Pedal handling
///////////////////////////////////////////////////////////////////////////////

#pragma region region_wheel

// Anti-Deadzone.
void SetControlADZ(eControl control, float value, float adz) {
    CONTROLS::_SET_CONTROL_NORMAL(0, control, sgn(value)*adz+(1.0f-adz)*value);
}

void functionRealReverse() {
    // Forward gear
    // Desired: Only brake
    if (ext.GetGearCurr(vehicle) > 0) {
        // LT behavior when stopped: Just brake
        if (carControls.BrakeVal > 0.01f && carControls.ThrottleVal < carControls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < 0.5f && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y >= -0.5f) { // < 0.5 so reverse never triggers
                                                                    //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stop");
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            ext.SetThrottleP(vehicle, 0.1f);
            ext.SetBrakeP(vehicle, 1.0f);
            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
        }
        // LT behavior when rolling back: Brake
        if (carControls.BrakeVal > 0.01f && carControls.ThrottleVal < carControls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < -0.5f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollback");
            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, carControls.BrakeVal);
            ext.SetThrottle(vehicle, 0.0f);
            ext.SetThrottleP(vehicle, 0.1f);
            ext.SetBrakeP(vehicle, 1.0f);
        }
        // RT behavior when rolling back: Burnout
        if (carControls.ThrottleVal > 0.5f && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < -1.0f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Rollback");
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, carControls.ThrottleVal);
            if (carControls.BrakeVal < 0.1f) {
                VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, false);
            }
            vehData.InduceBurnout = true;
        }
        else {
            vehData.InduceBurnout = false;
        }
    }
    // Reverse gear
    // Desired: RT reverses, LT brakes
    if (ext.GetGearCurr(vehicle) == 0) {
        // Enables reverse lights
        ext.SetThrottleP(vehicle, -0.1f);
        // RT behavior
        int throttleAndSomeBrake = 0;
        if (carControls.ThrottleVal > 0.01f && carControls.ThrottleVal > carControls.BrakeVal) {
            throttleAndSomeBrake++;
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Throttle @ Active Reverse");

            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, carControls.ThrottleVal);
        }
        // LT behavior when reversing
        if (carControls.BrakeVal > 0.01f &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y <= -0.5f) {
            throttleAndSomeBrake++;
            //showText(0.3, 0.35, 0.5, "functionRealReverse: Brake @ Reverse");

            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, carControls.BrakeVal);
        }
        // Throttle > brake  && BrakeVal > 0.1f
        if (throttleAndSomeBrake >= 2) {
            //showText(0.3, 0.4, 0.5, "functionRealReverse: Weird combo + rev it");

            CONTROLS::ENABLE_CONTROL_ACTION(0, ControlVehicleAccelerate, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleAccelerate, carControls.BrakeVal);
            fakeRev(false, 0);
        }

        // LT behavior when forward
        if (carControls.BrakeVal > 0.01f && carControls.ThrottleVal <= carControls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > 0.1f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Rollforwrd");

            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
            CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleBrake, carControls.BrakeVal);

            //CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            ext.SetBrakeP(vehicle, 1.0f);
        }

        // LT behavior when still
        if (carControls.BrakeVal > 0.01f && carControls.ThrottleVal <= carControls.BrakeVal &&
            ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > -0.5f && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y <= 0.1f) {
            //showText(0.3, 0.3, 0.5, "functionRealReverse: Brake @ Stopped");

            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
            CONTROLS::DISABLE_CONTROL_ACTION(0, ControlVehicleBrake, true);
            ext.SetBrakeP(vehicle, 1.0f);
        }
    }
}

// Forward gear: Throttle accelerates, Brake brakes (exclusive)
// Reverse gear: Throttle reverses, Brake brakes (exclusive)
void handlePedalsRealReverse(float wheelThrottleVal, float wheelBrakeVal) {
    wheelThrottleVal = pow(wheelThrottleVal, settings.ThrottleGamma);
    wheelBrakeVal = pow(wheelBrakeVal, settings.BrakeGamma);

    float speedThreshold = 0.5f;
    const float reverseThreshold = 2.0f;

    if (ext.GetGearCurr(vehicle) > 0) {
        // Going forward
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > speedThreshold) {
            //showText(0.3, 0.0, 1.0, "We are going forward");
            // Throttle Pedal normal
            if (wheelThrottleVal > 0.01f) {
                SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, carControls.ADZThrottle);
            }
            // Brake Pedal normal
            if (wheelBrakeVal > 0.01f) {
                SetControlADZ(ControlVehicleBrake, wheelBrakeVal, carControls.ADZBrake);
            }
        }

        // Standing still
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < speedThreshold && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y >= -speedThreshold) {
            //showText(0.3, 0.0, 1.0, "We are stopped");

            if (wheelThrottleVal > 0.01f) {
                SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, carControls.ADZThrottle);
            }

            if (wheelBrakeVal > 0.01f) {
                ext.SetThrottleP(vehicle, 0.1f);
                ext.SetBrakeP(vehicle, 1.0f);
                VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
            }
        }

        // Rolling back
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < -speedThreshold) {
            bool brakelights = false;
            // Just brake
            if (wheelThrottleVal <= 0.01f && wheelBrakeVal > 0.01f) {
                //showText(0.3, 0.0, 1.0, "We should brake");
                //showText(0.3, 0.05, 1.0, ("Brake pressure:" + std::to_string(wheelBrakeVal)));
                SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, carControls.ADZBrake);
                ext.SetThrottleP(vehicle, 0.1f);
                ext.SetBrakeP(vehicle, 1.0f);
                brakelights = true;
            }
            
            if (wheelThrottleVal > 0.01f && carControls.ClutchVal < settings.ClutchThreshold && !vehData.FakeNeutral) {
                //showText(0.3, 0.0, 1.0, "We should burnout");
                SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, carControls.ADZThrottle);
                SetControlADZ(ControlVehicleBrake, wheelThrottleVal, carControls.ADZThrottle);
                vehData.InduceBurnout = true;
            }
            else {
                vehData.InduceBurnout = false;
            }

            if (wheelThrottleVal > 0.01f && (carControls.ClutchVal > settings.ClutchThreshold || vehData.FakeNeutral)) {
                if (wheelBrakeVal > 0.01f) {
                    //showText(0.3, 0.0, 1.0, "We should rev and brake");
                    //showText(0.3, 0.05, 1.0, ("Brake pressure:" + std::to_string(wheelBrakeVal)) );
                    SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, carControls.ADZBrake);
                    ext.SetThrottleP(vehicle, 0.1f);
                    ext.SetBrakeP(vehicle, 1.0f);
                    brakelights = true;
                    fakeRev(false, 0);
                }
                else if (carControls.ClutchVal > 0.9 || vehData.FakeNeutral) {
                    //showText(0.3, 0.0, 1.0, "We should rev and do nothing");
                    ext.SetThrottleP(vehicle, wheelThrottleVal); 
                    fakeRev(false, 0);
                } else {
                    //showText(0.3, 0.0, 1.0, "We should rev and apply throttle");
                    SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, carControls.ADZThrottle);
                    ext.SetThrottleP(vehicle, wheelThrottleVal);
                    fakeRev(false, 0);
                }
            }
            VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, brakelights);
        }
    }

    if (ext.GetGearCurr(vehicle) == 0) {
        // Enables reverse lights
        ext.SetThrottleP(vehicle, -0.1f);
        
        // We're reversing
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < -speedThreshold) {
            //showText(0.3, 0.0, 1.0, "We are reversing");
            // Throttle Pedal Reverse
            if (wheelThrottleVal > 0.01f) {
                SetControlADZ(ControlVehicleBrake, wheelThrottleVal, carControls.ADZThrottle);
            }
            // Brake Pedal Reverse
            if (wheelBrakeVal > 0.01f) {
                SetControlADZ(ControlVehicleAccelerate, wheelBrakeVal, carControls.ADZBrake);
                ext.SetThrottleP(vehicle, -wheelBrakeVal);
                ext.SetBrakeP(vehicle, 1.0f);
            }
        }

        // Standing still
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < speedThreshold && ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y >= -speedThreshold) {
            //showText(0.3, 0.0, 1.0, "We are stopped");

            if (wheelThrottleVal > 0.01f) {
                SetControlADZ(ControlVehicleBrake, wheelThrottleVal, carControls.ADZThrottle);
            }

            if (wheelBrakeVal > 0.01f) {
                ext.SetThrottleP(vehicle, -wheelBrakeVal);
                ext.SetBrakeP(vehicle, 1.0f);
                VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, true);
            }
        }

        // We're rolling forwards
        if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > speedThreshold) {
            //showText(0.3, 0.0, 1.0, "We are rolling forwards");
            //bool brakelights = false;

            if (ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > reverseThreshold) {
                if (carControls.ClutchVal < settings.ClutchThreshold) {
                    CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, 1.0f);
                }
                // Brake Pedal Reverse
                if (wheelBrakeVal > 0.01f) {
                    SetControlADZ(ControlVehicleBrake, wheelBrakeVal, carControls.ADZBrake);
                    ext.SetThrottleP(vehicle, -wheelBrakeVal);
                    ext.SetBrakeP(vehicle, 1.0f);
                }
            }

            //VEHICLE::SET_VEHICLE_BRAKE_LIGHTS(vehicle, brakelights);
        }
    }
}

// Pedals behave like RT/LT
void handlePedalsDefault(float wheelThrottleVal, float wheelBrakeVal) {
    wheelThrottleVal = pow(wheelThrottleVal, settings.ThrottleGamma);
    wheelBrakeVal = pow(wheelBrakeVal, settings.BrakeGamma);
    if (wheelThrottleVal > 0.01f) {
        SetControlADZ(ControlVehicleAccelerate, wheelThrottleVal, carControls.ADZThrottle);
    }
    if (wheelBrakeVal > 0.01f) {
        SetControlADZ(ControlVehicleBrake, wheelBrakeVal, carControls.ADZBrake);
    }
}

void functionAutoReverse() {
    // Go forward
    if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) && 
        !CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) &&
        ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y > -1.0f &&
        ext.GetGearCurr(vehicle) == 0) {
        vehData.LockGear = 1;
    }

    // Reverse
    if (CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleBrake) && 
        !CONTROLS::IS_CONTROL_PRESSED(0, ControlVehicleAccelerate) &&
        ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y < 1.0f &&
        ext.GetGearCurr(vehicle) > 0) {
        vehData.FakeNeutral = false;
        vehData.LockGear = 0;
    }
}

///////////////////////////////////////////////////////////////////////////////
//                       Mod functions: Buttons
///////////////////////////////////////////////////////////////////////////////
// Blocks some vehicle controls on tapping, tries to activate them when holding.
// TODO: Some original "tap" controls don't work.
void blockButtons() {
    if (!settings.EnableManual || !settings.BlockCarControls ||
        carControls.PrevInput != CarControls::Controller || !isVehicleAvailable()) {
        return;
    }
    if (settings.ShiftMode == Automatic && ext.GetGearCurr(vehicle) > 1) {
        return;
    }

    if (carControls.UseLegacyController) {
        for (int i = 0; i < static_cast<int>(CarControls::LegacyControlType::SIZEOF_LegacyControlType); i++) {
            if (carControls.ControlNativeBlocks[i] == -1) continue;
            if (i != (int)CarControls::LegacyControlType::ShiftUp && 
                i != (int)CarControls::LegacyControlType::ShiftDown) continue;

            if (carControls.ButtonHeldOver(static_cast<CarControls::LegacyControlType>(i), 200)) {
                CONTROLS::_SET_CONTROL_NORMAL(0, carControls.ControlNativeBlocks[i], 1.0f);
            }
            else {
                CONTROLS::DISABLE_CONTROL_ACTION(0, carControls.ControlNativeBlocks[i], true);
            }

            if (carControls.ButtonReleasedAfter(static_cast<CarControls::LegacyControlType>(i), 200)) {
                // todo
            }
        }
        if (carControls.ControlNativeBlocks[(int)CarControls::LegacyControlType::Clutch] != -1) {
            CONTROLS::DISABLE_CONTROL_ACTION(0, carControls.ControlNativeBlocks[(int)CarControls::LegacyControlType::Clutch], true);
        }
    }
    else {
        for (int i = 0; i < static_cast<int>(CarControls::ControllerControlType::SIZEOF_ControllerControlType); i++) {
            if (carControls.ControlXboxBlocks[i] == -1) continue;
            if (i != (int)CarControls::ControllerControlType::ShiftUp && 
                i != (int)CarControls::ControllerControlType::ShiftDown) continue;

            if (carControls.ButtonHeldOver(static_cast<CarControls::ControllerControlType>(i), 200)) {
                CONTROLS::_SET_CONTROL_NORMAL(0, carControls.ControlXboxBlocks[i], 1.0f);
            }
            else {
                CONTROLS::DISABLE_CONTROL_ACTION(0, carControls.ControlXboxBlocks[i], true);
            }

            if (carControls.ButtonReleasedAfter(static_cast<CarControls::ControllerControlType>(i), 200)) {
                // todo
            }
        }
        if (carControls.ControlXboxBlocks[(int)CarControls::ControllerControlType::Clutch] != -1) {
            CONTROLS::DISABLE_CONTROL_ACTION(0, carControls.ControlXboxBlocks[(int)CarControls::ControllerControlType::Clutch], true);
        }
    }
}

void startStopEngine() {
    if (!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle) &&
        (carControls.PrevInput == CarControls::Controller	&& carControls.ButtonJustPressed(CarControls::ControllerControlType::Engine) ||
        carControls.PrevInput == CarControls::Controller	&& carControls.ButtonJustPressed(CarControls::LegacyControlType::Engine) ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::Engine) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::Engine) ||
        settings.ThrottleStart && carControls.ThrottleVal > 0.75f && (carControls.ClutchVal > settings.ClutchThreshold || vehData.FakeNeutral))) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, true, false, true);
    }
    if (VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle) &&
        (carControls.PrevInput == CarControls::Controller	&& carControls.ButtonJustPressed(CarControls::ControllerControlType::Engine) && settings.ToggleEngine ||
        carControls.PrevInput == CarControls::Controller	&& carControls.ButtonJustPressed(CarControls::LegacyControlType::Engine) && settings.ToggleEngine ||
        carControls.PrevInput == CarControls::Keyboard		&& carControls.ButtonJustPressed(CarControls::KeyboardControlType::Engine) ||
        carControls.PrevInput == CarControls::Wheel			&& carControls.ButtonJustPressed(CarControls::WheelControlType::Engine))) {
        VEHICLE::SET_VEHICLE_ENGINE_ON(vehicle, false, true, true);
    }
}

void checkWheelButtons() {
    if (carControls.PrevInput != CarControls::Wheel) {
        return;
    }

    if (carControls.HandbrakeVal > 0.1f) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, carControls.HandbrakeVal);
    }
    if (carControls.ButtonIn(CarControls::WheelControlType::Handbrake)) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHandbrake, 1.0f);
    }
    if (carControls.ButtonIn(CarControls::WheelControlType::Horn)) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHorn, 1.0f);
    }
    if (carControls.ButtonJustPressed(CarControls::WheelControlType::Lights)) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleHeadlight, 1.0f);
    }
    if (carControls.ButtonIn(CarControls::WheelControlType::LookBack)) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleLookBehind, 1.0f);
    }
    
    // who was first?
    if (carControls.ButtonIn(CarControls::WheelControlType::LookRight) &&
        carControls.ButtonJustPressed(CarControls::WheelControlType::LookLeft)) {
        vehData.LookBackRShoulder = true;
    }

    if (carControls.ButtonIn(CarControls::WheelControlType::LookLeft) &&
        carControls.ButtonIn(CarControls::WheelControlType::LookRight)) {
        CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(vehData.LookBackRShoulder ? -180.0f : 180.0f);
    }
    else if (carControls.ButtonIn(CarControls::WheelControlType::LookLeft)) {
        CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(0.0f, 1.0f);
        CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(90.0f);
    }
    else if (carControls.ButtonIn(CarControls::WheelControlType::LookRight)) {
        CAM::SET_GAMEPLAY_CAM_RELATIVE_PITCH(0.0f, 1.0f);
        CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(-90);
    }
    if (carControls.ButtonReleased(CarControls::WheelControlType::LookLeft) && !(carControls.ButtonIn(CarControls::WheelControlType::LookRight)) ||
        carControls.ButtonReleased(CarControls::WheelControlType::LookRight) && !(carControls.ButtonIn(CarControls::WheelControlType::LookLeft))) {
        CAM::SET_GAMEPLAY_CAM_RELATIVE_HEADING(0);
    }
    if (carControls.ButtonReleased(CarControls::WheelControlType::LookLeft)  ||
        carControls.ButtonReleased(CarControls::WheelControlType::LookRight)) {
        vehData.LookBackRShoulder = false;
    }

	// Set camera related decorators
	DECORATOR::DECOR_SET_BOOL(vehicle, (char *)decorLookingLeft, carControls.ButtonIn(CarControls::WheelControlType::LookLeft));
	DECORATOR::DECOR_SET_BOOL(vehicle, (char *)decorLookingRight, carControls.ButtonIn(CarControls::WheelControlType::LookRight));

	DECORATOR::DECOR_SET_BOOL(vehicle, (char *)decorLookingBack, 
		carControls.ButtonIn(CarControls::WheelControlType::LookBack) || 
		(
			carControls.ButtonIn(CarControls::WheelControlType::LookLeft) 
			&&
			carControls.ButtonIn(CarControls::WheelControlType::LookRight)
		)
	); // decorLookingBack = LookBack || (LookLeft && LookRight)

    if (carControls.ButtonJustPressed(CarControls::WheelControlType::Camera)) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlNextCamera, 1.0f);
    }


    if (carControls.ButtonHeld(CarControls::WheelControlType::RadioPrev, 1000) ||
        carControls.ButtonHeld(CarControls::WheelControlType::RadioNext, 1000)) {
        if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() != RadioOff) {
            vehData.RadioStationIndex = AUDIO::GET_PLAYER_RADIO_STATION_INDEX();
        }
        AUDIO::SET_VEH_RADIO_STATION(vehicle, "OFF");
        return;
    }
    if (carControls.ButtonReleased(CarControls::WheelControlType::RadioNext)) {
        if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() == RadioOff) {
            AUDIO::SET_RADIO_TO_STATION_INDEX(vehData.RadioStationIndex);
            return;
        }
        AUDIO::_0xFF266D1D0EB1195D(); // Next radio station
    }
    if (carControls.ButtonReleased(CarControls::WheelControlType::RadioPrev)) {
        if (AUDIO::GET_PLAYER_RADIO_STATION_INDEX() == RadioOff) {
            AUDIO::SET_RADIO_TO_STATION_INDEX(vehData.RadioStationIndex);
            return;
        }
        AUDIO::_0xDD6BCF9E94425DF9(); // Prev radio station
    }

    if (carControls.ButtonJustPressed(CarControls::WheelControlType::IndicatorLeft)) {
        if (!vehData.BlinkerLeft) {
            vehData.BlinkerTicks = 1;
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false); // L
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, true); // R
            vehData.BlinkerLeft = true;
            vehData.BlinkerRight = false;
            vehData.BlinkerHazard = false;
        }
        else {
            vehData.BlinkerTicks = 0;
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
            vehData.BlinkerLeft = false;
            vehData.BlinkerRight = false;
            vehData.BlinkerHazard = false;
        }
    }
    if (carControls.ButtonJustPressed(CarControls::WheelControlType::IndicatorRight)) {
        if (!vehData.BlinkerRight) {
            vehData.BlinkerTicks = 1;
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, true); // L
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false); // R
            vehData.BlinkerLeft = false;
            vehData.BlinkerRight = true;
            vehData.BlinkerHazard = false;
        }
        else {
            vehData.BlinkerTicks = 0;
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
            vehData.BlinkerLeft = false;
            vehData.BlinkerRight = false;
            vehData.BlinkerHazard = false;
        }
    }

    float wheelCenterDeviation = ( carControls.SteerVal - 0.5f) / 0.5f;

    if (vehData.BlinkerTicks == 1 && abs(wheelCenterDeviation) > 0.2f)
    {
        vehData.BlinkerTicks = 2;
    }

    if (vehData.BlinkerTicks == 2 && abs(wheelCenterDeviation) < 0.1f)
    {
        vehData.BlinkerTicks = 0;
        VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
        VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
        vehData.BlinkerLeft = false;
        vehData.BlinkerRight = false;
        vehData.BlinkerHazard = false;
    }

    if (carControls.ButtonJustPressed(CarControls::WheelControlType::IndicatorHazard)) {
        if (!vehData.BlinkerHazard) {
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, true); // L
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, true); // R
            vehData.BlinkerLeft = false;
            vehData.BlinkerRight = false;
            vehData.BlinkerHazard = true;
        }
        else {
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 0, false);
            VEHICLE::SET_VEHICLE_INDICATOR_LIGHTS(vehicle, 1, false);
            vehData.BlinkerLeft = false;
            vehData.BlinkerRight = false;
            vehData.BlinkerHazard = false;
        }
    }
}

///////////////////////////////////////////////////////////////////////////////
//                    Wheel input and force feedback
///////////////////////////////////////////////////////////////////////////////

#pragma region region_ffb

void doWheelSteering() {
    if (carControls.PrevInput != CarControls::Wheel)
        return;

    float steerMult;
    if (vehData.Class == VehicleClass::Bike || vehData.Class == VehicleClass::Quad)
        steerMult = settings.SteerAngleMax / settings.SteerAngleBike;
    else if (vehData.Class == VehicleClass::Car)
        steerMult = settings.SteerAngleMax / settings.SteerAngleCar;
    else {
        steerMult = settings.SteerAngleMax / settings.SteerAngleBoat;
    }

    float effSteer = steerMult * 2.0f * (carControls.SteerVal - 0.5f);

    /*
     * Patched steering is direct without any processing, and super direct.
     * _SET_CONTROL_NORMAL is with game processing and could have a bit of delay
     * Both should work without any deadzone, with a note that the second one
     * does need a specified anti-deadzone (recommended: 24-25%)
     * 
     */
    if (vehData.Class == VehicleClass::Car && settings.PatchSteeringControl) {
        ext.SetSteeringInputAngle(vehicle, -effSteer);
    }
    else {
        SetControlADZ(ControlVehicleMoveLeftRight, effSteer, carControls.ADZSteer);
    }
}

int calculateDamper(float gain, float wheelsOffGroundRatio) {
    Vector3 accelValsAvg = vehData.GetRelativeAccelerationAverage();
    
    // Just to use floats everywhere here
    float damperMax = static_cast<float>(settings.DamperMax);
    float damperMin = static_cast<float>(settings.DamperMin);
    float damperMinSpeed = static_cast<float>(settings.DamperMinSpeed);

    float absVehicleSpeed = abs(ENTITY::GET_ENTITY_SPEED_VECTOR(vehicle, true).y);
    float damperFactorSpeed = map(absVehicleSpeed, 0.0f, damperMinSpeed, damperMax, damperMin);
    damperFactorSpeed = fmaxf(damperFactorSpeed, damperMin);
    // already clamped on the upper bound by abs(vel) in map()

    // 2G of deceleration causes addition of damperMin. Heavier feeling steer when braking!
    float damperFactorAccel = map(-accelValsAvg.y / 9.81f, -2.0f, 2.0f, -damperMin, damperMin);
    damperFactorAccel = fmaxf(damperFactorAccel, -damperMin);
    damperFactorAccel = fminf(damperFactorAccel, damperMin);

    float damperForce = damperFactorSpeed + damperFactorAccel;

    damperForce = damperForce * (1.0f - wheelsOffGroundRatio);
    
    if (vehData.Class == VehicleClass::Car || vehData.Class == VehicleClass::Quad) {
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true)) {
            damperForce /= 2.0f;
        }
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 1, true)) {
            damperForce /= 2.0f;
        }
    }
    else if (vehData.Class == VehicleClass::Bike) {
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true)) {
            damperForce /= 4.0f;
        }
    }

    if (!VEHICLE::GET_IS_VEHICLE_ENGINE_RUNNING(vehicle)) {
        damperForce *= 2.0f;
    }

    // clamp
    damperForce = fmaxf(damperForce, damperMin / 4.0f);
    damperForce = fminf(damperForce, damperMax * 2.0f);

    damperForce *= gain;

    return static_cast<int>(damperForce);
}

int calculateDetail() {
    // Detail feel / suspension compression based
    float compSpeedTotal = 0.0f;
    auto compSpeed = vehData.GetWheelCompressionSpeeds();

    // More than 2 wheels! Trikes should be ok, etc.
    if (ext.GetNumWheels(vehicle) > 2) {
        // left should pull left, right should pull right
        compSpeedTotal = -compSpeed[0] + compSpeed[1];
    }

    return static_cast<int>(1000.0f * settings.DetailMult * compSpeedTotal);
}

int calculateSoftLock(int totalForce) {
    float steerMult;
    if (vehData.Class == VehicleClass::Bike || vehData.Class == VehicleClass::Quad)
        steerMult = settings.SteerAngleMax / settings.SteerAngleBike;
    else if (vehData.Class == VehicleClass::Car)
        steerMult = settings.SteerAngleMax / settings.SteerAngleCar;
    else {
        steerMult = settings.SteerAngleMax / settings.SteerAngleBoat;
    }
    float effSteer = steerMult * 2.0f * (carControls.SteerVal - 0.5f);

    if (effSteer > 1.0f) {
        totalForce = static_cast<int>((effSteer - 1.0f) * 100000) + totalForce;
        if (effSteer > 1.1f) {
            totalForce = 10000;
        }
    } else if (effSteer < -1.0f) {
        totalForce = static_cast<int>((-effSteer - 1.0f) * -100000) + totalForce;
        if (effSteer < -1.1f) {
            totalForce = -10000;
        }
    }
    return totalForce;
}

// Despite being scientifically inaccurate, "self-aligning torque" is the best description.
int calculateSat(int defaultGain, float steeringAngle, float wheelsOffGroundRatio, bool isCar) {
    Vector3 velocityWorld = ENTITY::GET_ENTITY_VELOCITY(vehicle);
    Vector3 positionWorld = ENTITY::GET_ENTITY_COORDS(vehicle, 1);
    Vector3 travelWorld = velocityWorld + positionWorld;
    Vector3 travelRelative = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, travelWorld.x, travelWorld.y, travelWorld.z);

    Vector3 rotationVelocity = ENTITY::GET_ENTITY_ROTATION_VELOCITY(vehicle);
    Vector3 turnWorld = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, ENTITY::GET_ENTITY_SPEED(vehicle)*-sin(rotationVelocity.z), ENTITY::GET_ENTITY_SPEED(vehicle)*cos(rotationVelocity.z), 0.0f);
    Vector3 turnRelative = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, turnWorld.x, turnWorld.y, turnWorld.z);
    float turnRelativeNormX = (travelRelative.x + turnRelative.x) / 2.0f;
    //float turnRelativeNormY = (travelRelative.y + turnRelative.y) / 2.0f;
    //Vector3 turnWorldNorm = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, turnRelativeNormX, turnRelativeNormY, 0.0f);

    float steeringAngleRelX = ENTITY::GET_ENTITY_SPEED(vehicle)*-sin(steeringAngle);
    float steeringAngleRelY = ENTITY::GET_ENTITY_SPEED(vehicle)*cos(steeringAngle);
    Vector3 steeringWorld = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, steeringAngleRelX, steeringAngleRelY, 0.0f);
    Vector3 steeringRelative = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, steeringWorld.x, steeringWorld.y, steeringWorld.z);

    float setpoint = travelRelative.x;

    // This can be tuned but it feels pretty nice right now with Kp = 1.0, Ki, Kd = 0.0.
    double error = pid.getOutput(steeringRelative.x, setpoint);

    int satForce = static_cast<int>(settings.SATAmpMult * defaultGain * -error);

    // "Reduction" effects - those that affect already calculated things
    bool understeering = false;
    float understeer = 0.0f;
    if (isCar) {
        understeer = sgn(travelRelative.x - steeringAngleRelX) * (turnRelativeNormX - steeringAngleRelX);
        if (steeringAngleRelX > turnRelativeNormX && turnRelativeNormX > travelRelative.x ||
            steeringAngleRelX < turnRelativeNormX && turnRelativeNormX < travelRelative.x) {
            satForce = (int)((float)satForce / std::max(1.0f, 3.3f * understeer + 1.0f));
            understeering = true;
        }
    }

    satForce = (int)((float)satForce * (1.0f - wheelsOffGroundRatio));

    if (vehData.Class == VehicleClass::Car || vehData.Class == VehicleClass::Quad) {
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true)) {
            satForce = satForce / 4;
        }
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 1, true)) {
            satForce = satForce / 4;
        }
    }
    else if (vehData.Class == VehicleClass::Bike) {
        if (VEHICLE::IS_VEHICLE_TYRE_BURST(vehicle, 0, true)) {
            satForce = satForce / 10;
        }
    }

    if (settings.DisplayInfo) {
        showText(0.85, 0.175, 0.4, "RelSteer:\t" + std::to_string(steeringRelative.x), 4);
        showText(0.85, 0.200, 0.4, "SetPoint:\t" + std::to_string(travelRelative.x), 4);
        showText(0.85, 0.225, 0.4, "Error:\t\t" + std::to_string(error), 4);
        showText(0.85, 0.250, 0.4, std::string(understeering ? "~b~" : "~w~") + "Under:\t\t" + std::to_string(understeer) + "~w~", 4);
    }

    return satForce;
}

// Despite being scientifically inaccurate, "self-aligning torque" is the best description.
void drawSteeringLines(float steeringAngle) {
    Vector3 velocityWorld = ENTITY::GET_ENTITY_VELOCITY(vehicle);
    Vector3 positionWorld = ENTITY::GET_ENTITY_COORDS(vehicle, 1);
    Vector3 travelWorld = velocityWorld + positionWorld;
    Vector3 travelRelative = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, travelWorld.x, travelWorld.y, travelWorld.z);

    Vector3 rotationVelocity = ENTITY::GET_ENTITY_ROTATION_VELOCITY(vehicle);
    Vector3 turnWorld = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, ENTITY::GET_ENTITY_SPEED(vehicle)*-sin(rotationVelocity.z), ENTITY::GET_ENTITY_SPEED(vehicle)*cos(rotationVelocity.z), 0.0f);
    Vector3 turnRelative = ENTITY::GET_OFFSET_FROM_ENTITY_GIVEN_WORLD_COORDS(vehicle, turnWorld.x, turnWorld.y, turnWorld.z);
    float turnRelativeNormX = (travelRelative.x + turnRelative.x) / 2.0f;
    float turnRelativeNormY = (travelRelative.y + turnRelative.y) / 2.0f;
    Vector3 turnWorldNorm = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, turnRelativeNormX, turnRelativeNormY, 0.0f);

    float steeringAngleRelX = ENTITY::GET_ENTITY_SPEED(vehicle)*-sin(steeringAngle);
    float steeringAngleRelY = ENTITY::GET_ENTITY_SPEED(vehicle)*cos(steeringAngle);
    Vector3 steeringWorld = ENTITY::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(vehicle, steeringAngleRelX, steeringAngleRelY, 0.0f);

    GRAPHICS::DRAW_LINE(positionWorld.x, positionWorld.y, positionWorld.z, travelWorld.x, travelWorld.y, travelWorld.z, 0, 255, 0, 255);
    GRAPHICS::DRAW_LINE(positionWorld.x, positionWorld.y, positionWorld.z, turnWorldNorm.x, turnWorldNorm.y, turnWorldNorm.z, 255, 0, 0, 255);
    GRAPHICS::DRAW_LINE(positionWorld.x, positionWorld.y, positionWorld.z, steeringWorld.x, steeringWorld.y, steeringWorld.z, 255, 0, 255, 255);
}

float getSteeringAngle(Vehicle v) {
    auto angles = ext.GetWheelSteeringAngles(v);
    float wheelsSteered = 0.0f;
    float avgAngle = 0.0f;

    for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
        if (i < 3 && angles[i] != 0.0f) {
            wheelsSteered += 1.0f;
            avgAngle += angles[i];
        }
    }

    if (wheelsSteered > 0.5f && wheelsSteered < 2.5f) { // bikes, cars, quads
        avgAngle /= wheelsSteered;
    }
    else {
        avgAngle = ext.GetSteeringAngle(vehicle)*ext.GetSteeringMultiplier(vehicle); // tank, forklift
    }
    return avgAngle;
}



float getFloatingSteeredWheelsRatio(Vehicle v) {
    auto suspensionStates = ext.GetWheelsOnGround(vehicle);
    auto angles = ext.GetWheelSteeringAngles(vehicle);

    float wheelsOffGroundRatio = 0.0f;
    float wheelsInAir = 0.0f;
    float wheelsSteered = 0.0f;

    for (int i = 0; i < ext.GetNumWheels(vehicle); i++) {
        if (angles[i] != 0.0f) {
            wheelsSteered += 1.0f;
            if (suspensionStates[i] == false) {
                wheelsInAir += 1.0f;
            }
        }
    }
    if (wheelsSteered != 0.0f) {
        wheelsOffGroundRatio = wheelsInAir / wheelsSteered;
    }
    return wheelsOffGroundRatio;
}

float getSteeringLock() {
    switch (vehData.Class) {
        case VehicleClass::Bike:
        case VehicleClass::Quad:
        case VehicleClass::Bicycle:
            return settings.SteerAngleBike;
        case VehicleClass::Car:
            return settings.SteerAngleCar;
        case VehicleClass::Boat:
            return settings.SteerAngleBoat;
        case VehicleClass::Plane:
        case VehicleClass::Heli:
        case VehicleClass::Train:
        case VehicleClass::Unknown:
        default: return settings.SteerAngleCar;
    }
}

void playFFBGround() {
    if (!settings.EnableFFB ||
        carControls.PrevInput != CarControls::Wheel) {
        return;
    }

    float rotationScale = 1.0f;
    if (settings.ScaleFFB) {
        rotationScale = getSteeringLock() / settings.SteerAngleMax * 0.66f + 0.34f;
    }

    if (settings.LogiLEDs) {
        carControls.PlayLEDs(vehData.RPM, 0.45f, 0.95f);
    }

    float avgAngle = getSteeringAngle(vehicle);
    float wheelsOffGroundRatio = getFloatingSteeredWheelsRatio(vehicle);

    int detailForce = calculateDetail();
    int satForce = calculateSat(2500, avgAngle, wheelsOffGroundRatio, vehData.Class == VehicleClass::Car);
    int damperForce = calculateDamper(50.0f, wheelsOffGroundRatio);

    // Decrease damper if sat rises, so constantForce doesn't fight against damper
    float damperMult = 1.0f - std::min(fabs((float)satForce), 10000.0f) / 10000.0f;
    damperForce = (int)(damperMult * (float)damperForce);

    int totalForce = satForce + detailForce;
    totalForce = (int)((float)totalForce * rotationScale);
    totalForce = calculateSoftLock(totalForce);
    carControls.PlayFFBDynamics(totalForce, damperForce);

    const float minGforce = 5.0f;
    const float maxGforce = 50.0f;
    const float minForce = 500.0f;
    const float maxForce = 10000.0f;
    float gForce = abs(vehData.GetRelativeAccelerationAverage().y) / 9.81f;
    bool collision = gForce > minGforce;
    int res = static_cast<int>(map(gForce, minGforce, maxGforce, minForce, maxForce) * settings.CollisionMult);
    if (collision) {
        carControls.PlayFFBCollision(res);
    }

    if (settings.DisplayInfo && collision) {
        std::string info = "Collision @ ~r~" + std::to_string(gForce) + "G~w~~n~";
        std::string info2 = "FFB: " + std::to_string(res);
        showNotification(mtPrefix + info + info2, &prevNotification);
    }
    if (settings.DisplayInfo) {
        showText(0.85, 0.275, 0.4, std::string(abs(satForce) > 10000 ? "~r~" : "~w~") + "FFBSat:\t\t" + std::to_string(satForce) + "~w~", 4);
        showText(0.85, 0.300, 0.4, std::string(abs(totalForce) > 10000 ? "~r~" : "~w~") + "FFBFin:\t\t" + std::to_string(totalForce) + "~w~", 4);
        showText(0.85, 0.325, 0.4, std::string("Damper:\t") + std::to_string(damperForce).c_str(), 4);
    }
}

void playFFBWater() {
    if (!settings.EnableFFB ||
        carControls.PrevInput != CarControls::Wheel) {
        return;
    }

    float rotationScale = 1.0f;
    if (settings.ScaleFFB) {
        rotationScale = getSteeringLock() / settings.SteerAngleMax * 0.66f + 0.34f;
    }

    if (settings.LogiLEDs) {
        carControls.PlayLEDs(vehData.RPM, 0.45f, 0.95f);
    }

    auto suspensionStates = ext.GetWheelsOnGround(vehicle);
    auto angles = ext.GetWheelSteeringAngles(vehicle);

    bool isInWater = ENTITY::GET_ENTITY_SUBMERGED_LEVEL(vehicle) > 0.10f;
    int damperForce = calculateDamper(50.0f, isInWater ? 0.25f : 1.0f);
    int detailForce = calculateDetail();
    int satForce = calculateSat(750, ENTITY::GET_ENTITY_ROTATION_VELOCITY(vehicle).z, 1.0f, false);

    if (!isInWater) {
        satForce = 0;
    }

    int totalForce = satForce + detailForce;
    totalForce = (int)((float)totalForce * rotationScale);
    totalForce = calculateSoftLock(totalForce);
    carControls.PlayFFBDynamics(totalForce, damperForce);

    if (settings.DisplayInfo) {
        showText(0.85, 0.275, 0.4, std::string(abs(satForce) > 10000 ? "~r~" : "~w~") + "FFBSat:\t\t" + std::to_string(satForce) + "~w~", 4);
        showText(0.85, 0.300, 0.4, std::string(abs(totalForce) > 10000 ? "~r~" : "~w~") + "FFBFin:\t\t" + std::to_string(totalForce) + "~w~", 4);
    }
}

#pragma endregion region_ffb

#pragma endregion region_wheel

///////////////////////////////////////////////////////////////////////////////
//                             Misc features
///////////////////////////////////////////////////////////////////////////////

void functionAutoLookback() {
    if (ext.GetGearCurr(vehicle) == 0) {
        CONTROLS::_SET_CONTROL_NORMAL(0, ControlVehicleLookBehind, 1.0f);
    }
}

void functionAutoGear1() {
    if (ext.GetThrottle(vehicle) < 0.1f && ENTITY::GET_ENTITY_SPEED(vehicle) < 0.1f && ext.GetGearCurr(vehicle) > 1) {
        vehData.LockGear = 1;
    }
}

void functionHillGravity() {
    if (!carControls.BrakeVal
        && ENTITY::GET_ENTITY_SPEED(vehicle) < 2.0f &&
        VEHICLE::IS_VEHICLE_ON_ALL_WHEELS(vehicle)) {
        float pitch = ENTITY::GET_ENTITY_PITCH(vehicle);;

        float clutchNeutral = vehData.FakeNeutral ? 1.0f : carControls.ClutchVal;
        if (pitch < 0 || clutchNeutral) {
            ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
                vehicle, 1, 0.0f, -1 * (pitch / 150.0f) * 1.1f * clutchNeutral, 0.0f, true, true, true, true);
        }
        if (pitch > 10.0f || clutchNeutral)
            ENTITY::APPLY_FORCE_TO_ENTITY_CENTER_OF_MASS(
            vehicle, 1, 0.0f, -1 * (pitch / 90.0f) * 0.35f * clutchNeutral, 0.0f, true, true, true, true);
    }
}

void functionHidePlayerInFPV() {
    if (settings.HidePlayerInFPV && CAM::GET_FOLLOW_PED_CAM_VIEW_MODE() == 4 && isVehicleAvailable()) {
        ENTITY::SET_ENTITY_VISIBLE(playerPed, false, false);
    }
    else {
        ENTITY::SET_ENTITY_VISIBLE(playerPed, true, false);
    }
}

///////////////////////////////////////////////////////////////////////////////
//                              Script entry
///////////////////////////////////////////////////////////////////////////////

void registerDecorator(const char *thing, eDecorType type) {
    std::string strType = "?????";
    switch (type) {
    case DECOR_TYPE_FLOAT: strType = "float"; break;
    case DECOR_TYPE_BOOL: strType = "bool"; break;
    case DECOR_TYPE_INT: strType = "int"; break;
    case DECOR_TYPE_UNK: strType = "unknown"; break;
    case DECOR_TYPE_TIME: strType = "time"; break;
    }

    if (!DECORATOR::DECOR_IS_REGISTERED_AS_TYPE((char*)thing, type)) {
        DECORATOR::DECOR_REGISTER((char*)thing, type);
        logger.Write(DEBUG, "DECOR: Registered \"%s\" as %s", thing, strType.c_str());
    }
}

// @Unknown Modder
BYTE* g_bIsDecorRegisterLockedPtr = nullptr;
bool setupGlobals() {
    auto addr = mem::FindPattern("\x40\x53\x48\x83\xEC\x20\x80\x3D\x00\x00\x00\x00\x00\x8B\xDA\x75\x29",
                                 "xxxxxxxx????xxxxx");
    if (!addr) {
        logger.Write(ERROR, "Couldn't find pattern for bIsDecorRegisterLockedPtr");
        return false;
    }

    g_bIsDecorRegisterLockedPtr = (BYTE*)(addr + *(int*)(addr + 8) + 13);

    logger.Write(DEBUG, "bIsDecorRegisterLockedPtr @ 0x%p", g_bIsDecorRegisterLockedPtr);

    *g_bIsDecorRegisterLockedPtr = 0;

    // New decorators! :)
    registerDecorator(decorCurrentGear, DECOR_TYPE_INT);
    registerDecorator(decorShiftNotice, DECOR_TYPE_INT);
    registerDecorator(decorFakeNeutral, DECOR_TYPE_INT);
    registerDecorator(decorSetShiftMode, DECOR_TYPE_INT);
    registerDecorator(decorGetShiftMode, DECOR_TYPE_INT);

	// Camera mods interoperability
	registerDecorator(decorLookingLeft, DECOR_TYPE_BOOL);
	registerDecorator(decorLookingRight, DECOR_TYPE_BOOL);
	registerDecorator(decorLookingBack, DECOR_TYPE_BOOL);

    *g_bIsDecorRegisterLockedPtr = 1;
    return true;
}

void readSettings() {
    settings.Read(&carControls);
    if (settings.LogLevel > 4) settings.LogLevel = 1;
    logger.SetMinLevel((LogLevel)settings.LogLevel);

    speedoIndex = static_cast<int>(std::find(speedoTypes.begin(), speedoTypes.end(), settings.Speedo) - speedoTypes.begin());
    if (speedoIndex >= speedoTypes.size()) {
        speedoIndex = 0;
    }
    vehData.FakeNeutral = settings.DefaultNeutral;
    menu.ReadSettings();
    logger.Write(INFO, "Settings read");
}

void main() {
    logger.Write(INFO, "Script started");
    absoluteModPath = Paths::GetModuleFolder(Paths::GetOurModuleHandle()) + mtDir;
    settingsGeneralFile = absoluteModPath + "\\settings_general.ini";
    settingsWheelFile = absoluteModPath + "\\settings_wheel.ini";
    settingsStickFile = absoluteModPath + "\\settings_stick.ini";
    settingsMenuFile = absoluteModPath + "\\settings_menu.ini";
    textureWheelFile = absoluteModPath + "\\texture_wheel.png";

    settings.SetFiles(settingsGeneralFile, settingsWheelFile);
    menu.RegisterOnMain(std::bind(onMenuInit));
    menu.RegisterOnExit(std::bind(onMenuClose));
    menu.SetFiles(settingsMenuFile);
    readSettings();
    ext.initOffsets();

    logger.Write(INFO, "Setting up globals");
    if (!setupGlobals()) {
        logger.Write(ERROR, "Global setup failed!");
    }

    initWheel();

    //if (!controls.StickControl.PreInit()) {
    //	logger.Write("STICK: DirectInput failed to initialize");
    //}

    if (FileExists(textureWheelFile)) {
        textureWheelId = createTexture(textureWheelFile.c_str());
    }
    else {
        logger.Write(ERROR, textureWheelFile + " does not exist.");
        textureWheelId = -1;
    }

    logger.Write(DEBUG, "START: Starting with MT:  %s", settings.EnableManual ? "ON" : "OFF");
    logger.Write(INFO, "START: Initialization finished");

    while (true) {
        update_globals();
        update_vehicle();
        update_inputs();
        update_steeringpatches();
        update_hud();
        update_input_controls();
        update_manual_transmission();
        update_misc_features();
        update_menu();
        WAIT(0);
    }
}

void ScriptMain() {
    srand(GetTickCount());
    main();
}
