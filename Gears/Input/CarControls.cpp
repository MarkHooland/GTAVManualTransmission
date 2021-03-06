#include "CarControls.hpp"

#include <Windows.h>
#include "keyboard.h"
#include "Util/Logger.hpp"
#include "Util/MathExt.h"

CarControls::CarControls(): PrevInput(Keyboard)
                          , SteerGUID()
                          , SteerAxisType(WheelAxisType::Steer)
                          , mXInputController(1)
                          , averageBrakeVals{0} {
    std::fill(ControlXboxBlocks.begin(), ControlXboxBlocks.end(), -1);
}

CarControls::~CarControls() = default;

void CarControls::InitWheel() {
    if (!mWheelInput.InitWheel()) {
        // Initialization failed somehow, so we skip
        return;
    }

    auto steerGUID = WheelAxesGUIDs[static_cast<int>(WheelAxisType::Steer)];
    auto steerAxis = mWheelInput.StringToAxis(WheelAxes[static_cast<int>(WheelAxisType::ForceFeedback)]);
    auto ffAxis = mWheelInput.StringToAxis(WheelAxes[static_cast<int>(WheelAxisType::Steer)]);
    
    if (mWheelInput.InitFFB(steerGUID, steerAxis)) {
        mWheelInput.UpdateCenterSteering(steerGUID, ffAxis);
    }
}

void CarControls::updateKeyboard() {
    ThrottleVal = IsKeyPressed(KBControl[static_cast<int>(KeyboardControlType::Throttle)]) ? 1.0f : 0.0f;
    BrakeVal = IsKeyPressed(KBControl[static_cast<int>(KeyboardControlType::Brake)]) ? 1.0f : 0.0f;
    ClutchVal = IsKeyPressed(KBControl[static_cast<int>(KeyboardControlType::Clutch)]) ? 1.0f : 0.0f;
}

void CarControls::updateController() {
    if (UseLegacyController) {
        ThrottleVal = mNativeController.GetAnalogValue(mNativeController.EControlToButton(LegacyControls[static_cast<int>(LegacyControlType::Throttle)]));
        BrakeVal = mNativeController.GetAnalogValue(mNativeController.EControlToButton(LegacyControls[static_cast<int>(LegacyControlType::Brake)]));
        ClutchVal = mNativeController.GetAnalogValue(mNativeController.EControlToButton(LegacyControls[static_cast<int>(LegacyControlType::Clutch)]));
    } else {
        ThrottleVal = mXInputController.GetAnalogValue(mXInputController.StringToButton(ControlXbox[static_cast<int>(ControllerControlType::Throttle)]));
        BrakeVal = mXInputController.GetAnalogValue(mXInputController.StringToButton(ControlXbox[static_cast<int>(ControllerControlType::Brake)]));
        ClutchVal = mXInputController.GetAnalogValue(mXInputController.StringToButton(ControlXbox[static_cast<int>(ControllerControlType::Clutch)]));
    }
}

// analog > button
float CarControls::getInputValue(WheelAxisType axisType, WheelControlType buttonType, float minRaw, float maxRaw) {
    float inputValue;
    auto axis = mWheelInput.StringToAxis(WheelAxes[static_cast<int>(axisType)]);
    auto axisGUID = WheelAxesGUIDs[static_cast<int>(axisType)];

    int axisValue = mWheelInput.GetAxisValue(axis, axisGUID);

    if (axisValue != -1) {
        inputValue = map(static_cast<float>(axisValue), minRaw, maxRaw, 0.0f, 1.0f);
    }
    else {
        inputValue = ButtonIn(buttonType) ? 1.0f : 0.0f;
    }
    inputValue = std::clamp(inputValue, 0.0f, 1.0f);
    return inputValue;
}

float CarControls::filterDeadzone(float input, float deadzone, float deadzoneOffset) {
    float filtered;
    if (deadzone > 0.005f) {
        float center = 0.5f + deadzoneOffset;
        float minLimit = center - deadzone;
        float maxLimit = center + deadzone;
        if (input < minLimit) {
            filtered = map(input, 0.0f, minLimit, 0.0f, 0.5f);
        }
        else if (input > maxLimit) {
            filtered = map(input, maxLimit, 1.0f, 0.5f, 1.0f);
        }
        else {
            filtered = 0.5f;
        }
    }
    else {
        filtered = input;
    }
    return filtered;
}

void CarControls::updateWheel() {
    ThrottleVal = getInputValue(WheelAxisType::Throttle, WheelControlType::Throttle, static_cast<float>(ThrottleMin), static_cast<float>(ThrottleMax));
    BrakeVal = getInputValue(WheelAxisType::Brake, WheelControlType::Brake, static_cast<float>(BrakeMin), static_cast<float>(BrakeMax));
    ClutchVal = getInputValue(WheelAxisType::Clutch, WheelControlType::Clutch, static_cast<float>(ClutchMin), static_cast<float>(ClutchMax));
    HandbrakeVal = getInputValue(WheelAxisType::Handbrake, WheelControlType::Handbrake, static_cast<float>(HandbrakeMin), static_cast<float>(HandbrakeMax));
    SteerValRaw = getInputValue(WheelAxisType::Steer, WheelControlType::UNKNOWN, static_cast<float>(SteerMin), static_cast<float>(SteerMax));
    SteerVal = filterDeadzone(SteerValRaw, DZSteer, DZSteerOffset);
}

void CarControls::UpdateValues(InputDevices prevInput, bool skipKeyboardInput) {
    if (UseLegacyController) {
        mNativeController.Update();
    }
    else {
        mXInputController.Update();
    }

    mWheelInput.Update();
    mWheelInput.UpdateButtonChangeStates();

    if (!skipKeyboardInput)   
        CheckCustomButtons();

    // Update ThrottleVal, BrakeVal and ClutchVal ranging from 0.0f (no touch) to 1.0f (full press)
    switch (prevInput) {
        case Keyboard: {
            updateKeyboard();
            break;
        }			
        case Controller: {
            updateController();
            break;
        }
        case Wheel: {
            updateWheel();
            break;
        }
        default: break;
    }

    averageBrakeVals[movAvgIndex] = BrakeVal;
    BrakeValAvg = 0.0f;
    for (float val : averageBrakeVals) {
        BrakeValAvg += val;
    }
    BrakeValAvg /= AVERAGEWINDOW;
    movAvgIndex = (movAvgIndex + 1) % AVERAGEWINDOW;
}

// Limitation: Only works for hardcoded input types. Currently throttle.
CarControls::InputDevices CarControls::GetLastInputDevice(InputDevices previousInput, bool enableWheel) {
    auto kbThrottleIdx = static_cast<int>(KeyboardControlType::Throttle);
    auto kbBrakeIdx = static_cast<int>(KeyboardControlType::Brake);
    if (IsKeyJustPressed(KBControl[kbThrottleIdx], KeyboardControlType::Throttle) ||
        IsKeyPressed(KBControl[kbThrottleIdx]) ||
        IsKeyJustPressed(KBControl[kbBrakeIdx], KeyboardControlType::Brake) ||
        IsKeyPressed(KBControl[kbBrakeIdx])) {
        return Keyboard;
    }

    if (UseLegacyController) {
        auto lcThrottleIdx = static_cast<int>(LegacyControlType::Throttle);
        auto lcThrottleBtn = mNativeController.EControlToButton(LegacyControls[lcThrottleIdx]);
        auto lcBrakeIdx = static_cast<int>(LegacyControlType::Brake);
        auto lcBrakeBtn = mNativeController.EControlToButton(LegacyControls[lcBrakeIdx]);
        if (mNativeController.IsButtonJustPressed(lcThrottleBtn) ||
            mNativeController.IsButtonPressed(lcThrottleBtn) ||
            mNativeController.IsButtonJustPressed(lcBrakeBtn) ||
            mNativeController.IsButtonPressed(lcBrakeBtn)) {
            return Controller;
        }
    } 
    else {
        auto cThrottleIdx = static_cast<int>(ControllerControlType::Throttle);
        auto cThrottleBtn = mXInputController.StringToButton(ControlXbox[cThrottleIdx]);
        auto cBrakeIdx = static_cast<int>(ControllerControlType::Brake);
        auto cBrakeBtn = mXInputController.StringToButton(ControlXbox[cBrakeIdx]);
        if (mXInputController.IsButtonJustPressed(cThrottleBtn) ||
            mXInputController.IsButtonPressed(cThrottleBtn) ||
            mXInputController.IsButtonJustPressed(cBrakeBtn) ||
            mXInputController.IsButtonPressed(cBrakeBtn)) {
            return Controller;
        }
    }
    if (enableWheel && mWheelInput.IsConnected(WheelAxesGUIDs[static_cast<int>(WheelAxisType::Steer)])) {
        float throttleVal = getInputValue(WheelAxisType::Throttle, WheelControlType::Throttle, static_cast<float>(ThrottleMin), static_cast<float>(ThrottleMax));
        float brakeVal = getInputValue(WheelAxisType::Brake, WheelControlType::Brake, static_cast<float>(BrakeMin), static_cast<float>(BrakeMax));
        float clutchVal = getInputValue(WheelAxisType::Clutch, WheelControlType::Clutch, static_cast<float>(ClutchMin), static_cast<float>(ClutchMax));

        if (throttleVal > 0.5f ||
            brakeVal > 0.5f ||
            clutchVal > 0.5f) {
            return Wheel;
        }
    }
    return previousInput;
}

/*
 * Keyboard section
 */

bool CarControls::IsKeyPressed(int key) {
    return IsKeyDown(key);
}

bool CarControls::IsKeyJustPressed(int key, KeyboardControlType control) {
    KBControlCurr[static_cast<int>(control)] = IsKeyPressed(key);

    // raising edge
    if (KBControlCurr[static_cast<int>(control)] && !KBControlPrev[static_cast<int>(control)]) {
        KBControlPrev[static_cast<int>(control)] = KBControlCurr[static_cast<int>(control)];
        return true;
    }

    KBControlPrev[static_cast<int>(control)] = KBControlCurr[static_cast<int>(control)];
    return false;
}

bool CarControls::ButtonJustPressed(KeyboardControlType control) {
    return IsKeyJustPressed(KBControl[static_cast<int>(control)], control);
}

/*
* Controller section
*/

bool CarControls::ButtonJustPressed(ControllerControlType control) {
    return mXInputController.IsButtonJustPressed(
        mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]));
}

bool CarControls::ButtonReleased(ControllerControlType control) {
    return mXInputController.IsButtonJustReleased(
        mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]));
}

bool CarControls::ButtonReleasedAfter(ControllerControlType control, int time) {
    return mXInputController.WasButtonHeldForMs(
        mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]), time);
}

bool CarControls::ButtonHeld(ControllerControlType control) {
    return mXInputController.WasButtonHeldForMs(
        mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]), CToggleTime);
}

bool CarControls::ButtonHeldOver(ControllerControlType control, int millis) {
    return mXInputController.WasButtonHeldOverMs(
        mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]), millis);
}

XInputController::TapState CarControls::ButtonTapped(ControllerControlType control) {
    return mXInputController.WasButtonTapped(mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]), MaxTapTime);
}

bool CarControls::ButtonIn(ControllerControlType control) {
    return mXInputController.IsButtonPressed(mXInputController.StringToButton(ControlXbox[static_cast<int>(control)]));
}

void CarControls::SetControllerTriggerLevel(float value) {
    mXInputController.SetTriggerValue(value);
    mNativeController.SetTriggerValue(value);
}

float CarControls::GetControllerTrigger() {
    return mXInputController.GetTriggerValue();
}

/*
 * Legacy stuff
 */
bool CarControls::ButtonJustPressed(LegacyControlType control) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.IsButtonJustPressed(gameButton);
}
bool CarControls::ButtonReleased(LegacyControlType control) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.IsButtonJustReleased(gameButton);
}

bool CarControls::ButtonHeld(LegacyControlType control) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.WasButtonHeldForMs(gameButton, CToggleTime);
}

bool CarControls::ButtonHeldOver(LegacyControlType control, int millis) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.WasButtonHeldOverMs(gameButton, millis);
}

bool CarControls::ButtonIn(LegacyControlType control) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.IsButtonPressed(gameButton);
}

NativeController::TapState CarControls::ButtonTapped(LegacyControlType control) {
    if (!UseLegacyController) return NativeController::TapState::ButtonUp;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.WasButtonTapped(gameButton, MaxTapTime);
}

bool CarControls::ButtonReleasedAfter(LegacyControlType control, int time) {
    if (!UseLegacyController) return false;
    auto gameButton = mNativeController.EControlToButton(LegacyControls[static_cast<int>(control)]);
    return mNativeController.WasButtonHeldForMs(gameButton, time);
}

/*
 * Wheel section
 */

bool CarControls::ButtonJustPressed(WheelControlType control) {
    if (!mWheelInput.IsConnected(WheelButtonGUIDs[static_cast<int>(control)]) ||
        WheelButton[static_cast<int>(control)] == -1) {
        return false;
    }
    return mWheelInput.IsButtonJustPressed(WheelButton[static_cast<int>(control)],
                                           WheelButtonGUIDs[static_cast<int>(control)]);
}

bool CarControls::ButtonReleased(WheelControlType control) {
    if (!mWheelInput.IsConnected(WheelButtonGUIDs[static_cast<int>(control)]) ||
        WheelButton[static_cast<int>(control)] == -1) {
        return false;
    }
    return mWheelInput.IsButtonJustReleased(WheelButton[static_cast<int>(control)],
                                            WheelButtonGUIDs[static_cast<int>(control)]);
}

bool CarControls::ButtonHeld(WheelControlType control, int delay) {
    if (!mWheelInput.IsConnected(WheelButtonGUIDs[static_cast<int>(control)]) ||
        WheelButton[static_cast<int>(control)] == -1) {
        return false;
    }
    return mWheelInput.WasButtonHeldForMs(WheelButton[static_cast<int>(control)],
                                          WheelButtonGUIDs[static_cast<int>(control)], delay);
}

bool CarControls::ButtonIn(WheelControlType control) {
    if (!mWheelInput.IsConnected(WheelButtonGUIDs[static_cast<int>(control)]) ||
        WheelButton[static_cast<int>(control)] == -1) {
        return false;
    }
    return mWheelInput.IsButtonPressed(WheelButton[static_cast<int>(control)],
                                       WheelButtonGUIDs[static_cast<int>(control)]);
}

void CarControls::CheckCustomButtons() {
    if (!mWheelInput.IsConnected(WheelAxesGUIDs[static_cast<int>(WheelAxisType::Steer)])) {
        return;
    }
    for (int i = 0; i < MAX_RGBBUTTONS; i++) {
        if (WheelToKey[i] != -1) {
            INPUT input;
            input.type = INPUT_KEYBOARD;
            input.ki.dwExtraInfo = 0;
            input.ki.wVk = 0;
            input.ki.wScan = MapVirtualKey(WheelToKey[i], MAPVK_VK_TO_VSC);

            if (mWheelInput.IsButtonJustPressed(i,WheelToKeyGUID)) {
                input.ki.dwFlags = KEYEVENTF_SCANCODE;
                SendInput(1, &input, sizeof(INPUT));
            }
            if (mWheelInput.IsButtonJustReleased(i, WheelToKeyGUID)) {
                input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                SendInput(1, &input, sizeof(INPUT));
            }
        }
    }
}

void CarControls::CheckGUIDs(const std::vector<_GUID> & guids) {
    auto foundGuids = mWheelInput.GetGuids();
    auto reggdGuids = guids;
    // We're only checking for devices that should be used but aren't found
    std::sort(foundGuids.begin(), foundGuids.end());
    std::sort(reggdGuids.begin(), reggdGuids.end());
    std::vector<GUID> missingReg;
    std::vector<GUID> missingFnd;

    // Registered but not enumerated
    std::set_difference(
        reggdGuids.begin(), reggdGuids.end(),
        foundGuids.begin(), foundGuids.end(), std::back_inserter(missingReg));

    if (!missingReg.empty()) {
        logger.Write(WARN, "WHEEL: Used in .ini, unavailable: ");
        for (auto g : missingReg) {
            logger.Write(WARN, "WHEEL: GUID:   %s", GUID2String(g).c_str());
        }
    }

    // Enumerated but not registered
    std::set_difference(
        foundGuids.begin(), foundGuids.end(),
        reggdGuids.begin(), reggdGuids.end(), std::back_inserter(missingFnd));

    FreeDevices.clear();

    if (!missingFnd.empty()) {
        logger.Write(INFO, "WHEEL: Not set up in .ini: ");
        for (auto g : missingFnd) {
            auto entry = mWheelInput.FindEntryFromGUID(g);
            if (entry == nullptr) continue;
            std::wstring wDevName = entry->diDeviceInstance.tszInstanceName;
            auto devName = std::string(wDevName.begin(), wDevName.end());
            logger.Write(INFO, "WHEEL: Device: %s", devName.c_str());
            logger.Write(INFO, "WHEEL: GUID:   %s", GUID2String(g).c_str());
            if (isSupportedDrivingDevice(entry->diDeviceInstance.dwDevType)) {
                FreeDevices.push_back(Device(devName, g));
            }
        }
    }
}

void CarControls::PlayFFBDynamics(int totalForce, int damperForce) {
    auto ffAxis = mWheelInput.StringToAxis(WheelAxes[static_cast<int>(WheelAxisType::ForceFeedback)]);
    mWheelInput.SetConstantForce(SteerGUID, ffAxis, totalForce);
    mWheelInput.SetDamper(SteerGUID, ffAxis, damperForce);
}

void CarControls::PlayFFBCollision(int collisionForce) {
    auto ffAxis = mWheelInput.StringToAxis(WheelAxes[static_cast<int>(WheelAxisType::ForceFeedback)]);
    mWheelInput.SetCollision(SteerGUID, ffAxis, collisionForce);
}

void CarControls::PlayLEDs(float rpm, float firstLed, float lastLed) {
    mWheelInput.PlayLedsDInput(SteerGUID, rpm, firstLed, lastLed);
}

void CarControls::StopFFB(bool turnOffLeds) {
    if (turnOffLeds) {
        mWheelInput.PlayLedsDInput(SteerGUID, 0.0, 0.5, 1.0);
    }
    mWheelInput.StopEffects();
}

bool CarControls::WheelAvailable() {
    return mWheelInput.IsConnected(SteerGUID);
}

WheelDirectInput& CarControls::GetWheel() {
    return mWheelInput;
}
